//! Bounded publications from the sole radio and physical owners to serial TX.

use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering, fence};

/// Configured physical radio duplex is present, independently of controller policy.
pub const FEEDBACK_DUPLEX_VALID: u32 = 1;
/// Request and radio-rendered PTT belong to the indicated request identity.
pub const FEEDBACK_REQUEST_VALID: u32 = 2;
/// A device owner completed output service; this is not electrical readback.
pub const FEEDBACK_SERVICE_VALID: u32 = 4;
/// Independent PTT readback succeeded, rather than a write being assumed observed.
pub const FEEDBACK_READBACK_VALID: u32 = 8;
/// The existing radio transmitter state is valid after the preceding render.
pub const FEEDBACK_PHASE_VALID: u32 = 16;
/// Acquisition position and age use this generation's TX sample clock.
pub const FEEDBACK_ACQUISITION_VALID: u32 = 32;

/// Immutable feedback immediately before one controller TX callback.
///
/// Positions describe completed radio rendering, not DAC presentation or RF
/// measurements. Only the existing transmitter state is exposed: fine STE,
/// receiver settling, and rekey completion are not measured by this contract.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RadioFeedback {
    /// Complete published prefix size; consumers may accept a larger record.
    pub struct_size: u32,
    /// Feedback contract, one.
    pub abi_version: u32,
    /// Radio lifecycle generation, never a controller policy generation.
    pub generation_id: u64,
    /// End of the latest successful radio render in native frames.
    pub sample_index: u64,
    /// Upcoming controller callback frame count.
    pub frame_count: u32,
    /// Independent FEEDBACK_* validity bits.
    pub flags: u32,
    /// Changes when controller demand or rendered hardware demand changes.
    pub request_id: u64,
    /// Request associated with completed service; zero if correlation is unavailable.
    pub service_request_id: u64,
    /// Request current when independent readback was acquired.
    pub readback_request_id: u64,
    /// Radio lifecycle generation of completed service; zero when absent.
    pub service_generation: u64,
    /// Radio lifecycle generation of independent readback; zero when absent.
    pub readback_generation: u64,
    /// TX position sampled by the physical owner before acquisition.
    pub acquired_sample_index: u64,
    /// Frames since that acquisition, never a restamped cached observation.
    pub acquisition_age_frames: u64,
    /// Explicit configured hardware radio duplex: half zero, full one.
    pub physical_duplex: u32,
    /// Controller demand before radio signaling tails, zero or one.
    pub requested_ptt: u32,
    /// Radio hardware demand including signaling tails, zero or one.
    pub rendered_ptt: u32,
    /// Successfully serviced output level, zero or one when valid.
    pub serviced_ptt: u32,
    /// Independently read PTT level, zero or one when valid.
    pub observed_ptt: u32,
    /// Existing radio transmitter state (0, 1, 2, 4, or 5), not fine phase.
    pub transmitter_state: i32,
    /// Optional TX-only buffering tail; the preceding physical prefix is 112 bytes.
    /// This is a conservative programme bound, never exact source provenance.
    pub programme_buffering: usbradioplus_core::ProgrammeBuffering,
    /// Matching committed fresh-history request; zero is unacknowledged/invalid.
    pub programme_retirement_id: u64,
}

/// Borrowed stopped-only binding, retained until stopped unbind/rebind or quiescent destruction.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RadioFeedbackPort {
    /// Readable prefix size; larger records are accepted without reading the tail.
    pub struct_size: u32,
    /// Binding contract, one.
    pub abi_version: u32,
    /// Borrowed independently serialized TX context, permitted to be null.
    pub context: *mut c_void,
    /// Optional bounded no-unwind callback; None unbinds while stopped.
    pub publish: Option<unsafe extern "C" fn(*mut c_void, *const RadioFeedback)>,
}

// SAFETY: binding requires the borrowed endpoint to move to the serial TX owner.
unsafe impl Send for RadioFeedbackPort {}

/// One coherent request sampled by the non-audio hardware owner.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RadioFeedbackRequest {
    /// Radio lifecycle generation.
    pub generation: u64,
    /// End of the latest successful native render.
    pub sample_index: u64,
    /// Current nonzero request identity.
    pub request_id: u64,
    /// Radio hardware demand, including its signaling tail.
    pub keyed: bool,
}

/// Single-writer atomic publication. A reader tries once, never spins on audio.
pub(crate) struct Slot<const N: usize> {
    sequence: AtomicU64,
    words: [AtomicU64; N],
}

impl<const N: usize> Default for Slot<N> {
    fn default() -> Self {
        Self {
            sequence: AtomicU64::new(0),
            words: std::array::from_fn(|_| AtomicU64::new(0)),
        }
    }
}

impl<const N: usize> Slot<N> {
    pub(crate) fn publish(&self, words: [u64; N]) {
        self.sequence.fetch_add(1, Ordering::AcqRel);
        // Any newly observed relaxed payload word pairs this release fence with
        // the reader's acquire fence. Its final sequence read must then see at
        // least the odd update and reject the in-progress publication.
        fence(Ordering::Release);
        for (destination, value) in self.words.iter().zip(words) {
            destination.store(value, Ordering::Relaxed);
        }
        self.sequence.fetch_add(1, Ordering::Release);
    }

    pub(crate) fn read(&self) -> Option<[u64; N]> {
        let before = self.sequence.load(Ordering::Acquire);
        let result = std::array::from_fn(|index| self.words[index].load(Ordering::Relaxed));
        fence(Ordering::Acquire);
        let after = self.sequence.load(Ordering::Relaxed);
        (before & 1 == 0 && before == after).then_some(result)
    }
}

#[derive(Default)]
pub(crate) struct FeedbackState {
    request: Slot<4>,
    physical: Slot<4>,
}

impl FeedbackState {
    pub(crate) fn publish_request(&self, value: RadioFeedbackRequest) {
        self.request.publish([
            value.generation,
            value.sample_index,
            value.request_id,
            u64::from(value.keyed),
        ]);
    }

    pub(crate) fn request(&self) -> Option<RadioFeedbackRequest> {
        let [generation, sample_index, request_id, keyed] = self.request.read()?;
        (generation != 0 && request_id != 0).then_some(RadioFeedbackRequest {
            generation,
            sample_index,
            request_id,
            keyed: keyed != 0,
        })
    }

    pub(crate) fn physical(
        &self,
        request: RadioFeedbackRequest,
        serviced: Option<bool>,
        observed: Option<bool>,
        acquired: bool,
    ) {
        let flags = u64::from(serviced.is_some())
            | (u64::from(serviced == Some(true)) << 1)
            | (u64::from(observed.is_some()) << 2)
            | (u64::from(observed == Some(true)) << 3)
            | (u64::from(acquired) << 4);
        self.physical.publish([
            request.generation,
            request.sample_index,
            request.request_id,
            flags,
        ]);
    }

    pub(crate) fn merge(&self, result: &mut RadioFeedback) {
        let Some([generation, position, request_id, flags]) = self.physical.read() else {
            return;
        };
        let Some(age) = result.sample_index.checked_sub(position) else {
            return;
        };
        if result.flags & FEEDBACK_REQUEST_VALID == 0
            || generation == 0
            || generation != result.generation_id
            || flags & 5 == 0
        {
            return;
        }
        if flags & 16 != 0 {
            result.flags |= FEEDBACK_ACQUISITION_VALID;
            result.acquired_sample_index = position;
            result.acquisition_age_frames = age;
        }
        if flags & 1 != 0 {
            result.flags |= FEEDBACK_SERVICE_VALID;
            result.service_generation = generation;
            result.service_request_id = request_id;
            result.serviced_ptt = u32::from(flags & 2 != 0);
        }
        if flags & 4 != 0 {
            result.flags |= FEEDBACK_READBACK_VALID;
            result.readback_generation = generation;
            result.readback_request_id = request_id;
            result.observed_ptt = u32::from(flags & 8 != 0);
        }
    }
}

#[cfg(test)]
#[path = "tests/feedback_tests.rs"]
mod tests;
