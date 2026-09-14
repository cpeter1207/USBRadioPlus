use super::*;

#[test]
fn shipped_profiles_match_each_source_role() {
    let local = ProcessingChain::shipped(ChainRole::LocalReceive);
    let link = ProcessingChain::shipped(ChainRole::Link);
    let transmit = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    assert!(local.receive.bandpass_enabled);
    assert_eq!(local.receive.pl_filter, PlFilter::HighPass);
    assert_eq!(local.output_gain_db, -6.2);
    assert!(!link.receive.bandpass_enabled);
    assert_eq!(link.receive.pl_filter, PlFilter::Disabled);
    assert_eq!(transmit.input_gain_db, 6.0);
    assert_eq!(transmit.output_gain_db, 0.0);
    assert!(local.validate().is_ok());
    assert!(link.validate().is_ok());
    assert!(transmit.validate().is_ok());
}

#[test]
fn source_specific_stages_are_rejected_elsewhere() {
    let mut link = ProcessingChain::shipped(ChainRole::Link);
    link.rnnoise_enabled = true;
    assert_eq!(
        link.validate(),
        Err(ProcessingConfigError::StageNotAllowed(ChainRole::Link))
    );
    let mut local = ProcessingChain::shipped(ChainRole::LocalReceive);
    local.transmit_tail.limiter_enabled = true;
    assert_eq!(
        local.validate(),
        Err(ProcessingConfigError::StageNotAllowed(
            ChainRole::LocalReceive
        ))
    );

    let mut link_bandpass = ProcessingChain::shipped(ChainRole::Link);
    link_bandpass.receive.bandpass_enabled = true;
    assert_eq!(
        link_bandpass.validate(),
        Err(ProcessingConfigError::StageNotAllowed(ChainRole::Link))
    );

    let mut link_pl = ProcessingChain::shipped(ChainRole::Link);
    link_pl.receive.pl_filter = PlFilter::HighPass;
    assert_eq!(
        link_pl.validate(),
        Err(ProcessingConfigError::StageNotAllowed(ChainRole::Link))
    );

    let mut local_bandpass = ProcessingChain::shipped(ChainRole::LocalReceive);
    local_bandpass.transmit_tail.bandpass_enabled = true;
    assert_eq!(
        local_bandpass.validate(),
        Err(ProcessingConfigError::StageNotAllowed(
            ChainRole::LocalReceive
        ))
    );
}

#[test]
fn stage_order_must_contain_every_enabled_stage() {
    let mut chain = ProcessingChain::shipped(ChainRole::LocalReceive);
    chain.stage_order = StageOrder::parse("equalizer", &[]).unwrap();
    chain.agc.enabled = true;
    assert_eq!(
        chain.validate(),
        Err(ProcessingConfigError::StageOrder(
            StageOrderError::MissingEnabledStage(ProcessingStage::Agc)
        ))
    );
}

#[test]
fn every_numeric_family_rejects_invalid_and_non_finite_values() {
    let cases: &[fn(&mut ProcessingChain)] = &[
        |c| c.input_gain_db = f64::NAN,
        |c| c.receive.bandpass_lowpass_hz = 20.0,
        |c| c.equalizer.low_slope = 0.0,
        |c| c.deesser.ratio = 21.0,
        |c| c.agc.activity_threshold_dbfs = c.agc.target_dbfs,
        |c| c.expander.sidechain_lowpass_hz = c.expander.sidechain_highpass_hz,
        |c| c.compressor.high_crossover_hz = c.compressor.low_crossover_hz,
        |c| c.limiter.high.attack_ms = 101.0,
        |c| c.transmit_tail.ceiling_dbfs = 0.0,
    ];
    for mutate in cases {
        let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
        mutate(&mut chain);
        assert!(chain.validate().is_err());
    }
}

#[test]
fn disabled_agc_sidechain_edges_are_valid_but_inverted_edges_are_not() {
    let mut chain = ProcessingChain::shipped(ChainRole::LocalReceive);
    chain.agc.sidechain_highpass_hz = 0.0;
    chain.agc.sidechain_lowpass_hz = 0.0;
    assert!(chain.validate().is_ok());
    chain.agc.sidechain_highpass_hz = 1_000.0;
    chain.agc.sidechain_lowpass_hz = 500.0;
    assert!(matches!(
        chain.validate(),
        Err(ProcessingConfigError::Ordering { .. })
    ));
}

#[test]
fn validation_errors_have_stable_contextual_messages() {
    assert_eq!(
        ProcessingConfigError::Range {
            field: "gain",
            minimum: -1.0,
            maximum: 1.0,
        }
        .to_string(),
        "gain must be finite and between -1 and 1"
    );
    assert_eq!(
        ProcessingConfigError::Ordering {
            upper: "upper",
            lower: "lower",
        }
        .to_string(),
        "upper must exceed lower"
    );
    assert_eq!(
        ProcessingConfigError::StageNotAllowed(ChainRole::Link).to_string(),
        "fixed stage is not valid for Link"
    );
    let order = ProcessingConfigError::from(StageOrderError::TooLong);
    assert_eq!(order.to_string(), StageOrderError::TooLong.to_string());
}
