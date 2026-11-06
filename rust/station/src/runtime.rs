//! Direct PortAudio host for one prepared station.

use std::cell::UnsafeCell;
use std::ffi::{c_int, c_void};
use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use usbradioplus_asl3::{
    ADVANCED_FRAME_SAMPLES, CtcssTone, CtcssToneIndex, ReceiveMetadata, ReceiveQualification,
    ReceiveStatus,
};
use usbradioplus_audio::{
    AudioError, AudioProvider, AudioStream, ReceiveWorkerEndpoint, StreamConfig, StreamStatistics,
    StreamTiming, TransmitWorkerEndpoint,
};
use usbradioplus_radio::{
    CTCSS_TONE_COUNT, ReceiveControls, ReceiveResult, TransmitControls, TransmitResult,
};

use crate::{
    SelectedHardwarePlan, StationControl, StationMedia, StationPlan, StationReceive,
    StationTransmit,
};

const CALLBACK_OK: c_int = 0;
const CALLBACK_FAILED: c_int = -1;

#[path = "update.rs"]
mod update;
use update::UpdateMailbox;
pub use update::{StationUpdate, StationUpdateError, StationUpdatePreparation};

const HARDWARE_CARRIER: u32 = 1 << 0;
const PARALLEL_CARRIER: u32 = 1 << 1;
const HARDWARE_SUBAUDIBLE: u32 = 1 << 2;
const PARALLEL_SUBAUDIBLE: u32 = 1 << 3;
const PHYSICAL_PTT: u32 = 1 << 4;
const CM119_GPIO_SHIFT: u32 = 5;
const PARALLEL_INPUT_SHIFT: u32 = 13;

const EXTERNAL_PTT: u32 = 1 << 0;
const RENDER_ADMITTED: u32 = 1 << 1;
const CTCSS_INHIBIT: u32 = 1 << 2;
const CALIBRATED_TEST_TONE: u32 = 1 << 3;
const RECEIVE_CTCSS_OVERRIDE: u32 = 1 << 4;
const FORCED_CTCSS_SHIFT: u32 = 5;

const LOGICAL_PTT: u32 = 1;
const OUTPUT_TONE_SHIFT: u32 = 1;

/// Hardware values already sampled by the non-real-time device-control owner.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HardwareInputs {
    /// Normal CM119 carrier input.
    pub carrier: bool,
    /// Normal parallel-port carrier input.
    pub parallel_carrier: bool,
    /// Normal CM119 CTCSS input.
    pub subaudible: bool,
    /// Normal parallel-port CTCSS input.
    pub parallel_subaudible: bool,
    /// Whether the hardware owner has applied PTT.
    pub physical_ptt: bool,
    /// Latest logical CM119 GPIO input bits.
    pub cm119_gpio_mask: u8,
    /// Latest configured parallel-port ordinary input bits.
    pub parallel_input_mask: u8,
}

/// Controller intent sampled by the DAC-paced transmit callback.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ControllerRequests {
    /// Whether the controller requests transmission.
    pub transmit: bool,
    /// Whether transmitter settling admits rendered audio.
    pub render_admitted: bool,
    /// Whether CTCSS generation is temporarily inhibited.
    pub ctcss_inhibit: bool,
    /// Whether the calibrated 1 kHz tone replaces program audio.
    pub calibrated_test_tone: bool,
    /// Whether receive subaudible qualification is temporarily bypassed.
    pub subaudible_override: bool,
    /// Forced transmit CTCSS frequency, when requested.
    pub forced_ctcss: Option<CtcssTone>,
}

/// Latest radio result needed by the non-real-time hardware owner.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HardwareOutputs {
    /// Qualified receiver state for configured hardware local repeat.
    pub receiver_keyed: bool,
    /// Logical radio-core PTT intent.
    pub logical_ptt: bool,
    /// Selected transmit CTCSS frequency, when one is active.
    pub selected_ctcss_tenths_hz: Option<u16>,
}

/// Lock-free callback health counters.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct CallbackStatistics {
    /// Capture callback worker calls.
    pub receive_calls: u64,
    /// Playback callback worker calls.
    pub transmit_calls: u64,
    /// Receive calls rejected by the radio or controller boundary.
    pub receive_failures: u64,
    /// Transmit calls rejected by the radio boundary.
    pub transmit_failures: u64,
}

/// Audio-adapter and station-callback statistics sampled together.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StationRuntimeStatistics {
    /// PortAudio/ALSA adapter statistics.
    pub audio: StreamStatistics,
    /// Direct station callback counters.
    pub callbacks: CallbackStatistics,
}

#[derive(Default)]
struct HardwareAtomics {
    inputs: AtomicU32,
    requests: AtomicU32,
    outputs: AtomicU32,
    receiver_keyed: AtomicU32,
    remote_control: AtomicU32,
    remote_transmit: AtomicU64,
    remote_signals: AtomicU32,
    remote_tune: AtomicU32,
    transmit_ctcss_ready: AtomicU32,
    receive_clip_events: AtomicU64,
    receive_observation_sequence: AtomicU64,
    receive_input_peak: AtomicU32,
    receive_input_rms: AtomicU32,
    receive_ctcss_decoder_peak: AtomicU32,
    receive_input_rail_samples: AtomicU64,
    receive_output_peak: AtomicU32,
    receive_output_rms: AtomicU32,
    receive_output_rail_samples: AtomicU64,
    receive_rssi_peak: AtomicU32,
    receive_rssi_updated: AtomicU32,
    receive_calls: AtomicU64,
    transmit_calls: AtomicU64,
    receive_failures: AtomicU64,
    transmit_failures: AtomicU64,
}

/// Shared lock-free control surface between audio and hardware owners.
#[derive(Clone, Default)]
pub struct SharedHardwareState(Arc<HardwareAtomics>);

/// One coherent applied rig-policy publication. Disabled preserves ordinary operation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RemoteRadioControl {
    /// A configured remote-base owner controls admission.
    pub enabled: bool,
    /// The current receive state is safe to route.
    pub receive_permitted: bool,
    /// The current applied state permits new transmit demand.
    pub transmit_permitted: bool,
    /// Applied mode requires transmit admission independent of open receive.
    pub non_fm: bool,
    /// The configured local carrier is explicitly open with no enabled tone qualifier.
    /// This is a source fact, not an assertion that all non-FM reception is idle.
    pub local_unqualified_open: bool,
}

impl RemoteRadioControl {
    fn flags(self) -> u32 {
        u32::from(self.enabled)
            | (u32::from(self.receive_permitted) << 1)
            | (u32::from(self.transmit_permitted) << 2)
            | (u32::from(self.non_fm) << 3)
            | (u32::from(self.local_unqualified_open) << 4)
    }
}

/// Raw source and intent state, distinct from routed audio and signaling tails.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RemoteRadioActivity {
    /// Latest qualified local source, before remote admission muting.
    pub local_receiving: bool,
    /// Current linked-source activity before controller timeout/muting.
    pub linked_receiving: bool,
    /// Controller demand before the radio renderer and remote inhibition.
    pub transmit_requested: bool,
    /// Independent controller CTCSS intent, including command-response delay.
    pub ctcss_requested: bool,
    /// Rising-edge epoch, published atomically with requested key state.
    pub transmit_epoch: u64,
}

/// Confirmed independent radio-control observations, never raw GPIO substitutes.
/// The selected PTT source is a generation property: keep it selected on failure
/// and publish `None`, rather than falling back to another provider.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RemoteHardwareSignals {
    /// Normalized carrier snapshot; None means unavailable or stale.
    pub carrier: Option<bool>,
    /// This generation explicitly selected independent radio-control PTT.
    pub ptt_selected: bool,
    /// Successfully serviced/observed PTT state, not logical transmit intent.
    pub physical_ptt: Option<bool>,
}

/// Latest complete receive measurement published by the native callback.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ReceiveObservation {
    /// Raw selected-channel normalized peak.
    pub input_peak: f32,
    /// Raw selected-channel normalized RMS.
    pub input_rms: f32,
    /// Raw samples at a hardware rail in the latest callback.
    pub input_rail_samples: u64,
    /// Post-decoder-gain CTCSS half peak-to-peak level as normalized PCM.
    pub ctcss_decoder_peak: f32,
    /// Processed normalized peak.
    pub output_peak: f32,
    /// Processed normalized RMS.
    pub output_rms: f32,
    /// Processed samples at a hardware rail in the latest callback.
    pub output_rail_samples: u64,
    /// Compatibility noise-detector peak in established signed-PCM units.
    pub rssi_peak: i16,
    /// Whether the latest callback completed an RSSI integration window.
    pub rssi_updated: bool,
}

impl SharedHardwareState {
    /// Publish bounded remote tune intent independently of controller/tuner
    /// requests. Existing radio rendering owns tone generation and PTT tails.
    pub fn publish_remote_tune(&self, enabled: bool) {
        self.0
            .remote_tune
            .store(u32::from(enabled), Ordering::Release);
    }
    /// Publish independent snapshots in one coherent callback-safe atomic word.
    pub fn publish_remote_signals(&self, value: RemoteHardwareSignals) {
        let flags = u32::from(value.carrier.is_some())
            | (u32::from(value.carrier == Some(true)) << 1)
            | (u32::from(value.physical_ptt.is_some()) << 2)
            | (u32::from(value.physical_ptt == Some(true)) << 3)
            | (u32::from(value.ptt_selected) << 4);
        self.0.remote_signals.store(flags, Ordering::Release);
    }
    /// Copy independent source observations without changing GPIO diagnostics.
    pub fn remote_signals(&self) -> RemoteHardwareSignals {
        let flags = self.0.remote_signals.load(Ordering::Acquire);
        RemoteHardwareSignals {
            carrier: (flags & 1 != 0).then_some(flags & 2 != 0),
            physical_ptt: (flags & 4 != 0).then_some(flags & 8 != 0),
            ptt_selected: flags & 16 != 0,
        }
    }
    /// Publish one applied remote policy before any blocking retune operation.
    pub fn publish_remote_control(&self, value: RemoteRadioControl) {
        self.0
            .remote_control
            .store(value.flags(), Ordering::Release);
    }
    /// Read the current complete remote policy without a lock or allocation.
    pub fn remote_control(&self) -> RemoteRadioControl {
        let flags = self.0.remote_control.load(Ordering::Acquire);
        RemoteRadioControl {
            enabled: flags & 1 != 0,
            receive_permitted: flags & 2 != 0,
            transmit_permitted: flags & 4 != 0,
            non_fm: flags & 8 != 0,
            local_unqualified_open: flags & 16 != 0,
        }
    }
    /// Read requested activity; radio-rendered PTT tails never advance its epoch.
    pub fn remote_activity(&self) -> RemoteRadioActivity {
        let transmit = self.0.remote_transmit.load(Ordering::Acquire);
        RemoteRadioActivity {
            local_receiving: self.0.receiver_keyed.load(Ordering::Acquire) != 0,
            linked_receiving: transmit & 4 != 0,
            transmit_requested: transmit & 1 != 0,
            ctcss_requested: transmit & 2 != 0,
            transmit_epoch: transmit >> 3,
        }
    }
    fn publish_transmit_intent(&self, requested: bool, ctcss: bool, linked: bool) {
        // TX is the sole writer. A single word keeps demand and its rising-edge
        // epoch coherent even if the worker samples during the publication.
        let previous = self.0.remote_transmit.load(Ordering::Relaxed);
        let next = if requested {
            previous.wrapping_add(if previous & 1 == 0 { 8 } else { 0 }) | 1
        } else {
            previous & !1
        };
        self.0.remote_transmit.store(
            (next & !6) | (u64::from(ctcss) << 1) | (u64::from(linked) << 2),
            Ordering::Release,
        );
    }
    /// Publish the latest already-read hardware inputs.
    pub fn publish_inputs(&self, inputs: HardwareInputs) {
        self.0.inputs.store(pack_inputs(inputs), Ordering::Release);
    }

    /// Read the latest complete hardware-input snapshot.
    #[must_use]
    pub fn inputs(&self) -> HardwareInputs {
        unpack_inputs(self.0.inputs.load(Ordering::Acquire))
    }

    /// Publish the complete current controller request.
    pub fn publish_requests(&self, requests: ControllerRequests) {
        self.0
            .requests
            .store(pack_requests(requests), Ordering::Release);
    }

    /// Read the latest complete controller request.
    #[must_use]
    pub fn requests(&self) -> ControllerRequests {
        unpack_requests(self.0.requests.load(Ordering::Acquire))
    }

    /// Read the latest radio output needed for hardware service.
    #[must_use]
    pub fn outputs(&self) -> HardwareOutputs {
        let mut outputs = unpack_outputs(self.0.outputs.load(Ordering::Acquire));
        outputs.receiver_keyed = self.0.receiver_keyed.load(Ordering::Acquire) != 0;
        outputs
    }

    /// Read callback call and failure counters without waiting.
    #[must_use]
    pub fn callback_statistics(&self) -> CallbackStatistics {
        CallbackStatistics {
            receive_calls: self.0.receive_calls.load(Ordering::Relaxed),
            transmit_calls: self.0.transmit_calls.load(Ordering::Relaxed),
            receive_failures: self.0.receive_failures.load(Ordering::Relaxed),
            transmit_failures: self.0.transmit_failures.load(Ordering::Relaxed),
        }
    }

    /// Read the monotonic number of receive callbacks which observed rail samples.
    #[must_use]
    pub fn receive_clip_events(&self) -> u64 {
        self.0.receive_clip_events.load(Ordering::Acquire)
    }

    /// Read the latest internally consistent receive measurement without locking.
    #[must_use]
    pub fn receive_observation(&self) -> ReceiveObservation {
        loop {
            let before = self.0.receive_observation_sequence.load(Ordering::Acquire);
            let observation = ReceiveObservation {
                input_peak: f32::from_bits(self.0.receive_input_peak.load(Ordering::Relaxed)),
                input_rms: f32::from_bits(self.0.receive_input_rms.load(Ordering::Relaxed)),
                input_rail_samples: self.0.receive_input_rail_samples.load(Ordering::Relaxed),
                ctcss_decoder_peak: f32::from_bits(
                    self.0.receive_ctcss_decoder_peak.load(Ordering::Relaxed),
                ),
                output_peak: f32::from_bits(self.0.receive_output_peak.load(Ordering::Relaxed)),
                output_rms: f32::from_bits(self.0.receive_output_rms.load(Ordering::Relaxed)),
                output_rail_samples: self.0.receive_output_rail_samples.load(Ordering::Relaxed),
                rssi_peak: self.0.receive_rssi_peak.load(Ordering::Relaxed) as i16,
                rssi_updated: self.0.receive_rssi_updated.load(Ordering::Relaxed) != 0,
            };
            let after = self.0.receive_observation_sequence.load(Ordering::Acquire);
            if let Some(observation) = stable_observation(before, after, observation) {
                return observation;
            }
            std::hint::spin_loop();
        }
    }

    fn receive_controls(&self) -> ReceiveControls {
        let inputs = self.inputs();
        let requests = self.requests();
        ReceiveControls {
            external_carrier: self.remote_signals().carrier,
            hardware_carrier: inputs.carrier,
            parallel_carrier: inputs.parallel_carrier,
            hardware_subaudible: inputs.subaudible,
            parallel_subaudible: inputs.parallel_subaudible,
            subaudible_override: requests.subaudible_override,
        }
    }

    fn transmit_controls(&self) -> TransmitControls {
        let inputs = self.inputs();
        let requests = self.requests();
        let remote = self.remote_signals();
        let tune = self.0.remote_tune.load(Ordering::Acquire) != 0;
        TransmitControls {
            external_ptt_request: requests.transmit || tune,
            physical_ptt_applied: if remote.ptt_selected {
                remote.physical_ptt == Some(true)
            } else {
                inputs.physical_ptt
            },
            render_admitted: requests.render_admitted || tune,
            ctcss_inhibit: requests.ctcss_inhibit,
            calibrated_test_tone: requests.calibrated_test_tone || tune,
            forced_ctcss_tenths_hz: requests
                .forced_ctcss
                .map_or(0, |tone| i32::from(tone.tenths_hz())),
        }
    }

    fn publish_transmit_result(&self, result: TransmitResult) {
        let tone = u16::try_from(result.selected_ctcss_tenths_hz)
            .ok()
            .filter(|value| *value != 0);
        let packed = pack_outputs(HardwareOutputs {
            receiver_keyed: false,
            logical_ptt: result.logical_ptt,
            selected_ctcss_tenths_hz: tone,
        });
        let previous = self.0.outputs.swap(packed, Ordering::AcqRel);
        if packed >> OUTPUT_TONE_SHIFT != previous >> OUTPUT_TONE_SHIFT {
            self.0
                .transmit_ctcss_ready
                .store(packed >> OUTPUT_TONE_SHIFT, Ordering::Release);
        }
    }

    fn publish_transmit_fault(&self) {
        // A failed controller callback must release hardware PTT immediately,
        // independently of the radio renderer's tone-off or drain state.
        self.0.outputs.store(0, Ordering::Release);
        self.0.transmit_ctcss_ready.store(0, Ordering::Release);
        self.publish_transmit_intent(false, false, false);
    }

    fn publish_receiver_keyed(&self, receiver_keyed: bool) {
        self.0
            .receiver_keyed
            .store(u32::from(receiver_keyed), Ordering::Release);
    }

    fn publish_receive_clipping(&self, rail_samples: u64) {
        if rail_samples != 0 {
            self.0.receive_clip_events.fetch_add(1, Ordering::Release);
        }
    }

    fn publish_receive_observation(&self, result: ReceiveResult) {
        self.0
            .receive_observation_sequence
            .fetch_add(1, Ordering::AcqRel);
        self.0
            .receive_input_peak
            .store(result.input_peak.to_bits(), Ordering::Relaxed);
        self.0
            .receive_input_rms
            .store(result.input_rms.to_bits(), Ordering::Relaxed);
        self.0
            .receive_input_rail_samples
            .store(result.input_rail_samples, Ordering::Relaxed);
        self.0
            .receive_ctcss_decoder_peak
            .store(result.ctcss_decoder_peak.to_bits(), Ordering::Relaxed);
        self.0
            .receive_output_peak
            .store(result.output_peak.to_bits(), Ordering::Relaxed);
        self.0
            .receive_output_rms
            .store(result.output_rms.to_bits(), Ordering::Relaxed);
        self.0
            .receive_output_rail_samples
            .store(result.output_rail_samples, Ordering::Relaxed);
        self.0
            .receive_rssi_peak
            .store(result.rssi_peak as u32, Ordering::Relaxed);
        self.0
            .receive_rssi_updated
            .store(u32::from(result.rssi_updated), Ordering::Relaxed);
        self.0
            .receive_observation_sequence
            .fetch_add(1, Ordering::Release);
    }

    fn take_transmit_ctcss(&self) -> Option<CtcssTone> {
        let value = self.0.transmit_ctcss_ready.swap(0, Ordering::AcqRel);
        u16::try_from(value)
            .ok()
            .and_then(CtcssTone::from_tenths_hz)
    }

    fn restore_transmit_ctcss(&self, tone: CtcssTone) {
        let _ = self.0.transmit_ctcss_ready.compare_exchange(
            0,
            u32::from(tone.tenths_hz()),
            Ordering::Release,
            Ordering::Relaxed,
        );
    }

    fn note_receive(&self, failed: bool) {
        self.0.receive_calls.fetch_add(1, Ordering::Relaxed);
        if failed {
            self.0.receive_failures.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn note_transmit(&self, failed: bool) {
        self.0.transmit_calls.fetch_add(1, Ordering::Relaxed);
        if failed {
            self.0.transmit_failures.fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn stable_observation(
    before: u64,
    after: u64,
    observation: ReceiveObservation,
) -> Option<ReceiveObservation> {
    (before & 1 == 0 && before == after).then_some(observation)
}

struct ReceiveContext {
    station: StationReceive,
    mono: Box<[f32]>,
    hardware: SharedHardwareState,
    controller_ctcss: [Option<CtcssTone>; CTCSS_TONE_COUNT],
    voter_reporting: bool,
    updates: Arc<UpdateMailbox>,
}

struct TransmitContext {
    station: StationTransmit,
    maximum_frames: usize,
    mono: Box<[f32]>,
    hardware: SharedHardwareState,
    updates: Arc<UpdateMailbox>,
    remote_burst_admitted: bool,
}

/// One stopped, running, or reload-suspended direct PortAudio station composition.
///
/// The stream is declared first so its destructor stops and destroys both
/// callbacks before their contexts or the boxed station media are released.
pub struct StationRuntime {
    stream: Option<AudioStream<'static, 'static>>,
    _receive_context: Box<UnsafeCell<ReceiveContext>>,
    _transmit_context: Box<UnsafeCell<TransmitContext>>,
    provider: AudioProvider,
    stream_config: StreamConfig,
    hardware: SharedHardwareState,
    updates: Arc<UpdateMailbox>,
    running: bool,
}

/// Serialized controller and radio-observer owner paired with one runtime.
///
/// This value is independent of [`StationRuntime`] so an Asterisk delivery
/// worker can own it without borrowing state mutated by live audio callbacks.
pub struct StationControlHost {
    plan: StationPlan,
    control: StationControl,
    controller: Option<usbradioplus_asl3::ControllerState>,
    hardware: SharedHardwareState,
    _updates: Arc<UpdateMailbox>,
}

impl StationControlHost {
    /// Return the immutable effective station plan.
    #[must_use]
    pub const fn plan(&self) -> &StationPlan {
        &self.plan
    }

    /// Borrow serialized ASL3 controller state.
    ///
    /// # Panics
    /// Panics for native media, whose controller remains in its attaching host.
    pub fn controller(&mut self) -> &mut usbradioplus_asl3::ControllerState {
        self.controller.as_mut().expect("ASL3 controller media")
    }

    /// Borrow the radio event and diagnostic observer.
    pub fn radio(&mut self) -> &mut StationControl {
        &mut self.control
    }

    /// Clone the lock-free state handle shared with both callbacks.
    #[must_use]
    pub fn hardware_state(&self) -> SharedHardwareState {
        self.hardware.clone()
    }
}

impl StationRuntime {
    /// Whether callbacks have started without a subsequent successful stop.
    #[must_use]
    pub fn running(&self) -> bool {
        self.running
    }

    /// Open native media using the same stereo callback workers, without an
    /// Asterisk controller or delivery worker. No additional audio ring is made.
    pub fn open_native(
        media: crate::NativeStationMedia,
        selected: &SelectedHardwarePlan,
        provider: AudioProvider,
    ) -> Result<(Self, StationControl), AudioError> {
        let crate::NativeStationMedia {
            config,
            receive,
            transmit,
            control,
        } = media;
        let runtime = Self::open_endpoints(
            receive,
            transmit,
            &config,
            selected.stream,
            provider,
            [None; CTCSS_TONE_COUNT],
            false,
        )?;
        Ok((runtime, control))
    }

    /// Open native media with the same prepared forward/reverse update owner as
    /// ASL3. No ASL controller or delivery worker is allocated.
    ///
    /// # Errors
    /// Rejects a plan that differs from the prepared radio description, or an
    /// audio-adapter failure. Callers retain one physical device selection.
    pub fn open_native_planned(
        media: crate::NativeStationMedia,
        plan: StationPlan,
        selected: &SelectedHardwarePlan,
        provider: AudioProvider,
    ) -> Result<(Self, StationControlHost), AudioError> {
        if media.config != *plan.radio() {
            return Err(AudioError::InvalidArgument);
        }
        let (runtime, control) = Self::open_native(media, selected, provider)?;
        let control = StationControlHost {
            plan,
            control,
            controller: None,
            hardware: runtime.hardware.clone(),
            _updates: Arc::clone(&runtime.updates),
        };
        Ok((runtime, control))
    }

    /// Open a stopped stream around one fully prepared station.
    ///
    /// # Errors
    ///
    /// Returns an audio-adapter error when callback bounds are incompatible or
    /// the selected stream cannot be opened.
    pub fn open(
        media: StationMedia,
        selected: &SelectedHardwarePlan,
        provider: AudioProvider,
    ) -> Result<(Self, StationControlHost), AudioError> {
        let controller_ctcss = controller_ctcss_table(&media.plan);
        let voter_reporting = media.plan.configuration().station.hardware.voter_reporting != 0;
        let StationMedia {
            plan,
            receive,
            transmit,
            control,
            controller,
        } = media;
        let runtime = Self::open_endpoints(
            receive,
            transmit,
            plan.radio(),
            selected.stream,
            provider,
            controller_ctcss,
            voter_reporting,
        )?;
        let control = StationControlHost {
            plan,
            control,
            controller: Some(controller),
            hardware: runtime.hardware.clone(),
            _updates: Arc::clone(&runtime.updates),
        };
        Ok((runtime, control))
    }

    fn open_endpoints(
        receive: StationReceive,
        transmit: StationTransmit,
        radio: &usbradioplus_radio::SessionConfig,
        stream: StreamConfig,
        provider: AudioProvider,
        controller_ctcss: [Option<CtcssTone>; CTCSS_TONE_COUNT],
        voter_reporting: bool,
    ) -> Result<Self, AudioError> {
        if stream.maximum_receive_frame_count == 0
            || stream.maximum_transmit_frame_count == 0
            || stream.maximum_receive_frame_count > ADVANCED_FRAME_SAMPLES as u32
            || stream.maximum_receive_frame_count > radio.maximum_receive_frame_count
            || stream.maximum_transmit_frame_count > radio.maximum_transmit_frame_count
        {
            return Err(AudioError::InvalidArgument);
        }
        let hardware = SharedHardwareState::default();
        let updates = Arc::new(UpdateMailbox::default());
        let receive_context = Box::new(UnsafeCell::new(ReceiveContext {
            station: receive,
            mono: vec![0.0; stream.maximum_receive_frame_count as usize].into_boxed_slice(),
            hardware: hardware.clone(),
            controller_ctcss,
            voter_reporting,
            updates: Arc::clone(&updates),
        }));
        let transmit_context = Box::new(UnsafeCell::new(TransmitContext {
            station: transmit,
            maximum_frames: stream.maximum_transmit_frame_count as usize,
            mono: vec![0.0; stream.maximum_transmit_frame_count as usize].into_boxed_slice(),
            hardware: hardware.clone(),
            updates: Arc::clone(&updates),
            remote_burst_admitted: true,
        }));
        let mut runtime = Self {
            stream: None,
            _receive_context: receive_context,
            _transmit_context: transmit_context,
            provider,
            stream_config: stream,
            hardware: hardware.clone(),
            updates: Arc::clone(&updates),
            running: false,
        };
        runtime.reopen()?;
        Ok(runtime)
    }

    /// Reopen a reload-suspended stream without starting its callbacks.
    ///
    /// The immutable configuration and stable callback boxes survive suspension;
    /// reopening therefore restores device ownership without replacing media.
    pub fn reopen(&mut self) -> Result<(), AudioError> {
        if self.stream.is_some() {
            return Ok(());
        }
        // SAFETY: no stream owns these endpoints while suspended. Both boxed
        // UnsafeCell contexts keep stable addresses until the reopened stream
        // is stopped and destroyed, with one serial mutator per disjoint context.
        let receive = unsafe {
            ReceiveWorkerEndpoint::from_raw(
                receive_callback,
                NonNull::new_unchecked(self._receive_context.get()).cast(),
            )
        };
        // SAFETY: the transmit context is disjoint from the receive context and
        // obeys the same stream-before-context teardown contract.
        let transmit = unsafe {
            TransmitWorkerEndpoint::from_raw(
                transmit_callback,
                NonNull::new_unchecked(self._transmit_context.get()).cast(),
            )
        };
        self.stream = Some(
            self.provider
                .open_stream(self.stream_config, receive, transmit)?,
        );
        Ok(())
    }

    /// Reopen if reload-suspended, then start capture and playback callbacks.
    pub fn start(&mut self) -> Result<(), AudioError> {
        self.reopen()?;
        self.stream
            .as_mut()
            .expect("successful reopen owns a stream")
            .start()?;
        self.running = true;
        Ok(())
    }

    /// Stop capture and playback callbacks; repeated calls are harmless.
    pub fn stop(&mut self) -> Result<(), AudioError> {
        self.stream.as_mut().map_or(Ok(()), AudioStream::stop)?;
        self.running = false;
        Ok(())
    }

    /// Stop callbacks and release the exclusive device lease for reload.
    ///
    /// A stop failure retains the stream because callback quiescence is not
    /// established. Successful suspension preserves media for rollback.
    pub fn suspend(&mut self) -> Result<(), AudioError> {
        self.stop()?;
        drop(self.stream.take());
        Ok(())
    }

    /// Read current audio and station callback statistics.
    /// Returns `InvalidArgument` while reload-suspended, without an open stream.
    pub fn statistics(&self) -> Result<StationRuntimeStatistics, AudioError> {
        Ok(StationRuntimeStatistics {
            audio: self
                .stream
                .as_ref()
                .ok_or(AudioError::InvalidArgument)?
                .statistics()?,
            callbacks: self.hardware.callback_statistics(),
        })
    }

    /// Read immutable PortAudio stream timing.
    /// Returns `InvalidArgument` while reload-suspended, without an open stream.
    pub fn timing(&self) -> Result<StreamTiming, AudioError> {
        self.stream
            .as_ref()
            .ok_or(AudioError::InvalidArgument)?
            .timing()
    }

    /// Clone the lock-free state handle used by the hardware/control owner.
    #[must_use]
    pub fn hardware_state(&self) -> SharedHardwareState {
        self.hardware.clone()
    }
}

impl Drop for StationRuntime {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

fn fail_receive(context: &mut ReceiveContext, frames: usize) -> c_int {
    let frames = frames.min(context.mono.len());
    context.mono[..frames].fill(0.0);
    context.hardware.publish_receiver_keyed(false);
    let qualification = ReceiveQualification {
        carrier_active: false,
        subaudible_active: false,
        receiver_keyed: false,
        ctcss_decoded: None,
        dcs_valid: false,
    };
    context.station.publish_qualification(qualification);
    if let Some(direct) = context.station.direct {
        let metadata = crate::DirectReceiveMetadata {
            struct_size: std::mem::size_of::<crate::DirectReceiveMetadata>() as u32,
            abi_version: 1,
            frame_count: frames as u32,
            vad_voice_sample_index: u64::MAX,
            status: CALLBACK_FAILED,
            ..crate::DirectReceiveMetadata::default()
        };
        // SAFETY: the retained direct endpoint accepts cancellation without source identity.
        unsafe {
            (direct.receive.expect("validated direct receive"))(
                direct.receive_context,
                &metadata,
                context.mono.as_mut_ptr(),
                frames as u32,
            );
        }
    } else {
        let _ = context.station.controller().publish(
            &context.mono[..frames],
            ReceiveMetadata {
                qualification,
                controller_ctcss: None,
                statuses: [None, None],
            },
        );
    }
    context.hardware.note_receive(true);
    CALLBACK_FAILED
}

unsafe extern "C" fn receive_callback(
    context: *mut c_void,
    input: *const f32,
    frame_count: u32,
) -> c_int {
    let Some(mut context) = NonNull::new(context).map(NonNull::cast::<ReceiveContext>) else {
        return CALLBACK_FAILED;
    };
    // SAFETY: stream setup binds this pointer to the uniquely owned stable box.
    let context = unsafe { context.as_mut() };
    let frames = frame_count as usize;
    if input.is_null() || frames == 0 || frames > context.mono.len() {
        return fail_receive(context, frames);
    }
    // SAFETY: the audio adapter supplies canonical stereo input for each frame.
    let input = unsafe { std::slice::from_raw_parts(input, frames * 2) };
    context.updates.receive(
        &mut context.station,
        &mut context.controller_ctcss,
        &mut context.voter_reporting,
    );
    let station = &mut context.station;
    let mut result = match station.radio().process(
        input,
        &mut context.mono[..frames],
        context.hardware.receive_controls(),
    ) {
        Ok(result) => result,
        Err(_) => {
            return fail_receive(context, frames);
        }
    };
    context
        .hardware
        .publish_receiver_keyed(result.receiver_keyed);
    context
        .hardware
        .publish_receive_clipping(result.input_rail_samples);
    context.hardware.publish_receive_observation(result);
    let remote = context.hardware.remote_control();
    if remote.enabled && !remote.receive_permitted {
        context.mono[..frames].fill(0.0);
        result.receiver_keyed = false;
        result.raw_receiver_keyed = false;
    }
    let metadata = receive_metadata(
        result,
        &context.controller_ctcss,
        context.voter_reporting,
        &context.hardware,
    );
    if let Some(direct) = station.direct {
        station.publish_qualification(metadata.qualification);
        let direct_metadata = crate::DirectReceiveMetadata::from_result(result);
        // SAFETY: attachment validated and retained these endpoints until stream
        // shutdown; mono is the exact processed span owned by this callback.
        let status = unsafe {
            (direct.receive.expect("validated direct receive"))(
                direct.receive_context,
                &direct_metadata,
                context.mono.as_mut_ptr(),
                frame_count,
            )
        };
        context.hardware.note_receive(status != CALLBACK_OK);
        return if status == CALLBACK_OK {
            CALLBACK_OK
        } else {
            CALLBACK_FAILED
        };
    }
    if station
        .controller()
        .publish(&context.mono[..frames], metadata)
        .is_err()
    {
        if let Some(ReceiveStatus::TransmitCtcssReady(tone)) = metadata.statuses[0] {
            context.hardware.restore_transmit_ctcss(tone);
        }
        context.hardware.note_receive(true);
        return CALLBACK_FAILED;
    }
    context.hardware.note_receive(false);
    CALLBACK_OK
}

unsafe fn exchange_remote_metadata(
    callbacks: crate::DirectCallbacks,
    control: RemoteRadioControl,
) -> Result<bool, ()> {
    let Some(metadata) = callbacks.metadata else {
        return Ok(false);
    };
    let mut linked = 0;
    // SAFETY: the caller retains this endpoint and invokes it on its serial TX owner.
    let result = unsafe { metadata(callbacks.transmit_context, control.flags(), &mut linked) };
    if result == CALLBACK_OK && linked <= 1 {
        Ok(linked != 0)
    } else {
        Err(())
    }
}

unsafe extern "C" fn transmit_callback(
    context: *mut c_void,
    output: *mut f32,
    frame_count: u32,
) -> c_int {
    let Some(mut context) = NonNull::new(context).map(NonNull::cast::<TransmitContext>) else {
        return CALLBACK_FAILED;
    };
    // SAFETY: stream setup binds this pointer to the uniquely owned stable box.
    let context = unsafe { context.as_mut() };
    let frames = frame_count as usize;
    if output.is_null() || frames == 0 || frames > context.maximum_frames {
        if context.station.native {
            context.hardware.publish_transmit_fault();
            if !output.is_null() {
                // SAFETY: the adapter advertises this writable stereo span even
                // when its frame count exceeds the prepared processing bound.
                unsafe { std::slice::from_raw_parts_mut(output, frames * 2) }.fill(0.0);
            }
        }
        context.hardware.note_transmit(true);
        return CALLBACK_FAILED;
    }
    // SAFETY: the audio adapter supplies two writable samples for each frame.
    let output = unsafe { std::slice::from_raw_parts_mut(output, frames * 2) };
    output.fill(0.0);
    context.updates.transmit(&mut context.station);
    let station = &mut context.station;
    let mut controls = context.hardware.transmit_controls();
    let mut direct_failed = false;
    let mut linked_activity = false;
    if let Some(direct) = station.direct {
        let mono = &mut context.mono[..frames];
        mono.fill(0.0);
        let mut keyed = 0;
        let mut ctcss_enabled = 0;
        let remote = context.hardware.remote_control();
        // SAFETY: the retained metadata endpoint belongs to this same serial TX owner.
        let before = unsafe { exchange_remote_metadata(direct, remote) };
        let status = if before.is_ok() {
            // SAFETY: attachment retains the serial TX endpoint; scratch and
            // result storage remain writable for this synchronous invocation.
            unsafe {
                (direct.transmit.expect("validated direct transmit"))(
                    direct.transmit_context,
                    mono.as_mut_ptr(),
                    frame_count,
                    &mut keyed,
                    &mut ctcss_enabled,
                )
            }
        } else {
            CALLBACK_FAILED
        };
        // SAFETY: query after rendering so published source activity belongs to this frame.
        let after = unsafe { exchange_remote_metadata(direct, remote) };
        if status != CALLBACK_OK || keyed > 1 || ctcss_enabled > 1 || after.is_err() {
            direct_failed = true;
            mono.fill(0.0);
            controls.external_ptt_request = false;
            controls.calibrated_test_tone = false;
            controls.forced_ctcss_tenths_hz = 0;
            controls.ctcss_inhibit = true;
            context.hardware.publish_transmit_fault();
            if station.native {
                context.hardware.note_transmit(true);
                return CALLBACK_FAILED;
            }
        } else {
            linked_activity = after.unwrap_or(false);
            // A manual tuner key owns its CTCSS intent even while the ordinary
            // controller is idle. Explicit inhibition still wins over both keys.
            controls.ctcss_inhibit |= ctcss_enabled == 0 && !controls.external_ptt_request;
            controls.external_ptt_request |= keyed != 0;
        }
        let remote = context.hardware.remote_control();
        if remote.enabled && !remote.transmit_permitted {
            mono.fill(0.0);
        }
        station.stage_direct(mono);
        // The direct source is already staged for this hardware callback, so no
        // Asterisk playout admission is needed (including during signaling tails).
        controls.render_admitted = true;
    }
    context.hardware.publish_transmit_intent(
        controls.external_ptt_request,
        !controls.ctcss_inhibit,
        linked_activity,
    );
    let remote = context.hardware.remote_control();
    if remote.enabled && !remote.transmit_permitted {
        context.remote_burst_admitted = false;
        controls.external_ptt_request = false;
        controls.calibrated_test_tone = false;
        controls.forced_ctcss_tenths_hz = 0;
        controls.ctcss_inhibit = true;
        controls.render_admitted = true;
    } else if controls.external_ptt_request || !remote.enabled {
        context.remote_burst_admitted = true;
    }
    match station.radio().render(output, controls) {
        Ok(mut result) => {
            if direct_failed {
                // Advance the radio with silent, unkeyed program above, but do
                // not let its retained signaling tail reassert PTT or PCM.
                output.fill(0.0);
                context.hardware.note_transmit(true);
                return CALLBACK_FAILED;
            }
            if !context.remote_burst_admitted {
                output.fill(0.0);
                result.logical_ptt = false;
                result.selected_ctcss_tenths_hz = 0;
            }
            context.hardware.publish_transmit_result(result);
            context.hardware.note_transmit(false);
            CALLBACK_OK
        }
        Err(_) => {
            output.fill(0.0);
            if station.native {
                context.hardware.publish_transmit_fault();
            }
            context.hardware.note_transmit(true);
            CALLBACK_FAILED
        }
    }
}

fn receive_metadata(
    result: ReceiveResult,
    controller_ctcss: &[Option<CtcssTone>; CTCSS_TONE_COUNT],
    voter_reporting: bool,
    hardware: &SharedHardwareState,
) -> ReceiveMetadata {
    let decoded = result
        .ctcss_decoded
        .and_then(|tone| CtcssToneIndex::new(tone.get()));
    let controller_tone = result
        .ctcss_decoded
        .and_then(|tone| controller_ctcss[usize::from(tone.get())]);
    let voter = (voter_reporting && result.receiver_keyed && result.rssi_updated)
        .then(|| ReceiveStatus::VoterRssi(voter_rssi(result.rssi_peak)));
    ReceiveMetadata {
        qualification: ReceiveQualification {
            carrier_active: result.carrier_active,
            subaudible_active: result.subaudible_active,
            receiver_keyed: result.receiver_keyed,
            ctcss_decoded: decoded,
            dcs_valid: result.dcs_valid,
        },
        controller_ctcss: controller_tone,
        statuses: [
            hardware
                .take_transmit_ctcss()
                .map(ReceiveStatus::TransmitCtcssReady),
            voter,
        ],
    }
}

fn controller_ctcss_table(plan: &StationPlan) -> [Option<CtcssTone>; CTCSS_TONE_COUNT] {
    let mut result = [None; CTCSS_TONE_COUNT];
    for tone in &plan.configuration().station.ctcss.receive_frequencies {
        result[tone.table_index()] = CtcssTone::from_tenths_hz(tone.tenths_hz());
    }
    result
}

fn voter_rssi(value: i16) -> u16 {
    let scaled = (32_767_i32 - i32::from(value)) * 1_000 / 32_767;
    scaled.clamp(0, 1_000) as u16
}

fn pack_inputs(value: HardwareInputs) -> u32 {
    (u32::from(value.carrier) * HARDWARE_CARRIER)
        | (u32::from(value.parallel_carrier) * PARALLEL_CARRIER)
        | (u32::from(value.subaudible) * HARDWARE_SUBAUDIBLE)
        | (u32::from(value.parallel_subaudible) * PARALLEL_SUBAUDIBLE)
        | (u32::from(value.physical_ptt) * PHYSICAL_PTT)
        | (u32::from(value.cm119_gpio_mask) << CM119_GPIO_SHIFT)
        | (u32::from(value.parallel_input_mask) << PARALLEL_INPUT_SHIFT)
}

fn unpack_inputs(value: u32) -> HardwareInputs {
    HardwareInputs {
        carrier: value & HARDWARE_CARRIER != 0,
        parallel_carrier: value & PARALLEL_CARRIER != 0,
        subaudible: value & HARDWARE_SUBAUDIBLE != 0,
        parallel_subaudible: value & PARALLEL_SUBAUDIBLE != 0,
        physical_ptt: value & PHYSICAL_PTT != 0,
        cm119_gpio_mask: (value >> CM119_GPIO_SHIFT) as u8,
        parallel_input_mask: (value >> PARALLEL_INPUT_SHIFT) as u8,
    }
}

fn pack_requests(value: ControllerRequests) -> u32 {
    (u32::from(value.transmit) * EXTERNAL_PTT)
        | (u32::from(value.render_admitted) * RENDER_ADMITTED)
        | (u32::from(value.ctcss_inhibit) * CTCSS_INHIBIT)
        | (u32::from(value.calibrated_test_tone) * CALIBRATED_TEST_TONE)
        | (u32::from(value.subaudible_override) * RECEIVE_CTCSS_OVERRIDE)
        | (u32::from(value.forced_ctcss.map_or(0, CtcssTone::tenths_hz)) << FORCED_CTCSS_SHIFT)
}

fn unpack_requests(value: u32) -> ControllerRequests {
    ControllerRequests {
        transmit: value & EXTERNAL_PTT != 0,
        render_admitted: value & RENDER_ADMITTED != 0,
        ctcss_inhibit: value & CTCSS_INHIBIT != 0,
        calibrated_test_tone: value & CALIBRATED_TEST_TONE != 0,
        subaudible_override: value & RECEIVE_CTCSS_OVERRIDE != 0,
        forced_ctcss: u16::try_from(value >> FORCED_CTCSS_SHIFT)
            .ok()
            .and_then(CtcssTone::from_tenths_hz),
    }
}

fn pack_outputs(value: HardwareOutputs) -> u32 {
    (u32::from(value.logical_ptt) * LOGICAL_PTT)
        | (u32::from(value.selected_ctcss_tenths_hz.unwrap_or(0)) << OUTPUT_TONE_SHIFT)
}

fn unpack_outputs(value: u32) -> HardwareOutputs {
    let tone = (value >> OUTPUT_TONE_SHIFT) as u16;
    HardwareOutputs {
        receiver_keyed: false,
        logical_ptt: value & LOGICAL_PTT != 0,
        selected_ctcss_tenths_hz: (tone != 0).then_some(tone),
    }
}

#[cfg(test)]
#[path = "tests/runtime_tests.rs"]
pub(crate) mod tests;
