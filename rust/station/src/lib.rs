//! Product composition for one typed USBRadioPlus station.
//!
//! This crate translates validated configuration into radio-session policy and
//! composes the app_rpt and rpt_advanced controller transports through the ASL3
//! adapter. Adapter-neutral behavior remains in the lower core, radio, and
//! runtime crates; this layer owns no direct hardware I/O or DSP algorithms.

#![deny(warnings)]
#![cfg_attr(coverage, feature(coverage_attribute))]

mod control;
mod hardware;
mod media;
mod program;
mod runtime;

use usbradioplus_core::{
    CarrierSource as ConfigCarrierSource, ChannelConfiguration, CtcssSource, CtcssTone,
    CtcssTurnoffMode, DcsPolarity, HardwareOutputAssignment, RadioDuplexMode, ReceiveAudioSource,
    SignalingMethod,
};
use usbradioplus_radio::{
    CarrierSource, CtcssReceiveConfig, CtcssToneIndex, CtcssToneMask, CtcssTransmitConfig,
    DcsReceiveConfig, DcsTransmitConfig, NoiseFilterProfile, OutputConfig, OutputRoute,
    QualificationConfig, RadioError, ReceiveConfig, ReceiveSignaling, SessionConfig,
    SubaudibleSource, ToneOffMode, TransmitConfig, TransmitSignaling,
};
use usbradioplus_runtime::NativeProcessingPlan;

pub use control::{PreparedStation, StationPlan, StationPreparationError};
pub use hardware::{
    Cm119Plan, HardwarePlan, HardwarePlanError, ParallelPlan, SelectedHardwarePlan, hardware_plan,
};
pub use media::{
    ControllerSetup, DirectCallbacks, StationControl, StationMedia, StationMediaError,
    StationReceive, StationTransmit,
};
pub use program::{
    ProgramRingConsumer, ProgramRingPlan, ProgramRingSetupError, prepare_program_ring,
    program_ring_plan,
};
pub use runtime::{
    CallbackStatistics, ControllerRequests, HardwareInputs, HardwareOutputs, ReceiveObservation,
    SharedHardwareState, StationControlHost, StationRuntime, StationRuntimeStatistics,
};

const TUNING_SCALE_MAXIMUM: u32 = 1_000;
const SIGNED_PCM_MAXIMUM: u32 = 32_767;

/// Controller transport using one USBRadioPlus channel.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControllerTransport {
    /// Fixed 8 kHz compatibility interface used by app_rpt.
    AppRpt,
    /// Fixed native-rate interface used by rpt_advanced.
    RptAdvanced,
}

/// Resolve the immutable native processing resources for one channel.
#[must_use]
pub fn native_processing_plan(channel: &ChannelConfiguration) -> NativeProcessingPlan {
    NativeProcessingPlan {
        local: channel.local.clone(),
        voice_telemetry: channel.voice_telemetry.clone(),
        deemphasis_enabled: channel.station.receive.audio_source == ReceiveAudioSource::Flat,
        deemphasis_corner_hz: channel.station.hardware.deemphasis_corner_hz,
        preemphasis_enabled: channel.station.transmit.preemphasis_enabled,
        preemphasis_corner_hz: channel.station.hardware.preemphasis_corner_hz,
    }
}

/// Translate one validated channel into immutable whole-session radio policy.
pub fn radio_session_config(
    channel: &ChannelConfiguration,
    transport: ControllerTransport,
    generation_id: u64,
    maximum_frame_count: u32,
) -> Result<SessionConfig, RadioError> {
    let station = &channel.station;
    let receive = &station.receive;
    let mut result = SessionConfig::new(generation_id, maximum_frame_count, maximum_frame_count);
    result.publication_interval_milliseconds = station.diagnostics.status_publication_interval_ms;
    result.receive_input_gain = if receive.audio_source == ReceiveAudioSource::Disabled {
        0.0
    } else {
        db_to_linear(channel.local.input_gain_db)
    };
    result.receive = ReceiveConfig {
        noise_filter_profile: match receive.noise_filter_type {
            0 => NoiseFilterProfile::Standard,
            _ => NoiseFilterProfile::Alternate,
        },
        squelch_open_level: (u32::from(999 - receive.squelch_level) * SIGNED_PCM_MAXIMUM)
            / TUNING_SCALE_MAXIMUM,
        squelch_hysteresis: u32::from(receive.noise_squelch_hysteresis),
        ctcss_decoder_gain: db_to_linear(station.ctcss.receive_decoder_gain_db),
        vox_threshold: i32::from(receive.vox_threshold),
        vox_hang_milliseconds: i32::from(receive.vox_hang_ms),
        signaling: receive_signaling(channel)?,
        cpu_saver_enabled: receive.cpu_saver_enabled,
        native_squelch_delay_frames: u32::from(receive.squelch_delay_ms) * 48,
    };
    result.qualification = QualificationConfig {
        carrier_source: carrier_source(receive.cos_assignment),
        subaudible_source: subaudible_source(channel),
        subaudible_override: station.ctcss.receive_override_enabled,
        advanced_transport: transport == ControllerTransport::RptAdvanced,
        radio_duplex: station.duplex.radio_mode == RadioDuplexMode::Full,
        receive_on_delay_blocks: u32::from(receive.on_delay_frames),
        transmit_off_delay_blocks: u32::from(station.transmit.off_delay_frames),
    };
    result.transmit = TransmitConfig {
        signaling: transmit_signaling(channel),
        tone_off_mode: tone_off_mode(station.ctcss.turnoff_mode),
        settle_time_milliseconds: i32::try_from(station.transmit.settle_ms)
            .map_err(|_| RadioError::InvalidArgument)?,
        cpu_saver_enabled: station.transmit.cpu_saver_enabled,
        receive_blanking_milliseconds: i32::from(station.transmit.rx_blanking_ms),
        output_a: output_config(station.hardware.output_a_assignment),
        output_b: output_config(station.hardware.output_b_assignment),
    };
    Ok(result)
}

fn db_to_linear(db: f64) -> f32 {
    10.0_f32.powf(db as f32 / 20.0)
}

fn carrier_source(source: ConfigCarrierSource) -> CarrierSource {
    match source {
        ConfigCarrierSource::Disabled => CarrierSource::Disabled,
        ConfigCarrierSource::Dsp => CarrierSource::DspNoise,
        ConfigCarrierSource::Vox => CarrierSource::Vox,
        ConfigCarrierSource::Usb => CarrierSource::Usb,
        ConfigCarrierSource::UsbInverted => CarrierSource::UsbInverted,
        ConfigCarrierSource::Parallel => CarrierSource::Parallel,
        ConfigCarrierSource::ParallelInverted => CarrierSource::ParallelInverted,
    }
}

fn subaudible_source(channel: &ChannelConfiguration) -> SubaudibleSource {
    match channel.station.receive.signaling_method {
        SignalingMethod::Carrier => SubaudibleSource::Disabled,
        SignalingMethod::Dcs => SubaudibleSource::Dsp,
        SignalingMethod::Ctcss => match channel.station.ctcss.receive_source {
            CtcssSource::Disabled => SubaudibleSource::Disabled,
            CtcssSource::Usb => SubaudibleSource::Usb,
            CtcssSource::UsbInverted => SubaudibleSource::UsbInverted,
            CtcssSource::Dsp => SubaudibleSource::Dsp,
            CtcssSource::Parallel => SubaudibleSource::Parallel,
            CtcssSource::ParallelInverted => SubaudibleSource::ParallelInverted,
        },
    }
}

fn receive_signaling(channel: &ChannelConfiguration) -> Result<ReceiveSignaling, RadioError> {
    let station = &channel.station;
    match station.receive.signaling_method {
        SignalingMethod::Carrier => Ok(ReceiveSignaling::Disabled),
        SignalingMethod::Ctcss if station.ctcss.receive_source == CtcssSource::Dsp => {
            Ok(ReceiveSignaling::Ctcss(CtcssReceiveConfig {
                tones: receive_tone_mask(&station.ctcss.receive_frequencies)?,
                relaxed: station.ctcss.receive_relax != 0,
            }))
        }
        SignalingMethod::Ctcss => Ok(ReceiveSignaling::Disabled),
        SignalingMethod::Dcs => Ok(ReceiveSignaling::Dcs(DcsReceiveConfig {
            code: i32::from(station.dcs.receive_code.value()),
            inverted: station.dcs.receive_code.polarity() == DcsPolarity::Inverted,
        })),
    }
}

fn receive_tone_mask(tones: &[CtcssTone]) -> Result<CtcssToneMask, RadioError> {
    tones.iter().try_fold(CtcssToneMask::EMPTY, |mask, tone| {
        let index = u8::try_from(tone.table_index()).map_err(|_| RadioError::InvalidArgument)?;
        let index = CtcssToneIndex::new(index).ok_or(RadioError::InvalidArgument)?;
        Ok(mask.with(index))
    })
}

fn transmit_signaling(channel: &ChannelConfiguration) -> TransmitSignaling {
    let station = &channel.station;
    match station.transmit.signaling_method {
        SignalingMethod::Carrier => TransmitSignaling::Disabled,
        SignalingMethod::Ctcss => {
            let mut mapped = [0; usbradioplus_radio::CTCSS_TONE_COUNT];
            for (receive, transmit) in station
                .ctcss
                .receive_frequencies
                .iter()
                .zip(&station.ctcss.transmit_frequencies)
            {
                mapped[receive.table_index()] = i32::from(transmit.tenths_hz());
            }
            TransmitSignaling::Ctcss(CtcssTransmitConfig {
                default_frequency_tenths_hz: i32::from(station.ctcss.transmit_default.tenths_hz()),
                mapped_frequencies_tenths_hz: mapped,
                peak: db_to_linear(station.ctcss.transmit_peak_dbfs),
                turnoff_duration_milliseconds: i32::from(station.ctcss.tail_duration_ms),
                turnoff_phase_shift_degrees: station.ctcss.phase_shift_degrees,
                turnoff_tail_tone_hz: station.ctcss.tail_frequency_hz,
            })
        }
        SignalingMethod::Dcs => TransmitSignaling::Dcs(DcsTransmitConfig {
            code: i32::from(station.dcs.transmit_code.value()),
            inverted: station.dcs.transmit_code.polarity() == DcsPolarity::Inverted,
            peak: db_to_linear(station.dcs.peak_dbfs),
            turnoff_enabled: station.dcs.turnoff_code_enabled,
            turnoff_duration_milliseconds: i32::from(station.dcs.turnoff_duration_ms),
        }),
    }
}

fn tone_off_mode(mode: CtcssTurnoffMode) -> ToneOffMode {
    match mode {
        CtcssTurnoffMode::None => ToneOffMode::None,
        CtcssTurnoffMode::PhaseShift => ToneOffMode::PhaseShift,
        CtcssTurnoffMode::ToneRemove => ToneOffMode::ToneRemove,
        CtcssTurnoffMode::TailTone => ToneOffMode::TailTone,
    }
}

fn output_config(assignment: HardwareOutputAssignment) -> OutputConfig {
    let route = match assignment {
        HardwareOutputAssignment::Off => OutputRoute::Disabled,
        HardwareOutputAssignment::Voice => OutputRoute::Voice,
        HardwareOutputAssignment::Ctcss => OutputRoute::Tone,
        HardwareOutputAssignment::VoiceCtcss => OutputRoute::Composite,
        HardwareOutputAssignment::AuxiliaryVoice => OutputRoute::AuxiliaryVoice,
    };
    OutputConfig {
        route,
        tone_gain: 1.0,
        tone_bias: 0.0,
    }
}

#[cfg(test)]
mod tests;
