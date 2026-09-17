use super::*;

use std::cell::{Cell, RefCell};
use std::ffi::{CStr, c_char};
use std::mem::size_of;
use std::ptr;

thread_local! {
    static GRAPH_CREATES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESSES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_DESTROYS: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESS_FAILS: Cell<bool> = const { Cell::new(false) };
    static GRAPH_CREATE_FAIL_AT: Cell<usize> = const { Cell::new(0) };
    static GRAPH_DESCRIPTIONS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static DENOISE_PROCESSES: Cell<usize> = const { Cell::new(0) };
    static DENOISE_PROCESS_FAILS: Cell<bool> = const { Cell::new(false) };
}

type GraphCreate = unsafe extern "C" fn(*const TestGraphConfig, *mut *mut c_void) -> c_int;
type GraphStreamingProcess =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int;
type GraphDestroy = unsafe extern "C" fn(*mut c_void);
type GraphProcess = unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int;

#[repr(C)]
struct TestGraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

#[repr(C)]
struct TestGraphDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<GraphCreate>,
    process: Option<GraphStreamingProcess>,
    destroy: Option<GraphDestroy>,
    process_block: Option<GraphProcess>,
}

// SAFETY: Test descriptors and their capability names are immutable statics.
unsafe impl Sync for TestGraphDescriptor {}

struct TestGraphState {
    maximum_frame_count: u32,
}

unsafe extern "C" fn graph_create(
    config: *const TestGraphConfig,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies live setup pointers according to the ABI.
    let config = unsafe { &*config };
    assert_eq!(config.sample_rate_hz, 48_000);
    // SAFETY: The setup wrapper supplies a live NUL-terminated description.
    let description = unsafe { CStr::from_ptr(config.filter_description) };
    assert!(!description.is_empty());
    GRAPH_DESCRIPTIONS.with(|descriptions| {
        descriptions
            .borrow_mut()
            .push(description.to_string_lossy().into_owned());
    });
    let ordinal = GRAPH_CREATES.get() + 1;
    GRAPH_CREATES.set(ordinal);
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe {
        *output = Box::into_raw(Box::new(TestGraphState {
            maximum_frame_count: config.maximum_frame_count,
        }))
        .cast();
    }
    if GRAPH_CREATE_FAIL_AT.get() == ordinal {
        -2
    } else {
        0
    }
}

unsafe extern "C" fn graph_process_adapter(
    state: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies the handle created above and exact spans.
    let state = unsafe { &*state.cast::<TestGraphState>() };
    if frame_count > state.maximum_frame_count || GRAPH_PROCESS_FAILS.get() {
        return -2;
    }
    // SAFETY: The wrapper supplies distinct exact-length test spans.
    let (input, output) = unsafe {
        (
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    output.copy_from_slice(input);
    GRAPH_PROCESSES.set(GRAPH_PROCESSES.get() + 1);
    0
}

unsafe extern "C" fn graph_destroy(state: *mut c_void) {
    if !state.is_null() {
        // SAFETY: Every non-null state came from graph_create exactly once.
        drop(unsafe { Box::from_raw(state.cast::<TestGraphState>()) });
        GRAPH_DESTROYS.set(GRAPH_DESTROYS.get() + 1);
    }
}

static GRAPH_DESCRIPTOR: TestGraphDescriptor = TestGraphDescriptor {
    struct_size: size_of::<TestGraphDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.ffmpeg".as_ptr(),
    create: Some(graph_create),
    process: None,
    destroy: Some(graph_destroy),
    process_block: Some(graph_process_adapter),
};

type DenoiseCreate = unsafe extern "C" fn(u32, u32, u32, *mut *mut c_void) -> c_int;
type DenoiseProcess =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, *mut f32) -> c_int;
type DenoiseDestroy = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
struct TestDenoiseDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<DenoiseCreate>,
    process: Option<DenoiseProcess>,
    destroy: Option<DenoiseDestroy>,
}

// SAFETY: Test descriptors and their capability names are immutable statics.
unsafe impl Sync for TestDenoiseDescriptor {}

unsafe extern "C" fn denoise_create(
    sample_rate_hz: u32,
    channels: u32,
    frame_count: u32,
    output: *mut *mut c_void,
) -> c_int {
    assert_eq!((sample_rate_hz, channels, frame_count), (48_000, 1, 480));
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    0
}

unsafe extern "C" fn denoise_create_fails(
    sample_rate_hz: u32,
    channels: u32,
    frame_count: u32,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: Reuse the valid constructor so adapter cleanup is also exercised.
    unsafe { denoise_create(sample_rate_hz, channels, frame_count, output) };
    -2
}

unsafe extern "C" fn denoise_process_adapter(
    _state: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
    vad_probability: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies distinct exact-length test spans.
    let (input, output) = unsafe {
        (
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    output.copy_from_slice(input);
    // SAFETY: The wrapper supplies one writable probability value.
    unsafe { *vad_probability = 0.5 };
    DENOISE_PROCESSES.set(DENOISE_PROCESSES.get() + 1);
    if DENOISE_PROCESS_FAILS.get() { -2 } else { 0 }
}

unsafe extern "C" fn denoise_destroy(state: *mut c_void) {
    if !state.is_null() {
        // SAFETY: Every non-null state came from denoise_create exactly once.
        drop(unsafe { Box::from_raw(state.cast::<()>()) });
    }
}

static DENOISE_DESCRIPTOR: TestDenoiseDescriptor = TestDenoiseDescriptor {
    struct_size: size_of::<TestDenoiseDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.rnnoise".as_ptr(),
    create: Some(denoise_create),
    process: Some(denoise_process_adapter),
    destroy: Some(denoise_destroy),
};

static FAILING_DENOISE_DESCRIPTOR: TestDenoiseDescriptor = TestDenoiseDescriptor {
    create: Some(denoise_create_fails),
    ..DENOISE_DESCRIPTOR
};

fn graph_provider(descriptor: &'static TestGraphDescriptor) -> GraphProvider {
    // SAFETY: Static test descriptors reproduce the external ABI layout.
    unsafe { GraphProvider::from_raw_descriptor(ptr::from_ref(descriptor).cast()) }.unwrap()
}

fn denoise_provider(descriptor: &'static TestDenoiseDescriptor) -> DenoiseProvider {
    // SAFETY: Static test descriptors reproduce the external ABI layout.
    unsafe { DenoiseProvider::from_raw_descriptor(ptr::from_ref(descriptor).cast()) }.unwrap()
}

fn factory() -> NativeProcessingFactory {
    NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&DENOISE_DESCRIPTOR),
        "agc.so",
        960,
    )
    .unwrap()
}

fn plan() -> NativeProcessingPlan {
    NativeProcessingPlan {
        local: ProcessingChain::shipped(ChainRole::LocalReceive),
        voice_telemetry: ProcessingChain::shipped(ChainRole::VoiceTelemetry),
        deemphasis_enabled: true,
        deemphasis_corner_hz: 300.0,
        preemphasis_enabled: true,
        preemphasis_corner_hz: 300.0,
    }
}

#[test]
fn only_local_and_voice_telemetry_roles_are_accepted() {
    let mut invalid = plan();
    invalid.local.role = ChainRole::Link;
    assert!(matches!(
        validate_plan(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    invalid = plan();
    invalid.voice_telemetry.role = ChainRole::Link;
    assert!(matches!(
        validate_plan(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    assert!(validate_plan(&plan()).is_ok());
}

#[test]
fn preparation_owns_only_the_processors_selected_by_the_plan() {
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    GRAPH_DESTROYS.set(0);
    let mut generation = factory().prepare(&plan()).unwrap();
    assert!(generation.receive_ctcss_notch.iter().all(Option::is_none));
    assert!(generation.receive_ctcss_tail_notch.is_none());
    assert!(generation.receive_noise_reduction.is_none());
    assert_eq!(GRAPH_CREATES.get(), 6);
    assert_eq!(GRAPH_PROCESSES.get(), 6 * GRAPH_WARMUP_BLOCKS);
    {
        let _ports = generation.session_ports(ProgramRingPort::silence());
    }
    drop(generation);
    assert_eq!(GRAPH_DESTROYS.get(), 6);
}

#[test]
fn notch_mode_prepares_every_supported_tone_and_rnnoise_once() {
    GRAPH_CREATES.set(0);
    GRAPH_DESCRIPTIONS.with(|descriptions| descriptions.borrow_mut().clear());
    DENOISE_PROCESSES.set(0);
    let mut selected = plan();
    selected.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    selected.local.rnnoise_enabled = true;
    let mut generation = factory().prepare(&selected).unwrap();
    assert!(generation.receive_ctcss_notch.iter().all(Option::is_some));
    assert!(generation.receive_ctcss_tail_notch.is_some());
    assert!(generation.receive_noise_reduction.is_some());
    assert_eq!(GRAPH_CREATES.get(), 7 + CTCSS_TONE_COUNT);
    assert!(GRAPH_DESCRIPTIONS.with(|descriptions| {
        descriptions
            .borrow()
            .iter()
            .any(|description| description.contains("bandreject=f=55.000000000"))
    }));

    let _ports = generation.session_ports(ProgramRingPort::silence());
}

#[test]
fn graph_and_denoise_ports_process_exact_spans_and_report_failures() {
    let mut selected = plan();
    selected.local.rnnoise_enabled = true;
    let mut generation = factory().prepare(&selected).unwrap();
    let input = [0.25_f32; 480];
    let mut output = [0.0_f32; 480];
    let graph_context = NonNull::from(&mut generation.receive_deemphasis)
        .cast()
        .as_ptr();
    // SAFETY: Context and exact test spans remain live for this call.
    let result =
        unsafe { super::graph_process(graph_context, input.as_ptr(), output.as_mut_ptr(), 480) };
    assert_eq!(result, PORT_OK);
    assert_eq!(output, input);
    GRAPH_PROCESS_FAILS.set(true);
    // SAFETY: Context and exact test spans remain live for this call.
    let result =
        unsafe { super::graph_process(graph_context, input.as_ptr(), output.as_mut_ptr(), 480) };
    assert_eq!(result, PORT_ERROR);
    GRAPH_PROCESS_FAILS.set(false);

    let denoise = generation.receive_noise_reduction.as_mut().unwrap();
    let denoise_context = NonNull::from(denoise).cast().as_ptr();
    // SAFETY: Context and exact test spans remain live for this call.
    let result =
        unsafe { denoise_process(denoise_context, input.as_ptr(), output.as_mut_ptr(), 480) };
    assert_eq!(result, PORT_OK);
    DENOISE_PROCESS_FAILS.set(true);
    // SAFETY: Context and exact test spans remain live for this call.
    let result =
        unsafe { denoise_process(denoise_context, input.as_ptr(), output.as_mut_ptr(), 480) };
    DENOISE_PROCESS_FAILS.set(false);
    assert_eq!(result, PORT_ERROR);
    // SAFETY: The denoiser context remains live and exclusively borrowed.
    assert_eq!(unsafe { denoise_bypass(denoise_context, 480) }, PORT_OK);
}

#[test]
fn raw_ports_reject_empty_or_missing_arguments() {
    let input = [0.0_f32; 1];
    let mut output = [0.0_f32; 1];
    // SAFETY: The callback validates the deliberately null context first.
    let result =
        unsafe { super::graph_process(ptr::null_mut(), input.as_ptr(), output.as_mut_ptr(), 1) };
    assert_eq!(result, PORT_ERROR);
    // SAFETY: The callback validates all deliberately empty arguments.
    let result = unsafe { super::graph_process(ptr::null_mut(), ptr::null(), ptr::null_mut(), 0) };
    assert_eq!(result, PORT_ERROR);
    // SAFETY: The callback validates the deliberately null context first.
    let result =
        unsafe { denoise_process(ptr::null_mut(), input.as_ptr(), output.as_mut_ptr(), 1) };
    assert_eq!(result, PORT_ERROR);
    // SAFETY: The callback validates the deliberately null context first.
    assert_eq!(unsafe { denoise_bypass(ptr::null_mut(), 1) }, PORT_ERROR);
}

#[test]
fn setup_errors_remain_specific_and_human_readable() {
    let stream_error = NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&DENOISE_DESCRIPTOR),
        "agc.so",
        0,
    )
    .err()
    .unwrap();
    assert!(matches!(stream_error, ProcessingRuntimeError::Stream(_)));
    assert!(
        stream_error
            .to_string()
            .starts_with("invalid native stream:")
    );

    let description_error = NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&DENOISE_DESCRIPTOR),
        "bad\npath",
        960,
    )
    .err()
    .unwrap();
    assert!(matches!(
        description_error,
        ProcessingRuntimeError::GraphDescription(_)
    ));
    assert!(
        description_error
            .to_string()
            .starts_with("invalid FFmpeg graph:")
    );

    let mut invalid = plan();
    invalid.local.input_gain_db = 31.0;
    let configuration_error = factory().prepare(&invalid).err().unwrap();
    assert!(matches!(
        configuration_error,
        ProcessingRuntimeError::Configuration(_)
    ));
    assert!(
        configuration_error
            .to_string()
            .starts_with("invalid processing settings:")
    );
    invalid = plan();
    invalid.voice_telemetry.input_gain_db = 31.0;
    assert!(matches!(
        factory().prepare(&invalid),
        Err(ProcessingRuntimeError::Configuration(_))
    ));

    GRAPH_CREATES.set(0);
    GRAPH_CREATE_FAIL_AT.set(1);
    let graph_error = factory().prepare(&plan()).err().unwrap();
    GRAPH_CREATE_FAIL_AT.set(0);
    assert!(matches!(
        graph_error,
        ProcessingRuntimeError::GraphAdapter(_)
    ));
    assert!(
        graph_error
            .to_string()
            .starts_with("FFmpeg adapter setup failed:")
    );

    GRAPH_CREATES.set(0);
    GRAPH_CREATE_FAIL_AT.set(4);
    let transmit_graph_error = factory().prepare(&plan()).err().unwrap();
    GRAPH_CREATE_FAIL_AT.set(0);
    assert!(matches!(
        transmit_graph_error,
        ProcessingRuntimeError::GraphAdapter(_)
    ));

    let mut notch_plan = plan();
    notch_plan.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    GRAPH_CREATES.set(0);
    GRAPH_CREATE_FAIL_AT.set(1);
    assert!(matches!(
        factory().prepare(&notch_plan),
        Err(ProcessingRuntimeError::GraphAdapter(_))
    ));
    GRAPH_CREATE_FAIL_AT.set(0);

    GRAPH_PROCESS_FAILS.set(true);
    assert!(matches!(
        factory().prepare(&plan()),
        Err(ProcessingRuntimeError::GraphAdapter(_))
    ));
    GRAPH_PROCESS_FAILS.set(false);

    let mut selected = plan();
    selected.local.rnnoise_enabled = true;
    let denoise_failure = NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&FAILING_DENOISE_DESCRIPTOR),
        "agc.so",
        960,
    )
    .unwrap()
    .prepare(&selected)
    .err()
    .unwrap();
    assert!(matches!(
        denoise_failure,
        ProcessingRuntimeError::DenoiseAdapter(_)
    ));
    assert!(
        denoise_failure
            .to_string()
            .starts_with("RNNoise adapter setup failed:")
    );

    selected.local.agc.enabled = true;
    let nul_factory = NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&DENOISE_DESCRIPTOR),
        "bad\0path",
        960,
    )
    .unwrap();
    let nul_error = nul_factory.prepare(&selected).err().unwrap();
    assert!(matches!(
        nul_error,
        ProcessingRuntimeError::InvalidGraphDescription
    ));
    assert_eq!(
        nul_error.to_string(),
        "FFmpeg graph contains an interior NUL byte"
    );

    assert_eq!(
        ProcessingRuntimeError::IncorrectChainRole.to_string(),
        "processing chain has the wrong role"
    );
}
