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
            request.fields = 1 | 2 | 64;
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
pub(crate) mod tests {
    use super::*;
    use crate::DriverConfiguration;

    pub(crate) fn configuration() -> ResolvedRemoteConfiguration {
        let document = DriverConfiguration::parse("test", "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM,USB\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n").unwrap();
        let mut configuration = document.remote("524950").unwrap().clone();
        configuration.offset_magnitude_hz = Some(600_000);
        configuration.power_presets_mw = [Some(2500), None, Some(10000)];
        configuration
    }
    fn current() -> raw::RptAdvRemoteSnapshot {
        // SAFETY: the C snapshot and nested records contain only scalars.
        let mut snapshot: raw::RptAdvRemoteSnapshot = unsafe { std::mem::zeroed() };
        snapshot.flags = raw::RPTADV_REMOTE_APPLIED;
        snapshot.applied.receive_hz = 146_520_000;
        snapshot.applied.transmit_hz = 146_520_000;
        snapshot.applied.flags = 1;
        snapshot.applied.mode = 1;
        snapshot.applied.encode.source = 1;
        snapshot.applied.decode.source = 1;
        snapshot
    }
    fn core(operation: RemoteOperation) -> raw::RptAdvRemoteRequest {
        let PreparedRemoteOperation::Core(request) =
            prepare_remote_operation(&configuration(), &current(), None, &[], operation).unwrap()
        else {
            panic!("not a core operation")
        };
        request
    }
    #[test]
    fn remote_operands_become_one_complete_engine_patch_without_io() {
        let mut frequency = RemoteOperation::new(Kind::Frequency);
        frequency.magnitude = 146_940_000;
        frequency.flags = 1;
        let request = core(frequency);
        assert_eq!(request.kind, raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SET);
        assert_eq!(request.fields, 1 | 2 | 64);
        assert_eq!(request.state.receive_hz, 146_940_000);
        assert_eq!(request.state.transmit_hz, 146_940_000);
        assert_eq!(request.state.offset_hz, -600_000);
        let mut step = RemoteOperation::new(Kind::Step);
        step.signed_value = -5000;
        let request = core(step);
        assert_eq!(request.state.receive_hz, 146_515_000);
        assert_eq!(request.state.transmit_hz, 146_515_000);
        let mut mode = RemoteOperation::new(Kind::Mode);
        mode.selector = 3;
        let request = core(mode);
        assert_eq!(
            (
                request.fields,
                request.state.mode,
                request.state.bandwidth_hz
            ),
            (12, 3, 0)
        );
        let request = core(RemoteOperation::new(Kind::PowerPreset));
        assert_eq!(request.state.power_mw, 2500);
        assert_eq!(core(RemoteOperation::new(Kind::Tune)).value_ms, 1000);
    }

    #[test]
    fn remote_scalar_conversion_is_exact_typed_and_preserves_unavailable_presets() {
        let setting = Setting {
            selector: 100,
            name: "AF".into(),
            readable: true,
            writable: true,
            value_kind: 4,
            units: 6,
            decimal_places: 6,
            minimum: 0,
            maximum: 1_000_000,
            choices: vec![],
        };
        let mut operation = RemoteOperation::new(Kind::SetSetting);
        operation.selector = 100;
        operation.magnitude = 5;
        operation.decimal_places = 1;
        let result = prepare_remote_operation(
            &configuration(),
            &current(),
            None,
            std::slice::from_ref(&setting),
            operation,
        )
        .unwrap();
        assert!(matches!(
            result,
            PreparedRemoteOperation::Auxiliary {
                selector: 100,
                value: 500_000
            }
        ));
        operation.flags = 1;
        assert!(
            prepare_remote_operation(&configuration(), &current(), None, &[setting], operation)
                .is_err()
        );
        let mut missing = RemoteOperation::new(Kind::PowerPreset);
        missing.selector = 1;
        assert!(
            prepare_remote_operation(&configuration(), &current(), None, &[], missing).is_err()
        );
    }

    #[test]
    fn remote_scan_and_tone_commands_reuse_engine_requests() {
        let mut operation = RemoteOperation::new(Kind::Recall);
        operation.selector = 12;
        assert_eq!((core(operation).kind, core(operation).selector), (2, 12));
        for (kind, expected) in [
            (Kind::PauseScan, 5),
            (Kind::ResumeScan, 6),
            (Kind::StopScan, 7),
            (Kind::Skip, 9),
            (Kind::Dwell, 10),
            (Kind::ResumeDelay, 11),
            (Kind::Priority, 12),
        ] {
            let mut operation = RemoteOperation::new(kind);
            operation.magnitude = 250;
            operation.selector = 2;
            let request = core(operation);
            assert_eq!(
                (request.kind, request.selector, request.value_ms),
                (expected, 2, 250)
            );
        }
        let mut operation = RemoteOperation::new(Kind::Tone);
        operation.magnitude = 1148;
        operation.flags = 1;
        let request = core(operation);
        assert_eq!(
            (request.fields, request.state.encode.ctcss_tenths_hz),
            (1024, 1148)
        );
        assert_eq!(request.state.encode.dcs_code, 0);
        assert_eq!(request.state.encode.source, 1);
        let mut operation = RemoteOperation::new(Kind::ToneEnabled);
        operation.flags = 2;
        let request = core(operation);
        assert_eq!((request.fields, request.state.decode.enabled), (2048, 1));
        let mut operation = RemoteOperation::new(Kind::StartScan);
        operation.selector = 7;
        assert_eq!((core(operation).kind, core(operation).direction), (4, 1));
        operation = RemoteOperation::new(Kind::Inhibit);
        operation.flags = 1;
        assert_eq!((core(operation).kind, core(operation).selector), (3, 1));
    }

    #[test]
    fn remote_common_scalar_writes_remain_under_complete_state_admission() {
        let setting = Setting {
            selector: 9,
            name: "RIT".into(),
            readable: true,
            writable: true,
            value_kind: 3,
            units: 1,
            decimal_places: 0,
            minimum: -10000,
            maximum: 10000,
            choices: vec![],
        };
        let mut operation = RemoteOperation::new(Kind::SetSetting);
        operation.selector = 9;
        operation.magnitude = 250;
        operation.flags = 1;
        let PreparedRemoteOperation::Core(request) =
            prepare_remote_operation(&configuration(), &current(), None, &[setting], operation)
                .unwrap()
        else {
            panic!("RF-affecting settings must not bypass engine admission")
        };
        assert_eq!((request.fields, request.state.rit_hz), (128, -250));
    }

    #[test]
    fn remote_scan_shortcut_requires_an_already_selected_list() {
        let mut config = configuration();
        config.scan_lists.push(usbradioplus_core::RemoteScanList {
            selector: 7,
            name: "local".into(),
            members: vec![1],
            dwell_ms: 500,
            resume_delay_ms: 2000,
            settle_ms: 0,
            priority: None,
        });
        config.scan_presets_ms[0] = Some(500);
        let request = RemoteOperation::new(Kind::ScanPreset);
        assert!(prepare_remote_operation(&config, &current(), None, &[], request).is_err());
        // SAFETY: this scan snapshot contains only scalar fields.
        let mut scan: raw::RptAdvRemoteScanSnapshot = unsafe { std::mem::zeroed() };
        scan.list = 7;
        let PreparedRemoteOperation::Core(request) =
            prepare_remote_operation(&config, &current(), Some(&scan), &[], request).unwrap()
        else {
            panic!("not core")
        };
        assert_eq!((request.selector, request.value_ms), (7, 500));
    }

    #[test]
    fn remote_split_scalar_changes_only_split_routing() {
        let setting = Setting {
            selector: 7,
            name: "split".into(),
            readable: true,
            writable: true,
            value_kind: 1,
            units: 0,
            decimal_places: 0,
            minimum: 0,
            maximum: 1,
            choices: vec![],
        };
        let mut operation = RemoteOperation::new(Kind::SetSetting);
        operation.selector = 7;
        operation.magnitude = 1;
        let PreparedRemoteOperation::Core(request) =
            prepare_remote_operation(&configuration(), &current(), None, &[setting], operation)
                .unwrap()
        else {
            panic!("not core")
        };
        assert_eq!(request.fields, 1 << 13);
        assert_eq!(request.state.flags & 8, 8);
        assert_eq!(request.state.transmit_hz, request.state.receive_hz);
    }
}
