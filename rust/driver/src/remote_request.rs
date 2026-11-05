//! Translate interface operands into the shared engine's typed requests.

use crate::RemoteRequestError;
use std::mem::size_of;
use usbradioplus_core::{
    ResolvedRemoteConfiguration,
    remote_operation::{RemoteOperation, RemoteOperationKind as Kind},
};
use usbradioplus_remote::{Setting, raw};

/// Prepared data only; the existing physical owner performs every operation.
pub enum PreparedRemoteOperation {
    /// One validated whole-state/scan request for the dynamic policy engine.
    Core(raw::RptAdvRemoteRequest),
    /// A declared native scalar outside the common RF operating-state fields.
    Auxiliary {
        /// Stable setting selector.
        selector: u32,
        /// Exact integer in the declared setting units.
        value: i64,
    },
}

fn invalid() -> RemoteRequestError {
    RemoteRequestError::Rejected(-1003)
}
fn unsupported() -> RemoteRequestError {
    RemoteRequestError::Rejected(-6)
}

/// Adapt one complete legacy frame into the same atomic RF transaction. The
/// legacy RTX backend has no power control; its H/L field remains non-operational.
pub(crate) fn prepare_parallel_request(
    config: &ResolvedRemoteConfiguration,
    snapshot: &raw::RptAdvRemoteSnapshot,
    input: usbradioplus_core::radio_control::ParallelRadioRequest,
) -> Result<raw::RptAdvRemoteRequest, RemoteRequestError> {
    use usbradioplus_core::radio_control::ParallelRadioRequest;
    // SAFETY: this complete request and its state contain only scalar fields.
    let mut request: raw::RptAdvRemoteRequest = unsafe { std::mem::zeroed() };
    request.struct_size = size_of::<raw::RptAdvRemoteRequest>() as u32;
    request.abi_version = 1;
    match input {
        ParallelRadioRequest::SelectChannel(channel) => {
            if channel > 15 {
                return Err(invalid());
            }
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_RECALL;
            request.selector = config
                .memories
                .iter()
                .find(|memory| memory.state.channel == Some(channel))
                .ok_or_else(unsupported)?
                .selector;
        }
        ParallelRadioRequest::Configure(input) => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SET;
            request.state = if snapshot.flags & raw::RPTADV_REMOTE_APPLIED != 0 {
                snapshot.applied
            } else {
                snapshot.requested
            };
            request.state.receive_hz = u64::from(input.receive_hz);
            request.state.transmit_hz = u64::from(input.transmit_hz);
            request.state.flags = (request.state.flags & !1) | u32::from(input.transmit_hz != 0);
            request.state.offset_hz = 0;
            request.state.mode = 1;
            request.fields = 1 | 2 | 4 | 64 | 1024 | 2048;
            for (signal, tone) in [
                (&mut request.state.encode, input.transmit_ctcss),
                (&mut request.state.decode, input.receive_ctcss),
            ] {
                signal.enabled = u32::from(tone.is_some());
                if let Some(tone) = tone {
                    signal.ctcss_tenths_hz = u32::from(tone.tenths_hz());
                    signal.dcs_code = 0;
                    signal.inverted = 0;
                }
            }
        }
    }
    Ok(request)
}

fn scalar(setting: &Setting, operation: RemoteOperation) -> Result<i64, RemoteRequestError> {
    if !setting.writable || !(1..=5).contains(&setting.value_kind) {
        return Err(unsupported());
    }
    if operation.flags & !1 != 0
        || operation.decimal_places > 6
        || setting.decimal_places > 6
        || operation.flags & 1 != 0 && matches!(setting.value_kind, 1 | 2 | 5)
    {
        return Err(invalid());
    }
    let mut value = i128::from(operation.magnitude);
    if operation.flags & 1 != 0 {
        value = -value;
    }
    if setting.decimal_places >= operation.decimal_places {
        value *= 10_i128.pow(setting.decimal_places - operation.decimal_places);
    } else {
        let divisor = 10_i128.pow(operation.decimal_places - setting.decimal_places);
        if value % divisor != 0 {
            return Err(invalid());
        }
        value /= divisor;
    }
    let value: i64 = value.try_into().map_err(|_| invalid())?;
    let value = if setting.units == 3 {
        // Keypads enter the printed octal digits. Every shared ABI continues to
        // carry the actual nine bits, matching the Hamlib adapter contract.
        if operation.decimal_places != 0 {
            return Err(invalid());
        }
        i64::from(
            format!("{value:03}N")
                .parse::<usbradioplus_core::DcsCode>()
                .map_err(|_| invalid())?
                .value(),
        )
    } else {
        value
    };
    if value < setting.minimum
        || value > setting.maximum
        || !setting.choices.is_empty() && !setting.choices.contains(&value)
        || setting.value_kind == 1 && !matches!(value, 0 | 1)
    {
        return Err(invalid());
    }
    Ok(value)
}

/// Normalize interface values without I/O, graph creation or RF-policy copies.
/// The shared engine still validates complete state against hardware/RF limits.
pub fn prepare_remote_operation(
    config: &ResolvedRemoteConfiguration,
    current: &raw::RptAdvRemoteSnapshot,
    scan: Option<&raw::RptAdvRemoteScanSnapshot>,
    settings: &[Setting],
    operation: RemoteOperation,
) -> Result<PreparedRemoteOperation, RemoteRequestError> {
    let kind = operation.kind().ok_or_else(invalid)?;
    // SAFETY: the fixed request and nested operating state contain only scalars.
    let mut request: raw::RptAdvRemoteRequest = unsafe { std::mem::zeroed() };
    request.struct_size = size_of::<raw::RptAdvRemoteRequest>() as u32;
    request.abi_version = 1;
    request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SET;
    request.state = current.applied;
    request.selector = operation.selector;
    request.value_ms = operation.magnitude;
    match kind {
        Kind::Recall => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_RECALL,
        Kind::Frequency => {
            let magnitude = || {
                config
                    .offset_magnitude_hz
                    .ok_or_else(unsupported)
                    .and_then(|value| i64::try_from(value).map_err(|_| invalid()))
            };
            request.state.offset_hz = match operation.flags {
                1 => -magnitude()?,
                2 => 0,
                3 => magnitude()?,
                _ => return Err(invalid()),
            };
            request.state.receive_hz = operation.magnitude;
            if request.state.flags & 1 != 0 {
                request.state.transmit_hz = operation.magnitude;
            }
            // Familiar repeater-offset tuning returns to common/simplex VFO
            // routing rather than retaining a previously selected split.
            request.state.flags &= !8;
            request.fields = 1 | 2 | 64 | (1 << 13);
        }
        Kind::Mode => {
            request.state.mode = operation.selector;
            request.state.bandwidth_hz = 0;
            request.fields = 12;
        }
        Kind::Step => {
            let shifted = |value: u64| {
                u64::try_from(i128::from(value) + i128::from(operation.signed_value))
                    .map_err(|_| invalid())
            };
            request.state.receive_hz = shifted(request.state.receive_hz)?;
            request.fields = 1;
            if request.state.flags & 1 != 0 {
                request.state.transmit_hz = shifted(request.state.transmit_hz)?;
                request.fields |= 2;
            }
        }
        Kind::PowerPreset => {
            request.state.power_mw = config
                .power_presets_mw
                .get(operation.selector as usize)
                .copied()
                .flatten()
                .ok_or_else(unsupported)?;
            request.state.flags |= 2;
            request.fields = 1 << 9;
        }
        Kind::Tone | Kind::ToneEnabled => {
            if operation.flags & !3 != 0 {
                return Err(invalid());
            }
            let (signal, bit) = if operation.flags & 1 != 0 {
                (&mut request.state.encode, 10)
            } else {
                (&mut request.state.decode, 11)
            };
            if kind == Kind::Tone {
                if signal.control_source == 2 || signal.control_source == 0 && signal.source == 2 {
                    return Err(unsupported());
                }
                if !(500..=3000).contains(&operation.magnitude) {
                    return Err(invalid());
                }
                signal.ctcss_tenths_hz = operation.magnitude as u32;
                signal.dcs_code = 0;
                signal.inverted = 0;
            } else {
                signal.enabled = u32::from(operation.flags & 2 != 0);
            }
            request.fields = 1 << bit;
        }
        Kind::StartScan | Kind::ScanPreset => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_START;
            request.direction = if kind == Kind::StartScan {
                1
            } else {
                operation.flags & 1
            };
            if kind == Kind::ScanPreset {
                request.selector = scan.map(|scan| scan.list).ok_or_else(unsupported)?;
                request.value_ms = config
                    .scan_presets_ms
                    .get(operation.selector as usize)
                    .copied()
                    .flatten()
                    .ok_or_else(unsupported)?;
            }
        }
        Kind::PauseScan => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_PAUSE,
        Kind::ResumeScan => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_RESUME,
        Kind::StopScan => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_STOP,
        Kind::ScanDirection => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_DIRECTION;
            if operation.flags > 1 {
                return Err(invalid());
            }
            request.direction = operation.flags;
        }
        Kind::Skip => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_SKIP,
        Kind::Dwell => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_DWELL,
        Kind::ResumeDelay => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_RESUME_DELAY
        }
        Kind::Priority => request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_PRIORITY,
        Kind::Inhibit => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_INHIBIT;
            if operation.flags > 1 {
                return Err(invalid());
            }
            request.selector = operation.flags;
        }
        Kind::Tune => {
            request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_TUNE;
            request.value_ms = config.tune_duration_ms;
        }
        Kind::SetSetting => {
            let setting = settings
                .iter()
                .find(|setting| setting.selector == operation.selector)
                .ok_or_else(unsupported)?;
            let value = scalar(setting, operation)?;
            let positive = || u64::try_from(value).map_err(|_| invalid());
            match operation.selector {
                1 => {
                    request.fields = 1;
                    request.state.receive_hz = positive()?;
                }
                2 => {
                    request.fields = 2;
                    request.state.transmit_hz = positive()?;
                    request.state.flags |= 1;
                }
                3 => {
                    request.fields = 4;
                    request.state.mode = positive()?.try_into().map_err(|_| invalid())?;
                }
                4 => {
                    request.fields = 8;
                    request.state.bandwidth_hz = positive()?.try_into().map_err(|_| invalid())?;
                }
                5 => {
                    request.fields = 16;
                    request.state.receive_vfo = positive()?.try_into().map_err(|_| invalid())?;
                }
                6 => {
                    request.fields = 32;
                    request.state.transmit_vfo = positive()?.try_into().map_err(|_| invalid())?;
                }
                7 => {
                    request.fields = 1 << 13;
                    request.state.flags = (request.state.flags & !8) | (u32::from(value != 0) << 3);
                }
                20 => return Err(unsupported()),
                8 => {
                    request.fields = 64;
                    request.state.offset_hz = value;
                }
                9 => {
                    request.fields = 128;
                    request.state.rit_hz = value;
                }
                10 => {
                    request.fields = 256;
                    request.state.xit_hz = value;
                }
                307 | 308 => {
                    let (bit, flag) = if operation.selector == 307 {
                        (14, 16)
                    } else {
                        (15, 32)
                    };
                    request.fields = 1 << bit;
                    request.state.flags =
                        (request.state.flags & !flag) | if value != 0 { flag } else { 0 };
                }
                // Internal split-mode companions never bypass complete-state
                // admission through the generic auxiliary-setting path.
                22 | 23 => return Err(unsupported()),
                11 => {
                    request.fields = 512;
                    request.state.power_mw = positive()?.try_into().map_err(|_| invalid())?;
                    request.state.flags |= 2;
                }
                12..=17 => {
                    let encode = matches!(operation.selector, 12 | 14 | 16);
                    let signal = if encode {
                        &mut request.state.encode
                    } else {
                        &mut request.state.decode
                    };
                    let programming = if signal.control_source == 0 {
                        signal.source
                    } else {
                        signal.control_source
                    };
                    if matches!(operation.selector, 12..=15) && programming == 2
                        || matches!(operation.selector, 14 | 15) && programming == 3
                    {
                        return Err(unsupported());
                    }
                    match operation.selector {
                        12 | 13 => {
                            signal.ctcss_tenths_hz =
                                positive()?.try_into().map_err(|_| invalid())?;
                            signal.dcs_code = 0;
                            signal.inverted = 0;
                        }
                        14 | 15 => {
                            signal.dcs_code = positive()?.try_into().map_err(|_| invalid())?;
                            signal.ctcss_tenths_hz = 0;
                        }
                        _ => signal.enabled = positive()?.try_into().map_err(|_| invalid())?,
                    }
                    request.fields = if encode { 1024 } else { 2048 };
                }
                400..=404 => {
                    let encode = matches!(operation.selector, 400 | 403);
                    let signal = if encode {
                        &mut request.state.encode
                    } else {
                        &mut request.state.decode
                    };
                    let value = positive()?.try_into().map_err(|_| invalid())?;
                    match operation.selector {
                        400 | 401 => signal.source = value,
                        402 => signal.control_source = value,
                        _ => signal.inverted = value,
                    }
                    request.fields = if encode { 1024 } else { 2048 };
                }
                _ => {
                    return Ok(PreparedRemoteOperation::Auxiliary {
                        selector: operation.selector,
                        value,
                    });
                }
            }
        }
        _ => return Err(unsupported()),
    }
    Ok(PreparedRemoteOperation::Core(request))
}

#[cfg(test)]
#[path = "tests/remote_request_tests.rs"]
pub(crate) mod tests;
