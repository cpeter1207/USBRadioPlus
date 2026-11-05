//! Controller capabilities composed from the selected rig, DSP and GPIO owners.

use usbradioplus_remote::{SelectedCapabilities, Setting, raw};

/// Publish only callable controller operations. Signaling is a shared logical
/// control; raw rig split companions and unbounded PTT writes remain private.
pub(crate) fn controller_catalog(
    mut settings: Vec<Setting>,
    caps: &SelectedCapabilities,
) -> Vec<Setting> {
    settings.retain(|setting| !matches!(setting.selector, 12..=17 | 22 | 23));
    if let Some(ptt) = settings.iter_mut().find(|setting| setting.selector == 20) {
        ptt.writable = false;
    }
    for (selector, name, kind, units, places, minimum, maximum, mask) in [
        (12, "encode_ctcss", 4, 1, 1, 500, 3000, 0),
        (13, "decode_ctcss", 4, 1, 1, 500, 3000, 0),
        (14, "encode_dcs", 2, 3, 0, 0, 511, 0),
        (15, "decode_dcs", 2, 3, 0, 0, 511, 0),
        (16, "encode_enabled", 1, 0, 0, 0, 1, 0),
        (17, "decode_enabled", 1, 0, 0, 0, 1, 0),
        (400, "encode_source", 5, 0, 0, 0, 3, caps.encode_sources),
        (401, "decode_source", 5, 0, 0, 0, 3, caps.decode_sources),
        (
            402,
            "decode_control_source",
            5,
            0,
            0,
            0,
            3,
            caps.decode_control_sources,
        ),
        (403, "encode_dcs_inverted", 1, 0, 0, 0, 1, 0),
        (404, "decode_dcs_inverted", 1, 0, 0, 0, 1, 0),
    ] {
        settings.push(Setting {
            selector,
            name: name.into(),
            readable: true,
            writable: true,
            value_kind: kind,
            units,
            decimal_places: places,
            minimum,
            maximum,
            choices: if kind == 5 {
                (0..=3)
                    .filter(|source| *source == 0 || mask & (1 << source) != 0)
                    .map(i64::from)
                    .collect()
            } else {
                vec![]
            },
        });
    }
    settings.sort_by_key(|setting| setting.selector);
    settings
}

/// Read acknowledged logical signaling without pretending its value is a rig
/// observation. Native-programmed tone/code getters still use the actual rig.
pub(crate) fn signal_value(state: &raw::RptAdvRemoteState, selector: u32) -> Option<i64> {
    match selector {
        400 => return Some(i64::from(state.encode.source)),
        401 => return Some(i64::from(state.decode.source)),
        402 => return Some(i64::from(state.decode.control_source)),
        403 => return Some(i64::from(state.encode.inverted)),
        404 => return Some(i64::from(state.decode.inverted)),
        12..=17 => (),
        _ => return None,
    }
    let signal = if matches!(selector, 12 | 14 | 16) {
        state.encode
    } else {
        state.decode
    };
    let control = if signal.control_source == 0 {
        signal.source
    } else {
        signal.control_source
    };
    if control == 3 {
        return None;
    }
    Some(i64::from(match selector {
        12 | 13 => signal.ctcss_tenths_hz,
        14 | 15 => signal.dcs_code,
        _ => signal.enabled,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use usbradioplus_remote::{SelectedCapabilities, Setting, raw};

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
        let settings =
            controller_catalog(vec![native(12), native(20), native(22), native(23)], &caps);
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
}
