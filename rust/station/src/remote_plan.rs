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
        Ok(self
            .with_radio_configuration(configuration, radio)
            .with_explicit_processing(processing))
    }
}

#[cfg(test)]
mod tests {
    use crate::media::tests::prepared;
    use usbradioplus_core::{RemoteSignal, RemoteSignalSource as Source};
    use usbradioplus_radio::{ReceiveSignaling, SubaudibleSource, TransmitSignaling};

    fn state() -> usbradioplus_core::RemoteOperatingState {
        usbradioplus_core::RemoteOperatingState {
            mode: 1,
            encode: RemoteSignal {
                source: Source::Dsp,
                enabled: true,
                tone_tenths_hz: 1148,
                ..RemoteSignal::default()
            },
            decode: RemoteSignal {
                source: Source::Dsp,
                enabled: true,
                tone_tenths_hz: 1000,
                ..RemoteSignal::default()
            },
            ..Default::default()
        }
    }

    #[test]
    fn remote_signaling_prepares_independent_dsp_directions_and_preserves_levels() {
        let (original, _) = prepared(crate::ControllerTransport::RptAdvanced, 4).into_parts();
        let updated = original
            .clone()
            .with_remote_signaling(&state(), SubaudibleSource::Disabled)
            .unwrap();
        let ReceiveSignaling::Ctcss(receive) = updated.radio().receive.signaling else {
            panic!("missing decoder")
        };
        let TransmitSignaling::Ctcss(transmit) = updated.radio().transmit.signaling else {
            panic!("missing encoder")
        };
        assert_ne!(receive.tones.bits(), 0);
        assert_eq!(transmit.default_frequency_tenths_hz, 1148);
        assert_eq!(
            transmit.peak,
            crate::db_to_linear(original.configuration().station.ctcss.transmit_peak_dbfs)
        );
        assert_eq!(
            updated.radio().receive_input_gain,
            original.radio().receive_input_gain
        );
        assert_eq!(
            updated.radio().transmit.output_a,
            original.radio().transmit.output_a
        );
        assert_eq!(
            updated.radio().transmit.output_b,
            original.radio().transmit.output_b
        );
        assert_eq!(updated.processing().local, original.processing().local);
        assert_eq!(
            updated.processing().voice_telemetry,
            original.processing().voice_telemetry
        );
        assert_eq!(updated.hardware(), original.hardware());
        assert_eq!(
            updated.radio().generation_id,
            original.radio().generation_id + 1
        );
    }

    #[test]
    fn remote_signaling_never_duplicates_rig_tones_or_fabricates_external_decode() {
        let (original, _) = prepared(crate::ControllerTransport::AppRpt, 4).into_parts();
        let mut requested = state();
        requested.encode.source = Source::Rig;
        requested.decode.source = Source::External;
        requested.decode.tone_tenths_hz = 0;
        requested.decode.dcs_code = 0o23;
        let updated = original
            .clone()
            .with_remote_signaling(&requested, SubaudibleSource::Parallel)
            .unwrap();
        assert_eq!(
            updated.radio().receive.signaling,
            ReceiveSignaling::Disabled
        );
        assert_eq!(
            updated.radio().transmit.signaling,
            TransmitSignaling::Disabled
        );
        assert_eq!(
            updated.radio().qualification.subaudible_source,
            SubaudibleSource::Parallel
        );
        assert!(
            original
                .clone()
                .with_remote_signaling(&requested, SubaudibleSource::Disabled)
                .is_err()
        );
        requested.decode.source = Source::Rig;
        assert!(
            original
                .with_remote_signaling(&requested, SubaudibleSource::Parallel)
                .is_err()
        );
    }

    #[test]
    fn remote_signaling_mode_changes_preserve_profiles_and_conditioning_corners() {
        let (original, _) = prepared(crate::ControllerTransport::RptAdvanced, 4).into_parts();
        let mut requested = state();
        requested.mode = 3;
        requested.encode.enabled = false;
        requested.decode.enabled = false;
        let updated = original
            .clone()
            .with_remote_signaling(&requested, SubaudibleSource::Disabled)
            .unwrap();
        assert!(!updated.processing().receive_deemphasis);
        assert!(!updated.processing().transmit_preemphasis);
        assert_eq!(
            updated.processing().receive_deemphasis_corner_hz,
            original.processing().receive_deemphasis_corner_hz
        );
        assert_eq!(
            updated.processing().transmit_preemphasis_corner_hz,
            original.processing().transmit_preemphasis_corner_hz
        );
        assert_eq!(
            updated.processing().receive_graph,
            original.processing().receive_graph
        );
        assert_eq!(
            updated.processing().transmit_graph,
            original.processing().transmit_graph
        );
        assert_eq!(
            updated.radio().receive.signaling,
            ReceiveSignaling::Disabled
        );
        assert_eq!(
            updated.radio().transmit.signaling,
            TransmitSignaling::Disabled
        );
        let restored = updated
            .with_remote_signaling(&state(), SubaudibleSource::Disabled)
            .unwrap();
        assert_eq!(
            restored.processing().receive_deemphasis,
            original.configuration().station.receive.audio_source
                == usbradioplus_core::ReceiveAudioSource::Flat
        );
        assert_eq!(
            restored.processing().transmit_preemphasis,
            original
                .configuration()
                .station
                .transmit
                .preemphasis_enabled
        );
    }
}
