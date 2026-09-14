//! Comment-preserving USBRadioPlus configuration document.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

/// Shared configuration sections that do not create radio channels.
const FLAT_SECTIONS: &[&str] = &[
    "general",
    "asterisk",
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
];

/// Configuration-document operation failure.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ConfigError {
    /// A section name was empty or contained a line delimiter or closing bracket.
    InvalidSectionName,
    /// An option name was empty or contained characters outside the established vocabulary.
    InvalidOptionName,
    /// A value contained a line delimiter and could not be represented by one assignment.
    InvalidValue,
    /// A named channel was required but was not present in the document.
    MissingChannel(String),
    /// A named channel references a profile section that does not exist.
    MissingProfile(String),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidSectionName => formatter.write_str("invalid configuration section name"),
            Self::InvalidOptionName => formatter.write_str("invalid configuration option name"),
            Self::InvalidValue => formatter.write_str("configuration values must fit on one line"),
            Self::MissingChannel(channel) => {
                write!(formatter, "missing channel section [{channel}]")
            }
            Self::MissingProfile(profile) => {
                write!(formatter, "missing profile section [{profile}]")
            }
        }
    }
}

impl std::error::Error for ConfigError {}

/// Owned, comment-preserving USBRadioPlus configuration text.
///
/// The parser recognizes only section headers and simple `name = value`
/// assignments. Unknown content remains byte-for-byte intact so the typed
/// profile resolver can warn or ignore it without the tuning utility damaging
/// operator comments.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ConfigDocument {
    text: String,
}

impl ConfigDocument {
    /// Construct a document from UTF-8 configuration text.
    pub fn new(text: impl Into<String>) -> Self {
        Self { text: text.into() }
    }

    /// Return the complete current configuration text.
    pub fn as_str(&self) -> &str {
        &self.text
    }

    /// Return exact section names in file order, omitting repeated headers.
    pub fn section_names(&self) -> Vec<String> {
        let mut seen = BTreeSet::new();
        self.lines()
            .filter_map(section_header)
            .filter(|section| seen.insert(section.to_ascii_lowercase()))
            .map(str::to_owned)
            .collect()
    }

    /// Return scoped sections whose prefix is outside the configuration schema.
    pub fn unknown_scoped_sections(&self) -> Vec<String> {
        self.section_names()
            .into_iter()
            .filter(|section| {
                section.contains(char::is_whitespace)
                    && section.split_whitespace().next().is_some_and(|kind| {
                        !FLAT_SECTIONS
                            .iter()
                            .any(|known| kind.eq_ignore_ascii_case(known))
                    })
            })
            .collect()
    }

    /// Return named radio channels in file order.
    pub fn configured_channels(&self) -> Vec<String> {
        self.section_names()
            .into_iter()
            .filter(|section| {
                !section.contains(char::is_whitespace)
                    && !FLAT_SECTIONS
                        .iter()
                        .any(|flat| section.eq_ignore_ascii_case(flat))
            })
            .collect()
    }

    /// Return explicitly assigned values from one exact section.
    ///
    /// Later duplicate assignments replace earlier assignments, matching the
    /// current tuning utility's effective-value behavior.
    pub fn explicit_values(&self, requested_section: &str) -> BTreeMap<String, String> {
        let mut values = BTreeMap::new();
        let mut active = false;
        for line in self.lines() {
            if let Some(section) = section_header(line) {
                active = section.eq_ignore_ascii_case(requested_section);
            } else if active {
                if let Some((name, value)) = assignment(line) {
                    values.insert(name.to_owned(), value.to_owned());
                }
            }
        }
        values
    }

    /// Resolve one channel's profile section or its flat fallback.
    pub fn resolved_section(&self, channel: &str, kind: &str) -> Result<String, ConfigError> {
        validate_section_name(channel)?;
        validate_section_name(kind)?;
        if !self
            .configured_channels()
            .iter()
            .any(|candidate| candidate.eq_ignore_ascii_case(channel))
        {
            return Err(ConfigError::MissingChannel(channel.to_owned()));
        }
        if kind.eq_ignore_ascii_case("general") {
            return Ok(channel.to_owned());
        }
        let selector = format!("{kind}_profile");
        let selected = self
            .explicit_values(channel)
            .into_iter()
            .find(|(name, _)| name.eq_ignore_ascii_case(&selector))
            .map_or_else(|| channel.to_owned(), |(_, value)| value);
        let scoped = format!("{kind} {selected}");
        if self.has_section(&scoped) {
            Ok(scoped)
        } else if selected.eq_ignore_ascii_case(channel) {
            Ok(kind.to_owned())
        } else {
            Err(ConfigError::MissingProfile(scoped))
        }
    }

    /// Merge flat defaults with one channel's selected profile values.
    pub fn resolved_values(
        &self,
        channel: &str,
        kind: &str,
    ) -> Result<BTreeMap<String, String>, ConfigError> {
        let resolved = self.resolved_section(channel, kind)?;
        if resolved.eq_ignore_ascii_case(channel) {
            return Ok(self.explicit_values(channel));
        }
        let mut values = self.explicit_values(kind);
        if !resolved.eq_ignore_ascii_case(kind) {
            values.extend(self.explicit_values(&resolved));
        }
        Ok(values)
    }

    /// Set one explicit assignment while preserving unrelated text and comments.
    ///
    /// Existing assignments are replaced in place. Missing flat or scoped
    /// profile sections are appended; a missing named channel is rejected.
    pub fn set_value(&mut self, section: &str, name: &str, value: &str) -> Result<(), ConfigError> {
        validate_section_name(section)?;
        validate_option_name(name)?;
        validate_value(value)?;
        let mut lines = split_inclusive_lines(&self.text);
        let mut active = false;
        let mut section_index = None;
        let mut section_end = lines.len();
        for index in 0..lines.len() {
            if let Some(found) = section_header(&lines[index]) {
                if active {
                    section_end = index;
                    break;
                }
                active = found.eq_ignore_ascii_case(section);
                if active {
                    section_index = Some(index);
                }
            } else if active {
                if let Some((found, _)) = assignment(&lines[index]) {
                    if found.eq_ignore_ascii_case(name) {
                        let ending = line_ending(&lines[index]);
                        lines[index] = format!("{name} = {value}{ending}");
                        self.text = lines.concat();
                        return Ok(());
                    }
                }
            }
        }
        if let Some(index) = section_index {
            lines.insert(section_end.max(index + 1), format!("{name} = {value}\n"));
            self.text = lines.concat();
            return Ok(());
        }
        let creatable = section.contains(char::is_whitespace)
            || FLAT_SECTIONS
                .iter()
                .any(|flat| section.eq_ignore_ascii_case(flat));
        if !creatable {
            return Err(ConfigError::MissingChannel(section.to_owned()));
        }
        if !self.text.is_empty() && !self.text.ends_with('\n') {
            self.text.push('\n');
        }
        if !self.text.is_empty() && !self.text.ends_with("\n\n") {
            self.text.push('\n');
        }
        self.text
            .push_str(&format!("[{section}]\n{name} = {value}\n"));
        Ok(())
    }

    /// Remove the first matching explicit assignment from one exact section.
    pub fn remove_value(&mut self, section: &str, name: &str) -> Result<bool, ConfigError> {
        validate_section_name(section)?;
        validate_option_name(name)?;
        let mut lines = split_inclusive_lines(&self.text);
        let mut active = false;
        for index in 0..lines.len() {
            if let Some(found) = section_header(&lines[index]) {
                active = found.eq_ignore_ascii_case(section);
            } else if active {
                if let Some((found, _)) = assignment(&lines[index]) {
                    if found.eq_ignore_ascii_case(name) {
                        lines.remove(index);
                        self.text = lines.concat();
                        return Ok(true);
                    }
                }
            }
        }
        Ok(false)
    }

    fn lines(&self) -> impl Iterator<Item = &str> {
        self.text.lines()
    }

    fn has_section(&self, requested: &str) -> bool {
        self.lines()
            .filter_map(section_header)
            .any(|section| section.eq_ignore_ascii_case(requested))
    }
}

fn validate_section_name(section: &str) -> Result<(), ConfigError> {
    if section.trim().is_empty() || section.contains([']', '\r', '\n']) {
        Err(ConfigError::InvalidSectionName)
    } else {
        Ok(())
    }
}

fn validate_option_name(name: &str) -> Result<(), ConfigError> {
    if name.is_empty()
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        Err(ConfigError::InvalidOptionName)
    } else {
        Ok(())
    }
}

fn validate_value(value: &str) -> Result<(), ConfigError> {
    if value.contains(['\r', '\n']) {
        Err(ConfigError::InvalidValue)
    } else {
        Ok(())
    }
}

fn section_header(line: &str) -> Option<&str> {
    let trimmed = line.trim_start();
    let rest = trimmed.strip_prefix('[')?;
    let closing = rest.find(']')?;
    let section = rest[..closing].trim();
    (!section.is_empty()).then_some(section)
}

fn assignment(line: &str) -> Option<(&str, &str)> {
    let trimmed = line.trim_start();
    if trimmed.starts_with([';', '#', '[']) {
        return None;
    }
    let (name, remainder) = trimmed.split_once('=')?;
    let name = name.trim();
    if validate_option_name(name).is_err() {
        return None;
    }
    let value = remainder
        .split([';', '#'])
        .next()
        .unwrap_or_default()
        .trim();
    Some((name, value))
}

fn split_inclusive_lines(text: &str) -> Vec<String> {
    if text.is_empty() {
        return Vec::new();
    }
    text.split_inclusive('\n').map(str::to_owned).collect()
}

fn line_ending(line: &str) -> &'static str {
    if line.ends_with("\r\n") {
        "\r\n"
    } else if line.ends_with('\n') {
        "\n"
    } else {
        ""
    }
}

#[cfg(test)]
#[path = "tests/config_document_tests.rs"]
mod tests;
