//! Safe endpoint ownership around the versioned canonical-F32 PCM ring.
//!
//! This private Rust component is the only `USBRadioPlus` code that knows the
//! external ring's C layout. Construction allocates on the control plane;
//! persistent producer and consumer endpoint calls only forward bounded PCM.

use std::cell::Cell;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt;
use std::mem::{offset_of, size_of};
use std::ptr::NonNull;
use std::sync::Arc;

const ABI_VERSION: u32 = 2;
const CAPABILITY: &CStr = c"rptadv.rate-adjusting-pcm-ring.f32";
const MINIMUM_CAPACITY_SAMPLES: u64 = 512;
const RESULT_OK: c_int = 0;
const RESULT_INVALID_ARGUMENT: c_int = -1;
const RESULT_NO_MEMORY: c_int = -2;

/// Converter-quality policy implemented by the external ring.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConversionQuality {
    /// Highest available sample-rate-conversion quality.
    Best = 0,
    /// Balanced conversion quality and CPU use.
    Medium = 1,
    /// Lowest-latency conversion offered by the selected adapter.
    Fastest = 2,
}

#[repr(C)]
struct Config {
    struct_size: u32,
    abi_version: u32,
    capacity_samples: u64,
    input_rate_hz: u32,
    output_rate_hz: u32,
    quality: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawObservation {
    struct_size: u32,
    abi_version: u32,
    capacity_samples: u64,
    available_samples: u64,
    reserve_samples: u64,
    filtered_occupancy_samples: u64,
    target_samples: u64,
    ratio_correction_ppm: i32,
    reserved: u32,
    discarded_samples: u64,
    missing_samples: u64,
    consecutive_shortfall_samples: u64,
    shortfall_average_milli: u64,
    adapter_error_count: u64,
}

const RAW_OBSERVATION_ABI_SIZE: u32 = 96;
const _: [(); size_of::<RawObservation>()] = [(); 96];

type CreateFn = unsafe extern "C" fn(*const Config, *mut *mut c_void) -> c_int;
type DestroyFn = unsafe extern "C" fn(*mut c_void);
type PushSampleFn = unsafe extern "C" fn(*mut c_void, f32, *mut bool) -> c_int;
type PushFn = unsafe extern "C" fn(*mut c_void, *const f32, u64, *mut u64) -> c_int;
type RenderSampleFn = unsafe extern "C" fn(*mut c_void, *mut f32, u64, *mut bool) -> c_int;
type RenderFn = unsafe extern "C" fn(*mut c_void, *mut f32, u64, u64, u64, *mut u64) -> c_int;
type ResetFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type ObserveFn = unsafe extern "C" fn(*const c_void, *mut RawObservation) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct Descriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<CreateFn>,
    destroy: Option<DestroyFn>,
    push_sample: Option<PushSampleFn>,
    push: Option<PushFn>,
    render_sample: Option<RenderSampleFn>,
    render: Option<RenderFn>,
    reset: Option<ResetFn>,
    observe: Option<ObserveFn>,
}

const REQUIRED_DESCRIPTOR_SIZE: usize =
    offset_of!(Descriptor, observe) + size_of::<Option<ObserveFn>>();

/// Ring setup or real-time operation failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RingError {
    /// Product composition supplied an incompatible descriptor.
    IncompatibleAdapter,
    /// Construction or a call used invalid rates, capacity, counts, or buffers.
    InvalidArgument,
    /// The control plane could not allocate the requested ring.
    NoMemory,
    /// The ring's sample-rate adapter was unavailable or failed.
    AdapterFailure,
}

impl fmt::Display for RingError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleAdapter => formatter.write_str("incompatible PCM-ring adapter"),
            Self::InvalidArgument => formatter.write_str("invalid PCM-ring argument"),
            Self::NoMemory => formatter.write_str("unable to allocate PCM ring"),
            Self::AdapterFailure => formatter.write_str("PCM-ring sample-rate adapter failed"),
        }
    }
}

impl std::error::Error for RingError {}

/// Individually current, non-transactional ring statistics.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RingObservation {
    /// Immutable producer-sample capacity.
    pub capacity_samples: u64,
    /// Producer samples available to the consumer.
    pub available_samples: u64,
    /// Latest caller-selected protected reserve.
    pub reserve_samples: u64,
    /// Filtered producer-sample occupancy used by clock recovery.
    pub filtered_occupancy_samples: u64,
    /// Latest caller-selected target occupancy.
    pub target_samples: u64,
    /// Applied correction relative to nominal conversion, in ppm.
    pub ratio_correction_ppm: i32,
    /// Producer samples rejected because unread storage was full.
    pub discarded_samples: u64,
    /// Consumer output samples supplied by concealment.
    pub missing_samples: u64,
    /// Current contiguous concealment run in output samples.
    pub consecutive_shortfall_samples: u64,
    /// Ten-second consecutive-shortfall average in millisamples.
    pub shortfall_average_milli: u64,
    /// Sample-rate-adapter failures after construction.
    pub adapter_error_count: u64,
}

/// Validated process-lifetime ring capability.
#[derive(Clone, Copy)]
pub struct RingProvider {
    functions: Functions,
}

#[derive(Clone, Copy)]
struct Functions {
    create: CreateFn,
    destroy: DestroyFn,
    push: PushFn,
    render: RenderFn,
    observe: ObserveFn,
}

impl RingProvider {
    /// Validate a process-lifetime descriptor supplied by product composition.
    ///
    /// # Safety
    ///
    /// `raw_descriptor` must either be null or point to immutable storage that
    /// remains valid until every endpoint created from it is dropped.
    ///
    /// # Errors
    ///
    /// Returns [`RingError::IncompatibleAdapter`] when the descriptor does not
    /// expose the required ABI and capability.
    pub unsafe fn from_raw_descriptor(raw_descriptor: *const c_void) -> Result<Self, RingError> {
        let descriptor = NonNull::new(raw_descriptor.cast_mut().cast::<Descriptor>())
            .ok_or(RingError::IncompatibleAdapter)?;
        // SAFETY: The caller promises process-lifetime readable descriptor
        // storage; only its fixed prefix is read before trusting its size.
        let descriptor = unsafe { descriptor.as_ref() };
        if (descriptor.struct_size as usize) < REQUIRED_DESCRIPTOR_SIZE
            || descriptor.abi_version != ABI_VERSION
            || descriptor.capability_name.is_null()
        {
            return Err(RingError::IncompatibleAdapter);
        }
        // SAFETY: The validated name is NUL terminated and has the same
        // process lifetime promised for the descriptor.
        if unsafe { CStr::from_ptr(descriptor.capability_name) } != CAPABILITY {
            return Err(RingError::IncompatibleAdapter);
        }
        let (
            Some(create),
            Some(destroy),
            Some(_),
            Some(push),
            Some(_),
            Some(render),
            Some(_reset),
            Some(observe),
        ) = (
            descriptor.create,
            descriptor.destroy,
            descriptor.push_sample,
            descriptor.push,
            descriptor.render_sample,
            descriptor.render,
            descriptor.reset,
            descriptor.observe,
        )
        else {
            return Err(RingError::IncompatibleAdapter);
        };
        Ok(Self {
            functions: Functions {
                create,
                destroy,
                push,
                render,
                observe,
            },
        })
    }

    /// Allocate one stopped ring and its persistent converter.
    ///
    /// # Errors
    ///
    /// Returns [`RingError::InvalidArgument`] for unsupported configuration,
    /// or the adapter error reported while creating the ring.
    pub fn prepare(
        self,
        capacity_samples: u64,
        input_rate_hz: u32,
        output_rate_hz: u32,
        quality: ConversionQuality,
    ) -> Result<PreparedRing, RingError> {
        if capacity_samples < MINIMUM_CAPACITY_SAMPLES || input_rate_hz == 0 || output_rate_hz == 0
        {
            return Err(RingError::InvalidArgument);
        }
        let ratio = f64::from(output_rate_hz) / f64::from(input_rate_hz);
        if !(1.0 / 256.0..=256.0).contains(&ratio) {
            return Err(RingError::InvalidArgument);
        }
        let config = Config {
            struct_size: u32::try_from(size_of::<Config>()).unwrap_or(u32::MAX),
            abi_version: ABI_VERSION,
            capacity_samples,
            input_rate_hz,
            output_rate_hz,
            quality: quality as u32,
        };
        let mut handle = std::ptr::null_mut();
        // SAFETY: The validated function receives one live synchronous config
        // and writable handle destination.
        let result = unsafe { (self.functions.create)(&config, &mut handle) };
        if result != RESULT_OK {
            if let Some(handle) = NonNull::new(handle) {
                // SAFETY: A non-null partial handle belongs to this descriptor.
                unsafe { (self.functions.destroy)(handle.as_ptr()) };
            }
            return Err(error_from_result(result));
        }
        let handle = NonNull::new(handle).ok_or(RingError::AdapterFailure)?;
        Ok(PreparedRing {
            inner: Arc::new(RingInner {
                provider: self,
                handle,
            }),
        })
    }
}

struct RingInner {
    provider: RingProvider,
    handle: NonNull<c_void>,
}

impl RingInner {
    fn observe(&self) -> Result<RingObservation, RingError> {
        let mut raw = RawObservation {
            struct_size: RAW_OBSERVATION_ABI_SIZE,
            ..RawObservation::default()
        };
        // SAFETY: The live ring permits concurrent observation and raw is a
        // complete writable caller-owned snapshot.
        map_result(unsafe { (self.provider.functions.observe)(self.handle.as_ptr(), &mut raw) })?;
        if raw.abi_version != ABI_VERSION {
            return Err(RingError::IncompatibleAdapter);
        }
        Ok(RingObservation {
            capacity_samples: raw.capacity_samples,
            available_samples: raw.available_samples,
            reserve_samples: raw.reserve_samples,
            filtered_occupancy_samples: raw.filtered_occupancy_samples,
            target_samples: raw.target_samples,
            ratio_correction_ppm: raw.ratio_correction_ppm,
            discarded_samples: raw.discarded_samples,
            missing_samples: raw.missing_samples,
            consecutive_shortfall_samples: raw.consecutive_shortfall_samples,
            shortfall_average_milli: raw.shortfall_average_milli,
            adapter_error_count: raw.adapter_error_count,
        })
    }
}

impl Drop for RingInner {
    fn drop(&mut self) {
        // SAFETY: The final control-plane owner drops this live handle once
        // after both real-time endpoints have stopped.
        unsafe { (self.provider.functions.destroy)(self.handle.as_ptr()) };
    }
}

// SAFETY: The external ABI provides exactly one lock-free producer and one
// lock-free consumer. Safe construction creates only those two endpoints.
unsafe impl Send for RingInner {}
// SAFETY: Producer, consumer, and observer operate on disjoint SPSC state by
// the external ABI contract; destruction waits for every endpoint to drop.
unsafe impl Sync for RingInner {}

/// A prepared ring before its two persistent endpoints are assigned.
pub struct PreparedRing {
    inner: Arc<RingInner>,
}

impl PreparedRing {
    /// Assign exactly one producer and one consumer owner.
    #[must_use]
    pub fn split(self) -> (RingProducer, RingConsumer) {
        let producer = RingProducer {
            inner: Arc::clone(&self.inner),
            _not_sync: Cell::new(()),
        };
        let consumer = RingConsumer {
            inner: self.inner,
            _not_sync: Cell::new(()),
        };
        (producer, consumer)
    }
}

/// Sole real-time producer endpoint.
pub struct RingProducer {
    inner: Arc<RingInner>,
    _not_sync: Cell<()>,
}

impl RingProducer {
    /// Append the chronological prefix that fits without waiting.
    ///
    /// # Errors
    ///
    /// Returns the adapter's operation error, or [`RingError::AdapterFailure`]
    /// if it reports accepting more samples than supplied.
    pub fn push(&mut self, input: &[f32]) -> Result<usize, RingError> {
        let samples = input.len() as u64;
        let mut accepted = 0;
        // SAFETY: The immutable input remains live for this synchronous call;
        // exclusive endpoint access preserves the single-producer contract.
        map_result(unsafe {
            (self.inner.provider.functions.push)(
                self.inner.handle.as_ptr(),
                input.as_ptr(),
                samples,
                &mut accepted,
            )
        })?;
        if accepted > samples {
            return Err(RingError::AdapterFailure);
        }
        Ok(usize::try_from(accepted).unwrap_or(input.len()))
    }
}

/// Sole real-time consumer endpoint.
pub struct RingConsumer {
    inner: Arc<RingInner>,
    _not_sync: Cell<()>,
}

impl RingConsumer {
    /// Render an arbitrary output block with persistent clock recovery.
    ///
    /// # Errors
    ///
    /// Returns the adapter's operation error, or [`RingError::AdapterFailure`]
    /// if it reports more real samples than requested.
    pub fn render(
        &mut self,
        output: &mut [f32],
        reserve_samples: u64,
        target_samples: u64,
    ) -> Result<usize, RingError> {
        let samples = output.len() as u64;
        let mut real_samples = 0;
        // SAFETY: The writable output remains live for this synchronous call;
        // exclusive endpoint access preserves the single-consumer contract.
        map_result(unsafe {
            (self.inner.provider.functions.render)(
                self.inner.handle.as_ptr(),
                output.as_mut_ptr(),
                samples,
                reserve_samples,
                target_samples,
                &mut real_samples,
            )
        })?;
        if real_samples > samples {
            return Err(RingError::AdapterFailure);
        }
        Ok(usize::try_from(real_samples).unwrap_or(output.len()))
    }

    /// Return a best-effort lock-free diagnostic snapshot.
    ///
    /// # Errors
    ///
    /// Returns the adapter's observation error or
    /// [`RingError::IncompatibleAdapter`] for an unexpected observation ABI.
    pub fn observe(&self) -> Result<RingObservation, RingError> {
        self.inner.observe()
    }
}

const fn map_result(result: c_int) -> Result<(), RingError> {
    if result == RESULT_OK {
        Ok(())
    } else {
        Err(error_from_result(result))
    }
}

const fn error_from_result(result: c_int) -> RingError {
    match result {
        RESULT_INVALID_ARGUMENT => RingError::InvalidArgument,
        RESULT_NO_MEMORY => RingError::NoMemory,
        _ => RingError::AdapterFailure,
    }
}

// SAFETY: Provider storage is immutable and process-lifetime by contract.
unsafe impl Send for RingProvider {}
// SAFETY: Provider storage is immutable and process-lifetime by contract.
unsafe impl Sync for RingProvider {}
// SAFETY: Each endpoint can move to its assigned serial owner; Cell prevents
// sharing it concurrently through safe Rust.
unsafe impl Send for RingProducer {}
// SAFETY: Each endpoint can move to its assigned serial owner; Cell prevents
// sharing it concurrently through safe Rust.
unsafe impl Send for RingConsumer {}

#[cfg(test)]
mod tests;
