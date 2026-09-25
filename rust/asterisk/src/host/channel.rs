//! Asterisk channel technologies and their pinned per-call state.

use std::ffi::{CStr, c_char, c_int, c_void};
use std::mem::{size_of, zeroed};
use std::ptr;
use std::sync::RwLockWriteGuard;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock, RwLock, TryLockError};
use std::thread::JoinHandle;

use super::super::{
    URP_AST_ASTERISK_FAILURE, URP_AST_CHANNEL_BUSY, URP_AST_CHANNEL_NOT_FOUND,
    URP_AST_JITTER_FIXED, URP_AST_OK, URP_AST_TRANSPORT_APP_RPT, URP_AST_TRANSPORT_RPT_ADVANCED,
    UrpAstChannelReserveArgs, UrpAstDescriptor, UrpAstJitterConfig, ffi,
};
use super::super::{UrpAstChannelCommand, UrpAstChannelStatus};
use super::control::{ControlOperation, ControlResult, run_control};
use super::delivery;

const APP_RPT_TECHNOLOGY: &str = "RadioPlus";
const ADVANCED_TECHNOLOGY: &str = "RadioPlusAdvanced";
const APP_RPT_RATE_HZ: u32 = 8_000;
const ADVANCED_RATE_HZ: u32 = 48_000;
const PENDING_TRANSMIT_VALID: u64 = 1 << 63;
const PENDING_TRANSMIT_KEYED: u64 = 1 << 32;
const FRAME_MILLISECONDS: u32 = 20;
const SOURCE_FILE: &CStr = c"usbradioplus-rust-host";
const SOURCE_FUNCTION: &CStr = c"channel_host";
const APP_RPT_DESCRIPTION: &CStr = c"USBRadioPlus app_rpt compatibility channel";
const ADVANCED_DESCRIPTION: &CStr = c"USBRadioPlus native rpt_advanced channel";

static HOST: OnceLock<Mutex<Option<HostState>>> = OnceLock::new();
static LIVE_CHANNELS: OnceLock<Mutex<Vec<usize>>> = OnceLock::new();
static CONTROL_GATE: OnceLock<RwLock<()>> = OnceLock::new();
static TASKPROCESSOR_SEQUENCE: AtomicUsize = AtomicUsize::new(0);
static ACTIVE_CHANNELS: AtomicUsize = AtomicUsize::new(0);

struct HostState {
    descriptor: usize,
    driver: usize,
    module: usize,
    app_technology: usize,
    advanced_technology: usize,
    app_format: usize,
    advanced_format: usize,
}

#[derive(Clone, Copy)]
struct HostSnapshot {
    descriptor: *const UrpAstDescriptor,
    driver: *mut c_void,
    module: *mut ffi::ast_module,
    technology: *mut ffi::ast_channel_tech,
    format: *mut ffi::ast_format,
}

/// A pinned channel wrapper referenced by Asterisk and injected Rust callbacks.
pub(super) struct Channel {
    _name: Box<str>,
    descriptor: *const UrpAstDescriptor,
    rust_channel: AtomicPtr<c_void>,
    control: *mut ffi::ast_taskprocessor,
    pub(super) dsp: *mut ffi::ast_dsp,
    pub(super) format: *mut ffi::ast_format,
    pub(super) sample_rate_hz: u32,
    pub(super) frame_samples: u32,
    owner: AtomicPtr<ffi::ast_channel>,
    worker: Mutex<Option<JoinHandle<()>>>,
    pub(super) delivery_stop: AtomicBool,
    pub(super) service_failed: AtomicBool,
    jitter_pending: AtomicBool,
    pending_transmit: AtomicU64,
    direct: AtomicBool,
}

// SAFETY: opaque Asterisk pointers are accessed only through Asterisk's public
// thread-safe APIs. Rust control calls are serialized by `control`.
unsafe impl Send for Channel {}
// SAFETY: mutable cross-thread state is atomic or protected by `worker`.
unsafe impl Sync for Channel {}

impl Channel {
    fn rust_channel(&self) -> *mut c_void {
        self.rust_channel.load(Ordering::Acquire)
    }

    fn control_admitted(&self, operation: ControlOperation) -> ControlResult {
        // SAFETY: control and descriptor outlive the channel, and the Rust
        // handle remains valid until its serialized Destroy operation.
        unsafe {
            run_control(
                self.control,
                self.descriptor,
                self.rust_channel(),
                operation,
            )
        }
    }

    fn control(&self, operation: ControlOperation) -> ControlResult {
        let gate = match control_gate().try_read() {
            Ok(gate) => gate,
            Err(TryLockError::WouldBlock) => {
                return ControlResult::Status(URP_AST_CHANNEL_BUSY);
            }
            Err(TryLockError::Poisoned(_)) => {
                return ControlResult::Status(URP_AST_ASTERISK_FAILURE);
            }
        };
        let result = self.control_admitted(operation);
        drop(gate);
        result
    }

    fn control_status(&self, operation: ControlOperation) -> i32 {
        self.control(operation).status()
    }

    fn control_status_admitted(&self, operation: ControlOperation) -> i32 {
        self.control_admitted(operation).status()
    }

    pub(super) fn name(&self) -> &str {
        &self._name
    }

    fn stop_worker(&self) {
        self.delivery_stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.lock().expect("worker lock poisoned").take() {
            let _ = worker.join();
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct TechnologyContract {
    pub(super) transport: u32,
    pub(super) sample_rate_hz: u32,
}

pub(super) fn technology_contract(name: &str) -> Option<TechnologyContract> {
    if name.eq_ignore_ascii_case(APP_RPT_TECHNOLOGY) {
        Some(TechnologyContract {
            transport: URP_AST_TRANSPORT_APP_RPT,
            sample_rate_hz: APP_RPT_RATE_HZ,
        })
    } else if name.eq_ignore_ascii_case(ADVANCED_TECHNOLOGY) {
        Some(TechnologyContract {
            transport: URP_AST_TRANSPORT_RPT_ADVANCED,
            sample_rate_hz: ADVANCED_RATE_HZ,
        })
    } else {
        None
    }
}

fn channel_name_length(name: &[u8]) -> Option<u32> {
    if name.is_empty() {
        None
    } else {
        u32::try_from(name.len()).ok()
    }
}

fn reservation_failure_diagnostic(
    technology: &str,
    channel: &[u8],
    status: c_int,
) -> (c_int, String) {
    let channel = String::from_utf8_lossy(channel);
    if status == URP_AST_CHANNEL_NOT_FOUND {
        (
            ffi::__LOG_WARNING as c_int,
            format!("{technology}/{channel}: Rust channel was not configured"),
        )
    } else {
        (
            ffi::__LOG_ERROR as c_int,
            format!("{technology}/{channel}: Rust channel reservation failed ({status})"),
        )
    }
}

fn log_reservation_failure(technology: &str, channel: &[u8], status: c_int) {
    let (level, message) = reservation_failure_diagnostic(technology, channel, status);
    // SAFETY: the precision bounds the readable byte span and every variadic
    // argument matches Asterisk's public logger declaration.
    unsafe {
        ffi::ast_log(
            level,
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
            c"%.*s\n".as_ptr(),
            message.len().min(c_int::MAX as usize) as c_int,
            message.as_ptr().cast::<c_char>(),
        )
    };
}

fn format_has_rate<T>(format: *mut T, expected: u32, rate: impl FnOnce(*mut T) -> u32) -> bool {
    !format.is_null() && rate(format) == expected
}

fn apply_pending_jitter(
    pending: &AtomicBool,
    configure: impl FnOnce() -> Result<(), ()>,
) -> Result<(), ()> {
    if !pending.load(Ordering::Acquire) {
        return Ok(());
    }
    configure()?;
    pending.store(false, Ordering::Release);
    Ok(())
}

pub(super) fn forced_ctcss_tenths_hz(payload: &[u8]) -> Result<u32, ()> {
    if payload.is_empty() {
        return Ok(0);
    }
    let payload = payload.strip_suffix(b"\0").unwrap_or(payload);
    if payload.is_empty() || payload.contains(&0) {
        return Err(());
    }
    let text = std::str::from_utf8(payload).map_err(|_| ())?;
    let frequency = text.parse::<f64>().map_err(|_| ())?;
    if !frequency.is_finite() || frequency < 0.0 || frequency > f64::from(u32::MAX) / 10.0 {
        return Err(());
    }
    Ok((frequency.mul_add(10.0, 0.5).floor()) as u32)
}

/// Check the payload fields that the legacy ASL3 radio drivers require.
///
/// `ast_frame.samples` is advisory at this boundary: `chan_usbradio` accepts
/// a frame when its signed-16 payload has the expected length and does not
/// reject it because the duplicate sample-count field is unset or stale.
pub(super) fn voice_frame_is_valid(
    frame_type: ffi::ast_frame_type,
    data_length: i32,
    _sample_count: i32,
    data_is_null: bool,
    expected_samples: u32,
) -> bool {
    frame_type == ffi::AST_FRAME_VOICE
        && !data_is_null
        && data_length >= 0
        && data_length % size_of::<i16>() as i32 == 0
        && u32::try_from(data_length / size_of::<i16>() as i32) == Ok(expected_samples)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct PendingTransmit(u64);

impl PendingTransmit {
    pub(super) const NONE: Self = Self(0);

    pub(super) const fn new(keyed: bool, ctcss_tenths_hz: u32) -> Self {
        Self(
            PENDING_TRANSMIT_VALID
                | if keyed { PENDING_TRANSMIT_KEYED } else { 0 }
                | ctcss_tenths_hz as u64,
        )
    }

    pub(super) const fn decode(self) -> Option<(bool, u32)> {
        if self.0 & PENDING_TRANSMIT_VALID == 0 {
            None
        } else {
            Some((self.0 & PENDING_TRANSMIT_KEYED != 0, self.0 as u32))
        }
    }

    pub(super) const fn raw(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TransmitAttempt {
    Deferred,
    Completed(i32),
    Failed,
}

fn finish_transmit_attempt(
    pending: &AtomicU64,
    intent: PendingTransmit,
    attempt: TransmitAttempt,
) -> c_int {
    match attempt {
        TransmitAttempt::Deferred => {
            pending.store(intent.raw(), Ordering::Release);
            0
        }
        TransmitAttempt::Completed(URP_AST_OK) => 0,
        TransmitAttempt::Completed(_) | TransmitAttempt::Failed => -1,
    }
}

pub(super) fn jitter_configuration(resolved: UrpAstJitterConfig) -> ffi::ast_jb_conf {
    let mut config = ffi::ast_jb_conf {
        flags: 0,
        max_size: resolved.maximum_size_ms.into(),
        resync_threshold: resolved.resync_threshold_ms.into(),
        impl_: [0; ffi::AST_JB_IMPL_NAME_SIZE as usize],
        target_extra: resolved.target_extra_ms.into(),
    };
    if resolved.enabled != 0 {
        config.flags |= ffi::AST_JB_ENABLED;
    }
    if resolved.force_enabled != 0 {
        config.flags |= ffi::AST_JB_FORCED;
    }
    if resolved.logging_enabled != 0 {
        config.flags |= ffi::AST_JB_LOG;
    }
    if resolved.video_sync_enabled != 0 {
        config.flags |= ffi::AST_JB_SYNC_VIDEO;
    }
    let implementation = if resolved.implementation == URP_AST_JITTER_FIXED {
        b"fixed".as_slice()
    } else {
        b"adaptive".as_slice()
    };
    for (output, input) in config.impl_.iter_mut().zip(implementation) {
        *output = *input as libc::c_char;
    }
    config
}

fn host() -> &'static Mutex<Option<HostState>> {
    HOST.get_or_init(|| Mutex::new(None))
}

fn live_channels() -> &'static Mutex<Vec<usize>> {
    LIVE_CHANNELS.get_or_init(|| Mutex::new(Vec::new()))
}

fn lock_idle_membership<'a, T>(
    membership: &'a Mutex<T>,
    active: &AtomicUsize,
) -> Result<MutexGuard<'a, T>, c_int> {
    let membership = membership
        .lock()
        .expect("live channel membership lock poisoned");
    if active.load(Ordering::Acquire) != 0 {
        return Err(URP_AST_CHANNEL_BUSY);
    }
    Ok(membership)
}

fn control_gate() -> &'static RwLock<()> {
    CONTROL_GATE.get_or_init(|| RwLock::new(()))
}

/// Stable driver access shared by reload and CLI host modules.
#[derive(Clone, Copy)]
pub(super) struct DriverContext {
    pub(super) descriptor: *const UrpAstDescriptor,
    pub(super) driver: *mut c_void,
}

pub(super) fn driver_context() -> Option<DriverContext> {
    let installed = host().lock().expect("channel host lock poisoned");
    let state = installed.as_ref()?;
    Some(DriverContext {
        descriptor: state.descriptor as *const UrpAstDescriptor,
        driver: state.driver as *mut c_void,
    })
}

/// Find the first configured profile which currently has a live channel.
pub(super) fn first_live_profile() -> Option<Box<str>> {
    let context = driver_context()?;
    let channels = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    first_profile(context, &channels)
}

fn first_profile(context: DriverContext, channels: &[usize]) -> Option<Box<str>> {
    // SAFETY: registration validated this process-lifetime descriptor.
    let query = unsafe { (*context.descriptor).driver_channel_name? };
    // The product descriptor enumerates nonempty names parsed from u32-sized
    // configuration input, so it reports exhaustion before the index can wrap.
    let mut index = 0_u32;
    loop {
        let mut length = 0;
        // SAFETY: the driver remains installed while channel membership is live.
        if unsafe { query(context.driver, index, ptr::null_mut(), 0, &raw mut length) }
            != URP_AST_OK
            || length == 0
        {
            return None;
        }
        let mut name = vec![0_u8; length as usize];
        // SAFETY: name advertises exactly length writable bytes.
        if unsafe {
            query(
                context.driver,
                index,
                name.as_mut_ptr(),
                length,
                &raw mut length,
            )
        } != URP_AST_OK
        {
            return None;
        }
        name.truncate(length as usize);
        let name = std::str::from_utf8(&name).ok()?;
        if channels
            .iter()
            .map(|address| {
                // SAFETY: the membership lock keeps each pinned allocation live.
                unsafe { &*(*address as *const Channel) }
            })
            .any(|channel| channel.name().eq_ignore_ascii_case(name))
        {
            return Some(name.to_owned().into_boxed_str());
        }
        index += 1;
    }
}

/// Run one CLI operation while teardown of the selected channel is excluded.
pub(super) fn with_live_channel<T>(name: &str, operation: impl FnOnce(&Channel) -> T) -> Option<T> {
    let channels = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    let channel = channels
        .iter()
        .map(|address| {
            // SAFETY: the membership lock keeps each pinned allocation live.
            unsafe { &*(*address as *const Channel) }
        })
        .find(|channel| channel.name().eq_ignore_ascii_case(name))?;
    Some(operation(channel))
}

/// Frozen live membership plus exclusive admission to channel control.
pub(super) struct LiveChannelsGuard {
    channels: MutexGuard<'static, Vec<usize>>,
    control: Option<RwLockWriteGuard<'static, ()>>,
}

impl LiveChannelsGuard {
    pub(super) fn iter(&self) -> impl Iterator<Item = &Channel> {
        self.channels.iter().map(|address| {
            // SAFETY: membership is frozen, so every address remains pinned.
            unsafe { &*(*address as *const Channel) }
        })
    }

    /// Reopen ordinary control admission while retaining stable membership.
    pub(super) fn reopen_control(&mut self) {
        self.control.take();
    }

    /// Resolve the first configured profile present in this frozen membership.
    pub(super) fn first_profile(&self, context: DriverContext) -> Option<Box<str>> {
        first_profile(context, &self.channels)
    }
}

pub(super) fn lock_live_channels() -> LiveChannelsGuard {
    let channels = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    let control = control_gate()
        .write()
        .expect("channel control admission lock poisoned");
    LiveChannelsGuard {
        channels,
        control: Some(control),
    }
}

fn descriptor_is_valid(descriptor: &UrpAstDescriptor) -> bool {
    descriptor.struct_size as usize >= size_of::<UrpAstDescriptor>()
        && descriptor.abi_version == super::super::ABI_VERSION
        && descriptor.driver_reload.is_some()
        && descriptor.driver_reload_finish.is_some()
        && descriptor.driver_channel_name.is_some()
        && descriptor.driver_active_channel.is_some()
        && descriptor.driver_set_active_channel.is_some()
        && descriptor.channel_reserve.is_some()
        && descriptor.channel_start.is_some()
        && descriptor.channel_stop.is_some()
        && descriptor.channel_reload_prepare.is_some()
        && descriptor.channel_reload_activate.is_some()
        && descriptor.channel_reload_finish.is_some()
        && descriptor.channel_write_voice.is_some()
        && descriptor.channel_write_text.is_some()
        && descriptor.channel_set_transmit.is_some()
        && descriptor.channel_set_dtmf.is_some()
        && descriptor.channel_set_echo.is_some()
        && descriptor.channel_set_direct_callbacks.is_some()
        && descriptor.channel_get_jitter_config.is_some()
        && descriptor.channel_command.is_some()
        && descriptor.channel_get_status.is_some()
        && descriptor.channel_service.is_some()
        && descriptor.channel_destroy.is_some()
}

unsafe fn release_ao2(pointer: *mut c_void) {
    if !pointer.is_null() {
        // SAFETY: pointer is one owned AO2 reference allocated by Asterisk.
        unsafe {
            ffi::__ao2_ref(
                pointer,
                -1,
                ptr::null(),
                SOURCE_FILE.as_ptr(),
                0,
                SOURCE_FUNCTION.as_ptr(),
            )
        };
    }
}

unsafe fn capability(format: *mut ffi::ast_format) -> *mut ffi::ast_format_cap {
    // SAFETY: all arguments follow the public Asterisk allocation contract.
    let result = unsafe {
        ffi::__ast_format_cap_alloc(
            ffi::AST_FORMAT_CAP_FLAG_DEFAULT,
            ptr::null(),
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
        )
    };
    if result.is_null() {
        return result;
    }
    // SAFETY: result and format are live Asterisk objects.
    if unsafe {
        ffi::__ast_format_cap_append(
            result,
            format,
            0,
            ptr::null(),
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
        )
    } != 0
    {
        // SAFETY: result owns one AO2 reference.
        unsafe { release_ao2(result.cast()) };
        ptr::null_mut()
    } else {
        result
    }
}

unsafe fn technology(
    name: &'static CStr,
    description: &'static CStr,
    capabilities: *mut ffi::ast_format_cap,
) -> Box<ffi::ast_channel_tech> {
    // SAFETY: an all-zero technology means unsupported optional callbacks.
    let mut technology: Box<ffi::ast_channel_tech> = Box::new(unsafe { zeroed() });
    technology.type_ = name.as_ptr();
    technology.description = description.as_ptr();
    technology.capabilities = capabilities;
    technology.requester = Some(request);
    technology.send_digit_begin = Some(digit_begin);
    technology.send_digit_end = Some(digit_end);
    technology.send_text = Some(send_text);
    technology.hangup = Some(hangup);
    technology.answer = Some(answer);
    technology.read = Some(read);
    technology.call = Some(call);
    technology.write = Some(write);
    technology.indicate = Some(indicate);
    technology.fixup = Some(fixup);
    technology.setoption = Some(setoption);
    technology
}

/// Register the two USBRadioPlus Asterisk channel technologies.
///
/// The driver must have been created with
/// [`usbradioplus_asterisk_channel_host_operations`](super::delivery::usbradioplus_asterisk_channel_host_operations).
///
/// # Safety
///
/// `descriptor`, `driver`, and `module` must remain valid until successful
/// unregistration, after every hosted channel has hung up.
pub(super) unsafe fn usbradioplus_asterisk_channel_host_register(
    descriptor: *const UrpAstDescriptor,
    driver: *mut c_void,
    module: *mut c_void,
) -> c_int {
    if descriptor.is_null() || driver.is_null() || module.is_null() {
        return super::super::URP_AST_INVALID_ARGUMENT;
    }
    // SAFETY: the caller promises a live process-lifetime descriptor.
    if !descriptor_is_valid(unsafe { &*descriptor }) {
        return super::super::URP_AST_INCOMPATIBLE_ABI;
    }
    let Ok(_membership) = lock_idle_membership(live_channels(), &ACTIVE_CHANNELS) else {
        return URP_AST_CHANNEL_BUSY;
    };
    let mut installed = host().lock().expect("channel host lock poisoned");
    if installed.is_some() {
        return URP_AST_CHANNEL_BUSY;
    }
    // SAFETY: these process-lifetime formats are owned by Asterisk.
    let app_format = unsafe { ffi::ast_format_slin };
    // SAFETY: the public format cache returns a process-lifetime object.
    let advanced_format = unsafe { ffi::ast_format_cache_get_slin_by_rate(ADVANCED_RATE_HZ) };
    if app_format.is_null()
        || !format_has_rate(advanced_format, ADVANCED_RATE_HZ, |format| {
            // SAFETY: the public format cache returned this live format.
            unsafe { ffi::ast_format_get_sample_rate(format) }
        })
    {
        return URP_AST_ASTERISK_FAILURE;
    }
    // SAFETY: the formats are live Asterisk objects.
    let app_capability = unsafe { capability(app_format) };
    // SAFETY: the format is a live Asterisk object.
    let advanced_capability = unsafe { capability(advanced_format) };
    if app_capability.is_null() || advanced_capability.is_null() {
        // SAFETY: each non-null capability owns one reference.
        unsafe {
            release_ao2(app_capability.cast());
            release_ao2(advanced_capability.cast());
        }
        return URP_AST_ASTERISK_FAILURE;
    }
    // SAFETY: callbacks and strings have process lifetime.
    let app = unsafe { technology(c"RadioPlus", APP_RPT_DESCRIPTION, app_capability) };
    // SAFETY: callbacks and strings have process lifetime.
    let advanced = unsafe {
        technology(
            c"RadioPlusAdvanced",
            ADVANCED_DESCRIPTION,
            advanced_capability,
        )
    };
    let app_pointer = Box::into_raw(app);
    let advanced_pointer = Box::into_raw(advanced);
    *installed = Some(HostState {
        descriptor: descriptor as usize,
        driver: driver as usize,
        module: module as usize,
        app_technology: app_pointer as usize,
        advanced_technology: advanced_pointer as usize,
        app_format: app_format as usize,
        advanced_format: advanced_format as usize,
    });
    // SAFETY: both boxed technologies and their capabilities remain installed.
    if unsafe { ffi::ast_channel_register(app_pointer) } != 0 {
        // SAFETY: registration published neither technology; installed owns both.
        unsafe {
            discard_host(
                installed.take().expect("installed host missing"),
                false,
                false,
            )
        };
        return URP_AST_ASTERISK_FAILURE;
    }
    // SAFETY: the boxed technology and its capability remain installed.
    if unsafe { ffi::ast_channel_register(advanced_pointer) } != 0 {
        // SAFETY: only app technology was registered; discard unregisters it first.
        unsafe {
            discard_host(
                installed.take().expect("installed host missing"),
                true,
                false,
            )
        };
        return URP_AST_ASTERISK_FAILURE;
    }
    URP_AST_OK
}

unsafe fn discard_host(state: HostState, app_registered: bool, advanced_registered: bool) {
    let app = state.app_technology as *mut ffi::ast_channel_tech;
    let advanced = state.advanced_technology as *mut ffi::ast_channel_tech;
    if advanced_registered {
        // SAFETY: advanced is currently registered.
        unsafe { ffi::ast_channel_unregister(advanced) };
    }
    if app_registered {
        // SAFETY: app is currently registered.
        unsafe { ffi::ast_channel_unregister(app) };
    }
    // SAFETY: capabilities each own one reference and technologies are no
    // longer visible to Asterisk.
    unsafe {
        release_ao2((*app).capabilities.cast());
        release_ao2((*advanced).capabilities.cast());
        drop(Box::from_raw(app));
        drop(Box::from_raw(advanced));
    }
}

/// Unregister the Rust-owned channel technologies.
///
/// Returns busy while an Asterisk channel is still hosted.
pub(super) fn usbradioplus_asterisk_channel_host_unregister() -> c_int {
    let Ok(_membership) = lock_idle_membership(live_channels(), &ACTIVE_CHANNELS) else {
        return URP_AST_CHANNEL_BUSY;
    };
    let mut installed = host().lock().expect("channel host lock poisoned");
    let Some(state) = installed.take() else {
        return URP_AST_OK;
    };
    // SAFETY: both technologies were registered together and no channel uses
    // them after the active count reached zero.
    unsafe { discard_host(state, true, true) };
    URP_AST_OK
}

fn snapshot(contract: TechnologyContract) -> Option<HostSnapshot> {
    let installed = host().lock().expect("channel host lock poisoned");
    let state = installed.as_ref()?;
    let advanced = contract.transport == URP_AST_TRANSPORT_RPT_ADVANCED;
    Some(HostSnapshot {
        descriptor: state.descriptor as *const UrpAstDescriptor,
        driver: state.driver as *mut c_void,
        module: state.module as *mut ffi::ast_module,
        technology: if advanced {
            state.advanced_technology
        } else {
            state.app_technology
        } as *mut ffi::ast_channel_tech,
        format: if advanced {
            state.advanced_format
        } else {
            state.app_format
        } as *mut ffi::ast_format,
    })
}

unsafe fn abandon(channel: *mut Channel) {
    // SAFETY: caller owns the sole Box reference during construction failure.
    let channel = unsafe { Box::from_raw(channel) };
    channel.stop_worker();
    if !channel.rust_channel().is_null() {
        let _ = channel.control_status(ControlOperation::Stop);
        let _ = channel.control_status(ControlOperation::Destroy);
        channel
            .rust_channel
            .store(ptr::null_mut(), Ordering::Release);
    }
    // SAFETY: request constructs Channel only after acquiring a taskprocessor.
    unsafe { ffi::ast_taskprocessor_unreference(channel.control) };
    if !channel.dsp.is_null() {
        // SAFETY: this channel owns the DSP.
        unsafe { ffi::ast_dsp_free(channel.dsp) };
    }
}

unsafe extern "C" fn request(
    type_: *const c_char,
    capabilities: *mut ffi::ast_format_cap,
    assigned_ids: *const ffi::ast_assigned_ids,
    requestor: *const ffi::ast_channel,
    data: *const c_char,
    cause: *mut c_int,
) -> *mut ffi::ast_channel {
    if type_.is_null() || capabilities.is_null() || data.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: Asterisk supplies valid NUL-terminated technology and address strings.
    let Ok(type_name) = unsafe { CStr::from_ptr(type_) }.to_str() else {
        return ptr::null_mut();
    };
    let Some(contract) = technology_contract(type_name) else {
        return ptr::null_mut();
    };
    let mut membership = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    let Some(host) = snapshot(contract) else {
        return ptr::null_mut();
    };
    // SAFETY: both capability sets are live for the registration lifetime.
    if unsafe { ffi::ast_format_cap_iscompatible(capabilities, (*host.technology).capabilities) }
        == 0
    {
        return ptr::null_mut();
    }
    // SAFETY: Asterisk supplies a valid NUL-terminated channel name.
    let channel_name = unsafe { CStr::from_ptr(data) }.to_bytes();
    let Some(channel_name_length) = channel_name_length(channel_name) else {
        return ptr::null_mut();
    };
    let sequence = TASKPROCESSOR_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let taskprocessor_name = std::ffi::CString::new(format!("usbradioplus/channel/{sequence}"))
        .expect("decimal taskprocessor name cannot contain NUL");
    // SAFETY: Asterisk copies the taskprocessor name.
    let control =
        unsafe { ffi::ast_taskprocessor_get(taskprocessor_name.as_ptr(), ffi::TPS_REF_DEFAULT) };
    if control.is_null() {
        return ptr::null_mut();
    }
    let dsp = if contract.transport == URP_AST_TRANSPORT_APP_RPT {
        // SAFETY: Asterisk allocates a private detector.
        let dsp = unsafe { ffi::ast_dsp_new() };
        if dsp.is_null() {
            // SAFETY: control owns one reference.
            unsafe { ffi::ast_taskprocessor_unreference(control) };
            return ptr::null_mut();
        }
        // SAFETY: dsp is exclusively owned during setup.
        unsafe {
            ffi::ast_dsp_set_features(dsp, ffi::DSP_FEATURE_DIGIT_DETECT as c_int);
            ffi::ast_dsp_set_digitmode(
                dsp,
                (ffi::DSP_DIGITMODE_DTMF
                    | ffi::DSP_DIGITMODE_MUTECONF
                    | ffi::DSP_DIGITMODE_RELAXDTMF) as c_int,
            );
        }
        dsp
    } else {
        ptr::null_mut()
    };
    let channel = Box::new(Channel {
        _name: String::from_utf8_lossy(channel_name)
            .into_owned()
            .into_boxed_str(),
        descriptor: host.descriptor,
        rust_channel: AtomicPtr::new(ptr::null_mut()),
        control,
        dsp,
        format: host.format,
        sample_rate_hz: contract.sample_rate_hz,
        frame_samples: contract.sample_rate_hz * FRAME_MILLISECONDS / 1_000,
        owner: AtomicPtr::new(ptr::null_mut()),
        worker: Mutex::new(None),
        delivery_stop: AtomicBool::new(false),
        service_failed: AtomicBool::new(false),
        jitter_pending: AtomicBool::new(false),
        pending_transmit: AtomicU64::new(PendingTransmit::NONE.raw()),
        direct: AtomicBool::new(false),
    });
    let channel = Box::into_raw(channel);
    let reserve = UrpAstChannelReserveArgs {
        struct_size: size_of::<UrpAstChannelReserveArgs>() as u32,
        abi_version: super::super::ABI_VERSION,
        channel_name: channel_name.as_ptr(),
        channel_name_length,
        transport: contract.transport,
        channel_context: channel.cast(),
    };
    let mut rust_channel = ptr::null_mut();
    // SAFETY: descriptor validity was checked at installation and reserve
    // receives a live driver plus a stable channel context.
    let result = unsafe {
        (*host.descriptor)
            .channel_reserve
            .expect("validated channel_reserve")(
            host.driver,
            &raw const reserve,
            &raw mut rust_channel,
        )
    };
    if result != URP_AST_OK {
        if !cause.is_null() && result == URP_AST_CHANNEL_BUSY {
            // SAFETY: cause is optional writable caller storage.
            unsafe { *cause = ffi::AST_CAUSE_BUSY as c_int };
        }
        log_reservation_failure(type_name, channel_name, result);
        // SAFETY: channel has not escaped to Asterisk.
        unsafe { abandon(channel) };
        return ptr::null_mut();
    }
    // SAFETY: channel remains exclusively owned until attached below.
    unsafe {
        (*channel)
            .rust_channel
            .store(rust_channel, Ordering::Release)
    };
    // SAFETY: this is the public backing function for ast_channel_alloc.
    let owner = unsafe {
        ffi::__ast_channel_alloc(
            1,
            ffi::AST_STATE_DOWN as c_int,
            ptr::null(),
            ptr::null(),
            c"".as_ptr(),
            ptr::null(),
            ptr::null(),
            assigned_ids,
            requestor,
            0,
            ptr::null_mut(),
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
            c"%s/%s".as_ptr(),
            type_,
            data,
        )
    };
    if owner.is_null() {
        // SAFETY: channel has not escaped to Asterisk.
        unsafe { abandon(channel) };
        return ptr::null_mut();
    }
    // SAFETY: owner is newly allocated and locked by this callback.
    unsafe {
        ffi::ast_channel_tech_set(owner, host.technology);
        ffi::ast_channel_internal_fd_set(owner, 0, -1);
        ffi::ast_channel_nativeformats_set(owner, (*host.technology).capabilities);
        ffi::ast_channel_set_readformat(owner, host.format);
        ffi::ast_channel_set_writeformat(owner, host.format);
        ffi::ast_channel_tech_pvt_set(owner, channel.cast());
        (*channel).owner.store(owner, Ordering::Release);
        ffi::__ast_module_ref(
            host.module,
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
        );
    }
    ACTIVE_CHANNELS.fetch_add(1, Ordering::AcqRel);
    membership.push(channel as usize);
    if !finish_request_jitter(
        || {
            // SAFETY: owner and channel are now mutually attached. Membership
            // remains frozen so reload cannot close control admission between
            // reservation and this initial configuration.
            unsafe { configure_jitter(&*channel) }
        },
        || {
            // SAFETY: requester callbacks must release the allocation lock.
            unsafe { unlock_owner(owner) };
        },
        || drop(membership),
        || {
            // SAFETY: owner and membership are unlocked before Asterisk
            // consumes the failed channel through its hangup callback.
            unsafe { ffi::ast_hangup(owner) };
        },
    ) {
        return ptr::null_mut();
    }
    owner
}

fn finish_request_jitter(
    configure: impl FnOnce() -> Result<(), ()>,
    unlock: impl FnOnce(),
    release_membership: impl FnOnce(),
    hangup: impl FnOnce(),
) -> bool {
    let configured = configure().is_ok();
    unlock();
    release_membership();
    if !configured {
        hangup();
    }
    configured
}

fn handoff_hangup_lock<T>(
    unpublish: impl FnOnce(),
    unlock_owner: impl FnOnce(),
    lock_membership: impl FnOnce() -> T,
    relock_owner: impl FnOnce(),
) -> T {
    unpublish();
    unlock_owner();
    let membership = lock_membership();
    relock_owner();
    membership
}

unsafe extern "C" fn call(
    owner: *mut ffi::ast_channel,
    _destination: *const c_char,
    _timeout: c_int,
) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let channel = unsafe { channel_from_owner(owner) };
    let Some(channel) = channel else { return -1 };
    // SAFETY: call owns the live reserved channel throughout synchronous setup.
    if apply_pending_jitter(&channel.jitter_pending, || unsafe {
        configure_jitter(channel)
    })
    .is_err()
    {
        return -1;
    }
    if start_media(
        channel.direct.load(Ordering::Acquire),
        || channel.control_status(ControlOperation::Start),
        || {
            channel.delivery_stop.store(false, Ordering::Release);
            channel.service_failed.store(false, Ordering::Release);
            let pointer = ptr::from_ref(channel) as usize;
            let worker = std::thread::Builder::new()
                .name("usbradioplus-delivery".into())
                .spawn(move || {
                    // SAFETY: hangup joins this worker before freeing the channel.
                    unsafe { delivery::run_worker(pointer as *const Channel) };
                })
                .map_err(|_| ())?;
            *channel.worker.lock().expect("worker lock poisoned") = Some(worker);
            Ok(())
        },
        || {
            let _ = channel.control_status(ControlOperation::Stop);
        },
    )
    .is_err()
    {
        return -1;
    }
    // SAFETY: owner is live for this callback.
    unsafe { ffi::ast_setstate(owner, ffi::AST_STATE_UP) };
    0
}

fn start_media(
    direct: bool,
    start: impl FnOnce() -> c_int,
    delivery: impl FnOnce() -> Result<(), ()>,
    stop: impl FnOnce(),
) -> Result<(), ()> {
    if start() != URP_AST_OK {
        return Err(());
    }
    if !direct && delivery().is_err() {
        stop();
        return Err(());
    }
    Ok(())
}

unsafe extern "C" fn hangup(owner: *mut ffi::ast_channel) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let pointer = unsafe { ffi::ast_channel_tech_pvt(owner) }.cast::<Channel>();
    if pointer.is_null() {
        return 0;
    }
    // SAFETY: tech_pvt keeps the pinned wrapper live through this callback.
    let channel = unsafe { &*pointer };
    let mut membership = handoff_hangup_lock(
        || channel.owner.store(ptr::null_mut(), Ordering::Release),
        || {
            // SAFETY: Asterisk invokes hangup with the owner locked.
            unsafe { unlock_owner(owner) };
        },
        || {
            live_channels()
                .lock()
                .expect("live channel membership lock poisoned")
        },
        || {
            // SAFETY: membership now excludes reload and CLI users before the
            // Asterisk owner is reacquired for the remainder of hangup.
            unsafe { relock_owner(owner) };
        },
    );
    let position = membership
        .iter()
        .position(|address| *address == pointer as usize)
        .expect("live channel missing from membership");
    membership.swap_remove(position);
    // SAFETY: hangup has exclusive final ownership of the wrapper after
    // removing it from frozen membership.
    let channel = unsafe { Box::from_raw(pointer) };
    channel.stop_worker();
    let result = channel.control_status(ControlOperation::Stop);
    let _ = channel.control_status(ControlOperation::Destroy);
    channel
        .rust_channel
        .store(ptr::null_mut(), Ordering::Release);
    // SAFETY: owner is live and its private pointer no longer references channel.
    unsafe { ffi::ast_channel_tech_pvt_set(owner, ptr::null_mut()) };
    // SAFETY: channel owns these Asterisk resources.
    unsafe {
        ffi::ast_taskprocessor_unreference(channel.control);
        if !channel.dsp.is_null() {
            ffi::ast_dsp_free(channel.dsp);
        }
    }
    let module = host()
        .lock()
        .expect("channel host lock poisoned")
        .as_ref()
        .expect("active channel host missing")
        .module as *mut ffi::ast_module;
    // SAFETY: request took one nonnull module reference; unregister cannot
    // remove the host while membership contains an active channel.
    unsafe { ffi::__ast_module_unref(module, SOURCE_FILE.as_ptr(), 0, SOURCE_FUNCTION.as_ptr()) };
    ACTIVE_CHANNELS.fetch_sub(1, Ordering::AcqRel);
    drop(membership);
    if result == URP_AST_OK { 0 } else { -1 }
}

unsafe extern "C" fn answer(owner: *mut ffi::ast_channel) -> c_int {
    // SAFETY: owner is live for this callback.
    unsafe { ffi::ast_setstate(owner, ffi::AST_STATE_UP) };
    0
}

unsafe extern "C" fn read(owner: *mut ffi::ast_channel) -> *mut ffi::ast_frame {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let Some(channel) = (unsafe { channel_from_owner(owner) }) else {
        return ptr::null_mut();
    };
    if channel.service_failed.load(Ordering::Acquire) {
        ptr::null_mut()
    } else {
        // SAFETY: ast_null_frame has process lifetime.
        &raw mut ffi::ast_null_frame
    }
}

unsafe extern "C" fn write(owner: *mut ffi::ast_channel, frame: *mut ffi::ast_frame) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let Some(channel) = (unsafe { channel_from_owner(owner) }) else {
        return -1;
    };
    if frame.is_null() {
        // app_rpt uses a null frame as an accepted control-path no-op.
        // Keep the channel-driver contract used by chan_usbradio and
        // chan_simpleusb: only a failed voice write is an error.
        return 0;
    }
    // SAFETY: Asterisk supplies a readable frame for this callback.
    let frame = unsafe { &*frame };
    // SAFETY: data.ptr is the active frame union member for PCM voice.
    let data = unsafe { frame.data.ptr };
    if !voice_frame_is_valid(
        frame.frametype,
        frame.datalen,
        frame.samples,
        data.is_null(),
        channel.frame_samples,
    ) {
        // Asterisk may send non-voice and advisory-metadata frames while it
        // is pacing/controlling the radio.  They carry no PCM for this driver.
        return 0;
    }
    // SAFETY: descriptor was validated and frame holds exactly frame_samples i16 values.
    let result = unsafe {
        (*channel.descriptor)
            .channel_write_voice
            .expect("validated channel_write_voice")(
            channel.rust_channel(),
            data.cast::<i16>(),
            channel.frame_samples,
        )
    };
    if result == URP_AST_OK { 0 } else { -1 }
}

unsafe extern "C" fn send_text(owner: *mut ffi::ast_channel, text: *const c_char) -> c_int {
    // SAFETY: Asterisk owns owner and text is NUL terminated when non-null.
    let Some(channel) = (unsafe { channel_from_owner(owner) }) else {
        return -1;
    };
    if text.is_null() {
        return -1;
    }
    // SAFETY: checked non-null and supplied by Asterisk.
    let text = unsafe { CStr::from_ptr(text) }.to_bytes();
    if text.len() > u32::MAX as usize {
        return -1;
    }
    if channel.control_status(ControlOperation::Text(text.to_vec())) == URP_AST_OK {
        0
    } else {
        -1
    }
}

unsafe extern "C" fn indicate(
    owner: *mut ffi::ast_channel,
    condition: c_int,
    data: *const c_void,
    data_length: usize,
) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let Some(channel) = (unsafe { channel_from_owner(owner) }) else {
        return -1;
    };
    match condition as u32 {
        ffi::AST_CONTROL_BUSY
        | ffi::AST_CONTROL_CONGESTION
        | ffi::AST_CONTROL_RINGING
        | ffi::AST_CONTROL_VIDUPDATE => 0,
        ffi::AST_CONTROL_HOLD => {
            // SAFETY: Asterisk defines data as an optional NUL-terminated class.
            unsafe { ffi::ast_moh_start(owner, data.cast::<c_char>(), c"default".as_ptr()) }
        }
        ffi::AST_CONTROL_UNHOLD | ffi::AST_CONTROL_PROCEEDING | ffi::AST_CONTROL_PROGRESS => {
            // SAFETY: owner is live for this callback.
            unsafe { ffi::ast_moh_stop(owner) };
            0
        }
        ffi::AST_CONTROL_RADIO_KEY => {
            let tone = if data.is_null() || data_length == 0 {
                Ok(0)
            } else if data_length >= 32 {
                Err(())
            } else {
                // SAFETY: Asterisk promises data_length readable payload bytes.
                forced_ctcss_tenths_hz(unsafe {
                    std::slice::from_raw_parts(data.cast::<u8>(), data_length)
                })
            };
            match tone {
                Ok(tone) => transmit(channel, true, tone),
                Err(()) => -1,
            }
        }
        ffi::AST_CONTROL_RADIO_UNKEY => transmit(channel, false, 0),
        _ => -1,
    }
}

fn transmit(channel: &Channel, keyed: bool, ctcss_tenths_hz: u32) -> c_int {
    let pending = PendingTransmit::new(keyed, ctcss_tenths_hz);
    if channel.pending_transmit.load(Ordering::Acquire) != PendingTransmit::NONE.raw() {
        channel
            .pending_transmit
            .store(pending.raw(), Ordering::Release);
        return 0;
    }
    let attempt = match control_gate().try_read() {
        Ok(gate) => {
            let status = channel.control_status_admitted(ControlOperation::Transmit {
                keyed,
                ctcss_tenths_hz,
            });
            drop(gate);
            TransmitAttempt::Completed(status)
        }
        Err(TryLockError::WouldBlock) => TransmitAttempt::Deferred,
        Err(TryLockError::Poisoned(_)) => TransmitAttempt::Failed,
    };
    finish_transmit_attempt(&channel.pending_transmit, pending, attempt)
}

unsafe extern "C" fn fixup(
    _old_owner: *mut ffi::ast_channel,
    new_owner: *mut ffi::ast_channel,
) -> c_int {
    // SAFETY: Asterisk has already transferred technology-private state.
    let Some(channel) = (unsafe { channel_from_owner(new_owner) }) else {
        return -1;
    };
    channel.owner.store(new_owner, Ordering::Release);
    0
}

unsafe extern "C" fn setoption(
    owner: *mut ffi::ast_channel,
    option: c_int,
    data: *mut c_void,
    data_length: c_int,
) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let Some(channel) = (unsafe { channel_from_owner(owner) }) else {
        // SAFETY: errno is thread-local on supported Linux targets.
        unsafe { *libc::__errno_location() = libc::EINVAL };
        return -1;
    };
    if data.is_null() || data_length < 1 {
        // SAFETY: errno is thread-local on supported Linux targets.
        unsafe { *libc::__errno_location() = libc::EINVAL };
        return -1;
    }
    if option == super::super::URP_AST_OPTION_LINK_ATTACH {
        if data_length as usize != size_of::<super::super::UrpAstLinkAttach>() {
            return -1;
        }
        // SAFETY: the option supplies exactly one readable/writable descriptor;
        // byte-aligned callers do not have to align the pointer fields.
        let binding = unsafe {
            data.cast::<super::super::UrpAstLinkAttach>()
                .read_unaligned()
        };
        if binding.struct_size as usize != size_of::<super::super::UrpAstLinkAttach>()
            || binding.abi_version != 1
            || binding.peer_channel.is_null()
        {
            return -1;
        }
        let profile = channel.name().to_owned();
        // Asterisk calls setoption with the radio locked. The link scanner takes
        // LINK_CONTROL before visiting channels, so never wait for it while holding
        // this owner. Copy the profile first; do not use technology-private state
        // after releasing the owner. The caller retains both channel references.
        // SAFETY: release Asterisk's caller-held owner lock for graph preparation.
        unsafe { unlock_owner(owner) };
        // SAFETY: the option contract retains this peer until the call returns.
        let attached = unsafe { super::link::bind_peer(binding.peer_channel.cast(), &profile) };
        // SAFETY: restore the caller's lock on both success and preparation failure.
        unsafe { relock_owner(owner) };
        if attached.is_err() {
            return -1;
        }
        // SAFETY: complete writable descriptor was validated above.
        unsafe {
            ptr::addr_of_mut!(
                (*data.cast::<super::super::UrpAstLinkAttach>()).accepted_abi_version
            )
            .write_unaligned(1);
        }
        return 0;
    }
    if option == super::super::URP_AST_OPTION_DIRECT_CALLBACKS {
        // SAFETY: Asterisk supplies data_length readable bytes for this option.
        let callbacks = unsafe { direct_option(channel.sample_rate_hz, data, data_length) };
        let Ok(callbacks) = callbacks else {
            // SAFETY: errno is thread-local on supported Linux targets.
            unsafe { *libc::__errno_location() = libc::EINVAL };
            return -1;
        };
        if channel.control_status(ControlOperation::Direct(callbacks)) != URP_AST_OK {
            return -1;
        }
        // SAFETY: direct_option validated the complete writable option payload;
        // byte-aligned Asterisk callers need not align this trailing output field.
        unsafe {
            ptr::addr_of_mut!(
                (*data.cast::<super::super::UrpAstDirectCallbacks>()).accepted_abi_version
            )
            .write_unaligned(super::super::UrpAstDirectCallbacks::ABI_VERSION);
        }
        channel.direct.store(true, Ordering::Release);
        return 0;
    }
    if option as u32 != ffi::AST_OPTION_TONE_VERIFY {
        // SAFETY: errno is thread-local on supported Linux targets.
        unsafe { *libc::__errno_location() = libc::ENOSYS };
        return -1;
    }
    // SAFETY: data_length guarantees at least one readable byte.
    let enabled = unsafe { *data.cast::<u8>() } != 3;
    if channel.control_status(ControlOperation::Dtmf(enabled)) == URP_AST_OK {
        0
    } else {
        -1
    }
}

unsafe fn direct_option(
    sample_rate_hz: u32,
    data: *mut c_void,
    length: c_int,
) -> Result<super::super::UrpAstDirectCallbacks, ()> {
    if sample_rate_hz != ADVANCED_RATE_HZ
        || data.is_null()
        || length as usize != size_of::<super::super::UrpAstDirectCallbacks>()
    {
        return Err(());
    }
    // SAFETY: exact size is checked above; the public option supplies this readable
    // descriptor for the synchronous call, without requiring pointer alignment.
    let callbacks = unsafe {
        data.cast::<super::super::UrpAstDirectCallbacks>()
            .read_unaligned()
    };
    callbacks.is_valid().then_some(callbacks).ok_or(())
}

unsafe extern "C" fn digit_begin(_owner: *mut ffi::ast_channel, _digit: c_char) -> c_int {
    0
}

unsafe extern "C" fn digit_end(
    _owner: *mut ffi::ast_channel,
    digit: c_char,
    duration: u32,
) -> c_int {
    // SAFETY: format string and arguments match the public variadic API.
    unsafe {
        ffi::__ast_verbose(
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
            -1,
            c" << RadioPlus received digit %c of duration %u ms >>\n".as_ptr(),
            c_int::from(digit),
            duration,
        )
    };
    0
}

unsafe fn channel_from_owner<'a>(owner: *mut ffi::ast_channel) -> Option<&'a Channel> {
    if owner.is_null() {
        return None;
    }
    // SAFETY: owner is live and Asterisk owns its technology-private pointer.
    let channel = unsafe { ffi::ast_channel_tech_pvt(owner) }.cast::<Channel>();
    // SAFETY: the wrapper remains pinned until Asterisk calls hangup.
    unsafe { channel.as_ref() }
}

pub(super) unsafe fn lock_owner(channel: &Channel) -> Option<*mut ffi::ast_channel> {
    let owner = channel.owner.load(Ordering::Acquire);
    if owner.is_null() {
        return None;
    }
    // SAFETY: owner is an AO2 channel pointer while published atomically.
    if unsafe {
        ffi::__ao2_trylock(
            owner.cast(),
            ffi::AO2_LOCK_REQ_MUTEX,
            SOURCE_FILE.as_ptr(),
            SOURCE_FUNCTION.as_ptr(),
            0,
            c"owner".as_ptr(),
        )
    } != 0
    {
        return None;
    }
    if owner != channel.owner.load(Ordering::Acquire) {
        // SAFETY: this call owns the AO2 mutex.
        unsafe { unlock_owner(owner) };
        None
    } else {
        Some(owner)
    }
}

pub(super) unsafe fn unlock_owner(owner: *mut ffi::ast_channel) {
    // SAFETY: caller owns the AO2 mutex for owner.
    unsafe {
        ffi::__ao2_unlock(
            owner.cast(),
            SOURCE_FILE.as_ptr(),
            SOURCE_FUNCTION.as_ptr(),
            0,
            c"owner".as_ptr(),
        )
    };
}

unsafe fn relock_owner(owner: *mut ffi::ast_channel) {
    // SAFETY: owner remains referenced throughout its synchronous Asterisk callback.
    let _ = unsafe {
        __ao2_lock(
            owner.cast(),
            ffi::AO2_LOCK_REQ_MUTEX,
            SOURCE_FILE.as_ptr(),
            SOURCE_FUNCTION.as_ptr(),
            0,
            c"owner".as_ptr(),
        )
    };
}

unsafe extern "C" {
    fn __ao2_lock(
        object: *mut c_void,
        lock_how: ffi::ao2_lock_req,
        file: *const c_char,
        function: *const c_char,
        line: c_int,
        variable: *const c_char,
    ) -> c_int;
}

unsafe fn configure_jitter(channel: &Channel) -> Result<(), ()> {
    // SAFETY: owner remains locked through configuration.
    let owner = unsafe { lock_owner(channel) }.ok_or(())?;
    let result = match channel.control(ControlOperation::Jitter) {
        ControlResult::Jitter(status, resolved) if status == URP_AST_OK => {
            let configuration = jitter_configuration(resolved);
            // SAFETY: owner is locked and configuration is fully initialized.
            unsafe { ffi::ast_jb_configure(owner, &raw const configuration) };
            Ok(())
        }
        _ => Err(()),
    };
    // SAFETY: owner was locked above.
    unsafe { unlock_owner(owner) };
    result
}

pub(super) fn replay_transmit(channel: &Channel) -> i32 {
    let raw = channel.pending_transmit.load(Ordering::Acquire);
    let pending = PendingTransmit(raw);
    let Some((keyed, ctcss_tenths_hz)) = pending.decode() else {
        return URP_AST_OK;
    };
    let result = channel.control_status(ControlOperation::Transmit {
        keyed,
        ctcss_tenths_hz,
    });
    if result == URP_AST_OK {
        let _ = channel.pending_transmit.compare_exchange(
            raw,
            PendingTransmit::NONE.raw(),
            Ordering::AcqRel,
            Ordering::Acquire,
        );
    }
    result
}

pub(super) fn service(channel: &Channel) -> i32 {
    channel.control_status(ControlOperation::Service)
}

pub(super) fn reload_prepare(channel: &Channel) -> i32 {
    channel.control_status_admitted(ControlOperation::ReloadPrepare)
}

pub(super) fn reload_activate(channel: &Channel) -> i32 {
    channel.control_status_admitted(ControlOperation::ReloadActivate)
}

pub(super) fn reload_finish(channel: &Channel, commit: bool) -> i32 {
    channel.control_status_admitted(ControlOperation::ReloadFinish(commit))
}

pub(super) fn mark_jitter_pending(channel: &Channel) {
    channel.jitter_pending.store(true, Ordering::Release);
}

pub(super) fn set_transmit(channel: &Channel, keyed: bool, ctcss_tenths_hz: u32) -> i32 {
    channel.control_status(ControlOperation::Transmit {
        keyed,
        ctcss_tenths_hz,
    })
}

pub(super) fn set_echo(channel: &Channel, enabled: bool) -> i32 {
    channel.control_status(ControlOperation::Echo(enabled))
}

pub(super) fn run_command(channel: &Channel, command: &mut UrpAstChannelCommand) -> i32 {
    match channel.control(ControlOperation::Command(*command)) {
        ControlResult::Command(status, output) => {
            *command = output;
            status
        }
        _ => URP_AST_ASTERISK_FAILURE,
    }
}

pub(super) fn read_status(channel: &Channel, output: &mut UrpAstChannelStatus) -> i32 {
    match channel.control(ControlOperation::Status) {
        ControlResult::ChannelStatus(status, status_output) => {
            *output = status_output;
            status
        }
        _ => URP_AST_ASTERISK_FAILURE,
    }
}

pub(super) fn configure_pending_jitter(channel: &Channel) {
    // SAFETY: the caller retains channel ownership through synchronous setup.
    let _ = apply_pending_jitter(&channel.jitter_pending, || unsafe {
        configure_jitter(channel)
    });
}

#[cfg(test)]
#[path = "channel_tests.rs"]
pub(super) mod tests;
