//! Bounded, nonblocking commands for the existing physical radio owner.

use crossbeam_queue::ArrayQueue;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicI32, AtomicU8, Ordering},
};
use usbradioplus_core::{
    RemoteMemory, RemoteScanList, radio_control::ParallelRadioRequest,
    remote_operation::RemoteOperation,
};
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
        /// First detached legacy command error, independent of radio fault/inhibit.
        request_error: i32,
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
    detached_error: Option<Arc<AtomicI32>>,
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
            if let (Some(error), Err(result)) = (&self.detached_error, &result) {
                let code = match result {
                    RemoteRequestError::Rejected(code) => *code,
                    RemoteRequestError::Busy => -9,
                    RemoteRequestError::Stopped | RemoteRequestError::StaleSource => -10,
                };
                let _ = error.compare_exchange(0, code, Ordering::AcqRel, Ordering::Acquire);
            }
            // Only the successful claimant publishes once into this preallocated slot.
            let _ = self.result.push(result);
        }
    }
}

/// Caller-owned request lease. Dropping it revokes pending/in-flight command work;
/// it never waits for device I/O, a worker, or a completion consumer.
pub struct RemoteTicket(Arc<TicketState>);
/// Read-only source lifetime retained by blocking physical work. It never owns
/// a controller/peer pointer and does not revoke the caller when dropped.
#[derive(Clone)]
pub struct RemoteSourceLease(Arc<TicketState>);
impl RemoteSourceLease {
    /// Copy current source state; caller retirement/revocation is terminal.
    pub fn source(&self) -> RemoteSource {
        self.0.source()
    }
}
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
    input: RemoteInput,
    state: Arc<TicketState>,
}
enum RemoteInput {
    Command(RemoteOperation),
    Parallel(ParallelRadioRequest),
}
impl RemoteWork {
    /// Retain only atomic source validity for stop checks between device calls.
    pub fn source_lease(&self) -> RemoteSourceLease {
        RemoteSourceLease(Arc::clone(&self.state))
    }
    /// Borrow the complete pointer-free input.
    pub const fn operation(&self) -> Option<RemoteOperation> {
        match self.input {
            RemoteInput::Command(operation) => Some(operation),
            RemoteInput::Parallel(_) => None,
        }
    }
    /// Existing ASL text request, never encoded in a fake numeric rig operation.
    pub const fn parallel_request(&self) -> Option<ParallelRadioRequest> {
        match self.input {
            RemoteInput::Parallel(request) => Some(request),
            RemoteInput::Command(_) => None,
        }
    }
    /// Read current source validity immediately before applying physical changes.
    pub fn source(&self) -> RemoteSource {
        self.state.source()
    }
    /// Publish exactly one final result. A retired generation cannot report success.
    pub fn complete(self, result: Result<RemoteReply, RemoteRequestError>) {
        self.state
            .complete(if !self.state.generation.load(Ordering::Acquire) {
                Err(RemoteRequestError::Stopped)
            } else if result.is_ok() && !self.source().valid {
                Err(RemoteRequestError::StaleSource)
            } else {
                result
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
    ready: AtomicBool,
    parallel_error: Arc<AtomicI32>,
    accepting: Arc<AtomicBool>,
    ordinary: ArrayQueue<RemoteWork>,
    urgent: ArrayQueue<RemoteWork>,
}
impl RemoteQueue {
    /// Allocate ordinary slots and one reserved urgent slot before admitting work.
    pub fn new(capacity: usize) -> Self {
        Self {
            ready: AtomicBool::new(false),
            parallel_error: Arc::new(AtomicI32::new(0)),
            accepting: Arc::new(AtomicBool::new(true)),
            ordinary: ArrayQueue::new(capacity),
            urgent: ArrayQueue::new(1),
        }
    }
    /// Initial complete RF/DSP state was committed by this physical owner.
    pub(crate) fn mark_ready(&self) {
        self.ready.store(true, Ordering::Release);
    }
    /// Retirement is terminal even if a final in-flight operation acknowledges.
    pub fn ready(&self) -> bool {
        self.accepting.load(Ordering::Acquire) && self.ready.load(Ordering::Acquire)
    }
    /// Admit a complete request, or report full/stopped/invalid input immediately.
    pub fn submit(
        &self,
        operation: RemoteOperation,
        local: bool,
        active: bool,
    ) -> Result<RemoteTicket, RemoteRequestError> {
        self.enqueue(RemoteInput::Command(operation), local, active)
            .map(RemoteTicket)
    }
    /// Submit an already-authorized complete legacy controller frame. Its owner
    /// is this hardware generation, not a caller ticket that would cancel on drop.
    pub fn submit_parallel(&self, request: ParallelRadioRequest) -> Result<(), RemoteRequestError> {
        self.enqueue(RemoteInput::Parallel(request), true, false)
            .map(drop)
    }
    /// First legacy request failure, independent of the physical radio's health.
    pub fn parallel_error(&self) -> i32 {
        self.parallel_error.load(Ordering::Acquire)
    }
    fn enqueue(
        &self,
        input: RemoteInput,
        local: bool,
        active: bool,
    ) -> Result<Arc<TicketState>, RemoteRequestError> {
        if !self.accepting.load(Ordering::Acquire) {
            return Err(RemoteRequestError::Stopped);
        }
        if matches!(input, RemoteInput::Command(operation) if operation.kind().is_none()) {
            return Err(RemoteRequestError::Rejected(-1003));
        }
        let state = Arc::new(TicketState {
            detached_error: matches!(input, RemoteInput::Parallel(_))
                .then(|| Arc::clone(&self.parallel_error)),
            generation: Arc::clone(&self.accepting),
            local,
            source: AtomicU8::new(1 | (u8::from(active) << 1)),
            completed: AtomicBool::new(false),
            result: ArrayQueue::new(1),
        });
        let queue = if matches!(input, RemoteInput::Command(operation) if operation.urgent()) {
            &self.urgent
        } else {
            &self.ordinary
        };
        queue
            .push(RemoteWork {
                input,
                state: Arc::clone(&state),
            })
            .map_err(|_| RemoteRequestError::Busy)?;
        Ok(state)
    }
    /// Take urgent work first. Only the existing physical worker consumes this port.
    pub fn next(&self) -> Option<RemoteWork> {
        self.urgent.pop().or_else(|| self.ordinary.pop())
    }
    /// Take only reserved safety work before servicing physical demand.
    pub fn next_urgent(&self) -> Option<RemoteWork> {
        self.urgent.pop()
    }
    /// Take at most one ordinary operation per physical service iteration.
    pub fn next_ordinary(&self) -> Option<RemoteWork> {
        self.ordinary.pop()
    }
    /// Cheap physical-owner cancellation check between blocking exchanges.
    pub fn has_urgent(&self) -> bool {
        !self.urgent.is_empty()
    }
    /// Revoke the generation before safe shutdown. Pending tickets become terminal
    /// without waiting for slow device I/O or requiring another controller pump.
    pub fn retire(&self) {
        self.accepting.store(false, Ordering::Release);
    }
}

#[cfg(test)]
#[path = "tests/remote_base_tests.rs"]
mod tests;
