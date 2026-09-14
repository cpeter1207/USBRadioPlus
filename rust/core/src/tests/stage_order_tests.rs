use super::*;

#[test]
fn all_optional_stages_parse_case_insensitively_in_requested_order() {
    let enabled = [
        ProcessingStage::Equalizer,
        ProcessingStage::Expander,
        ProcessingStage::Agc,
        ProcessingStage::Deesser,
        ProcessingStage::Compressor,
        ProcessingStage::Limiter,
    ];
    let order = StageOrder::parse(
        " equalizer, EXPANDER, agc, deesser, compressor, limiter ",
        &enabled,
    )
    .unwrap();
    assert_eq!(order.stages(), &enabled);
}

#[test]
fn unknown_empty_fixed_duplicate_and_missing_stages_are_rejected() {
    assert_eq!(
        StageOrder::parse("agc,,limiter", &[]),
        Err(StageOrderError::UnknownStage(String::new()))
    );
    assert_eq!(
        StageOrder::parse("agc,bandpass", &[]),
        Err(StageOrderError::UnknownStage("bandpass".to_owned()))
    );
    assert_eq!(
        StageOrder::parse("agc,AGC", &[]),
        Err(StageOrderError::DuplicateStage(ProcessingStage::Agc))
    );
    assert_eq!(
        StageOrder::parse("agc", &[ProcessingStage::Compressor]),
        Err(StageOrderError::MissingEnabledStage(
            ProcessingStage::Compressor
        ))
    );
    assert_eq!(
        StageOrder::parse(&"x".repeat(128), &[]),
        Err(StageOrderError::TooLong)
    );
}

#[test]
fn stage_names_errors_and_unknown_tokens_are_stable() {
    for (text, stage) in [
        ("expander", ProcessingStage::Expander),
        ("agc", ProcessingStage::Agc),
        ("compressor", ProcessingStage::Compressor),
        ("limiter", ProcessingStage::Limiter),
        ("equalizer", ProcessingStage::Equalizer),
        ("deesser", ProcessingStage::Deesser),
    ] {
        assert_eq!(ProcessingStage::parse(text), Some(stage));
        assert_eq!(stage.name(), text);
    }
    assert_eq!(ProcessingStage::parse(""), None);
    assert_eq!(ProcessingStage::parse("highpass"), None);
    assert_eq!(
        StageOrder::standard().stages(),
        &[
            ProcessingStage::Equalizer,
            ProcessingStage::Expander,
            ProcessingStage::Agc,
            ProcessingStage::Deesser,
            ProcessingStage::Compressor,
            ProcessingStage::Limiter,
        ]
    );
    assert_eq!(
        StageOrder::parse("agc", &[])
            .unwrap()
            .require_enabled(&[ProcessingStage::Limiter]),
        Err(StageOrderError::MissingEnabledStage(
            ProcessingStage::Limiter
        ))
    );
    for error in [
        StageOrderError::TooLong,
        StageOrderError::UnknownStage(String::new()),
        StageOrderError::DuplicateStage(ProcessingStage::Agc),
        StageOrderError::MissingEnabledStage(ProcessingStage::Limiter),
    ] {
        assert!(!error.to_string().is_empty());
    }
}
