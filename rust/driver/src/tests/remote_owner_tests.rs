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
    apply_error: Option<i32>,
    idle_result: Option<Result<bool, CallError>>,
    setting_error: Option<i32>,
}
impl OperatingIo for Device {
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
        Ok(self.state)
    }
    fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        self.writes += 1;
        if let Some(error) = self.apply_error {
            return Err(CallError(error));
        }
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
        self.idle_result.unwrap_or(Ok(!self.keyed))
    }
    fn get_setting(&mut self, _: u32, _: &raw::RptAdvRemoteSnapshot) -> Result<i64, CallError> {
        self.setting_error
            .map_or(Ok(self.scalar), |code| Err(CallError(code)))
    }
    fn set_setting(&mut self, _: u32, value: i64) -> Result<(), CallError> {
        if let Some(error) = self.setting_error {
            return Err(CallError(error));
        }
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
    configured_owner(crate::remote_request::tests::configuration())
}
fn configured_owner(
    configuration: ResolvedRemoteConfiguration,
) -> (RemoteOwner, Arc<RemoteQueue>, Device) {
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
        scan_modes: 2 | 8,
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
        configuration,
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
        apply_error: None,
        idle_result: None,
        setting_error: None,
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

fn query(
    owner: &mut RemoteOwner,
    queue: &RemoteQueue,
    device: &mut Device,
    operation: RemoteOperation,
    now: u64,
) -> Result<RemoteReply, RemoteRequestError> {
    let ticket = queue.submit(operation, true, false).unwrap();
    owner.service(now, activity(), device).unwrap();
    ticket
        .take_completion()
        .expect("query completes in its admission cycle")
}

#[test]
fn remote_owner_catalogs_cover_named_entries_empty_pages_and_rejected_queries() {
    let mut config = crate::remote_request::tests::configuration();
    for (selector, name) in [(7, "local"), (8, "other")] {
        config.scan_lists.push(usbradioplus_core::RemoteScanList {
            selector,
            name: name.into(),
            members: vec![1],
            dwell_ms: 500,
            resume_delay_ms: 2000,
            settle_ms: 0,
            priority: None,
        });
    }
    let (mut owner, queue, mut device) = configured_owner(config);
    owner.service(0, activity(), &mut device).unwrap();
    for (kind, selector) in [
        (Kind::Memory, 1),
        (Kind::Catalog, 2),
        (Kind::Catalog, 3),
        (Kind::ScanStatus, 7),
    ] {
        let mut operation = RemoteOperation::new(kind);
        operation.selector = selector;
        operation.magnitude = 1;
        match query(&mut owner, &queue, &mut device, operation, 1).unwrap() {
            RemoteReply::Memories(entries) => assert_eq!(entries.len(), 1),
            RemoteReply::Scans(entries) => assert_eq!(entries.len(), 2),
            RemoteReply::Scan { definition, state } => {
                assert_eq!(definition.selector, 7);
                assert!(state.is_none());
            }
            _ => panic!("unexpected catalog response"),
        }
    }
    for (kind, selector, page, code) in [
        (Kind::Memory, 99, 1, -7),
        (Kind::Catalog, 0, 1, -1003),
        (Kind::Catalog, 1, 0, -1003),
        (Kind::Catalog, 1, u64::MAX, -1003),
        (Kind::Catalog, 1, 99, -7),
        (Kind::ScanStatus, 99, 1, -7),
        (Kind::GetSetting, 99, 1, -6),
    ] {
        let mut operation = RemoteOperation::new(kind);
        operation.selector = selector;
        operation.magnitude = page;
        assert_eq!(
            query(&mut owner, &queue, &mut device, operation, 1).err(),
            Some(RemoteRequestError::Rejected(code))
        );
    }
    let mut start = RemoteOperation::new(Kind::StartScan);
    start.selector = 7;
    start.flags = 1;
    let ticket = queue.submit(start, true, false).unwrap();
    owner.service(2, activity(), &mut device).unwrap();
    owner.service(3, activity(), &mut device).unwrap();
    assert!(matches!(
        ticket.take_completion(),
        Some(Ok(RemoteReply::Applied))
    ));
    for (selector, present) in [(7, true), (8, false)] {
        let mut operation = RemoteOperation::new(Kind::ScanStatus);
        operation.selector = selector;
        let RemoteReply::Scan { state, .. } =
            query(&mut owner, &queue, &mut device, operation, 4).unwrap()
        else {
            panic!("scan query")
        };
        assert_eq!(state.is_some(), present);
    }
    let (mut owner, queue, mut device) = self::owner();
    owner.service(0, activity(), &mut device).unwrap();
    let mut empty = RemoteOperation::new(Kind::Catalog);
    empty.selector = 3;
    empty.magnitude = 1;
    assert!(
        matches!(query(&mut owner,&queue,&mut device,empty,1),Ok(RemoteReply::Scans(entries)) if entries.is_empty())
    );
}

#[test]
fn remote_owner_waits_for_startup_and_completes_revoked_pending_sources() {
    let (mut owner, queue, mut device) = owner();
    let mut busy = activity();
    busy.flags = raw::RPTADV_REMOTE_PHYSICAL_PTT;
    let mut read = RemoteOperation::new(Kind::GetSetting);
    read.selector = 400;
    let read = queue.submit(read, true, false).unwrap();
    owner.service(0, busy, &mut device).unwrap();
    assert_eq!(
        read.take_completion().unwrap().err(),
        Some(RemoteRequestError::Rejected(-6))
    );
    let mut recall = RemoteOperation::new(Kind::Recall);
    recall.selector = 1;
    let blocked = queue.submit(recall, true, false).unwrap();
    owner.service(1, busy, &mut device).unwrap();
    assert_eq!(
        blocked.take_completion().unwrap().err(),
        Some(RemoteRequestError::Busy)
    );
    let mut setting = RemoteOperation::new(Kind::SetSetting);
    setting.selector = 100;
    setting.magnitude = 250_000;
    setting.decimal_places = 6;
    let waiting = queue.submit(setting, true, false).unwrap();
    owner.service(2, busy, &mut device).unwrap();
    assert!(waiting.take_completion().is_none());
    assert_eq!(device.scalar, 500_000);
    owner.service(3, activity(), &mut device).unwrap();
    assert!(matches!(
        waiting.take_completion(),
        Some(Ok(RemoteReply::Applied))
    ));
    let ticket = queue.submit(recall, true, true).unwrap();
    owner.service(4, activity(), &mut device).unwrap();
    ticket.source_state(false, false);
    owner.service(5, activity(), &mut device).unwrap();
    assert_eq!(
        ticket.take_completion().unwrap().err(),
        Some(RemoteRequestError::StaleSource)
    );
}

#[test]
fn remote_owner_auxiliary_waits_preserve_source_and_physical_failures() {
    let (mut owner, queue, mut device) = owner();
    owner.service(0, activity(), &mut device).unwrap();
    let mut setting = RemoteOperation::new(Kind::SetSetting);
    setting.selector = 100;
    setting.magnitude = 250_000;
    setting.decimal_places = 6;
    let pending = queue.submit(setting, true, true).unwrap();
    owner.service(1, activity(), &mut device).unwrap();
    pending.source_state(false, false);
    owner.service(2, activity(), &mut device).unwrap();
    assert_eq!(
        pending.take_completion().unwrap().err(),
        Some(RemoteRequestError::StaleSource)
    );
    device.idle_result = Some(Ok(false));
    let waiting = queue.submit(setting, true, false).unwrap();
    owner.service(3, activity(), &mut device).unwrap();
    assert!(waiting.take_completion().is_none());
    assert_eq!(device.scalar, 500_000);
    device.idle_result = Some(Err(CallError(-42)));
    owner.service(4, activity(), &mut device).unwrap();
    assert_eq!(
        waiting.take_completion().unwrap().err(),
        Some(RemoteRequestError::Rejected(-42))
    );
    device.idle_result = None;
    device.setting_error = Some(-42);
    assert_eq!(
        query(&mut owner, &queue, &mut device, setting, 5).err(),
        Some(RemoteRequestError::Rejected(-42))
    );
    let mut read = RemoteOperation::new(Kind::GetSetting);
    read.selector = 100;
    assert_eq!(
        query(&mut owner, &queue, &mut device, read, 6).err(),
        Some(RemoteRequestError::Rejected(-42))
    );
    assert_eq!(device.scalar, 500_000);
}

#[test]
fn remote_owner_drains_physical_failure_and_already_completed_retired_work() {
    let (mut owner, queue, mut device) = owner();
    owner.service(0, activity(), &mut device).unwrap();
    device.apply_error = Some(-47);
    let mut recall = RemoteOperation::new(Kind::Recall);
    recall.selector = 1;
    let ticket = queue.submit(recall, true, false).unwrap();
    owner.service(1, activity(), &mut device).unwrap();
    assert_eq!(
        owner.service(2, activity(), &mut device).err(),
        Some(CallError(-8))
    );
    assert_eq!(
        ticket.take_completion().unwrap().err(),
        Some(RemoteRequestError::Rejected(-8))
    );
    assert_eq!(owner.snapshot().unwrap().failure_code, -47);
    let (mut owner, queue, mut device) = self::owner();
    owner.service(0, activity(), &mut device).unwrap();
    let ticket = queue
        .submit(RemoteOperation::new(Kind::Inhibit), true, false)
        .unwrap();
    owner.service(1, activity(), &mut device).unwrap();
    assert_eq!(owner.pending.len(), 1);
    owner.retire().unwrap();
    assert_eq!(
        owner.service(2, activity(), &mut device).err(),
        Some(CallError(-10))
    );
    assert!(owner.pending.is_empty());
    assert_eq!(
        ticket.take_completion().unwrap().err(),
        Some(RemoteRequestError::Stopped)
    );
}

#[test]
fn remote_owner_legacy_channel_frames_use_the_same_engine_transaction() {
    use usbradioplus_core::radio_control::ParallelRadioRequest;
    let mut config = crate::remote_request::tests::configuration();
    config.memories[0].state.channel = Some(7);
    let (mut owner, queue, mut device) = configured_owner(config);
    owner.service(0, activity(), &mut device).unwrap();
    queue
        .submit_parallel(ParallelRadioRequest::SelectChannel(7))
        .unwrap();
    owner.service(1, activity(), &mut device).unwrap();
    owner.service(2, activity(), &mut device).unwrap();
    assert_eq!(queue.parallel_error(), 0);
    assert_eq!(device.state.channel, 7);
    queue
        .submit_parallel(ParallelRadioRequest::SelectChannel(8))
        .unwrap();
    owner.service(3, activity(), &mut device).unwrap();
    assert_eq!(queue.parallel_error(), -6);
}
