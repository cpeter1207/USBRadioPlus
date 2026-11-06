//! Commands and completions on the existing serialized physical radio owner.

use crate::{
    PreparedRemoteOperation, RemoteQueue, RemoteReply, RemoteRequestError, RemoteSourceLease,
    RemoteWork, prepare_remote_operation,
};
use std::sync::Arc;
use usbradioplus_core::{
    ResolvedRemoteConfiguration, remote_operation::RemoteOperationKind as Kind,
};
use usbradioplus_remote::{
    CallError, OperatingIo, RemoteProvider, RemoteSession, SelectedCapabilities, Setting,
    operating_configuration, raw,
};

/// Declared scalar operations on the same non-audio physical device owner.
/// RF-affecting operating fields enter the dynamic engine instead of this path.
pub trait RemoteDevice: OperatingIo {
    /// Confirm the selected physical PTT output is off before an auxiliary write.
    /// Unknown state is not idle; implementations must never key to answer this.
    fn transmit_idle(&mut self) -> Result<bool, CallError>;
    /// Read one actual supported control using current applied/observed context.
    fn get_setting(
        &mut self,
        selector: u32,
        snapshot: &raw::RptAdvRemoteSnapshot,
    ) -> Result<i64, CallError>;
    /// Set one admitted auxiliary control, never frequency/mode/PTT/power.
    fn set_setting(&mut self, selector: u32, value: i64) -> Result<(), CallError>;
}

struct Pending {
    id: u64,
    work: RemoteWork,
}
struct Auxiliary {
    work: RemoteWork,
    selector: u32,
    value: i64,
}

/// One creating-thread-owned command bridge to the portable remote engine.
/// It adds no tune/scan policy and no device thread. Slow presentation consumes
/// copied completions without retaining the physical owner or foreign callbacks.
pub struct RemoteOwner {
    auxiliary: Option<Auxiliary>,
    configuration: ResolvedRemoteConfiguration,
    session: RemoteSession,
    settings: Vec<Setting>,
    queue: Arc<RemoteQueue>,
    // The engine admits one ordinary operation and one reserved critical one.
    pending: Vec<Pending>,
}

fn rejected(code: i32) -> RemoteRequestError {
    match code {
        -9 => RemoteRequestError::Busy,
        -10 => RemoteRequestError::StaleSource,
        code => RemoteRequestError::Rejected(code),
    }
}
fn page<T: Clone>(values: &[T], page: u64) -> Result<Vec<T>, RemoteRequestError> {
    let offset = page
        .checked_sub(1)
        .and_then(|page| page.checked_mul(5))
        .and_then(|page| usize::try_from(page).ok())
        .ok_or_else(|| rejected(-1003))?;
    if offset >= values.len() && offset != 0 {
        return Err(rejected(-7));
    }
    Ok(values.iter().skip(offset).take(5).cloned().collect())
}

impl RemoteOwner {
    /// Create on the existing physical worker after actual capability admission.
    /// The engine copies definitions and validates every configured state before
    /// its first physical operation. Catalog copies use resolved source choices.
    pub fn new(
        mut configuration: ResolvedRemoteConfiguration,
        caps: &SelectedCapabilities,
        settings: Vec<Setting>,
        queue: Arc<RemoteQueue>,
        provider: Arc<RemoteProvider>,
    ) -> Result<Self, CallError> {
        let session = RemoteSession::configured(provider, &configuration, caps)?;
        for memory in &mut configuration.memories {
            memory.state = operating_configuration(&session.configured_memory(memory.selector)?)?;
        }
        Ok(Self {
            auxiliary: None,
            configuration,
            session,
            settings: crate::remote_catalog::controller_catalog(settings, caps),
            queue,
            pending: Vec::with_capacity(2),
        })
    }
    /// Retain atomic validity for stop checks between exchanges in a blocking
    /// operation. Neither source objects nor controller/peer pointers are shared.
    pub fn pending_source(&self) -> Option<RemoteSourceLease> {
        self.pending
            .iter()
            .find(|pending| {
                !pending
                    .work
                    .operation()
                    .is_some_and(|operation| operation.urgent())
            })
            .map(|pending| pending.work.source_lease())
    }
    /// Copy current engine state without physical I/O.
    pub fn snapshot(&self) -> Result<raw::RptAdvRemoteSnapshot, CallError> {
        self.session.snapshot()
    }
    /// Revoke admission before worker-local safe shutdown. Existing caller
    /// tickets become stopped even while the physical unkey is being retried.
    pub fn retire(&mut self) -> Result<(), CallError> {
        self.queue.retire();
        self.auxiliary = None;
        self.pending.clear();
        self.session.retire()
    }
    /// Service safety work first, then renderer/PTT demand and completions, then
    /// at most one ordinary request. Each rig has its own owner and bounded port.
    pub fn service<D: RemoteDevice>(
        &mut self,
        now: u64,
        mut activity: raw::RptAdvRemoteActivity,
        device: &mut D,
    ) -> Result<raw::RptAdvRemoteSnapshot, CallError> {
        if let Some(work) = self.queue.next_urgent() {
            self.admit(work, now, device);
        }
        if let Some(source) = self.pending_source().map(|lease| lease.source()) {
            activity.flags |= (u32::from(source.active) * raw::RPTADV_REMOTE_COMMAND_ACTIVE)
                | (u32::from(!source.valid) * raw::RPTADV_REMOTE_COMMAND_CANCELLED);
        }
        let result = self.session.advance_with(now, &activity, device);
        while let Some(completion) = self.session.take_completion()? {
            if let Some(index) = self
                .pending
                .iter()
                .position(|pending| pending.id == completion.request_id)
            {
                let pending = self.pending.swap_remove(index);
                pending.work.complete(if completion.result == 0 {
                    Ok(RemoteReply::Applied)
                } else {
                    Err(rejected(completion.result))
                });
            }
        }
        if self.auxiliary.is_none() {
            if let Some(work) = self.queue.next_ordinary() {
                self.admit(work, now, device);
            }
        }
        if result.is_ok() {
            if let Some(waiting) = self.auxiliary.take() {
                let snapshot = self.session.snapshot()?;
                if !waiting.work.source().valid {
                    waiting.work.complete(Err(RemoteRequestError::StaleSource));
                } else if waiting.work.source().active
                    || snapshot.flags & raw::RPTADV_REMOTE_PENDING != 0
                    || activity.flags
                        & (raw::RPTADV_REMOTE_PHYSICAL_PTT | raw::RPTADV_REMOTE_RENDERED_PTT)
                        != 0
                {
                    self.auxiliary = Some(waiting);
                } else {
                    match device.transmit_idle() {
                        Ok(false) => self.auxiliary = Some(waiting),
                        Ok(true) => {
                            // The physical exchange may block while the initiating
                            // connection, administrative grant or key state changes.
                            let source = waiting.work.source();
                            if !source.valid {
                                waiting.work.complete(Err(RemoteRequestError::StaleSource));
                            } else if source.active {
                                self.auxiliary = Some(waiting);
                            } else {
                                waiting.work.complete(
                                    device
                                        .set_setting(waiting.selector, waiting.value)
                                        .map(|()| RemoteReply::Applied)
                                        .map_err(|error| rejected(error.0)),
                                );
                            }
                        }
                        Err(error) => waiting.work.complete(Err(rejected(error.0))),
                    }
                }
            }
        }
        result?;
        self.session.snapshot()
    }
    fn admit<D: RemoteDevice>(&mut self, work: RemoteWork, now: u64, device: &mut D) {
        if !work.source().valid {
            work.complete(Err(RemoteRequestError::StaleSource));
            return;
        }
        let result = self.prepare(&work, device);
        match result {
            Ok(Ready::Reply(reply)) => work.complete(Ok(reply)),
            Ok(Ready::Auxiliary { selector, value }) => {
                self.auxiliary = Some(Auxiliary {
                    work,
                    selector,
                    value,
                })
            }
            Ok(Ready::Core(request)) => match self.session.request(&request, now) {
                Ok(id) => self.pending.push(Pending { id, work }),
                Err(error) => work.complete(Err(rejected(error.0))),
            },
            Err(error) => work.complete(Err(error)),
        }
    }
    fn prepare<D: RemoteDevice>(
        &mut self,
        work: &RemoteWork,
        device: &mut D,
    ) -> Result<Ready, RemoteRequestError> {
        if let Some(request) = work.parallel_request() {
            let snapshot = self.session.snapshot().map_err(|error| rejected(error.0))?;
            return crate::remote_request::prepare_parallel_request(
                &self.configuration,
                &snapshot,
                request,
            )
            .map(Ready::Core);
        }
        let operation = work.operation().expect("one typed request variant");
        let kind = operation.kind().ok_or_else(|| rejected(-1003))?;
        let mut snapshot = self.session.snapshot().map_err(|error| rejected(error.0))?;
        let scan = self
            .session
            .scan_snapshot()
            .map_err(|error| rejected(error.0))?;
        let reply = match kind {
            Kind::Status | Kind::CurrentStatus => RemoteReply::Status {
                request_error: self.queue.parallel_error(),
                snapshot: Box::new(snapshot),
                scan,
            },
            Kind::GetSetting => {
                let setting = self
                    .settings
                    .iter()
                    .find(|setting| setting.selector == operation.selector && setting.readable)
                    .ok_or_else(|| rejected(-6))?
                    .clone();
                let value = match crate::remote_catalog::signal_value(
                    &snapshot.applied,
                    operation.selector,
                ) {
                    Some(value) if snapshot.flags & raw::RPTADV_REMOTE_APPLIED != 0 => value,
                    Some(_) => return Err(rejected(-6)),
                    None => device
                        .get_setting(operation.selector, &snapshot)
                        .map_err(|error| rejected(error.0))?,
                };
                RemoteReply::Value { setting, value }
            }
            Kind::Memory => RemoteReply::Memories(vec![
                self.configuration
                    .memories
                    .iter()
                    .find(|memory| memory.selector == operation.selector)
                    .ok_or_else(|| rejected(-7))?
                    .clone(),
            ]),
            Kind::Catalog => match operation.selector {
                1 => RemoteReply::Capabilities(page(&self.settings, operation.magnitude)?),
                2 => {
                    RemoteReply::Memories(page(&self.configuration.memories, operation.magnitude)?)
                }
                3 => RemoteReply::Scans(page(&self.configuration.scan_lists, operation.magnitude)?),
                _ => return Err(rejected(-1003)),
            },
            Kind::ScanStatus => RemoteReply::Scan {
                definition: self
                    .configuration
                    .scan_lists
                    .iter()
                    .find(|list| list.selector == operation.selector)
                    .ok_or_else(|| rejected(-7))?
                    .clone(),
                state: scan.filter(|scan| scan.list == operation.selector),
            },
            _ => {
                if snapshot.flags & raw::RPTADV_REMOTE_APPLIED == 0 {
                    snapshot.applied = snapshot.requested;
                }
                return match prepare_remote_operation(
                    &self.configuration,
                    &snapshot,
                    scan.as_ref(),
                    &self.settings,
                    operation,
                )? {
                    PreparedRemoteOperation::Core(request) => Ok(Ready::Core(request)),
                    PreparedRemoteOperation::Auxiliary { selector, value } => {
                        Ok(Ready::Auxiliary { selector, value })
                    }
                };
            }
        };
        Ok(Ready::Reply(reply))
    }
}
enum Ready {
    Auxiliary { selector: u32, value: i64 },
    Reply(RemoteReply),
    Core(raw::RptAdvRemoteRequest),
}

impl Drop for RemoteOwner {
    fn drop(&mut self) {
        // A lost/panicked physical owner cannot leave admitted callers waiting.
        // Physical retirement/unkey remains on the original service worker.
        self.queue.retire();
    }
}

#[cfg(test)]
#[path = "tests/remote_owner_tests.rs"]
mod tests;
