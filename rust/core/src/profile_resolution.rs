//! Tolerant resolution of raw processing-profile overlays.

use std::collections::BTreeMap;
use std::fmt;

use crate::{
    BandLayout, ChainRole, ConfigDocument, ConfigError, PlFilter, ProcessingChain,
    ProcessingConfigError, StageOrder,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum OverlayLayer {
    Flat,
    Scoped,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct RawSetting {
    name: String,
    value: String,
}

/// Unparsed assignments from one flat or scoped source-chain section.
///
/// An overlay retains its origin so resolution warnings can identify the input
/// file and section. Use [`RawOverlay::flat`] and [`RawOverlay::scoped`] to make
/// the precedence of manually assembled overlays explicit.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RawOverlay {
    role: ChainRole,
    layer: OverlayLayer,
    source: String,
    section: String,
    settings: Vec<RawSetting>,
}

impl RawOverlay {
    /// Construct the lower-precedence flat overlay for a source-chain role.
    pub fn flat(
        role: ChainRole,
        source: impl Into<String>,
        values: impl IntoIterator<Item = (String, String)>,
    ) -> Self {
        Self::new(role, OverlayLayer::Flat, source, role_section(role), values)
    }

    /// Construct the higher-precedence scoped or named-profile overlay.
    pub fn scoped(
        role: ChainRole,
        source: impl Into<String>,
        section: impl Into<String>,
        values: impl IntoIterator<Item = (String, String)>,
    ) -> Self {
        Self::new(role, OverlayLayer::Scoped, source, section, values)
    }

    fn new(
        role: ChainRole,
        layer: OverlayLayer,
        source: impl Into<String>,
        section: impl Into<String>,
        values: impl IntoIterator<Item = (String, String)>,
    ) -> Self {
        Self {
            role,
            layer,
            source: source.into(),
            section: section.into(),
            settings: values
                .into_iter()
                .map(|(name, value)| RawSetting { name, value })
                .collect(),
        }
    }
}

/// Classification of a nonfatal configuration-resolution diagnostic.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResolutionWarningKind {
    /// A scoped section prefix was outside the configuration schema.
    UnknownSection,
    /// The section contained a parameter outside the processing vocabulary.
    UnknownOption,
    /// A known parameter does not belong to this source-chain schema.
    UnsupportedOption,
    /// A known parameter value could not be safely applied.
    InvalidValue,
}

/// One ADR 0019 warning whose setting fell back through normal inheritance.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResolutionWarning {
    /// Warning classification.
    pub kind: ResolutionWarningKind,
    /// Diagnostic source, normally a configuration-file path.
    pub source: String,
    /// Configuration section containing the assignment.
    pub section: String,
    /// Supplied parameter name.
    pub name: String,
    /// Supplied value. Display formatting escapes control characters.
    pub supplied_value: String,
    /// Effective inherited/default value, or `ignored` for an unknown option.
    pub fallback: String,
    /// Concise explanation of why the assignment was not applied.
    pub reason: String,
}

impl fmt::Display for ResolutionWarning {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{} [{}]: {}={:?} {}; using {}",
            self.source, self.section, self.name, self.supplied_value, self.reason, self.fallback
        )
    }
}

/// Failure to construct one safe, deterministic resolved processing profile.
#[derive(Clone, Debug, PartialEq)]
pub enum ProfileResolutionError {
    /// An overlay for a different source role was supplied to the resolver.
    RoleMismatch {
        /// Role requested by the resolver.
        expected: ChainRole,
        /// Role declared by the overlay.
        actual: ChainRole,
        /// Section attached to the mismatched overlay.
        section: String,
    },
    /// A flat/scoped overlay was supplied in the other precedence position.
    LayerMismatch {
        /// Expected layer name.
        expected: &'static str,
        /// Actual layer name.
        actual: &'static str,
        /// Section attached to the mismatched overlay.
        section: String,
    },
    /// Case variants assigned incompatible values to the same option.
    ConflictingAssignment {
        /// Diagnostic source, normally a configuration-file path.
        source: String,
        /// Configuration section containing the conflict.
        section: String,
        /// Case-normalized parameter name.
        name: String,
        /// First value encountered.
        first: String,
        /// Conflicting value encountered later.
        second: String,
    },
    /// A channel selected more than one named profile through case variants.
    AmbiguousProfileSelection {
        /// Named channel section.
        channel: String,
        /// Case-normalized profile-selector parameter.
        selector: String,
    },
    /// The configuration document could not resolve the requested section.
    Document(ConfigError),
    /// Cross-field or final source-chain validation failed.
    InvalidResolvedProfile(ProcessingConfigError),
}

impl fmt::Display for ProfileResolutionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::RoleMismatch {
                expected,
                actual,
                section,
            } => write!(
                formatter,
                "overlay [{section}] is for {actual:?}, not requested {expected:?}"
            ),
            Self::LayerMismatch {
                expected,
                actual,
                section,
            } => write!(
                formatter,
                "overlay [{section}] is a {actual} layer, not the required {expected} layer"
            ),
            Self::ConflictingAssignment {
                source,
                section,
                name,
                first,
                second,
            } => write!(
                formatter,
                "{source} [{section}]: conflicting {name} values {first:?} and {second:?}"
            ),
            Self::AmbiguousProfileSelection { channel, selector } => write!(
                formatter,
                "channel [{channel}] has conflicting case variants of {selector}"
            ),
            Self::Document(error) => error.fmt(formatter),
            Self::InvalidResolvedProfile(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for ProfileResolutionError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Document(error) => Some(error),
            Self::InvalidResolvedProfile(error) => Some(error),
            _ => None,
        }
    }
}

impl From<ConfigError> for ProfileResolutionError {
    fn from(value: ConfigError) -> Self {
        Self::Document(value)
    }
}

impl From<ProcessingConfigError> for ProfileResolutionError {
    fn from(value: ProcessingConfigError) -> Self {
        Self::InvalidResolvedProfile(value)
    }
}

/// Fully typed effective settings and all nonfatal fallback diagnostics.
#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedProfile {
    chain: ProcessingChain,
    warnings: Vec<ResolutionWarning>,
}

impl ResolvedProfile {
    /// Resolve shipped defaults, then a flat overlay, then an optional scoped overlay.
    ///
    /// Scalar syntax/range failures are warnings and preserve the value already
    /// inherited at that point. Final cross-field, source-role, and enabled-stage
    /// ordering invariants are errors because no unambiguous field can be discarded.
    pub fn resolve(
        role: ChainRole,
        flat: &RawOverlay,
        scoped: Option<&RawOverlay>,
    ) -> Result<Self, ProfileResolutionError> {
        require_overlay(flat, role, OverlayLayer::Flat)?;
        if let Some(overlay) = scoped {
            require_overlay(overlay, role, OverlayLayer::Scoped)?;
        }

        let mut chain = ProcessingChain::shipped(role);
        let mut warnings = Vec::new();
        apply_overlay(&mut chain, flat, &mut warnings)?;
        if let Some(overlay) = scoped {
            apply_overlay(&mut chain, overlay, &mut warnings)?;
        }
        chain.validate()?;
        Ok(Self { chain, warnings })
    }

    /// Resolve the flat and selected scoped sections for one configured channel.
    pub fn from_document(
        document: &ConfigDocument,
        source: impl Into<String>,
        channel: &str,
        role: ChainRole,
    ) -> Result<Self, ProfileResolutionError> {
        let source = source.into();
        let kind = role_section(role);
        reject_ambiguous_selector(document, channel, kind)?;
        let resolved_section = document.resolved_section(channel, kind)?;
        let flat = RawOverlay::flat(role, source.clone(), document.explicit_values(kind));
        let scoped = (!resolved_section.eq_ignore_ascii_case(kind)).then(|| {
            RawOverlay::scoped(
                role,
                source,
                resolved_section.clone(),
                document.explicit_values(&resolved_section),
            )
        });
        Self::resolve(role, &flat, scoped.as_ref())
    }

    /// Consume the result and return its validated effective source chain.
    pub fn into_chain(self) -> ProcessingChain {
        self.chain
    }

    /// Return every nonfatal resolution warning in precedence order.
    pub fn warnings(&self) -> &[ResolutionWarning] {
        &self.warnings
    }
}

fn require_overlay(
    overlay: &RawOverlay,
    role: ChainRole,
    layer: OverlayLayer,
) -> Result<(), ProfileResolutionError> {
    if overlay.role != role {
        return Err(ProfileResolutionError::RoleMismatch {
            expected: role,
            actual: overlay.role,
            section: overlay.section.clone(),
        });
    }
    if overlay.layer != layer {
        return Err(ProfileResolutionError::LayerMismatch {
            expected: layer_name(layer),
            actual: layer_name(overlay.layer),
            section: overlay.section.clone(),
        });
    }
    Ok(())
}

const fn layer_name(layer: OverlayLayer) -> &'static str {
    match layer {
        OverlayLayer::Flat => "flat",
        OverlayLayer::Scoped => "scoped",
    }
}

const fn role_section(role: ChainRole) -> &'static str {
    match role {
        ChainRole::LocalReceive => "local",
        ChainRole::Link => "link",
        ChainRole::VoiceTelemetry => "voice_telemetry",
    }
}

fn reject_ambiguous_selector(
    document: &ConfigDocument,
    channel: &str,
    kind: &str,
) -> Result<(), ProfileResolutionError> {
    let selector = format!("{kind}_profile");
    let mut selected: Option<String> = None;
    for (name, value) in document.explicit_values(channel) {
        if !name.eq_ignore_ascii_case(&selector) {
            continue;
        }
        if let Some(previous) = &selected {
            if !previous.trim().eq_ignore_ascii_case(value.trim()) {
                return Err(ProfileResolutionError::AmbiguousProfileSelection {
                    channel: channel.to_owned(),
                    selector,
                });
            }
        } else {
            selected = Some(value);
        }
    }
    Ok(())
}

fn apply_overlay(
    chain: &mut ProcessingChain,
    overlay: &RawOverlay,
    warnings: &mut Vec<ResolutionWarning>,
) -> Result<(), ProfileResolutionError> {
    let mut seen = BTreeMap::<String, String>::new();
    for setting in &overlay.settings {
        let name = setting.name.trim().to_ascii_lowercase();
        if let Some(previous) = seen.get(&name) {
            if previous.trim() != setting.value.trim() {
                return Err(ProfileResolutionError::ConflictingAssignment {
                    source: overlay.source.clone(),
                    section: overlay.section.clone(),
                    name,
                    first: previous.clone(),
                    second: setting.value.clone(),
                });
            }
            continue;
        }
        seen.insert(name.clone(), setting.value.clone());
        apply_setting(chain, overlay, setting, &name, warnings);
    }
    Ok(())
}

fn apply_setting(
    chain: &mut ProcessingChain,
    overlay: &RawOverlay,
    setting: &RawSetting,
    name: &str,
    warnings: &mut Vec<ResolutionWarning>,
) {
    macro_rules! boolean {
        ($target:expr) => {{ assign_boolean(&mut $target, warnings, overlay, setting) }};
    }
    macro_rules! number {
        ($target:expr, $minimum:expr, $maximum:expr) => {{
            assign_number(
                &mut $target,
                $minimum,
                $maximum,
                false,
                warnings,
                overlay,
                setting,
            )
        }};
    }
    macro_rules! disabled_number {
        ($target:expr, $minimum:expr, $maximum:expr) => {{
            assign_number(
                &mut $target,
                $minimum,
                $maximum,
                true,
                warnings,
                overlay,
                setting,
            )
        }};
    }

    match name {
        "enabled" => boolean!(chain.enabled),
        "stage_order" => {
            let fallback = format_stage_order(&chain.stage_order);
            match StageOrder::parse(&setting.value, &[]) {
                Ok(value) => chain.stage_order = value,
                Err(error) => {
                    invalid_warning(warnings, overlay, setting, fallback, error.to_string())
                }
            }
        }
        "input_gain_db" => number!(chain.input_gain_db, -30.0, 30.0),
        "output_gain_db" => number!(chain.output_gain_db, -30.0, 30.0),
        "equalizer_enabled" => boolean!(chain.equalizer.enabled),
        "equalizer_low_gain_db" => number!(chain.equalizer.low_gain_db, -12.0, 12.0),
        "equalizer_low_frequency_hz" => {
            number!(chain.equalizer.low_frequency_hz, 20.0, 1_000.0)
        }
        "equalizer_low_slope" => number!(chain.equalizer.low_slope, 0.1, 1.0),
        "equalizer_mid_gain_db" => number!(chain.equalizer.mid_gain_db, -12.0, 12.0),
        "equalizer_mid_frequency_hz" => {
            number!(chain.equalizer.mid_frequency_hz, 100.0, 4_000.0)
        }
        "equalizer_mid_width_octaves" => {
            number!(chain.equalizer.mid_width_octaves, 0.1, 4.0)
        }
        "equalizer_high_gain_db" => number!(chain.equalizer.high_gain_db, -12.0, 12.0),
        "equalizer_high_frequency_hz" => {
            number!(chain.equalizer.high_frequency_hz, 1_000.0, 5_000.0)
        }
        "equalizer_high_slope" => number!(chain.equalizer.high_slope, 0.1, 1.0),
        "deesser_enabled" => boolean!(chain.deesser.enabled),
        "deesser_frequency_hz" => number!(chain.deesser.frequency_hz, 2_000.0, 8_000.0),
        "deesser_width_octaves" => number!(chain.deesser.width_octaves, 0.1, 4.0),
        "deesser_threshold_dbfs" => number!(chain.deesser.threshold_dbfs, -60.0, -1.0),
        "deesser_ratio" => number!(chain.deesser.ratio, 1.0, 20.0),
        "deesser_max_reduction_db" => number!(chain.deesser.max_reduction_db, 0.1, 20.0),
        "deesser_attack_ms" => number!(chain.deesser.attack_ms, 0.1, 100.0),
        "deesser_release_ms" => number!(chain.deesser.release_ms, 1.0, 2_000.0),
        "agc_enabled" => boolean!(chain.agc.enabled),
        "agc_target_dbfs" => number!(chain.agc.target_dbfs, -40.0, -3.0),
        "agc_max_gain_db" => number!(chain.agc.max_gain_db, 0.0, 30.0),
        "agc_max_attenuation_db" => number!(chain.agc.max_attenuation_db, 0.0, 60.0),
        "agc_rms_averaging_ms" => number!(chain.agc.rms_averaging_ms, 10.0, 5_000.0),
        "agc_gain_increase_db_per_second" => {
            number!(chain.agc.gain_increase_db_per_second, 0.1, 100.0)
        }
        "agc_gain_decrease_db_per_second" => {
            number!(chain.agc.gain_decrease_db_per_second, 0.1, 100.0)
        }
        "agc_activity_threshold_dbfs" => {
            number!(chain.agc.activity_threshold_dbfs, -100.0, -3.0)
        }
        "agc_activity_hysteresis_db" => {
            number!(chain.agc.activity_hysteresis_db, 0.0, 12.0)
        }
        "agc_hold_ms" => number!(chain.agc.hold_ms, 0.0, 10_000.0),
        "agc_deadband_db" => number!(chain.agc.deadband_db, 0.0, 6.0),
        "agc_sidechain_highpass_hz" => {
            disabled_number!(chain.agc.sidechain_highpass_hz, 50.0, 2_000.0)
        }
        "agc_sidechain_lowpass_hz" => {
            disabled_number!(chain.agc.sidechain_lowpass_hz, 0.0, 3_500.0)
        }
        "expander_enabled" => boolean!(chain.expander.enabled),
        "expander_threshold_dbfs" => number!(chain.expander.threshold_dbfs, -100.0, -10.0),
        "expander_ratio" => number!(chain.expander.ratio, 1.0, 10.0),
        "expander_max_attenuation_db" => {
            number!(chain.expander.max_attenuation_db, 0.0, 40.0)
        }
        "expander_attack_ms" => number!(chain.expander.attack_ms, 1.0, 1_000.0),
        "expander_release_ms" => number!(chain.expander.release_ms, 1.0, 10_000.0),
        "expander_sidechain_highpass_hz" => {
            number!(chain.expander.sidechain_highpass_hz, 50.0, 2_000.0)
        }
        "expander_sidechain_lowpass_hz" => {
            number!(chain.expander.sidechain_lowpass_hz, 50.0, 3_500.0)
        }
        "compressor_enabled" => boolean!(chain.compressor.enabled),
        "compressor_bands" => band_layout(&mut chain.compressor.layout, warnings, overlay, setting),
        "compressor_low_crossover_hz" => {
            number!(chain.compressor.low_crossover_hz, 100.0, 2_000.0)
        }
        "compressor_high_crossover_hz" => {
            number!(chain.compressor.high_crossover_hz, 100.0, 5_000.0)
        }
        "compressor_threshold_dbfs" => {
            number!(chain.compressor.full.threshold_dbfs, -60.0, 0.0)
        }
        "compressor_ratio" => number!(chain.compressor.full.ratio, 1.0, 20.0),
        "compressor_makeup_gain_db" => {
            number!(chain.compressor.full.makeup_gain_db, -30.0, 30.0)
        }
        "compressor_attack_ms" => number!(chain.compressor.full.attack_ms, 1.0, 1_000.0),
        "compressor_release_ms" => number!(chain.compressor.full.release_ms, 1.0, 9_000.0),
        "compressor_low_threshold_dbfs" => {
            number!(chain.compressor.low.threshold_dbfs, -60.0, 0.0)
        }
        "compressor_low_ratio" => number!(chain.compressor.low.ratio, 1.0, 20.0),
        "compressor_low_makeup_gain_db" => {
            number!(chain.compressor.low.makeup_gain_db, -30.0, 30.0)
        }
        "compressor_low_knee_db" => number!(chain.compressor.low.knee_db, 0.0, 18.0),
        "compressor_low_attack_ms" => number!(chain.compressor.low.attack_ms, 1.0, 1_000.0),
        "compressor_low_release_ms" => {
            number!(chain.compressor.low.release_ms, 1.0, 9_000.0)
        }
        "compressor_mid_threshold_dbfs" => {
            number!(chain.compressor.mid.threshold_dbfs, -60.0, 0.0)
        }
        "compressor_mid_ratio" => number!(chain.compressor.mid.ratio, 1.0, 20.0),
        "compressor_mid_makeup_gain_db" => {
            number!(chain.compressor.mid.makeup_gain_db, -30.0, 30.0)
        }
        "compressor_mid_knee_db" => number!(chain.compressor.mid.knee_db, 0.0, 18.0),
        "compressor_mid_attack_ms" => number!(chain.compressor.mid.attack_ms, 1.0, 1_000.0),
        "compressor_mid_release_ms" => {
            number!(chain.compressor.mid.release_ms, 1.0, 9_000.0)
        }
        "compressor_high_threshold_dbfs" => {
            number!(chain.compressor.high.threshold_dbfs, -60.0, 0.0)
        }
        "compressor_high_ratio" => number!(chain.compressor.high.ratio, 1.0, 20.0),
        "compressor_high_makeup_gain_db" => {
            number!(chain.compressor.high.makeup_gain_db, -30.0, 30.0)
        }
        "compressor_high_knee_db" => number!(chain.compressor.high.knee_db, 0.0, 18.0),
        "compressor_high_attack_ms" => {
            number!(chain.compressor.high.attack_ms, 1.0, 1_000.0)
        }
        "compressor_high_release_ms" => {
            number!(chain.compressor.high.release_ms, 1.0, 9_000.0)
        }
        "compressor_sidechain_highpass_hz" => {
            number!(chain.compressor.sidechain_highpass_hz, 50.0, 2_000.0)
        }
        "compressor_sidechain_lowpass_hz" => {
            number!(chain.compressor.sidechain_lowpass_hz, 50.0, 3_500.0)
        }
        "limiter_enabled" => boolean!(chain.limiter.enabled),
        "limiter_bands" => band_layout(&mut chain.limiter.layout, warnings, overlay, setting),
        "limiter_low_crossover_hz" => {
            number!(chain.limiter.low_crossover_hz, 100.0, 2_000.0)
        }
        "limiter_high_crossover_hz" => {
            number!(chain.limiter.high_crossover_hz, 100.0, 5_000.0)
        }
        "limiter_threshold_dbfs" => number!(chain.limiter.full.threshold_dbfs, -40.0, -1.0),
        "limiter_ratio" => number!(chain.limiter.full.ratio, 1.0, 20.0),
        "limiter_knee_db" => number!(chain.limiter.full.knee_db, 0.0, 18.0),
        "limiter_attack_ms" => number!(chain.limiter.full.attack_ms, 0.1, 1_000.0),
        "limiter_release_ms" => number!(chain.limiter.full.release_ms, 1.0, 9_000.0),
        "limiter_low_threshold_dbfs" => {
            number!(chain.limiter.low.threshold_dbfs, -40.0, -1.0)
        }
        "limiter_low_ratio" => number!(chain.limiter.low.ratio, 1.0, 20.0),
        "limiter_low_knee_db" => number!(chain.limiter.low.knee_db, 0.0, 18.0),
        "limiter_low_attack_ms" => number!(chain.limiter.low.attack_ms, 0.1, 1_000.0),
        "limiter_low_release_ms" => number!(chain.limiter.low.release_ms, 1.0, 9_000.0),
        "limiter_mid_threshold_dbfs" => {
            number!(chain.limiter.mid.threshold_dbfs, -40.0, -1.0)
        }
        "limiter_mid_ratio" => number!(chain.limiter.mid.ratio, 1.0, 20.0),
        "limiter_mid_knee_db" => number!(chain.limiter.mid.knee_db, 0.0, 18.0),
        "limiter_mid_attack_ms" => number!(chain.limiter.mid.attack_ms, 0.1, 1_000.0),
        "limiter_mid_release_ms" => number!(chain.limiter.mid.release_ms, 1.0, 9_000.0),
        "limiter_high_threshold_dbfs" => {
            number!(chain.limiter.high.threshold_dbfs, -30.0, -1.0)
        }
        "limiter_high_ratio" => number!(chain.limiter.high.ratio, 1.0, 20.0),
        "limiter_high_knee_db" => number!(chain.limiter.high.knee_db, 0.0, 18.0),
        "limiter_high_attack_ms" => number!(chain.limiter.high.attack_ms, 0.1, 100.0),
        "limiter_high_release_ms" => number!(chain.limiter.high.release_ms, 1.0, 1_000.0),
        "rnnoise_enabled" if chain.role == ChainRole::LocalReceive => {
            boolean!(chain.rnnoise_enabled)
        }
        "receive_bandpass_enabled" if chain.role == ChainRole::LocalReceive => {
            boolean!(chain.receive.bandpass_enabled)
        }
        "receive_bandpass_highpass_hz" if chain.role == ChainRole::LocalReceive => {
            number!(chain.receive.bandpass_highpass_hz, 20.0, 2_000.0)
        }
        "receive_bandpass_lowpass_hz" if chain.role == ChainRole::LocalReceive => {
            number!(chain.receive.bandpass_lowpass_hz, 20.0, 6_000.0)
        }
        "ctcss_filter_mode" if chain.role == ChainRole::LocalReceive => {
            pl_filter(&mut chain.receive.pl_filter, warnings, overlay, setting)
        }
        "ctcss_notch_width_hz" if chain.role == ChainRole::LocalReceive => {
            number!(chain.receive.notch_width_hz, 10.0, 10.0)
        }
        "ctcss_highpass_hz" if chain.role == ChainRole::LocalReceive => {
            number!(chain.receive.highpass_hz, 50.0, 500.0)
        }
        "rnnoise_enabled"
        | "receive_bandpass_enabled"
        | "receive_bandpass_highpass_hz"
        | "receive_bandpass_lowpass_hz"
        | "ctcss_filter_mode"
        | "ctcss_notch_width_hz"
        | "ctcss_highpass_hz" => unsupported_warning(warnings, overlay, setting),
        "lookahead_limiter_enabled" if chain.role == ChainRole::VoiceTelemetry => {
            boolean!(chain.transmit_tail.limiter_enabled)
        }
        "lookahead_limiter_ceiling_dbfs" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.ceiling_dbfs, -30.0, -0.1)
        }
        "lookahead_limiter_lookahead_ms" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.lookahead_ms, 0.1, 20.0)
        }
        "lookahead_limiter_attack_ms" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.attack_ms, 0.1, 20.0)
        }
        "lookahead_limiter_release_ms" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.release_ms, 1.0, 5_000.0)
        }
        "post_limiter_bandpass_enabled" if chain.role == ChainRole::VoiceTelemetry => {
            boolean!(chain.transmit_tail.bandpass_enabled)
        }
        "post_limiter_bandpass_highpass_hz" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.bandpass_highpass_hz, 0.0, 300.0)
        }
        "post_limiter_bandpass_lowpass_hz" if chain.role == ChainRole::VoiceTelemetry => {
            number!(chain.transmit_tail.bandpass_lowpass_hz, 2_500.0, 20_000.0)
        }
        "lookahead_limiter_enabled"
        | "lookahead_limiter_ceiling_dbfs"
        | "lookahead_limiter_lookahead_ms"
        | "lookahead_limiter_attack_ms"
        | "lookahead_limiter_release_ms"
        | "post_limiter_bandpass_enabled"
        | "post_limiter_bandpass_highpass_hz"
        | "post_limiter_bandpass_lowpass_hz" => unsupported_warning(warnings, overlay, setting),
        _ => unknown_warning(warnings, overlay, setting),
    }
}

fn assign_boolean(
    target: &mut bool,
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    let fallback = yes_no(*target);
    if let Some(value) = parse_boolean(&setting.value) {
        *target = value;
    } else {
        invalid_warning(
            warnings,
            overlay,
            setting,
            fallback,
            "requires yes or no".to_owned(),
        );
    }
}

fn assign_number(
    target: &mut f64,
    minimum: f64,
    maximum: f64,
    zero_disables: bool,
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    let fallback = format_number(*target);
    match parse_number(&setting.value, minimum, maximum, zero_disables) {
        Ok(value) => *target = value,
        Err(reason) => invalid_warning(warnings, overlay, setting, fallback, reason),
    }
}

fn parse_boolean(value: &str) -> Option<bool> {
    if value.trim().eq_ignore_ascii_case("yes") {
        Some(true)
    } else if value.trim().eq_ignore_ascii_case("no") {
        Some(false)
    } else {
        None
    }
}

fn parse_number(
    value: &str,
    minimum: f64,
    maximum: f64,
    zero_disables: bool,
) -> Result<f64, String> {
    let parsed = value
        .trim()
        .parse::<f64>()
        .map_err(|_| format!("requires a finite number from {minimum} through {maximum}"))?;
    if parsed.is_finite()
        && ((zero_disables && parsed == 0.0) || (parsed >= minimum && parsed <= maximum))
    {
        Ok(parsed)
    } else if zero_disables {
        Err(format!(
            "requires zero or a finite number from {minimum} through {maximum}"
        ))
    } else {
        Err(format!(
            "requires a finite number from {minimum} through {maximum}"
        ))
    }
}

fn band_layout(
    target: &mut BandLayout,
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    match setting.value.trim() {
        "1" => *target = BandLayout::FullBand,
        "3" => *target = BandLayout::ThreeBand,
        _ => invalid_warning(
            warnings,
            overlay,
            setting,
            match target {
                BandLayout::FullBand => "1",
                BandLayout::ThreeBand => "3",
            }
            .to_owned(),
            "requires 1 or 3".to_owned(),
        ),
    }
}

fn pl_filter(
    target: &mut PlFilter,
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    let parsed = if setting.value.trim().eq_ignore_ascii_case("disabled") {
        Some(PlFilter::Disabled)
    } else if setting.value.trim().eq_ignore_ascii_case("notch") {
        Some(PlFilter::DecodedToneNotch)
    } else if setting.value.trim().eq_ignore_ascii_case("highpass") {
        Some(PlFilter::HighPass)
    } else {
        None
    };
    if let Some(value) = parsed {
        *target = value;
    } else {
        invalid_warning(
            warnings,
            overlay,
            setting,
            pl_filter_name(*target).to_owned(),
            "requires disabled, notch, or highpass".to_owned(),
        );
    }
}

const fn pl_filter_name(value: PlFilter) -> &'static str {
    match value {
        PlFilter::Disabled => "disabled",
        PlFilter::DecodedToneNotch => "notch",
        PlFilter::HighPass => "highpass",
    }
}

fn yes_no(value: bool) -> String {
    if value { "yes" } else { "no" }.to_owned()
}

fn format_number(value: f64) -> String {
    value.to_string()
}

fn format_stage_order(value: &StageOrder) -> String {
    value
        .stages()
        .iter()
        .map(|stage| stage.name())
        .collect::<Vec<_>>()
        .join(",")
}

fn invalid_warning(
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
    fallback: String,
    reason: String,
) {
    warnings.push(warning(
        ResolutionWarningKind::InvalidValue,
        overlay,
        setting,
        fallback,
        reason,
    ));
}

fn unsupported_warning(
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    warnings.push(warning(
        ResolutionWarningKind::UnsupportedOption,
        overlay,
        setting,
        "ignored".to_owned(),
        format!("is not supported for {:?}", overlay.role),
    ));
}

fn unknown_warning(
    warnings: &mut Vec<ResolutionWarning>,
    overlay: &RawOverlay,
    setting: &RawSetting,
) {
    warnings.push(warning(
        ResolutionWarningKind::UnknownOption,
        overlay,
        setting,
        "ignored".to_owned(),
        "is unknown and was ignored".to_owned(),
    ));
}

fn warning(
    kind: ResolutionWarningKind,
    overlay: &RawOverlay,
    setting: &RawSetting,
    fallback: String,
    reason: String,
) -> ResolutionWarning {
    ResolutionWarning {
        kind,
        source: overlay.source.clone(),
        section: overlay.section.clone(),
        name: setting.name.clone(),
        supplied_value: setting.value.clone(),
        fallback,
        reason,
    }
}

#[cfg(test)]
#[path = "tests/profile_resolution_tests.rs"]
mod tests;
