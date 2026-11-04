//! Standalone composition aliases over the shared station and processing resolver.
//!
//! Resolution has no device or filesystem side effects. Native defaults are
//! explicit here; the ASL composition continues to use its existing defaults.

use std::collections::BTreeMap;
use std::fmt;

use crate::station_config::{
    Setting, assign_display, assign_f64, assign_u32, invalid_warning, record_assignment,
    reject_ambiguous_selector, unknown_warning, yes_no,
};
use crate::{
    ChannelConfiguration, ChannelConfigurationError, ConfigDocument, ResolutionWarning,
    ResolvedChannelConfiguration,
};

/// Native device admission policy, independent of device discovery.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NativeDeviceSelection {
    /// Require the supplied exact device identity.
    Exact,
    /// Select the lowest admitted ALSA card when no exact identity is supplied.
    AutomaticLowestAlsaCard,
}

impl fmt::Display for NativeDeviceSelection {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Exact => "exact",
            Self::AutomaticLowestAlsaCard => "automatic_lowest_alsa_card",
        })
    }
}

/// Physical request details which are specific to the standalone composition.
#[derive(Clone, Debug, PartialEq)]
pub struct NativePhysicalOptions {
    /// Exact selection is the native default.
    pub device_selection: NativeDeviceSelection,
    /// Physical capture channels, one or two.
    pub input_device_channels: u32,
    /// Physical playback channels, one or two; native defaults to mono.
    pub output_device_channels: u32,
    /// Explicit capture-gain intention; omission preserves the mixer setting.
    pub hardware_input_gain_db: Option<f64>,
    /// Explicit output-A gain intention; omission preserves the mixer setting.
    pub hardware_output_a_gain_db: Option<f64>,
    /// Explicit output-B gain intention; omission preserves the mixer setting.
    pub hardware_output_b_gain_db: Option<f64>,
    /// Raw receive graph used when no explicit local processing profile exists.
    pub receive_graph: String,
    /// Raw transmit graph used when no explicit voice/telemetry profile exists.
    pub transmit_graph: String,
}

impl Default for NativePhysicalOptions {
    fn default() -> Self {
        Self {
            device_selection: NativeDeviceSelection::Exact,
            input_device_channels: 1,
            output_device_channels: 1,
            hardware_input_gain_db: None,
            hardware_output_a_gain_db: None,
            hardware_output_b_gain_db: None,
            receive_graph: "anull".to_owned(),
            transmit_graph: "anull".to_owned(),
        }
    }
}

/// Shared native configuration and its borrowed-inspection backing data.
#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedNativeConfiguration {
    resolved: ResolvedChannelConfiguration,
    physical: NativePhysicalOptions,
    document: ConfigDocument,
    source_document: ConfigDocument,
    warnings: Vec<ResolutionWarning>,
}

impl ResolvedNativeConfiguration {
    /// Resolve `[radio]`, `[radio NODE]` and selected shared profiles without I/O.
    ///
    /// Radio keys are aliases into the existing station schema. Invalid values
    /// retain the shared resolver's inherited value and produce its diagnostics.
    pub fn from_document(
        document: &ConfigDocument,
        source: impl Into<String>,
        node: &str,
    ) -> Result<Self, ChannelConfigurationError> {
        let source = source.into();
        let defaults = crate::StationConfig {
            hardware: crate::HardwareConfig {
                output_a_assignment: crate::HardwareOutputAssignment::Voice,
                output_b_assignment: crate::HardwareOutputAssignment::Ctcss,
                eeprom_enabled: false,
                ..crate::HardwareConfig::default()
            },
            transmit: crate::TransmitConfig {
                preemphasis_enabled: false,
                ..crate::TransmitConfig::default()
            },
            ..crate::StationConfig::default()
        };
        let mut normalized = ConfigDocument::new(format!(
            "[{node}]\n[hardware]\nhardware_output_a_assignment={}\nhardware_output_b_assignment={}\nhardware_eeprom_enabled={}\n[transmit]\npreemphasis_enabled={}\n",
            defaults.hardware.output_a_assignment,
            defaults.hardware.output_b_assignment,
            yes_no(defaults.hardware.eeprom_enabled),
            yes_no(defaults.transmit.preemphasis_enabled)
        ));
        let mut physical = NativePhysicalOptions::default();
        let mut warnings = Vec::new();
        let mut origins = Vec::new();
        // Require a real node and validate its name through the existing document contract.
        document
            .resolved_section(node, "general", &source, &mut warnings)
            .map_err(crate::StationConfigError::from)?;
        let local_profile_configured =
            native_processing_profile_configured(document, node, "local");
        let mut local_defaults = crate::ProcessingChain::shipped(crate::ChainRole::LocalReceive);
        if !local_profile_configured {
            local_defaults.output_gain_db = 0.0;
            normalized
                .set_value(
                    "local",
                    "output_gain_db",
                    &local_defaults.output_gain_db.to_string(),
                )
                .map_err(crate::StationConfigError::from)?;
        }
        for section in ["radio".to_owned(), format!("radio {node}")] {
            let mut seen = BTreeMap::new();
            for (name, value) in document.explicit_values(&section) {
                let key = name.to_ascii_lowercase();
                if !record_assignment(&mut seen, &source, &section, &key, &value)? {
                    continue;
                }
                let setting = Setting {
                    source: &source,
                    section: &section,
                    name: &name,
                    value: &value,
                };
                if let Some(alias) = ALIASES.iter().find(|alias| {
                    alias.native == key || (alias.section == "hardware" && alias.key == key)
                }) {
                    if alias.section == "local" && local_profile_configured {
                        continue;
                    }
                    if matches!(alias.value, ValueAlias::WholeDb)
                        && value.trim().parse::<i32>().is_err()
                    {
                        // Keep the legacy raw ABI integer constraint while delegating
                        // inherited gain/default resolution to the shared profile owner.
                        let inherited = crate::ResolvedProfile::from_document_with_defaults(
                            &normalized,
                            &source,
                            node,
                            crate::ChainRole::LocalReceive,
                            local_defaults.clone(),
                        )?
                        .into_chain()
                        .output_gain_db;
                        invalid_warning(
                            &mut warnings,
                            setting,
                            inherited.to_string(),
                            "requires an integer from -30 through 30 dB".to_owned(),
                        );
                        continue;
                    }
                    let destination = if section == "radio" {
                        alias.section.to_owned()
                    } else {
                        format!("{} {node}", alias.section)
                    };
                    let value = alias.value.to_shared(&value);
                    normalized
                        .set_value(&destination, alias.key, &value)
                        .map_err(crate::StationConfigError::from)?;
                    origins.push((
                        destination,
                        alias,
                        section.clone(),
                        name.clone(),
                        setting.value.to_owned(),
                    ));
                } else {
                    apply_physical(&mut physical, &key, setting, &mut warnings);
                }
            }
        }
        // Preserve only shared configuration sections and explicit node selectors.
        // Controller policy remains in its own owner and is never interpreted here.
        for section in document.section_names() {
            if section.eq_ignore_ascii_case(node) {
                for (key, value) in document.explicit_values(&section) {
                    if key.to_ascii_lowercase().ends_with("_profile")
                        || key.eq_ignore_ascii_case("channel_enabled")
                    {
                        if let Some(kind) = key.to_ascii_lowercase().strip_suffix("_profile") {
                            reject_ambiguous_selector(document, node, kind)?;
                        }
                        normalized
                            .set_value(node, &key, &value)
                            .map_err(crate::StationConfigError::from)?;
                    }
                }
            } else if is_shared_section(&section) {
                // Keep even empty named sections: their presence selects a profile.
                let mut text = normalized.as_str().to_owned();
                text.push_str(&format!("\n[{section}]\n"));
                for (key, value) in document.explicit_values(&section) {
                    text.push_str(&format!("{key}={value}\n"));
                }
                normalized = ConfigDocument::new(text);
            }
        }
        let resolved = ResolvedChannelConfiguration::from_document_with_defaults(
            &normalized,
            &source,
            node,
            defaults,
            local_defaults,
        )?;
        let selected_hardware = normalized
            .resolved_section(node, "hardware", &source, &mut Vec::new())
            .map_err(crate::StationConfigError::from)?;
        for section in ["hardware", &selected_hardware] {
            let values = normalized.explicit_values(section);
            for (key, target) in [
                (
                    "hardware_input_gain_db",
                    &mut physical.hardware_input_gain_db,
                ),
                (
                    "hardware_output_a_gain_db",
                    &mut physical.hardware_output_a_gain_db,
                ),
                (
                    "hardware_output_b_gain_db",
                    &mut physical.hardware_output_b_gain_db,
                ),
            ] {
                if let Some((name, value)) = values
                    .iter()
                    .find(|(name, _)| name.eq_ignore_ascii_case(key))
                {
                    assign_gain_intention(
                        target,
                        Setting {
                            source: &source,
                            section,
                            name,
                            value,
                        },
                    );
                }
            }
        }
        for warning in resolved.warnings() {
            let mut warning = warning.clone();
            if let Some((_, alias, section, name, supplied)) =
                origins
                    .iter()
                    .rev()
                    .find(|(destination, alias, _, _, supplied)| {
                        destination.eq_ignore_ascii_case(&warning.section)
                            && alias.key.eq_ignore_ascii_case(&warning.name)
                            && alias.value.to_shared(supplied) == warning.supplied_value
                    })
            {
                warning.section = section.clone();
                warning.name = name.clone();
                warning.supplied_value = supplied.clone();
                warning.fallback = alias.value.to_native(&warning.fallback);
            }
            warnings.push(warning);
        }
        Ok(Self {
            resolved,
            physical,
            document: normalized,
            source_document: document.clone(),
            warnings,
        })
    }

    /// Return the effective common station and processing configuration.
    pub const fn config(&self) -> &ChannelConfiguration {
        self.resolved.config()
    }
    /// Borrow the common configuration for EEPROM preflight before immutable preparation.
    pub fn config_mut(&mut self) -> &mut ChannelConfiguration {
        self.resolved.config_mut()
    }
    /// Consume the result and return the common station configuration.
    pub fn into_config(self) -> ChannelConfiguration {
        self.resolved.into_config()
    }
    /// Borrow the native physical request details.
    pub const fn physical(&self) -> &NativePhysicalOptions {
        &self.physical
    }
    /// Borrow the normalized shared document, including selected named profiles.
    pub const fn normalized_document(&self) -> &ConfigDocument {
        &self.document
    }
    /// Borrow the original native text; generated aliases never imply an explicit profile.
    pub const fn source_document(&self) -> &ConfigDocument {
        &self.source_document
    }
    /// Borrow canonical diagnostics for effective canonical tuner values.
    pub fn shared_warnings(&self) -> &[ResolutionWarning] {
        self.resolved.warnings()
    }
    /// Borrow deterministic fallback diagnostics.
    pub fn warnings(&self) -> &[ResolutionWarning] {
        &self.warnings
    }
}

/// Return the native `[radio]` key for one canonical tuner section/key.
pub fn native_radio_setting_destination(section: &str, key: &str) -> Option<&'static str> {
    ALIASES
        .iter()
        .find(|alias| {
            alias.section.eq_ignore_ascii_case(section) && alias.key.eq_ignore_ascii_case(key)
        })
        .map(|alias| alias.native)
}

/// Translate a canonical tuner value to the corresponding native alias value.
pub fn native_radio_setting_value(section: &str, key: &str, value: &str) -> Option<String> {
    ALIASES
        .iter()
        .find(|alias| {
            alias.section.eq_ignore_ascii_case(section) && alias.key.eq_ignore_ascii_case(key)
        })
        .map(|alias| alias.value.to_native(value))
}

/// List actual native node sections, excluding controller/shared profile sections.
pub fn native_configured_nodes(document: &ConfigDocument) -> Vec<String> {
    const CONTROLLER_SECTIONS: &[&str] = &[
        "radio",
        "echolink",
        "identifier",
        "announcement",
        "tail",
        "courtesy",
        "morse",
        "speech",
        "time",
        "template",
        "message",
        "macro",
        "event",
        "permanent",
        "schedule",
        "sensor",
        "input",
        "output",
        "alarm",
        "fan",
    ];
    document
        .configured_channels()
        .into_iter()
        .filter(|section| {
            !CONTROLLER_SECTIONS
                .iter()
                .any(|reserved| section.eq_ignore_ascii_case(reserved))
        })
        .collect()
}

/// Return whether a role has an operator-supplied flat, scoped or named selection.
pub fn native_processing_profile_configured(
    document: &ConfigDocument,
    node: &str,
    kind: &str,
) -> bool {
    let scoped = format!("{kind} {node}");
    let selector = format!("{kind}_profile");
    document
        .section_names()
        .iter()
        .any(|section| section.eq_ignore_ascii_case(kind) || section.eq_ignore_ascii_case(&scoped))
        || document
            .explicit_values(node)
            .keys()
            .any(|key| key.eq_ignore_ascii_case(&selector))
}

fn is_shared_section(section: &str) -> bool {
    let kind = section.split_whitespace().next().unwrap_or_default();
    [
        "hardware",
        "receive",
        "transmit",
        "ctcss",
        "dcs",
        "duplex",
        "diagnostics",
        "local",
        "link",
        "voice_telemetry",
    ]
    .iter()
    .any(|known| kind.eq_ignore_ascii_case(known))
}

fn assign_gain_intention(target: &mut Option<f64>, setting: Setting<'_>) {
    let mut gain = target.unwrap_or_default();
    let mut warnings = Vec::new();
    assign_f64(&mut gain, -30.0, 30.0, setting, &mut warnings);
    if warnings.is_empty() {
        *target = Some(gain);
    }
}

fn apply_physical(
    target: &mut NativePhysicalOptions,
    key: &str,
    setting: Setting<'_>,
    warnings: &mut Vec<ResolutionWarning>,
) {
    match key {
        "device_selection" => assign_display(
            &mut target.device_selection,
            setting,
            warnings,
            |value| match value {
                "exact" => Ok(NativeDeviceSelection::Exact),
                "automatic_lowest_alsa_card" => Ok(NativeDeviceSelection::AutomaticLowestAlsaCard),
                _ => Err("requires exact or automatic_lowest_alsa_card".to_owned()),
            },
        ),
        "input_device_channels" => {
            assign_u32(&mut target.input_device_channels, 1, 2, setting, warnings)
        }
        "output_device_channels" => {
            assign_u32(&mut target.output_device_channels, 1, 2, setting, warnings)
        }
        "receive_graph" | "transmit_graph" => {
            let graph = if key == "receive_graph" {
                &mut target.receive_graph
            } else {
                &mut target.transmit_graph
            };
            if setting.value.is_empty() || setting.value.len() > 4096 {
                invalid_warning(
                    warnings,
                    setting,
                    graph.clone(),
                    "requires 1 through 4096 UTF-8 bytes".to_owned(),
                );
            } else {
                *graph = setting.value.to_owned();
            }
        }
        _ => unknown_warning(warnings, setting),
    }
}

struct Alias {
    native: &'static str,
    section: &'static str,
    key: &'static str,
    value: ValueAlias,
}

#[derive(Clone, Copy)]
enum ValueAlias {
    Identity,
    WholeDb,
    Audio,
    Carrier,
    CtcssSource,
    Output,
    Interface,
    Noise,
    Duplex,
    Turnoff,
    BooleanNumber,
    Milliseconds,
}

impl ValueAlias {
    fn pairs(self) -> &'static [(&'static str, &'static str)] {
        match self {
            Self::Audio => &[("disabled", "no")],
            Self::Carrier | Self::CtcssSource => &[
                ("disabled", "no"),
                ("cm119", "usb"),
                ("cm119_inverted", "usbinvert"),
                ("parallel", "pp"),
                ("parallel_inverted", "ppinvert"),
            ],
            Self::Output => &[
                ("disabled", "off"),
                ("tone", "ctcss"),
                ("composite", "voice_ctcss"),
                ("auxiliary_voice", "auxvoice"),
            ],
            Self::Interface => &[
                ("dudeusb", "0"),
                ("sphusb", "1"),
                ("nhrc", "2"),
                ("custom", "3"),
            ],
            Self::Noise => &[("standard", "0"), ("alternate", "1")],
            Self::Duplex => &[("half", "0"), ("full", "1")],
            Self::Turnoff => &[
                ("none", "no"),
                ("phase_shift", "ctcss_phase_shift"),
                ("tone_remove", "ctcss_tone_remove"),
                ("tail_tone", "ctcss_tail_tone"),
            ],
            Self::BooleanNumber => &[("no", "0"), ("yes", "1")],
            Self::Identity | Self::WholeDb | Self::Milliseconds => &[],
        }
    }

    fn to_shared(self, value: &str) -> String {
        if matches!(self, Self::Milliseconds) {
            return value
                .parse::<u16>()
                .map(|value| u32::from(value).div_ceil(20).to_string())
                .unwrap_or_else(|_| value.to_owned());
        }
        self.pairs()
            .iter()
            .find(|(native, _)| value.eq_ignore_ascii_case(native))
            .map_or(value, |(_, shared)| shared)
            .to_owned()
    }

    fn to_native(self, value: &str) -> String {
        if matches!(self, Self::Milliseconds) {
            return value
                .parse::<u16>()
                .map(|value| (u32::from(value) * 20).to_string())
                .unwrap_or_else(|_| value.to_owned());
        }
        self.pairs()
            .iter()
            .find(|(_, shared)| value.eq_ignore_ascii_case(shared))
            .map_or(value, |(native, _)| native)
            .to_owned()
    }
}

macro_rules! alias {
    ($native:literal, $section:literal, $key:literal, $value:ident) => {
        Alias {
            native: $native,
            section: $section,
            key: $key,
            value: ValueAlias::$value,
        }
    };
}

const ALIASES: &[Alias] = &[
    alias!(
        "device_identifier",
        "hardware",
        "hardware_device_identifier",
        Identity
    ),
    alias!("usb_serial", "hardware", "hardware_serial", Identity),
    alias!(
        "hardware_input_gain_db",
        "hardware",
        "hardware_input_gain_db",
        Identity
    ),
    alias!(
        "hardware_output_a_gain_db",
        "hardware",
        "hardware_output_a_gain_db",
        Identity
    ),
    alias!(
        "hardware_output_b_gain_db",
        "hardware",
        "hardware_output_b_gain_db",
        Identity
    ),
    alias!(
        "input_extra_buffer_ms",
        "hardware",
        "hardware_input_extra_buffer_ms",
        Identity
    ),
    alias!(
        "output_extra_buffer_ms",
        "hardware",
        "hardware_output_extra_buffer_ms",
        Identity
    ),
    alias!(
        "receive_deemphasis_corner_hz",
        "hardware",
        "hardware_deemphasis_corner_hz",
        Identity
    ),
    alias!(
        "transmit_preemphasis_corner_hz",
        "hardware",
        "hardware_preemphasis_corner_hz",
        Identity
    ),
    alias!(
        "cm119_profile",
        "hardware",
        "hardware_interface_type",
        Interface
    ),
    alias!(
        "cm119_ptt_inverted",
        "hardware",
        "hardware_ptt_inverted",
        Identity
    ),
    alias!(
        "cm119_clip_led_gpio",
        "hardware",
        "hardware_clip_led_gpio",
        Identity
    ),
    alias!(
        "hardware_eeprom_enabled",
        "hardware",
        "hardware_eeprom_enabled",
        Identity
    ),
    alias!(
        "cm119_gpio_1_mode",
        "hardware",
        "hardware_gpio_1_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_2_mode",
        "hardware",
        "hardware_gpio_2_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_3_mode",
        "hardware",
        "hardware_gpio_3_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_4_mode",
        "hardware",
        "hardware_gpio_4_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_5_mode",
        "hardware",
        "hardware_gpio_5_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_6_mode",
        "hardware",
        "hardware_gpio_6_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_7_mode",
        "hardware",
        "hardware_gpio_7_mode",
        Identity
    ),
    alias!(
        "cm119_gpio_8_mode",
        "hardware",
        "hardware_gpio_8_mode",
        Identity
    ),
    alias!("receive_audio", "receive", "audio_source", Audio),
    alias!("receive_signaling", "receive", "signaling_method", Identity),
    alias!(
        "receive_cpu_saver_enabled",
        "receive",
        "cpu_saver_enabled",
        Identity
    ),
    alias!("carrier_source", "receive", "cos_assignment", Carrier),
    alias!("squelch_level", "receive", "squelch_level", Identity),
    alias!(
        "squelch_hysteresis",
        "receive",
        "noise_squelch_hysteresis",
        Identity
    ),
    alias!("noise_filter", "receive", "noise_filter_type", Noise),
    alias!("vox_threshold", "receive", "vox_threshold", Identity),
    alias!("vox_hang_ms", "receive", "vox_hang_ms", Identity),
    alias!(
        "receive_on_delay_ms",
        "receive",
        "on_delay_frames",
        Milliseconds
    ),
    alias!("ctcss_source", "ctcss", "receive_source", CtcssSource),
    alias!(
        "receive_ctcss_tones_hz",
        "ctcss",
        "receive_frequencies",
        Identity
    ),
    alias!(
        "ctcss_decoder_gain_db",
        "ctcss",
        "receive_decoder_gain_db",
        Identity
    ),
    alias!("ctcss_relaxed", "ctcss", "receive_relax", BooleanNumber),
    alias!(
        "ctcss_override",
        "ctcss",
        "receive_override_enabled",
        Identity
    ),
    alias!(
        "transmit_ctcss_tones_hz",
        "ctcss",
        "transmit_frequencies",
        Identity
    ),
    alias!(
        "transmit_ctcss_default_hz",
        "ctcss",
        "transmit_default_hz",
        Identity
    ),
    alias!(
        "transmit_ctcss_level_dbfs",
        "ctcss",
        "transmit_peak_dbfs",
        Identity
    ),
    alias!(
        "transmit_ctcss_turnoff_mode",
        "ctcss",
        "turnoff_mode",
        Turnoff
    ),
    alias!(
        "transmit_ctcss_phase_shift_degrees",
        "ctcss",
        "phase_shift_degrees",
        Identity
    ),
    alias!(
        "transmit_ctcss_turnoff_duration_ms",
        "ctcss",
        "tail_duration_ms",
        Identity
    ),
    alias!(
        "transmit_ctcss_tail_tone_hz",
        "ctcss",
        "tail_frequency_hz",
        Identity
    ),
    alias!("dcs_receive_code", "dcs", "receive_code", Identity),
    alias!("transmit_dcs_code", "dcs", "transmit_code", Identity),
    alias!("transmit_dcs_level_dbfs", "dcs", "peak_dbfs", Identity),
    alias!(
        "transmit_dcs_turnoff_enabled",
        "dcs",
        "turnoff_code_enabled",
        Identity
    ),
    alias!(
        "transmit_dcs_turnoff_duration_ms",
        "dcs",
        "turnoff_duration_ms",
        Identity
    ),
    alias!(
        "transmit_signaling",
        "transmit",
        "signaling_method",
        Identity
    ),
    alias!(
        "transmit_cpu_saver_enabled",
        "transmit",
        "cpu_saver_enabled",
        Identity
    ),
    alias!(
        "transmit_preemphasis_enabled",
        "transmit",
        "preemphasis_enabled",
        Identity
    ),
    alias!("transmit_settle_ms", "transmit", "settle_ms", Identity),
    alias!(
        "transmit_receive_blanking_ms",
        "transmit",
        "rx_blanking_ms",
        Identity
    ),
    alias!(
        "transmit_off_delay_ms",
        "transmit",
        "off_delay_frames",
        Milliseconds
    ),
    alias!(
        "transmit_output_a_assignment",
        "hardware",
        "hardware_output_a_assignment",
        Output
    ),
    alias!(
        "transmit_output_b_assignment",
        "hardware",
        "hardware_output_b_assignment",
        Output
    ),
    alias!("radio_duplex_mode", "duplex", "duplex_radio_mode", Duplex),
    alias!("receive_input_gain_db", "local", "input_gain_db", Identity),
    alias!("receive_output_gain_db", "local", "output_gain_db", WholeDb),
    alias!(
        "status_publication_interval_ms",
        "diagnostics",
        "diagnostics_status_publication_interval_ms",
        Identity
    ),
    alias!(
        "parallel_port_device",
        "hardware",
        "hardware_parallel_port_device",
        Identity
    ),
    alias!(
        "parallel_port_base_address",
        "hardware",
        "hardware_parallel_port_base_address",
        Identity
    ),
    alias!(
        "parallel_pin_2_assignment",
        "hardware",
        "hardware_parallel_pin_2_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_3_assignment",
        "hardware",
        "hardware_parallel_pin_3_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_4_assignment",
        "hardware",
        "hardware_parallel_pin_4_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_5_assignment",
        "hardware",
        "hardware_parallel_pin_5_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_6_assignment",
        "hardware",
        "hardware_parallel_pin_6_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_7_assignment",
        "hardware",
        "hardware_parallel_pin_7_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_8_assignment",
        "hardware",
        "hardware_parallel_pin_8_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_9_assignment",
        "hardware",
        "hardware_parallel_pin_9_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_10_assignment",
        "hardware",
        "hardware_parallel_pin_10_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_12_assignment",
        "hardware",
        "hardware_parallel_pin_12_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_13_assignment",
        "hardware",
        "hardware_parallel_pin_13_assignment",
        Identity
    ),
    alias!(
        "parallel_pin_15_assignment",
        "hardware",
        "hardware_parallel_pin_15_assignment",
        Identity
    ),
];
