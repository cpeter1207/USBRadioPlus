use super::*;

use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::ptr;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::host::support::Fixture as HostFixture;

#[derive(Default)]
struct FakeState {
    events: Vec<&'static str>,
    notices: Vec<String>,
    diagnostics: Vec<String>,
    prepare_profiles: Vec<(&'static str, String)>,
    graphs: HashMap<usize, FakeGraph>,
    next_graph: usize,
    prepare_result: i32,
    attach_result: i32,
    process_calls: Vec<(usize, u32, u32, u32)>,
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
    LinkHost::with_operations(
        ptr::dangling_mut::<c_void>(),
        ptr::null_mut(),
        fake_product_operations(),
        fake_asterisk_operations(),
    )
}

fn fake_product_operations() -> LinkProductOperations {
    LinkProductOperations {
        prepare: fake_prepare,
        prepare_reload: fake_prepare_reload,
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
    // SAFETY: the host supplies a readable byte-counted profile.
    let profile = unsafe { std::slice::from_raw_parts(profile, profile_length as usize) };
    let profile = String::from_utf8(profile.to_vec()).expect("profile must be UTF-8");
    with_state(|state| {
        state.events.push(event);
        state.prepare_profiles.push((event, profile));
        if state.prepare_result != URP_AST_OK {
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
    URP_AST_OK
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
    URP_AST_OK
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
