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
