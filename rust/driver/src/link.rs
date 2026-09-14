//! ASL3 link-audio processing prepared outside the audiohook callback.

use std::ffi::CString;
use std::fmt;

use usbradioplus_asl3::{ExactRateGraph, LinkError, PreparedLink};
use usbradioplus_core::{
    ChainRole, GraphDescriptionError, GraphDescriptionFactory, ProcessingChain,
};
use usbradioplus_ffmpeg::{GraphError, GraphProvider, PreparedGraph};

const GRAPH_WARMUP_BLOCKS: usize = 8;

/// Failure to prepare one negotiated-rate ASL3 link processor.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum LinkPreparationError {
    /// Only the link processing profile is valid at this boundary.
    IncorrectRole,
    /// The configured graph could not be described.
    Description(GraphDescriptionError),
    /// The graph text contained an interior NUL byte.
    InvalidDescription,
    /// The external FFmpeg adapter rejected preparation or warm-up.
    Graph(GraphError),
    /// The link audiohook bounds were invalid.
    Link(LinkError),
}

impl fmt::Display for LinkPreparationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncorrectRole => formatter.write_str("processing profile is not a link profile"),
            Self::Description(error) => write!(formatter, "link graph description failed: {error}"),
            Self::InvalidDescription => formatter.write_str("link graph contains a NUL byte"),
            Self::Graph(error) => write!(formatter, "link graph preparation failed: {error}"),
            Self::Link(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for LinkPreparationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::IncorrectRole | Self::InvalidDescription => None,
            Self::Description(error) => Some(error),
            Self::Graph(error) => Some(error),
            Self::Link(error) => Some(error),
        }
    }
}

impl From<GraphDescriptionError> for LinkPreparationError {
    fn from(error: GraphDescriptionError) -> Self {
        Self::Description(error)
    }
}

impl From<GraphError> for LinkPreparationError {
    fn from(error: GraphError) -> Self {
        Self::Graph(error)
    }
}

impl From<LinkError> for LinkPreparationError {
    fn from(error: LinkError) -> Self {
        Self::Link(error)
    }
}

/// Reusable control-plane factory for Asterisk link audiohooks.
#[derive(Clone)]
pub struct LinkProcessingFactory {
    descriptions: GraphDescriptionFactory,
    provider: GraphProvider,
}

impl LinkProcessingFactory {
    /// Construct a factory using the installed Rust AGC LADSPA effect.
    ///
    /// # Errors
    ///
    /// Returns an error when the plug-in path cannot be represented safely in
    /// an FFmpeg graph description.
    pub fn new(
        provider: GraphProvider,
        agc_plugin_path: impl Into<String>,
    ) -> Result<Self, LinkPreparationError> {
        Ok(Self {
            descriptions: GraphDescriptionFactory::new(agc_plugin_path)?,
            provider,
        })
    }

    /// Prepare and warm one graph at the negotiated Asterisk link rate.
    ///
    /// The returned object owns its exact-rate graph and bounded conversion
    /// workspaces. Its audiohook operation performs no allocation, locking,
    /// logging, or I/O.
    ///
    /// # Errors
    ///
    /// Returns a typed error for the wrong profile role, invalid rate or frame
    /// bound, graph construction, external-adapter setup, or warm-up failure.
    pub fn prepare(
        &self,
        profile: &ProcessingChain,
        sample_rate_hz: u32,
        maximum_frame_count: u32,
    ) -> Result<PreparedLink, LinkPreparationError> {
        if profile.role != ChainRole::Link {
            return Err(LinkPreparationError::IncorrectRole);
        }
        if sample_rate_hz == 0 || maximum_frame_count == 0 {
            return Err(LinkError::InvalidConfiguration.into());
        }
        let description = CString::new(self.descriptions.dynamics(profile)?)
            .map_err(|_| LinkPreparationError::InvalidDescription)?;
        let mut graph =
            self.provider
                .prepare_at_rate(&description, sample_rate_hz, maximum_frame_count)?;
        let maximum = maximum_frame_count as usize;
        let silence = vec![0.0; maximum];
        let mut output = vec![0.0; maximum];
        graph.warm_up(&silence, &mut output, GRAPH_WARMUP_BLOCKS)?;
        Ok(
            PreparedLink::new(sample_rate_hz, maximum, Box::new(LinkGraph(graph)))
                .expect("nonzero link rate and frame bound were validated"),
        )
    }
}

struct LinkGraph(PreparedGraph);

impl ExactRateGraph for LinkGraph {
    fn process_exact(&mut self, input: &[f32], output: &mut [f32]) -> bool {
        self.0.process_block(input, output).is_ok()
    }
}

#[cfg(test)]
#[path = "tests/link_tests.rs"]
mod tests;
