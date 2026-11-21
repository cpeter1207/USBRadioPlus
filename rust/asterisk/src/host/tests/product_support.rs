//! Test-only dynamic product binding and shared provider fixtures.

use std::ffi::{CStr, CString, c_int, c_void};
use std::mem::size_of;
use std::path::Path;
use std::ptr;
use std::sync::{Mutex, OnceLock};

use crate::{ABI_VERSION, UrpAstDescriptor, UrpAstDirectCallbacks, UrpAstProviderManifest};

// The product tests exercise the remaining failure controls in this shared fixture.
#[allow(dead_code)]
#[path = "../../../../product/src/tests/provider_support.rs"]
pub(crate) mod provider_support;

pub(crate) static PROVIDER_TEST_LOCK: Mutex<()> = Mutex::new(());

/// Load the real product once and retain its code for every borrowed descriptor.
pub(crate) fn descriptor() -> &'static UrpAstDescriptor {
    static ADDRESS: OnceLock<usize> = OnceLock::new();
    let address = *ADDRESS.get_or_init(|| {
        let path = std::env::var_os("URP_PRODUCT_DSO").map_or_else(
            || {
                Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("../../build/libusbradioplus_product.so.1")
            },
            Into::into,
        );
        let path = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
        // SAFETY: the terminated path is retained for this synchronous call.
        let library = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
        assert!(
            !library.is_null(),
            "Build the product DSO before host tests: {path:?}"
        );
        // SAFETY: the library is open and the requested ABI entry has this exact signature.
        let symbol =
            unsafe { libc::dlsym(library, c"usbradioplus_product_descriptor_v1".as_ptr()) };
        assert!(!symbol.is_null(), "Product descriptor export is missing");
        // SAFETY: the versioned entry point exports the documented C descriptor signature.
        let entry: unsafe extern "C" fn() -> *const UrpAstDescriptor =
            unsafe { std::mem::transmute(symbol) };
        // SAFETY: the immutable descriptor and its callbacks live as long as this library.
        let pointer = unsafe { entry() };
        assert!(!pointer.is_null());
        // SAFETY: the successful versioned entry returns a complete product descriptor.
        let descriptor = unsafe { &*pointer };
        assert_eq!(
            descriptor.struct_size as usize,
            size_of::<UrpAstDescriptor>()
        );
        assert_eq!(descriptor.abi_version, ABI_VERSION);
        assert_eq!(
            // SAFETY: the descriptor contract requires a static terminated capability string.
            unsafe { CStr::from_ptr(descriptor.capability_name) },
            c"usbradioplus.product1"
        );
        // Deliberately never dlclose: host fixtures borrow these callbacks for process lifetime.
        pointer as usize
    });
    // SAFETY: initialization retained the product DSO and validated its immutable descriptor.
    unsafe { &*(address as *const UrpAstDescriptor) }
}

unsafe extern "C" fn receive_noop(
    _: *mut c_void,
    _: *const crate::UrpNativeReceiveMetadata,
    _: *mut f32,
    _: u32,
) -> c_int {
    0
}

unsafe extern "C" fn transmit_noop(
    _: *mut c_void,
    _: *mut f32,
    _: u32,
    _: *mut u32,
    _: *mut u32,
) -> c_int {
    0
}

/// Supply borrowed endpoints that never dereference their context tokens.
pub(crate) fn direct_callbacks() -> UrpAstDirectCallbacks {
    UrpAstDirectCallbacks {
        struct_size: size_of::<UrpAstDirectCallbacks>() as u32,
        abi_version: UrpAstDirectCallbacks::ABI_VERSION,
        receive_context: ptr::dangling_mut::<u8>().cast(),
        receive: Some(receive_noop),
        transmit_context: ptr::dangling_mut::<u8>().cast(),
        transmit: Some(transmit_noop),
        metadata: None,
        accepted_abi_version: 0,
    }
}

pub(crate) fn fail_station_preflight() {
    provider_support::set_failure(provider_support::FAIL_PREFLIGHT);
}
pub(crate) fn station_audio_events() -> (u32, u32) {
    provider_support::audio_lifecycle()
}
pub(crate) fn station_audio_clock(enabled: bool) {
    provider_support::periodic_audio(enabled);
}

pub(crate) fn set_start_levels(levels: [u32; 3]) {
    provider_support::restored_mixer_levels(levels);
}
pub(crate) fn start_levels() -> [u32; 3] {
    provider_support::audio_start_mixer_levels()
}
pub(crate) fn fail_audio_stop() {
    provider_support::set_failure(provider_support::FAIL_STOP);
}
pub(crate) fn station_mixer_levels() -> [u32; 3] {
    provider_support::mixer_levels()
}
pub(crate) fn fail_mixer_write() {
    provider_support::set_failure(provider_support::FAIL_MIXER);
}
pub(crate) fn mixer_discoveries() -> u32 {
    provider_support::mixer_discoveries()
}
pub(crate) fn count_station_streams(enabled: bool) {
    provider_support::exclusive_audio(enabled);
}
