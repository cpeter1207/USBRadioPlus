use crate::*;
use usbradioplus_core::remote_operation::{RemoteOperation, RemoteOperationKind};
use usbradioplus_driver::{RemoteQueue, RemoteReply};

#[test]
fn activation_status_queries_reject_absent_handles_without_io() {
    let api = product_descriptor();
    // SAFETY: null handles must reject before any owner or device is accessed.
    unsafe {
        assert_eq!(
            api.native_activation_status.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.channel_activation_status.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
    }
}

unsafe extern "C" fn copy_text(context: *mut c_void, data: *const u8, length: u32) {
    // SAFETY: the test lends its owned String and the SDK lends UTF-8 through return.
    let (output, bytes) = unsafe {
        (
            &mut *context.cast::<String>(),
            std::slice::from_raw_parts(data, length as usize),
        )
    };
    output.push_str(std::str::from_utf8(bytes).unwrap());
}
#[test]
fn remote_ticket_boundary_is_nonblocking_and_keeps_exact_source_revocation() {
    let api = product_descriptor();
    for revoke in [false, true] {
        let queue = RemoteQueue::new(4);
        let ticket = queue
            .submit(
                RemoteOperation::new(RemoteOperationKind::Status),
                false,
                true,
            )
            .unwrap();
        let handle = Box::into_raw(Box::new(ticket)).cast();
        let mut output = String::new();
        // SAFETY: the test owns the opaque ticket and copies borrowed JSON synchronously.
        unsafe {
            assert_eq!(
                api.remote_take.unwrap()(
                    handle,
                    Some(copy_text),
                    ptr::from_mut(&mut output).cast()
                ),
                URP_AST_RELOAD_PENDING
            );
            assert!(output.is_empty());
            assert_eq!(
                api.remote_source.unwrap()(handle, 4),
                URP_AST_INVALID_ARGUMENT
            );
            assert_eq!(
                api.remote_source.unwrap()(handle, if revoke { 0 } else { 2 }),
                URP_AST_OK
            );
            queue.next().unwrap().complete(Ok(RemoteReply::Applied));
            assert_eq!(
                api.remote_take.unwrap()(
                    handle,
                    Some(copy_text),
                    ptr::from_mut(&mut output).cast()
                ),
                URP_AST_OK
            );
            assert!(output.contains(if revoke { "stale_source" } else { "applied" }));
            assert_eq!(
                api.remote_take.unwrap()(
                    handle,
                    Some(copy_text),
                    ptr::from_mut(&mut output).cast()
                ),
                URP_AST_RELOAD_PENDING
            );
            api.remote_destroy.unwrap()(handle);
        }
    }
}

#[test]
fn remote_abi_rejects_missing_owners_and_does_not_return_stale_tickets() {
    let api = product_descriptor();
    let operation = RemoteOperation::new(RemoteOperationKind::Status);
    // SAFETY: null owners are intentionally rejected; all other storage is live.
    unsafe {
        for submit in [
            api.native_remote_submit.unwrap(),
            api.channel_remote_submit.unwrap(),
        ] {
            let mut ticket = ptr::dangling_mut();
            assert_eq!(
                submit(ptr::null_mut(), &operation, 0, &mut ticket),
                URP_AST_INVALID_ARGUMENT
            );
            assert!(ticket.is_null());
        }
        assert_eq!(
            api.remote_source.unwrap()(ptr::null_mut(), 0),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.remote_take.unwrap()(ptr::null_mut(), Some(copy_text), ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.remote_take.unwrap()(ptr::null_mut(), None, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        api.remote_destroy.unwrap()(ptr::null_mut());
    }
}

#[test]
fn remote_scan_status_retains_failure_and_activity_hold() {
    use super::*;
    // SAFETY: the scan snapshot is a complete fixed scalar record.
    let mut snapshot: raw::RptAdvRemoteScanSnapshot = unsafe { std::mem::zeroed() };
    snapshot.flags = 1 | 4 | 8 | 16;
    snapshot.list = 7;
    snapshot.direction = 1;
    snapshot.failed_memory = 2;
    snapshot.last_error = -42;
    snapshot.priority = 3;
    snapshot.next_deadline_ms = 9000;
    let value = scan(Some(&snapshot));
    for (name, expected) in [
        ("direction", 1),
        ("failed_memory", 2),
        ("last_error", -42),
        ("priority", 3),
        ("deadline_ms", 9000),
    ] {
        assert_eq!(value[name], expected, "{name}");
    }
    assert_eq!(value["activity_hold"], true);
}

#[test]
fn remote_status_preserves_exact_units_and_truthful_observation_presence() {
    use super::*;
    use usbradioplus_core::RemoteScanList;
    // SAFETY: the copied snapshot contains only fixed scalar fields.
    let mut snapshot: raw::RptAdvRemoteSnapshot = unsafe { std::mem::zeroed() };
    snapshot.requested.receive_hz = 146_520_000;
    snapshot.requested.transmit_hz = 146_520_000;
    snapshot.requested.flags = 3;
    snapshot.requested.offset_hz = -600_000;
    snapshot.requested.power_mw = 2500;
    snapshot.requested.encode.source = 1;
    snapshot.requested.encode.enabled = 1;
    snapshot.requested.encode.ctcss_tenths_hz = 1148;
    let value = reply(Ok(RemoteReply::Status {
        request_error: 0,
        snapshot: Box::new(snapshot),
        scan: None,
    }));
    assert_eq!(value["version"], 1);
    assert_eq!(value["requested"]["offset_hz"], -600_000);
    assert_eq!(value["requested"]["power_mw"], 2500);
    assert_eq!(value["requested"]["encode"]["ctcss_tenths_hz"], 1148);
    assert!(value["applied"].is_null() && value["observed"].is_null());
    assert!(value["observed_values"].is_null());
    snapshot.flags = raw::RPTADV_REMOTE_OBSERVED | raw::RPTADV_REMOTE_INHIBITED;
    snapshot.observed = snapshot.requested;
    snapshot.observed_fields = 1;
    let value = reply(Ok(RemoteReply::Status {
        request_error: -123,
        snapshot: Box::new(snapshot),
        scan: None,
    }));
    assert_eq!(value["observed_values"]["rx_hz"], 146_520_000);
    assert!(value["observed_values"].get("tx_hz").is_none());
    assert_eq!(value["tx_inhibited"], true);
    assert!(value["serviced_ptt"].is_null());
    let list = RemoteScanList {
        selector: 3,
        name: "local \"channels\"".into(),
        members: vec![1, 2],
        dwell_ms: 500,
        resume_delay_ms: 2000,
        settle_ms: 0,
        priority: None,
    };
    let value = reply(Ok(RemoteReply::Scans(vec![list])));
    let decoded: Value = serde_json::from_str(&value.to_string()).unwrap();
    assert_eq!(decoded["entries"][0]["name"], "local \"channels\"");
    assert_eq!(decoded["entries"][0]["dwell_ms"], 500);
}

#[test]
fn remote_reply_copies_all_catalog_forms_and_failure_causes() {
    use super::*;
    use usbradioplus_core::{RemoteMemory, RemoteOperatingState, RemoteScanList};
    for (error, reason) in [
        (RemoteRequestError::Busy, "busy"),
        (RemoteRequestError::Stopped, "stopped"),
        (RemoteRequestError::StaleSource, "stale_source"),
        (RemoteRequestError::Rejected(-42), "rejected"),
    ] {
        let value = reply(Err(error));
        assert_eq!(value["reason"], reason);
        assert_eq!(value["version"], 1);
    }
    let definition = Setting {
        selector: 11,
        name: "power".into(),
        readable: true,
        writable: true,
        value_kind: 2,
        units: 2,
        decimal_places: 0,
        minimum: 0,
        maximum: 10_000,
        choices: vec![2500, 5000],
    };
    assert_eq!(
        reply(Ok(RemoteReply::Value {
            setting: definition.clone(),
            value: 2500
        }))["value"],
        2500
    );
    assert_eq!(
        reply(Ok(RemoteReply::Capabilities(vec![definition])))["entries"][0]["choices"],
        json!([2500, 5000])
    );
    let memory = RemoteMemory {
        selector: 1,
        name: "Local".into(),
        state: RemoteOperatingState {
            receive_hz: 146_520_000,
            transmit_hz: Some(146_520_000),
            mode: 1,
            ..RemoteOperatingState::default()
        },
    };
    assert_eq!(
        reply(Ok(RemoteReply::Memories(vec![memory])))["entries"][0]["name"],
        "Local"
    );
    let definition = RemoteScanList {
        selector: 2,
        name: "Local".into(),
        members: vec![1],
        dwell_ms: 500,
        resume_delay_ms: 2000,
        settle_ms: 0,
        priority: None,
    };
    assert!(
        reply(Ok(RemoteReply::Scan {
            definition: definition.clone(),
            state: None
        }))["state"]
            .is_null()
    );
    // SAFETY: the provider's copied snapshot contains only scalar fields.
    let mut snapshot: raw::RptAdvRemoteSnapshot = unsafe { std::mem::zeroed() };
    snapshot.requested.receive_hz = 146_520_000;
    snapshot.requested.transmit_hz = 146_520_000;
    snapshot.requested.mode = 1;
    snapshot.requested.flags = 1;
    snapshot.applied = snapshot.requested;
    snapshot.observed = snapshot.requested;
    snapshot.flags = raw::RPTADV_REMOTE_APPLIED
        | raw::RPTADV_REMOTE_OBSERVED
        | raw::RPTADV_REMOTE_PTT_APPLIED
        | raw::RPTADV_REMOTE_PTT_HIGH;
    let value = reply(Ok(RemoteReply::Status {
        request_error: 0,
        snapshot: Box::new(snapshot),
        scan: None,
    }));
    assert_eq!(value["serviced_ptt"], true);
    assert!(
        value["observed_values"].is_null(),
        "no observed fields are invented"
    );
    // Reject an unknown enum returned across the provider ABI instead of
    // inventing a valid state or publishing untrusted telemetry fragments.
    snapshot.requested.encode.source = u32::MAX;
    let invalid = reply(Ok(RemoteReply::Status {
        request_error: 0,
        snapshot: Box::new(snapshot),
        scan: None,
    }));
    assert_eq!(invalid["reason"], "rejected");
    assert_eq!(invalid["code"], -1003);
}
