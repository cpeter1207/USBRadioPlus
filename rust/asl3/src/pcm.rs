//! Signed-16 PCM representation at the Asterisk boundary.

use std::fmt;

/// Samples in one 20 ms app_rpt frame at 8 kHz.
pub const APP_RPT_FRAME_SAMPLES: usize = 160;
/// Samples in one 20 ms rpt_advanced frame at 48 kHz.
pub const ADVANCED_FRAME_SAMPLES: usize = 960;
const ASSEMBLY_CAPACITY: usize = ADVANCED_FRAME_SAMPLES * 2;

/// Asterisk-facing radio-controller PCM mode.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AsteriskPcmMode {
    /// app_rpt compatibility transport at 8 kHz.
    AppRpt,
    /// rpt_advanced transport at the fixed 48 kHz native rate.
    Advanced,
}

/// One complete fixed-duration controller PCM frame.
///
/// Fixed maximum storage keeps both interfaces allocation-free without an
/// enum whose native variant is six times larger than its compatibility one.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ControllerPcmFrame {
    mode: AsteriskPcmMode,
    samples: [i16; ADVANCED_FRAME_SAMPLES],
}

impl ControllerPcmFrame {
    /// Construct a zero-filled frame for one controller interface.
    pub const fn silence(mode: AsteriskPcmMode) -> Self {
        Self {
            mode,
            samples: [0; ADVANCED_FRAME_SAMPLES],
        }
    }

    /// Construct one complete ordinary 8 kHz `app_rpt` frame.
    pub fn app_rpt(input: [i16; APP_RPT_FRAME_SAMPLES]) -> Self {
        let mut samples = [0; ADVANCED_FRAME_SAMPLES];
        samples[..APP_RPT_FRAME_SAMPLES].copy_from_slice(&input);
        Self {
            mode: AsteriskPcmMode::AppRpt,
            samples,
        }
    }

    /// Construct one complete native 48 kHz `rpt_advanced` frame.
    pub const fn advanced(samples: [i16; ADVANCED_FRAME_SAMPLES]) -> Self {
        Self {
            mode: AsteriskPcmMode::Advanced,
            samples,
        }
    }

    /// Transport represented by this frame.
    pub const fn mode(&self) -> AsteriskPcmMode {
        self.mode
    }

    /// Borrow the signed-16 samples in this frame.
    pub fn samples(&self) -> &[i16] {
        &self.samples[..self.mode.frame_samples()]
    }
}

impl AsteriskPcmMode {
    /// Sample rate supplied to Asterisk.
    pub const fn sample_rate_hz(self) -> u32 {
        match self {
            Self::AppRpt => 8_000,
            Self::Advanced => 48_000,
        }
    }

    /// Samples in one complete 20 ms Asterisk frame.
    pub const fn frame_samples(self) -> usize {
        match self {
            Self::AppRpt => APP_RPT_FRAME_SAMPLES,
            Self::Advanced => ADVANCED_FRAME_SAMPLES,
        }
    }
}

/// Invalid PCM boundary or frame-assembly operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PcmBoundaryError {
    /// Appending the supplied samples would exceed the preallocated bound.
    AssemblyOverflow,
    /// The output slice does not match the selected Asterisk frame size.
    InvalidFrameSize,
}

impl fmt::Display for PcmBoundaryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AssemblyOverflow => formatter.write_str("Asterisk PCM assembly overflow"),
            Self::InvalidFrameSize => formatter.write_str("invalid Asterisk PCM frame size"),
        }
    }
}

impl std::error::Error for PcmBoundaryError {}

/// Convert one signed-16 Asterisk sample to canonical normalized F32.
pub fn s16_to_f32(sample: i16) -> f32 {
    f32::from(sample) / 32_768.0
}

/// Quantize one canonical F32 sample at the signed-16 Asterisk boundary.
///
/// Non-finite input becomes silence. Finite samples are rounded to nearest,
/// ties to even, and saturated so no PCM value can wrap.
pub fn f32_to_s16(sample: f32) -> i16 {
    if !sample.is_finite() {
        return 0;
    }
    let scaled = f64::from(sample) * 32_768.0;
    if scaled >= f64::from(i16::MAX) {
        i16::MAX
    } else if scaled <= f64::from(i16::MIN) {
        i16::MIN
    } else {
        scaled.round_ties_even() as i16
    }
}

/// Preallocated assembly of arbitrary callback output into 20 ms Asterisk frames.
pub struct FrameAssembler {
    mode: AsteriskPcmMode,
    samples: [i16; ASSEMBLY_CAPACITY],
    count: usize,
}

impl FrameAssembler {
    /// Construct an empty assembler for one immutable transport mode.
    pub const fn new(mode: AsteriskPcmMode) -> Self {
        Self {
            mode,
            samples: [0; ASSEMBLY_CAPACITY],
            count: 0,
        }
    }

    /// Append a bounded converted span without allocating.
    pub fn append(&mut self, samples: &[i16]) -> Result<(), PcmBoundaryError> {
        let end = self.count + samples.len();
        if end > self.samples.len() {
            return Err(PcmBoundaryError::AssemblyOverflow);
        }
        self.samples[self.count..end].copy_from_slice(samples);
        self.count = end;
        Ok(())
    }

    /// Copy and remove one complete frame when available.
    pub fn take(&mut self, output: &mut [i16]) -> Result<bool, PcmBoundaryError> {
        let frame_samples = self.mode.frame_samples();
        if output.len() != frame_samples {
            return Err(PcmBoundaryError::InvalidFrameSize);
        }
        if self.count < frame_samples {
            return Ok(false);
        }
        output.copy_from_slice(&self.samples[..frame_samples]);
        self.samples.copy_within(frame_samples..self.count, 0);
        self.count -= frame_samples;
        Ok(true)
    }

    /// Copy all available samples into one zero-padded complete frame.
    ///
    /// This is used only at an established 20 ms native boundary, where a
    /// streaming converter may have a genuine startup deficit.
    pub fn take_padded(&mut self, output: &mut [i16]) -> Result<(), PcmBoundaryError> {
        let frame_samples = self.mode.frame_samples();
        if output.len() != frame_samples {
            return Err(PcmBoundaryError::InvalidFrameSize);
        }
        let copied = self.count.min(frame_samples);
        output[..copied].copy_from_slice(&self.samples[..copied]);
        output[copied..].fill(0);
        self.samples.copy_within(copied..self.count, 0);
        self.count -= copied;
        Ok(())
    }

    /// Discard a quiesced or discontinuous partial frame.
    pub fn clear(&mut self) {
        self.samples[..self.count].fill(0);
        self.count = 0;
    }

    /// Number of converted samples awaiting a complete frame.
    pub const fn pending_samples(&self) -> usize {
        self.count
    }
}

#[cfg(test)]
mod tests;
