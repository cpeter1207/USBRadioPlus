//! Complete typed configuration for one named USBRadioPlus channel.

use std::fmt;

use crate::{
    ChainRole, ConfigDocument, ProcessingChain, ProfileResolutionError, ResolutionWarning,
    ResolvedProfile, ResolvedStationConfig, StationConfig, StationConfigError,
};

/// Failure to resolve one complete named channel.
#[derive(Clone, Debug, PartialEq)]
pub enum ChannelConfigurationError {
    /// Station, hardware, or signaling settings could not be resolved safely.
    Station(StationConfigError),
    /// One audio-source processing profile could not be resolved safely.
    Processing(ProfileResolutionError),
}

impl fmt::Display for ChannelConfigurationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Station(error) => error.fmt(formatter),
            Self::Processing(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for ChannelConfigurationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Station(error) => Some(error),
            Self::Processing(error) => Some(error),
        }
    }
}

impl From<StationConfigError> for ChannelConfigurationError {
    fn from(error: StationConfigError) -> Self {
        Self::Station(error)
    }
}

impl From<ProfileResolutionError> for ChannelConfigurationError {
    fn from(error: ProfileResolutionError) -> Self {
        Self::Processing(error)
    }
}

/// Effective station and processing settings for one named channel.
#[derive(Clone, Debug, PartialEq)]
pub struct ChannelConfiguration {
    /// Hardware, signaling, Asterisk, duplex, and diagnostic settings.
    pub station: StationConfig,
    /// Local-receiver filtering and optional processing.
    pub local: ProcessingChain,
    /// Linked-audio optional processing.
    pub link: ProcessingChain,
    /// Voice, telemetry, and final-transmitter processing.
    pub voice_telemetry: ProcessingChain,
}

/// Fully resolved channel and its nonfatal fallback diagnostics.
#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedChannelConfiguration {
    channel: String,
    config: ChannelConfiguration,
    warnings: Vec<ResolutionWarning>,
}

impl ResolvedChannelConfiguration {
    /// Resolve all settings using compiled, flat, and selected-profile precedence.
    pub fn from_document(
        document: &ConfigDocument,
        source: impl Into<String>,
        channel: &str,
    ) -> Result<Self, ChannelConfigurationError> {
        let source = source.into();
        let station = ResolvedStationConfig::from_document(document, source.clone(), channel)?;
        let local = ResolvedProfile::from_document(
            document,
            source.clone(),
            channel,
            ChainRole::LocalReceive,
        )?;
        let link =
            ResolvedProfile::from_document(document, source.clone(), channel, ChainRole::Link)?;
        let voice_telemetry =
            ResolvedProfile::from_document(document, source, channel, ChainRole::VoiceTelemetry)?;

        let mut warnings = station.warnings().to_vec();
        warnings.extend_from_slice(local.warnings());
        warnings.extend_from_slice(link.warnings());
        warnings.extend_from_slice(voice_telemetry.warnings());
        Ok(Self {
            channel: channel.to_owned(),
            config: ChannelConfiguration {
                station: station.into_config(),
                local: local.into_chain(),
                link: link.into_chain(),
                voice_telemetry: voice_telemetry.into_chain(),
            },
            warnings,
        })
    }

    /// Return the selected named channel.
    pub fn channel(&self) -> &str {
        &self.channel
    }

    /// Return the complete effective configuration.
    pub const fn config(&self) -> &ChannelConfiguration {
        &self.config
    }

    /// Mutably borrow the effective configuration before immutable station preparation.
    ///
    /// This narrow preflight hook lets a hardware owner apply persistent tuning
    /// after canonical device selection and before any DSP or audio stream opens.
    pub fn config_mut(&mut self) -> &mut ChannelConfiguration {
        &mut self.config
    }

    /// Consume the resolution result and return its effective configuration.
    pub fn into_config(self) -> ChannelConfiguration {
        self.config
    }

    /// Return all nonfatal warnings in deterministic resolution order.
    pub fn warnings(&self) -> &[ResolutionWarning] {
        &self.warnings
    }
}

#[cfg(test)]
#[path = "tests/channel_configuration_tests.rs"]
mod tests;
