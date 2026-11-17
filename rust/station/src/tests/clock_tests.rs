use super::*;

// C3 uses the real station/graph/final gate with an independent old audio prefix.
thread_local! {
    static CLOCK_NOW: std::cell::Cell<u64> = const { std::cell::Cell::new(100_000_000) };
    static CLOCK_STEP: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
    static CLOCK_CALLS: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

unsafe extern "C" fn clock_now() -> u64 {
    CLOCK_CALLS.set(CLOCK_CALLS.get() + 1);
    let now = CLOCK_NOW.get();
    CLOCK_NOW.set(now.saturating_add(CLOCK_STEP.get()));
    now
}

#[repr(C)]
struct ClockAudioDescriptor {
    base: AudioDescriptor,
    alsa: Option<Unused>,
    timed_create: Option<Unused>,
    now: Option<unsafe extern "C" fn() -> u64>,
}

fn clock_runtime() -> (StationRuntime, StationControlHost) {
    clock_runtime_generation(25)
}

fn clock_runtime_generation(generation: u64) -> (StationRuntime, StationControlHost) {
    assert_eq!(size_of::<AudioDescriptor>(), 192);
    assert_eq!(size_of::<ClockAudioDescriptor>(), 216);
    // SAFETY: this immutable fixture table contains only Copy scalar/pointer fields.
    let mut base = unsafe { ptr::read(ptr::from_ref(&AUDIO_DESCRIPTOR)) };
    base.struct_size = 216;
    let descriptor = Box::leak(Box::new(ClockAudioDescriptor {
        base,
        alsa: None,
        timed_create: None,
        now: Some(clock_now),
    }));
    let provider =
        // SAFETY: the fixture table and functions remain live for this process.
        unsafe { AudioProvider::from_raw_descriptor(ptr::from_ref(descriptor).cast()) }.unwrap();
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, generation);
    let selected = selected(&prepared, ADVANCED_FRAME_SAMPLES as u32);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 4 },
        )
        .unwrap();
    StationRuntime::open(media, &selected, provider).unwrap()
}

#[derive(Default)]
struct ClockProbe {
    calls: u32,
    size: u32,
    clock: (u32, u32, u64),
    samples: [u64; 8],
    frames: [u32; 8],
}

unsafe extern "C" fn clock_observed(
    raw: *mut c_void,
    value: *const usbradioplus_runtime::ProgrammeObservationV1,
) -> u32 {
    // SAFETY: caller retains this probe; the fixed prefix is readable in both cohorts.
    let (probe, size) = unsafe { (&mut *raw.cast::<ClockProbe>(), (*value).struct_size) };
    probe.calls += 1;
    probe.size = size;
    probe.clock = (0, 0, 0);
    if size >= 112 {
        let bytes = value.cast::<u8>();
        // SAFETY: advertised complete optional tail, without forming a new-layout reference.
        probe.clock = unsafe {
            (
                bytes.add(96).cast::<u32>().read_unaligned(),
                bytes.add(100).cast::<u32>().read_unaligned(),
                bytes.add(104).cast::<u64>().read_unaligned(),
            )
        };
    }
    if probe.calls <= 8 {
        probe.samples[probe.calls as usize - 1] = probe.clock.2;
        // SAFETY: frame_count belongs to the unchanged readable prefix.
        probe.frames[probe.calls as usize - 1] = unsafe { (*value).frame_count };
    }
    1
}

fn clock_bind(runtime: &mut StationRuntime, probe: &mut ClockProbe) {
    runtime.hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..Default::default()
    });
    // SAFETY: each fixture retains probe/code through stopped unbind.
    unsafe {
        runtime.bind_programme_fence(Some(usbradioplus_runtime::ProgrammeFence::Observed {
            context: ptr::from_mut(probe).cast(),
            permit: clock_observed,
        }))
    }
    .unwrap();
}

fn clock_render(runtime: &mut StationRuntime) {
    let mut output = [0.0; 1920];
    assert_eq!(
        // SAFETY: fake adapter has no background thread, and this is its sole TX owner.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                960,
            )
        },
        0
    );
}

#[test]
fn stage_c_clock_observation_order() {
    CLOCK_CALLS.set(0);
    CLOCK_NOW.set(100_000_000);
    let (mut runtime, _control) = clock_runtime();
    let mut probe = ClockProbe::default();
    clock_bind(&mut runtime, &mut probe);
    // A deterministic in-graph stall changes the clock after callback entry.
    crate::control::tests::during_next_graph(|| CLOCK_NOW.set(1_000_000_000));
    runtime.start().unwrap();
    assert_eq!(
        probe.clock,
        (1, 1, 1_000_000_000),
        "the actual final permit needs a fresh post-graph clock, not callback entry"
    );
    assert_eq!(probe.size, 112);
    clock_render(&mut runtime);
    assert_eq!(CLOCK_CALLS.get(), probe.calls);
    runtime.stop().unwrap();
    // SAFETY: confirmed stop before releasing callback state.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
}

#[test]
fn stage_c_clock_preentry_stall_and_each_split_reads_fresh() {
    CLOCK_CALLS.set(0);
    CLOCK_STEP.set(1);
    CLOCK_NOW.set(100_000_000);
    let (mut runtime, _control) = clock_runtime();
    let mut probe = ClockProbe::default();
    clock_bind(&mut runtime, &mut probe);
    // Pre-entry pause: no preparation/entry timestamp may survive as final now.
    CLOCK_NOW.set(1_000_000_000);
    crate::media::tests::CLOCK_GRAPH_SPLIT.set(240);
    runtime.start().unwrap();
    crate::media::tests::CLOCK_GRAPH_SPLIT.set(0);
    CLOCK_STEP.set(0);
    assert_eq!(probe.calls, 4);
    assert_eq!(CLOCK_CALLS.get(), 4);
    assert_eq!(
        &probe.samples[..4],
        &[1_000_000_000, 1_000_000_001, 1_000_000_002, 1_000_000_003]
    );
    assert_eq!(&probe.frames[..4], &[240; 4]);
    runtime.stop().unwrap();
    // SAFETY: confirmed stop before releasing the fixture callback state.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
}

#[test]
fn stage_c_clock_faults_latch_through_graph_update_and_failed_stop() {
    for bad in [0, 99] {
        CLOCK_CALLS.set(0);
        CLOCK_NOW.set(100);
        let (mut runtime, mut control) = clock_runtime();
        let mut probe = ClockProbe::default();
        clock_bind(&mut runtime, &mut probe);
        runtime.start().unwrap();
        assert_eq!(probe.clock, (1, 1, 100));
        clock_render(&mut runtime); // equality is valid.
        assert_eq!(probe.clock, (1, 1, 100));
        CLOCK_NOW.set(101);
        clock_render(&mut runtime);
        assert_eq!(probe.clock, (1, 1, 101));
        CLOCK_NOW.set(bad);
        clock_render(&mut runtime);
        assert_eq!(probe.clock, (0, 0, 0));
        let reads = CLOCK_CALLS.get();
        CLOCK_NOW.set(200);
        runtime.start().unwrap(); // started no-op must not reset.
        clock_render(&mut runtime);
        assert_eq!(probe.clock, (0, 0, 0));
        let mut config = control.plan().configuration().clone();
        config.voice_telemetry.output_gain_db = -3.0;
        let plan = StationPlan::new("usb", config, control.plan().transport(), 25, 960).unwrap();
        let update = runtime
            .prepare_update(&control, plan, &factory(), radio_provider())
            .unwrap();
        runtime.begin_update(&control, update).unwrap();
        update_tick(&mut runtime, false);
        update_tick(&mut runtime, true);
        runtime.finish_update(&mut control, true).unwrap();
        clock_render(&mut runtime);
        assert_eq!(
            probe.clock,
            (0, 0, 0),
            "graph replacement cannot reset the serial owner"
        );
        PARTIAL_STOP_ERROR.set(true);
        assert!(runtime.stop().is_err());
        // SAFETY: a failed stop must reject releasing any still-borrowed callback.
        assert!(unsafe { runtime.bind_programme_fence(None) }.is_err());
        runtime.start().unwrap();
        clock_render(&mut runtime);
        assert_eq!(probe.clock, (0, 0, 0));
        assert_eq!(
            CLOCK_CALLS.get(),
            reads,
            "sticky fault performs no further provider reads"
        );
        PARTIAL_STOP_ERROR.set(false);
        runtime.stop().unwrap();
        CLOCK_NOW.set(50);
        runtime.start().unwrap();
        assert_eq!(
            probe.clock,
            (1, 1, 50),
            "confirmed quiescence permits owner reset"
        );
        CLOCK_NOW.set(u64::MAX);
        clock_render(&mut runtime);
        assert_eq!(
            probe.clock,
            (1, 1, u64::MAX),
            "raw representable value needs no arithmetic"
        );
        CLOCK_NOW.set(0); // C1 maps any conversion overflow to this unavailable sentinel.
        clock_render(&mut runtime);
        assert_eq!(probe.clock, (0, 0, 0));
        runtime.stop().unwrap();
        // SAFETY: confirmed quiescence before releasing the borrowed probe.
        unsafe { runtime.bind_programme_fence(None) }.unwrap();
    }
}

#[test]
fn stage_c_clock_reader_retained_through_failed_start_and_stop_retry() {
    CLOCK_CALLS.set(0);
    CLOCK_NOW.set(100);
    let (mut runtime, _control) = clock_runtime();
    let mut probe = ClockProbe::default();
    clock_bind(&mut runtime, &mut probe);
    PARTIAL_START_ERROR.set(true);
    PARTIAL_STOP_ERROR.set(true);
    assert!(runtime.start().is_err());
    assert!(runtime.running());
    assert_eq!(
        probe.clock,
        (1, 1, 100),
        "actual startup callback used retained provider reader"
    );
    assert!(runtime.stop().is_err());
    CLOCK_NOW.set(101);
    clock_render(&mut runtime);
    assert_eq!(
        probe.clock,
        (1, 1, 101),
        "failed rollback retains actual SDK reader"
    );
    // SAFETY: failed stop cannot release the borrowed observed callback.
    assert!(unsafe { runtime.bind_programme_fence(None) }.is_err());
    PARTIAL_STOP_ERROR.set(false);
    runtime.stop().unwrap();
    PARTIAL_START_ERROR.set(false);
    CLOCK_NOW.set(1);
    runtime.start().unwrap();
    assert_eq!(probe.clock, (1, 1, 1));
    runtime.suspend().unwrap();
    CLOCK_NOW.set(2);
    runtime.start().unwrap();
    assert_eq!(
        probe.clock,
        (1, 1, 2),
        "reopen uses the same retained audio provider"
    );
    runtime.stop().unwrap();
    // SAFETY: no remaining callback can borrow the probe after successful unbind.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
    let reads = CLOCK_CALLS.get();
    drop(runtime);
    assert_eq!(
        CLOCK_CALLS.get(),
        reads,
        "quiescent destruction does not read provider clock"
    );
}

#[test]
fn stage_c_clock_independent_owners_old_consumer_and_skipped_graph() {
    let (mut first, _first_control) = clock_runtime();
    let (mut second, _second_control) = clock_runtime();
    let mut a = ClockProbe::default();
    let mut b = ClockProbe::default();
    clock_bind(&mut first, &mut a);
    clock_bind(&mut second, &mut b);
    CLOCK_NOW.set(1000);
    first.start().unwrap();
    CLOCK_NOW.set(10);
    second.start().unwrap();
    assert_eq!(
        b.clock,
        (1, 1, 10),
        "there is no global cross-owner watermark"
    );
    first.stop().unwrap();
    second.stop().unwrap();
    // SAFETY: both independent owners have quiesced before probe release.
    unsafe {
        first.bind_programme_fence(None).unwrap();
        second.bind_programme_fence(None).unwrap();
    }
    // New consumer safely reads an exactly old-size record and treats clock as Unknown.
    let mut old = [0_u64; 12];
    old[0] = (1_u64 << 32) | 96;
    // SAFETY: old record contains exactly the readable original prefix; helper checks extent first.
    unsafe { clock_observed(ptr::from_mut(&mut a).cast(), old.as_ptr().cast()) };
    assert_eq!(a.clock, (0, 0, 0));
    let (mut skipped, _control) = clock_runtime_generation(24);
    clock_bind(&mut skipped, &mut a);
    CLOCK_CALLS.set(0);
    let calls = a.calls;
    skipped.start().unwrap();
    clock_render(&mut skipped);
    assert_eq!((CLOCK_CALLS.get(), a.calls), (0, calls));
    skipped.stop().unwrap();
    // SAFETY: callback borrower is quiescent.
    unsafe { skipped.bind_programme_fence(None) }.unwrap();
}

#[test]
fn stage_c_clock_actual_serial_reader_is_allocation_free_including_fault() {
    CLOCK_CALLS.set(0);
    CLOCK_NOW.set(100);
    let (mut runtime, _control) = clock_runtime();
    let mut probe = ClockProbe::default();
    clock_bind(&mut runtime, &mut probe);
    // First/warmed actual callback and successful/failing reader paths all share
    // the existing allocator guard. No boxed test graph hook is armed here.
    for now in [100, 100, 101, 0, 102] {
        CLOCK_NOW.set(now);
        crate::allocation::without_heap(|| clock_render(&mut runtime));
    }
    assert_eq!(CLOCK_CALLS.get(), 4);
    assert_eq!(probe.clock, (0, 0, 0));
    runtime.stop().unwrap();
    // SAFETY: callback state is no longer borrowed after confirmed quiescence.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
}

#[test]
fn stage_c_clock_unbound_and_v1_never_read_optional_clock() {
    unsafe extern "C" fn permit(_: *mut c_void, _: u64, _: u32) -> u32 {
        1
    }
    let (mut runtime, _control) = clock_runtime();
    runtime.hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..Default::default()
    });
    CLOCK_CALLS.set(0);
    runtime.start().unwrap();
    clock_render(&mut runtime);
    runtime.stop().unwrap();
    // SAFETY: static legacy callback and no context, bound while quiescent.
    unsafe {
        runtime.bind_programme_fence(Some(usbradioplus_runtime::ProgrammeFence::Legacy {
            context: ptr::null_mut(),
            permit,
        }))
    }
    .unwrap();
    runtime.start().unwrap();
    clock_render(&mut runtime);
    assert_eq!(CLOCK_CALLS.get(), 0);
    runtime.stop().unwrap();
    // SAFETY: confirmed callback quiescence.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
}

#[test]
fn stage_c_clock_old_audio_provider_observation_is_unknown() {
    CLOCK_CALLS.set(0);
    let (mut runtime, _control) = runtime(25); // Exact retained 192-byte descriptor.
    let mut probe = ClockProbe::default();
    clock_bind(&mut runtime, &mut probe);
    runtime.start().unwrap();
    assert_eq!(probe.size, 112);
    assert_eq!(probe.clock, (0, 0, 0));
    assert_eq!(CLOCK_CALLS.get(), 0);
    runtime.stop().unwrap();
    // SAFETY: callback state is released only after confirmed stop.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
}
