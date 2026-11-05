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
    if let Some(power) = settings.iter_mut().find(|setting| setting.selector == 11) {
        power.readable &= caps.readback_fields & 512 != 0;
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
#[path = "tests/remote_catalog_tests.rs"]
mod tests;
