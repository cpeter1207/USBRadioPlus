// SPDX-License-Identifier: GPL-2.0-only
//! Integrated mono output loudness and true-peak observation without changing audio.
use ebur128::{EbuR128, Mode};
use std::fmt;
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

static NEXT_EPOCH: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// Failure to prepare or feed the bounded output analyzer.
pub enum OutputLoudnessError {
    /// Sample rate is outside 8 through 384 kHz.
    UnsupportedSampleRate,
    /// Prepared frame bound is outside 1 through 4096 samples.
    InvalidMaximumFrameCount,
    /// Input exceeds the bound selected at setup.
    FrameTooLarge,
}
impl fmt::Display for OutputLoudnessError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::UnsupportedSampleRate => {
                "output meter sample rate must be 8000 through 384000 Hz"
            }
            Self::InvalidMaximumFrameCount => {
                "output meter maximum frame count must be 1 through 4096"
            }
            Self::FrameTooLarge => "output meter input exceeds its prepared frame bound",
        })
    }
}
impl std::error::Error for OutputLoudnessError {}

#[derive(Clone, Copy, Debug, PartialEq)]
/// One coherent published measurement epoch; absent values are not measured.
pub struct OutputLoudnessSnapshot {
    /// Identifier changed only by preparation or an explicit reset.
    pub epoch: u64,
    /// Input samples accumulated in this epoch, including silence.
    pub sample_count: u64,
    /// Gated integrated loudness; absent before 400 ms or without accepted energy.
    pub integrated_lufs: Option<f64>,
    /// Maximum estimated true peak; absent before 400 ms or without a positive peak.
    pub true_peak_dbtp: Option<f64>,
}
#[derive(Debug)]
struct Published {
    sequence: AtomicU64,
    words: [AtomicU64; 4],
}
impl Published {
    fn new(epoch: u64) -> Self {
        Self {
            sequence: AtomicU64::new(0),
            words: [
                AtomicU64::new(epoch),
                AtomicU64::new(0),
                AtomicU64::new(f64::NAN.to_bits()),
                AtomicU64::new(f64::NAN.to_bits()),
            ],
        }
    }
    fn publish(&self, snapshot: OutputLoudnessSnapshot) {
        let values = [
            snapshot.epoch,
            snapshot.sample_count,
            snapshot.integrated_lufs.unwrap_or(f64::NAN).to_bits(),
            snapshot.true_peak_dbtp.unwrap_or(f64::NAN).to_bits(),
        ];
        self.sequence.fetch_add(1, Ordering::SeqCst);
        for (word, value) in self.words.iter().zip(values) {
            word.store(value, Ordering::SeqCst);
        }
        self.sequence.fetch_add(1, Ordering::SeqCst);
    }
    fn snapshot(&self) -> OutputLoudnessSnapshot {
        for _ in 0..3 {
            let sequence = self.sequence.load(Ordering::SeqCst);
            if sequence & 1 != 0 {
                continue;
            }
            let words = self
                .words
                .each_ref()
                .map(|word| word.load(Ordering::SeqCst));
            if self.sequence.load(Ordering::SeqCst) != sequence {
                continue;
            }
            let integrated = f64::from_bits(words[2]);
            let peak = f64::from_bits(words[3]);
            return OutputLoudnessSnapshot {
                epoch: words[0],
                sample_count: words[1],
                integrated_lufs: integrated.is_finite().then_some(integrated),
                true_peak_dbtp: peak.is_finite().then_some(peak),
            };
        }
        OutputLoudnessSnapshot {
            epoch: self.words[0].load(Ordering::SeqCst),
            sample_count: 0,
            integrated_lufs: None,
            true_peak_dbtp: None,
        }
    }
}
#[derive(Clone, Debug)]
/// Control-side lease retaining measurement state independently of the audio owner.
pub struct OutputLoudnessObserver {
    published: Arc<Published>,
}
impl OutputLoudnessObserver {
    /// Read a coherent snapshot, or unmeasured state if a writer stays busy.
    pub fn snapshot(&self) -> OutputLoudnessSnapshot {
        self.published.snapshot()
    }
}
#[derive(Debug)]
/// Single-owner mono analyzer with scratch and histogram storage prepared at setup.
pub struct OutputLoudnessMeter {
    meter: EbuR128,
    scratch: Box<[f32]>,
    rate: u32,
    samples: u64,
    phase: usize,
    integrated: Option<f64>,
    epoch: u64,
    published: Arc<Published>,
}
impl OutputLoudnessMeter {
    /// Prepare an epoch and storage before audio callbacks start.
    ///
    /// # Errors
    /// Rejects unsupported rates or frame bounds.
    pub fn new(rate: u32, maximum_samples: usize) -> Result<Self, OutputLoudnessError> {
        if !(8000..=384000).contains(&rate) {
            return Err(OutputLoudnessError::UnsupportedSampleRate);
        }
        if !(1..=4096).contains(&maximum_samples) {
            return Err(OutputLoudnessError::InvalidMaximumFrameCount);
        }
        let meter = EbuR128::new(1, rate, Mode::I | Mode::TRUE_PEAK | Mode::HISTOGRAM)
            .expect("bounded mono output meter");
        let epoch = NEXT_EPOCH.fetch_add(1, Ordering::Relaxed);
        Ok(Self {
            meter,
            scratch: vec![0.0; maximum_samples].into_boxed_slice(),
            rate,
            samples: 0,
            phase: 0,
            integrated: None,
            epoch,
            published: Arc::new(Published::new(epoch)),
        })
    }
    /// Clone a control-side observer without allocating another measurement history.
    pub fn observer(&self) -> OutputLoudnessObserver {
        OutputLoudnessObserver {
            published: Arc::clone(&self.published),
        }
    }
    /// Start a new unmeasured epoch without reallocating; never reset merely for silence.
    pub fn reset(&mut self) {
        self.meter.reset();
        self.samples = 0;
        self.phase = 0;
        self.integrated = None;
        self.epoch = NEXT_EPOCH.fetch_add(1, Ordering::Relaxed);
        self.published.publish(OutputLoudnessSnapshot {
            epoch: self.epoch,
            sample_count: 0,
            integrated_lufs: None,
            true_peak_dbtp: None,
        });
    }
    /// Observe final output, replacing nonfinite analysis inputs with silence.
    /// Callers exclude setup warmup and choose actual output/epoch boundaries.
    /// Program samples are untouched. Gated loudness is queried on 100 ms crossings;
    /// sample count and maximum true peak are published for every nonempty block.
    ///
    /// # Errors
    /// Rejects oversized input without changing measurement state.
    pub fn process(&mut self, samples: &[f32]) -> Result<(), OutputLoudnessError> {
        if samples.len() > self.scratch.len() {
            return Err(OutputLoudnessError::FrameTooLarge);
        }
        if samples.is_empty() {
            return Ok(());
        }
        let length = samples.len();
        let prefix = &mut self.scratch[..length];
        for (i, &sample) in samples.iter().enumerate() {
            prefix[i] = if sample.is_finite() { sample } else { 0.0 };
        }
        self.meter
            .add_frames_f32(prefix)
            .expect("prepared mono meter");
        self.samples = self.samples.saturating_add(length as u64);
        self.phase += length;
        let interval = (self.rate as usize + 5) / 10;
        if self.phase >= interval {
            self.phase %= interval;
            if self.samples >= 4 * interval as u64 {
                let value = self
                    .meter
                    .loudness_global()
                    .expect("prepared integrated mode");
                self.integrated = value.is_finite().then_some(value);
            }
        }
        let peak = if self.samples >= 4 * interval as u64 {
            let amplitude = self.meter.true_peak(0).expect("prepared mono true peak");
            if amplitude.is_finite() && amplitude > 0.0 {
                Some(20.0 * amplitude.log10())
            } else {
                None
            }
        } else {
            None
        };
        self.published.publish(OutputLoudnessSnapshot {
            epoch: self.epoch,
            sample_count: self.samples,
            integrated_lufs: self.integrated,
            true_peak_dbtp: peak,
        });
        Ok(())
    }
}

#[cfg(test)]
#[path = "tests/loudness_meter_tests.rs"]
mod tests;
