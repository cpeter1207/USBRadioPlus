use super::*;
use crate::DriverConfiguration;

pub(crate) fn configuration() -> ResolvedRemoteConfiguration {
    let document = DriverConfiguration::parse("test", "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM,USB\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n").unwrap();
    let mut configuration = document.remote("524950").unwrap().clone();
    configuration
        .hamlib_options
        .insert("ptt_type".into(), "RIG".into());
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

#[test]
fn legacy_parallel_frames_enter_one_complete_shared_request() {
    use usbradioplus_core::radio_control::{CtcssTone, ParallelRadioRequest, RemoteRadio};
    let mut config = configuration();
    config.memories[0].state.channel = Some(7);
    let recall =
        prepare_parallel_request(&config, &current(), ParallelRadioRequest::SelectChannel(7))
            .unwrap();
    assert_eq!(
        (recall.kind, recall.selector),
        (raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_RECALL, 1)
    );
    assert!(
        prepare_parallel_request(&config, &current(), ParallelRadioRequest::SelectChannel(16))
            .is_err()
    );
    assert!(
        prepare_parallel_request(&config, &current(), ParallelRadioRequest::SelectChannel(8))
            .is_err()
    );
    let input = RemoteRadio {
        receive_hz: 146940000,
        transmit_hz: 146340000,
        receive_ctcss: CtcssTone::from_tenths_hz(1148),
        transmit_ctcss: CtcssTone::from_tenths_hz(1230),
        high_power: true,
    };
    let request =
        prepare_parallel_request(&config, &current(), ParallelRadioRequest::Configure(input))
            .unwrap();
    assert_eq!(request.fields, 1 | 2 | 4 | 64 | 1024 | 2048);
    assert_eq!(
        (
            request.state.receive_hz,
            request.state.transmit_hz,
            request.state.offset_hz
        ),
        (146940000, 146340000, 0)
    );
    assert_eq!(
        (
            request.state.encode.ctcss_tenths_hz,
            request.state.decode.ctcss_tenths_hz
        ),
        (1230, 1148)
    );
    assert_eq!(
        (request.state.encode.enabled, request.state.decode.enabled),
        (1, 1)
    );
    let input = RemoteRadio {
        transmit_hz: 0,
        receive_ctcss: None,
        transmit_ctcss: None,
        ..input
    };
    let mut snapshot = current();
    snapshot.flags = 0;
    snapshot.requested = snapshot.applied;
    let request =
        prepare_parallel_request(&config, &snapshot, ParallelRadioRequest::Configure(input))
            .unwrap();
    assert_eq!(
        (
            request.state.flags & 1,
            request.state.encode.enabled,
            request.state.decode.enabled
        ),
        (0, 0, 0)
    );
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
fn remote_signal_scalars_preserve_octal_codes_and_use_whole_signal_transactions() {
    let caps = usbradioplus_remote::SelectedCapabilities {
        encode_sources: 2 | 4 | 8,
        decode_sources: 2 | 4,
        decode_control_sources: 2 | 4 | 8,
        ..Default::default()
    };
    let settings = crate::remote_catalog::controller_catalog(vec![], &caps);
    for (selector, magnitude) in [
        (14, 245),
        (15, 777),
        (400, 2),
        (401, 2),
        (402, 3),
        (403, 1),
        (404, 1),
    ] {
        let mut operation = RemoteOperation::new(Kind::SetSetting);
        operation.selector = selector;
        operation.magnitude = magnitude;
        let PreparedRemoteOperation::Core(request) =
            prepare_remote_operation(&configuration(), &current(), None, &settings, operation)
                .unwrap()
        else {
            panic!("signal controls cannot bypass the engine")
        };
        let encode = matches!(selector, 14 | 400 | 403);
        assert_eq!(request.fields, if encode { 1024 } else { 2048 });
        match selector {
            14 => assert_eq!(request.state.encode.dcs_code, 0o245),
            15 => assert_eq!(request.state.decode.dcs_code, 0o777),
            400 => assert_eq!(request.state.encode.source, 2),
            401 => assert_eq!(request.state.decode.source, 2),
            402 => assert_eq!(request.state.decode.control_source, 3),
            403 => assert_eq!(request.state.encode.inverted, 1),
            404 => assert_eq!(request.state.decode.inverted, 1),
            _ => unreachable!(),
        }
    }
    let mut operation = RemoteOperation::new(Kind::SetSetting);
    operation.selector = 14;
    operation.magnitude = 888;
    assert!(
        prepare_remote_operation(&configuration(), &current(), None, &settings, operation).is_err()
    );
    operation.selector = 401;
    operation.magnitude = 3;
    assert!(
        prepare_remote_operation(&configuration(), &current(), None, &settings, operation).is_err()
    );
    let mut external = current();
    external.applied.encode.source = 2;
    operation.selector = 12;
    operation.magnitude = 1148;
    operation.decimal_places = 1;
    assert!(
        prepare_remote_operation(&configuration(), &external, None, &settings, operation).is_err(),
        "a GPIO enable cannot program an external tone generator"
    );
}
#[test]
fn remote_operands_become_one_complete_engine_patch_without_io() {
    let mut frequency = RemoteOperation::new(Kind::Frequency);
    frequency.magnitude = 146_940_000;
    frequency.flags = 1;
    let request = core(frequency);
    assert_eq!(request.kind, raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SET);
    assert_eq!(request.fields, 1 | 2 | 64 | (1 << 13));
    assert_eq!(request.state.flags & 8, 0);
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
    assert!(prepare_remote_operation(&configuration(), &current(), None, &[], missing).is_err());
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
fn remote_incremental_enables_cannot_bypass_rf_admission() {
    for (selector, field, flag) in [(307, 14, 16), (308, 15, 32)] {
        let setting = Setting {
            selector,
            name: "incremental enabled".into(),
            readable: true,
            writable: true,
            value_kind: 1,
            units: 0,
            decimal_places: 0,
            minimum: 0,
            maximum: 1,
            choices: vec![],
        };
        for enabled in [0, 1] {
            let mut operation = RemoteOperation::new(Kind::SetSetting);
            operation.selector = selector;
            operation.magnitude = enabled;
            let PreparedRemoteOperation::Core(request) = prepare_remote_operation(
                &configuration(),
                &current(),
                None,
                std::slice::from_ref(&setting),
                operation,
            )
            .unwrap() else {
                panic!("incremental enable must use complete-state validation")
            };
            assert_eq!(request.fields, 1 << field);
            assert_eq!(
                request.state.flags & flag,
                if enabled == 1 { flag } else { 0 }
            );
        }
    }
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
