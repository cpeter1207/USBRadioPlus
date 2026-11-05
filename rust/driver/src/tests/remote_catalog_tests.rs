use super::*;
use usbradioplus_remote::{SelectedCapabilities, Setting, raw};

#[test]
fn catalog_power_readback_requires_observed_transmit_context() {
    let power = Setting {
        selector: 11,
        name: "power".into(),
        readable: true,
        writable: true,
        value_kind: 2,
        units: 2,
        decimal_places: 0,
        minimum: 0,
        maximum: 10000,
        choices: vec![],
    };
    for (fields, readable) in [(0, false), (512, true)] {
        let caps = SelectedCapabilities {
            readback_fields: fields,
            ..Default::default()
        };
        let catalog = controller_catalog(vec![power.clone()], &caps);
        let actual = catalog
            .iter()
            .find(|setting| setting.selector == 11)
            .unwrap();
        assert_eq!(actual.readable, readable);
        assert!(actual.writable);
    }
}

#[test]
fn remote_catalog_exposes_dsp_signaling_without_private_or_unbounded_ptt_writes() {
    let native = |selector| Setting {
        selector,
        name: "native".into(),
        readable: true,
        writable: true,
        value_kind: 2,
        units: 0,
        decimal_places: 0,
        minimum: 0,
        maximum: 999,
        choices: vec![],
    };
    let caps = SelectedCapabilities {
        encode_sources: 2 | 8,
        decode_sources: 2 | 4,
        decode_control_sources: 2 | 4 | 8,
        ..Default::default()
    };
    let settings = controller_catalog(vec![native(12), native(20), native(22), native(23)], &caps);
    assert!(
        !settings
            .iter()
            .any(|entry| matches!(entry.selector, 22 | 23))
    );
    assert!(
        !settings
            .iter()
            .find(|entry| entry.selector == 20)
            .unwrap()
            .writable
    );
    for selector in [12, 13, 14, 15, 16, 17, 400, 401, 402, 403, 404] {
        assert_eq!(
            settings
                .iter()
                .filter(|entry| entry.selector == selector)
                .count(),
            1
        );
    }
    assert_eq!(
        settings
            .iter()
            .find(|entry| entry.selector == 400)
            .unwrap()
            .choices,
        vec![0, 1, 3]
    );
    assert_eq!(
        settings
            .iter()
            .find(|entry| entry.selector == 402)
            .unwrap()
            .choices,
        vec![0, 1, 2, 3]
    );
}

#[test]
fn remote_signal_queries_use_acknowledged_dsp_values_and_keep_native_reads_native() {
    // SAFETY: this fixed operating record contains only scalar fields.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.encode.source = 1;
    state.encode.enabled = 1;
    state.encode.dcs_code = 0o245;
    state.encode.inverted = 1;
    state.decode.source = 2;
    state.decode.control_source = 3;
    state.decode.ctcss_tenths_hz = 1148;
    for (selector, expected) in [
        (14, Some(0o245)),
        (16, Some(1)),
        (400, Some(1)),
        (403, Some(1)),
        (401, Some(2)),
        (402, Some(3)),
        (13, None),
        (20, None),
    ] {
        assert_eq!(
            signal_value(&state, selector),
            expected,
            "selector {selector}"
        );
    }
    state.decode.control_source = 0;
    assert_eq!(signal_value(&state, 13), Some(1148));
}
