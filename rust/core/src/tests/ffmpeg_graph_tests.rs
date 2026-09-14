use super::*;
use crate::{ChainRole, StageOrder};

fn factory() -> GraphDescriptionFactory {
    GraphDescriptionFactory::new("/usr/lib/usbradioplus/agc's:effect.so").unwrap()
}

#[test]
fn receive_graphs_cover_bypass_emphasis_bandpass_highpass_and_notch() {
    assert_eq!(
        factory().receive_deemphasis(false, 300.0).unwrap(),
        "[in]anull[out]"
    );
    assert!(
        factory()
            .receive_deemphasis(true, 300.0)
            .unwrap()
            .contains("biquad=b0=")
    );

    let local = ProcessingChain::shipped(ChainRole::LocalReceive);
    let filter = factory().receive_filter(&local).unwrap();
    assert!(filter.contains("split=20.000000000:order=20th"));
    assert!(filter.contains("split=300.000000000:order=20th"));
    let notch = factory().decoded_tone_notch(100.0, 5.0).unwrap();
    assert_eq!(notch.matches("bandreject=").count(), 4);
    assert!(notch.ends_with("[out];"));
    assert_eq!(
        factory().decoded_tone_notch(301.0, 5.0),
        Err(GraphDescriptionError::InvalidNotchFrequency)
    );
    assert_eq!(
        factory().decoded_tone_notch(f64::NAN, 5.0),
        Err(GraphDescriptionError::InvalidNotchFrequency)
    );
}

#[test]
fn dcs_filters_preserve_calibrated_normal_and_turnoff_levels() {
    assert_eq!(
        factory().dcs_filter(false),
        "[in]acrossover=split=250:order=20th:precision=float[low][high];\
         [high]anullsink;[low]volume=-3.42dB,aformat=sample_fmts=flt[out]"
    );
    assert_eq!(
        factory().dcs_filter(true),
        "[in]acrossover=split=250:order=20th:precision=float[low][high];\
         [high]anullsink;[low]aformat=sample_fmts=flt[out]"
    );
}

#[test]
fn dynamics_graph_uses_only_enabled_stages_in_requested_order() {
    let mut chain = ProcessingChain::shipped(ChainRole::Link);
    chain.equalizer.enabled = false;
    chain.expander.enabled = true;
    chain.agc.enabled = true;
    chain.stage_order = StageOrder::parse(
        "agc,expander",
        &[ProcessingStage::Agc, ProcessingStage::Expander],
    )
    .unwrap();
    let graph = factory().dynamics(&chain).unwrap();
    assert!(graph.find("ladspa=").unwrap() < graph.find("sidechaingate=").unwrap());
    assert!(graph.contains("file='/usr/lib/usbradioplus/agc\\'s\\:effect.so'"));
    assert!(graph.ends_with("[out]"));
}

#[test]
fn transmitter_graph_contains_preemphasis_gain_limiter_and_final_bandpass() {
    let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    chain.equalizer.enabled = false;
    chain.transmit_tail.limiter_enabled = true;
    chain.transmit_tail.bandpass_enabled = true;
    chain.transmit_tail.bandpass_highpass_hz = 0.0;
    chain.transmit_tail.bandpass_lowpass_hz = 5_000.0;
    let graph = factory().transmitter(&chain, true, 300.0).unwrap();
    assert!(graph.contains("volume=1.995262314969"));
    assert!(graph.contains("biquad=b0="));
    assert!(graph.contains("alimiter=limit="));
    assert!(graph.contains("split=5000.000000000:order=20th[out]"));
}

#[test]
fn full_band_and_three_band_dynamics_are_both_generated() {
    let mut chain = ProcessingChain::shipped(ChainRole::Link);
    chain.equalizer.enabled = false;
    chain.compressor.enabled = true;
    chain.limiter.enabled = true;
    chain.stage_order = StageOrder::parse(
        "compressor,limiter",
        &[ProcessingStage::Compressor, ProcessingStage::Limiter],
    )
    .unwrap();
    let three = factory().dynamics(&chain).unwrap();
    assert_eq!(three.matches("acrossover=split=").count(), 2);
    chain.compressor.layout = BandLayout::FullBand;
    chain.limiter.layout = BandLayout::FullBand;
    let full = factory().dynamics(&chain).unwrap();
    assert!(!full.contains("acrossover=split="));
    assert!(full.contains("sidechaincompress="));
    assert!(full.contains("detection=peak"));
}

#[test]
fn invalid_factory_and_crossover_are_rejected() {
    assert_eq!(
        GraphDescriptionFactory::new("bad\npath"),
        Err(GraphDescriptionError::InvalidAgcPluginPath)
    );
    let mut chain = ProcessingChain::shipped(ChainRole::Link);
    chain.compressor.enabled = true;
    chain.compressor.high_crossover_hz = 24_000.0;
    assert_eq!(
        factory().dynamics(&chain),
        Err(GraphDescriptionError::CrossoverAboveNyquist)
    );

    chain.compressor.enabled = false;
    chain.limiter.enabled = true;
    chain.limiter.high_crossover_hz = 24_000.0;
    assert_eq!(
        factory().dynamics(&chain),
        Err(GraphDescriptionError::CrossoverAboveNyquist)
    );
}

#[test]
fn graph_errors_have_stable_messages_and_format_conversion() {
    let cases = [
        (
            GraphDescriptionError::CrossoverAboveNyquist,
            "dynamics crossover must be below Nyquist",
        ),
        (
            GraphDescriptionError::InvalidNotchFrequency,
            "decoded CTCSS notch must be between 50 and 300 Hz",
        ),
        (
            GraphDescriptionError::InvalidAgcPluginPath,
            "AGC plug-in path contains an unsupported line delimiter",
        ),
        (
            GraphDescriptionError::Formatting,
            "unable to format FFmpeg graph",
        ),
    ];
    for (error, message) in cases {
        assert_eq!(error.to_string(), message);
    }
    assert_eq!(
        GraphDescriptionError::from(std::fmt::Error),
        GraphDescriptionError::Formatting
    );
}

#[test]
fn bypass_and_one_sided_brickwall_paths_are_explicit() {
    let mut local = ProcessingChain::shipped(ChainRole::LocalReceive);
    local.receive.bandpass_enabled = false;
    local.receive.pl_filter = PlFilter::Disabled;
    assert_eq!(factory().receive_filter(&local).unwrap(), "[in]anull[out]");

    local.receive.bandpass_enabled = true;
    local.receive.bandpass_highpass_hz = 0.0;
    local.receive.bandpass_lowpass_hz = 0.0;
    assert!(
        factory()
            .receive_filter(&local)
            .unwrap()
            .starts_with("[in]anull[rxbandpass];")
    );

    local.receive.bandpass_highpass_hz = 150.0;
    assert!(
        factory()
            .receive_filter(&local)
            .unwrap()
            .contains("split=150.000000000:order=20th[rxbplo][rxbandpass]")
    );
}

#[test]
fn local_bypass_omits_deemphasis_owned_gain_and_disabled_dynamics() {
    let mut local = ProcessingChain::shipped(ChainRole::LocalReceive);
    local.enabled = false;
    local.output_gain_db = 0.0;
    let graph = factory().dynamics(&local).unwrap();
    assert_eq!(graph, "[in]anull[out]");
}

#[test]
fn transmitter_tail_stages_are_independently_optional() {
    let mut transmit = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    transmit.equalizer.enabled = false;
    transmit.transmit_tail.limiter_enabled = false;
    transmit.transmit_tail.bandpass_enabled = false;
    let graph = factory().transmitter(&transmit, false, 300.0).unwrap();
    assert!(graph.ends_with("anull[out]"));
    assert!(!graph.contains("alimiter="));
    assert!(!graph.contains("biquad="));
}

#[test]
fn equalizer_deesser_zero_sidechains_and_makeup_are_generated() {
    let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    chain.enabled = true;
    chain.equalizer.enabled = true;
    chain.deesser.enabled = true;
    chain.expander.enabled = true;
    chain.expander.sidechain_highpass_hz = 0.0;
    chain.expander.sidechain_lowpass_hz = 0.0;
    chain.agc.enabled = true;
    chain.agc.sidechain_highpass_hz = 0.0;
    chain.agc.sidechain_lowpass_hz = 0.0;
    chain.compressor.enabled = true;
    chain.compressor.low.makeup_gain_db = 1.0;
    chain.stage_order = StageOrder::parse(
        "equalizer,deesser,expander,agc,compressor",
        &[
            ProcessingStage::Equalizer,
            ProcessingStage::Deesser,
            ProcessingStage::Expander,
            ProcessingStage::Agc,
            ProcessingStage::Compressor,
        ],
    )
    .unwrap();
    let graph = factory().dynamics(&chain).unwrap();
    assert!(graph.contains("bass=g="));
    assert!(graph.contains("adynamicequalizer="));
    assert!(graph.contains("anull,anull"));
    assert!(graph.contains(",volume=1.122018454302:precision=float"));
}
