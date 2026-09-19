//! Reload must revalidate its captured host after waiting for control ownership.

use super::*;
use crate::host::support::Fixture;
use std::time::Instant;

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
