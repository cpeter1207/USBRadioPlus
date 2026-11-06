//! Composition-specific configuration resolution for the shared tuner menus.
//!
//! Implementations use their composition's shared resolver. This boundary does
//! not interpret radio aliases, reproduce defaults, or own calibration logic.

use std::collections::BTreeMap;

use usbradioplus_core::{
    ChainRole, ConfigDocument, ResolutionWarning, ResolutionWarningKind,
    ResolvedNativeConfiguration, ResolvedProfile, ResolvedStationConfig, StationConfig,
    native_configured_nodes, native_radio_setting_destination, native_radio_setting_value,
};

/// Exact file assignment selected for one canonical menu setting.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SettingDestination {
    /// Actual configuration section, including its profile or node scope.
    pub section: String,
    /// Actual configuration option name in that composition.
    pub key: String,
}

/// Configuration interpretation supplied by the selected radio composition.
///
/// Section values use the existing catalog's canonical keys. Destinations use
/// actual file names, so calibration and typed editors share the same mapping.
pub trait ConfigurationResolver {
    /// Whether menus must omit controls belonging to the Asterisk composition.
    fn standalone(&self) -> bool {
        false
    }
    /// Return configured radio identities accepted by the runtime backend.
    fn configured_channels(&self, document: &ConfigDocument) -> Vec<String>;

    /// Resolve station settings through the shared configuration owner.
    ///
    /// # Errors
    /// Returns the shared resolver's configuration error.
    fn station_config(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: &str,
    ) -> Result<StationConfig, String>;

    /// Resolve effective canonical values and their destination section.
    ///
    /// # Errors
    /// Returns an error when the channel or section cannot be resolved.
    fn section_values(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        warnings: &mut Vec<ResolutionWarning>,
    ) -> Result<(String, BTreeMap<String, String>), String>;

    /// Resolve the exact destination for one canonical setting.
    ///
    /// # Errors
    /// Returns an error when the destination cannot be resolved safely.
    fn setting_destination(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        key: &str,
        warnings: &mut Vec<ResolutionWarning>,
    ) -> Result<SettingDestination, String> {
        let (section, _) = self.section_values(document, source, channel, section, warnings)?;
        Ok(SettingDestination {
            section,
            key: key.to_owned(),
        })
    }

    /// Encode a canonical menu value for the composition's actual assignment.
    fn setting_value(
        &self,
        _section: &str,
        _key: &str,
        _destination: &SettingDestination,
        value: &str,
    ) -> String {
        value.to_owned()
    }

    /// Validate the actual edited assignment using the shared resolver.
    ///
    /// # Errors
    /// Returns a diagnostic when the edited value is rejected or falls back.
    fn validate_candidate(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        key: &str,
    ) -> Result<(), String>;
}

/// Existing ASL3 configuration scopes, inheritance, and validation.
#[derive(Clone, Copy, Debug, Default)]
pub struct AslConfiguration;

impl ConfigurationResolver for AslConfiguration {
    fn configured_channels(&self, document: &ConfigDocument) -> Vec<String> {
        document.configured_channels()
    }

    fn station_config(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: &str,
    ) -> Result<StationConfig, String> {
        ResolvedStationConfig::from_document(document, source, channel)
            .map(ResolvedStationConfig::into_config)
            .map_err(|error| error.to_string())
    }

    fn section_values(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        warnings: &mut Vec<ResolutionWarning>,
    ) -> Result<(String, BTreeMap<String, String>), String> {
        let Some(channel) = channel else {
            return Ok((section.to_owned(), document.explicit_values(section)));
        };
        let target = document
            .resolved_section(channel, section, source, warnings)
            .map_err(|error| error.to_string())?;
        let mut values = document
            .resolved_values(channel, section, source, warnings)
            .map_err(|error| error.to_string())?;
        if let Some(role) = processing_role(section) {
            if let Ok(resolved) = ResolvedProfile::from_document(document, source, channel, role) {
                apply_warning_fallbacks(&mut values, resolved.warnings());
            }
        } else if let Ok(resolved) = ResolvedStationConfig::from_document(document, source, channel)
        {
            apply_warning_fallbacks(&mut values, resolved.warnings());
        }
        Ok((target, values))
    }

    fn validate_candidate(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        target: &str,
        key: &str,
    ) -> Result<(), String> {
        let Some(channel) = channel else {
            return Ok(());
        };
        if let Some(role) = processing_role(target) {
            let resolved = ResolvedProfile::from_document(document, source, channel, role)
                .map_err(|error| error.to_string())?;
            reject_setting_warning(resolved.warnings(), target, key)
        } else {
            let resolved = ResolvedStationConfig::from_document(document, source, channel)
                .map_err(|error| error.to_string())?;
            reject_setting_warning(resolved.warnings(), target, key)
        }
    }
}

/// Standalone radio aliases and profiles resolved by the common core owner.
#[derive(Clone, Copy, Debug, Default)]
pub struct NativeConfiguration;

impl ConfigurationResolver for NativeConfiguration {
    fn standalone(&self) -> bool {
        true
    }
    fn configured_channels(&self, document: &ConfigDocument) -> Vec<String> {
        native_configured_nodes(document)
    }

    fn station_config(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: &str,
    ) -> Result<StationConfig, String> {
        ResolvedNativeConfiguration::from_document(document, source, channel)
            .map(|resolved| resolved.into_config().station)
            .map_err(|error| error.to_string())
    }

    fn section_values(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        warnings: &mut Vec<ResolutionWarning>,
    ) -> Result<(String, BTreeMap<String, String>), String> {
        let channel = channel
            .ok_or_else(|| "select a radio node before editing its configuration".to_owned())?;
        let resolved = ResolvedNativeConfiguration::from_document(document, source, channel)
            .map_err(|error| error.to_string())?;
        let (target, mut values) = AslConfiguration.section_values(
            resolved.normalized_document(),
            source,
            Some(channel),
            section,
            &mut Vec::new(),
        )?;
        apply_warning_fallbacks(&mut values, resolved.shared_warnings());
        warnings.extend_from_slice(resolved.warnings());
        Ok((target, values))
    }

    fn setting_destination(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        key: &str,
        warnings: &mut Vec<ResolutionWarning>,
    ) -> Result<SettingDestination, String> {
        let channel = channel
            .ok_or_else(|| "select a radio node before editing its configuration".to_owned())?;
        let resolved = ResolvedNativeConfiguration::from_document(document, source, channel)
            .map_err(|error| error.to_string())?;
        let shared = resolved
            .normalized_document()
            .resolved_section(channel, section, source, warnings)
            .map_err(|error| error.to_string())?;
        if document
            .section_names()
            .iter()
            .any(|name| name.eq_ignore_ascii_case(&shared))
        {
            return Ok(SettingDestination {
                section: shared,
                key: key.to_owned(),
            });
        }
        if let Some(alias) = native_radio_setting_destination(section, key) {
            let scoped = format!("radio {channel}");
            let target = document
                .section_names()
                .into_iter()
                .find(|name| name.eq_ignore_ascii_case(&scoped))
                .unwrap_or_else(|| "radio".to_owned());
            return Ok(SettingDestination {
                section: target,
                key: alias.to_owned(),
            });
        }
        AslConfiguration.setting_destination(
            document,
            source,
            Some(channel),
            section,
            key,
            warnings,
        )
    }

    fn setting_value(
        &self,
        section: &str,
        key: &str,
        destination: &SettingDestination,
        value: &str,
    ) -> String {
        if destination
            .section
            .split_whitespace()
            .next()
            .is_some_and(|kind| kind.eq_ignore_ascii_case("radio"))
        {
            native_radio_setting_value(section, key, value).unwrap_or_else(|| value.to_owned())
        } else {
            value.to_owned()
        }
    }

    fn validate_candidate(
        &self,
        document: &ConfigDocument,
        source: &str,
        channel: Option<&str>,
        section: &str,
        key: &str,
    ) -> Result<(), String> {
        let channel = channel
            .ok_or_else(|| "select a radio node before editing its configuration".to_owned())?;
        let resolved = ResolvedNativeConfiguration::from_document(document, source, channel)
            .map_err(|error| error.to_string())?;
        reject_setting_warning(resolved.warnings(), section, key)
    }
}

pub(crate) fn processing_role(section: &str) -> Option<ChainRole> {
    let kind = section.split_once(' ').map_or(section, |(kind, _)| kind);
    match kind {
        "local" => Some(ChainRole::LocalReceive),
        "link" => Some(ChainRole::Link),
        "voice_telemetry" => Some(ChainRole::VoiceTelemetry),
        _ => None,
    }
}

pub(crate) fn reject_setting_warning(
    warnings: &[ResolutionWarning],
    target: &str,
    key: &str,
) -> Result<(), String> {
    warnings
        .iter()
        .find(|warning| {
            warning.section.eq_ignore_ascii_case(target)
                && warning.name.eq_ignore_ascii_case(key)
                && warning.kind != ResolutionWarningKind::UnknownOption
        })
        .map_or(Ok(()), |warning| Err(warning.to_string()))
}

pub(crate) fn apply_warning_fallbacks(
    values: &mut BTreeMap<String, String>,
    warnings: &[ResolutionWarning],
) {
    for warning in warnings {
        if warning.kind != ResolutionWarningKind::UnknownOption
            && values
                .get(&warning.name)
                .is_some_and(|value| value == &warning.supplied_value)
        {
            values.insert(warning.name.clone(), warning.fallback.clone());
        }
    }
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;

    #[test]
    fn vad_edit_destinations_and_fallbacks_match_each_composition() {
        for (resolver, text, node, target) in [
            (
                &AslConfiguration as &dyn ConfigurationResolver,
                "[usb]\n[receive usb]\ncos_assignment=no\nvad_mode=kerchunk\nvad_hang_ms=32768\n",
                "usb",
                "receive usb",
            ),
            (
                &NativeConfiguration as &dyn ConfigurationResolver,
                "[1000]\n[radio 1000]\ncarrier_source=disabled\nvad_mode=kerchunk\nvad_hang_ms=32768\n",
                "1000",
                "radio 1000",
            ),
        ] {
            let document = ConfigDocument::new(text);
            for key in ["vad_mode", "vad_hang_ms"] {
                let destination = resolver
                    .setting_destination(
                        &document,
                        "vad.conf",
                        Some(node),
                        "receive",
                        key,
                        &mut Vec::new(),
                    )
                    .unwrap();
                assert_eq!(destination.section, target);
                assert_eq!(destination.key, key);
                assert!(
                    resolver
                        .validate_candidate(&document, "vad.conf", Some(node), target, key)
                        .unwrap_err()
                        .contains(key)
                );
                assert_eq!(
                    resolver.setting_value("receive", key, &destination, "on"),
                    "on"
                );
            }
            let (_, values) = resolver
                .section_values(
                    &document,
                    "vad.conf",
                    Some(node),
                    "receive",
                    &mut Vec::new(),
                )
                .unwrap();
            assert_eq!(values["vad_mode"], "on");
            assert_eq!(values["vad_hang_ms"], "1000");
        }
    }

    #[test]
    fn native_menu_values_use_native_defaults_and_shared_invalid_value_fallbacks() {
        let document = ConfigDocument::new(
            "[1000]\n[radio]\nsquelch_level=650\n[radio 1000]\nsquelch_level=1000\n",
        );
        let (_, hardware) = NativeConfiguration
            .section_values(
                &document,
                "rpt.conf",
                Some("1000"),
                "hardware",
                &mut Vec::new(),
            )
            .unwrap();
        assert_eq!(hardware["hardware_output_a_assignment"], "voice");
        assert_eq!(hardware["hardware_output_b_assignment"], "ctcss");
        assert_eq!(hardware["hardware_eeprom_enabled"], "no");
        let (_, receive) = NativeConfiguration
            .section_values(
                &document,
                "rpt.conf",
                Some("1000"),
                "receive",
                &mut Vec::new(),
            )
            .unwrap();
        assert_eq!(receive["squelch_level"], "650");
        assert!(
            NativeConfiguration
                .validate_candidate(
                    &document,
                    "rpt.conf",
                    Some("1000"),
                    "radio 1000",
                    "squelch_level"
                )
                .unwrap_err()
                .contains("squelch_level")
        );
    }

    #[test]
    fn native_profile_destinations_and_values_reuse_the_core_alias_table() {
        for (text, section, key, want_section, want_key, value, encoded) in [
            (
                "[1000]\n",
                "local",
                "compressor_enabled",
                "local",
                "compressor_enabled",
                "true",
                "true",
            ),
            (
                "[1000]\n[radio 1000]\n",
                "receive",
                "on_delay_frames",
                "radio 1000",
                "receive_on_delay_ms",
                "3",
                "60",
            ),
            (
                "[1000]\n",
                "hardware",
                "hardware_output_a_assignment",
                "radio",
                "transmit_output_a_assignment",
                "voice_ctcss",
                "composite",
            ),
            (
                "[1000]\nhardware_profile=site\n[hardware site]\n",
                "hardware",
                "hardware_output_a_assignment",
                "hardware site",
                "hardware_output_a_assignment",
                "voice_ctcss",
                "voice_ctcss",
            ),
            (
                "[1000]\nlocal_profile=studio\n[local studio]\n",
                "local",
                "input_gain_db",
                "local studio",
                "input_gain_db",
                "6",
                "6",
            ),
            (
                "[1000]\n",
                "local",
                "input_gain_db",
                "radio",
                "receive_input_gain_db",
                "6",
                "6",
            ),
            (
                "[1000]\nhardware_profile=missing\n[radio 1000]\nhardware_input_gain_db=-2\n",
                "hardware",
                "hardware_input_gain_db",
                "radio 1000",
                "hardware_input_gain_db",
                "0",
                "0",
            ),
            (
                "[1000]\n[hardware]\nhardware_input_gain_db=-4\n[radio 1000]\nhardware_input_gain_db=-2\n",
                "hardware",
                "hardware_input_gain_db",
                "radio 1000",
                "hardware_input_gain_db",
                "0",
                "0",
            ),
            (
                "[1000]\n[hardware 1000]\nhardware_output_a_assignment=voice\n[radio 1000]\ntransmit_output_a_assignment=composite\n",
                "hardware",
                "hardware_output_a_assignment",
                "hardware 1000",
                "hardware_output_a_assignment",
                "voice_ctcss",
                "voice_ctcss",
            ),
        ] {
            let document = ConfigDocument::new(text);
            let destination = NativeConfiguration
                .setting_destination(
                    &document,
                    "rpt.conf",
                    Some("1000"),
                    section,
                    key,
                    &mut Vec::new(),
                )
                .unwrap();
            assert_eq!(destination.section, want_section);
            assert_eq!(destination.key, want_key);
            assert_eq!(
                NativeConfiguration.setting_value(section, key, &destination, value),
                encoded
            );
        }
    }
}
