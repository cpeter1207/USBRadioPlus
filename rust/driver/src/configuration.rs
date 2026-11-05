//! Complete configuration catalogue for one module generation.

use std::fmt;

use usbradioplus_core::{
    ChannelConfigurationError, ConfigDocument, RemoteConfigurationError, ResolutionWarning,
    ResolutionWarningKind, ResolvedChannelConfiguration, ResolvedRemoteConfiguration,
};

/// Failure to resolve a complete driver configuration generation.
#[derive(Clone, Debug, PartialEq)]
pub enum DriverConfigurationError {
    /// The file did not define a named radio channel.
    NoChannels,
    /// One named channel could not be resolved.
    Channel {
        /// Name of the channel whose configuration failed.
        name: String,
        /// Typed resolver failure.
        source: Box<ChannelConfigurationError>,
    },
    /// An enabled remote radio has no safe complete configuration.
    Remote {
        /// Owning channel name.
        name: String,
        /// Shared resolver failure, with transport credentials omitted.
        source: Box<RemoteConfigurationError>,
    },
}

impl fmt::Display for DriverConfigurationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoChannels => formatter.write_str("configuration contains no radio channels"),
            Self::Channel { name, source } => {
                write!(formatter, "channel [{name}] is invalid: {source}")
            }
            Self::Remote { name, source } => {
                write!(formatter, "remote radio [{name}] is invalid: {source}")
            }
        }
    }
}

impl std::error::Error for DriverConfigurationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::NoChannels => None,
            Self::Channel { source, .. } => Some(source),
            Self::Remote { source, .. } => Some(source),
        }
    }
}

/// Fully resolved named channels for one immutable module generation.
#[derive(Clone, Debug, PartialEq)]
pub struct DriverConfiguration {
    channels: Vec<ResolvedChannelConfiguration>,
    remote: Vec<(String, ResolvedRemoteConfiguration)>,
    warnings: Vec<ResolutionWarning>,
}

impl DriverConfiguration {
    /// Parse and resolve every named channel in deterministic file order.
    ///
    /// Flat sections remain fallback defaults. Profile sections are not
    /// channels and are resolved only when a named channel references them.
    ///
    /// # Errors
    ///
    /// Returns [`DriverConfigurationError::NoChannels`] for a file containing
    /// only defaults, or identifies the first invalid named channel.
    pub fn parse(
        source: impl Into<String>,
        text: impl Into<String>,
    ) -> Result<Self, DriverConfigurationError> {
        let source = source.into();
        let document = ConfigDocument::new(text);
        let names = document.configured_channels();
        if names.is_empty() {
            return Err(DriverConfigurationError::NoChannels);
        }
        let channels = names
            .into_iter()
            .map(|name| {
                ResolvedChannelConfiguration::from_document(&document, source.clone(), &name)
                    .map_err(|error| DriverConfigurationError::Channel {
                        name,
                        source: Box::new(error),
                    })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let mut remote = Vec::new();
        for channel in &channels {
            if let Some(configuration) = ResolvedRemoteConfiguration::from_document(
                &document,
                &source,
                channel.channel(),
                &channel.config().station,
            )
            .map_err(|error| DriverConfigurationError::Remote {
                name: channel.channel().to_owned(),
                source: Box::new(error),
            })? {
                remote.push((channel.channel().to_owned(), configuration));
            }
        }
        let mut warnings = document
            .unknown_scoped_sections()
            .into_iter()
            .map(|section| ResolutionWarning {
                kind: ResolutionWarningKind::UnknownSection,
                source: source.clone(),
                section: section.clone(),
                name: "section".to_owned(),
                supplied_value: section,
                fallback: "ignored".to_owned(),
                reason: "is not recognized".to_owned(),
            })
            .collect::<Vec<_>>();
        warnings.extend(
            channels
                .iter()
                .flat_map(|channel| channel.warnings().iter().cloned()),
        );
        warnings.extend(
            remote
                .iter()
                .flat_map(|(_, configuration)| configuration.warnings.iter().cloned()),
        );
        Ok(Self {
            channels,
            remote,
            warnings,
        })
    }

    /// Resolved channels in deterministic file order.
    pub fn channels(&self) -> &[ResolvedChannelConfiguration] {
        &self.channels
    }

    /// Shared remote configuration retained before reservation/device acquisition.
    /// A disabled or ordinary channel has no remote composition or provider load.
    pub fn remote(&self, name: &str) -> Option<&ResolvedRemoteConfiguration> {
        self.remote
            .iter()
            .find(|(channel, _)| channel.eq_ignore_ascii_case(name))
            .map(|(_, configuration)| configuration)
    }

    /// All nonfatal resolution warnings in deterministic reporting order.
    pub fn warnings(&self) -> &[ResolutionWarning] {
        &self.warnings
    }

    /// Find a configured channel using Asterisk's case-insensitive names.
    pub fn channel(&self, name: &str) -> Option<&ResolvedChannelConfiguration> {
        self.channels
            .iter()
            .find(|channel| channel.channel().eq_ignore_ascii_case(name))
    }
}

#[cfg(test)]
#[path = "tests/configuration_tests.rs"]
mod tests;
