use super::*;

use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

const OK: c_int = 0;
const AUDIO_ABI: u32 = 2;
const GPIO_ABI: u32 = 1;
pub(super) const FAIL_PREFLIGHT: u32 = 1;
pub(super) const FAIL_GRAPH: u32 = 2;
pub(super) const FAIL_OPEN: u32 = 3;
pub(super) const FAIL_START: u32 = 4;
pub(super) const FAIL_STOP: u32 = 5;
pub(super) const FAIL_STATISTICS: u32 = 6;
pub(super) const FAIL_TIMING: u32 = 7;
pub(super) const FAIL_SNAPSHOT: u32 = 8;
pub(super) const FAIL_MIXER: u32 = 9;
pub(super) const FAIL_EEPROM: u32 = 10;
pub(super) const PUBLISH_ON_STOP: u32 = 11;
pub(super) const FAIL_START_ONCE: u32 = 12;
pub(super) const FAIL_OPEN_ONCE: u32 = 13;
static FAILURE: AtomicU32 = AtomicU32::new(0);
static EXCLUSIVE_AUDIO: AtomicU32 = AtomicU32::new(0);
static AUDIO_STREAMS: AtomicU32 = AtomicU32::new(0);
static AUDIO_CREATES: AtomicU32 = AtomicU32::new(0);
static AUDIO_DESTROYS: AtomicU32 = AtomicU32::new(0);

pub(super) fn exclusive_audio(enabled: bool) {
    assert_eq!(AUDIO_STREAMS.load(Ordering::Acquire), 0);
    if enabled {
        AUDIO_CREATES.store(0, Ordering::Release);
        AUDIO_DESTROYS.store(0, Ordering::Release);
    }
    EXCLUSIVE_AUDIO.store(u32::from(enabled), Ordering::Release);
}

pub(super) fn audio_streams() -> u32 {
    AUDIO_STREAMS.load(Ordering::Acquire)
}

pub(super) fn audio_lifecycle() -> (u32, u32) {
    (
        AUDIO_CREATES.load(Ordering::Acquire),
        AUDIO_DESTROYS.load(Ordering::Acquire),
    )
}

fn failed(stage: u32) -> bool {
    FAILURE.load(Ordering::Acquire) == stage
}

pub(super) fn set_failure(stage: u32) {
    FAILURE.store(stage, Ordering::Release);
}

pub(super) fn clear_failure() {
    set_failure(0);
}

pub(super) fn set_inputs(gpio: u32, parallel: u32) {
    GPIO_INPUTS.store(gpio, Ordering::Release);
    PARALLEL_INPUTS.store(parallel, Ordering::Release);
}

#[repr(C)]
struct GraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

#[repr(C)]
struct GraphDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<unsafe extern "C" fn(*const GraphConfig, *mut *mut c_void) -> c_int>,
    stream: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const f32,
            u32,
            *mut f32,
            u32,
            *mut u32,
            *mut u32,
        ) -> c_int,
    >,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    block: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for GraphDescriptor {}

struct GraphState(u32);

unsafe extern "C" fn graph_create(config: *const GraphConfig, output: *mut *mut c_void) -> c_int {
    if failed(FAIL_GRAPH) {
        return -1;
    }
    // SAFETY: the product wrapper supplies live configuration and output storage.
    unsafe { *output = Box::into_raw(Box::new(GraphState((*config).maximum_frame_count))).cast() };
    OK
}

unsafe extern "C" fn graph_block(
    handle: *mut c_void,
    input: *const f32,
    count: u32,
    output: *mut f32,
) -> c_int {
    // SAFETY: all pointers and counts originate from the product wrapper.
    unsafe {
        assert!(count <= (*handle.cast::<GraphState>()).0);
        ptr::copy_nonoverlapping(input, output, count as usize);
    }
    OK
}

unsafe extern "C" fn graph_destroy(handle: *mut c_void) {
    // SAFETY: every non-null handle came from graph_create and is consumed once.
    if !handle.is_null() {
        // SAFETY: the ownership invariant is established above.
        drop(unsafe { Box::from_raw(handle.cast::<GraphState>()) });
    }
}

static GRAPH: GraphDescriptor = GraphDescriptor {
    struct_size: size_of::<GraphDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.ffmpeg".as_ptr(),
    create: Some(graph_create),
    stream: None,
    destroy: Some(graph_destroy),
    block: Some(graph_block),
};

#[repr(C)]
struct DenoiseDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<unsafe extern "C" fn(u32, u32, u32, *mut *mut c_void) -> c_int>,
    process:
        Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, *mut f32) -> c_int>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for DenoiseDescriptor {}

unsafe extern "C" fn unit_create(
    _rate: u32,
    _channels: u32,
    _frames: u32,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies writable output storage.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    OK
}

unsafe extern "C" fn denoise_process(
    _handle: *mut c_void,
    input: *const f32,
    count: u32,
    output: *mut f32,
    probability: *mut f32,
) -> c_int {
    // SAFETY: the wrapper supplies exact input/output spans and VAD storage.
    unsafe {
        ptr::copy_nonoverlapping(input, output, count as usize);
        *probability = 0.0;
    }
    OK
}

unsafe extern "C" fn unit_destroy(handle: *mut c_void) {
    // SAFETY: every non-null handle is the unit allocation made above.
    if !handle.is_null() {
        // SAFETY: the ownership invariant is established above.
        drop(unsafe { Box::from_raw(handle.cast::<()>()) });
    }
}

static DENOISE: DenoiseDescriptor = DenoiseDescriptor {
    struct_size: size_of::<DenoiseDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.rnnoise".as_ptr(),
    create: Some(unit_create),
    process: Some(denoise_process),
    destroy: Some(unit_destroy),
};

#[repr(C)]
struct RingConfig {
    struct_size: u32,
    abi_version: u32,
    capacity_samples: u64,
    input_rate_hz: u32,
    output_rate_hz: u32,
    quality: u32,
}

#[repr(C)]
#[derive(Default)]
struct RingObservation {
    values: [u64; 12],
}

#[repr(C)]
struct RingDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<unsafe extern "C" fn(*const RingConfig, *mut *mut c_void) -> c_int>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    push_sample: Option<unsafe extern "C" fn(*mut c_void, f32, *mut bool) -> c_int>,
    push: Option<unsafe extern "C" fn(*mut c_void, *const f32, u64, *mut u64) -> c_int>,
    render_sample: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u64, *mut bool) -> c_int>,
    render: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u64, u64, u64, *mut u64) -> c_int>,
    observe: Option<unsafe extern "C" fn(*const c_void, *mut RingObservation) -> c_int>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for RingDescriptor {}

unsafe extern "C" fn ring_create(_config: *const RingConfig, output: *mut *mut c_void) -> c_int {
    // SAFETY: the wrapper supplies writable output storage.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    OK
}

unsafe extern "C" fn ring_push_sample(
    _handle: *mut c_void,
    _sample: f32,
    accepted: *mut bool,
) -> c_int {
    // SAFETY: the wrapper supplies writable result storage.
    unsafe { *accepted = true };
    OK
}

unsafe extern "C" fn ring_push(
    _handle: *mut c_void,
    _input: *const f32,
    count: u64,
    accepted: *mut u64,
) -> c_int {
    // SAFETY: the wrapper supplies writable result storage.
    unsafe { *accepted = count };
    OK
}

unsafe extern "C" fn ring_render_sample(
    _handle: *mut c_void,
    output: *mut f32,
    _target: u64,
    ready: *mut bool,
) -> c_int {
    // SAFETY: the wrapper supplies writable result storage.
    unsafe {
        *output = 0.0;
        *ready = true;
    }
    OK
}

unsafe extern "C" fn ring_render(
    _handle: *mut c_void,
    output: *mut f32,
    count: u64,
    _reserve: u64,
    _target: u64,
    rendered: *mut u64,
) -> c_int {
    // SAFETY: the wrapper supplies a writable count-sized output span.
    unsafe {
        std::slice::from_raw_parts_mut(output, count as usize).fill(0.0);
        *rendered = count;
    }
    OK
}

unsafe extern "C" fn ring_observe(_handle: *const c_void, output: *mut RingObservation) -> c_int {
    // SAFETY: the wrapper supplies writable observation storage.
    unsafe { output.write(RingObservation::default()) };
    OK
}

static RING: RingDescriptor = RingDescriptor {
    struct_size: size_of::<RingDescriptor>() as u32,
    abi_version: 2,
    capability_name: c"rptadv.rate-adjusting-pcm-ring.f32".as_ptr(),
    create: Some(ring_create),
    destroy: Some(unit_destroy),
    push_sample: Some(ring_push_sample),
    push: Some(ring_push),
    render_sample: Some(ring_render_sample),
    render: Some(ring_render),
    observe: Some(ring_observe),
};

#[repr(C)]
struct Converter(u8);

#[repr(C)]
struct ConverterDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<unsafe extern "C" fn(c_int, u32, *mut *mut Converter) -> c_int>,
    reset: Option<unsafe extern "C" fn(*mut Converter) -> c_int>,
    process: Option<
        unsafe extern "C" fn(
            *mut Converter,
            *const f32,
            u32,
            *mut f32,
            u32,
            f64,
            *mut u32,
            *mut u32,
        ) -> c_int,
    >,
    destroy: Option<unsafe extern "C" fn(*mut Converter)>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for ConverterDescriptor {}

unsafe extern "C" fn converter_create(
    _quality: c_int,
    _channels: u32,
    output: *mut *mut Converter,
) -> c_int {
    // SAFETY: the wrapper supplies writable output storage.
    unsafe { *output = Box::into_raw(Box::new(Converter(0))) };
    OK
}

unsafe extern "C" fn converter_reset(_handle: *mut Converter) -> c_int {
    OK
}

unsafe extern "C" fn converter_process(
    _handle: *mut Converter,
    input: *const f32,
    input_count: u32,
    output: *mut f32,
    output_capacity: u32,
    ratio: f64,
    input_used: *mut u32,
    output_generated: *mut u32,
) -> c_int {
    let generated = ((f64::from(input_count) * ratio).round() as u32).min(output_capacity);
    // SAFETY: the wrapper supplies exact input/output spans and progress storage.
    unsafe {
        let input = std::slice::from_raw_parts(input, input_count as usize);
        let output = std::slice::from_raw_parts_mut(output, generated as usize);
        let output_len = output.len();
        for (index, sample) in output.iter_mut().enumerate() {
            *sample = input[(index * input.len() / output_len).min(input.len() - 1)];
        }
        *input_used = input_count;
        *output_generated = generated;
    }
    OK
}

unsafe extern "C" fn converter_destroy(handle: *mut Converter) {
    // SAFETY: every non-null handle came from converter_create and is consumed once.
    if !handle.is_null() {
        // SAFETY: the ownership invariant is established above.
        drop(unsafe { Box::from_raw(handle) });
    }
}

static CONVERTER: ConverterDescriptor = ConverterDescriptor {
    struct_size: size_of::<ConverterDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.samplerate".as_ptr(),
    create: Some(converter_create),
    reset: Some(converter_reset),
    process: Some(converter_process),
    destroy: Some(converter_destroy),
};

type RadioCreate = unsafe extern "C" fn(*const c_void, *const c_void, *mut *mut c_void) -> c_int;
type RadioWarm = unsafe extern "C" fn(*mut c_void) -> c_int;
type RadioReceive = unsafe extern "C" fn(
    *mut c_void,
    *const f32,
    *mut f32,
    u32,
    *const c_void,
    *mut c_void,
) -> c_int;
type RadioTransmit =
    unsafe extern "C" fn(*mut c_void, *mut f32, u32, *const c_void, *mut c_void) -> c_int;
type RadioSnapshot = unsafe extern "C" fn(*const c_void, *mut c_void) -> c_int;
type RadioPop = unsafe extern "C" fn(*const c_void, *mut c_void) -> u32;

#[repr(C)]
struct RadioConfigPrefix {
    _struct_size: u32,
    _abi_version: u32,
    generation_id: u64,
}

struct RadioState {
    generation_id: u64,
}

#[repr(C)]
#[derive(Default)]
struct RadioReceiveResult {
    generation_id: u64,
    first_sample_index: u64,
    frame_count: u32,
    carrier_active: u32,
    subaudible_active: u32,
    receiver_keyed: u32,
    ctcss_decoded_index: i32,
    dcs_valid: u32,
    rssi_peak: i16,
    rssi_updated: u32,
    ctcss_decoder_peak: f32,
    input_peak: f32,
    input_rms: f32,
    input_rail_samples: u64,
    output_peak: f32,
    output_rms: f32,
    output_rail_samples: u64,
    periodic_status_due: u32,
}

#[repr(C)]
#[derive(Default)]
struct RadioRingObservation {
    occupancy_frames: u32,
    reserve_frames: u32,
    target_frames: u32,
    capacity_frames: u32,
    ratio: f64,
    underrun_samples: u64,
    overrun_samples: u64,
    concealment_samples: u64,
}

#[repr(C)]
#[derive(Default)]
struct RadioTransmitResult {
    generation_id: u64,
    first_sample_index: u64,
    frame_count: u32,
    logical_ptt: u32,
    transmitter_state: i32,
    selected_ctcss_tenths_hz: i32,
    program_peak: f32,
    program_rms: f32,
    program_rail_samples: u64,
    output_peak: f32,
    output_rms: f32,
    output_rail_samples: u64,
    periodic_status_due: u32,
    program_ring: RadioRingObservation,
}

#[repr(C)]
#[derive(Default)]
struct RadioSnapshotValue {
    generation_id: u64,
    receive_frames: u64,
    transmit_frames: u64,
    receive_input_peak: f32,
    receive_input_rms: f32,
    receive_ctcss_decoder_peak: f32,
    receive_output_peak: f32,
    receive_output_rms: f32,
    transmit_program_peak: f32,
    transmit_program_rms: f32,
    transmit_output_peak: f32,
    transmit_output_rms: f32,
    receive_input_rail_samples: u64,
    receive_output_rail_samples: u64,
    transmit_program_rail_samples: u64,
    transmit_output_rail_samples: u64,
    provider_failures: u64,
    receive_event_drops: u64,
    transmit_event_drops: u64,
    carrier_active: u32,
    subaudible_active: u32,
    receiver_keyed: u32,
    logical_ptt: u32,
    ctcss_decoded_index: i32,
    dcs_valid: u32,
    program_ring: RadioRingObservation,
}

#[repr(C)]
struct RadioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<RadioCreate>,
    warm: Option<RadioWarm>,
    receive: Option<RadioReceive>,
    transmit: Option<RadioTransmit>,
    snapshot: Option<RadioSnapshot>,
    pop_receive: Option<RadioPop>,
    pop_transmit: Option<RadioPop>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for RadioDescriptor {}

unsafe extern "C" fn radio_create(
    config: *const c_void,
    _ports: *const c_void,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies one complete configuration prefix and output storage.
    unsafe {
        *output = Box::into_raw(Box::new(RadioState {
            generation_id: (*config.cast::<RadioConfigPrefix>()).generation_id,
        }))
        .cast()
    };
    OK
}

unsafe extern "C" fn radio_warm(_handle: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn radio_receive(
    handle: *mut c_void,
    _input: *const f32,
    output: *mut f32,
    frames: u32,
    _controls: *const c_void,
    result: *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies exact output and result storage.
    unsafe {
        std::slice::from_raw_parts_mut(output, frames as usize).fill(0.25);
        result
            .cast::<RadioReceiveResult>()
            .write(RadioReceiveResult {
                generation_id: (*handle.cast::<RadioState>()).generation_id,
                frame_count: frames,
                carrier_active: 1,
                subaudible_active: 1,
                receiver_keyed: 1,
                ctcss_decoded_index: -1,
                rssi_peak: 123,
                rssi_updated: 1,
                ctcss_decoder_peak: 0.1,
                input_peak: 0.5,
                input_rms: 0.25,
                output_peak: 0.5,
                output_rms: 0.25,
                ..RadioReceiveResult::default()
            })
    };
    OK
}

unsafe extern "C" fn radio_transmit(
    handle: *mut c_void,
    output: *mut f32,
    frames: u32,
    _controls: *const c_void,
    result: *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies exact stereo output and result storage.
    unsafe {
        std::slice::from_raw_parts_mut(output, frames as usize * 2).fill(0.125);
        result
            .cast::<RadioTransmitResult>()
            .write(RadioTransmitResult {
                generation_id: (*handle.cast::<RadioState>()).generation_id,
                frame_count: frames,
                logical_ptt: 1,
                transmitter_state: 1,
                program_peak: 0.25,
                program_rms: 0.125,
                output_peak: 0.25,
                output_rms: 0.125,
                program_ring: RadioRingObservation {
                    occupancy_frames: 12,
                    reserve_frames: 2,
                    target_frames: 6,
                    capacity_frames: 24,
                    ratio: 1.0,
                    ..RadioRingObservation::default()
                },
                ..RadioTransmitResult::default()
            })
    };
    OK
}

unsafe extern "C" fn radio_snapshot(handle: *const c_void, output: *mut c_void) -> c_int {
    if failed(FAIL_SNAPSHOT) {
        return -1;
    }
    // SAFETY: the wrapper supplies a live handle and writable snapshot storage.
    unsafe {
        output
            .cast::<RadioSnapshotValue>()
            .write(RadioSnapshotValue {
                generation_id: (*handle.cast::<RadioState>()).generation_id,
                receive_frames: 960,
                transmit_frames: 960,
                receive_input_peak: 0.5,
                receive_input_rms: 0.25,
                receive_ctcss_decoder_peak: 0.1,
                receive_output_peak: 0.4,
                receive_output_rms: 0.2,
                transmit_program_peak: 0.3,
                transmit_program_rms: 0.15,
                transmit_output_peak: 0.25,
                transmit_output_rms: 0.125,
                carrier_active: 1,
                subaudible_active: 1,
                receiver_keyed: 1,
                logical_ptt: 1,
                ctcss_decoded_index: -1,
                program_ring: RadioRingObservation {
                    occupancy_frames: 12,
                    reserve_frames: 2,
                    target_frames: 6,
                    capacity_frames: 24,
                    ratio: 1.0,
                    ..RadioRingObservation::default()
                },
                ..RadioSnapshotValue::default()
            })
    };
    OK
}

unsafe extern "C" fn radio_pop(_handle: *const c_void, _output: *mut c_void) -> u32 {
    0
}

unsafe extern "C" fn radio_destroy(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: every non-null handle was allocated by radio_create.
        drop(unsafe { Box::from_raw(handle.cast::<RadioState>()) });
    }
}

static RADIO: RadioDescriptor = RadioDescriptor {
    struct_size: size_of::<RadioDescriptor>() as u32,
    abi_version: 4,
    capability_name: c"rptadv.radio-core".as_ptr(),
    create: Some(radio_create),
    warm: Some(radio_warm),
    receive: Some(radio_receive),
    transmit: Some(radio_transmit),
    snapshot: Some(radio_snapshot),
    pop_receive: Some(radio_pop),
    pop_transmit: Some(radio_pop),
    destroy: Some(radio_destroy),
};

#[repr(C)]
struct AudioStreamConfig {
    struct_size: u32,
    abi_version: u32,
    native_sample_rate_hz: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
    input_device_index: i32,
    output_device_index: i32,
    input_device_channels: u32,
    output_device_channels: u32,
    receive_worker: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32>,
    receive_context: *mut c_void,
    transmit_worker: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32>,
    transmit_context: *mut c_void,
}

#[repr(C)]
#[derive(Default)]
struct AudioStatistics {
    struct_size: u32,
    abi_version: u32,
    callback_count: u64,
    callback_frame_count: u64,
    oversized_callback_count: u64,
    worker_failure_count: u64,
    input_overflow_count: u64,
    output_underflow_count: u64,
    device_error_count: u64,
    input_clip_sample_count: u64,
    output_clip_sample_count: u64,
    input_peak: f32,
    input_rms: f32,
    output_peak: f32,
    output_rms: f32,
    last_portaudio_error: i32,
    callback_last_duration_ns: u64,
    callback_max_duration_ns: u64,
    callback_last_start_delay_ns: u64,
    callback_max_start_delay_ns: u64,
    callback_late_start_count: u64,
    callback_late_start_tolerance_ns: u64,
    last_input_xrun_monotonic_ns: u64,
    last_output_xrun_monotonic_ns: u64,
    callback_clock_error_count: u64,
    capture_callback_count: u64,
}

#[repr(C)]
#[derive(Default)]
struct AudioTiming {
    struct_size: u32,
    abi_version: u32,
    input_latency_seconds: f64,
    output_latency_seconds: f64,
    sample_rate_hz: f64,
}

#[repr(C)]
struct AudioMixerConfig {
    struct_size: u32,
    card: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct UsbMixerConfig {
    struct_size: u32,
    interface: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct AudioDeviceIdentity {
    struct_size: u32,
    interface: *const c_char,
    serial: *const c_char,
    input_channels: u32,
    output_channels: u32,
}

#[repr(C)]
#[derive(Default)]
struct AudioDeviceSelection {
    struct_size: u32,
    abi_version: u32,
    alsa_card_index: u32,
    input_device_index: i32,
    output_device_index: i32,
}

#[repr(C)]
struct AudioDeviceSelector {
    struct_size: u32,
    policy: u32,
    identifier: *const c_char,
    serial: *const c_char,
    input_channels: u32,
    output_channels: u32,
}

#[repr(C)]
struct AudioDeviceMatch {
    struct_size: u32,
    abi_version: u32,
    interface: [c_char; 256],
    serial: [c_char; 256],
    selection: AudioDeviceSelection,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AudioMixerPath {
    element: [c_char; 64],
    element_index: u32,
    channel: u32,
    direction: u32,
    capabilities: u32,
}

impl Default for AudioMixerPath {
    fn default() -> Self {
        Self {
            element: [0; 64],
            element_index: 0,
            channel: 0,
            direction: 0,
            capabilities: 0,
        }
    }
}

#[repr(C)]
#[derive(Default)]
struct AudioMixerPaths {
    struct_size: u32,
    abi_version: u32,
    receive_count: u32,
    transmit_count: u32,
    sidetone_count: u32,
    compatibility_count: u32,
    receive: [AudioMixerPath; 2],
    transmit: [AudioMixerPath; 2],
    sidetone: [AudioMixerPath; 2],
    compatibility: [AudioMixerPath; 2],
}

#[repr(C)]
struct AudioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability: *const c_char,
    stream_create:
        Option<unsafe extern "C" fn(*const AudioStreamConfig, *mut *mut c_void) -> c_int>,
    stream_start: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    stream_stop: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    stream_statistics: Option<unsafe extern "C" fn(*const c_void, *mut AudioStatistics) -> c_int>,
    stream_destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    mixer_create: Option<unsafe extern "C" fn(*const AudioMixerConfig, *mut *mut c_void) -> c_int>,
    mixer_range_centibels: Option<unsafe extern "C" fn(*const c_void, *mut i64, *mut i64) -> c_int>,
    mixer_get_centibels: Option<unsafe extern "C" fn(*const c_void, *mut i64) -> c_int>,
    mixer_set_centibels: Option<unsafe extern "C" fn(*mut c_void, i64) -> c_int>,
    mixer_destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    mixer_usb_create:
        Option<unsafe extern "C" fn(*const UsbMixerConfig, *mut *mut c_void) -> c_int>,
    mixer_range_steps: Option<unsafe extern "C" fn(*const c_void, *mut i64, *mut i64) -> c_int>,
    mixer_get_steps: Option<unsafe extern "C" fn(*const c_void, *mut i64) -> c_int>,
    mixer_set_steps: Option<unsafe extern "C" fn(*mut c_void, i64) -> c_int>,
    mixer_get_normalized: Option<unsafe extern "C" fn(*const c_void, *mut u32) -> c_int>,
    mixer_set_normalized: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    mixer_get_switch: Option<unsafe extern "C" fn(*const c_void, *mut u32) -> c_int>,
    mixer_set_switch: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    device_resolve: Option<
        unsafe extern "C" fn(*const AudioDeviceIdentity, *mut AudioDeviceSelection) -> c_int,
    >,
    device_select:
        Option<unsafe extern "C" fn(*const AudioDeviceSelector, *mut AudioDeviceMatch) -> c_int>,
    stream_timing: Option<unsafe extern "C" fn(*const c_void, *mut AudioTiming) -> c_int>,
    mixer_paths: Option<unsafe extern "C" fn(*const c_char, *mut AudioMixerPaths) -> c_int>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for AudioDescriptor {}

struct FakeStream {
    exclusive: bool,
    receive: unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32,
    receive_context: *mut c_void,
    transmit: unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32,
    transmit_context: *mut c_void,
}

struct FakeMixer {
    normalized: u32,
    enabled: u32,
}

unsafe extern "C" fn audio_stream_create(
    config: *const AudioStreamConfig,
    output: *mut *mut c_void,
) -> c_int {
    if failed(FAIL_OPEN_ONCE) {
        clear_failure();
        return -1;
    }
    if failed(FAIL_OPEN) {
        return -1;
    }
    let exclusive = EXCLUSIVE_AUDIO.load(Ordering::Acquire) != 0;
    if exclusive && AUDIO_STREAMS.swap(1, Ordering::AcqRel) != 0 {
        return -6;
    }
    if exclusive {
        AUDIO_CREATES.fetch_add(1, Ordering::AcqRel);
    }
    // SAFETY: the validated wrapper supplies complete live objects.
    let (config, output) = unsafe { (&*config, &mut *output) };
    *output = Box::into_raw(Box::new(FakeStream {
        exclusive,
        receive: config.receive_worker.unwrap(),
        receive_context: config.receive_context,
        transmit: config.transmit_worker.unwrap(),
        transmit_context: config.transmit_context,
    }))
    .cast();
    OK
}

unsafe extern "C" fn audio_stream_start(stream: *mut c_void) -> c_int {
    if failed(FAIL_START_ONCE) {
        clear_failure();
        return -1;
    }
    if failed(FAIL_START) {
        return -1;
    }
    // SAFETY: the handle originates from audio_stream_create.
    let stream = unsafe { &mut *stream.cast::<FakeStream>() };
    let input = [0.25_f32; 1_920];
    let mut output = [0.0_f32; 1_920];
    // SAFETY: contexts and canonical 20 ms buffers are live for these calls.
    let receive = unsafe { (stream.receive)(stream.receive_context, input.as_ptr(), 960) };
    // SAFETY: contexts and canonical 20 ms buffers are live for these calls.
    let transmit = unsafe { (stream.transmit)(stream.transmit_context, output.as_mut_ptr(), 960) };
    if receive == 0 && transmit == 0 {
        OK
    } else {
        -1
    }
}

unsafe extern "C" fn audio_stream_control(stream: *mut c_void) -> c_int {
    if failed(FAIL_STOP) {
        return -1;
    }
    if failed(PUBLISH_ON_STOP) {
        // SAFETY: the handle originates from audio_stream_create and its receive context is live.
        let stream = unsafe { &mut *stream.cast::<FakeStream>() };
        let input = [0.25_f32; 1_920];
        // SAFETY: the receive context and canonical input are live for this call.
        let _ = unsafe { (stream.receive)(stream.receive_context, input.as_ptr(), 960) };
    }
    OK
}

unsafe extern "C" fn audio_stream_statistics(
    _stream: *const c_void,
    output: *mut AudioStatistics,
) -> c_int {
    if failed(FAIL_STATISTICS) {
        return -1;
    }
    // SAFETY: the wrapper supplies complete writable storage.
    unsafe {
        (*output).abi_version = AUDIO_ABI;
        (*output).callback_count = 2;
    }
    OK
}

unsafe extern "C" fn audio_stream_timing(
    _stream: *const c_void,
    output: *mut AudioTiming,
) -> c_int {
    if failed(FAIL_TIMING) {
        return -1;
    }
    // SAFETY: the wrapper supplies complete writable storage.
    unsafe {
        (*output).abi_version = AUDIO_ABI;
        (*output).input_latency_seconds = 0.01;
        (*output).output_latency_seconds = 0.02;
        (*output).sample_rate_hz = 48_000.0;
    }
    OK
}

unsafe extern "C" fn audio_stream_destroy(stream: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by create.
    let stream = unsafe { Box::from_raw(stream.cast::<FakeStream>()) };
    if stream.exclusive {
        AUDIO_STREAMS.fetch_sub(1, Ordering::AcqRel);
        AUDIO_DESTROYS.fetch_add(1, Ordering::AcqRel);
    }
    drop(stream);
}

unsafe extern "C" fn unused_audio_mixer_create(
    _config: *const AudioMixerConfig,
    _output: *mut *mut c_void,
) -> c_int {
    -1
}

unsafe extern "C" fn audio_mixer_create(
    _config: *const UsbMixerConfig,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies writable handle storage.
    unsafe {
        *output = Box::into_raw(Box::new(FakeMixer {
            normalized: 500,
            enabled: 0,
        }))
        .cast();
    }
    OK
}

unsafe extern "C" fn audio_mixer_range(
    _mixer: *const c_void,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: the wrapper supplies writable range fields.
    unsafe {
        *minimum = 0;
        *maximum = 2_000;
    }
    OK
}

unsafe extern "C" fn audio_mixer_get_i64(mixer: *const c_void, value: *mut i64) -> c_int {
    if failed(FAIL_MIXER) {
        return -1;
    }
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = i64::from((*mixer.cast::<FakeMixer>()).normalized) * 2 };
    OK
}

unsafe extern "C" fn audio_mixer_set_i64(mixer: *mut c_void, value: i64) -> c_int {
    if failed(FAIL_MIXER) {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).normalized = (value / 2) as u32 };
    OK
}

unsafe extern "C" fn audio_mixer_get(mixer: *const c_void, value: *mut u32) -> c_int {
    if failed(FAIL_MIXER) {
        return -1;
    }
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).normalized };
    OK
}

unsafe extern "C" fn audio_mixer_set(mixer: *mut c_void, value: u32) -> c_int {
    if failed(FAIL_MIXER) {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).normalized = value };
    OK
}

unsafe extern "C" fn audio_mixer_get_switch(mixer: *const c_void, value: *mut u32) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).enabled };
    OK
}

unsafe extern "C" fn audio_mixer_set_switch(mixer: *mut c_void, value: u32) -> c_int {
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).enabled = value };
    OK
}

unsafe extern "C" fn audio_mixer_destroy(mixer: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by create.
    drop(unsafe { Box::from_raw(mixer.cast::<FakeMixer>()) });
}

unsafe extern "C" fn audio_device_resolve(
    _identity: *const AudioDeviceIdentity,
    _selection: *mut AudioDeviceSelection,
) -> c_int {
    -1
}

unsafe extern "C" fn audio_device_select(
    _selector: *const AudioDeviceSelector,
    output: *mut AudioDeviceMatch,
) -> c_int {
    if failed(FAIL_PREFLIGHT) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    copy_c_string(&mut output.interface, b"3-1:1.0");
    copy_c_string(&mut output.serial, b"ABC");
    output.selection = AudioDeviceSelection {
        struct_size: size_of::<AudioDeviceSelection>() as u32,
        abi_version: AUDIO_ABI,
        alsa_card_index: 1,
        input_device_index: 2,
        output_device_index: 3,
    };
    OK
}

unsafe extern "C" fn audio_mixer_paths(
    _interface: *const c_char,
    output: *mut AudioMixerPaths,
) -> c_int {
    // SAFETY: the wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    output.receive_count = 1;
    output.transmit_count = 2;
    output.sidetone_count = 1;
    output.compatibility_count = 1;
    set_mixer_path(&mut output.receive[0], b"rx", 0, 3);
    set_mixer_path(&mut output.transmit[0], b"tx-a", 1, 3);
    set_mixer_path(&mut output.transmit[1], b"tx-b", 1, 3);
    set_mixer_path(&mut output.sidetone[0], b"side", 1, 3);
    set_mixer_path(&mut output.compatibility[0], b"compat", 1, 2);
    OK
}

fn set_mixer_path(path: &mut AudioMixerPath, name: &[u8], direction: u32, capabilities: u32) {
    copy_c_string(&mut path.element, name);
    path.direction = direction;
    path.capabilities = capabilities;
}

fn copy_c_string<const N: usize>(output: &mut [c_char; N], value: &[u8]) {
    for (destination, source) in output.iter_mut().zip(value) {
        *destination = *source as c_char;
    }
}

static AUDIO: AudioDescriptor = AudioDescriptor {
    struct_size: size_of::<AudioDescriptor>() as u32,
    abi_version: AUDIO_ABI,
    capability: c"rptadv.portaudio-alsa-audio".as_ptr(),
    stream_create: Some(audio_stream_create),
    stream_start: Some(audio_stream_start),
    stream_stop: Some(audio_stream_control),
    stream_statistics: Some(audio_stream_statistics),
    stream_destroy: Some(audio_stream_destroy),
    mixer_create: Some(unused_audio_mixer_create),
    mixer_range_centibels: Some(audio_mixer_range),
    mixer_get_centibels: Some(audio_mixer_get_i64),
    mixer_set_centibels: Some(audio_mixer_set_i64),
    mixer_destroy: Some(audio_mixer_destroy),
    mixer_usb_create: Some(audio_mixer_create),
    mixer_range_steps: Some(audio_mixer_range),
    mixer_get_steps: Some(audio_mixer_get_i64),
    mixer_set_steps: Some(audio_mixer_set_i64),
    mixer_get_normalized: Some(audio_mixer_get),
    mixer_set_normalized: Some(audio_mixer_set),
    mixer_get_switch: Some(audio_mixer_get_switch),
    mixer_set_switch: Some(audio_mixer_set_switch),
    device_resolve: Some(audio_device_resolve),
    device_select: Some(audio_device_select),
    stream_timing: Some(audio_stream_timing),
    mixer_paths: Some(audio_mixer_paths),
};

static GPIO_INPUTS: AtomicU32 = AtomicU32::new(0);
static PARALLEL_INPUTS: AtomicU32 = AtomicU32::new(0);
static PTT: AtomicU32 = AtomicU32::new(0);

#[repr(C)]
struct GpioCm119Outputs {
    struct_size: u32,
    abi_version: u32,
    ptt_asserted: u32,
    gpio_output_mask: u32,
}

#[repr(C)]
struct GpioCm119Pulse {
    struct_size: u32,
    abi_version: u32,
    ptt_invert: u32,
    gpio_invert_mask: u32,
    duration_ms: u32,
    ptt_cancel: u32,
    gpio_cancel_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioCm119Inputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    cor_active: u32,
    ctcss_active: u32,
    gpio_input_mask: u32,
    hid_report: [u8; 4],
}

#[repr(C)]
#[derive(Default)]
struct GpioCm119Statistics {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    usb_error_count: u64,
    ptt_applied: u32,
    online: u32,
    last_usb_error: i32,
    eeprom_read_count: u64,
    eeprom_write_count: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct GpioEeprom {
    struct_size: u32,
    abi_version: u32,
    checksum_valid: u32,
    magic_valid: u32,
    words: [u16; 64],
}

#[repr(C)]
struct GpioParallelOutputs {
    struct_size: u32,
    abi_version: u32,
    output_mask: u32,
    pulse_mask: u32,
    duration_ms: u32,
    cancel_pulse: u32,
}

#[repr(C)]
struct GpioParallelPulse {
    struct_size: u32,
    abi_version: u32,
    invert_mask: u32,
    duration_ms: u32,
    cancel_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioParallelInputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    status_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioParallelStatistics {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    io_error_count: u64,
    online: u32,
    last_io_error: i32,
    applied_output_mask: u32,
}

type GpioOpen = unsafe extern "C" fn(*const c_void, *mut *mut c_void) -> c_int;
type GpioPublish = unsafe extern "C" fn(*mut c_void, *const GpioCm119Outputs) -> c_int;
type GpioControl = unsafe extern "C" fn(*mut c_void) -> c_int;
type GpioInputs = unsafe extern "C" fn(*const c_void, *mut GpioCm119Inputs) -> c_int;
type GpioStatistics = unsafe extern "C" fn(*const c_void, *mut GpioCm119Statistics) -> c_int;
type GpioEepromIo = unsafe extern "C" fn(*mut c_void, *mut GpioEeprom) -> c_int;
type ParallelPublish = unsafe extern "C" fn(*mut c_void, *const GpioParallelOutputs) -> c_int;
type RawParallelInputs = unsafe extern "C" fn(*const c_void, *mut GpioParallelInputs) -> c_int;
type ParallelStatistics = unsafe extern "C" fn(*const c_void, *mut GpioParallelStatistics) -> c_int;

#[repr(C)]
struct GpioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability: *const c_char,
    probe: Option<unsafe extern "C" fn(*const c_void, *mut c_void) -> c_int>,
    open: Option<GpioOpen>,
    publish: Option<GpioPublish>,
    service: Option<GpioControl>,
    inputs: Option<GpioInputs>,
    statistics: Option<GpioStatistics>,
    close: Option<unsafe extern "C" fn(*mut c_void)>,
    discover: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    read_eeprom: Option<GpioEepromIo>,
    write_eeprom: Option<GpioEepromIo>,
    parallel_open: Option<GpioOpen>,
    parallel_publish: Option<ParallelPublish>,
    parallel_service: Option<GpioControl>,
    parallel_write: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    parallel_inputs: Option<RawParallelInputs>,
    parallel_statistics: Option<ParallelStatistics>,
    parallel_close: Option<unsafe extern "C" fn(*mut c_void)>,
    old_gpio_pulse: Option<unsafe extern "C" fn() -> c_int>,
    old_parallel_pulse: Option<unsafe extern "C" fn() -> c_int>,
    gpio_pulse: Option<unsafe extern "C" fn(*mut c_void, *const GpioCm119Pulse) -> c_int>,
    parallel_pulse: Option<unsafe extern "C" fn(*mut c_void, *const GpioParallelPulse) -> c_int>,
    set_channel: Option<unsafe extern "C" fn(*mut c_void, u8) -> c_int>,
    program_rtx: Option<unsafe extern "C" fn(*mut c_void, u32, u32, u32, u32) -> c_int>,
    clear_rtx: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
}

// SAFETY: the immutable descriptor contains only function pointers.
unsafe impl Sync for GpioDescriptor {}

unsafe extern "C" fn gpio_probe(_config: *const c_void, _output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_discover(_output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_open(_config: *const c_void, output: *mut *mut c_void) -> c_int {
    // SAFETY: the wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(0_u8)).cast() };
    OK
}

unsafe extern "C" fn gpio_publish(_device: *mut c_void, outputs: *const GpioCm119Outputs) -> c_int {
    // SAFETY: the wrapper supplies a complete output snapshot.
    PTT.store(unsafe { (*outputs).ptt_asserted }, Ordering::Release);
    OK
}

unsafe extern "C" fn gpio_service(_device: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_inputs(_device: *const c_void, output: *mut GpioCm119Inputs) -> c_int {
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioCm119Inputs {
            struct_size: size_of::<GpioCm119Inputs>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            cor_active: 1,
            ctcss_active: 1,
            gpio_input_mask: GPIO_INPUTS.load(Ordering::Acquire),
            hid_report: [0; 4],
        };
    }
    OK
}

unsafe extern "C" fn gpio_statistics(
    _device: *const c_void,
    output: *mut GpioCm119Statistics,
) -> c_int {
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioCm119Statistics {
            struct_size: size_of::<GpioCm119Statistics>() as u32,
            abi_version: GPIO_ABI,
            input_read_count: 1,
            output_apply_count: 1,
            ptt_applied: PTT.load(Ordering::Acquire),
            online: 1,
            ..GpioCm119Statistics::default()
        };
    }
    OK
}

unsafe extern "C" fn gpio_close(device: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by open.
    drop(unsafe { Box::from_raw(device.cast::<u8>()) });
}

unsafe extern "C" fn gpio_read_eeprom(_device: *mut c_void, image: *mut GpioEeprom) -> c_int {
    if failed(FAIL_EEPROM) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable image storage.
    unsafe {
        (*image).struct_size = size_of::<GpioEeprom>() as u32;
        (*image).abi_version = GPIO_ABI;
        (*image).checksum_valid = 1;
        (*image).magic_valid = 1;
        (*image).words[52] = 321;
        (*image).words[53] = 411;
        (*image).words[54] = 512;
        (*image).words[59] = 63;
        (*image).words[60] = 654;
    }
    OK
}

unsafe extern "C" fn gpio_write_eeprom(_device: *mut c_void, image: *mut GpioEeprom) -> c_int {
    if failed(FAIL_EEPROM) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable image storage.
    unsafe {
        (*image).checksum_valid = 1;
        (*image).magic_valid = 1;
    }
    OK
}

unsafe extern "C" fn gpio_pulse(_device: *mut c_void, _pulse: *const GpioCm119Pulse) -> c_int {
    OK
}

unsafe extern "C" fn parallel_publish(
    _device: *mut c_void,
    _outputs: *const GpioParallelOutputs,
) -> c_int {
    OK
}

unsafe extern "C" fn parallel_inputs(
    _device: *const c_void,
    output: *mut GpioParallelInputs,
) -> c_int {
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioParallelInputs {
            struct_size: size_of::<GpioParallelInputs>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            status_mask: PARALLEL_INPUTS.load(Ordering::Acquire),
        };
    }
    OK
}

unsafe extern "C" fn parallel_statistics(
    _device: *const c_void,
    output: *mut GpioParallelStatistics,
) -> c_int {
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioParallelStatistics {
            struct_size: size_of::<GpioParallelStatistics>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            ..GpioParallelStatistics::default()
        };
    }
    OK
}

unsafe extern "C" fn parallel_write(_device: *mut c_void, _value: u32) -> c_int {
    OK
}

unsafe extern "C" fn parallel_pulse(
    _device: *mut c_void,
    _pulse: *const GpioParallelPulse,
) -> c_int {
    OK
}

unsafe extern "C" fn parallel_set_channel(_device: *mut c_void, _channel: u8) -> c_int {
    OK
}

unsafe extern "C" fn parallel_program_rtx(
    _device: *mut c_void,
    _receive_hz: u32,
    _transmit_hz: u32,
    _transmitting: u32,
    _high_power: u32,
) -> c_int {
    OK
}

unsafe extern "C" fn parallel_control(_device: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn unused_gpio_pulse() -> c_int {
    OK
}

static GPIO: GpioDescriptor = GpioDescriptor {
    struct_size: size_of::<GpioDescriptor>() as u32,
    abi_version: GPIO_ABI,
    capability: c"rptadv.cm119-hid-gpio".as_ptr(),
    probe: Some(gpio_probe),
    open: Some(gpio_open),
    publish: Some(gpio_publish),
    service: Some(gpio_service),
    inputs: Some(gpio_inputs),
    statistics: Some(gpio_statistics),
    close: Some(gpio_close),
    discover: Some(gpio_discover),
    read_eeprom: Some(gpio_read_eeprom),
    write_eeprom: Some(gpio_write_eeprom),
    parallel_open: Some(gpio_open),
    parallel_publish: Some(parallel_publish),
    parallel_service: Some(gpio_service),
    parallel_write: Some(parallel_write),
    parallel_inputs: Some(parallel_inputs),
    parallel_statistics: Some(parallel_statistics),
    parallel_close: Some(gpio_close),
    old_gpio_pulse: Some(unused_gpio_pulse),
    old_parallel_pulse: Some(unused_gpio_pulse),
    gpio_pulse: Some(gpio_pulse),
    parallel_pulse: Some(parallel_pulse),
    set_channel: Some(parallel_set_channel),
    program_rtx: Some(parallel_program_rtx),
    clear_rtx: Some(parallel_control),
};

pub(super) fn manifest() -> UrpAstProviderManifest {
    UrpAstProviderManifest {
        struct_size: size_of::<UrpAstProviderManifest>() as u32,
        abi_version: ABI_VERSION,
        ffmpeg: ptr::from_ref(&GRAPH).cast(),
        rnnoise: ptr::from_ref(&DENOISE).cast(),
        ring: ptr::from_ref(&RING).cast(),
        radio: ptr::from_ref(&RADIO).cast(),
        samplerate: ptr::from_ref(&CONVERTER).cast(),
        audio: ptr::from_ref(&AUDIO).cast(),
        gpio: ptr::from_ref(&GPIO).cast(),
    }
}
