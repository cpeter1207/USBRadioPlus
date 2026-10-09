use super::*;

use std::cell::{Cell, RefCell};
use std::ffi::{CStr, c_char};
use std::mem::size_of;
use std::ptr;

#[test]
fn explicit_native_processing_preserves_graphs_gain_and_only_selected_notches() {
    GRAPH_CREATES.set(0);
    GRAPH_DESTROYS.set(0);
    DENOISE_CREATES.set(0);
    GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
    let plan = ExplicitNativeProcessingPlan {
        receive_graph: "highpass=f=60,lowpass=f=5500".to_owned(),
        transmit_graph: "volume=-2dB".to_owned(),
        receive_deemphasis: true,
        receive_output_gain_db: -6,
        receive_ctcss_mask: 1,
        transmit_dcs: true,
    };
    let generation =
        NativeProcessingFactory::prepare_explicit(graph_provider(&GRAPH_DESCRIPTOR), &plan, 960)
            .unwrap();
    let descriptions = GRAPH_DESCRIPTIONS.with(|values| values.borrow().clone());
    assert!(descriptions.contains(&plan.receive_graph));
    assert!(descriptions.contains(&plan.transmit_graph));
    assert!(descriptions.contains(&"volume=-6dB".to_owned()));
    assert_eq!(
        descriptions
            .iter()
            .filter(|value| value.contains("bandreject"))
            .count(),
        2
    );
    assert!(descriptions.iter().any(|value| value.contains("biquad")));
    assert!(
        descriptions
            .iter()
            .any(|value| value.contains("volume=-3.42dB"))
    );
    assert_eq!(DENOISE_CREATES.get(), 0);
    drop(generation);
    assert_eq!(GRAPH_CREATES.get(), GRAPH_DESTROYS.get());
}

thread_local! {
    static GRAPH_CREATES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESSES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_DESTROYS: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESS_FAILS: Cell<bool> = const { Cell::new(false) };
    static GRAPH_CREATE_FAIL_AT: Cell<usize> = const { Cell::new(0) };
    static GRAPH_DESCRIPTIONS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static GRAPH_REPORT_HISTORY: Cell<bool> = const { Cell::new(false) };
    static GRAPH_SIMULATE_LOOKAHEAD: Cell<bool> = const { Cell::new(false) };
    static DENOISE_CREATES: Cell<usize> = const { Cell::new(0) };
    static DENOISE_PROCESSES: Cell<usize> = const { Cell::new(0) };
    static DENOISE_DESTROYS: Cell<usize> = const { Cell::new(0) };
    static DENOISE_PROCESS_FAILS: Cell<bool> = const { Cell::new(false) };
    static DENOISE_GAIN: Cell<f32> = const { Cell::new(1.0) };
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
    processed_blocks: usize,
    delay: Vec<f32>,
    delay_cursor: usize,
    gain: f32,
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
    let description = description.to_string_lossy();
    let lookahead = GRAPH_SIMULATE_LOOKAHEAD
        .get()
        .then(|| {
            description.split("alimiter=").nth(1).map(|limiter| {
                let attack = limiter.split("attack=").nth(1).unwrap();
                let milliseconds = attack.split(':').next().unwrap().parse::<f64>().unwrap();
                // Model the measured tail: limiter delay plus three IIR samples.
                (milliseconds * 48.0).ceil() as usize + 3
            })
        })
        .flatten()
        .unwrap_or(0);
    let gain = if GRAPH_SIMULATE_LOOKAHEAD.get() {
        description.split("volume=").nth(1).map_or(1.0, |volume| {
            volume
                .split([':', '[', ','])
                .next()
                .unwrap()
                .parse::<f32>()
                .unwrap_or(1.0)
        })
    } else {
        1.0
    };
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe {
        *output = Box::into_raw(Box::new(TestGraphState {
            maximum_frame_count: config.maximum_frame_count,
            processed_blocks: 0,
            delay: vec![0.0; lookahead],
            delay_cursor: 0,
            gain,
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
    let state = unsafe { &mut *state.cast::<TestGraphState>() };
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
    for sample in output.iter_mut() {
        *sample *= state.gain;
        if !state.delay.is_empty() {
            std::mem::swap(sample, &mut state.delay[state.delay_cursor]);
            state.delay_cursor = (state.delay_cursor + 1) % state.delay.len();
        }
    }
    state.processed_blocks += 1;
    if GRAPH_REPORT_HISTORY.get() {
        output.fill(state.processed_blocks as f32);
    }
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
    DENOISE_CREATES.set(DENOISE_CREATES.get() + 1);
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
    for sample in output.iter_mut() {
        *sample *= DENOISE_GAIN.get();
    }
    // SAFETY: The wrapper supplies one writable probability value.
    unsafe { *vad_probability = 0.5 };
    DENOISE_PROCESSES.set(DENOISE_PROCESSES.get() + 1);
    if DENOISE_PROCESS_FAILS.get() { -2 } else { 0 }
}

unsafe extern "C" fn denoise_destroy(state: *mut c_void) {
    if !state.is_null() {
        // SAFETY: Every non-null state came from denoise_create exactly once.
        drop(unsafe { Box::from_raw(state.cast::<()>()) });
        DENOISE_DESTROYS.set(DENOISE_DESTROYS.get() + 1);
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
    factory_with_maximum(960)
}

fn factory_with_maximum(maximum: u32) -> NativeProcessingFactory {
    NativeProcessingFactory::new(
        graph_provider(&GRAPH_DESCRIPTOR),
        denoise_provider(&DENOISE_DESCRIPTOR),
        "agc.so",
        maximum,
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
fn unchanged_replacement_does_not_create_or_warm_processors() {
    let mut selected = plan();
    selected.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    selected.local.rnnoise_enabled = true;
    let original = factory().prepare(&selected).unwrap();
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    GRAPH_DESTROYS.set(0);
    DENOISE_CREATES.set(0);
    let replacement =
        // SAFETY: these generations are never processed concurrently.
        unsafe { factory().prepare_replacement(&selected, &selected, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 0);
    assert_eq!(GRAPH_PROCESSES.get(), 0);
    assert_eq!(DENOISE_CREATES.get(), 0);
    drop(original);
    assert_eq!(GRAPH_DESTROYS.get(), 0);
    drop(replacement);
    assert_eq!(GRAPH_DESTROYS.get(), 7 + CTCSS_TONE_COUNT);
}

fn process_receive_history(generation: &mut ProcessingGeneration) -> f32 {
    let input = [0.0; 1];
    let mut output = [0.0; 1];
    let context = generation.receive_deemphasis.pointer().cast().as_ptr();
    assert_eq!(
        // SAFETY: the test owns this processor serially and supplies exact spans.
        unsafe { graph_process(context, input.as_ptr(), output.as_mut_ptr(), 1) },
        PORT_OK
    );
    output[0]
}

#[test]
fn transmit_change_preserves_receive_history_and_fixed_processors() {
    let original_plan = plan();
    let mut original = factory().prepare(&original_plan).unwrap();
    GRAPH_REPORT_HISTORY.set(true);
    assert_eq!(process_receive_history(&mut original), 9.0);
    let mut changed = original_plan.clone();
    changed.voice_telemetry.output_gain_db = -3.0;
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    GRAPH_DESTROYS.set(0);
    let mut replacement =
        // SAFETY: old and replacement processors retain the same serial owner.
        unsafe { factory().prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 1);
    assert_eq!(GRAPH_PROCESSES.get(), 8);
    drop(original);
    // The changed TX stage retains its audible predecessor until control reclaim.
    assert_eq!(GRAPH_DESTROYS.get(), 0);
    let calls = GRAPH_PROCESSES.get();
    assert_eq!(process_receive_history(&mut replacement), 10.0);
    assert_eq!(GRAPH_PROCESSES.get(), calls + 1);
    GRAPH_REPORT_HISTORY.set(false);
}

fn process_transmit(generation: &mut ProcessingGeneration, input: &[f32]) -> Vec<f32> {
    let mut output = vec![0.0; input.len()];
    let context = generation.transmit_program.pointer().cast().as_ptr();
    assert_eq!(
        // SAFETY: the test owns the TX processor serially and supplies exact spans.
        unsafe {
            graph_process(
                context,
                input.as_ptr(),
                output.as_mut_ptr(),
                input.len() as u32,
            )
        },
        PORT_OK
    );
    output
}

fn process_denoise(generation: &mut ProcessingGeneration, input: &[f32]) -> Vec<f32> {
    let mut output = vec![0.0; input.len()];
    let context = generation
        .receive_noise_reduction
        .as_ref()
        .unwrap()
        .pointer()
        .cast()
        .as_ptr();
    assert_eq!(
        // SAFETY: this test owns the model serially and supplies exact spans.
        unsafe {
            denoise_process(
                context,
                input.as_ptr(),
                output.as_mut_ptr(),
                input.len() as u32,
            )
        },
        PORT_OK
    );
    output
}

fn bypass_denoise(generation: &mut ProcessingGeneration) {
    let context = generation
        .receive_noise_reduction
        .as_ref()
        .unwrap()
        .pointer()
        .cast()
        .as_ptr();
    assert_eq!(
        // SAFETY: the same serial test owner controls this live model.
        unsafe { denoise_bypass(context, 64) },
        PORT_OK
    );
}

fn denoise_lease_counts(generation: &ProcessingGeneration) -> (usize, usize) {
    let stage = &generation.receive_noise_reduction.as_ref().unwrap().0;
    (Arc::strong_count(stage), Arc::strong_count(&stage.stream.0))
}

#[test]
fn enabling_denoise_bridges_startup_once_then_preserves_unchanged_and_burst_behavior() {
    DENOISE_GAIN.set(0.5);
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut enabled = original_plan.clone();
    enabled.local.rnnoise_enabled = true;
    let mut generation =
        // SAFETY: the replacement retains this same serial RX owner.
        unsafe { factory().prepare_replacement(&enabled, &original_plan, &original) }.unwrap();
    let leases = denoise_lease_counts(&generation);
    let destroys = DENOISE_DESTROYS.get();
    assert_eq!(process_denoise(&mut generation, &[0.5; 300]), [0.5; 300]);
    assert_eq!(process_denoise(&mut generation, &[0.5; 660]), [0.5; 660]);
    let fade = process_denoise(&mut generation, &[0.5; 48]);
    for (index, sample) in fade.iter().enumerate() {
        let expected = 0.5 - 0.25 * (index + 1) as f32 / 48.0;
        assert!((*sample - expected).abs() < 0.0001);
    }
    assert_eq!(denoise_lease_counts(&generation), leases);
    assert_eq!(DENOISE_DESTROYS.get(), destroys);
    let creates = DENOISE_CREATES.get();
    let mut unchanged =
        // SAFETY: only the successor is processed after this snapshot.
        unsafe { factory().prepare_replacement(&enabled, &enabled, &generation) }.unwrap();
    assert_eq!(DENOISE_CREATES.get(), creates);
    assert_eq!(
        generation
            .receive_noise_reduction
            .as_ref()
            .unwrap()
            .pointer(),
        unchanged
            .receive_noise_reduction
            .as_ref()
            .unwrap()
            .pointer()
    );
    assert_eq!(process_denoise(&mut unchanged, &[0.5; 17]), [0.25; 17]);
    bypass_denoise(&mut unchanged);
    assert_eq!(
        process_denoise(&mut unchanged, &[0.5; 960]),
        [0.0; 960],
        "one-shot enabling bridge must not change established burst startup"
    );
    let mut initial = factory().prepare(&enabled).unwrap();
    assert_eq!(
        process_denoise(&mut initial, &[0.5; 960]),
        [0.0; 960],
        "initial-generation startup policy remains unchanged"
    );
    DENOISE_GAIN.set(1.0);
}

#[test]
fn bypass_before_denoise_activation_completes_restarts_its_live_prime() {
    DENOISE_GAIN.set(0.5);
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut enabled = original_plan.clone();
    enabled.local.rnnoise_enabled = true;
    let mut generation =
        // SAFETY: the same serial owner controls all generations.
        unsafe { factory().prepare_replacement(&enabled, &original_plan, &original) }.unwrap();
    assert_eq!(process_denoise(&mut generation, &[0.5; 300]), [0.5; 300]);
    bypass_denoise(&mut generation);
    assert_eq!(process_denoise(&mut generation, &[0.5; 660]), [0.5; 660]);
    assert_eq!(process_denoise(&mut generation, &[0.5; 300]), [0.5; 300]);
    assert!(
        process_denoise(&mut generation, &[0.5; 48])
            .iter()
            .all(|sample| *sample >= 0.25)
    );
    assert_eq!(process_denoise(&mut generation, &[0.5; 17]), [0.25; 17]);
    DENOISE_GAIN.set(1.0);
}

#[test]
fn restoring_denoise_after_disabling_reuses_model_and_discards_stale_framing() {
    DENOISE_GAIN.set(0.5);
    let mut original_plan = plan();
    original_plan.local.rnnoise_enabled = true;
    let mut original = factory().prepare(&original_plan).unwrap();
    process_denoise(&mut original, &[0.5; 1440]);
    let mut disabled_plan = original_plan.clone();
    disabled_plan.local.rnnoise_enabled = false;
    let disabled =
        // SAFETY: forward disable keeps the same serial owner.
        unsafe { factory().prepare_replacement(&disabled_plan, &original_plan, &original) }.unwrap();
    DENOISE_CREATES.set(0);
    DENOISE_PROCESSES.set(0);
    let mut reverse =
        // SAFETY: inverse follows the disabled forward owner.
        unsafe { factory().prepare_restore(&original_plan, &original, &disabled) }.unwrap();
    assert_eq!(DENOISE_CREATES.get(), 0);
    assert_eq!(DENOISE_PROCESSES.get(), 0);
    assert_eq!(process_denoise(&mut reverse, &[0.8; 960]), [0.8; 960]);
    assert!(
        process_denoise(&mut reverse, &[0.8; 48])
            .iter()
            .all(|sample| *sample >= 0.4)
    );
    assert_eq!(process_denoise(&mut reverse, &[0.8; 17]), [0.4; 17]);
    DENOISE_GAIN.set(1.0);
}

#[test]
fn changed_transmit_graph_covers_real_lookahead_without_silent_samples() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    for maximum in [64, 960] {
        for lookahead_ms in [5.0, 20.0] {
            let factory = factory_with_maximum(maximum);
            let mut original_plan = plan();
            original_plan.preemphasis_enabled = false;
            original_plan.voice_telemetry.enabled = false;
            original_plan.voice_telemetry.input_gain_db = 0.0;
            original_plan.voice_telemetry.transmit_tail.limiter_enabled = true;
            original_plan.voice_telemetry.transmit_tail.lookahead_ms = lookahead_ms;
            let mut original = factory.prepare(&original_plan).unwrap();
            for _ in 0..32 {
                process_transmit(&mut original, &vec![0.5; maximum as usize]);
            }
            let mut changed = original_plan.clone();
            changed.voice_telemetry.output_gain_db = -6.020_599_913;
            let mut replacement =
                // SAFETY: both generations retain the same serial owner.
                unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
            GRAPH_DESTROYS.set(0);
            let mut processed = 0;
            for count in [1, 17, 31, maximum as usize].into_iter().cycle().take(128) {
                let calls = GRAPH_PROCESSES.get();
                let output = process_transmit(&mut replacement, &vec![0.5; count]);
                assert!(
                    output.iter().all(|sample| *sample >= 0.249),
                    "silent handoff: maximum={maximum}, lookahead={lookahead_ms}, offset={processed}, output={output:?}"
                );
                assert!(GRAPH_PROCESSES.get() - calls <= 2);
                if processed > 2_000 {
                    assert_eq!(GRAPH_PROCESSES.get() - calls, 1);
                    assert!(
                        output.iter().all(|sample| (*sample - 0.25).abs() < 0.0001),
                        "output={output:?}"
                    );
                }
                processed += count;
                assert_eq!(GRAPH_DESTROYS.get(), 0, "callback must not reclaim graphs");
            }
            drop(original);
            assert_eq!(GRAPH_DESTROYS.get(), 0);
            drop(replacement);
            assert_eq!(GRAPH_DESTROYS.get(), 7);
        }
    }
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn rapid_graph_replacements_keep_one_audible_predecessor_and_bounded_work() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    GRAPH_CREATES.set(0);
    GRAPH_DESTROYS.set(0);
    let factory = factory();
    let mut active_plan = plan();
    active_plan.preemphasis_enabled = false;
    active_plan.voice_telemetry.enabled = false;
    active_plan.voice_telemetry.input_gain_db = 0.0;
    active_plan.voice_telemetry.transmit_tail.limiter_enabled = true;
    let mut active = factory.prepare(&active_plan).unwrap();
    process_transmit(&mut active, &[0.5; 960]);
    for gain in [-3.0, -6.0, -9.0, -12.0] {
        let mut changed = active_plan.clone();
        changed.voice_telemetry.output_gain_db = gain;
        let replacement =
            // SAFETY: each replacement takes over the same serial owner.
            unsafe { factory.prepare_replacement(&changed, &active_plan, &active) }.unwrap();
        active = replacement;
        active_plan = changed;
        assert!(
            GRAPH_CREATES.get() - GRAPH_DESTROYS.get() <= 8,
            "only six active stages plus two flat predecessors may remain"
        );
        let calls = GRAPH_PROCESSES.get();
        let output = process_transmit(&mut active, &[0.5; 17]);
        assert!(
            output.iter().all(|sample| (*sample - 0.5).abs() < 0.0001),
            "output={output:?}"
        );
        assert_eq!(GRAPH_PROCESSES.get() - calls, 2);
    }
    process_transmit(&mut active, &[0.5; 960]);
    let completed = process_transmit(&mut active, &[0.5; 960]);
    let expected = 0.5 * 10_f32.powf(-12.0 / 20.0);
    assert!((completed[959] - expected).abs() < 0.0001);
    let mut changed = active_plan.clone();
    changed.voice_telemetry.output_gain_db = -18.0;
    let mut final_generation =
        // SAFETY: completion does not change the assigned owner.
        unsafe { factory.prepare_replacement(&changed, &active_plan, &active) }.unwrap();
    let first = process_transmit(&mut final_generation, &[0.5; 1]);
    assert!(
        (first[0] - expected).abs() < 0.0001,
        "a completed transition must become the next audible predecessor"
    );
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn graph_transition_has_an_exact_bounded_fade_and_never_changes_callback_leases() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    let factory = factory_with_maximum(64);
    let mut original_plan = plan();
    original_plan.preemphasis_enabled = false;
    original_plan.voice_telemetry.enabled = false;
    original_plan.voice_telemetry.input_gain_db = 0.0;
    let original = factory.prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    changed.voice_telemetry.output_gain_db = -6.020_599_913;
    let mut replacement =
        // SAFETY: old and new ports are only invoked by this serial test owner.
        unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    drop(original);
    let lease_counts = || {
        let stage = &replacement.transmit_program.0;
        (
            Arc::strong_count(stage),
            Arc::strong_count(&stage.primary.0),
            Arc::strong_count(&stage.previous.as_ref().unwrap().current.0),
        )
    };
    let before = lease_counts();
    GRAPH_DESTROYS.set(0);
    GRAPH_PROCESSES.set(0);
    let context = replacement.transmit_program.pointer().cast().as_ptr();
    let mut oversized = [0.0; 65];
    assert_eq!(
        // SAFETY: exact spans intentionally exceed the prepared maximum.
        unsafe { graph_process(context, [1.0; 65].as_ptr(), oversized.as_mut_ptr(), 65) },
        PORT_ERROR
    );
    assert_eq!(GRAPH_PROCESSES.get(), 0);
    assert_eq!(process_transmit(&mut replacement, &[1.0; 64]), [1.0; 64]);
    let fade = process_transmit(&mut replacement, &[1.0; 48]);
    for (index, sample) in fade.iter().enumerate() {
        let expected = 1.0 - 0.5 * (index + 1) as f32 / 48.0;
        assert!((*sample - expected).abs() < 0.0001);
    }
    let calls = GRAPH_PROCESSES.get();
    assert_eq!(process_transmit(&mut replacement, &[1.0; 17]), [0.5; 17]);
    assert_eq!(GRAPH_PROCESSES.get(), calls + 1);
    let stage = &replacement.transmit_program.0;
    let after = (
        Arc::strong_count(stage),
        Arc::strong_count(&stage.primary.0),
        Arc::strong_count(&stage.previous.as_ref().unwrap().current.0),
    );
    assert_eq!(after, before);
    assert_eq!(GRAPH_DESTROYS.get(), 0);
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn delayed_publication_selects_the_now_audible_predecessor() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    let factory = factory();
    let mut original_plan = plan();
    original_plan.preemphasis_enabled = false;
    original_plan.voice_telemetry.enabled = false;
    original_plan.voice_telemetry.input_gain_db = 0.0;
    original_plan.voice_telemetry.transmit_tail.limiter_enabled = true;
    let mut original = factory.prepare(&original_plan).unwrap();
    process_transmit(&mut original, &[0.5; 960]);
    let mut first_plan = original_plan.clone();
    first_plan.voice_telemetry.output_gain_db = -6.020_599_913;
    let mut first =
        // SAFETY: all generations retain this same serial owner.
        unsafe { factory.prepare_replacement(&first_plan, &original_plan, &original) }.unwrap();
    process_transmit(&mut first, &[0.0; 64]);
    let mut second_plan = first_plan.clone();
    second_plan.voice_telemetry.output_gain_db = -12.041_199_827;
    let mut second =
        // SAFETY: preparation precedes publication while the same owner runs first.
        unsafe { factory.prepare_replacement(&second_plan, &first_plan, &first) }.unwrap();
    // Finish the first transition during silence, then let speech resume before
    // publishing the second. Its old fallback now contains stale silent PCM.
    for _ in 0..2 {
        process_transmit(&mut first, &[0.0; 960]);
    }
    for _ in 0..2 {
        process_transmit(&mut first, &[1.0; 960]);
    }
    let output = process_transmit(&mut second, &[1.0; 1]);
    assert!(
        (output[0] - 0.5).abs() < 0.0001,
        "stale fallback: {output:?}"
    );
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn repeated_idle_receive_updates_remain_flat_and_process_only_two_graphs() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    GRAPH_CREATES.set(0);
    GRAPH_DESTROYS.set(0);
    let factory = factory_with_maximum(64);
    let mut active_plan = plan();
    active_plan.local.enabled = false;
    active_plan.local.output_gain_db = 0.0;
    let mut active = factory.prepare(&active_plan).unwrap();
    for gain in 1..=10 {
        let mut changed = active_plan.clone();
        changed.local.output_gain_db = -f64::from(gain);
        let replacement =
            // SAFETY: idle RX has no concurrent calls and keeps the same owner.
            unsafe { factory.prepare_replacement(&changed, &active_plan, &active) }.unwrap();
        active = replacement;
        active_plan = changed;
        assert!(GRAPH_CREATES.get() - GRAPH_DESTROYS.get() <= 8);
    }
    let mut output = [0.0; 1];
    let context = active.receive_dynamics.pointer().cast().as_ptr();
    GRAPH_PROCESSES.set(0);
    assert_eq!(
        // SAFETY: the same RX owner begins processing exact spans after idle.
        unsafe { graph_process(context, [0.5].as_ptr(), output.as_mut_ptr(), 1) },
        PORT_OK
    );
    assert_eq!(output, [0.5]);
    assert_eq!(GRAPH_PROCESSES.get(), 2);
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn inverse_handoff_primes_retained_originals_without_recreation_or_silent_pcm() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    for complete_forward in [false, true] {
        let factory = factory();
        let mut original_plan = plan();
        original_plan.preemphasis_enabled = false;
        original_plan.voice_telemetry.enabled = false;
        original_plan.voice_telemetry.input_gain_db = 0.0;
        original_plan.voice_telemetry.transmit_tail.limiter_enabled = true;
        let mut original = factory.prepare(&original_plan).unwrap();
        process_transmit(&mut original, &[0.5; 960]);
        let mut changed = original_plan.clone();
        changed.voice_telemetry.output_gain_db = -6.020_599_913;
        let mut forward =
            // SAFETY: all ports retain one serial owner.
            unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
        GRAPH_CREATES.set(0);
        GRAPH_PROCESSES.set(0);
        let mut reverse =
            // SAFETY: the inverse will immediately follow this forward update.
            unsafe { factory.prepare_restore(&original_plan, &original, &forward) }.unwrap();
        assert_eq!(GRAPH_CREATES.get(), 0);
        assert_eq!(GRAPH_PROCESSES.get(), 0);
        if complete_forward {
            for _ in 0..2 {
                process_transmit(&mut forward, &[0.0; 960]);
            }
            for _ in 0..2 {
                process_transmit(&mut forward, &[1.0; 960]);
            }
            for _ in 0..4 {
                let output = process_transmit(&mut reverse, &[1.0; 480]);
                assert!(output.iter().all(|sample| *sample >= 0.499));
            }
            assert_eq!(process_transmit(&mut reverse, &[1.0; 1]), [1.0]);
        } else {
            let calls = GRAPH_PROCESSES.get();
            assert_eq!(process_transmit(&mut reverse, &[0.5; 17]), [0.5; 17]);
            assert_eq!(
                GRAPH_PROCESSES.get(),
                calls + 1,
                "restoring the still-audible primary must not process it twice"
            );
        }
    }
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn inverse_accepts_a_target_that_finishes_its_own_transition_after_preparation() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    let factory = factory_with_maximum(64);
    let mut original_plan = plan();
    original_plan.preemphasis_enabled = false;
    original_plan.voice_telemetry.enabled = false;
    original_plan.voice_telemetry.input_gain_db = 0.0;
    let original = factory.prepare(&original_plan).unwrap();
    let mut target_plan = original_plan.clone();
    target_plan.voice_telemetry.output_gain_db = -6.020_599_913;
    let mut target =
        // SAFETY: the same serial owner will adopt each successive generation.
        unsafe { factory.prepare_replacement(&target_plan, &original_plan, &original) }.unwrap();
    let mut changed = target_plan.clone();
    changed.voice_telemetry.output_gain_db = -12.041_199_827;
    let mut forward =
        // SAFETY: target remains active while preparing its successor.
        unsafe { factory.prepare_replacement(&changed, &target_plan, &target) }.unwrap();
    let mut reverse =
        // SAFETY: inverse immediately follows forward under the same owner.
        unsafe { factory.prepare_restore(&target_plan, &target, &forward) }.unwrap();
    for _ in 0..3 {
        process_transmit(&mut target, &[1.0; 64]);
    }
    process_transmit(&mut forward, &[1.0; 1]);
    let calls = GRAPH_PROCESSES.get();
    assert_eq!(process_transmit(&mut reverse, &[1.0; 1]), [0.5]);
    assert_eq!(GRAPH_PROCESSES.get(), calls + 1);
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn ineffective_setting_change_keeps_the_same_graphs() {
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    // Local input gain is applied by the radio, not its dynamics graph.
    changed.local.input_gain_db = -3.0;
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    let _replacement =
        // SAFETY: neither generation is published to a callback in this test.
        unsafe { factory().prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 0);
    assert_eq!(GRAPH_PROCESSES.get(), 0);
}

#[test]
fn failed_replacement_leaves_active_processor_history_untouched() {
    let original_plan = plan();
    let mut original = factory().prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    changed.deemphasis_corner_hz = 250.0;
    changed.voice_telemetry.output_gain_db = -3.0;
    GRAPH_CREATES.set(0);
    GRAPH_DESTROYS.set(0);
    GRAPH_CREATE_FAIL_AT.set(2);
    // SAFETY: replacement construction never processes the original owners.
    let result = unsafe { factory().prepare_replacement(&changed, &original_plan, &original) };
    GRAPH_CREATE_FAIL_AT.set(0);
    assert!(matches!(
        result,
        Err(ProcessingRuntimeError::GraphAdapter(_))
    ));
    assert_eq!(GRAPH_DESTROYS.get(), 2);
    GRAPH_REPORT_HISTORY.set(true);
    assert_eq!(process_receive_history(&mut original), 9.0);
    GRAPH_REPORT_HISTORY.set(false);
}

#[test]
fn receive_changes_replace_only_the_affected_stage() {
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut changes = [
        original_plan.clone(),
        original_plan.clone(),
        original_plan.clone(),
    ];
    changes[0].deemphasis_corner_hz = 250.0;
    changes[1].local.receive.bandpass_enabled = !original_plan.local.receive.bandpass_enabled;
    changes[2].local.output_gain_db = -3.0;
    for changed in changes {
        GRAPH_CREATES.set(0);
        GRAPH_PROCESSES.set(0);
        let _replacement =
            // SAFETY: no generation in this test is bound to a live session.
            unsafe { factory().prepare_replacement(&changed, &original_plan, &original) }.unwrap();
        assert_eq!(GRAPH_CREATES.get(), 1);
        assert_eq!(GRAPH_PROCESSES.get(), 8);
    }
}

#[test]
fn invalid_replacement_is_rejected_before_touching_processors() {
    let mut original_plan = plan();
    original_plan.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    original_plan.local.rnnoise_enabled = true;
    let original = factory().prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    changed.local.receive.notch_width_hz += 1.0;
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    DENOISE_CREATES.set(0);
    // SAFETY: neither generation is bound to a live session.
    let replacement = unsafe { factory().prepare_replacement(&changed, &original_plan, &original) };
    assert!(matches!(
        replacement,
        Err(ProcessingRuntimeError::Configuration(_))
    ));
    assert_eq!(GRAPH_CREATES.get(), 0);
    assert_eq!(GRAPH_PROCESSES.get(), 0);
    assert_eq!(DENOISE_CREATES.get(), 0);
}

#[test]
fn enabling_and_disabling_optional_processors_retains_unrelated_stages() {
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut enabled = original_plan.clone();
    enabled.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    enabled.local.rnnoise_enabled = true;
    GRAPH_CREATES.set(0);
    DENOISE_CREATES.set(0);
    let enabled_generation =
        // SAFETY: these generations are never processed concurrently.
        unsafe { factory().prepare_replacement(&enabled, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), CTCSS_TONE_COUNT + 2);
    assert_eq!(DENOISE_CREATES.get(), 1);
    GRAPH_CREATES.set(0);
    DENOISE_CREATES.set(0);
    let disabled =
        // SAFETY: these generations are never processed concurrently.
        unsafe { factory().prepare_replacement(&original_plan, &enabled, &enabled_generation) }
            .unwrap();
    assert_eq!(GRAPH_CREATES.get(), 1);
    assert_eq!(DENOISE_CREATES.get(), 0);
    assert!(disabled.receive_noise_reduction.is_none());
    assert!(disabled.receive_ctcss_notch.iter().all(Option::is_none));
    assert!(disabled.receive_ctcss_tail_notch.is_none());
}

#[test]
fn graph_and_denoise_ports_process_exact_spans_and_report_failures() {
    let mut selected = plan();
    selected.local.rnnoise_enabled = true;
    let mut generation = factory().prepare(&selected).unwrap();
    let input = [0.25_f32; 480];
    let mut output = [0.0_f32; 480];
    let graph_context = generation.receive_deemphasis.pointer().cast().as_ptr();
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
    let denoise_context = denoise.pointer().cast().as_ptr();
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
    for ordinal in [1, CTCSS_TONE_COUNT + 1] {
        GRAPH_CREATES.set(0);
        GRAPH_CREATE_FAIL_AT.set(ordinal);
        assert!(matches!(
            factory().prepare(&notch_plan),
            Err(ProcessingRuntimeError::GraphAdapter(_))
        ));
        GRAPH_CREATE_FAIL_AT.set(0);
    }

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
