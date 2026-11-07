use super::*;

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::ptr;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::host::support::Fixture as HostFixture;
use crate::host::support::timing_support;
use crate::host::support::{
    FakeChannel as NativeChannel, product_support, with_state as with_native_state,
};

thread_local! {
    static AUDIO_ACTIVE: Cell<bool> = const { Cell::new(false) };
    static AUDIO_ALLOCS: Cell<usize> = const { Cell::new(0) };
    static TEARDOWN_CHANNEL: Cell<*mut FakeChannel> = const { Cell::new(ptr::null_mut()) };
}

struct CheckedAllocator;

// SAFETY: allocator inputs pass unchanged to System; thread-local counters allocate nothing.
unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        AUDIO_ACTIVE.with(|active| {
            if active.get() {
                AUDIO_ALLOCS.with(|count| count.set(count.get() + 1));
            }
        });
        // SAFETY: preserve the caller's valid allocation layout.
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        // SAFETY: preserve the original allocation's pointer and matching layout.
        unsafe { System.dealloc(pointer, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

fn native_channel() -> *mut ffi::ast_channel {
    let mut channel = NativeChannel::new(ptr::null_mut());
    channel.name = c"IAX2/native".to_owned();
    channel.application = c"Rpt".to_owned();
    channel.data = c"Remote Rx".to_owned();
    let raw = channel.as_ptr();
    with_native_state(|state| state.channels.insert(raw as usize, channel));
    raw
}

#[test]
fn production_operation_table_preserves_asterisk_metadata_and_ownership() {
    let _fixture = HostFixture::new();
    reset();
    let operations = production_asterisk_operations();
    let channel = native_channel();
    let rate = 48_000_u32;
    let module = ptr::dangling_mut();
    // SAFETY: Asterisk initializes a caller-owned empty audiohook aggregate.
    let mut hook: ffi::ast_audiohook = unsafe { std::mem::zeroed() };
    let hook_address = ptr::from_mut(&mut hook) as usize;
    let mutex_address = ptr::addr_of_mut!(hook.lock) as usize;
    // SAFETY: the fixture retains every channel, format, hook, and metadata input.
    unsafe {
        assert!(c_bytes(ptr::null()).is_empty());
        assert_eq!(c_text(c"\xff".as_ptr()), "\u{fffd}");
        (operations.channel_lock)(channel);
        assert_eq!(
            CStr::from_ptr((operations.channel_name)(channel)),
            c"IAX2/native"
        );
        assert_eq!(
            CStr::from_ptr((operations.channel_application)(channel)),
            c"Rpt"
        );
        assert_eq!(
            CStr::from_ptr((operations.channel_data)(channel)),
            c"Remote Rx"
        );
        assert_eq!((operations.channel_sample_rate)(channel), 0);
        with_native_state(|state| {
            state.channels.get_mut(&(channel as usize)).unwrap().formats[1] =
                ptr::from_ref(&rate) as usize
        });
        assert_eq!((operations.channel_sample_rate)(channel), rate);
        assert_eq!(
            (operations.format_sample_rate)(ptr::from_ref(&rate).cast_mut().cast()),
            rate
        );
        (operations.channel_unlock)(channel);

        assert_eq!((operations.audiohook_init)(&mut hook), 0);
        assert_eq!(hook.type_, ffi::AST_AUDIOHOOK_TYPE_MANIPULATE);
        assert_eq!(hook.init_flags, ffi::AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
        assert_eq!(CStr::from_ptr(hook.source), c"USBRadioPlus");
        assert_eq!(hook.status, ffi::AST_AUDIOHOOK_STATUS_NEW);
        (operations.audiohook_lock)(&mut hook);
        (operations.audiohook_unlock)(&mut hook);
        assert_eq!((operations.audiohook_attach)(channel, &mut hook), 0);
        assert_eq!(hook.status, ffi::AST_AUDIOHOOK_STATUS_RUNNING);
        (operations.audiohook_detach)(&mut hook);
        (operations.audiohook_destroy)(&mut hook);
        assert_eq!(hook.status, ffi::AST_AUDIOHOOK_STATUS_DONE);

        assert!((operations.datastore_find)(channel, datastore_info()).is_null());
        let datastore = (operations.datastore_alloc)(datastore_info(), module);
        assert!(!datastore.is_null());
        assert_eq!((*datastore).info, datastore_info());
        assert_eq!((*datastore).mod_, module);
        (operations.datastore_add)(channel, datastore);
        assert_eq!(
            (operations.datastore_find)(channel, datastore_info()),
            datastore
        );
        (operations.datastore_remove)(channel, datastore);
        assert!((operations.datastore_find)(channel, datastore_info()).is_null());
        (operations.datastore_free)(datastore);

        let iterator = (operations.iterator_new)();
        assert!(!iterator.is_null());
        assert_eq!((operations.iterator_next)(iterator), channel);
        (operations.channel_unref)(channel);
        assert!((operations.iterator_next)(iterator).is_null());
        (operations.iterator_destroy)(iterator);
    }
    (operations.log_notice)("attached");
    (operations.log_error)("failed");
    with_native_state(|state| {
        assert_eq!(state.iterator_allocations, 1);
        assert_eq!(state.iterator_frees, 1);
        assert_eq!(state.channel_unrefs, [channel as usize]);
        assert_eq!(state.datastore_allocations, state.datastore_frees);
        assert_eq!(
            state.audiohook_calls,
            [
                ("init", hook_address),
                ("lock", mutex_address),
                ("unlock", mutex_address),
                ("attach", hook_address),
                ("detach", hook_address),
                ("destroy", hook_address),
            ]
        );
        assert!(
            state
                .messages
                .contains(&(0, ffi::__LOG_NOTICE as c_int, "attached\n".into()))
        );
        assert!(
            state
                .messages
                .contains(&(0, ffi::__LOG_ERROR as c_int, "failed\n".into()))
        );
    });
}

#[test]
fn native_asterisk_link_publication_balances_every_failure_stage() {
    for staged in [false, true] {
        for failure in 0..4 {
            let _fixture = HostFixture::new();
            reset();
            let channel = native_channel();
            let host = LinkHost::with_operations(
                ptr::dangling_mut(),
                ptr::dangling_mut(),
                fake_product_operations(),
                production_asterisk_operations(),
            );
            with_native_state(|state| match failure {
                1 => state.audiohook_init_result = -1,
                2 => state.datastore_allocation_fails = true,
                3 => state.audiohook_attach_result = -1,
                _ => {}
            });
            let mut reload = LinkReload::default();
            // SAFETY: the shared fixture retains this complete channel throughout publication.
            let result = unsafe {
                if staged {
                    host.stage_channel(channel, Some("alpha"), &mut reload)
                        .map(|()| true)
                } else {
                    host.attach_channel(channel, "alpha")
                }
            };
            if failure == 0 {
                assert_eq!(result, Ok(true));
                reload.finish(true);
                // SAFETY: publication retained a complete active graph and datastore.
                unsafe {
                    assert_eq!(
                        host.observe_channel(channel)
                            .unwrap()
                            .unwrap()
                            .observation
                            .processed_blocks,
                        7
                    );
                    let hook =
                        with_native_state(|state| state.channels[&(channel as usize)].audiohook)
                            as *mut ffi::ast_audiohook;
                    let rate = 48_000_u32;
                    let mut samples = [0_i16; 160];
                    let mut frame = voice_frame(&mut samples);
                    frame.subclass.__bindgen_anon_1.format = ptr::from_ref(&rate).cast_mut().cast();
                    assert_eq!(
                        (*hook).manipulate_callback.unwrap()(
                            hook,
                            channel,
                            &mut frame,
                            ffi::AST_AUDIOHOOK_DIRECTION_WRITE
                        ),
                        0
                    );
                    assert_eq!(samples[0], 1);
                    host.detach_channel(channel);
                }
                timing_support::drain(channel);
                assert_eq!(
                    with_state(|state| state.process_calls.clone()),
                    [(1, URP_AST_LINK_DIRECTION_WRITE, 48_000, 160)]
                );
            } else {
                assert_eq!(result, Err(LinkHostError::Asterisk));
            }
            with_native_state(|state| {
                assert_eq!(state.channels[&(channel as usize)].datastore, 0);
                assert_eq!(state.channels[&(channel as usize)].audiohook, 0);
                assert_eq!(state.datastore_allocations, state.datastore_frees);
                assert_eq!(
                    state
                        .audiohook_calls
                        .iter()
                        .filter(|(name, _)| *name == "destroy")
                        .count(),
                    usize::from(failure != 1)
                );
            });
            assert!(with_state(|state| state.graphs.is_empty()));
        }
    }
}

#[test]
fn production_scanner_start_rejects_invalid_and_failed_thread_ownership() {
    use crate::host::support::{urp_test_fail_thread_create, urp_test_thread_create_calls};

    let _fixture = HostFixture::new();
    reset();
    let descriptor = product_support::descriptor();
    assert_eq!(
        start(descriptor, ptr::null_mut(), ptr::null_mut()),
        crate::URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: one thread-local failure affects only the next scanner spawn.
    unsafe { urp_test_fail_thread_create(1) };
    let failed = start(descriptor, ptr::dangling_mut(), ptr::dangling_mut());
    // SAFETY: retrieve and disarm the calling thread's native test seam.
    let attempts = unsafe {
        let attempts = urp_test_thread_create_calls();
        urp_test_fail_thread_create(0);
        attempts
    };
    assert_eq!(failed, URP_AST_ASTERISK_FAILURE);
    assert_eq!(attempts, 1);
    assert!(lock(running_host()).is_none());
    assert_eq!(
        start(descriptor, ptr::dangling_mut(), ptr::dangling_mut()),
        URP_AST_OK
    );
    assert_eq!(
        start(descriptor, ptr::dangling_mut(), ptr::dangling_mut()),
        URP_AST_ASTERISK_FAILURE
    );
    stop();
    assert!(lock(running_host()).is_none());
    with_native_state(|state| assert_eq!(state.iterator_allocations, state.iterator_frees));
}

#[test]
fn discovery_skips_ineligible_channels_and_empty_external_datastores() {
    let _fixture = HostFixture::new();
    reset();
    let channel = native_channel();
    let host = LinkHost::with_operations(
        ptr::dangling_mut(),
        ptr::dangling_mut(),
        fake_product_operations(),
        production_asterisk_operations(),
    );
    let mut reload = LinkReload::default();
    // SAFETY: the shared fixture owns the referenced channel and datastore.
    unsafe {
        assert!(host.observe_channel(channel).unwrap().is_none());
        host.stage_channel(channel, None, &mut reload).unwrap();
        with_native_state(|state| {
            state
                .channels
                .get_mut(&(channel as usize))
                .unwrap()
                .application = c"Other".to_owned();
        });
        assert_eq!(host.attach_channel(channel, "alpha"), Ok(false));
        host.stage_channel(channel, Some("alpha"), &mut reload)
            .unwrap();
        let datastore = (host.asterisk.datastore_alloc)(datastore_info(), host.module_self);
        (host.asterisk.datastore_add)(channel, datastore);
        assert!(host.snapshot(channel).hook.is_none());
        assert!(host.statistics_all().unwrap().is_empty());
        host.detach_channel(channel);
        with_native_state(|state| state.iterator_allocation_fails = true);
        assert!(matches!(
            host.statistics_all(),
            Err(LinkHostError::Asterisk)
        ));
    }
    assert!(reload.hooks.is_empty());
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn duplicate_publication_and_teardown_release_only_their_own_graphs() {
    for staged in [false, true] {
        let _fixture = HostFixture::new();
        reset();
        let host = fake_host();
        let mut channel = FakeChannel::eligible("IAX2/duplicate", 8_000);
        assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
        assert!(
            // SAFETY: a competing publication already owns this live channel datastore.
            unsafe {
                host.install(
                    channel.raw(),
                    "alpha",
                    "IAX2/duplicate",
                    8_000,
                    if staged {
                        InstallMode::Staged
                    } else {
                        InstallMode::Active
                    },
                )
            }
            .unwrap()
            .is_none()
        );
        assert_eq!(with_state(|state| state.graphs.len()), 1);
        detach(&host, &mut channel);
        assert!(with_state(|state| state.graphs.is_empty()));
    }
    for result in [0, -1] {
        let _fixture = HostFixture::new();
        reset();
        with_state(|state| state.attach_result = result);
        let mut host = fake_host();
        host.asterisk.audiohook_attach = teardown_during_attach;
        let mut channel = FakeChannel::eligible("IAX2/teardown", 8_000);
        assert_eq!(
            attach(&host, &mut channel, "alpha"),
            Err(LinkHostError::Asterisk)
        );
        assert!(channel.datastore.is_null());
        assert!(with_state(|state| state.graphs.is_empty()));
        assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
        assert_eq!(
            with_state(|state| state.detach_calls),
            usize::from(result == 0)
        );
    }
}

unsafe fn teardown_during_attach(
    channel: *mut ffi::ast_channel,
    hook: *mut ffi::ast_audiohook,
) -> i32 {
    // SAFETY: publication retains the builder reference while external teardown runs.
    unsafe {
        let result = fake_audiohook_attach(channel, hook);
        let datastore = (*channel.cast::<FakeChannel>()).datastore;
        fake_datastore_remove(channel, datastore);
        fake_datastore_free(datastore);
        result
    }
}

#[test]
fn dormant_hooks_preserve_candidates_and_surface_product_errors() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/dormant", 8_000);
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    let retained = retain(&host, &mut channel).unwrap();
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    assert!(observe(&host, &mut channel).unwrap().is_none());
    let mut samples = [9_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert_eq!(samples[0], 9);
    for status in [URP_AST_OK, URP_AST_NOT_READY] {
        with_state(|state| state.prepare_result = status);
        assert_eq!(
            retained.prepare_reload(host.driver, 8_000),
            Err(LinkHostError::Asterisk)
        );
        assert_eq!(with_state(|state| state.graphs.len()), 1);
    }
    reload.finish(false);
    with_state(|state| state.prepare_result = -8);
    assert_eq!(
        attach(&host, &mut channel, "alpha"),
        Err(LinkHostError::Product(-8))
    );
    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    assert!(with_state(|state| state.graphs.is_empty()));
    with_state(|state| state.prepare_result = URP_AST_OK);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    with_state(|state| state.observe_result = -8);
    assert!(matches!(
        observe(&host, &mut channel),
        Err(LinkHostError::Product(-8))
    ));
    register(&mut channel);
    // SAFETY: the fixture retains every channel visited by this synchronous query.
    assert!(unsafe { host.statistics_all() }.unwrap().is_empty());
    detach(&host, &mut channel);
    assert_eq!(
        retained.prepare_active_if_missing(host.driver, "alpha", 8_000),
        Ok(())
    );
    drop(retained);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn preparation_rejects_missing_graph_and_retires_candidate_after_teardown() {
    let _fixture = HostFixture::new();
    reset();
    let mut host = fake_host();
    with_state(|state| state.prepare_empty = true);
    assert_eq!(
        host.prepare("alpha", 8_000, false),
        Err(LinkHostError::Product(URP_AST_OK))
    );
    with_state(|state| state.prepare_empty = false);
    let mut channel = FakeChannel::eligible("IAX2/preparing", 8_000);
    host.driver = channel.raw().cast();
    host.product.prepare = teardown_during_prepare;
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(false);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    timer_tick(&mut channel);
    timer_tick(&mut channel);
    assert!(channel.datastore.is_null());
    assert!(with_state(|state| state.graphs.is_empty()));
    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
}

unsafe extern "C" fn teardown_during_prepare(
    driver: *mut c_void,
    profile: *const u8,
    length: u32,
    rate: u32,
    maximum: u32,
    output: *mut *mut c_void,
) -> i32 {
    // SAFETY: the fixture supplies a live channel as context and keeps the retained hook alive.
    unsafe {
        let result = fake_prepare(driver, profile, length, rate, maximum, output);
        let channel = driver.cast::<ffi::ast_channel>();
        let datastore = (*channel.cast::<FakeChannel>()).datastore;
        fake_datastore_remove(channel, datastore);
        fake_datastore_free(datastore);
        result
    }
}

#[test]
fn detached_reload_discards_replacement_and_malformed_frames_are_ignored() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/frames", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let mut samples = [9_i16; 160];
    let mut frame = voice_frame(&mut samples);
    assert_eq!(
        // SAFETY: a null hook is rejected before either frame or channel is dereferenced.
        unsafe {
            link_audiohook_callback(
                ptr::null_mut(),
                ptr::null_mut(),
                &mut frame,
                ffi::AST_AUDIOHOOK_DIRECTION_READ,
            )
        },
        0
    );
    for count in [0, -1] {
        frame.samples = count;
        invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    }
    frame.samples = 160;
    frame.data.ptr = ptr::null_mut();
    invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert!(with_state(|state| state.process_calls.is_empty()));
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    detach(&host, &mut channel);
    reload.finish(true);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn reference_count_guards_and_final_owner_preserve_teardown() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let graph = host.prepare("alpha", 8_000, false).unwrap().unwrap();
    let raw = Box::into_raw(Box::new(LinkHook::new(
        host.product,
        host.asterisk,
        "alpha",
        "IAX2/owned",
        8_000,
        graph,
        InstallMode::Active,
    )));
    // SAFETY: this test initializes a pinned, exclusively owned hook before creating its RAII owner.
    let owned = unsafe {
        fake_audiohook_init(ptr::addr_of_mut!((*raw).audiohook));
        (*raw).attachment.store(LINK_ATTACHED, Ordering::Release);
        LinkHookRef::from_owned(raw)
    };
    for invalid in [0, usize::MAX] {
        // SAFETY: exclusive test ownership permits a temporary count fault; the count is restored before drop.
        unsafe { (*raw).references.store(invalid, Ordering::Release) };
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            // SAFETY: the allocation remains live while the count invariant is deliberately tested.
            unsafe { LinkHook::retain(raw) };
        }));
        // SAFETY: restore the one real owner even when the assertion unwound.
        unsafe { (*raw).references.store(1, Ordering::Release) };
        assert!(result.is_err());
    }
    drop(owned);
    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[derive(Default)]
struct FakeState {
    events: Vec<&'static str>,
    notices: Vec<String>,
    diagnostics: Vec<String>,
    prepare_profiles: Vec<(&'static str, String)>,
    graphs: HashMap<usize, FakeGraph>,
    next_graph: usize,
    prepare_result: i32,
    preparation_observer: Option<fn()>,
    prepare_empty: bool,
    reload_unchanged: bool,
    reload_compare_result: i32,
    reload_comparisons: Vec<(String, u32, u32)>,
    observe_result: i32,
    attach_result: i32,
    process_calls: Vec<(usize, u32, u32, u32)>,
    silence_inputs: Vec<bool>,
    process_result: i32,
    wakes: usize,
    detach_calls: usize,
    destroy_hook_calls: usize,
    channels: Vec<usize>,
}

#[derive(Clone, Copy)]
struct FakeGraph {
    observation: UrpAstLinkObservation,
}

struct FakeChannel {
    name: Vec<u8>,
    application: Vec<u8>,
    data: Vec<u8>,
    sample_rate_hz: u32,
    datastore: *mut ffi::ast_datastore,
    audiohook: *mut ffi::ast_audiohook,
}

fn state() -> &'static Mutex<FakeState> {
    static STATE: OnceLock<Mutex<FakeState>> = OnceLock::new();
    STATE.get_or_init(|| {
        Mutex::new(FakeState {
            next_graph: 1,
            ..FakeState::default()
        })
    })
}

fn with_state<T>(operation: impl FnOnce(&mut FakeState) -> T) -> T {
    operation(&mut state().lock().unwrap())
}

thread_local! {
    static PENDING_HOOK: Cell<*mut LinkHook> = const { Cell::new(ptr::null_mut()) };
}

fn mark_reload_pending_during_prepare() {
    PENDING_HOOK.with(|hook| {
        // SAFETY: the test retains this hook through the synchronous preparation callback.
        unsafe { (*hook.get()).reload_pending.store(true, Ordering::Release) };
    });
}

fn reset() {
    stop();
    *state().lock().unwrap() = FakeState {
        next_graph: 1,
        ..FakeState::default()
    };
}

/// Retain the existing injected link fixture for concrete reload integration.
/// The caller holds HostFixture's process-state serialization guard.
pub(in crate::host) struct RunningFixture {
    _channel: Option<Box<FakeChannel>>,
}

impl RunningFixture {
    pub(in crate::host) fn new(with_channel: bool) -> Self {
        reset();
        let mut channel = with_channel.then(|| FakeChannel::eligible("IAX2/reload", 8_000));
        if let Some(channel) = channel.as_mut() {
            register(channel);
        }
        assert_eq!(start_with(fake_host(), missing_profile), URP_AST_OK);
        Self { _channel: channel }
    }

    pub(in crate::host) fn preparation_result(&self, result: i32) {
        with_state(|state| state.prepare_result = result);
    }

    pub(in crate::host) fn observe_preparation(&self, observer: fn()) {
        with_state(|state| state.preparation_observer = Some(observer));
    }

    pub(in crate::host) fn set_observation(&self, observation: UrpAstLinkObservation) {
        with_state(|state| {
            for graph in state.graphs.values_mut() {
                graph.observation = observation;
            }
        });
    }

    pub(in crate::host) fn peer(&mut self) -> *mut ffi::ast_channel {
        self._channel.as_mut().unwrap().raw()
    }
}

impl Drop for RunningFixture {
    fn drop(&mut self) {
        stop();
    }
}

impl FakeChannel {
    fn eligible(name: &str, sample_rate_hz: u32) -> Box<Self> {
        Box::new(Self {
            name: nul(name),
            application: nul("Rpt"),
            data: nul("Remote Rx"),
            sample_rate_hz,
            datastore: ptr::null_mut(),
            audiohook: ptr::null_mut(),
        })
    }

    fn raw(&mut self) -> *mut ffi::ast_channel {
        ptr::from_mut(self).cast()
    }
}

impl Drop for FakeChannel {
    fn drop(&mut self) {
        if !self.datastore.is_null() {
            // SAFETY: the fake channel owns this fake datastore exactly once.
            unsafe { fake_datastore_free(self.datastore) };
            self.datastore = ptr::null_mut();
        }
        timing_support::destroy(self.raw());
    }
}

fn nul(value: &str) -> Vec<u8> {
    value
        .as_bytes()
        .iter()
        .copied()
        .chain(std::iter::once(0))
        .collect()
}

fn fake_host() -> LinkHost {
    // SAFETY: zeroed optional C callbacks are valid; the six consumed entries are supplied below.
    let mut descriptor: UrpAstDescriptor = unsafe { std::mem::zeroed() };
    descriptor.link_prepare = Some(fake_prepare);
    descriptor.link_prepare_reload = Some(fake_prepare_reload);
    descriptor.link_reload_unchanged = Some(fake_reload_unchanged);
    descriptor.link_process = Some(fake_process);
    descriptor.link_observe = Some(fake_observe);
    descriptor.link_destroy = Some(fake_graph_destroy);
    let mut host = LinkHost::new(&descriptor, ptr::dangling_mut::<c_void>(), ptr::null_mut());
    host.asterisk = fake_asterisk_operations();
    host
}

fn fake_product_operations() -> LinkProductOperations {
    LinkProductOperations {
        prepare: fake_prepare,
        prepare_reload: fake_prepare_reload,
        reload_unchanged: fake_reload_unchanged,
        process: fake_process,
        observe: fake_observe,
        destroy: fake_graph_destroy,
    }
}

fn fake_asterisk_operations() -> AsteriskOperations {
    AsteriskOperations {
        audiohook_init: fake_audiohook_init,
        audiohook_attach: fake_audiohook_attach,
        audiohook_detach: fake_audiohook_detach,
        audiohook_destroy: fake_audiohook_destroy,
        audiohook_lock: fake_audiohook_lock,
        audiohook_unlock: fake_audiohook_unlock,
        timer_open: ffi::ast_timer_open,
        timer_close: ffi::ast_timer_close,
        timer_fd: ffi::ast_timer_fd,
        timer_set_rate: ffi::ast_timer_set_rate,
        timer_ack: ffi::ast_timer_ack,
        timer_get_event: ffi::ast_timer_get_event,
        framehook_attach: ffi::ast_framehook_attach,
        framehook_detach: ffi::ast_framehook_detach,
        channel_fd_add: ffi::ast_channel_fd_add,
        channel_fd: ffi::ast_channel_fd,
        channel_fdno: ffi::ast_channel_fdno,
        channel_fdno_set: ffi::ast_channel_fdno_set,
        channel_set_fd: ffi::ast_channel_set_fd,
        channel_wake: fake_channel_wake,
        module_ref: ffi::__ast_module_ref,
        module_unref: ffi::__ast_module_unref,
        datastore_alloc: fake_datastore_alloc,
        datastore_free: fake_datastore_free,
        channel_lock: fake_channel_lock,
        channel_unlock: fake_channel_unlock,
        datastore_find: fake_datastore_find,
        datastore_add: fake_datastore_add,
        datastore_remove: fake_datastore_remove,
        channel_name: fake_channel_name,
        channel_application: fake_channel_application,
        channel_data: fake_channel_data,
        channel_sample_rate: fake_channel_sample_rate,
        format_sample_rate: fake_format_sample_rate,
        iterator_new: fake_iterator_new,
        iterator_next: fake_iterator_next,
        iterator_destroy: fake_iterator_destroy,
        channel_unref: fake_channel_unref,
        log_notice: fake_log_notice,
        log_error: fake_log_error,
    }
}

fn fake_log_notice(message: &str) {
    with_state(|state| state.notices.push(message.to_owned()));
}

fn fake_log_error(message: &str) {
    with_state(|state| state.diagnostics.push(message.to_owned()));
}

unsafe fn fake_prepare_common(
    profile: *const u8,
    profile_length: u32,
    output: *mut *mut c_void,
    event: &'static str,
) -> i32 {
    if let Some(observer) = with_state(|state| state.preparation_observer) {
        observer();
    }
    // SAFETY: the host supplies a readable byte-counted profile.
    let profile = unsafe { std::slice::from_raw_parts(profile, profile_length as usize) };
    let profile = String::from_utf8(profile.to_vec()).expect("profile must be UTF-8");
    with_state(|state| {
        state.events.push(event);
        state.prepare_profiles.push((event, profile));
        if state.prepare_result != URP_AST_OK || state.prepare_empty {
            return state.prepare_result;
        }
        let id = state.next_graph;
        state.next_graph += 1;
        state.graphs.insert(
            id,
            FakeGraph {
                observation: UrpAstLinkObservation {
                    struct_size: std::mem::size_of::<UrpAstLinkObservation>() as u32,
                    abi_version: ABI_VERSION,
                    processed_blocks: id as u64 * 7,
                    bypassed_blocks: id as u64 * 2,
                    failed_blocks: id as u64,
                    ..UrpAstLinkObservation::default()
                },
            },
        );
        // SAFETY: the host supplies writable graph-pointer storage.
        unsafe { output.write(id as *mut c_void) };
        URP_AST_OK
    })
}

unsafe extern "C" fn fake_prepare(
    _driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
    _sample_rate_hz: u32,
    _maximum_frame_count: u32,
    output: *mut *mut c_void,
) -> i32 {
    // SAFETY: forwarded test fixture storage follows the product ABI.
    unsafe { fake_prepare_common(channel_name, channel_name_length, output, "prepare") }
}

unsafe extern "C" fn fake_reload_unchanged(
    _driver: *mut c_void,
    _link: *mut c_void,
    profile: *const u8,
    profile_length: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    output: *mut u32,
) -> c_int {
    // SAFETY: the host borrows its complete UTF-8 profile through this synchronous call.
    let profile = unsafe { std::slice::from_raw_parts(profile, profile_length as usize) };
    let profile = std::str::from_utf8(profile).unwrap();
    with_state(|state| {
        state
            .reload_comparisons
            .push((profile.to_owned(), sample_rate_hz, maximum_frame_count));
        // SAFETY: the host retains this writable result until the callback returns.
        unsafe { output.write(u32::from(state.reload_unchanged)) };
        state.reload_compare_result
    })
}

unsafe extern "C" fn fake_prepare_reload(
    _driver: *mut c_void,
    channel_name: *const u8,
    channel_name_length: u32,
    _sample_rate_hz: u32,
    _maximum_frame_count: u32,
    output: *mut *mut c_void,
) -> i32 {
    // SAFETY: forwarded test fixture storage follows the product ABI.
    unsafe { fake_prepare_common(channel_name, channel_name_length, output, "prepare_reload") }
}

unsafe extern "C" fn fake_process(
    graph: *mut c_void,
    direction: u32,
    sample_rate_hz: u32,
    samples: *mut i16,
    sample_count: u32,
) -> i32 {
    if direction == URP_AST_LINK_DIRECTION_SILENCE {
        // SAFETY: SILENCE supplies a nonempty workspace of the declared bounded length.
        let zero = unsafe { std::slice::from_raw_parts(samples, sample_count as usize) }
            .iter()
            .all(|sample| *sample == 0);
        with_state(|state| state.silence_inputs.push(zero));
    }
    let id = graph as usize;
    with_state(|state| {
        state
            .process_calls
            .push((id, direction, sample_rate_hz, sample_count));
    });
    if !samples.is_null() && sample_count != 0 {
        // SAFETY: the callback supplied at least one writable sample.
        unsafe { *samples = i16::try_from(id).unwrap() };
    }
    with_state(|state| state.process_result)
}

unsafe extern "C" fn fake_observe(graph: *mut c_void, output: *mut UrpAstLinkObservation) -> i32 {
    let outside_link_control = LINK_CONTROL.try_lock().is_ok();
    let observation = with_state(|state| {
        state.events.push(if outside_link_control {
            "observe_outside_link_control"
        } else {
            "observe_under_link_control"
        });
        state.graphs[&(graph as usize)].observation
    });
    // SAFETY: the caller supplies writable observation storage.
    unsafe { output.write(observation) };
    with_state(|state| state.observe_result)
}

unsafe extern "C" fn fake_graph_destroy(graph: *mut c_void) {
    with_state(|state| {
        state.events.push("graph_destroy");
        assert!(state.graphs.remove(&(graph as usize)).is_some());
    });
}

unsafe fn fake_audiohook_init(audiohook: *mut ffi::ast_audiohook) -> i32 {
    with_state(|state| state.events.push("audiohook_init"));
    // SAFETY: the host supplied its initialized-layout audiohook field.
    unsafe { (*audiohook).status = ffi::AST_AUDIOHOOK_STATUS_NEW };
    0
}

unsafe fn fake_audiohook_attach(
    channel: *mut ffi::ast_channel,
    audiohook: *mut ffi::ast_audiohook,
) -> i32 {
    with_state(|state| {
        state.events.push("audiohook_attach");
        if state.attach_result != 0 {
            return state.attach_result;
        }
        // SAFETY: fixture pointers came from FakeChannel and LinkHook.
        unsafe {
            (*channel.cast::<FakeChannel>()).audiohook = audiohook;
            (*audiohook).status = ffi::AST_AUDIOHOOK_STATUS_RUNNING;
        }
        0
    })
}

unsafe fn fake_audiohook_detach(audiohook: *mut ffi::ast_audiohook) {
    with_state(|state| {
        state.detach_calls += 1;
        state.events.push("audiohook_detach");
    });
    // SAFETY: fixture pointer refers to the live embedded audiohook.
    unsafe { (*audiohook).status = ffi::AST_AUDIOHOOK_STATUS_DONE };
}

unsafe fn fake_audiohook_destroy(audiohook: *mut ffi::ast_audiohook) {
    with_state(|state| {
        state.destroy_hook_calls += 1;
        state.events.push("audiohook_destroy");
    });
    // SAFETY: fixture pointer refers to the live embedded audiohook.
    unsafe { (*audiohook).status = ffi::AST_AUDIOHOOK_STATUS_DONE };
}

unsafe fn fake_audiohook_lock(_audiohook: *mut ffi::ast_audiohook) {
    with_state(|state| state.events.push("audiohook_lock"));
}

unsafe fn fake_audiohook_unlock(_audiohook: *mut ffi::ast_audiohook) {
    with_state(|state| state.events.push("audiohook_unlock"));
}

unsafe fn fake_datastore_alloc(
    info: *const ffi::ast_datastore_info,
    _module: *mut ffi::ast_module,
) -> *mut ffi::ast_datastore {
    // SAFETY: every bit pattern in the generated C datastore structure is valid.
    let empty = unsafe { std::mem::zeroed() };
    let mut datastore: Box<ffi::ast_datastore> = Box::new(empty);
    datastore.info = info;
    Box::into_raw(datastore)
}

unsafe fn fake_datastore_free(datastore: *mut ffi::ast_datastore) {
    if datastore.is_null() {
        return;
    }
    // SAFETY: the fixture owns this allocation and its optional payload.
    let datastore = unsafe { Box::from_raw(datastore) };
    // SAFETY: fake_datastore_alloc installed a live process-lifetime info pointer.
    let info = unsafe { datastore.info.as_ref() };
    if let Some(destroy) = info.and_then(|info| info.destroy) {
        // SAFETY: the datastore payload follows its registered destructor contract.
        unsafe { destroy(datastore.data) };
    }
}

unsafe fn fake_channel_lock(_channel: *mut ffi::ast_channel) {}
unsafe fn fake_channel_unlock(_channel: *mut ffi::ast_channel) {}
unsafe fn fake_channel_wake(_channel: *mut ffi::ast_channel) {
    with_state(|state| state.wakes += 1);
}

unsafe fn fake_datastore_find(
    channel: *mut ffi::ast_channel,
    _info: *const ffi::ast_datastore_info,
) -> *mut ffi::ast_datastore {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).datastore }
}

unsafe fn fake_datastore_add(channel: *mut ffi::ast_channel, datastore: *mut ffi::ast_datastore) {
    with_state(|state| state.events.push("datastore_add"));
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).datastore = datastore };
}

unsafe fn fake_datastore_remove(
    channel: *mut ffi::ast_channel,
    datastore: *mut ffi::ast_datastore,
) {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    let channel = unsafe { &mut *channel.cast::<FakeChannel>() };
    if channel.datastore == datastore {
        channel.datastore = ptr::null_mut();
    }
}

unsafe fn fake_channel_name(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).name.as_ptr().cast() }
}

unsafe fn fake_channel_application(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).application.as_ptr().cast() }
}

unsafe fn fake_channel_data(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).data.as_ptr().cast() }
}

unsafe fn fake_channel_sample_rate(channel: *mut ffi::ast_channel) -> u32 {
    // SAFETY: fixture channel pointer came from FakeChannel::raw.
    unsafe { (*channel.cast::<FakeChannel>()).sample_rate_hz }
}

unsafe fn fake_format_sample_rate(_format: *mut ffi::ast_format) -> u32 {
    0
}

struct FakeIterator {
    channels: Vec<usize>,
    index: usize,
}

unsafe fn fake_iterator_new() -> *mut ffi::ast_channel_iterator {
    let channels = with_state(|state| {
        state.events.push("iterator_new");
        state.channels.clone()
    });
    Box::into_raw(Box::new(FakeIterator { channels, index: 0 })).cast()
}

unsafe fn fake_iterator_next(iterator: *mut ffi::ast_channel_iterator) -> *mut ffi::ast_channel {
    // SAFETY: fake_iterator_new created this live iterator allocation.
    let iterator = unsafe { &mut *iterator.cast::<FakeIterator>() };
    let Some(channel) = iterator.channels.get(iterator.index).copied() else {
        return ptr::null_mut();
    };
    iterator.index += 1;
    channel as *mut ffi::ast_channel
}

unsafe fn fake_iterator_destroy(iterator: *mut ffi::ast_channel_iterator) {
    // SAFETY: consumes the allocation returned by fake_iterator_new exactly once.
    drop(unsafe { Box::from_raw(iterator.cast::<FakeIterator>()) });
}

unsafe fn fake_channel_unref(_channel: *mut ffi::ast_channel) {}

fn voice_frame(samples: &mut [i16]) -> ffi::ast_frame {
    // SAFETY: all-zero is a valid empty C frame before fields are assigned.
    let mut frame: ffi::ast_frame = unsafe { std::mem::zeroed() };
    frame.frametype = ffi::AST_FRAME_VOICE;
    frame.samples = i32::try_from(samples.len()).unwrap();
    frame.data.ptr = samples.as_mut_ptr().cast();
    frame
}

fn invoke(
    channel: &mut FakeChannel,
    frame: *mut ffi::ast_frame,
    direction: ffi::ast_audiohook_direction,
) {
    let hook = channel.audiohook;
    assert!(!hook.is_null());
    // SAFETY: hook is the live audiohook installed on this fake channel.
    let callback = unsafe { (*hook).manipulate_callback }.unwrap();
    // SAFETY: all pointers refer to live fixture objects for this call.
    let result = unsafe { callback(hook, channel.raw(), frame, direction) };
    assert_eq!(result, 0);
}

fn attach(
    host: &LinkHost,
    channel: &mut FakeChannel,
    profile: &str,
) -> Result<bool, LinkHostError> {
    // SAFETY: the fake channel remains live through the synchronous operation.
    unsafe { host.attach_channel(channel.raw(), profile) }
}

fn retain(host: &LinkHost, channel: &mut FakeChannel) -> Option<LinkHookRef> {
    // SAFETY: the fake channel remains live and owns its attached datastore.
    unsafe { host.retain_attached(channel.raw()) }
}

fn detach(host: &LinkHost, channel: &mut FakeChannel) {
    // SAFETY: the fake channel remains live and owns its attached datastore.
    unsafe { host.detach_channel(channel.raw()) };
    timing_support::drain(channel.raw());
}

fn stage(
    host: &LinkHost,
    channel: &mut FakeChannel,
    profile: Option<&str>,
    reload: &mut LinkReload,
) -> Result<(), LinkHostError> {
    // SAFETY: the fake channel remains live through graph preparation.
    unsafe { host.stage_channel(channel.raw(), profile, reload) }
}

fn observe(
    host: &LinkHost,
    channel: &mut FakeChannel,
) -> Result<Option<LinkStatistics>, LinkHostError> {
    // SAFETY: the fake channel remains live through synchronous observation.
    unsafe { host.observe_channel(channel.raw()) }
}

fn mark_audiohook_done(channel: &mut FakeChannel) {
    // SAFETY: the fake channel owns a live attached audiohook.
    unsafe { (*channel.audiohook).status = ffi::AST_AUDIOHOOK_STATUS_DONE };
}

fn register(channel: &mut FakeChannel) {
    with_state(|state| state.channels.push(channel.raw() as usize));
}

fn fake_profile() -> Option<Box<str>> {
    Some("alpha".into())
}

fn missing_profile() -> Option<Box<str>> {
    with_state(|state| state.events.push("missing_profile_resolved"));
    None
}

fn ordered_fake_profile() -> Option<Box<str>> {
    let outside_link_control = LINK_CONTROL.try_lock().is_ok();
    with_state(|state| {
        state.events.push(if outside_link_control {
            "profile_before_link_control"
        } else {
            "profile_under_link_control"
        });
    });
    fake_profile()
}

fn wait_until(mut ready: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !ready() {
        assert!(
            Instant::now() < deadline,
            "scanner did not reach expected state"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn attachment_count() -> usize {
    with_state(|state| {
        state
            .events
            .iter()
            .filter(|event| **event == "audiohook_attach")
            .count()
    })
}

fn iterator_count() -> usize {
    with_state(|state| {
        state
            .events
            .iter()
            .filter(|event| **event == "iterator_new")
            .count()
    })
}

#[test]
fn audiohook_is_the_first_pinned_member() {
    assert_eq!(std::mem::offset_of!(LinkHook, audiohook), 0);
}

#[test]
fn graph_is_prepared_before_hook_publication() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-1", 8_000);

    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    assert_eq!(
        with_state(|state| state.notices.clone()),
        ["USBRadioPlus link processing attached to IAX2/506316-1"]
    );
    assert_eq!(
        with_state(|state| state.events.clone()),
        vec![
            "prepare",
            "audiohook_init",
            "datastore_add",
            "audiohook_attach"
        ]
    );

    // A duplicate scan neither prepares nor publishes another graph.
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    assert_eq!(with_state(|state| state.graphs.len()), 1);
}

#[test]
fn newly_staged_hook_reports_the_staged_attachment_notice() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-staged", 8_000);
    let mut reload = LinkReload::default();

    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    assert_eq!(
        with_state(|state| state.notices.clone()),
        ["USBRadioPlus link processing attached to IAX2/506316-staged for staged reload"]
    );
    reload.finish(false);
}

#[test]
fn dormant_hook_reactivation_uses_the_current_profile() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-current-profile", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));

    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(true);

    with_state(|state| {
        state.prepare_result = URP_AST_OK;
        state.prepare_profiles.clear();
    });
    assert_eq!(attach(&host, &mut channel, "beta"), Ok(false));
    assert_eq!(
        with_state(|state| state.prepare_profiles.clone()),
        vec![("prepare", "beta".to_owned())]
    );
}

#[test]
fn callback_processes_voice_without_control_plane_work() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-2", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));

    let events_before = with_state(|state| state.events.len());
    let mut samples = [9_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(
        &mut channel,
        ptr::from_mut(&mut frame),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    assert_eq!(samples[0], 1);
    assert_eq!(
        with_state(|state| state.process_calls.clone()),
        vec![(1, URP_AST_LINK_DIRECTION_READ, 8_000, 160)]
    );
    assert_eq!(with_state(|state| state.events.len()), events_before);

    frame.frametype = ffi::AST_FRAME_CONTROL;
    invoke(
        &mut channel,
        ptr::from_mut(&mut frame),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    invoke(
        &mut channel,
        ptr::null_mut(),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    mark_audiohook_done(&mut channel);
    frame.frametype = ffi::AST_FRAME_VOICE;
    invoke(
        &mut channel,
        ptr::from_mut(&mut frame),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    assert_eq!(with_state(|state| state.process_calls.len()), 1);
}

fn timer_tick(channel: &mut FakeChannel) {
    // SAFETY: a zeroed aggregate is a valid empty Asterisk frame.
    let mut frame: ffi::ast_frame = unsafe { std::mem::zeroed() };
    let returned = timing_support::event(
        channel.raw(),
        &raw mut frame,
        ffi::AST_FRAMEHOOK_EVENT_READ,
        true,
    );
    assert_eq!(returned, &raw mut frame);
}

#[test]
fn restarted_framehook_traversal_acknowledges_only_one_timer_quantum() {
    struct Replacement {
        frame: *mut ffi::ast_frame,
        replaced: bool,
    }
    unsafe extern "C" fn replace_once(
        _: *mut ffi::ast_channel,
        frame: *mut ffi::ast_frame,
        event: ffi::ast_framehook_event,
        data: *mut c_void,
    ) -> *mut ffi::ast_frame {
        if event == ffi::AST_FRAMEHOOK_EVENT_READ {
            // SAFETY: this fixture retains its replacement state through detachment.
            let state = unsafe { &mut *data.cast::<Replacement>() };
            if !state.replaced {
                state.replaced = true;
                return state.frame;
            }
        }
        frame
    }
    let _fixture = HostFixture::new();
    reset();
    let mut channel = FakeChannel::eligible("IAX2/restarted-read", 8_000);
    assert_eq!(attach(&fake_host(), &mut channel, "alpha"), Ok(true));
    // SAFETY: empty C frame/interface representations have valid null optional fields.
    let (mut first, mut replacement, mut interface): (
        ffi::ast_frame,
        ffi::ast_frame,
        ffi::ast_framehook_interface,
    ) = unsafe { std::mem::zeroed() };
    let mut state = Replacement {
        frame: &raw mut replacement,
        replaced: false,
    };
    interface.version = ffi::AST_FRAMEHOOK_INTERFACE_VERSION as u16;
    interface.event_cb = Some(replace_once);
    interface.data = (&raw mut state).cast();
    // SAFETY: the fixture retains this complete downstream framehook and its stack data.
    let id = unsafe { ffi::ast_framehook_attach(channel.raw(), &raw mut interface) };
    let result = timing_support::event(
        channel.raw(),
        &raw mut first,
        ffi::AST_FRAMEHOOK_EVENT_READ,
        true,
    );
    assert_eq!(result, &raw mut replacement);
    assert_eq!(with_state(|state| state.process_calls.len()), 1);
    assert_eq!(timing_support::counts().2, 1);
    // SAFETY: detach is requested before the retained stack callback state leaves scope.
    unsafe { ffi::ast_framehook_detach(channel.raw(), id) };
    timing_support::drain(channel.raw());
}

fn notify_datastore_move(
    datastore: *mut ffi::ast_datastore,
    old: *mut ffi::ast_channel,
    new: *mut ffi::ast_channel,
) {
    // SAFETY: the serialized fixture retains this registered datastore and both channel owners.
    unsafe {
        if let Some(fixup) = (*(*datastore).info).chan_fixup {
            fixup((*datastore).data, old, new);
        }
    }
}

fn assert_masquerade_teardown(attach_fails: bool) {
    let _fixture = HostFixture::new();
    reset();
    let mut source = FakeChannel::eligible("IAX2/move-source", 8_000);
    let mut destination = FakeChannel::eligible("IAX2/move-destination", 8_000);
    assert_eq!(attach(&fake_host(), &mut source, "alpha"), Ok(true));
    if attach_fails {
        timing_support::fail(timing_support::Failure::Attach);
    } else {
        timing_support::fail(timing_support::Failure::Ack);
        timer_tick(&mut source);
        timing_support::fail(timing_support::Failure::None);
    }
    let old = source.raw();
    let new = destination.raw();
    let datastore = source.datastore;
    timing_support::masquerade(old, new, || notify_datastore_move(datastore, old, new));
    timing_support::drain(new);
    assert_eq!(timing_support::counts().1, 1);
    // SAFETY: both fixture channels remain retained after the copied timer lease is destroyed.
    unsafe {
        assert_eq!(ffi::ast_channel_fd(old, 0), -1);
        assert_eq!(ffi::ast_channel_fd(new, 0), -1);
    }
    assert_eq!(with_native_state(|state| state.module_references), 0);
}

#[test]
fn masquerade_attach_failure_removes_copied_timer_fd_before_close() {
    assert_masquerade_teardown(true);
}

#[test]
fn masquerade_preserves_pending_asynchronous_detach() {
    assert_masquerade_teardown(false);
}

#[test]
fn idle_timer_advances_only_read_pcm_deficit() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/idle", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    assert_eq!(with_state(|state| state.wakes), 1);

    timer_tick(&mut channel);
    assert_eq!(
        with_state(|state| state.process_calls.clone()),
        [(1, 3, 8_000, 160)]
    );
    let mut samples = [0_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(
        &mut channel,
        &raw mut frame,
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    timer_tick(&mut channel);
    assert_eq!(
        with_state(|state| state.process_calls.clone()),
        [(1, 3, 8_000, 160), (1, 1, 8_000, 160)]
    );
    invoke(
        &mut channel,
        &raw mut frame,
        ffi::AST_AUDIOHOOK_DIRECTION_WRITE,
    );
    timer_tick(&mut channel);
    assert_eq!(
        with_state(|state| state.process_calls.clone()),
        [
            (1, 3, 8_000, 160),
            (1, 1, 8_000, 160),
            (1, 2, 8_000, 160),
            (1, 3, 8_000, 160),
        ]
    );
}

#[test]
fn idle_clock_preserves_fractional_rates_and_bounds_work_and_credit() {
    for rate in [8_000, 11_025, 44_100, 48_000, 96_000, 192_000, 384_000] {
        let _fixture = HostFixture::new();
        reset();
        let mut channel = FakeChannel::eligible("IAX2/fractional", rate);
        assert_eq!(attach(&fake_host(), &mut channel, "alpha"), Ok(true));
        let hz = rate.div_ceil(960).max(50);
        assert_eq!(timing_support::rates(), [hz]);
        for _ in 0..hz {
            timer_tick(&mut channel);
        }
        let counts = with_state(|state| {
            state
                .process_calls
                .iter()
                .map(|call| call.3)
                .collect::<Vec<_>>()
        });
        assert!(counts.iter().all(|count| *count <= 960));
        assert_eq!(counts.iter().sum::<u32>(), rate);
    }
    let mut clock = IdleClock::new(8_000);
    for _ in 0..10 {
        clock.credit(8_000, 960);
    }
    for _ in 0..6 {
        assert_eq!(clock.tick(), 0);
    }
    assert_eq!(clock.tick(), 160);
    clock.credit(48_000, 960);
    clock.credit(8_000, 961);
    assert_eq!(clock.tick(), 160);
    clock.credit(8_000, 160);
    clock.set_rate(8_000);
    assert_eq!(clock.tick(), 0);
    clock.credit(8_000, 160);
    clock.set_rate(48_000);
    assert_eq!(clock.tick(), 960);
}

#[test]
fn idle_setup_failures_balance_timer_graph_and_module_ownership() {
    use timing_support::Failure;
    for failure in [
        Failure::Open,
        Failure::Fd,
        Failure::Rate,
        Failure::Register,
        Failure::Attach,
    ] {
        let _fixture = HostFixture::new();
        reset();
        let mut channel = FakeChannel::eligible("IAX2/setup-failure", 8_000);
        timing_support::fail(failure);
        let host = fake_host();
        assert_eq!(
            attach(&host, &mut channel, "alpha"),
            Err(LinkHostError::Asterisk)
        );
        // SAFETY: the same retained peer also exercises explicit binding's failure unwind.
        let bound = unsafe { host.bind_channel(channel.raw(), "alpha") };
        assert_eq!(bound, Err(LinkHostError::Asterisk));
        assert!(channel.datastore.is_null());
        assert!(with_state(|state| state.graphs.is_empty()));
        let (opened, closed, _) = timing_support::counts();
        assert_eq!(opened, closed);
        assert_eq!(with_native_state(|state| state.module_references), 0);
    }
}

#[test]
fn datastore_teardown_during_timer_setup_prevents_late_publication() {
    unsafe extern "C" fn teardown_during_open() -> *mut ffi::ast_timer {
        // SAFETY: the fixture's ordinary timer provider owns its returned allocation.
        let timer = unsafe { ffi::ast_timer_open() };
        TEARDOWN_CHANNEL.with(|owner| {
            // SAFETY: this serialized fixture retains the supplied channel through timer setup.
            let channel = unsafe { &mut *owner.get() };
            let datastore = channel.datastore;
            channel.datastore = ptr::null_mut();
            // SAFETY: simulate external destruction after audiohook attachment, before clock publication.
            unsafe { fake_datastore_free(datastore) };
        });
        timer
    }
    let _fixture = HostFixture::new();
    reset();
    let mut host = fake_host();
    host.asterisk.timer_open = teardown_during_open;
    let mut channel = FakeChannel::eligible("IAX2/setup-teardown", 8_000);
    TEARDOWN_CHANNEL.with(|owner| owner.set(ptr::from_mut(channel.as_mut())));
    assert_eq!(
        attach(&host, &mut channel, "alpha"),
        Err(LinkHostError::Asterisk)
    );
    TEARDOWN_CHANNEL.with(|owner| owner.set(ptr::null_mut()));
    assert!(channel.datastore.is_null());
    assert!(with_state(|state| state.graphs.is_empty()));
    assert_eq!(timing_support::counts(), (1, 1, 0));
    assert_eq!(with_native_state(|state| state.module_references), 0);
}

#[test]
fn idle_guards_reject_unregistered_superseded_or_detaching_callbacks() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut source = FakeChannel::eligible("IAX2/guard-source", 8_000);
    let mut recipient = FakeChannel::eligible("IAX2/guard-recipient", 8_000);
    assert_eq!(attach(&host, &mut source, "alpha"), Ok(true));
    assert_eq!(attach(&host, &mut recipient, "alpha"), Ok(true));
    let source_ref = retain(&host, &mut source).unwrap();
    let recipient_ref = retain(&host, &mut recipient).unwrap();
    // SAFETY: these independent retained references keep both hook allocations live.
    let (source_hook, recipient_hook) =
        unsafe { (source_ref.raw.as_ref(), recipient_ref.raw.as_ref()) };
    source_hook
        .attachment
        .store(LINK_DETACHED, Ordering::Release);
    let mut samples = [0; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(
        &mut source,
        &raw mut frame,
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    assert!(with_state(|state| state.process_calls.is_empty()));
    source_hook
        .attachment
        .store(LINK_ATTACHED, Ordering::Release);
    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    let mut reload = LinkReload::default();
    stage(&host, &mut source, Some("alpha"), &mut reload).unwrap();
    reload.finish(true);
    timer_tick(&mut source);
    assert!(with_state(|state| state.process_calls.is_empty()));
    let source_lease = source_hook.idle.load(Ordering::Acquire);
    let recipient_lease = recipient_hook.idle.load(Ordering::Acquire);
    let old = source.raw();
    let new = recipient.raw();
    let datastore = source.datastore;
    timing_support::fail(timing_support::Failure::Register);
    timing_support::masquerade(old, new, || notify_datastore_move(datastore, old, new));
    let acknowledgements = timing_support::counts().2;
    // SAFETY: emulate provider callbacks while retained leases are pending asynchronous teardown.
    unsafe {
        ffi::ast_channel_fdno_set(old, 0);
        idle_event(
            old,
            ptr::null_mut(),
            ffi::AST_FRAMEHOOK_EVENT_READ,
            source_lease.cast(),
        );
        ffi::ast_channel_fdno_set(new, -1);
        idle_event(
            new,
            ptr::null_mut(),
            ffi::AST_FRAMEHOOK_EVENT_READ,
            recipient_lease.cast(),
        );
        (*source_lease).detach(new);
        ffi::ast_channel_fdno_set(new, 0);
        idle_event(
            new,
            ptr::null_mut(),
            ffi::AST_FRAMEHOOK_EVENT_READ,
            source_lease.cast(),
        );
    }
    assert_eq!(timing_support::counts().2, acknowledgements);
    timing_support::drain(new);
    // SAFETY: empty/clockless datastores remain valid no-op fixup inputs after lease destruction.
    unsafe {
        link_datastore_fixup(ptr::null_mut(), old, new);
        link_datastore_fixup((*datastore).data, old, new);
    }
    assert_eq!(with_native_state(|state| state.module_references), 0);
}

#[test]
fn asynchronous_idle_detach_retains_graph_and_module_until_destroy() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/asynchronous", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    assert_eq!(with_native_state(|state| state.module_references), 1);
    // SAFETY: the fixture retains the channel through explicit asynchronous teardown.
    unsafe { host.detach_channel(channel.raw()) };
    assert_eq!(with_state(|state| state.graphs.len()), 1);
    assert_eq!(with_native_state(|state| state.module_references), 1);
    timing_support::drain(channel.raw());
    assert!(with_state(|state| state.graphs.is_empty()));
    assert_eq!(with_native_state(|state| state.module_references), 0);
    assert_eq!(timing_support::counts(), (1, 1, 0));
}

#[test]
fn timer_events_preserve_controls_and_fail_without_replay_or_output() {
    let _fixture = HostFixture::new();
    reset();
    let mut channel = FakeChannel::eligible("IAX2/events", 8_000);
    assert_eq!(attach(&fake_host(), &mut channel, "alpha"), Ok(true));
    let mut data = [123_i16; 160];
    let mut frame = voice_frame(&mut data);
    frame.frametype = ffi::AST_FRAME_CONTROL;
    frame.subclass.integer = ffi::AST_CONTROL_RADIO_UNKEY as c_int;
    timing_support::event(
        channel.raw(),
        &raw mut frame,
        ffi::AST_FRAMEHOOK_EVENT_READ,
        false,
    );
    timing_support::event(
        channel.raw(),
        &raw mut frame,
        ffi::AST_FRAMEHOOK_EVENT_WRITE,
        true,
    );
    assert!(with_state(|state| state.process_calls.is_empty()));
    assert_eq!(data, [123; 160]);
    assert_eq!(frame.frametype, ffi::AST_FRAME_CONTROL);
    assert_eq!(
        frame.subclass.integer,
        ffi::AST_CONTROL_RADIO_UNKEY as c_int
    );
    timing_support::continuous(true);
    timer_tick(&mut channel);
    assert_eq!(timing_support::counts().2, 0);
    timing_support::continuous(false);
    timing_support::fail(timing_support::Failure::Ack);
    timer_tick(&mut channel);
    assert!(with_state(|state| state.process_calls.is_empty()));
    assert_eq!(timing_support::rates(), [0]);
    timing_support::drain(channel.raw());
    assert_eq!(timing_support::counts(), (1, 1, 1));
}

#[test]
fn idle_callback_allocates_nothing_and_discards_failure_output() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/no-allocation", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let retained = retain(&host, &mut channel).unwrap();
    // SAFETY: the retained hook and framehook own this lease until fixture teardown.
    let lease = unsafe { retained.raw.as_ref() }
        .idle
        .load(Ordering::Acquire);
    with_state(|state| {
        state.events.reserve(1_000);
        state.process_calls.reserve(300);
        state.silence_inputs.reserve(300);
        state.process_result = -9;
    });
    AUDIO_ALLOCS.with(|count| count.set(0));
    AUDIO_ACTIVE.with(|active| active.set(true));
    for _ in 0..250 {
        timing_support::select_timer(channel.raw());
        // SAFETY: the fixture selects the retained lease's timer and serializes this callback.
        unsafe {
            idle_event(
                channel.raw(),
                ptr::null_mut(),
                ffi::AST_FRAMEHOOK_EVENT_READ,
                lease.cast(),
            )
        };
    }
    AUDIO_ACTIVE.with(|active| active.set(false));
    AUDIO_ALLOCS.with(|count| assert_eq!(count.get(), 0));
    assert_eq!(with_state(|state| state.process_calls.len()), 250);
    assert!(with_state(|state| state
        .silence_inputs
        .iter()
        .all(|zero| *zero)));
    assert!(
        // SAFETY: this control-plane check holds the retained lease after callbacks have stopped.
        unsafe { &(*lease).scratch }
            .iter()
            .all(|sample| *sample == 0)
    );
}

#[test]
fn idle_reload_preserves_credit_or_adopts_new_fractional_clock() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/rate-reload", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let mut samples = [0; 160];
    let mut frame = voice_frame(&mut samples);
    with_state(|state| {
        state.process_result = -7;
        state.reload_unchanged = true;
    });
    invoke(
        &mut channel,
        &raw mut frame,
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(true);
    timer_tick(&mut channel);
    assert_eq!(with_state(|state| state.process_calls.len()), 1);
    with_state(|state| {
        state.process_result = 0;
        state.reload_unchanged = false;
    });
    channel.sample_rate_hz = 96_001;
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(true);
    timer_tick(&mut channel);
    assert_eq!(timing_support::rates(), [101]);
    assert_eq!(
        with_state(|state| state.process_calls.last().copied()),
        Some((2, 3, 96_001, 950))
    );
    channel.sample_rate_hz = 192_000;
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(false);
    timer_tick(&mut channel);
    assert_eq!(timing_support::rates(), [101]);
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(true);
    let before = with_state(|state| state.process_calls.len());
    timing_support::fail(timing_support::Failure::Rate);
    timer_tick(&mut channel);
    assert_eq!(with_state(|state| state.process_calls.len()), before);
    assert_eq!(timing_support::rates(), [0]);
    timing_support::drain(channel.raw());
    assert_eq!(with_native_state(|state| state.module_references), 0);
}

#[test]
fn masquerade_rebinds_recipient_timer_and_retains_both_leases() {
    for registration_fails in [false, true] {
        let _fixture = HostFixture::new();
        reset();
        let host = fake_host();
        let mut source = FakeChannel::eligible("IAX2/source", 8_000);
        let mut recipient = FakeChannel::eligible("IAX2/recipient", 8_000);
        assert_eq!(attach(&host, &mut source, "alpha"), Ok(true));
        assert_eq!(attach(&host, &mut recipient, "alpha"), Ok(true));
        if registration_fails {
            timing_support::fail(timing_support::Failure::Register);
        }
        let old = source.raw();
        let new = recipient.raw();
        let datastore = source.datastore;
        timing_support::masquerade(old, new, || notify_datastore_move(datastore, old, new));
        assert_eq!(timing_support::counts().1, 0);
        assert_eq!(with_native_state(|state| state.module_references), 2);
        timing_support::drain(recipient.raw());
        assert_eq!(timing_support::counts().1, usize::from(registration_fails));
        timer_tick(&mut recipient);
        assert_eq!(
            with_state(|state| state.process_calls.last().copied()),
            Some((1, 3, 8_000, 160))
        );
        timing_support::destroy(recipient.raw());
        assert_eq!(timing_support::counts().1, 2);
        assert_eq!(with_native_state(|state| state.module_references), 0);
    }
}

#[test]
fn recipient_pending_detach_preserves_incoming_timer_registration() {
    let _fixture = HostFixture::new();
    reset();
    let host = fake_host();
    let mut source = FakeChannel::eligible("IAX2/source-pending-recipient", 8_000);
    let mut recipient = FakeChannel::eligible("IAX2/pending-recipient", 8_000);
    assert_eq!(attach(&host, &mut source, "alpha"), Ok(true));
    assert_eq!(attach(&host, &mut recipient, "alpha"), Ok(true));
    timing_support::fail(timing_support::Failure::Ack);
    timer_tick(&mut recipient);
    timing_support::fail(timing_support::Failure::None);
    let old = source.raw();
    let new = recipient.raw();
    let datastore = source.datastore;
    timing_support::masquerade(old, new, || notify_datastore_move(datastore, old, new));
    timing_support::drain(new);
    assert_eq!(timing_support::counts().1, 1);
    timer_tick(&mut recipient);
    assert_eq!(
        with_state(|state| state.process_calls.last().copied()),
        Some((1, 3, 8_000, 160))
    );
    timing_support::destroy(new);
    assert_eq!(timing_support::counts().1, 2);
    assert_eq!(with_native_state(|state| state.module_references), 0);
}

#[test]
fn retained_hook_survives_datastore_destruction_until_callback_is_quiescent() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-3", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let retained = retain(&host, &mut channel).unwrap();
    let datastore = channel.datastore;
    channel.datastore = ptr::null_mut();
    // SAFETY: the fixture datastore is removed from its channel and consumed once.
    unsafe { fake_datastore_free(datastore) };

    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert_eq!(with_state(|state| state.destroy_hook_calls), 0);
    assert_eq!(with_state(|state| state.graphs.len()), 1);
    timer_tick(&mut channel);
    timer_tick(&mut channel);
    drop(retained);
    assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn explicit_detach_removes_and_frees_the_datastore() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-detach", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    detach(&host, &mut channel);
    assert!(channel.datastore.is_null());
    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn staged_reload_survives_channel_masquerade_and_swaps_in_place() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut original = FakeChannel::eligible("IAX2/506316-before", 8_000);
    assert_eq!(attach(&host, &mut original, "alpha"), Ok(true));

    let mut reload = LinkReload::default();
    stage(&host, &mut original, Some("alpha"), &mut reload).unwrap();
    assert_eq!(with_state(|state| state.graphs.len()), 2);

    // Model Asterisk moving the datastore/audiohook to a replacement channel.
    let mut replacement = FakeChannel::eligible("IAX2/506316-after", 8_000);
    replacement.datastore = original.datastore;
    replacement.audiohook = original.audiohook;
    let old = original.raw();
    let new = replacement.raw();
    let datastore = replacement.datastore;
    timing_support::masquerade(old, new, || notify_datastore_move(datastore, old, new));
    original.datastore = ptr::null_mut();
    original.audiohook = ptr::null_mut();

    reload.finish(true);
    assert_eq!(with_state(|state| state.detach_calls), 0);
    assert_eq!(with_state(|state| state.graphs.len()), 1);
    let mut samples = [0_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(
        &mut replacement,
        ptr::from_mut(&mut frame),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    assert_eq!(samples[0], 2);
}

#[test]
fn rollback_discards_candidate_and_keeps_active_graph() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-4", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let mut reload = LinkReload::default();
    stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
    reload.finish(false);

    let mut samples = [0_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(
        &mut channel,
        ptr::from_mut(&mut frame),
        ffi::AST_AUDIOHOOK_DIRECTION_READ,
    );
    assert_eq!(samples[0], 1);
    assert_eq!(with_state(|state| state.graphs.len()), 1);
}

#[test]
fn observation_reads_the_active_graph_while_quiesced() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-5", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    let statistics = observe(&host, &mut channel).unwrap().unwrap();
    assert_eq!(&*statistics.asterisk_channel, "IAX2/506316-5");
    assert_eq!(statistics.observation.processed_blocks, 7);
    assert_eq!(statistics.observation.bypassed_blocks, 2);
    assert_eq!(statistics.observation.failed_blocks, 1);
    assert!(with_state(|state| {
        state.events.windows(3).any(|events| {
            events
                == [
                    "audiohook_lock",
                    "observe_outside_link_control",
                    "audiohook_unlock",
                ]
        })
    }));
}

#[test]
fn invalid_link_observation_records_are_rejected_before_meter_tail() {
    for (size, abi) in [(32, 1), (72, 2), (80, 1)] {
        let _fixture = HostFixture::new();
        reset();
        let host = fake_host();
        let mut channel = FakeChannel::eligible("IAX2/observation-contract", 8_000);
        assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
        with_state(|state| {
            let observation = &mut state.graphs.get_mut(&1).unwrap().observation;
            observation.struct_size = size;
            observation.abi_version = abi;
        });
        // SAFETY: the fixture retains this channel and its complete observer output storage.
        let result = unsafe { host.observe_channel(channel.raw()) };
        assert!(
            matches!(result, Err(LinkHostError::Product(status)) if status == crate::URP_AST_INCOMPATIBLE_ABI)
        );
    }
}

#[test]
fn failed_attachment_destroys_unpublished_graph() {
    let _guard = HostFixture::new();
    reset();
    with_state(|state| state.attach_result = -1);
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-6", 8_000);
    assert_eq!(
        attach(&host, &mut channel, "alpha"),
        Err(LinkHostError::Asterisk)
    );
    assert!(channel.datastore.is_null());
    assert!(with_state(|state| state.graphs.is_empty()));
    assert_eq!(with_state(|state| state.destroy_hook_calls), 1);
}

#[test]
fn scanner_logs_failed_attachment_channel_and_product_status() {
    let _guard = HostFixture::new();
    reset();
    with_state(|state| state.prepare_result = URP_AST_ASTERISK_FAILURE);
    let mut channel = FakeChannel::eligible("IAX2/506316-log", 8_000);
    register(&mut channel);
    assert_eq!(start_with(fake_host(), fake_profile), URP_AST_OK);
    wait_until(|| !with_state(|state| state.diagnostics.is_empty()));

    assert_eq!(
        with_state(|state| state.diagnostics[0].clone()),
        "Unable to attach USBRadioPlus link processing to IAX2/506316-log (-8)"
    );
    stop();
}

#[test]
fn staged_reload_logs_failed_link_channel_and_product_status() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/506316-reload-log", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    with_state(|state| {
        state.prepare_result = -8;
        state.diagnostics.clear();
    });
    let mut reload = LinkReload::default();

    assert_eq!(
        stage(&host, &mut channel, Some("alpha"), &mut reload),
        Err(LinkHostError::Product(-8))
    );
    assert_eq!(
        with_state(|state| state.diagnostics[0].clone()),
        "Unable to prepare replacement USBRadioPlus link processing for IAX2/506316-reload-log (-8)"
    );
}

#[test]
fn explicit_peer_binding_retains_its_radio_profile_even_when_disabled() {
    let _guard = HostFixture::new();
    reset();
    let mut peer = FakeChannel::eligible("IAX2/advanced", 8_000);
    peer.application = nul("RptAdvanced");
    peer.data = nul("different-node-name");
    register(&mut peer);
    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    assert_eq!(start_with(fake_host(), missing_profile), URP_AST_OK);
    // SAFETY: the fixture owns the peer until host stop detaches its hook.
    assert_eq!(unsafe { bind_peer(peer.raw(), "radio-two") }, Ok(()));
    assert!(!peer.audiohook.is_null());
    // An existing datastore cannot be rebound while its hook is detached.
    // SAFETY: this fixture exclusively owns the pinned hook and restores its live state.
    unsafe {
        let hook = (*peer.datastore).data.cast::<LinkHook>();
        (*hook).attachment.store(LINK_DETACHED, Ordering::Release);
        assert_eq!(
            bind_peer(peer.raw(), "radio-two"),
            Err(LinkHostError::Asterisk)
        );
        (*hook).attachment.store(LINK_ATTACHED, Ordering::Release);
    }
    let mut samples = [12_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(&mut peer, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert_eq!(samples, [12; 160]);
    // A duplicate binding cannot redirect processing to another node's settings.
    // SAFETY: the fixture still retains the same peer channel.
    unsafe {
        assert_eq!(bind_peer(peer.raw(), "radio-two"), Ok(()));
        assert_eq!(
            bind_peer(peer.raw(), "wrong-radio"),
            Err(LinkHostError::Asterisk)
        );
    }
    with_state(|state| state.prepare_result = URP_AST_OK);
    reload_prepare(Some("wrong-default")).unwrap().finish(true);
    invoke(&mut peer, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert_eq!(samples[0], 1);
    assert_eq!(
        with_state(|state| state.process_calls.clone()),
        [(1, URP_AST_LINK_DIRECTION_READ, 8_000, 160)]
    );
    assert!(with_state(|state| state
        .prepare_profiles
        .iter()
        .all(|(_, profile)| profile == "radio-two")));
    assert_eq!(attachment_count(), 1);
    // A disabled bound peer may also satisfy the legacy scanner's heuristic.
    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    reload_prepare(None).unwrap().finish(true);
    peer.application = nul("Rpt");
    peer.data = nul("Remote Rx");
    with_state(|state| state.prepare_result = URP_AST_OK);
    assert_eq!(attach(&fake_host(), &mut peer, "wrong-default"), Ok(false));
    assert_eq!(
        with_state(|state| state.prepare_profiles.last().cloned()),
        Some(("prepare", "radio-two".into()))
    );
    stop();
    assert!(peer.datastore.is_null());
    timing_support::drain(peer.raw());
    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert!(with_state(|state| state.graphs.is_empty()));
    assert_eq!(
        // SAFETY: the channel remains live, but the process host has stopped.
        unsafe { bind_peer(peer.raw(), "radio-two") },
        Err(LinkHostError::Asterisk)
    );
}

#[test]
fn ineligible_and_disabled_links_are_clean_no_ops() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("Local/console", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    with_state(|state| state.prepare_result = URP_AST_NOT_READY);
    channel.name = nul("IAX2/506316-7");
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(false));
    assert!(channel.datastore.is_null());
}

#[test]
fn process_host_scans_periodically_and_stop_detaches_every_hook() {
    let _guard = HostFixture::new();
    reset();
    let mut eligible = FakeChannel::eligible("IAX2/506316-global", 8_000);
    let mut ineligible = FakeChannel::eligible("Local/console", 8_000);

    assert_eq!(start_with(fake_host(), ordered_fake_profile), URP_AST_OK);
    wait_until(|| iterator_count() == 1);
    register(&mut eligible);
    register(&mut ineligible);
    wait_until(|| attachment_count() == 1);
    stop();

    timing_support::drain(eligible.raw());
    timing_support::drain(ineligible.raw());
    assert!(eligible.datastore.is_null());
    assert!(ineligible.datastore.is_null());
    assert_eq!(with_state(|state| state.detach_calls), 1);
    assert!(with_state(|state| state.graphs.is_empty()));
    assert!(with_state(|state| {
        state.events.contains(&"profile_before_link_control")
            && !state.events.contains(&"profile_under_link_control")
    }));
}

#[test]
fn unchanged_reload_keeps_the_live_graph_history_and_statistics() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/unchanged", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    with_state(|state| state.reload_unchanged = true);
    let before = observe(&host, &mut channel).unwrap().unwrap().observation;
    let mut samples = [9_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert_eq!(samples[0], 1);
    for commit in [true, false] {
        let mut reload = LinkReload::default();
        stage(&host, &mut channel, Some("alpha"), &mut reload).unwrap();
        assert_eq!(with_state(|state| state.graphs.len()), 1);
        reload.finish(commit);
        let after = observe(&host, &mut channel).unwrap().unwrap().observation;
        assert_eq!(after.processed_blocks, before.processed_blocks);
        invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
        assert_eq!(samples[0], 1);
    }
    assert_eq!(
        with_state(|state| state.reload_comparisons.clone()),
        [("alpha".into(), 8_000, 960), ("alpha".into(), 8_000, 960)]
    );
}

#[test]
fn descriptor_comparison_failure_retains_graph_and_unlocks_before_returning() {
    let _guard = HostFixture::new();
    reset();
    let host = fake_host();
    let mut channel = FakeChannel::eligible("IAX2/compare-failure", 8_000);
    assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
    with_state(|state| state.reload_compare_result = -17);
    let hook = retain(&host, &mut channel).unwrap();
    assert_eq!(
        hook.prepare_reload(host.driver, 8_000),
        Err(LinkHostError::Product(-17))
    );
    with_state(|state| {
        assert_eq!(state.events.last(), Some(&"audiohook_unlock"));
        assert_eq!(state.graphs.len(), 1);
    });
    let mut samples = [9_i16; 160];
    let mut frame = voice_frame(&mut samples);
    invoke(&mut channel, &mut frame, ffi::AST_AUDIOHOOK_DIRECTION_READ);
    assert_eq!(samples[0], 1);
    detach(&host, &mut channel);
    drop(hook);
    assert!(with_state(|state| state.graphs.is_empty()));
}

#[test]
fn late_pending_reload_destroys_only_prepared_candidate() {
    for (result, candidate_created) in [(URP_AST_OK, true), (URP_AST_NOT_READY, false)] {
        let _guard = HostFixture::new();
        reset();
        let host = fake_host();
        let mut channel = FakeChannel::eligible("IAX2/late-pending", 8_000);
        assert_eq!(attach(&host, &mut channel, "alpha"), Ok(true));
        let hook = retain(&host, &mut channel).unwrap();
        PENDING_HOOK.with(|pending| pending.set(hook.raw.as_ptr()));
        with_state(|state| {
            state.prepare_result = result;
            state.preparation_observer = Some(mark_reload_pending_during_prepare);
        });
        assert_eq!(
            hook.prepare_reload(host.driver, 8_000),
            Err(LinkHostError::Asterisk)
        );
        with_state(|state| {
            assert_eq!(state.graphs.len(), 1);
            assert_eq!(
                state.events.last(),
                Some(&if candidate_created {
                    "graph_destroy"
                } else {
                    "audiohook_unlock"
                })
            );
            state.preparation_observer = None;
        });
        // SAFETY: the retained hook remains live; restore the simulated flag before teardown.
        unsafe {
            hook.raw
                .as_ref()
                .reload_pending
                .store(false, Ordering::Release)
        };
        PENDING_HOOK.with(|pending| pending.set(ptr::null_mut()));
        detach(&host, &mut channel);
        drop(hook);
        assert!(with_state(|state| state.graphs.is_empty()));
    }
}

#[test]
fn process_reload_retains_hooks_and_statistics_until_finish() {
    let _guard = HostFixture::new();
    reset();
    let mut channel = FakeChannel::eligible("IAX2/506316-reload", 8_000);
    register(&mut channel);
    assert_eq!(start_with(fake_host(), fake_profile), URP_AST_OK);
    wait_until(|| attachment_count() == 1);

    let before = statistics();
    assert_eq!(before.len(), 1);
    assert_eq!(&*before[0].asterisk_channel, "IAX2/506316-reload");
    assert_eq!(before[0].observation.processed_blocks, 7);
    crate::host::cli::tests::assert_link_snapshot("IAX2/506316-reload", 7, 2, 1);

    // The reload owns LINK_CONTROL until finish; public statistics also takes it.
    let reload = reload_prepare(Some("alpha")).unwrap();
    assert_eq!(with_state(|state| state.graphs.len()), 2);
    reload.finish(true);

    let after = statistics();
    assert_eq!(after[0].observation.processed_blocks, 14);
    stop();
}

#[test]
fn reload_uses_frozen_profile_before_scanner_observes_it() {
    let _guard = HostFixture::new();
    reset();
    let mut channel = FakeChannel::eligible("IAX2/506316-between-scans", 8_000);
    register(&mut channel);
    assert_eq!(start_with(fake_host(), missing_profile), URP_AST_OK);
    wait_until(|| with_state(|state| state.events.contains(&"missing_profile_resolved")));
    assert_eq!(attachment_count(), 0);

    let reload = reload_prepare(Some("alpha")).unwrap();
    assert_eq!(attachment_count(), 1);
    reload.finish(true);

    assert_eq!(statistics().len(), 1);
    stop();
}

#[test]
fn process_statistics_holds_link_control_through_graph_observation() {
    let _guard = HostFixture::new();
    reset();
    let mut channel = FakeChannel::eligible("IAX2/506316-statistics", 8_000);
    register(&mut channel);
    assert_eq!(start_with(fake_host(), fake_profile), URP_AST_OK);
    wait_until(|| attachment_count() == 1);
    with_state(|state| state.events.clear());

    assert_eq!(statistics().len(), 1);
    assert!(with_state(|state| {
        state.events.contains(&"observe_under_link_control")
            && !state.events.contains(&"observe_outside_link_control")
    }));

    stop();
}
