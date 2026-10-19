//! Control-only site I/O through the station's existing hardware owner.

/// Replace ownership on the existing hardware worker; acceptance only means queued.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UrpAstSiteBinding {
    /// Complete structure size.
    pub struct_size: u32,
    /// Product record ABI, one.
    pub abi_version: u32,
    /// Expected current owner generation.
    pub generation: u64,
    /// Spare pins to claim; reserved pins are rejected.
    pub output_mask: u32,
    /// Physical high levels for newly claimed pins.
    pub initial_high_mask: u32,
    /// Physical high levels when pins are released.
    pub safe_high_mask: u32,
}

/// Steady output intent; pulse timing belongs to the site controller.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UrpAstSiteRequest {
    /// Complete structure size.
    pub struct_size: u32,
    /// Product record ABI, one.
    pub abi_version: u32,
    /// Acknowledged owner generation.
    pub generation: u64,
    /// Owned pins to change.
    pub output_mask: u32,
    /// Physical high levels within output_mask.
    pub high_mask: u32,
}

/// Cached physical-owner observation; querying never performs device I/O.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UrpAstSiteSnapshot {
    /// Complete structure size.
    pub struct_size: u32,
    /// Product record ABI, one.
    pub abi_version: u32,
    /// Current owner generation; zero means retired.
    pub generation: u64,
    /// Available configured physical input pins.
    pub input_mask: u32,
    /// Available configured physical output pins.
    pub output_mask: u32,
    /// Pins reserved by radio signaling or existing controls.
    pub reserved_mask: u32,
    /// Pins owned by this site configuration.
    pub claimed_mask: u32,
    /// Latest persistent output intent.
    pub requested_high_mask: u32,
    /// Last output levels submitted to the provider.
    pub submitted_high_mask: u32,
    /// Owner generation of that submission.
    pub submitted_generation: u64,
    /// Generation whose submission completed service.
    pub serviced_generation: u64,
    /// Output levels at that successful service, not electrical readback.
    pub serviced_high_mask: u32,
    /// Pins with independent output readback; zero for these providers.
    pub readback_valid_mask: u32,
    /// Last successfully acquired physical input levels.
    pub input_high_mask: u32,
    /// Input acquisition quality: zero unavailable, one valid, two fault.
    pub input_quality: u32,
    /// Last successful acquisition on the hardware owner's monotonic clock.
    pub observed_at_ms: u64,
    /// One if observed_at_ms is valid; zero otherwise.
    pub observed_at_valid: u32,
    /// Current time on the same clock; subtract observation time for age.
    pub snapshot_at_ms: u64,
    /// Commands rejected by the hardware owner.
    pub rejected_commands: u64,
    /// Zero none, one unavailable, two stale, three mask, four busy, five fault.
    pub last_command_error: u32,
}

use crate::{ABI_VERSION, Status, channel_control, copy_abi, ffi_status, native, write_abi};
use std::ffi::{c_int, c_void};
use std::mem::size_of;
use usbradioplus_driver::{
    HardwareService, HardwareStationError, SiteBank, SiteDigitalBinding, SiteDigitalRequest,
    SiteDigitalSnapshot, SiteError, SiteReadQuality,
};

fn bank(value: u32) -> Result<SiteBank, Status> {
    match value {
        0 => Ok(SiteBank::Cm119),
        1 => Ok(SiteBank::Parallel),
        _ => Err(Status::InvalidArgument),
    }
}

fn error(error: HardwareStationError) -> Status {
    match error {
        HardwareStationError::Site(SiteError::InvalidMask) => Status::InvalidArgument,
        HardwareStationError::Site(SiteError::Busy) | HardwareStationError::ControlQueueFull => {
            Status::ChannelBusy
        }
        HardwareStationError::Site(SiteError::Unavailable | SiteError::StaleGeneration)
        | HardwareStationError::NotRunning => Status::NotReady,
        _ => Status::SetupFailed,
    }
}

unsafe fn service<'a>(handle: *mut c_void, is_native: bool) -> Result<&'a HardwareService, Status> {
    if is_native {
        // SAFETY: the caller serializes control on a live native handle.
        unsafe { native::site_service(handle) }
    } else {
        // SAFETY: the Asterisk taskprocessor owns this channel control endpoint.
        let (_, control) = unsafe { channel_control(handle)? };
        control
            .hardware
            .as_ref()
            .map(|station| station.site_service())
            .ok_or(Status::NotReady)
    }
}

unsafe fn bind(
    handle: *mut c_void,
    is_native: bool,
    bank_id: u32,
    raw: *const UrpAstSiteBinding,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the C caller supplies a readable size/version-prefixed record.
        let raw = unsafe { copy_abi(raw)? };
        // SAFETY: the host retains its exclusive control ownership through this call.
        unsafe { service(handle, is_native)? }
            .site_bind(
                bank(bank_id)?,
                SiteDigitalBinding {
                    generation: raw.generation,
                    output_mask: raw.output_mask,
                    initial_high_mask: raw.initial_high_mask,
                    safe_high_mask: raw.safe_high_mask,
                },
            )
            .map_err(error)
    })
}

unsafe fn publish(
    handle: *mut c_void,
    is_native: bool,
    bank_id: u32,
    raw: *const UrpAstSiteRequest,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the C caller supplies a readable size/version-prefixed record.
        let raw = unsafe { copy_abi(raw)? };
        // SAFETY: the host retains its exclusive control ownership through this call.
        unsafe { service(handle, is_native)? }
            .site_publish(
                bank(bank_id)?,
                SiteDigitalRequest {
                    generation: raw.generation,
                    output_mask: raw.output_mask,
                    high_mask: raw.high_mask,
                },
            )
            .map_err(error)
    })
}

unsafe fn snapshot(
    handle: *mut c_void,
    is_native: bool,
    bank_id: u32,
    raw: *mut UrpAstSiteSnapshot,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the host retains its exclusive control ownership through this call.
        let value = unsafe { service(handle, is_native)? }
            .site_snapshot(bank(bank_id)?)
            .map_err(error)?;
        // SAFETY: the caller supplies initialized, writable size/version-prefixed storage.
        unsafe { write_abi(raw, value.into()) }
    })
}

impl From<SiteDigitalSnapshot> for UrpAstSiteSnapshot {
    fn from(value: SiteDigitalSnapshot) -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
            generation: value.generation,
            input_mask: value.input_mask,
            output_mask: value.output_mask,
            reserved_mask: value.reserved_mask,
            claimed_mask: value.claimed_mask,
            requested_high_mask: value.requested_high_mask,
            submitted_high_mask: value.submitted_high_mask,
            submitted_generation: value.submitted_generation,
            serviced_generation: value.serviced_generation,
            serviced_high_mask: value.serviced_high_mask,
            readback_valid_mask: value.readback_valid_mask,
            input_high_mask: value.input_high_mask,
            input_quality: match value.input_quality {
                SiteReadQuality::Unavailable => 0,
                SiteReadQuality::Valid => 1,
                SiteReadQuality::Fault => 2,
            },
            observed_at_ms: value.observed_at_ms.unwrap_or(0),
            observed_at_valid: u32::from(value.observed_at_ms.is_some()),
            snapshot_at_ms: value.snapshot_at_ms,
            rejected_commands: value.rejected_commands,
            last_command_error: match value.last_command_error {
                None => 0,
                Some(SiteError::Unavailable) => 1,
                Some(SiteError::StaleGeneration) => 2,
                Some(SiteError::InvalidMask) => 3,
                Some(SiteError::Busy) => 4,
                Some(SiteError::Fault) => 5,
            },
        }
    }
}

pub(crate) unsafe extern "C" fn native_bind(
    handle: *mut c_void,
    bank: u32,
    raw: *const UrpAstSiteBinding,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { bind(handle, true, bank, raw) }
}

pub(crate) unsafe extern "C" fn native_publish(
    handle: *mut c_void,
    bank: u32,
    raw: *const UrpAstSiteRequest,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { publish(handle, true, bank, raw) }
}

pub(crate) unsafe extern "C" fn native_snapshot(
    handle: *mut c_void,
    bank: u32,
    raw: *mut UrpAstSiteSnapshot,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { snapshot(handle, true, bank, raw) }
}

pub(crate) unsafe extern "C" fn channel_bind(
    handle: *mut c_void,
    bank: u32,
    raw: *const UrpAstSiteBinding,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { bind(handle, false, bank, raw) }
}

pub(crate) unsafe extern "C" fn channel_publish(
    handle: *mut c_void,
    bank: u32,
    raw: *const UrpAstSiteRequest,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { publish(handle, false, bank, raw) }
}

pub(crate) unsafe extern "C" fn channel_snapshot(
    handle: *mut c_void,
    bank: u32,
    raw: *mut UrpAstSiteSnapshot,
) -> c_int {
    // SAFETY: forwarded unchanged from the host's serialized control-only ABI call.
    unsafe { snapshot(handle, false, bank, raw) }
}
