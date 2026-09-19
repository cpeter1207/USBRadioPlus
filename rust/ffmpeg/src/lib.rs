//! Safe ownership around the versioned external FFmpeg graph descriptor.
//!
//! This private Rust component is the only USBRadioPlus code that knows the
//! external adapter's C layout. The adapter-neutral core supplies graph text
//! and normalized F32 buffers without importing any FFmpeg or adapter type.

#![cfg_attr(coverage, feature(coverage_attribute))]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt;
use std::mem::{offset_of, size_of};
use std::ptr::NonNull;

const ABI_VERSION: u32 = 1;
const CAPABILITY: &CStr = c"rptadv.ffmpeg";
const RESULT_OK: c_int = 0;

type CreateFn = unsafe extern "C" fn(*const GraphConfig, *mut *mut c_void) -> c_int;
type StreamingProcessFn =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int;
type DestroyFn = unsafe extern "C" fn(*mut c_void);
type ProcessBlockFn = unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int;

#[repr(C)]
struct GraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Descriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<CreateFn>,
    process: Option<StreamingProcessFn>,
    destroy: Option<DestroyFn>,
    process_block: Option<ProcessBlockFn>,
}

const REQUIRED_DESCRIPTOR_SIZE: usize =
    offset_of!(Descriptor, process_block) + size_of::<Option<ProcessBlockFn>>();

/// FFmpeg adapter setup or processing failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GraphError {
    /// A null or incompatible descriptor was supplied by product composition.
    IncompatibleAdapter,
    /// The requested graph properties or PCM buffers were invalid.
    InvalidArgument,
    /// The external adapter rejected graph creation or processing.
    AdapterFailure,
}

impl fmt::Display for GraphError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompatibleAdapter => formatter.write_str("incompatible FFmpeg adapter"),
            Self::InvalidArgument => formatter.write_str("invalid FFmpeg graph argument"),
            Self::AdapterFailure => formatter.write_str("FFmpeg adapter operation failed"),
        }
    }
}

impl std::error::Error for GraphError {}

/// Validated process-lifetime external FFmpeg capability.
#[derive(Clone, Copy)]
pub struct GraphProvider {
    create: CreateFn,
    destroy: DestroyFn,
    process_block: ProcessBlockFn,
}

impl GraphProvider {
    /// Validate a process-lifetime descriptor supplied by product composition.
    ///
    /// # Safety
    ///
    /// `raw_descriptor` must either be null or point to immutable storage that
    /// remains valid until every provider and graph made from it is dropped.
    pub unsafe fn from_raw_descriptor(raw_descriptor: *const c_void) -> Result<Self, GraphError> {
        let descriptor = NonNull::new(raw_descriptor.cast_mut().cast::<Descriptor>())
            .ok_or(GraphError::IncompatibleAdapter)?;
        // SAFETY: The caller promises process-lifetime readable descriptor
        // storage; only the fixed prefix is inspected before its size is used.
        let descriptor = unsafe { descriptor.as_ref() };
        if (descriptor.struct_size as usize) < REQUIRED_DESCRIPTOR_SIZE
            || descriptor.abi_version != ABI_VERSION
            || descriptor.capability_name.is_null()
        {
            return Err(GraphError::IncompatibleAdapter);
        }
        // SAFETY: The validated non-null name belongs to the promised
        // process-lifetime descriptor and must be NUL terminated by ABI.
        if unsafe { CStr::from_ptr(descriptor.capability_name) } != CAPABILITY {
            return Err(GraphError::IncompatibleAdapter);
        }
        let (Some(create), Some(destroy), Some(process_block)) = (
            descriptor.create,
            descriptor.destroy,
            descriptor.process_block,
        ) else {
            return Err(GraphError::IncompatibleAdapter);
        };
        Ok(Self {
            create,
            destroy,
            process_block,
        })
    }

    /// Prepare one fixed-48-kHz, exact-block graph.
    pub fn prepare(
        self,
        filter_description: &CStr,
        maximum_frame_count: u32,
    ) -> Result<PreparedGraph, GraphError> {
        self.prepare_at_rate(filter_description, 48_000, maximum_frame_count)
    }

    /// Prepare an exact-block graph at an external boundary's established rate.
    ///
    /// Native USBRadioPlus processing uses [`Self::prepare`] and remains fixed
    /// at 48 kHz. This entry point exists only for an ASL3 link audiohook,
    /// whose Asterisk format determines the rate before graph preparation.
    pub fn prepare_at_rate(
        self,
        filter_description: &CStr,
        sample_rate_hz: u32,
        maximum_frame_count: u32,
    ) -> Result<PreparedGraph, GraphError> {
        if filter_description.to_bytes().is_empty()
            || sample_rate_hz == 0
            || maximum_frame_count == 0
        {
            return Err(GraphError::InvalidArgument);
        }
        let config = GraphConfig {
            struct_size: size_of::<GraphConfig>() as u32,
            abi_version: ABI_VERSION,
            sample_rate_hz,
            maximum_frame_count,
            filter_description: filter_description.as_ptr(),
        };
        let mut handle = std::ptr::null_mut();
        // SAFETY: Both pointers remain valid for this synchronous setup call;
        // the validated provider owns the function contract.
        let result = unsafe { (self.create)(&config, &mut handle) };
        let Some(handle) = NonNull::new(handle) else {
            return Err(GraphError::AdapterFailure);
        };
        if result != RESULT_OK {
            // SAFETY: A non-null partial handle returned by create belongs to
            // this provider and is destroyed exactly once here.
            unsafe { (self.destroy)(handle.as_ptr()) };
            return Err(GraphError::AdapterFailure);
        }
        Ok(PreparedGraph {
            provider: self,
            handle,
            maximum_frame_count,
        })
    }
}

/// One prepared graph owned by exactly one real-time callback stream.
pub struct PreparedGraph {
    provider: GraphProvider,
    handle: NonNull<c_void>,
    maximum_frame_count: u32,
}

impl PreparedGraph {
    /// Process one complete, bounded normalized-F32 block.
    ///
    /// Input and output must have the same nonzero length. This call allocates
    /// no memory and forwards exactly one prepared adapter operation.
    pub fn process_block(&mut self, input: &[f32], output: &mut [f32]) -> Result<(), GraphError> {
        if input.is_empty()
            || input.len() > self.maximum_frame_count as usize
            || output.len() != input.len()
        {
            return Err(GraphError::InvalidArgument);
        }
        // The maximum-frame check proves the conversion is lossless.
        let frame_count = input.len() as u32;
        // SAFETY: Slices are disjoint caller-owned blocks of frame_count
        // elements, and the graph handle remains exclusively borrowed.
        let result = unsafe {
            (self.provider.process_block)(
                self.handle.as_ptr(),
                input.as_ptr(),
                frame_count,
                output.as_mut_ptr(),
            )
        };
        if result == RESULT_OK {
            Ok(())
        } else {
            Err(GraphError::AdapterFailure)
        }
    }

    /// Exercise an unpublished graph with preallocated silence workspaces.
    pub fn warm_up(
        &mut self,
        silence: &[f32],
        output: &mut [f32],
        blocks: usize,
    ) -> Result<(), GraphError> {
        for _ in 0..blocks {
            self.process_block(silence, output)?;
        }
        Ok(())
    }
}

impl Drop for PreparedGraph {
    fn drop(&mut self) {
        // SAFETY: This graph owns the handle and Drop runs exactly once after
        // exclusive processing has ceased.
        unsafe { (self.provider.destroy)(self.handle.as_ptr()) };
    }
}

// SAFETY: The external contract keeps the shared object containing these
// immutable function pointers loaded for every derived graph's lifetime.
unsafe impl Send for GraphProvider {}
// SAFETY: The copied function pointers contain no mutable provider state.
unsafe impl Sync for GraphProvider {}
// SAFETY: A prepared graph may move between setup and its single callback
// owner; `&mut self` prevents simultaneous processing through safe Rust.
unsafe impl Send for PreparedGraph {}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static DESTROYS: AtomicUsize = AtomicUsize::new(0);

    struct FakeGraphState {
        sample_rate_hz: u32,
    }

    unsafe extern "C" fn create(config: *const GraphConfig, output: *mut *mut c_void) -> c_int {
        // SAFETY: Tests call this through the same validated ABI with live pointers.
        let config = unsafe { &*config };
        assert_eq!(config.abi_version, ABI_VERSION);
        // SAFETY: The caller supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeGraphState {
                sample_rate_hz: config.sample_rate_hz,
            }))
            .cast()
        };
        RESULT_OK
    }

    unsafe extern "C" fn failing_create(
        config: *const GraphConfig,
        output: *mut *mut c_void,
    ) -> c_int {
        // SAFETY: Tests call this through the same validated ABI with a live pointer.
        let config = unsafe { &*config };
        // SAFETY: The caller supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeGraphState {
                sample_rate_hz: config.sample_rate_hz,
            }))
            .cast()
        };
        -2
    }

    unsafe extern "C" fn null_create(
        _config: *const GraphConfig,
        output: *mut *mut c_void,
    ) -> c_int {
        // SAFETY: The caller supplies writable output storage.
        unsafe { *output = std::ptr::null_mut() };
        RESULT_OK
    }

    unsafe extern "C" fn process(
        handle: *mut c_void,
        input: *const f32,
        frames: u32,
        output: *mut f32,
    ) -> c_int {
        // SAFETY: The ABI call supplies the live handle created above.
        let state = unsafe { &*handle.cast::<FakeGraphState>() };
        // SAFETY: The tested wrapper supplies distinct slices of `frames` elements.
        let input = unsafe { std::slice::from_raw_parts(input, frames as usize) };
        // SAFETY: The tested wrapper supplies writable output of `frames` elements.
        let output = unsafe { std::slice::from_raw_parts_mut(output, frames as usize) };
        let gain = if state.sample_rate_hz == 8_000 {
            0.25
        } else {
            0.5
        };
        for (destination, source) in output.iter_mut().zip(input) {
            *destination = *source * gain;
        }
        RESULT_OK
    }

    unsafe extern "C" fn failing_process(
        _handle: *mut c_void,
        _input: *const f32,
        _frames: u32,
        _output: *mut f32,
    ) -> c_int {
        -2
    }

    unsafe extern "C" fn destroy(handle: *mut c_void) {
        // SAFETY: Every handle in these tests originated from Box::into_raw,
        // and the validated graph owner never destroys a null handle.
        drop(unsafe { Box::from_raw(handle.cast::<FakeGraphState>()) });
        DESTROYS.fetch_add(1, Ordering::Relaxed);
    }

    static VALID: Descriptor = Descriptor {
        struct_size: size_of::<Descriptor>() as u32,
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY.as_ptr(),
        create: Some(create),
        process: None,
        destroy: Some(destroy),
        process_block: Some(process),
    };

    // SAFETY: Test descriptors are immutable and all pointed-to strings and
    // functions have static lifetime.
    unsafe impl Sync for Descriptor {}

    fn provider(descriptor: &'static Descriptor) -> Result<GraphProvider, GraphError> {
        // SAFETY: The static test descriptor satisfies the documented lifetime.
        unsafe { GraphProvider::from_raw_descriptor(std::ptr::from_ref(descriptor).cast()) }
    }

    #[test]
    fn descriptor_validation_rejects_every_incompatible_prefix() {
        assert_eq!(
            // SAFETY: Null is explicitly accepted and rejected.
            unsafe { GraphProvider::from_raw_descriptor(std::ptr::null()) }.err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.struct_size = (REQUIRED_DESCRIPTOR_SIZE - 1) as u32;
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.abi_version += 1;
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.capability_name = std::ptr::null();
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.capability_name = c"wrong".as_ptr();
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.create = None;
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.destroy = None;
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
        let mut descriptor = VALID;
        descriptor.process_block = None;
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(GraphError::IncompatibleAdapter)
        );
    }

    #[test]
    fn graph_lifecycle_processing_and_warmup_are_exact_block() {
        DESTROYS.store(0, Ordering::Relaxed);
        {
            let mut graph = provider(&VALID).unwrap().prepare(c"volume=0.5", 4).unwrap();
            let input = [1.0, 0.5, -0.5, -1.0];
            let mut output = [0.0; 4];
            graph.process_block(&input, &mut output).unwrap();
            assert_eq!(output, [0.5, 0.25, -0.25, -0.5]);
            graph.warm_up(&[0.0; 4], &mut output, 3).unwrap();
        }
        assert_eq!(DESTROYS.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn external_boundary_graph_uses_its_established_rate() {
        let mut graph = provider(&VALID)
            .unwrap()
            .prepare_at_rate(c"volume=0.25", 8_000, 2)
            .unwrap();
        let mut output = [0.0; 2];
        graph.process_block(&[1.0, -1.0], &mut output).unwrap();
        assert_eq!(output, [0.25, -0.25]);
        assert_eq!(
            provider(&VALID)
                .unwrap()
                .prepare_at_rate(c"anull", 0, 1)
                .err(),
            Some(GraphError::InvalidArgument)
        );
    }

    #[test]
    fn invalid_arguments_and_adapter_failures_do_not_leak_handles() {
        assert_eq!(
            GraphError::IncompatibleAdapter.to_string(),
            "incompatible FFmpeg adapter"
        );
        assert_eq!(
            GraphError::InvalidArgument.to_string(),
            "invalid FFmpeg graph argument"
        );
        assert_eq!(
            GraphError::AdapterFailure.to_string(),
            "FFmpeg adapter operation failed"
        );
        assert_eq!(
            provider(&VALID).unwrap().prepare(c"", 4).err(),
            Some(GraphError::InvalidArgument)
        );
        assert_eq!(
            provider(&VALID).unwrap().prepare(c"anull", 0).err(),
            Some(GraphError::InvalidArgument)
        );
        let mut graph = provider(&VALID).unwrap().prepare(c"anull", 2).unwrap();
        assert_eq!(
            graph.process_block(&[], &mut []),
            Err(GraphError::InvalidArgument)
        );
        assert_eq!(
            graph.process_block(&[0.0; 2], &mut [0.0; 1]),
            Err(GraphError::InvalidArgument)
        );
        assert_eq!(
            graph.process_block(&[0.0; 3], &mut [0.0; 3]),
            Err(GraphError::InvalidArgument)
        );

        let descriptor = Descriptor {
            create: Some(null_create),
            ..VALID
        };
        assert_eq!(
            provider(Box::leak(Box::new(descriptor)))
                .unwrap()
                .prepare(c"anull", 1)
                .err(),
            Some(GraphError::AdapterFailure)
        );

        let descriptor = Descriptor {
            create: Some(failing_create),
            ..VALID
        };
        let before = DESTROYS.load(Ordering::Relaxed);
        assert_eq!(
            provider(Box::leak(Box::new(descriptor)))
                .unwrap()
                .prepare(c"anull", 1)
                .err(),
            Some(GraphError::AdapterFailure)
        );
        assert_eq!(DESTROYS.load(Ordering::Relaxed), before + 1);

        let descriptor = Descriptor {
            process_block: Some(failing_process),
            ..VALID
        };
        let mut graph = provider(Box::leak(Box::new(descriptor)))
            .unwrap()
            .prepare(c"anull", 1)
            .unwrap();
        assert_eq!(
            graph.process_block(&[0.0], &mut [0.0]),
            Err(GraphError::AdapterFailure)
        );
        assert_eq!(
            graph.warm_up(&[0.0], &mut [0.0], 1),
            Err(GraphError::AdapterFailure)
        );
    }
}
