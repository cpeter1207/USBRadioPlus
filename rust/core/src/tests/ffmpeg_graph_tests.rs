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
    assert!(graph.contains("anull,anull"));
    assert!(graph.ends_with("[out]"));
}

#[test]
fn transmitter_graph_places_bandpass_and_all_gain_before_final_limiter() {
    let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    chain.equalizer.enabled = false;
    chain.output_gain_db = 3.0;
    chain.transmit_tail.limiter_enabled = true;
    chain.transmit_tail.bandpass_enabled = true;
    chain.transmit_tail.bandpass_highpass_hz = 0.0;
    chain.transmit_tail.bandpass_lowpass_hz = 5_000.0;
    let graph = factory().transmitter(&chain, true, 300.0).unwrap();
    assert!(graph.contains("volume=1.995262314969"));
    assert!(graph.contains("biquad=b0="));
    assert!(graph.contains("alimiter=limit="));
    assert!(graph.contains("split=5000.000000000:order=20th"));
    assert!(graph.contains("aresample=192000"));
    assert!(graph.contains("aresample=48000"));
    assert!(graph.find("biquad=b0=").unwrap() < graph.rfind("volume=").unwrap());
    assert!(graph.rfind("volume=").unwrap() < graph.find("split=5000.").unwrap());
    assert!(graph.find("split=5000.").unwrap() < graph.find("aresample=192000").unwrap());
    assert!(graph.find("aresample=192000").unwrap() < graph.find("alimiter=").unwrap());
    assert!(graph.find("alimiter=").unwrap() < graph.find("aresample=48000").unwrap());
}

#[test]
fn final_limiter_oversampling_is_selectable_and_disabled_with_the_limiter() {
    let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
    chain.transmit_tail.limiter_enabled = true;
    for (factor, rate) in [(1, None), (2, Some("96000")), (4, Some("192000"))] {
        chain.transmit_tail.oversample = factor;
        let graph = factory().transmitter(&chain, false, 300.0).unwrap();
        assert_eq!(graph.matches("alimiter=").count(), 1);
        assert!(graph.contains(":level=0:latency=0"));
        if let Some(rate) = rate {
            assert!(graph.contains(&format!("aresample={rate},alimiter=")));
            assert!(graph.contains(",aresample=48000["));
        } else {
            assert!(!graph.contains("aresample="));
        }
    }
    chain.transmit_tail.limiter_enabled = false;
    let graph = factory().transmitter(&chain, false, 300.0).unwrap();
    assert!(!graph.contains("alimiter="));
    assert!(!graph.contains("aresample="));
}

fn ffmpeg_true_peak(source: &str, graph: &str) -> f64 {
    let graph = format!(
        "{};[meter]aformat=sample_fmts=flt,ebur128=peak=true:metadata=1:framelog=verbose,ametadata=print:key=lavfi.r128.true_peak:file=-[out]",
        graph.replace("[in]", "[0:a]").replace("[out]", "[meter]")
    );
    let output = std::process::Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-nostdin",
            "-v",
            "error",
            "-filter_complex_threads",
            "1",
            "-f",
            "lavfi",
            "-i",
            source,
            "-filter_complex",
            &graph,
            "-map",
            "[out]",
            "-f",
            "null",
            "-",
        ])
        .output()
        .expect("FFmpeg CLI is required for final-limiter output tests");
    assert!(
        output.status.success(),
        "FFmpeg failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut max_peak: Option<f64> = None;
    for line in stdout.lines() {
        if let Some(peak) = line.strip_prefix("lavfi.r128.true_peak=") {
            let value = peak.parse::<f64>().expect("invalid FFmpeg true peak");
            max_peak = Some(max_peak.map_or(value, |maximum| maximum.max(value)));
        }
    }
    max_peak.expect("FFmpeg did not report true peak")
}

#[test]
fn final_limiter_bounds_true_peak_after_return_to_native_rate() {
    let sources = [
        "aevalsrc=1.41*sin(2*PI*12000*t+PI/4):s=48000:d=1,afade=t=in:d=0.01,afade=t=out:st=0.99:d=0.01",
        "aevalsrc='if(gte(sin(2*PI*1000*t),0),1.5,-1.5)':s=48000:d=1",
        "anoisesrc=color=white:amplitude=1:sample_rate=48000:duration=1:seed=1,volume=3",
    ];
    std::thread::scope(|scope| {
        let mut tasks = Vec::new();
        for source in sources {
            for bandpass_enabled in [false, true] {
                let mut chain = ProcessingChain::shipped(ChainRole::VoiceTelemetry);
                chain.enabled = false;
                chain.input_gain_db = 0.0;
                chain.output_gain_db = 0.0;
                chain.transmit_tail.limiter_enabled = true;
                chain.transmit_tail.ceiling_dbfs = -2.0;
                chain.transmit_tail.oversample = 4;
                chain.transmit_tail.bandpass_enabled = bandpass_enabled;
                chain.transmit_tail.bandpass_lowpass_hz = 5_000.0;
                let graph = factory().transmitter(&chain, false, 300.0).unwrap();
                tasks.push(scope.spawn(move || {
                    let peak = ffmpeg_true_peak(source, &graph);
                    assert!(
                        peak <= db_to_linear(-1.0),
                        "source={source}, bandpass={bandpass_enabled}, true peak={peak}"
                    );
                }));
            }
        }
        for task in tasks {
            task.join().unwrap();
        }
    });
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
