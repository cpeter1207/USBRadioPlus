//! Callback-partition assembly around the fixed-frame `RNNoise` adapter.

#![cfg_attr(coverage, feature(coverage_attribute))]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt;
use std::mem::{offset_of, size_of};
use std::ptr::NonNull;

const RNNOISE_FRAME_COUNT: usize = 480;
const RNNOISE_FRAME_COUNT_U32: u32 = 480;
const ABI_VERSION: u32 = 1;
const CAPABILITY: &CStr = c"rptadv.rnnoise";
const RESULT_OK: c_int = 0;

type CreateFn = unsafe extern "C" fn(u32, u32, u32, *mut *mut c_void) -> c_int;
type ProcessFn = unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, *mut f32) -> c_int;
type DestroyFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
struct Descriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<CreateFn>,
    process: Option<ProcessFn>,
    destroy: Option<DestroyFn>,
}

const REQUIRED_DESCRIPTOR_SIZE: usize =
    offset_of!(Descriptor, destroy) + size_of::<Option<DestroyFn>>();

/// `RNNoise` adapter or stream-processing failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DenoiseError {
    /// Product composition supplied an incompatible descriptor.
    IncompatibleAdapter,
    /// The external adapter could not create or process its state.
    AdapterFailure,
}

impl fmt::Display for DenoiseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleAdapter => formatter.write_str("incompatible RNNoise adapter"),
            Self::AdapterFailure => formatter.write_str("RNNoise adapter operation failed"),
        }
    }
}

impl std::error::Error for DenoiseError {}

/// Validated process-lifetime `RNNoise` capability.
#[derive(Clone, Copy)]
pub struct DenoiseProvider {
    create: CreateFn,
    process: ProcessFn,
    destroy: DestroyFn,
}

impl DenoiseProvider {
    /// Validate a process-lifetime descriptor supplied by product composition.
    ///
    /// # Safety
    ///
    /// `raw_descriptor` must either be null or point to immutable storage that
    /// remains valid until every provider and stream made from it is dropped.
    ///
    /// # Errors
    ///
    /// Returns [`DenoiseError::IncompatibleAdapter`] when the descriptor does
    /// not expose the required ABI and capability.
    pub unsafe fn from_raw_descriptor(raw_descriptor: *const c_void) -> Result<Self, DenoiseError> {
        let descriptor = NonNull::new(raw_descriptor.cast_mut().cast::<Descriptor>())
            .ok_or(DenoiseError::IncompatibleAdapter)?;
        // SAFETY: The caller promises process-lifetime readable descriptor
        // storage; only its fixed prefix is read before the size is trusted.
        let descriptor = unsafe { descriptor.as_ref() };
        if (descriptor.struct_size as usize) < REQUIRED_DESCRIPTOR_SIZE
            || descriptor.abi_version != ABI_VERSION
            || descriptor.capability_name.is_null()
        {
            return Err(DenoiseError::IncompatibleAdapter);
        }
        // SAFETY: A validated descriptor owns this NUL-terminated name for the
        // same process lifetime promised by the caller.
        if unsafe { CStr::from_ptr(descriptor.capability_name) } != CAPABILITY {
            return Err(DenoiseError::IncompatibleAdapter);
        }
        let (Some(create), Some(process), Some(destroy)) =
            (descriptor.create, descriptor.process, descriptor.destroy)
        else {
            return Err(DenoiseError::IncompatibleAdapter);
        };
        Ok(Self {
            create,
            process,
            destroy,
        })
    }

    /// Allocate one prepared 48 kHz mono stream on the control plane.
    ///
    /// # Errors
    ///
    /// Returns [`DenoiseError::AdapterFailure`] if creation or warm-up fails.
    pub fn prepare(self) -> Result<DenoiseStream, DenoiseError> {
        let mut handle = std::ptr::null_mut();
        // SAFETY: The validated function receives the fixed ABI properties and
        // a live destination pointer for this synchronous setup operation.
        let result = unsafe { (self.create)(48_000, 1, RNNOISE_FRAME_COUNT_U32, &mut handle) };
        let Some(handle) = NonNull::new(handle) else {
            return Err(DenoiseError::AdapterFailure);
        };
        if result != RESULT_OK {
            // SAFETY: A non-null partial handle belongs to this provider.
            unsafe { (self.destroy)(handle.as_ptr()) };
            return Err(DenoiseError::AdapterFailure);
        }
        let silence = [0.0; RNNOISE_FRAME_COUNT];
        let mut output = [0.0; RNNOISE_FRAME_COUNT];
        let mut vad_probability = 0.0;
        for _ in 0..2 {
            // SAFETY: Setup owns the new handle exclusively and supplies exact
            // fixed-size buffers. Warm-up occurs before callback publication.
            let result = unsafe {
                (self.process)(
                    handle.as_ptr(),
                    silence.as_ptr(),
                    RNNOISE_FRAME_COUNT_U32,
                    output.as_mut_ptr(),
                    &mut vad_probability,
                )
            };
            if result != RESULT_OK {
                // SAFETY: This setup path still owns the live handle.
                unsafe { (self.destroy)(handle.as_ptr()) };
                return Err(DenoiseError::AdapterFailure);
            }
        }
        Ok(DenoiseStream {
            provider: self,
            handle,
            input: [0.0; RNNOISE_FRAME_COUNT],
            input_count: 0,
            output: [0.0; RNNOISE_FRAME_COUNT],
            output_index: 0,
            output_count: 0,
            scratch: [0.0; RNNOISE_FRAME_COUNT],
            active: false,
            primed: false,
        })
    }
}

/// Prepared denoiser plus callback-independent input/output assembly.
pub struct DenoiseStream {
    provider: DenoiseProvider,
    handle: NonNull<c_void>,
    input: [f32; RNNOISE_FRAME_COUNT],
    input_count: usize,
    output: [f32; RNNOISE_FRAME_COUNT],
    output_index: usize,
    output_count: usize,
    scratch: [f32; RNNOISE_FRAME_COUNT],
    active: bool,
    primed: bool,
}

impl DenoiseStream {
    /// Process an arbitrary native callback partition in place.
    ///
    /// This operation performs no allocation, locking, I/O, or logging. It
    /// retains the established two-live-frame startup policy independently of
    /// callback partitioning.
    ///
    /// # Errors
    ///
    /// Returns [`DenoiseError::AdapterFailure`] if frame processing fails.
    pub fn process(&mut self, samples: &mut [f32]) -> Result<(), DenoiseError> {
        self.active = true;
        let mut offset = 0;
        while offset < samples.len() {
            let span = (RNNOISE_FRAME_COUNT - self.input_count).min(samples.len() - offset);
            self.input[self.input_count..self.input_count + span]
                .copy_from_slice(&samples[offset..offset + span]);
            if self.output_count >= span {
                samples[offset..offset + span]
                    .copy_from_slice(&self.output[self.output_index..self.output_index + span]);
                self.output_index += span;
                self.output_count -= span;
            } else {
                samples[offset..offset + span].fill(0.0);
            }
            self.input_count += span;
            offset += span;
            if self.input_count == RNNOISE_FRAME_COUNT {
                self.process_complete_frame()?;
            }
        }
        Ok(())
    }

    /// Bypass live framing while retaining the prepared `RNNoise` model history.
    pub fn bypass(&mut self) {
        if self.active {
            self.input_count = 0;
            self.output_index = 0;
            self.output_count = 0;
            self.active = false;
            self.primed = false;
        }
    }

    fn process_complete_frame(&mut self) -> Result<(), DenoiseError> {
        let mut vad_probability = 0.0;
        // SAFETY: Fixed arrays exactly satisfy the adapter frame contract and
        // the exclusively borrowed handle serializes access.
        let result = unsafe {
            (self.provider.process)(
                self.handle.as_ptr(),
                self.input.as_ptr(),
                RNNOISE_FRAME_COUNT_U32,
                self.scratch.as_mut_ptr(),
                &mut vad_probability,
            )
        };
        self.input_count = 0;
        if result != RESULT_OK {
            return Err(DenoiseError::AdapterFailure);
        }
        if self.primed {
            self.output.copy_from_slice(&self.scratch);
            self.output_index = 0;
            self.output_count = RNNOISE_FRAME_COUNT;
        } else {
            self.primed = true;
        }
        Ok(())
    }
}

impl Drop for DenoiseStream {
    fn drop(&mut self) {
        // SAFETY: This stream owns the handle and Drop runs once after
        // exclusive real-time access has ceased.
        unsafe { (self.provider.destroy)(self.handle.as_ptr()) };
    }
}

// SAFETY: Provider storage is immutable and process-lifetime by contract.
unsafe impl Send for DenoiseProvider {}
// SAFETY: Provider storage is immutable and process-lifetime by contract.
unsafe impl Sync for DenoiseProvider {}
// SAFETY: A stream may move from setup to one callback owner; safe processing
// requires `&mut self` and therefore remains serialized.
unsafe impl Send for DenoiseStream {}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static DESTROYS: AtomicUsize = AtomicUsize::new(0);
    static PARTIAL_DESTROYS: AtomicUsize = AtomicUsize::new(0);

    #[repr(C)]
    struct FakeState {
        frame: u16,
        fail_at: Option<u16>,
    }

    unsafe extern "C" fn create(
        rate: u32,
        channels: u32,
        frames: u32,
        output: *mut *mut c_void,
    ) -> c_int {
        assert_eq!((rate, channels, frames), (48_000, 1, 480));
        // SAFETY: The wrapper supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeState {
                frame: 0,
                fail_at: None,
            }))
            .cast();
        };
        RESULT_OK
    }

    unsafe extern "C" fn failing_create(
        _rate: u32,
        _channels: u32,
        _frames: u32,
        output: *mut *mut c_void,
    ) -> c_int {
        // SAFETY: The wrapper supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeState {
                frame: 0,
                fail_at: Some(0),
            }))
            .cast();
        };
        -2
    }

    unsafe extern "C" fn null_create(
        _rate: u32,
        _channels: u32,
        _frames: u32,
        output: *mut *mut c_void,
    ) -> c_int {
        // SAFETY: The wrapper supplies writable output storage.
        unsafe { *output = std::ptr::null_mut() };
        RESULT_OK
    }

    unsafe extern "C" fn create_live_failure(
        _rate: u32,
        _channels: u32,
        _frames: u32,
        output: *mut *mut c_void,
    ) -> c_int {
        // SAFETY: The wrapper supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeState {
                frame: 0,
                fail_at: Some(2),
            }))
            .cast();
        }
        RESULT_OK
    }

    unsafe extern "C" fn process(
        handle: *mut c_void,
        input: *const f32,
        frames: u32,
        output: *mut f32,
        vad: *mut f32,
    ) -> c_int {
        // SAFETY: The test wrapper supplies its live fake handle.
        let state = unsafe { &mut *handle.cast::<FakeState>() };
        if state.fail_at == Some(state.frame) {
            return -2;
        }
        assert_eq!(frames, 480);
        // SAFETY: The wrapper supplies exact fixed-size buffers.
        let input = unsafe { std::slice::from_raw_parts(input, usize::try_from(frames).unwrap()) };
        // SAFETY: The wrapper supplies exact writable fixed-size output.
        let output =
            unsafe { std::slice::from_raw_parts_mut(output, usize::try_from(frames).unwrap()) };
        output.copy_from_slice(input);
        state.frame += 1;
        // SAFETY: The wrapper supplies writable VAD storage.
        unsafe { *vad = f32::from(state.frame) / 10.0 };
        RESULT_OK
    }

    const unsafe extern "C" fn failing_process(
        _handle: *mut c_void,
        _input: *const f32,
        _frames: u32,
        _output: *mut f32,
        _vad: *mut f32,
    ) -> c_int {
        -2
    }

    unsafe extern "C" fn destroy(handle: *mut c_void) {
        // SAFETY: Test handles originate from Box::into_raw above, and the
        // validated owner never calls destroy for a null handle.
        let state = unsafe { Box::from_raw(handle.cast::<FakeState>()) };
        if state.fail_at == Some(0) {
            PARTIAL_DESTROYS.fetch_add(1, Ordering::Relaxed);
        }
        drop(state);
        DESTROYS.fetch_add(1, Ordering::Relaxed);
    }

    fn valid_descriptor() -> Descriptor {
        Descriptor {
            struct_size: u32::try_from(size_of::<Descriptor>()).unwrap(),
            abi_version: ABI_VERSION,
            capability_name: CAPABILITY.as_ptr(),
            create: Some(create),
            process: Some(process),
            destroy: Some(destroy),
        }
    }

    fn provider(descriptor: &'static Descriptor) -> Result<DenoiseProvider, DenoiseError> {
        // SAFETY: Test descriptors have immutable static lifetime.
        unsafe { DenoiseProvider::from_raw_descriptor(std::ptr::from_ref(descriptor).cast()) }
    }

    fn assert_samples_equal(samples: &[f32], expected: f32) {
        let expected = expected.to_bits();
        assert!(samples.iter().all(|sample| sample.to_bits() == expected));
    }

    #[test]
    fn descriptor_validation_rejects_invalid_prefixes() {
        assert_eq!(
            // SAFETY: Null is explicitly accepted and rejected.
            unsafe { DenoiseProvider::from_raw_descriptor(std::ptr::null()) }.err(),
            Some(DenoiseError::IncompatibleAdapter)
        );
        for descriptor in [
            Descriptor {
                struct_size: u32::try_from(REQUIRED_DESCRIPTOR_SIZE - 1).unwrap(),
                ..valid_descriptor()
            },
            Descriptor {
                abi_version: 2,
                ..valid_descriptor()
            },
            Descriptor {
                capability_name: std::ptr::null(),
                ..valid_descriptor()
            },
            Descriptor {
                capability_name: c"wrong".as_ptr(),
                ..valid_descriptor()
            },
            Descriptor {
                create: None,
                ..valid_descriptor()
            },
            Descriptor {
                process: None,
                ..valid_descriptor()
            },
            Descriptor {
                destroy: None,
                ..valid_descriptor()
            },
        ] {
            let descriptor = Box::leak(Box::new(descriptor));
            assert_eq!(
                provider(descriptor).err(),
                Some(DenoiseError::IncompatibleAdapter)
            );
        }
    }

    #[test]
    fn arbitrary_partitions_retain_two_frame_startup_and_then_stream() {
        let mut stream = provider(Box::leak(Box::new(valid_descriptor())))
            .unwrap()
            .prepare()
            .unwrap();
        let mut first = [0.25; 300];
        stream.process(&mut first).unwrap();
        assert_samples_equal(&first, 0.0);
        let mut second = [0.5; 660];
        stream.process(&mut second).unwrap();
        assert_samples_equal(&second, 0.0);
        let mut third = [0.75; 480];
        stream.process(&mut third).unwrap();
        assert_samples_equal(&third, 0.5);
    }

    #[test]
    fn bypass_restarts_framing_without_recreating_model() {
        let mut stream = provider(Box::leak(Box::new(valid_descriptor())))
            .unwrap()
            .prepare()
            .unwrap();
        let mut samples = [1.0; 960];
        stream.process(&mut samples).unwrap();
        stream.bypass();
        stream.bypass();
        let mut after = [1.0; 480];
        stream.process(&mut after).unwrap();
        assert_samples_equal(&after, 0.0);
    }

    #[test]
    fn adapter_failures_propagate_and_partial_handles_are_destroyed() {
        assert_eq!(
            DenoiseError::IncompatibleAdapter.to_string(),
            "incompatible RNNoise adapter"
        );
        assert_eq!(
            DenoiseError::AdapterFailure.to_string(),
            "RNNoise adapter operation failed"
        );
        let descriptor = Box::leak(Box::new(Descriptor {
            create: Some(null_create),
            ..valid_descriptor()
        }));
        assert_eq!(
            provider(descriptor).unwrap().prepare().err(),
            Some(DenoiseError::AdapterFailure)
        );

        let descriptor = Box::leak(Box::new(Descriptor {
            create: Some(failing_create),
            ..valid_descriptor()
        }));
        let before = PARTIAL_DESTROYS.load(Ordering::Relaxed);
        assert_eq!(
            provider(descriptor).unwrap().prepare().err(),
            Some(DenoiseError::AdapterFailure)
        );
        assert_eq!(PARTIAL_DESTROYS.load(Ordering::Relaxed), before + 1);

        let descriptor = Box::leak(Box::new(Descriptor {
            process: Some(failing_process),
            ..valid_descriptor()
        }));
        let before = DESTROYS.load(Ordering::Relaxed);
        assert_eq!(
            provider(descriptor).unwrap().prepare().err(),
            Some(DenoiseError::AdapterFailure)
        );
        assert_eq!(DESTROYS.load(Ordering::Relaxed), before + 1);

        let descriptor = Box::leak(Box::new(Descriptor {
            create: Some(create_live_failure),
            ..valid_descriptor()
        }));
        let mut stream = provider(descriptor).unwrap().prepare().unwrap();
        assert_eq!(
            stream.process(&mut [0.0; 480]),
            Err(DenoiseError::AdapterFailure)
        );
    }
}
