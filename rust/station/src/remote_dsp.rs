//! Off-thread radio preparation through the station's existing update mailbox.

use crate::{
    StationControlHost, StationRuntime, StationUpdate, StationUpdateError, StationUpdatePreparation,
};
use crossbeam_queue::ArrayQueue;
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};
use usbradioplus_core::RemoteOperatingState;
use usbradioplus_radio::{RadioProvider, SubaudibleSource};
use usbradioplus_runtime::NativeProcessingFactory;

/// Bounded physical-owner wait or station-update failure.
#[derive(Debug)]
pub enum RemoteDspError {
    /// Another pending exchange occupies the bounded handoff.
    Busy,
    /// The original control owner has retired.
    Stopped,
    /// Urgent work, retirement, or source revocation cancelled this exchange.
    Cancelled,
    /// The owner's existing control-response deadline elapsed.
    Timeout,
    /// Original station preparation/adoption error, retained without replacement.
    Update(StationUpdateError),
}

struct Exchange {
    target: RemoteOperatingState,
    external_decode: SubaudibleSource,
    carrier: usbradioplus_core::CarrierSource,
    preparation: ArrayQueue<StationUpdatePreparation>,
    ready: ArrayQueue<Result<StationUpdate, StationUpdateError>>,
    result: ArrayQueue<Result<(), RemoteDspError>>,
    completed: AtomicBool,
    cancelled: AtomicBool,
}
impl Exchange {
    fn complete(&self, result: Result<(), RemoteDspError>) {
        if self
            .completed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            let _ = self.result.push(result);
        }
    }
}

/// Physical-worker port. It contains no station pointer or ControlObserver;
/// all live mailbox operations remain on the original serialized control owner.
pub struct RemoteDspPort {
    pending: ArrayQueue<Arc<Exchange>>,
    live: AtomicBool,
}
impl RemoteDspPort {
    /// Apply on the physical worker only. The caller supplies its existing bounded
    /// control-response timeout and a cheap stop/urgent-work check. This method
    /// may prepare graphs and wait; it must never run on audio or control callbacks.
    /// Cancellation retains a half-adopted update until both reverse ACKs arrive.
    pub fn apply(
        &self,
        target: RemoteOperatingState,
        external_decode: SubaudibleSource,
        carrier: usbradioplus_core::CarrierSource,
        timeout: Duration,
        mut cancelled: impl FnMut() -> bool,
    ) -> Result<(), RemoteDspError> {
        if !self.live.load(Ordering::Acquire) {
            return Err(RemoteDspError::Stopped);
        }
        let exchange = Arc::new(Exchange {
            target,
            external_decode,
            carrier,
            preparation: ArrayQueue::new(1),
            ready: ArrayQueue::new(1),
            result: ArrayQueue::new(1),
            completed: AtomicBool::new(false),
            cancelled: AtomicBool::new(false),
        });
        self.pending
            .push(Arc::clone(&exchange))
            .map_err(|_| RemoteDspError::Busy)?;
        let started = Instant::now();
        loop {
            if let Some(result) = exchange.result.pop() {
                return result;
            }
            let failure = if !self.live.load(Ordering::Acquire) {
                Some(RemoteDspError::Stopped)
            } else if cancelled() {
                Some(RemoteDspError::Cancelled)
            } else if started.elapsed() >= timeout {
                Some(RemoteDspError::Timeout)
            } else {
                None
            };
            if let Some(failure) = failure {
                exchange.cancelled.store(true, Ordering::Release);
                return Err(failure);
            }
            if let Some(preparation) = exchange.preparation.pop() {
                let candidate = preparation
                    .plan()
                    .clone()
                    .with_remote_carrier(exchange.carrier)
                    .with_remote_signaling(&exchange.target, exchange.external_decode)
                    .map_err(StationUpdateError::Preparation)
                    .and_then(|plan| preparation.prepare(plan));
                // One worker and one control owner each publish only one phase.
                let _ = exchange.ready.push(candidate);
            }
            std::thread::park_timeout(Duration::from_millis(1));
        }
    }
}

struct Active {
    exchange: Arc<Exchange>,
    prepared: bool,
    started: bool,
    reversing: bool,
}

/// Original control owner's nonblocking preparation/adoption pump. Graphs are
/// built by its physical worker; callbacks use only the existing atomic mailbox.
pub struct RemoteDspControl {
    port: Arc<RemoteDspPort>,
    active: Option<Active>,
}
impl Default for RemoteDspControl {
    fn default() -> Self {
        Self::new()
    }
}
impl RemoteDspControl {
    /// Allocate one pending exchange before starting the physical owner.
    pub fn new() -> Self {
        Self {
            port: Arc::new(RemoteDspPort {
                pending: ArrayQueue::new(1),
                live: AtomicBool::new(true),
            }),
            active: None,
        }
    }
    /// Retain a worker port without sharing the live station/control owner.
    pub fn port(&self) -> Arc<RemoteDspPort> {
        Arc::clone(&self.port)
    }
    /// An exchange is preparing, pending adoption, or retaining rollback storage.
    pub fn is_busy(&self) -> bool {
        self.active.is_some() || !self.port.pending.is_empty()
    }
    /// Both callback owners still owe forward or reverse adoption acknowledgments.
    pub fn awaiting_callbacks(&self) -> bool {
        self.active.as_ref().is_some_and(|active| active.started)
    }
    /// Advance one exchange on the original serialized station owner. Busy/revised
    /// generations are recaptured; Pending never waits. Failed/cancelled adoption
    /// is reversed using retained existing mailbox storage, not a second graph path.
    pub fn pump(
        &mut self,
        runtime: &mut StationRuntime,
        control: &mut StationControlHost,
        factory: &NativeProcessingFactory,
        provider: RadioProvider,
    ) {
        if self.active.is_none() {
            self.active = self.port.pending.pop().map(|exchange| Active {
                exchange,
                prepared: false,
                started: false,
                reversing: false,
            });
        }
        let Some(active) = &mut self.active else {
            return;
        };
        if active.exchange.cancelled.load(Ordering::Acquire) {
            if !active.started {
                self.active = None;
                return;
            }
            active.reversing = true;
        }
        if !active.prepared {
            match runtime.snapshot_update(control, factory, provider) {
                Ok(preparation) => {
                    let _ = active.exchange.preparation.push(preparation);
                    active.prepared = true;
                }
                Err(StationUpdateError::Busy) => {}
                Err(error) => {
                    active.exchange.complete(Err(RemoteDspError::Update(error)));
                    self.active = None;
                }
            }
            return;
        }
        if !active.started {
            let Some(prepared) = active.exchange.ready.pop() else {
                return;
            };
            match prepared.and_then(|update| runtime.begin_update(control, update)) {
                Ok(()) => active.started = true,
                Err(StationUpdateError::Busy) => {
                    active.prepared = false;
                    return;
                }
                Err(error) => {
                    active.exchange.complete(Err(RemoteDspError::Update(error)));
                    self.active = None;
                    return;
                }
            }
        }
        match runtime.finish_update(control, !active.reversing) {
            Ok(()) => {
                active.exchange.complete(if active.reversing {
                    Err(RemoteDspError::Cancelled)
                } else {
                    Ok(())
                });
                self.active = None;
            }
            Err(StationUpdateError::Pending) => {}
            Err(error) => {
                active.reversing = true;
                active.exchange.complete(Err(RemoteDspError::Update(error)));
            }
        }
    }
}
impl Drop for RemoteDspControl {
    fn drop(&mut self) {
        self.port.live.store(false, Ordering::Release);
    }
}

#[cfg(test)]
#[path = "tests/remote_dsp_tests.rs"]
mod tests;
