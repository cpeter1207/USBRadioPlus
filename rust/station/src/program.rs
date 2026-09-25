//! Sole transmitter program-ring composition.

use std::cell::UnsafeCell;
use std::ffi::{c_int, c_void};
use std::fmt;
use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

use usbradioplus_asl3::{
    CtcssToneIndex as AslCtcssToneIndex, ProgramProducer, ProgramProducerError,
    ReceiveQualification,
};
use usbradioplus_radio::{
    CtcssToneIndex as RadioCtcssToneIndex, ProgramRingPort, ProgramRingResult,
    RingObservation as RadioRingObservation,
};
use usbradioplus_ring::{
    PlcMode, RingConsumer, RingError, RingObservation, RingProducer, RingProvider, RingSettings,
};

use crate::ControllerTransport;

const NATIVE_RATE_HZ: u32 = 48_000;
const APP_RPT_RATE_HZ: u32 = 8_000;
const RESERVE_MILLISECONDS: u64 = 20;
const TARGET_MILLISECONDS: u64 = 40;
const CAPACITY_MILLISECONDS: u64 = 80;
const PROVIDER_OK: c_int = 0;
const PROVIDER_FAILED: c_int = -1;

const CARRIER_BIT: u32 = 1 << 0;
const SUBAUDIBLE_BIT: u32 = 1 << 1;
const KEYED_BIT: u32 = 1 << 2;
const DCS_BIT: u32 = 1 << 3;
const TONE_SHIFT: u32 = 4;

/// Immutable ring settings for one controller interface.
pub type ProgramRingPlan = RingSettings;

/// Resolve immutable buffering and callback bounds for one ASL3 interface.
/// app_rpt reserves 162 input samples for a drift-adjusted 960-sample native
/// callback and enables PLC. Advanced fallback retains its 20/40/80 ms policy
/// without PLC; attached direct callbacks continue to bypass this ring.
#[must_use]
pub const fn program_ring_plan(transport: ControllerTransport) -> ProgramRingPlan {
    let input_rate_hz = match transport {
        ControllerTransport::AppRpt => APP_RPT_RATE_HZ,
        ControllerTransport::RptAdvanced => NATIVE_RATE_HZ,
    };
    ProgramRingPlan {
        capacity_samples: milliseconds_to_samples(input_rate_hz, CAPACITY_MILLISECONDS),
        input_rate_hz,
        output_rate_hz: NATIVE_RATE_HZ,
        reserve_samples: match transport {
            ControllerTransport::AppRpt => 162,
            ControllerTransport::RptAdvanced => {
                milliseconds_to_samples(input_rate_hz, RESERVE_MILLISECONDS)
            }
        },
        target_samples: milliseconds_to_samples(input_rate_hz, TARGET_MILLISECONDS),
        max_producer_samples: milliseconds_to_samples(input_rate_hz, 20),
        max_output_samples: 960,
        plc_mode: match transport {
            ControllerTransport::AppRpt => PlcMode::G711AppendixI,
            ControllerTransport::RptAdvanced => PlcMode::Disabled,
        },
    }
}

const fn milliseconds_to_samples(rate_hz: u32, milliseconds: u64) -> u64 {
    (rate_hz as u64 * milliseconds).div_ceil(1_000)
}

/// Program-ring setup failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProgramRingSetupError(pub RingError);

impl fmt::Display for ProgramRingSetupError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "unable to prepare transmitter program ring: {}",
            self.0
        )
    }
}

impl std::error::Error for ProgramRingSetupError {}

/// Prepare the sole program ring and assign its serialized producer and
/// hardware-paced consumer owners.
pub fn prepare_program_ring(
    provider: RingProvider,
    plan: ProgramRingPlan,
) -> Result<(Box<dyn ProgramProducer>, ProgramRingConsumer), ProgramRingSetupError> {
    let prepared = provider.prepare(plan).map_err(ProgramRingSetupError)?;
    let (producer, consumer) = prepared.split();
    let qualification = Arc::new(AtomicU32::new(0));
    Ok((
        Box::new(ControllerProgramProducer {
            ring: producer,
            qualification: Arc::clone(&qualification),
        }),
        ProgramRingConsumer {
            ring: consumer,
            qualification,
            nominal_ratio: f64::from(plan.output_rate_hz) / f64::from(plan.input_rate_hz),
            direct: Arc::new(DirectProgram::default()),
        },
    ))
}

struct ControllerProgramProducer {
    ring: RingProducer,
    qualification: Arc<AtomicU32>,
}

impl ProgramProducer for ControllerProgramProducer {
    fn push(
        &mut self,
        input: &[f32],
        receive: ReceiveQualification,
    ) -> Result<usize, ProgramProducerError> {
        let accepted = self.ring.push(input).map_err(|_| ProgramProducerError)?;
        if accepted != 0 {
            self.qualification
                .store(pack_qualification(receive), Ordering::Release);
        }
        Ok(accepted)
    }
}

/// Hardware-paced consumer for the sole transmitter program ring.
pub struct ProgramRingConsumer {
    ring: RingConsumer,
    pub(crate) qualification: Arc<AtomicU32>,
    nominal_ratio: f64,
    direct: Arc<DirectProgram>,
}

/// Separate allocation shared by the serial TX staging owner and its radio port.
/// The consumer remains exclusively borrowed by the radio session. All source
/// access uses shared references; only this cell's contents are mutated. Clones
/// are made during preparation, never from an audio callback.
#[derive(Default)]
pub(crate) struct DirectProgram {
    block: UnsafeCell<Option<(Box<[f32]>, usize)>>,
}

// SAFETY: all content access is unsafe and requires the sole TX owner's serial
// staging/render contract. No borrow survives a call or overlaps another access.
unsafe impl Sync for DirectProgram {}

impl DirectProgram {
    /// Allocate before callbacks begin, with no concurrent source access.
    pub(crate) unsafe fn prepare(&self, maximum_frames: usize) {
        // SAFETY: the caller owns the quiescent preparation phase.
        unsafe {
            *self.block.get() = Some((vec![0.0; maximum_frames].into_boxed_slice(), 0));
        }
    }

    /// Stage within the sole TX owner, without overlapping any source render.
    pub(crate) unsafe fn stage(&self, samples: &[f32]) {
        // SAFETY: the caller serializes staging against all source access.
        let block = unsafe { &mut *self.block.get() };
        let (buffer, count) = block.as_mut().expect("direct source was prepared");
        buffer[..samples.len()].copy_from_slice(samples);
        *count = samples.len();
    }

    unsafe fn render(&self, output: &mut [f32]) -> Option<Result<(), RingError>> {
        // SAFETY: the radio's sole TX owner calls this after staging has returned.
        let (samples, count) = unsafe { &*self.block.get() }.as_ref()?;
        Some(if *count == output.len() {
            output.copy_from_slice(&samples[..*count]);
            Ok(())
        } else {
            Err(RingError::InvalidArgument)
        })
    }
}

impl ProgramRingConsumer {
    pub(crate) fn direct_source(&self) -> Arc<DirectProgram> {
        Arc::clone(&self.direct)
    }

    /// Borrow this stable consumer as a prepared radio-core callback port.
    ///
    /// The returned port prevents safe movement or destruction of this owner
    /// for the lifetime of the radio session that borrows it.
    pub fn radio_port(&mut self) -> ProgramRingPort<'_> {
        let context = NonNull::from(self).cast::<c_void>();
        // SAFETY: the returned port borrows this unique consumer, so its
        // context remains valid and exclusively assigned to the TX callback.
        unsafe { ProgramRingPort::from_raw(context, render_program, None) }
    }

    fn render(
        &mut self,
        output: &mut [f32],
    ) -> Result<(RingObservation, ReceiveQualification), RingError> {
        // SAFETY: the radio owns this consumer exclusively; staging uses its
        // independent source allocation and must finish before radio rendering.
        if let Some(result) = unsafe { self.direct.render(output) } {
            result?;
            return Ok((
                RingObservation::default(),
                unpack_qualification(self.qualification.load(Ordering::Acquire)),
            ));
        }
        self.ring.render(output)?;
        let observation = self.ring.observe()?;
        let qualification = unpack_qualification(self.qualification.load(Ordering::Acquire));
        Ok((observation, qualification))
    }
}

unsafe extern "C" fn render_program(
    context: *mut c_void,
    output: *mut f32,
    frame_count: u32,
    result: *mut ProgramRingResult,
) -> c_int {
    let (Some(mut context), Some(mut output), Some(result)) = (
        NonNull::new(context).map(NonNull::cast::<ProgramRingConsumer>),
        NonNull::new(output),
        NonNull::new(result),
    ) else {
        return PROVIDER_FAILED;
    };
    let frame_count = frame_count as usize;
    // SAFETY: the radio ABI supplies exactly frame_count writable samples and
    // serializes this callback through the sole transmit owner.
    let output = unsafe { std::slice::from_raw_parts_mut(output.as_mut(), frame_count) };
    output.fill(0.0);
    // SAFETY: construction binds the pointer to one live, uniquely assigned
    // ProgramRingConsumer for the complete borrow of the radio session.
    let consumer = unsafe { context.as_mut() };
    let Ok((observation, qualification)) = consumer.render(output) else {
        return PROVIDER_FAILED;
    };
    let tone = qualification
        .ctcss_decoded
        .and_then(|value| RadioCtcssToneIndex::new(value.get()));
    let ring = radio_observation(observation, consumer.nominal_ratio);
    // SAFETY: result is a live, aligned writable destination supplied by the
    // radio ABI for this synchronous call.
    unsafe {
        result.as_ptr().write(ProgramRingResult::new(
            ring,
            qualification.receiver_keyed,
            tone,
            qualification.dcs_valid,
        ));
    }
    PROVIDER_OK
}

fn radio_observation(value: RingObservation, nominal_ratio: f64) -> RadioRingObservation {
    RadioRingObservation {
        occupancy_frames: saturating_u32(value.available_samples),
        reserve_frames: saturating_u32(value.reserve_samples),
        target_frames: saturating_u32(value.target_samples),
        capacity_frames: saturating_u32(value.capacity_samples),
        ratio: nominal_ratio * (1.0 + f64::from(value.ratio_correction_ppm) / 1_000_000.0),
        underrun_samples: value.missing_samples,
        overrun_samples: value.discarded_samples,
        concealment_samples: value.missing_samples,
    }
}

fn saturating_u32(value: u64) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

pub(crate) fn pack_qualification(value: ReceiveQualification) -> u32 {
    let mut packed = 0;
    if value.carrier_active {
        packed |= CARRIER_BIT;
    }
    if value.subaudible_active {
        packed |= SUBAUDIBLE_BIT;
    }
    if value.receiver_keyed {
        packed |= KEYED_BIT;
    }
    if value.dcs_valid {
        packed |= DCS_BIT;
    }
    if let Some(tone) = value.ctcss_decoded {
        packed |= (u32::from(tone.get()) + 1) << TONE_SHIFT;
    }
    packed
}

fn unpack_qualification(value: u32) -> ReceiveQualification {
    let encoded_tone = value >> TONE_SHIFT;
    ReceiveQualification {
        carrier_active: value & CARRIER_BIT != 0,
        subaudible_active: value & SUBAUDIBLE_BIT != 0,
        receiver_keyed: value & KEYED_BIT != 0,
        ctcss_decoded: encoded_tone
            .checked_sub(1)
            .and_then(|tone| u8::try_from(tone).ok())
            .and_then(AslCtcssToneIndex::new),
        dcs_valid: value & DCS_BIT != 0,
    }
}

#[cfg(test)]
#[path = "tests/program_tests.rs"]
pub(crate) mod tests;
