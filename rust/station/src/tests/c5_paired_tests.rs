// Test-only Task 1 timing. Fixture DSP is not the production DSP configuration.
// All reporting and pair comparisons are outside C5_MEASURE's processor bracket.
type C5Reader = unsafe extern "C" fn() -> u64;
const C5_PAIRS: usize = 257;
const C5_READER_PAIRS: usize = 4097;

thread_local! {
    static C5_VALIDATION: std::cell::Cell<(u32, u32)> = const { std::cell::Cell::new((0, 0)) };
    static C5_RAW_READER: std::cell::Cell<Option<C5Reader>> = const { std::cell::Cell::new(None) };
}

fn c5_validate_graph(_: u32, successful: bool) {
    let (attempts, successes) = C5_VALIDATION.get();
    C5_VALIDATION.set((attempts + 1, successes + u32::from(successful)));
}

// A same-signature, non-counting control. This is reader substitution, not
// total observation overhead. black_box at the call site consumes its result.
unsafe extern "C" fn c5_scripted_raw() -> u64 {
    CLOCK_NOW.get()
}

unsafe extern "C" fn c5_counted_raw() -> u64 {
    CLOCK_CALLS.set(CLOCK_CALLS.get() + 1);
    // SAFETY: only count replays install a reader retained beyond the runtime.
    unsafe { C5_RAW_READER.get().unwrap()() }
}

unsafe extern "C" fn c5_minimal_legacy(_: *mut c_void, _: u64, _: u32) -> u32 {
    1
}

unsafe extern "C" fn c5_minimal_observed(
    _: *mut c_void,
    _: *const usbradioplus_runtime::ProgrammeObservationV1,
) -> u32 {
    1
}

#[derive(Default)]
struct C5Probe {
    base: ClockProbe,
    last: Option<usbradioplus_runtime::ProgrammeObservationV1>,
}

unsafe extern "C" fn c5_observed_validation(
    raw: *mut c_void,
    value: *const usbradioplus_runtime::ProgrammeObservationV1,
) -> u32 {
    // SAFETY: only the exact current test target calls this complete-record probe.
    let probe = unsafe { &mut *raw.cast::<C5Probe>() };
    // SAFETY: this callback receives the current complete observation.
    probe.last = Some(unsafe { *value });
    // SAFETY: retained base probe and the same current record.
    unsafe { clock_observed(ptr::from_mut(&mut probe.base).cast(), value) }
}

fn c5_semantics_equal(a: &C5Arm, b: &C5Arm) {
    let mut a = a.probe.last.unwrap();
    let mut b = b.probe.last.unwrap();
    // The selected reader intentionally changes only these three fields.
    a.clock_flags = 0;
    a.clock_domain = 0;
    a.observed_monotonic_ns = 0;
    b.clock_flags = 0;
    b.clock_domain = 0;
    b.observed_monotonic_ns = 0;
    assert_eq!(a, b, "non-clock observation fields are equivalent");
}

// Field order is intentional: even assertion unwinding destroys the sole
// native owner before its borrowed probe. No worker or real device is opened.
struct C5Arm {
    runtime: StationRuntime,
    _control: StationControlHost,
    probe: Box<C5Probe>,
}

impl C5Arm {
    fn new(generation: u64, reader: Option<C5Reader>, legacy: bool, validation: bool) -> Self {
        let (mut runtime, control) = clock_runtime_reader(generation, reader);
        let mut probe = Box::<C5Probe>::default();
        runtime.hardware.publish_requests(ControllerRequests {
            transmit: true,
            render_admitted: true,
            ..Default::default()
        });
        let fence = if legacy {
            usbradioplus_runtime::ProgrammeFence::Legacy {
                context: ptr::null_mut(),
                permit: c5_minimal_legacy,
            }
        } else {
            usbradioplus_runtime::ProgrammeFence::Observed {
                context: ptr::from_mut(probe.as_mut()).cast(),
                permit: if validation {
                    c5_observed_validation
                } else {
                    c5_minimal_observed
                },
            }
        };
        // SAFETY: quiescent bind; the runtime field is destroyed before probe.
        unsafe { runtime.bind_programme_fence(Some(fence)) }.unwrap();
        Self {
            runtime,
            _control: control,
            probe,
        }
    }

    fn render(&mut self, frames: u32) -> [f32; 1920] {
        self.render_status(frames, 0)
    }

    fn render_status(&mut self, frames: u32, expected_status: i32) -> [f32; 1920] {
        let mut output = [0.0; 1920];
        assert!(frames <= 960);
        crate::allocation::without_heap(|| {
            assert_eq!(
                // SAFETY: serial sole fake TX owner and exact writable stereo span.
                unsafe {
                    transmit_callback(
                        self.runtime._transmit_context.get().cast(),
                        output.as_mut_ptr(),
                        frames,
                    )
                },
                expected_status
            );
        });
        output
    }
}

// Reset all thread-local test instrumentation even after an assertion failure.
struct C5Reset;
impl Drop for C5Reset {
    fn drop(&mut self) {
        crate::media::tests::C5_MEASURE.set(None);
        crate::media::tests::C5_VALIDATE.set(None);
        crate::media::tests::CLOCK_GRAPH_SPLIT.set(0);
        crate::control::tests::C5_FAIL_GRAPH.set(false);
        C5_RAW_READER.set(None);
        CLOCK_STEP.set(0);
    }
}

fn c5_prepare_storage(capacity: usize) -> C5Reset {
    C5_COSTS.with(|costs| *costs.borrow_mut() = Vec::with_capacity(capacity));
    C5_VALIDATION.set((0, 0));
    C5_RAW_READER.set(None);
    CLOCK_NOW.set(100);
    CLOCK_CALLS.set(0);
    CLOCK_STEP.set(0);
    crate::control::tests::c5_prepare_graph_tls();
    crate::media::tests::C5_VALIDATE.set(None);
    crate::media::tests::C5_MEASURE.set(None);
    crate::media::tests::CLOCK_GRAPH_SPLIT.set(0);
    crate::allocation::without_heap(|| {});
    let _ = std::time::Instant::now().elapsed();
    C5Reset
}

fn c5_require_counts(attempts: u32, successes: u32, observations: u32, reads: u32, arm: &C5Arm) {
    assert_eq!(
        C5_VALIDATION.get(),
        (attempts, successes),
        "independent attempted/successful graph count"
    );
    assert_eq!(
        arm.probe.base.calls, observations,
        "observed invocation count"
    );
    assert_eq!(CLOCK_CALLS.get(), reads, "reader invocation count");
    C5_COSTS.with(|costs| {
        assert_eq!(
            costs.borrow().len(),
            attempts as usize,
            "every attempted final graph must have a timing record"
        )
    });
}

#[test]
fn stage_c5_paired_count_oracle() {
    let _reset = c5_prepare_storage(8);
    let mut arm = C5Arm::new(25, Some(clock_now), false, true);
    crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
    crate::media::tests::C5_MEASURE.set(Some(c5_cost));
    crate::media::tests::CLOCK_GRAPH_SPLIT.set(240);
    arm.render(960);
    c5_require_counts(4, 4, 4, 4, &arm);
    C5_COSTS.with(|costs| assert!(costs.borrow().iter().all(|(frames, _)| *frames == 240)));
}

#[test]
#[should_panic(expected = "every attempted final graph must have a timing record")]
fn stage_c5_paired_missing_timing_hook_is_rejected() {
    let _reset = c5_prepare_storage(8);
    let mut arm = C5Arm::new(25, Some(clock_now), false, true);
    crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
    arm.render(960);
    c5_require_counts(1, 1, 1, 1, &arm);
}

#[test]
fn stage_c5_paired_failed_and_skipped_graph_counts() {
    let _reset = c5_prepare_storage(8);
    let mut arm = C5Arm::new(25, Some(clock_now), false, true);
    crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
    crate::media::tests::C5_MEASURE.set(Some(c5_cost));
    crate::control::tests::C5_FAIL_GRAPH.set(true);
    let failed = arm.render_status(960, -1);
    c5_require_counts(1, 0, 0, 0, &arm);
    assert!(failed.iter().all(|sample| *sample == 0.0));
    crate::control::tests::C5_FAIL_GRAPH.set(false);
    C5_COSTS.with(|costs| costs.borrow_mut().clear());
    C5_VALIDATION.set((0, 0));
    let mut skipped = C5Arm::new(24, Some(clock_now), false, true);
    skipped.render(960);
    c5_require_counts(0, 0, 0, 0, &skipped);
}

#[test]
fn stage_c5_paired_fault_count_replay() {
    for bad in [0, 99] {
        let _reset = c5_prepare_storage(8);
        let mut arm = C5Arm::new(25, Some(clock_now), false, true);
        crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
        crate::media::tests::C5_MEASURE.set(Some(c5_cost));
        for (index, now) in [100, 100, 101, bad, 200].into_iter().enumerate() {
            CLOCK_NOW.set(now);
            let output = arm.render(960);
            assert!(output.iter().any(|sample| *sample != 0.0));
            c5_require_counts(
                (index + 1) as u32,
                (index + 1) as u32,
                (index + 1) as u32,
                (index + 1).min(4) as u32,
                &arm,
            );
            assert_eq!(
                arm.probe.base.clock,
                if index < 3 { (1, 1, now) } else { (0, 0, 0) }
            );
        }
    }
}

fn c5_print_pairs(
    name: &str,
    frames: u32,
    partition: usize,
    pairs: &[([u64; 4], [u64; 4])],
    phase: &str,
) {
    let chunks = if partition == 0 {
        1
    } else {
        frames as usize / partition
    };
    for (pair, (a, b)) in pairs.iter().enumerate() {
        let order = if pair % 2 == 0 { "AB" } else { "BA" };
        let phase = if phase == "live" {
            if pair == 0 {
                "first_after_preparation"
            } else {
                "warmed"
            }
        } else {
            phase
        };
        for chunk in 0..chunks {
            let a_ns = a[chunk];
            let b_ns = b[chunk];
            let delta = i128::from(b_ns) - i128::from(a_ns);
            let actual = if partition == 0 {
                frames
            } else {
                partition as u32
            };
            println!(
                "C5_PAIR comparison={name} pair={pair} order={order} frames={actual} host_frames={frames} partition={partition} chunk={chunk} phase={phase} a_ns={a_ns} b_ns={b_ns} delta_ns={delta}"
            );
        }
    }
}

fn c5_timed_arm(arm: &mut C5Arm, frames: u32, chunks: usize) -> ([f32; 1920], [u64; 4]) {
    C5_COSTS.with(|costs| costs.borrow_mut().clear());
    crate::media::tests::C5_MEASURE.set(Some(c5_cost));
    let output = arm.render(frames);
    crate::media::tests::C5_MEASURE.set(None);
    let mut durations = [0; 4];
    C5_COSTS.with(|costs| {
        let costs = costs.borrow();
        assert_eq!(
            costs.len(),
            chunks,
            "every attempted final graph must have a timing record"
        );
        for (index, (actual, ns)) in costs.iter().enumerate() {
            assert_eq!(*actual, frames / chunks as u32);
            durations[index] = *ns;
        }
    });
    (output, durations)
}

#[test]
#[ignore = "C5 exact external cohort required; use workflows/tools/run_c5_cohort_tests.py --task1"]
fn stage_c5_paired_final_operation_timing() {
    assert!(
        !cfg!(debug_assertions),
        "use optimized test profile with debug assertions disabled"
    );
    let provider = C5Provider::open("C5_NEW_ADAPTER");
    let real = provider.reader().unwrap();
    for (frames, partition) in [(480, 0), (960, 0), (960, 240)] {
        for comparison in ["legacy_vs_absent", "absent_vs_raw_c1"] {
            let _reset = c5_prepare_storage(4);
            let mut pairs = Vec::with_capacity(C5_PAIRS);
            let mut a = C5Arm::new(25, None, comparison == "legacy_vs_absent", false);
            let mut b = C5Arm::new(
                25,
                if comparison == "absent_vs_raw_c1" {
                    Some(real)
                } else {
                    None
                },
                false,
                false,
            );
            crate::media::tests::CLOCK_GRAPH_SPLIT.set(partition);
            let chunks = if partition == 0 {
                1
            } else {
                frames as usize / partition
            };
            for pair in 0..C5_PAIRS {
                let (left, right) = if pair % 2 == 0 {
                    let left = c5_timed_arm(&mut a, frames, chunks);
                    (left, c5_timed_arm(&mut b, frames, chunks))
                } else {
                    let right = c5_timed_arm(&mut b, frames, chunks);
                    (c5_timed_arm(&mut a, frames, chunks), right)
                };
                assert_eq!(left.0, right.0, "paired PCM equivalence outside timing");
                assert!(
                    left.0[..frames as usize * 2]
                        .iter()
                        .any(|sample| *sample != 0.0)
                );
                pairs.push((left.1, right.1));
            }
            c5_print_pairs(comparison, frames, partition, &pairs, "live");
        }
    }
    for (name, bad) in [
        ("zero_first_vs_latched", 0),
        ("regression_first_vs_latched", 99),
    ] {
        for frames in [480, 960] {
            let _reset = c5_prepare_storage(4);
            let mut pairs = Vec::with_capacity(C5_PAIRS);
            for pair in 0..C5_PAIRS {
                let mut a = C5Arm::new(25, Some(c5_scripted_raw), false, false);
                let mut b = C5Arm::new(25, Some(c5_scripted_raw), false, false);
                // Identical input/graph work: two untimed preparations in each arm.
                CLOCK_NOW.set(100);
                assert_eq!(a.render(frames), b.render(frames));
                CLOCK_NOW.set(101);
                let a_prepared = a.render(frames);
                CLOCK_NOW.set(bad);
                assert_eq!(a_prepared, b.render(frames));
                let (left, right) = if pair % 2 == 0 {
                    let left = c5_timed_arm(&mut a, frames, 1);
                    (left, c5_timed_arm(&mut b, frames, 1))
                } else {
                    let right = c5_timed_arm(&mut b, frames, 1);
                    (c5_timed_arm(&mut a, frames, 1), right)
                };
                assert_eq!(left.0, right.0);
                pairs.push((left.1, right.1));
            }
            c5_print_pairs(name, frames, 0, &pairs, "two_render_prepared");
        }
    }
}

#[test]
#[ignore = "C5 exact external cohort required; use workflows/tools/run_c5_cohort_tests.py --task1"]
fn stage_c5_paired_raw_reader_timing() {
    assert!(
        !cfg!(debug_assertions),
        "use optimized test profile with debug assertions disabled"
    );
    let provider = C5Provider::open("C5_NEW_ADAPTER");
    let real = provider.reader().unwrap();
    let _reset = c5_prepare_storage(4);
    let mut samples = Vec::with_capacity(C5_READER_PAIRS);
    let mut empty = Vec::with_capacity(C5_READER_PAIRS);
    fn timed(reader: C5Reader) -> u64 {
        let began = std::time::Instant::now();
        // SAFETY: exact retained raw pointer or static same-signature control.
        let result = unsafe { std::hint::black_box(reader)() };
        std::hint::black_box(result);
        began.elapsed().as_nanos() as u64
    }
    for pair in 0..C5_READER_PAIRS {
        let mut a = 0;
        let mut b = 0;
        crate::allocation::without_heap(|| {
            if pair % 2 == 0 {
                a = timed(c5_scripted_raw);
                b = timed(real);
            } else {
                b = timed(real);
                a = timed(c5_scripted_raw);
            }
        });
        samples.push(([a, 0, 0, 0], [b, 0, 0, 0]));
        let began = std::time::Instant::now();
        let ns = began.elapsed().as_nanos() as u64;
        empty.push(ns);
    }
    c5_print_pairs("raw_reader_substitution", 0, 0, &samples, "live");
    for (sample, ns) in empty.into_iter().enumerate() {
        println!("C5_TIMER sample={sample} ns={ns}");
    }
}

#[test]
#[ignore = "C5 exact external cohort required; use workflows/tools/run_c5_cohort_tests.py --task1"]
fn stage_c5_paired_raw_reader_count_replay() {
    let provider = C5Provider::open("C5_NEW_ADAPTER");
    let real = provider.reader().unwrap();
    // Off the callback path: retain the actual loaded file mappings for the cohort.
    for line in std::fs::read_to_string("/proc/self/maps").unwrap().lines() {
        if line.contains(".so") || line.contains("usbradioplus_station-") {
            println!("C5_MAP {line}");
        }
    }
    for (frames, partition) in [(480, 0), (960, 0), (960, 240)] {
        let _reset = c5_prepare_storage(8);
        C5_RAW_READER.set(Some(real));
        let mut absent = C5Arm::new(25, None, false, true);
        let mut actual = C5Arm::new(25, Some(c5_counted_raw), false, true);
        let mut legacy = C5Arm::new(25, None, true, false);
        crate::media::tests::CLOCK_GRAPH_SPLIT.set(partition);
        let count = if partition == 0 {
            1
        } else {
            frames / partition as u32
        };
        for pair in 0..C5_PAIRS {
            let mut outputs = [[0.0; 1920]; 2];
            for index in if pair % 2 == 0 { [0, 1] } else { [1, 0] } {
                C5_COSTS.with(|costs| costs.borrow_mut().clear());
                C5_VALIDATION.set((0, 0));
                CLOCK_CALLS.set(0);
                let arm = if index == 0 { &mut absent } else { &mut actual };
                arm.probe.base.calls = 0;
                crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
                crate::media::tests::C5_MEASURE.set(Some(c5_cost));
                outputs[index] = arm.render(frames);
                c5_require_counts(count, count, count, if index == 0 { 0 } else { count }, arm);
                assert_eq!(arm.probe.base.clock.0, index as u32);
                C5_COSTS.with(|costs| {
                    assert!(costs.borrow().iter().all(|(n, _)| *n == frames / count))
                });
                assert_eq!(
                    &arm.probe.base.frames[..count as usize],
                    &[frames / count; 4][..count as usize]
                );
            }
            assert_eq!(absent.probe.base.clock, (0, 0, 0));
            assert!(actual.probe.base.clock.2 > 0);
            c5_semantics_equal(&absent, &actual);
            assert_eq!(outputs[0], outputs[1]);
            crate::media::tests::C5_MEASURE.set(None);
            crate::media::tests::C5_VALIDATE.set(None);
            CLOCK_CALLS.set(0);
            assert_eq!(legacy.render(frames), outputs[0]);
            assert_eq!(CLOCK_CALLS.get(), 0);
        }
        println!(
            "C5_COUNTS frames={frames} partition={partition} pairs={C5_PAIRS} attempts_per_arm={} successes_per_arm={} observations_per_observed_arm={} raw_reads={} absent_reads=0 legacy_reads=0 timings_per_observed_arm={} mode=count_replay_not_timing",
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count
        );
    }
    // A direct-reader replay uses the same raw call topology, with an explicit
    // wrapper only in this validation run, never in the timing run.
    let _reset = c5_prepare_storage(8);
    C5_RAW_READER.set(Some(real));
    for pair in 0..C5_READER_PAIRS {
        // SAFETY: both pointers are retained throughout this count-only replay.
        unsafe {
            if pair % 2 == 0 {
                std::hint::black_box(clock_now());
                std::hint::black_box(c5_counted_raw());
            } else {
                std::hint::black_box(c5_counted_raw());
                std::hint::black_box(clock_now());
            }
        }
    }
    assert_eq!(CLOCK_CALLS.get(), 2 * C5_READER_PAIRS as u32);
    println!(
        "C5_READER_COUNTS pairs={C5_READER_PAIRS} actual_calls={C5_READER_PAIRS} scripted_calls={C5_READER_PAIRS} mode=count_replay_not_timing"
    );
}

#[test]
fn stage_c5_paired_fault_arm_equivalence() {
    for bad in [0, 99] {
        for frames in [480, 960] {
            let _reset = c5_prepare_storage(8);
            for pair in 0..C5_PAIRS {
                let mut a = C5Arm::new(25, Some(clock_now), false, true);
                let mut b = C5Arm::new(25, Some(clock_now), false, true);
                CLOCK_NOW.set(100);
                assert_eq!(a.render(frames), b.render(frames));
                CLOCK_NOW.set(101);
                let a_prepared = a.render(frames);
                CLOCK_NOW.set(bad);
                assert_eq!(a_prepared, b.render(frames));
                let mut outputs = [[0.0; 1920]; 2];
                for index in if pair % 2 == 0 { [0, 1] } else { [1, 0] } {
                    C5_COSTS.with(|costs| costs.borrow_mut().clear());
                    C5_VALIDATION.set((0, 0));
                    CLOCK_CALLS.set(0);
                    let arm = if index == 0 { &mut a } else { &mut b };
                    arm.probe.base.calls = 0;
                    crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
                    crate::media::tests::C5_MEASURE.set(Some(c5_cost));
                    outputs[index] = arm.render(frames);
                    c5_require_counts(1, 1, 1, u32::from(index == 0), arm);
                    assert_eq!(arm.probe.base.clock, (0, 0, 0));
                    assert_eq!(arm.probe.base.frames[0], frames);
                }
                c5_semantics_equal(&a, &b);
                assert_eq!(outputs[0], outputs[1]);
                crate::media::tests::C5_VALIDATE.set(None);
                crate::media::tests::C5_MEASURE.set(None);
            }
            println!(
                "C5_FAULT_COUNTS bad={bad} frames={frames} pairs={C5_PAIRS} first_reads={C5_PAIRS} latched_reads=0 attempts_per_arm={C5_PAIRS} successes_per_arm={C5_PAIRS} observations_per_arm={C5_PAIRS} timings_per_arm={C5_PAIRS}"
            );
        }
    }
}

#[test]
fn stage_c5_paired_legacy_count_replay() {
    for (frames, partition) in [(480, 0), (960, 0), (960, 240)] {
        let _reset = c5_prepare_storage(8);
        let mut legacy = C5Arm::new(25, None, true, true);
        let mut absent = C5Arm::new(25, None, false, true);
        crate::media::tests::CLOCK_GRAPH_SPLIT.set(partition);
        let count = if partition == 0 {
            1
        } else {
            frames / partition as u32
        };
        for pair in 0..C5_PAIRS {
            let mut outputs = [[0.0; 1920]; 2];
            for index in if pair % 2 == 0 { [0, 1] } else { [1, 0] } {
                C5_COSTS.with(|costs| costs.borrow_mut().clear());
                C5_VALIDATION.set((0, 0));
                CLOCK_CALLS.set(0);
                let arm = if index == 0 { &mut legacy } else { &mut absent };
                arm.probe.base.calls = 0;
                crate::media::tests::C5_VALIDATE.set(Some(c5_validate_graph));
                crate::media::tests::C5_MEASURE.set(Some(c5_cost));
                outputs[index] = arm.render(frames);
                c5_require_counts(count, count, if index == 0 { 0 } else { count }, 0, arm);
                C5_COSTS.with(|costs| {
                    assert!(costs.borrow().iter().all(|(n, _)| *n == frames / count))
                });
            }
            assert_eq!(outputs[0], outputs[1]);
            assert_eq!(absent.probe.base.clock, (0, 0, 0));
        }
        println!(
            "C5_LEGACY_COUNTS frames={frames} partition={partition} pairs={C5_PAIRS} attempts_per_arm={} successes_per_arm={} legacy_observations=0 absent_observations={} reads=0 timings_per_arm={}",
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count,
            C5_PAIRS as u32 * count
        );
    }
}
