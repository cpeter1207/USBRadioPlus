//! Scanner shutdown and reload synchronize with control ownership.

use super::*;
use crate::host::support::Fixture;
use std::cell::RefCell;
use std::sync::mpsc;
use std::time::Instant;

thread_local! {
    static PROFILE_GATE: RefCell<Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>> =
        const { RefCell::new(None) };
}

fn wait_for_stop_request() -> Option<Box<str>> {
    PROFILE_GATE.with(|gate| {
        let gate = gate.borrow();
        let (entered, resume) = gate.as_ref().unwrap();
        entered.send(()).unwrap();
        resume.recv_timeout(Duration::from_secs(2)).unwrap();
    });
    None
}

#[test]
fn scanner_stops_after_profile_resolution_without_scanning_or_waiting() {
    let _fixture = Fixture::new();
    let host = LinkHost::new(ptr::dangling_mut(), ptr::dangling_mut());
    let stop = Arc::new((Mutex::new(false), Condvar::new()));
    let worker_stop = Arc::clone(&stop);
    let (entered, reached) = mpsc::channel();
    let (resume, released) = mpsc::channel();
    let control = lock(&LINK_CONTROL);
    let worker = thread::spawn(move || {
        PROFILE_GATE.with(|gate| *gate.borrow_mut() = Some((entered, released)));
        scan_loop(host, wait_for_stop_request, worker_stop);
    });
    let reached = reached.recv_timeout(Duration::from_secs(2));
    *lock(&stop.0) = true;
    let released = resume.send(());
    drop(control);
    let joined = worker.join();
    assert!(reached.is_ok(), "scanner must enter profile resolution");
    assert!(
        released.is_ok(),
        "profile resolver must await the stop request"
    );
    assert!(
        joined.is_ok(),
        "stopped scanner must return without waiting"
    );
    scan_loop(
        host,
        || panic!("stopped scanner must not resolve a profile"),
        stop,
    );
    crate::host::support::with_state(|state| {
        assert_eq!(state.iterator_allocations, 0);
        assert!(state.audiohook_calls.is_empty());
    });
}

struct StopGuard;

impl Drop for StopGuard {
    fn drop(&mut self) {
        stop();
    }
}

#[test]
fn reload_rejects_a_stopped_or_replaced_host_after_waiting_for_control() {
    for removed in [false, true] {
        let _fixture = Fixture::new();
        let _cleanup = StopGuard;
        assert!(lock(running_host()).is_none());
        *lock(running_host()) = Some(RunningLinkHost {
            host: LinkHost::new(ptr::dangling_mut(), ptr::dangling_mut()),
            generation: Arc::new(()),
            stop: Arc::new((Mutex::new(false), Condvar::new())),
            scanner: thread::spawn(|| {}),
        });
        thread::scope(|scope| {
            let control = lock(&LINK_CONTROL);
            let worker =
                scope.spawn(|| matches!(reload_prepare(None), Err(LinkHostError::Asterisk)));
            let deadline = Instant::now() + Duration::from_secs(2);
            let captured = loop {
                // Borrow rather than clone: only reload can add this reference.
                let references = {
                    let running = lock(running_host());
                    Arc::strong_count(&running.as_ref().unwrap().generation)
                };
                if references > 1 {
                    break true;
                }
                if Instant::now() >= deadline {
                    break false;
                }
                thread::yield_now();
            };
            if !captured {
                // A failing assertion must not strand the scoped worker.
                drop(control);
                panic!("reload did not capture the installed generation");
            }
            let retired = if removed {
                lock(running_host()).take()
            } else {
                lock(running_host()).as_mut().unwrap().generation = Arc::new(());
                None
            };
            drop(control);
            if let Some(retired) = retired {
                retired.scanner.join().unwrap();
            }
            assert!(worker.join().unwrap());
        });
    }
}
