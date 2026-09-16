//! Asterisk channel technologies and their pinned per-call state.

use std::ffi::{CStr, c_char, c_int, c_void};
use std::mem::{size_of, zeroed};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock, RwLock, RwLockWriteGuard, TryLockError};
use std::thread::JoinHandle;

use super::super::{
    URP_AST_ASTERISK_FAILURE, URP_AST_CHANNEL_BUSY, URP_AST_JITTER_FIXED, URP_AST_OK,
    URP_AST_TRANSPORT_APP_RPT, URP_AST_TRANSPORT_RPT_ADVANCED, UrpAstChannelCommand,
    UrpAstChannelReserveArgs, UrpAstChannelStatus, UrpAstDescriptor, UrpAstJitterConfig, ffi,
};
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
    name: Box<str>,
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
        match self.control(operation) {
            ControlResult::Status(status) => status,
            ControlResult::Jitter(status, _) => status,
            ControlResult::Command(status, _) => status,
            ControlResult::ChannelStatus(status, _) => status,
        }
    }

    fn control_status_admitted(&self, operation: ControlOperation) -> i32 {
        match self.control_admitted(operation) {
            ControlResult::Status(status) => status,
            ControlResult::Jitter(status, _) => status,
            ControlResult::Command(status, _) => status,
            ControlResult::ChannelStatus(status, _) => status,
        }
    }

    pub(super) fn name(&self) -> &str {
        &self.name
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

pub(super) fn forced_ctcss_tenths_hz(payload: &[u8]) -> Result<u32, ()> {
    if payload.is_empty() {
        return Ok(0);
    }
    let text = std::str::from_utf8(payload).map_err(|_| ())?;
    let frequency = text.parse::<f64>().map_err(|_| ())?;
    if !frequency.is_finite() || frequency < 0.0 || frequency > f64::from(u32::MAX) / 10.0 {
        return Err(());
    }
    Ok((frequency.mul_add(10.0, 0.5).floor()) as u32)
}

pub(super) fn voice_frame_is_valid(
    frame_type: ffi::ast_frame_type,
    data_length: i32,
    sample_count: i32,
    data_is_null: bool,
    expected_samples: u32,
) -> bool {
    frame_type == ffi::AST_FRAME_VOICE
        && !data_is_null
        && data_length >= 0
        && data_length % size_of::<i16>() as i32 == 0
        && u32::try_from(data_length / size_of::<i16>() as i32) == Ok(expected_samples)
        && u32::try_from(sample_count) == Ok(expected_samples)
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

/// Frozen live membership plus exclusive admission to channel control.
pub(super) struct LiveChannelsGuard {
    channels: MutexGuard<'static, Vec<usize>>,
    _control: RwLockWriteGuard<'static, ()>,
}

impl LiveChannelsGuard {
    pub(super) fn iter(&self) -> impl Iterator<Item = &Channel> {
        self.channels.iter().map(|address| {
            // SAFETY: membership is frozen, so every address remains pinned.
            unsafe { &*(*address as *const Channel) }
        })
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
        _control: control,
    }
}

fn descriptor_is_valid(descriptor: &UrpAstDescriptor) -> bool {
    descriptor.struct_size as usize >= size_of::<UrpAstDescriptor>()
        && descriptor.abi_version == super::super::ABI_VERSION
        && descriptor.channel_reserve.is_some()
        && descriptor.channel_start.is_some()
        && descriptor.channel_stop.is_some()
        && descriptor.channel_write_voice.is_some()
        && descriptor.channel_write_text.is_some()
        && descriptor.channel_set_transmit.is_some()
        && descriptor.channel_set_dtmf.is_some()
        && descriptor.channel_get_jitter_config.is_some()
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn usbradioplus_asterisk_channel_host_register(
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
    let mut installed = host().lock().expect("channel host lock poisoned");
    if installed.is_some() {
        return URP_AST_CHANNEL_BUSY;
    }
    // SAFETY: these process-lifetime formats are owned by Asterisk.
    let app_format = unsafe { ffi::ast_format_slin };
    // SAFETY: the public format cache returns a process-lifetime object.
    let advanced_format = unsafe { ffi::ast_format_cache_get_slin_by_rate(ADVANCED_RATE_HZ) };
    if app_format.is_null() || advanced_format.is_null() {
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
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_asterisk_channel_host_unregister() -> c_int {
    if ACTIVE_CHANNELS.load(Ordering::Acquire) != 0 {
        return URP_AST_CHANNEL_BUSY;
    }
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
    if !channel.control.is_null() {
        // SAFETY: this channel owns one taskprocessor reference.
        unsafe { ffi::ast_taskprocessor_unreference(channel.control) };
    }
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
    let Ok(channel_name_length) = u32::try_from(channel_name.len()) else {
        return ptr::null_mut();
    };
    let mut membership = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    let sequence = TASKPROCESSOR_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let Ok(taskprocessor_name) = std::ffi::CString::new(format!("usbradioplus/channel/{sequence}"))
    else {
        return ptr::null_mut();
    };
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
        name: String::from_utf8_lossy(channel_name)
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
        let _ = ffi::ast_channel_set_readformat(owner, host.format);
        let _ = ffi::ast_channel_set_writeformat(owner, host.format);
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
    drop(membership);
    // SAFETY: owner and channel are now mutually attached.
    if unsafe { configure_jitter(&*channel) }.is_err() {
        // SAFETY: Asterisk invokes hangup and releases the owner.
        unsafe { ffi::ast_hangup(owner) };
        return ptr::null_mut();
    }
    // SAFETY: requester callbacks return an unlocked channel.
    unsafe { unlock_owner(owner) };
    owner
}

unsafe extern "C" fn call(
    owner: *mut ffi::ast_channel,
    _destination: *const c_char,
    _timeout: c_int,
) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let channel = unsafe { channel_from_owner(owner) };
    let Some(channel) = channel else { return -1 };
    if channel.jitter_pending.swap(false, Ordering::AcqRel)
        && unsafe { configure_jitter(channel) }.is_err()
    {
        return -1;
    }
    if channel.control_status(ControlOperation::Start) != URP_AST_OK {
        return -1;
    }
    channel.delivery_stop.store(false, Ordering::Release);
    channel.service_failed.store(false, Ordering::Release);
    let pointer = ptr::from_ref(channel) as usize;
    let worker = std::thread::Builder::new()
        .name("usbradioplus-delivery".into())
        .spawn(move || {
            // SAFETY: hangup joins this worker before freeing the channel.
            unsafe { delivery::run_worker(pointer as *const Channel) };
        });
    let Ok(worker) = worker else {
        let _ = channel.control_status(ControlOperation::Stop);
        return -1;
    };
    *channel.worker.lock().expect("worker lock poisoned") = Some(worker);
    // SAFETY: owner is live for this callback.
    unsafe { ffi::ast_setstate(owner, ffi::AST_STATE_UP) };
    0
}

unsafe extern "C" fn hangup(owner: *mut ffi::ast_channel) -> c_int {
    // SAFETY: Asterisk owns owner and its technology-private pointer.
    let pointer = unsafe { ffi::ast_channel_tech_pvt(owner) }.cast::<Channel>();
    if pointer.is_null() {
        return 0;
    }
    let mut membership = live_channels()
        .lock()
        .expect("live channel membership lock poisoned");
    if let Some(position) = membership
        .iter()
        .position(|address| *address == pointer as usize)
    {
        membership.swap_remove(position);
    }
    // SAFETY: hangup has exclusive final ownership of the wrapper after
    // removing it from frozen membership.
    let channel = unsafe { Box::from_raw(pointer) };
    channel.owner.store(ptr::null_mut(), Ordering::Release);
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
        .map(|state| state.module as *mut ffi::ast_module)
        .unwrap_or(ptr::null_mut());
    if !module.is_null() {
        // SAFETY: request took one module reference for this channel.
        unsafe {
            ffi::__ast_module_unref(module, SOURCE_FILE.as_ptr(), 0, SOURCE_FUNCTION.as_ptr())
        };
    }
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
        return -1;
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
        return -1;
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
    let result = channel.control_status(ControlOperation::Transmit {
        keyed,
        ctcss_tenths_hz,
    });
    if result == URP_AST_CHANNEL_BUSY {
        channel
            .pending_transmit
            .store(pending.raw(), Ordering::Release);
        0
    } else if result == URP_AST_OK {
        0
    } else {
        -1
    }
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
    if option as u32 != ffi::AST_OPTION_TONE_VERIFY {
        return 0;
    }
    // SAFETY: data_length guarantees at least one readable byte.
    let enabled = unsafe { *data.cast::<u8>() } != 3;
    if channel.control_status(ControlOperation::Dtmf(enabled)) == URP_AST_OK {
        0
    } else {
        -1
    }
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
    if channel.jitter_pending.load(Ordering::Acquire)
        && unsafe { configure_jitter(channel) }.is_ok()
    {
        channel.jitter_pending.store(false, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ABI_VERSION, URP_AST_JITTER_ADAPTIVE};

    #[test]
    fn technologies_keep_existing_rates_and_transports() {
        assert_eq!(
            technology_contract("radioplus"),
            Some(TechnologyContract {
                transport: URP_AST_TRANSPORT_APP_RPT,
                sample_rate_hz: 8_000,
            })
        );
        assert_eq!(
            technology_contract("RADIOPLUSADVANCED"),
            Some(TechnologyContract {
                transport: URP_AST_TRANSPORT_RPT_ADVANCED,
                sample_rate_hz: 48_000,
            })
        );
        assert_eq!(technology_contract("unknown"), None);
    }

    #[test]
    fn forced_ctcss_matches_decimal_boundary_contract() {
        assert_eq!(forced_ctcss_tenths_hz(b""), Ok(0));
        assert_eq!(forced_ctcss_tenths_hz(b"100.04"), Ok(1_000));
        assert_eq!(forced_ctcss_tenths_hz(b"100.05"), Ok(1_001));
        assert_eq!(forced_ctcss_tenths_hz(b"-1"), Err(()));
        assert_eq!(forced_ctcss_tenths_hz(b"nan"), Err(()));
        assert_eq!(forced_ctcss_tenths_hz(b"not-a-tone"), Err(()));
        assert_eq!(forced_ctcss_tenths_hz(b"429496729.6"), Err(()));
    }

    #[test]
    fn voice_frames_must_match_the_fixed_technology_contract() {
        assert!(voice_frame_is_valid(
            ffi::AST_FRAME_VOICE,
            320,
            160,
            false,
            160,
        ));
        assert!(!voice_frame_is_valid(
            ffi::AST_FRAME_TEXT,
            320,
            160,
            false,
            160,
        ));
        assert!(!voice_frame_is_valid(
            ffi::AST_FRAME_VOICE,
            319,
            160,
            false,
            160,
        ));
        assert!(!voice_frame_is_valid(
            ffi::AST_FRAME_VOICE,
            320,
            159,
            false,
            160,
        ));
        assert!(!voice_frame_is_valid(
            ffi::AST_FRAME_VOICE,
            320,
            160,
            true,
            160,
        ));
    }

    #[test]
    fn pending_transmit_keeps_the_latest_complete_intent() {
        let keyed = PendingTransmit::new(true, 1_230);
        assert_eq!(keyed.decode(), Some((true, 1_230)));
        let unkeyed = PendingTransmit::new(false, 0);
        assert_eq!(unkeyed.decode(), Some((false, 0)));
        assert_eq!(PendingTransmit::NONE.decode(), None);
    }

    #[test]
    fn jitter_mapping_preserves_every_asterisk_setting() {
        let resolved = UrpAstJitterConfig {
            struct_size: size_of::<UrpAstJitterConfig>() as u32,
            abi_version: ABI_VERSION,
            enabled: 1,
            maximum_size_ms: 500,
            resync_threshold_ms: 1_000,
            implementation: URP_AST_JITTER_FIXED,
            logging_enabled: 1,
            force_enabled: 1,
            target_extra_ms: 40,
            video_sync_enabled: 1,
        };
        let mapped = jitter_configuration(resolved);
        assert_eq!(
            mapped.flags,
            ffi::AST_JB_ENABLED | ffi::AST_JB_FORCED | ffi::AST_JB_LOG | ffi::AST_JB_SYNC_VIDEO
        );
        assert_eq!(mapped.max_size, 500);
        assert_eq!(mapped.resync_threshold, 1_000);
        assert_eq!(mapped.target_extra, 40);
        assert_eq!(
            mapped
                .impl_
                .iter()
                .map(|byte| *byte as u8)
                .take_while(|byte| *byte != 0)
                .collect::<Vec<_>>(),
            b"fixed"
        );

        let mapped = jitter_configuration(UrpAstJitterConfig {
            implementation: URP_AST_JITTER_ADAPTIVE,
            enabled: 2,
            force_enabled: 2,
            logging_enabled: 2,
            video_sync_enabled: 2,
            ..resolved
        });
        assert_eq!(
            mapped.flags,
            ffi::AST_JB_ENABLED | ffi::AST_JB_FORCED | ffi::AST_JB_LOG | ffi::AST_JB_SYNC_VIDEO
        );
        assert_eq!(
            mapped
                .impl_
                .iter()
                .map(|byte| *byte as u8)
                .take_while(|byte| *byte != 0)
                .collect::<Vec<_>>(),
            b"adaptive"
        );
    }
}
