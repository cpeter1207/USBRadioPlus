//! Translate admitted operating signaling into the existing DSP update path.

use crate::{StationPlan, StationPreparationError};
use usbradioplus_core::{
    CtcssSource, CtcssTone, DcsCode, RemoteOperatingState, RemoteSignal,
    RemoteSignalSource as Source, SignalingMethod,
};
use usbradioplus_radio::{RadioError, ReceiveSignaling, SubaudibleSource};

fn tone(value: u32) -> Result<CtcssTone, RadioError> {
    u16::try_from(value)
        .ok()
        .and_then(CtcssTone::from_tenths_hz)
        .ok_or(RadioError::InvalidArgument)
}

fn dcs(value: RemoteSignal) -> Result<DcsCode, RadioError> {
    format!(
        "{:03o}{}",
        value.dcs_code,
        if value.inverted { 'I' } else { 'N' }
    )
    .parse()
    .map_err(|_| RadioError::InvalidArgument)
}

impl StationPlan {
    /// Preserve live CAT-owned signaling/mode on an unrelated file/audio edit.
    /// A deliberate remote tune carries its own state and is never overwritten.
    pub(crate) fn retain_remote_operating(
        self,
        current: &StationPlan,
    ) -> Result<Self, StationPreparationError> {
        if self.remote_configuration().is_some()
            && self.remote_configuration() == current.remote_configuration()
            && self.remote_operating.is_none()
        {
            if let Some(state) = &current.remote_operating {
                return self
                    .with_remote_carrier(current.configuration().station.receive.cos_assignment)
                    .with_remote_signaling(state, current.radio().qualification.subaudible_source);
            }
        }
        Ok(self)
    }
    /// Retain the selected carrier provider in both configuration and the radio
    /// plan, so later tune updates cannot revert it to the original DSP input.
    pub fn with_remote_carrier(self, carrier: usbradioplus_core::CarrierSource) -> Self {
        let mut configuration = self.configuration().clone();
        configuration.station.receive.cos_assignment = carrier;
        let mut radio = *self.radio();
        radio.qualification.carrier_source = crate::carrier_source(carrier);
        self.with_radio_configuration(configuration, radio)
    }

    /// Prepare effective DSP signaling for an already RF-admitted complete state.
    /// External qualification must name a real selected hardware input; generic
    /// rig DCD is never substituted for decoded CTCSS/DCS. Frequency/VFO/power
    /// remain with the remote owner. This only builds a replacement plan: the
    /// existing mailbox must acknowledge both callback owners before commit.
    pub fn with_remote_signaling(
        self,
        state: &RemoteOperatingState,
        external_decode: SubaudibleSource,
    ) -> Result<Self, StationPreparationError> {
        let mut configuration = self.configuration().clone();
        let station = &mut configuration.station;
        station.receive.signaling_method = SignalingMethod::Carrier;
        station.transmit.signaling_method = SignalingMethod::Carrier;

        let qualification = if !state.decode.enabled {
            SubaudibleSource::Disabled
        } else {
            let method = if state.decode.tone_tenths_hz != 0 {
                station.ctcss.receive_frequencies = vec![tone(state.decode.tone_tenths_hz)?];
                SignalingMethod::Ctcss
            } else {
                station.dcs.receive_code = dcs(state.decode)?;
                SignalingMethod::Dcs
            };
            station.receive.signaling_method = method;
            match state.decode.source {
                Source::Dsp => {
                    station.ctcss.receive_source = CtcssSource::Dsp;
                    SubaudibleSource::Dsp
                }
                Source::External => {
                    station.ctcss.receive_source = match external_decode {
                        SubaudibleSource::Usb => CtcssSource::Usb,
                        SubaudibleSource::UsbInverted => CtcssSource::UsbInverted,
                        SubaudibleSource::Parallel => CtcssSource::Parallel,
                        SubaudibleSource::ParallelInverted => CtcssSource::ParallelInverted,
                        _ => return Err(RadioError::InvalidArgument.into()),
                    };
                    external_decode
                }
                _ => return Err(RadioError::InvalidArgument.into()),
            }
        };
        if state.encode.enabled && state.encode.source == Source::Dsp {
            station.transmit.signaling_method = if state.encode.tone_tenths_hz != 0 {
                let selected = tone(state.encode.tone_tenths_hz)?;
                station.ctcss.transmit_default = selected;
                station.ctcss.transmit_frequencies =
                    vec![selected; station.ctcss.receive_frequencies.len()];
                SignalingMethod::Ctcss
            } else {
                station.dcs.transmit_code = dcs(state.encode)?;
                SignalingMethod::Dcs
            };
        } else if state.encode.enabled
            && !matches!(state.encode.source, Source::External | Source::Rig)
        {
            return Err(RadioError::InvalidArgument.into());
        }

        let mut radio = *self.radio();
        radio.generation_id = radio
            .generation_id
            .checked_add(1)
            .ok_or(RadioError::InvalidArgument)?;
        radio.receive.signaling = if qualification == SubaudibleSource::Dsp {
            crate::receive_signaling(&configuration)?
        } else {
            ReceiveSignaling::Disabled
        };
        radio.qualification.subaudible_source = qualification;
        radio.transmit.signaling = crate::transmit_signaling(&configuration);
        let mut processing = self.processing().clone();
        let fm = matches!(state.mode, 1 | 9 | 10);
        // Software transport duplex cannot override a physically half-duplex
        // non-FM radio. Genuine full duplex remains independently configured.
        radio.qualification.advanced_transport =
            fm && self.transport() == crate::ControllerTransport::RptAdvanced;
        processing.receive_deemphasis = fm
            && configuration.station.receive.audio_source
                == usbradioplus_core::ReceiveAudioSource::Flat;
        processing.transmit_preemphasis = fm && configuration.station.transmit.preemphasis_enabled;
        processing.receive_ctcss_mask = match radio.receive.signaling {
            ReceiveSignaling::Ctcss(receive)
                if processing.receive_ctcss_mask != 0
                    || processing.local.as_ref().is_some_and(|profile| {
                        profile.receive.pl_filter == usbradioplus_core::PlFilter::DecodedToneNotch
                    }) =>
            {
                receive.tones.bits()
            }
            _ => 0,
        };
        processing.transmit_dcs = matches!(
            radio.transmit.signaling,
            usbradioplus_radio::TransmitSignaling::Dcs(_)
        );
        let mut plan = self
            .with_radio_configuration(configuration, radio)
            .with_explicit_processing(processing);
        plan.remote_operating = Some(state.clone());
        Ok(plan)
    }
}

#[cfg(test)]
#[path = "tests/remote_plan_tests.rs"]
mod tests;
