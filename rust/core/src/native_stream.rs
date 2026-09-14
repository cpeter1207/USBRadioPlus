//! Fixed-rate native-stream invariants shared by runtime objects.

use std::num::NonZeroU32;

/// Native sample rate selected for the lifetime of every current radio stream.
pub const NATIVE_SAMPLE_RATE_HZ: u32 = 48_000;

/// Failure to construct a valid fixed-rate stream.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StreamSpecError {
    /// The requested stream rate is not the supported fixed native rate.
    UnsupportedSampleRate,
    /// The adapter did not provide a nonzero maximum callback frame count.
    EmptyMaximumFrameCount,
}

/// Immutable native-stream properties fixed when the audio stream opens.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NativeStreamSpec {
    maximum_frame_count: NonZeroU32,
}

impl NativeStreamSpec {
    /// Validate the selected native rate and callback bound.
    pub fn new(sample_rate_hz: u32, maximum_frame_count: u32) -> Result<Self, StreamSpecError> {
        if sample_rate_hz != NATIVE_SAMPLE_RATE_HZ {
            return Err(StreamSpecError::UnsupportedSampleRate);
        }
        let maximum_frame_count =
            NonZeroU32::new(maximum_frame_count).ok_or(StreamSpecError::EmptyMaximumFrameCount)?;
        Ok(Self {
            maximum_frame_count,
        })
    }

    /// Return the largest frame span preallocated at stream open.
    pub const fn maximum_frame_count(self) -> u32 {
        self.maximum_frame_count.get()
    }
}

#[cfg(test)]
#[path = "tests/native_stream_tests.rs"]
mod tests;
