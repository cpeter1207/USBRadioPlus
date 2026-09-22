use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;

use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_radio::RadioProvider;
use usbradioplus_ring::RingProvider;
use usbradioplus_rnnoise::DenoiseProvider;
use usbradioplus_samplerate::SampleRateAdapter;

use crate::StationProviders;

const OK: c_int = 0;
const GRAPH_WARMUP_BLOCKS: usize = 8;

#[repr(C)]
struct GraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

type GraphCreate = unsafe extern "C" fn(*const GraphConfig, *mut *mut c_void) -> c_int;
type GraphStream =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int;
type GraphBlock = unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct GraphDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<GraphCreate>,
    stream: Option<GraphStream>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    block: Option<GraphBlock>,
}

// SAFETY: Test descriptors and every referenced function are static.
unsafe impl Sync for GraphDescriptor {}

struct GraphState {
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    calls: usize,
}

unsafe extern "C" fn graph_create(config: *const GraphConfig, output: *mut *mut c_void) -> c_int {
    // SAFETY: The wrapper supplies one live configuration and handle destination.
    unsafe {
        *output = Box::into_raw(Box::new(GraphState {
            sample_rate_hz: (*config).sample_rate_hz,
            maximum_frame_count: (*config).maximum_frame_count,
            calls: 0,
        }))
        .cast()
    };
    OK
}

unsafe extern "C" fn graph_create_fails(
    _config: *const GraphConfig,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe {
        *output = Box::into_raw(Box::new(GraphState {
            sample_rate_hz: 48_000,
            maximum_frame_count: 1,
            calls: 0,
        }))
        .cast()
    };
    -1
}

unsafe extern "C" fn graph_block(
    handle: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies this fake's live handle.
    let state = unsafe { &*handle.cast::<GraphState>() };
    if frame_count > state.maximum_frame_count {
        return -1;
    }
    // SAFETY: The wrapper supplies distinct exact-length PCM spans.
    let (input, output) = unsafe {
        (
            std::slice::from_raw_parts(input, frame_count as usize),
            std::slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    let gain = state.sample_rate_hz as f32 / 16_000.0;
    for (output, input) in output.iter_mut().zip(input) {
        *output = *input * gain;
    }
    OK
}

unsafe extern "C" fn graph_block_fails(
    _handle: *mut c_void,
    _input: *const f32,
    _frame_count: u32,
    _output: *mut f32,
) -> c_int {
    -1
}

unsafe extern "C" fn graph_block_fails_after_warmup(
    handle: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies this fake's live handle.
    let state = unsafe { &mut *handle.cast::<GraphState>() };
    if state.calls == GRAPH_WARMUP_BLOCKS {
        return -1;
    }
    state.calls += 1;
    // SAFETY: Forward the same valid call to the ordinary fake graph.
    unsafe { graph_block(handle, input, frame_count, output) }
}

unsafe extern "C" fn graph_destroy(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: Every non-null graph handle was allocated by graph_create.
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

static FAILING_GRAPH_CREATE: GraphDescriptor = GraphDescriptor {
    create: Some(graph_create_fails),
    ..GRAPH
};

static FAILING_GRAPH_PROCESSING: GraphDescriptor = GraphDescriptor {
    block: Some(graph_block_fails),
    ..GRAPH
};

static FAILING_GRAPH_LIVE: GraphDescriptor = GraphDescriptor {
    block: Some(graph_block_fails_after_warmup),
    ..GRAPH
};

fn graph_provider_from(descriptor: &'static GraphDescriptor) -> GraphProvider {
    // SAFETY: The descriptor and every referenced function have process lifetime.
    unsafe { GraphProvider::from_raw_descriptor(ptr::from_ref(descriptor).cast()) }.unwrap()
}

pub(crate) fn graph_provider() -> GraphProvider {
    graph_provider_from(&GRAPH)
}

pub(crate) fn failing_graph_create_provider() -> GraphProvider {
    graph_provider_from(&FAILING_GRAPH_CREATE)
}

pub(crate) fn processing_failing_graph_provider() -> GraphProvider {
    graph_provider_from(&FAILING_GRAPH_PROCESSING)
}

pub(crate) fn live_failing_graph_provider() -> GraphProvider {
    graph_provider_from(&FAILING_GRAPH_LIVE)
}

type DenoiseCreate = unsafe extern "C" fn(u32, u32, u32, *mut *mut c_void) -> c_int;
type DenoiseProcess =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, *mut f32) -> c_int;

#[repr(C)]
struct DenoiseDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<DenoiseCreate>,
    process: Option<DenoiseProcess>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: The descriptor and every referenced function are static.
unsafe impl Sync for DenoiseDescriptor {}

unsafe extern "C" fn denoise_create(
    _rate: u32,
    _channels: u32,
    _frames: u32,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    OK
}

unsafe extern "C" fn denoise_process(
    _handle: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
    probability: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies exact PCM spans and VAD storage.
    unsafe {
        ptr::copy_nonoverlapping(input, output, frame_count as usize);
        *probability = 0.0;
    }
    OK
}

unsafe extern "C" fn destroy_unit(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: Each non-null handle is one Box<()> allocation.
        drop(unsafe { Box::from_raw(handle.cast::<()>()) });
    }
}

static DENOISE: DenoiseDescriptor = DenoiseDescriptor {
    struct_size: size_of::<DenoiseDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.rnnoise".as_ptr(),
    create: Some(denoise_create),
    process: Some(denoise_process),
    destroy: Some(destroy_unit),
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
struct RingObservation([u64; 12]);

type RingCreate = unsafe extern "C" fn(*const RingConfig, *mut *mut c_void) -> c_int;
type RingPushSample = unsafe extern "C" fn(*mut c_void, f32, *mut bool) -> c_int;
type RingPush = unsafe extern "C" fn(*mut c_void, *const f32, u64, *mut u64) -> c_int;
type RingRenderSample = unsafe extern "C" fn(*mut c_void, *mut f32, u64, *mut bool) -> c_int;
type RingRender = unsafe extern "C" fn(*mut c_void, *mut f32, u64, u64, u64, *mut u64) -> c_int;
type RingReset = unsafe extern "C" fn(*mut c_void) -> c_int;
type RingObserve = unsafe extern "C" fn(*const c_void, *mut RingObservation) -> c_int;

#[repr(C)]
struct RingDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<RingCreate>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    push_sample: Option<RingPushSample>,
    push: Option<RingPush>,
    render_sample: Option<RingRenderSample>,
    render: Option<RingRender>,
    reset: Option<RingReset>,
    observe: Option<RingObserve>,
}

// SAFETY: The descriptor and every referenced function are static.
unsafe impl Sync for RingDescriptor {}

unsafe extern "C" fn ring_create(_config: *const RingConfig, output: *mut *mut c_void) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    OK
}

unsafe extern "C" fn ring_push_sample(
    _handle: *mut c_void,
    _sample: f32,
    _accepted: *mut bool,
) -> c_int {
    OK
}

unsafe extern "C" fn ring_push(
    _handle: *mut c_void,
    _input: *const f32,
    _count: u64,
    _accepted: *mut u64,
) -> c_int {
    OK
}

unsafe extern "C" fn ring_render_sample(
    _handle: *mut c_void,
    _output: *mut f32,
    _target: u64,
    _ready: *mut bool,
) -> c_int {
    OK
}

unsafe extern "C" fn ring_render(
    _handle: *mut c_void,
    _output: *mut f32,
    _count: u64,
    _reserve: u64,
    _target: u64,
    _rendered: *mut u64,
) -> c_int {
    OK
}

unsafe extern "C" fn ring_observe(
    _handle: *const c_void,
    _observation: *mut RingObservation,
) -> c_int {
    OK
}

unsafe extern "C" fn ring_reset(_handle: *mut c_void) -> c_int {
    OK
}

static RING: RingDescriptor = RingDescriptor {
    struct_size: size_of::<RingDescriptor>() as u32,
    abi_version: 2,
    capability_name: c"rptadv.rate-adjusting-pcm-ring.f32".as_ptr(),
    create: Some(ring_create),
    destroy: Some(destroy_unit),
    push_sample: Some(ring_push_sample),
    push: Some(ring_push),
    render_sample: Some(ring_render_sample),
    render: Some(ring_render),
    reset: Some(ring_reset),
    observe: Some(ring_observe),
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

// SAFETY: The descriptor and every referenced function are static.
unsafe impl Sync for RadioDescriptor {}

unsafe extern "C" fn radio_create(
    config: *const c_void,
    _ports: *const c_void,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies one complete configuration prefix and one
    // writable handle destination.
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
    _output: *mut f32,
    frames: u32,
    _controls: *const c_void,
    result: *mut c_void,
) -> c_int {
    // SAFETY: The fake handle and result destination originate from the radio wrapper.
    unsafe {
        result
            .cast::<RadioReceiveResult>()
            .write(RadioReceiveResult {
                generation_id: (*handle.cast::<RadioState>()).generation_id,
                frame_count: frames,
                ctcss_decoded_index: -1,
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
    _output: *mut f32,
    frames: u32,
    _controls: *const c_void,
    result: *mut c_void,
) -> c_int {
    // SAFETY: The fake handle and result destination originate from the radio wrapper.
    unsafe {
        result
            .cast::<RadioTransmitResult>()
            .write(RadioTransmitResult {
                generation_id: (*handle.cast::<RadioState>()).generation_id,
                frame_count: frames,
                transmitter_state: 0,
                program_ring: RadioRingObservation {
                    ratio: 1.0,
                    ..RadioRingObservation::default()
                },
                ..RadioTransmitResult::default()
            })
    };
    OK
}

unsafe extern "C" fn radio_destroy(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: Every non-null handle was allocated by radio_create.
        drop(unsafe { Box::from_raw(handle.cast::<RadioState>()) });
    }
}

unsafe extern "C" fn radio_snapshot(_handle: *const c_void, _output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn radio_pop(_handle: *const c_void, _output: *mut c_void) -> u32 {
    0
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
struct Converter(u8);

type ConverterCreate = unsafe extern "C" fn(c_int, u32, *mut *mut Converter) -> c_int;
type ConverterReset = unsafe extern "C" fn(*mut Converter) -> c_int;
type ConverterProcess = unsafe extern "C" fn(
    *mut Converter,
    *const f32,
    u32,
    *mut f32,
    u32,
    f64,
    *mut u32,
    *mut u32,
) -> c_int;
type ConverterDestroy = unsafe extern "C" fn(*mut Converter);

#[repr(C)]
#[derive(Clone, Copy)]
struct ConverterDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<ConverterCreate>,
    reset: Option<ConverterReset>,
    process: Option<ConverterProcess>,
    destroy: Option<ConverterDestroy>,
}

// SAFETY: Test descriptors and every referenced function are static.
unsafe impl Sync for ConverterDescriptor {}

unsafe extern "C" fn converter_create(
    _quality: c_int,
    _channels: u32,
    output: *mut *mut Converter,
) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe { *output = Box::into_raw(Box::new(Converter(0))) };
    OK
}

unsafe extern "C" fn converter_create_fails(
    _quality: c_int,
    _channels: u32,
    _output: *mut *mut Converter,
) -> c_int {
    -2
}

unsafe extern "C" fn converter_reset(_handle: *mut Converter) -> c_int {
    OK
}

unsafe extern "C" fn converter_process(
    handle: *mut Converter,
    input: *const f32,
    input_count: u32,
    output: *mut f32,
    output_count: u32,
    ratio: f64,
    input_used: *mut u32,
    output_generated: *mut u32,
) -> c_int {
    // SAFETY: The wrapper supplies this fake's live handle.
    let handle = unsafe { &mut *handle };
    if handle.0 == 1 {
        return -2;
    }
    handle.0 += 1;
    assert!((ratio - 1.0 / 6.0).abs() < f64::EPSILON);
    let generated = (input_count / 6).min(output_count);
    // SAFETY: The wrapper supplies exact readable and writable spans.
    let (input, output) = unsafe {
        (
            std::slice::from_raw_parts(input, input_count as usize),
            std::slice::from_raw_parts_mut(output, output_count as usize),
        )
    };
    for (output, input) in output.iter_mut().zip(input.iter().step_by(6)) {
        *output = *input;
    }
    // SAFETY: The wrapper supplies writable progress counters.
    unsafe {
        *input_used = input_count;
        *output_generated = generated;
    }
    OK
}

unsafe extern "C" fn converter_destroy(handle: *mut Converter) {
    if !handle.is_null() {
        // SAFETY: Every handle was allocated by converter_create.
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

static FAILING_CONVERTER: ConverterDescriptor = ConverterDescriptor {
    create: Some(converter_create_fails),
    ..CONVERTER
};

fn sample_rate_adapter_from(descriptor: &'static ConverterDescriptor) -> SampleRateAdapter {
    // SAFETY: The descriptor and every referenced function have process lifetime.
    unsafe { SampleRateAdapter::from_raw(ptr::from_ref(descriptor).cast()) }.unwrap()
}

pub(crate) fn sample_rate_adapter() -> SampleRateAdapter {
    sample_rate_adapter_from(&CONVERTER)
}

pub(crate) fn failing_sample_rate_adapter() -> SampleRateAdapter {
    sample_rate_adapter_from(&FAILING_CONVERTER)
}

pub(crate) fn station_providers(
    graph: GraphProvider,
    sample_rate: SampleRateAdapter,
) -> StationProviders {
    // SAFETY: Every descriptor and referenced function has process lifetime.
    unsafe {
        StationProviders {
            graph,
            denoise: DenoiseProvider::from_raw_descriptor(ptr::from_ref(&DENOISE).cast()).unwrap(),
            ring: RingProvider::from_raw_descriptor(ptr::from_ref(&RING).cast()).unwrap(),
            radio: RadioProvider::from_raw_descriptor(ptr::from_ref(&RADIO).cast()).unwrap(),
            sample_rate,
        }
    }
}
