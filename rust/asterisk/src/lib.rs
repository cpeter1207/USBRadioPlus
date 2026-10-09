//! Rust-owned USBRadioPlus host for Asterisk.
//!
//! The metadata-only C module selects released provider descriptors and
//! forwards load, reload, and unload. This crate owns Asterisk channel,
//! audiohook, CLI, configuration, media, and controller lifecycles.

#![deny(warnings)]

// These are generated declarations/bitfield accessors for external Asterisk
// headers, not owned implementation. Safety obligations are documented at our
// call sites; bindgen does not generate Clippy's per-block safety comments.
#[allow(
    warnings,
    missing_docs,
    unsafe_op_in_unsafe_fn,
    clippy::all,
    clippy::undocumented_unsafe_blocks
)]
mod ffi {
    include!(concat!(env!("OUT_DIR"), "/asterisk.rs"));
}

mod host;

use std::ffi::{CStr, c_int, c_void};
use std::mem::size_of;

// Generated C ABI declarations only; implementation lives in the product DSO.
#[allow(
    warnings,
    missing_docs,
    unsafe_op_in_unsafe_fn,
    clippy::all,
    clippy::undocumented_unsafe_blocks
)]
mod product_abi {
    include!(concat!(env!("OUT_DIR"), "/product.rs"));
}
pub use product_abi::*;

const ABI_VERSION: u32 = 1;

impl Default for UrpAstDtmfResult {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            event_kind: URP_AST_DTMF_NONE,
            digit: 0,
            reserved: [0; 7],
        }
    }
}
/// Private positive Asterisk setoption ID (ASCII RPAD), passed with block zero.
pub const URP_AST_OPTION_DIRECT_CALLBACKS: c_int = 0x5250_4144;
/// Bind a peer to the selected radio's incoming link graph, using block zero.
pub const URP_AST_OPTION_LINK_ATTACH: c_int = 0x5250_4C41;
/// Exact peer-binding payload, synchronously borrowed by the channel option.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpAstLinkAttach {
    /// Exact size of this payload in bytes.
    pub struct_size: u32,
    /// Exact peer-binding ABI, currently 1.
    pub abi_version: u32,
    /// Caller-retained Asterisk peer channel, valid until the option returns.
    pub peer_channel: *mut c_void,
    /// Initialize to zero; written to 1 only after the hook is attached.
    pub accepted_abi_version: u32,
}

impl UrpAstDirectCallbacks {
    /// Exact borrowed direct PCM endpoint ABI.
    pub const ABI_VERSION: u32 = 3;

    /// Check the complete two-endpoint attachment before retaining its contexts.
    pub fn is_valid(&self) -> bool {
        self.struct_size as usize == size_of::<Self>()
            && self.abi_version == Self::ABI_VERSION
            && !self.receive_context.is_null()
            && self.receive.is_some()
            && !self.transmit_context.is_null()
            && self.transmit.is_some()
    }
}

/// Validate the selected product before hardware preparation or function calls.
///
/// # Safety
/// The caller retains a readable, immutable descriptor and its capability string.
unsafe fn product_descriptor_is_valid(raw: *const UrpAstDescriptor) -> bool {
    if raw.is_null() {
        return false;
    }
    // SAFETY: the loader promises readable descriptor header bytes.
    let size = unsafe { raw.cast::<u32>().read() };
    if (size as usize) < size_of::<UrpAstDescriptor>() {
        return false;
    }
    // SAFETY: the header confirms the entire descriptor is present.
    let descriptor = unsafe { &*raw };
    descriptor.abi_version == ABI_VERSION
        && !descriptor.capability_name.is_null()
        // SAFETY: the loader retains this NUL-terminated capability string.
        && unsafe { CStr::from_ptr(descriptor.capability_name) } == c"usbradioplus.product1"
        && descriptor.driver_create.is_some()
        && descriptor.driver_reload.is_some()
        && descriptor.driver_reload_finish.is_some()
        && descriptor.driver_channel_name.is_some()
        && descriptor.driver_active_channel.is_some()
        && descriptor.driver_set_active_channel.is_some()
        && descriptor.driver_destroy.is_some()
        && descriptor.link_prepare.is_some()
        && descriptor.link_prepare_reload.is_some()
        && descriptor.link_process.is_some()
        && descriptor.link_observe.is_some()
        && descriptor.link_destroy.is_some()
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
        && descriptor.link_reload_unchanged.is_some()
}

#[cfg(test)]
mod product_tests;
