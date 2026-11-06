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
        local: None,
        voice_telemetry: None,
        receive_input_gain: 1.0,
        receive_graph: "highpass=f=60,lowpass=f=5500".to_owned(),
        transmit_graph: "volume=-2dB".to_owned(),
        receive_deemphasis: true,
        receive_deemphasis_corner_hz: 300.0,
        transmit_preemphasis: false,
        transmit_preemphasis_corner_hz: 300.0,
        receive_output_gain_db: -6,
        receive_ctcss_mask: 1,
        transmit_dcs: true,
    };
    let generation = NativeProcessingFactory::prepare_explicit(
        graph_provider(&GRAPH_DESCRIPTOR),
        None,
        "",
        &plan,
        960,
    )
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

#[test]
fn explicit_native_graph_uses_passthrough_for_zero_gain_and_reports_notch_failure() {
    GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
    let mut plan = ExplicitNativeProcessingPlan {
        local: None,
        voice_telemetry: None,
        receive_input_gain: 1.0,
        receive_graph: "anull".to_owned(),
        transmit_graph: "anull".to_owned(),
        receive_deemphasis: false,
        receive_deemphasis_corner_hz: 300.0,
        transmit_preemphasis: false,
        transmit_preemphasis_corner_hz: 300.0,
        receive_output_gain_db: 0,
        receive_ctcss_mask: 0,
        transmit_dcs: false,
    };
    let generation = NativeProcessingFactory::prepare_explicit(
        graph_provider(&GRAPH_DESCRIPTOR),
        None,
        "",
        &plan,
        960,
    )
    .unwrap();
    assert!(generation.receive_ctcss_notch.iter().all(Option::is_none));
    GRAPH_DESCRIPTIONS.with(|values| assert_eq!(values.borrow()[2], "anull"));
    drop(generation);

    plan.receive_ctcss_mask = 1;
    GRAPH_CREATES.set(0);
    GRAPH_CREATE_FAIL_AT.set(1);
    assert!(matches!(
        NativeProcessingFactory::prepare_explicit(
            graph_provider(&GRAPH_DESCRIPTOR),
            None,
            "",
            &plan,
            960
        ),
        Err(ProcessingRuntimeError::GraphAdapter(_))
    ));
    GRAPH_CREATE_FAIL_AT.set(0);
}

#[test]
fn explicit_native_graph_reports_deemphasis_and_receive_gain_prepare_failures() {
    let plan = ExplicitNativeProcessingPlan {
        local: None,
        voice_telemetry: None,
        receive_input_gain: 1.0,
        receive_graph: "anull".to_owned(),
        transmit_graph: "anull".to_owned(),
        receive_deemphasis: true,
        receive_deemphasis_corner_hz: 300.0,
        transmit_preemphasis: false,
        transmit_preemphasis_corner_hz: 300.0,
        receive_output_gain_db: -6,
        receive_ctcss_mask: 0,
        transmit_dcs: false,
    };
    // De-emphasis and receive filtering precede the receive-gain graph.
    for (failed_ordinal, expected) in [(1, "biquad"), (2, "anull"), (3, "volume=-6dB")] {
        GRAPH_CREATES.set(0);
        GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
        GRAPH_CREATE_FAIL_AT.set(failed_ordinal);
        let result = NativeProcessingFactory::prepare_explicit(
            graph_provider(&GRAPH_DESCRIPTOR),
            None,
            "",
            &plan,
            960,
        );
        GRAPH_CREATE_FAIL_AT.set(0);
        assert!(matches!(
            result,
            Err(ProcessingRuntimeError::GraphAdapter(_))
        ));
        GRAPH_DESCRIPTIONS.with(|values| {
            let values = values.borrow();
            assert!(values[failed_ordinal - 1].contains(expected));
        });
    }
}

#[test]
fn explicit_native_profiles_use_the_same_prepared_chains_and_conditioning() {
    GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
    DENOISE_CREATES.set(0);
    let mut profile = plan();
    profile.local.rnnoise_enabled = true;
    profile.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    profile.local.input_gain_db = -3.0;
    profile.local.output_gain_db = -8.0;
    profile.local.agc.enabled = true;
    profile.local.limiter.enabled = true;
    profile.voice_telemetry.limiter.enabled = true;
    profile.voice_telemetry.transmit_tail.bandpass_enabled = true;
    let reference = factory().prepare(&profile).unwrap();
    let expected = GRAPH_DESCRIPTIONS.with(|values| values.borrow().clone());
    drop(reference);
    GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
    let request = ExplicitNativeProcessingPlan {
        local: Some(profile.local),
        voice_telemetry: Some(profile.voice_telemetry),
        receive_input_gain: 1.0,
        receive_graph: "unused receive graph".to_owned(),
        transmit_graph: "unused transmit graph".to_owned(),
        receive_deemphasis: profile.deemphasis_enabled,
        receive_deemphasis_corner_hz: profile.deemphasis_corner_hz,
        transmit_preemphasis: profile.preemphasis_enabled,
        transmit_preemphasis_corner_hz: profile.preemphasis_corner_hz,
        receive_output_gain_db: 17,
        receive_ctcss_mask: (1_u64 << CTCSS_TONE_COUNT) - 1,
        transmit_dcs: true,
    };
    let native = NativeProcessingFactory::prepare_explicit(
        graph_provider(&GRAPH_DESCRIPTOR),
        Some(denoise_provider(&DENOISE_DESCRIPTOR)),
        "agc.so",
        &request,
        960,
    )
    .unwrap();
    assert_eq!(
        GRAPH_DESCRIPTIONS.with(|values| values.borrow().clone()),
        expected
    );
    assert!(
        expected
            .iter()
            .any(|graph| graph.contains("ladspa=file='agc.so'"))
    );
    assert!(expected.iter().any(|graph| graph.contains("acompressor=")));
    assert!(!expected.contains(&request.receive_graph));
    assert!(!expected.contains(&request.transmit_graph));
    assert_eq!(DENOISE_CREATES.get(), 2);
    assert!(native.receive_noise_reduction.is_some());
}

#[test]
fn explicit_native_profiles_preserve_unconfigured_roles_and_require_denoise_when_enabled() {
    for local_present in [false, true] {
        let mut local = ProcessingChain::shipped(ChainRole::LocalReceive);
        local.receive.pl_filter = PlFilter::Disabled;
        let request = ExplicitNativeProcessingPlan {
            local: local_present.then_some(local),
            voice_telemetry: (!local_present)
                .then(|| ProcessingChain::shipped(ChainRole::VoiceTelemetry)),
            receive_input_gain: 1.0,
            receive_graph: "volume=0.4".to_owned(),
            transmit_graph: "volume=0.5".to_owned(),
            receive_deemphasis: true,
            receive_deemphasis_corner_hz: 250.0,
            transmit_preemphasis: false,
            transmit_preemphasis_corner_hz: 300.0,
            receive_output_gain_db: -7,
            receive_ctcss_mask: 1,
            transmit_dcs: false,
        };
        GRAPH_DESCRIPTIONS.with(|values| values.borrow_mut().clear());
        let generation = NativeProcessingFactory::prepare_explicit(
            graph_provider(&GRAPH_DESCRIPTOR),
            None,
            "",
            &request,
            960,
        )
        .unwrap();
        let descriptions = GRAPH_DESCRIPTIONS.with(|values| values.borrow().clone());
        assert_eq!(
            descriptions.contains(&request.receive_graph),
            !local_present
        );
        assert_eq!(
            descriptions.contains(&"volume=-7dB".to_owned()),
            !local_present
        );
        assert_eq!(
            descriptions.contains(&request.transmit_graph),
            local_present
        );
        assert_eq!(
            generation.receive_ctcss_tail_notch.is_some(),
            !local_present
        );
        assert!(descriptions.iter().any(|graph| {
            graph
                == &GraphDescriptionFactory::new("")
                    .unwrap()
                    .receive_deemphasis(true, 250.0)
                    .unwrap()
        }));
        if local_present {
            let mut invalid = request;
            invalid.local.as_mut().unwrap().rnnoise_enabled = true;
            assert!(matches!(
                NativeProcessingFactory::prepare_explicit(
                    graph_provider(&GRAPH_DESCRIPTOR),
                    None,
                    "",
                    &invalid,
                    960
                ),
                Err(ProcessingRuntimeError::DenoiseAdapter(
                    DenoiseError::IncompatibleAdapter
                ))
            ));
        }
    }
}

#[test]
fn graph_description_accessor_exposes_the_validated_factory() {
    assert_eq!(
        factory()
            .graph_descriptions()
            .receive_deemphasis(false, 300.0)
            .unwrap(),
        "[in]anull[out]"
    );
}

thread_local! {
    static GRAPH_CREATES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESSES: Cell<usize> = const { Cell::new(0) };
    static GRAPH_DESTROYS: Cell<usize> = const { Cell::new(0) };
    static GRAPH_PROCESS_FAILS: Cell<bool> = const { Cell::new(false) };
    static GRAPH_FAIL_GAIN: Cell<Option<f32>> = const { Cell::new(None) };
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
    if frame_count > state.maximum_frame_count
        || GRAPH_PROCESS_FAILS.get()
        || GRAPH_FAIL_GAIN.get() == Some(state.gain)
    {
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

fn explicit_raw_plan() -> ExplicitNativeProcessingPlan {
    ExplicitNativeProcessingPlan {
        local: None,
        voice_telemetry: None,
        receive_input_gain: 1.0,
        receive_graph: "highpass=f=60".to_owned(),
        transmit_graph: "volume=-2dB".to_owned(),
        receive_deemphasis: false,
        receive_deemphasis_corner_hz: 300.0,
        transmit_preemphasis: false,
        transmit_preemphasis_corner_hz: 300.0,
        receive_output_gain_db: 0,
        receive_ctcss_mask: 0,
        transmit_dcs: false,
    }
}

#[test]
fn explicit_replacement_keeps_unchanged_raw_stages_and_restores_exact_graphs() {
    let factory = NativeProcessingFactory::new_explicit(
        graph_provider(&GRAPH_DESCRIPTOR),
        None,
        "agc.so",
        960,
    )
    .unwrap();
    let original_plan = explicit_raw_plan();
    let original = factory.prepare(&original_plan).unwrap();
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    let unchanged =
        // SAFETY: all generations retain the same sole serial RX/TX owners.
        unsafe { factory.prepare_replacement(&original_plan, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 0);
    assert_eq!(GRAPH_PROCESSES.get(), 0);
    drop(unchanged);
    let mut changed = original_plan.clone();
    changed.receive_graph = "highpass=f=120".to_owned();
    let forward =
        // SAFETY: this replacement is prepared for those same serial owners.
        unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 1);
    assert_eq!(GRAPH_PROCESSES.get(), GRAPH_WARMUP_BLOCKS);
    assert_eq!(
        forward.transmit_program.0.primary.pointer(),
        original.transmit_program.0.primary.pointer()
    );
    assert_ne!(
        forward.receive_filter.0.primary.pointer(),
        original.receive_filter.0.primary.pointer()
    );
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    // SAFETY: rollback immediately follows its matching forward generation.
    let restored = unsafe { factory.prepare_restore(&original_plan, &original, &forward) }.unwrap();
    assert_eq!(
        restored.receive_filter.0.primary.pointer(),
        original.receive_filter.0.primary.pointer()
    );
    assert_eq!(GRAPH_CREATES.get(), 0);
    assert_eq!(GRAPH_PROCESSES.get(), 0);
}

#[test]
fn raw_receive_gain_changes_reset_only_receive_history_and_rollback_epoch() {
    let factory = factory();
    for gain in [0.125, 0.0] {
        let original_plan = explicit_raw_plan();
        let original = factory.prepare(&original_plan).unwrap();
        let original_rx = original.receive_loudness().unwrap().epoch;
        let original_tx = original.transmit_loudness().unwrap().epoch;
        let unchanged =
            // SAFETY: the unchanged snapshot retains the original serial owners.
            unsafe { factory.prepare_replacement(&original_plan, &original_plan, &original) }
                .unwrap();
        assert_eq!(unchanged.receive_loudness().unwrap().epoch, original_rx);
        let mut changed = original_plan.clone();
        changed.receive_input_gain = gain;
        GRAPH_CREATES.set(0);
        let forward =
            // SAFETY: the gain-only update retains those same RX/TX owners.
            unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
        let forward_rx = forward.receive_loudness().unwrap().epoch;
        assert_ne!(forward_rx, original_rx);
        assert_eq!(forward.transmit_loudness().unwrap().epoch, original_tx);
        assert_eq!(forward.receive_loudness().unwrap().sample_count, 0);
        assert_eq!(GRAPH_CREATES.get(), 0);
        let restored =
            // SAFETY: rollback immediately follows this matching gain-only update.
            unsafe { factory.prepare_restore(&original_plan, &original, &forward) }.unwrap();
        assert_ne!(restored.receive_loudness().unwrap().epoch, forward_rx);
        assert_ne!(restored.receive_loudness().unwrap().epoch, original_rx);
        assert_eq!(restored.transmit_loudness().unwrap().epoch, original_tx);
        assert_eq!(
            restored.receive_input_gain,
            original_plan.receive_input_gain
        );
        assert_eq!(GRAPH_CREATES.get(), 0);
    }
}

#[test]
fn explicit_replacement_supports_profile_notch_dcs_and_denoise_transitions() {
    let factory = factory();
    let original_plan = explicit_raw_plan();
    let original = factory.prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    let mut local = ProcessingChain::shipped(ChainRole::LocalReceive);
    local.rnnoise_enabled = true;
    local.receive.pl_filter = PlFilter::DecodedToneNotch;
    changed.local = Some(local);
    changed.voice_telemetry = Some(ProcessingChain::shipped(ChainRole::VoiceTelemetry));
    changed.receive_ctcss_mask = 1;
    changed.transmit_dcs = true;
    let forward =
        // SAFETY: the replacement retains the same serial audio owners.
        unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    assert!(forward.receive_noise_reduction.is_some());
    assert_eq!(
        forward
            .receive_ctcss_notch
            .iter()
            .filter(|notch| notch.is_some())
            .count(),
        1
    );
    assert!(forward.receive_ctcss_tail_notch.is_some());
    assert_ne!(
        forward.transmit_dcs_normal_filter.0.primary.pointer(),
        original.transmit_dcs_normal_filter.0.primary.pointer()
    );
    // SAFETY: the inverse immediately follows its matching replacement.
    let restored = unsafe { factory.prepare_restore(&original_plan, &original, &forward) }.unwrap();
    assert!(restored.receive_noise_reduction.is_none());
    assert!(restored.receive_ctcss_notch.iter().all(Option::is_none));
    assert_eq!(
        restored.transmit_dcs_normal_filter.0.primary.pointer(),
        original.transmit_dcs_normal_filter.0.primary.pointer()
    );
}

#[test]
fn only_local_and_voice_telemetry_roles_are_accepted() {
    let mut invalid = plan();
    invalid.local.role = ChainRole::Link;
    assert!(matches!(
        validate_plan(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    assert!(matches!(
        factory().prepare(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    invalid = plan();
    invalid.voice_telemetry.role = ChainRole::Link;
    assert!(matches!(
        validate_plan(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    assert!(matches!(
        factory().prepare(&invalid),
        Err(ProcessingRuntimeError::IncorrectChainRole)
    ));
    assert!(validate_plan(plan()).is_ok());
}

#[test]
fn preparation_owns_only_the_processors_selected_by_the_plan() {
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    GRAPH_DESTROYS.set(0);
    let mut generation = factory().prepare(plan()).unwrap();
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
    let (context, process) = match &generation.transmit_loudness {
        Some(stage) => (
            NonNull::from(stage.as_ref()).cast().as_ptr(),
            metered_graph_process as unsafe extern "C" fn(_, _, _, _) -> _,
        ),
        None => (
            generation.transmit_program.pointer().cast().as_ptr(),
            graph_process as unsafe extern "C" fn(_, _, _, _) -> _,
        ),
    };
    assert_eq!(
        // SAFETY: the test owns the TX processor serially and supplies exact spans.
        unsafe {
            process(
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

#[test]
fn transmit_meter_excludes_warmup_and_measures_final_program_without_changing_it() {
    let settings = plan();
    let mut generation = factory().prepare(settings).unwrap();
    assert_eq!(generation.transmit_loudness().unwrap().sample_count, 0);
    let mut expected = Vec::new();
    let mut actual = Vec::new();
    for block in 0..50 {
        let input: Vec<f32> = (0..960)
            .map(|i| {
                (std::f64::consts::TAU * 1000.0 * (block * 960 + i) as f64 / 48_000.0).sin() as f32
                    * 0.1
            })
            .collect();
        expected.extend_from_slice(&input);
        actual.extend(process_transmit(&mut generation, &input));
    }
    assert_eq!(actual, expected);
    let measured = generation.transmit_loudness().unwrap();
    assert_eq!(measured.sample_count, 48_000);
    assert!((measured.integrated_lufs.unwrap() + 23.05).abs() < 0.1);
    assert!((measured.true_peak_dbtp.unwrap() + 20.0).abs() < 0.1);
    for _ in 0..50 {
        process_transmit(&mut generation, &[0.0; 960]);
    }
    let paused = generation.transmit_loudness().unwrap();
    assert_eq!(paused.sample_count, 96_000);
    assert_eq!(paused.epoch, measured.epoch);
    assert!(paused.integrated_lufs.unwrap() > measured.integrated_lufs.unwrap() - 1.0);
    assert_eq!(paused.true_peak_dbtp, measured.true_peak_dbtp);
}

#[test]
fn transmit_meter_epoch_follows_transmit_graph_and_cannot_be_disabled() {
    let original_plan = plan();
    let mut original = factory().prepare(original_plan.clone()).unwrap();
    process_transmit(&mut original, &[0.1; 960]);
    let first = original.transmit_loudness().unwrap();
    let mut receive_plan = original_plan.clone();
    receive_plan.local.input_gain_db += 1.0;
    let receive =
        // SAFETY: replacements stay with this test's original serial owners.
        unsafe { factory().prepare_replacement(&receive_plan, &original_plan, &original) }.unwrap();
    assert_eq!(receive.transmit_loudness().unwrap().epoch, first.epoch);
    assert_eq!(receive.transmit_loudness().unwrap().sample_count, 960);
    let mut transmit_plan = receive_plan.clone();
    transmit_plan.voice_telemetry.input_gain_db += 1.0;
    let transmit =
        // SAFETY: same sole owners and matching preceding plan.
        unsafe { factory().prepare_replacement(&transmit_plan, &receive_plan, &receive) }.unwrap();
    let changed = transmit.transmit_loudness().unwrap();
    assert_ne!(changed.epoch, first.epoch);
    assert_eq!(changed.sample_count, 0);
    let restored =
        // SAFETY: this inverse immediately follows its matching forward update.
        unsafe { factory().prepare_restore(&receive_plan, &receive, &transmit) }.unwrap();
    assert_ne!(restored.transmit_loudness().unwrap().epoch, first.epoch);
    let mut bypass_plan = original_plan.clone();
    bypass_plan.voice_telemetry.enabled = false;
    bypass_plan.voice_telemetry.peak_limiter.enabled = false;
    let bypassed =
        // SAFETY: the same serial owners retain the corresponding stages.
        unsafe { factory().prepare_replacement(&bypass_plan, &original_plan, &original) }.unwrap();
    assert!(bypassed.transmit_loudness().is_some());
    assert!(bypassed.receive_loudness().is_some());
}

#[test]
fn transmit_meter_does_not_count_rejected_blocks_or_setup_samples() {
    let settings = plan();
    let mut generation = factory().prepare(&settings).unwrap();
    {
        let _ports = generation.session_ports(ProgramRingPort::silence());
    }
    let context = NonNull::from(generation.transmit_loudness.as_ref().unwrap().as_ref())
        .cast()
        .as_ptr();
    let input = [0.1_f32; 961];
    let mut output = [0.0_f32; 961];
    // SAFETY: all non-null spans cover the supplied counts; invalid arguments
    // are deliberately rejected before those pointers are dereferenced.
    unsafe {
        assert_eq!(
            metered_graph_process(ptr::null_mut(), input.as_ptr(), output.as_mut_ptr(), 1),
            PORT_ERROR
        );
        assert_eq!(
            metered_graph_process(context, ptr::null(), output.as_mut_ptr(), 1),
            PORT_ERROR
        );
        assert_eq!(
            metered_graph_process(context, input.as_ptr(), ptr::null_mut(), 1),
            PORT_ERROR
        );
        assert_eq!(
            metered_graph_process(context, input.as_ptr(), output.as_mut_ptr(), 0),
            PORT_ERROR
        );
        assert_eq!(
            metered_graph_process(context, input.as_ptr(), output.as_mut_ptr(), 961),
            PORT_ERROR
        );
    }
    assert_eq!(generation.transmit_loudness().unwrap().sample_count, 0);
    // Deliberately mismatch the private analysis bound to exercise its defensive
    // error path while the public graph still accepts this complete block.
    let stage = generation.transmit_loudness.as_ref().unwrap();
    // SAFETY: no callback is running; this test exclusively owns the TX stage.
    unsafe {
        *stage.meter.get() = OutputLoudnessMeter::new(48_000, 1).unwrap();
    }
    assert_eq!(
        // SAFETY: both spans contain at least two samples and this is the sole owner.
        unsafe { metered_graph_process(context, input.as_ptr(), output.as_mut_ptr(), 2) },
        PORT_ERROR
    );
    let plan: ExplicitNativeProcessingPlan = settings.into();
    let invalid = factory()
        .prepare(plan.clone())
        .unwrap()
        .with_output_meters(&plan, 0, None);
    let error = invalid.err().unwrap();
    assert!(matches!(error, ProcessingRuntimeError::Loudness(_)));
    assert!(error.to_string().contains("output meter setup failed"));
}

#[test]
fn tiny_graph_warmup_exercises_at_least_sixty_four_input_samples() {
    GRAPH_CREATES.set(0);
    GRAPH_PROCESSES.set(0);
    let _generation = factory_with_maximum(1).prepare(plan()).unwrap();
    assert_eq!(GRAPH_PROCESSES.get(), GRAPH_CREATES.get() * 64);
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
    let fade = process_denoise(&mut generation, &[0.5; 64]);
    for (index, sample) in fade[..48].iter().enumerate() {
        let expected = 0.5 - 0.25 * (index + 1) as f32 / 48.0;
        assert!((*sample - expected).abs() < 0.0001);
    }
    assert_eq!(&fade[48..], &[0.25; 16]);
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
fn tiny_limiter_handoff_primes_the_resampled_filter_before_its_48_frame_fade() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    let factory = factory_with_maximum(1);
    let mut original_plan = plan();
    original_plan.preemphasis_enabled = false;
    original_plan.voice_telemetry.enabled = false;
    original_plan.voice_telemetry.input_gain_db = 0.0;
    original_plan.voice_telemetry.peak_limiter.enabled = true;
    original_plan.voice_telemetry.peak_limiter.lookahead_ms = 5.0;
    let mut original = factory.prepare(&original_plan).unwrap();
    for _ in 0..300 {
        process_transmit(&mut original, &[0.5]);
    }
    let mut changed = original_plan.clone();
    changed.voice_telemetry.output_gain_db = -6.020_599_913;
    let mut replacement =
        // SAFETY: both generations retain this test's original serial TX owner.
        unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    for frame in 0..(240 + 256) {
        assert_eq!(
            process_transmit(&mut replacement, &[0.5]),
            [0.5],
            "prime frame {frame}"
        );
    }
    for frame in 1..=48 {
        let expected = 0.5 - 0.25 * frame as f32 / 48.0;
        assert!((process_transmit(&mut replacement, &[0.5])[0] - expected).abs() < 0.0001);
    }
    assert!((process_transmit(&mut replacement, &[0.5])[0] - 0.25).abs() < 0.0001);
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
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
            original_plan.voice_telemetry.peak_limiter.enabled = true;
            original_plan.voice_telemetry.peak_limiter.lookahead_ms = lookahead_ms;
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
    active_plan.voice_telemetry.peak_limiter.enabled = true;
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
            Arc::strong_count(&stage.handoff.as_ref().unwrap().previous.current.0),
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
        Arc::strong_count(&stage.handoff.as_ref().unwrap().previous.current.0),
    );
    assert_eq!(after, before);
    assert_eq!(GRAPH_DESTROYS.get(), 0);
    GRAPH_SIMULATE_LOOKAHEAD.set(false);
}

#[test]
fn predecessor_graph_failure_is_reported_without_publishing_a_transition() {
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
        // SAFETY: the replacement keeps the same serial TX owner.
        unsafe { factory.prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    let before = replacement
        .transmit_program
        .0
        .audible
        .load(Ordering::Acquire);
    GRAPH_FAIL_GAIN.set(Some(1.0));
    let mut output = [0.0; 1];
    let context = replacement.transmit_program.pointer().cast().as_ptr();
    assert_eq!(
        // SAFETY: this test owns the live graph and exact input/output spans.
        unsafe { graph_process(context, [1.0].as_ptr(), output.as_mut_ptr(), 1) },
        PORT_ERROR
    );
    GRAPH_FAIL_GAIN.set(None);
    assert_eq!(
        replacement
            .transmit_program
            .0
            .audible
            .load(Ordering::Acquire),
        before
    );
    assert_eq!(process_transmit(&mut replacement, &[1.0; 1]), [1.0]);
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
    original_plan.voice_telemetry.peak_limiter.enabled = true;
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
fn replacement_of_an_unpublished_generation_leases_its_newly_audible_pending_graph() {
    GRAPH_SIMULATE_LOOKAHEAD.set(true);
    let factory = factory_with_maximum(64);
    let mut original_plan = plan();
    original_plan.preemphasis_enabled = false;
    original_plan.voice_telemetry.enabled = false;
    original_plan.voice_telemetry.input_gain_db = 0.0;
    let original = factory.prepare(&original_plan).unwrap();
    let mut first_plan = original_plan.clone();
    first_plan.voice_telemetry.output_gain_db = -6.020_599_913;
    let mut first =
        // SAFETY: each candidate keeps the same serial TX owner.
        unsafe { factory.prepare_replacement(&first_plan, &original_plan, &original) }.unwrap();
    let mut second_plan = first_plan.clone();
    second_plan.voice_telemetry.output_gain_db = -12.041_199_827;
    let second =
        // SAFETY: the first generation remains active during preparation.
        unsafe { factory.prepare_replacement(&second_plan, &first_plan, &first) }.unwrap();
    process_transmit(&mut first, &[1.0; 64]);
    process_transmit(&mut first, &[1.0; 64]);
    let mut third_plan = second_plan.clone();
    third_plan.voice_telemetry.output_gain_db = -18.061_799_740;
    let mut third =
        // SAFETY: the unpublished second generation is superseded under the same owner.
        unsafe { factory.prepare_replacement(&third_plan, &second_plan, &second) }.unwrap();
    assert_eq!(process_transmit(&mut third, &[1.0; 1]), [0.5]);
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
        original_plan.voice_telemetry.peak_limiter.enabled = true;
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
fn each_changed_receive_graph_propagates_adapter_preparation_failure() {
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
        GRAPH_CREATE_FAIL_AT.set(1);
        assert!(matches!(
            // SAFETY: the failed candidate is never published to a callback.
            unsafe { factory().prepare_replacement(&changed, &original_plan, &original) },
            Err(ProcessingRuntimeError::GraphAdapter(_))
        ));
        GRAPH_CREATE_FAIL_AT.set(0);
    }
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
fn enabled_notches_and_denoiser_keep_their_owners_across_replacement_and_restore() {
    let mut original_plan = plan();
    original_plan.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    original_plan.local.rnnoise_enabled = true;
    let original = factory().prepare(&original_plan).unwrap();
    let mut changed = original_plan.clone();
    changed.local.output_gain_db = -3.0;
    GRAPH_CREATES.set(0);
    DENOISE_CREATES.set(0);
    let replacement =
        // SAFETY: both generations remain under this same serial RX owner.
        unsafe { factory().prepare_replacement(&changed, &original_plan, &original) }.unwrap();
    assert_eq!(GRAPH_CREATES.get(), 1);
    assert_eq!(DENOISE_CREATES.get(), 0);
    assert_eq!(
        original.receive_ctcss_notch[0].as_ref().unwrap().pointer(),
        replacement.receive_ctcss_notch[0]
            .as_ref()
            .unwrap()
            .pointer()
    );
    assert_eq!(
        original.receive_noise_reduction.as_ref().unwrap().pointer(),
        replacement
            .receive_noise_reduction
            .as_ref()
            .unwrap()
            .pointer()
    );
    let restored =
        // SAFETY: this inverse immediately follows the forward generation.
        unsafe { factory().prepare_restore(&original_plan, &original, &replacement) }.unwrap();
    assert_eq!(
        original.receive_ctcss_notch[0].as_ref().unwrap().pointer(),
        restored.receive_ctcss_notch[0].as_ref().unwrap().pointer()
    );
    assert_eq!(
        original.receive_noise_reduction.as_ref().unwrap().pointer(),
        restored.receive_noise_reduction.as_ref().unwrap().pointer()
    );
}

#[test]
fn newly_enabled_notch_bank_propagates_tone_and_tail_setup_failures() {
    let original_plan = plan();
    let original = factory().prepare(&original_plan).unwrap();
    let mut enabled = original_plan.clone();
    enabled.local.receive.pl_filter = PlFilter::DecodedToneNotch;
    for failed_ordinal in [1, CTCSS_TONE_COUNT + 1] {
        GRAPH_CREATES.set(0);
        GRAPH_CREATE_FAIL_AT.set(failed_ordinal);
        assert!(matches!(
            // SAFETY: a failed candidate is never published to an audio owner.
            unsafe { factory().prepare_replacement(&enabled, &original_plan, &original) },
            Err(ProcessingRuntimeError::GraphAdapter(_))
        ));
        GRAPH_CREATE_FAIL_AT.set(0);
    }
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
    let graph_error = factory().prepare(plan()).err().unwrap();
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
    let transmit_graph_error = factory().prepare(plan()).err().unwrap();
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
        factory().prepare(plan()),
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

#[test]
fn mandatory_receive_meter_tracks_complete_path_and_independent_transmit_history() {
    let mut selected = plan();
    let mut current = factory().prepare(&selected).unwrap();
    let rx_epoch = current.receive_loudness().unwrap().epoch;
    let tx_epoch = current.transmit_loudness().unwrap().epoch;
    assert_ne!(rx_epoch, tx_epoch);
    assert_eq!(current.receive_loudness().unwrap().sample_count, 0);
    let input = [0.1; 960];
    let mut output = [0.0; 960];
    for _ in 0..50 {
        let stage = current.receive_loudness.as_ref().unwrap();
        assert_eq!(
            // SAFETY: this test is the sole RX owner and supplies complete spans.
            unsafe {
                metered_graph_process(
                    NonNull::from(stage.as_ref()).cast().as_ptr(),
                    input.as_ptr(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            PORT_OK,
        );
    }
    assert_eq!(current.receive_loudness().unwrap().sample_count, 48_000);
    assert_eq!(current.transmit_loudness().unwrap().sample_count, 0);
    let mut next = selected.clone();
    next.voice_telemetry.output_gain_db -= 1.0;
    current =
        // SAFETY: each replacement retains these original serial owners.
        unsafe { factory().prepare_replacement(&next, &selected, &current) }.unwrap();
    assert_eq!(current.receive_loudness().unwrap().epoch, rx_epoch);
    assert_eq!(current.receive_loudness().unwrap().sample_count, 48_000);
    let tx_epoch = current.transmit_loudness().unwrap().epoch;
    selected = next;
    let changes: [fn(&mut NativeProcessingPlan); 4] = [
        |p| p.local.input_gain_db += 1.0,
        |p| p.local.receive.bandpass_lowpass_hz -= 500.0,
        |p| p.local.rnnoise_enabled = true,
        |p| p.local.receive.pl_filter = PlFilter::DecodedToneNotch,
    ];
    for change in changes {
        let previous_rx = current.receive_loudness().unwrap().epoch;
        let mut next = selected.clone();
        change(&mut next);
        current =
            // SAFETY: same factory and same sole RX/TX owners for this test.
            unsafe { factory().prepare_replacement(&next, &selected, &current) }.unwrap();
        assert_ne!(current.receive_loudness().unwrap().epoch, previous_rx);
        assert_eq!(current.receive_loudness().unwrap().sample_count, 0);
        assert_eq!(current.transmit_loudness().unwrap().epoch, tx_epoch);
        selected = next;
    }
    let raw = factory().prepare(explicit_raw_plan()).unwrap();
    assert!(raw.receive_loudness().is_some());
    assert!(raw.transmit_loudness().is_some());
}
