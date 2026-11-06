use super::*;

fn resolve(text: &str) -> ResolvedNativeConfiguration {
    ResolvedNativeConfiguration::from_document(&ConfigDocument::new(text), "native.conf", "1000")
        .unwrap()
}

#[test]
fn native_defaults_preserve_voice_tone_preemphasis_and_eeprom_policy() {
    let resolved = resolve("[1000]\n");
    let station = &resolved.config().station;
    assert_eq!(
        station.hardware.output_a_assignment,
        HardwareOutputAssignment::Voice
    );
    assert_eq!(
        station.hardware.output_b_assignment,
        HardwareOutputAssignment::Ctcss
    );
    assert!(!station.transmit.preemphasis_enabled);
    assert!(!station.hardware.eeprom_enabled);
    assert_eq!(resolved.config().local.input_gain_db, 0.0);
    assert_eq!(resolved.config().local.output_gain_db, 0.0);
    assert_eq!(
        StationConfig::default().hardware.output_a_assignment,
        HardwareOutputAssignment::VoiceCtcss
    );
    assert!(StationConfig::default().transmit.preemphasis_enabled);
    assert!(StationConfig::default().hardware.eeprom_enabled);
}

#[test]
fn native_invalid_flat_values_cannot_enable_eeprom_or_asl_preemphasis() {
    let resolved = resolve(
        "[1000]\n[radio]\nhardware_eeprom_enabled=wrong\ntransmit_preemphasis_enabled=wrong\ntransmit_output_a_assignment=wrong\ntransmit_output_b_assignment=wrong\nreceive_output_gain_db=wrong\n",
    );
    let station = &resolved.config().station;
    assert!(!station.hardware.eeprom_enabled);
    assert!(!station.transmit.preemphasis_enabled);
    assert_eq!(
        station.hardware.output_a_assignment,
        HardwareOutputAssignment::Voice
    );
    assert_eq!(
        station.hardware.output_b_assignment,
        HardwareOutputAssignment::Ctcss
    );
    assert_eq!(resolved.config().local.output_gain_db, 0.0);
    assert_eq!(resolved.warnings().len(), 5);
}

#[test]
fn native_raw_output_gain_requires_integer_aliases_but_shared_profiles_keep_fractions() {
    for (text, inherited, section) in [
        (
            "[1000]\n[radio]\nreceive_output_gain_db=-6.2\n",
            0.0,
            "radio",
        ),
        (
            "[1000]\n[radio]\nreceive_output_gain_db=1.0\n",
            0.0,
            "radio",
        ),
        (
            "[1000]\n[radio]\nreceive_output_gain_db=-3\n[radio 1000]\nreceive_output_gain_db=-6.2\n",
            -3.0,
            "radio 1000",
        ),
    ] {
        let resolved = resolve(text);
        assert_eq!(resolved.config().local.output_gain_db, inherited, "{text}");
        assert_eq!(resolved.warnings().len(), 1);
        let warning = &resolved.warnings()[0];
        assert_eq!(warning.section, section);
        assert_eq!(warning.name, "receive_output_gain_db");
        assert_eq!(warning.fallback, inherited.to_string());
    }
    let resolved = resolve(
        "[1000]\nlocal_profile=precise\n[radio]\nreceive_output_gain_db=-6.2\n[local precise]\noutput_gain_db=-4.7\n",
    );
    assert_eq!(resolved.config().local.output_gain_db, -4.7);
    assert!(resolved.warnings().is_empty());
}

#[test]
fn native_normalized_menu_gain_keeps_raw_graph_and_original_profile_absence() {
    let resolved = resolve("[1000]\n[radio]\nreceive_graph=volume=0.5\n");
    let menu = ResolvedProfile::from_document(
        resolved.normalized_document(),
        "native.conf",
        "1000",
        ChainRole::LocalReceive,
    )
    .unwrap()
    .into_chain();
    assert_eq!(menu.output_gain_db, 0.0);
    assert_eq!(resolved.physical().receive_graph, "volume=0.5");
    assert!(!native_processing_profile_configured(
        resolved.source_document(),
        "1000",
        "local"
    ));
}

#[test]
fn native_radio_aliases_preserve_asymmetric_ctcss() {
    let resolved = resolve(
        "[1000]\n[radio]\nreceive_signaling=ctcss\ntransmit_signaling=ctcss\nreceive_ctcss_tones_hz=100.0,103.5\ntransmit_ctcss_tones_hz=123.0,127.3\nreceive_cpu_saver_enabled=yes\ntransmit_cpu_saver_enabled=no\nreceive_input_gain_db=-2\n[radio 1000]\nsquelch_level=650\n",
    );
    let config = resolved.config();
    assert_eq!(
        config.station.receive.signaling_method,
        SignalingMethod::Ctcss
    );
    assert_eq!(
        config.station.ctcss.receive_frequencies[0].tenths_hz(),
        1000
    );
    assert_eq!(
        config.station.ctcss.transmit_frequencies[0].tenths_hz(),
        1230
    );
    assert_eq!(resolved.warnings().len(), 2);
    assert!(
        resolved
            .warnings()
            .iter()
            .all(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
    );
    assert_eq!(config.station.receive.squelch_level, 650);
    assert_eq!(config.local.input_gain_db, -2.0);
}

#[test]
fn retired_native_cpu_savers_warn_at_both_radio_overlays() {
    for value in ["yes", "no", "invalid"] {
        let resolved = resolve(&format!(
            "[1000]\n[radio]\nreceive_cpu_saver_enabled={value}\ntransmit_cpu_saver_enabled={value}\n[radio 1000]\nreceive_cpu_saver_enabled={value}\ntransmit_cpu_saver_enabled={value}\n"
        ));
        assert_eq!(resolved.warnings().len(), 4);
        assert!(
            resolved
                .warnings()
                .iter()
                .all(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
        );
    }
}

#[test]
fn native_profiles_reuse_named_selection_and_missing_profile_fallbacks() {
    let resolved = resolve(
        "[1000]\nlocal_profile=receiver\nlink_profile=network\nvoice_telemetry_profile=missing\n[local receiver]\ninput_gain_db=3\n[link network]\ninput_gain_db=-4\n[voice_telemetry]\ninput_gain_db=-5\n",
    );
    assert_eq!(resolved.config().local.input_gain_db, 3.0);
    assert_eq!(resolved.config().link.input_gain_db, -4.0);
    assert_eq!(resolved.config().voice_telemetry.input_gain_db, -5.0);
    assert!(
        resolved
            .warnings()
            .iter()
            .any(|warning| warning.supplied_value == "missing")
    );
}

#[test]
fn native_invalid_radio_values_keep_inherited_shared_values_and_warn() {
    let resolved = resolve(
        "[1000]\n[radio]\nsquelch_level=650\nreceive_audio=speaker\n[radio 1000]\nsquelch_level=1000\nreceive_audio=wrong\n",
    );
    assert_eq!(resolved.config().station.receive.squelch_level, 650);
    assert_eq!(
        resolved.config().station.receive.audio_source,
        ReceiveAudioSource::Speaker
    );
    assert_eq!(
        resolved
            .warnings()
            .iter()
            .filter(|warning| warning.kind == ResolutionWarningKind::InvalidValue)
            .count(),
        2
    );
    assert!(
        resolved
            .warnings()
            .iter()
            .all(|warning| warning.section == "radio 1000")
    );
    assert!(
        resolved
            .warnings()
            .iter()
            .any(|warning| warning.name == "receive_audio" && warning.fallback == "speaker")
    );
}

#[test]
fn native_physical_options_preserve_raw_graphs_gain_intentions_and_inheritance() {
    let text = "[1000]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\ninput_device_channels=2\noutput_device_channels=2\nhardware_input_gain_db=3.5\nhardware_output_a_gain_db=-6\nreceive_graph=highpass=f=60,lowpass=f=5500\ntransmit_graph=volume=-2dB\nreceive_output_gain_db=-4\n[radio 1000]\nhardware_input_gain_db=nan\noutput_device_channels=3\n";
    let resolved = resolve(text);
    let physical = resolved.physical();
    assert_eq!(
        physical.device_selection,
        NativeDeviceSelection::AutomaticLowestAlsaCard
    );
    assert_eq!(physical.input_device_channels, 2);
    assert_eq!(physical.output_device_channels, 2);
    assert_eq!(physical.hardware_input_gain_db, Some(3.5));
    assert_eq!(physical.hardware_output_a_gain_db, Some(-6.0));
    assert_eq!(physical.hardware_output_b_gain_db, None);
    assert_eq!(physical.receive_graph, "highpass=f=60,lowpass=f=5500");
    assert_eq!(physical.transmit_graph, "volume=-2dB");
    assert_eq!(resolved.config().local.output_gain_db, -4.0);
    assert_eq!(resolved.config().station.hardware.input_gain_db, 3.5);
    let defaults = resolve("[1000]\n");
    assert_eq!(defaults.physical(), &NativePhysicalOptions::default());
    assert_eq!(resolved.source_document().as_str(), text);
    assert!(!native_processing_profile_configured(
        resolved.source_document(),
        "1000",
        "local"
    ));
}

#[test]
fn native_tuner_aliases_round_trip_units_enums_and_real_destinations() {
    for (section, key, native, canonical, original) in [
        ("receive", "audio_source", "receive_audio", "no", "disabled"),
        (
            "receive",
            "noise_filter_type",
            "noise_filter",
            "1",
            "alternate",
        ),
        (
            "duplex",
            "duplex_radio_mode",
            "radio_duplex_mode",
            "1",
            "full",
        ),
        ("ctcss", "receive_relax", "ctcss_relaxed", "0", "no"),
        (
            "hardware",
            "hardware_output_a_assignment",
            "transmit_output_a_assignment",
            "voice_ctcss",
            "composite",
        ),
        (
            "receive",
            "cos_assignment",
            "carrier_source",
            "usbinvert",
            "cm119_inverted",
        ),
        (
            "ctcss",
            "turnoff_mode",
            "transmit_ctcss_turnoff_mode",
            "ctcss_phase_shift",
            "phase_shift",
        ),
        (
            "receive",
            "on_delay_frames",
            "receive_on_delay_ms",
            "3",
            "60",
        ),
    ] {
        assert_eq!(native_radio_setting_destination(section, key), Some(native));
        assert_eq!(
            native_radio_setting_value(section, key, canonical).as_deref(),
            Some(original)
        );
        let resolved = resolve(&format!("[1000]\n[radio]\n{native}={original}\n"));
        assert_eq!(
            resolved
                .normalized_document()
                .explicit_values(section)
                .get(key)
                .map(String::as_str),
            Some(canonical)
        );
    }
    assert_eq!(
        native_radio_setting_destination("local", "compressor_enabled"),
        None
    );
    assert_eq!(
        native_radio_setting_value("local", "compressor_enabled", "yes"),
        None
    );
    assert_eq!(
        native_configured_nodes(&ConfigDocument::new(
            "[general]\n[radio]\n[1000]\n[2000]\n[main]\n[echolink]\n[local 1000]\n"
        )),
        ["1000", "2000", "main"]
    );
}

#[test]
fn native_invalid_boundary_options_keep_safe_inherited_values_and_source_metadata() {
    let mut resolved = resolve(&format!(
        "[1000]\nenabled=no\nchannel_enabled=no\n[radio]\ndevice_selection=automatic_lowest_alsa_card\nsquelch_level=500\nSQUELCH_LEVEL=500\n[radio 1000]\ndevice_selection=wrong\nreceive_graph=\ntransmit_graph={}\nfuture_option=yes\n",
        "x".repeat(4097)
    ));
    assert!(!resolved.config().station.channel_enabled);
    assert_eq!(
        resolved.physical().device_selection.to_string(),
        "automatic_lowest_alsa_card"
    );
    assert_eq!(resolved.physical().receive_graph, "anull");
    assert_eq!(resolved.physical().transmit_graph, "anull");
    assert!(
        resolved
            .warnings()
            .iter()
            .any(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
    );
    assert_eq!(
        resolved
            .warnings()
            .iter()
            .filter(|warning| warning.kind == ResolutionWarningKind::InvalidValue)
            .count(),
        3
    );
    resolved.config_mut().station.hardware.input_gain_db = 6.0;
    assert_eq!(resolved.into_config().station.hardware.input_gain_db, 6.0);
    let bad_radio = resolve("[1000]\n[radio]\nreceive_audio=bad\nreceive_on_delay_ms=bad\n");
    assert!(
        bad_radio
            .shared_warnings()
            .iter()
            .any(|warning| warning.name == "audio_source")
    );
    assert_eq!(
        native_radio_setting_value("receive", "on_delay_frames", "bad").as_deref(),
        Some("bad")
    );
    assert!(
        ResolvedNativeConfiguration::from_document(
            &ConfigDocument::new("[1000]\n"),
            "native.conf",
            "missing"
        )
        .is_err()
    );
}

#[test]
fn native_selected_common_profiles_own_effective_gain_intentions() {
    let resolved = resolve(
        "[1000]\nhardware_profile=site\n[radio 1000]\nhardware_input_gain_db=12\nreceive_input_gain_db=6\n[hardware site]\nhardware_input_gain_db=-3\n[local]\ninput_gain_db=2\n",
    );
    assert_eq!(resolved.config().station.hardware.input_gain_db, -3.0);
    assert_eq!(resolved.physical().hardware_input_gain_db, Some(-3.0));
    assert_eq!(resolved.config().local.input_gain_db, 2.0);
}

#[test]
fn native_aliases_keep_shared_conflicting_assignment_errors() {
    for text in [
        "[1000]\n[radio]\nsquelch_level=500\nSQUELCH_LEVEL=600\n",
        "[1000]\nlocal_profile=first\nLOCAL_PROFILE=second\n[local first]\n[local second]\n",
    ] {
        assert!(
            ResolvedNativeConfiguration::from_document(
                &ConfigDocument::new(text),
                "native.conf",
                "1000"
            )
            .is_err()
        );
    }
}
