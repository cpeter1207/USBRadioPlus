//! Typed settings for one radio's three FFmpeg processing chains.

use std::fmt;

use crate::{ProcessingStage, StageOrder, StageOrderError};

/// Context in which a processing chain runs.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ChainRole {
    /// Native local-receiver audio.
    LocalReceive,
    /// Audio received from an Asterisk link.
    Link,
    /// Voice and telemetry immediately before transmit conditioning.
    VoiceTelemetry,
}

/// Local-receiver PL rejection mode.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlFilter {
    /// Bypass PL rejection.
    Disabled,
    /// Notch the currently decoded CTCSS frequency.
    DecodedToneNotch,
    /// Reject the subaudible band with a high-pass filter.
    HighPass,
}

/// One three-band equalizer configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Equalizer {
    /// Whether the equalizer participates in the optional graph.
    pub enabled: bool,
    /// Low-shelf gain in dB.
    pub low_gain_db: f64,
    /// Low-shelf transition frequency in Hz.
    pub low_frequency_hz: f64,
    /// Low-shelf slope.
    pub low_slope: f64,
    /// Mid-band gain in dB.
    pub mid_gain_db: f64,
    /// Mid-band center frequency in Hz.
    pub mid_frequency_hz: f64,
    /// Mid-band width in octaves.
    pub mid_width_octaves: f64,
    /// High-shelf gain in dB.
    pub high_gain_db: f64,
    /// High-shelf transition frequency in Hz.
    pub high_frequency_hz: f64,
    /// High-shelf slope.
    pub high_slope: f64,
}

impl Default for Equalizer {
    fn default() -> Self {
        Self {
            enabled: true,
            low_gain_db: 2.0,
            low_frequency_hz: 500.0,
            low_slope: 0.7,
            mid_gain_db: -0.5,
            mid_frequency_hz: 1_000.0,
            mid_width_octaves: 1.0,
            high_gain_db: -1.0,
            high_frequency_hz: 2_000.0,
            high_slope: 0.7,
        }
    }
}

/// One split-band de-esser configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Deesser {
    /// Whether the de-esser participates in the optional graph.
    pub enabled: bool,
    /// Detector center frequency in Hz.
    pub frequency_hz: f64,
    /// Detector width in octaves.
    pub width_octaves: f64,
    /// Detector threshold in dBFS.
    pub threshold_dbfs: f64,
    /// Compression ratio applied to the selected band.
    pub ratio: f64,
    /// Maximum selected-band attenuation in dB.
    pub max_reduction_db: f64,
    /// Attack time in milliseconds.
    pub attack_ms: f64,
    /// Release time in milliseconds.
    pub release_ms: f64,
}

impl Default for Deesser {
    fn default() -> Self {
        Self {
            enabled: false,
            frequency_hz: 4_000.0,
            width_octaves: 1.0,
            threshold_dbfs: -18.0,
            ratio: 3.0,
            max_reduction_db: 4.0,
            attack_ms: 2.0,
            release_ms: 60.0,
        }
    }
}

/// Automatic gain-control configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Agc {
    /// Whether AGC participates in the optional graph.
    pub enabled: bool,
    /// Detector target in dBFS.
    pub target_dbfs: f64,
    /// Maximum positive gain in dB.
    pub max_gain_db: f64,
    /// Maximum attenuation in dB.
    pub max_attenuation_db: f64,
    /// RMS detector averaging time in milliseconds.
    pub rms_averaging_ms: f64,
    /// Maximum gain-rise speed in dB per second.
    pub gain_increase_db_per_second: f64,
    /// Maximum gain-reduction speed in dB per second.
    pub gain_decrease_db_per_second: f64,
    /// Activity-open threshold in dBFS.
    pub activity_threshold_dbfs: f64,
    /// Activity-close hysteresis in dB.
    pub activity_hysteresis_db: f64,
    /// Continuous qualification time before raising gain.
    pub hold_ms: f64,
    /// Target deadband in dB.
    pub deadband_db: f64,
    /// Detector high-pass edge in Hz; zero disables the edge.
    pub sidechain_highpass_hz: f64,
    /// Detector low-pass edge in Hz; zero disables the edge.
    pub sidechain_lowpass_hz: f64,
}

impl Default for Agc {
    fn default() -> Self {
        Self {
            enabled: false,
            target_dbfs: -24.0,
            max_gain_db: 6.0,
            max_attenuation_db: 6.0,
            rms_averaging_ms: 200.0,
            gain_increase_db_per_second: 2.0,
            gain_decrease_db_per_second: 6.0,
            activity_threshold_dbfs: -50.0,
            activity_hysteresis_db: 3.0,
            hold_ms: 500.0,
            deadband_db: 1.0,
            sidechain_highpass_hz: 800.0,
            sidechain_lowpass_hz: 1_500.0,
        }
    }
}

/// Downward-expander configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Expander {
    /// Whether the expander participates in the optional graph.
    pub enabled: bool,
    /// Expansion threshold in dBFS.
    pub threshold_dbfs: f64,
    /// Expansion ratio below the threshold.
    pub ratio: f64,
    /// Maximum attenuation in dB.
    pub max_attenuation_db: f64,
    /// Attack time in milliseconds.
    pub attack_ms: f64,
    /// Release time in milliseconds.
    pub release_ms: f64,
    /// Detector high-pass edge in Hz.
    pub sidechain_highpass_hz: f64,
    /// Detector low-pass edge in Hz.
    pub sidechain_lowpass_hz: f64,
}

impl Default for Expander {
    fn default() -> Self {
        Self {
            enabled: false,
            threshold_dbfs: -55.0,
            ratio: 1.5,
            max_attenuation_db: 9.0,
            attack_ms: 10.0,
            release_ms: 250.0,
            sidechain_highpass_hz: 800.0,
            sidechain_lowpass_hz: 1_500.0,
        }
    }
}

/// One compressor or limiter band's controls.
#[derive(Clone, Debug, PartialEq)]
pub struct DynamicsBand {
    /// Threshold in dBFS.
    pub threshold_dbfs: f64,
    /// Ratio above the threshold.
    pub ratio: f64,
    /// Makeup gain in dB; unused by the limiter.
    pub makeup_gain_db: f64,
    /// Knee width in dB.
    pub knee_db: f64,
    /// Attack time in milliseconds.
    pub attack_ms: f64,
    /// Release time in milliseconds.
    pub release_ms: f64,
}

/// Full-band or three-band dynamics selection.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BandLayout {
    /// Process the full signal as one band.
    FullBand,
    /// Split the signal into low, middle, and high bands.
    ThreeBand,
}

/// Compressor configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Compressor {
    /// Whether the compressor participates in the optional graph.
    pub enabled: bool,
    /// Selected band layout.
    pub layout: BandLayout,
    /// Low-to-middle crossover in Hz.
    pub low_crossover_hz: f64,
    /// Middle-to-high crossover in Hz.
    pub high_crossover_hz: f64,
    /// Full-band controls.
    pub full: DynamicsBand,
    /// Low-band controls.
    pub low: DynamicsBand,
    /// Middle-band controls.
    pub mid: DynamicsBand,
    /// High-band controls.
    pub high: DynamicsBand,
    /// Detector high-pass edge in Hz.
    pub sidechain_highpass_hz: f64,
    /// Detector low-pass edge in Hz.
    pub sidechain_lowpass_hz: f64,
}

impl Default for Compressor {
    fn default() -> Self {
        let band = DynamicsBand {
            threshold_dbfs: -6.0,
            ratio: 2.0,
            makeup_gain_db: 0.0,
            knee_db: 9.0,
            attack_ms: 75.0,
            release_ms: 300.0,
        };
        Self {
            enabled: false,
            layout: BandLayout::ThreeBand,
            low_crossover_hz: 500.0,
            high_crossover_hz: 2_000.0,
            full: band.clone(),
            low: band.clone(),
            mid: band.clone(),
            high: band,
            sidechain_highpass_hz: 800.0,
            sidechain_lowpass_hz: 1_500.0,
        }
    }
}

/// Shared FFmpeg limiter configuration.
#[derive(Clone, Debug, PartialEq)]
pub struct Limiter {
    /// Whether the limiter participates in the optional graph.
    pub enabled: bool,
    /// Selected band layout.
    pub layout: BandLayout,
    /// Low-to-middle crossover in Hz.
    pub low_crossover_hz: f64,
    /// Middle-to-high crossover in Hz.
    pub high_crossover_hz: f64,
    /// Full-band controls.
    pub full: DynamicsBand,
    /// Low-band controls.
    pub low: DynamicsBand,
    /// Middle-band controls.
    pub mid: DynamicsBand,
    /// High-band controls.
    pub high: DynamicsBand,
}

impl Default for Limiter {
    fn default() -> Self {
        Self {
            enabled: false,
            layout: BandLayout::ThreeBand,
            low_crossover_hz: 500.0,
            high_crossover_hz: 2_000.0,
            full: DynamicsBand {
                threshold_dbfs: -1.5,
                ratio: 20.0,
                makeup_gain_db: 0.0,
                knee_db: 0.0,
                attack_ms: 1.0,
                release_ms: 50.0,
            },
            low: DynamicsBand {
                threshold_dbfs: -1.5,
                ratio: 10.0,
                makeup_gain_db: 0.0,
                knee_db: 6.0,
                attack_ms: 50.0,
                release_ms: 250.0,
            },
            mid: DynamicsBand {
                threshold_dbfs: -1.5,
                ratio: 10.0,
                makeup_gain_db: 0.0,
                knee_db: 6.0,
                attack_ms: 10.0,
                release_ms: 100.0,
            },
            high: DynamicsBand {
                threshold_dbfs: -1.5,
                ratio: 20.0,
                makeup_gain_db: 0.0,
                knee_db: 6.0,
                attack_ms: 0.5,
                release_ms: 25.0,
            },
        }
    }
}

/// Fixed receive conditioning before the optional graph.
#[derive(Clone, Debug, PartialEq)]
pub struct ReceiveConditioning {
    /// Whether the brick-wall receive band-pass is enabled.
    pub bandpass_enabled: bool,
    /// Brick-wall lower edge in Hz.
    pub bandpass_highpass_hz: f64,
    /// Brick-wall upper edge in Hz.
    pub bandpass_lowpass_hz: f64,
    /// PL rejection mode.
    pub pl_filter: PlFilter,
    /// Fixed width of decoded-tone and 55 Hz tail notches in Hz.
    pub notch_width_hz: f64,
    /// High-pass edge used in high-pass mode.
    pub highpass_hz: f64,
}

impl Default for ReceiveConditioning {
    fn default() -> Self {
        Self {
            bandpass_enabled: true,
            bandpass_highpass_hz: 20.0,
            bandpass_lowpass_hz: 5_000.0,
            pl_filter: PlFilter::HighPass,
            notch_width_hz: 10.0,
            highpass_hz: 300.0,
        }
    }
}

/// Fixed final transmitter limiter and band-pass.
#[derive(Clone, Debug, PartialEq)]
pub struct TransmitTail {
    /// Whether the final limiter is enabled.
    pub limiter_enabled: bool,
    /// Final-limiter ceiling in dBFS.
    pub ceiling_dbfs: f64,
    /// Lookahead in milliseconds.
    pub lookahead_ms: f64,
    /// Attack time in milliseconds.
    pub attack_ms: f64,
    /// Release time in milliseconds.
    pub release_ms: f64,
    /// Whether the post-limiter brick-wall band-pass is enabled.
    pub bandpass_enabled: bool,
    /// Post-limiter lower edge in Hz; zero disables the lower edge.
    pub bandpass_highpass_hz: f64,
    /// Post-limiter upper edge in Hz.
    pub bandpass_lowpass_hz: f64,
}

impl Default for TransmitTail {
    fn default() -> Self {
        Self {
            limiter_enabled: false,
            ceiling_dbfs: -3.0,
            lookahead_ms: 5.0,
            attack_ms: 1.0,
            release_ms: 100.0,
            bandpass_enabled: false,
            bandpass_highpass_hz: 0.0,
            bandpass_lowpass_hz: 8_000.0,
        }
    }
}

/// Complete settings for one source chain.
#[derive(Clone, Debug, PartialEq)]
pub struct ProcessingChain {
    /// Source-chain role.
    pub role: ChainRole,
    /// Master enable for optional processing.
    pub enabled: bool,
    /// RNNoise enable; valid only on local receive.
    pub rnnoise_enabled: bool,
    /// Input gain in dB.
    pub input_gain_db: f64,
    /// Output gain in dB.
    pub output_gain_db: f64,
    /// Optional-stage execution order.
    pub stage_order: StageOrder,
    /// Fixed receive conditioning.
    pub receive: ReceiveConditioning,
    /// Equalizer controls.
    pub equalizer: Equalizer,
    /// De-esser controls.
    pub deesser: Deesser,
    /// AGC controls.
    pub agc: Agc,
    /// Expander controls.
    pub expander: Expander,
    /// Compressor controls.
    pub compressor: Compressor,
    /// Multiband limiter controls.
    pub limiter: Limiter,
    /// Fixed final transmitter stages.
    pub transmit_tail: TransmitTail,
}

impl ProcessingChain {
    /// Build the shipped settings for one source role.
    pub fn shipped(role: ChainRole) -> Self {
        let mut chain = Self {
            role,
            enabled: true,
            rnnoise_enabled: false,
            input_gain_db: 0.0,
            output_gain_db: -6.2,
            stage_order: StageOrder::standard(),
            receive: ReceiveConditioning::default(),
            equalizer: Equalizer::default(),
            deesser: Deesser::default(),
            agc: Agc::default(),
            expander: Expander::default(),
            compressor: Compressor::default(),
            limiter: Limiter::default(),
            transmit_tail: TransmitTail::default(),
        };
        match role {
            ChainRole::LocalReceive => {}
            ChainRole::Link => {
                chain.receive.bandpass_enabled = false;
                chain.receive.pl_filter = PlFilter::Disabled;
            }
            ChainRole::VoiceTelemetry => {
                chain.receive.bandpass_enabled = false;
                chain.receive.pl_filter = PlFilter::Disabled;
                chain.input_gain_db = 6.0;
                chain.output_gain_db = 0.0;
            }
        }
        chain
    }

    /// Validate settings without constructing an FFmpeg graph.
    pub fn validate(&self) -> Result<(), ProcessingConfigError> {
        self.stage_order.require_enabled(&self.enabled_stages())?;
        if self.role != ChainRole::LocalReceive
            && (self.rnnoise_enabled
                || self.receive.bandpass_enabled
                || self.receive.pl_filter != PlFilter::Disabled)
        {
            return Err(ProcessingConfigError::StageNotAllowed(self.role));
        }
        if self.role != ChainRole::VoiceTelemetry
            && (self.transmit_tail.limiter_enabled || self.transmit_tail.bandpass_enabled)
        {
            return Err(ProcessingConfigError::StageNotAllowed(self.role));
        }

        finite_range("input_gain_db", self.input_gain_db, -30.0, 30.0)?;
        finite_range("output_gain_db", self.output_gain_db, -30.0, 30.0)?;
        self.validate_receive()?;
        self.validate_equalizer()?;
        self.validate_deesser()?;
        self.validate_agc()?;
        self.validate_expander()?;
        self.validate_compressor()?;
        self.validate_limiter()?;
        self.validate_transmit_tail()
    }

    fn enabled_stages(&self) -> Vec<ProcessingStage> {
        [
            (self.equalizer.enabled, ProcessingStage::Equalizer),
            (self.expander.enabled, ProcessingStage::Expander),
            (self.agc.enabled, ProcessingStage::Agc),
            (self.deesser.enabled, ProcessingStage::Deesser),
            (self.compressor.enabled, ProcessingStage::Compressor),
            (self.limiter.enabled, ProcessingStage::Limiter),
        ]
        .into_iter()
        .filter_map(|(enabled, stage)| enabled.then_some(stage))
        .collect()
    }

    fn validate_receive(&self) -> Result<(), ProcessingConfigError> {
        finite_range(
            "receive_bandpass_highpass_hz",
            self.receive.bandpass_highpass_hz,
            20.0,
            2_000.0,
        )?;
        finite_range(
            "receive_bandpass_lowpass_hz",
            self.receive.bandpass_lowpass_hz,
            20.0,
            6_000.0,
        )?;
        require_greater(
            "receive_bandpass_lowpass_hz",
            self.receive.bandpass_lowpass_hz,
            "receive_bandpass_highpass_hz",
            self.receive.bandpass_highpass_hz,
        )?;
        finite_range(
            "ctcss_notch_width_hz",
            self.receive.notch_width_hz,
            10.0,
            10.0,
        )?;
        finite_range("ctcss_highpass_hz", self.receive.highpass_hz, 50.0, 500.0)
    }

    fn validate_equalizer(&self) -> Result<(), ProcessingConfigError> {
        let equalizer = &self.equalizer;
        finite_range("equalizer_low_gain_db", equalizer.low_gain_db, -12.0, 12.0)?;
        finite_range(
            "equalizer_low_frequency_hz",
            equalizer.low_frequency_hz,
            20.0,
            1_000.0,
        )?;
        finite_range("equalizer_low_slope", equalizer.low_slope, 0.1, 1.0)?;
        finite_range("equalizer_mid_gain_db", equalizer.mid_gain_db, -12.0, 12.0)?;
        finite_range(
            "equalizer_mid_frequency_hz",
            equalizer.mid_frequency_hz,
            100.0,
            4_000.0,
        )?;
        finite_range(
            "equalizer_mid_width_octaves",
            equalizer.mid_width_octaves,
            0.1,
            4.0,
        )?;
        finite_range(
            "equalizer_high_gain_db",
            equalizer.high_gain_db,
            -12.0,
            12.0,
        )?;
        finite_range(
            "equalizer_high_frequency_hz",
            equalizer.high_frequency_hz,
            1_000.0,
            5_000.0,
        )?;
        finite_range("equalizer_high_slope", equalizer.high_slope, 0.1, 1.0)
    }

    fn validate_deesser(&self) -> Result<(), ProcessingConfigError> {
        let deesser = &self.deesser;
        finite_range(
            "deesser_frequency_hz",
            deesser.frequency_hz,
            2_000.0,
            8_000.0,
        )?;
        finite_range("deesser_width_octaves", deesser.width_octaves, 0.1, 4.0)?;
        finite_range(
            "deesser_threshold_dbfs",
            deesser.threshold_dbfs,
            -60.0,
            -1.0,
        )?;
        finite_range("deesser_ratio", deesser.ratio, 1.0, 20.0)?;
        finite_range(
            "deesser_max_reduction_db",
            deesser.max_reduction_db,
            0.1,
            20.0,
        )?;
        finite_range("deesser_attack_ms", deesser.attack_ms, 0.1, 100.0)?;
        finite_range("deesser_release_ms", deesser.release_ms, 1.0, 2_000.0)
    }

    fn validate_agc(&self) -> Result<(), ProcessingConfigError> {
        let agc = &self.agc;
        finite_range("agc_target_dbfs", agc.target_dbfs, -40.0, -3.0)?;
        finite_range("agc_max_gain_db", agc.max_gain_db, 0.0, 30.0)?;
        finite_range("agc_max_attenuation_db", agc.max_attenuation_db, 0.0, 60.0)?;
        finite_range("agc_rms_averaging_ms", agc.rms_averaging_ms, 10.0, 5_000.0)?;
        finite_range(
            "agc_gain_increase_db_per_second",
            agc.gain_increase_db_per_second,
            0.1,
            100.0,
        )?;
        finite_range(
            "agc_gain_decrease_db_per_second",
            agc.gain_decrease_db_per_second,
            0.1,
            100.0,
        )?;
        finite_range(
            "agc_activity_threshold_dbfs",
            agc.activity_threshold_dbfs,
            -100.0,
            -3.0,
        )?;
        if agc.activity_threshold_dbfs >= agc.target_dbfs {
            return Err(ProcessingConfigError::Ordering {
                upper: "agc_target_dbfs",
                lower: "agc_activity_threshold_dbfs",
            });
        }
        finite_range(
            "agc_activity_hysteresis_db",
            agc.activity_hysteresis_db,
            0.0,
            12.0,
        )?;
        finite_range("agc_hold_ms", agc.hold_ms, 0.0, 10_000.0)?;
        finite_range("agc_deadband_db", agc.deadband_db, 0.0, 6.0)?;
        disabled_or_range(
            "agc_sidechain_highpass_hz",
            agc.sidechain_highpass_hz,
            50.0,
            2_000.0,
        )?;
        finite_range(
            "agc_sidechain_lowpass_hz",
            agc.sidechain_lowpass_hz,
            0.0,
            3_500.0,
        )?;
        if agc.sidechain_lowpass_hz != 0.0 {
            require_greater(
                "agc_sidechain_lowpass_hz",
                agc.sidechain_lowpass_hz,
                "agc_sidechain_highpass_hz",
                agc.sidechain_highpass_hz,
            )?;
        }
        Ok(())
    }

    fn validate_expander(&self) -> Result<(), ProcessingConfigError> {
        let expander = &self.expander;
        finite_range(
            "expander_threshold_dbfs",
            expander.threshold_dbfs,
            -100.0,
            -10.0,
        )?;
        finite_range("expander_ratio", expander.ratio, 1.0, 10.0)?;
        finite_range(
            "expander_max_attenuation_db",
            expander.max_attenuation_db,
            0.0,
            40.0,
        )?;
        finite_range("expander_attack_ms", expander.attack_ms, 1.0, 1_000.0)?;
        finite_range("expander_release_ms", expander.release_ms, 1.0, 10_000.0)?;
        finite_range(
            "expander_sidechain_highpass_hz",
            expander.sidechain_highpass_hz,
            50.0,
            2_000.0,
        )?;
        finite_range(
            "expander_sidechain_lowpass_hz",
            expander.sidechain_lowpass_hz,
            50.0,
            3_500.0,
        )?;
        require_greater(
            "expander_sidechain_lowpass_hz",
            expander.sidechain_lowpass_hz,
            "expander_sidechain_highpass_hz",
            expander.sidechain_highpass_hz,
        )
    }

    fn validate_compressor(&self) -> Result<(), ProcessingConfigError> {
        let compressor = &self.compressor;
        finite_range(
            "compressor_low_crossover_hz",
            compressor.low_crossover_hz,
            100.0,
            2_000.0,
        )?;
        finite_range(
            "compressor_high_crossover_hz",
            compressor.high_crossover_hz,
            100.0,
            5_000.0,
        )?;
        require_greater(
            "compressor_high_crossover_hz",
            compressor.high_crossover_hz,
            "compressor_low_crossover_hz",
            compressor.low_crossover_hz,
        )?;
        validate_compressor_band("compressor", &compressor.full)?;
        validate_compressor_band("compressor_low", &compressor.low)?;
        validate_compressor_band("compressor_mid", &compressor.mid)?;
        validate_compressor_band("compressor_high", &compressor.high)?;
        finite_range(
            "compressor_sidechain_highpass_hz",
            compressor.sidechain_highpass_hz,
            50.0,
            2_000.0,
        )?;
        finite_range(
            "compressor_sidechain_lowpass_hz",
            compressor.sidechain_lowpass_hz,
            50.0,
            3_500.0,
        )?;
        require_greater(
            "compressor_sidechain_lowpass_hz",
            compressor.sidechain_lowpass_hz,
            "compressor_sidechain_highpass_hz",
            compressor.sidechain_highpass_hz,
        )
    }

    fn validate_limiter(&self) -> Result<(), ProcessingConfigError> {
        let limiter = &self.limiter;
        finite_range(
            "limiter_low_crossover_hz",
            limiter.low_crossover_hz,
            100.0,
            2_000.0,
        )?;
        finite_range(
            "limiter_high_crossover_hz",
            limiter.high_crossover_hz,
            100.0,
            5_000.0,
        )?;
        require_greater(
            "limiter_high_crossover_hz",
            limiter.high_crossover_hz,
            "limiter_low_crossover_hz",
            limiter.low_crossover_hz,
        )?;
        validate_limiter_band("limiter", &limiter.full, -40.0, 1_000.0, 9_000.0)?;
        validate_limiter_band("limiter_low", &limiter.low, -40.0, 1_000.0, 9_000.0)?;
        validate_limiter_band("limiter_mid", &limiter.mid, -40.0, 1_000.0, 9_000.0)?;
        validate_limiter_band("limiter_high", &limiter.high, -30.0, 100.0, 1_000.0)
    }

    fn validate_transmit_tail(&self) -> Result<(), ProcessingConfigError> {
        let tail = &self.transmit_tail;
        finite_range(
            "lookahead_limiter_ceiling_dbfs",
            tail.ceiling_dbfs,
            -30.0,
            -0.1,
        )?;
        finite_range(
            "lookahead_limiter_lookahead_ms",
            tail.lookahead_ms,
            0.1,
            20.0,
        )?;
        finite_range("lookahead_limiter_attack_ms", tail.attack_ms, 0.1, 20.0)?;
        finite_range(
            "lookahead_limiter_release_ms",
            tail.release_ms,
            1.0,
            5_000.0,
        )?;
        finite_range(
            "post_limiter_bandpass_highpass_hz",
            tail.bandpass_highpass_hz,
            0.0,
            300.0,
        )?;
        finite_range(
            "post_limiter_bandpass_lowpass_hz",
            tail.bandpass_lowpass_hz,
            2_500.0,
            20_000.0,
        )
    }
}

/// Invalid typed processing configuration.
#[derive(Clone, Debug, PartialEq)]
pub enum ProcessingConfigError {
    /// A numeric setting was non-finite or outside its inclusive range.
    Range {
        /// Configuration setting name.
        field: &'static str,
        /// Inclusive lower bound.
        minimum: f64,
        /// Inclusive upper bound.
        maximum: f64,
    },
    /// One frequency or threshold must exceed another.
    Ordering {
        /// Name of the value that must be larger.
        upper: &'static str,
        /// Name of the lower value.
        lower: &'static str,
    },
    /// A source role enabled a stage that cannot run on that source.
    StageNotAllowed(ChainRole),
    /// The optional-stage order was invalid.
    StageOrder(StageOrderError),
}

impl fmt::Display for ProcessingConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Range {
                field,
                minimum,
                maximum,
            } => {
                write!(
                    formatter,
                    "{field} must be finite and between {minimum} and {maximum}"
                )
            }
            Self::Ordering { upper, lower } => write!(formatter, "{upper} must exceed {lower}"),
            Self::StageNotAllowed(role) => {
                write!(formatter, "fixed stage is not valid for {role:?}")
            }
            Self::StageOrder(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for ProcessingConfigError {}

impl From<StageOrderError> for ProcessingConfigError {
    fn from(value: StageOrderError) -> Self {
        Self::StageOrder(value)
    }
}

fn finite_range(
    field: &'static str,
    value: f64,
    minimum: f64,
    maximum: f64,
) -> Result<(), ProcessingConfigError> {
    if value.is_finite() && value >= minimum && value <= maximum {
        Ok(())
    } else {
        Err(ProcessingConfigError::Range {
            field,
            minimum,
            maximum,
        })
    }
}

fn disabled_or_range(
    field: &'static str,
    value: f64,
    minimum: f64,
    maximum: f64,
) -> Result<(), ProcessingConfigError> {
    if value == 0.0 {
        Ok(())
    } else {
        finite_range(field, value, minimum, maximum)
    }
}

fn require_greater(
    upper_name: &'static str,
    upper: f64,
    lower_name: &'static str,
    lower: f64,
) -> Result<(), ProcessingConfigError> {
    if upper > lower {
        Ok(())
    } else {
        Err(ProcessingConfigError::Ordering {
            upper: upper_name,
            lower: lower_name,
        })
    }
}

fn validate_compressor_band(
    prefix: &'static str,
    band: &DynamicsBand,
) -> Result<(), ProcessingConfigError> {
    finite_range(prefix, band.threshold_dbfs, -60.0, 0.0)?;
    finite_range(prefix, band.ratio, 1.0, 20.0)?;
    finite_range(prefix, band.makeup_gain_db, -30.0, 30.0)?;
    finite_range(prefix, band.knee_db, 0.0, 18.0)?;
    finite_range(prefix, band.attack_ms, 1.0, 1_000.0)?;
    finite_range(prefix, band.release_ms, 1.0, 9_000.0)
}

fn validate_limiter_band(
    prefix: &'static str,
    band: &DynamicsBand,
    minimum_threshold: f64,
    maximum_attack_ms: f64,
    maximum_release_ms: f64,
) -> Result<(), ProcessingConfigError> {
    finite_range(prefix, band.threshold_dbfs, minimum_threshold, -1.0)?;
    finite_range(prefix, band.ratio, 1.0, 20.0)?;
    finite_range(prefix, band.knee_db, 0.0, 18.0)?;
    finite_range(prefix, band.attack_ms, 0.1, maximum_attack_ms)?;
    finite_range(prefix, band.release_ms, 1.0, maximum_release_ms)
}

#[cfg(test)]
#[path = "tests/processing_profile_tests.rs"]
mod tests;
