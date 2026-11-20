//! Public DTO errors retain their field token for controller-side fallback.
use super::*;
use serde_json::{Value, json};

fn rejected(dto: &Value, field: &str) {
    let failure = prepare(&standalone_radio(), dto.to_string().as_bytes(), |_| {}).err();
    assert_eq!(
        failure,
        Some((Status::InvalidConfiguration.code(), field.to_owned())),
        "{dto}"
    );
}

#[test]
fn station_metadata_bounds_and_role_restrictions_report_the_owned_field() {
    for (group, member, value, field) in [
        (
            "",
            "voter_reporting",
            json!(2147483648_u32),
            "voter_reporting",
        ),
        (
            "",
            "gpio_usb_port_path",
            json!("a\0b"),
            "gpio_usb_port_path",
        ),
        ("parallel_port", "device", json!("a\0b"), "parallel_port"),
        ("parallel_port", "base_address", json!(0), "parallel_port"),
        (
            "receive",
            "squelch_delay_ms",
            json!(512),
            "squelch_delay_ms",
        ),
        (
            "receive",
            "frequency_hz",
            json!(2147483648_u32),
            "frequency_hz",
        ),
        (
            "transmit",
            "frequency_hz",
            json!(2147483648_u32),
            "frequency_hz",
        ),
        (
            "duplex",
            "local_repeat_level",
            json!(1000),
            "local_repeat_level",
        ),
        (
            "diagnostics",
            "status_publication_interval_ms",
            json!(0),
            "status_publication_interval_ms",
        ),
        (
            "ctcss",
            "receive_frequencies",
            json!([]),
            "ctcss_frequencies",
        ),
        (
            "ctcss",
            "transmit_frequencies",
            json!(vec![1000; 39]),
            "ctcss_frequencies",
        ),
        ("ctcss", "receive_relax", json!(2), "receive_relax"),
        (
            "ctcss",
            "transmit_peak_dbfs",
            json!(1),
            "transmit_peak_dbfs",
        ),
        (
            "ctcss",
            "phase_shift_degrees",
            json!(0),
            "phase_shift_degrees",
        ),
        ("ctcss", "tail_duration_ms", json!(39), "tail_duration_ms"),
        ("ctcss", "tail_frequency_hz", json!(0), "tail_frequency_hz"),
        (
            "dcs",
            "receive_code",
            json!({"value":512,"polarity":"normal"}),
            "dcs_code",
        ),
        (
            "dcs",
            "transmit_code",
            json!({"value":512,"polarity":"inverted"}),
            "dcs_code",
        ),
        (
            "dcs",
            "turnoff_duration_ms",
            json!(149),
            "turnoff_duration_ms",
        ),
        ("dcs", "peak_dbfs", json!(-91), "peak_dbfs"),
    ] {
        let station = if group.is_empty() {
            json!({member: value})
        } else {
            json!({group:{member:value}})
        };
        rejected(&json!({"station":station}), field);
    }
    for (role, field) in [
        (json!({"rnnoise_enabled":true}), "role"),
        (json!({"stage_order":[]}), "stage_order"),
        (json!({"stage_order":vec!["agc";7]}), "stage_order"),
        (
            json!({"stage_order":["limiter"],"agc":{"enabled":true}}),
            "stage_order",
        ),
        (json!({"peak_limiter":{"oversample":3}}), "oversample"),
    ] {
        rejected(&json!({"link":role}), field);
    }
    let mut profile: Value = serde_json::from_str(include_str!("complete.json")).unwrap();
    profile["link"]["expander"]["sidechain_highpass_hz"] = json!(1600);
    rejected(&profile, "expander_sidechain_lowpass_hz");
}

#[test]
fn malformed_record_and_collection_shapes_fail_before_semantic_admission() {
    for dto in [
        json!({"link":"network"}),
        json!({"local":{"stage_order":[{}]}}),
        json!({"link":{"stage_order":[[]]}}),
        json!({"remote":{"memories":[false]}}),
        json!({"remote":{"receive_ranges":[[100]]}}),
        json!({"remote":{"receive_ranges":[[100,"two"]]}}),
        json!({"remote":{"hamlib_options":["secret"]}}),
        json!({"remote":{"hamlib_options":{"key":9}}}),
        json!({"remote":{"external_encode_output":{"parallel":1}}}),
    ] {
        rejected(&dto, "settings");
    }
}

#[test]
fn remote_transport_rf_memories_and_scan_limits_report_static_fields() {
    let base: Value = serde_json::from_str(include_str!("remote.json")).unwrap();
    for (path, value, field) in [
        ("/hamlib_model", json!("DUMMY"), "hamlib_model"),
        ("/hamlib_model", json!("RIG_MODEL_"), "hamlib_model"),
        ("/hamlib_model", json!("RIG_MODEL_BAD!"), "hamlib_model"),
        ("/hamlib_options", json!({"":"v"}), "hamlib_options"),
        ("/hamlib_options", json!({"a\0":"v"}), "hamlib_options"),
        ("/hamlib_options", json!({"k":"a\0b"}), "hamlib_options"),
        ("/hamlib_options", json!({"timeout":"3"}), "hamlib_options"),
        ("/timeout_ms", json!(10001), "timeout_ms"),
        ("/retries", json!(11), "retries"),
        ("/timeout_retries", json!(101), "timeout_retries"),
        ("/poll_interval_ms", json!(0), "poll_interval_ms"),
        ("/tune_duration_ms", json!(5001), "tune_duration_ms"),
        ("/dtmf_prefix", json!(""), "dtmf_prefix"),
        ("/dtmf_prefix", json!("5x"), "dtmf_prefix"),
        ("/ptt_source", json!("missing"), "ptt_source"),
        ("/cor_source", json!("missing"), "cor_source"),
        ("/scan_presets_ms/0", json!(0), "scan_presets_ms"),
        ("/receive_ranges", json!([]), "ranges"),
        ("/receive_ranges/0", json!([0, 144]), "ranges"),
        ("/transmit_ranges/0", json!([145, 144]), "ranges"),
        ("/modes", json!([]), "modes"),
        ("/modes", json!([13]), "modes"),
        ("/memories/0/selector", json!(0), "memories"),
        ("/memories/0/name", json!(""), "memories"),
        ("/memories/0/name", json!("a\0b"), "memories"),
        ("/memories/0/state/receive_hz", json!(0), "memories"),
        ("/memories/0/state/mode", json!(13), "memories"),
        ("/memories/0/state/receive_vfo", json!(6), "memories"),
        ("/memories/0/state/transmit_vfo", json!(6), "memories"),
        ("/memories/0/state/encode/dcs_code", json!(512), "signal"),
        (
            "/memories/0/state/decode/tone_tenths_hz",
            json!(499),
            "signal",
        ),
        ("/memories/0/state/decode/dcs_code", json!(1), "signal"),
        ("/scan_lists/0/selector", json!(0), "scan_lists"),
        ("/scan_lists/0/name", json!(""), "scan_lists"),
        ("/scan_lists/0/name", json!("a\0b"), "scan_lists"),
        ("/scan_lists/0/members", json!([]), "scan_lists"),
        ("/scan_lists/0/dwell_ms", json!(0), "scan_lists"),
        ("/scan_lists/0/settle_ms", json!(u64::MAX), "scan_lists"),
        ("/scan_lists/0/priority", json!(6), "scan_lists"),
    ] {
        let mut dto = base.clone();
        *dto["remote"].pointer_mut(path).unwrap() = value;
        rejected(&dto, field);
    }
    let mut dto = base;
    dto["remote"]["memories"][0]["state"]["channel"] = json!(16);
    rejected(&dto, "memories");
}

#[test]
fn missing_remote_engine_is_a_provider_error_without_a_field_or_candidate() {
    // The runtime search path is process-local: sibling tests retain their released provider.
    const CHILD: &str = "URP_TEST_MISSING_REMOTE_ENGINE";
    if std::env::var_os(CHILD).is_some() {
        let failure = prepare(&standalone_radio(), include_bytes!("remote.json"), |_| {})
            .err()
            .unwrap();
        assert_eq!(failure, (Status::IncompatibleAbi.code(), String::new()));
        return;
    }
    let directory = std::env::temp_dir().join(format!("urp-missing-remote-{}", std::process::id()));
    std::fs::create_dir(&directory).unwrap();
    std::fs::write(directory.join("librptadvremote.so.1"), []).unwrap();
    let mut paths = vec![directory.clone()];
    if let Some(original) = std::env::var_os("LD_LIBRARY_PATH") {
        paths.extend(std::env::split_paths(&original));
    }
    const TEST_NAME: &str = concat!(
        "tests::native::typed_preparation_admission::settings_bounds::",
        "missing_remote_engine_is_a_provider_error_without_a_field_or_candidate"
    );
    let result = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", TEST_NAME])
        .env("LD_LIBRARY_PATH", std::env::join_paths(paths).unwrap())
        .env(CHILD, "1")
        .output();
    std::fs::remove_file(directory.join("librptadvremote.so.1")).unwrap();
    std::fs::remove_dir(directory).unwrap();
    let result = result.unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stdout)
    );
    assert!(String::from_utf8_lossy(&result.stdout).contains("1 passed"));
}

#[test]
fn alsa_remote_does_not_inherit_cm119_wiring_or_ignore_parallel_pin_claims() {
    let mut dto: Value = serde_json::from_str(include_str!("remote.json")).unwrap();
    dto["remote"]["audio_source"] = json!("alsa");
    dto["remote"]["memories"][0]["state"]["encode"]["tone_tenths_hz"] = json!(0);
    // Transport options are opaque strings, including keys that name other DTO groups.
    dto["remote"]["hamlib_options"]["link"] = json!("opaque");
    let candidate = prepare(&standalone_radio(), dto.to_string().as_bytes(), |_| {}).unwrap();
    // SAFETY: the successful typed owner retains its configuration.
    let request = unsafe { crate::native_configuration::prepared(candidate.0).unwrap() };
    assert!(request.hardware.cm119.is_none());
    dto["remote"]["backend"] = json!("parallel_channel");
    dto["remote"]["external_encode_output"] = json!({"parallel":2});
    rejected(&dto, "remote");
}

#[test]
fn remote_record_deserializer_explains_malformed_transport_records() {
    // The shared type is also consumed independently of the controller's named-record DTO.
    use usbradioplus_core::ResolvedRemoteConfiguration;
    assert!(serde_json::from_str::<ResolvedRemoteConfiguration>("[]").is_err());
    let mut dto: Value = serde_json::from_str(include_str!("remote.json")).unwrap();
    dto["remote"]["hamlib_options"] = json!(false);
    let error =
        serde_json::from_value::<ResolvedRemoteConfiguration>(dto["remote"].clone()).unwrap_err();
    assert!(error.to_string().contains("named string option map"));
}
