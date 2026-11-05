//! Copied asynchronous remote operations; no host or peer pointer reaches CAT.

use crate::{Status, UrpNativeProcessingDiagnostic, copy_abi, ffi_status, output_pointer};
use serde_json::{Value, json};
use std::ffi::{c_int, c_void};
use usbradioplus_core::{RemoteOperatingState, RemoteSignal};
use usbradioplus_driver::{HardwareService, RemoteReply, RemoteRequestError, RemoteTicket};
use usbradioplus_remote::{Setting, operating_configuration, raw};

/// Pointer-free operation ABI shared with the remote engine's host adapters.
pub use usbradioplus_core::remote_operation::RemoteOperation as UrpRemoteOperation;
/// Nonblocking submission; success transfers one caller-owned ticket.
pub type UrpRemoteSubmit =
    unsafe extern "C" fn(*mut c_void, *const UrpRemoteOperation, u32, *mut *mut c_void) -> c_int;

fn submit(
    service: &HardwareService,
    operation: UrpRemoteOperation,
    flags: u32,
    output: *mut *mut c_void,
) -> Result<(), Status> {
    let output = output_pointer(output)?;
    if flags & !3 != 0 {
        return Err(Status::InvalidArgument);
    }
    let ticket = service
        .remote_submit(operation, flags & 1 != 0, flags & 2 != 0)
        .map_err(|error| match error {
            RemoteRequestError::Busy => Status::ChannelBusy,
            RemoteRequestError::Rejected(_) => Status::InvalidArgument,
            RemoteRequestError::Stopped | RemoteRequestError::StaleSource => Status::NotReady,
        })?;
    // SAFETY: output_pointer admitted one writable pointer, and ownership transfers.
    unsafe {
        output
            .as_ptr()
            .write(Box::into_raw(Box::new(ticket)).cast())
    };
    Ok(())
}

pub(crate) unsafe extern "C" fn native_submit(
    handle: *mut c_void,
    operation: *const UrpRemoteOperation,
    flags: u32,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let _ = output_pointer(output)?;
        // SAFETY: the host serializes this live handle and lends the complete input.
        let (service, operation) =
            unsafe { (crate::native::site_service(handle)?, copy_abi(operation)?) };
        submit(service, operation, flags, output)
    })
}
pub(crate) unsafe extern "C" fn channel_submit(
    handle: *mut c_void,
    operation: *const UrpRemoteOperation,
    flags: u32,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let _ = output_pointer(output)?;
        // SAFETY: the taskprocessor serializes this live handle and complete input.
        let ((_, channel), operation) =
            unsafe { (crate::channel_control(handle)?, copy_abi(operation)?) };
        let station = channel.hardware.as_mut().ok_or(Status::NotReady)?;
        submit(
            station.maintenance_mut().service(),
            operation,
            flags,
            output,
        )
    })
}
pub(crate) unsafe extern "C" fn source(handle: *mut c_void, flags: u32) -> c_int {
    ffi_status(|| {
        if flags & !3 != 0 {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the serialized host retains its ticket through this call.
        let ticket = unsafe { handle.cast::<RemoteTicket>().as_ref() };
        let ticket = ticket.ok_or(Status::InvalidArgument)?;
        ticket.source_state(flags & 1 != 0, flags & 2 != 0);
        Ok(())
    })
}
pub(crate) unsafe extern "C" fn take(
    handle: *mut c_void,
    sink: Option<UrpNativeProcessingDiagnostic>,
    context: *mut c_void,
) -> c_int {
    ffi_status(|| {
        let sink = sink.ok_or(Status::InvalidArgument)?;
        // SAFETY: the serialized host retains its ticket through copying the result.
        let ticket = unsafe { handle.cast::<RemoteTicket>().as_ref() };
        let ticket = ticket.ok_or(Status::InvalidArgument)?;
        let result = ticket.take_completion().ok_or(Status::ReloadPending)?;
        let text = reply(result).to_string();
        let length = text.len().try_into().map_err(|_| Status::InternalFailure)?;
        // SAFETY: the host's sink copies this borrowed UTF-8 before returning.
        unsafe { sink(context, text.as_ptr(), length) };
        Ok(())
    })
}
pub(crate) unsafe extern "C" fn destroy(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: the caller transfers its sole ticket after source/poll calls stop.
        drop(unsafe { Box::from_raw(handle.cast::<RemoteTicket>()) });
    }
}

fn signal(value: RemoteSignal) -> Value {
    json!({"source":value.source as u32,"control_source":value.control_source.map(|source| source as u32),
        "enabled":value.enabled,"ctcss_tenths_hz":value.tone_tenths_hz,"dcs_bits":value.dcs_code,"inverted":value.inverted})
}
fn state(value: &RemoteOperatingState) -> Value {
    json!({"rx_hz":value.receive_hz,"tx_hz":value.transmit_hz,"mode":value.mode,
        "bandwidth_hz":value.bandwidth_hz,"rx_vfo":value.receive_vfo,"tx_vfo":value.transmit_vfo,
        "split":value.split,"offset_hz":value.offset_hz,"rit_hz":value.rit_hz,"xit_hz":value.xit_hz,
        "rit_enabled":value.rit_enabled,"xit_enabled":value.xit_enabled,
        "power_mw":value.power_mw,"channel":value.channel,"encode":signal(value.encode),"decode":signal(value.decode)})
}
fn setting(value: &Setting) -> Value {
    json!({"selector":value.selector,"name":value.name,"readable":value.readable,"writable":value.writable,
        "value_kind":value.value_kind,"units":value.units,"decimal_places":value.decimal_places,
        "minimum":value.minimum,"maximum":value.maximum,"choices":value.choices})
}

/// Expose only physically observed fields; defaults in the raw snapshot are
/// never presented as radio readings. The mask remains owned by this bridge.
fn observations(value: &Value, mask: u64) -> Value {
    let mut result = serde_json::Map::new();
    for (name, bit) in [
        ("rx_hz", 0),
        ("tx_hz", 1),
        ("mode", 2),
        ("bandwidth_hz", 3),
        ("rx_vfo", 4),
        ("tx_vfo", 5),
        ("offset_hz", 6),
        ("rit_hz", 7),
        ("xit_hz", 8),
        ("power_mw", 9),
        ("encode", 10),
        ("decode", 11),
        ("channel", 12),
        ("split", 13),
        ("rit_enabled", 14),
        ("xit_enabled", 15),
    ] {
        if mask & (1 << bit) != 0 {
            result.insert(name.into(), value[name].clone());
        }
    }
    if result.is_empty() {
        Value::Null
    } else {
        Value::Object(result)
    }
}
fn scan(value: Option<&raw::RptAdvRemoteScanSnapshot>) -> Value {
    value.map_or(Value::Null, |value| json!({"flags":value.flags,"list":value.list,"memory":value.current_memory,
        "running":value.flags & 1 != 0,"paused":value.flags & 2 != 0,
        "deadline_ms":value.next_deadline_ms,"dwell_ms":value.dwell_ms,"resume_delay_ms":value.resume_delay_ms,"skipped":value.skipped}))
}
fn error(value: RemoteRequestError) -> Value {
    let (reason, code) = match value {
        RemoteRequestError::Busy => ("busy", None),
        RemoteRequestError::Stopped => ("stopped", None),
        RemoteRequestError::StaleSource => ("stale_source", None),
        RemoteRequestError::Rejected(code) => ("rejected", Some(code)),
    };
    json!({"kind":"error","reason":reason,"code":code})
}
fn reply(result: Result<RemoteReply, RemoteRequestError>) -> Value {
    let mut value = match result {
        Err(value) => error(value),
        Ok(RemoteReply::Applied) => json!({"kind":"applied"}),
        Ok(RemoteReply::Value {
            setting: definition,
            value,
        }) => json!({"kind":"value","setting":setting(&definition),"value":value}),
        Ok(RemoteReply::Capabilities(values)) => {
            json!({"kind":"capabilities","entries":values.iter().map(setting).collect::<Vec<_>>()})
        }
        Ok(RemoteReply::Memories(values)) => {
            json!({"kind":"memories","entries":values.iter().map(|value|
            json!({"selector":value.selector,"name":value.name,"state":state(&value.state)})).collect::<Vec<_>>()})
        }
        Ok(RemoteReply::Scans(values)) => json!({"kind":"scans","entries":values.iter().map(|value|
            json!({"selector":value.selector,"name":value.name,"members":value.members,"dwell_ms":value.dwell_ms,
                "resume_delay_ms":value.resume_delay_ms,"priority":value.priority})).collect::<Vec<_>>()}),
        Ok(RemoteReply::Scan {
            definition,
            state: current,
        }) => json!({"kind":"scan","selector":definition.selector,
            "name":definition.name,"members":definition.members,"state":scan(current.as_ref())}),
        Ok(RemoteReply::Status {
            snapshot,
            scan: current,
        }) => {
            let convert = |value| operating_configuration(value).map(|value| state(&value));
            match (
                convert(&snapshot.requested),
                convert(&snapshot.applied),
                convert(&snapshot.observed),
            ) {
                (Ok(requested), Ok(applied), Ok(observed)) => {
                    json!({"kind":"status","flags":snapshot.flags,
                    "tx_inhibited":snapshot.flags & raw::RPTADV_REMOTE_INHIBITED != 0,
                    "receive_permitted":snapshot.flags & raw::RPTADV_REMOTE_RECEIVE_PERMITTED != 0,
                    "serviced_ptt":if snapshot.flags & raw::RPTADV_REMOTE_PTT_APPLIED != 0 { json!(snapshot.flags & raw::RPTADV_REMOTE_PTT_HIGH != 0) } else { Value::Null },
                    "observed_values":if snapshot.flags & raw::RPTADV_REMOTE_OBSERVED != 0 { observations(&observed, snapshot.observed_fields) } else { Value::Null },
                    "requested":requested,"applied":if snapshot.flags & 1 != 0 { applied } else { Value::Null },
                    "observed":if snapshot.flags & 2 != 0 { observed } else { Value::Null },
                    "observed_fields":snapshot.observed_fields,"failure_code":snapshot.failure_code,
                    "scan_rejected_bursts":snapshot.scan_rejected_bursts,"scan":scan(current.as_ref())})
                }
                _ => error(RemoteRequestError::Rejected(-1003)),
            }
        }
    };
    value["version"] = json!(1);
    value
}

#[cfg(test)]
mod tests {
    use crate::*;
    use usbradioplus_core::remote_operation::{RemoteOperation, RemoteOperationKind};
    use usbradioplus_driver::{RemoteQueue, RemoteReply};

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
}
