//! Independent sized-ABI fixtures for the stopped-bound final-programme fence.
use super::*;

type Permit = unsafe extern "C" fn(*mut c_void, u64, u32) -> u32;
pub(super) type Bind = unsafe extern "C" fn(*mut c_void, *mut Binding) -> c_int;

#[repr(C)]
pub(super) struct Binding {
    size: u32,
    version: u32,
    context: *mut c_void,
    pub(super) permit: Option<Permit>,
    pub(super) accepted: u32,
    reserved: u32,
}

pub(super) fn bindings() -> [Bind; 2] {
    assert_eq!(size_of::<UrpProgrammeFenceBinding>(), 32);
    assert_eq!(
        std::mem::offset_of!(UrpProgrammeFenceBinding, accepted_abi_version),
        24
    );
    let offset = std::mem::offset_of!(UrpAstDescriptor, channel_programme_retire)
        + size_of::<Option<unsafe extern "C" fn()>>();
    assert!(
        DESCRIPTOR.struct_size as usize >= offset + 2 * size_of::<Option<Bind>>(),
        "the optional native/channel post-DSP programme fence is absent"
    );
    // SAFETY: the advertised complete tail is checked before reading either callback.
    let callbacks = unsafe {
        ptr::from_ref(&DESCRIPTOR)
            .cast::<u8>()
            .add(offset)
            .cast::<[Option<Bind>; 2]>()
            .read_unaligned()
    };
    callbacks.map(|callback| callback.expect("advertised programme fence must be callable"))
}

pub(super) fn binding() -> Binding {
    Binding {
        size: size_of::<Binding>() as u32,
        version: 1,
        context: ptr::null_mut(),
        permit: Some(deny),
        accepted: 0,
        reserved: 0,
    }
}

unsafe extern "C" fn deny(_: *mut c_void, generation: u64, frames: u32) -> u32 {
    assert_ne!(generation, 0);
    assert!((1..=960).contains(&frames));
    0
}

pub(super) fn reject_invalid(call: Bind, handle: *mut c_void) {
    // SAFETY: null and a physically short size-only record are rejected before reading a body.
    unsafe {
        assert_ne!(call(handle, ptr::null_mut()), 0);
        let short = 4_u32;
        assert_ne!(call(handle, ptr::from_ref(&short).cast_mut().cast()), 0);
    }
    for defect in 0..4 {
        let mut value = binding();
        match defect {
            0 => value.size -= 1,
            1 => value.version = 2,
            2 => value.reserved = 1,
            _ => value.accepted = 1,
        }
        // SAFETY: every record is fully allocated, even when its declared extent is short.
        assert_ne!(unsafe { call(handle, &mut value) }, 0);
    }
}

#[test]
fn programme_fence_channel_requires_stopped_sized_ack_and_retains_through_restart() {
    let call = bindings()[1];
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let driver = create_driver(&context, b"[usb]\n");
    // SAFETY: the fixture retains this unique channel and static permit through destruction.
    unsafe {
        let channel = reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED);
        reject_invalid(call, channel);
        let mut value = binding();
        assert_ne!(call(ptr::null_mut(), &mut value), 0);
        assert_eq!(value.accepted, 0);
        assert_eq!(call(channel, &mut value), 0);
        assert_eq!(value.accepted, 1);
        assert_eq!(channel_start(channel), 0);
        value.accepted = 0;
        value.permit = None;
        assert_eq!(call(channel, &mut value), URP_AST_CHANNEL_BUSY);
        assert_eq!(value.accepted, 0);
        provider_support::set_failure(provider_support::FAIL_STOP);
        assert_ne!(channel_stop(channel), 0);
        assert_eq!(call(channel, &mut value), URP_AST_CHANNEL_BUSY);
        assert_eq!(value.accepted, 0);
        provider_support::clear_failure();
        assert_eq!(channel_stop(channel), 0);
        assert_eq!(channel_start(channel), 0);
        assert_eq!(channel_stop(channel), 0);
        assert_eq!(call(channel, &mut value), 0);
        assert_eq!(value.accepted, 1);
        channel_destroy(channel);
        driver_destroy(driver);
    }
}

#[test]
fn programme_fence_preserves_actual_callback_across_replacement_rollback_and_stopped_rebind() {
    struct Probe {
        marker: u32,
        calls: u32,
        generation: u64,
    }
    unsafe fn record(raw: *mut c_void, generation: u64, frames: u32, marker: u32) -> u32 {
        // SAFETY: this serialized fixture retains the exact probe until quiescence.
        let probe = unsafe { &mut *raw.cast::<Probe>() };
        assert_eq!((probe.marker, frames), (marker, 960));
        assert_ne!(generation, 0);
        probe.calls += 1;
        probe.generation = generation;
        0
    }
    unsafe extern "C" fn first(raw: *mut c_void, generation: u64, frames: u32) -> u32 {
        // SAFETY: the fixture binds the first live probe to this callback.
        unsafe { record(raw, generation, frames, 11) }
    }
    unsafe extern "C" fn second(raw: *mut c_void, generation: u64, frames: u32) -> u32 {
        // SAFETY: the fixture binds the second live probe to this callback.
        unsafe { record(raw, generation, frames, 22) }
    }
    #[repr(C)]
    struct Extended {
        binding: Binding,
        tail: [u64; 2],
    }
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    provider_support::execute_tx_graph(true);
    let bind = bindings()[1];
    for commit in [0, 1] {
        let context = RefCell::new(Calls::default());
        let driver = create_driver(&context, b"[usb]\n");
        let mut old = Probe {
            marker: 11,
            calls: 0,
            generation: 0,
        };
        let mut new = Probe {
            marker: 22,
            calls: 0,
            generation: 0,
        };
        let mut value = Extended {
            binding: binding(),
            tail: [0xface, 0xdead_beef],
        };
        value.binding.size = size_of::<Extended>() as u32;
        value.binding.context = ptr::from_mut(&mut old).cast();
        value.binding.permit = Some(first);
        // SAFETY: both contexts and callbacks outlive all synchronous stopped lifecycle calls.
        unsafe {
            let channel = reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED);
            assert_eq!(bind(channel, ptr::from_mut(&mut value).cast()), 0);
            assert_eq!(value.binding.accepted, 1);
            assert_eq!(value.tail, [0xface, 0xdead_beef]);
            assert_eq!(channel_start(channel), 0);
            assert_eq!(old.calls, 1);
            assert!(!provider_support::tx_graph_audible());
            let original = old.generation;
            assert_eq!(
                stage_reload(
                    driver,
                    b"[usb]\n[hardware]\nhardware_output_extra_buffer_ms=20\n"
                ),
                0
            );
            assert_eq!(prepare_reload(channel), 0);
            value.binding.accepted = 0;
            assert_eq!(
                bind(channel, ptr::from_mut(&mut value).cast()),
                URP_AST_CHANNEL_BUSY
            );
            assert_eq!(activate_reload(channel), 0);
            let replacement = old.generation;
            assert_ne!(replacement, original);
            assert!(old.calls >= 2);
            assert!(!provider_support::tx_graph_audible());
            assert_eq!(finish_reload(channel, commit), 0);
            assert_eq!(driver_reload_finish(driver, commit), 0);
            assert_eq!(
                old.generation,
                if commit == 0 { original } else { replacement }
            );
            assert_eq!(channel_stop(channel), 0);
            let old_calls = old.calls;
            value.binding.context = ptr::from_mut(&mut new).cast();
            value.binding.permit = Some(second);
            assert_eq!(bind(channel, ptr::from_mut(&mut value).cast()), 0);
            assert_eq!(value.binding.accepted, 1);
            assert_eq!(value.tail, [0xface, 0xdead_beef]);
            assert_eq!(channel_start(channel), 0);
            assert_eq!(new.calls, 1);
            assert_eq!(old.calls, old_calls);
            assert_eq!(new.generation, old.generation);
            assert!(!provider_support::tx_graph_audible());
            assert_eq!(channel_stop(channel), 0);
            value.binding.accepted = 0;
            value.binding.permit = None;
            assert_eq!(bind(channel, ptr::from_mut(&mut value).cast()), 0);
            assert_eq!(channel_start(channel), 0);
            assert_eq!((old.calls, new.calls), (old_calls, 1));
            assert!(
                provider_support::tx_graph_audible(),
                "unbound witness must really produce PCM"
            );
            assert_eq!(channel_stop(channel), 0);
            channel_destroy(channel);
            driver_destroy(driver);
        }
    }
    provider_support::execute_tx_graph(false);
}

#[test]
fn programme_fence_retains_binding_during_stopped_reload_transactions() {
    unsafe extern "C" fn count(raw: *mut c_void, generation: u64, frames: u32) -> u32 {
        assert_ne!(generation, 0);
        assert_eq!(frames, 960);
        // SAFETY: the test retains its counter through confirmed callback quiescence.
        unsafe { *raw.cast::<u32>() += 1 };
        0
    }
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    provider_support::execute_tx_graph(true);
    let bind = bindings()[1];
    for commit in [0, 1] {
        let context = RefCell::new(Calls::default());
        let driver = create_driver(&context, b"[usb]\n");
        let mut calls = 0_u32;
        let mut value = binding();
        value.context = ptr::from_mut(&mut calls).cast();
        value.permit = Some(count);
        // SAFETY: the stopped reservation and counter remain live through destruction.
        unsafe {
            let channel = reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED);
            assert_eq!(bind(channel, &mut value), URP_AST_OK);
            assert_eq!(value.accepted, 1);
            assert_eq!(
                stage_reload(
                    driver,
                    b"[usb]\n[hardware]\nhardware_output_extra_buffer_ms=20\n"
                ),
                URP_AST_OK
            );
            assert_eq!(prepare_reload(channel), URP_AST_OK);
            value.accepted = 0;
            value.permit = None;
            assert_eq!(bind(channel, &mut value), URP_AST_CHANNEL_BUSY);
            assert_eq!(value.accepted, 0);
            assert_eq!(activate_reload(channel), URP_AST_OK);
            assert_eq!(bind(channel, &mut value), URP_AST_CHANNEL_BUSY);
            assert_eq!(value.accepted, 0);
            assert_eq!(finish_reload(channel, commit), URP_AST_OK);
            assert_eq!(driver_reload_finish(driver, commit), URP_AST_OK);
            assert_eq!(channel_start(channel), URP_AST_OK);
            assert_eq!(calls, 1, "rejected unbind retained the original callback");
            assert!(!provider_support::tx_graph_audible());
            assert_eq!(channel_stop(channel), URP_AST_OK);
            assert_eq!(bind(channel, &mut value), URP_AST_OK);
            assert_eq!(value.accepted, 1);
            assert_eq!(channel_start(channel), URP_AST_OK);
            assert_eq!(calls, 1, "quiescent unbind released the original callback");
            assert!(provider_support::tx_graph_audible());
            assert_eq!(channel_stop(channel), URP_AST_OK);
            channel_destroy(channel);
            driver_destroy(driver);
        }
    }
    provider_support::execute_tx_graph(false);
}
