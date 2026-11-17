//! Enumerated-site, current-test-thread counters. Not a native DSO probe.
use std::cell::Cell;

pub(crate) const SITES: [&str; N] = [
    "output_loudness",
    "retirement_pending",
    "retirement_admission",
    "retirement_prepare_failed",
    "retirement_read_outcome",
    "retirement_apply_failed",
    "retirement_complete",
    "snapshot_update",
    "begin_update",
    "accept_update",
    "finish_update",
];
pub(crate) const N: usize = 11;
pub(crate) const CONTROL: usize = 1;
thread_local! {
    static PHASE: Cell<usize> = const { Cell::new(0) };
    static COUNTS: Cell<[[[u64; 3]; N]; 5]> = const { Cell::new([[[0; 3]; N]; 5]) };
    static OMIT: Cell<Option<(usize, usize)>> = const { Cell::new(None) };
}
pub(crate) fn reset(omit: Option<(usize, usize)>) {
    PHASE.set(0);
    COUNTS.set([[[0; 3]; N]; 5]);
    OMIT.set(omit);
}
pub(crate) fn phase<T>(phase: usize, work: impl FnOnce() -> T) -> T {
    struct Restore(usize);
    impl Drop for Restore {
        fn drop(&mut self) {
            PHASE.set(self.0);
        }
    }
    let _restore = Restore(PHASE.replace(phase));
    work()
}
pub(crate) fn snapshot(phase: usize) -> [[u64; 3]; N] {
    COUNTS.get()[phase]
}
pub(crate) fn covered() {
    for (site, count) in SITES.iter().zip(snapshot(CONTROL)) {
        assert!(count[0] > 0, "missing attempt hook: {site}");
        assert_eq!(count[0], count[1], "missing acquired hook: {site}");
    }
}
pub(crate) fn report(phase: usize) {
    assert_eq!(PHASE.get(), 0, "reporting is outside armed regions");
    for (site, count) in SITES.iter().zip(snapshot(phase)) {
        println!(
            "C5_LOCK site={site} phase={phase} attempts={} acquired={} poisoned={}",
            count[0], count[1], count[2]
        );
    }
}

pub(crate) const FIRST: usize = 2;
pub(crate) const WARM: usize = 3;
pub(crate) const FAULT: usize = 4;
pub(crate) fn attempt(site: usize) {
    let phase = PHASE.get();
    if phase != 0 && OMIT.get() != Some((site, 0)) {
        let mut counts = COUNTS.get();
        counts[phase][site][0] += 1;
        COUNTS.set(counts);
    }
}
pub(crate) fn acquired(site: usize, poisoned: bool) {
    let phase = PHASE.get();
    if phase != 0 && OMIT.get() != Some((site, 1)) {
        let mut counts = COUNTS.get();
        counts[phase][site][1] += 1;
        counts[phase][site][2] += u64::from(poisoned);
        COUNTS.set(counts);
    }
}
pub(crate) fn zero(phase: usize) {
    assert_eq!(
        snapshot(phase),
        [[0; 3]; N],
        "enumerated lock in callback phase"
    );
}

pub(crate) fn missing_site_controls(work: impl Fn()) {
    for omitted in 0..N {
        for event in 0..2 {
            reset(Some((omitted, event)));
            phase(CONTROL, &work);
            let result = std::panic::catch_unwind(covered).expect_err("missing hook must fail");
            let message = result
                .downcast_ref::<String>()
                .expect("exact assertion payload");
            assert!(message.contains(SITES[omitted]));
            let counts = snapshot(CONTROL);
            assert_eq!(counts[omitted][event], 0);
            assert!(counts[omitted][1 - event] > 0);
            for (site, count) in counts.iter().enumerate() {
                if site != omitted {
                    assert!(count[0] > 0);
                    assert_eq!(count[0], count[1]);
                }
            }
            println!("C5_MISSING_SITE rejected={} event={event}", SITES[omitted]);
        }
    }
    reset(None);
}

/// Artifact and executing-thread receipts are collected only while disarmed.
pub(crate) fn identity() {
    assert_eq!(PHASE.get(), 0);
    println!("C5_LOCK_THREAD {:?}", std::thread::current());
    println!(
        "C5_LOCK_EXECUTABLE={}",
        std::env::current_exe().unwrap().display()
    );
    let maps = std::fs::read_to_string("/proc/self/maps").expect("Linux unit evidence");
    let paths: std::collections::BTreeSet<_> = maps
        .lines()
        .filter_map(|line| line.split_whitespace().last())
        .filter(|path| path.starts_with('/'))
        .collect();
    for path in paths {
        println!("C5_LOCK_MAPPED={path}");
    }
}
