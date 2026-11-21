//! Concrete loader boundary and provider-backed lifecycle transactions.

use super::*;
use crate::host::support::product_support::{PROVIDER_TEST_LOCK, provider_support};
use crate::host::support::{Fixture, product_support, with_state};
use std::ffi::CString;

fn providers() -> LoaderProviderManifest {
    let product = provider_support::manifest();
    LoaderProviderManifest {
        struct_size: size_of::<LoaderProviderManifest>() as u32,
        abi_version: LOADER_ABI_VERSION,
        product: product_support::descriptor(),
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

#[test]
fn site_option_reaches_real_product_owner_and_retains_ownership_through_reload() {
    use crate::{UrpAstSiteBinding, UrpAstSiteControl};
    let _fixture = Fixture::new();
    let _providers = PROVIDER_TEST_LOCK.lock().unwrap();
    let _cleanup = LoadedGuard;
    cli::tests::set_registration_result(0);
    provider_support::clear_failure();
    with_state(|state| {
        state.configuration_text =
            Some(c"[usb]\n[hardware]\nhardware_gpio_6_mode = out0\n".to_owned())
    });
    let manifest = providers();
    assert_eq!(
        // SAFETY: the fake Asterisk host and dynamic product fixture retain all provider tables.
        unsafe { loader_load(&manifest, ptr::dangling_mut::<u8>().cast()) },
        LOADER_OK
    );
    let technology = with_state(|state| state.registered[0]) as *const crate::ffi::ast_channel_tech;
    // SAFETY: the loaded host owns this technology and the requester returns a retained owner.
    let (owner, option) = unsafe {
        let tech = &*technology;
        let owner = tech.requester.unwrap()(
            tech.type_,
            tech.capabilities,
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        );
        assert!(!owner.is_null());
        assert_eq!(tech.call.unwrap()(owner, ptr::null(), 0), 0);
        (owner, tech.setoption.unwrap())
    };
    let send = |request: &mut UrpAstSiteControl| {
        // SAFETY: the loaded channel and complete envelope outlive each synchronous option call.
        unsafe {
            option(
                owner,
                crate::URP_AST_OPTION_SITE_IO as c_int,
                ptr::from_mut(request).cast(),
                size_of::<UrpAstSiteControl>() as c_int,
            )
        }
    };
    let snapshot = || {
        let mut request = UrpAstSiteControl {
            struct_size: size_of::<UrpAstSiteControl>() as u32,
            abi_version: 1,
            operation: crate::URP_AST_SITE_SNAPSHOT,
            ..Default::default()
        };
        assert_eq!(send(&mut request), URP_AST_OK);
        assert_eq!(request.accepted_abi_version, 1);
        request.snapshot
    };
    let initial = snapshot();
    assert_eq!(initial.output_mask & 0x20, 0x20);
    let mut binding = UrpAstSiteControl {
        struct_size: size_of::<UrpAstSiteControl>() as u32,
        abi_version: 1,
        operation: crate::URP_AST_SITE_BIND,
        binding: UrpAstSiteBinding {
            struct_size: size_of::<UrpAstSiteBinding>() as u32,
            abi_version: 1,
            generation: initial.generation,
            output_mask: 0x20,
            initial_high_mask: 0x20,
            safe_high_mask: 0,
        },
        ..Default::default()
    };
    assert_eq!(send(&mut binding), URP_AST_OK);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    let bound = loop {
        let value = snapshot();
        if value.generation != initial.generation && value.serviced_generation == value.generation {
            break value;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "site binding must be serviced"
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    };
    assert_eq!(bound.serviced_high_mask & 0x20, 0x20);
    assert_eq!(bound.readback_valid_mask, 0);
    assert_eq!(loader_reload(), URP_AST_OK);
    let reloaded = snapshot();
    assert_eq!(reloaded.generation, bound.generation);
    assert_eq!(reloaded.claimed_mask, 0x20);
    assert_eq!(reloaded.serviced_high_mask & 0x20, 0x20);
}

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
fn loader_rejects_invalid_product_before_configuration_or_driver_creation() {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static CREATES: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn capture_create(
        _: *const UrpAstDriverCreateArgs,
        _: *mut *mut c_void,
    ) -> c_int {
        CREATES.fetch_add(1, Ordering::Relaxed);
        crate::URP_AST_SETUP_FAILED
    }

    let _fixture = Fixture::new();
    let _cleanup = LoadedGuard;
    with_state(|state| state.configuration_text = Some(c"[usb]\n".to_owned()));
    for case in 0..5 {
        let mut product = *product_support::descriptor();
        product.driver_create = Some(capture_create);
        match case {
            0 => product.struct_size = size_of::<u32>() as u32,
            1 => product.abi_version += 1,
            2 => product.capability_name = ptr::null(),
            3 => product.capability_name = c"wrong.product".as_ptr(),
            _ => product.link_reload_unchanged = None,
        }
        let manifest = LoaderProviderManifest {
            product: &product,
            ..providers()
        };
        assert_eq!(
            // SAFETY: the descriptor copy and provider fixtures remain live until load returns.
            unsafe { loader_load(&manifest, ptr::dangling_mut::<u8>().cast()) },
            LOADER_DECLINE
        );
    }
    assert_eq!(CREATES.load(Ordering::Relaxed), 0);
    with_state(|state| {
        assert!(state.configuration_paths.is_empty());
        assert!(state.capabilities.is_empty());
        assert!(state.channels.is_empty());
    });
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

#[test]
fn typed_station_request_pins_driver_and_consumes_only_the_exact_dormant_owner() {
    typed_station_lifecycle(0);
}
#[test]
fn typed_station_initial_gain_is_applied_before_audio_and_write_failure_is_retryable() {
    typed_station_lifecycle(1);
}
#[test]
fn typed_station_raw_alsa_dsp_forward_and_reverse_keep_the_stream() {
    typed_station_lifecycle(2);
}
fn typed_station_lifecycle(fixture_kind: u32) {
    use crate::{
        UrpBoundStationRequest,
        host::channel::station::usbradioplus_asterisk_station_binding_descriptor,
    };
    use std::ffi::CStr;
    unsafe extern "C" {
        fn urp_test_physical_station(
            product: *const crate::UrpAstDescriptor,
            vad_mode: u32,
            inverted: u32,
            fixture_kind: u32,
            out: *mut *mut c_void,
        ) -> c_int;
    }
    struct Request {
        tech: *const crate::ffi::ast_channel_tech,
        address: Option<CString>,
        owner: *mut crate::ffi::ast_channel,
        open: bool,
    }
    impl Request {
        unsafe fn dial(&self, address: &CStr) -> *mut crate::ffi::ast_channel {
            // SAFETY: this fixture retains the registered Asterisk technology and its offered formats.
            unsafe {
                ((*self.tech).requester.unwrap())(
                    (*self.tech).type_,
                    (*self.tech).capabilities,
                    ptr::null(),
                    ptr::null(),
                    address.as_ptr(),
                    ptr::null_mut(),
                )
            }
        }
    }
    unsafe extern "C" fn dial(context: *mut c_void, address: *const std::ffi::c_char) -> c_int {
        // SAFETY: with_request synchronously lends both caller-owned context and terminated address.
        let (context, address) =
            unsafe { (&mut *context.cast::<Request>(), CStr::from_ptr(address)) };
        context.address = Some(address.to_owned());
        if context.open {
            // SAFETY: the driver remains registered during this synchronous public request.
            context.owner = unsafe { context.dial(address) };
            if context.owner.is_null() {
                return -1;
            }
        }
        0
    }
    let _fixture = Fixture::new();
    let _providers = PROVIDER_TEST_LOCK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let _cleanup = LoadedGuard;
    cli::tests::set_registration_result(0);
    provider_support::clear_failure();
    with_state(|state| state.configuration_text = Some(c"[different-ini-site]\n".to_owned()));
    let manifest = providers();
    assert_eq!(
        // SAFETY: the fake registered Asterisk host retains its callback table for the test.
        unsafe { loader_load(&manifest, ptr::dangling_mut::<u8>().cast()) },
        LOADER_OK
    );
    let tech = with_state(|s| s.registered[1]) as *const crate::ffi::ast_channel_tech;
    let mut channel = Request {
        tech,
        address: None,
        owner: ptr::null_mut(),
        open: false,
    };
    // SAFETY: the adapter publishes a complete static descriptor for this loaded test driver.
    let ops = unsafe { &*usbradioplus_asterisk_station_binding_descriptor() };
    let baseline = with_state(|s| s.module_references);
    product_support::count_station_streams(true);
    let streams = product_support::station_audio_events();
    let mut source = ptr::null_mut();
    assert_eq!(
        // SAFETY: the helper resolves the actual matched physical preparation getter.
        unsafe { urp_test_physical_station(manifest.product, 0, 0, fixture_kind, &mut source) },
        0
    );
    product_support::fail_station_preflight();
    let mut request = UrpBoundStationRequest {
        struct_size: size_of::<UrpBoundStationRequest>() as u32,
        abi_version: 1,
        port_uuid: [0x37; 16],
        candidate_identity: 51,
        product: ptr::null(),
        prepared: source,
    };
    let mut plan = ptr::dangling_mut();
    // SAFETY: all requests, plans, channels and test-provider DSOs stay live through each synchronous call.
    unsafe {
        assert_ne!(ops.prepare.unwrap()(&request, &mut plan), 0);
        assert!(plan.is_null());
        request.product = manifest.product;
        assert_eq!(ops.prepare.unwrap()(&request, &mut plan), 0);
        assert_eq!(with_state(|s| s.module_references), baseline + 1);
        assert_ne!(loader_unload(), LOADER_OK);
        assert_eq!(product_support::station_audio_events(), streams);
        let mut previous = ptr::null_mut();
        request.port_uuid = [0x42; 16];
        assert_eq!(ops.prepare.unwrap()(&request, &mut previous), 0);
        (*manifest.product).native_configuration_destroy.unwrap()(source);
        let with = ops.with_request.unwrap();
        assert_ne!(
            with(plan, ptr::from_mut(&mut channel).cast(), Some(dial)),
            0
        );
        assert!(channel.dial(channel.address.as_deref().unwrap()).is_null());
        channel.open = true;
        if fixture_kind != 2 {
            assert_ne!(
                with(plan, ptr::from_mut(&mut channel).cast(), Some(dial)),
                0
            );
            assert!(channel.dial(channel.address.as_deref().unwrap()).is_null());
        }
        provider_support::clear_failure();
        let inactive = product_support::station_audio_events();
        assert_eq!(
            with(plan, ptr::from_mut(&mut channel).cast(), Some(dial)),
            0
        );
        let mut owner = channel.owner;
        assert!(channel.dial(channel.address.as_deref().unwrap()).is_null());
        assert_ne!(
            with(plan, ptr::from_mut(&mut channel).cast(), Some(dial)),
            0
        );
        assert_ne!(ops.stage.unwrap()(owner.cast(), previous), 0);
        assert_eq!(product_support::station_audio_events(), inactive);
        let mut candidate = ptr::null_mut();
        assert_eq!(
            urp_test_physical_station(manifest.product, 2, 0, fixture_kind, &mut source),
            0
        );
        request.port_uuid = [0x37; 16];
        request.candidate_identity = 52;
        request.prepared = source;
        assert_eq!(ops.prepare.unwrap()(&request, &mut candidate), 0);
        (*manifest.product).native_configuration_destroy.unwrap()(source);
        product_support::station_audio_clock(true);
        product_support::set_start_levels([750, 795, 245]);
        if fixture_kind == 1 {
            product_support::fail_mixer_write();
            assert_ne!(
                (*tech).call.unwrap()(owner, ptr::null(), 0),
                0,
                "failed gain must prevent start"
            );
            provider_support::clear_failure();
            crate::ffi::ast_hangup(owner);
            assert_eq!(
                with(plan, ptr::from_mut(&mut channel).cast(), Some(dial)),
                0
            );
            owner = channel.owner;
        }

        assert_eq!((*tech).call.unwrap()(owner, ptr::null(), 0), 0);
        assert_eq!(
            product_support::start_levels(),
            if fixture_kind == 1 {
                [750, 795, 706]
            } else {
                [750, 795, 245]
            }
        );
        if fixture_kind == 1 {
            let service = crate::host::channel::test_station_start_stop;
            product_support::fail_audio_stop();
            assert_ne!(service(owner, false), 0, "retain the still-running stream");
            let restored = [721, 742, 763];
            product_support::set_start_levels(restored);
            product_support::fail_mixer_write();
            assert_eq!(
                service(owner, true),
                0,
                "reacquire GPIO without writing active gains"
            );
            assert_eq!(product_support::station_mixer_levels(), restored);
            provider_support::clear_failure();
        }
        let mixer_probes = product_support::mixer_discoveries();
        let active = product_support::station_audio_events();
        assert_eq!(active.0, active.1 + 1);
        let receive_policy = || {
            let mut command: crate::UrpAstChannelCommand = std::mem::zeroed();
            command.struct_size = size_of::<crate::UrpAstChannelCommand>() as u32;
            command.abi_version = 1;
            command.command = crate::URP_AST_COMMAND_GET_RECEIVE_POLICY;
            command.value = -1;
            assert_eq!(
                (*tech).setoption.unwrap()(
                    owner,
                    crate::URP_AST_OPTION_RECEIVE_POLICY,
                    ptr::from_mut(&mut command).cast(),
                    size_of::<crate::UrpAstChannelCommand>() as c_int
                ),
                0
            );
            (command.value, command.flags)
        };
        assert_eq!(receive_policy(), (0, 0));
        if fixture_kind != 2 {
            let mut handoff = ptr::null_mut();
            assert_eq!(
                urp_test_physical_station(manifest.product, 0, 1, fixture_kind, &mut source),
                0
            );
            request.candidate_identity = 53;
            request.prepared = source;
            assert_eq!(ops.prepare.unwrap()(&request, &mut handoff), 0);
            (*manifest.product).native_configuration_destroy.unwrap()(source);
            assert_eq!(ops.stage.unwrap()(owner.cast(), handoff), 1);
            assert_eq!(receive_policy(), (0, 0));
            ops.release.unwrap()(handoff);
        }
        let poll = |operation: &mut dyn FnMut() -> i32| {
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(4);
            loop {
                let code = operation();
                if code != 2 {
                    return code;
                }
                assert!(
                    std::time::Instant::now() < deadline,
                    "station control remains pending"
                );
                std::thread::sleep(std::time::Duration::from_millis(2));
            }
        };
        assert_eq!(poll(&mut || ops.stage.unwrap()(owner.cast(), candidate)), 0);
        assert_eq!(poll(&mut || ops.finish.unwrap()(owner.cast(), 1)), 0);
        assert_eq!(receive_policy(), (2, 7680));
        // A later unrelated port fails. Reverse through the explicit original preparation
        // while preserving the already-open Asterisk channel and native audio stream.
        if fixture_kind != 2 {
            product_support::fail_station_preflight();
            assert_ne!(
                with(previous, ptr::from_mut(&mut channel).cast(), Some(dial)),
                0
            );
            provider_support::clear_failure();
        }
        assert_eq!(poll(&mut || ops.stage.unwrap()(owner.cast(), plan)), 0);
        assert_eq!(poll(&mut || ops.finish.unwrap()(owner.cast(), 1)), 0);
        assert_eq!(receive_policy(), (0, 0));
        assert_eq!(product_support::station_audio_events(), active);
        if fixture_kind == 2 {
            assert_eq!(product_support::mixer_discoveries(), mixer_probes);
        }
        ops.release.unwrap()(candidate);
        ops.release.unwrap()(plan);
        ops.release.unwrap()(previous);
        assert_eq!(with_state(|s| s.module_references), baseline + 1);
        assert_ne!(loader_unload(), LOADER_OK);
        crate::ffi::ast_hangup(owner);
        product_support::station_audio_clock(false);
        assert_eq!(with_state(|s| s.module_references), baseline);
    }
    assert_eq!(loader_unload(), LOADER_OK);
    product_support::count_station_streams(false);
}
