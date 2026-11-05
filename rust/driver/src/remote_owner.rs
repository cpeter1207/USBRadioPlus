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
mod tests {
    use super::*;
    use crate::{RemoteQueue, RemoteReply, RemoteRequestError};
    use std::{mem::size_of, sync::Arc};
    use usbradioplus_core::remote_operation::{RemoteOperation, RemoteOperationKind as Kind};
    use usbradioplus_remote::{
        CallError, OperatingIo, RemoteProvider, SelectedCapabilities, Setting, raw,
    };

    struct Device {
        idle_change: Option<(Arc<crate::RemoteTicket>, bool)>,
        state: raw::RptAdvRemoteState,
        writes: usize,
        scalar: i64,
        keyed: bool,
    }
    impl OperatingIo for Device {
        fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
            Ok(self.state)
        }
        fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
            self.writes += 1;
            self.state = *state;
            Ok(())
        }
        fn set_ptt(&mut self, keyed: bool) -> Result<(), CallError> {
            self.keyed = keyed;
            Ok(())
        }
        fn set_tune(&mut self, _: bool) -> Result<(), CallError> {
            Ok(())
        }
        fn monotonic_ms(&self) -> u64 {
            0
        }
        fn transmit_epoch(&self) -> u64 {
            0
        }
    }
    impl RemoteDevice for Device {
        fn transmit_idle(&mut self) -> Result<bool, CallError> {
            if let Some((ticket, rekey)) = self.idle_change.take() {
                ticket.source_state(rekey, rekey);
            }
            Ok(!self.keyed)
        }
        fn get_setting(&mut self, _: u32, _: &raw::RptAdvRemoteSnapshot) -> Result<i64, CallError> {
            Ok(self.scalar)
        }
        fn set_setting(&mut self, _: u32, value: i64) -> Result<(), CallError> {
            self.scalar = value;
            Ok(())
        }
    }
    fn activity() -> raw::RptAdvRemoteActivity {
        raw::RptAdvRemoteActivity {
            struct_size: size_of::<raw::RptAdvRemoteActivity>() as u32,
            flags: 0,
        }
    }

    #[test]
    fn remote_owner_reads_applied_dsp_signaling_without_a_native_scalar_call() {
        let (mut owner, queue, mut device) = owner();
        owner.service(0, activity(), &mut device).unwrap();
        let state = owner.snapshot().unwrap().applied;
        for (selector, expected) in [
            (12, i64::from(state.encode.ctcss_tenths_hz)),
            (14, i64::from(state.encode.dcs_code)),
            (16, i64::from(state.encode.enabled)),
            (400, i64::from(state.encode.source)),
            (401, i64::from(state.decode.source)),
            (402, i64::from(state.decode.control_source)),
            (403, i64::from(state.encode.inverted)),
        ] {
            let mut operation = RemoteOperation::new(Kind::GetSetting);
            operation.selector = selector;
            let ticket = queue.submit(operation, true, false).unwrap();
            owner.service(1, activity(), &mut device).unwrap();
            match ticket.take_completion() {
                Some(Ok(RemoteReply::Value { value, .. })) => {
                    assert_eq!(value, expected, "selector {selector}")
                }
                Some(Err(error)) => panic!("selector {selector}: {error:?}"),
                _ => panic!("selector {selector} did not complete with a scalar"),
            }
        }
        assert_eq!(device.scalar, 500000);
    }

    #[test]
    fn auxiliary_radio_writes_wait_for_command_unkey_and_serviced_ptt_off() {
        let (mut owner, queue, mut device) = owner();
        owner.service(0, activity(), &mut device).unwrap();
        let mut operation = RemoteOperation::new(Kind::SetSetting);
        operation.selector = 100;
        operation.magnitude = 250000;
        operation.decimal_places = 6;
        let ticket = queue.submit(operation, true, true).unwrap();
        owner.service(1, activity(), &mut device).unwrap();
        assert_eq!(device.scalar, 500000, "no CAT write before source unkeys");
        assert!(ticket.take_completion().is_none());
        ticket.source_state(false, true);
        let mut keyed = activity();
        keyed.flags = raw::RPTADV_REMOTE_PHYSICAL_PTT
            | raw::RPTADV_REMOTE_RENDERED_PTT
            | raw::RPTADV_REMOTE_REQUEST_PTT;
        owner.service(2, keyed, &mut device).unwrap();
        assert_eq!(device.scalar, 500000, "no live-transmitter auxiliary write");
        owner.service(3, activity(), &mut device).unwrap();
        assert_eq!(device.scalar, 250000);
        assert!(matches!(
            ticket.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
    }
    #[test]
    fn auxiliary_radio_rechecks_source_after_the_physical_idle_exchange() {
        for rekey in [false, true] {
            let (mut owner, queue, mut device) = owner();
            owner.service(0, activity(), &mut device).unwrap();
            let mut operation = RemoteOperation::new(Kind::SetSetting);
            operation.selector = 100;
            operation.magnitude = 250000;
            operation.decimal_places = 6;
            let ticket = Arc::new(queue.submit(operation, true, false).unwrap());
            device.idle_change = Some((Arc::clone(&ticket), rekey));
            owner.service(1, activity(), &mut device).unwrap();
            assert_eq!(device.scalar, 500000);
            if rekey {
                assert!(ticket.take_completion().is_none());
                ticket.source_state(false, true);
                owner.service(2, activity(), &mut device).unwrap();
                assert_eq!(device.scalar, 250000);
            } else {
                assert!(matches!(
                    ticket.take_completion(),
                    Some(Err(RemoteRequestError::StaleSource))
                ));
            }
        }
    }
    fn owner() -> (RemoteOwner, Arc<RemoteQueue>, Device) {
        let range = raw::RptAdvRemoteCoverage {
            minimum: 144000000,
            maximum: 148000000,
            mode_mask: 2 | 8,
            flags: 0,
            minimum_power_mw: 0,
            maximum_power_mw: 0,
        };
        let caps = SelectedCapabilities {
            receive: vec![range],
            transmit: vec![range],
            modes: 2 | 8,
            manual_tune: true,
            encode_sources: 2,
            decode_sources: 2,
            decode_control_sources: 2,
            ..Default::default()
        };
        let queue = Arc::new(RemoteQueue::new(2));
        let setting = Setting {
            selector: 100,
            name: "AF".into(),
            readable: true,
            writable: true,
            value_kind: 4,
            units: 6,
            decimal_places: 6,
            minimum: 0,
            maximum: 1000000,
            choices: vec![],
        };
        let owner = RemoteOwner::new(
            crate::remote_request::tests::configuration(),
            &caps,
            vec![setting],
            Arc::clone(&queue),
            Arc::new(RemoteProvider::load().unwrap()),
        )
        .unwrap();
        let device = Device {
            idle_change: None,
            // SAFETY: all fields are scalar C records.
            state: unsafe { std::mem::zeroed() },
            writes: 0,
            scalar: 500000,
            keyed: false,
        };
        (owner, queue, device)
    }
    #[test]
    fn remote_owner_retains_source_until_unkey_and_completes_actual_application() {
        let (mut owner, queue, mut device) = owner();
        owner.service(0, activity(), &mut device).unwrap();
        assert_eq!(device.writes, 1);
        let mut recall = RemoteOperation::new(Kind::Recall);
        recall.selector = 1;
        let ticket = queue.submit(recall, false, true).unwrap();
        owner.service(1, activity(), &mut device).unwrap();
        owner.service(2, activity(), &mut device).unwrap();
        assert_eq!(device.writes, 1);
        assert!(ticket.take_completion().is_none());
        ticket.source_state(false, true);
        owner.service(3, activity(), &mut device).unwrap();
        assert_eq!(device.writes, 2);
        assert!(matches!(
            ticket.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
        assert!(owner.pending_source().is_none());
        let stale = queue.submit(recall, false, false).unwrap();
        stale.source_state(false, false);
        owner.service(4, activity(), &mut device).unwrap();
        assert!(matches!(
            stale.take_completion(),
            Some(Err(RemoteRequestError::StaleSource))
        ));
        assert_eq!(device.writes, 2);
    }
    #[test]
    fn remote_owner_queries_are_bounded_and_urgent_inhibit_bypasses_ordinary_work() {
        let (mut owner, queue, mut device) = owner();
        owner.service(0, activity(), &mut device).unwrap();
        let mut read = RemoteOperation::new(Kind::GetSetting);
        read.selector = 100;
        let ticket = queue.submit(read, true, false).unwrap();
        owner.service(1, activity(), &mut device).unwrap();
        assert!(matches!(
            ticket.take_completion(),
            Some(Ok(RemoteReply::Value { value: 500000, .. }))
        ));
        let mut recall = RemoteOperation::new(Kind::Recall);
        recall.selector = 1;
        let pending = queue.submit(recall, true, true).unwrap();
        owner.service(2, activity(), &mut device).unwrap();
        let mut inhibit = RemoteOperation::new(Kind::Inhibit);
        inhibit.flags = 1;
        let urgent = queue.submit(inhibit, true, true).unwrap();
        owner.service(3, activity(), &mut device).unwrap();
        assert!(matches!(
            urgent.take_completion(),
            Some(Ok(RemoteReply::Applied))
        ));
        assert!(pending.take_completion().is_none());
        owner.retire().unwrap();
        assert!(matches!(
            pending.take_completion(),
            Some(Err(RemoteRequestError::Stopped))
        ));
        assert_eq!(
            owner.service(4, activity(), &mut device).err(),
            Some(CallError(-10))
        );
        assert!(!device.keyed);
    }

    #[test]
    fn remote_owner_loss_retires_queued_requests_and_catalog_pages_are_bounded() {
        let (mut owner, queue, mut device) = owner();
        owner.service(0, activity(), &mut device).unwrap();
        let mut query = RemoteOperation::new(Kind::Catalog);
        query.selector = 1;
        query.magnitude = 1;
        let ticket = queue.submit(query, true, false).unwrap();
        owner.service(1, activity(), &mut device).unwrap();
        assert!(
            matches!(ticket.take_completion(), Some(Ok(RemoteReply::Capabilities(entries))) if entries.len() == 5)
        );
        let waiting = queue
            .submit(RemoteOperation::new(Kind::Status), true, false)
            .unwrap();
        drop(owner);
        assert!(matches!(
            waiting.take_completion(),
            Some(Err(RemoteRequestError::Stopped))
        ));
    }
}
