use super::*;

#[test]
fn remote_external_encoder_pin_has_one_typed_bank_and_safe_fallback() {
    for (value, present, warning) in [
        ("usb:1", true, false),
        ("usb:8", true, false),
        ("parallel:2", true, false),
        ("parallel:9", true, false),
        ("", false, false),
        ("usb:0", false, true),
        ("parallel:1", false, true),
        ("other:1", false, true),
    ] {
        let source = CONFIG.replace(
            "backend=hamlib",
            &format!(
                "backend=hamlib\nexternal_encode_output={value}\nexternal_encode_inverted=yes"
            ),
        );
        let config = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(source),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert_eq!(config.external_encode_output.is_some(), present, "{value}");
        assert!(config.external_encode_inverted);
        assert_eq!(!config.warnings.is_empty(), warning, "{value}");
    }
}

const CONFIG: &str = "[524950]\n[remote_base]\nenabled=no\ndtmf_access=locked\npoll_interval_ms=600\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_IC9700\nhamlib_device=/dev/serial/by-id/rig\nhamlib_option_serial_speed=115200\nstartup_memory=local\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\nallowed_modes=FM,USB\nmax_power_w=10\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\npower_w=5\nctcss_encode_source=dsp\nctcss_encode_hz=114.8\nctcss_decode_source=external\n[remote_scan 524950 local_channels]\nselector=1\nmembers=local\ndwell_ms=500\nresume_delay_ms=2000\n";

#[test]
fn remote_memory_incremental_offsets_have_independent_enable_settings() {
    for (extra, enabled) in [
        ("rit_hz=125\nxit_hz=-250\n", true),
        (
            "rit_hz=125\nxit_hz=-250\nrit_enabled=no\nxit_enabled=no\n",
            false,
        ),
    ] {
        let source = CONFIG.replace("power_w=5\n", &format!("power_w=5\n{extra}"));
        let config = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(source),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        let state = &config.memories[0].state;
        assert_eq!((state.rit_hz, state.xit_hz), (125, -250));
        assert_eq!((state.rit_enabled, state.xit_enabled), (enabled, enabled));
    }
}

#[test]
fn remote_command_prefix_uses_the_existing_63_digit_collector_bound() {
    let source = CONFIG.replace(
        "backend=hamlib",
        &format!("backend=hamlib\ndtmf_prefix={}", "6".repeat(64)),
    );
    let configuration = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(source),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(configuration.dtmf_prefix, "6");
    assert!(
        configuration
            .warnings
            .iter()
            .any(|warning| warning.name == "dtmf_prefix")
    );
}

#[test]
fn remote_memory_split_is_explicit_with_frequency_based_default() {
    for (source, expected) in [
        (CONFIG.to_owned(), false),
        (
            CONFIG.replace("power_w=5\n", "power_w=5\nsplit=yes\n"),
            true,
        ),
        (
            CONFIG.replace("tx_frequency_hz=146520000", "tx_frequency_hz=146940000"),
            true,
        ),
        (
            CONFIG.replace(
                "tx_frequency_hz=146520000",
                "tx_frequency_hz=146940000\nsplit=no",
            ),
            false,
        ),
    ] {
        let config = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(source),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert_eq!(config.memories[0].state.split, expected);
    }
}

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

#[test]
fn remote_configuration_uses_the_adr_alsa_audio_category() {
    let text = CONFIG.replace("enabled=yes", "enabled=yes\naudio_source=alsa");
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(format!("{:?}", config.audio_source), "Alsa");
    assert!(config.warnings.is_empty());
}

#[test]
fn remote_configuration_separates_decoder_programming_from_qualification() {
    let text = CONFIG.replace(
        "ctcss_decode_source=external",
        "ctcss_decode_source=external\nctcss_decode_hz=114.8\ndecode_control_source=hamlib",
    );
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    let decode = config.memories[0].state.decode;
    assert_eq!(decode.source, RemoteSignalSource::External);
    assert_eq!(decode.control_source, Some(RemoteSignalSource::Rig));
    assert_eq!(decode.tone_tenths_hz, 1148);
    assert!(config.warnings.is_empty());
}

#[test]
fn remote_configuration_rejects_invalid_identity_limits_and_definitions() {
    let resolve = |source: String| {
        ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(source),
            "test",
            "524950",
            &StationConfig::default(),
        )
    };
    for (old, new) in [
        ("backend=hamlib", "backend=unknown"),
        ("RIG_MODEL_IC9700", "RIG_MODEL_"),
        ("RIG_MODEL_IC9700", "RIG_MODEL_BAD!"),
        ("hamlib_option_serial_speed=115200", "hamlib_option_=value"),
        (
            "hamlib_option_serial_speed=115200",
            "hamlib_option_timeout=1",
        ),
        ("allowed_modes=FM,USB", "allowed_modes=unsupported"),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=",
        ),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=144000000",
        ),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=bad-148000000",
        ),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=144000000-bad",
        ),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=0-148000000",
        ),
        (
            "allowed_rx_ranges_hz=144000000-148000000",
            "allowed_rx_ranges_hz=148000000-144000000",
        ),
        ("selector=1\nrx_frequency_hz", "selector=0\nrx_frequency_hz"),
        ("rx_frequency_hz=146520000", "rx_frequency_hz=0"),
        ("tx_frequency_hz=146520000", "tx_frequency_hz=bad"),
        ("power_w=5", "power_w=4294968"),
        ("power_w=5", "power_w=.5"),
        ("power_w=5", "power_w=bad"),
        ("power_w=5", "power_w=1.2345"),
        ("power_w=5", "power_w=5\nchannel=16"),
        ("power_w=5", "power_w=5\nchannel=256"),
        (
            "ctcss_encode_hz=114.8",
            "ctcss_encode_hz=114.8\ndcs_encode_code=023N",
        ),
        (
            "ctcss_decode_source=external",
            "ctcss_decode_source=external\ndcs_decode_source=dsp",
        ),
        (
            "ctcss_decode_source=external",
            "ctcss_decode_source=external\ndecode_control_source=bad",
        ),
        ("[remote_memory 524950 local]", "[remote_memory 524950]"),
        (
            "[remote_scan 524950 local_channels]",
            "[remote_scan 524950]",
        ),
        ("selector=1\nmembers", "selector=0\nmembers"),
        ("members=local", "members=local\npriority=missing"),
        (
            "dwell_ms=500",
            "dwell_ms=500\nsettle_ms=18446744073709551615",
        ),
        ("backend=hamlib", "backend=hamlib\nptt_source=bad"),
        ("backend=hamlib", "backend=hamlib\ncor_source=bad"),
        (
            "backend=hamlib",
            "backend=hamlib\npoll_interval_ms=100\nPOLL_INTERVAL_MS=200",
        ),
    ] {
        assert!(resolve(CONFIG.replace(old, new)).is_err(), "{new}");
    }
    let extra_memory =
        "[remote_memory 524950 other]\nselector=1\nrx_frequency_hz=146540000\nmode=FM\n";
    assert!(resolve(format!("{CONFIG}{extra_memory}")).is_err());
    let extra_scan = "[remote_scan 524950 other]\nselector=1\nmembers=local\n";
    assert!(resolve(format!("{CONFIG}{extra_scan}")).is_err());
    let other = extra_memory.replace("selector=1", "selector=2");
    assert!(
        resolve(format!(
            "{}{other}",
            CONFIG.replace("members=local", "members=local\npriority=other")
        ))
        .is_err()
    );
}

#[test]
fn remote_configuration_normalized_optional_values_keep_defaults_and_warnings() {
    for extra in [
        "audio_source=unknown",
        "dtmf_access=bad",
        "dtmf_prefix=",
        "dtmf_prefix=6A",
        "timeout_ms=0",
        "timeout_ms=10001",
        "retries=11",
        "timeout_retries=101",
        "poll_interval_ms=0",
        "tune_duration_ms=0",
        "tune_duration_ms=5001",
        "power_low_w=bad",
        "power_medium_w=1.2222",
        "power_medium_w=1.a",
        "power_high_w=-1",
        "scan_slow_ms=0",
        "scan_medium_ms=bad",
        "scan_fast_ms=0",
        "ctcss_encode_hz=49.9",
        "ctcss_decode_hz=300.1",
        "ctcss_encode_source=bad",
        "dcs_decode_code=invalid",
        "dcs_encode_source=bad",
        "external_encode_inverted=invalid",
    ] {
        let text = CONFIG.replace("backend=hamlib", &format!("backend=hamlib\n{extra}"));
        let result = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(text),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert!(!result.warnings.is_empty(), "{extra}");
    }
}

#[test]
fn remote_configuration_optional_transport_and_automatic_tones_remain_explicit() {
    let source = CONFIG
        .replace("hamlib_device=/dev/serial/by-id/rig\n", "")
        .replace("ctcss_encode_source=dsp\n", "");
    let resolve = |text: &str| {
        ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(text),
            "test",
            "524950",
            &StationConfig::default(),
        )
    };
    let config = resolve(&source).unwrap().unwrap();
    assert!(!config.hamlib_options.contains_key("rig_pathname"));
    assert_eq!(
        config.memories[0].state.encode.source,
        RemoteSignalSource::Automatic
    );
    let source = CONFIG.replace(
        "ctcss_encode_source=dsp\nctcss_encode_hz=114.8",
        "dcs_encode_source=external\ndcs_encode_code=023N",
    );
    let config = resolve(&source).unwrap().unwrap();
    assert_eq!(
        config.memories[0].state.encode.source,
        RemoteSignalSource::External
    );
    assert_eq!(config.memories[0].state.encode.dcs_code, 0o23);
    let error =
        resolve(&CONFIG.replace("allowed_rx_ranges_hz=144000000-148000000\n", "")).unwrap_err();
    let text = error.to_string();
    assert!(text.contains("allowed_rx_ranges_hz"));
    assert!(text.contains("remote_base 524950"));
}

#[test]
fn remote_configuration_inherits_station_signals_and_valid_vfo_scan_settings() {
    let basic = CONFIG.replace(
        "ctcss_encode_source=dsp\nctcss_encode_hz=114.8\nctcss_decode_source=external\n",
        "",
    );
    for method in [
        SignalingMethod::Carrier,
        SignalingMethod::Ctcss,
        SignalingMethod::Dcs,
    ] {
        for source in [CtcssSource::Disabled, CtcssSource::Dsp, CtcssSource::Usb] {
            let mut station = StationConfig::default();
            station.receive.signaling_method = method;
            station.transmit.signaling_method = method;
            station.ctcss.receive_source = source;
            station.dcs.receive_code = "023I".parse().unwrap();
            station.dcs.transmit_code = "245N".parse().unwrap();
            let result = ResolvedRemoteConfiguration::from_document(
                &ConfigDocument::new(&basic),
                "test",
                "524950",
                &station,
            )
            .unwrap()
            .unwrap();
            if method == SignalingMethod::Dcs {
                assert_eq!(result.memories[0].state.decode.dcs_code, 0o23);
                assert!(result.memories[0].state.decode.inverted);
            }
            assert_eq!(
                result.memories[0].state.decode.enabled,
                method != SignalingMethod::Carrier && source != CtcssSource::Disabled
            );
        }
    }
    for (vfo, expected) in [("A", 1), ("B", 2), ("main", 3), ("sub", 4), ("memory", 5)] {
        let text = CONFIG.replace(
            "power_w=5",
            &format!("power_w=5.125\nreceive_vfo={vfo}\ntransmit_vfo={vfo}"),
        );
        let result = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(text),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert_eq!(
            (
                result.memories[0].state.receive_vfo,
                result.memories[0].state.transmit_vfo
            ),
            (expected, expected)
        );
        assert_eq!(result.memories[0].state.power_mw, Some(5125));
    }
    let text=basic.replace("ctcss_encode_hz=114.8","").replace("backend=hamlib","backend=parallel_rtx\naudio_source=alsa\ndtmf_access=disabled\nptt_source=parallel\ncor_source=open\npower_low_w=1\npower_medium_w=2.5\npower_high_w=5\nscan_slow_ms=1000\nscan_medium_ms=500\nscan_fast_ms=100").replace("power_w=5","power_w=5\ntx_frequency_hz=none").replace("members=local","members=local\npriority=local");
    let result = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(text),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(result.backend, RemoteBackend::ParallelRtx);
    assert_eq!(result.dtmf_access, RemoteDtmfAccess::Disabled);
    assert_eq!(
        result.power_presets_mw,
        [Some(1000), Some(2500), Some(5000)]
    );
    assert_eq!(result.scan_presets_ms, [Some(1000), Some(500), Some(100)]);
    assert_eq!(result.scan_lists[0].priority, Some(1));
}

#[test]
fn remote_configuration_accepts_automatic_dcs_and_ignores_other_nodes() {
    let mut source = CONFIG.replace(
        "ctcss_encode_source=dsp\nctcss_encode_hz=114.8\nctcss_decode_source=external\n",
        "dcs_encode_code=245I\ndcs_decode_code=023N\n",
    );
    source.push_str("[remote_memory 999999 ignored]\nselector=1\nrx_frequency_hz=1\n[remote_scan 999999 ignored]\nselector=1\nmembers=missing\n");
    let config = ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(source),
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    assert_eq!(config.memories.len(), 1);
    assert_eq!(config.scan_lists.len(), 1);
    assert_eq!(
        config.memories[0].state.encode.source,
        RemoteSignalSource::Automatic
    );
    assert!(config.memories[0].state.encode.inverted);
    assert_eq!(config.memories[0].state.decode.dcs_code, 0o23);
    let mut station = StationConfig::default();
    station.ctcss.receive_frequencies.clear();
    station.ctcss.receive_source = CtcssSource::Disabled;
    station.receive.signaling_method = SignalingMethod::Carrier;
    let basic = CONFIG.replace(
        "ctcss_encode_source=dsp\nctcss_encode_hz=114.8\nctcss_decode_source=external\n",
        "",
    );
    assert!(
        ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(basic),
            "test",
            "524950",
            &station
        )
        .is_ok()
    );
    for source in ["disabled", "dsp", "external", "hamlib", "auto"] {
        let text = CONFIG.replace(
            "ctcss_encode_source=dsp",
            &format!("ctcss_encode_source={source}"),
        );
        let result = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(text),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert_eq!(
            result.memories[0].state.encode.enabled,
            source != "disabled"
        );
    }
}

#[test]
fn remote_memory_normalization_retains_valid_default_operating_fields() {
    for extra in [
        "mode=bad",
        "receive_vfo=bad",
        "transmit_vfo=bad",
        "bandwidth_hz=bad",
        "offset_hz=bad",
        "rit_hz=bad",
        "xit_hz=bad",
    ] {
        let text = CONFIG.replace("power_w=5", &format!("power_w=5\n{extra}"));
        let result = ResolvedRemoteConfiguration::from_document(
            &ConfigDocument::new(text),
            "test",
            "524950",
            &StationConfig::default(),
        )
        .unwrap()
        .unwrap();
        assert!(!result.warnings.is_empty(), "{extra}");
        assert_eq!(result.memories[0].state.mode, 1);
    }
}
