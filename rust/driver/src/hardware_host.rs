//! Physical-device ownership for one prepared station.
//!
//! The audio callbacks own only native PCM work. This host resolves one
//! canonical CM119, applies its ALSA mixer setup once, and runs USB/parallel
//! control I/O on a bounded non-real-time service thread.

use std::fmt;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU8, AtomicU64, Ordering};
use std::sync::mpsc::{
    Receiver, RecvTimeoutError, SyncSender, TryRecvError, TrySendError, sync_channel,
};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use usbradioplus_asl3::{
    ControlAction, ControlMessage, CtcssTone, GpioPin, OutputRequest, ParallelPin, RemoteRadio,
};
use usbradioplus_audio::{
    AudioError, AudioProvider, Mixer, MixerPath, SelectedDevice, StreamTiming,
};
use usbradioplus_core::ChannelConfiguration;
use usbradioplus_gpio::{
    Cm119Device, Cm119Inputs, Cm119Outputs, Cm119Pulse, Cm119Statistics, EepromImage, GpioAdapter,
    GpioError, ParallelDevice, ParallelInputs, ParallelOutputs, ParallelPulse, ParallelRtx,
    ParallelStatistics,
};
use usbradioplus_station::{
    ControllerTransport, HardwareInputs, HardwarePlan, HardwarePlanError, ParallelPlan,
    ReceiveObservation, SelectedHardwarePlan, SharedHardwareState, StationControlHost,
    StationMedia, StationRuntime, StationRuntimeStatistics,
};

const SERVICE_INTERVAL: Duration = Duration::from_millis(5);
const RETRY_INTERVAL: Duration = Duration::from_millis(500);
const CONTROL_QUEUE_CAPACITY: usize = 32;
const INPUT_EVENT_QUEUE_CAPACITY: usize = 64;
const CONTROL_TICK_MILLISECONDS: u32 = 100;
const CONTROL_RESPONSE_TIMEOUT: Duration = Duration::from_secs(2);
const CLIP_LED_HOLD: Duration = Duration::from_millis(500);
const PARALLEL_BINARY_MASK: u8 = 0xf0;
const PARALLEL_RTX_CONTROL_MASK: u8 = 0x1f;
const PARALLEL_RTX_TRANSMIT_MASK: u8 = 0x08;
const PARALLEL_RTX_POWER_MASK: u8 = 0x10;
const EEPROM_TUNING_START_WORD: usize = 51;
const EEPROM_RECEIVE_MIXER_OFFSET: usize = 1;
const EEPROM_TRANSMIT_A_MIXER_OFFSET: usize = 2;
const EEPROM_TRANSMIT_B_MIXER_OFFSET: usize = 3;
const EEPROM_TRANSMIT_CTCSS_OFFSET: usize = 8;
const EEPROM_RECEIVE_SQUELCH_OFFSET: usize = 9;
const MAXIMUM_TUNING_LEVEL: u16 = 999;

/// Hardware operation which most recently failed.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareOperation {
    /// Opening the selected CM119 HID interface.
    OpenCm119 = 1,
    /// Opening the configured parallel port.
    OpenParallel,
    /// Publishing CM119 outputs.
    PublishCm119,
    /// Servicing CM119 HID input and output.
    ServiceCm119,
    /// Reading CM119 inputs.
    ReadCm119,
    /// Reading CM119 statistics.
    ReadCm119Statistics,
    /// Publishing parallel-port outputs.
    PublishParallel,
    /// Servicing parallel-port input and output.
    ServiceParallel,
    /// Reading parallel-port inputs.
    ReadParallel,
    /// Reading parallel-port statistics.
    ReadParallelStatistics,
    /// Scheduling a CM119 GPIO pulse.
    PulseCm119,
    /// Scheduling a parallel-port pulse.
    PulseParallel,
    /// Reading CM119 tuning EEPROM.
    ReadEeprom,
    /// Writing CM119 tuning EEPROM.
    WriteEeprom,
    /// Applying a parallel-port binary channel selection.
    SelectChannel,
    /// Programming a parallel-port RTX synthesizer.
    ProgramRemoteRadio,
    /// Releasing a parallel-port RTX transmitter.
    ClearRemoteRadioTransmit,
    /// Applying CM119 hardware local-repeat gain or switching.
    ApplySidetone,
}

impl HardwareOperation {
    fn from_u8(value: u8) -> Option<Self> {
        match value {
            1 => Some(Self::OpenCm119),
            2 => Some(Self::OpenParallel),
            3 => Some(Self::PublishCm119),
            4 => Some(Self::ServiceCm119),
            5 => Some(Self::ReadCm119),
            6 => Some(Self::ReadCm119Statistics),
            7 => Some(Self::PublishParallel),
            8 => Some(Self::ServiceParallel),
            9 => Some(Self::ReadParallel),
            10 => Some(Self::ReadParallelStatistics),
            11 => Some(Self::PulseCm119),
            12 => Some(Self::PulseParallel),
            13 => Some(Self::ReadEeprom),
            14 => Some(Self::WriteEeprom),
            15 => Some(Self::SelectChannel),
            16 => Some(Self::ProgramRemoteRadio),
            17 => Some(Self::ClearRemoteRadioTransmit),
            18 => Some(Self::ApplySidetone),
            _ => None,
        }
    }
}

impl fmt::Display for HardwareOperation {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::OpenCm119 => "open CM119",
            Self::OpenParallel => "open parallel port",
            Self::PublishCm119 => "publish CM119 outputs",
            Self::ServiceCm119 => "service CM119",
            Self::ReadCm119 => "read CM119 inputs",
            Self::ReadCm119Statistics => "read CM119 statistics",
            Self::PublishParallel => "publish parallel outputs",
            Self::ServiceParallel => "service parallel port",
            Self::ReadParallel => "read parallel inputs",
            Self::ReadParallelStatistics => "read parallel statistics",
            Self::PulseCm119 => "schedule CM119 pulse",
            Self::PulseParallel => "schedule parallel pulse",
            Self::ReadEeprom => "read CM119 EEPROM",
            Self::WriteEeprom => "write CM119 EEPROM",
            Self::SelectChannel => "select parallel channel",
            Self::ProgramRemoteRadio => "program parallel remote radio",
            Self::ClearRemoteRadioTransmit => "release parallel remote-radio transmitter",
            Self::ApplySidetone => "apply CM119 hardware local repeat",
        })
    }
}

/// Failure to compose or operate one physical station.
#[derive(Debug)]
pub enum HardwareStationError {
    /// PortAudio/ALSA selection, mixer, or stream failure.
    Audio(AudioError),
    /// The selected audio endpoint conflicts with the configured HID identity.
    Plan(HardwarePlanError),
    /// CM119 or parallel-port adapter failure.
    Gpio {
        /// Operation which failed.
        operation: HardwareOperation,
        /// Adapter failure.
        source: GpioError,
    },
    /// A GPIO or parallel output is not configured for product control.
    UnavailableOutput,
    /// A pulse duration cannot be represented in milliseconds.
    PulseTooLong,
    /// The bounded control queue is full.
    ControlQueueFull,
    /// Hardware service is not running.
    NotRunning,
    /// EEPROM tuning storage is disabled in station configuration.
    EepromDisabled,
    /// A synchronous control-plane request did not complete within its bound.
    ControlTimeout,
    /// The hardware service thread terminated unexpectedly.
    WorkerPanicked,
}

impl fmt::Display for HardwareStationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Audio(error) => write!(formatter, "audio hardware failed: {error}"),
            Self::Plan(error) => write!(formatter, "hardware identity failed: {error}"),
            Self::Gpio { operation, source } => write!(formatter, "{operation} failed: {source}"),
            Self::UnavailableOutput => formatter.write_str("output is not configured for control"),
            Self::PulseTooLong => formatter.write_str("output pulse duration is too long"),
            Self::ControlQueueFull => formatter.write_str("hardware control queue is full"),
            Self::NotRunning => formatter.write_str("hardware service is not running"),
            Self::EepromDisabled => formatter.write_str("EEPROM tuning storage is disabled"),
            Self::ControlTimeout => formatter.write_str("hardware control request timed out"),
            Self::WorkerPanicked => formatter.write_str("hardware service thread panicked"),
        }
    }
}

impl std::error::Error for HardwareStationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Audio(error) => Some(error),
            Self::Plan(error) => Some(error),
            Self::Gpio { source, .. } => Some(source),
            _ => None,
        }
    }
}

impl From<AudioError> for HardwareStationError {
    fn from(error: AudioError) -> Self {
        Self::Audio(error)
    }
}

impl From<HardwarePlanError> for HardwareStationError {
    fn from(error: HardwarePlanError) -> Self {
        Self::Plan(error)
    }
}

/// Lock-free snapshot of non-real-time hardware service health.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HardwareServiceStatistics {
    /// Completed service attempts.
    pub service_cycles: u64,
    /// Service attempts which failed.
    pub service_failures: u64,
    /// Most recent failing operation, retained after recovery.
    pub last_failure: Option<HardwareOperation>,
    /// Latest CM119 adapter statistics.
    pub cm119: Cm119Statistics,
    /// Latest parallel-port statistics when configured.
    pub parallel: Option<ParallelStatistics>,
    /// Input edges dropped because the bounded consumer queue was full.
    pub dropped_input_events: u64,
}

/// Physical digital input which changed state.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareInput {
    /// One-based CM119 GPIO input.
    Cm119(GpioPin),
    /// Physical parallel-port status input: 10, 12, 13, or 15.
    Parallel(u8),
}

/// One ordinary hardware-input edge for the Asterisk control boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HardwareInputEvent {
    /// Input whose logical state changed.
    pub input: HardwareInput,
    /// New logical state.
    pub active: bool,
}

/// Persistent operator-controlled hardware state transferable across reload.
///
/// Timed pulses are deliberately excluded and restart from the inactive state;
/// retained state covers ordinary outputs, parallel channel selection, and the
/// current parallel remote-radio request.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HardwareTransientState {
    cm119_output_mask: u8,
    parallel_output_mask: u8,
    remote_radio: Option<RemoteRadio>,
}

impl HardwareTransientState {
    /// Return the retained logical CM119 output mask.
    #[must_use]
    pub const fn cm119_output_mask(self) -> u8 {
        self.cm119_output_mask
    }

    /// Return the retained logical parallel-port output and channel mask.
    #[must_use]
    pub const fn parallel_output_mask(self) -> u8 {
        self.parallel_output_mask
    }

    /// Return the retained parallel remote-radio request.
    #[must_use]
    pub const fn remote_radio(self) -> Option<RemoteRadio> {
        self.remote_radio
    }
}

/// Complete physical-station diagnostic snapshot.
#[derive(Clone, Debug, PartialEq)]
pub struct HardwareStationDiagnostics {
    /// PortAudio and native callback statistics.
    pub runtime: StationRuntimeStatistics,
    /// PortAudio stream latency and rate.
    pub timing: StreamTiming,
    /// CM119 and optional parallel-port service statistics.
    pub hardware: HardwareServiceStatistics,
    /// Latest complete native receive measurement.
    pub receive: ReceiveObservation,
}

/// One user-adjustable CM119 audio mixer path.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareMixer {
    /// Receiver capture level.
    Receive,
    /// First transmitter playback output.
    TransmitA,
    /// Second transmitter playback output.
    TransmitB,
}

/// Current persisted radio tuning independent of EEPROM word layout.
///
/// Mixer and signaling values retain their established zero-through-999
/// calibration scales. Raw EEPROM offsets remain private to this host.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EepromTuning {
    /// Receiver hardware-mixer level.
    receive_mixer_level: u16,
    /// Transmitter output-A hardware-mixer level.
    transmit_a_mixer_level: u16,
    /// Transmitter output-B hardware-mixer level.
    transmit_b_mixer_level: u16,
    /// Generated transmit CTCSS peak level.
    transmit_ctcss_level: u16,
    /// DSP carrier-squelch threshold.
    receive_squelch_level: u16,
}

impl EepromTuning {
    /// Construct one complete current tuning set.
    ///
    /// Returns `None` when any value is outside its zero-through-999 scale.
    #[must_use]
    pub fn new(
        receive_mixer_level: u16,
        transmit_a_mixer_level: u16,
        transmit_b_mixer_level: u16,
        transmit_ctcss_level: u16,
        receive_squelch_level: u16,
    ) -> Option<Self> {
        let tuning = Self {
            receive_mixer_level,
            transmit_a_mixer_level,
            transmit_b_mixer_level,
            transmit_ctcss_level,
            receive_squelch_level,
        };
        tuning.valid().then_some(tuning)
    }

    /// Return the receiver hardware-mixer level.
    #[must_use]
    pub const fn receive_mixer_level(self) -> u16 {
        self.receive_mixer_level
    }

    /// Return the transmitter output-A hardware-mixer level.
    #[must_use]
    pub const fn transmit_a_mixer_level(self) -> u16 {
        self.transmit_a_mixer_level
    }

    /// Return the transmitter output-B hardware-mixer level.
    #[must_use]
    pub const fn transmit_b_mixer_level(self) -> u16 {
        self.transmit_b_mixer_level
    }

    /// Return the generated transmit CTCSS level.
    #[must_use]
    pub const fn transmit_ctcss_level(self) -> u16 {
        self.transmit_ctcss_level
    }

    /// Return the DSP carrier-squelch threshold.
    #[must_use]
    pub const fn receive_squelch_level(self) -> u16 {
        self.receive_squelch_level
    }

    fn decode(image: &EepromImage) -> Option<Self> {
        if !image.magic_valid || !image.checksum_valid {
            return None;
        }
        Self::new(
            image.words[EEPROM_TUNING_START_WORD + EEPROM_RECEIVE_MIXER_OFFSET],
            image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_A_MIXER_OFFSET],
            image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_B_MIXER_OFFSET],
            image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_CTCSS_OFFSET],
            image.words[EEPROM_TUNING_START_WORD + EEPROM_RECEIVE_SQUELCH_OFFSET],
        )
    }

    fn valid(self) -> bool {
        [
            self.receive_mixer_level,
            self.transmit_a_mixer_level,
            self.transmit_b_mixer_level,
            self.transmit_ctcss_level,
            self.receive_squelch_level,
        ]
        .into_iter()
        .all(|value| value <= MAXIMUM_TUNING_LEVEL)
    }

    fn encode(self) -> EepromImage {
        let mut image = EepromImage::default();
        image.words[EEPROM_TUNING_START_WORD + EEPROM_RECEIVE_MIXER_OFFSET] =
            self.receive_mixer_level;
        image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_A_MIXER_OFFSET] =
            self.transmit_a_mixer_level;
        image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_B_MIXER_OFFSET] =
            self.transmit_b_mixer_level;
        image.words[EEPROM_TUNING_START_WORD + EEPROM_TRANSMIT_CTCSS_OFFSET] =
            self.transmit_ctcss_level;
        image.words[EEPROM_TUNING_START_WORD + EEPROM_RECEIVE_SQUELCH_OFFSET] =
            self.receive_squelch_level;
        image
    }

    /// Apply persistent tuning to mutable effective configuration before preparation.
    pub fn apply_to(self, configuration: &mut ChannelConfiguration) {
        let station = &mut configuration.station;
        station.hardware.input_gain_db = hardware_level_to_db(self.receive_mixer_level);
        station.hardware.output_a_gain_db = hardware_level_to_db(self.transmit_a_mixer_level);
        station.hardware.output_b_gain_db = hardware_level_to_db(self.transmit_b_mixer_level);
        station.ctcss.transmit_peak_dbfs = ctcss_level_to_dbfs(self.transmit_ctcss_level);
        station.receive.squelch_level = self.receive_squelch_level;
    }
}

/// Canonical device selection and optional startup EEPROM read completed before audio opens.
///
/// The temporary HID owner is closed before this value is returned. A caller
/// may therefore apply the exposed tuning image, rebuild immutable station
/// media, and pass this same selection to [`HardwareStation::open_preflighted`].
#[derive(Clone, Debug, PartialEq)]
pub struct HardwarePreflight {
    selected: SelectedDevice,
    startup_tuning: Option<EepromTuning>,
}

impl HardwarePreflight {
    fn new(selected: SelectedDevice, startup_eeprom: Option<EepromImage>) -> Self {
        let startup_tuning = startup_eeprom.as_ref().and_then(EepromTuning::decode);
        Self {
            selected,
            startup_tuning,
        }
    }

    /// Apply startup EEPROM tuning before immutable station preparation.
    ///
    /// Returns whether a complete valid current tuning image was available.
    pub fn apply_startup_tuning(&self, configuration: &mut ChannelConfiguration) -> bool {
        if let Some(tuning) = self.startup_tuning {
            tuning.apply_to(configuration);
            true
        } else {
            false
        }
    }
}

/// One selected CM119, its mixers, hardware service, and native station stream.
pub struct HardwareStation {
    selected: SelectedDevice,
    runtime: StationRuntime,
    control: StationControlHost,
    service: HardwareService,
    mixers: HardwareMixers,
}

impl HardwareStation {
    /// Select one canonical USB identity and read its optional tuning EEPROM.
    ///
    /// This pre-audio operation temporarily owns only the CM119 HID handle and
    /// closes it before returning. Invalid or unreadable EEPROM data is treated
    /// as absent, preserving the established best-effort startup behavior.
    ///
    /// # Errors
    ///
    /// Returns a device-selection, identity-binding, or CM119-open failure.
    pub fn preflight(
        plan: &HardwarePlan,
        eeprom_enabled: bool,
        audio: AudioProvider,
        gpio: GpioAdapter,
    ) -> Result<HardwarePreflight, HardwareStationError> {
        let selected = audio.select_device(&plan.audio_selector)?;
        let selected_plan = plan.select(&selected, 1)?;
        let startup_eeprom = if eeprom_enabled {
            let mut device = gpio.open_cm119(&selected_plan.cm119).map_err(|source| {
                HardwareStationError::Gpio {
                    operation: HardwareOperation::OpenCm119,
                    source,
                }
            })?;
            device
                .read_eeprom()
                .ok()
                .filter(|image| image.magic_valid && image.checksum_valid)
        } else {
            None
        };
        Ok(HardwarePreflight::new(selected, startup_eeprom))
    }

    /// Preflight a replacement while preserving prepare-before-stop reloads.
    ///
    /// When the replacement resolves to the live CM119, EEPROM is read through
    /// its existing serialized service owner instead of attempting a second
    /// exclusive HID open. A distinct physical device uses normal preflight.
    ///
    /// # Errors
    ///
    /// Returns a selection, identity, live-service, or EEPROM adapter failure.
    pub fn preflight_replacement(
        &self,
        plan: &HardwarePlan,
        eeprom_enabled: bool,
        audio: AudioProvider,
        gpio: GpioAdapter,
    ) -> Result<HardwarePreflight, HardwareStationError> {
        let selected = audio.select_device(&plan.audio_selector)?;
        plan.select(&selected, 1)?;
        if selected.interface_path != self.selected.interface_path || !self.service.running() {
            return Self::preflight(plan, eeprom_enabled, audio, gpio);
        }
        let startup_eeprom = if eeprom_enabled {
            self.service
                .read_eeprom_for_preflight()?
                .filter(|image| image.magic_valid && image.checksum_valid)
        } else {
            None
        };
        Ok(HardwarePreflight::new(selected, startup_eeprom))
    }

    /// Prepare a station from a previously completed canonical-device preflight.
    ///
    /// This is the startup path for callers which apply EEPROM tuning before
    /// constructing immutable [`StationMedia`]. No device is started here.
    ///
    /// # Errors
    ///
    /// Returns an identity, mixer, or stream preparation failure. Partially
    /// opened resources are released by RAII.
    pub fn open_preflighted(
        media: StationMedia,
        audio: AudioProvider,
        gpio: GpioAdapter,
        maximum_frame_count: u32,
        preflight: HardwarePreflight,
    ) -> Result<Self, HardwareStationError> {
        let plan = media.plan.hardware().clone();
        let hardware = media.plan.configuration().station.hardware.clone();
        let local_repeat_level = hardware_local_repeat_level(
            media.plan.transport(),
            media.plan.configuration().station.duplex.local_repeat_level,
        );
        let initial_remote_radio = configured_remote_radio(media.plan.configuration());
        let selected = preflight.selected;
        let selected_plan = plan.select(&selected, maximum_frame_count)?;
        let (mixers, sidetone) = HardwareMixers::open(
            audio,
            &selected,
            &hardware,
            preflight.startup_tuning,
            local_repeat_level,
        )?;
        let (runtime, control) = StationRuntime::open(media, &selected_plan, audio)?;
        let service = HardwareService::new(
            gpio,
            &plan,
            selected_plan,
            runtime.hardware_state(),
            HardwareServiceSetup {
                eeprom_enabled: hardware.eeprom_enabled,
                sidetone,
                initial_remote_radio,
            },
        );
        Ok(Self {
            selected,
            runtime,
            control,
            service,
            mixers,
        })
    }

    /// Start hardware servicing and then the prepared audio callbacks.
    ///
    /// Repeated calls are harmless.
    ///
    /// # Errors
    ///
    /// Returns a GPIO open/service failure or a PortAudio start failure.
    pub fn start(&mut self) -> Result<(), HardwareStationError> {
        if self.service.running() {
            return Ok(());
        }
        self.service.start()?;
        if let Err(error) = self.runtime.start() {
            let _ = self.service.stop();
            return Err(error.into());
        }
        Ok(())
    }

    /// Stop audio, fail-safe unkey hardware, and join the service thread.
    ///
    /// Repeated calls are harmless.
    ///
    /// # Errors
    ///
    /// Returns an audio stop failure or an unexpected service-thread panic.
    pub fn stop(&mut self) -> Result<(), HardwareStationError> {
        let audio = self.runtime.stop().map_err(HardwareStationError::Audio);
        let hardware = self.service.stop();
        audio.and(hardware)
    }

    /// Quiesce audio and hardware, then release the audio device for reload.
    ///
    /// The service is joined and GPIO fail-safe unkeyed before stream destruction.
    /// Media and callback contexts remain owned here for transaction rollback.
    ///
    /// # Errors
    ///
    /// Returns a stop failure without releasing a possibly active stream.
    pub fn suspend(&mut self) -> Result<(), HardwareStationError> {
        self.stop()?;
        self.runtime.suspend().map_err(HardwareStationError::Audio)
    }

    /// Reacquire a suspended audio device without starting callbacks or GPIO.
    ///
    /// # Errors
    ///
    /// Returns an audio open failure, leaving the station suspended and RF-safe.
    pub fn reopen(&mut self) -> Result<(), HardwareStationError> {
        self.runtime.reopen().map_err(HardwareStationError::Audio)
    }

    /// Borrow serialized controller and radio-observer ownership.
    pub fn control(&mut self) -> &mut StationControlHost {
        &mut self.control
    }

    /// Clone the lock-free state shared with the native callbacks.
    #[must_use]
    pub fn hardware_state(&self) -> SharedHardwareState {
        self.runtime.hardware_state()
    }

    /// Read the last applied value on the established zero-through-999 scale.
    ///
    /// # Errors
    ///
    /// Retains a result type shared with mixer mutation for a uniform tuner API.
    pub fn mixer_level(&self, mixer: HardwareMixer) -> Result<u32, HardwareStationError> {
        Ok(self.mixers.level(mixer))
    }

    /// Set one hardware mixer on its normalized zero-through-999 scale.
    ///
    /// All paths representing the selected logical mixer are changed together.
    ///
    /// # Errors
    ///
    /// Returns an audio-adapter error when the value or mixer operation is invalid.
    pub fn set_mixer_level(
        &mut self,
        mixer: HardwareMixer,
        level: u32,
    ) -> Result<(), HardwareStationError> {
        self.mixers.set_level(mixer, level)?;
        Ok(())
    }

    /// Read the latest complete native receive measurement without locking.
    #[must_use]
    pub fn receive_observation(&self) -> ReceiveObservation {
        self.hardware_state().receive_observation()
    }

    /// Publish controller PTT and audio-admission intent atomically.
    ///
    /// DAC admission remains independent of PTT so an unkeyed callback can
    /// finish transmitter signaling and release the hardware.
    pub fn set_transmit_request(
        &self,
        transmit: bool,
        render_admitted: bool,
        forced_ctcss: Option<CtcssTone>,
    ) {
        self.update_requests(|requests| {
            requests.transmit = transmit;
            requests.render_admitted = render_admitted;
            requests.forced_ctcss = forced_ctcss;
        });
    }

    /// Enable or disable the calibrated transmitter test tone.
    pub fn set_calibrated_test_tone(&self, enabled: bool) {
        self.update_requests(|requests| requests.calibrated_test_tone = enabled);
    }

    /// Temporarily inhibit or restore configured transmit CTCSS generation.
    pub fn set_ctcss_inhibited(&self, inhibited: bool) {
        self.update_requests(|requests| requests.ctcss_inhibit = inhibited);
    }

    /// Temporarily bypass or restore receive subaudible qualification.
    pub fn set_subaudible_override(&self, enabled: bool) {
        self.update_requests(|requests| requests.subaudible_override = enabled);
    }

    /// Apply one translated controller request through its single hardware boundary.
    ///
    /// Controller state and lock-free native callback controls are updated
    /// before any bounded GPIO or parallel-port work is queued.
    ///
    /// # Errors
    ///
    /// Returns an error when a requested GPIO or parallel operation is not
    /// configured, the hardware service is stopped, or its bounded queue is full.
    pub fn apply_control(
        &mut self,
        message: ControlMessage,
    ) -> Result<ControlAction, HardwareStationError> {
        let action = self.control.controller().apply_control(message);
        match action {
            ControlAction::SetTransmit {
                keyed,
                forced_ctcss,
            } => {
                // A live DAC callback remains admitted while the radio core
                // finishes its tone-off sequence and releases logical PTT.
                self.set_transmit_request(keyed, true, forced_ctcss);
            }
            ControlAction::SelectChannel(channel) => self.select_channel(channel)?,
            ControlAction::SetReceiveCtcss(enabled) => self.set_subaudible_override(!enabled),
            ControlAction::SetTransmitCtcss(enabled) => {
                self.set_ctcss_inhibited(!enabled);
            }
            ControlAction::SetGpio(pin, request) => self.set_gpio(pin, request)?,
            ControlAction::SetParallel(pin, request) => self.set_parallel(pin, request)?,
            ControlAction::ConfigureRemoteRadio(settings) => {
                self.configure_remote_radio(settings)?;
            }
        }
        Ok(action)
    }

    fn update_requests(&self, update: impl FnOnce(&mut usbradioplus_station::ControllerRequests)) {
        let state = self.hardware_state();
        let mut requests = state.requests();
        update(&mut requests);
        state.publish_requests(requests);
    }

    /// Queue one persistent or timed CM119 GPIO request.
    ///
    /// # Errors
    ///
    /// Returns an error if the pin is not a configured ordinary output, the
    /// pulse duration overflows, or the bounded service queue cannot accept it.
    pub fn set_gpio(
        &self,
        pin: GpioPin,
        request: OutputRequest,
    ) -> Result<(), HardwareStationError> {
        let bit = 1_u8 << (pin.get() - 1);
        self.service.queue(OutputCommand::Cm119(bit, request))
    }

    /// Queue one persistent or timed parallel-port output request.
    ///
    /// # Errors
    ///
    /// Returns an error if the pin is unavailable or reserved for PTT, the
    /// pulse duration overflows, or the bounded service queue cannot accept it.
    pub fn set_parallel(
        &self,
        pin: ParallelPin,
        request: OutputRequest,
    ) -> Result<(), HardwareStationError> {
        let bit = 1_u8 << (pin.get() - 2);
        self.service.queue(OutputCommand::Parallel(bit, request))
    }

    /// Queue one established active-low parallel channel selection.
    ///
    /// # Errors
    ///
    /// Returns an error when no parallel transport is configured or the
    /// bounded service queue cannot accept the request.
    pub fn select_channel(&self, channel: u8) -> Result<(), HardwareStationError> {
        self.service
            .queue_service(ServiceCommand::SelectChannel(channel))
    }

    /// Queue one complete parallel-port remote-radio request.
    ///
    /// The service owner reapplies this request whenever logical PTT changes.
    ///
    /// # Errors
    ///
    /// Returns an error when no parallel transport is configured or the
    /// bounded service queue cannot accept the request.
    pub fn configure_remote_radio(
        &self,
        settings: RemoteRadio,
    ) -> Result<(), HardwareStationError> {
        self.service
            .queue_service(ServiceCommand::ConfigureRemoteRadio(settings))
    }

    /// Read tuning EEPROM through the serialized hardware service owner.
    ///
    /// # Errors
    ///
    /// Returns an error when EEPROM is disabled, hardware is stopped, the
    /// request times out, or the GPIO adapter rejects the transfer.
    pub fn read_eeprom(&self) -> Result<EepromImage, HardwareStationError> {
        self.service.read_eeprom()
    }

    /// Write tuning EEPROM through the serialized hardware service owner.
    ///
    /// The adapter returns any checksum or metadata changes in `image`.
    ///
    /// # Errors
    ///
    /// Returns an error when EEPROM is disabled, hardware is stopped, the
    /// request times out, or the GPIO adapter rejects the transfer.
    pub fn write_eeprom(&self, image: &mut EepromImage) -> Result<(), HardwareStationError> {
        self.service.write_eeprom(image)
    }

    /// Encode and persist current live radio tuning through the hardware owner.
    ///
    /// The tuner need not know EEPROM offsets or checksum policy. Mixer values
    /// are read live; CTCSS and squelch values come from the active immutable
    /// station generation.
    ///
    /// # Errors
    ///
    /// Returns a mixer, service-state, timeout, or EEPROM adapter failure.
    pub fn save_current_tuning_to_eeprom(&self) -> Result<EepromTuning, HardwareStationError> {
        let station = &self.control.plan().configuration().station;
        let tuning = EepromTuning {
            receive_mixer_level: self.mixers.level(HardwareMixer::Receive) as u16,
            transmit_a_mixer_level: self.mixers.level(HardwareMixer::TransmitA) as u16,
            transmit_b_mixer_level: self.mixers.level(HardwareMixer::TransmitB) as u16,
            transmit_ctcss_level: ctcss_dbfs_to_level(station.ctcss.transmit_peak_dbfs),
            receive_squelch_level: station.receive.squelch_level,
        };
        let mut image = tuning.encode();
        self.write_eeprom(&mut image)?;
        Ok(tuning)
    }

    /// Take the next ordinary CM119 or parallel input edge without waiting.
    #[must_use]
    pub fn take_input_event(&self) -> Option<HardwareInputEvent> {
        self.service.take_input_event()
    }

    /// Capture persistent GPIO, parallel-channel, and remote-radio state.
    ///
    /// # Errors
    ///
    /// Returns an error when the service is stopped, busy, or unavailable.
    pub fn transient_state(&self) -> Result<HardwareTransientState, HardwareStationError> {
        self.service.transient_state()
    }

    /// Apply persistent state captured from the station being replaced.
    ///
    /// This may be called before [`Self::start`], avoiding an initial-output
    /// glitch during a prepared hot reload. Timed pulses intentionally restart
    /// inactive.
    ///
    /// # Errors
    ///
    /// Returns an error when a running service cannot accept the request.
    pub fn restore_transient_state(
        &mut self,
        state: HardwareTransientState,
    ) -> Result<(), HardwareStationError> {
        self.service.restore_transient_state(state)
    }

    /// Read runtime, stream-timing, and hardware-service diagnostics.
    ///
    /// # Errors
    ///
    /// Returns an audio adapter failure if its statistics cannot be read.
    pub fn diagnostics(&self) -> Result<HardwareStationDiagnostics, HardwareStationError> {
        Ok(HardwareStationDiagnostics {
            runtime: self.runtime.statistics()?,
            timing: self.runtime.timing()?,
            hardware: self.service.statistics(),
            receive: self.receive_observation(),
        })
    }
}

impl Drop for HardwareStation {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

struct HardwareMixers {
    receive: Vec<Mixer>,
    transmit: Vec<Mixer>,
    levels: [u32; 3],
    _receive_compatibility: Vec<Mixer>,
}

impl HardwareMixers {
    fn open(
        provider: AudioProvider,
        selected: &SelectedDevice,
        config: &usbradioplus_core::HardwareConfig,
        startup_tuning: Option<EepromTuning>,
        local_repeat_level: u32,
    ) -> Result<(Self, Option<HardwareSidetone>), AudioError> {
        let paths = provider.cm119_mixer_paths(&selected.interface_path)?;
        if paths.transmit.len() != 2 {
            return Err(AudioError::Unsupported);
        }
        let mut receive = open_paths(provider, &selected.interface_path, &paths.receive)?;
        let mut transmit = open_paths(provider, &selected.interface_path, &paths.transmit)?;
        let sidetone = open_paths(provider, &selected.interface_path, &paths.sidetone)?;
        let mut receive_compatibility = open_paths(
            provider,
            &selected.interface_path,
            &paths.receive_compatibility_switch,
        )?;

        let levels = startup_tuning.map_or_else(
            || {
                [
                    mixer_level(config.input_gain_db),
                    mixer_level(config.output_a_gain_db),
                    mixer_level(config.output_b_gain_db),
                ]
            },
            |tuning| {
                [
                    u32::from(tuning.receive_mixer_level),
                    u32::from(tuning.transmit_a_mixer_level),
                    u32::from(tuning.transmit_b_mixer_level),
                ]
            },
        );
        apply_gain(&mut receive, levels[0])?;
        apply_gain(&mut transmit[..1], levels[1])?;
        apply_gain(&mut transmit[1..], levels[2])?;
        set_enabled_where_supported(&paths.receive, &mut receive, true)?;
        set_enabled_where_supported(&paths.transmit, &mut transmit, true)?;
        let sidetone = HardwareSidetone::new(paths.sidetone, sidetone, local_repeat_level)?;
        for mixer in &mut receive_compatibility {
            mixer.set_enabled(true)?;
        }
        Ok((
            Self {
                receive,
                transmit,
                levels,
                _receive_compatibility: receive_compatibility,
            },
            sidetone,
        ))
    }

    fn level(&self, mixer: HardwareMixer) -> u32 {
        self.levels[mixer as usize]
    }

    fn set_level(&mut self, mixer: HardwareMixer, level: u32) -> Result<(), AudioError> {
        for path in self.paths_mut(mixer) {
            path.set_hardware_level(level)?;
        }
        self.levels[mixer as usize] = level;
        Ok(())
    }

    fn paths_mut(&mut self, mixer: HardwareMixer) -> &mut [Mixer] {
        match mixer {
            HardwareMixer::Receive => &mut self.receive,
            HardwareMixer::TransmitA => &mut self.transmit[..1],
            HardwareMixer::TransmitB => &mut self.transmit[1..],
        }
    }
}

fn open_paths(
    provider: AudioProvider,
    interface: &str,
    paths: &[MixerPath],
) -> Result<Vec<Mixer>, AudioError> {
    paths
        .iter()
        .map(|path| provider.open_mixer(interface, path))
        .collect()
}

fn apply_gain(mixers: &mut [Mixer], level: u32) -> Result<(), AudioError> {
    for mixer in mixers {
        mixer.set_hardware_level(level)?;
    }
    Ok(())
}

fn set_enabled_where_supported(
    paths: &[MixerPath],
    mixers: &mut [Mixer],
    enabled: bool,
) -> Result<(), AudioError> {
    for (path, mixer) in paths.iter().zip(mixers) {
        if path.has_switch {
            mixer.set_enabled(enabled)?;
        }
    }
    Ok(())
}

struct HardwareSidetone {
    paths: Vec<MixerPath>,
    mixers: Vec<Mixer>,
    level: u32,
    applied: Option<bool>,
}

impl HardwareSidetone {
    fn new(
        paths: Vec<MixerPath>,
        mixers: Vec<Mixer>,
        level: u32,
    ) -> Result<Option<Self>, AudioError> {
        if level > 999 {
            return Err(AudioError::InvalidArgument);
        }
        if paths.is_empty() {
            return if level == 0 {
                Ok(None)
            } else {
                Err(AudioError::Unsupported)
            };
        }
        if level != 0
            && paths
                .iter()
                .any(|path| !path.has_switch || !path.has_volume)
        {
            return Err(AudioError::Unsupported);
        }
        let mut result = Self {
            paths,
            mixers,
            level,
            applied: None,
        };
        result.apply(false)?;
        Ok(Some(result))
    }

    fn apply(&mut self, receiver_keyed: bool) -> Result<(), AudioError> {
        let enabled = self.level != 0 && receiver_keyed;
        if self.applied == Some(enabled) {
            return Ok(());
        }
        self.applied = None;
        if !enabled {
            set_enabled_where_supported(&self.paths, &mut self.mixers, false)?;
        }
        for mixer in &mut self.mixers {
            mixer.set_normalized(if enabled { self.level } else { 0 })?;
        }
        if enabled {
            set_enabled_where_supported(&self.paths, &mut self.mixers, true)?;
        }
        self.applied = Some(enabled);
        Ok(())
    }
}

impl Drop for HardwareSidetone {
    fn drop(&mut self) {
        let _ = self.apply(false);
    }
}

const fn hardware_local_repeat_level(transport: ControllerTransport, configured: u16) -> u32 {
    match transport {
        ControllerTransport::AppRpt => configured as u32,
        ControllerTransport::RptAdvanced => 0,
    }
}

fn configured_remote_radio(configuration: &ChannelConfiguration) -> Option<RemoteRadio> {
    (configuration.station.receive.frequency_hz != 0).then_some(RemoteRadio {
        receive_hz: configuration.station.receive.frequency_hz,
        transmit_hz: configuration.station.transmit.frequency_hz,
        receive_ctcss: None,
        transmit_ctcss: None,
        high_power: false,
    })
}

fn mixer_level(gain_db: f64) -> u32 {
    (500.0 * 10.0_f64.powf(gain_db / 20.0))
        .round()
        .clamp(0.0, 999.0) as u32
}

fn hardware_level_to_db(level: u16) -> f64 {
    if level == 0 {
        -30.0
    } else {
        (20.0 * (f64::from(level) / 500.0).log10()).clamp(-30.0, 30.0)
    }
}

fn ctcss_level_to_dbfs(level: u16) -> f64 {
    if level == 0 {
        -90.0
    } else {
        (20.0 * (f64::from(level) / 999.0).log10()).clamp(-90.0, 0.0)
    }
}

fn ctcss_dbfs_to_level(level_dbfs: f64) -> u16 {
    (999.0 * 10.0_f64.powf(level_dbfs / 20.0))
        .round()
        .clamp(0.0, 999.0) as u16
}

#[derive(Clone, Copy)]
enum OutputCommand {
    Cm119(u8, OutputRequest),
    Parallel(u8, OutputRequest),
}

enum ServiceCommand {
    Output(OutputCommand),
    ReadEeprom(SyncSender<Result<EepromImage, GpioError>>),
    WriteEeprom(EepromImage, SyncSender<Result<EepromImage, GpioError>>),
    SelectChannel(u8),
    ConfigureRemoteRadio(RemoteRadio),
    Snapshot(SyncSender<HardwareTransientState>),
    Restore(HardwareTransientState),
}

struct HardwareService {
    provider: GpioAdapter,
    selected: SelectedHardwarePlan,
    parallel: Option<ParallelPlan>,
    state: SharedHardwareState,
    cm119_input_mask: u8,
    clip_led_mask: u8,
    eeprom_enabled: bool,
    sidetone: Option<HardwareSidetone>,
    initial_remote_radio: Option<RemoteRadio>,
    pending_transient: Option<HardwareTransientState>,
    statistics: Arc<ServiceStatistics>,
    stop: Arc<AtomicBool>,
    sender: Option<SyncSender<ServiceCommand>>,
    input_events: Option<Receiver<HardwareInputEvent>>,
    worker: Option<JoinHandle<Option<HardwareSidetone>>>,
}

struct HardwareServiceSetup {
    eeprom_enabled: bool,
    sidetone: Option<HardwareSidetone>,
    initial_remote_radio: Option<RemoteRadio>,
}

impl HardwareService {
    fn new(
        provider: GpioAdapter,
        plan: &HardwarePlan,
        selected: SelectedHardwarePlan,
        state: SharedHardwareState,
        setup: HardwareServiceSetup,
    ) -> Self {
        Self {
            provider,
            parallel: plan.parallel.clone(),
            statistics: Arc::new(ServiceStatistics::new(plan.parallel.is_some())),
            cm119_input_mask: plan.cm119.input_mask,
            clip_led_mask: plan.cm119.clip_led_mask,
            selected,
            state,
            eeprom_enabled: setup.eeprom_enabled,
            sidetone: setup.sidetone,
            initial_remote_radio: setup.initial_remote_radio,
            pending_transient: None,
            stop: Arc::new(AtomicBool::new(false)),
            sender: None,
            input_events: None,
            worker: None,
        }
    }

    fn running(&self) -> bool {
        self.worker.is_some()
    }

    fn start(&mut self) -> Result<(), HardwareStationError> {
        if self.running() {
            return Ok(());
        }
        let mut devices =
            ServiceDevices::open(self.provider, &self.selected, self.parallel.as_ref())
                .map_err(HardwareStationError::from)?;
        let mut outputs = OutputState::new(
            &self.selected,
            self.parallel.as_ref(),
            self.initial_remote_radio,
        );
        if let Some(state) = self.pending_transient.take() {
            outputs.restore(state, &self.selected, self.parallel.as_ref());
        }
        let mut inputs = InputTracker::default();
        let (input_sender, input_receiver) = sync_channel(INPUT_EVENT_QUEUE_CAPACITY);
        let (sender, receiver) = sync_channel(CONTROL_QUEUE_CAPACITY);
        self.stop.store(false, Ordering::Release);
        let worker = ServiceWorker {
            provider: self.provider,
            selected: self.selected.clone(),
            parallel: self.parallel.clone(),
            state: self.state.clone(),
            statistics: Arc::clone(&self.statistics),
            stop: Arc::clone(&self.stop),
            receiver,
            cm119_input_mask: self.cm119_input_mask,
            clip_led_mask: self.clip_led_mask,
            input_sender,
        };
        service_once(
            &mut devices,
            &mut outputs,
            &mut inputs,
            self.sidetone.as_mut(),
            &worker,
        )
        .map_err(HardwareStationError::from)?;
        let sidetone = self.sidetone.take();
        self.worker = Some(thread::spawn(move || {
            worker.run(devices, outputs, inputs, sidetone)
        }));
        self.sender = Some(sender);
        self.input_events = Some(input_receiver);
        Ok(())
    }

    fn stop(&mut self) -> Result<(), HardwareStationError> {
        let Some(worker) = self.worker.take() else {
            return Ok(());
        };
        self.stop.store(true, Ordering::Release);
        worker.thread().unpark();
        self.sender = None;
        self.input_events = None;
        self.sidetone = worker
            .join()
            .map_err(|_| HardwareStationError::WorkerPanicked)?;
        Ok(())
    }

    fn queue(&self, command: OutputCommand) -> Result<(), HardwareStationError> {
        self.validate_command(command)?;
        self.queue_service(ServiceCommand::Output(command))
    }

    fn queue_service(&self, command: ServiceCommand) -> Result<(), HardwareStationError> {
        if matches!(
            &command,
            ServiceCommand::SelectChannel(_) | ServiceCommand::ConfigureRemoteRadio(_)
        ) && self.parallel.is_none()
        {
            return Err(HardwareStationError::UnavailableOutput);
        }
        let sender = self
            .sender
            .as_ref()
            .ok_or(HardwareStationError::NotRunning)?;
        sender.try_send(command).map_err(|error| match error {
            TrySendError::Full(_) => HardwareStationError::ControlQueueFull,
            TrySendError::Disconnected(_) => HardwareStationError::NotRunning,
        })?;
        self.worker
            .as_ref()
            .expect("a running service owns its worker")
            .thread()
            .unpark();
        Ok(())
    }

    fn read_eeprom(&self) -> Result<EepromImage, HardwareStationError> {
        if !self.eeprom_enabled {
            return Err(HardwareStationError::EepromDisabled);
        }
        self.read_eeprom_required()
    }

    fn read_eeprom_for_preflight(&self) -> Result<Option<EepromImage>, HardwareStationError> {
        self.read_eeprom_required().map(Some).or_else(|error| {
            if matches!(error, HardwareStationError::Gpio { .. }) {
                Ok(None)
            } else {
                Err(error)
            }
        })
    }

    fn read_eeprom_required(&self) -> Result<EepromImage, HardwareStationError> {
        let (sender, receiver) = sync_channel(1);
        self.queue_service(ServiceCommand::ReadEeprom(sender))?;
        receive_response(receiver, HardwareOperation::ReadEeprom)
    }

    fn write_eeprom(&self, image: &mut EepromImage) -> Result<(), HardwareStationError> {
        if !self.eeprom_enabled {
            return Err(HardwareStationError::EepromDisabled);
        }
        let (sender, receiver) = sync_channel(1);
        self.queue_service(ServiceCommand::WriteEeprom(image.clone(), sender))?;
        *image = receive_response(receiver, HardwareOperation::WriteEeprom)?;
        Ok(())
    }

    fn take_input_event(&self) -> Option<HardwareInputEvent> {
        self.input_events
            .as_ref()
            .and_then(|receiver| receiver.try_recv().ok())
    }

    fn transient_state(&self) -> Result<HardwareTransientState, HardwareStationError> {
        let (sender, receiver) = sync_channel(1);
        self.queue_service(ServiceCommand::Snapshot(sender))?;
        receive_value(receiver)
    }

    fn restore_transient_state(
        &mut self,
        state: HardwareTransientState,
    ) -> Result<(), HardwareStationError> {
        if self.running() {
            self.queue_service(ServiceCommand::Restore(state))
        } else {
            self.pending_transient = Some(state);
            Ok(())
        }
    }

    fn validate_command(&self, command: OutputCommand) -> Result<(), HardwareStationError> {
        let (available, request) = match command {
            OutputCommand::Cm119(bit, request) => {
                (self.selected.cm119.output_enable_mask & bit != 0, request)
            }
            OutputCommand::Parallel(bit, request) => (
                self.parallel
                    .as_ref()
                    .is_some_and(|plan| plan.config.output_enable_mask & !plan.ptt_mask & bit != 0),
                request,
            ),
        };
        if !available {
            return Err(HardwareStationError::UnavailableOutput);
        }
        if let OutputRequest::Pulse(ticks) = request {
            ticks
                .checked_mul(CONTROL_TICK_MILLISECONDS)
                .ok_or(HardwareStationError::PulseTooLong)?;
        }
        Ok(())
    }

    fn statistics(&self) -> HardwareServiceStatistics {
        self.statistics.snapshot()
    }
}

impl Drop for HardwareService {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

fn receive_response<T>(
    receiver: Receiver<Result<T, GpioError>>,
    operation: HardwareOperation,
) -> Result<T, HardwareStationError> {
    match receive_value(receiver) {
        Ok(Ok(value)) => Ok(value),
        Ok(Err(source)) => Err(HardwareStationError::Gpio { operation, source }),
        Err(error) => Err(error),
    }
}

fn receive_value<T>(receiver: Receiver<T>) -> Result<T, HardwareStationError> {
    receiver
        .recv_timeout(CONTROL_RESPONSE_TIMEOUT)
        .map_err(|error| match error {
            RecvTimeoutError::Timeout => HardwareStationError::ControlTimeout,
            RecvTimeoutError::Disconnected => HardwareStationError::NotRunning,
        })
}

struct ServiceDevices {
    cm119: Cm119Device,
    parallel: Option<ParallelDevice>,
    ptt_inverted: bool,
}

impl ServiceDevices {
    fn open(
        provider: GpioAdapter,
        selected: &SelectedHardwarePlan,
        parallel: Option<&ParallelPlan>,
    ) -> Result<Self, ServiceFailure> {
        let cm119 = provider
            .open_cm119(&selected.cm119)
            .map_err(|source| ServiceFailure(HardwareOperation::OpenCm119, source))?;
        let parallel = parallel
            .map(|plan| {
                provider
                    .open_parallel(&plan.config)
                    .map_err(|source| ServiceFailure(HardwareOperation::OpenParallel, source))
            })
            .transpose()?;
        Ok(Self {
            cm119,
            parallel,
            ptt_inverted: selected.cm119.ptt_inverted,
        })
    }

    fn fail_safe_unkey(&mut self, parallel: Option<&ParallelPlan>, outputs: &OutputState) {
        let _ = self.cm119.publish_outputs(Cm119Outputs {
            ptt_asserted: false,
            gpio_output_mask: outputs.cm119,
        });
        let _ = self.cm119.service();
        if let Some(plan) = parallel {
            let device = self
                .parallel
                .as_mut()
                .expect("a configured parallel plan owns a device");
            if outputs.remote_radio.is_some() {
                let _ = device.clear_rtx_transmit();
            }
            let _ = device.publish_outputs(ParallelOutputs {
                output_mask: parallel_output(
                    plan,
                    outputs.parallel & !(PARALLEL_RTX_TRANSMIT_MASK | PARALLEL_RTX_POWER_MASK),
                    false,
                    self.ptt_inverted,
                ),
                ..ParallelOutputs::default()
            });
            let _ = device.service();
        }
    }
}

struct OutputState {
    cm119: u8,
    parallel: u8,
    remote_radio: Option<RemoteRadio>,
    remote_radio_dirty: bool,
    remote_radio_ptt: Option<bool>,
    clip_events_seen: u64,
    clip_led_until: Option<Instant>,
}

impl OutputState {
    fn new(
        selected: &SelectedHardwarePlan,
        parallel: Option<&ParallelPlan>,
        remote_radio: Option<RemoteRadio>,
    ) -> Self {
        Self {
            cm119: selected.cm119.output_initial_mask,
            parallel: parallel.map_or(0, |plan| plan.config.output_initial_mask),
            remote_radio,
            remote_radio_dirty: remote_radio.is_some(),
            remote_radio_ptt: None,
            clip_events_seen: 0,
            clip_led_until: None,
        }
    }

    fn apply(
        &mut self,
        command: OutputCommand,
        devices: &ServiceDevices,
    ) -> Result<(), ServiceFailure> {
        match command {
            OutputCommand::Cm119(bit, request) => {
                let pulse = match request {
                    OutputRequest::Pulse(ticks) => Cm119Pulse {
                        invert_gpio_mask: bit,
                        duration_milliseconds: ticks * CONTROL_TICK_MILLISECONDS,
                        ..Cm119Pulse::default()
                    },
                    OutputRequest::Inactive | OutputRequest::Active => Cm119Pulse {
                        cancel_gpio_mask: bit,
                        ..Cm119Pulse::default()
                    },
                };
                devices
                    .cm119
                    .schedule_pulse(pulse)
                    .map_err(|source| ServiceFailure(HardwareOperation::PulseCm119, source))?;
                apply_persistent(&mut self.cm119, bit, request);
            }
            OutputCommand::Parallel(bit, request) => {
                let device = devices
                    .parallel
                    .as_ref()
                    .expect("a validated parallel output owns a device");
                let pulse = match request {
                    OutputRequest::Pulse(ticks) => ParallelPulse {
                        invert_mask: bit,
                        duration_milliseconds: ticks * CONTROL_TICK_MILLISECONDS,
                        cancel_mask: 0,
                    },
                    OutputRequest::Inactive | OutputRequest::Active => ParallelPulse {
                        invert_mask: 0,
                        duration_milliseconds: 0,
                        cancel_mask: bit,
                    },
                };
                device
                    .schedule_pulse(pulse)
                    .map_err(|source| ServiceFailure(HardwareOperation::PulseParallel, source))?;
                apply_persistent(&mut self.parallel, bit, request);
            }
        }
        Ok(())
    }

    const fn snapshot(&self) -> HardwareTransientState {
        HardwareTransientState {
            cm119_output_mask: self.cm119,
            parallel_output_mask: self.parallel,
            remote_radio: self.remote_radio,
        }
    }

    fn restore(
        &mut self,
        state: HardwareTransientState,
        selected: &SelectedHardwarePlan,
        parallel: Option<&ParallelPlan>,
    ) {
        let cm119_mask = selected.cm119.output_enable_mask;
        self.cm119 = (self.cm119 & !cm119_mask) | (state.cm119_output_mask & cm119_mask);
        if let Some(plan) = parallel {
            self.parallel = state.parallel_output_mask & !plan.ptt_mask;
            self.remote_radio = state.remote_radio;
            self.remote_radio_dirty = state.remote_radio.is_some();
            self.remote_radio_ptt = None;
        }
    }

    fn select_channel(
        &mut self,
        channel: u8,
        devices: &mut ServiceDevices,
    ) -> Result<(), ServiceFailure> {
        let device = devices.parallel.as_mut().ok_or(ServiceFailure(
            HardwareOperation::SelectChannel,
            GpioError::Unsupported,
        ))?;
        device
            .set_binary_channel(channel)
            .map_err(|source| ServiceFailure(HardwareOperation::SelectChannel, source))?;
        self.parallel = (self.parallel | PARALLEL_BINARY_MASK) & !(channel << 4);
        Ok(())
    }

    fn configure_remote_radio(&mut self, settings: RemoteRadio) {
        self.remote_radio = Some(settings);
        self.remote_radio_dirty = true;
    }

    fn service_remote_radio(
        &mut self,
        devices: &mut ServiceDevices,
        logical_ptt: bool,
    ) -> Result<(), ServiceFailure> {
        let Some(settings) = self.remote_radio else {
            return Ok(());
        };
        if !self.remote_radio_dirty && self.remote_radio_ptt == Some(logical_ptt) {
            return Ok(());
        }
        let device = devices.parallel.as_mut().ok_or(ServiceFailure(
            HardwareOperation::ProgramRemoteRadio,
            GpioError::Unsupported,
        ))?;
        if settings.receive_hz == 0 {
            if self.remote_radio_ptt == Some(true) && !logical_ptt {
                device.clear_rtx_transmit().map_err(|source| {
                    ServiceFailure(HardwareOperation::ClearRemoteRadioTransmit, source)
                })?;
                self.parallel &= !(PARALLEL_RTX_TRANSMIT_MASK | PARALLEL_RTX_POWER_MASK);
            }
        } else {
            device
                .program_rtx(ParallelRtx {
                    receive_hz: settings.receive_hz,
                    transmit_hz: settings.transmit_hz,
                    transmitting: logical_ptt,
                    high_power: settings.high_power,
                })
                .map_err(|source| ServiceFailure(HardwareOperation::ProgramRemoteRadio, source))?;
            self.parallel &= !PARALLEL_RTX_CONTROL_MASK;
            if logical_ptt {
                self.parallel |= PARALLEL_RTX_TRANSMIT_MASK;
            }
        }
        self.remote_radio_dirty = false;
        self.remote_radio_ptt = Some(logical_ptt);
        Ok(())
    }

    fn service_clip_led(
        &mut self,
        devices: &ServiceDevices,
        clip_led_mask: u8,
        clip_events: u64,
    ) -> Result<(), ServiceFailure> {
        if clip_events == self.clip_events_seen {
            return Ok(());
        }
        self.clip_events_seen = clip_events;
        if clip_led_mask == 0
            || self
                .clip_led_until
                .is_some_and(|deadline| Instant::now() < deadline)
        {
            return Ok(());
        }
        devices
            .cm119
            .schedule_pulse(Cm119Pulse {
                invert_gpio_mask: clip_led_mask,
                duration_milliseconds: CLIP_LED_HOLD.as_millis() as u32,
                ..Cm119Pulse::default()
            })
            .map_err(|source| ServiceFailure(HardwareOperation::PulseCm119, source))?;
        self.clip_led_until = Some(Instant::now() + CLIP_LED_HOLD);
        Ok(())
    }
}

fn apply_persistent(value: &mut u8, bit: u8, request: OutputRequest) {
    match request {
        OutputRequest::Inactive => *value &= !bit,
        OutputRequest::Active => *value |= bit,
        OutputRequest::Pulse(_) => {}
    }
}

struct ServiceWorker {
    provider: GpioAdapter,
    selected: SelectedHardwarePlan,
    parallel: Option<ParallelPlan>,
    state: SharedHardwareState,
    statistics: Arc<ServiceStatistics>,
    stop: Arc<AtomicBool>,
    receiver: Receiver<ServiceCommand>,
    cm119_input_mask: u8,
    clip_led_mask: u8,
    input_sender: SyncSender<HardwareInputEvent>,
}

impl ServiceWorker {
    fn run(
        self,
        initial_devices: ServiceDevices,
        mut outputs: OutputState,
        mut inputs: InputTracker,
        mut sidetone: Option<HardwareSidetone>,
    ) -> Option<HardwareSidetone> {
        let mut devices = Some(initial_devices);
        while !self.stop.load(Ordering::Acquire) {
            if devices.is_none() {
                match ServiceDevices::open(self.provider, &self.selected, self.parallel.as_ref()) {
                    Ok(opened) => devices = Some(opened),
                    Err(failure) => {
                        self.statistics.failure(failure);
                        self.state.publish_inputs(HardwareInputs::default());
                        thread::park_timeout(RETRY_INTERVAL);
                        continue;
                    }
                }
            }
            let current = devices.as_mut().expect("devices were opened above");
            match service_commands(
                &self.receiver,
                &mut outputs,
                current,
                &self.selected,
                self.parallel.as_ref(),
            )
            .and_then(|()| {
                service_once(current, &mut outputs, &mut inputs, sidetone.as_mut(), &self)
            }) {
                Ok(()) => thread::park_timeout(SERVICE_INTERVAL),
                Err(failure) => {
                    self.statistics.failure(failure);
                    self.state.publish_inputs(HardwareInputs::default());
                    devices = None;
                    thread::park_timeout(RETRY_INTERVAL);
                }
            }
        }
        if let Some(mut current) = devices {
            current.fail_safe_unkey(self.parallel.as_ref(), &outputs);
        }
        if let Some(control) = &mut sidetone {
            let _ = control.apply(false);
        }
        self.state.publish_inputs(HardwareInputs::default());
        sidetone
    }
}

fn service_commands(
    receiver: &Receiver<ServiceCommand>,
    outputs: &mut OutputState,
    devices: &mut ServiceDevices,
    selected: &SelectedHardwarePlan,
    parallel: Option<&ParallelPlan>,
) -> Result<(), ServiceFailure> {
    loop {
        match receiver.try_recv() {
            Ok(ServiceCommand::Output(command)) => outputs.apply(command, devices)?,
            Ok(ServiceCommand::ReadEeprom(response)) => {
                let _ = response.send(devices.cm119.read_eeprom());
            }
            Ok(ServiceCommand::WriteEeprom(mut image, response)) => {
                let result = devices.cm119.write_eeprom(&mut image).map(|()| image);
                let _ = response.send(result);
            }
            Ok(ServiceCommand::SelectChannel(channel)) => {
                outputs.select_channel(channel, devices)?;
            }
            Ok(ServiceCommand::ConfigureRemoteRadio(settings)) => {
                outputs.configure_remote_radio(settings);
            }
            Ok(ServiceCommand::Snapshot(response)) => {
                let _ = response.send(outputs.snapshot());
            }
            Ok(ServiceCommand::Restore(state)) => {
                outputs.restore(state, selected, parallel);
            }
            Err(TryRecvError::Empty | TryRecvError::Disconnected) => return Ok(()),
        }
    }
}

fn service_once(
    devices: &mut ServiceDevices,
    outputs: &mut OutputState,
    input_tracker: &mut InputTracker,
    sidetone: Option<&mut HardwareSidetone>,
    worker: &ServiceWorker,
) -> Result<(), ServiceFailure> {
    let radio = worker.state.outputs();
    if let Some(control) = sidetone {
        if control.apply(radio.receiver_keyed).is_err() {
            worker
                .statistics
                .failure_operation(HardwareOperation::ApplySidetone);
        }
    }
    outputs.service_clip_led(
        devices,
        worker.clip_led_mask,
        worker.state.receive_clip_events(),
    )?;
    devices
        .cm119
        .publish_outputs(Cm119Outputs {
            ptt_asserted: radio.logical_ptt,
            gpio_output_mask: outputs.cm119,
        })
        .map_err(|source| ServiceFailure(HardwareOperation::PublishCm119, source))?;
    devices
        .cm119
        .service()
        .map_err(|source| ServiceFailure(HardwareOperation::ServiceCm119, source))?;
    let cm119 = devices
        .cm119
        .inputs()
        .map_err(|source| ServiceFailure(HardwareOperation::ReadCm119, source))?;
    let cm119_statistics = devices
        .cm119
        .statistics()
        .map_err(|source| ServiceFailure(HardwareOperation::ReadCm119Statistics, source))?;

    outputs.service_remote_radio(devices, radio.logical_ptt)?;
    let parallel_inputs = if let Some(plan) = worker.parallel.as_ref() {
        let device = devices
            .parallel
            .as_mut()
            .expect("a configured parallel plan owns a device");
        device
            .publish_outputs(ParallelOutputs {
                output_mask: parallel_output(
                    plan,
                    outputs.parallel,
                    radio.logical_ptt,
                    devices.ptt_inverted,
                ),
                ..ParallelOutputs::default()
            })
            .map_err(|source| ServiceFailure(HardwareOperation::PublishParallel, source))?;
        device
            .service()
            .map_err(|source| ServiceFailure(HardwareOperation::ServiceParallel, source))?;
        let inputs = device
            .inputs()
            .map_err(|source| ServiceFailure(HardwareOperation::ReadParallel, source))?;
        let device_statistics = device
            .statistics()
            .map_err(|source| ServiceFailure(HardwareOperation::ReadParallelStatistics, source))?;
        worker.statistics.publish_parallel(device_statistics);
        Some(inputs)
    } else {
        None
    };

    worker.state.publish_inputs(hardware_inputs(
        cm119,
        worker.parallel.as_ref(),
        parallel_inputs,
        cm119_statistics,
    ));
    input_tracker.publish(
        cm119,
        worker.cm119_input_mask,
        parallel_inputs,
        worker.parallel.as_ref(),
        &worker.input_sender,
        &worker.statistics,
    );
    worker.statistics.success(cm119_statistics);
    Ok(())
}

#[derive(Clone, Copy, Debug)]
struct ServiceFailure(HardwareOperation, GpioError);

impl From<ServiceFailure> for HardwareStationError {
    fn from(failure: ServiceFailure) -> Self {
        Self::Gpio {
            operation: failure.0,
            source: failure.1,
        }
    }
}

fn parallel_output(
    plan: &ParallelPlan,
    persistent: u8,
    logical_ptt: bool,
    ptt_inverted: bool,
) -> u8 {
    let mut output = persistent & !plan.ptt_mask;
    if logical_ptt != ptt_inverted {
        output |= plan.ptt_mask;
    }
    output
}

fn hardware_inputs(
    cm119: Cm119Inputs,
    parallel_plan: Option<&ParallelPlan>,
    parallel: Option<ParallelInputs>,
    statistics: Cm119Statistics,
) -> HardwareInputs {
    let parallel_online = parallel.filter(|inputs| inputs.online);
    HardwareInputs {
        carrier: cm119.online && cm119.cor_active,
        subaudible: cm119.online && cm119.ctcss_active,
        parallel_carrier: parallel_online
            .zip(parallel_plan)
            .is_some_and(|(inputs, plan)| inputs.status_mask & plan.carrier_mask != 0),
        parallel_subaudible: parallel_online
            .zip(parallel_plan)
            .is_some_and(|(inputs, plan)| inputs.status_mask & plan.ctcss_mask != 0),
        physical_ptt: statistics.online && statistics.ptt_applied,
        cm119_gpio_mask: if cm119.online {
            cm119.gpio_input_mask
        } else {
            0
        },
        parallel_input_mask: parallel_online.map_or(0, |inputs| inputs.status_mask),
    }
}

#[derive(Default)]
struct InputTracker {
    cm119: Option<u8>,
    parallel: Option<u8>,
}

impl InputTracker {
    fn publish(
        &mut self,
        cm119: Cm119Inputs,
        cm119_input_mask: u8,
        parallel: Option<ParallelInputs>,
        parallel_plan: Option<&ParallelPlan>,
        sender: &SyncSender<HardwareInputEvent>,
        statistics: &ServiceStatistics,
    ) {
        let cm119_active = if cm119.online {
            cm119.gpio_input_mask & cm119_input_mask
        } else {
            0
        };
        publish_changed_inputs(
            &mut self.cm119,
            cm119_active,
            cm119_input_mask,
            |index| HardwareInput::Cm119(GpioPin::new(index + 1).expect("GPIO index is bounded")),
            sender,
            statistics,
        );

        if let Some(plan) = parallel_plan {
            let active = parallel
                .filter(|value| value.online)
                .map_or(0, |value| value.status_mask & plan.input_mask);
            publish_changed_inputs(
                &mut self.parallel,
                active,
                plan.input_mask,
                parallel_input,
                sender,
                statistics,
            );
        }
    }
}

fn publish_changed_inputs(
    previous: &mut Option<u8>,
    active: u8,
    configured: u8,
    input: fn(u8) -> HardwareInput,
    sender: &SyncSender<HardwareInputEvent>,
    statistics: &ServiceStatistics,
) {
    let Some(old) = previous.replace(active) else {
        return;
    };
    let mut changed = (old ^ active) & configured;
    while changed != 0 {
        let index = changed.trailing_zeros() as u8;
        let bit = 1_u8 << index;
        let event = HardwareInputEvent {
            input: input(index),
            active: active & bit != 0,
        };
        if matches!(sender.try_send(event), Err(TrySendError::Full(_))) {
            statistics
                .dropped_input_events
                .fetch_add(1, Ordering::Relaxed);
        }
        changed &= !bit;
    }
}

fn parallel_input(index: u8) -> HardwareInput {
    HardwareInput::Parallel([0, 0, 0, 15, 13, 12, 10, 0][usize::from(index)])
}

struct ServiceStatistics {
    cycles: AtomicU64,
    failures: AtomicU64,
    last_failure: AtomicU8,
    cm119_input_reads: AtomicU64,
    cm119_output_applies: AtomicU64,
    cm119_errors: AtomicU64,
    cm119_ptt: AtomicBool,
    cm119_online: AtomicBool,
    cm119_last_error: AtomicI32,
    eeprom_reads: AtomicU64,
    eeprom_writes: AtomicU64,
    parallel_present: bool,
    parallel_input_reads: AtomicU64,
    parallel_output_applies: AtomicU64,
    parallel_errors: AtomicU64,
    parallel_online: AtomicBool,
    parallel_last_error: AtomicI32,
    parallel_applied_output: AtomicU8,
    dropped_input_events: AtomicU64,
}

impl ServiceStatistics {
    fn new(parallel_present: bool) -> Self {
        Self {
            cycles: AtomicU64::new(0),
            failures: AtomicU64::new(0),
            last_failure: AtomicU8::new(0),
            cm119_input_reads: AtomicU64::new(0),
            cm119_output_applies: AtomicU64::new(0),
            cm119_errors: AtomicU64::new(0),
            cm119_ptt: AtomicBool::new(false),
            cm119_online: AtomicBool::new(false),
            cm119_last_error: AtomicI32::new(0),
            eeprom_reads: AtomicU64::new(0),
            eeprom_writes: AtomicU64::new(0),
            parallel_present,
            parallel_input_reads: AtomicU64::new(0),
            parallel_output_applies: AtomicU64::new(0),
            parallel_errors: AtomicU64::new(0),
            parallel_online: AtomicBool::new(false),
            parallel_last_error: AtomicI32::new(0),
            parallel_applied_output: AtomicU8::new(0),
            dropped_input_events: AtomicU64::new(0),
        }
    }

    fn success(&self, value: Cm119Statistics) {
        self.cycles.fetch_add(1, Ordering::Relaxed);
        self.cm119_input_reads
            .store(value.input_read_count, Ordering::Relaxed);
        self.cm119_output_applies
            .store(value.output_apply_count, Ordering::Relaxed);
        self.cm119_errors
            .store(value.usb_error_count, Ordering::Relaxed);
        self.cm119_ptt.store(value.ptt_applied, Ordering::Relaxed);
        self.cm119_online.store(value.online, Ordering::Relaxed);
        self.cm119_last_error
            .store(value.last_usb_error, Ordering::Relaxed);
        self.eeprom_reads
            .store(value.eeprom_read_count, Ordering::Relaxed);
        self.eeprom_writes
            .store(value.eeprom_write_count, Ordering::Relaxed);
    }

    fn publish_parallel(&self, value: ParallelStatistics) {
        self.parallel_input_reads
            .store(value.input_read_count, Ordering::Relaxed);
        self.parallel_output_applies
            .store(value.output_apply_count, Ordering::Relaxed);
        self.parallel_errors
            .store(value.io_error_count, Ordering::Relaxed);
        self.parallel_online.store(value.online, Ordering::Relaxed);
        self.parallel_last_error
            .store(value.last_io_error, Ordering::Relaxed);
        self.parallel_applied_output
            .store(value.applied_output_mask, Ordering::Relaxed);
    }

    fn failure(&self, failure: ServiceFailure) {
        let ServiceFailure(operation, _source) = failure;
        self.cycles.fetch_add(1, Ordering::Relaxed);
        self.failure_operation(operation);
    }

    fn failure_operation(&self, operation: HardwareOperation) {
        self.failures.fetch_add(1, Ordering::Relaxed);
        self.last_failure.store(operation as u8, Ordering::Relaxed);
    }

    fn snapshot(&self) -> HardwareServiceStatistics {
        HardwareServiceStatistics {
            service_cycles: self.cycles.load(Ordering::Relaxed),
            service_failures: self.failures.load(Ordering::Relaxed),
            last_failure: HardwareOperation::from_u8(self.last_failure.load(Ordering::Relaxed)),
            cm119: Cm119Statistics {
                input_read_count: self.cm119_input_reads.load(Ordering::Relaxed),
                output_apply_count: self.cm119_output_applies.load(Ordering::Relaxed),
                usb_error_count: self.cm119_errors.load(Ordering::Relaxed),
                ptt_applied: self.cm119_ptt.load(Ordering::Relaxed),
                online: self.cm119_online.load(Ordering::Relaxed),
                last_usb_error: self.cm119_last_error.load(Ordering::Relaxed),
                eeprom_read_count: self.eeprom_reads.load(Ordering::Relaxed),
                eeprom_write_count: self.eeprom_writes.load(Ordering::Relaxed),
            },
            parallel: self.parallel_present.then(|| ParallelStatistics {
                input_read_count: self.parallel_input_reads.load(Ordering::Relaxed),
                output_apply_count: self.parallel_output_applies.load(Ordering::Relaxed),
                io_error_count: self.parallel_errors.load(Ordering::Relaxed),
                online: self.parallel_online.load(Ordering::Relaxed),
                last_io_error: self.parallel_last_error.load(Ordering::Relaxed),
                applied_output_mask: self.parallel_applied_output.load(Ordering::Relaxed),
            }),
            dropped_input_events: self.dropped_input_events.load(Ordering::Relaxed),
        }
    }
}

#[cfg(test)]
#[path = "tests/hardware_host_tests.rs"]
mod tests;
