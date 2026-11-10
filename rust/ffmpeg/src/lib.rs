//! Safe ownership around the versioned external FFmpeg graph descriptor.
//!
//! This private Rust component is the only USBRadioPlus code that knows the
//! external adapter's C layout. The adapter-neutral core supplies graph text
//! and normalized F32 buffers without importing any FFmpeg or adapter type.

#![cfg_attr(coverage, feature(coverage_attribute))]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::fmt;
use std::mem::{offset_of, size_of};
use std::ptr::{self, NonNull};
use usbradioplus_core::{
    PROGRAMME_COUNTS_VALID, PROGRAMME_CURRENT_BOUND_VALID, PROGRAMME_LAST_BLOCK_VALID,
    PROGRAMME_PREPARED_BOUND_VALID, ProgrammeBuffering, ProgrammeBufferingPlan,
};

const ABI_VERSION: u32 = 1;
const CAPABILITY: &CStr = c"rptadv.ffmpeg";
const RESULT_OK: c_int = 0;

type CreateFn = unsafe extern "C" fn(*const GraphConfig, *mut *mut c_void) -> c_int;
type StreamingProcessFn =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int;
type DestroyFn = unsafe extern "C" fn(*mut c_void);
type ProcessBlockFn = unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int;
type BufferingStateFn = unsafe extern "C" fn(*const c_void, *mut BufferingState) -> c_int;

#[repr(C)]
struct BufferingState {
    struct_size: u32,
    abi_version: u32,
    flags: u32,
    reserved: u32,
    accepted_frames: u64,
    delivered_frames: u64,
    output_fifo_frames: u64,
    zero_prefix_frames: u32,
    zero_suffix_frames: u32,
}

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
    buffering_state: Option<BufferingStateFn>,
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
    buffering_state: Option<BufferingStateFn>,
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
        // SAFETY: the advertised header is readable. Never form a larger
        // reference over a provider exposing only the ordinary 48-byte prefix.
        let length = unsafe { ptr::addr_of!((*descriptor.as_ptr()).struct_size).read() } as usize;
        if length < REQUIRED_DESCRIPTOR_SIZE {
            return Err(GraphError::IncompatibleAdapter);
        }
        // SAFETY: nullable function pointers are zero-valid. Copy only complete
        // readable capability groups; missing optional tails remain unavailable.
        let mut copied: Descriptor = unsafe { std::mem::zeroed() };
        let count = if length >= size_of::<Descriptor>() {
            size_of::<Descriptor>()
        } else {
            REQUIRED_DESCRIPTOR_SIZE
        };
        // SAFETY: prefix length was checked against the provider's declaration.
        unsafe {
            ptr::copy_nonoverlapping(
                descriptor.as_ptr().cast::<u8>(),
                ptr::from_mut(&mut copied).cast::<u8>(),
                count,
            )
        };
        let descriptor = copied;
        if descriptor.abi_version != ABI_VERSION || descriptor.capability_name.is_null() {
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
            buffering_state: descriptor.buffering_state,
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
        self.prepare_at_rate_with_plan(
            filter_description,
            sample_rate_hz,
            maximum_frame_count,
            ProgrammeBufferingPlan::Unknown,
        )
    }

    /// Prepare the same graph with a separately audited programme-buffering plan.
    /// Missing query/model support leaves bounds unknown and ordinary PCM available.
    pub fn prepare_at_rate_with_plan(
        self,
        filter_description: &CStr,
        sample_rate_hz: u32,
        maximum_frame_count: u32,
        plan: ProgrammeBufferingPlan,
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
            maximum_frame_count: maximum_frame_count.max(64),
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
        let mut graph = PreparedGraph {
            provider: self,
            handle,
            maximum_frame_count,
            provider_maximum: config.maximum_frame_count,
            plan,
            buffering: ProgrammeBuffering::default(),
            buffering_faulted: false,
        };
        graph.refresh_buffering(0);
        Ok(graph)
    }
}

/// One prepared graph owned by exactly one real-time callback stream.
pub struct PreparedGraph {
    provider: GraphProvider,
    handle: NonNull<c_void>,
    maximum_frame_count: u32,
    provider_maximum: u32,
    plan: ProgrammeBufferingPlan,
    buffering: ProgrammeBuffering,
    buffering_faulted: bool,
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
            self.refresh_buffering(frame_count);
            Ok(())
        } else {
            self.invalidate_buffering();
            Err(GraphError::AdapterFailure)
        }
    }

    /// Return the last owner-serialized observation; this never infers source time.
    pub const fn buffering_state(&self) -> ProgrammeBuffering {
        self.buffering
    }

    /// End control-plane silence preparation without resetting graph history or
    /// counters. Warmup is not a live programme observation or retirement barrier.
    pub fn finish_warmup(&mut self) {
        self.buffering.flags &= !(PROGRAMME_CURRENT_BOUND_VALID | PROGRAMME_LAST_BLOCK_VALID);
        self.buffering.current_max_delay_frames = 0;
        self.buffering.zero_prefix_frames = 0;
        self.buffering.zero_suffix_frames = 0;
    }

    fn invalidate_buffering(&mut self) {
        self.buffering = ProgrammeBuffering::default();
        self.buffering_faulted = true;
    }

    fn refresh_buffering(&mut self, advance: u32) {
        let Some(query) = self.provider.buffering_state else {
            return;
        };
        if self.buffering_faulted {
            return;
        }
        let mut state = BufferingState {
            struct_size: size_of::<BufferingState>() as u32,
            abi_version: 1,
            flags: 0,
            reserved: 0,
            accepted_frames: 0,
            delivered_frames: 0,
            output_fifo_frames: 0,
            zero_prefix_frames: 0,
            zero_suffix_frames: 0,
        };
        // SAFETY: processing and this complete stack query share the sole graph owner.
        let result = unsafe { query(self.handle.as_ptr(), &mut state) };
        if result != RESULT_OK
            || state.struct_size < size_of::<BufferingState>() as u32
            || state.abi_version != 1
            || state.reserved != 0
            || state.flags & 1 == 0
            || self
                .buffering
                .accepted_frames
                .checked_add(u64::from(advance))
                != Some(state.accepted_frames)
            || state.delivered_frames < self.buffering.delivered_frames
            || state.delivered_frames > state.accepted_frames
            || state.output_fifo_frames > u64::from(self.provider_maximum) * 8
            || u64::from(state.zero_prefix_frames) + u64::from(state.zero_suffix_frames)
                > u64::from(advance)
            || (advance != 0 && state.flags & 2 == 0)
        {
            self.invalidate_buffering();
            return;
        }
        self.buffering = ProgrammeBuffering {
            flags: PROGRAMME_COUNTS_VALID
                | if state.flags & 2 != 0 {
                    PROGRAMME_LAST_BLOCK_VALID
                } else {
                    0
                },
            accepted_frames: state.accepted_frames,
            delivered_frames: state.delivered_frames,
            output_fifo_frames: state.output_fifo_frames,
            zero_prefix_frames: state.zero_prefix_frames,
            zero_suffix_frames: state.zero_suffix_frames,
            ..ProgrammeBuffering::default()
        };
        if let ProgrammeBufferingPlan::Known {
            maximum_deficit_frames,
            additional_delay_frames,
        } = self.plan
        {
            if state.flags & 4 == 0 {
                // A prepared unknown backend cannot become qualified midway
                // through a stream and relabel retained output as fresh.
                self.plan = ProgrammeBufferingPlan::Unknown;
                return;
            }
            let Some(prepared) = maximum_deficit_frames.checked_add(additional_delay_frames) else {
                return;
            };
            let backlog = state.accepted_frames - state.delivered_frames;
            if backlog > maximum_deficit_frames || state.output_fifo_frames > backlog {
                self.invalidate_buffering();
                return;
            }
            self.buffering.prepared_max_delay_frames = prepared;
            self.buffering.flags |= PROGRAMME_PREPARED_BOUND_VALID;
            if state.flags & 2 != 0 {
                self.buffering.current_max_delay_frames = backlog + additional_delay_frames;
                self.buffering.flags |= PROGRAMME_CURRENT_BOUND_VALID;
            }
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
    use std::cell::Cell;

    #[link(name = "rptadv_ffmpeg_adapter")]
    unsafe extern "C" {
        fn rptadv_ffmpeg_adapter_descriptor() -> *const c_void;
    }

    fn real_provider() -> GraphProvider {
        // SAFETY: the versioned shared provider retains its immutable descriptor
        // and callback code for this process's entire native test lifetime.
        unsafe { GraphProvider::from_raw_descriptor(rptadv_ffmpeg_adapter_descriptor()) }.unwrap()
    }

    #[test]
    fn real_explicit_recipes_preserve_pcm_and_bound_every_variable_partition() {
        use usbradioplus_core::{ChainRole, GraphDescriptionFactory, ProcessingChain};
        let descriptions = GraphDescriptionFactory::new("/unused/agc.so").unwrap();
        let explicit_src = ":filter_size=32:phase_shift=10:linear_interp=1:exact_rational=1:cutoff=0.97:resampler=swr:async=0";
        for rate in [8000, 48000, 384000] {
            for factor in [1, 2, 4] {
                let mut chain = ProcessingChain::shipped(ChainRole::Link);
                chain.peak_limiter.enabled = true;
                chain.peak_limiter.oversample = factor;
                let recipe = descriptions.dynamics_recipe_at_rate(&chain, rate).unwrap();
                let original = recipe
                    .description
                    .replace(explicit_src, "")
                    .replace(":blocksize=0", "");
                let original = std::ffi::CString::new(original).unwrap();
                let explicit = std::ffi::CString::new(recipe.description).unwrap();
                let mut graph = real_provider()
                    .prepare_at_rate_with_plan(&explicit, rate, 4096, recipe.buffering)
                    .unwrap();
                let mut before = real_provider()
                    .prepare_at_rate(&original, rate, 4096)
                    .unwrap();
                assert_ne!(
                    graph.buffering_state().flags & PROGRAMME_PREPARED_BOUND_VALID,
                    0
                );
                let (mut actual, mut reference) = ([0.0; 4096], [0.0; 4096]);
                let mut source = 0_u64;
                for count in [1, 7, 64, 960, 4096, 1, 4096, 7, 960, 64, 1, 7] {
                    let mut input = [0.0; 4096];
                    for sample in &mut input[..count] {
                        *sample = ((source % 257) as f32 - 128.0) / 512.0;
                        source += 1;
                    }
                    graph
                        .process_block(&input[..count], &mut actual[..count])
                        .unwrap();
                    before
                        .process_block(&input[..count], &mut reference[..count])
                        .unwrap();
                    assert_eq!(
                        &actual[..count],
                        &reference[..count],
                        "rate{rate} factor{factor} count{count}"
                    );
                    let state = graph.buffering_state();
                    assert_ne!(
                        state.flags & PROGRAMME_CURRENT_BOUND_VALID,
                        0,
                        "rate{rate} factor{factor} count{count}: {state:?}"
                    );
                    assert!(state.current_max_delay_frames <= state.prepared_max_delay_frames);
                    assert_eq!(state.accepted_frames, source);
                }
            }
        }
    }

    #[test]
    fn real_custom_delay_cannot_be_qualified_by_current_counters() {
        let mut graph = real_provider().prepare(c"aecho=1:1:600:0.5", 960).unwrap();
        let mut input = [0.0; 960];
        input[0] = 0.25;
        let mut output = [0.0; 960];
        let mut heard = false;
        for block in 0..32 {
            graph.process_block(&input, &mut output).unwrap();
            input.fill(0.0);
            let state = graph.buffering_state();
            assert_ne!(state.flags & PROGRAMME_COUNTS_VALID, 0);
            assert_eq!(state.accepted_frames, state.delivered_frames);
            assert_eq!(state.output_fifo_frames, 0);
            assert_eq!(state.flags & 6, 0);
            if block != 0 && output.iter().any(|sample| sample.abs() > 0.1) {
                assert!(block >= 30);
                heard = true;
            }
        }
        assert!(
            heard,
            "ordinary custom processing stays available despite unknown age bounds"
        );
    }

    #[test]
    fn real_limiter_and_src_retained_impulse_never_outlive_the_prepared_bound() {
        use usbradioplus_core::{ChainRole, GraphDescriptionFactory, ProcessingChain};
        let descriptions = GraphDescriptionFactory::new("/unused/agc.so").unwrap();
        for factor in [1, 2, 4] {
            let mut chain = ProcessingChain::shipped(ChainRole::Link);
            chain.enabled = false;
            chain.input_gain_db = 0.0;
            chain.output_gain_db = 0.0;
            chain.peak_limiter.enabled = true;
            chain.peak_limiter.oversample = factor;
            let recipe = descriptions.dynamics_recipe_at_rate(&chain, 48000).unwrap();
            let description = std::ffi::CString::new(recipe.description).unwrap();
            let mut graph = real_provider()
                .prepare_at_rate_with_plan(&description, 48000, 4096, recipe.buffering)
                .unwrap();
            let bound = graph.buffering_state().prepared_max_delay_frames;
            let (mut input_position, mut output_position) = (0, 0);
            let mut heard = false;
            for count in [1, 7, 64, 960, 4096, 1, 7, 960, 64] {
                let mut input = [0.0; 4096];
                if input_position == 0 {
                    input[0] = 0.25;
                }
                input_position += count;
                let mut output = [0.0; 4096];
                graph
                    .process_block(&input[..count], &mut output[..count])
                    .unwrap();
                assert_ne!(
                    graph.buffering_state().flags & PROGRAMME_CURRENT_BOUND_VALID,
                    0
                );
                for sample in &output[..count] {
                    if *sample != 0.0 {
                        assert!(
                            output_position <= bound,
                            "factor{factor} output{output_position} bound{bound}"
                        );
                        heard = true;
                    }
                    output_position += 1;
                }
            }
            assert!(heard);
        }
    }

    thread_local! {
        static DESTROYS: Cell<usize> = const { Cell::new(0) };
        static QUERY_MODE: Cell<u8> = const { Cell::new(0) };
    }

    struct FakeGraphState {
        sample_rate_hz: u32,
        maximum_frame_count: u32,
        accepted_frames: u64,
        delivered_frames: u64,
    }

    unsafe extern "C" fn create(config: *const GraphConfig, output: *mut *mut c_void) -> c_int {
        // SAFETY: Tests call this through the same validated ABI with live pointers.
        let config = unsafe { &*config };
        assert_eq!(config.abi_version, ABI_VERSION);
        // SAFETY: The caller supplies writable output storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeGraphState {
                sample_rate_hz: config.sample_rate_hz,
                maximum_frame_count: config.maximum_frame_count,
                accepted_frames: 0,
                delivered_frames: 0,
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
                maximum_frame_count: config.maximum_frame_count,
                accepted_frames: 0,
                delivered_frames: 0,
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
        let state = unsafe { &mut *handle.cast::<FakeGraphState>() };
        state.accepted_frames += u64::from(frames);
        state.delivered_frames += u64::from(frames);
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
        DESTROYS.set(DESTROYS.get() + 1);
    }

    static VALID: Descriptor = Descriptor {
        struct_size: size_of::<Descriptor>() as u32,
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY.as_ptr(),
        create: Some(create),
        process: None,
        destroy: Some(destroy),
        process_block: Some(process),
        buffering_state: None,
    };

    unsafe extern "C" fn query(handle: *const c_void, output: *mut BufferingState) -> c_int {
        // SAFETY: the wrapper retains its unique handle and a complete writable result.
        let (state, output) = unsafe { (&*handle.cast::<FakeGraphState>(), &mut *output) };
        output.flags = if state.accepted_frames == 0 { 5 } else { 7 };
        output.accepted_frames = state.accepted_frames;
        output.delivered_frames = state.delivered_frames;
        match QUERY_MODE.get() {
            1 => return -2,
            2 => output.struct_size = 8,
            3 => output.abi_version = 2,
            4 => output.reserved = 1,
            5 => output.flags &= !1,
            6 => output.flags &= !4,
            7 => output.delivered_frames += 1,
            8 => output.accepted_frames = 0,
            9 => output.output_fifo_frames = u64::MAX,
            10 => output.zero_prefix_frames = u32::MAX,
            11 => output.flags &= !2,
            12 => output.delivered_frames = 0,
            13 => output.output_fifo_frames = 1,
            _ => {}
        }
        0
    }

    #[test]
    fn optional_buffering_state_qualifies_only_known_recipes_and_backend_models() {
        use usbradioplus_core::ProgrammeBufferingPlan;
        QUERY_MODE.set(0);
        let descriptor = Box::leak(Box::new(Descriptor {
            buffering_state: Some(query),
            ..VALID
        }));
        let known = ProgrammeBufferingPlan::Known {
            maximum_deficit_frames: 8,
            additional_delay_frames: 4,
        };
        for (plan, mode, qualified) in [
            (known, 0, true),
            (known, 6, false),
            (ProgrammeBufferingPlan::Unknown, 0, false),
        ] {
            QUERY_MODE.set(mode);
            let mut graph = provider(descriptor)
                .unwrap()
                .prepare_at_rate_with_plan(c"anull", 48_000, 8, plan)
                .unwrap();
            assert_eq!(graph.buffering_state().flags & 2 != 0, qualified);
            let mut output = [0.0; 4];
            graph.process_block(&[0.25; 4], &mut output).unwrap();
            assert_eq!(output, [0.125; 4]);
            let state = graph.buffering_state();
            assert_eq!((state.accepted_frames, state.delivered_frames), (4, 4));
            assert_eq!(state.flags & 4 != 0, qualified);
            if qualified {
                assert_eq!(
                    (
                        state.prepared_max_delay_frames,
                        state.current_max_delay_frames
                    ),
                    (12, 4)
                );
            }
        }
        QUERY_MODE.set(0);
        let mut graph = provider(&VALID).unwrap().prepare(c"anull", 8).unwrap();
        assert_eq!(graph.buffering_state().flags, 0);
        graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
        assert_eq!(graph.buffering_state().flags, 0);
    }

    #[test]
    fn malformed_or_faulted_buffering_never_recovers_qualification_on_the_same_graph() {
        use usbradioplus_core::ProgrammeBufferingPlan;
        let descriptor = Box::leak(Box::new(Descriptor {
            buffering_state: Some(query),
            ..VALID
        }));
        for mode in [1, 2, 3, 4, 5, 7, 8, 9, 10] {
            QUERY_MODE.set(0);
            let mut graph = provider(descriptor)
                .unwrap()
                .prepare_at_rate_with_plan(
                    c"anull",
                    48_000,
                    8,
                    ProgrammeBufferingPlan::Known {
                        maximum_deficit_frames: 8,
                        additional_delay_frames: 4,
                    },
                )
                .unwrap();
            QUERY_MODE.set(mode);
            graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
            assert_eq!(graph.buffering_state().flags & 6, 0, "mode {mode}");
            QUERY_MODE.set(0);
            graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
            assert_eq!(graph.buffering_state().flags & 6, 0, "mode {mode}");
        }
        QUERY_MODE.set(0);
    }

    #[test]
    fn buffering_rejects_regression_deficit_fifo_and_overflow_without_touching_pcm() {
        let descriptor = Box::leak(Box::new(Descriptor {
            buffering_state: Some(query),
            ..VALID
        }));
        let known = ProgrammeBufferingPlan::Known {
            maximum_deficit_frames: 8,
            additional_delay_frames: 4,
        };
        for mode in [11, 12, 13] {
            QUERY_MODE.set(0);
            let mut graph = provider(descriptor)
                .unwrap()
                .prepare_at_rate_with_plan(c"anull", 48000, 8, known)
                .unwrap();
            graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
            QUERY_MODE.set(mode);
            let mut pcm = [0.0; 4];
            graph.process_block(&[0.25; 4], &mut pcm).unwrap();
            assert_eq!(pcm, [0.125; 4]);
            assert_eq!(graph.buffering_state().flags, 0, "mode{mode}");
        }
        QUERY_MODE.set(0);
        let overflow = ProgrammeBufferingPlan::Known {
            maximum_deficit_frames: u64::MAX,
            additional_delay_frames: 1,
        };
        let mut graph = provider(descriptor)
            .unwrap()
            .prepare_at_rate_with_plan(c"anull", 48000, 8, overflow)
            .unwrap();
        assert_eq!(graph.buffering_state().flags & 6, 0);
        graph.buffering.accepted_frames = u64::MAX;
        graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
        assert_eq!(graph.buffering_state().flags, 0);
        let mut graph = provider(descriptor)
            .unwrap()
            .prepare_at_rate_with_plan(c"anull", 48000, 8, known)
            .unwrap();
        QUERY_MODE.set(12);
        graph.process_block(&[0.25; 8], &mut [0.0; 8]).unwrap();
        assert_ne!(graph.buffering_state().flags & 6, 0);
        graph.process_block(&[0.25; 8], &mut [0.0; 8]).unwrap();
        assert_eq!(graph.buffering_state().flags, 0);
        QUERY_MODE.set(0);
    }

    #[test]
    fn complete_ordinary_prefix_and_unknown_model_keep_processing_without_upgrading_bounds() {
        use usbradioplus_core::ProgrammeBufferingPlan;
        let known = ProgrammeBufferingPlan::Known {
            maximum_deficit_frames: 8,
            additional_delay_frames: 4,
        };
        for length in [REQUIRED_DESCRIPTOR_SIZE, size_of::<Descriptor>() - 1] {
            let descriptor = Descriptor {
                buffering_state: Some(query),
                ..VALID
            };
            let mut prefix = vec![0_u64; length.div_ceil(8)];
            // SAFETY: copy only the complete readable ordinary prefix into
            // aligned backing storage; the provider cannot inspect a larger tail.
            unsafe {
                ptr::copy_nonoverlapping(
                    ptr::from_ref(&descriptor).cast::<u8>(),
                    prefix.as_mut_ptr().cast::<u8>(),
                    REQUIRED_DESCRIPTOR_SIZE,
                );
                prefix.as_mut_ptr().cast::<u32>().write(length as u32);
            }
            let raw = prefix.as_ptr().cast();
            // SAFETY: code/static name retain lifetime and the copied table
            // outlives construction; GraphProvider owns its copied callbacks.
            let selected = unsafe { GraphProvider::from_raw_descriptor(raw) }.unwrap();
            let mut graph = selected
                .prepare_at_rate_with_plan(c"anull", 48_000, 8, known)
                .unwrap();
            let mut output = [0.0; 4];
            graph.process_block(&[0.25; 4], &mut output).unwrap();
            assert_eq!(output, [0.125; 4]);
            assert_eq!(graph.buffering_state().flags, 0);
        }
        let descriptor = Box::leak(Box::new(Descriptor {
            buffering_state: Some(query),
            ..VALID
        }));
        QUERY_MODE.set(6);
        let mut graph = provider(descriptor)
            .unwrap()
            .prepare_at_rate_with_plan(c"anull", 48_000, 8, known)
            .unwrap();
        QUERY_MODE.set(0);
        graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
        assert_eq!(graph.buffering_state().flags & 6, 0);
    }

    #[test]
    fn warmup_is_not_a_live_programme_observation_and_does_not_reset_accounting() {
        use usbradioplus_core::ProgrammeBufferingPlan;
        QUERY_MODE.set(0);
        let descriptor = Box::leak(Box::new(Descriptor {
            buffering_state: Some(query),
            ..VALID
        }));
        let mut graph = provider(descriptor)
            .unwrap()
            .prepare_at_rate_with_plan(
                c"anull",
                48000,
                8,
                ProgrammeBufferingPlan::Known {
                    maximum_deficit_frames: 8,
                    additional_delay_frames: 4,
                },
            )
            .unwrap();
        graph.warm_up(&[0.0; 4], &mut [0.0; 4], 2).unwrap();
        graph.finish_warmup();
        let state = graph.buffering_state();
        assert_eq!(
            state.flags,
            PROGRAMME_COUNTS_VALID | PROGRAMME_PREPARED_BOUND_VALID
        );
        assert_eq!((state.accepted_frames, state.delivered_frames), (8, 8));
        assert_eq!(state.current_max_delay_frames, 0);
        graph.process_block(&[0.25; 4], &mut [0.0; 4]).unwrap();
        let state = graph.buffering_state();
        assert_ne!(state.flags & PROGRAMME_CURRENT_BOUND_VALID, 0);
        assert_eq!((state.accepted_frames, state.delivered_frames), (12, 12));
    }

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
    fn tiny_public_blocks_prepare_capacity_for_resampler_bursts() {
        let mut graph = provider(&VALID).unwrap().prepare(c"anull", 1).unwrap();
        // SAFETY: this fixture provider returns a retained FakeGraphState handle.
        let state = unsafe { graph.handle.cast::<FakeGraphState>().as_ref() };
        assert_eq!(state.maximum_frame_count, 64);
        assert_eq!(graph.maximum_frame_count, 1);
        let mut output = [0.0; 1];
        graph.process_block(&[1.0], &mut output).unwrap();
        assert_eq!(output, [0.5]);
        assert_eq!(
            graph.process_block(&[0.0; 2], &mut [0.0; 2]),
            Err(GraphError::InvalidArgument)
        );
    }

    #[test]
    fn graph_lifecycle_processing_and_warmup_are_exact_block() {
        DESTROYS.set(0);
        {
            let mut graph = provider(&VALID).unwrap().prepare(c"volume=0.5", 4).unwrap();
            let input = [1.0, 0.5, -0.5, -1.0];
            let mut output = [0.0; 4];
            graph.process_block(&input, &mut output).unwrap();
            assert_eq!(output, [0.5, 0.25, -0.25, -0.5]);
            graph.warm_up(&[0.0; 4], &mut output, 3).unwrap();
        }
        // Another test thread's graph lifetime must not enter this fixture's count.
        std::thread::spawn(|| drop(provider(&VALID).unwrap().prepare(c"anull", 1).unwrap()))
            .join()
            .unwrap();
        assert_eq!(DESTROYS.get(), 1);
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
        let before = DESTROYS.get();
        assert_eq!(
            provider(Box::leak(Box::new(descriptor)))
                .unwrap()
                .prepare(c"anull", 1)
                .err(),
            Some(GraphError::AdapterFailure)
        );
        assert_eq!(DESTROYS.get(), before + 1);

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
