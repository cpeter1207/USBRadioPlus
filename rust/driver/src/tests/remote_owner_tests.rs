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
