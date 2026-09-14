//! Safe ownership for the ABI-3 whole-session radio core.
//!
//! Product composition validates the process-lifetime descriptor and prepares
//! all borrowed processing ports on its control plane. A prepared session then
//! splits exactly once into independent receive and transmit callback owners
//! plus one control observer. The callback methods operate only on exact,
//! bounded normalized-F32 spans at the fixed 48 kHz native rate.

#![deny(warnings)]
#![cfg_attr(coverage, feature(coverage_attribute))]

use std::cell::Cell;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::mem::{align_of, offset_of, size_of};
use std::ptr::{self, NonNull};
use std::sync::Arc;

/// Immutable native sample rate accepted by the radio core.
pub const NATIVE_SAMPLE_RATE_HZ: u32 = 48_000;
/// Interleaved channels in each native capture and playback frame.
pub const CANONICAL_CHANNELS: usize = 2;
/// Number of entries in the established receive CTCSS table.
pub const CTCSS_TONE_COUNT: usize = 38;
/// Normalized receive CTCSS decoder peak used by the calibration procedure.
pub const CTCSS_CALIBRATION_TARGET: f32 = 2_400.0 / 32_768.0;

const ABI_VERSION: u32 = 3;
const CAPABILITY: &CStr = c"rptadv.radio-core";
const RESULT_OK: c_int = 0;
const RESULT_INVALID_ARGUMENT: c_int = -1;
const RESULT_PROVIDER_FAILED: c_int = -2;
const RESULT_FRAME_COUNT_EXCEEDED: c_int = -3;
const RESULT_UNSUPPORTED: c_int = -4;
const RESULT_NOT_READY: c_int = -5;
const RESULT_BUSY: c_int = -6;
const CTCSS_VALID_BITS: u64 = (1_u64 << CTCSS_TONE_COUNT) - 1;

#[repr(C)]
struct OpaqueSession {
    _private: [u8; 0],
}

/// Failure while validating, preparing, or operating a radio session.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RadioError {
    /// Product composition supplied an incompatible function table.
    IncompatibleAdapter,
    /// Configuration, controls, a buffer, or returned data was invalid.
    InvalidArgument,
    /// One borrowed processing or program-ring provider failed.
    ProviderFailed,
    /// A callback span exceeded the endpoint's prepared bound.
    FrameCountExceeded,
    /// The radio core does not implement the requested fixed stream shape.
    Unsupported,
    /// The external session was not warmed before a callback.
    NotReady,
    /// More than one caller entered the same serial endpoint.
    Busy,
    /// The adapter returned an undocumented result or malformed observation.
    AdapterFailure,
}

impl fmt::Display for RadioError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleAdapter => formatter.write_str("incompatible radio-core adapter"),
            Self::InvalidArgument => formatter.write_str("invalid radio-session argument"),
            Self::ProviderFailed => formatter.write_str("radio-session provider failed"),
            Self::FrameCountExceeded => formatter.write_str("radio-session frame bound exceeded"),
            Self::Unsupported => formatter.write_str("unsupported radio-session stream shape"),
            Self::NotReady => formatter.write_str("radio session is not warmed"),
            Self::Busy => formatter.write_str("radio-session endpoint is already busy"),
            Self::AdapterFailure => formatter.write_str("radio-core adapter returned invalid data"),
        }
    }
}

impl std::error::Error for RadioError {}

/// Capture channel selected from each canonical stereo input frame.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ReceiveChannel {
    /// First interleaved capture channel.
    #[default]
    First = 0,
    /// Second interleaved capture channel.
    Second = 1,
}

/// Native discriminator-noise detector response.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum NoiseFilterProfile {
    /// Established default discriminator-noise response.
    #[default]
    Standard = 0,
    /// Alternate established discriminator-noise response.
    Alternate = 1,
}

/// Receive carrier-indication source.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CarrierSource {
    /// Do not admit carrier from any source.
    #[default]
    Disabled = 0,
    /// Native discriminator-noise detector.
    DspNoise = 1,
    /// Native audio-level detector.
    Vox = 2,
    /// Normal USB GPIO carrier indication.
    Usb = 3,
    /// Inverted USB GPIO carrier indication.
    UsbInverted = 4,
    /// Normal parallel-port carrier indication.
    Parallel = 5,
    /// Inverted parallel-port carrier indication.
    ParallelInverted = 6,
}

/// Receive subaudible-indication source.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum SubaudibleSource {
    /// Do not require a separate subaudible indication.
    #[default]
    Disabled = 0,
    /// Normal USB GPIO subaudible indication.
    Usb = 1,
    /// Inverted USB GPIO subaudible indication.
    UsbInverted = 2,
    /// Native CTCSS or DCS qualification.
    Dsp = 3,
    /// Normal parallel-port subaudible indication.
    Parallel = 4,
    /// Inverted parallel-port subaudible indication.
    ParallelInverted = 5,
}

/// Transmit CTCSS/DCS turn-off policy.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ToneOffMode {
    /// Release normally.
    #[default]
    None = 0,
    /// Apply a reverse-burst phase shift before release.
    PhaseShift = 1,
    /// Remove tone before PTT release.
    ToneRemove = 2,
    /// Send the configured tail tone before release.
    TailTone = 3,
}

/// Signal assignment for one physical playback channel.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum OutputRoute {
    /// Produce silence.
    #[default]
    Disabled = 0,
    /// Route program voice.
    Voice = 1,
    /// Route CTCSS/DCS only.
    Tone = 2,
    /// Route program voice plus CTCSS/DCS.
    Composite = 3,
    /// Route the auxiliary program voice path.
    AuxiliaryVoice = 4,
}

/// Zero-based index in the established CTCSS frequency table.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssToneIndex(u8);

impl CtcssToneIndex {
    /// Validate and construct a CTCSS table index.
    #[must_use]
    pub const fn new(index: u8) -> Option<Self> {
        if (index as usize) < CTCSS_TONE_COUNT {
            Some(Self(index))
        } else {
            None
        }
    }

    /// Return the zero-based table index.
    #[must_use]
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Enabled receive CTCSS entries.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct CtcssToneMask(u64);

impl CtcssToneMask {
    /// No enabled tones.
    pub const EMPTY: Self = Self(0);

    /// Validate a raw bit mask.
    pub const fn from_bits(bits: u64) -> Result<Self, RadioError> {
        if bits & !CTCSS_VALID_BITS == 0 {
            Ok(Self(bits))
        } else {
            Err(RadioError::InvalidArgument)
        }
    }

    /// Return the raw table-index bit mask.
    #[must_use]
    pub const fn bits(self) -> u64 {
        self.0
    }

    /// Return a mask with one table entry enabled.
    #[must_use]
    pub const fn with(self, index: CtcssToneIndex) -> Self {
        Self(self.0 | (1_u64 << index.0))
    }
}

/// Native receive CTCSS decoder policy.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssReceiveConfig {
    /// Enabled receive tone-table entries.
    pub tones: CtcssToneMask,
    /// Whether the decoder uses relaxed qualification tolerance.
    pub relaxed: bool,
}

/// Native receive DCS decoder policy.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DcsReceiveConfig {
    /// Three-octal-digit DCS code represented as an integer.
    pub code: i32,
    /// Whether receive DCS polarity is inverted.
    pub inverted: bool,
}

/// Native subaudible decoder selected for receive qualification.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ReceiveSignaling {
    /// Do not run a native subaudible decoder.
    #[default]
    Disabled,
    /// Run the native CTCSS decoder.
    Ctcss(CtcssReceiveConfig),
    /// Run the native DCS decoder.
    Dcs(DcsReceiveConfig),
}

/// Immutable high-level receive detector policy.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReceiveConfig {
    /// Selected built-in discriminator-noise response.
    pub noise_filter_profile: NoiseFilterProfile,
    /// MICOR opening threshold.
    pub squelch_open_level: u32,
    /// MICOR closing hysteresis.
    pub squelch_hysteresis: u32,
    /// Linear gain applied only to the CTCSS decoder input.
    pub ctcss_decoder_gain: f32,
    /// VOX comparator threshold in established signed-PCM codes.
    pub vox_threshold: i32,
    /// VOX carrier hang duration in milliseconds.
    pub vox_hang_milliseconds: i32,
    /// Native CTCSS, DCS, or disabled decoder policy.
    pub signaling: ReceiveSignaling,
    /// Whether idle receive voice/calibration work may be frozen.
    pub cpu_saver_enabled: bool,
    /// Native pre-deemphasis delay in frames.
    pub native_squelch_delay_frames: u32,
}

impl Default for ReceiveConfig {
    fn default() -> Self {
        Self {
            noise_filter_profile: NoiseFilterProfile::Standard,
            squelch_open_level: 0,
            squelch_hysteresis: 0,
            ctcss_decoder_gain: 1.0,
            vox_threshold: 0,
            vox_hang_milliseconds: 0,
            signaling: ReceiveSignaling::Disabled,
            cpu_saver_enabled: false,
            native_squelch_delay_frames: 0,
        }
    }
}

/// Immutable receive qualification and duplex policy.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct QualificationConfig {
    /// Carrier source used for receive admission.
    pub carrier_source: CarrierSource,
    /// Subaudible source used for receive admission.
    pub subaudible_source: SubaudibleSource,
    /// Whether carrier may be admitted without subaudible qualification.
    pub subaudible_override: bool,
    /// Whether the advanced full-rate controller transport is active.
    pub advanced_transport: bool,
    /// Whether receive is permitted during transmit.
    pub radio_duplex: bool,
    /// Required 20 ms receive-admission blocks.
    pub receive_on_delay_blocks: u32,
    /// Post-transmit 20 ms guard blocks.
    pub transmit_off_delay_blocks: u32,
}

/// Transmit CTCSS selection and tail policy.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CtcssTransmitConfig {
    /// Default selected transmit frequency in tenths of a hertz.
    pub default_frequency_tenths_hz: i32,
    /// Receive-index-to-transmit-frequency mapping; zero means receive-only.
    pub mapped_frequencies_tenths_hz: [i32; CTCSS_TONE_COUNT],
    /// Normalized CTCSS oscillator peak.
    pub peak: f32,
    /// Configured CTCSS tail duration in milliseconds.
    pub turnoff_duration_milliseconds: i32,
    /// Reverse-burst phase shift in degrees.
    pub turnoff_phase_shift_degrees: f64,
    /// Tail-tone frequency in hertz.
    pub turnoff_tail_tone_hz: f64,
}

impl Default for CtcssTransmitConfig {
    fn default() -> Self {
        Self {
            default_frequency_tenths_hz: 0,
            mapped_frequencies_tenths_hz: [0; CTCSS_TONE_COUNT],
            peak: 0.0,
            turnoff_duration_milliseconds: 0,
            turnoff_phase_shift_degrees: 0.0,
            turnoff_tail_tone_hz: 0.0,
        }
    }
}

/// Transmit DCS selection and tail policy.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DcsTransmitConfig {
    /// Three-octal-digit DCS code represented as an integer.
    pub code: i32,
    /// Whether transmit DCS polarity is inverted.
    pub inverted: bool,
    /// Normalized DCS peak.
    pub peak: f32,
    /// Whether the configured DCS turn-off waveform is emitted.
    pub turnoff_enabled: bool,
    /// DCS turn-off duration in milliseconds.
    pub turnoff_duration_milliseconds: i32,
}

/// Native subaudible signaling selected for transmit rendering.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum TransmitSignaling {
    /// Do not render native subaudible signaling.
    #[default]
    Disabled,
    /// Render native CTCSS according to the selected tone map.
    Ctcss(CtcssTransmitConfig),
    /// Render one native DCS code.
    Dcs(DcsTransmitConfig),
}

/// Level calibration and routing for one physical playback channel.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OutputConfig {
    /// Signal assigned to this output.
    pub route: OutputRoute,
    /// Linear CTCSS/DCS calibration gain.
    pub tone_gain: f32,
    /// Constant tone-path bias.
    pub tone_bias: f32,
}

/// Immutable high-level transmitter policy.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TransmitConfig {
    /// Native CTCSS, DCS, or disabled signaling policy.
    pub signaling: TransmitSignaling,
    /// Selected signaling turn-off behavior.
    pub tone_off_mode: ToneOffMode,
    /// Audio delay after physical PTT is applied, in milliseconds.
    pub settle_time_milliseconds: i32,
    /// Whether idle transmitter rendering may be halted.
    pub cpu_saver_enabled: bool,
    /// Receive blanking armed after a completed transmission, in milliseconds.
    pub receive_blanking_milliseconds: i32,
    /// First physical playback-channel route and calibration.
    pub output_a: OutputConfig,
    /// Second physical playback-channel route and calibration.
    pub output_b: OutputConfig,
}

/// Complete immutable setup for one runtime generation.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SessionConfig {
    /// Generation tag copied into every result, event, and snapshot.
    pub generation_id: u64,
    /// Largest receive callback span preallocated on the control plane.
    pub maximum_receive_frame_count: u32,
    /// Largest transmit callback span preallocated on the control plane.
    pub maximum_transmit_frame_count: u32,
    /// Periodic status cadence in milliseconds.
    pub publication_interval_milliseconds: u32,
    /// Capture channel selected from canonical stereo input.
    pub receive_channel: ReceiveChannel,
    /// Linear post-deemphasis receive gain.
    pub receive_input_gain: f32,
    /// Native receive detector policy.
    pub receive: ReceiveConfig,
    /// Receiver admission and duplex policy.
    pub qualification: QualificationConfig,
    /// Transmitter signaling, level, and routing policy.
    pub transmit: TransmitConfig,
}

impl SessionConfig {
    /// Construct a safe disabled session around explicit callback bounds.
    #[must_use]
    pub fn new(
        generation_id: u64,
        maximum_receive_frame_count: u32,
        maximum_transmit_frame_count: u32,
    ) -> Self {
        Self {
            generation_id,
            maximum_receive_frame_count,
            maximum_transmit_frame_count,
            publication_interval_milliseconds: 50,
            receive_channel: ReceiveChannel::First,
            receive_input_gain: 1.0,
            receive: ReceiveConfig::default(),
            qualification: QualificationConfig::default(),
            transmit: TransmitConfig::default(),
        }
    }
}

/// Hardware state sampled for one receive callback.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ReceiveControls {
    /// Normal USB GPIO carrier input.
    pub hardware_carrier: bool,
    /// Normal parallel-port carrier input.
    pub parallel_carrier: bool,
    /// Normal USB GPIO subaudible input.
    pub hardware_subaudible: bool,
    /// Normal parallel-port subaudible input.
    pub parallel_subaudible: bool,
    /// Whether this callback admits carrier without subaudible qualification.
    pub subaudible_override: bool,
}

/// Control and hardware state sampled for one transmit callback.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TransmitControls {
    /// Whether program/control requests transmit.
    pub external_ptt_request: bool,
    /// Whether hardware reports physical PTT applied.
    pub physical_ptt_applied: bool,
    /// Whether this DAC span is admitted for rendering.
    pub render_admitted: bool,
    /// Whether selected transmit CTCSS is transiently inhibited.
    pub ctcss_inhibit: bool,
    /// Whether program audio is replaced by the calibrated 1 kHz tone.
    pub calibrated_test_tone: bool,
    /// Forced CTCSS frequency in tenths of a hertz, or zero for normal selection.
    pub forced_ctcss_tenths_hz: i32,
}

/// Latest diagnostics from the prepared program-ring provider.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RingObservation {
    /// PCM frames currently retained.
    pub occupancy_frames: u32,
    /// Protected minimum occupancy.
    pub reserve_frames: u32,
    /// Occupancy servo target.
    pub target_frames: u32,
    /// Allocated frame capacity.
    pub capacity_frames: u32,
    /// Current input-to-output conversion ratio.
    pub ratio: f64,
    /// Cumulative absent output samples.
    pub underrun_samples: u64,
    /// Cumulative discarded input samples.
    pub overrun_samples: u64,
    /// Cumulative concealment output samples.
    pub concealment_samples: u64,
}

/// Sample-associated qualification returned by a program-ring callback.
///
/// Construct this value with [`ProgramRingResult::new`] and write it to the
/// destination supplied to [`ProgramRingRenderF32`].
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProgramRingResult {
    observation: RingObservation,
    receiver_keyed: u32,
    ctcss_decoded_index: i32,
    dcs_valid: u32,
}

impl ProgramRingResult {
    /// Construct one valid program-ring result.
    #[must_use]
    pub fn new(
        observation: RingObservation,
        receiver_keyed: bool,
        ctcss_decoded: Option<CtcssToneIndex>,
        dcs_valid: bool,
    ) -> Self {
        Self {
            observation,
            receiver_keyed: u32::from(receiver_keyed),
            ctcss_decoded_index: ctcss_decoded.map_or(-1, |index| i32::from(index.get())),
            dcs_valid: u32::from(dcs_valid),
        }
    }
}

impl Default for ProgramRingResult {
    fn default() -> Self {
        Self::new(RingObservation::default(), false, None, false)
    }
}

/// Process one exact mono normalized-F32 span.
pub type ProcessF32 = unsafe extern "C" fn(*mut c_void, *const f32, *mut f32, u32) -> c_int;
/// Exercise one prepared external object without publishing output.
pub type Warm = unsafe extern "C" fn(*mut c_void, u32) -> c_int;
/// Advance one prepared bypass path without producing PCM.
pub type Bypass = unsafe extern "C" fn(*mut c_void, u32) -> c_int;
/// Render one exact mono normalized-F32 program-ring span.
pub type ProgramRingRenderF32 =
    unsafe extern "C" fn(*mut c_void, *mut f32, u32, *mut ProgramRingResult) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct RawProcessorPort {
    context: *mut c_void,
    process_f32: Option<ProcessF32>,
    bypass: Option<Bypass>,
    warm: Option<Warm>,
}

/// Borrowed, already-prepared mono processor binding.
pub struct ProcessorPort<'a> {
    raw: RawProcessorPort,
    _lifetime: PhantomData<&'a mut c_void>,
}

impl ProcessorPort<'_> {
    /// Construct a pass-through processor with no external state.
    #[must_use]
    pub const fn passthrough() -> Self {
        Self {
            raw: RawProcessorPort {
                context: ptr::null_mut(),
                process_f32: None,
                bypass: None,
                warm: None,
            },
            _lifetime: PhantomData,
        }
    }

    /// Bind callbacks to a borrowed provider context.
    ///
    /// # Safety
    ///
    /// The callbacks must interpret `context` correctly and it must remain
    /// valid until every split session endpoint is dropped. The bound object
    /// must be movable to the assigned endpoint thread and obey the external
    /// ABI's exact-frame, allocation-free, lock-free, nonblocking, non-unwind
    /// contract. Distinct receive and transmit ports must support their
    /// documented concurrent access.
    pub unsafe fn from_raw(
        context: NonNull<c_void>,
        process_f32: ProcessF32,
        bypass: Option<Bypass>,
        warm: Option<Warm>,
    ) -> Self {
        Self {
            raw: RawProcessorPort {
                context: context.as_ptr(),
                process_f32: Some(process_f32),
                bypass,
                warm,
            },
            _lifetime: PhantomData,
        }
    }
}

impl Default for ProcessorPort<'_> {
    fn default() -> Self {
        Self::passthrough()
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawProgramRingPort {
    context: *mut c_void,
    render_f32: Option<ProgramRingRenderF32>,
    warm: Option<Warm>,
}

/// Borrowed, already-prepared program-ring consumer binding.
pub struct ProgramRingPort<'a> {
    raw: RawProgramRingPort,
    _lifetime: PhantomData<&'a mut c_void>,
}

impl ProgramRingPort<'_> {
    /// Construct a provider that renders silence.
    #[must_use]
    pub const fn silence() -> Self {
        Self {
            raw: RawProgramRingPort {
                context: ptr::null_mut(),
                render_f32: None,
                warm: None,
            },
            _lifetime: PhantomData,
        }
    }

    /// Bind callbacks to a borrowed program-ring consumer context.
    ///
    /// # Safety
    ///
    /// The callbacks must interpret `context` correctly and it must remain
    /// valid until every split session endpoint is dropped. The consumer must
    /// be movable to the transmit owner and obey the external ABI's
    /// exact-frame, allocation-free, lock-free, nonblocking, non-unwind
    /// contract.
    pub unsafe fn from_raw(
        context: NonNull<c_void>,
        render_f32: ProgramRingRenderF32,
        warm: Option<Warm>,
    ) -> Self {
        Self {
            raw: RawProgramRingPort {
                context: context.as_ptr(),
                render_f32: Some(render_f32),
                warm,
            },
            _lifetime: PhantomData,
        }
    }
}

impl Default for ProgramRingPort<'_> {
    fn default() -> Self {
        Self::silence()
    }
}

/// Borrowed processing and program-ring objects for one session.
pub struct SessionPorts<'a> {
    /// Receive deemphasis processor.
    pub receive_deemphasis: ProcessorPort<'a>,
    /// Fixed receive filtering graph.
    pub receive_filter: ProcessorPort<'a>,
    /// Prepared one-tone CTCSS notch processors, indexed by decoded tone.
    ///
    /// Leave entries as [`ProcessorPort::passthrough`] when their tone is not
    /// configured for this session.
    pub receive_ctcss_notch: [ProcessorPort<'a>; CTCSS_TONE_COUNT],
    /// Optional receive noise-reduction processor.
    pub receive_noise_reduction: ProcessorPort<'a>,
    /// Receive dynamics processor.
    pub receive_dynamics: ProcessorPort<'a>,
    /// Final transmit program graph.
    pub transmit_program: ProcessorPort<'a>,
    /// Normal DCS NRZ shaping filter, including its configured level compensation.
    pub transmit_dcs_normal_filter: ProcessorPort<'a>,
    /// DCS turn-off sine shaping filter, without normal NRZ level compensation.
    pub transmit_dcs_turnoff_filter: ProcessorPort<'a>,
    /// Program-ring consumer.
    pub program_ring: ProgramRingPort<'a>,
}

impl Default for SessionPorts<'_> {
    fn default() -> Self {
        Self {
            receive_deemphasis: ProcessorPort::passthrough(),
            receive_filter: ProcessorPort::passthrough(),
            receive_ctcss_notch: std::array::from_fn(|_| ProcessorPort::passthrough()),
            receive_noise_reduction: ProcessorPort::passthrough(),
            receive_dynamics: ProcessorPort::passthrough(),
            transmit_program: ProcessorPort::passthrough(),
            transmit_dcs_normal_filter: ProcessorPort::passthrough(),
            transmit_dcs_turnoff_filter: ProcessorPort::passthrough(),
            program_ring: ProgramRingPort::silence(),
        }
    }
}

/// Immediate observation from one receive callback.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReceiveResult {
    /// Runtime generation tag.
    pub generation_id: u64,
    /// First receive stream sample in the callback.
    pub first_sample_index: u64,
    /// Native frames consumed.
    pub frame_count: u32,
    /// Resolved carrier state.
    pub carrier_active: bool,
    /// Resolved subaudible state.
    pub subaudible_active: bool,
    /// Qualified receiver state.
    pub receiver_keyed: bool,
    /// Decoded CTCSS table entry, if any.
    pub ctcss_decoded: Option<CtcssToneIndex>,
    /// Whether the configured DCS code is decoded.
    pub dcs_valid: bool,
    /// Compatibility RSSI peak in established signed-PCM units.
    pub rssi_peak: i16,
    /// Whether the RSSI integration window completed.
    pub rssi_updated: bool,
    /// Post-decoder-gain CTCSS half peak-to-peak level as normalized PCM.
    pub ctcss_decoder_peak: f32,
    /// Raw selected-channel normalized peak.
    pub input_peak: f32,
    /// Raw selected-channel normalized RMS.
    pub input_rms: f32,
    /// Raw samples at a hardware rail.
    pub input_rail_samples: u64,
    /// Processed normalized peak.
    pub output_peak: f32,
    /// Processed normalized RMS.
    pub output_rms: f32,
    /// Processed samples at a hardware rail.
    pub output_rail_samples: u64,
    /// Whether the periodic publication deadline crossed.
    pub periodic_status_due: bool,
}

/// Transmitter signaling state after one callback.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransmitterState {
    /// No transmit or tail activity.
    Idle,
    /// Normal transmit rendering is active.
    Active,
    /// CTCSS or DCS turn-off rendering is active.
    ToneOff,
    /// Final buffered transmit drain is active.
    Finishing,
    /// Transmit completion is pending cleanup.
    Complete,
}

/// Immediate observation from one transmit callback.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TransmitResult {
    /// Runtime generation tag.
    pub generation_id: u64,
    /// First transmit stream sample in the callback.
    pub first_sample_index: u64,
    /// Native frames produced.
    pub frame_count: u32,
    /// Current logical PTT intent.
    pub logical_ptt: bool,
    /// Current transmitter signaling state.
    pub transmitter_state: TransmitterState,
    /// Selected transmit CTCSS frequency in tenths of a hertz.
    pub selected_ctcss_tenths_hz: i32,
    /// Post-graph mono program peak.
    pub program_peak: f32,
    /// Post-graph mono program RMS.
    pub program_rms: f32,
    /// Post-graph program samples at a normalized rail.
    pub program_rail_samples: u64,
    /// Routed stereo output peak.
    pub output_peak: f32,
    /// Routed stereo output RMS.
    pub output_rms: f32,
    /// Routed stereo samples at a normalized rail.
    pub output_rail_samples: u64,
    /// Whether the periodic publication deadline crossed.
    pub periodic_status_due: bool,
    /// Latest program-ring diagnostics.
    pub program_ring: RingObservation,
}

/// Typed payload from one owner-ordered session event.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EventValue {
    /// Carrier state changed.
    Carrier(bool),
    /// Subaudible qualification changed.
    Subaudible(bool),
    /// Qualified receiver state changed.
    ReceiverKeyed(bool),
    /// Decoded CTCSS selection changed.
    CtcssDecode(Option<CtcssToneIndex>),
    /// DCS decode validity changed.
    DcsDecode(bool),
    /// Logical PTT state changed.
    Ptt(bool),
    /// Selected transmit CTCSS frequency changed, in tenths of a hertz.
    CtcssTransmit(i32),
    /// Receive blanking was armed for this many milliseconds.
    ReceiverBlanking(u32),
    /// A borrowed processing provider failed.
    ProviderFailure,
}

/// One owner-ordered, generation-tagged event.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RadioEvent {
    /// Runtime generation tag.
    pub generation_id: u64,
    /// Owner stream position after the event was detected.
    pub sample_index: u64,
    /// Typed event payload.
    pub value: EventValue,
}

/// Best-effort lock-free whole-session diagnostics.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SessionSnapshot {
    /// Runtime generation tag.
    pub generation_id: u64,
    /// Cumulative receive frames.
    pub receive_frames: u64,
    /// Cumulative transmit frames.
    pub transmit_frames: u64,
    /// Latest raw selected-channel receive peak.
    pub receive_input_peak: f32,
    /// Latest raw selected-channel receive RMS.
    pub receive_input_rms: f32,
    /// Latest post-decoder-gain normalized CTCSS half peak-to-peak level.
    pub receive_ctcss_decoder_peak: f32,
    /// Latest processed receive peak.
    pub receive_output_peak: f32,
    /// Latest processed receive RMS.
    pub receive_output_rms: f32,
    /// Latest post-graph transmit program peak.
    pub transmit_program_peak: f32,
    /// Latest post-graph transmit program RMS.
    pub transmit_program_rms: f32,
    /// Latest routed stereo transmit peak.
    pub transmit_output_peak: f32,
    /// Latest routed stereo transmit RMS.
    pub transmit_output_rms: f32,
    /// Cumulative raw receive samples at a hardware rail.
    pub receive_input_rail_samples: u64,
    /// Cumulative processed receive samples at a normalized rail.
    pub receive_output_rail_samples: u64,
    /// Cumulative post-graph transmit program samples at a normalized rail.
    pub transmit_program_rail_samples: u64,
    /// Cumulative routed transmit samples at a normalized rail.
    pub transmit_output_rail_samples: u64,
    /// Cumulative borrowed-provider failures.
    pub provider_failures: u64,
    /// Receive events dropped because their bounded queue was full.
    pub receive_event_drops: u64,
    /// Transmit events dropped because their bounded queue was full.
    pub transmit_event_drops: u64,
    /// Latest carrier state.
    pub carrier_active: bool,
    /// Latest subaudible state.
    pub subaudible_active: bool,
    /// Latest qualified receiver state.
    pub receiver_keyed: bool,
    /// Latest logical PTT intent.
    pub logical_ptt: bool,
    /// Latest decoded CTCSS table entry, if any.
    pub ctcss_decoded: Option<CtcssToneIndex>,
    /// Latest DCS decode state.
    pub dcs_valid: bool,
    /// Latest program-ring diagnostics.
    pub program_ring: RingObservation,
}

mod raw;
mod session;

use raw::*;
pub use session::*;

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests;
