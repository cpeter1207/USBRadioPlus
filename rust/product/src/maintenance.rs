//! Shared physical radio measurements and maintenance semantics for both hosts.

use crate::{
    ABI_VERSION, Status, UrpAstChannelCommand, UrpAstChannelStatus, eeprom_flags, mixer, switch,
};
use std::fmt::Write;
use std::mem::size_of;
use usbradioplus_core::{ChannelConfiguration, radio_control::*};
use usbradioplus_driver::{HardwareMaintenance, HardwareStationDiagnostics, HardwareStationError};
use usbradioplus_gpio::EepromImage;
use usbradioplus_radio::SessionSnapshot;

/// Render control-plane diagnostics for the final mono transmit programme.
/// `valid` describes gated integrated loudness; true peak can be available sooner.
pub(crate) fn processing_status_text(
    observation: usbradioplus_station::CallbackStatistics,
    loudness: Option<usbradioplus_core::OutputLoudnessSnapshot>,
) -> String {
    let mut output = format!(
        "radio_receive_calls={}\nradio_transmit_calls={}\nradio_receive_failures={}\nradio_transmit_failures={}\n",
        observation.receive_calls,
        observation.transmit_calls,
        observation.receive_failures,
        observation.transmit_failures,
    );
    writeln!(
        output,
        "transmit_loudness_meter_enabled={}",
        u8::from(loudness.is_some())
    )
    .expect("writing to String cannot fail");
    writeln!(
        output,
        "transmit_loudness_epoch={}",
        loudness.map_or(0, |value| value.epoch)
    )
    .expect("writing to String cannot fail");
    writeln!(
        output,
        "transmit_loudness_sample_count={}",
        loudness.map_or(0, |value| value.sample_count)
    )
    .expect("writing to String cannot fail");
    writeln!(
        output,
        "transmit_loudness_valid={}",
        u8::from(loudness.is_some_and(|value| value.integrated_lufs.is_some()))
    )
    .expect("writing to String cannot fail");
    for (key, value) in [
        (
            "transmit_integrated_lufs",
            loudness.and_then(|value| value.integrated_lufs),
        ),
        (
            "transmit_true_peak_dbtp",
            loudness.and_then(|value| value.true_peak_dbtp),
        ),
    ] {
        match value {
            Some(value) => writeln!(output, "{key}={value:.2}"),
            None => writeln!(output, "{key}=unavailable"),
        }
        .expect("writing to String cannot fail");
    }
    output
}

/// Physical-only native station measurements; controller rings remain with the controller.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UrpNativePhysicalStatus {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// Nonzero while the physical station is started.
    pub running: u32,
    /// Latest raw receive peak.
    pub receive_input_peak: f32,
    /// Latest raw receive RMS.
    pub receive_input_rms: f32,
    /// Latest processed receive peak.
    pub receive_output_peak: f32,
    /// Latest processed receive RMS.
    pub receive_output_rms: f32,
    /// Raw receive samples at a hardware rail in the latest callback.
    pub receive_input_rail_samples: u64,
    /// Processed receive samples at a hardware rail in the latest callback.
    pub receive_output_rail_samples: u64,
    /// Post-decoder-gain CTCSS half peak-to-peak level as normalized PCM.
    pub receive_ctcss_decoder_peak: f32,
    /// Latest compatibility discriminator-noise measurement.
    pub receive_rssi_peak: i32,
    /// Nonzero when the latest callback completed an RSSI integration window.
    pub receive_rssi_updated: u32,
    /// Latest post-processing transmitter-program peak.
    pub transmit_program_peak: f32,
    /// Latest post-processing transmitter-program RMS.
    pub transmit_program_rms: f32,
    /// Latest routed transmitter peak.
    pub transmit_output_peak: f32,
    /// Latest routed transmitter RMS.
    pub transmit_output_rms: f32,
    /// Cumulative PortAudio input overflows.
    pub input_overflow_count: u64,
    /// Cumulative PortAudio output underflows.
    pub output_underflow_count: u64,
    /// Cumulative raw input samples at or beyond full scale.
    pub input_clip_sample_count: u64,
    /// Cumulative output samples at or beyond full scale.
    pub output_clip_sample_count: u64,
    /// Most recent audio-callback duration in nanoseconds.
    pub callback_last_duration_ns: u64,
    /// Maximum audio-callback duration in nanoseconds.
    pub callback_max_duration_ns: u64,
    /// Most recent callback start-gap excess in nanoseconds.
    pub callback_last_start_delay_ns: u64,
    /// Maximum callback start-gap excess in nanoseconds.
    pub callback_max_start_delay_ns: u64,
    /// Cumulative late callback starts.
    pub callback_late_start_count: u64,
    /// Latest input-xrun monotonic timestamp in nanoseconds.
    pub last_input_xrun_monotonic_ns: u64,
    /// Latest output-xrun monotonic timestamp in nanoseconds.
    pub last_output_xrun_monotonic_ns: u64,
    /// PortAudio input-latency estimate in seconds.
    pub input_latency_seconds: f64,
    /// PortAudio output-latency estimate in seconds.
    pub output_latency_seconds: f64,
    /// Actual hardware-stream sample rate in hertz.
    pub native_sample_rate_hz: f64,
    /// Nonzero while carrier is detected.
    pub carrier_active: u32,
    /// Nonzero while CTCSS or DCS qualification is detected.
    pub subaudible_active: u32,
    /// Nonzero while the local receiver is qualified.
    pub receiver_keyed: u32,
    /// Nonzero while logical PTT is active.
    pub logical_ptt: u32,
    /// Decoded CTCSS table index, or -1 when absent.
    pub ctcss_decode_index: i32,
    /// Nonzero while the configured DCS code is valid.
    pub dcs_valid: u32,
    /// Normalized receiver hardware mixer level.
    pub receive_mixer_level: u32,
    /// Normalized transmitter-A hardware mixer level.
    pub transmit_a_mixer_level: u32,
    /// Normalized transmitter-B hardware mixer level.
    pub transmit_b_mixer_level: u32,
}

impl UrpNativePhysicalStatus {
    /// Render stable measurement keys consumed by the shared tuner.
    pub fn text(&self) -> String {
        let mut text = String::new();
        let _ = writeln!(text, "running={}", self.running);
        let _ = writeln!(text, "receive_input_peak={}", self.receive_input_peak);
        let _ = writeln!(text, "receive_input_rms={}", self.receive_input_rms);
        let _ = writeln!(text, "receive_output_peak={}", self.receive_output_peak);
        let _ = writeln!(text, "receive_output_rms={}", self.receive_output_rms);
        let _ = writeln!(
            text,
            "receive_input_rail_samples={}",
            self.receive_input_rail_samples
        );
        let _ = writeln!(
            text,
            "receive_output_rail_samples={}",
            self.receive_output_rail_samples
        );
        let _ = writeln!(
            text,
            "receive_ctcss_decoder_peak={}",
            self.receive_ctcss_decoder_peak
        );
        let _ = writeln!(text, "receive_rssi_peak={}", self.receive_rssi_peak);
        let _ = writeln!(text, "receive_rssi_updated={}", self.receive_rssi_updated);
        let _ = writeln!(text, "transmit_program_peak={}", self.transmit_program_peak);
        let _ = writeln!(text, "transmit_program_rms={}", self.transmit_program_rms);
        let _ = writeln!(text, "transmit_output_peak={}", self.transmit_output_peak);
        let _ = writeln!(text, "transmit_output_rms={}", self.transmit_output_rms);
        let _ = writeln!(text, "input_overflow_count={}", self.input_overflow_count);
        let _ = writeln!(
            text,
            "output_underflow_count={}",
            self.output_underflow_count
        );
        let _ = writeln!(
            text,
            "input_clip_sample_count={}",
            self.input_clip_sample_count
        );
        let _ = writeln!(
            text,
            "output_clip_sample_count={}",
            self.output_clip_sample_count
        );
        let _ = writeln!(
            text,
            "callback_last_duration_ns={}",
            self.callback_last_duration_ns
        );
        let _ = writeln!(
            text,
            "callback_max_duration_ns={}",
            self.callback_max_duration_ns
        );
        let _ = writeln!(
            text,
            "callback_last_start_delay_ns={}",
            self.callback_last_start_delay_ns
        );
        let _ = writeln!(
            text,
            "callback_max_start_delay_ns={}",
            self.callback_max_start_delay_ns
        );
        let _ = writeln!(
            text,
            "callback_late_start_count={}",
            self.callback_late_start_count
        );
        let _ = writeln!(
            text,
            "last_input_xrun_monotonic_ns={}",
            self.last_input_xrun_monotonic_ns
        );
        let _ = writeln!(
            text,
            "last_output_xrun_monotonic_ns={}",
            self.last_output_xrun_monotonic_ns
        );
        let _ = writeln!(text, "input_latency_seconds={}", self.input_latency_seconds);
        let _ = writeln!(
            text,
            "output_latency_seconds={}",
            self.output_latency_seconds
        );
        let _ = writeln!(text, "native_sample_rate_hz={}", self.native_sample_rate_hz);
        let _ = writeln!(text, "carrier_active={}", self.carrier_active);
        let _ = writeln!(text, "subaudible_active={}", self.subaudible_active);
        let _ = writeln!(text, "receiver_keyed={}", self.receiver_keyed);
        let _ = writeln!(text, "logical_ptt={}", self.logical_ptt);
        let _ = writeln!(text, "ctcss_decode_index={}", self.ctcss_decode_index);
        let _ = writeln!(text, "dcs_valid={}", self.dcs_valid);
        let _ = writeln!(text, "receive_mixer_level={}", self.receive_mixer_level);
        let _ = writeln!(
            text,
            "transmit_a_mixer_level={}",
            self.transmit_a_mixer_level
        );
        let _ = writeln!(
            text,
            "transmit_b_mixer_level={}",
            self.transmit_b_mixer_level
        );
        text
    }
}

/// Copy existing physical observations and current mixer values without recomputing meters.
pub(crate) fn collect_physical_status(
    maintenance: &mut HardwareMaintenance,
    diagnostics: HardwareStationDiagnostics,
    radio: &SessionSnapshot,
    running: bool,
) -> Result<UrpNativePhysicalStatus, HardwareStationError> {
    let mut status = UrpNativePhysicalStatus {
        struct_size: size_of::<UrpNativePhysicalStatus>() as u32,
        abi_version: ABI_VERSION,
        running: u32::from(running),
        ctcss_decode_index: -1,
        ..UrpNativePhysicalStatus::default()
    };
    status.receive_input_peak = radio.receive_input_peak;
    status.receive_input_rms = radio.receive_input_rms;
    status.receive_output_peak = radio.receive_output_peak;
    status.receive_output_rms = radio.receive_output_rms;
    status.receive_input_rail_samples = diagnostics.receive.input_rail_samples;
    status.receive_output_rail_samples = diagnostics.receive.output_rail_samples;
    status.receive_ctcss_decoder_peak = diagnostics.receive.ctcss_decoder_peak;
    status.receive_rssi_peak = i32::from(diagnostics.receive.rssi_peak);
    status.receive_rssi_updated = u32::from(diagnostics.receive.rssi_updated);
    status.transmit_program_peak = radio.transmit_program_peak;
    status.transmit_program_rms = radio.transmit_program_rms;
    status.transmit_output_peak = radio.transmit_output_peak;
    status.transmit_output_rms = radio.transmit_output_rms;
    status.input_overflow_count = diagnostics.runtime.audio.input_overflow_count;
    status.output_underflow_count = diagnostics.runtime.audio.output_underflow_count;
    status.input_clip_sample_count = diagnostics.runtime.audio.input_clip_sample_count;
    status.output_clip_sample_count = diagnostics.runtime.audio.output_clip_sample_count;
    status.callback_last_duration_ns = diagnostics.runtime.audio.callback_last_duration_ns;
    status.callback_max_duration_ns = diagnostics.runtime.audio.callback_max_duration_ns;
    status.callback_last_start_delay_ns = diagnostics.runtime.audio.callback_last_start_delay_ns;
    status.callback_max_start_delay_ns = diagnostics.runtime.audio.callback_max_start_delay_ns;
    status.callback_late_start_count = diagnostics.runtime.audio.callback_late_start_count;
    status.last_input_xrun_monotonic_ns = diagnostics.runtime.audio.last_input_xrun_monotonic_ns;
    status.last_output_xrun_monotonic_ns = diagnostics.runtime.audio.last_output_xrun_monotonic_ns;
    status.input_latency_seconds = diagnostics.timing.input_latency_seconds;
    status.output_latency_seconds = diagnostics.timing.output_latency_seconds;
    status.native_sample_rate_hz = diagnostics.timing.sample_rate_hz;
    status.carrier_active = u32::from(radio.carrier_active);
    status.subaudible_active = u32::from(radio.subaudible_active);
    status.receiver_keyed = u32::from(radio.receiver_keyed);
    status.logical_ptt = u32::from(radio.logical_ptt);
    status.ctcss_decode_index = radio.ctcss_decoded.map_or(-1, |tone| i32::from(tone.get()));
    status.dcs_valid = u32::from(radio.dcs_valid);
    [
        status.receive_mixer_level,
        status.transmit_a_mixer_level,
        status.transmit_b_mixer_level,
    ] = maintenance.mixer_levels()?;
    Ok(status)
}

/// Project the physical subset into the published ASL status record.
pub(crate) fn copy_physical_status(from: &UrpNativePhysicalStatus, to: &mut UrpAstChannelStatus) {
    to.running = from.running;
    to.receive_input_peak = from.receive_input_peak;
    to.receive_input_rms = from.receive_input_rms;
    to.receive_output_peak = from.receive_output_peak;
    to.receive_output_rms = from.receive_output_rms;
    to.receive_input_rail_samples = from.receive_input_rail_samples;
    to.receive_output_rail_samples = from.receive_output_rail_samples;
    to.receive_ctcss_decoder_peak = from.receive_ctcss_decoder_peak;
    to.receive_rssi_peak = from.receive_rssi_peak;
    to.receive_rssi_updated = from.receive_rssi_updated;
    to.transmit_program_peak = from.transmit_program_peak;
    to.transmit_program_rms = from.transmit_program_rms;
    to.transmit_output_peak = from.transmit_output_peak;
    to.transmit_output_rms = from.transmit_output_rms;
    to.input_overflow_count = from.input_overflow_count;
    to.output_underflow_count = from.output_underflow_count;
    to.input_clip_sample_count = from.input_clip_sample_count;
    to.output_clip_sample_count = from.output_clip_sample_count;
    to.callback_last_duration_ns = from.callback_last_duration_ns;
    to.callback_max_duration_ns = from.callback_max_duration_ns;
    to.callback_last_start_delay_ns = from.callback_last_start_delay_ns;
    to.callback_max_start_delay_ns = from.callback_max_start_delay_ns;
    to.callback_late_start_count = from.callback_late_start_count;
    to.last_input_xrun_monotonic_ns = from.last_input_xrun_monotonic_ns;
    to.last_output_xrun_monotonic_ns = from.last_output_xrun_monotonic_ns;
    to.input_latency_seconds = from.input_latency_seconds;
    to.output_latency_seconds = from.output_latency_seconds;
    to.native_sample_rate_hz = from.native_sample_rate_hz;
    to.carrier_active = from.carrier_active;
    to.subaudible_active = from.subaudible_active;
    to.receiver_keyed = from.receiver_keyed;
    to.logical_ptt = from.logical_ptt;
    to.ctcss_decode_index = from.ctcss_decode_index;
    to.dcs_valid = from.dcs_valid;
    to.receive_mixer_level = from.receive_mixer_level;
    to.transmit_a_mixer_level = from.transmit_a_mixer_level;
    to.transmit_b_mixer_level = from.transmit_b_mixer_level;
}

/// Validation or serialized hardware failure from a shared maintenance operation.
pub(crate) enum CommandError {
    Status(Status),
    Hardware(HardwareStationError),
}
impl From<Status> for CommandError {
    fn from(error: Status) -> Self {
        Self::Status(error)
    }
}
impl From<HardwareStationError> for CommandError {
    fn from(error: HardwareStationError) -> Self {
        Self::Hardware(error)
    }
}
impl CommandError {
    pub(crate) fn status(&self) -> Status {
        match self {
            Self::Status(status) => *status,
            Self::Hardware(_) => Status::SetupFailed,
        }
    }
}

/// Execute the existing typed tuning catalog on its unique shared hardware owner.
pub(crate) fn execute(
    maintenance: &mut HardwareMaintenance,
    configuration: &ChannelConfiguration,
    command: &mut UrpAstChannelCommand,
) -> Result<(), CommandError> {
    match command.command {
        URP_AST_COMMAND_GET_MIXER => {
            command.value = i64::from(maintenance.mixer_level(mixer(command.target)?)?);
        }
        URP_AST_COMMAND_SET_MIXER => {
            let level = u32::try_from(command.value)
                .ok()
                .filter(|value| *value <= 999)
                .ok_or(Status::InvalidArgument)?;
            maintenance.set_mixer_level(mixer(command.target)?, level)?;
        }
        URP_AST_COMMAND_SET_TEST_TONE => {
            maintenance.set_calibrated_test_tone(switch(command.value)?);
        }
        URP_AST_COMMAND_READ_EEPROM => {
            let image = maintenance.read_eeprom()?;
            command.eeprom_words = image.words;
            command.flags = eeprom_flags(&image);
        }
        URP_AST_COMMAND_WRITE_EEPROM => {
            let mut image = EepromImage {
                checksum_valid: command.flags & URP_AST_EEPROM_CHECKSUM_VALID != 0,
                magic_valid: command.flags & URP_AST_EEPROM_MAGIC_VALID != 0,
                words: command.eeprom_words,
            };
            maintenance.write_eeprom(&mut image)?;
            command.eeprom_words = image.words;
            command.flags = eeprom_flags(&image);
        }
        URP_AST_COMMAND_SET_CTCSS_INHIBIT => {
            maintenance.set_ctcss_inhibited(switch(command.value)?);
        }
        URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE => {
            maintenance.set_subaudible_override(switch(command.value)?);
        }
        URP_AST_COMMAND_SAVE_TUNING_EEPROM => {
            maintenance.save_current_tuning_to_eeprom(configuration)?;
            command.value = 1;
        }
        _ => return Err(CommandError::Status(Status::InvalidArgument)),
    }
    Ok(())
}

/// Emit one physical snapshot with the shared tuner's stable text keys.
pub(crate) unsafe extern "C" fn native_status_text(
    native: *mut std::ffi::c_void,
    sink: Option<crate::UrpNativeProcessingDiagnostic>,
    context: *mut std::ffi::c_void,
) -> std::ffi::c_int {
    crate::ffi_status(|| {
        let sink = sink.ok_or(Status::InvalidArgument)?;
        // SAFETY: the caller retains and serializes the live native station.
        let status = unsafe { crate::native::snapshot(native) }?;
        let text = status.text();
        // SAFETY: the sink and its context remain live for this synchronous call.
        unsafe { sink(context, text.as_ptr(), text.len() as u32) };
        Ok(())
    })
}
