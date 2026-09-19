use super::*;

fn resolve(text: &str) -> Result<ResolvedStationConfig, StationConfigError> {
    ResolvedStationConfig::from_document(
        &ConfigDocument::new(text),
        "/etc/asterisk/usbradioplus.conf",
        "usb",
    )
}

#[test]
fn shipped_station_defaults_are_concrete_and_safe() {
    let resolved = resolve("[usb]\n").unwrap();
    let config = resolved.config();
    assert_eq!(resolved.channel(), "usb");
    assert!(resolved.warnings().is_empty());
    assert!(config.channel_enabled);
    assert_eq!(
        config.asterisk,
        AsteriskConfig {
            jitter_buffer_enabled: false,
            jitter_buffer_max_size_ms: 200,
            jitter_buffer_resync_threshold_ms: 1_000,
            jitter_buffer_implementation: JitterBufferImplementation::Fixed,
            jitter_buffer_logging_enabled: false,
            jitter_buffer_force_enabled: false,
            jitter_buffer_target_extra_ms: 40,
            jitter_buffer_video_sync_enabled: false,
        }
    );
    assert_eq!(config.hardware.input_gain_db, 0.0);
    assert_eq!(config.hardware.output_a_gain_db, 0.0);
    assert_eq!(config.hardware.output_b_gain_db, 0.0);
    assert_eq!(
        config.hardware.output_a_assignment,
        HardwareOutputAssignment::VoiceCtcss
    );
    assert_eq!(
        config.hardware.output_b_assignment,
        HardwareOutputAssignment::Off
    );
    assert!(!config.hardware.ptt_inverted);
    assert_eq!(config.hardware.deemphasis_corner_hz, 300.0);
    assert_eq!(config.hardware.preemphasis_corner_hz, 300.0);
    assert_eq!(config.hardware.device_identifier, "");
    assert_eq!(config.hardware.serial, "");
    assert_eq!(
        config.hardware.interface_type,
        HardwareInterfaceType::DudeUsb
    );
    assert!(config.hardware.eeprom_enabled);
    assert_eq!(config.hardware.gpio_usb_port_path, "");
    assert_eq!(config.hardware.voter_reporting, 0);
    assert_eq!(config.hardware.clip_led_gpio, None);
    assert_eq!(config.hardware.gpio_modes, [GpioMode::Input; 8]);
    assert_eq!(config.hardware.parallel_port, ParallelPortConfig::default());
    assert_eq!(config.receive, ReceiveConfig::default());
    assert_eq!(config.transmit, TransmitConfig::default());
    assert_eq!(config.ctcss, CtcssConfig::default());
    assert_eq!(config.dcs, DcsConfig::default());
    assert_eq!(config.duplex, DuplexConfig::default());
    assert_eq!(config.diagnostics, DiagnosticsConfig::default());
}

#[test]
fn every_current_non_processing_option_is_typed() {
    let resolved = resolve(
        "[usb]\n\
             channel_enabled = no\n\
             \n\
             [asterisk]\n\
             asterisk_jitter_buffer_enabled = yes\n\
             asterisk_jitter_buffer_max_size_ms = 300\n\
             asterisk_jitter_buffer_resync_threshold_ms = 1200\n\
             asterisk_jitter_buffer_implementation = adaptive\n\
             asterisk_jitter_buffer_logging_enabled = yes\n\
             asterisk_jitter_buffer_force_enabled = yes\n\
             asterisk_jitter_buffer_target_extra_ms = 50\n\
             asterisk_jitter_buffer_video_sync_enabled = yes\n\
             \n\
             [hardware]\n\
             hardware_input_gain_db = -3.5\n\
             hardware_output_a_gain_db = 2.5\n\
             hardware_output_b_gain_db = 1.5\n\
             hardware_output_a_assignment = voice\n\
             hardware_output_b_assignment = ctcss\n\
             hardware_ptt_inverted = yes\n\
             hardware_deemphasis_corner_hz = 250\n\
             hardware_preemphasis_corner_hz = 350\n\
             hardware_device_identifier = 3-1\n\
             hardware_serial = serial-1\n\
             hardware_interface_type = 1\n\
             hardware_eeprom_enabled = no\n\
             hardware_gpio_usb_port_path = 3-1\n\
             hardware_voter_reporting = 1\n\
             hardware_clip_led_gpio = 8\n\
             hardware_gpio_1_mode = out0\n\
             hardware_gpio_2_mode = out1\n\
             hardware_gpio_3_mode = in\n\
             hardware_gpio_4_mode = out0\n\
             hardware_gpio_5_mode = out1\n\
             hardware_gpio_6_mode = in\n\
             hardware_gpio_7_mode = out0\n\
             hardware_gpio_8_mode = out1\n\
             hardware_parallel_port_device = /dev/parport1\n\
             hardware_parallel_port_base_address = 0x400\n\
             hardware_parallel_pin_2_assignment = out0\n\
             hardware_parallel_pin_3_assignment = out1\n\
             hardware_parallel_pin_4_assignment = ptt\n\
             hardware_parallel_pin_5_assignment = out0\n\
             hardware_parallel_pin_6_assignment = out1\n\
             hardware_parallel_pin_7_assignment = ptt\n\
             hardware_parallel_pin_8_assignment = out0\n\
             hardware_parallel_pin_9_assignment = out1\n\
             hardware_parallel_pin_10_assignment = in\n\
             hardware_parallel_pin_12_assignment = cor\n\
             hardware_parallel_pin_13_assignment = ctcss\n\
             hardware_parallel_pin_15_assignment = in\n\
             \n\
             [receive]\n\
             signaling_method = ctcss\n\
             cpu_saver_enabled = yes\n\
             audio_source = speaker\n\
             cos_assignment = usb\n\
             vox_hang_ms = 100\n\
             vox_threshold = 200\n\
             noise_squelch_hysteresis = 300\n\
             noise_filter_type = 1\n\
             squelch_delay_ms = 400\n\
             on_delay_frames = 500\n\
             squelch_level = 600\n\
             frequency_hz = 145000000\n\
             \n\
             [transmit]\n\
             signaling_method = ctcss\n\
             cpu_saver_enabled = yes\n\
             preemphasis_enabled = no\n\
             settle_ms = 600\n\
             rx_blanking_ms = 700\n\
             off_delay_frames = 800\n\
             frequency_hz = 146000000\n\
             \n\
             [ctcss]\n\
             receive_frequencies = 67.0, 71.9\n\
             transmit_frequencies = 74.4, 77.0\n\
             receive_source = usb\n\
             receive_decoder_gain_db = 3.5\n\
             receive_override_enabled = yes\n\
             receive_relax = 0\n\
             transmit_default_hz = 79.7\n\
             transmit_peak_dbfs = -18\n\
             turnoff_mode = ctcss_tail_tone\n\
             phase_shift_degrees = 90\n\
             tail_duration_ms = 200\n\
             tail_frequency_hz = 60\n\
             \n\
             [dcs]\n\
             receive_code = 754i\n\
             transmit_code = 125n\n\
             turnoff_code_enabled = no\n\
             turnoff_duration_ms = 190\n\
             peak_dbfs = -20\n\
             \n\
             [duplex]\n\
             duplex_radio_mode = 1\n\
             duplex_local_repeat_level = 750\n\
             \n\
             [diagnostics]\n\
             diagnostics_status_publication_interval_ms = 25\n",
    )
    .unwrap();
    let config = resolved.config();
    assert!(resolved.warnings().is_empty());
    assert!(!config.channel_enabled);
    assert_eq!(
        config.asterisk.jitter_buffer_implementation,
        JitterBufferImplementation::Adaptive
    );
    assert_eq!(config.asterisk.jitter_buffer_max_size_ms, 300);
    assert_eq!(config.hardware.input_gain_db, -3.5);
    assert_eq!(
        config.hardware.output_b_assignment,
        HardwareOutputAssignment::Ctcss
    );
    assert_eq!(config.hardware.clip_led_gpio, Some(8));
    assert_eq!(
        config.hardware.gpio_modes,
        [
            GpioMode::OutputLow,
            GpioMode::OutputHigh,
            GpioMode::Input,
            GpioMode::OutputLow,
            GpioMode::OutputHigh,
            GpioMode::Input,
            GpioMode::OutputLow,
            GpioMode::OutputHigh,
        ]
    );
    assert_eq!(config.hardware.parallel_port.base_address, 0x400);
    assert_eq!(
        config.hardware.parallel_port.output_assignments[2],
        Some(ParallelOutputAssignment::PushToTalk)
    );
    assert_eq!(
        config.hardware.parallel_port.input_assignments,
        [
            Some(ParallelInputAssignment::Input),
            Some(ParallelInputAssignment::Carrier),
            Some(ParallelInputAssignment::Ctcss),
            Some(ParallelInputAssignment::Input),
        ]
    );
    assert_eq!(config.receive.signaling_method, SignalingMethod::Ctcss);
    assert_eq!(config.receive.audio_source, ReceiveAudioSource::Speaker);
    assert_eq!(config.receive.frequency_hz, 145_000_000);
    assert_eq!(config.transmit.signaling_method, SignalingMethod::Ctcss);
    assert_eq!(config.transmit.frequency_hz, 146_000_000);
    assert_eq!(
        format_tone_list(&config.ctcss.receive_frequencies),
        "67.0,71.9"
    );
    assert_eq!(config.ctcss.receive_source, CtcssSource::Usb);
    assert_eq!(config.ctcss.turnoff_mode, CtcssTurnoffMode::TailTone);
    assert_eq!(config.dcs.receive_code.to_string(), "754I");
    assert_eq!(config.dcs.transmit_code.to_string(), "125N");
    assert_eq!(config.duplex.radio_mode, RadioDuplexMode::Full);
    assert_eq!(config.duplex.local_repeat_level, 750);
    assert_eq!(config.diagnostics.status_publication_interval_ms, 25);
}

#[test]
fn flat_values_then_selected_scoped_values_preserve_inheritance() {
    let resolved = resolve(
        "[general]\n\
             channel_enabled = no\n\
             [usb]\n\
             channel_enabled = yes\n\
             hardware_profile = hill\n\
             [hardware]\n\
             hardware_input_gain_db = 3\n\
             hardware_output_a_gain_db = 4\n\
             [hardware hill]\n\
             hardware_input_gain_db = bad\n\
             hardware_output_b_gain_db = 5\n\
             future_hardware_knob = on\n\
             [receive usb]\n\
             squelch_level = 450\n",
    )
    .unwrap();
    let config = resolved.config();
    assert!(config.channel_enabled);
    assert_eq!(config.hardware.input_gain_db, 3.0);
    assert_eq!(config.hardware.output_a_gain_db, 4.0);
    assert_eq!(config.hardware.output_b_gain_db, 5.0);
    assert_eq!(config.receive.squelch_level, 450);
    assert_eq!(resolved.warnings().len(), 2);
    let invalid = resolved
        .warnings()
        .iter()
        .find(|warning| warning.name == "hardware_input_gain_db")
        .unwrap();
    assert_eq!(invalid.kind, ResolutionWarningKind::InvalidValue);
    assert_eq!(invalid.section, "hardware hill");
    assert_eq!(invalid.fallback, "3");
    let unknown = resolved
        .warnings()
        .iter()
        .find(|warning| warning.name == "future_hardware_knob")
        .unwrap();
    assert_eq!(unknown.kind, ResolutionWarningKind::UnknownOption);
    assert_eq!(unknown.fallback, "ignored");
}

#[test]
fn adr_0039_silently_ignores_retired_duplex_names() {
    let resolved = resolve(
        "[usb]\n\
             duplexmode = 99\n\
             duplex3 = 321\n\
             direct_repeat_level = 321\n\
             duplex3mode = hardware\n\
             [duplex]\n\
             duplex_local_repeat_mode = software\n\
             duplexmode = nonsense\n\
             duplex_local_repeat_level = 321\n",
    )
    .unwrap();
    assert_eq!(resolved.config().duplex.local_repeat_level, 321);
    assert_eq!(resolved.warnings().len(), 3);
    assert!(
        resolved
            .warnings()
            .iter()
            .all(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
    );
    for name in ["duplex3", "direct_repeat_level", "duplex3mode"] {
        assert!(
            resolved
                .warnings()
                .iter()
                .any(|warning| warning.name == name)
        );
    }
    assert!(resolved.warnings().iter().all(|warning| {
        warning.name != "duplexmode" && warning.name != "duplex_local_repeat_mode"
    }));
}

#[test]
fn retired_hardware_options_are_unknown() {
    let resolved = resolve(
        "[usb]\n\
             [hardware]\n\
             hardware_audio_fragment_count = 196\n\
             hardware_audio_queue_size = 20\n\
             hardware_audio_backend = portaudio_poc\n\
             hardware_gpio_backend = cm119_poc\n\
             hardware_portaudio_input_device_index = 4\n\
             hardware_portaudio_output_device_index = 5\n",
    )
    .unwrap();
    assert_eq!(resolved.warnings().len(), 6);
    assert_eq!(
        resolved
            .warnings()
            .iter()
            .filter(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
            .count(),
        6
    );
}

#[test]
fn retired_native_signaling_and_trace_options_are_not_configuration() {
    let resolved = resolve(
        "[usb]\n\
             [hardware]\n\
             hardware_repeater_number = 1\n\
             hardware_area = 2\n\
             hardware_user_key = key\n\
             hardware_idle_interval = 3\n\
             hardware_turnoff_count = 4\n\
             [receive]\n\
             polarity_inverted = yes\n\
             lsd_polarity_inverted = yes\n\
             [transmit]\n\
             polarity_inverted = yes\n\
             lsd_polarity_inverted = yes\n\
             [diagnostics]\n\
             diagnostics_trace_type = 1\n\
             diagnostics_trace_level = 2\n\
             diagnostics_fever = 3\n",
    )
    .unwrap();
    assert_eq!(resolved.config(), &StationConfig::default());
    assert_eq!(resolved.warnings().len(), 12);
    assert!(
        resolved
            .warnings()
            .iter()
            .all(|warning| warning.kind == ResolutionWarningKind::UnknownOption)
    );
}

#[test]
fn malformed_scalars_warn_and_fall_back_at_their_overlay_layer() {
    let resolved = resolve(
        "[usb]\n\
             [receive]\n\
             cpu_saver_enabled = true\n\
             vox_threshold = -1\n\
             noise_filter_type = 2\n\
             squelch_delay_ms = 512\n\
             frequency_hz = 2147483648\n\
             [transmit]\n\
             settle_ms = 1.5\n\
             [ctcss]\n\
             receive_decoder_gain_db = NaN\n\
             tail_duration_ms = 39\n\
             [dcs]\n\
             turnoff_duration_ms = 201\n\
             peak_dbfs = 0.1\n\
             [duplex]\n\
             duplex_radio_mode = 2\n\
             duplex_local_repeat_level = 1000\n",
    )
    .unwrap();
    assert_eq!(resolved.config(), &StationConfig::default());
    assert_eq!(resolved.warnings().len(), 12);
    assert!(
        resolved
            .warnings()
            .iter()
            .all(|warning| warning.kind == ResolutionWarningKind::InvalidValue)
    );
}

#[test]
fn c_style_integer_syntax_is_complete_and_bounded() {
    let resolved = resolve(
        "[usb]\n\
             [hardware]\n\
             hardware_parallel_port_base_address = 0x400\n\
             [receive]\n\
             frequency_hz = 077\n\
             vox_threshold = 08\n",
    )
    .unwrap();
    assert_eq!(resolved.config().hardware.parallel_port.base_address, 0x400);
    assert_eq!(resolved.config().receive.frequency_hz, 0o77);
    assert_eq!(resolved.config().receive.vox_threshold, 0);
    assert_eq!(resolved.warnings().len(), 1);
}

#[test]
fn gpio_pin_zero_is_tolerantly_ignored() {
    let resolved = resolve("[usb]\n[hardware]\nhardware_gpio_0_mode = out1\n").unwrap();

    assert_eq!(resolved.config().hardware.gpio_modes, [GpioMode::Input; 8]);
    assert_eq!(resolved.warnings().len(), 1);
    assert_eq!(
        resolved.warnings()[0].kind,
        ResolutionWarningKind::UnknownOption
    );
}

#[test]
fn dcs_codes_require_exact_syntax_and_serialize_canonically() {
    let normal: DcsCode = "023n".parse().unwrap();
    let inverted: DcsCode = "777I".parse().unwrap();
    assert_eq!(normal.value(), 0o23);
    assert_eq!(normal.polarity(), DcsPolarity::Normal);
    assert_eq!(normal.to_string(), "023N");
    assert_eq!(inverted.value(), 0o777);
    assert_eq!(inverted.to_string(), "777I");
    for invalid in [
        "23N", "0023N", "028N", "023X", "-23N", "777 I", " 023N", "023N ", "",
    ] {
        assert_eq!(invalid.parse::<DcsCode>(), Err(DcsCodeParseError));
    }
}

#[test]
fn ctcss_tones_match_the_complete_native_table() {
    for tenths in CTCSS_TENTHS_HZ {
        let tone = CtcssTone::from_tenths_hz(tenths).unwrap();
        assert_eq!(tone.tenths_hz(), tenths);
        assert_eq!(tone.to_string().parse::<CtcssTone>().unwrap(), tone);
    }
    assert!(CtcssTone::from_tenths_hz(1_001).is_none());
    for invalid in ["", "0", "66.9", "100.1", "250.4", "NaN", "inf"] {
        assert_eq!(invalid.parse::<CtcssTone>(), Err(CtcssToneParseError));
    }
    assert!(parse_tone_list(&["100.0"; 38].join(",")).is_ok());
    assert!(parse_tone_list(&["100.0"; 39].join(",")).is_err());
}

#[test]
fn supported_ctcss_tones_retain_native_decoder_order() {
    let tones: Vec<_> = CtcssTone::supported().collect();
    assert_eq!(tones.len(), 38);
    for (index, tone) in tones.into_iter().enumerate() {
        assert_eq!(tone.table_index(), index);
    }
}

#[test]
fn unsafe_cross_field_combinations_are_hard_errors() {
    let cases = [
        (
            "[usb]\n[receive]\nsignaling_method = ctcss\n[ctcss]\nreceive_source = no\n",
            StationValidationError::DisabledCtcssReceiveSource,
        ),
        (
            "[usb]\n[receive]\nsignaling_method = ctcss\n[transmit]\nsignaling_method = ctcss\n[ctcss]\nreceive_frequencies = 67.0,71.9\ntransmit_frequencies = 67.0\n",
            StationValidationError::CtcssMapLength {
                receive: 2,
                transmit: 1,
            },
        ),
        (
            "[usb]\n[hardware]\nhardware_output_a_assignment = voice\nhardware_output_b_assignment = off\n[transmit]\nsignaling_method = dcs\n",
            StationValidationError::MissingTransmitSignalingRoute,
        ),
    ];
    for (document, expected) in cases {
        assert_eq!(
            resolve(document),
            Err(StationConfigError::InvalidResolvedConfiguration(expected))
        );
    }
}

#[test]
fn structural_profile_ambiguity_and_missing_sections_remain_errors() {
    let conflict = resolve(
        "[usb]\n\
             Hardware_Profile = one\n\
             hardware_profile = two\n\
             [hardware one]\n\
             [hardware two]\n",
    );
    assert!(matches!(
        conflict,
        Err(StationConfigError::AmbiguousProfileSelection { .. })
    ));

    let conflict = resolve(
        "[usb]\n\
             [hardware]\n\
             hardware_input_gain_db = 1\n\
             HARDWARE_INPUT_GAIN_DB = 2\n",
    );
    assert!(matches!(
        conflict,
        Err(StationConfigError::ConflictingAssignment { .. })
    ));

    assert_eq!(
        resolve("[other]\n").unwrap_err(),
        StationConfigError::Document(ConfigError::MissingChannel("usb".to_owned()))
    );
    let resolved = resolve("[usb]\nhardware_profile = absent\n").unwrap();
    assert_eq!(resolved.config(), &StationConfig::default());
    assert_eq!(resolved.warnings().len(), 1);
    assert_eq!(resolved.warnings()[0].name, "hardware_profile");
    assert_eq!(resolved.warnings()[0].supplied_value, "absent");
    assert_eq!(resolved.warnings()[0].fallback, "hardware");
}

#[test]
fn public_enum_text_and_error_context_are_complete() {
    use std::error::Error as _;

    let values = [
        (JitterBufferImplementation::Fixed.to_string(), "fixed"),
        (JitterBufferImplementation::Adaptive.to_string(), "adaptive"),
        (HardwareInterfaceType::DudeUsb.to_string(), "0"),
        (HardwareInterfaceType::SphUsb.to_string(), "1"),
        (HardwareOutputAssignment::Off.to_string(), "off"),
        (HardwareOutputAssignment::Voice.to_string(), "voice"),
        (HardwareOutputAssignment::Ctcss.to_string(), "ctcss"),
        (
            HardwareOutputAssignment::VoiceCtcss.to_string(),
            "voice_ctcss",
        ),
        (
            HardwareOutputAssignment::AuxiliaryVoice.to_string(),
            "auxvoice",
        ),
        (GpioMode::Input.to_string(), "in"),
        (GpioMode::OutputLow.to_string(), "out0"),
        (GpioMode::OutputHigh.to_string(), "out1"),
        (ParallelOutputAssignment::Low.to_string(), "out0"),
        (ParallelOutputAssignment::High.to_string(), "out1"),
        (ParallelOutputAssignment::PushToTalk.to_string(), "ptt"),
        (ParallelInputAssignment::Input.to_string(), "in"),
        (ParallelInputAssignment::Carrier.to_string(), "cor"),
        (ParallelInputAssignment::Ctcss.to_string(), "ctcss"),
        (SignalingMethod::Carrier.to_string(), "carrier"),
        (SignalingMethod::Ctcss.to_string(), "ctcss"),
        (SignalingMethod::Dcs.to_string(), "dcs"),
        (ReceiveAudioSource::Disabled.to_string(), "no"),
        (ReceiveAudioSource::Speaker.to_string(), "speaker"),
        (ReceiveAudioSource::Flat.to_string(), "flat"),
        (CarrierSource::Disabled.to_string(), "no"),
        (CarrierSource::Dsp.to_string(), "dsp"),
        (CarrierSource::Vox.to_string(), "vox"),
        (CarrierSource::Usb.to_string(), "usb"),
        (CarrierSource::UsbInverted.to_string(), "usbinvert"),
        (CarrierSource::Parallel.to_string(), "pp"),
        (CarrierSource::ParallelInverted.to_string(), "ppinvert"),
        (CtcssSource::Disabled.to_string(), "no"),
        (CtcssSource::Usb.to_string(), "usb"),
        (CtcssSource::UsbInverted.to_string(), "usbinvert"),
        (CtcssSource::Dsp.to_string(), "dsp"),
        (CtcssSource::Parallel.to_string(), "pp"),
        (CtcssSource::ParallelInverted.to_string(), "ppinvert"),
        (CtcssTurnoffMode::None.to_string(), "no"),
        (
            CtcssTurnoffMode::PhaseShift.to_string(),
            "ctcss_phase_shift",
        ),
        (
            CtcssTurnoffMode::ToneRemove.to_string(),
            "ctcss_tone_remove",
        ),
        (CtcssTurnoffMode::TailTone.to_string(), "ctcss_tail_tone"),
        (DcsPolarity::Normal.to_string(), "N"),
        (DcsPolarity::Inverted.to_string(), "I"),
        (RadioDuplexMode::Half.to_string(), "0"),
        (RadioDuplexMode::Full.to_string(), "1"),
    ];
    for (actual, expected) in values {
        assert_eq!(actual, expected);
    }

    let tone = CtcssTone::from_tenths_hz(670).unwrap();
    assert_eq!(tone.as_hz(), 67.0);
    assert!(!CtcssToneParseError.to_string().is_empty());
    assert!(!DcsCodeParseError.to_string().is_empty());

    let validation_errors = [
        StationValidationError::DisabledCtcssReceiveSource,
        StationValidationError::CtcssMapLength {
            receive: 2,
            transmit: 1,
        },
        StationValidationError::MissingTransmitSignalingRoute,
    ];
    for error in validation_errors {
        assert!(!error.to_string().is_empty());
    }

    let document = StationConfigError::Document(ConfigError::MissingChannel("usb".to_owned()));
    let conflict = StationConfigError::ConflictingAssignment {
        source: "radio.conf".to_owned(),
        section: "hardware".to_owned(),
        name: "setting".to_owned(),
        first: "one".to_owned(),
        second: "two".to_owned(),
    };
    let ambiguous = StationConfigError::AmbiguousProfileSelection {
        channel: "usb".to_owned(),
        selector: "hardware_profile".to_owned(),
    };
    let invalid = StationConfigError::from(StationValidationError::DisabledCtcssReceiveSource);
    for error in [&document, &conflict, &ambiguous, &invalid] {
        assert!(!error.to_string().is_empty());
    }
    assert!(document.source().is_some());
    assert!(invalid.source().is_some());
    assert!(conflict.source().is_none());
    assert!(ambiguous.source().is_none());

    let resolved = resolve("[usb]\n").unwrap();
    assert_eq!(resolved.clone().into_config(), resolved.config().clone());
}

#[test]
fn private_value_parsers_cover_every_supported_spelling_and_failure() {
    assert_eq!(
        parse_jitter_implementation("fixed").unwrap(),
        JitterBufferImplementation::Fixed
    );
    assert_eq!(
        parse_jitter_implementation("ADAPTIVE").unwrap(),
        JitterBufferImplementation::Adaptive
    );
    assert!(parse_jitter_implementation("other").is_err());
    assert_eq!(
        parse_hardware_interface("0").unwrap(),
        HardwareInterfaceType::DudeUsb
    );
    assert_eq!(
        parse_hardware_interface("1").unwrap(),
        HardwareInterfaceType::SphUsb
    );
    assert!(parse_hardware_interface("2").is_err());

    for (text, expected) in [
        ("off", HardwareOutputAssignment::Off),
        ("voice", HardwareOutputAssignment::Voice),
        ("ctcss", HardwareOutputAssignment::Ctcss),
        ("voice_ctcss", HardwareOutputAssignment::VoiceCtcss),
        ("auxvoice", HardwareOutputAssignment::AuxiliaryVoice),
    ] {
        assert_eq!(parse_hardware_output(text).unwrap(), expected);
    }
    assert!(parse_hardware_output("other").is_err());

    for (text, expected) in [
        ("in", GpioMode::Input),
        ("out0", GpioMode::OutputLow),
        ("out1", GpioMode::OutputHigh),
    ] {
        assert_eq!(parse_gpio_mode(text).unwrap(), expected);
    }
    assert!(parse_gpio_mode("other").is_err());

    for (text, expected) in [
        ("out0", ParallelOutputAssignment::Low),
        ("out1", ParallelOutputAssignment::High),
        ("ptt", ParallelOutputAssignment::PushToTalk),
    ] {
        assert_eq!(parse_parallel_output(text).unwrap(), expected);
    }
    assert!(parse_parallel_output("other").is_err());

    for (text, expected) in [
        ("in", ParallelInputAssignment::Input),
        ("cor", ParallelInputAssignment::Carrier),
        ("ctcss", ParallelInputAssignment::Ctcss),
    ] {
        assert_eq!(parse_parallel_input(text).unwrap(), expected);
    }
    assert!(parse_parallel_input("other").is_err());

    for (text, expected) in [
        ("carrier", SignalingMethod::Carrier),
        ("ctcss", SignalingMethod::Ctcss),
        ("dcs", SignalingMethod::Dcs),
    ] {
        assert_eq!(parse_signaling_method(text).unwrap(), expected);
    }
    assert!(parse_signaling_method("other").is_err());

    for (text, expected) in [
        ("no", ReceiveAudioSource::Disabled),
        ("speaker", ReceiveAudioSource::Speaker),
        ("flat", ReceiveAudioSource::Flat),
    ] {
        assert_eq!(parse_receive_audio_source(text).unwrap(), expected);
    }
    assert!(parse_receive_audio_source("other").is_err());

    for (text, expected) in [
        ("no", CarrierSource::Disabled),
        ("dsp", CarrierSource::Dsp),
        ("vox", CarrierSource::Vox),
        ("usb", CarrierSource::Usb),
        ("usbinvert", CarrierSource::UsbInverted),
        ("pp", CarrierSource::Parallel),
        ("ppinvert", CarrierSource::ParallelInverted),
    ] {
        assert_eq!(parse_carrier_source(text).unwrap(), expected);
    }
    assert!(parse_carrier_source("other").is_err());

    for (text, expected) in [
        ("no", CtcssSource::Disabled),
        ("usb", CtcssSource::Usb),
        ("usbinvert", CtcssSource::UsbInverted),
        ("dsp", CtcssSource::Dsp),
        ("pp", CtcssSource::Parallel),
        ("ppinvert", CtcssSource::ParallelInverted),
    ] {
        assert_eq!(parse_ctcss_source(text).unwrap(), expected);
    }
    assert!(parse_ctcss_source("other").is_err());

    for (text, expected) in [
        ("no", CtcssTurnoffMode::None),
        ("ctcss_phase_shift", CtcssTurnoffMode::PhaseShift),
        ("ctcss_tone_remove", CtcssTurnoffMode::ToneRemove),
        ("ctcss_tail_tone", CtcssTurnoffMode::TailTone),
    ] {
        assert_eq!(parse_ctcss_turnoff_mode(text).unwrap(), expected);
    }
    assert!(parse_ctcss_turnoff_mode("other").is_err());
    assert_eq!(parse_radio_duplex_mode("0").unwrap(), RadioDuplexMode::Half);
    assert_eq!(parse_radio_duplex_mode("1").unwrap(), RadioDuplexMode::Full);
    assert!(parse_radio_duplex_mode("2").is_err());
}

#[test]
fn c_integer_and_pin_index_helpers_cover_radix_and_malformed_input() {
    assert_eq!(parse_c_unsigned("+10"), Ok(10));
    assert_eq!(parse_c_unsigned("0X10"), Ok(16));
    assert_eq!(parse_c_unsigned("077"), Ok(0o77));
    assert!(parse_c_unsigned("").is_err());
    assert!(parse_c_unsigned("-").is_err());
    assert!(parse_c_unsigned("0x").is_err());
    assert_eq!(gpio_mode_index("hardware_gpio_1_mode"), Some(0));
    assert_eq!(gpio_mode_index("hardware_gpio_8_mode"), Some(7));
    assert_eq!(gpio_mode_index("hardware_gpio_0_mode"), None);
    assert_eq!(gpio_mode_index("hardware_gpio_9_mode"), None);
    assert_eq!(gpio_mode_index("gpio_1_mode"), None);
    assert_eq!(gpio_mode_index("hardware_gpio_1"), None);
    assert_eq!(gpio_mode_index("hardware_gpio_bad_mode"), None);
    assert_eq!(
        parallel_output_index("hardware_parallel_pin_2_assignment"),
        Some(0)
    );
    assert_eq!(parallel_output_index("other"), None);
    assert_eq!(
        parallel_input_index("hardware_parallel_pin_15_assignment"),
        Some(3)
    );
    assert_eq!(parallel_input_index("other"), None);
    assert_eq!(yes_no(true), "yes");
    assert_eq!(yes_no(false), "no");
    assert_eq!(format_number(1.5), "1.5");
}

#[test]
fn invalid_and_unknown_settings_retain_fallbacks_with_context() {
    let mut config = StationConfig::default();
    let mut warnings = Vec::new();
    let setting = |name, value| Setting {
        source: "radio.conf",
        section: "test",
        name,
        value,
    };

    apply_general(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_radio(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_asterisk(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_receive(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_transmit(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_dcs(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_duplex(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );
    apply_diagnostics(
        &mut config,
        "future",
        setting("future", "on"),
        &mut warnings,
    );

    apply_hardware(
        &mut config,
        "hardware_parallel_port_device",
        setting("hardware_parallel_port_device", ""),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_clip_led_gpio",
        setting("hardware_clip_led_gpio", "8"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_clip_led_gpio",
        setting("hardware_clip_led_gpio", "8"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_clip_led_gpio",
        setting("hardware_clip_led_gpio", "9"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_parallel_port_base_address",
        setting("hardware_parallel_port_base_address", "0"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_input_gain_db",
        setting("hardware_input_gain_db", "-31"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_deemphasis_corner_hz",
        setting("hardware_deemphasis_corner_hz", "501"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_deemphasis_corner_hz",
        setting("hardware_deemphasis_corner_hz", "0"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_deemphasis_corner_hz",
        setting("hardware_deemphasis_corner_hz", "NaN"),
        &mut warnings,
    );
    apply_hardware(
        &mut config,
        "hardware_parallel_pin_2_assignment",
        setting("hardware_parallel_pin_2_assignment", "bad"),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "transmit_default_hz",
        setting("transmit_default_hz", "bad"),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "receive_frequencies",
        setting("receive_frequencies", ""),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "transmit_frequencies",
        setting("transmit_frequencies", "bad"),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "phase_shift_degrees",
        setting("phase_shift_degrees", "0"),
        &mut warnings,
    );
    apply_ctcss(
        &mut config,
        "tail_frequency_hz",
        setting("tail_frequency_hz", "NaN"),
        &mut warnings,
    );
    apply_dcs(
        &mut config,
        "receive_code",
        setting("receive_code", "bad"),
        &mut warnings,
    );
    assert!(warnings.len() >= 21);
    assert!(warnings.iter().all(|warning| !warning.fallback.is_empty()));
}

#[test]
fn duplicate_equal_assignments_and_validation_short_circuits_are_safe() {
    let uppercase_flat = resolve("[usb]\n[HARDWARE]\nhardware_input_gain_db = 1\n").unwrap();
    assert_eq!(uppercase_flat.config().hardware.input_gain_db, 1.0);

    let document = ConfigDocument::new(
        "[usb]\nHardware_Profile = one\nhardware_profile = ONE\n[hardware one]\n",
    );
    assert_eq!(
        reject_ambiguous_selector(&document, "usb", "hardware"),
        Ok(())
    );

    let mut config = StationConfig::default();
    let mut warnings = Vec::new();
    let values = BTreeMap::from([
        ("Channel_Enabled".to_owned(), " yes ".to_owned()),
        ("channel_enabled".to_owned(), "yes".to_owned()),
    ]);
    apply_overlay(
        &mut config,
        apply_general,
        "radio.conf",
        "general",
        values,
        &mut warnings,
    )
    .unwrap();
    assert!(config.channel_enabled);

    let mut candidate = StationConfig::default();
    candidate.receive.signaling_method = SignalingMethod::Ctcss;
    candidate.transmit.signaling_method = SignalingMethod::Carrier;
    assert_eq!(candidate.validate(), Ok(()));

    candidate.transmit.signaling_method = SignalingMethod::Ctcss;
    candidate.hardware.output_a_assignment = HardwareOutputAssignment::VoiceCtcss;
    assert_eq!(candidate.validate(), Ok(()));

    candidate.hardware.output_a_assignment = HardwareOutputAssignment::Off;
    candidate.hardware.output_b_assignment = HardwareOutputAssignment::Ctcss;
    assert_eq!(candidate.validate(), Ok(()));
}
