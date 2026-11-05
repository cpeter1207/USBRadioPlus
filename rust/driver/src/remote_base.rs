//! Bounded, nonblocking commands for the existing physical radio owner.

use crossbeam_queue::ArrayQueue;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicU8, Ordering},
};
use usbradioplus_core::{RemoteMemory, RemoteScanList, remote_operation::RemoteOperation};
use usbradioplus_remote::{Setting, raw};

/// Final owned result, copied off the physical worker before presentation.
pub enum RemoteReply {
    /// The operating change completed successfully.
    Applied,
    /// One actual supported getter returned this value in the declared units.
    Value {
        /// Declared setting metadata.
        setting: Setting,
        /// Observed value.
        value: i64,
    },
    /// Complete requested/applied/observed state and current scan state.
    Status {
        /// Copied engine snapshot, with truthful observation masks.
        snapshot: Box<raw::RptAdvRemoteSnapshot>,
        /// Current selected scan, if any.
        scan: Option<raw::RptAdvRemoteScanSnapshot>,
    },
    /// A bounded page of configured memories, or one queried memory.
    Memories(Vec<RemoteMemory>),
    /// A bounded page of configured scans.
    Scans(Vec<RemoteScanList>),
    /// A bounded page of declared setting capabilities.
    Capabilities(Vec<Setting>),
    /// One configured list with its current live state, if selected.
    Scan {
        /// Immutable configured definition.
        definition: RemoteScanList,
        /// Current matching live state.
        state: Option<raw::RptAdvRemoteScanSnapshot>,
    },
}

/// Explicit admission or terminal failure. Original adapter codes are retained.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemoteRequestError {
    /// The bounded queue has no free ordinary or reserved urgent slot.
    Busy,
    /// This physical generation has stopped or its owner was lost.
    Stopped,
    /// The initiating connection or command grant is no longer valid.
    StaleSource,
    /// The input record is incompatible or the selected operation was rejected.
    Rejected(i32),
}

/// Latest command-source state; local RF and peer identity stay in their host.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RemoteSource {
    /// This command originated at the local receiver.
    pub local: bool,
    /// Its source remains authorized and connected; revocation is terminal.
    pub valid: bool,
    /// That command source is still keyed, independently of other receivers.
    pub active: bool,
}

struct TicketState {
    generation: Arc<AtomicBool>,
    local: bool,
    source: AtomicU8,
    completed: AtomicBool,
    result: ArrayQueue<Result<RemoteReply, RemoteRequestError>>,
}

impl TicketState {
    fn source(&self) -> RemoteSource {
        let bits = self.source.load(Ordering::Acquire);
        RemoteSource {
            local: self.local,
            valid: bits & 1 != 0 && self.generation.load(Ordering::Acquire),
            active: bits & 2 != 0,
        }
    }
    fn complete(&self, result: Result<RemoteReply, RemoteRequestError>) {
        if self
            .completed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            // Only the successful claimant publishes once into this preallocated slot.
            let _ = self.result.push(result);
        }
    }
}

/// Caller-owned request lease. Dropping it revokes pending/in-flight command work;
/// it never waits for device I/O, a worker, or a completion consumer.
pub struct RemoteTicket(Arc<TicketState>);
impl RemoteTicket {
    /// Publish source-key and authorization state. A revoked source cannot revive.
    pub fn source_state(&self, active: bool, valid: bool) {
        let _ = self
            .0
            .source
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |old| {
                Some(u8::from(valid && old & 1 != 0) | (u8::from(active) << 1))
            });
    }
    /// Take the one terminal completion, without waiting. Retirement also completes
    /// callers whose work raced with queue shutdown, even if it was never dequeued.
    pub fn take_completion(&self) -> Option<Result<RemoteReply, RemoteRequestError>> {
        if !self.0.generation.load(Ordering::Acquire) {
            self.0.complete(Err(RemoteRequestError::Stopped));
        }
        self.0.result.pop()
    }
}
impl Drop for RemoteTicket {
    fn drop(&mut self) {
        self.0.source.fetch_and(!1, Ordering::Release);
    }
}

/// One dequeued request retained exclusively by the physical worker. Abandoning
/// work completes it as stopped, so callers cannot wait forever for a lost owner.
pub struct RemoteWork {
    operation: RemoteOperation,
    state: Arc<TicketState>,
}
impl RemoteWork {
    /// Borrow the complete pointer-free input.
    pub const fn operation(&self) -> RemoteOperation {
        self.operation
    }
    /// Read current source validity immediately before applying physical changes.
    pub fn source(&self) -> RemoteSource {
        self.state.source()
    }
    /// Publish exactly one final result. A retired generation cannot report success.
    pub fn complete(self, result: Result<RemoteReply, RemoteRequestError>) {
        self.state
            .complete(if self.state.generation.load(Ordering::Acquire) {
                result
            } else {
                Err(RemoteRequestError::Stopped)
            });
    }
}
impl Drop for RemoteWork {
    fn drop(&mut self) {
        self.state.complete(Err(RemoteRequestError::Stopped));
    }
}

/// Bounded per-rig command port, shared by control producers and the existing
/// physical owner. No rig context, I/O, callback, or controller reference is stored.
pub struct RemoteQueue {
    accepting: Arc<AtomicBool>,
    ordinary: ArrayQueue<RemoteWork>,
    urgent: ArrayQueue<RemoteWork>,
}
impl RemoteQueue {
    /// Allocate ordinary slots and one reserved urgent slot before admitting work.
    pub fn new(capacity: usize) -> Self {
        Self {
            accepting: Arc::new(AtomicBool::new(true)),
            ordinary: ArrayQueue::new(capacity),
            urgent: ArrayQueue::new(1),
        }
    }
    /// Admit a complete request, or report full/stopped/invalid input immediately.
    pub fn submit(
        &self,
        operation: RemoteOperation,
        local: bool,
        active: bool,
    ) -> Result<RemoteTicket, RemoteRequestError> {
        if !self.accepting.load(Ordering::Acquire) {
            return Err(RemoteRequestError::Stopped);
        }
        if operation.kind().is_none() {
            return Err(RemoteRequestError::Rejected(-1003));
        }
        let state = Arc::new(TicketState {
            generation: Arc::clone(&self.accepting),
            local,
            source: AtomicU8::new(1 | (u8::from(active) << 1)),
            completed: AtomicBool::new(false),
            result: ArrayQueue::new(1),
        });
        let queue = if operation.urgent() {
            &self.urgent
        } else {
            &self.ordinary
        };
        queue
            .push(RemoteWork {
                operation,
                state: Arc::clone(&state),
            })
            .map_err(|_| RemoteRequestError::Busy)?;
        Ok(RemoteTicket(state))
    }
    /// Take urgent work first. Only the existing physical worker consumes this port.
    pub fn next(&self) -> Option<RemoteWork> {
        self.urgent.pop().or_else(|| self.ordinary.pop())
    }
    /// Revoke the generation before safe shutdown. Pending tickets become terminal
    /// without waiting for slow device I/O or requiring another controller pump.
    pub fn retire(&self) {
        self.accepting.store(false, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use usbradioplus_core::remote_operation::{RemoteOperation, RemoteOperationKind as Kind};

    #[test]
    fn remote_queue_has_explicit_backpressure_and_reserved_stop() {
        let queue = RemoteQueue::new(2);
        let request = RemoteOperation::new(Kind::Tune);
        let first = queue.submit(request, true, false).unwrap();
        let second = queue.submit(request, false, true).unwrap();
        assert!(matches!(
            queue.submit(request, true, false),
            Err(RemoteRequestError::Busy)
        ));
        let stop = queue
            .submit(RemoteOperation::new(Kind::StopScan), true, false)
            .unwrap();
        assert!(matches!(
            queue.submit(RemoteOperation::new(Kind::StopScan), true, false),
            Err(RemoteRequestError::Busy)
        ));
        let work = queue.next().unwrap();
        assert_eq!(work.operation().kind(), Some(Kind::StopScan));
        work.complete(Ok(RemoteReply::Applied));
        assert!(matches!(
            stop.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
        let work = queue.next().unwrap();
        assert!(work.source().local);
        work.complete(Ok(RemoteReply::Applied));
        assert!(matches!(
            first.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
        let second_work = queue.next().unwrap();
        assert!(!second_work.source().local);
        assert!(second.take_completion().is_none());
        assert!(queue.next().is_none());
    }

    #[test]
    fn remote_queue_source_revocation_and_retirement_are_terminal() {
        let queue = RemoteQueue::new(2);
        let request = RemoteOperation::new(Kind::Tune);
        let ticket = queue.submit(request, false, true).unwrap();
        ticket.source_state(false, true);
        assert!(!queue.next().unwrap().source().active);
        let stale = queue.submit(request, false, false).unwrap();
        stale.source_state(false, false);
        stale.source_state(true, true);
        let work = queue.next().unwrap();
        assert!(!work.source().valid);
        work.complete(Err(RemoteRequestError::StaleSource));
        assert!(matches!(
            stale.take_completion(),
            Some(Err(RemoteRequestError::StaleSource))
        ));
        let retired = queue.submit(request, true, false).unwrap();
        queue.retire();
        assert!(matches!(
            retired.take_completion(),
            Some(Err(RemoteRequestError::Stopped))
        ));
        assert!(retired.take_completion().is_none());
        assert!(matches!(
            queue.submit(request, true, false),
            Err(RemoteRequestError::Stopped)
        ));
        assert!(!queue.next().unwrap().source().valid);
    }

    #[test]
    fn remote_queue_caller_drop_cancels_before_dequeue_and_other_rigs_stay_independent() {
        let blocked = RemoteQueue::new(1);
        let other = RemoteQueue::new(1);
        let request = RemoteOperation::new(Kind::Tune);
        let cancelled = blocked.submit(request, true, false).unwrap();
        drop(cancelled);
        assert!(!blocked.next().unwrap().source().valid);
        let inflight = blocked.submit(request, true, false).unwrap();
        let work = blocked.next().unwrap();
        let live = other.submit(request, true, false).unwrap();
        other.next().unwrap().complete(Ok(RemoteReply::Applied));
        assert!(matches!(
            live.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
        assert!(inflight.take_completion().is_none());
        blocked.retire();
        work.complete(Ok(RemoteReply::Applied));
        assert!(matches!(
            inflight.take_completion(),
            Some(Err(RemoteRequestError::Stopped))
        ));
    }
}
