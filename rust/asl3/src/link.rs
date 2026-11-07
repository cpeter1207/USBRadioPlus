//! Per-remote-link processing shared by native PCM and Asterisk audiohooks.

use std::fmt;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::{f32_to_s16, s16_to_f32};
use usbradioplus_core::{OutputLoudnessMeter, OutputLoudnessObserver, OutputLoudnessSnapshot};

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
    /// Advance an idle clock with silence without producing link media.
    Silence,
}

/// Invalid per-link control-plane setup.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LinkError {
    /// Sample rate must be 8..384 kHz and the frame bound must be 1..4096.
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
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LinkObservation {
    /// Successfully processed blocks.
    pub processed_blocks: u64,
    /// Frames intentionally bypassed.
    pub bypassed_blocks: u64,
    /// Graph failures that retained original PCM.
    pub failed_blocks: u64,
    /// Mandatory final-output integrated loudness and true-peak measurement.
    pub loudness: OutputLoudnessSnapshot,
}

#[derive(Default)]
struct LinkCounters {
    processed_blocks: AtomicU64,
    bypassed_blocks: AtomicU64,
    failed_blocks: AtomicU64,
}

/// Immutable ownership of the existing counters, independent of graph/provider lifetime.
#[derive(Clone)]
pub struct LinkObserver {
    counters: Arc<LinkCounters>,
    loudness: OutputLoudnessObserver,
}

impl LinkObserver {
    /// Read a best-effort set of independent counters while audio runs or after graph drop.
    pub fn observe(&self) -> LinkObservation {
        LinkObservation {
            processed_blocks: self.counters.processed_blocks.load(Ordering::Relaxed),
            bypassed_blocks: self.counters.bypassed_blocks.load(Ordering::Relaxed),
            failed_blocks: self.counters.failed_blocks.load(Ordering::Relaxed),
            loudness: self.loudness.snapshot(),
        }
    }
}

/// One prepared exact-rate link graph and its callback workspaces.
///
/// One serial PCM owner retains this value. Detaching and quiescing that owner
/// before drop keeps graph lifetime out of the callback ABI.
pub struct PreparedLink {
    sample_rate_hz: u32,
    input: Box<[f32]>,
    output: Box<[f32]>,
    graph: Box<dyn ExactRateGraph>,
    meter: OutputLoudnessMeter,
    observation: LinkObserver,
}

impl PreparedLink {
    /// Allocate bounded conversion workspaces and take graph ownership on the control plane.
    pub fn new(
        sample_rate_hz: u32,
        maximum_frame_count: usize,
        graph: Box<dyn ExactRateGraph>,
    ) -> Result<Self, LinkError> {
        let meter = OutputLoudnessMeter::new(sample_rate_hz, maximum_frame_count)
            .map_err(|_| LinkError::InvalidConfiguration)?;
        let loudness = meter.observer();
        Ok(Self {
            sample_rate_hz,
            input: vec![0.0; maximum_frame_count].into_boxed_slice(),
            output: vec![0.0; maximum_frame_count].into_boxed_slice(),
            graph,
            meter,
            observation: LinkObserver {
                counters: Arc::new(LinkCounters::default()),
                loudness,
            },
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
        self.process(direction, sample_rate_hz, pcm, s16_to_f32, f32_to_s16)
    }

    /// Process one normalized native-F32 voice frame in place without quantization.
    ///
    /// The same exact-rate, read-direction and frame-bound checks as the signed-16
    /// boundary apply. Prepared workspaces retain the original PCM on graph failure.
    pub fn process_f32(
        &mut self,
        direction: LinkDirection,
        sample_rate_hz: u32,
        pcm: &mut [f32],
    ) -> LinkProcessOutcome {
        self.process(
            direction,
            sample_rate_hz,
            pcm,
            std::convert::identity,
            std::convert::identity,
        )
    }

    fn process<T: Copy>(
        &mut self,
        direction: LinkDirection,
        sample_rate_hz: u32,
        pcm: &mut [T],
        to_float: impl Fn(T) -> f32,
        from_float: impl Fn(f32) -> T,
    ) -> LinkProcessOutcome {
        if direction == LinkDirection::Write
            || sample_rate_hz != self.sample_rate_hz
            || pcm.is_empty()
            || pcm.len() > self.input.len()
        {
            self.observation
                .counters
                .bypassed_blocks
                .fetch_add(1, Ordering::Relaxed);
            return LinkProcessOutcome::Bypassed;
        }
        if direction == LinkDirection::Silence {
            let outcome = self.advance_silence(pcm.len());
            pcm.fill(from_float(0.0));
            return outcome;
        }
        for (output, input) in self.input[..pcm.len()].iter_mut().zip(pcm.iter()) {
            *output = to_float(*input);
        }
        if !self
            .graph
            .process_exact(&self.input[..pcm.len()], &mut self.output[..pcm.len()])
        {
            self.meter
                .process(&self.input[..pcm.len()])
                .expect("validated link frame bound");
            self.observation
                .counters
                .failed_blocks
                .fetch_add(1, Ordering::Relaxed);
            return LinkProcessOutcome::GraphFailed;
        }
        for ((output, measured), processed) in pcm.iter_mut().zip(&mut self.input).zip(&self.output)
        {
            *output = from_float(*processed);
            *measured = to_float(*output);
        }
        self.meter
            .process(&self.input[..pcm.len()])
            .expect("validated link frame bound");
        self.observation
            .counters
            .processed_blocks
            .fetch_add(1, Ordering::Relaxed);
        LinkProcessOutcome::Processed
    }

    /// Advance one bounded idle interval without emitting audio or resetting history.
    /// All workspaces are prepared; graph tails are discarded and silence is metered.
    pub fn advance_silence(&mut self, count: usize) -> LinkProcessOutcome {
        if count == 0 || count > self.input.len() {
            self.observation
                .counters
                .bypassed_blocks
                .fetch_add(1, Ordering::Relaxed);
            return LinkProcessOutcome::Bypassed;
        }
        self.input[..count].fill(0.0);
        let processed = self
            .graph
            .process_exact(&self.input[..count], &mut self.output[..count]);
        self.output[..count].fill(0.0);
        self.meter
            .process(&self.input[..count])
            .expect("validated idle frame bound");
        if processed {
            self.observation
                .counters
                .processed_blocks
                .fetch_add(1, Ordering::Relaxed);
            LinkProcessOutcome::Processed
        } else {
            self.observation
                .counters
                .failed_blocks
                .fetch_add(1, Ordering::Relaxed);
            LinkProcessOutcome::GraphFailed
        }
    }

    /// Read one coherent-enough set of independent diagnostic counters.
    pub fn observe(&self) -> LinkObservation {
        self.observation.observe()
    }

    /// Retain diagnostics on the control plane before transferring this graph to audio.
    pub fn observer(&self) -> LinkObserver {
        self.observation.clone()
    }
}

#[cfg(test)]
#[path = "link/tests.rs"]
mod tests;
