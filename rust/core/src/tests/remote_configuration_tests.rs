use super::*;

const CONFIG: &str = "[524950]\n[remote_base]\nenabled=no\ndtmf_access=locked\npoll_interval_ms=600\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_IC9700\nhamlib_device=/dev/serial/by-id/rig\nhamlib_option_serial_speed=115200\nstartup_memory=local\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\nallowed_modes=FM,USB\nmax_power_w=10\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\npower_w=5\nctcss_encode_source=dsp\nctcss_encode_hz=114.8\nctcss_decode_source=external\n[remote_scan 524950 local_channels]\nselector=1\nmembers=local\ndwell_ms=500\nresume_delay_ms=2000\n";

#[test]
fn remote_configuration_is_identical_across_hosts() {
    let document = ConfigDocument::new(CONFIG);
    let channel = ResolvedChannelConfiguration::from_document(&document, "test", "524950").unwrap();
    let native = ResolvedNativeConfiguration::from_document(&document, "test", "524950").unwrap();
    let asl = ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "524950",
        &channel.config().station,
    )
    .unwrap()
    .unwrap();
    let standalone = ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "524950",
        &native.config().station,
    )
    .unwrap()
    .unwrap();
    assert_eq!(asl, standalone);
    assert_eq!(asl.poll_interval_ms, 600);
    assert_eq!(asl.memories[0].state.encode.tone_tenths_hz, 1148);
    assert_eq!(
        asl.memories[0].state.decode.source,
        RemoteSignalSource::External
    );
    assert!(asl.memories[0].state.decode.enabled);
    assert_eq!(asl.memories[0].state.power_mw, Some(5000));
    assert_eq!(asl.scan_lists[0].members, vec![1]);
    assert_eq!(asl.startup_memory, 1);
    assert_eq!(document.configured_channels(), vec!["524950"]);
    assert!(document.unknown_scoped_sections().is_empty());
}

#[test]
fn remote_configuration_rejects_unsafe_or_unresolvable_definitions() {
    for (old, new) in [
        ("RIG_MODEL_IC9700", "3081"),
        ("members=local", "members=missing"),
        ("max_power_w=10", "max_power_w=-1"),
        (
            "rx_frequency_hz=146520000",
            "rx_frequency_hz=not-a-frequency",
        ),
    ] {
        let document = ConfigDocument::new(CONFIG.replace(old, new));
        assert!(
            ResolvedRemoteConfiguration::from_document(
                &document,
                "test",
                "524950",
                &StationConfig::default()
            )
            .is_err(),
            "{old}"
        );
    }
    assert!(
        ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new("[524950]\n"),
            "test",
            "524950",
            &StationConfig::default()
        )
        .unwrap()
        .is_none()
    );
}

#[test]
fn remote_memory_definitions_are_configuration_only_and_keep_independent_signals() {
    let document = ConfigDocument::new(CONFIG);
    let before = document.as_str().to_owned();
    let config = ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(
        config.memories[0].state.encode.source,
        RemoteSignalSource::Dsp
    );
    assert_eq!(
        config.memories[0].state.decode.source,
        RemoteSignalSource::External
    );
    assert_eq!(before, document.as_str());
    assert!(config.power_presets_mw.iter().all(Option::is_none));
}

#[test]
fn remote_configuration_normalizes_each_layer_and_reports_unknown_keys() {
    let text = CONFIG
        .replace(
            "poll_interval_ms=600",
            "poll_interval_ms=600\nunknown_global=ignored",
        )
        .replace("enabled=yes", "enabled=yes\nPOLL_INTERVAL_MS=200")
        .replace("power_w=5", "power_w=5\nunknown_memory=ignored")
        .replace("dwell_ms=500", "dwell_ms=500\nunknown_scan=ignored");
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(config.poll_interval_ms, 200);
    assert_eq!(config.warnings.len(), 3);
}

#[test]
fn remote_configuration_debug_never_exposes_transport_credentials() {
    let text = CONFIG.replace(
        "hamlib_device=/dev/serial/by-id/rig",
        "hamlib_device=redacted-secret-fixture",
    );
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert!(!format!("{config:?}").contains("redacted-secret-fixture"));
}

#[test]
fn remote_configuration_invalid_values_warn_and_keep_safe_inherited_defaults() {
    let text = CONFIG
        .replace("poll_interval_ms=600", "poll_interval_ms=0")
        .replace("dwell_ms=500", "dwell_ms=0")
        .replace("ctcss_encode_hz=114.8", "ctcss_encode_hz=114.88");
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(config.poll_interval_ms, 500);
    assert_eq!(config.scan_lists[0].dwell_ms, 500);
    assert_eq!(config.memories[0].state.encode.tone_tenths_hz, 1000);
    assert_eq!(config.warnings.len(), 3);
    let text = CONFIG.replace("enabled=yes", "enabled=yes\npoll_interval_ms=0");
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(config.poll_interval_ms, 600);
    assert_eq!(config.warnings.len(), 1);
}
