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
    AudioError, AudioProvider, AudioStream, ReceiveWorkerEndpoint, StreamStatistics, StreamTiming,
    TransmitWorkerEndpoint,
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
        TransmitControls {
            external_ptt_request: requests.transmit,
            physical_ptt_applied: inputs.physical_ptt,
            render_admitted: requests.render_admitted,
            ctcss_inhibit: requests.ctcss_inhibit,
            calibrated_test_tone: requests.calibrated_test_tone,
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
}

struct TransmitContext {
    station: StationTransmit,
    maximum_frames: usize,
    hardware: SharedHardwareState,
}

/// One stopped or running direct PortAudio station composition.
///
/// The stream is declared first so its destructor stops and destroys both
/// callbacks before their contexts or the boxed station media are released.
pub struct StationRuntime {
    stream: AudioStream<'static, 'static>,
    _receive_context: Box<UnsafeCell<ReceiveContext>>,
    _transmit_context: Box<UnsafeCell<TransmitContext>>,
    hardware: SharedHardwareState,
}

/// Serialized controller and radio-observer owner paired with one runtime.
///
/// This value is independent of [`StationRuntime`] so an Asterisk delivery
/// worker can own it without borrowing state mutated by live audio callbacks.
pub struct StationControlHost {
    plan: StationPlan,
    control: StationControl,
    controller: usbradioplus_asl3::ControllerState,
    hardware: SharedHardwareState,
}

impl StationControlHost {
    /// Return the immutable effective station plan.
    #[must_use]
    pub const fn plan(&self) -> &StationPlan {
        &self.plan
    }

    /// Borrow serialized ASL3 controller state.
    pub fn controller(&mut self) -> &mut usbradioplus_asl3::ControllerState {
        &mut self.controller
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
        let radio = media.plan.radio();
        let stream = selected.stream;
        if stream.maximum_receive_frame_count == 0
            || stream.maximum_transmit_frame_count == 0
            || stream.maximum_receive_frame_count > ADVANCED_FRAME_SAMPLES as u32
            || stream.maximum_receive_frame_count > radio.maximum_receive_frame_count
            || stream.maximum_transmit_frame_count > radio.maximum_transmit_frame_count
        {
            return Err(AudioError::InvalidArgument);
        }

        let controller_ctcss = controller_ctcss_table(&media.plan);
        let voter_reporting = media.plan.configuration().station.hardware.voter_reporting != 0;
        let hardware = SharedHardwareState::default();
        let StationMedia {
            plan,
            receive,
            transmit,
            control,
            controller,
        } = media;
        let receive_context = Box::new(UnsafeCell::new(ReceiveContext {
            station: receive,
            mono: vec![0.0; stream.maximum_receive_frame_count as usize].into_boxed_slice(),
            hardware: hardware.clone(),
            controller_ctcss,
            voter_reporting,
        }));
        let transmit_context = Box::new(UnsafeCell::new(TransmitContext {
            station: transmit,
            maximum_frames: stream.maximum_transmit_frame_count as usize,
            hardware: hardware.clone(),
        }));
        // SAFETY: both boxed UnsafeCell contexts have stable addresses and are
        // retained until the stream is stopped and destroyed. Each callback is
        // the sole serial mutator of its disjoint context.
        let receive = unsafe {
            ReceiveWorkerEndpoint::from_raw(
                receive_callback,
                NonNull::new_unchecked(receive_context.get()).cast(),
            )
        };
        // SAFETY: the transmit context is disjoint from the receive context and
        // obeys the same stream-before-context teardown contract.
        let transmit = unsafe {
            TransmitWorkerEndpoint::from_raw(
                transmit_callback,
                NonNull::new_unchecked(transmit_context.get()).cast(),
            )
        };
        let stream = provider.open_stream(stream, receive, transmit)?;
        Ok((
            Self {
                stream,
                _receive_context: receive_context,
                _transmit_context: transmit_context,
                hardware: hardware.clone(),
            },
            StationControlHost {
                plan,
                control,
                controller,
                hardware,
            },
        ))
    }

    /// Start capture and playback callbacks.
    pub fn start(&mut self) -> Result<(), AudioError> {
        self.stream.start()
    }

    /// Stop capture and playback callbacks; repeated calls are harmless.
    pub fn stop(&mut self) -> Result<(), AudioError> {
        self.stream.stop()
    }

    /// Read current audio and station callback statistics.
    pub fn statistics(&self) -> Result<StationRuntimeStatistics, AudioError> {
        Ok(StationRuntimeStatistics {
            audio: self.stream.statistics()?,
            callbacks: self.hardware.callback_statistics(),
        })
    }

    /// Read immutable PortAudio stream timing.
    pub fn timing(&self) -> Result<StreamTiming, AudioError> {
        self.stream.timing()
    }

    /// Clone the lock-free state handle used by the hardware/control owner.
    #[must_use]
    pub fn hardware_state(&self) -> SharedHardwareState {
        self.hardware.clone()
    }
}

impl Drop for StationRuntime {
    fn drop(&mut self) {
        let _ = self.stream.stop();
    }
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
        context.hardware.publish_receiver_keyed(false);
        context.hardware.note_receive(true);
        return CALLBACK_FAILED;
    }
    // SAFETY: the audio adapter supplies canonical stereo input for each frame.
    let input = unsafe { std::slice::from_raw_parts(input, frames * 2) };
    let station = &mut context.station;
    let result = match station.radio().process(
        input,
        &mut context.mono[..frames],
        context.hardware.receive_controls(),
    ) {
        Ok(result) => result,
        Err(_) => {
            context.hardware.publish_receiver_keyed(false);
            context.hardware.note_receive(true);
            return CALLBACK_FAILED;
        }
    };
    let metadata = receive_metadata(
        result,
        &context.controller_ctcss,
        context.voter_reporting,
        &context.hardware,
    );
    context
        .hardware
        .publish_receiver_keyed(result.receiver_keyed);
    context
        .hardware
        .publish_receive_clipping(result.input_rail_samples);
    context.hardware.publish_receive_observation(result);
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
        context.hardware.note_transmit(true);
        return CALLBACK_FAILED;
    }
    // SAFETY: the audio adapter supplies two writable samples for each frame.
    let output = unsafe { std::slice::from_raw_parts_mut(output, frames * 2) };
    output.fill(0.0);
    let station = &mut context.station;
    match station
        .radio()
        .render(output, context.hardware.transmit_controls())
    {
        Ok(result) => {
            context.hardware.publish_transmit_result(result);
            context.hardware.note_transmit(false);
            CALLBACK_OK
        }
        Err(_) => {
            output.fill(0.0);
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
mod tests;
