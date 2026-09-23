//! Typed, tolerant resolution of non-processing station configuration.

use std::collections::BTreeMap;
use std::fmt;
use std::str::FromStr;

use crate::{ConfigDocument, ConfigError, ResolutionWarning, ResolutionWarningKind};

const PROFILE_KINDS: [(&str, SettingApplier); 8] = [
    ("asterisk", apply_asterisk),
    ("hardware", apply_hardware),
    ("receive", apply_receive),
    ("transmit", apply_transmit),
    ("ctcss", apply_ctcss),
    ("dcs", apply_dcs),
    ("duplex", apply_duplex),
    ("diagnostics", apply_diagnostics),
];
const ALL_PROFILE_SELECTORS: [&str; 11] = [
    "asterisk_profile",
    "hardware_profile",
    "receive_profile",
    "transmit_profile",
    "ctcss_profile",
    "dcs_profile",
    "duplex_profile",
    "diagnostics_profile",
    "local_profile",
    "link_profile",
    "voice_telemetry_profile",
];
const SILENTLY_IGNORED_SETTINGS: [&str; 2] = ["duplexmode", "duplex_local_repeat_mode"];
const CTCSS_TENTHS_HZ: [u16; 38] = [
    670, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148, 1188,
    1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862, 1928, 2035, 2107,
    2181, 2257, 2336, 2418, 2503,
];

/// Asterisk jitter-buffer implementation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JitterBufferImplementation {
    /// Fixed-size jitter buffering.
    Fixed,
    /// Adaptive jitter buffering.
    Adaptive,
}

impl fmt::Display for JitterBufferImplementation {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Fixed => "fixed",
            Self::Adaptive => "adaptive",
        })
    }
}

/// Effective Asterisk receive jitter-buffer controls.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AsteriskConfig {
    /// Whether jitter buffering is enabled.
    pub jitter_buffer_enabled: bool,
    /// Maximum jitter-buffer length in milliseconds.
    pub jitter_buffer_max_size_ms: u32,
    /// Timestamp discontinuity which causes resynchronization, in milliseconds.
    pub jitter_buffer_resync_threshold_ms: u32,
    /// Selected jitter-buffer algorithm.
    pub jitter_buffer_implementation: JitterBufferImplementation,
    /// Whether jitter-buffer frame diagnostics are enabled.
    pub jitter_buffer_logging_enabled: bool,
    /// Whether Asterisk must use the jitter buffer.
    pub jitter_buffer_force_enabled: bool,
    /// Extra adaptive target depth in milliseconds.
    pub jitter_buffer_target_extra_ms: u32,
    /// Whether video is delayed with buffered audio.
    pub jitter_buffer_video_sync_enabled: bool,
}

impl Default for AsteriskConfig {
    fn default() -> Self {
        Self {
            jitter_buffer_enabled: false,
            jitter_buffer_max_size_ms: 200,
            jitter_buffer_resync_threshold_ms: 1_000,
            jitter_buffer_implementation: JitterBufferImplementation::Fixed,
            jitter_buffer_logging_enabled: false,
            jitter_buffer_force_enabled: false,
            jitter_buffer_target_extra_ms: 40,
            jitter_buffer_video_sync_enabled: false,
        }
    }
}

/// Supported CM119 interface wiring layouts.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareInterfaceType {
    /// DudeUSB-compatible wiring layout (configuration value `0`).
    DudeUsb,
    /// SphUSB-compatible wiring layout (configuration value `1`).
    SphUsb,
}

impl fmt::Display for HardwareInterfaceType {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::DudeUsb => "0",
            Self::SphUsb => "1",
        })
    }
}

/// Signal routed to one CM119 playback channel.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareOutputAssignment {
    /// Silence the output.
    Off,
    /// Route processed voice only.
    Voice,
    /// Route selected CTCSS or DCS signaling only.
    Ctcss,
    /// Route processed voice mixed with selected signaling.
    VoiceCtcss,
    /// Route auxiliary voice.
    AuxiliaryVoice,
}

impl HardwareOutputAssignment {
    const fn carries_signaling(self) -> bool {
        matches!(self, Self::Ctcss | Self::VoiceCtcss)
    }
}

impl fmt::Display for HardwareOutputAssignment {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Off => "off",
            Self::Voice => "voice",
            Self::Ctcss => "ctcss",
            Self::VoiceCtcss => "voice_ctcss",
            Self::AuxiliaryVoice => "auxvoice",
        })
    }
}

/// Direction and initial level of one configurable CM119 GPIO.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpioMode {
    /// Configure the pin as an input.
    Input,
    /// Configure the pin as a low output.
    OutputLow,
    /// Configure the pin as a high output.
    OutputHigh,
}

impl fmt::Display for GpioMode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Input => "in",
            Self::OutputLow => "out0",
            Self::OutputHigh => "out1",
        })
    }
}

/// Assignment of one parallel-port output pin.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParallelOutputAssignment {
    /// Start and hold the output low.
    Low,
    /// Start and hold the output high.
    High,
    /// Carry the physical push-to-talk signal.
    PushToTalk,
}

impl fmt::Display for ParallelOutputAssignment {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Low => "out0",
            Self::High => "out1",
            Self::PushToTalk => "ptt",
        })
    }
}

/// Assignment of one parallel-port input pin.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParallelInputAssignment {
    /// Expose a general-purpose input.
    Input,
    /// Carry carrier-operated squelch.
    Carrier,
    /// Carry an external CTCSS indication.
    Ctcss,
}

impl fmt::Display for ParallelInputAssignment {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Input => "in",
            Self::Carrier => "cor",
            Self::Ctcss => "ctcss",
        })
    }
}

/// Effective optional parallel-port settings.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParallelPortConfig {
    /// Device node used if a pin assignment enables the adapter.
    pub device: String,
    /// Parallel-port I/O base address.
    pub base_address: u32,
    /// Assignments for output pins 2 through 9; `None` leaves a pin unconfigured.
    pub output_assignments: [Option<ParallelOutputAssignment>; 8],
    /// Assignments for input pins 10, 12, 13, and 15.
    pub input_assignments: [Option<ParallelInputAssignment>; 4],
}

impl Default for ParallelPortConfig {
    fn default() -> Self {
        Self {
            device: "/dev/parport0".to_owned(),
            base_address: 0x378,
            output_assignments: [None; 8],
            input_assignments: [None; 4],
        }
    }
}

/// Effective physical interface, routing, and signaling metadata.
#[derive(Clone, Debug, PartialEq)]
pub struct HardwareConfig {
    /// CM119 capture gain in dB relative to the normalized midpoint.
    pub input_gain_db: f64,
    /// CM119 output-A gain in dB relative to the normalized midpoint.
    pub output_a_gain_db: f64,
    /// CM119 output-B gain in dB relative to the normalized midpoint.
    pub output_b_gain_db: f64,
    /// Additional PortAudio capture-buffer request in milliseconds (0-500).
    pub input_extra_buffer_ms: u32,
    /// Additional PortAudio playback-buffer request in milliseconds (0-500).
    pub output_extra_buffer_ms: u32,
    /// Signal routed to output A.
    pub output_a_assignment: HardwareOutputAssignment,
    /// Signal routed to output B.
    pub output_b_assignment: HardwareOutputAssignment,
    /// Whether the physical PTT output is inverted.
    pub ptt_inverted: bool,
    /// Receiver de-emphasis corner in hertz.
    pub deemphasis_corner_hz: f64,
    /// Transmitter pre-emphasis corner in hertz.
    pub preemphasis_corner_hz: f64,
    /// Optional USB device identifier; empty selects automatic resolution.
    pub device_identifier: String,
    /// Optional USB serial number; empty selects automatic resolution.
    pub serial: String,
    /// CM119 interface wiring layout.
    pub interface_type: HardwareInterfaceType,
    /// Whether EEPROM tuning storage is enabled.
    pub eeprom_enabled: bool,
    /// Optional stable USB topology constraint.
    pub gpio_usb_port_path: String,
    /// Nonzero voter-reporting selection.
    pub voter_reporting: u32,
    /// Optional clipping-indicator GPIO number, from 1 through 8.
    pub clip_led_gpio: Option<u8>,
    /// GPIO 1 through 8 modes in pin order.
    pub gpio_modes: [GpioMode; 8],
    /// Optional parallel-port configuration.
    pub parallel_port: ParallelPortConfig,
}

impl Default for HardwareConfig {
    fn default() -> Self {
        Self {
            input_gain_db: 0.0,
            output_a_gain_db: 0.0,
            output_b_gain_db: 0.0,
            input_extra_buffer_ms: 0,
            output_extra_buffer_ms: 0,
            output_a_assignment: HardwareOutputAssignment::VoiceCtcss,
            output_b_assignment: HardwareOutputAssignment::Off,
            ptt_inverted: false,
            deemphasis_corner_hz: 300.0,
            preemphasis_corner_hz: 300.0,
            device_identifier: String::new(),
            serial: String::new(),
            interface_type: HardwareInterfaceType::DudeUsb,
            eeprom_enabled: true,
            gpio_usb_port_path: String::new(),
            voter_reporting: 0,
            clip_led_gpio: None,
            gpio_modes: [GpioMode::Input; 8],
            parallel_port: ParallelPortConfig::default(),
        }
    }
}

/// Receive or transmit signaling protocol.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SignalingMethod {
    /// Carrier-operated squelch without a continuous tone/code.
    Carrier,
    /// Continuous Tone-Coded Squelch System.
    Ctcss,
    /// Digitally Coded Squelch.
    Dcs,
}

impl fmt::Display for SignalingMethod {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Carrier => "carrier",
            Self::Ctcss => "ctcss",
            Self::Dcs => "dcs",
        })
    }
}

/// Receiver audio connection.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReceiveAudioSource {
    /// Do not admit receiver audio.
    Disabled,
    /// Use speaker audio with radio-provided de-emphasis.
    Speaker,
    /// Use flat discriminator audio and module de-emphasis.
    Flat,
}

impl fmt::Display for ReceiveAudioSource {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Disabled => "no",
            Self::Speaker => "speaker",
            Self::Flat => "flat",
        })
    }
}

/// Receiver carrier-operated-squelch source.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CarrierSource {
    /// Disable external carrier qualification.
    Disabled,
    /// Use native discriminator-noise detection.
    Dsp,
    /// Use audio-level VOX detection.
    Vox,
    /// Use a normal-polarity CM119 input.
    Usb,
    /// Use an inverted CM119 input.
    UsbInverted,
    /// Use a normal-polarity parallel input.
    Parallel,
    /// Use an inverted parallel input.
    ParallelInverted,
}

impl fmt::Display for CarrierSource {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Disabled => "no",
            Self::Dsp => "dsp",
            Self::Vox => "vox",
            Self::Usb => "usb",
            Self::UsbInverted => "usbinvert",
            Self::Parallel => "pp",
            Self::ParallelInverted => "ppinvert",
        })
    }
}

/// Effective receiver controls.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ReceiveConfig {
    /// Selected receive signaling method.
    pub signaling_method: SignalingMethod,
    /// Whether idle optional receive processing is bypassed.
    pub cpu_saver_enabled: bool,
    /// Receiver audio connection.
    pub audio_source: ReceiveAudioSource,
    /// Carrier qualification source.
    pub cos_assignment: CarrierSource,
    /// VOX release hold in milliseconds.
    pub vox_hang_ms: u16,
    /// VOX detector threshold.
    pub vox_threshold: u16,
    /// Noise-squelch open-state margin.
    pub noise_squelch_hysteresis: u16,
    /// Native noise-detector filter, `0` or `1`.
    pub noise_filter_type: u8,
    /// Receive tail delay in milliseconds.
    pub squelch_delay_ms: u16,
    /// Carrier acceptance delay in 20 ms frames.
    pub on_delay_frames: u16,
    /// Carrier-squelch threshold on the 0–999 tuning scale.
    pub squelch_level: u16,
    /// Integral receiver frequency in hertz.
    pub frequency_hz: u32,
}

impl Default for ReceiveConfig {
    fn default() -> Self {
        Self {
            signaling_method: SignalingMethod::Carrier,
            cpu_saver_enabled: false,
            audio_source: ReceiveAudioSource::Flat,
            cos_assignment: CarrierSource::Dsp,
            vox_hang_ms: 2_000,
            vox_threshold: 0,
            noise_squelch_hysteresis: 3_000,
            noise_filter_type: 0,
            squelch_delay_ms: 0,
            on_delay_frames: 0,
            squelch_level: 500,
            frequency_hz: 0,
        }
    }
}

/// Effective transmitter controls.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TransmitConfig {
    /// Selected transmit signaling method.
    pub signaling_method: SignalingMethod,
    /// Whether idle optional transmit work is bypassed.
    pub cpu_saver_enabled: bool,
    /// Whether transmitter pre-emphasis is enabled.
    pub preemphasis_enabled: bool,
    /// Delay after PTT assertion in milliseconds.
    pub settle_ms: u32,
    /// Receive blanking interval in milliseconds.
    pub rx_blanking_ms: u16,
    /// Post-transmit receive delay in 20 ms frames.
    pub off_delay_frames: u16,
    /// Integral transmitter frequency in hertz.
    pub frequency_hz: u32,
}

impl Default for TransmitConfig {
    fn default() -> Self {
        Self {
            signaling_method: SignalingMethod::Carrier,
            cpu_saver_enabled: false,
            preemphasis_enabled: true,
            settle_ms: 500,
            rx_blanking_ms: 0,
            off_delay_frames: 0,
            frequency_hz: 0,
        }
    }
}

/// One exact tone supported by the native CTCSS engine.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct CtcssTone(u16);

impl CtcssTone {
    /// Construct a supported tone from tenths of a hertz.
    pub fn from_tenths_hz(tenths_hz: u16) -> Option<Self> {
        CTCSS_TENTHS_HZ
            .contains(&tenths_hz)
            .then_some(Self(tenths_hz))
    }

    /// Return the exact tone in tenths of a hertz.
    pub const fn tenths_hz(self) -> u16 {
        self.0
    }

    /// Return every supported tone in decoder-table order.
    pub fn supported() -> impl ExactSizeIterator<Item = Self> {
        CTCSS_TENTHS_HZ.into_iter().map(Self)
    }

    /// Return this tone's zero-based native decoder-table index.
    pub fn table_index(self) -> usize {
        CTCSS_TENTHS_HZ
            .iter()
            .position(|frequency| *frequency == self.0)
            .expect("CtcssTone can contain only a supported table value")
    }

    /// Return the tone frequency in hertz.
    pub fn as_hz(self) -> f32 {
        f32::from(self.0) / 10.0
    }
}

impl fmt::Display for CtcssTone {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}.{:01}", self.0 / 10, self.0 % 10)
    }
}

impl FromStr for CtcssTone {
    type Err = CtcssToneParseError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let parsed = text
            .trim()
            .parse::<f32>()
            .map_err(|_| CtcssToneParseError)?;
        if !parsed.is_finite() {
            return Err(CtcssToneParseError);
        }
        CTCSS_TENTHS_HZ
            .into_iter()
            .find(|tenths| parsed.to_bits() == (f32::from(*tenths) / 10.0).to_bits())
            .map(Self)
            .ok_or(CtcssToneParseError)
    }
}

/// Error returned for a frequency outside the native CTCSS tone table.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssToneParseError;

impl fmt::Display for CtcssToneParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("frequency is not a supported CTCSS tone")
    }
}

impl std::error::Error for CtcssToneParseError {}

/// Source of received CTCSS qualification.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CtcssSource {
    /// Do not qualify reception with CTCSS.
    Disabled,
    /// Use a normal-polarity CM119 input.
    Usb,
    /// Use an inverted CM119 input.
    UsbInverted,
    /// Use the native CTCSS decoder.
    Dsp,
    /// Use a normal-polarity parallel input.
    Parallel,
    /// Use an inverted parallel input.
    ParallelInverted,
}

impl fmt::Display for CtcssSource {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Disabled => "no",
            Self::Usb => "usb",
            Self::UsbInverted => "usbinvert",
            Self::Dsp => "dsp",
            Self::Parallel => "pp",
            Self::ParallelInverted => "ppinvert",
        })
    }
}

/// CTCSS action performed immediately before PTT release.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CtcssTurnoffMode {
    /// End CTCSS with PTT release.
    None,
    /// Shift CTCSS phase for the configured tail interval.
    PhaseShift,
    /// Remove CTCSS for the configured tail interval.
    ToneRemove,
    /// Replace CTCSS with the configured tail tone.
    TailTone,
}

impl fmt::Display for CtcssTurnoffMode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::None => "no",
            Self::PhaseShift => "ctcss_phase_shift",
            Self::ToneRemove => "ctcss_tone_remove",
            Self::TailTone => "ctcss_tail_tone",
        })
    }
}

/// Effective CTCSS settings retained independently for each signaling direction.
#[derive(Clone, Debug, PartialEq)]
pub struct CtcssConfig {
    /// Supported receive-tone list.
    pub receive_frequencies: Vec<CtcssTone>,
    /// Optional receive-to-transmit tone map.
    pub transmit_frequencies: Vec<CtcssTone>,
    /// Receive CTCSS indication source.
    pub receive_source: CtcssSource,
    /// Decoder-only gain in dB.
    pub receive_decoder_gain_db: f64,
    /// Whether received-tone qualification is bypassed in CTCSS mode.
    pub receive_override_enabled: bool,
    /// Decoder talk-off tolerance, `0` for strict or `1` for relaxed.
    pub receive_relax: u8,
    /// Default transmitted tone.
    pub transmit_default: CtcssTone,
    /// Direct generated CTCSS peak in dBFS.
    pub transmit_peak_dbfs: f64,
    /// Pre-unkey CTCSS action.
    pub turnoff_mode: CtcssTurnoffMode,
    /// Phase shift applied by the phase-tail mode.
    pub phase_shift_degrees: f64,
    /// Non-disabled CTCSS tail duration in milliseconds.
    pub tail_duration_ms: u16,
    /// Replacement tail-tone frequency in hertz.
    pub tail_frequency_hz: f64,
}

impl Default for CtcssConfig {
    fn default() -> Self {
        let default_tone = CtcssTone(1_000);
        Self {
            receive_frequencies: vec![default_tone],
            transmit_frequencies: vec![default_tone],
            receive_source: CtcssSource::Dsp,
            receive_decoder_gain_db: 0.0,
            receive_override_enabled: false,
            receive_relax: 1,
            transmit_default: default_tone,
            transmit_peak_dbfs: -24.0,
            turnoff_mode: CtcssTurnoffMode::PhaseShift,
            phase_shift_degrees: 120.0,
            tail_duration_ms: 180,
            tail_frequency_hz: 55.0,
        }
    }
}

/// DCS code polarity.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum DcsPolarity {
    /// Normal DCS polarity (`N`).
    Normal,
    /// Inverted DCS polarity (`I`).
    Inverted,
}

impl fmt::Display for DcsPolarity {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Normal => "N",
            Self::Inverted => "I",
        })
    }
}

/// Exact three-octal-digit DCS code and polarity selection.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct DcsCode {
    value: u16,
    polarity: DcsPolarity,
}

impl DcsCode {
    /// Return the numeric value represented by the three octal digits.
    pub const fn value(self) -> u16 {
        self.value
    }

    /// Return the selected code polarity.
    pub const fn polarity(self) -> DcsPolarity {
        self.polarity
    }
}

impl fmt::Display for DcsCode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{:03o}{}", self.value, self.polarity)
    }
}

impl FromStr for DcsCode {
    type Err = DcsCodeParseError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let bytes = text.as_bytes();
        if bytes.len() != 4 || !bytes[..3].iter().all(|byte| matches!(byte, b'0'..=b'7')) {
            return Err(DcsCodeParseError);
        }
        let value = u16::from(bytes[0] - b'0') * 64
            + u16::from(bytes[1] - b'0') * 8
            + u16::from(bytes[2] - b'0');
        let polarity = match bytes[3] {
            b'N' | b'n' => DcsPolarity::Normal,
            b'I' | b'i' => DcsPolarity::Inverted,
            _ => return Err(DcsCodeParseError),
        };
        Ok(Self { value, polarity })
    }
}

/// Error returned for text which is not exactly three octal digits plus `N` or `I`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DcsCodeParseError;

impl fmt::Display for DcsCodeParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("DCS code requires exactly three octal digits followed by N or I")
    }
}

impl std::error::Error for DcsCodeParseError {}

/// Effective DCS settings retained independently for each signaling direction.
#[derive(Clone, Debug, PartialEq)]
pub struct DcsConfig {
    /// Receive code and polarity.
    pub receive_code: DcsCode,
    /// Transmit code and polarity.
    pub transmit_code: DcsCode,
    /// Whether the 134.4 Hz end-of-transmission code is enabled.
    pub turnoff_code_enabled: bool,
    /// DCS turn-off duration in milliseconds.
    pub turnoff_duration_ms: u16,
    /// Direct generated DCS peak in dBFS.
    pub peak_dbfs: f64,
}

impl Default for DcsConfig {
    fn default() -> Self {
        let default_code = DcsCode {
            value: 0o23,
            polarity: DcsPolarity::Normal,
        };
        Self {
            receive_code: default_code,
            transmit_code: default_code,
            turnoff_code_enabled: true,
            turnoff_duration_ms: 180,
            peak_dbfs: -24.0,
        }
    }
}

/// Radio receive-during-transmit behavior.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RadioDuplexMode {
    /// Half-duplex radio operation (`0`).
    Half,
    /// Full-duplex radio operation (`1`).
    Full,
}

impl fmt::Display for RadioDuplexMode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Half => "0",
            Self::Full => "1",
        })
    }
}

/// Effective duplex and hardware local-repeat controls.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DuplexConfig {
    /// Whether the radio can receive while transmitting.
    pub radio_mode: RadioDuplexMode,
    /// Hardware local-repeat mixer level from 0 through 999.
    pub local_repeat_level: u16,
}

impl Default for DuplexConfig {
    fn default() -> Self {
        Self {
            radio_mode: RadioDuplexMode::Half,
            local_repeat_level: 0,
        }
    }
}

/// Effective low-level radio diagnostics.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DiagnosticsConfig {
    /// Periodic meter and FIFO status publication interval in milliseconds.
    pub status_publication_interval_ms: u32,
}

impl Default for DiagnosticsConfig {
    fn default() -> Self {
        Self {
            status_publication_interval_ms: 50,
        }
    }
}

/// Complete non-processing configuration for one named radio channel.
#[derive(Clone, Debug, PartialEq)]
pub struct StationConfig {
    /// Whether this named channel is the initial tuning-interface target.
    pub channel_enabled: bool,
    /// Asterisk boundary settings.
    pub asterisk: AsteriskConfig,
    /// Physical interface and routing settings.
    pub hardware: HardwareConfig,
    /// Receiver settings.
    pub receive: ReceiveConfig,
    /// Transmitter settings.
    pub transmit: TransmitConfig,
    /// CTCSS settings.
    pub ctcss: CtcssConfig,
    /// DCS settings.
    pub dcs: DcsConfig,
    /// Duplex and hardware repeat settings.
    pub duplex: DuplexConfig,
    /// Diagnostic settings.
    pub diagnostics: DiagnosticsConfig,
}

impl Default for StationConfig {
    fn default() -> Self {
        Self {
            channel_enabled: true,
            asterisk: AsteriskConfig::default(),
            hardware: HardwareConfig::default(),
            receive: ReceiveConfig::default(),
            transmit: TransmitConfig::default(),
            ctcss: CtcssConfig::default(),
            dcs: DcsConfig::default(),
            duplex: DuplexConfig::default(),
            diagnostics: DiagnosticsConfig::default(),
        }
    }
}

/// Cross-field condition which prevents construction of a safe station configuration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum StationValidationError {
    /// Receive CTCSS was selected with its indication source disabled.
    DisabledCtcssReceiveSource,
    /// Simultaneous receive/transmit CTCSS lists had different lengths.
    CtcssMapLength {
        /// Number of receive tones.
        receive: usize,
        /// Number of transmit-map tones.
        transmit: usize,
    },
    /// Transmit CTCSS or DCS was selected without a hardware signaling route.
    MissingTransmitSignalingRoute,
}

impl fmt::Display for StationValidationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DisabledCtcssReceiveSource => formatter.write_str(
                "receive signaling_method=ctcss requires a non-disabled CTCSS receive source",
            ),
            Self::CtcssMapLength { receive, transmit } => write!(
                formatter,
                "receive/transmit CTCSS maps must have equal lengths ({receive} != {transmit})"
            ),
            Self::MissingTransmitSignalingRoute => formatter.write_str(
                "transmit CTCSS or DCS requires hardware output A or B to carry signaling",
            ),
        }
    }
}

impl std::error::Error for StationValidationError {}

/// Failure to construct a deterministic typed station configuration.
#[derive(Clone, Debug, PartialEq)]
pub enum StationConfigError {
    /// The configuration document could not resolve a channel or selected profile.
    Document(ConfigError),
    /// Case variants assigned incompatible values to one option in one section.
    ConflictingAssignment {
        /// Diagnostic source, normally a configuration-file path.
        source: String,
        /// Configuration section containing the conflict.
        section: String,
        /// Case-normalized option name.
        name: String,
        /// First value encountered.
        first: String,
        /// Conflicting value encountered later.
        second: String,
    },
    /// A channel selected multiple differently named profiles through case variants.
    AmbiguousProfileSelection {
        /// Named channel section.
        channel: String,
        /// Case-normalized selector name.
        selector: String,
    },
    /// Individually valid fields formed an unsafe combination.
    InvalidResolvedConfiguration(StationValidationError),
}

impl fmt::Display for StationConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Document(error) => error.fmt(formatter),
            Self::ConflictingAssignment {
                source,
                section,
                name,
                first,
                second,
            } => write!(
                formatter,
                "{source} [{section}]: conflicting {name} values {first:?} and {second:?}"
            ),
            Self::AmbiguousProfileSelection { channel, selector } => write!(
                formatter,
                "channel [{channel}] has conflicting case variants of {selector}"
            ),
            Self::InvalidResolvedConfiguration(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for StationConfigError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Document(error) => Some(error),
            Self::InvalidResolvedConfiguration(error) => Some(error),
            _ => None,
        }
    }
}

impl From<ConfigError> for StationConfigError {
    fn from(value: ConfigError) -> Self {
        Self::Document(value)
    }
}

impl From<StationValidationError> for StationConfigError {
    fn from(value: StationValidationError) -> Self {
        Self::InvalidResolvedConfiguration(value)
    }
}

/// Fully typed effective station settings and nonfatal ADR 0019 diagnostics.
#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedStationConfig {
    channel: String,
    config: StationConfig,
    warnings: Vec<ResolutionWarning>,
}

impl ResolvedStationConfig {
    /// Resolve shipped defaults, flat sections, and selected scoped profiles.
    pub fn from_document(
        document: &ConfigDocument,
        source: impl Into<String>,
        channel: &str,
    ) -> Result<Self, StationConfigError> {
        let source = source.into();
        document.resolved_section(channel, "general")?;
        let mut config = StationConfig::default();
        let mut warnings = Vec::new();

        for (kind, _) in PROFILE_KINDS {
            reject_ambiguous_selector(document, channel, kind)?;
        }

        apply_overlay(
            &mut config,
            apply_general,
            &source,
            "general",
            document.explicit_values("general"),
            &mut warnings,
        )?;
        apply_overlay(
            &mut config,
            apply_radio,
            &source,
            channel,
            document.explicit_values(channel),
            &mut warnings,
        )?;

        for (kind, apply) in PROFILE_KINDS {
            let resolved_section =
                document.profile_section(channel, kind, &source, &mut warnings)?;
            apply_overlay(
                &mut config,
                apply,
                &source,
                kind,
                document.explicit_values(kind),
                &mut warnings,
            )?;
            if !resolved_section.eq_ignore_ascii_case(kind) {
                apply_overlay(
                    &mut config,
                    apply,
                    &source,
                    &resolved_section,
                    document.explicit_values(&resolved_section),
                    &mut warnings,
                )?;
            }
        }
        config.validate()?;
        Ok(Self {
            channel: channel.to_owned(),
            config,
            warnings,
        })
    }

    /// Return the explicitly selected channel name.
    pub fn channel(&self) -> &str {
        &self.channel
    }

    /// Return the validated effective station configuration.
    pub const fn config(&self) -> &StationConfig {
        &self.config
    }

    /// Consume the result and return its validated station configuration.
    pub fn into_config(self) -> StationConfig {
        self.config
    }

    /// Return every nonfatal fallback warning in precedence order.
    pub fn warnings(&self) -> &[ResolutionWarning] {
        &self.warnings
    }
}

impl StationConfig {
    /// Validate cross-field requirements which cannot be resolved per setting.
    pub fn validate(&self) -> Result<(), StationValidationError> {
        if self.receive.signaling_method == SignalingMethod::Ctcss
            && self.ctcss.receive_source == CtcssSource::Disabled
        {
            return Err(StationValidationError::DisabledCtcssReceiveSource);
        }
        if self.receive.signaling_method == SignalingMethod::Ctcss
            && self.transmit.signaling_method == SignalingMethod::Ctcss
            && self.ctcss.receive_frequencies.len() != self.ctcss.transmit_frequencies.len()
        {
            return Err(StationValidationError::CtcssMapLength {
                receive: self.ctcss.receive_frequencies.len(),
                transmit: self.ctcss.transmit_frequencies.len(),
            });
        }
        if matches!(
            self.transmit.signaling_method,
            SignalingMethod::Ctcss | SignalingMethod::Dcs
        ) && !self.hardware.output_a_assignment.carries_signaling()
            && !self.hardware.output_b_assignment.carries_signaling()
        {
            return Err(StationValidationError::MissingTransmitSignalingRoute);
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
struct Setting<'a> {
    source: &'a str,
    section: &'a str,
    name: &'a str,
    value: &'a str,
}

type SettingApplier =
    for<'a> fn(&mut StationConfig, &str, Setting<'a>, &mut Vec<ResolutionWarning>);

fn reject_ambiguous_selector(
    document: &ConfigDocument,
    channel: &str,
    kind: &str,
) -> Result<(), StationConfigError> {
    let selector = format!("{kind}_profile");
    let mut selected: Option<String> = None;
    for (name, value) in document.explicit_values(channel) {
        if !name.eq_ignore_ascii_case(&selector) {
            continue;
        }
        if let Some(previous) = &selected {
            if !previous.trim().eq_ignore_ascii_case(value.trim()) {
                return Err(StationConfigError::AmbiguousProfileSelection {
                    channel: channel.to_owned(),
                    selector,
                });
            }
        } else {
            selected = Some(value);
        }
    }
    Ok(())
}

fn apply_overlay(
    config: &mut StationConfig,
    apply: SettingApplier,
    source: &str,
    section: &str,
    values: BTreeMap<String, String>,
    warnings: &mut Vec<ResolutionWarning>,
) -> Result<(), StationConfigError> {
    let mut seen = BTreeMap::<String, String>::new();
    for (raw_name, value) in &values {
        let name = raw_name.trim().to_ascii_lowercase();
        if SILENTLY_IGNORED_SETTINGS.contains(&name.as_str()) {
            continue;
        }
        if let Some(previous) = seen.get(&name) {
            if previous.trim() != value.trim() {
                return Err(StationConfigError::ConflictingAssignment {
                    source: source.to_owned(),
                    section: section.to_owned(),
                    name,
                    first: previous.clone(),
                    second: value.clone(),
                });
            }
            continue;
        }
        seen.insert(name.clone(), value.clone());
        let setting = Setting {
            source,
            section,
            name: raw_name,
            value,
        };
        apply(config, &name, setting, warnings);
    }
    Ok(())
}

fn apply_general(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    if name == "channel_enabled" {
        assign_bool(&mut config.channel_enabled, setting, warnings);
    } else {
        unknown_warning(warnings, setting);
    }
}

fn apply_radio(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    if name == "channel_enabled" {
        assign_bool(&mut config.channel_enabled, setting, warnings);
    } else if ALL_PROFILE_SELECTORS.contains(&name) {
        // Profile selectors are consumed by ConfigDocument and the processing resolver.
    } else {
        unknown_warning(warnings, setting);
    }
}

fn apply_asterisk(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.asterisk;
    match name {
        "asterisk_jitter_buffer_enabled" => {
            assign_bool(&mut target.jitter_buffer_enabled, setting, warnings);
        }
        "asterisk_jitter_buffer_max_size_ms" => assign_u32(
            &mut target.jitter_buffer_max_size_ms,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        "asterisk_jitter_buffer_resync_threshold_ms" => assign_u32(
            &mut target.jitter_buffer_resync_threshold_ms,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        "asterisk_jitter_buffer_implementation" => assign_display(
            &mut target.jitter_buffer_implementation,
            setting,
            warnings,
            parse_jitter_implementation,
        ),
        "asterisk_jitter_buffer_logging_enabled" => {
            assign_bool(&mut target.jitter_buffer_logging_enabled, setting, warnings);
        }
        "asterisk_jitter_buffer_force_enabled" => {
            assign_bool(&mut target.jitter_buffer_force_enabled, setting, warnings);
        }
        "asterisk_jitter_buffer_target_extra_ms" => assign_u32(
            &mut target.jitter_buffer_target_extra_ms,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        "asterisk_jitter_buffer_video_sync_enabled" => {
            assign_bool(
                &mut target.jitter_buffer_video_sync_enabled,
                setting,
                warnings,
            );
        }
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_hardware(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.hardware;
    match name {
        "hardware_input_gain_db" => {
            assign_f64(&mut target.input_gain_db, -30.0, 30.0, setting, warnings);
        }
        "hardware_output_a_gain_db" => {
            assign_f64(&mut target.output_a_gain_db, -30.0, 30.0, setting, warnings);
        }
        "hardware_output_b_gain_db" => {
            assign_f64(&mut target.output_b_gain_db, -30.0, 30.0, setting, warnings);
        }
        "hardware_input_extra_buffer_ms" => {
            assign_u32(&mut target.input_extra_buffer_ms, 0, 500, setting, warnings)
        }
        "hardware_output_extra_buffer_ms" => assign_u32(
            &mut target.output_extra_buffer_ms,
            0,
            500,
            setting,
            warnings,
        ),
        "hardware_output_a_assignment" => assign_display(
            &mut target.output_a_assignment,
            setting,
            warnings,
            parse_hardware_output,
        ),
        "hardware_output_b_assignment" => assign_display(
            &mut target.output_b_assignment,
            setting,
            warnings,
            parse_hardware_output,
        ),
        "hardware_ptt_inverted" => assign_bool(&mut target.ptt_inverted, setting, warnings),
        "hardware_deemphasis_corner_hz" => {
            assign_positive_bounded_f64(&mut target.deemphasis_corner_hz, 500.0, setting, warnings)
        }
        "hardware_preemphasis_corner_hz" => {
            assign_positive_bounded_f64(&mut target.preemphasis_corner_hz, 500.0, setting, warnings)
        }
        "hardware_device_identifier" => target.device_identifier = setting.value.to_owned(),
        "hardware_serial" => target.serial = setting.value.to_owned(),
        "hardware_interface_type" => assign_display(
            &mut target.interface_type,
            setting,
            warnings,
            parse_hardware_interface,
        ),
        "hardware_eeprom_enabled" => assign_bool(&mut target.eeprom_enabled, setting, warnings),
        "hardware_gpio_usb_port_path" => target.gpio_usb_port_path = setting.value.to_owned(),
        "hardware_voter_reporting" => assign_u32(
            &mut target.voter_reporting,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        "hardware_clip_led_gpio" => {
            let fallback = target
                .clip_led_gpio
                .map_or_else(|| "0".to_owned(), |pin| pin.to_string());
            if let Some(value) = parsed_u64(setting, 0, 8, fallback, warnings) {
                target.clip_led_gpio = (value != 0).then_some(value as u8);
            }
        }
        "hardware_parallel_port_device" => {
            let fallback = target.parallel_port.device.clone();
            if setting.value.is_empty() {
                invalid_warning(
                    warnings,
                    setting,
                    fallback,
                    "requires a nonempty device path".to_owned(),
                );
            } else {
                target.parallel_port.device = setting.value.to_owned();
            }
        }
        "hardware_parallel_port_base_address" => {
            let fallback = format!("0x{:x}", target.parallel_port.base_address);
            if let Some(value) = parsed_u64(setting, 1, u32::MAX.into(), fallback, warnings) {
                target.parallel_port.base_address = value as u32;
            }
        }
        _ => {
            if let Some(index) = gpio_mode_index(name) {
                assign_display(
                    &mut target.gpio_modes[index],
                    setting,
                    warnings,
                    parse_gpio_mode,
                );
            } else if let Some(index) = parallel_output_index(name) {
                assign_optional_display(
                    &mut target.parallel_port.output_assignments[index],
                    setting,
                    warnings,
                    parse_parallel_output,
                );
            } else if let Some(index) = parallel_input_index(name) {
                assign_optional_display(
                    &mut target.parallel_port.input_assignments[index],
                    setting,
                    warnings,
                    parse_parallel_input,
                );
            } else {
                unknown_warning(warnings, setting);
            }
        }
    }
}

fn apply_receive(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.receive;
    match name {
        "signaling_method" => assign_display(
            &mut target.signaling_method,
            setting,
            warnings,
            parse_signaling_method,
        ),
        "cpu_saver_enabled" => assign_bool(&mut target.cpu_saver_enabled, setting, warnings),
        "audio_source" => assign_display(
            &mut target.audio_source,
            setting,
            warnings,
            parse_receive_audio_source,
        ),
        "cos_assignment" => assign_display(
            &mut target.cos_assignment,
            setting,
            warnings,
            parse_carrier_source,
        ),
        "vox_hang_ms" => assign_u16(
            &mut target.vox_hang_ms,
            0,
            i16::MAX as u16,
            setting,
            warnings,
        ),
        "vox_threshold" => {
            assign_u16(
                &mut target.vox_threshold,
                0,
                i16::MAX as u16,
                setting,
                warnings,
            );
        }
        "noise_squelch_hysteresis" => assign_u16(
            &mut target.noise_squelch_hysteresis,
            0,
            i16::MAX as u16,
            setting,
            warnings,
        ),
        "noise_filter_type" => {
            assign_u8(&mut target.noise_filter_type, 0, 1, setting, warnings);
        }
        "squelch_delay_ms" => {
            assign_u16(&mut target.squelch_delay_ms, 0, 511, setting, warnings);
        }
        "on_delay_frames" => {
            assign_u16(&mut target.on_delay_frames, 0, 3_000, setting, warnings);
        }
        "squelch_level" => {
            assign_u16(&mut target.squelch_level, 0, 999, setting, warnings);
        }
        "frequency_hz" => assign_u32(
            &mut target.frequency_hz,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_transmit(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.transmit;
    match name {
        "signaling_method" => assign_display(
            &mut target.signaling_method,
            setting,
            warnings,
            parse_signaling_method,
        ),
        "cpu_saver_enabled" => assign_bool(&mut target.cpu_saver_enabled, setting, warnings),
        "preemphasis_enabled" => assign_bool(&mut target.preemphasis_enabled, setting, warnings),
        "settle_ms" => assign_u32(&mut target.settle_ms, 0, i32::MAX as u32, setting, warnings),
        "rx_blanking_ms" => assign_u16(
            &mut target.rx_blanking_ms,
            0,
            i16::MAX as u16,
            setting,
            warnings,
        ),
        "off_delay_frames" => {
            assign_u16(&mut target.off_delay_frames, 0, 3_000, setting, warnings);
        }
        "frequency_hz" => assign_u32(
            &mut target.frequency_hz,
            0,
            i32::MAX as u32,
            setting,
            warnings,
        ),
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_ctcss(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.ctcss;
    match name {
        "receive_frequencies" => {
            assign_tone_list(&mut target.receive_frequencies, setting, warnings)
        }
        "transmit_frequencies" => {
            assign_tone_list(&mut target.transmit_frequencies, setting, warnings)
        }
        "receive_source" => assign_display(
            &mut target.receive_source,
            setting,
            warnings,
            parse_ctcss_source,
        ),
        "receive_decoder_gain_db" => assign_f64(
            &mut target.receive_decoder_gain_db,
            -60.0,
            60.0,
            setting,
            warnings,
        ),
        "receive_override_enabled" => {
            assign_bool(&mut target.receive_override_enabled, setting, warnings);
        }
        "receive_relax" => assign_u8(&mut target.receive_relax, 0, 1, setting, warnings),
        "transmit_default_hz" => {
            let fallback = target.transmit_default.to_string();
            match setting.value.parse::<CtcssTone>() {
                Ok(value) => target.transmit_default = value,
                Err(error) => invalid_warning(warnings, setting, fallback, error.to_string()),
            }
        }
        "transmit_peak_dbfs" => assign_f64(
            &mut target.transmit_peak_dbfs,
            -90.0,
            0.0,
            setting,
            warnings,
        ),
        "turnoff_mode" => assign_display(
            &mut target.turnoff_mode,
            setting,
            warnings,
            parse_ctcss_turnoff_mode,
        ),
        "phase_shift_degrees" => assign_positive_f64(
            &mut target.phase_shift_degrees,
            setting,
            warnings,
            "requires a positive finite number of degrees",
        ),
        "tail_duration_ms" => {
            assign_u16(
                &mut target.tail_duration_ms,
                40,
                i16::MAX as u16,
                setting,
                warnings,
            );
        }
        "tail_frequency_hz" => assign_positive_f64(
            &mut target.tail_frequency_hz,
            setting,
            warnings,
            "requires a positive finite frequency",
        ),
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_dcs(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.dcs;
    match name {
        "receive_code" => assign_dcs_code(&mut target.receive_code, setting, warnings),
        "transmit_code" => assign_dcs_code(&mut target.transmit_code, setting, warnings),
        "turnoff_code_enabled" => {
            assign_bool(&mut target.turnoff_code_enabled, setting, warnings);
        }
        "turnoff_duration_ms" => {
            assign_u16(&mut target.turnoff_duration_ms, 150, 200, setting, warnings);
        }
        "peak_dbfs" => {
            assign_f64(&mut target.peak_dbfs, -90.0, 0.0, setting, warnings);
        }
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_duplex(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.duplex;
    match name {
        "duplex_radio_mode" => assign_display(
            &mut target.radio_mode,
            setting,
            warnings,
            parse_radio_duplex_mode,
        ),
        "duplex_local_repeat_level" => {
            assign_u16(&mut target.local_repeat_level, 0, 999, setting, warnings);
        }
        _ => unknown_warning(warnings, setting),
    }
}

fn apply_diagnostics(
    config: &mut StationConfig,
    name: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let target = &mut config.diagnostics;
    match name {
        "diagnostics_status_publication_interval_ms" => assign_u32(
            &mut target.status_publication_interval_ms,
            1,
            u32::MAX,
            setting,
            warnings,
        ),
        _ => unknown_warning(warnings, setting),
    }
}

fn assign_bool(target: &mut bool, setting: Setting<'_>, warnings: &mut Vec<ResolutionWarning>) {
    let fallback = yes_no(*target);
    match setting.value.trim() {
        value if value.eq_ignore_ascii_case("yes") => *target = true,
        value if value.eq_ignore_ascii_case("no") => *target = false,
        _ => invalid_warning(warnings, setting, fallback, "requires yes or no".to_owned()),
    }
}

fn assign_u8(
    target: &mut u8,
    minimum: u8,
    maximum: u8,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    if let Some(value) = parsed_u64(
        setting,
        minimum.into(),
        maximum.into(),
        target.to_string(),
        warnings,
    ) {
        *target = value as u8;
    }
}

fn assign_u16(
    target: &mut u16,
    minimum: u16,
    maximum: u16,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    if let Some(value) = parsed_u64(
        setting,
        minimum.into(),
        maximum.into(),
        target.to_string(),
        warnings,
    ) {
        *target = value as u16;
    }
}

fn assign_u32(
    target: &mut u32,
    minimum: u32,
    maximum: u32,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    if let Some(value) = parsed_u64(
        setting,
        minimum.into(),
        maximum.into(),
        target.to_string(),
        warnings,
    ) {
        *target = value as u32;
    }
}

fn parsed_u64(
    setting: Setting<'_>,
    minimum: u64,
    maximum: u64,
    fallback: String,
    warnings: &mut Vec<ResolutionWarning>,
) -> Option<u64> {
    match parse_c_unsigned(setting.value) {
        Ok(value) if value >= minimum && value <= maximum => Some(value),
        _ => {
            invalid_warning(
                warnings,
                setting,
                fallback,
                format!("requires an integer from {minimum} through {maximum}"),
            );
            None
        }
    }
}

fn parse_c_unsigned(value: &str) -> Result<u64, ()> {
    let text = value.trim();
    let unsigned = text.strip_prefix('+').unwrap_or(text);
    if unsigned.is_empty() || unsigned.starts_with('-') {
        return Err(());
    }
    parse_c_magnitude(unsigned)
}

fn parse_c_magnitude(text: &str) -> Result<u64, ()> {
    let (digits, radix) =
        if let Some(rest) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
            (rest, 16)
        } else if text.len() > 1 && text.starts_with('0') {
            (&text[1..], 8)
        } else {
            (text, 10)
        };
    if digits.is_empty() {
        return Err(());
    }
    u64::from_str_radix(digits, radix).map_err(|_| ())
}

fn assign_f64(
    target: &mut f64,
    minimum: f64,
    maximum: f64,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let fallback = format_number(*target);
    match setting.value.trim().parse::<f64>() {
        Ok(value) if value.is_finite() && value >= minimum && value <= maximum => {
            *target = value;
        }
        _ => invalid_warning(
            warnings,
            setting,
            fallback,
            format!("requires a finite number from {minimum} through {maximum}"),
        ),
    }
}

fn assign_positive_f64(
    target: &mut f64,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
    reason: &str,
) {
    let fallback = format_number(*target);
    match setting.value.trim().parse::<f64>() {
        Ok(value) if value.is_finite() && value > 0.0 => *target = value,
        _ => invalid_warning(warnings, setting, fallback, reason.to_owned()),
    }
}

fn assign_positive_bounded_f64(
    target: &mut f64,
    maximum: f64,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let fallback = format_number(*target);
    match setting.value.trim().parse::<f64>() {
        Ok(value) if value.is_finite() && value > 0.0 && value <= maximum => *target = value,
        _ => invalid_warning(
            warnings,
            setting,
            fallback,
            format!("requires a positive finite number through {maximum}"),
        ),
    }
}

fn assign_display<T>(
    target: &mut T,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
    parse: impl FnOnce(&str) -> Result<T, String>,
) where
    T: fmt::Display,
{
    let fallback = target.to_string();
    match parse(setting.value.trim()) {
        Ok(value) => *target = value,
        Err(reason) => invalid_warning(warnings, setting, fallback, reason),
    }
}

fn assign_optional_display<T>(
    target: &mut Option<T>,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
    parse: impl FnOnce(&str) -> Result<T, String>,
) where
    T: fmt::Display,
{
    let fallback = target
        .as_ref()
        .map_or_else(|| "unconfigured".to_owned(), ToString::to_string);
    match parse(setting.value.trim()) {
        Ok(value) => *target = Some(value),
        Err(reason) => invalid_warning(warnings, setting, fallback, reason),
    }
}

fn assign_tone_list(
    target: &mut Vec<CtcssTone>,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let fallback = format_tone_list(target);
    match parse_tone_list(setting.value) {
        Ok(value) => *target = value,
        Err(reason) => invalid_warning(warnings, setting, fallback, reason),
    }
}

fn assign_dcs_code(
    target: &mut DcsCode,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    let fallback = target.to_string();
    match setting.value.parse::<DcsCode>() {
        Ok(value) => *target = value,
        Err(error) => invalid_warning(warnings, setting, fallback, error.to_string()),
    }
}

fn parse_tone_list(value: &str) -> Result<Vec<CtcssTone>, String> {
    if value.trim().is_empty() {
        return Err("requires a nonempty comma-separated list of supported CTCSS tones".to_owned());
    }
    let tones = value
        .split(',')
        .map(|entry| {
            entry.trim().parse::<CtcssTone>().map_err(|_| {
                "requires a nonempty comma-separated list of supported CTCSS tones".to_owned()
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    if tones.len() > CTCSS_TENTHS_HZ.len() {
        return Err(format!(
            "supports at most {} CTCSS tone entries",
            CTCSS_TENTHS_HZ.len()
        ));
    }
    Ok(tones)
}

fn format_tone_list(tones: &[CtcssTone]) -> String {
    tones
        .iter()
        .map(ToString::to_string)
        .collect::<Vec<_>>()
        .join(",")
}

fn parse_jitter_implementation(value: &str) -> Result<JitterBufferImplementation, String> {
    if value.eq_ignore_ascii_case("fixed") {
        Ok(JitterBufferImplementation::Fixed)
    } else if value.eq_ignore_ascii_case("adaptive") {
        Ok(JitterBufferImplementation::Adaptive)
    } else {
        Err("requires fixed or adaptive".to_owned())
    }
}

fn parse_hardware_interface(value: &str) -> Result<HardwareInterfaceType, String> {
    match parse_c_unsigned(value) {
        Ok(0) => Ok(HardwareInterfaceType::DudeUsb),
        Ok(1) => Ok(HardwareInterfaceType::SphUsb),
        _ => Err("requires 0 or 1".to_owned()),
    }
}

fn parse_hardware_output(value: &str) -> Result<HardwareOutputAssignment, String> {
    if value.eq_ignore_ascii_case("off") {
        Ok(HardwareOutputAssignment::Off)
    } else if value.eq_ignore_ascii_case("voice") {
        Ok(HardwareOutputAssignment::Voice)
    } else if value.eq_ignore_ascii_case("ctcss") {
        Ok(HardwareOutputAssignment::Ctcss)
    } else if value.eq_ignore_ascii_case("voice_ctcss") {
        Ok(HardwareOutputAssignment::VoiceCtcss)
    } else if value.eq_ignore_ascii_case("auxvoice") {
        Ok(HardwareOutputAssignment::AuxiliaryVoice)
    } else {
        Err("requires off, voice, ctcss, voice_ctcss, or auxvoice".to_owned())
    }
}

fn parse_gpio_mode(value: &str) -> Result<GpioMode, String> {
    if value.eq_ignore_ascii_case("in") {
        Ok(GpioMode::Input)
    } else if value.eq_ignore_ascii_case("out0") {
        Ok(GpioMode::OutputLow)
    } else if value.eq_ignore_ascii_case("out1") {
        Ok(GpioMode::OutputHigh)
    } else {
        Err("requires in, out0, or out1".to_owned())
    }
}

fn parse_parallel_output(value: &str) -> Result<ParallelOutputAssignment, String> {
    if value.eq_ignore_ascii_case("out0") {
        Ok(ParallelOutputAssignment::Low)
    } else if value.eq_ignore_ascii_case("out1") {
        Ok(ParallelOutputAssignment::High)
    } else if value.eq_ignore_ascii_case("ptt") {
        Ok(ParallelOutputAssignment::PushToTalk)
    } else {
        Err("requires out0, out1, or ptt".to_owned())
    }
}

fn parse_parallel_input(value: &str) -> Result<ParallelInputAssignment, String> {
    if value.eq_ignore_ascii_case("in") {
        Ok(ParallelInputAssignment::Input)
    } else if value.eq_ignore_ascii_case("cor") {
        Ok(ParallelInputAssignment::Carrier)
    } else if value.eq_ignore_ascii_case("ctcss") {
        Ok(ParallelInputAssignment::Ctcss)
    } else {
        Err("requires in, cor, or ctcss".to_owned())
    }
}

fn parse_signaling_method(value: &str) -> Result<SignalingMethod, String> {
    if value.eq_ignore_ascii_case("carrier") {
        Ok(SignalingMethod::Carrier)
    } else if value.eq_ignore_ascii_case("ctcss") {
        Ok(SignalingMethod::Ctcss)
    } else if value.eq_ignore_ascii_case("dcs") {
        Ok(SignalingMethod::Dcs)
    } else {
        Err("requires carrier, ctcss, or dcs".to_owned())
    }
}

fn parse_receive_audio_source(value: &str) -> Result<ReceiveAudioSource, String> {
    if value.eq_ignore_ascii_case("no") {
        Ok(ReceiveAudioSource::Disabled)
    } else if value.eq_ignore_ascii_case("speaker") {
        Ok(ReceiveAudioSource::Speaker)
    } else if value.eq_ignore_ascii_case("flat") {
        Ok(ReceiveAudioSource::Flat)
    } else {
        Err("requires no, speaker, or flat".to_owned())
    }
}

fn parse_carrier_source(value: &str) -> Result<CarrierSource, String> {
    if value.eq_ignore_ascii_case("no") {
        Ok(CarrierSource::Disabled)
    } else if value.eq_ignore_ascii_case("dsp") {
        Ok(CarrierSource::Dsp)
    } else if value.eq_ignore_ascii_case("vox") {
        Ok(CarrierSource::Vox)
    } else if value.eq_ignore_ascii_case("usb") {
        Ok(CarrierSource::Usb)
    } else if value.eq_ignore_ascii_case("usbinvert") {
        Ok(CarrierSource::UsbInverted)
    } else if value.eq_ignore_ascii_case("pp") {
        Ok(CarrierSource::Parallel)
    } else if value.eq_ignore_ascii_case("ppinvert") {
        Ok(CarrierSource::ParallelInverted)
    } else {
        Err("requires no, dsp, vox, usb, usbinvert, pp, or ppinvert".to_owned())
    }
}

fn parse_ctcss_source(value: &str) -> Result<CtcssSource, String> {
    if value.eq_ignore_ascii_case("no") {
        Ok(CtcssSource::Disabled)
    } else if value.eq_ignore_ascii_case("usb") {
        Ok(CtcssSource::Usb)
    } else if value.eq_ignore_ascii_case("usbinvert") {
        Ok(CtcssSource::UsbInverted)
    } else if value.eq_ignore_ascii_case("dsp") {
        Ok(CtcssSource::Dsp)
    } else if value.eq_ignore_ascii_case("pp") {
        Ok(CtcssSource::Parallel)
    } else if value.eq_ignore_ascii_case("ppinvert") {
        Ok(CtcssSource::ParallelInverted)
    } else {
        Err("requires no, usb, usbinvert, dsp, pp, or ppinvert".to_owned())
    }
}

fn parse_ctcss_turnoff_mode(value: &str) -> Result<CtcssTurnoffMode, String> {
    if value.eq_ignore_ascii_case("no") {
        Ok(CtcssTurnoffMode::None)
    } else if value.eq_ignore_ascii_case("ctcss_phase_shift") {
        Ok(CtcssTurnoffMode::PhaseShift)
    } else if value.eq_ignore_ascii_case("ctcss_tone_remove") {
        Ok(CtcssTurnoffMode::ToneRemove)
    } else if value.eq_ignore_ascii_case("ctcss_tail_tone") {
        Ok(CtcssTurnoffMode::TailTone)
    } else {
        Err("requires no, ctcss_phase_shift, ctcss_tone_remove, or ctcss_tail_tone".to_owned())
    }
}

fn parse_radio_duplex_mode(value: &str) -> Result<RadioDuplexMode, String> {
    match parse_c_unsigned(value) {
        Ok(0) => Ok(RadioDuplexMode::Half),
        Ok(1) => Ok(RadioDuplexMode::Full),
        _ => Err("requires 0 or 1".to_owned()),
    }
}

fn gpio_mode_index(name: &str) -> Option<usize> {
    let pin = name
        .strip_prefix("hardware_gpio_")?
        .strip_suffix("_mode")?
        .parse::<usize>()
        .ok()?;
    (1..=8).contains(&pin).then(|| pin - 1)
}

fn parallel_output_index(name: &str) -> Option<usize> {
    const NAMES: [&str; 8] = [
        "hardware_parallel_pin_2_assignment",
        "hardware_parallel_pin_3_assignment",
        "hardware_parallel_pin_4_assignment",
        "hardware_parallel_pin_5_assignment",
        "hardware_parallel_pin_6_assignment",
        "hardware_parallel_pin_7_assignment",
        "hardware_parallel_pin_8_assignment",
        "hardware_parallel_pin_9_assignment",
    ];
    NAMES.iter().position(|candidate| *candidate == name)
}

fn parallel_input_index(name: &str) -> Option<usize> {
    const NAMES: [&str; 4] = [
        "hardware_parallel_pin_10_assignment",
        "hardware_parallel_pin_12_assignment",
        "hardware_parallel_pin_13_assignment",
        "hardware_parallel_pin_15_assignment",
    ];
    NAMES.iter().position(|candidate| *candidate == name)
}

fn yes_no(value: bool) -> String {
    if value { "yes" } else { "no" }.to_owned()
}

fn format_number(value: f64) -> String {
    value.to_string()
}

fn invalid_warning(
    warnings: &mut Vec<ResolutionWarning>,
    setting: Setting<'_>,
    fallback: String,
    reason: String,
) {
    warnings.push(ResolutionWarning {
        kind: ResolutionWarningKind::InvalidValue,
        source: setting.source.to_owned(),
        section: setting.section.to_owned(),
        name: setting.name.to_owned(),
        supplied_value: setting.value.to_owned(),
        fallback,
        reason,
    });
}

fn unknown_warning(warnings: &mut Vec<ResolutionWarning>, setting: Setting<'_>) {
    warnings.push(ResolutionWarning {
        kind: ResolutionWarningKind::UnknownOption,
        source: setting.source.to_owned(),
        section: setting.section.to_owned(),
        name: setting.name.to_owned(),
        supplied_value: setting.value.to_owned(),
        fallback: "ignored".to_owned(),
        reason: "is unknown and was ignored".to_owned(),
    });
}

#[cfg(test)]
#[path = "tests/station_config_tests.rs"]
mod tests;
