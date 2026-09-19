//! Concrete loader boundary and provider-backed lifecycle transactions.

use super::*;
use crate::host::support::{Fixture, with_state};
use crate::tests::{PROVIDER_TEST_LOCK, provider_support};
use std::ffi::CString;

fn providers() -> LoaderProviderManifest {
    let product = provider_support::manifest();
    LoaderProviderManifest {
        struct_size: size_of::<LoaderProviderManifest>() as u32,
        abi_version: LOADER_ABI_VERSION,
        ffmpeg: product.ffmpeg,
        rnnoise: product.rnnoise,
        ring: product.ring,
        radio: product.radio,
        samplerate: product.samplerate,
        audio: product.audio,
        gpio: product.gpio,
    }
}

struct LoadedGuard;

impl Drop for LoadedGuard {
    fn drop(&mut self) {
        // Gather addresses without holding state across actual hangup callbacks.
        let channels = with_state(|state| state.channels.keys().copied().collect::<Vec<_>>());
        for channel in channels {
            // SAFETY: each address remains owned by the serialized host fixture.
            unsafe { crate::ffi::ast_hangup(channel as *mut crate::ffi::ast_channel) };
        }
        assert_eq!(loader_unload(), URP_AST_OK);
        cli::tests::set_registration_result(0);
        provider_support::clear_failure();
        // SAFETY: disarm the thread-local failure seam before another test starts.
        unsafe { crate::host::support::urp_test_fail_thread_create(0) };
    }
}

#[test]
fn exported_loader_rejects_bad_arguments_before_constructing_resources() {
    let _fixture = Fixture::new();
    let _providers = PROVIDER_TEST_LOCK.lock().unwrap();
    let _cleanup = LoadedGuard;
    cli::tests::set_registration_result(0);
    provider_support::clear_failure();
    let module = ptr::dangling_mut::<u8>().cast();
    let mut manifest = providers();
    // SAFETY: the returned descriptor has process lifetime and complete callbacks.
    let descriptor = unsafe { &*usbradioplus_asterisk_loader_descriptor() };
    assert_eq!(
        descriptor.struct_size as usize,
        size_of::<AsteriskLoaderDescriptor>()
    );
    assert_eq!(descriptor.abi_version, LOADER_ABI_VERSION);
    assert_eq!(
        // SAFETY: the capability is a process-lifetime terminated string.
        unsafe { std::ffi::CStr::from_ptr(descriptor.capability) },
        LOADER_CAPABILITY
    );
    assert_eq!(descriptor.reload.unwrap()(), URP_AST_NOT_READY);
    assert_eq!(descriptor.unload.unwrap()(), URP_AST_OK);
    // SAFETY: nonnull manifests remain readable; invalid outer pointers are rejected.
    unsafe {
        assert_eq!(
            descriptor.load.unwrap()(ptr::null(), module),
            LOADER_DECLINE
        );
        assert_eq!(
            descriptor.load.unwrap()(&manifest, ptr::null_mut()),
            LOADER_DECLINE
        );
        manifest.abi_version = 0;
        assert_eq!(descriptor.load.unwrap()(&manifest, module), LOADER_DECLINE);
    }
    with_state(|state| assert!(state.configuration_paths.is_empty()));
}

#[test]
fn loader_failures_release_every_completed_external_stage() {
    for case in 0..6 {
        let _fixture = Fixture::new();
        let _providers = PROVIDER_TEST_LOCK.lock().unwrap();
        let _cleanup = LoadedGuard;
        cli::tests::set_registration_result(0);
        provider_support::clear_failure();
        with_state(|state| {
            state.configuration_text = match case {
                0 => None,
                1 => Some(CString::new("not a configuration").unwrap()),
                _ => Some(c"[usb]\n".to_owned()),
            };
            if case == 5 {
                state.register_fail_at = 2;
            }
        });
        match case {
            3 => cli::tests::set_registration_result(-1),
            4 => {
                // SAFETY: only this test thread's next scanner creation fails.
                unsafe { crate::host::support::urp_test_fail_thread_create(1) };
            }
            _ => {}
        }
        let mut manifest = providers();
        if case == 2 {
            manifest.ffmpeg = provider_support::incompatible_manifest().ffmpeg;
        }
        // SAFETY: provider descriptors and module token remain live through this call.
        let result = unsafe { loader_load(&manifest, ptr::dangling_mut::<u8>().cast()) };
        assert_eq!(
            result,
            if case < 3 {
                LOADER_DECLINE
            } else {
                LOADER_FAILURE
            },
            "case {case}"
        );
        assert!(LIFECYCLE.lock().unwrap().operations.is_none());
        assert_eq!(loader_reload(), URP_AST_NOT_READY);
        with_state(|state| {
            assert!(state.capabilities.is_empty());
            assert!(state.channels.is_empty());
            assert_eq!(state.module_references, 0);
            assert_eq!(state.iterator_allocations, state.iterator_frees);
        });
    }
}

#[test]
fn real_loader_reloads_and_refuses_unload_until_live_channel_hangs_up() {
    let _fixture = Fixture::new();
    let _providers = PROVIDER_TEST_LOCK.lock().unwrap();
    let _cleanup = LoadedGuard;
    cli::tests::set_registration_result(0);
    provider_support::clear_failure();
    with_state(|state| state.configuration_text = Some(c"[usb]\n".to_owned()));
    let manifest = providers();
    let module = ptr::dangling_mut::<u8>().cast();
    // SAFETY: the test retains all provider descriptors and the module token.
    unsafe {
        assert_eq!(loader_load(&manifest, module), LOADER_OK);
        assert_eq!(loader_load(&manifest, module), LOADER_FAILURE);
    }
    assert_eq!(loader_reload(), URP_AST_OK);
    let technology = with_state(|state| state.registered[0]) as *const crate::ffi::ast_channel_tech;
    // SAFETY: successful load retains this technology and its capability.
    let owner = unsafe {
        (*technology).requester.unwrap()(
            (*technology).type_,
            (*technology).capabilities,
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        )
    };
    assert!(!owner.is_null());
    assert_eq!(loader_unload(), URP_AST_CHANNEL_BUSY);
    assert!(LIFECYCLE.lock().unwrap().operations.is_some());
    // SAFETY: the iterator takes its own retained snapshot before owner hangup.
    let iterator = unsafe { crate::ffi::ast_channel_iterator_all_new() };
    assert!(!iterator.is_null());
    // SAFETY: the fixture owner still has a readable, terminated name.
    let name = unsafe { std::ffi::CStr::from_ptr(crate::ffi::ast_channel_name(owner)) }.to_owned();
    // SAFETY: the live Asterisk owner owns its reservation until hangup.
    unsafe { crate::ffi::ast_hangup(owner) };
    with_state(|state| assert!(state.retired_channels.contains_key(&(owner as usize))));
    // SAFETY: iterator retention keeps the hung-up owner storage readable.
    unsafe {
        let retained = crate::ffi::ast_channel_iterator_next(iterator);
        assert_eq!(retained, owner);
        assert_eq!(
            std::ffi::CStr::from_ptr(crate::ffi::ast_channel_name(retained)),
            name.as_c_str()
        );
        crate::ffi::__ao2_ref(
            retained.cast(),
            -1,
            ptr::null(),
            ptr::null(),
            0,
            ptr::null(),
        );
        assert!(crate::ffi::ast_channel_iterator_next(iterator).is_null());
        crate::ffi::ast_channel_iterator_destroy(iterator);
    }
    assert_eq!(loader_unload(), URP_AST_OK);
    assert_eq!(loader_unload(), URP_AST_OK);
    assert_eq!(loader_reload(), URP_AST_NOT_READY);
    with_state(|state| {
        assert!(state.channels.is_empty());
        assert!(state.capabilities.is_empty());
        assert_eq!(state.module_references, 0);
        assert_eq!(state.iterator_allocations, state.iterator_frees);
    });
}
