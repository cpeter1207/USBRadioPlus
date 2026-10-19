//! Control-plane spare-pin ownership within the existing hardware service.

use std::sync::atomic::{AtomicU64, Ordering};

pub(super) static NEXT_GENERATION: AtomicU64 = AtomicU64::new(1);

fn next_generation() -> u64 {
    // Zero disables ownership if the token space is ever exhausted; never reuse a token.
    NEXT_GENERATION
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| {
            value.checked_add(1)
        })
        .unwrap_or(0)
}

/// Physical bank already owned by the station's GPIO service.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SiteBank {
    /// CM119 spare GPIO pins.
    Cm119,
    /// Configured parallel-port data/status pins.
    Parallel,
}

impl SiteBank {
    pub(super) const fn index(self) -> usize {
        match self {
            Self::Cm119 => 0,
            Self::Parallel => 1,
        }
    }
}

/// Result of the most recent physical input acquisition, not of a cached query.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum SiteReadQuality {
    /// No successful acquisition has occurred.
    #[default]
    Unavailable,
    /// The most recent acquisition succeeded.
    Valid,
    /// The most recent acquisition failed; the last-good value is retained.
    Fault,
}

/// Atomically replace a bank's site ownership on its serial hardware worker.
#[derive(Clone, Copy, Debug)]
pub struct SiteDigitalBinding {
    /// Current owner generation; a successful bind advances it.
    pub generation: u64,
    /// Spare output pins claimed by this configuration.
    pub output_mask: u32,
    /// Physical high levels for newly claimed pins only.
    pub initial_high_mask: u32,
    /// Physical high levels to apply when these pins are released.
    pub safe_high_mask: u32,
}

/// Steady output intent; pulse timing belongs to the site policy owner.
#[derive(Clone, Copy, Debug)]
pub struct SiteDigitalRequest {
    /// Generation acknowledged by the hardware owner after binding.
    pub generation: u64,
    /// Claimed pins to change; unrelated and radio-signaling pins are preserved.
    pub output_mask: u32,
    /// Physical high levels, restricted to `output_mask`.
    pub high_mask: u32,
}

/// Cached bank state. Submission is not independent physical output readback.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct SiteDigitalSnapshot {
    /// Current ownership generation.
    pub generation: u64,
    /// Configured, physically supported input pins.
    pub input_mask: u32,
    /// Configured, physically supported output pins.
    pub output_mask: u32,
    /// Pins reserved for radio signaling or another existing control function.
    pub reserved_mask: u32,
    /// Output pins exclusively claimed by the site controller.
    pub claimed_mask: u32,
    /// Latest requested physical output levels.
    pub requested_high_mask: u32,
    /// Last output levels successfully submitted to the GPIO adapter.
    pub submitted_high_mask: u32,
    /// Owner generation associated with the most recent submission.
    pub submitted_generation: u64,
    /// Generation whose submitted outputs completed a successful service cycle.
    pub serviced_generation: u64,
    /// Submitted levels at that service cycle; not independent electrical readback.
    pub serviced_high_mask: u32,
    /// Pins with genuine independent readback; zero for these GPIO providers.
    pub readback_valid_mask: u32,
    /// Last-good physical input levels.
    pub input_high_mask: u32,
    /// Latest input acquisition result.
    pub input_quality: SiteReadQuality,
    /// Last successful acquisition time on the hardware owner's monotonic clock.
    pub observed_at_ms: Option<u64>,
    /// Snapshot time on the same monotonic clock, for computing acquisition age.
    pub snapshot_at_ms: u64,
    /// Rejected commands, including commands superseded while queued.
    pub rejected_commands: u64,
    /// Most recent command rejection; retained after recovery.
    pub last_command_error: Option<SiteError>,
}

/// Invalid or superseded spare-pin command; rejection leaves outputs unchanged.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SiteError {
    /// The requested bank/pins are unavailable, reserved, or not claimed.
    Unavailable,
    /// The caller's ownership generation is no longer current.
    StaleGeneration,
    /// A value sets pins outside the declared mask.
    InvalidMask,
    /// A coherent cached snapshot is being replaced; retry on a later control poll.
    Busy,
    /// Device operation failed before ownership could be transferred.
    Fault,
}

pub(super) struct SiteBankState {
    snapshot: SiteDigitalSnapshot,
    safe_high_mask: u32,
}

impl SiteBankState {
    pub(super) fn from_snapshot(snapshot: SiteDigitalSnapshot) -> Self {
        Self {
            snapshot,
            safe_high_mask: 0,
        }
    }
    pub(super) fn new(inputs: u32, outputs: u32, reserved: u32, initial: u32) -> Self {
        Self {
            snapshot: SiteDigitalSnapshot {
                generation: next_generation(),
                input_mask: inputs,
                output_mask: outputs,
                reserved_mask: reserved,
                requested_high_mask: initial,
                ..SiteDigitalSnapshot::default()
            },
            safe_high_mask: 0,
        }
    }

    pub(super) fn bind(
        &mut self,
        binding: SiteDigitalBinding,
        output: &mut u8,
    ) -> Result<(), SiteError> {
        self.validate_binding(binding)?;
        let next = next_generation();
        if next == 0 {
            return Err(SiteError::StaleGeneration);
        }
        let removed = self.snapshot.claimed_mask & !binding.output_mask;
        let added = binding.output_mask & !self.snapshot.claimed_mask;
        let value = (u32::from(*output) & !(removed | added))
            | (self.safe_high_mask & removed)
            | (binding.initial_high_mask & added);
        *output = value as u8;
        self.snapshot.requested_high_mask = value;
        self.snapshot.claimed_mask = binding.output_mask;
        self.safe_high_mask = (self.safe_high_mask & !binding.output_mask) | binding.safe_high_mask;
        self.snapshot.generation = next;
        Ok(())
    }

    pub(super) fn validate_binding(&self, binding: SiteDigitalBinding) -> Result<(), SiteError> {
        self.validate_generation(binding.generation)?;
        if binding.output_mask & (!self.snapshot.output_mask | self.snapshot.reserved_mask) != 0 {
            return Err(SiteError::Unavailable);
        }
        if (binding.initial_high_mask | binding.safe_high_mask) & !binding.output_mask != 0 {
            return Err(SiteError::InvalidMask);
        }
        Ok(())
    }

    pub(super) fn apply(
        &mut self,
        request: SiteDigitalRequest,
        output: &mut u8,
    ) -> Result<(), SiteError> {
        self.validate_request(request)?;
        let value = (u32::from(*output) & !request.output_mask) | request.high_mask;
        *output = value as u8;
        self.snapshot.requested_high_mask = value;
        Ok(())
    }

    pub(super) fn validate_request(&self, request: SiteDigitalRequest) -> Result<(), SiteError> {
        self.validate_generation(request.generation)?;
        if request.output_mask & !self.snapshot.claimed_mask != 0 {
            return Err(SiteError::Unavailable);
        }
        if request.high_mask & !request.output_mask != 0 {
            return Err(SiteError::InvalidMask);
        }
        Ok(())
    }

    pub(super) fn reserve(&mut self, mask: u32) -> bool {
        if self.snapshot.claimed_mask & mask != 0 {
            self.reject(SiteError::Unavailable);
            false
        } else {
            self.snapshot.reserved_mask |= mask;
            self.safe_high_mask &= !mask;
            true
        }
    }

    fn validate_generation(&self, generation: u64) -> Result<(), SiteError> {
        if generation != 0 && generation == self.snapshot.generation {
            Ok(())
        } else {
            Err(SiteError::StaleGeneration)
        }
    }

    pub(super) fn permits_ordinary(&self, mask: u32) -> bool {
        // Existing control commands retain their configured authority, including
        // the clip indicator. Site ownership only excludes pins it has claimed.
        mask & (!self.snapshot.output_mask | self.snapshot.claimed_mask) == 0
    }

    /// End this ownership epoch before safe-output I/O or device replacement.
    pub(super) fn retire(&mut self, output: &mut u8) {
        *output = self.retired_output(*output);
        self.snapshot.requested_high_mask = u32::from(*output);
        self.snapshot.generation = 0;
        self.snapshot.input_quality = SiteReadQuality::Unavailable;
    }

    /// Substitute safe levels for site intent when transferring ordinary state.
    pub(super) const fn retired_output(&self, output: u8) -> u8 {
        ((output as u32 & !self.snapshot.claimed_mask) | self.safe_high_mask) as u8
    }

    /// Preserve high safe levels across recovery until another owner changes them.
    pub(super) const fn safe_high(&self) -> u8 {
        self.safe_high_mask as u8
    }

    /// Restore only unclaimed, available safe levels; keep current bindings intact.
    pub(super) fn restore_safe_high(&mut self, high: u8) {
        let retained = u32::from(high) & self.snapshot.output_mask & !self.snapshot.reserved_mask;
        self.safe_high_mask = (self.safe_high_mask & self.snapshot.claimed_mask)
            | (retained & !self.snapshot.claimed_mask);
    }

    /// An accepted ordinary command supersedes any retired site's safe level.
    pub(super) fn release_safe_high(&mut self, mask: u8) {
        self.safe_high_mask &= !u32::from(mask);
    }

    pub(super) fn observe(&mut self, value: Option<u32>, now_ms: u64) {
        if let Some(value) = value {
            self.snapshot.input_high_mask = value & self.snapshot.input_mask;
            self.snapshot.observed_at_ms = Some(now_ms);
            self.snapshot.input_quality = SiteReadQuality::Valid;
        } else {
            self.snapshot.input_quality = SiteReadQuality::Fault;
        }
    }

    pub(super) fn submitted(&mut self, value: u32) {
        self.snapshot.submitted_high_mask = value & self.snapshot.output_mask;
        self.snapshot.submitted_generation = self.snapshot.generation;
    }

    pub(super) fn serviced(&mut self, success: bool) {
        self.snapshot.serviced_generation = if success {
            self.snapshot.submitted_generation
        } else {
            0
        };
        if success {
            self.snapshot.serviced_high_mask = self.snapshot.submitted_high_mask;
        }
    }

    pub(super) fn reject(&mut self, error: SiteError) {
        self.snapshot.rejected_commands = self.snapshot.rejected_commands.saturating_add(1);
        self.snapshot.last_command_error = Some(error);
    }

    pub(super) fn snapshot(&self) -> SiteDigitalSnapshot {
        self.snapshot
    }
}

/// Single hardware writer, bounded control readers; no device wait or mutex.
#[derive(Default)]
pub(super) struct SiteCache {
    sequence: AtomicU64,
    words: [AtomicU64; 18],
}

impl SiteCache {
    /// Called only by the service owner, or before its start/after its join.
    pub(super) fn publish(&self, snapshot: SiteDigitalSnapshot, now_ms: u64) {
        let values = [
            snapshot.generation,
            u64::from(snapshot.input_mask),
            u64::from(snapshot.output_mask),
            u64::from(snapshot.reserved_mask),
            u64::from(snapshot.claimed_mask),
            u64::from(snapshot.requested_high_mask),
            u64::from(snapshot.submitted_high_mask),
            u64::from(snapshot.readback_valid_mask),
            u64::from(snapshot.input_high_mask),
            match snapshot.input_quality {
                SiteReadQuality::Unavailable => 0,
                SiteReadQuality::Valid => 1,
                SiteReadQuality::Fault => 2,
            },
            snapshot.observed_at_ms.unwrap_or(0),
            u64::from(snapshot.observed_at_ms.is_some()),
            now_ms,
            snapshot.rejected_commands,
            match snapshot.last_command_error {
                None => 0,
                Some(SiteError::Unavailable) => 1,
                Some(SiteError::StaleGeneration) => 2,
                Some(SiteError::InvalidMask) => 3,
                Some(SiteError::Busy) => 4,
                Some(SiteError::Fault) => 5,
            },
            snapshot.submitted_generation,
            snapshot.serviced_generation,
            u64::from(snapshot.serviced_high_mask),
        ];
        self.sequence.fetch_add(1, Ordering::SeqCst);
        for (word, value) in self.words.iter().zip(values) {
            word.store(value, Ordering::SeqCst);
        }
        self.sequence.fetch_add(1, Ordering::SeqCst);
    }

    pub(super) fn read(&self) -> Result<SiteDigitalSnapshot, SiteError> {
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
            return Ok(SiteDigitalSnapshot {
                generation: words[0],
                input_mask: words[1] as u32,
                output_mask: words[2] as u32,
                reserved_mask: words[3] as u32,
                claimed_mask: words[4] as u32,
                requested_high_mask: words[5] as u32,
                submitted_high_mask: words[6] as u32,
                readback_valid_mask: words[7] as u32,
                input_high_mask: words[8] as u32,
                input_quality: match words[9] {
                    1 => SiteReadQuality::Valid,
                    2 => SiteReadQuality::Fault,
                    _ => SiteReadQuality::Unavailable,
                },
                observed_at_ms: (words[11] != 0).then_some(words[10]),
                snapshot_at_ms: words[12],
                rejected_commands: words[13],
                last_command_error: match words[14] {
                    1 => Some(SiteError::Unavailable),
                    2 => Some(SiteError::StaleGeneration),
                    3 => Some(SiteError::InvalidMask),
                    4 => Some(SiteError::Busy),
                    5 => Some(SiteError::Fault),
                    _ => None,
                },
                submitted_generation: words[15],
                serviced_generation: words[16],
                serviced_high_mask: words[17] as u32,
            });
        }
        Err(SiteError::Busy)
    }
}
