//! Independent, hardware-free typed incoming-link preparation for non-radio ports.

use crate::preparation_settings::{LinkSettings, MAX_SETTINGS_BYTES, parse};
use crate::{
    Status, UrpNativeProcessingDiagnostic, ffi_status, native_processing, output_pointer,
    required_utf8,
};
use std::ffi::{c_char, c_int, c_void};
use usbradioplus_core::ProcessingChain;

/// Immutable opaque owner returned by the independent link preparation capability.
pub struct UrpPreparedLinkRole(Option<ProcessingChain>);

/// Complete process-lifetime capability for resolving a peer role without a station.
#[repr(C)]
pub struct UrpLinkPreparationDescriptor {
    /// Size of this complete table in bytes.
    pub struct_size: u32,
    /// One.
    pub abi_version: u32,
    /// NUL-terminated exact capability identity.
    pub capability_name: *const c_char,
    /// Copy and validate a role DTO without opening any device or creating a graph.
    pub prepare: unsafe extern "C" fn(
        *const u8,
        u32,
        Option<UrpNativeProcessingDiagnostic>,
        *mut c_void,
        *mut *mut UrpPreparedLinkRole,
    ) -> c_int,
    /// Compare role presence and resolved settings.
    pub equal: unsafe extern "C" fn(
        *const UrpPreparedLinkRole,
        *const UrpPreparedLinkRole,
        *mut u32,
    ) -> c_int,
    /// Create a product1 peer using the negotiated rate and frame limit.
    pub link_prepare: unsafe extern "C" fn(
        *const UrpPreparedLinkRole,
        *const c_void,
        u32,
        u32,
        *mut *mut c_void,
    ) -> c_int,
    /// Drop one owned role. A null pointer is accepted.
    pub destroy: unsafe extern "C" fn(*mut UrpPreparedLinkRole),
}

// SAFETY: all entries point to process-lifetime immutable code or static data.
unsafe impl Sync for UrpLinkPreparationDescriptor {}

/// Return the independent version-one hardware-free link preparation descriptor.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_link_preparation_descriptor() -> *const UrpLinkPreparationDescriptor
{
    static DESCRIPTOR: UrpLinkPreparationDescriptor = UrpLinkPreparationDescriptor {
        struct_size: size_of::<UrpLinkPreparationDescriptor>() as u32,
        abi_version: 1,
        capability_name: c"usbradioplus.link-preparation1".as_ptr(),
        prepare,
        equal,
        link_prepare,
        destroy,
    };
    &DESCRIPTOR
}

unsafe extern "C" fn prepare(
    settings: *const u8,
    length: u32,
    diagnostic: Option<UrpNativeProcessingDiagnostic>,
    context: *mut c_void,
    out: *mut *mut UrpPreparedLinkRole,
) -> c_int {
    ffi_status(|| {
        if !out.is_aligned() {
            return Err(Status::InvalidArgument);
        }
        let output = output_pointer(out.cast())?;
        let report = |field: &'static str| {
            if let Some(diagnostic) = diagnostic {
                // SAFETY: the caller retains the callback and its context through this synchronous call.
                unsafe { diagnostic(context, field.as_ptr(), field.len() as u32) };
            }
            Status::InvalidConfiguration
        };
        if !(1..=MAX_SETTINGS_BYTES).contains(&length) {
            return Err(report("settings"));
        }
        // SAFETY: the caller supplies a readable bounded span throughout preparation.
        let text = unsafe { required_utf8(settings, length) }.map_err(|_| report("settings"))?;
        let chain = parse::<LinkSettings>(text)
            .and_then(LinkSettings::resolve)
            .map_err(report)?;
        let owned = Box::new(UrpPreparedLinkRole(chain));
        // SAFETY: the validated writable output receives the uniquely owned role.
        unsafe { output.as_ptr().write(Box::into_raw(owned).cast()) };
        Ok(())
    })
}

unsafe fn role<'a>(owner: *const UrpPreparedLinkRole) -> Result<&'a UrpPreparedLinkRole, Status> {
    if !owner.is_aligned() {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: caller retains a live immutable handle returned by this capability.
    unsafe { owner.as_ref() }.ok_or(Status::InvalidArgument)
}

unsafe extern "C" fn equal(
    a: *const UrpPreparedLinkRole,
    b: *const UrpPreparedLinkRole,
    out: *mut u32,
) -> c_int {
    ffi_status(|| {
        if out.is_null() || !out.is_aligned() {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: both role owners and the writable output live through the call.
        unsafe { out.write(u32::from(role(a)?.0 == role(b)?.0)) };
        Ok(())
    })
}

unsafe extern "C" fn link_prepare(
    owner: *const UrpPreparedLinkRole,
    ffmpeg: *const c_void,
    rate: u32,
    frames: u32,
    out: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        if !out.is_aligned() {
            return Err(Status::InvalidArgument);
        }
        let output = output_pointer(out)?;
        // SAFETY: owner survives the call and the provider survives the resulting peer.
        unsafe {
            native_processing::prepare_link(role(owner)?.0.clone(), ffmpeg, rate, frames, output)
        }
    })
}

unsafe extern "C" fn destroy(owner: *mut UrpPreparedLinkRole) {
    if !owner.is_null() {
        // SAFETY: the caller transfers its uniquely owned role back exactly once.
        unsafe { drop(Box::from_raw(owner)) };
    }
}
