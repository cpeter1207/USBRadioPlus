use super::*;
use crate::{ProcessingStage, StageOrderError};

trait ResolvedProfileTestExt {
    fn chain(&self) -> ProcessingChain;
}

impl ResolvedProfileTestExt for ResolvedProfile {
    fn chain(&self) -> ProcessingChain {
        self.clone().into_chain()
    }
}

fn values(entries: &[(&str, &str)]) -> Vec<(String, String)> {
    entries
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect()
}

fn common_values() -> Vec<(String, String)> {
    values(&[
        ("enabled", "yes"),
        (
            "stage_order",
            "equalizer,expander,agc,deesser,compressor,limiter",
        ),
        ("input_gain_db", "1"),
        ("output_gain_db", "-1"),
        ("equalizer_enabled", "yes"),
        ("equalizer_low_gain_db", "1"),
        ("equalizer_low_frequency_hz", "400"),
        ("equalizer_low_slope", "0.5"),
        ("equalizer_mid_gain_db", "1"),
        ("equalizer_mid_frequency_hz", "1200"),
        ("equalizer_mid_width_octaves", "0.8"),
        ("equalizer_high_gain_db", "1"),
        ("equalizer_high_frequency_hz", "2500"),
        ("equalizer_high_slope", "0.5"),
        ("deesser_enabled", "yes"),
        ("deesser_frequency_hz", "4500"),
        ("deesser_width_octaves", "0.8"),
        ("deesser_threshold_dbfs", "-20"),
        ("deesser_ratio", "4"),
        ("deesser_max_reduction_db", "5"),
        ("deesser_attack_ms", "3"),
        ("deesser_release_ms", "70"),
        ("agc_enabled", "yes"),
        ("agc_target_dbfs", "-20"),
        ("agc_max_gain_db", "8"),
        ("agc_max_attenuation_db", "8"),
        ("agc_rms_averaging_ms", "250"),
        ("agc_gain_increase_db_per_second", "3"),
        ("agc_gain_decrease_db_per_second", "7"),
        ("agc_activity_threshold_dbfs", "-45"),
        ("agc_activity_hysteresis_db", "4"),
        ("agc_hold_ms", "600"),
        ("agc_deadband_db", "2"),
        ("agc_sidechain_highpass_hz", "700"),
        ("agc_sidechain_lowpass_hz", "1600"),
        ("expander_enabled", "yes"),
        ("expander_threshold_dbfs", "-50"),
        ("expander_ratio", "2"),
        ("expander_max_attenuation_db", "10"),
        ("expander_attack_ms", "20"),
        ("expander_release_ms", "300"),
        ("expander_sidechain_highpass_hz", "400"),
        ("expander_sidechain_lowpass_hz", "1600"),
        ("compressor_enabled", "yes"),
        ("compressor_bands", "1"),
        ("compressor_low_crossover_hz", "600"),
        ("compressor_high_crossover_hz", "2200"),
        ("compressor_threshold_dbfs", "-8"),
        ("compressor_ratio", "3"),
        ("compressor_makeup_gain_db", "1"),
        ("compressor_attack_ms", "80"),
        ("compressor_release_ms", "350"),
        ("compressor_low_threshold_dbfs", "-8"),
        ("compressor_low_ratio", "3"),
        ("compressor_low_makeup_gain_db", "1"),
        ("compressor_low_knee_db", "8"),
        ("compressor_low_attack_ms", "80"),
        ("compressor_low_release_ms", "350"),
        ("compressor_mid_threshold_dbfs", "-8"),
        ("compressor_mid_ratio", "3"),
        ("compressor_mid_makeup_gain_db", "1"),
        ("compressor_mid_knee_db", "8"),
        ("compressor_mid_attack_ms", "80"),
        ("compressor_mid_release_ms", "350"),
        ("compressor_high_threshold_dbfs", "-8"),
        ("compressor_high_ratio", "3"),
        ("compressor_high_makeup_gain_db", "1"),
        ("compressor_high_knee_db", "8"),
        ("compressor_high_attack_ms", "80"),
        ("compressor_high_release_ms", "350"),
        ("compressor_sidechain_highpass_hz", "700"),
        ("compressor_sidechain_lowpass_hz", "1600"),
        ("limiter_enabled", "yes"),
        ("limiter_bands", "1"),
        ("limiter_low_crossover_hz", "600"),
        ("limiter_high_crossover_hz", "2200"),
        ("limiter_threshold_dbfs", "-2"),
        ("limiter_ratio", "18"),
        ("limiter_knee_db", "1"),
        ("limiter_attack_ms", "2"),
        ("limiter_release_ms", "60"),
        ("limiter_low_threshold_dbfs", "-2"),
        ("limiter_low_ratio", "12"),
        ("limiter_low_knee_db", "5"),
        ("limiter_low_attack_ms", "40"),
        ("limiter_low_release_ms", "200"),
        ("limiter_mid_threshold_dbfs", "-2"),
        ("limiter_mid_ratio", "12"),
        ("limiter_mid_knee_db", "5"),
        ("limiter_mid_attack_ms", "8"),
        ("limiter_mid_release_ms", "80"),
        ("limiter_high_threshold_dbfs", "-2"),
        ("limiter_high_ratio", "18"),
        ("limiter_high_knee_db", "5"),
        ("limiter_high_attack_ms", "0.8"),
        ("limiter_high_release_ms", "40"),
    ])
}

#[test]
fn precedence_is_compiled_then_flat_then_scoped() {
    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("input_gain_db", "1.0"), ("equalizer_low_gain_db", "3.0")]),
    );
    let scoped = RawOverlay::scoped(
        ChainRole::Link,
        "radio.conf",
        "link hilltop",
        values(&[("input_gain_db", "2.0")]),
    );
    let resolved = ResolvedProfile::resolve(ChainRole::Link, &flat, Some(&scoped)).unwrap();
    assert_eq!(resolved.chain().input_gain_db, 2.0);
    assert_eq!(resolved.chain().equalizer.low_gain_db, 3.0);
    assert_eq!(resolved.chain().output_gain_db, -6.2);
    assert!(resolved.warnings().is_empty());
}

#[test]
fn every_common_and_source_specific_schema_field_is_applied() {
    let mut local_values = common_values();
    local_values.extend(values(&[
        ("rnnoise_enabled", "yes"),
        ("receive_bandpass_enabled", "yes"),
        ("receive_bandpass_highpass_hz", "30"),
        ("receive_bandpass_lowpass_hz", "4800"),
        ("ctcss_filter_mode", "disabled"),
        ("ctcss_notch_width_hz", "4"),
        ("ctcss_highpass_hz", "200"),
    ]));
    let local = RawOverlay::flat(ChainRole::LocalReceive, "radio.conf", local_values);
    let local = ResolvedProfile::resolve(ChainRole::LocalReceive, &local, None).unwrap();
    assert!(local.warnings().is_empty());
    assert_eq!(local.chain().compressor.layout, BandLayout::FullBand);
    assert_eq!(local.chain().limiter.high.attack_ms, 0.8);
    assert!(local.chain().rnnoise_enabled);
    assert_eq!(local.chain().receive.pl_filter, PlFilter::Disabled);

    let mut voice_values = common_values();
    voice_values.extend(values(&[
        ("lookahead_limiter_enabled", "yes"),
        ("lookahead_limiter_ceiling_dbfs", "-2"),
        ("lookahead_limiter_lookahead_ms", "4"),
        ("lookahead_limiter_attack_ms", "2"),
        ("lookahead_limiter_release_ms", "80"),
        ("post_limiter_bandpass_enabled", "yes"),
        ("post_limiter_bandpass_highpass_hz", "100"),
        ("post_limiter_bandpass_lowpass_hz", "6000"),
    ]));
    let voice = RawOverlay::flat(ChainRole::VoiceTelemetry, "radio.conf", voice_values);
    let voice = ResolvedProfile::resolve(ChainRole::VoiceTelemetry, &voice, None).unwrap();
    assert!(voice.warnings().is_empty());
    assert!(voice.chain().transmit_tail.limiter_enabled);
    assert!(voice.chain().transmit_tail.bandpass_enabled);
    assert_eq!(voice.chain().transmit_tail.ceiling_dbfs, -2.0);
}

#[test]
fn malformed_unknown_and_source_inapplicable_values_warn_and_fall_back() {
    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[
            ("agc_target_dbfs", "-20"),
            ("future_knob", "future-value"),
            ("rnnoise_enabled", "yes"),
        ]),
    );
    let scoped = RawOverlay::scoped(
        ChainRole::Link,
        "radio.conf",
        "link hilltop",
        values(&[
            ("agc_target_dbfs", "loud"),
            ("equalizer_enabled", "sometimes"),
        ]),
    );
    let resolved = ResolvedProfile::resolve(ChainRole::Link, &flat, Some(&scoped)).unwrap();
    assert_eq!(resolved.chain().agc.target_dbfs, -20.0);
    assert!(resolved.chain().equalizer.enabled);
    assert!(!resolved.chain().rnnoise_enabled);
    assert_eq!(
        resolved
            .warnings()
            .iter()
            .map(|warning| warning.kind)
            .collect::<Vec<_>>(),
        [
            ResolutionWarningKind::UnknownOption,
            ResolutionWarningKind::UnsupportedOption,
            ResolutionWarningKind::InvalidValue,
            ResolutionWarningKind::InvalidValue,
        ]
    );
    let malformed = &resolved.warnings()[2];
    assert_eq!(malformed.source, "radio.conf");
    assert_eq!(malformed.section, "link hilltop");
    assert_eq!(malformed.fallback, "-20");
    assert!(malformed.to_string().contains("agc_target_dbfs=\"loud\""));
}

#[test]
fn scalar_and_enum_fallbacks_keep_the_value_inherited_at_each_layer() {
    assert!(parse_number("NaN", -1.0, 1.0, false).is_err());
    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[
            ("enabled", "no"),
            ("compressor_bands", "1"),
            ("limiter_bands", "invalid"),
            ("input_gain_db", "999"),
            ("agc_sidechain_highpass_hz", "-1"),
            ("agc_sidechain_lowpass_hz", "0"),
        ]),
    );
    let scoped = RawOverlay::scoped(
        ChainRole::Link,
        "radio.conf",
        "link hilltop",
        values(&[("compressor_bands", "invalid")]),
    );
    let resolved = ResolvedProfile::resolve(ChainRole::Link, &flat, Some(&scoped)).unwrap();
    assert!(!resolved.chain().enabled);
    assert_eq!(resolved.chain().compressor.layout, BandLayout::FullBand);
    assert_eq!(resolved.chain().limiter.layout, BandLayout::ThreeBand);
    assert_eq!(resolved.chain().input_gain_db, 0.0);
    assert_eq!(resolved.chain().agc.sidechain_highpass_hz, 800.0);
    assert_eq!(resolved.chain().agc.sidechain_lowpass_hz, 0.0);
    assert_eq!(resolved.warnings().len(), 4);
    assert_eq!(resolved.warnings()[0].fallback, "3");
    assert_eq!(resolved.warnings()[3].fallback, "1");

    for (inherited, expected) in [
        ("disabled", PlFilter::Disabled),
        ("notch", PlFilter::DecodedToneNotch),
        ("highpass", PlFilter::HighPass),
    ] {
        let flat = RawOverlay::flat(
            ChainRole::LocalReceive,
            "radio.conf",
            values(&[("ctcss_filter_mode", inherited)]),
        );
        let scoped = RawOverlay::scoped(
            ChainRole::LocalReceive,
            "radio.conf",
            "local hilltop",
            values(&[("ctcss_filter_mode", "legacy-mode")]),
        );
        let resolved =
            ResolvedProfile::resolve(ChainRole::LocalReceive, &flat, Some(&scoped)).unwrap();
        assert_eq!(resolved.chain().receive.pl_filter, expected);
        assert_eq!(resolved.warnings()[0].fallback, inherited);
    }
}

#[test]
fn malformed_stage_order_warns_but_enabled_stage_conflict_is_a_final_error() {
    let malformed = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("stage_order", "agc,,equalizer")]),
    );
    let resolved = ResolvedProfile::resolve(ChainRole::Link, &malformed, None).unwrap();
    assert_eq!(resolved.chain().stage_order, StageOrder::standard());
    assert_eq!(resolved.warnings().len(), 1);

    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("stage_order", "equalizer")]),
    );
    let scoped = RawOverlay::scoped(
        ChainRole::Link,
        "radio.conf",
        "link hilltop",
        values(&[("agc_enabled", "yes")]),
    );
    assert_eq!(
        ResolvedProfile::resolve(ChainRole::Link, &flat, Some(&scoped)),
        Err(ProfileResolutionError::InvalidResolvedProfile(
            ProcessingConfigError::StageOrder(StageOrderError::MissingEnabledStage(
                ProcessingStage::Agc
            ))
        ))
    );
}

#[test]
fn individually_valid_but_inverted_edges_are_rejected_after_all_overlays() {
    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("agc_sidechain_highpass_hz", "1000")]),
    );
    let scoped = RawOverlay::scoped(
        ChainRole::Link,
        "radio.conf",
        "link hilltop",
        values(&[("agc_sidechain_lowpass_hz", "500")]),
    );
    assert!(matches!(
        ResolvedProfile::resolve(ChainRole::Link, &flat, Some(&scoped)),
        Err(ProfileResolutionError::InvalidResolvedProfile(
            ProcessingConfigError::Ordering { .. }
        ))
    ));
}

#[test]
fn document_resolution_uses_selected_profile_over_flat_values() {
    let document = ConfigDocument::new(
        "[usb]\nlocal_profile = hilltop\n\
             [local]\ninput_gain_db = 1\noutput_gain_db = -4\n\
             [local hilltop]\ninput_gain_db = 2\n",
    );
    let resolved = ResolvedProfile::from_document(
        &document,
        "/etc/asterisk/usbradioplus.conf",
        "usb",
        ChainRole::LocalReceive,
    )
    .unwrap();
    assert_eq!(resolved.chain().input_gain_db, 2.0);
    assert_eq!(resolved.chain().output_gain_db, -4.0);
}

#[test]
fn document_without_a_profile_uses_flat_values_and_reports_structural_errors() {
    let document = ConfigDocument::new(
        "[usb]\nfuture_selector = retained\nLINK_PROFILE = usb\nlink_profile = USB\n\
             [link]\ninput_gain_db = 3\n",
    );
    let resolved =
        ResolvedProfile::from_document(&document, "radio.conf", "usb", ChainRole::Link).unwrap();
    assert_eq!(resolved.chain().input_gain_db, 3.0);
    assert_eq!(resolved.into_chain().role, ChainRole::Link);

    let missing =
        ResolvedProfile::from_document(&document, "radio.conf", "missing", ChainRole::Link)
            .unwrap_err();
    assert!(matches!(
        missing,
        ProfileResolutionError::Document(ConfigError::MissingChannel(_))
    ));
}

#[test]
fn ambiguous_case_variants_and_overlay_contract_mismatches_are_errors() {
    let document = ConfigDocument::new(
        "[usb]\nlocal_profile = one\nLOCAL_PROFILE = two\n\
             [local]\n\
             [local one]\n\
             [local two]\n",
    );
    assert!(matches!(
        ResolvedProfile::from_document(&document, "radio.conf", "usb", ChainRole::LocalReceive),
        Err(ProfileResolutionError::AmbiguousProfileSelection { .. })
    ));

    let flat = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("Enabled", "yes"), ("enabled", "no")]),
    );
    assert!(matches!(
        ResolvedProfile::resolve(ChainRole::Link, &flat, None),
        Err(ProfileResolutionError::ConflictingAssignment { .. })
    ));

    let wrong_role = RawOverlay::flat(ChainRole::LocalReceive, "radio.conf", values(&[]));
    assert!(matches!(
        ResolvedProfile::resolve(ChainRole::Link, &wrong_role, None),
        Err(ProfileResolutionError::RoleMismatch { .. })
    ));

    let scoped = RawOverlay::scoped(ChainRole::Link, "radio.conf", "link one", values(&[]));
    assert!(matches!(
        ResolvedProfile::resolve(ChainRole::Link, &scoped, None),
        Err(ProfileResolutionError::LayerMismatch { .. })
    ));

    let correct_flat = RawOverlay::flat(ChainRole::Link, "radio.conf", values(&[]));
    assert!(matches!(
        ResolvedProfile::resolve(ChainRole::Link, &correct_flat, Some(&correct_flat)),
        Err(ProfileResolutionError::LayerMismatch { .. })
    ));

    let duplicate = RawOverlay::flat(
        ChainRole::Link,
        "radio.conf",
        values(&[("Enabled", "yes"), ("enabled", "yes")]),
    );
    assert!(ResolvedProfile::resolve(ChainRole::Link, &duplicate, None).is_ok());
}

#[test]
fn resolution_errors_have_contextual_messages_and_error_sources() {
    use std::error::Error as _;

    let errors = [
        ProfileResolutionError::RoleMismatch {
            expected: ChainRole::Link,
            actual: ChainRole::LocalReceive,
            section: "local".to_owned(),
        },
        ProfileResolutionError::LayerMismatch {
            expected: "flat",
            actual: "scoped",
            section: "link one".to_owned(),
        },
        ProfileResolutionError::ConflictingAssignment {
            source: "radio.conf".to_owned(),
            section: "link".to_owned(),
            name: "enabled".to_owned(),
            first: "yes".to_owned(),
            second: "no".to_owned(),
        },
        ProfileResolutionError::AmbiguousProfileSelection {
            channel: "usb".to_owned(),
            selector: "link_profile".to_owned(),
        },
        ConfigError::MissingChannel("usb".to_owned()).into(),
        ProcessingConfigError::StageNotAllowed(ChainRole::Link).into(),
    ];
    for error in &errors {
        assert!(!error.to_string().is_empty());
    }
    assert!(errors[0].source().is_none());
    assert!(errors[4].source().is_some());
    assert!(errors[5].source().is_some());
}

#[test]
fn all_source_specific_options_apply_only_to_their_own_schema() {
    let local = RawOverlay::flat(
        ChainRole::LocalReceive,
        "radio.conf",
        values(&[
            ("rnnoise_enabled", "yes"),
            ("ctcss_filter_mode", "notch"),
            ("lookahead_limiter_enabled", "yes"),
            ("lookahead_limiter_ceiling_dbfs", "-2"),
            ("lookahead_limiter_lookahead_ms", "4"),
            ("lookahead_limiter_attack_ms", "2"),
            ("lookahead_limiter_release_ms", "80"),
            ("post_limiter_bandpass_enabled", "yes"),
            ("post_limiter_bandpass_highpass_hz", "100"),
            ("post_limiter_bandpass_lowpass_hz", "5000"),
        ]),
    );
    let local = ResolvedProfile::resolve(ChainRole::LocalReceive, &local, None).unwrap();
    assert!(local.chain().rnnoise_enabled);
    assert_eq!(local.chain().receive.pl_filter, PlFilter::DecodedToneNotch);
    assert!(!local.chain().transmit_tail.limiter_enabled);
    assert_eq!(local.warnings().len(), 8);

    let voice = RawOverlay::flat(
        ChainRole::VoiceTelemetry,
        "radio.conf",
        values(&[
            ("lookahead_limiter_enabled", "yes"),
            ("post_limiter_bandpass_lowpass_hz", "5000"),
            ("rnnoise_enabled", "yes"),
            ("receive_bandpass_enabled", "yes"),
            ("receive_bandpass_highpass_hz", "100"),
            ("receive_bandpass_lowpass_hz", "5000"),
            ("ctcss_filter_mode", "notch"),
            ("ctcss_notch_width_hz", "4"),
            ("ctcss_highpass_hz", "200"),
        ]),
    );
    let voice = ResolvedProfile::resolve(ChainRole::VoiceTelemetry, &voice, None).unwrap();
    assert!(voice.chain().transmit_tail.limiter_enabled);
    assert_eq!(voice.chain().transmit_tail.bandpass_lowpass_hz, 5_000.0);
    assert_eq!(voice.chain().receive.pl_filter, PlFilter::Disabled);
    assert_eq!(voice.warnings().len(), 7);
}
