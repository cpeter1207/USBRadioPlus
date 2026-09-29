//! FFmpeg graph descriptions derived from validated processing objects.

use std::f64::consts::PI;
use std::fmt::{self, Write};

use crate::{BandLayout, DynamicsBand, PlFilter, ProcessingChain, ProcessingStage};

/// Failure while constructing one FFmpeg graph description.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum GraphDescriptionError {
    /// The selected crossover is not below the fixed 48 kHz Nyquist frequency.
    CrossoverAboveNyquist,
    /// A decoded-tone notch frequency is outside the supported PL band.
    InvalidNotchFrequency,
    /// The configured LADSPA path cannot be represented safely in a graph.
    InvalidAgcPluginPath,
    /// A formatting operation unexpectedly failed.
    Formatting,
}

impl fmt::Display for GraphDescriptionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::CrossoverAboveNyquist => {
                formatter.write_str("dynamics crossover must be below Nyquist")
            }
            Self::InvalidNotchFrequency => {
                formatter.write_str("decoded CTCSS notch must be between 50 and 300 Hz")
            }
            Self::InvalidAgcPluginPath => {
                formatter.write_str("AGC plug-in path contains an unsupported line delimiter")
            }
            Self::Formatting => formatter.write_str("unable to format FFmpeg graph"),
        }
    }
}

impl std::error::Error for GraphDescriptionError {}

impl From<fmt::Error> for GraphDescriptionError {
    fn from(_: fmt::Error) -> Self {
        Self::Formatting
    }
}

/// Control-plane factory for USBRadioPlus FFmpeg graph descriptions.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GraphDescriptionFactory {
    agc_plugin_path: String,
}

impl GraphDescriptionFactory {
    /// Construct a factory using the installed Rust AGC LADSPA effect.
    pub fn new(agc_plugin_path: impl Into<String>) -> Result<Self, GraphDescriptionError> {
        let agc_plugin_path = agc_plugin_path.into();
        if agc_plugin_path.contains(['\r', '\n']) {
            return Err(GraphDescriptionError::InvalidAgcPluginPath);
        }
        Ok(Self { agc_plugin_path })
    }

    /// Build the fixed receiver de-emphasis graph.
    pub fn receive_deemphasis(
        &self,
        enabled: bool,
        corner_hz: f64,
    ) -> Result<String, GraphDescriptionError> {
        if enabled {
            emphasis_graph(false, corner_hz)
        } else {
            Ok("[in]anull[out]".to_owned())
        }
    }

    /// Build the fixed local receive band-pass and non-notch PL filter.
    ///
    /// Decoded-tone notch mode uses [`Self::decoded_tone_notch`] after this
    /// graph so its tone can change without rebuilding the base receive graph.
    pub fn receive_filter(&self, chain: &ProcessingChain) -> Result<String, GraphDescriptionError> {
        let mut graph = String::new();
        let mut current = "in".to_owned();
        if chain.receive.bandpass_enabled {
            append_brickwall_bandpass(
                &mut graph,
                &current,
                "rxbandpass",
                "rxbp",
                chain.receive.bandpass_highpass_hz,
                chain.receive.bandpass_lowpass_hz,
            )?;
            current = "rxbandpass".to_owned();
        }
        if chain.receive.pl_filter == PlFilter::HighPass {
            write!(
                graph,
                "[{current}]acrossover=split={:.9}:order=20th[ctlow][out];[ctlow]anullsink;",
                chain.receive.highpass_hz
            )?;
        } else {
            write!(graph, "[{current}]anull[out]")?;
        }
        Ok(graph)
    }

    /// Live samples needed to fill the receiver low-pass FIR and partition guard.
    pub fn receive_filter_history_frames(chain: &ProcessingChain) -> usize {
        if chain.receive.bandpass_enabled {
            lowpass_history_frames(chain.receive.bandpass_lowpass_hz)
        } else {
            0
        }
    }

    /// Live samples needed to fill the transmitter low-pass FIR/guard, excluding limiting.
    pub fn transmit_filter_history_frames(chain: &ProcessingChain) -> usize {
        if chain.transmit_tail.bandpass_enabled {
            lowpass_history_frames(chain.transmit_tail.bandpass_lowpass_hz)
        } else {
            0
        }
    }

    /// Build one fixed decoded-CTCSS notch graph.
    pub fn decoded_tone_notch(
        &self,
        frequency_hz: f64,
        width_hz: f64,
    ) -> Result<String, GraphDescriptionError> {
        if !frequency_hz.is_finite() || !(50.0..=300.0).contains(&frequency_hz) {
            return Err(GraphDescriptionError::InvalidNotchFrequency);
        }
        let mut graph = String::new();
        let mut current = "in".to_owned();
        for section in 0..4 {
            let next = if section == 3 {
                "out".to_owned()
            } else {
                format!("ctn{section}")
            };
            write!(
                graph,
                "[{current}]bandreject=f={frequency_hz:.9}:t=h:w={width_hz:.9}:r=f32[{next}];"
            )?;
            current = next;
        }
        Ok(graph)
    }

    /// Build the fixed shaping graph for generated DCS signaling.
    ///
    /// Normal NRZ DCS receives the established 3.42 dB calibration
    /// compensation. The sine-wave turn-off code does not require it.
    pub fn dcs_filter(&self, turnoff: bool) -> String {
        let compensation = if turnoff { "" } else { "volume=-3.42dB," };
        format!(
            "[in]acrossover=split=250:order=20th:precision=float[low][high];\
             [high]anullsink;[low]{compensation}aformat=sample_fmts=flt[out]"
        )
    }

    /// Build the optional local/link dynamics graph without fixed RX or TX stages.
    pub fn dynamics(&self, chain: &ProcessingChain) -> Result<String, GraphDescriptionError> {
        self.ordered_chain(chain, false, false, 0.0)
    }

    /// Build the voice/telemetry graph and fixed transmitter tail.
    pub fn transmitter(
        &self,
        chain: &ProcessingChain,
        preemphasis_enabled: bool,
        preemphasis_corner_hz: f64,
    ) -> Result<String, GraphDescriptionError> {
        self.ordered_chain(chain, true, preemphasis_enabled, preemphasis_corner_hz)
    }

    fn ordered_chain(
        &self,
        chain: &ProcessingChain,
        include_transmit_tail: bool,
        preemphasis_enabled: bool,
        preemphasis_corner_hz: f64,
    ) -> Result<String, GraphDescriptionError> {
        const NYQUIST_HZ: f64 = 24_000.0;
        if (chain.compressor.enabled
            && chain.compressor.layout == BandLayout::ThreeBand
            && chain.compressor.high_crossover_hz >= NYQUIST_HZ)
            || (chain.limiter.enabled
                && chain.limiter.layout == BandLayout::ThreeBand
                && chain.limiter.high_crossover_hz >= NYQUIST_HZ)
        {
            return Err(GraphDescriptionError::CrossoverAboveNyquist);
        }

        let mut graph = String::new();
        let mut current = "in".to_owned();
        let mut serial = 0_u32;
        let input_gain_db = if include_transmit_tail || chain.role != crate::ChainRole::LocalReceive
        {
            chain.input_gain_db
        } else {
            0.0
        };
        if input_gain_db != 0.0 {
            let next = next_label(&mut serial);
            write!(
                graph,
                "[{current}]volume={:.12}:precision=float[{next}];",
                db_to_linear(input_gain_db)
            )?;
            current = next;
        }

        if chain.enabled {
            for stage in chain.stage_order.stages() {
                if !stage_enabled(chain, *stage) {
                    continue;
                }
                let next = next_label(&mut serial);
                self.append_stage(&mut graph, chain, *stage, &current, &next, serial)?;
                current = next;
            }
        }

        if include_transmit_tail && preemphasis_enabled {
            let next = next_label(&mut serial);
            append_emphasis(&mut graph, &current, &next, true, preemphasis_corner_hz)?;
            current = next;
        }
        if chain.output_gain_db != 0.0 {
            let next = next_label(&mut serial);
            write!(
                graph,
                "[{current}]volume={:.12}:precision=float[{next}];",
                db_to_linear(chain.output_gain_db)
            )?;
            current = next;
        }
        if include_transmit_tail && chain.transmit_tail.limiter_enabled {
            let next = next_label(&mut serial);
            write!(
                graph,
                "[{current}]alimiter=limit={:.12}:attack={:.9}:release={:.9}:level=0:latency=0[{next}];",
                db_to_linear(chain.transmit_tail.ceiling_dbfs),
                chain.transmit_tail.lookahead_ms,
                chain.transmit_tail.release_ms,
            )?;
            current = next;
        }
        if include_transmit_tail && chain.transmit_tail.bandpass_enabled {
            append_brickwall_bandpass(
                &mut graph,
                &current,
                "out",
                "cln",
                chain.transmit_tail.bandpass_highpass_hz,
                chain.transmit_tail.bandpass_lowpass_hz,
            )?;
        } else {
            write!(graph, "[{current}]anull[out]")?;
        }
        Ok(graph)
    }

    fn append_stage(
        &self,
        graph: &mut String,
        chain: &ProcessingChain,
        stage: ProcessingStage,
        input: &str,
        output: &str,
        serial: u32,
    ) -> Result<(), GraphDescriptionError> {
        let prefix = format!("d{serial}");
        match stage {
            ProcessingStage::Equalizer => write!(
                graph,
                "[{input}]bass=g={:.9}:f={:.9}:t=s:w={:.9}:r=f32,equalizer=f={:.9}:t=o:w={:.9}:g={:.9}:r=f32,treble=g={:.9}:f={:.9}:t=s:w={:.9}:r=f32[{output}];",
                chain.equalizer.low_gain_db,
                chain.equalizer.low_frequency_hz,
                chain.equalizer.low_slope,
                chain.equalizer.mid_frequency_hz,
                chain.equalizer.mid_width_octaves,
                chain.equalizer.mid_gain_db,
                chain.equalizer.high_gain_db,
                chain.equalizer.high_frequency_hz,
                chain.equalizer.high_slope,
            )?,
            ProcessingStage::Deesser => {
                let octave_ratio = 2.0_f64.powf(chain.deesser.width_octaves);
                let q = octave_ratio.sqrt() / (octave_ratio - 1.0);
                write!(
                    graph,
                    "[{input}]adynamicequalizer=threshold={:.12}:dfrequency={:.9}:dqfactor={q:.12}:tfrequency={:.9}:tqfactor={q:.12}:attack={:.9}:release={:.9}:ratio={:.9}:range={:.12}:mode=cutabove:dftype=bandpass:tftype=bell:precision=float[{output}];",
                    db_to_linear(chain.deesser.threshold_dbfs),
                    chain.deesser.frequency_hz,
                    chain.deesser.frequency_hz,
                    chain.deesser.attack_ms,
                    chain.deesser.release_ms,
                    chain.deesser.ratio,
                    chain.deesser.max_reduction_db / 2.0,
                )?;
            }
            ProcessingStage::Agc => {
                let plugin = escape_filter_value(&self.agc_plugin_path);
                let options = format!(
                    "inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,ladspa=file='{plugin}':plugin=usbradioplus_agc:controls=c0={:.12}|c1={:.12}|c2={:.12}|c3={:.12}|c4={:.12}|c5={:.12}|c6={:.12}|c7={:.12}|c8={:.12}|c9={:.12},aformat=channel_layouts=mono",
                    chain.agc.target_dbfs,
                    chain.agc.rms_averaging_ms,
                    chain.agc.gain_increase_db_per_second,
                    chain.agc.gain_decrease_db_per_second,
                    chain.agc.max_gain_db,
                    chain.agc.max_attenuation_db,
                    chain.agc.activity_threshold_dbfs,
                    chain.agc.activity_hysteresis_db,
                    chain.agc.hold_ms,
                    chain.agc.deadband_db,
                );
                append_sidechain(
                    graph,
                    GraphEdge::new(input, output, &prefix),
                    "join",
                    chain.agc.sidechain_highpass_hz,
                    chain.agc.sidechain_lowpass_hz,
                    &options,
                )?;
            }
            ProcessingStage::Expander => {
                let options = format!(
                    "threshold={:.12}:ratio={:.9}:range={:.12}:attack={:.9}:release={:.9}:knee=2.828427:detection=rms",
                    db_to_linear(chain.expander.threshold_dbfs),
                    chain.expander.ratio,
                    db_to_linear(-chain.expander.max_attenuation_db.abs()),
                    chain.expander.attack_ms,
                    chain.expander.release_ms,
                );
                append_sidechain(
                    graph,
                    GraphEdge::new(input, output, &prefix),
                    "sidechaingate",
                    chain.expander.sidechain_highpass_hz,
                    chain.expander.sidechain_lowpass_hz,
                    &options,
                )?;
            }
            ProcessingStage::Compressor => {
                if chain.compressor.layout == BandLayout::ThreeBand {
                    append_three_band(
                        graph,
                        GraphEdge::new(input, output, &prefix),
                        (
                            chain.compressor.low_crossover_hz,
                            chain.compressor.high_crossover_hz,
                        ),
                        [
                            &chain.compressor.low,
                            &chain.compressor.mid,
                            &chain.compressor.high,
                        ],
                        "rms",
                    )?;
                } else {
                    let band = &chain.compressor.full;
                    let options = format!(
                        "mode=downward:threshold={:.12}:ratio={:.9}:attack={:.9}:release={:.9}:knee=2.828427:detection=rms,volume={:.12}:precision=float",
                        db_to_linear(band.threshold_dbfs),
                        band.ratio,
                        band.attack_ms,
                        band.release_ms,
                        db_to_linear(band.makeup_gain_db),
                    );
                    append_sidechain(
                        graph,
                        GraphEdge::new(input, output, &prefix),
                        "sidechaincompress",
                        chain.compressor.sidechain_highpass_hz,
                        chain.compressor.sidechain_lowpass_hz,
                        &options,
                    )?;
                }
            }
            ProcessingStage::Limiter => {
                if chain.limiter.layout == BandLayout::ThreeBand {
                    append_three_band(
                        graph,
                        GraphEdge::new(input, output, &prefix),
                        (
                            chain.limiter.low_crossover_hz,
                            chain.limiter.high_crossover_hz,
                        ),
                        [&chain.limiter.low, &chain.limiter.mid, &chain.limiter.high],
                        "peak",
                    )?;
                } else {
                    let band = &chain.limiter.full;
                    write!(
                        graph,
                        "[{input}]acompressor=mode=downward:threshold={:.12}:ratio={:.9}:knee={:.12}:attack={:.9}:release={:.9}:detection=peak[{output}];",
                        db_to_linear(band.threshold_dbfs),
                        band.ratio,
                        db_to_linear(band.knee_db),
                        band.attack_ms,
                        band.release_ms,
                    )?;
                }
            }
        }
        Ok(())
    }
}

fn stage_enabled(chain: &ProcessingChain, stage: ProcessingStage) -> bool {
    match stage {
        ProcessingStage::Expander => chain.expander.enabled,
        ProcessingStage::Agc => chain.agc.enabled,
        ProcessingStage::Compressor => chain.compressor.enabled,
        ProcessingStage::Limiter => chain.limiter.enabled,
        ProcessingStage::Equalizer => chain.equalizer.enabled,
        ProcessingStage::Deesser => chain.deesser.enabled,
    }
}

fn next_label(serial: &mut u32) -> String {
    let label = format!("s{serial}");
    *serial += 1;
    label
}

fn db_to_linear(db: f64) -> f64 {
    10.0_f64.powf(db / 20.0)
}

fn escape_filter_value(value: &str) -> String {
    value
        .replace('\\', "\\\\")
        .replace(':', "\\:")
        .replace('\'', "\\'")
}

#[derive(Clone, Copy)]
struct GraphEdge<'a> {
    input: &'a str,
    output: &'a str,
    prefix: &'a str,
}

impl<'a> GraphEdge<'a> {
    const fn new(input: &'a str, output: &'a str, prefix: &'a str) -> Self {
        Self {
            input,
            output,
            prefix,
        }
    }
}

fn emphasis_graph(production: bool, corner_hz: f64) -> Result<String, GraphDescriptionError> {
    let mut graph = String::new();
    append_emphasis(&mut graph, "in", "out", production, corner_hz)?;
    Ok(graph)
}

fn append_emphasis(
    graph: &mut String,
    input: &str,
    output: &str,
    production: bool,
    corner_hz: f64,
) -> Result<(), GraphDescriptionError> {
    const SAMPLE_RATE_HZ: f64 = 48_000.0;
    const REFERENCE_HZ: f64 = 1_000.0;
    let pole = (-2.0 * PI * corner_hz / SAMPLE_RATE_HZ).exp();
    let omega = 2.0 * PI * REFERENCE_HZ / SAMPLE_RATE_HZ;
    let inverse_at_reference = (1.0 + pole * pole - 2.0 * pole * omega.cos()).sqrt() / (1.0 - pole);
    if production {
        let scale = 1.0 / inverse_at_reference;
        write!(
            graph,
            "[{input}]biquad=b0={:.17}:b1={:.17}:b2=0:a0=1:a1=0:a2=0:precision=f32[{output}];",
            scale / (1.0 - pole),
            -pole * scale / (1.0 - pole),
        )?;
    } else {
        write!(
            graph,
            "[{input}]biquad=b0={:.17}:b1=0:b2=0:a0=1:a1={:.17}:a2=0:precision=f32[{output}];",
            inverse_at_reference * (1.0 - pole),
            -pole,
        )?;
    }
    Ok(())
}

fn append_sidechain(
    graph: &mut String,
    edge: GraphEdge<'_>,
    filter: &str,
    highpass_hz: f64,
    lowpass_hz: f64,
    options: &str,
) -> Result<(), GraphDescriptionError> {
    let GraphEdge {
        input,
        output,
        prefix,
    } = edge;
    let highpass = if highpass_hz > 0.0 {
        format!("highpass=f={highpass_hz:.9}:p=2")
    } else {
        "anull".to_owned()
    };
    let lowpass = if lowpass_hz > 0.0 {
        format!("lowpass=f={lowpass_hz:.9}:p=2")
    } else {
        "anull".to_owned()
    };
    write!(
        graph,
        "[{input}]asplit=2[{prefix}main][{prefix}sc];[{prefix}sc]{highpass},{lowpass}[{prefix}det];[{prefix}main][{prefix}det]{filter}={options}[{output}];"
    )?;
    Ok(())
}

fn append_three_band(
    graph: &mut String,
    edge: GraphEdge<'_>,
    crossovers_hz: (f64, f64),
    bands: [&DynamicsBand; 3],
    detection: &str,
) -> Result<(), GraphDescriptionError> {
    let GraphEdge {
        input,
        output,
        prefix,
    } = edge;
    let (low_crossover_hz, high_crossover_hz) = crossovers_hz;
    write!(
        graph,
        "[{input}]acrossover=split={low_crossover_hz:.9} {high_crossover_hz:.9}:order=4th[{prefix}lo][{prefix}mid][{prefix}hi];"
    )?;
    for (name, band) in ["lo", "mid", "hi"].into_iter().zip(bands) {
        write!(
            graph,
            "[{prefix}{name}]acompressor=mode=downward:threshold={:.12}:ratio={:.9}:knee={:.12}:attack={:.9}:release={:.9}:detection={detection}",
            db_to_linear(band.threshold_dbfs),
            band.ratio,
            db_to_linear(band.knee_db),
            band.attack_ms,
            band.release_ms,
        )?;
        if band.makeup_gain_db != 0.0 {
            write!(
                graph,
                ",volume={:.12}:precision=float",
                db_to_linear(band.makeup_gain_db)
            )?;
        }
        write!(graph, "[{prefix}{name}c];")?;
    }
    write!(
        graph,
        "[{prefix}loc][{prefix}midc][{prefix}hic]amix=inputs=3:normalize=0[{output}];"
    )?;
    Ok(())
}

/// Match the resampler's Kaiser size at the normalized low-pass edge, bounded
/// to 32,767 coefficients for unusually low receive cutoffs. Odd lengths give
/// an integral sample delay: 614 samples at 5 kHz, before the partition guard.
fn kaiser_fir_taps(cutoff_hz: f64) -> usize {
    if cutoff_hz == 0.0 {
        0
    } else {
        ((256.0 * 24_000.0 / cutoff_hz).ceil().min(32_767.0) as usize) | 1
    }
}

fn lowpass_history_frames(lowpass_hz: f64) -> usize {
    let taps = kaiser_fir_taps(lowpass_hz);
    if taps == 0 { 0 } else { taps - 1 + 7 }
}

/// Retain the existing IIR high-pass; a low-frequency FIR would delay speech.
/// Use FFmpeg's Kaiser-windowed sinc only for the sharp low-pass edge.
/// Keep the FIR in the existing graph so setup/warm-up stays off the callback.
fn append_brickwall_bandpass(
    graph: &mut String,
    input: &str,
    output: &str,
    prefix: &str,
    highpass_hz: f64,
    lowpass_hz: f64,
) -> Result<(), GraphDescriptionError> {
    let highpass_output = format!("{prefix}pass");
    let input = if highpass_hz == 0.0 {
        input
    } else {
        write!(
            graph,
            "[{input}]acrossover=split={highpass_hz:.9}:order=20th[{prefix}lo][{highpass_output}];[{prefix}lo]anullsink;"
        )?;
        &highpass_output
    };
    if lowpass_hz == 0.0 {
        write!(graph, "[{input}]anull[{output}];")?;
    } else {
        let lowpass_taps = kaiser_fir_taps(lowpass_hz);
        // FFmpeg's smallest usable FFT partition is eight samples. The mono
        // adelay emits seven leading samples before forwarding its input, so
        // convolution can satisfy even one-sample callbacks without gaps.
        write!(
            graph,
            "sinc=r=48000:hp=0.000000000:lp={lowpass_hz:.9}:beta=9:phase=50:hptaps=0:lptaps={lowpass_taps}[{prefix}ir];\
             [{input}]adelay=7S[{prefix}pad];\
             [{prefix}pad][{prefix}ir]afir=irnorm=-1:irgain=1:dry=1:wet=1:minp=8:maxp=8192:precision=float[{output}];"
        )?;
    }
    Ok(())
}

#[cfg(test)]
#[path = "tests/ffmpeg_graph_tests.rs"]
mod tests;
