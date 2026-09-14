//! Per-remote-link processing owned behind an Asterisk audiohook.

use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::{f32_to_s16, s16_to_f32};

/// Exact-rate graph operation prepared by product composition.
///
/// Implementations own their graph handle and must process synchronously
/// without allocation, locking, logging, or I/O.
pub trait ExactRateGraph: Send {
    /// Process one nonempty block into an equally sized output workspace.
    fn process_exact(&mut self, input: &[f32], output: &mut [f32]) -> bool;
}

/// Asterisk audiohook stream direction after ABI translation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LinkDirection {
    /// Audio read from the remote link before controller mixing.
    Read,
    /// Audio written toward the remote link.
    Write,
}

/// Invalid per-link control-plane setup.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LinkError {
    /// Sample rate and maximum frame count must both be positive.
    InvalidConfiguration,
}

impl fmt::Display for LinkError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("invalid ASL3 link-processor configuration")
    }
}

impl std::error::Error for LinkError {}

/// Result of one synchronous audiohook operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LinkProcessOutcome {
    /// Read audio was processed in place by the prepared graph.
    Processed,
    /// The frame was outside the prepared direction, rate, or size.
    Bypassed,
    /// The prepared graph rejected the block and original PCM was retained.
    GraphFailed,
}

/// Lock-free counters readable by diagnostics while an audiohook is active.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct LinkObservation {
    /// Successfully processed blocks.
    pub processed_blocks: u64,
    /// Frames intentionally bypassed.
    pub bypassed_blocks: u64,
    /// Graph failures that retained original PCM.
    pub failed_blocks: u64,
}

/// One prepared exact-rate link graph and its callback workspaces.
///
/// The Asterisk datastore owns this value. Detaching and quiescing the
/// audiohook before drop keeps graph lifetime out of the callback ABI.
pub struct PreparedLink {
    sample_rate_hz: u32,
    input: Box<[f32]>,
    output: Box<[f32]>,
    graph: Box<dyn ExactRateGraph>,
    processed_blocks: AtomicU64,
    bypassed_blocks: AtomicU64,
    failed_blocks: AtomicU64,
}

impl PreparedLink {
    /// Allocate bounded conversion workspaces and take graph ownership on the control plane.
    pub fn new(
        sample_rate_hz: u32,
        maximum_frame_count: usize,
        graph: Box<dyn ExactRateGraph>,
    ) -> Result<Self, LinkError> {
        if sample_rate_hz == 0 || maximum_frame_count == 0 {
            return Err(LinkError::InvalidConfiguration);
        }
        Ok(Self {
            sample_rate_hz,
            input: vec![0.0; maximum_frame_count].into_boxed_slice(),
            output: vec![0.0; maximum_frame_count].into_boxed_slice(),
            graph,
            processed_blocks: AtomicU64::new(0),
            bypassed_blocks: AtomicU64::new(0),
            failed_blocks: AtomicU64::new(0),
        })
    }

    /// Process one translated signed-16 voice frame in place.
    ///
    /// Only read-direction, nonempty frames at the prepared exact rate and
    /// within the prepared bound reach the graph. Conversion and graph work
    /// use only control-plane-allocated storage. A failure leaves `pcm` intact.
    pub fn process_s16(
        &mut self,
        direction: LinkDirection,
        sample_rate_hz: u32,
        pcm: &mut [i16],
    ) -> LinkProcessOutcome {
        if direction != LinkDirection::Read
            || sample_rate_hz != self.sample_rate_hz
            || pcm.is_empty()
            || pcm.len() > self.input.len()
        {
            self.bypassed_blocks.fetch_add(1, Ordering::Relaxed);
            return LinkProcessOutcome::Bypassed;
        }
        for (output, input) in self.input[..pcm.len()].iter_mut().zip(pcm.iter()) {
            *output = s16_to_f32(*input);
        }
        if !self
            .graph
            .process_exact(&self.input[..pcm.len()], &mut self.output[..pcm.len()])
        {
            self.failed_blocks.fetch_add(1, Ordering::Relaxed);
            return LinkProcessOutcome::GraphFailed;
        }
        for (output, processed) in pcm.iter_mut().zip(&self.output) {
            *output = f32_to_s16(*processed);
        }
        self.processed_blocks.fetch_add(1, Ordering::Relaxed);
        LinkProcessOutcome::Processed
    }

    /// Read one coherent-enough set of independent diagnostic counters.
    pub fn observe(&self) -> LinkObservation {
        LinkObservation {
            processed_blocks: self.processed_blocks.load(Ordering::Relaxed),
            bypassed_blocks: self.bypassed_blocks.load(Ordering::Relaxed),
            failed_blocks: self.failed_blocks.load(Ordering::Relaxed),
        }
    }
}

#[cfg(test)]
#[path = "link/tests.rs"]
mod tests;
