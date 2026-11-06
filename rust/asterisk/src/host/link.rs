//! Asterisk incoming-link audiohook ownership.
//!
//! Graph construction and replacement happen on the control plane. The
//! manipulate callback only inspects one frame, loads the already prepared
//! graph, and calls the existing bounded Rust link processor.

use super::super::{
    ABI_VERSION, URP_AST_ASTERISK_FAILURE, URP_AST_LINK_DIRECTION_READ,
    URP_AST_LINK_DIRECTION_SILENCE, URP_AST_LINK_DIRECTION_WRITE, URP_AST_NOT_READY, URP_AST_OK,
    UrpAstDescriptor, UrpAstLinkObservation, ffi,
};
use super::channel;

use std::cell::UnsafeCell;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::mem::ManuallyDrop;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU8, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::Duration;

const FALLBACK_LINK_RATE_HZ: u32 = 8_000;
const MAXIMUM_LINK_FRAME_COUNT: u32 = 960;
const LINK_BUILDING: u8 = 0;
const LINK_ATTACHED: u8 = 1;
const LINK_DETACHED: u8 = 2;
const LINK_SCAN_INTERVAL: Duration = Duration::from_millis(250);

type ProfileResolver = fn() -> Option<Box<str>>;

static LINK_CONTROL: Mutex<()> = Mutex::new(());

/// Failure reported while attaching or replacing an incoming-link hook.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum LinkHostError {
    /// An Asterisk host operation failed.
    Asterisk,
    /// The existing Rust link-processing API rejected preparation or observation.
    Product(c_int),
}

/// Required C graph operations copied from the validated product descriptor.
#[derive(Clone, Copy)]
pub(super) struct LinkProductOperations {
    pub(super) prepare:
        unsafe extern "C" fn(*mut c_void, *const u8, u32, u32, u32, *mut *mut c_void) -> c_int,
    pub(super) prepare_reload:
        unsafe extern "C" fn(*mut c_void, *const u8, u32, u32, u32, *mut *mut c_void) -> c_int,
    pub(super) reload_unchanged:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *const u8, u32, u32, u32, *mut u32) -> c_int,
    pub(super) process: unsafe extern "C" fn(*mut c_void, u32, u32, *mut i16, u32) -> c_int,
    pub(super) observe: unsafe extern "C" fn(*mut c_void, *mut UrpAstLinkObservation) -> c_int,
    pub(super) destroy: unsafe extern "C" fn(*mut c_void),
}

/// Narrow Asterisk operations used outside the real-time graph processor.
#[derive(Clone, Copy)]
pub(super) struct AsteriskOperations {
    pub(super) audiohook_init: unsafe fn(*mut ffi::ast_audiohook) -> c_int,
    pub(super) audiohook_attach: unsafe fn(*mut ffi::ast_channel, *mut ffi::ast_audiohook) -> c_int,
    pub(super) audiohook_detach: unsafe fn(*mut ffi::ast_audiohook),
    pub(super) audiohook_destroy: unsafe fn(*mut ffi::ast_audiohook),
    pub(super) audiohook_lock: unsafe fn(*mut ffi::ast_audiohook),
    pub(super) audiohook_unlock: unsafe fn(*mut ffi::ast_audiohook),
    pub(super) timer_open: unsafe extern "C" fn() -> *mut ffi::ast_timer,
    pub(super) timer_close: unsafe extern "C" fn(*mut ffi::ast_timer),
    pub(super) timer_fd: unsafe extern "C" fn(*const ffi::ast_timer) -> c_int,
    pub(super) timer_set_rate: unsafe extern "C" fn(*const ffi::ast_timer, u32) -> c_int,
    pub(super) timer_ack: unsafe extern "C" fn(*const ffi::ast_timer, u32) -> c_int,
    pub(super) timer_get_event: unsafe extern "C" fn(*const ffi::ast_timer) -> ffi::ast_timer_event,
    pub(super) framehook_attach:
        unsafe extern "C" fn(*mut ffi::ast_channel, *mut ffi::ast_framehook_interface) -> c_int,
    pub(super) framehook_detach: unsafe extern "C" fn(*mut ffi::ast_channel, c_int) -> c_int,
    pub(super) channel_fd_add: unsafe extern "C" fn(*mut ffi::ast_channel, c_int) -> c_int,
    pub(super) channel_fd: unsafe extern "C" fn(*const ffi::ast_channel, c_int) -> c_int,
    pub(super) channel_fdno: unsafe extern "C" fn(*const ffi::ast_channel) -> c_int,
    pub(super) channel_fdno_set: unsafe extern "C" fn(*mut ffi::ast_channel, c_int),
    pub(super) channel_set_fd: unsafe extern "C" fn(*mut ffi::ast_channel, c_int, c_int),
    pub(super) channel_wake: unsafe fn(*mut ffi::ast_channel),
    pub(super) module_ref: unsafe extern "C" fn(
        *mut ffi::ast_module,
        *const c_char,
        c_int,
        *const c_char,
    ) -> *mut ffi::ast_module,
    pub(super) module_unref:
        unsafe extern "C" fn(*mut ffi::ast_module, *const c_char, c_int, *const c_char),
    pub(super) datastore_alloc:
        unsafe fn(*const ffi::ast_datastore_info, *mut ffi::ast_module) -> *mut ffi::ast_datastore,
    pub(super) datastore_free: unsafe fn(*mut ffi::ast_datastore),
    pub(super) channel_lock: unsafe fn(*mut ffi::ast_channel),
    pub(super) channel_unlock: unsafe fn(*mut ffi::ast_channel),
    pub(super) datastore_find:
        unsafe fn(*mut ffi::ast_channel, *const ffi::ast_datastore_info) -> *mut ffi::ast_datastore,
    pub(super) datastore_add: unsafe fn(*mut ffi::ast_channel, *mut ffi::ast_datastore),
    pub(super) datastore_remove: unsafe fn(*mut ffi::ast_channel, *mut ffi::ast_datastore),
    pub(super) channel_name: unsafe fn(*mut ffi::ast_channel) -> *const c_char,
    pub(super) channel_application: unsafe fn(*mut ffi::ast_channel) -> *const c_char,
    pub(super) channel_data: unsafe fn(*mut ffi::ast_channel) -> *const c_char,
    pub(super) channel_sample_rate: unsafe fn(*mut ffi::ast_channel) -> u32,
    pub(super) format_sample_rate: unsafe fn(*mut ffi::ast_format) -> u32,
    pub(super) iterator_new: unsafe fn() -> *mut ffi::ast_channel_iterator,
    pub(super) iterator_next: unsafe fn(*mut ffi::ast_channel_iterator) -> *mut ffi::ast_channel,
    pub(super) iterator_destroy: unsafe fn(*mut ffi::ast_channel_iterator),
    pub(super) channel_unref: unsafe fn(*mut ffi::ast_channel),
    pub(super) log_notice: fn(&str),
    pub(super) log_error: fn(&str),
}

/// Rust owner for link attachment and staged graph replacement.
#[derive(Clone, Copy)]
pub(super) struct LinkHost {
    driver: *mut c_void,
    module_self: *mut ffi::ast_module,
    product: LinkProductOperations,
    asterisk: AsteriskOperations,
}

// SAFETY: the driver and module handles have process lifetime while this host
// runs, and every Asterisk operation is safe to call from the scanner thread.
unsafe impl Send for LinkHost {}
// SAFETY: LinkHost is immutable; callback-visible state is independently
// synchronized by Asterisk and the atomics in LinkHook.
unsafe impl Sync for LinkHost {}

struct RunningLinkHost {
    host: LinkHost,
    generation: Arc<()>,
    stop: Arc<(Mutex<bool>, Condvar)>,
    scanner: JoinHandle<()>,
}

fn running_host() -> &'static Mutex<Option<RunningLinkHost>> {
    static HOST: OnceLock<Mutex<Option<RunningLinkHost>>> = OnceLock::new();
    HOST.get_or_init(|| Mutex::new(None))
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Start periodic discovery of eligible incoming AllStarLink channels.
pub(super) fn start(
    descriptor: &UrpAstDescriptor,
    driver: *mut c_void,
    module_self: *mut ffi::ast_module,
) -> c_int {
    if driver.is_null() {
        return super::super::URP_AST_INVALID_ARGUMENT;
    }
    start_with(
        LinkHost::new(descriptor, driver, module_self),
        channel::first_live_profile,
    )
}

fn start_with(host: LinkHost, profile: ProfileResolver) -> c_int {
    let mut running = lock(running_host());
    if running.is_some() {
        return URP_AST_ASTERISK_FAILURE;
    }

    let generation = Arc::new(());
    let stop = Arc::new((Mutex::new(false), Condvar::new()));
    let scanner_stop = Arc::clone(&stop);
    let scanner = match thread::Builder::new()
        .name("usbradioplus-link-scanner".into())
        .spawn(move || scan_loop(host, profile, scanner_stop))
    {
        Ok(scanner) => scanner,
        Err(_) => return URP_AST_ASTERISK_FAILURE,
    };
    *running = Some(RunningLinkHost {
        host,
        generation,
        stop,
        scanner,
    });
    URP_AST_OK
}

fn scan_loop(host: LinkHost, resolve_profile: ProfileResolver, stop: Arc<(Mutex<bool>, Condvar)>) {
    loop {
        if *lock(&stop.0) {
            break;
        }
        let profile = resolve_profile();
        {
            let _control = lock(&LINK_CONTROL);
            if !*lock(&stop.0) {
                // SAFETY: the scanner holds the process control lock and the
                // running host keeps the driver/module generation alive.
                let _ = unsafe { host.scan_all(profile.as_deref()) };
            }
        }

        let stopped = lock(&stop.0);
        if *stopped {
            break;
        }
        let (stopped, _) = stop
            .1
            .wait_timeout(stopped, LINK_SCAN_INTERVAL)
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if *stopped {
            break;
        }
    }
}

/// Stop channel discovery and detach every installed incoming-link hook.
pub(super) fn stop() {
    let Some(running) = lock(running_host()).take() else {
        return;
    };
    *lock(&running.stop.0) = true;
    running.stop.1.notify_all();
    let _ = running.scanner.join();

    let _control = lock(&LINK_CONTROL);
    // SAFETY: the scanner has stopped, this operation is serialized against
    // reload/statistics, and the running generation remains live here.
    let _ = unsafe { running.host.detach_all() };
}

/// Prepare replacement graphs for every currently attached or eligible link.
pub(super) fn reload_prepare(profile: Option<&str>) -> Result<LinkReload, LinkHostError> {
    let generation = {
        let running = lock(running_host());
        let running = running.as_ref().ok_or(LinkHostError::Asterisk)?;
        Arc::clone(&running.generation)
    };
    let control = lock(&LINK_CONTROL);
    let host = {
        let running = lock(running_host());
        let running = running.as_ref().ok_or(LinkHostError::Asterisk)?;
        if !Arc::ptr_eq(&generation, &running.generation) {
            return Err(LinkHostError::Asterisk);
        }
        running.host
    };
    let mut reload = LinkReload {
        hooks: Vec::new(),
        finished: false,
        control: Some(control),
    };
    // SAFETY: the reload transaction owns LINK_CONTROL until finish/drop, and
    // the process host remains installed for the serialized operation.
    unsafe { host.stage_all(profile, &mut reload) }?;
    Ok(reload)
}

/// Bind a referenced peer to the reserved radio profile before its first read.
/// Graph setup is control-plane-only and serialized with reload and shutdown.
pub(super) unsafe fn bind_peer(
    channel: *mut ffi::ast_channel,
    profile: &str,
) -> Result<(), LinkHostError> {
    let _control = lock(&LINK_CONTROL);
    let host = lock(running_host())
        .as_ref()
        .map(|running| running.host)
        .ok_or(LinkHostError::Asterisk)?;
    // SAFETY: the caller retains the peer for this synchronous control operation.
    unsafe { host.bind_channel(channel, profile) }
}

/// Snapshot every active incoming-link graph for status presentation.
pub(super) fn statistics() -> Vec<LinkStatistics> {
    let _control = lock(&LINK_CONTROL);
    let host = lock(running_host()).as_ref().map(|running| running.host);
    let Some(host) = host else {
        return Vec::new();
    };
    // SAFETY: channel/datastore retention and each audiohook lock make
    // observation safe while scanner, reload, or stop operates concurrently.
    unsafe { host.statistics_all() }.unwrap_or_default()
}

/// One named active-link processing snapshot for CLI/status presentation.
pub(super) struct LinkStatistics {
    /// Asterisk channel name captured when the hook was installed.
    pub(super) asterisk_channel: Box<str>,
    /// Lock-free processing counters copied while the callback was quiesced.
    pub(super) observation: UrpAstLinkObservation,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum InstallMode {
    Active,
    Staged,
    // Explicit ownership survives a disabled graph, so reload can enable it.
    Bound,
}

impl LinkHost {
    unsafe fn bind_channel(
        &self,
        channel: *mut ffi::ast_channel,
        profile: &str,
    ) -> Result<(), LinkHostError> {
        // SAFETY: caller retains the peer throughout this serialized operation.
        let snapshot = unsafe { self.snapshot(channel) };
        if let Some(hook) = snapshot.hook {
            // SAFETY: this retained reference keeps the immutable profile alive.
            let hook = unsafe { hook.raw.as_ref() };
            return if &*hook.profile == profile
                && hook.attachment.load(Ordering::Acquire) == LINK_ATTACHED
            {
                hook.profile_bound.store(true, Ordering::Release);
                Ok(())
            } else {
                Err(LinkHostError::Asterisk)
            };
        }
        // SAFETY: the snapshot belongs to the same caller-retained peer.
        unsafe {
            self.install(
                channel,
                profile,
                &snapshot.name,
                snapshot.sample_rate_hz,
                InstallMode::Bound,
            )
        }?
        .ok_or(LinkHostError::Asterisk)
        .map(|_| ())
    }

    /// Construct a host around a live driver and its validated product descriptor.
    pub(super) fn new(
        descriptor: &UrpAstDescriptor,
        driver: *mut c_void,
        module_self: *mut ffi::ast_module,
    ) -> Self {
        Self::with_operations(
            driver,
            module_self,
            LinkProductOperations {
                prepare: descriptor.link_prepare.expect("validated link_prepare"),
                prepare_reload: descriptor
                    .link_prepare_reload
                    .expect("validated link_prepare_reload"),
                reload_unchanged: descriptor
                    .link_reload_unchanged
                    .expect("validated link_reload_unchanged"),
                process: descriptor.link_process.expect("validated link_process"),
                observe: descriptor.link_observe.expect("validated link_observe"),
                destroy: descriptor.link_destroy.expect("validated link_destroy"),
            },
            production_asterisk_operations(),
        )
    }

    fn with_operations(
        driver: *mut c_void,
        module_self: *mut ffi::ast_module,
        product: LinkProductOperations,
        asterisk: AsteriskOperations,
    ) -> Self {
        Self {
            driver,
            module_self,
            product,
            asterisk,
        }
    }

    /// Attach a prepared active graph to one eligible incoming-link channel.
    ///
    /// Returns `true` only when this call published a new hook. An ineligible,
    /// disabled, or already attached channel is a successful no-op.
    /// The caller retains the channel and serializes graph changes with
    /// `LINK_CONTROL`; Asterisk datastore teardown can still run concurrently.
    pub(super) unsafe fn attach_channel(
        &self,
        channel: *mut ffi::ast_channel,
        profile: &str,
    ) -> Result<bool, LinkHostError> {
        // SAFETY: the caller holds one live Asterisk channel reference.
        let snapshot = unsafe { self.snapshot(channel) };
        if !snapshot.eligible {
            return Ok(false);
        }
        let result = if let Some(hook) = snapshot.hook {
            hook.prepare_active_if_missing(self.driver, profile, snapshot.sample_rate_hz)
                .map(|()| false)
        } else {
            // SAFETY: the snapshot was captured from this same referenced channel.
            unsafe {
                self.install(
                    channel,
                    profile,
                    &snapshot.name,
                    snapshot.sample_rate_hz,
                    InstallMode::Active,
                )
            }
            .map(|hook| hook.is_some())
        };
        if let Err(error) = result {
            (self.asterisk.log_error)(&link_attachment_diagnostic(&snapshot.name, error));
        }
        result
    }

    /// Stage a replacement for an existing hook, or attach a new dormant hook.
    ///
    /// `profile` is needed only when the eligible channel has no existing
    /// datastore. Direct retained hook ownership makes the resulting
    /// transaction independent of later channel masquerades.
    pub(super) unsafe fn stage_channel(
        &self,
        channel: *mut ffi::ast_channel,
        profile: Option<&str>,
        reload: &mut LinkReload,
    ) -> Result<(), LinkHostError> {
        // SAFETY: the caller holds one live Asterisk channel reference.
        let snapshot = unsafe { self.snapshot(channel) };
        let result = if let Some(hook) = snapshot.hook {
            hook.prepare_reload(self.driver, snapshot.sample_rate_hz)
                .map(|changed| changed.then_some(hook))
        } else if let Some(profile) = profile.filter(|_| snapshot.eligible) {
            // SAFETY: the snapshot was captured from this same referenced channel.
            unsafe {
                self.install(
                    channel,
                    profile,
                    &snapshot.name,
                    snapshot.sample_rate_hz,
                    InstallMode::Staged,
                )
            }
        } else {
            Ok(None)
        };
        match result {
            Ok(Some(hook)) => reload.hooks.push(hook),
            Ok(None) => {}
            Err(error) => {
                (self.asterisk.log_error)(&link_reload_diagnostic(&snapshot.name, error));
                return Err(error);
            }
        }
        Ok(())
    }

    /// Retain the hook currently owned by a channel datastore.
    pub(super) unsafe fn retain_attached(
        &self,
        channel: *mut ffi::ast_channel,
    ) -> Option<LinkHookRef> {
        // SAFETY: the caller holds one live Asterisk channel reference.
        unsafe { self.snapshot(channel) }.hook
    }

    /// Remove and free one channel's link datastore after callback quiescence.
    pub(super) unsafe fn detach_channel(&self, channel: *mut ffi::ast_channel) {
        // SAFETY: the caller holds one live Asterisk channel reference.
        unsafe { (self.asterisk.channel_lock)(channel) };
        // SAFETY: channel is locked and the info pointer has process lifetime.
        let datastore = unsafe { (self.asterisk.datastore_find)(channel, datastore_info()) };
        if !datastore.is_null() {
            // SAFETY: the locked datastore retains its hook and current framehook lease.
            if let Some(hook) = unsafe { (*datastore).data.cast::<LinkHook>().as_ref() } {
                let idle = hook.idle.load(Ordering::Acquire);
                if !idle.is_null() {
                    // SAFETY: this channel lock prevents the current lease's destruction.
                    unsafe { (*idle).detach(channel) };
                }
            }
            // SAFETY: the located datastore belongs to this locked channel.
            unsafe { (self.asterisk.datastore_remove)(channel, datastore) };
        }
        // SAFETY: balances channel_lock above.
        unsafe { (self.asterisk.channel_unlock)(channel) };
        if !datastore.is_null() {
            // SAFETY: the datastore was removed and is consumed exactly once.
            unsafe { (self.asterisk.datastore_free)(datastore) };
        }
    }

    /// Read active graph statistics while the callback is quiesced.
    pub(super) unsafe fn observe_channel(
        &self,
        channel: *mut ffi::ast_channel,
    ) -> Result<Option<LinkStatistics>, LinkHostError> {
        // SAFETY: the caller holds one live Asterisk channel reference.
        let Some(hook) = (unsafe { self.retain_attached(channel) }) else {
            return Ok(None);
        };
        hook.observe()
    }

    unsafe fn scan_all(&self, profile: Option<&str>) -> Result<(), LinkHostError> {
        let Some(profile) = profile else {
            return Ok(());
        };
        // SAFETY: visit_channels retains each channel for the synchronous call.
        unsafe {
            self.visit_channels(|channel| {
                let _ = self.attach_channel(channel, profile);
                Ok(())
            })
        }
    }

    unsafe fn stage_all(
        &self,
        profile: Option<&str>,
        reload: &mut LinkReload,
    ) -> Result<(), LinkHostError> {
        // SAFETY: visit_channels retains each channel for the synchronous call.
        unsafe { self.visit_channels(|channel| self.stage_channel(channel, profile, reload)) }
    }

    unsafe fn detach_all(&self) -> Result<(), LinkHostError> {
        // SAFETY: visit_channels retains each channel for the synchronous call.
        unsafe {
            self.visit_channels(|channel| {
                self.detach_channel(channel);
                Ok(())
            })
        }
    }

    unsafe fn statistics_all(&self) -> Result<Vec<LinkStatistics>, LinkHostError> {
        let mut statistics = Vec::new();
        // SAFETY: visit_channels retains each channel for the synchronous call.
        unsafe {
            self.visit_channels(|channel| {
                if let Ok(Some(observation)) = self.observe_channel(channel) {
                    statistics.push(observation);
                }
                Ok(())
            })
        }?;
        Ok(statistics)
    }

    unsafe fn visit_channels(
        &self,
        mut operation: impl FnMut(*mut ffi::ast_channel) -> Result<(), LinkHostError>,
    ) -> Result<(), LinkHostError> {
        // SAFETY: the injected iterator factory follows Asterisk ownership rules.
        let iterator = unsafe { (self.asterisk.iterator_new)() };
        if iterator.is_null() {
            return Err(LinkHostError::Asterisk);
        }

        let mut result = Ok(());
        loop {
            // SAFETY: iterator remains live until iterator_destroy below.
            let channel = unsafe { (self.asterisk.iterator_next)(iterator) };
            if channel.is_null() {
                break;
            }
            let channel_result = operation(channel);
            // SAFETY: iterator_next transfers one channel reference.
            unsafe { (self.asterisk.channel_unref)(channel) };
            if channel_result.is_err() {
                result = channel_result;
                break;
            }
        }
        // SAFETY: consumes the iterator returned above exactly once.
        unsafe { (self.asterisk.iterator_destroy)(iterator) };
        result
    }

    unsafe fn snapshot(&self, channel: *mut ffi::ast_channel) -> ChannelSnapshot {
        // SAFETY: the caller holds one live Asterisk channel reference.
        unsafe { (self.asterisk.channel_lock)(channel) };
        // SAFETY: the channel is locked for every field/datastore read below.
        let datastore = unsafe { (self.asterisk.datastore_find)(channel, datastore_info()) };
        let hook = if datastore.is_null() {
            None
        } else {
            // SAFETY: the datastore remains attached while the channel lock is held.
            let raw = unsafe { (*datastore).data.cast::<LinkHook>() };
            if raw.is_null() {
                None
            } else {
                // SAFETY: the datastore owns one live hook reference.
                Some(unsafe { LinkHookRef::retain(raw) })
            }
        };
        // SAFETY: direct channel access remains protected by its lock.
        let name = unsafe { c_text((self.asterisk.channel_name)(channel)) };
        // SAFETY: direct channel access remains protected by its lock.
        let application = unsafe { c_bytes((self.asterisk.channel_application)(channel)) };
        // SAFETY: direct channel access remains protected by its lock.
        let data = unsafe { c_bytes((self.asterisk.channel_data)(channel)) };
        // SAFETY: direct channel access remains protected by its lock.
        let sample_rate_hz = unsafe { (self.asterisk.channel_sample_rate)(channel) };
        // SAFETY: balances channel_lock above.
        unsafe { (self.asterisk.channel_unlock)(channel) };
        ChannelSnapshot {
            hook,
            eligible: name.as_bytes().starts_with(b"IAX2/")
                && application == b"Rpt"
                && data == b"Remote Rx",
            name,
            sample_rate_hz: if sample_rate_hz == 0 {
                FALLBACK_LINK_RATE_HZ
            } else {
                sample_rate_hz
            },
        }
    }

    unsafe fn install(
        &self,
        channel: *mut ffi::ast_channel,
        profile: &str,
        channel_name: &str,
        sample_rate_hz: u32,
        mode: InstallMode,
    ) -> Result<Option<LinkHookRef>, LinkHostError> {
        let staged = mode == InstallMode::Staged;
        let graph = self.prepare(profile, sample_rate_hz, staged)?;
        if graph.is_none() && mode != InstallMode::Bound {
            return Ok(None);
        }
        let hook = Box::new(LinkHook::new(
            self.product,
            self.asterisk,
            profile,
            channel_name,
            sample_rate_hz,
            graph.unwrap_or(ptr::null_mut()),
            mode,
        ));
        let raw = Box::into_raw(hook);
        // SAFETY: raw points to a pinned heap allocation whose first member is audiohook.
        let audiohook = unsafe { ptr::addr_of_mut!((*raw).audiohook) };
        // SAFETY: audiohook remains pinned until its matching destroy operation.
        if unsafe { (self.asterisk.audiohook_init)(audiohook) } != 0 {
            // SAFETY: Asterisk did not initialize/publish the embedded hook.
            unsafe { destroy_uninitialized(raw, false) };
            return Err(LinkHostError::Asterisk);
        }
        // SAFETY: audiohook_init completed and the callback ABI matches Asterisk.
        unsafe { (*audiohook).manipulate_callback = Some(link_audiohook_callback) };
        // SAFETY: info/module pointers have the required process lifetime.
        let datastore =
            unsafe { (self.asterisk.datastore_alloc)(datastore_info(), self.module_self) };
        if datastore.is_null() {
            // SAFETY: initialized but unpublished hook is consumed exactly once.
            unsafe { destroy_uninitialized(raw, true) };
            return Err(LinkHostError::Asterisk);
        }
        // One reference belongs to the builder and one to the datastore.
        // SAFETY: raw is the live builder-owned hook allocation.
        unsafe { (*raw).references.store(2, Ordering::Release) };
        // SAFETY: this datastore is exclusively owned before publication.
        unsafe { (*datastore).data = raw.cast() };
        // SAFETY: caller owns a live channel reference.
        unsafe { (self.asterisk.channel_lock)(channel) };
        // SAFETY: the channel is locked for duplicate detection and publication.
        let duplicate =
            unsafe { !(self.asterisk.datastore_find)(channel, datastore_info()).is_null() };
        if !duplicate {
            // SAFETY: datastore is unpublished and channel is locked.
            unsafe { (self.asterisk.datastore_add)(channel, datastore) };
        }
        // SAFETY: balances channel_lock above.
        unsafe { (self.asterisk.channel_unlock)(channel) };
        if duplicate {
            // SAFETY: frees the unpublished datastore and releases its hook reference.
            unsafe { (self.asterisk.datastore_free)(datastore) };
            // SAFETY: releases the builder reference.
            unsafe { LinkHook::release(raw) };
            return Ok(None);
        }
        // SAFETY: channel owns the datastore and the embedded hook stays pinned.
        if unsafe { (self.asterisk.audiohook_attach)(channel, audiohook) } != 0 {
            // SAFETY: caller owns a live channel reference.
            unsafe { (self.asterisk.channel_lock)(channel) };
            // SAFETY: the channel is locked while checking/removing this datastore.
            let still_attached =
                unsafe { (self.asterisk.datastore_find)(channel, datastore_info()) == datastore };
            if still_attached {
                // SAFETY: this datastore belongs to the locked channel.
                unsafe { (self.asterisk.datastore_remove)(channel, datastore) };
            }
            // SAFETY: balances channel_lock above.
            unsafe { (self.asterisk.channel_unlock)(channel) };
            if still_attached {
                // SAFETY: removed datastore is consumed once and drops its reference.
                unsafe { (self.asterisk.datastore_free)(datastore) };
            }
            // SAFETY: releases the builder reference; moved datastore teardown owns the other.
            unsafe { LinkHook::release(raw) };
            return Err(LinkHostError::Asterisk);
        }
        // Publish callback eligibility only after graph preparation, datastore insertion,
        // and successful Asterisk attachment.
        // SAFETY: raw remains protected by builder and datastore references.
        if unsafe {
            (*raw).attachment.compare_exchange(
                LINK_BUILDING,
                LINK_ATTACHED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
        }
        .is_err()
        {
            // SAFETY: successful attach must be balanced after concurrent teardown won.
            unsafe { (self.asterisk.audiohook_detach)(audiohook) };
            // SAFETY: releases the builder reference.
            unsafe { LinkHook::release(raw) };
            return Err(LinkHostError::Asterisk);
        }
        if let Err(error) =
            // SAFETY: builder and datastore references retain the successfully attached hook.
            unsafe { IdleLease::install(raw, channel, self.module_self, sample_rate_hz) }
        {
            // SAFETY: the caller retains this channel while publication is unwound.
            unsafe { self.detach_channel(channel) };
            // SAFETY: consumes the remaining builder reference.
            unsafe { LinkHook::release(raw) };
            return Err(error);
        }
        (self.asterisk.log_notice)(&link_attached_diagnostic(channel_name, staged));
        // SAFETY: the builder reference transfers to this RAII owner. Active
        // callers drop it immediately; staged callers retain it through commit.
        Ok(Some(unsafe { LinkHookRef::from_owned(raw) }))
    }

    fn prepare(
        &self,
        profile: &str,
        sample_rate_hz: u32,
        staged: bool,
    ) -> Result<Option<*mut c_void>, LinkHostError> {
        let mut graph = ptr::null_mut();
        let prepare = if staged {
            self.product.prepare_reload
        } else {
            self.product.prepare
        };
        // SAFETY: driver/profile/output obey the existing synchronous product ABI.
        let result = unsafe {
            prepare(
                self.driver,
                profile.as_ptr(),
                profile.len() as u32,
                sample_rate_hz,
                MAXIMUM_LINK_FRAME_COUNT,
                ptr::from_mut(&mut graph),
            )
        };
        match result {
            URP_AST_OK if !graph.is_null() => Ok(Some(graph)),
            URP_AST_NOT_READY => Ok(None),
            URP_AST_OK => Err(LinkHostError::Product(URP_AST_OK)),
            error => Err(LinkHostError::Product(error)),
        }
    }
}

struct ChannelSnapshot {
    hook: Option<LinkHookRef>,
    eligible: bool,
    name: String,
    sample_rate_hz: u32,
}

/// Direct retained link hooks prepared for one serialized reload.
#[derive(Default)]
pub(super) struct LinkReload {
    hooks: Vec<LinkHookRef>,
    finished: bool,
    control: Option<MutexGuard<'static, ()>>,
}

impl LinkReload {
    /// Commit every prepared graph, or discard every candidate on rollback.
    pub(super) fn finish(mut self, commit: bool) {
        self.finish_inner(commit);
        self.finished = true;
        self.control.take();
    }

    fn finish_inner(&mut self, commit: bool) {
        for hook in self.hooks.drain(..) {
            let raw = hook.raw.as_ptr();
            // SAFETY: the retained hook remains live through this critical section.
            let audiohook = unsafe { ptr::addr_of_mut!((*raw).audiohook) };
            // SAFETY: control-plane replacement quiesces the Asterisk callback.
            unsafe { ((*raw).asterisk.audiohook_lock)(audiohook) };
            // Only successfully staged hooks enter this transaction, and drain
            // consumes each once while LINK_CONTROL excludes another reload.
            // SAFETY: retained ownership protects the pending flag and candidate.
            unsafe { (*raw).reload_pending.store(false, Ordering::Release) };
            // SAFETY: this transaction exclusively owns candidate consumption.
            let candidate = unsafe { (*raw).staged.swap(ptr::null_mut(), Ordering::AcqRel) };
            let retired = if commit
                // SAFETY: attachment is atomically published by install/teardown.
                && unsafe { (*raw).attachment.load(Ordering::Acquire) } == LINK_ATTACHED
            {
                // SAFETY: callback is quiesced by the audiohook lock.
                unsafe {
                    (*(*raw).clock.get()).set_rate((*raw).staged_rate.load(Ordering::Acquire));
                    (*raw).active.swap(candidate, Ordering::AcqRel)
                }
            } else {
                candidate
            };
            // SAFETY: balances audiohook_lock above.
            unsafe { ((*raw).asterisk.audiohook_unlock)(audiohook) };
            if !retired.is_null() {
                // SAFETY: retired graph is no longer callback-visible.
                unsafe { ((*raw).product.destroy)(retired) };
            }
        }
    }
}

impl Drop for LinkReload {
    fn drop(&mut self) {
        if !self.finished {
            self.finish_inner(false);
        }
    }
}

struct IdleClock {
    rate: u32,
    hz: u32,
    remainder: u64,
    credit: u32,
}

impl IdleClock {
    fn new(rate: u32) -> Self {
        Self {
            rate,
            hz: rate.div_ceil(MAXIMUM_LINK_FRAME_COUNT).max(50),
            remainder: 0,
            credit: 0,
        }
    }

    fn set_rate(&mut self, rate: u32) {
        if self.rate != rate {
            *self = Self::new(rate);
        }
    }

    fn credit(&mut self, rate: u32, count: u32) {
        if rate == self.rate && count <= MAXIMUM_LINK_FRAME_COUNT {
            self.credit = self
                .credit
                .saturating_add(count)
                .min(MAXIMUM_LINK_FRAME_COUNT);
        }
    }

    fn tick(&mut self) -> u32 {
        self.remainder += u64::from(self.rate);
        let count = (self.remainder / u64::from(self.hz)) as u32;
        self.remainder %= u64::from(self.hz);
        let covered = self.credit.min(count);
        self.credit -= covered;
        count - covered
    }
}

struct IdleLease {
    hook: ManuallyDrop<LinkHookRef>,
    module: *mut ffi::ast_module,
    current_channel: *mut ffi::ast_channel,
    moving_to: *mut ffi::ast_channel,
    detaching: bool,
    id: c_int,
    slot: c_int,
    timer: *mut ffi::ast_timer,
    fd: c_int,
    timer_hz: u32,
    scratch: [i16; MAXIMUM_LINK_FRAME_COUNT as usize],
}

impl IdleLease {
    fn hook(&self) -> &LinkHook {
        // SAFETY: this lease owns one retained hook reference until Drop.
        unsafe { self.hook.raw.as_ref() }
    }

    unsafe fn install(
        raw: *mut LinkHook,
        channel: *mut ffi::ast_channel,
        module: *mut ffi::ast_module,
        rate: u32,
    ) -> Result<(), LinkHostError> {
        // SAFETY: the builder retains this pinned hook during setup.
        let operations = unsafe { (*raw).asterisk };
        // SAFETY: the Asterisk timer provider owns the returned handle.
        let timer = unsafe { (operations.timer_open)() };
        if timer.is_null() {
            return Err(LinkHostError::Asterisk);
        }
        // SAFETY: timer_open succeeded and setup has exclusive ownership.
        let fd = unsafe { (operations.timer_fd)(timer) };
        let hz = IdleClock::new(rate).hz;
        // SAFETY: the unpublished timer remains exclusively owned during cadence setup.
        if fd < 0 || unsafe { (operations.timer_set_rate)(timer, hz) } != 0 {
            // SAFETY: this unpublished timer has exactly one owner.
            unsafe { (operations.timer_close)(timer) };
            return Err(LinkHostError::Asterisk);
        }
        // SAFETY: the builder owns a live reference while acquiring this independent lease.
        let hook = unsafe { LinkHookRef::retain(raw) };
        // SAFETY: the module remains live through setup and this lease's paired unref.
        let module = unsafe {
            (operations.module_ref)(module, c"link".as_ptr(), line!() as c_int, c"idle".as_ptr())
        };
        let mut lease = Box::new(Self {
            hook: ManuallyDrop::new(hook),
            module,
            current_channel: channel,
            moving_to: ptr::null_mut(),
            detaching: false,
            id: -1,
            slot: -1,
            timer,
            fd,
            timer_hz: hz,
            scratch: [0; MAXIMUM_LINK_FRAME_COUNT as usize],
        });
        // SAFETY: the caller retains channel through setup and publication.
        unsafe { (operations.channel_lock)(channel) };
        if lease.hook().attachment.load(Ordering::Acquire) != LINK_ATTACHED {
            // SAFETY: concurrent datastore teardown won; no framehook was published.
            unsafe { (operations.channel_unlock)(channel) };
            return Err(LinkHostError::Asterisk);
        }
        // SAFETY: this channel is locked; registration precedes framehook publication.
        lease.slot = unsafe { (operations.channel_fd_add)(channel, fd) };
        if lease.slot < 0 {
            // SAFETY: balances channel_lock before the unpublished lease is dropped.
            unsafe { (operations.channel_unlock)(channel) };
            return Err(LinkHostError::Asterisk);
        }
        // SAFETY: null optional callback pointers are valid in this C aggregate.
        let mut interface: ffi::ast_framehook_interface = unsafe { std::mem::zeroed() };
        interface.version = ffi::AST_FRAMEHOOK_INTERFACE_VERSION as u16;
        interface.event_cb = Some(idle_event);
        interface.destroy_cb = Some(idle_destroy);
        interface.chan_fixup_cb = Some(idle_fixup);
        interface.chan_breakdown_cb = Some(idle_breakdown);
        interface.data = ptr::from_mut(lease.as_mut()).cast();
        // SAFETY: channel is locked; Asterisk copies the interface and calls ATTACHED immediately.
        lease.id = unsafe { (operations.framehook_attach)(channel, &raw mut interface) };
        if lease.id < 0 {
            // SAFETY: setup still owns the registered slot under the channel lock.
            unsafe { lease.unregister(channel) };
            // SAFETY: balances channel_lock before reclaiming the unpublished lease.
            unsafe { (operations.channel_unlock)(channel) };
            return Err(LinkHostError::Asterisk);
        }
        let lease = Box::into_raw(lease);
        // SAFETY: the framehook now owns this lease, protected by the channel lock.
        unsafe { (*raw).idle.store(lease, Ordering::Release) };
        // SAFETY: refresh an already-blocked nonbridged reader's FD wait set after publication.
        unsafe { (operations.channel_wake)(channel) };
        // SAFETY: balances setup's channel lock after ownership transfer.
        unsafe { (operations.channel_unlock)(channel) };
        Ok(())
    }

    unsafe fn unregister(&mut self, channel: *mut ffi::ast_channel) {
        let operations = self.hook().asterisk;
        if self.slot >= 0
            // SAFETY: the callback/setup caller holds this live channel's lock.
            && unsafe { (operations.channel_fd)(channel, self.slot) } == self.fd
        {
            // SAFETY: only the still-matching registration is removed.
            unsafe { (operations.channel_set_fd)(channel, self.slot, -1) };
        }
        self.slot = -1;
    }

    unsafe fn detach(&mut self, channel: *mut ffi::ast_channel) {
        self.detaching = true;
        let operations = self.hook().asterisk;
        // SAFETY: the caller holds the current channel lock; detach is asynchronous.
        let _ = unsafe { (operations.framehook_detach)(channel, self.id) };
        // SAFETY: a null wake ensures pending detachment is serviced without media injection.
        unsafe { (operations.channel_wake)(channel) };
    }

    unsafe fn fail(&mut self, channel: *mut ffi::ast_channel) {
        let operations = self.hook().asterisk;
        // SAFETY: this lease owns the timer; disarming prevents a failed FD from spinning.
        let _ = unsafe { (operations.timer_set_rate)(self.timer, 0) };
        // SAFETY: callback owns the current channel lock and retains this lease.
        unsafe { self.detach(channel) };
    }
}

impl Drop for IdleLease {
    fn drop(&mut self) {
        let operations = self.hook().asterisk;
        // SAFETY: setup failed before publication or DETACHED already removed the FD.
        unsafe { (operations.timer_close)(self.timer) };
        // SAFETY: destroy_cb/setup failure consumes this lease's single hook reference.
        unsafe { ManuallyDrop::drop(&mut self.hook) };
        // SAFETY: the module lease stays live through graph/hook destruction above.
        unsafe {
            (operations.module_unref)(
                self.module,
                c"link".as_ptr(),
                line!() as c_int,
                c"idle".as_ptr(),
            )
        };
    }
}

unsafe extern "C" fn idle_event(
    channel: *mut ffi::ast_channel,
    frame: *mut ffi::ast_frame,
    event: ffi::ast_framehook_event,
    data: *mut c_void,
) -> *mut ffi::ast_frame {
    // SAFETY: Asterisk retains our callback data and holds the channel lock.
    let lease = unsafe { &mut *data.cast::<IdleLease>() };
    let operations = lease.hook().asterisk;
    if event == ffi::AST_FRAMEHOOK_EVENT_DETACHED && channel == lease.current_channel {
        let slot = lease.slot;
        // SAFETY: terminal detachment owns this channel's registered timer slot.
        unsafe { lease.unregister(channel) };
        if !lease.moving_to.is_null() {
            lease.slot = slot;
            // SAFETY: datastore fixup recorded the locked destination before failed framehook copying.
            unsafe { lease.unregister(lease.moving_to) };
            lease.moving_to = ptr::null_mut();
        }
        lease.current_channel = ptr::null_mut();
        lease.hook().idle.store(ptr::null_mut(), Ordering::Release);
    } else if event == ffi::AST_FRAMEHOOK_EVENT_READ
        && channel == lease.current_channel
        && lease.slot >= 0
        && !lease.detaching
        // SAFETY: Asterisk holds the current channel lock during this event.
        && unsafe { (operations.channel_fdno)(channel) } == lease.slot
    {
        // SAFETY: this lease owns the timer until its eventual destroy callback.
        if unsafe { (operations.timer_get_event)(lease.timer) } != ffi::AST_TIMING_EVENT_EXPIRED {
            return frame;
        }
        // SAFETY: only an expired, selected timer is acknowledged, once per wake.
        if unsafe { (operations.timer_ack)(lease.timer, 1) } != 0 {
            // SAFETY: channel remains locked during failure cleanup scheduling.
            unsafe { lease.fail(channel) };
            return frame;
        }
        // SAFETY: consume only our selected FD; restarted framehook traversal must not ACK it twice.
        unsafe { (operations.channel_fdno_set)(channel, -1) };
        // SAFETY: the independent hook reference remains retained throughout this event.
        let hook = unsafe { lease.hook.raw.as_ref() };
        let audiohook = ptr::from_ref(&hook.audiohook).cast_mut();
        // SAFETY: the existing audiohook mutex serializes graph and clock access.
        unsafe { (operations.audiohook_lock)(audiohook) };
        // SAFETY: this callback holds the graph's existing serial-owner mutex.
        let clock = unsafe { &mut *hook.clock.get() };
        let graph = hook.active.load(Ordering::Acquire);
        let rate = clock.rate;
        let hz = clock.hz;
        let attached = hook.attachment.load(Ordering::Acquire) == LINK_ATTACHED;
        if lease.timer_hz != hz
            // SAFETY: the timer remains owned; only negotiated-rate changes reconfigure cadence.
            && unsafe { (operations.timer_set_rate)(lease.timer, hz) } != 0
        {
            // SAFETY: balances the serial-owner mutex before failure cleanup.
            unsafe { (operations.audiohook_unlock)(audiohook) };
            // SAFETY: current channel remains locked and the lease remains retained.
            unsafe { lease.fail(channel) };
            return frame;
        }
        lease.timer_hz = hz;
        if attached && !graph.is_null() {
            let count = clock.tick();
            if count != 0 {
                lease.scratch.fill(0);
                // SAFETY: count is at most960 and SILENCE consumes/discards this private workspace.
                let _ = unsafe {
                    (hook.product.process)(
                        graph,
                        URP_AST_LINK_DIRECTION_SILENCE,
                        rate,
                        lease.scratch.as_mut_ptr(),
                        count,
                    )
                };
                lease.scratch.fill(0);
            }
        }
        // SAFETY: balances the graph/clock serial-owner mutex above.
        unsafe { (operations.audiohook_unlock)(audiohook) };
        if !attached {
            // SAFETY: a datastore teardown invalidated DSP; schedule the lease's final destruction.
            unsafe { lease.detach(channel) };
        }
    }
    frame
}

unsafe extern "C" fn idle_fixup(
    data: *mut c_void,
    id: c_int,
    old: *mut ffi::ast_channel,
    new: *mut ffi::ast_channel,
) {
    // SAFETY: Asterisk holds both channel locks and preserves this data across the move.
    let lease = unsafe { &mut *data.cast::<IdleLease>() };
    let slot = lease.slot;
    // SAFETY: remove only this timer's old registration while both channels are locked.
    unsafe { lease.unregister(old) };
    lease.current_channel = new;
    lease.moving_to = ptr::null_mut();
    lease.id = id;
    // Asterisk copied every old FD slot before invoking framehook fixup.
    lease.slot = slot;
    if lease.detaching {
        // SAFETY: Asterisk reattachment does not carry its old pending-detach flag.
        unsafe { lease.detach(new) };
    } else {
        // SAFETY: both channel locks remain held while the new reader is notified.
        unsafe { (lease.hook().asterisk.channel_wake)(new) };
    }
}

unsafe extern "C" fn idle_breakdown(
    data: *mut c_void,
    id: c_int,
    _old: *mut ffi::ast_channel,
    new: *mut ffi::ast_channel,
) {
    // SAFETY: Asterisk holds both channel locks while preserving this recipient hook.
    let lease = unsafe { &mut *data.cast::<IdleLease>() };
    let operations = lease.hook().asterisk;
    lease.current_channel = new;
    lease.id = id;
    if lease.detaching {
        // SAFETY: preserve recipient cleanup intent after its FD array was replaced.
        unsafe { lease.detach(new) };
        return;
    }
    // SAFETY: incoming channel FD slots may have replaced this timer's old slot.
    lease.slot = unsafe { (operations.channel_fd_add)(new, lease.fd) };
    if lease.slot < 0 {
        // SAFETY: recipient channel remains locked during asynchronous cleanup scheduling.
        unsafe { lease.fail(new) };
    } else {
        // SAFETY: refresh the recipient reader after registering its surviving timer.
        unsafe { (operations.channel_wake)(new) };
    }
}

unsafe extern "C" fn idle_destroy(data: *mut c_void) {
    // SAFETY: Asterisk calls destroy once, after terminal DETACHED and never for preserved old data.
    drop(unsafe { Box::from_raw(data.cast::<IdleLease>()) });
}

/// Pinned Asterisk audiohook followed by Rust-owned lifecycle state.
#[repr(C)]
struct LinkHook {
    audiohook: ffi::ast_audiohook,
    product: LinkProductOperations,
    asterisk: AsteriskOperations,
    active: AtomicPtr<c_void>,
    staged: AtomicPtr<c_void>,
    staged_rate: AtomicU32,
    clock: UnsafeCell<IdleClock>,
    idle: AtomicPtr<IdleLease>,
    references: AtomicUsize,
    attachment: AtomicU8,
    reload_pending: AtomicBool,
    profile_bound: AtomicBool,
    profile: Box<str>,
    asterisk_channel: Box<str>,
}

impl LinkHook {
    fn new(
        product: LinkProductOperations,
        asterisk: AsteriskOperations,
        profile: &str,
        asterisk_channel: &str,
        sample_rate_hz: u32,
        graph: *mut c_void,
        mode: InstallMode,
    ) -> Self {
        let staged = mode == InstallMode::Staged;
        // SAFETY: every bit pattern in the generated C audiohook structure is valid;
        // Asterisk initializes the complete value before publication.
        let audiohook = unsafe { std::mem::zeroed() };
        Self {
            audiohook,
            product,
            asterisk,
            active: AtomicPtr::new(if staged { ptr::null_mut() } else { graph }),
            staged: AtomicPtr::new(if staged { graph } else { ptr::null_mut() }),
            staged_rate: AtomicU32::new(sample_rate_hz),
            clock: UnsafeCell::new(IdleClock::new(sample_rate_hz)),
            idle: AtomicPtr::new(ptr::null_mut()),
            references: AtomicUsize::new(1),
            attachment: AtomicU8::new(LINK_BUILDING),
            reload_pending: AtomicBool::new(staged),
            profile_bound: AtomicBool::new(mode == InstallMode::Bound),
            profile: profile.into(),
            asterisk_channel: asterisk_channel.into(),
        }
    }

    fn prepare_active_if_missing(
        &self,
        driver: *mut c_void,
        profile: &str,
        sample_rate_hz: u32,
    ) -> Result<(), LinkHostError> {
        if self.attachment.load(Ordering::Acquire) != LINK_ATTACHED
            || !self.active.load(Ordering::Acquire).is_null()
            || self.reload_pending.load(Ordering::Acquire)
        {
            return Ok(());
        }
        let mut candidate = ptr::null_mut();
        // Explicit peers retain their exact radio; legacy discovery keeps its
        // existing active-profile selection when reactivating a dormant hook.
        let profile = if self.profile_bound.load(Ordering::Acquire) {
            &self.profile
        } else {
            profile
        };
        // SAFETY: profile and driver remain live for this synchronous call.
        let result = unsafe {
            (self.product.prepare)(
                driver,
                profile.as_ptr(),
                profile.len() as u32,
                sample_rate_hz,
                MAXIMUM_LINK_FRAME_COUNT,
                ptr::from_mut(&mut candidate),
            )
        };
        if result != URP_AST_OK && result != URP_AST_NOT_READY {
            return Err(LinkHostError::Product(result));
        }
        // SAFETY: self is pinned while the datastore or this retained reference exists.
        let audiohook = ptr::from_ref(&self.audiohook).cast_mut();
        // SAFETY: control-plane update quiesces the callback.
        unsafe { (self.asterisk.audiohook_lock)(audiohook) };
        // LINK_CONTROL keeps active/pending unchanged during preparation, but
        // external datastore destruction may have detached the retained hook.
        if self.attachment.load(Ordering::Acquire) == LINK_ATTACHED {
            // SAFETY: the audiohook lock serializes clock and graph replacement.
            unsafe { (*self.clock.get()).set_rate(sample_rate_hz) };
            self.active.store(candidate, Ordering::Release);
            candidate = ptr::null_mut();
        }
        // SAFETY: balances audiohook_lock above.
        unsafe { (self.asterisk.audiohook_unlock)(audiohook) };
        if !candidate.is_null() {
            // SAFETY: candidate never became callback-visible.
            unsafe { (self.product.destroy)(candidate) };
        }
        Ok(())
    }

    fn prepare_reload(
        &self,
        driver: *mut c_void,
        sample_rate_hz: u32,
    ) -> Result<bool, LinkHostError> {
        let audiohook = ptr::from_ref(&self.audiohook).cast_mut();
        // SAFETY: this retained hook stays pinned; the lock protects active
        // graph lifetime against both callbacks and external datastore teardown.
        unsafe { (self.asterisk.audiohook_lock)(audiohook) };
        let active = self.active.load(Ordering::Acquire);
        let unchanged = if self.reload_pending.load(Ordering::Acquire) {
            Err(LinkHostError::Asterisk)
        } else if active.is_null() {
            Ok(false)
        } else {
            let mut unchanged = 0;
            // SAFETY: the live graph is retained and quiescent while its immutable
            // description is compared; no graph construction or warmup occurs.
            let status = unsafe {
                (self.product.reload_unchanged)(
                    driver,
                    active,
                    self.profile.as_ptr(),
                    self.profile.len() as u32,
                    sample_rate_hz,
                    MAXIMUM_LINK_FRAME_COUNT,
                    &raw mut unchanged,
                )
            };
            if status == URP_AST_OK {
                Ok(unchanged != 0)
            } else {
                Err(LinkHostError::Product(status))
            }
        };
        // SAFETY: balances the lock above, including comparison errors.
        unsafe { (self.asterisk.audiohook_unlock)(audiohook) };
        if unchanged? {
            return Ok(false);
        }
        let mut candidate = ptr::null_mut();
        // SAFETY: profile storage and driver remain live for this synchronous call.
        let result = unsafe {
            (self.product.prepare_reload)(
                driver,
                self.profile.as_ptr(),
                self.profile.len() as u32,
                sample_rate_hz,
                MAXIMUM_LINK_FRAME_COUNT,
                ptr::from_mut(&mut candidate),
            )
        };
        if result != URP_AST_OK && result != URP_AST_NOT_READY {
            return Err(LinkHostError::Product(result));
        }
        // SAFETY: the serialized reload control plane quiesces the callback.
        unsafe { (self.asterisk.audiohook_lock)(audiohook) };
        if self.reload_pending.swap(true, Ordering::AcqRel) {
            // SAFETY: balances audiohook_lock before returning the invariant failure.
            unsafe { (self.asterisk.audiohook_unlock)(audiohook) };
            if !candidate.is_null() {
                // SAFETY: candidate was never published.
                unsafe { (self.product.destroy)(candidate) };
            }
            return Err(LinkHostError::Asterisk);
        }
        self.staged.store(candidate, Ordering::Release);
        self.staged_rate.store(sample_rate_hz, Ordering::Release);
        // SAFETY: balances audiohook_lock above.
        unsafe { (self.asterisk.audiohook_unlock)(audiohook) };
        Ok(true)
    }

    fn observe(&self) -> Result<Option<LinkStatistics>, LinkHostError> {
        let audiohook = ptr::from_ref(&self.audiohook).cast_mut();
        // SAFETY: control-plane observation quiesces the callback.
        unsafe { (self.asterisk.audiohook_lock)(audiohook) };
        let graph = self.active.load(Ordering::Acquire);
        let result = if graph.is_null() {
            Ok(None)
        } else {
            let mut observation = UrpAstLinkObservation {
                struct_size: std::mem::size_of::<UrpAstLinkObservation>() as u32,
                abi_version: ABI_VERSION,
                ..UrpAstLinkObservation::default()
            };
            // SAFETY: callback is quiesced and output has the complete current ABI size.
            let status = unsafe { (self.product.observe)(graph, ptr::from_mut(&mut observation)) };
            if status != URP_AST_OK {
                Err(LinkHostError::Product(status))
            } else if observation.struct_size as usize
                == std::mem::size_of::<UrpAstLinkObservation>()
                && observation.abi_version == ABI_VERSION
            {
                Ok(Some(LinkStatistics {
                    asterisk_channel: self.asterisk_channel.clone(),
                    observation,
                }))
            } else {
                Err(LinkHostError::Product(
                    super::super::URP_AST_INCOMPATIBLE_ABI,
                ))
            }
        };
        // SAFETY: balances audiohook_lock above.
        unsafe { (self.asterisk.audiohook_unlock)(audiohook) };
        result
    }

    unsafe fn retain(raw: *mut Self) {
        // SAFETY: caller holds an existing reference while incrementing.
        let prior = unsafe { (*raw).references.fetch_add(1, Ordering::Relaxed) };
        assert!(
            prior != 0 && prior != usize::MAX,
            "invalid link-hook reference count"
        );
    }

    unsafe fn release(raw: *mut Self) {
        // SAFETY: caller transfers one live reference.
        if unsafe { (*raw).references.fetch_sub(1, Ordering::AcqRel) } != 1 {
            return;
        }
        // SAFETY: final ownership permits teardown and allocation recovery.
        let hook = unsafe { &mut *raw };
        if hook.attachment.swap(LINK_DETACHED, Ordering::AcqRel) == LINK_ATTACHED {
            // SAFETY: final teardown prevents future callbacks before graph destruction.
            unsafe { (hook.asterisk.audiohook_detach)(ptr::addr_of_mut!(hook.audiohook)) };
        }
        // SAFETY: audiohook was initialized before reference publication.
        unsafe { (hook.asterisk.audiohook_destroy)(ptr::addr_of_mut!(hook.audiohook)) };
        let active = hook.active.swap(ptr::null_mut(), Ordering::AcqRel);
        let staged = hook.staged.swap(ptr::null_mut(), Ordering::AcqRel);
        if !active.is_null() {
            // SAFETY: detached callback can no longer access this graph.
            unsafe { (hook.product.destroy)(active) };
        }
        if !staged.is_null() {
            // SAFETY: staged graph was never callback-visible after final detachment.
            unsafe { (hook.product.destroy)(staged) };
        }
        // SAFETY: final reference consumes the unique pinned allocation.
        drop(unsafe { Box::from_raw(raw) });
    }
}

/// One direct hook reference independent of Asterisk channel masquerades.
pub(super) struct LinkHookRef {
    raw: std::ptr::NonNull<LinkHook>,
}

impl LinkHookRef {
    unsafe fn from_owned(raw: *mut LinkHook) -> Self {
        Self {
            // SAFETY: owned hook allocations are never null.
            raw: unsafe { std::ptr::NonNull::new_unchecked(raw) },
        }
    }

    unsafe fn retain(raw: *mut LinkHook) -> Self {
        // SAFETY: caller holds datastore ownership while retaining.
        unsafe { LinkHook::retain(raw) };
        // SAFETY: retained hook pointers are never null.
        unsafe { Self::from_owned(raw) }
    }

    fn prepare_active_if_missing(
        &self,
        driver: *mut c_void,
        profile: &str,
        sample_rate_hz: u32,
    ) -> Result<(), LinkHostError> {
        // SAFETY: this RAII reference keeps the hook alive.
        unsafe { self.raw.as_ref() }.prepare_active_if_missing(driver, profile, sample_rate_hz)
    }

    fn prepare_reload(
        &self,
        driver: *mut c_void,
        sample_rate_hz: u32,
    ) -> Result<bool, LinkHostError> {
        // SAFETY: this RAII reference keeps the hook alive.
        unsafe { self.raw.as_ref() }.prepare_reload(driver, sample_rate_hz)
    }

    fn observe(&self) -> Result<Option<LinkStatistics>, LinkHostError> {
        // SAFETY: this RAII reference keeps the hook alive.
        unsafe { self.raw.as_ref() }.observe()
    }
}

impl Drop for LinkHookRef {
    fn drop(&mut self) {
        // SAFETY: this owner releases exactly its retained reference.
        unsafe { LinkHook::release(self.raw.as_ptr()) };
    }
}

unsafe extern "C" fn link_datastore_destroy(data: *mut c_void) {
    if data.is_null() {
        return;
    }
    let raw = data.cast::<LinkHook>();
    // SAFETY: datastore payload is one live LinkHook allocation.
    let hook = unsafe { &*raw };
    if hook.attachment.swap(LINK_DETACHED, Ordering::AcqRel) == LINK_ATTACHED {
        // SAFETY: Asterisk detachment quiesces the manipulate callback.
        unsafe { (hook.asterisk.audiohook_detach)(ptr::from_ref(&hook.audiohook).cast_mut()) };
    }
    // SAFETY: consumes the datastore's single hook reference.
    unsafe { LinkHook::release(raw) };
}

unsafe extern "C" fn link_datastore_fixup(
    data: *mut c_void,
    _old: *mut ffi::ast_channel,
    new: *mut ffi::ast_channel,
) {
    // SAFETY: Asterisk retains its datastore payload and holds both channel locks during fixup.
    if let Some(hook) = unsafe { data.cast::<LinkHook>().as_ref() } {
        let idle = hook.idle.load(Ordering::Acquire);
        if !idle.is_null() {
            // SAFETY: both channel locks retain the lease until the ensuing framehook move completes.
            unsafe { (*idle).moving_to = new };
        }
    }
}

unsafe extern "C" fn link_audiohook_callback(
    audiohook: *mut ffi::ast_audiohook,
    _channel: *mut ffi::ast_channel,
    frame: *mut ffi::ast_frame,
    direction: ffi::ast_audiohook_direction,
) -> c_int {
    if audiohook.is_null() || frame.is_null() {
        return 0;
    }
    // SAFETY: LinkHook is repr(C) and audiohook is its pinned first member.
    let hook = unsafe { &*audiohook.cast::<LinkHook>() };
    // SAFETY: Asterisk owns these frame/audiohook fields for this callback duration.
    let status = unsafe { (*audiohook).status };
    // SAFETY: frame remains live and immutable except for its PCM payload.
    let frame_type = unsafe { (*frame).frametype };
    // SAFETY: frame remains live and immutable except for its PCM payload.
    let sample_count = unsafe { (*frame).samples };
    if hook.attachment.load(Ordering::Acquire) != LINK_ATTACHED
        || status == ffi::AST_AUDIOHOOK_STATUS_DONE
        || frame_type != ffi::AST_FRAME_VOICE
        || sample_count <= 0
    {
        return 0;
    }
    // SAFETY: voice-frame data union contains its writable PCM pointer.
    let samples = unsafe { (*frame).data.ptr.cast::<i16>() };
    if samples.is_null() {
        return 0;
    }
    let graph = hook.active.load(Ordering::Acquire);
    if graph.is_null() {
        return 0;
    }
    // SAFETY: voice-frame subclass contains its negotiated format pointer.
    let format = unsafe { (*frame).subclass.__bindgen_anon_1.format };
    let sample_rate_hz = if format.is_null() {
        0
    } else {
        // SAFETY: format belongs to this live frame.
        unsafe { (hook.asterisk.format_sample_rate)(format) }
    };
    let sample_rate_hz = if sample_rate_hz == 0 {
        FALLBACK_LINK_RATE_HZ
    } else {
        sample_rate_hz
    };
    let product_direction = if direction == ffi::AST_AUDIOHOOK_DIRECTION_READ {
        // SAFETY: Asterisk serializes this manipulate callback with the audiohook mutex.
        unsafe { (*hook.clock.get()).credit(sample_rate_hz, sample_count as u32) };
        URP_AST_LINK_DIRECTION_READ
    } else {
        URP_AST_LINK_DIRECTION_WRITE
    };
    // SAFETY: graph and frame storage remain live and exclusive for this bounded callback.
    let _ = unsafe {
        (hook.product.process)(
            graph,
            product_direction,
            sample_rate_hz,
            samples,
            sample_count as u32,
        )
    };
    0
}

struct SharedDatastoreInfo(ffi::ast_datastore_info);

// SAFETY: every field is immutable process-lifetime metadata or a function pointer.
unsafe impl Sync for SharedDatastoreInfo {}

static LINK_DATASTORE_INFO: SharedDatastoreInfo = SharedDatastoreInfo(ffi::ast_datastore_info {
    type_: c"usbradioplus-link".as_ptr(),
    duplicate: None,
    destroy: Some(link_datastore_destroy),
    chan_fixup: Some(link_datastore_fixup),
    chan_breakdown: None,
});

fn datastore_info() -> *const ffi::ast_datastore_info {
    ptr::from_ref(&LINK_DATASTORE_INFO.0)
}

unsafe fn destroy_uninitialized(raw: *mut LinkHook, audiohook_initialized: bool) {
    // SAFETY: caller has unique ownership before reference publication.
    let hook = unsafe { &mut *raw };
    if audiohook_initialized {
        // SAFETY: exactly balances the successful initialization.
        unsafe { (hook.asterisk.audiohook_destroy)(ptr::addr_of_mut!(hook.audiohook)) };
    }
    let active = hook.active.swap(ptr::null_mut(), Ordering::AcqRel);
    let staged = hook.staged.swap(ptr::null_mut(), Ordering::AcqRel);
    if !active.is_null() {
        // SAFETY: unpublished graph is consumed exactly once.
        unsafe { (hook.product.destroy)(active) };
    }
    if !staged.is_null() {
        // SAFETY: unpublished staged graph is consumed exactly once.
        unsafe { (hook.product.destroy)(staged) };
    }
    // SAFETY: allocation was never published and is uniquely owned.
    drop(unsafe { Box::from_raw(raw) });
}

unsafe fn c_bytes(raw: *const c_char) -> Vec<u8> {
    if raw.is_null() {
        Vec::new()
    } else {
        // SAFETY: Asterisk channel string accessors return live NUL-terminated text.
        unsafe { CStr::from_ptr(raw) }.to_bytes().to_vec()
    }
}

unsafe fn c_text(raw: *const c_char) -> String {
    // SAFETY: forwarded channel accessor follows the same string contract.
    String::from_utf8_lossy(&unsafe { c_bytes(raw) }).into_owned()
}

fn production_asterisk_operations() -> AsteriskOperations {
    AsteriskOperations {
        audiohook_init: production_audiohook_init,
        audiohook_attach: production_audiohook_attach,
        audiohook_detach: production_audiohook_detach,
        audiohook_destroy: production_audiohook_destroy,
        audiohook_lock: production_audiohook_lock,
        audiohook_unlock: production_audiohook_unlock,
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
        channel_wake: production_channel_wake,
        module_ref: ffi::__ast_module_ref,
        module_unref: ffi::__ast_module_unref,
        datastore_alloc: production_datastore_alloc,
        datastore_free: production_datastore_free,
        channel_lock: production_channel_lock,
        channel_unlock: production_channel_unlock,
        datastore_find: production_datastore_find,
        datastore_add: production_datastore_add,
        datastore_remove: production_datastore_remove,
        channel_name: production_channel_name,
        channel_application: production_channel_application,
        channel_data: production_channel_data,
        channel_sample_rate: production_channel_sample_rate,
        format_sample_rate: production_format_sample_rate,
        iterator_new: production_iterator_new,
        iterator_next: production_iterator_next,
        iterator_destroy: production_iterator_destroy,
        channel_unref: production_channel_unref,
        log_notice: production_log_notice,
        log_error: production_log_error,
    }
}

fn link_attached_diagnostic(channel: &str, staged: bool) -> String {
    let suffix = if staged { " for staged reload" } else { "" };
    format!("USBRadioPlus link processing attached to {channel}{suffix}")
}

fn link_attachment_diagnostic(channel: &str, error: LinkHostError) -> String {
    match error {
        LinkHostError::Asterisk => {
            format!("Unable to attach USBRadioPlus link processing to {channel} (Asterisk failure)")
        }
        LinkHostError::Product(status) => {
            format!("Unable to attach USBRadioPlus link processing to {channel} ({status})")
        }
    }
}

fn link_reload_diagnostic(channel: &str, error: LinkHostError) -> String {
    match error {
        LinkHostError::Asterisk => format!(
            "Unable to prepare replacement USBRadioPlus link processing for {channel} (Asterisk failure)"
        ),
        LinkHostError::Product(status) => format!(
            "Unable to prepare replacement USBRadioPlus link processing for {channel} ({status})"
        ),
    }
}

fn production_log_notice(message: &str) {
    // SAFETY: message length bounds the readable byte span and all variadic
    // arguments match Asterisk's public logging declaration.
    unsafe {
        ffi::ast_log(
            ffi::__LOG_NOTICE as c_int,
            c"usbradioplus-rust-host".as_ptr(),
            0,
            c"link".as_ptr(),
            c"%.*s\n".as_ptr(),
            message.len().min(c_int::MAX as usize) as c_int,
            message.as_ptr().cast::<c_char>(),
        )
    };
}

fn production_log_error(message: &str) {
    // SAFETY: message length bounds the readable byte span and all variadic
    // arguments match Asterisk's public logging declaration.
    unsafe {
        ffi::ast_log(
            ffi::__LOG_ERROR as c_int,
            c"usbradioplus-rust-host".as_ptr(),
            0,
            c"link".as_ptr(),
            c"%.*s\n".as_ptr(),
            message.len().min(c_int::MAX as usize) as c_int,
            message.as_ptr().cast::<c_char>(),
        )
    };
}

unsafe fn production_audiohook_init(audiohook: *mut ffi::ast_audiohook) -> c_int {
    // SAFETY: caller supplies pinned writable audiohook storage.
    unsafe {
        ffi::ast_audiohook_init(
            audiohook,
            ffi::AST_AUDIOHOOK_TYPE_MANIPULATE,
            c"USBRadioPlus".as_ptr(),
            ffi::AST_AUDIOHOOK_MANIPULATE_ALL_RATES,
        )
    }
}

unsafe fn production_channel_wake(channel: *mut ffi::ast_channel) {
    // SAFETY: the caller holds this live channel's lock; Asterisk copies the null frame.
    let _ = unsafe { ffi::ast_queue_frame(channel, &raw mut ffi::ast_null_frame) };
}

unsafe fn production_audiohook_attach(
    channel: *mut ffi::ast_channel,
    audiohook: *mut ffi::ast_audiohook,
) -> c_int {
    // SAFETY: caller supplies live channel and initialized hook pointers.
    unsafe { ffi::ast_audiohook_attach(channel, audiohook) }
}

unsafe fn production_audiohook_detach(audiohook: *mut ffi::ast_audiohook) {
    // SAFETY: caller supplies one initialized attached or detached hook.
    let _ = unsafe { ffi::ast_audiohook_detach(audiohook) };
}

unsafe fn production_audiohook_destroy(audiohook: *mut ffi::ast_audiohook) {
    // SAFETY: caller balances one successful audiohook initialization.
    let _ = unsafe { ffi::ast_audiohook_destroy(audiohook) };
}

unsafe fn production_audiohook_lock(audiohook: *mut ffi::ast_audiohook) {
    // SAFETY: caller supplies the mutex embedded in a live initialized hook.
    let _ = unsafe {
        __ast_pthread_mutex_lock(
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            line!() as c_int,
            c"production_audiohook_lock".as_ptr(),
            c"audiohook->lock".as_ptr(),
            ptr::addr_of_mut!((*audiohook).lock),
        )
    };
}

unsafe fn production_audiohook_unlock(audiohook: *mut ffi::ast_audiohook) {
    // SAFETY: caller balances a successful audiohook lock.
    let _ = unsafe {
        __ast_pthread_mutex_unlock(
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            line!() as c_int,
            c"production_audiohook_unlock".as_ptr(),
            c"audiohook->lock".as_ptr(),
            ptr::addr_of_mut!((*audiohook).lock),
        )
    };
}

unsafe fn production_datastore_alloc(
    info: *const ffi::ast_datastore_info,
    module: *mut ffi::ast_module,
) -> *mut ffi::ast_datastore {
    // SAFETY: metadata and module pointers have process lifetime.
    unsafe {
        __ast_datastore_alloc(
            info,
            ptr::null(),
            module,
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            line!() as c_int,
            c"production_datastore_alloc".as_ptr(),
        )
    }
}

unsafe fn production_datastore_free(datastore: *mut ffi::ast_datastore) {
    // SAFETY: caller transfers one removed or unpublished datastore.
    let _ = unsafe { ffi::ast_datastore_free(datastore) };
}

unsafe fn production_channel_lock(channel: *mut ffi::ast_channel) {
    // SAFETY: caller holds a live Asterisk channel reference.
    let _ = unsafe {
        __ao2_lock(
            channel.cast(),
            0,
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            c"production_channel_lock".as_ptr(),
            line!() as c_int,
            c"channel".as_ptr(),
        )
    };
}

unsafe fn production_channel_unlock(channel: *mut ffi::ast_channel) {
    // SAFETY: caller balances a successful channel lock.
    let _ = unsafe {
        __ao2_unlock(
            channel.cast(),
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            c"production_channel_unlock".as_ptr(),
            line!() as c_int,
            c"channel".as_ptr(),
        )
    };
}

unsafe fn production_datastore_find(
    channel: *mut ffi::ast_channel,
    info: *const ffi::ast_datastore_info,
) -> *mut ffi::ast_datastore {
    // SAFETY: caller holds the channel lock.
    unsafe { ffi::ast_channel_datastore_find(channel, info, ptr::null()) }
}

unsafe fn production_datastore_add(
    channel: *mut ffi::ast_channel,
    datastore: *mut ffi::ast_datastore,
) {
    // SAFETY: caller holds the channel lock and unpublished datastore ownership.
    let _ = unsafe { ffi::ast_channel_datastore_add(channel, datastore) };
}

unsafe fn production_datastore_remove(
    channel: *mut ffi::ast_channel,
    datastore: *mut ffi::ast_datastore,
) {
    // SAFETY: caller holds the channel lock and datastore belongs to it.
    let _ = unsafe { ffi::ast_channel_datastore_remove(channel, datastore) };
}

unsafe fn production_channel_name(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: caller holds the channel lock.
    unsafe { ffi::ast_channel_name(channel) }
}

unsafe fn production_channel_application(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: caller holds the channel lock.
    unsafe { ffi::ast_channel_appl(channel) }
}

unsafe fn production_channel_data(channel: *mut ffi::ast_channel) -> *const c_char {
    // SAFETY: caller holds the channel lock.
    unsafe { ffi::ast_channel_data(channel) }
}

unsafe fn production_channel_sample_rate(channel: *mut ffi::ast_channel) -> u32 {
    // SAFETY: caller holds the channel lock.
    let format = unsafe { ffi::ast_channel_rawreadformat(channel) };
    if format.is_null() {
        0
    } else {
        // SAFETY: format is borrowed from this locked channel.
        unsafe { ffi::ast_format_get_sample_rate(format) }
    }
}

unsafe fn production_format_sample_rate(format: *mut ffi::ast_format) -> u32 {
    // SAFETY: caller supplies a live frame format pointer.
    unsafe { ffi::ast_format_get_sample_rate(format) }
}

unsafe fn production_iterator_new() -> *mut ffi::ast_channel_iterator {
    // SAFETY: creates one owned Asterisk channel iterator.
    unsafe { ffi::ast_channel_iterator_all_new() }
}

unsafe fn production_iterator_next(
    iterator: *mut ffi::ast_channel_iterator,
) -> *mut ffi::ast_channel {
    // SAFETY: caller supplies one live Asterisk channel iterator.
    unsafe { ffi::ast_channel_iterator_next(iterator) }
}

unsafe fn production_iterator_destroy(iterator: *mut ffi::ast_channel_iterator) {
    // SAFETY: consumes the iterator returned by ast_channel_iterator_all_new.
    let _ = unsafe { ffi::ast_channel_iterator_destroy(iterator) };
}

unsafe fn production_channel_unref(channel: *mut ffi::ast_channel) {
    // SAFETY: iterator_next returned one owned AO2 channel reference.
    unsafe {
        ffi::__ao2_ref(
            channel.cast(),
            -1,
            ptr::null(),
            c"rust/asterisk/src/host/link.rs".as_ptr(),
            line!() as c_int,
            c"production_channel_unref".as_ptr(),
        )
    };
}

unsafe extern "C" {
    fn __ast_datastore_alloc(
        info: *const ffi::ast_datastore_info,
        uid: *const c_char,
        module: *mut ffi::ast_module,
        file: *const c_char,
        line: c_int,
        function: *const c_char,
    ) -> *mut ffi::ast_datastore;

    fn __ao2_lock(
        object: *mut c_void,
        lock_how: ffi::ao2_lock_req,
        file: *const c_char,
        function: *const c_char,
        line: c_int,
        variable: *const c_char,
    ) -> c_int;

    fn __ao2_unlock(
        object: *mut c_void,
        file: *const c_char,
        function: *const c_char,
        line: c_int,
        variable: *const c_char,
    ) -> c_int;

    fn __ast_pthread_mutex_lock(
        file: *const c_char,
        line: c_int,
        function: *const c_char,
        mutex_name: *const c_char,
        mutex: *mut ffi::ast_mutex_t,
    ) -> c_int;

    fn __ast_pthread_mutex_unlock(
        file: *const c_char,
        line: c_int,
        function: *const c_char,
        mutex_name: *const c_char,
        mutex: *mut ffi::ast_mutex_t,
    ) -> c_int;
}

#[cfg(test)]
#[path = "link_tests.rs"]
pub(super) mod tests;

#[cfg(test)]
#[path = "link_concurrency_tests.rs"]
mod concurrency_tests;
