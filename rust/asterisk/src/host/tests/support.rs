//! Shared, serialized external-Asterisk boundary for real host tests.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::ptr;
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::thread::JoinHandle;

use crate::ffi;
use std::collections::HashMap;

#[path = "abi_tests.rs"]
mod abi_tests;

#[path = "link_support.rs"]
mod link_support;

#[path = "large_cstr.rs"]
mod large_cstr;
pub(crate) use large_cstr::OversizedCString;

// This archive is never linked by the production library: this entire module
// exists only in the Rust unit-test executable.
#[link(name = "urp_ast_test_ffi", kind = "static")]
unsafe extern "C" {}

unsafe extern "C" {
    pub(crate) fn urp_test_fail_thread_create(nth: u32);
    pub(crate) fn urp_test_thread_create_calls() -> u32;
    pub(crate) fn urp_test_fail_clock_read();
}

static TEST_LOCK: Mutex<()> = Mutex::new(());

/// One stable opaque Asterisk owner, separate from the Rust host Channel.
pub(crate) struct FakeChannel {
    pub tech_pvt: usize,
    pub name: CString,
    pub state: c_int,
    pub technology: usize,
    pub formats: [usize; 3],
    pub fd: (c_int, c_int),
    pub application: CString,
    pub data: CString,
    pub datastore: usize,
    pub audiohook: usize,
}

impl FakeChannel {
    pub fn new(tech_pvt: *mut c_void) -> Box<Self> {
        Box::new(Self {
            tech_pvt: tech_pvt as usize,
            name: c"test-channel".to_owned(),
            state: 0,
            technology: 0,
            formats: [0; 3],
            fd: (0, 0),
            application: CString::default(),
            data: CString::default(),
            datastore: 0,
            audiohook: 0,
        })
    }

    pub fn as_ptr(&mut self) -> *mut ffi::ast_channel {
        ptr::from_mut(self).cast()
    }
}

/// Captured external operations and narrowly controlled failure results.
#[derive(Debug)]
pub(crate) struct CapturedFrame {
    pub kind: ffi::ast_frame_type,
    pub subclass: c_int,
    pub samples: c_int,
    pub duration: libc::c_long,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Copy, Default)]
pub(crate) enum DspOutput {
    #[default]
    Input,
    Null,
    Allocated {
        kind: ffi::ast_frame_type,
        digit: c_int,
    },
}

/// Track the caller-held channel lock across one synchronous option callback.
pub(crate) struct OwnerLockTrace {
    pub owner: usize,
    pub depth: i32,
    pub events: Vec<(&'static str, i32)>,
}

/// Mutable external results shared only by serialized host tests.
#[derive(Default)]
pub(crate) struct State {
    pub iterator_allocation_fails: bool,
    pub iterator_allocations: usize,
    pub iterator_frees: usize,
    pub channel_unrefs: Vec<usize>,
    pub audiohook_init_result: c_int,
    pub audiohook_attach_result: c_int,
    pub audiohook_calls: Vec<(&'static str, usize)>,
    pub datastore_allocation_fails: bool,
    pub datastore_allocations: usize,
    pub datastore_frees: usize,
    pub taskprocessor_push_result: c_int,
    pub taskprocessor_push_calls: usize,
    pub channel_allocation_fails: bool,
    /// (kind: log=0, CLI=1, verbose=2, level or fd, rendered message).
    pub messages: Vec<(c_int, c_int, String)>,
    pub channels: HashMap<usize, Box<FakeChannel>>,
    // Iterator snapshots can outlive hangup; the fixture frees these owners
    // only after the lifecycle guard has stopped the scanner.
    pub retired_channels: HashMap<usize, Box<FakeChannel>>,
    pub moh_result: c_int,
    pub moh_started: Vec<(String, String)>,
    pub moh_stopped: usize,
    pub trylock_result: c_int,
    pub trylock_hook: Option<Box<dyn FnOnce() + Send>>,
    pub unlocks: usize,
    pub owner_lock_trace: Option<OwnerLockTrace>,
    pub jitter: Vec<(usize, ffi::ast_jb_conf)>,
    pub advanced_format_missing: bool,
    pub advanced_format_wrong_rate: bool,
    pub capability_allocations: usize,
    pub capability_fail_at: usize,
    pub capability_appends: usize,
    pub append_fail_at: usize,
    pub capabilities: HashMap<usize, Box<usize>>,
    pub incompatible_formats: bool,
    pub registrations: usize,
    pub register_fail_at: usize,
    pub registered: Vec<usize>,
    pub unregistered: Vec<String>,
    pub taskprocessor_get_fails: bool,
    pub taskprocessors: HashMap<usize, Box<u8>>,
    pub dsp_allocation_fails: bool,
    pub dsps: HashMap<usize, Box<u8>>,
    pub dsp_features: c_int,
    pub dsp_digitmode: c_int,
    pub module_references: c_int,
    pub hangups: usize,
    pub queued: Vec<CapturedFrame>,
    pub queue_result: c_int,
    pub dsp_output: DspOutput,
    pub dsp_input: Option<CapturedFrame>,
    pub freed_frames: usize,
    pub configuration_text: Option<CString>,
    pub configuration_paths: Vec<String>,
    pub freed_texts: usize,
    workers: Vec<JoinHandle<()>>,
}

fn state() -> &'static Mutex<State> {
    static STATE: OnceLock<Mutex<State>> = OnceLock::new();
    STATE.get_or_init(|| Mutex::new(State::default()))
}

pub(crate) fn with_state<T>(operation: impl FnOnce(&mut State) -> T) -> T {
    operation(
        &mut state()
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner),
    )
}

/// Serialize tests using exported symbols and wait for every submitted task.
pub(crate) struct Fixture {
    _guard: MutexGuard<'static, ()>,
}

impl Fixture {
    pub fn new() -> Self {
        let guard = TEST_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        with_state(|state| *state = State::default());
        Self { _guard: guard }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // Never hold fixture state while a task invokes a captured host call.
        let workers = with_state(|state| std::mem::take(&mut state.workers));
        for worker in workers {
            worker.join().expect("test taskprocessor worker panicked");
        }
        with_state(|state| *state = State::default());
    }
}

static APP_FORMAT: u32 = 8_000;
static ADVANCED_FORMAT: u32 = 48_000;
static WRONG_FORMAT: u32 = 44_100;

#[unsafe(no_mangle)]
static mut ast_format_slin: *mut ffi::ast_format = (&raw const APP_FORMAT).cast_mut().cast();

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_format_cache_get_slin_by_rate(rate: u32) -> *mut ffi::ast_format {
    if rate == 8_000 {
        return (&raw const APP_FORMAT).cast_mut().cast();
    }
    with_state(|state| {
        if state.advanced_format_missing {
            ptr::null_mut()
        } else if state.advanced_format_wrong_rate {
            (&raw const WRONG_FORMAT).cast_mut().cast()
        } else {
            (&raw const ADVANCED_FORMAT).cast_mut().cast()
        }
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_format_get_sample_rate(format: *const ffi::ast_format) -> u32 {
    // SAFETY: format cache entries point to process-lifetime fixture rate values.
    unsafe { *format.cast::<u32>() }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_format_cap_alloc(
    _: ffi::ast_format_cap_flags,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> *mut ffi::ast_format_cap {
    with_state(|state| {
        state.capability_allocations += 1;
        if state.capability_allocations == state.capability_fail_at {
            return ptr::null_mut();
        }
        let mut capability = Box::new(0usize);
        let raw = ptr::from_mut(&mut *capability).cast();
        state.capabilities.insert(raw as usize, capability);
        raw
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_format_cap_append(
    capability: *mut ffi::ast_format_cap,
    format: *mut ffi::ast_format,
    _: u32,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    with_state(|state| {
        state.capability_appends += 1;
        if state.capability_appends == state.append_fail_at {
            return -1;
        }
        **state.capabilities.get_mut(&(capability as usize)).unwrap() = format as usize;
        0
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_format_cap_iscompatible(
    left: *const ffi::ast_format_cap,
    right: *const ffi::ast_format_cap,
) -> c_int {
    with_state(|state| {
        i32::from(
            !state.incompatible_formats
                && state.capabilities.get(&(left as usize))
                    == state.capabilities.get(&(right as usize)),
        )
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ao2_ref(
    object: *mut c_void,
    delta: c_int,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    with_state(|state| {
        assert_eq!(delta, -1);
        if state.channels.contains_key(&(object as usize))
            || state.retired_channels.contains_key(&(object as usize))
        {
            state.channel_unrefs.push(object as usize);
        } else {
            assert!(state.capabilities.remove(&(object as usize)).is_some());
        }
    });
    1
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_register(technology: *const ffi::ast_channel_tech) -> c_int {
    with_state(|state| {
        state.registrations += 1;
        if state.registrations == state.register_fail_at {
            return -1;
        }
        state.registered.push(technology as usize);
        0
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_unregister(technology: *const ffi::ast_channel_tech) {
    // SAFETY: the host unregisters its still-live boxed technology and static name.
    let name = unsafe { CStr::from_ptr((*technology).type_) }
        .to_string_lossy()
        .into_owned();
    with_state(|state| {
        state
            .registered
            .retain(|address| *address != technology as usize);
        state.unregistered.push(name);
    });
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_taskprocessor_get(
    _: *const c_char,
    _: ffi::ast_tps_options,
) -> *mut ffi::ast_taskprocessor {
    with_state(|state| {
        if state.taskprocessor_get_fails {
            return ptr::null_mut();
        }
        let mut taskprocessor = Box::new(0u8);
        let raw = ptr::from_mut(&mut *taskprocessor).cast();
        state.taskprocessors.insert(raw as usize, taskprocessor);
        raw
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_taskprocessor_unreference(
    taskprocessor: *mut ffi::ast_taskprocessor,
) -> *mut c_void {
    with_state(|state| {
        state.taskprocessors.remove(&(taskprocessor as usize));
    });
    ptr::null_mut()
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_dsp_new() -> *mut ffi::ast_dsp {
    with_state(|state| {
        if state.dsp_allocation_fails {
            return ptr::null_mut();
        }
        let mut dsp = Box::new(0u8);
        let raw = ptr::from_mut(&mut *dsp).cast();
        state.dsps.insert(raw as usize, dsp);
        raw
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_dsp_free(dsp: *mut ffi::ast_dsp) {
    with_state(|state| {
        state.dsps.remove(&(dsp as usize));
    });
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_dsp_set_features(_: *mut ffi::ast_dsp, features: c_int) {
    with_state(|state| state.dsp_features = features);
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_dsp_set_digitmode(_: *mut ffi::ast_dsp, mode: c_int) -> c_int {
    with_state(|state| state.dsp_digitmode = mode);
    0
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_module_ref(
    module: *mut ffi::ast_module,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> *mut ffi::ast_module {
    with_state(|state| state.module_references += 1);
    module
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_module_unref(
    _: *mut ffi::ast_module,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) {
    with_state(|state| state.module_references -= 1);
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ao2_lock(
    owner: *mut c_void,
    _: ffi::ao2_lock_req,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    with_state(|state| {
        if let Some(trace) = state
            .owner_lock_trace
            .as_mut()
            .filter(|trace| trace.owner == owner as usize)
        {
            trace.depth += 1;
            trace.events.push(("lock", trace.depth));
        }
    });
    0
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_tech_set(
    owner: *mut ffi::ast_channel,
    technology: *const ffi::ast_channel_tech,
) {
    // SAFETY: the host retains this opaque owner throughout synchronous setup.
    unsafe { (*owner.cast::<FakeChannel>()).technology = technology as usize };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_internal_fd_set(
    owner: *mut ffi::ast_channel,
    which: c_int,
    value: c_int,
) {
    // SAFETY: the host retains this opaque owner throughout synchronous setup.
    unsafe { (*owner.cast::<FakeChannel>()).fd = (which, value) };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_nativeformats_set(
    owner: *mut ffi::ast_channel,
    value: *mut ffi::ast_format_cap,
) {
    // SAFETY: the host retains this opaque owner throughout synchronous setup.
    unsafe { (*owner.cast::<FakeChannel>()).formats[0] = value as usize };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_set_readformat(
    owner: *mut ffi::ast_channel,
    value: *mut ffi::ast_format,
) {
    // SAFETY: the host retains this opaque owner throughout synchronous setup.
    unsafe { (*owner.cast::<FakeChannel>()).formats[1] = value as usize };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_set_writeformat(
    owner: *mut ffi::ast_channel,
    value: *mut ffi::ast_format,
) {
    // SAFETY: the host retains this opaque owner throughout synchronous setup.
    unsafe { (*owner.cast::<FakeChannel>()).formats[2] = value as usize };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_hangup(owner: *mut ffi::ast_channel) {
    with_state(|state| state.hangups += 1);
    // SAFETY: allocated owners retain the host technology until unregistration.
    let technology =
        unsafe { (*owner.cast::<FakeChannel>()).technology as *const ffi::ast_channel_tech };
    // SAFETY: this is the real registered hangup callback for this live owner.
    unsafe { (*technology).hangup.unwrap()(owner) };
    with_state(|state| {
        let channel = state.channels.remove(&(owner as usize)).unwrap();
        state.retired_channels.insert(owner as usize, channel);
    });
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_tech_pvt(owner: *const ffi::ast_channel) -> *mut c_void {
    // SAFETY: fixture owners are stable FakeChannel allocations for each call.
    unsafe { (*owner.cast::<FakeChannel>()).tech_pvt as *mut c_void }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_tech_pvt_set(owner: *mut ffi::ast_channel, value: *mut c_void) {
    // SAFETY: fixture owners are exclusively locked by the caller when mutated.
    unsafe { (*owner.cast::<FakeChannel>()).tech_pvt = value as usize };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_taskprocessor_push(
    _: *mut ffi::ast_taskprocessor,
    callback: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    data: *mut c_void,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    let result = with_state(|state| {
        state.taskprocessor_push_calls += 1;
        state.taskprocessor_push_result
    });
    if result != 0 {
        return result;
    }
    let Some(callback) = callback else {
        return -1;
    };
    let data = data as usize;
    // run_control's zero-capacity reply channel requires a separate worker.
    let worker = std::thread::spawn(move || {
        // SAFETY: successful submission transfers the live task to this worker.
        unsafe { callback(data as *mut c_void) };
    });
    with_state(|state| state.workers.push(worker));
    0
}

#[unsafe(no_mangle)]
unsafe extern "C" fn urp_test_message(kind: c_int, value: c_int, text: *const c_char) {
    // SAFETY: the C wrapper supplies a live, terminated bounded buffer.
    let text = unsafe { CStr::from_ptr(text) }
        .to_string_lossy()
        .into_owned();
    with_state(|state| state.messages.push((kind, value, text)));
}

#[unsafe(no_mangle)]
unsafe extern "C" fn urp_test_channel_alloc(
    state_value: c_int,
    name: *const c_char,
) -> *mut ffi::ast_channel {
    // SAFETY: the C wrapper supplies a live, terminated bounded channel name.
    let name = unsafe { CStr::from_ptr(name) }.to_owned();
    with_state(|state| {
        if state.channel_allocation_fails {
            return ptr::null_mut();
        }
        let mut channel = FakeChannel::new(ptr::null_mut());
        channel.name = name;
        channel.state = state_value;
        let raw = channel.as_ptr();
        state.channels.insert(raw as usize, channel);
        raw
    })
}

#[unsafe(no_mangle)]
static mut ast_null_frame: ffi::ast_frame = {
    // SAFETY: the frame contains only integer fields, pointers, and unions.
    let mut frame: ffi::ast_frame = unsafe { std::mem::zeroed() };
    frame.frametype = ffi::AST_FRAME_NULL;
    frame
};

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_setstate(
    owner: *mut ffi::ast_channel,
    value: ffi::ast_channel_state,
) -> c_int {
    // SAFETY: the serialized test retains exclusive owner storage.
    unsafe { (*owner.cast::<FakeChannel>()).state = value as c_int };
    0
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_moh_start(
    _: *mut ffi::ast_channel,
    class: *const c_char,
    fallback: *const c_char,
) -> c_int {
    // SAFETY: the callback provides optional terminated strings for this call.
    let class = if class.is_null() {
        String::new()
    } else {
        // SAFETY: a non-null class is terminated and retained for this call.
        unsafe { CStr::from_ptr(class) }
            .to_string_lossy()
            .into_owned()
    };
    // SAFETY: the host always supplies its static default class string.
    let fallback = unsafe { CStr::from_ptr(fallback) }
        .to_string_lossy()
        .into_owned();
    with_state(|state| {
        state.moh_started.push((class, fallback));
        state.moh_result
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_moh_stop(_: *mut ffi::ast_channel) {
    with_state(|state| state.moh_stopped += 1);
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ao2_trylock(
    _: *mut c_void,
    _: ffi::ao2_lock_req,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    let (result, hook) = with_state(|state| (state.trylock_result, state.trylock_hook.take()));
    if let Some(hook) = hook {
        hook();
    }
    result
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ao2_unlock(
    owner: *mut c_void,
    _: *const c_char,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    with_state(|state| {
        state.unlocks += 1;
        if let Some(trace) = state
            .owner_lock_trace
            .as_mut()
            .filter(|trace| trace.owner == owner as usize)
        {
            trace.depth -= 1;
            trace.events.push(("unlock", trace.depth));
        }
    });
    0
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_jb_configure(
    owner: *mut ffi::ast_channel,
    configuration: *const ffi::ast_jb_conf,
) {
    // SAFETY: the host supplies a fully initialized configuration for this call.
    let configuration = unsafe { *configuration };
    with_state(|state| state.jitter.push((owner as usize, configuration)));
}

unsafe fn capture_frame(frame: *const ffi::ast_frame) -> CapturedFrame {
    // SAFETY: the caller keeps the complete frame and any payload live for this copy.
    let frame = unsafe { &*frame };
    let bytes = if frame.datalen > 0 {
        // SAFETY: host callbacks supply exactly datalen readable bytes for this call.
        unsafe { std::slice::from_raw_parts(frame.data.ptr.cast::<u8>(), frame.datalen as usize) }
            .to_vec()
    } else {
        Vec::new()
    };
    CapturedFrame {
        kind: frame.frametype,
        // SAFETY: integer is the active member for control/DTMF; voice tests ignore it.
        subclass: frame.subclass.integer,
        samples: frame.samples,
        duration: frame.len,
        bytes,
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_state(owner: *const ffi::ast_channel) -> ffi::ast_channel_state {
    // SAFETY: the serialized fixture retains this opaque owner for the call.
    unsafe { (*owner.cast::<FakeChannel>()).state as ffi::ast_channel_state }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_queue_frame(
    _: *mut ffi::ast_channel,
    frame: *mut ffi::ast_frame,
) -> c_int {
    // SAFETY: queue callbacks keep their stack frame and payload live through this copy.
    let captured = unsafe { capture_frame(frame) };
    with_state(|state| {
        state.queued.push(captured);
        state.queue_result
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_dsp_process(
    _: *mut ffi::ast_channel,
    _: *mut ffi::ast_dsp,
    input: *mut ffi::ast_frame,
) -> *mut ffi::ast_frame {
    // SAFETY: the DSP caller supplies one live input frame and PCM buffer.
    let captured = unsafe { capture_frame(input) };
    let output = with_state(|state| {
        state.dsp_input = Some(captured);
        state.dsp_output
    });
    match output {
        DspOutput::Input => input,
        DspOutput::Null => ptr::null_mut(),
        DspOutput::Allocated { kind, digit } => {
            // SAFETY: ast_frame is initialized to its empty all-zero representation.
            let mut frame: ffi::ast_frame = unsafe { std::mem::zeroed() };
            frame.frametype = kind;
            frame.subclass.integer = digit;
            Box::into_raw(Box::new(frame))
        }
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_frame_free(frame: *mut ffi::ast_frame, _: c_int) {
    // SAFETY: delivery frees only separately allocated results from ast_dsp_process.
    drop(unsafe { Box::from_raw(frame) });
    with_state(|state| state.freed_frames += 1);
}

#[unsafe(no_mangle)]
pub(crate) static mut ast_config_AST_CONFIG_DIR: *const c_char = c"/test-config".as_ptr();

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_read_textfile(path: *const c_char) -> *mut c_char {
    // SAFETY: read_configuration retains its NUL-terminated path through this call.
    let path = unsafe { CStr::from_ptr(path) }
        .to_string_lossy()
        .into_owned();
    with_state(|state| {
        state.configuration_paths.push(path);
        state
            .configuration_text
            .as_ref()
            .map_or(ptr::null_mut(), |text| {
                // SAFETY: strdup copies the complete retained C string into a malloc allocation.
                unsafe { libc::strdup(text.as_ptr()) }
            })
    })
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_free_ptr(allocation: *mut c_void) {
    // SAFETY: the caller transfers the malloc allocation returned by ast_read_textfile.
    unsafe { libc::free(allocation) };
    with_state(|state| state.freed_texts += 1);
}
