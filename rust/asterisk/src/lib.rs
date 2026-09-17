//! Rust-owned USBRadioPlus host for Asterisk.
//!
//! The metadata-only C module selects released provider descriptors and
//! forwards load, reload, and unload. This crate owns Asterisk channel,
//! audiohook, CLI, configuration, media, and controller lifecycles.

#![deny(warnings)]

// These are generated declarations/bitfield accessors for external Asterisk
// headers, not owned implementation. Safety obligations are documented at our
// call sites; bindgen does not generate Clippy's per-block safety comments.
#[allow(
    warnings,
    missing_docs,
    unsafe_op_in_unsafe_fn,
    clippy::all,
    clippy::undocumented_unsafe_blocks
)]
mod ffi {
    include!(concat!(env!("OUT_DIR"), "/asterisk.rs"));
}

mod host;

use std::cell::UnsafeCell;
use std::collections::HashSet;
use std::ffi::{c_char, c_int, c_void};
use std::mem::{align_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::{self, NonNull};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};

use usbradioplus_asl3::{
    AsteriskPcmMode, ControllerPcmFrame, ControllerReloadState, CtcssTone, DeliveryAction,
    DtmfAction, DtmfEvent, DtmfEventKind, EchoConfiguration, LatestHandoff, LinkDirection,
    LinkObservation, PreparedLink, Producer,
};
use usbradioplus_audio::AudioProvider;
use usbradioplus_core::{AsteriskConfig, JitterBufferImplementation};
use usbradioplus_driver::{
    ConfigurationRegistry, ControllerConfiguration, DriverConfiguration, HardwareInput,
    HardwareInputEvent, HardwareMixer, HardwarePreflight, HardwareStation, HardwareTransientState,
    LinkProcessingFactory, StationFactory, StationProviders,
};
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_gpio::{EepromImage, GpioAdapter};
use usbradioplus_radio::RadioProvider;
use usbradioplus_ring::RingProvider;
use usbradioplus_rnnoise::DenoiseProvider;
use usbradioplus_samplerate::SampleRateAdapter;
/// Private initial-alpha direct callback descriptor; contexts remain caller-owned.
pub use usbradioplus_station::DirectCallbacks as UrpAstDirectCallbacks;
use usbradioplus_station::{StationMedia, hardware_plan};

const ABI_VERSION: u32 = 4;
/// Private positive Asterisk setoption ID (ASCII RPAD), passed with block zero.
pub const URP_AST_OPTION_DIRECT_CALLBACKS: c_int = 0x5250_4144;
const MAXIMUM_FRAME_COUNT: u32 = 960;
const HANDOFF_SLOTS: usize = 3;
const CAPABILITY: &std::ffi::CStr = c"usbradioplus.asterisk";

/// Successful C ABI operation.
pub const URP_AST_OK: c_int = 0;
/// A pointer, structure, enum, string, or PCM span was invalid.
pub const URP_AST_INVALID_ARGUMENT: c_int = -1;
/// A required descriptor or function-table ABI was incompatible.
pub const URP_AST_INCOMPATIBLE_ABI: c_int = -2;
/// The supplied USBRadioPlus configuration was invalid.
pub const URP_AST_INVALID_CONFIGURATION: c_int = -3;
/// The requested configured channel was not found.
pub const URP_AST_CHANNEL_NOT_FOUND: c_int = -4;
/// The requested configured channel is already reserved.
pub const URP_AST_CHANNEL_BUSY: c_int = -5;
/// Station planning or provider setup failed.
pub const URP_AST_SETUP_FAILED: c_int = -6;
/// The requested operation requires runtime hardware binding not yet active.
pub const URP_AST_NOT_READY: c_int = -7;
/// An injected Asterisk operation failed.
pub const URP_AST_ASTERISK_FAILURE: c_int = -8;
/// A Rust panic was contained at the C ABI boundary.
pub const URP_AST_INTERNAL_FAILURE: c_int = -9;

/// Fixed 8 kHz interface used by `app_rpt`.
pub const URP_AST_TRANSPORT_APP_RPT: u32 = 1;
/// Fixed 48 kHz interface used by `rpt_advanced`.
pub const URP_AST_TRANSPORT_RPT_ADVANCED: u32 = 2;

/// Audio read from an incoming Asterisk link before controller mixing.
pub const URP_AST_LINK_DIRECTION_READ: u32 = 1;
/// Audio written toward an Asterisk link; intentionally bypassed by link processing.
pub const URP_AST_LINK_DIRECTION_WRITE: u32 = 2;

/// Fixed Asterisk jitter-buffer implementation.
pub const URP_AST_JITTER_FIXED: u32 = 1;
/// Adaptive Asterisk jitter-buffer implementation.
pub const URP_AST_JITTER_ADAPTIVE: u32 = 2;

/// No DTMF event was detected in a voice frame.
pub const URP_AST_DTMF_NONE: u32 = 0;
/// Start of one detected DTMF digit.
pub const URP_AST_DTMF_BEGIN: u32 = 1;
/// End of one detected DTMF digit.
pub const URP_AST_DTMF_END: u32 = 2;

/// Queue a radio-key control frame.
pub const URP_AST_CONTROL_RECEIVER_KEY: u32 = 1;
/// Queue a radio-unkey control frame.
pub const URP_AST_CONTROL_RECEIVER_UNKEY: u32 = 2;
/// Queue a DTMF-begin frame.
pub const URP_AST_CONTROL_DTMF_BEGIN: u32 = 3;
/// Queue a DTMF-end frame.
pub const URP_AST_CONTROL_DTMF_END: u32 = 4;
/// Queue an Asterisk null frame in place of a muted pseudo digit.
pub const URP_AST_CONTROL_NULL: u32 = 5;

/// Informational adapter log message.
pub const URP_AST_LOG_INFO: u32 = 1;
/// Configuration or runtime warning.
pub const URP_AST_LOG_WARNING: u32 = 2;
/// Operation failure.
pub const URP_AST_LOG_ERROR: u32 = 3;

/// Read one hardware mixer level into the command value.
pub const URP_AST_COMMAND_GET_MIXER: u32 = 1;
/// Set one hardware mixer level from the command value.
pub const URP_AST_COMMAND_SET_MIXER: u32 = 2;
/// Enable or disable the calibrated transmitter test tone.
pub const URP_AST_COMMAND_SET_TEST_TONE: u32 = 3;
/// Read the complete hardware tuning EEPROM image.
pub const URP_AST_COMMAND_READ_EEPROM: u32 = 4;
/// Write the complete hardware tuning EEPROM image.
pub const URP_AST_COMMAND_WRITE_EEPROM: u32 = 5;
/// Temporarily inhibit or restore transmit CTCSS generation.
pub const URP_AST_COMMAND_SET_CTCSS_INHIBIT: u32 = 7;
/// Temporarily bypass or restore receive subaudible qualification.
pub const URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE: u32 = 8;
/// Persist the current live mixer, CTCSS, and squelch tuning to EEPROM.
pub const URP_AST_COMMAND_SAVE_TUNING_EEPROM: u32 = 9;

/// Normalized CTCSS decoder peak used by the established 2,400-code calibration.
pub const URP_AST_CTCSS_CALIBRATION_TARGET: f32 = usbradioplus_radio::CTCSS_CALIBRATION_TARGET;

/// Receiver ADC mixer.
pub const URP_AST_MIXER_RECEIVE: u32 = 1;
/// Transmitter DAC A mixer.
pub const URP_AST_MIXER_TRANSMIT_A: u32 = 2;
/// Transmitter DAC B mixer.
pub const URP_AST_MIXER_TRANSMIT_B: u32 = 3;

/// EEPROM image has a valid checksum.
pub const URP_AST_EEPROM_CHECKSUM_VALID: u32 = 1 << 0;
/// EEPROM image has the established tuning magic value.
pub const URP_AST_EEPROM_MAGIC_VALID: u32 = 1 << 1;

#[derive(Clone, Copy)]
#[repr(C)]
struct AbiHeader {
    struct_size: u32,
    abi_version: u32,
}

/// Result storage supplied to the injected DTMF analyzer.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstDtmfResult {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// One of `URP_AST_DTMF_*`.
    pub event_kind: u32,
    /// ASCII digit for a begin or end event.
    pub digit: u8,
    /// Reserved; callers initialize these bytes to zero.
    pub reserved: [u8; 7],
}

impl Default for UrpAstDtmfResult {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            event_kind: URP_AST_DTMF_NONE,
            digit: 0,
            reserved: [0; 7],
        }
    }
}

/// Queue one complete signed-linear Asterisk frame.
pub type UrpAstQueueVoice = unsafe extern "C" fn(
    application_context: *mut c_void,
    channel_context: *mut c_void,
    samples: *const i16,
    sample_count: u32,
    sample_rate_hz: u32,
) -> c_int;

/// Queue one translated Asterisk control frame.
pub type UrpAstQueueControl = unsafe extern "C" fn(
    application_context: *mut c_void,
    channel_context: *mut c_void,
    kind: u32,
    value: i32,
    duration_ms: u64,
) -> c_int;

/// Queue one byte-counted Asterisk text frame.
pub type UrpAstQueueText = unsafe extern "C" fn(
    application_context: *mut c_void,
    channel_context: *mut c_void,
    text: *const u8,
    text_length: u32,
) -> c_int;

/// Analyze one voice frame using Asterisk's unavoidable DTMF detector.
///
/// Return zero when no event was detected, a positive value after filling
/// `result`, or a negative value on failure.
pub type UrpAstAnalyzeDtmf = unsafe extern "C" fn(
    application_context: *mut c_void,
    channel_context: *mut c_void,
    samples: *mut i16,
    sample_count: u32,
    sample_rate_hz: u32,
    result: *mut UrpAstDtmfResult,
) -> c_int;

/// Read monotonic elapsed time in milliseconds.
pub type UrpAstMonotonicMilliseconds =
    unsafe extern "C" fn(application_context: *mut c_void) -> u64;

/// Emit one byte-counted message outside real-time callbacks.
pub type UrpAstLog = unsafe extern "C" fn(
    application_context: *mut c_void,
    level: u32,
    message: *const u8,
    message_length: u32,
);

/// Asterisk-owned operations injected into the Rust adapter.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstOperations {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// Opaque context returned to every operation.
    pub application_context: *mut c_void,
    /// Queue one complete voice frame.
    pub queue_voice: Option<UrpAstQueueVoice>,
    /// Queue one translated control frame.
    pub queue_control: Option<UrpAstQueueControl>,
    /// Queue one translated text frame.
    pub queue_text: Option<UrpAstQueueText>,
    /// Invoke Asterisk's DTMF analyzer.
    pub analyze_dtmf: Option<UrpAstAnalyzeDtmf>,
    /// Read Asterisk's monotonic clock.
    pub monotonic_milliseconds: Option<UrpAstMonotonicMilliseconds>,
    /// Emit a non-real-time diagnostic.
    pub log: Option<UrpAstLog>,
}

/// Process-lifetime descriptors for the selected product composition.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstProviderManifest {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// Released FFmpeg graph adapter descriptor.
    pub ffmpeg: *const c_void,
    /// Released RNNoise adapter descriptor.
    pub rnnoise: *const c_void,
    /// Released F32 rate-adjusting ring descriptor.
    pub ring: *const c_void,
    /// Released radio-core descriptor.
    pub radio: *const c_void,
    /// Released libsamplerate adapter descriptor.
    pub samplerate: *const c_void,
    /// Released PortAudio/ALSA adapter descriptor.
    pub audio: *const c_void,
    /// Released CM119/parallel GPIO adapter descriptor.
    pub gpio: *const c_void,
}

/// Arguments for constructing one Rust-owned driver generation.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstDriverCreateArgs {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// UTF-8 configuration source name; not NUL terminated.
    pub config_source: *const u8,
    /// Byte length of `config_source`.
    pub config_source_length: u32,
    /// UTF-8 configuration document; not NUL terminated.
    pub config_text: *const u8,
    /// Byte length of `config_text`.
    pub config_text_length: u32,
    /// UTF-8 path to the installed AGC LADSPA object; not NUL terminated.
    pub agc_plugin_path: *const u8,
    /// Byte length of `agc_plugin_path`.
    pub agc_plugin_path_length: u32,
    /// Required Asterisk operation table.
    pub operations: *const UrpAstOperations,
    /// Complete selected provider manifest.
    pub providers: *const UrpAstProviderManifest,
}

/// Arguments for reserving one configured channel.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstChannelReserveArgs {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// UTF-8 configured channel name; not NUL terminated.
    pub channel_name: *const u8,
    /// Byte length of `channel_name`.
    pub channel_name_length: u32,
    /// One of `URP_AST_TRANSPORT_*`.
    pub transport: u32,
    /// Opaque Asterisk channel context returned to queue operations.
    pub channel_context: *mut c_void,
}

/// Resolved Asterisk jitter-buffer settings for one channel.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstJitterConfig {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// Nonzero enables Asterisk jitter buffering.
    pub enabled: u32,
    /// Maximum buffer length in milliseconds.
    pub maximum_size_ms: u32,
    /// Timestamp discontinuity which resets the buffer, in milliseconds.
    pub resync_threshold_ms: u32,
    /// One of `URP_AST_JITTER_*`.
    pub implementation: u32,
    /// Nonzero enables Asterisk jitter-buffer diagnostics.
    pub logging_enabled: u32,
    /// Nonzero forces buffering even when Asterisk considers it unnecessary.
    pub force_enabled: u32,
    /// Extra adaptive target depth in milliseconds.
    pub target_extra_ms: u32,
    /// Nonzero delays video with buffered audio.
    pub video_sync_enabled: u32,
}

/// One typed, non-real-time hardware or tuning operation.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstChannelCommand {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// One of `URP_AST_COMMAND_*`.
    pub command: u32,
    /// Command-specific target such as `URP_AST_MIXER_*`.
    pub target: u32,
    /// Command input or result value.
    pub value: i64,
    /// Command-specific input or result flags.
    pub flags: u32,
    /// Complete physical EEPROM image for EEPROM commands.
    pub eeprom_words: [u16; 64],
}

/// Best-effort typed status for one active channel.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct UrpAstChannelStatus {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// One of `URP_AST_TRANSPORT_*`.
    pub transport: u32,
    /// Nonzero while the physical station is started.
    pub running: u32,
    /// Nonzero while legacy echo is enabled.
    pub echo_enabled: u32,
    /// Nonzero while Asterisk DTMF analysis is enabled.
    pub dtmf_enabled: u32,
    /// Pending receive-to-Asterisk handoff frames.
    pub receive_handoff_available: u64,
    /// Receive-to-Asterisk handoff frames discarded to bound latency.
    pub receive_handoff_discarded: u64,
    /// Pending Asterisk-to-program handoff frames.
    pub program_handoff_available: u64,
    /// Asterisk-to-program frames discarded to bound latency.
    pub program_handoff_discarded: u64,
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
    /// Latest program-ring occupancy in PCM frames.
    pub ring_occupancy_frames: u32,
    /// Program-ring protected reserve in PCM frames.
    pub ring_reserve_frames: u32,
    /// Program-ring target in PCM frames.
    pub ring_target_frames: u32,
    /// Allocated program-ring capacity in PCM frames.
    pub ring_capacity_frames: u32,
    /// Current program-ring input-to-output conversion ratio.
    pub ring_ratio: f64,
    /// Program output samples absent at the ring.
    pub ring_underrun_samples: u64,
    /// Program input samples discarded by a full ring.
    pub ring_overrun_samples: u64,
    /// Program output samples supplied by concealment.
    pub ring_concealment_samples: u64,
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

/// Lock-free processing counters for one attached incoming-link graph.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct UrpAstLinkObservation {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal the adapter ABI version.
    pub abi_version: u32,
    /// Successfully processed blocks.
    pub processed_blocks: u64,
    /// Frames bypassed because their direction, rate, or size did not match.
    pub bypassed_blocks: u64,
    /// Graph failures which retained the original PCM.
    pub failed_blocks: u64,
}

/// Construct a Rust-owned driver from a complete configuration and manifest.
pub type UrpAstDriverCreate =
    unsafe extern "C" fn(args: *const UrpAstDriverCreateArgs, output: *mut *mut c_void) -> c_int;
/// Stage a validated configuration generation for a serialized reload transaction.
pub type UrpAstDriverReload = unsafe extern "C" fn(
    driver: *mut c_void,
    source: *const u8,
    source_length: u32,
    text: *const u8,
    text_length: u32,
) -> c_int;
/// Publish or discard the staged driver generation after channel and link adoption.
pub type UrpAstDriverReloadFinish = unsafe extern "C" fn(driver: *mut c_void, commit: u32) -> c_int;
/// Copy one configured channel name by deterministic zero-based index.
pub type UrpAstDriverChannelName = unsafe extern "C" fn(
    driver: *mut c_void,
    index: u32,
    output: *mut u8,
    output_capacity: u32,
    output_length: *mut u32,
) -> c_int;
/// Copy the currently selected tuning channel name.
pub type UrpAstDriverActiveChannel = unsafe extern "C" fn(
    driver: *mut c_void,
    output: *mut u8,
    output_capacity: u32,
    output_length: *mut u32,
) -> c_int;
/// Select one reserved channel for subsequent tuning operations.
pub type UrpAstDriverSetActiveChannel = unsafe extern "C" fn(
    driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
) -> c_int;
/// Destroy one driver handle.
pub type UrpAstDriverDestroy = unsafe extern "C" fn(driver: *mut c_void);
/// Prepare one negotiated-rate incoming-link graph outside its audiohook callback.
pub type UrpAstLinkPrepare = unsafe extern "C" fn(
    driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    output: *mut *mut c_void,
) -> c_int;
/// Prepare one incoming-link graph against the staged driver generation.
pub type UrpAstLinkPrepareReload = UrpAstLinkPrepare;
/// Process one incoming-link signed-linear frame in place without blocking or allocating.
pub type UrpAstLinkProcess = unsafe extern "C" fn(
    link: *mut c_void,
    direction: u32,
    sample_rate_hz: u32,
    samples: *mut i16,
    sample_count: u32,
) -> c_int;
/// Copy one best-effort link-processing observation.
pub type UrpAstLinkObserve =
    unsafe extern "C" fn(link: *mut c_void, output: *mut UrpAstLinkObservation) -> c_int;
/// Destroy one detached and quiesced incoming-link graph.
pub type UrpAstLinkDestroy = unsafe extern "C" fn(link: *mut c_void);
/// Reserve and prepare one named channel.
pub type UrpAstChannelReserve = unsafe extern "C" fn(
    driver: *mut c_void,
    args: *const UrpAstChannelReserveArgs,
    output: *mut *mut c_void,
) -> c_int;
/// Start one fully bound channel.
pub type UrpAstChannelStart = unsafe extern "C" fn(channel: *mut c_void) -> c_int;
/// Copy borrowed native callbacks into a reserved, not-yet-started Advanced channel.
pub type UrpAstChannelSetDirectCallbacks =
    unsafe extern "C" fn(channel: *mut c_void, callbacks: *const UrpAstDirectCallbacks) -> c_int;
/// Stop one channel; repeated calls are harmless.
pub type UrpAstChannelStop = unsafe extern "C" fn(channel: *mut c_void) -> c_int;
/// Prepare one live channel against the staged driver generation.
pub type UrpAstChannelReloadPrepare = unsafe extern "C" fn(channel: *mut c_void) -> c_int;
/// Adopt one prepared live channel while retaining its prior generation for rollback.
pub type UrpAstChannelReloadActivate = unsafe extern "C" fn(channel: *mut c_void) -> c_int;
/// Commit detached rollback ownership, or roll back on the channel taskprocessor.
pub type UrpAstChannelReloadFinish =
    unsafe extern "C" fn(channel: *mut c_void, commit: u32) -> c_int;
/// Append one complete Asterisk voice frame to the station program ring.
pub type UrpAstChannelWriteVoice =
    unsafe extern "C" fn(channel: *mut c_void, samples: *const i16, sample_count: u32) -> c_int;
/// Submit one Asterisk radio-control text message.
pub type UrpAstChannelWriteText =
    unsafe extern "C" fn(channel: *mut c_void, text: *const u8, text_length: u32) -> c_int;
/// Publish Asterisk's RADIO_KEY or RADIO_UNKEY request.
pub type UrpAstChannelSetTransmit =
    unsafe extern "C" fn(channel: *mut c_void, keyed: u32, forced_ctcss_tenths_hz: u32) -> c_int;
/// Enable or disable retained app_rpt DTMF analysis.
pub type UrpAstChannelSetDtmf = unsafe extern "C" fn(channel: *mut c_void, enabled: u32) -> c_int;
/// Enable or disable retained app_rpt echo recording and playback.
pub type UrpAstChannelSetEcho = unsafe extern "C" fn(channel: *mut c_void, enabled: u32) -> c_int;
/// Copy the channel's resolved jitter-buffer settings to caller storage.
pub type UrpAstChannelGetJitterConfig =
    unsafe extern "C" fn(channel: *mut c_void, output: *mut UrpAstJitterConfig) -> c_int;
/// Execute one typed, non-real-time station primitive.
pub type UrpAstChannelCommandFn =
    unsafe extern "C" fn(channel: *mut c_void, command: *mut UrpAstChannelCommand) -> c_int;
/// Copy one best-effort typed station status snapshot.
pub type UrpAstChannelGetStatus =
    unsafe extern "C" fn(channel: *mut c_void, output: *mut UrpAstChannelStatus) -> c_int;
/// Deliver currently published station frames through injected operations.
pub type UrpAstChannelService = unsafe extern "C" fn(channel: *mut c_void) -> c_int;
/// Release one channel reservation and its prepared resources.
pub type UrpAstChannelDestroy = unsafe extern "C" fn(channel: *mut c_void);

/// Versioned Asterisk-entry adapter descriptor.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UrpAstDescriptor {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Adapter ABI version.
    pub abi_version: u32,
    /// NUL-terminated capability name.
    pub capability_name: *const c_char,
    /// Construct a driver.
    pub driver_create: Option<UrpAstDriverCreate>,
    /// Stage configuration for a reload transaction.
    pub driver_reload: Option<UrpAstDriverReload>,
    /// Publish or discard staged configuration.
    pub driver_reload_finish: Option<UrpAstDriverReloadFinish>,
    /// Enumerate configured channel names.
    pub driver_channel_name: Option<UrpAstDriverChannelName>,
    /// Read the selected tuning channel.
    pub driver_active_channel: Option<UrpAstDriverActiveChannel>,
    /// Select a reserved tuning channel.
    pub driver_set_active_channel: Option<UrpAstDriverSetActiveChannel>,
    /// Destroy a driver.
    pub driver_destroy: Option<UrpAstDriverDestroy>,
    /// Prepare an incoming-link graph.
    pub link_prepare: Option<UrpAstLinkPrepare>,
    /// Prepare an incoming-link graph against staged configuration.
    pub link_prepare_reload: Option<UrpAstLinkPrepareReload>,
    /// Process an incoming-link frame.
    pub link_process: Option<UrpAstLinkProcess>,
    /// Read incoming-link counters.
    pub link_observe: Option<UrpAstLinkObserve>,
    /// Destroy an incoming-link graph.
    pub link_destroy: Option<UrpAstLinkDestroy>,
    /// Reserve and prepare a channel.
    pub channel_reserve: Option<UrpAstChannelReserve>,
    /// Start a channel.
    pub channel_start: Option<UrpAstChannelStart>,
    /// Stop a channel.
    pub channel_stop: Option<UrpAstChannelStop>,
    /// Prepare a channel reload.
    pub channel_reload_prepare: Option<UrpAstChannelReloadPrepare>,
    /// Adopt a prepared channel reload.
    pub channel_reload_activate: Option<UrpAstChannelReloadActivate>,
    /// Commit or roll back a channel reload.
    pub channel_reload_finish: Option<UrpAstChannelReloadFinish>,
    /// Submit a voice frame.
    pub channel_write_voice: Option<UrpAstChannelWriteVoice>,
    /// Submit a text frame.
    pub channel_write_text: Option<UrpAstChannelWriteText>,
    /// Publish transmitter key state.
    pub channel_set_transmit: Option<UrpAstChannelSetTransmit>,
    /// Configure DTMF detection.
    pub channel_set_dtmf: Option<UrpAstChannelSetDtmf>,
    /// Configure retained legacy echo.
    pub channel_set_echo: Option<UrpAstChannelSetEcho>,
    /// Attach direct native callbacks before the first station start.
    pub channel_set_direct_callbacks: Option<UrpAstChannelSetDirectCallbacks>,
    /// Read resolved jitter-buffer settings.
    pub channel_get_jitter_config: Option<UrpAstChannelGetJitterConfig>,
    /// Execute one typed tuning or hardware primitive.
    pub channel_command: Option<UrpAstChannelCommandFn>,
    /// Read one typed status snapshot.
    pub channel_get_status: Option<UrpAstChannelGetStatus>,
    /// Deliver pending frames.
    pub channel_service: Option<UrpAstChannelService>,
    /// Release a channel.
    pub channel_destroy: Option<UrpAstChannelDestroy>,
}

// SAFETY: the descriptor and every referenced function and string are static.
unsafe impl Sync for UrpAstDescriptor {}

static DESCRIPTOR: UrpAstDescriptor = UrpAstDescriptor {
    struct_size: size_of::<UrpAstDescriptor>() as u32,
    abi_version: ABI_VERSION,
    capability_name: CAPABILITY.as_ptr(),
    driver_create: Some(driver_create),
    driver_reload: Some(driver_reload),
    driver_reload_finish: Some(driver_reload_finish),
    driver_channel_name: Some(driver_channel_name),
    driver_active_channel: Some(driver_active_channel),
    driver_set_active_channel: Some(driver_set_active_channel),
    driver_destroy: Some(driver_destroy),
    link_prepare: Some(link_prepare),
    link_prepare_reload: Some(link_prepare_reload),
    link_process: Some(link_process),
    link_observe: Some(link_observe),
    link_destroy: Some(link_destroy),
    channel_reserve: Some(channel_reserve),
    channel_start: Some(channel_start),
    channel_stop: Some(channel_stop),
    channel_reload_prepare: Some(channel_reload_prepare),
    channel_reload_activate: Some(channel_reload_activate),
    channel_reload_finish: Some(channel_reload_finish),
    channel_write_voice: Some(channel_write_voice),
    channel_write_text: Some(channel_write_text),
    channel_set_transmit: Some(channel_set_transmit),
    channel_set_dtmf: Some(channel_set_dtmf),
    channel_set_echo: Some(channel_set_echo),
    channel_set_direct_callbacks: Some(channel_set_direct_callbacks),
    channel_get_jitter_config: Some(channel_get_jitter_config),
    channel_command: Some(channel_command),
    channel_get_status: Some(channel_get_status),
    channel_service: Some(channel_service),
    channel_destroy: Some(channel_destroy),
};

/// Return the process-lifetime product operations used by the Rust host.
pub(crate) fn product_descriptor() -> &'static UrpAstDescriptor {
    &DESCRIPTOR
}

#[derive(Clone, Copy)]
struct Operations {
    application_context: usize,
    queue_voice: UrpAstQueueVoice,
    queue_control: UrpAstQueueControl,
    queue_text: UrpAstQueueText,
    analyze_dtmf: UrpAstAnalyzeDtmf,
    monotonic_milliseconds: UrpAstMonotonicMilliseconds,
    log: UrpAstLog,
}

impl Operations {
    unsafe fn from_raw(raw: *const UrpAstOperations) -> Result<Self, Status> {
        // SAFETY: the caller promises readable bytes described by the ABI header.
        let raw = unsafe { copy_abi(raw)? };
        let (
            Some(queue_voice),
            Some(queue_control),
            Some(queue_text),
            Some(analyze_dtmf),
            Some(monotonic_milliseconds),
            Some(log),
        ) = (
            raw.queue_voice,
            raw.queue_control,
            raw.queue_text,
            raw.analyze_dtmf,
            raw.monotonic_milliseconds,
            raw.log,
        )
        else {
            return Err(Status::IncompatibleAbi);
        };
        Ok(Self {
            application_context: raw.application_context as usize,
            queue_voice,
            queue_control,
            queue_text,
            analyze_dtmf,
            monotonic_milliseconds,
            log,
        })
    }

    const fn context(self) -> *mut c_void {
        self.application_context as *mut c_void
    }

    fn queue_voice(
        self,
        channel: usize,
        samples: &[i16],
        sample_rate_hz: u32,
    ) -> Result<(), Status> {
        // SAFETY: callbacks and contexts passed descriptor validation; the
        // frame remains readable for the synchronous call.
        let result = unsafe {
            (self.queue_voice)(
                self.context(),
                channel as *mut c_void,
                samples.as_ptr(),
                samples.len() as u32,
                sample_rate_hz,
            )
        };
        callback_result(result)
    }

    fn queue_control(
        self,
        channel: usize,
        kind: u32,
        value: i32,
        duration_ms: u64,
    ) -> Result<(), Status> {
        // SAFETY: callbacks and contexts passed descriptor validation.
        callback_result(unsafe {
            (self.queue_control)(
                self.context(),
                channel as *mut c_void,
                kind,
                value,
                duration_ms,
            )
        })
    }

    fn queue_text(self, channel: usize, text: &str) -> Result<(), Status> {
        // SAFETY: callbacks and contexts passed descriptor validation; the
        // byte span remains readable for the synchronous call.
        callback_result(unsafe {
            (self.queue_text)(
                self.context(),
                channel as *mut c_void,
                text.as_ptr(),
                text.len() as u32,
            )
        })
    }

    fn analyze_dtmf(
        self,
        channel: usize,
        samples: &mut [i16],
        sample_rate_hz: u32,
    ) -> Result<Option<DtmfEvent>, Status> {
        let mut result = UrpAstDtmfResult::default();
        // SAFETY: callbacks and contexts passed descriptor validation; the
        // input and output spans remain live for the synchronous call.
        let detected = unsafe {
            (self.analyze_dtmf)(
                self.context(),
                channel as *mut c_void,
                samples.as_mut_ptr(),
                samples.len() as u32,
                sample_rate_hz,
                &mut result,
            )
        };
        if detected < 0 {
            return Err(Status::AsteriskFailure);
        }
        if detected == 0 {
            return Ok(None);
        }
        if result.struct_size < size_of::<UrpAstDtmfResult>() as u32 {
            return Err(Status::AsteriskFailure);
        }
        let kind = match result.event_kind {
            URP_AST_DTMF_BEGIN => DtmfEventKind::Begin,
            URP_AST_DTMF_END => DtmfEventKind::End,
            _ => return Err(Status::AsteriskFailure),
        };
        Ok(Some(DtmfEvent {
            kind,
            digit: result.digit,
        }))
    }

    fn monotonic_milliseconds(self) -> u64 {
        // SAFETY: callback and context passed descriptor validation.
        unsafe { (self.monotonic_milliseconds)(self.context()) }
    }

    fn log(self, level: u32, message: &str) {
        // SAFETY: callback and context passed descriptor validation; the byte
        // span remains readable for the synchronous call.
        unsafe {
            (self.log)(
                self.context(),
                level,
                message.as_ptr(),
                message.len() as u32,
            )
        };
    }
}

struct DriverInner {
    configuration: ConfigurationRegistry,
    pending_reload: Mutex<Option<std::sync::Arc<DriverConfiguration>>>,
    factory: StationFactory,
    link_factory: LinkProcessingFactory,
    audio: AudioProvider,
    gpio: GpioAdapter,
    operations: Operations,
    reservations: Mutex<ReservationState>,
    station_generation: AtomicU64,
}

struct DriverHandle(DriverInner);

#[derive(Default)]
struct ReservationState {
    names: HashSet<String>,
    active: Option<String>,
    configured_active: Option<String>,
}

struct ChannelHandle {
    driver: NonNull<DriverInner>,
    reservation: String,
    channel_context: usize,
    mode: AsteriskPcmMode,
    program: UnsafeCell<Producer<ControllerPcmFrame>>,
    control: UnsafeCell<ChannelControl>,
    rollback: Mutex<Option<AdoptedChannelReload>>,
}

struct ChannelControl {
    media: Option<StationMedia>,
    preflight: Option<HardwarePreflight>,
    hardware: Option<HardwareStation>,
    program: usbradioplus_asl3::Consumer<ControllerPcmFrame>,
    voice: ControllerPcmFrame,
    jitter: UrpAstJitterConfig,
    reload: Option<PreparedChannelReload>,
    running: bool,
    direct: Option<UrpAstDirectCallbacks>,
}

struct PreparedChannelReload {
    media: StationMedia,
    preflight: HardwarePreflight,
    jitter: UrpAstJitterConfig,
}

struct AdoptedChannelReload {
    previous: PreviousChannelGeneration,
    jitter: UrpAstJitterConfig,
}

enum PreviousChannelGeneration {
    Prepared {
        media: Box<StationMedia>,
        preflight: HardwarePreflight,
    },
    Hardware {
        hardware: HardwareStation,
        transient: Option<HardwareTransientState>,
    },
}

struct RetainedControllerState {
    controller: ControllerReloadState,
    control: usbradioplus_asl3::ControlSnapshot,
}

impl ChannelHandle {
    fn driver(&self) -> &DriverInner {
        // SAFETY: the C lifecycle contract requires the driver to outlive all
        // of its channel handles.
        unsafe { self.driver.as_ref() }
    }

    fn publish_program(&self, frame: ControllerPcmFrame) {
        // SAFETY: the ABI contract assigns exactly one Asterisk voice producer
        // to this endpoint. It never aliases the disjoint control owner.
        unsafe { &mut *self.program.get() }.push(frame);
    }

    fn log_error(&self, message: &str) {
        self.driver().operations.log(URP_AST_LOG_ERROR, message);
    }
}

impl ChannelControl {
    fn controller(&mut self) -> Result<&mut usbradioplus_asl3::ControllerState, Status> {
        if let Some(hardware) = self.hardware.as_mut() {
            Ok(hardware.control().controller())
        } else {
            self.media
                .as_mut()
                .map(|media| &mut media.controller)
                .ok_or(Status::NotReady)
        }
    }
}

impl RetainedControllerState {
    fn capture(controller: &usbradioplus_asl3::ControllerState) -> Self {
        Self {
            controller: controller
                .reload_state()
                .expect("receive handoff was drained before reload-state capture"),
            control: controller.control_snapshot(),
        }
    }

    fn apply_to_controller(self, controller: &mut usbradioplus_asl3::ControllerState) {
        controller.restore_reload_state(self.controller);
    }

    fn apply_to_hardware(self, hardware: &mut HardwareStation) {
        hardware
            .control()
            .controller()
            .restore_reload_state(self.controller);
        hardware.set_transmit_request(self.control.transmit_keyed, true, self.control.forced_ctcss);
        hardware.set_subaudible_override(!self.control.receive_ctcss_enabled);
        hardware.set_ctcss_inhibited(!self.control.transmit_ctcss_enabled);
    }
}

// SAFETY: concurrent state is confined to the split lock-free program handoff
// and the control-plane-only rollback mutex. Asterisk supplies one voice
// producer and one serialized control owner; audio callbacks never take the
// rollback mutex.
unsafe impl Sync for ChannelHandle {}

impl Drop for ChannelHandle {
    fn drop(&mut self) {
        self.driver()
            .reservations
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .release(&self.reservation);
    }
}

impl ReservationState {
    fn new(configured_active: Option<String>) -> Self {
        Self {
            names: HashSet::new(),
            active: None,
            configured_active,
        }
    }

    fn reserve(&mut self, name: String) -> bool {
        if !self.names.insert(name.clone()) {
            return false;
        }
        if self.active.is_none() || self.configured_active.as_deref() == Some(&name) {
            self.active = Some(name);
        }
        true
    }

    fn release(&mut self, name: &str) {
        self.names.remove(name);
        if self.active.as_deref() == Some(name) {
            self.active = None;
        }
    }

    fn reload(&mut self, configured_active: Option<String>) {
        self.configured_active = configured_active;
        if let Some(name) = self
            .configured_active
            .as_ref()
            .filter(|name| self.names.contains(*name))
        {
            self.active = Some(name.clone());
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum Status {
    InvalidArgument,
    IncompatibleAbi,
    InvalidConfiguration,
    ChannelNotFound,
    ChannelBusy,
    SetupFailed,
    NotReady,
    AsteriskFailure,
    InternalFailure,
}

impl Status {
    const fn code(self) -> c_int {
        match self {
            Self::InvalidArgument => URP_AST_INVALID_ARGUMENT,
            Self::IncompatibleAbi => URP_AST_INCOMPATIBLE_ABI,
            Self::InvalidConfiguration => URP_AST_INVALID_CONFIGURATION,
            Self::ChannelNotFound => URP_AST_CHANNEL_NOT_FOUND,
            Self::ChannelBusy => URP_AST_CHANNEL_BUSY,
            Self::SetupFailed => URP_AST_SETUP_FAILED,
            Self::NotReady => URP_AST_NOT_READY,
            Self::AsteriskFailure => URP_AST_ASTERISK_FAILURE,
            Self::InternalFailure => URP_AST_INTERNAL_FAILURE,
        }
    }
}

unsafe extern "C" fn driver_create(
    args: *const UrpAstDriverCreateArgs,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the caller promises readable bytes described by the ABI header.
        let args = unsafe { copy_abi(args)? };
        // SAFETY: the nested pointer follows the same process-lifetime contract.
        let operations = unsafe { Operations::from_raw(args.operations)? };
        // SAFETY: these byte spans are readable for this synchronous call.
        let source = unsafe { required_utf8(args.config_source, args.config_source_length)? };
        // SAFETY: these byte spans are readable for this synchronous call.
        let text = unsafe { required_utf8(args.config_text, args.config_text_length)? };
        // SAFETY: these byte spans are readable for this synchronous call.
        let agc = unsafe { required_utf8(args.agc_plugin_path, args.agc_plugin_path_length)? };
        let configuration = DriverConfiguration::parse(source, text).map_err(|error| {
            operations.log(URP_AST_LOG_ERROR, &error.to_string());
            Status::InvalidConfiguration
        })?;
        log_configuration_warnings(operations, &configuration);
        // SAFETY: the caller promises a complete process-lifetime manifest.
        let providers = unsafe { validate_providers(args.providers)? };
        let factory =
            StationFactory::new(providers.station, agc, MAXIMUM_FRAME_COUNT).map_err(|error| {
                operations.log(URP_AST_LOG_ERROR, &error.to_string());
                Status::SetupFailed
            })?;
        let link_factory =
            LinkProcessingFactory::new(providers.station.graph, agc).map_err(|error| {
                operations.log(URP_AST_LOG_ERROR, &error.to_string());
                Status::SetupFailed
            })?;
        let configured_active = configured_active_channel(&configuration);
        let handle = Box::new(DriverHandle(DriverInner {
            configuration: ConfigurationRegistry::new(configuration),
            pending_reload: Mutex::new(None),
            factory,
            link_factory,
            audio: providers.audio,
            gpio: providers.gpio,
            operations,
            reservations: Mutex::new(ReservationState::new(configured_active)),
            station_generation: AtomicU64::new(1),
        }));
        // SAFETY: output_pointer validated this writable destination.
        unsafe { output.as_ptr().write(Box::into_raw(handle).cast()) };
        Ok(())
    })
}

unsafe extern "C" fn driver_reload(
    driver: *mut c_void,
    source: *const u8,
    source_length: u32,
    text: *const u8,
    text_length: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        // SAFETY: these byte spans are readable for this synchronous call.
        let source = unsafe { required_utf8(source, source_length)? };
        // SAFETY: these byte spans are readable for this synchronous call.
        let text = unsafe { required_utf8(text, text_length)? };
        let configuration = ConfigurationRegistry::prepare(source, text).map_err(|error| {
            driver
                .0
                .operations
                .log(URP_AST_LOG_ERROR, &error.to_string());
            Status::InvalidConfiguration
        })?;
        log_configuration_warnings(driver.0.operations, &configuration);
        let mut pending = driver
            .0
            .pending_reload
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if pending.is_some() {
            return Err(Status::ChannelBusy);
        }
        *pending = Some(configuration);
        Ok(())
    })
}

unsafe extern "C" fn driver_reload_finish(driver: *mut c_void, commit: u32) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        let commit = boolean(commit)?;
        let configuration = driver
            .0
            .pending_reload
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take()
            .ok_or(Status::NotReady)?;
        if commit {
            let configured_active = configured_active_channel(&configuration);
            driver.0.configuration.publish(configuration);
            driver
                .0
                .reservations
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .reload(configured_active);
        }
        Ok(())
    })
}

fn log_configuration_warnings(operations: Operations, configuration: &DriverConfiguration) {
    for warning in configuration.warnings() {
        operations.log(URP_AST_LOG_WARNING, &warning.to_string());
    }
}

unsafe extern "C" fn driver_channel_name(
    driver: *mut c_void,
    index: u32,
    output: *mut u8,
    output_capacity: u32,
    output_length: *mut u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        let snapshot = driver.0.configuration.snapshot();
        let channel = snapshot
            .channels()
            .get(index as usize)
            .ok_or(Status::ChannelNotFound)?;
        // SAFETY: caller supplies optional byte storage and required length storage.
        unsafe {
            write_bytes(
                channel.channel().as_bytes(),
                output,
                output_capacity,
                output_length,
            )
        }
    })
}

unsafe extern "C" fn driver_active_channel(
    driver: *mut c_void,
    output: *mut u8,
    output_capacity: u32,
    output_length: *mut u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        let selected = driver
            .0
            .reservations
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .active
            .clone()
            .ok_or(Status::ChannelNotFound)?;
        let snapshot = driver.0.configuration.snapshot();
        let channel = snapshot.channel(&selected).ok_or(Status::ChannelNotFound)?;
        // SAFETY: caller supplies optional byte storage and required length storage.
        unsafe {
            write_bytes(
                channel.channel().as_bytes(),
                output,
                output_capacity,
                output_length,
            )
        }
    })
}

unsafe extern "C" fn driver_set_active_channel(
    driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        // SAFETY: the byte span is readable for this synchronous call.
        let name = unsafe { required_utf8(channel_name, channel_name_length)? };
        let name = name.to_ascii_lowercase();
        let mut reservations = driver
            .0
            .reservations
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if !reservations.names.contains(&name) {
            return Err(Status::ChannelNotFound);
        }
        reservations.active = Some(name);
        Ok(())
    })
}

unsafe extern "C" fn driver_destroy(driver: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !driver.is_null() {
            // SAFETY: the pointer came from driver_create and is consumed once.
            drop(unsafe { Box::from_raw(driver.cast::<DriverHandle>()) });
        }
    }));
}

unsafe extern "C" fn link_prepare(
    driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        // SAFETY: the byte span is readable for this synchronous setup call.
        let name = unsafe { required_utf8(channel_name, channel_name_length)? };
        let snapshot = driver.0.configuration.snapshot();
        let link = prepare_link(
            &driver.0,
            &snapshot,
            name,
            sample_rate_hz,
            maximum_frame_count,
        )?;
        // SAFETY: output_pointer validated this writable destination.
        unsafe { output.as_ptr().write(Box::into_raw(Box::new(link)).cast()) };
        Ok(())
    })
}

unsafe extern "C" fn link_prepare_reload(
    driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        // SAFETY: the byte span is readable for this synchronous setup call.
        let name = unsafe { required_utf8(channel_name, channel_name_length)? };
        let configuration = driver
            .0
            .pending_reload
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
            .ok_or(Status::NotReady)?;
        let link = prepare_link(
            &driver.0,
            &configuration,
            name,
            sample_rate_hz,
            maximum_frame_count,
        )?;
        // SAFETY: output_pointer validated this writable destination.
        unsafe { output.as_ptr().write(Box::into_raw(Box::new(link)).cast()) };
        Ok(())
    })
}

fn prepare_link(
    driver: &DriverInner,
    configuration: &DriverConfiguration,
    name: &str,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
) -> Result<PreparedLink, Status> {
    let channel = configuration.channel(name).ok_or(Status::ChannelNotFound)?;
    if !channel.config().link.enabled {
        return Err(Status::NotReady);
    }
    driver
        .link_factory
        .prepare(&channel.config().link, sample_rate_hz, maximum_frame_count)
        .map_err(|error| {
            driver.operations.log(URP_AST_LOG_ERROR, &error.to_string());
            Status::SetupFailed
        })
}

unsafe extern "C" fn link_process(
    link: *mut c_void,
    direction: u32,
    sample_rate_hz: u32,
    samples: *mut i16,
    sample_count: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the audiohook exclusively owns this live prepared link.
        let link = unsafe { link_mut(link)? };
        let direction = match direction {
            URP_AST_LINK_DIRECTION_READ => LinkDirection::Read,
            URP_AST_LINK_DIRECTION_WRITE => LinkDirection::Write,
            _ => return Err(Status::InvalidArgument),
        };
        if samples.is_null() {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the audiohook supplies sample_count writable signed-linear samples.
        let samples = unsafe { std::slice::from_raw_parts_mut(samples, sample_count as usize) };
        let _ = link.process_s16(direction, sample_rate_hz, samples);
        Ok(())
    })
}

unsafe extern "C" fn link_observe(link: *mut c_void, output: *mut UrpAstLinkObservation) -> c_int {
    ffi_status(|| {
        // SAFETY: control-plane callers quiesce the owning audiohook before observing.
        let observation = unsafe { link_mut(link)? }.observe();
        // SAFETY: the caller supplies initialized writable ABI storage.
        unsafe { write_abi(output, link_observation(observation)) }
    })
}

unsafe extern "C" fn link_destroy(link: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !link.is_null() {
            // SAFETY: the pointer came from link_prepare and is consumed once after detachment.
            drop(unsafe { Box::from_raw(link.cast::<PreparedLink>()) });
        }
    }));
}

unsafe extern "C" fn channel_reserve(
    driver: *mut c_void,
    args: *const UrpAstChannelReserveArgs,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the Rust host owns a live handle returned by driver_create.
        let driver = unsafe { driver_ref(driver)? };
        if driver
            .0
            .pending_reload
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .is_some()
        {
            return Err(Status::ChannelBusy);
        }
        // SAFETY: the caller promises readable bytes described by the ABI header.
        let args = unsafe { copy_abi(args)? };
        // SAFETY: this byte span is readable for the synchronous reservation.
        let name = unsafe { required_utf8(args.channel_name, args.channel_name_length)? };
        let (mode, controller) = controller_configuration(args.transport)?;
        let snapshot = driver.0.configuration.snapshot();
        let channel = snapshot
            .channel(name)
            .cloned()
            .ok_or(Status::ChannelNotFound)?;
        let jitter = jitter_config(&channel.config().station.asterisk);
        let reservation = name.to_ascii_lowercase();
        {
            let mut active = driver
                .0
                .reservations
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if !active.reserve(reservation.clone()) {
                return Err(Status::ChannelBusy);
            }
        }
        let generation = driver.0.station_generation.fetch_add(1, Ordering::Relaxed);
        let plan = hardware_plan(channel.config());
        let eeprom_enabled = channel.config().station.hardware.eeprom_enabled;
        let preflight = match HardwareStation::preflight(
            &plan,
            eeprom_enabled,
            driver.0.audio,
            driver.0.gpio,
        ) {
            Ok(preflight) => preflight,
            Err(error) => {
                return Err(reservation_setup_failure(&driver.0, &reservation, error));
            }
        };
        let mut channel = channel;
        preflight.apply_startup_tuning(channel.config_mut());
        let media = match driver.0.factory.prepare(channel, generation, controller) {
            Ok(media) => media,
            Err(error) => {
                return Err(reservation_setup_failure(&driver.0, &reservation, error));
            }
        };
        let (program, consumer) =
            LatestHandoff::split(HANDOFF_SLOTS).map_err(|_| Status::SetupFailed)?;
        let handle = Box::new(ChannelHandle {
            driver: NonNull::from(&driver.0),
            reservation,
            channel_context: args.channel_context as usize,
            mode,
            program: UnsafeCell::new(program),
            control: UnsafeCell::new(ChannelControl {
                media: Some(media),
                preflight: Some(preflight),
                hardware: None,
                program: consumer,
                voice: ControllerPcmFrame::silence(mode),
                jitter,
                reload: None,
                running: false,
                direct: None,
            }),
            rollback: Mutex::new(None),
        });
        // SAFETY: output_pointer validated this writable destination.
        unsafe { output.as_ptr().write(Box::into_raw(handle).cast()) };
        Ok(())
    })
}

unsafe extern "C" fn channel_start(channel: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        if let Some(hardware) = control.hardware.as_mut() {
            let result = hardware.start().map_err(|error| {
                channel.log_error(&error.to_string());
                Status::SetupFailed
            });
            if result.is_ok() {
                control.running = true;
            }
            return result;
        }
        let media = control.media.take().ok_or(Status::NotReady)?;
        let preflight = control.preflight.take().ok_or(Status::NotReady)?;
        let mut hardware = HardwareStation::open_preflighted(
            media,
            channel.driver().audio,
            channel.driver().gpio,
            MAXIMUM_FRAME_COUNT,
            preflight,
        )
        .map_err(|error| {
            channel.log_error(&error.to_string());
            Status::SetupFailed
        })?;
        let result = hardware.start().map_err(|error| {
            channel.log_error(&error.to_string());
            Status::SetupFailed
        });
        control.hardware = Some(hardware);
        if result.is_ok() {
            control.running = true;
        }
        result
    })
}

unsafe extern "C" fn channel_stop(channel: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        let result = control.hardware.as_mut().map_or(Ok(()), |hardware| {
            hardware.stop().map_err(|error| {
                channel.log_error(&error.to_string());
                Status::SetupFailed
            })
        });
        if result.is_ok() {
            control.running = false;
        }
        result
    })
}

unsafe extern "C" fn channel_reload_prepare(channel: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        if control.reload.is_some()
            || channel
                .rollback
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .is_some()
        {
            return Err(Status::ChannelBusy);
        }
        let driver = channel.driver();
        let configuration = driver
            .pending_reload
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
            .ok_or(Status::NotReady)?;
        let mut configuration = configuration
            .channel(&channel.reservation)
            .cloned()
            .ok_or_else(|| {
                driver.operations.log(
                    URP_AST_LOG_ERROR,
                    "configuration reload cannot remove a reserved channel",
                );
                Status::InvalidConfiguration
            })?;
        let jitter = jitter_config(&configuration.config().station.asterisk);
        let transport = match channel.mode {
            AsteriskPcmMode::AppRpt => URP_AST_TRANSPORT_APP_RPT,
            AsteriskPcmMode::Advanced => URP_AST_TRANSPORT_RPT_ADVANCED,
        };
        let (_, controller) = controller_configuration(transport)?;
        let station_generation = driver.station_generation.fetch_add(1, Ordering::Relaxed);
        let plan = hardware_plan(configuration.config());
        let eeprom_enabled = configuration.config().station.hardware.eeprom_enabled;
        let preflight = if let Some(hardware) = control.hardware.as_ref() {
            hardware.preflight_replacement(&plan, eeprom_enabled, driver.audio, driver.gpio)
        } else {
            HardwareStation::preflight(&plan, eeprom_enabled, driver.audio, driver.gpio)
        }
        .map_err(|error| {
            channel.log_error(&error.to_string());
            Status::SetupFailed
        })?;
        preflight.apply_startup_tuning(configuration.config_mut());
        let mut media = driver
            .factory
            .prepare(configuration, station_generation, controller)
            .map_err(|error| {
                channel.log_error(&error.to_string());
                Status::SetupFailed
            })?;
        if let Some(direct) = control.direct {
            // SAFETY: reload retains the original attachment lifetime and stops
            // the previous PortAudio generation before starting its replacement.
            unsafe { media.set_direct_callbacks(direct) }.map_err(|_| Status::SetupFailed)?;
        }
        control.reload = Some(PreparedChannelReload {
            media,
            preflight,
            jitter,
        });
        Ok(())
    })
}

unsafe extern "C" fn channel_reload_activate(channel: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        let mut prepared = control.reload.take().ok_or(Status::NotReady)?;
        if let Err(status) = drain_receive(channel, control) {
            control.reload = Some(prepared);
            return Err(status);
        }
        if control.hardware.is_none() {
            let retained = RetainedControllerState::capture(control.controller()?);
            let media = control.media.take().ok_or(Status::NotReady)?;
            let preflight = control.preflight.take().ok_or(Status::NotReady)?;
            let jitter = control.jitter;
            retained.apply_to_controller(&mut prepared.media.controller);
            control.media = Some(prepared.media);
            control.preflight = Some(prepared.preflight);
            control.jitter = prepared.jitter;
            *channel
                .rollback
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(AdoptedChannelReload {
                previous: PreviousChannelGeneration::Prepared {
                    media: Box::new(media),
                    preflight,
                },
                jitter,
            });
            return Ok(());
        }

        // Persistent outputs survive generation replacement. Timed pulses
        // restart inactive, matching the established reload behavior.
        let transient = if control.running {
            Some(
                control
                    .hardware
                    .as_ref()
                    .ok_or(Status::NotReady)?
                    .transient_state()
                    .map_err(|error| {
                        channel.log_error(&error.to_string());
                        Status::SetupFailed
                    })?,
            )
        } else {
            None
        };
        // A stopped stream still owns its exclusive device lease. Suspend closes
        // it while retaining the media and callback boxes needed for rollback.
        if let Err(error) = control.hardware.as_mut().ok_or(Status::NotReady)?.suspend() {
            channel.log_error(&error.to_string());
            if let Some(state) = transient {
                control.running = restore_previous(
                    channel,
                    control
                        .hardware
                        .as_mut()
                        .expect("the previous generation remains owned after stop failure"),
                    Some(state),
                );
            }
            control.reload = Some(prepared);
            return Err(Status::SetupFailed);
        }
        if let Err(status) = drain_receive(channel, control) {
            let restored = restore_previous(
                channel,
                control
                    .hardware
                    .as_mut()
                    .expect("the suspended previous generation remains owned"),
                transient,
            );
            control.running = restored && transient.is_some();
            control.reload = Some(prepared);
            return Err(status);
        }
        let retained = RetainedControllerState::capture(control.controller()?);
        let mut previous = control.hardware.take().ok_or(Status::NotReady)?;
        let replacement = HardwareStation::open_preflighted(
            prepared.media,
            channel.driver().audio,
            channel.driver().gpio,
            MAXIMUM_FRAME_COUNT,
            prepared.preflight,
        );
        let mut replacement = match replacement {
            Ok(replacement) => replacement,
            Err(error) => {
                channel.log_error(&error.to_string());
                let restored = restore_previous(channel, &mut previous, transient);
                control.running = restored && transient.is_some();
                control.hardware = Some(previous);
                return Err(Status::SetupFailed);
            }
        };
        if let Some(state) = transient {
            if let Err(error) = replacement.restore_transient_state(state) {
                channel.log_error(&error.to_string());
                drop(replacement);
                control.running = restore_previous(channel, &mut previous, Some(state));
                control.hardware = Some(previous);
                return Err(Status::SetupFailed);
            }
        }
        retained.apply_to_hardware(&mut replacement);
        if control.running {
            if let Err(error) = replacement.start() {
                channel.log_error(&error.to_string());
                drop(replacement);
                control.running = restore_previous(channel, &mut previous, transient);
                control.hardware = Some(previous);
                return Err(Status::SetupFailed);
            }
        }
        let jitter = control.jitter;
        control.hardware = Some(replacement);
        control.jitter = prepared.jitter;
        *channel
            .rollback
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(AdoptedChannelReload {
            previous: PreviousChannelGeneration::Hardware {
                hardware: previous,
                transient,
            },
            jitter,
        });
        Ok(())
    })
}

unsafe extern "C" fn channel_reload_finish(channel: *mut c_void, commit: u32) -> c_int {
    ffi_status(|| {
        let commit = boolean(commit)?;
        if commit {
            // SAFETY: commit touches only the control-plane rollback slot; the
            // adopted generation and its audio callbacks remain disjoint.
            let channel = unsafe { channel_ref(channel)? };
            let retired = channel
                .rollback
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .take();
            drop(retired);
            return Ok(());
        }

        // SAFETY: abort runs on the channel's serialized taskprocessor.
        let (channel, control) = unsafe { channel_control(channel)? };
        if control.reload.take().is_some() {
            return Ok(());
        }
        let Some(adopted) = channel
            .rollback
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take()
        else {
            return Ok(());
        };
        control.jitter = adopted.jitter;
        match adopted.previous {
            PreviousChannelGeneration::Prepared { media, preflight } => {
                control.media = Some(*media);
                control.preflight = Some(preflight);
                Ok(())
            }
            PreviousChannelGeneration::Hardware {
                mut hardware,
                transient,
            } => {
                let mut degraded = false;
                if let Some(mut replacement) = control.hardware.take() {
                    if let Err(error) = replacement.stop() {
                        channel.log_error(&format!(
                            "configuration reload rollback could not stop the candidate generation: {error}"
                        ));
                        degraded = true;
                    }
                } else {
                    degraded = true;
                }
                let restored = restore_previous(channel, &mut hardware, transient);
                control.running = restored && transient.is_some();
                degraded |= !restored;
                control.hardware = Some(hardware);
                if degraded {
                    Err(Status::SetupFailed)
                } else {
                    Ok(())
                }
            }
        }
    })
}

unsafe extern "C" fn channel_write_voice(
    channel: *mut c_void,
    samples: *const i16,
    sample_count: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the Rust host owns a live handle returned by channel_reserve.
        let channel = unsafe { channel_ref(channel)? };
        let expected = channel.mode.frame_samples();
        if sample_count as usize != expected || samples.is_null() {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the caller supplies exactly sample_count readable samples.
        let samples = unsafe { std::slice::from_raw_parts(samples, expected) };
        let frame = match channel.mode {
            AsteriskPcmMode::AppRpt => {
                let mut frame = [0; usbradioplus_asl3::APP_RPT_FRAME_SAMPLES];
                frame.copy_from_slice(samples);
                ControllerPcmFrame::app_rpt(frame)
            }
            AsteriskPcmMode::Advanced => {
                let mut frame = [0; usbradioplus_asl3::ADVANCED_FRAME_SAMPLES];
                frame.copy_from_slice(samples);
                ControllerPcmFrame::advanced(frame)
            }
        };
        channel.publish_program(frame);
        Ok(())
    })
}

unsafe extern "C" fn channel_write_text(
    channel: *mut c_void,
    text: *const u8,
    text_length: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        // SAFETY: validates that the supplied control text is readable UTF-8.
        let text = unsafe { required_utf8(text, text_length)? };
        let message =
            usbradioplus_asl3::parse_controller_text(text).map_err(|_| Status::InvalidArgument)?;
        apply_control(channel, control, message)
    })
}

unsafe extern "C" fn channel_set_direct_callbacks(
    channel: *mut c_void,
    callbacks: *const UrpAstDirectCallbacks,
) -> c_int {
    ffi_status(|| {
        // SAFETY: this serialized task owns the live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        if channel.mode != AsteriskPcmMode::Advanced
            || control.running
            || control.hardware.is_some()
            || control.direct.is_some()
            || control.reload.is_some()
        {
            return Err(Status::InvalidArgument);
        }
        if callbacks.is_null() {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the internal operation accepts a complete readable descriptor;
        // the public setoption validates its byte length before submitting it.
        let callbacks = unsafe { callbacks.read_unaligned() };
        if !callbacks.is_valid() {
            return Err(Status::InvalidArgument);
        }
        let media = control.media.as_mut().ok_or(Status::NotReady)?;
        // SAFETY: the attaching controller retains both contexts through synchronous
        // station shutdown; this operation only copies their addresses.
        unsafe { media.set_direct_callbacks(callbacks) }.map_err(|_| Status::InvalidArgument)?;
        control.direct = Some(callbacks);
        Ok(())
    })
}

unsafe extern "C" fn channel_set_transmit(
    channel: *mut c_void,
    keyed: u32,
    forced_ctcss_tenths_hz: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        let message = if keyed == 0 {
            if forced_ctcss_tenths_hz != 0 {
                return Err(Status::InvalidArgument);
            }
            usbradioplus_asl3::ControlMessage::TransmitUnkey
        } else {
            let tone = if forced_ctcss_tenths_hz == 0 {
                None
            } else {
                u16::try_from(forced_ctcss_tenths_hz)
                    .ok()
                    .and_then(CtcssTone::from_tenths_hz)
                    .ok_or(Status::InvalidArgument)?
                    .into()
            };
            usbradioplus_asl3::ControlMessage::TransmitKey(tone)
        };
        apply_control(channel, control, message)
    })
}

unsafe extern "C" fn channel_set_dtmf(channel: *mut c_void, enabled: u32) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (_, control) = unsafe { channel_control(channel)? };
        control.controller()?.set_dtmf_detection(enabled != 0);
        Ok(())
    })
}

unsafe extern "C" fn channel_set_echo(channel: *mut c_void, enabled: u32) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (_, control) = unsafe { channel_control(channel)? };
        control.controller()?.set_echo_enabled(enabled != 0);
        Ok(())
    })
}

unsafe extern "C" fn channel_get_jitter_config(
    channel: *mut c_void,
    output: *mut UrpAstJitterConfig,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (_, control) = unsafe { channel_control(channel)? };
        // SAFETY: caller initialized writable ABI-sized output storage.
        unsafe { write_abi(output, control.jitter) }
    })
}

unsafe extern "C" fn channel_command(
    channel: *mut c_void,
    command: *mut UrpAstChannelCommand,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        // SAFETY: the caller supplies initialized writable ABI storage.
        let mut command_value = unsafe { copy_abi(command.cast_const())? };
        let hardware = control.hardware.as_mut().ok_or(Status::NotReady)?;
        match command_value.command {
            URP_AST_COMMAND_GET_MIXER => {
                command_value.value = i64::from(
                    hardware
                        .mixer_level(mixer(command_value.target)?)
                        .map_err(|error| command_error(channel, error))?,
                );
            }
            URP_AST_COMMAND_SET_MIXER => {
                let level = u32::try_from(command_value.value)
                    .ok()
                    .filter(|value| *value <= 999)
                    .ok_or(Status::InvalidArgument)?;
                hardware
                    .set_mixer_level(mixer(command_value.target)?, level)
                    .map_err(|error| command_error(channel, error))?;
            }
            URP_AST_COMMAND_SET_TEST_TONE => {
                hardware.set_calibrated_test_tone(switch(command_value.value)?);
            }
            URP_AST_COMMAND_READ_EEPROM => {
                let image = hardware
                    .read_eeprom()
                    .map_err(|error| command_error(channel, error))?;
                command_value.eeprom_words = image.words;
                command_value.flags = eeprom_flags(&image);
            }
            URP_AST_COMMAND_WRITE_EEPROM => {
                let mut image = EepromImage {
                    checksum_valid: command_value.flags & URP_AST_EEPROM_CHECKSUM_VALID != 0,
                    magic_valid: command_value.flags & URP_AST_EEPROM_MAGIC_VALID != 0,
                    words: command_value.eeprom_words,
                };
                hardware
                    .write_eeprom(&mut image)
                    .map_err(|error| command_error(channel, error))?;
                command_value.eeprom_words = image.words;
                command_value.flags = eeprom_flags(&image);
            }
            URP_AST_COMMAND_SET_CTCSS_INHIBIT => {
                hardware.set_ctcss_inhibited(switch(command_value.value)?);
            }
            URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE => {
                hardware.set_subaudible_override(switch(command_value.value)?);
            }
            URP_AST_COMMAND_SAVE_TUNING_EEPROM => {
                hardware
                    .save_current_tuning_to_eeprom()
                    .map_err(|error| command_error(channel, error))?;
                command_value.value = 1;
            }
            _ => return Err(Status::InvalidArgument),
        }
        // SAFETY: the same complete writable command storage was validated above.
        unsafe { write_abi(command, command_value) }
    })
}

unsafe extern "C" fn channel_get_status(
    channel: *mut c_void,
    output: *mut UrpAstChannelStatus,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        let (echo_enabled, dtmf_enabled, receive_handoff) = {
            let controller = control.controller()?;
            (
                controller.echo_enabled(),
                controller.dtmf_detection_enabled(),
                controller.handoff_observation(),
            )
        };
        let program_handoff = control.program.observe();
        let mut status = UrpAstChannelStatus {
            struct_size: size_of::<UrpAstChannelStatus>() as u32,
            abi_version: ABI_VERSION,
            transport: match channel.mode {
                AsteriskPcmMode::AppRpt => URP_AST_TRANSPORT_APP_RPT,
                AsteriskPcmMode::Advanced => URP_AST_TRANSPORT_RPT_ADVANCED,
            },
            running: u32::from(control.running),
            echo_enabled: u32::from(echo_enabled),
            dtmf_enabled: u32::from(dtmf_enabled),
            receive_handoff_available: receive_handoff.available as u64,
            receive_handoff_discarded: receive_handoff.discarded,
            program_handoff_available: program_handoff.available as u64,
            program_handoff_discarded: program_handoff.discarded,
            ctcss_decode_index: -1,
            ..UrpAstChannelStatus::default()
        };
        if let Some(hardware) = control.hardware.as_mut() {
            let diagnostics = hardware
                .diagnostics()
                .map_err(|error| command_error(channel, error))?;
            let radio = hardware
                .control()
                .radio()
                .radio()
                .snapshot()
                .map_err(|error| {
                    channel.log_error(&error.to_string());
                    Status::SetupFailed
                })?;
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
            status.callback_last_start_delay_ns =
                diagnostics.runtime.audio.callback_last_start_delay_ns;
            status.callback_max_start_delay_ns =
                diagnostics.runtime.audio.callback_max_start_delay_ns;
            status.callback_late_start_count = diagnostics.runtime.audio.callback_late_start_count;
            status.last_input_xrun_monotonic_ns =
                diagnostics.runtime.audio.last_input_xrun_monotonic_ns;
            status.last_output_xrun_monotonic_ns =
                diagnostics.runtime.audio.last_output_xrun_monotonic_ns;
            status.input_latency_seconds = diagnostics.timing.input_latency_seconds;
            status.output_latency_seconds = diagnostics.timing.output_latency_seconds;
            status.native_sample_rate_hz = diagnostics.timing.sample_rate_hz;
            status.ring_occupancy_frames = radio.program_ring.occupancy_frames;
            status.ring_reserve_frames = radio.program_ring.reserve_frames;
            status.ring_target_frames = radio.program_ring.target_frames;
            status.ring_capacity_frames = radio.program_ring.capacity_frames;
            status.ring_ratio = radio.program_ring.ratio;
            status.ring_underrun_samples = radio.program_ring.underrun_samples;
            status.ring_overrun_samples = radio.program_ring.overrun_samples;
            status.ring_concealment_samples = radio.program_ring.concealment_samples;
            status.carrier_active = u32::from(radio.carrier_active);
            status.subaudible_active = u32::from(radio.subaudible_active);
            status.receiver_keyed = u32::from(radio.receiver_keyed);
            status.logical_ptt = u32::from(radio.logical_ptt);
            status.ctcss_decode_index =
                radio.ctcss_decoded.map_or(-1, |tone| i32::from(tone.get()));
            status.dcs_valid = u32::from(radio.dcs_valid);
            status.receive_mixer_level = hardware
                .mixer_level(HardwareMixer::Receive)
                .map_err(|error| command_error(channel, error))?;
            status.transmit_a_mixer_level = hardware
                .mixer_level(HardwareMixer::TransmitA)
                .map_err(|error| command_error(channel, error))?;
            status.transmit_b_mixer_level = hardware
                .mixer_level(HardwareMixer::TransmitB)
                .map_err(|error| command_error(channel, error))?;
        }
        // SAFETY: caller initialized writable ABI-sized output storage.
        unsafe { write_abi(output, status) }
    })
}

fn mixer(value: u32) -> Result<HardwareMixer, Status> {
    match value {
        URP_AST_MIXER_RECEIVE => Ok(HardwareMixer::Receive),
        URP_AST_MIXER_TRANSMIT_A => Ok(HardwareMixer::TransmitA),
        URP_AST_MIXER_TRANSMIT_B => Ok(HardwareMixer::TransmitB),
        _ => Err(Status::InvalidArgument),
    }
}

fn switch(value: i64) -> Result<bool, Status> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(Status::InvalidArgument),
    }
}

fn boolean(value: u32) -> Result<bool, Status> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(Status::InvalidArgument),
    }
}

fn eeprom_flags(image: &EepromImage) -> u32 {
    let checksum = u32::from(image.checksum_valid) * URP_AST_EEPROM_CHECKSUM_VALID;
    let magic = u32::from(image.magic_valid) * URP_AST_EEPROM_MAGIC_VALID;
    checksum | magic
}

fn command_error(channel: &ChannelHandle, error: impl std::fmt::Display) -> Status {
    channel.log_error(&error.to_string());
    Status::SetupFailed
}

unsafe extern "C" fn channel_service(channel: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the taskprocessor owns this live channel control endpoint.
        let (channel, control) = unsafe { channel_control(channel)? };
        if let Some(hardware) = control.hardware.as_ref() {
            while let Some(event) = hardware.take_input_event() {
                channel
                    .driver()
                    .operations
                    .queue_text(channel.channel_context, &hardware_input_text(event))?;
            }
        }
        drain_program(control)?;
        drain_receive(channel, control)?;
        Ok(())
    })
}

fn drain_program(control: &mut ChannelControl) -> Result<(), Status> {
    while let Some(frame) = control.program.pop() {
        control
            .controller()?
            .write_program(&frame)
            .map_err(|_| Status::SetupFailed)?;
    }
    Ok(())
}

fn drain_receive(channel: &ChannelHandle, control: &mut ChannelControl) -> Result<(), Status> {
    loop {
        let mut voice = control.voice;
        let action = control.controller()?.next_action(&mut voice);
        control.voice = voice;
        let Some(action) = action else {
            return Ok(());
        };
        deliver(channel, control, action)?;
    }
}

fn hardware_input_text(event: HardwareInputEvent) -> String {
    let (kind, number) = match event.input {
        HardwareInput::Cm119(pin) => ("GPIO", pin.get()),
        HardwareInput::Parallel(pin) => ("PP", pin),
    };
    format!("{kind}{number} {}\n", u8::from(event.active))
}

unsafe extern "C" fn channel_destroy(channel: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !channel.is_null() {
            // SAFETY: the pointer came from channel_reserve and is consumed once.
            drop(unsafe { Box::from_raw(channel.cast::<ChannelHandle>()) });
        }
    }));
}

fn apply_control(
    channel: &ChannelHandle,
    control: &mut ChannelControl,
    message: usbradioplus_asl3::ControlMessage,
) -> Result<(), Status> {
    let hardware = control.hardware.as_mut().ok_or(Status::NotReady)?;
    hardware
        .apply_control(message)
        .map(|_| ())
        .map_err(|error| {
            channel.log_error(&error.to_string());
            Status::SetupFailed
        })
}

/// Restore the previous device lease, preserving its original running/stopped state.
fn restore_previous(
    channel: &ChannelHandle,
    previous: &mut HardwareStation,
    hardware_state: Option<HardwareTransientState>,
) -> bool {
    if let Err(error) = previous.reopen() {
        channel.log_error(&format!(
            "configuration reload rollback failed; prior generation remains RF-safe and stopped: {error}"
        ));
        return false;
    }
    let Some(hardware_state) = hardware_state else {
        return true;
    };
    if let Err(error) = previous.restore_transient_state(hardware_state) {
        channel.log_error(&format!(
            "configuration reload rollback failed; prior generation remains RF-safe and stopped: {error}"
        ));
        return false;
    }
    if let Err(error) = previous.start() {
        channel.log_error(&format!(
            "configuration reload rollback failed; prior generation remains RF-safe and stopped: {error}"
        ));
        return false;
    }
    true
}

fn reservation_setup_failure(
    driver: &DriverInner,
    reservation: &str,
    error: impl std::fmt::Display,
) -> Status {
    driver
        .reservations
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .release(reservation);
    driver.operations.log(URP_AST_LOG_ERROR, &error.to_string());
    Status::SetupFailed
}

fn deliver(
    channel: &ChannelHandle,
    control: &mut ChannelControl,
    action: DeliveryAction,
) -> Result<(), Status> {
    let operations = channel.driver().operations;
    let context = channel.channel_context;
    match action {
        DeliveryAction::ReceiverKey { ctcss } => operations.queue_control(
            context,
            URP_AST_CONTROL_RECEIVER_KEY,
            ctcss.map_or(0, |tone| i32::from(tone.tenths_hz())),
            0,
        ),
        DeliveryAction::ReceiverUnkey => {
            operations.queue_control(context, URP_AST_CONTROL_RECEIVER_UNKEY, 0, 0)
        }
        DeliveryAction::Voice => deliver_voice(channel, control),
        DeliveryAction::TransmitCtcssReady(tone) => {
            let tenths = tone.tenths_hz();
            operations.queue_text(context, &format!("cstx={}.{}", tenths / 10, tenths % 10))
        }
        DeliveryAction::VoterRssi(value) => operations.queue_text(context, &format!("R {value}")),
    }
}

fn deliver_voice(channel: &ChannelHandle, control: &mut ChannelControl) -> Result<(), Status> {
    let operations = channel.driver().operations;
    let context = channel.channel_context;
    let detect_dtmf = control.controller()?.dtmf_detection_enabled();
    let samples = control.voice.samples();
    let rate = control.voice.mode().sample_rate_hz();
    if !detect_dtmf {
        return operations.queue_voice(context, samples, rate);
    }
    let mut analyzed = [0_i16; usbradioplus_asl3::ADVANCED_FRAME_SAMPLES];
    analyzed[..samples.len()].copy_from_slice(samples);
    let analyzed = &mut analyzed[..samples.len()];
    let Some(event) = operations.analyze_dtmf(context, analyzed, rate)? else {
        return operations.queue_voice(context, analyzed, rate);
    };
    deliver_dtmf_action(
        operations,
        context,
        analyzed,
        rate,
        control
            .controller()?
            .handle_dtmf(event, operations.monotonic_milliseconds()),
    )
}

fn deliver_dtmf_action(
    operations: Operations,
    context: usize,
    analyzed: &[i16],
    rate: u32,
    action: DtmfAction,
) -> Result<(), Status> {
    match action {
        DtmfAction::PassVoice => operations.queue_voice(context, analyzed, rate),
        DtmfAction::ForwardBegin(digit) => {
            operations.queue_control(context, URP_AST_CONTROL_DTMF_BEGIN, i32::from(digit), 0)
        }
        DtmfAction::ForwardEnd { digit, duration_ms } => operations.queue_control(
            context,
            URP_AST_CONTROL_DTMF_END,
            i32::from(digit),
            duration_ms,
        ),
        DtmfAction::MutePseudoDigit => {
            operations.queue_control(context, URP_AST_CONTROL_NULL, 0, 0)
        }
        DtmfAction::SuppressRepeatedBegin => Ok(()),
    }
}

fn controller_configuration(
    transport: u32,
) -> Result<(AsteriskPcmMode, ControllerConfiguration), Status> {
    match transport {
        URP_AST_TRANSPORT_APP_RPT => Ok((
            AsteriskPcmMode::AppRpt,
            ControllerConfiguration::AppRpt {
                handoff_slots: HANDOFF_SLOTS,
                echo: EchoConfiguration::default(),
            },
        )),
        URP_AST_TRANSPORT_RPT_ADVANCED => Ok((
            AsteriskPcmMode::Advanced,
            ControllerConfiguration::RptAdvanced {
                handoff_slots: HANDOFF_SLOTS,
            },
        )),
        _ => Err(Status::InvalidArgument),
    }
}

fn configured_active_channel(configuration: &DriverConfiguration) -> Option<String> {
    configuration
        .channels()
        .iter()
        .rfind(|channel| channel.config().station.channel_enabled)
        .map(|channel| channel.channel().to_ascii_lowercase())
}

fn jitter_config(config: &AsteriskConfig) -> UrpAstJitterConfig {
    UrpAstJitterConfig {
        struct_size: size_of::<UrpAstJitterConfig>() as u32,
        abi_version: ABI_VERSION,
        enabled: u32::from(config.jitter_buffer_enabled),
        maximum_size_ms: config.jitter_buffer_max_size_ms,
        resync_threshold_ms: config.jitter_buffer_resync_threshold_ms,
        implementation: match config.jitter_buffer_implementation {
            JitterBufferImplementation::Fixed => URP_AST_JITTER_FIXED,
            JitterBufferImplementation::Adaptive => URP_AST_JITTER_ADAPTIVE,
        },
        logging_enabled: u32::from(config.jitter_buffer_logging_enabled),
        force_enabled: u32::from(config.jitter_buffer_force_enabled),
        target_extra_ms: config.jitter_buffer_target_extra_ms,
        video_sync_enabled: u32::from(config.jitter_buffer_video_sync_enabled),
    }
}

fn link_observation(observation: LinkObservation) -> UrpAstLinkObservation {
    UrpAstLinkObservation {
        struct_size: size_of::<UrpAstLinkObservation>() as u32,
        abi_version: ABI_VERSION,
        processed_blocks: observation.processed_blocks,
        bypassed_blocks: observation.bypassed_blocks,
        failed_blocks: observation.failed_blocks,
    }
}

struct ValidatedProviders {
    station: StationProviders,
    audio: AudioProvider,
    gpio: GpioAdapter,
}

unsafe fn validate_providers(
    raw: *const UrpAstProviderManifest,
) -> Result<ValidatedProviders, Status> {
    // SAFETY: the caller promises readable bytes described by the ABI header.
    let raw = unsafe { copy_abi(raw)? };
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let graph = unsafe { GraphProvider::from_raw_descriptor(raw.ffmpeg) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let denoise = unsafe { DenoiseProvider::from_raw_descriptor(raw.rnnoise) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let ring = unsafe { RingProvider::from_raw_descriptor(raw.ring) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let radio = unsafe { RadioProvider::from_raw_descriptor(raw.radio) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let sample_rate = unsafe { SampleRateAdapter::from_raw(raw.samplerate) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let audio = unsafe { AudioProvider::from_raw_descriptor(raw.audio) }
        .map_err(|_| Status::IncompatibleAbi)?;
    // SAFETY: every pointer has the process lifetime required by the manifest.
    let gpio = unsafe { GpioAdapter::from_raw(raw.gpio) }.map_err(|_| Status::IncompatibleAbi)?;
    Ok(ValidatedProviders {
        station: StationProviders {
            graph,
            denoise,
            ring,
            radio,
            sample_rate,
        },
        audio,
        gpio,
    })
}

fn ffi_status(operation: impl FnOnce() -> Result<(), Status>) -> c_int {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(())) => URP_AST_OK,
        Ok(Err(status)) => status.code(),
        Err(_) => Status::InternalFailure.code(),
    }
}

fn callback_result(result: c_int) -> Result<(), Status> {
    if result == 0 {
        Ok(())
    } else {
        Err(Status::AsteriskFailure)
    }
}

fn output_pointer(output: *mut *mut c_void) -> Result<NonNull<*mut c_void>, Status> {
    let output = NonNull::new(output).ok_or(Status::InvalidArgument)?;
    // SAFETY: the caller supplied writable output-pointer storage.
    unsafe { output.as_ptr().write(ptr::null_mut()) };
    Ok(output)
}

unsafe fn driver_ref<'a>(driver: *mut c_void) -> Result<&'a DriverHandle, Status> {
    // SAFETY: the caller owns a live handle created by this adapter.
    unsafe { driver.cast::<DriverHandle>().as_ref() }.ok_or(Status::InvalidArgument)
}

unsafe fn channel_ref<'a>(channel: *mut c_void) -> Result<&'a ChannelHandle, Status> {
    // SAFETY: the caller owns a live handle created by this adapter.
    unsafe { channel.cast::<ChannelHandle>().as_ref() }.ok_or(Status::InvalidArgument)
}

unsafe fn link_mut<'a>(link: *mut c_void) -> Result<&'a mut PreparedLink, Status> {
    // SAFETY: the caller exclusively owns a live handle returned by link_prepare.
    unsafe { link.cast::<PreparedLink>().as_mut() }.ok_or(Status::InvalidArgument)
}

unsafe fn channel_control<'a>(
    channel: *mut c_void,
) -> Result<(&'a ChannelHandle, &'a mut ChannelControl), Status> {
    // SAFETY: the taskprocessor is the sole control owner for this live handle.
    let channel = unsafe { channel_ref(channel)? };
    // SAFETY: the control endpoint is disjoint from the voice-producer endpoint.
    let control = unsafe { &mut *channel.control.get() };
    Ok((channel, control))
}

unsafe fn copy_abi<T: Copy>(raw: *const T) -> Result<T, Status> {
    // SAFETY: the caller promises readable bytes described by the ABI header.
    unsafe { validate_abi(raw.cast(), align_of::<T>(), size_of::<T>())? };
    // SAFETY: the validated advertised size covers the complete structure.
    Ok(unsafe { ptr::read(raw) })
}

unsafe fn write_abi<T: Copy>(output: *mut T, value: T) -> Result<(), Status> {
    // SAFETY: the caller promises writable bytes described by the ABI header.
    unsafe { validate_abi(output.cast_const().cast(), align_of::<T>(), size_of::<T>())? };
    // SAFETY: the caller advertised and supplied complete writable storage.
    unsafe { output.write(value) };
    Ok(())
}

unsafe fn validate_abi(
    raw: *const c_void,
    alignment: usize,
    required_size: usize,
) -> Result<(), Status> {
    if raw.is_null() || (raw as usize) % alignment != 0 {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: the caller promises at least the common readable ABI header.
    let header = unsafe { ptr::read(raw.cast::<AbiHeader>()) };
    if header.struct_size < required_size as u32 || header.abi_version != ABI_VERSION {
        return Err(Status::IncompatibleAbi);
    }
    Ok(())
}

unsafe fn write_bytes(
    value: &[u8],
    output: *mut u8,
    output_capacity: u32,
    output_length: *mut u32,
) -> Result<(), Status> {
    if output_length.is_null() || (output_length as usize) % align_of::<u32>() != 0 {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: the pointer was checked non-null above.
    let output_length = unsafe { NonNull::new_unchecked(output_length) };
    let required = u32::try_from(value.len()).map_err(|_| Status::InvalidArgument)?;
    // SAFETY: caller supplied writable length storage.
    unsafe { output_length.as_ptr().write(required) };
    if output.is_null() {
        return if output_capacity == 0 {
            Ok(())
        } else {
            Err(Status::InvalidArgument)
        };
    }
    if output_capacity < required {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: caller advertises at least required writable output bytes and
    // value is a disjoint Rust-owned byte span.
    unsafe { ptr::copy_nonoverlapping(value.as_ptr(), output, value.len()) };
    Ok(())
}

unsafe fn required_utf8<'a>(raw: *const u8, length: u32) -> Result<&'a str, Status> {
    if length == 0 || raw.is_null() {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: the caller promises a readable byte span of exactly this length.
    let bytes = unsafe { std::slice::from_raw_parts(raw, length as usize) };
    std::str::from_utf8(bytes).map_err(|_| Status::InvalidArgument)
}

#[cfg(test)]
mod tests;
