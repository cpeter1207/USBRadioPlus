use super::*;
use crate::{ABI_VERSION, URP_AST_JITTER_ADAPTIVE};
use std::cell::RefCell;
use std::sync::Arc;
use std::sync::mpsc::sync_channel;

#[repr(C)]
struct DirectV2 {
    struct_size: u32,
    abi_version: u32,
    receive_context: *mut c_void,
    receive: Option<unsafe extern "C" fn(*mut c_void, u32, *mut f32, u32) -> c_int>,
    transmit_context: *mut c_void,
    transmit: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u32, *mut u32) -> c_int>,
    accepted_abi_version: u32,
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_channel_tech_pvt(owner: *const ffi::ast_channel) -> *mut c_void {
    owner.cast_mut().cast()
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_taskprocessor_push(
    _: *mut ffi::ast_taskprocessor,
    callback: unsafe extern "C" fn(*mut c_void) -> c_int,
    data: *mut c_void,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> c_int {
    let data = data as usize;
    // SAFETY: the control fixture transfers a live Task to its single worker.
    std::thread::spawn(move || unsafe { callback(data as *mut c_void) });
    0
}

unsafe extern "C" fn retain_direct(
    channel: *mut c_void,
    _: *const crate::UrpAstDirectCallbacks,
) -> c_int {
    // SAFETY: the fixture supplies a live retention result until reply arrives.
    unsafe { *channel.cast::<c_int>() }
}

#[test]
fn direct_attachment_acknowledges_only_valid_retained_descriptor() {
    let callbacks = crate::tests::direct_callbacks();
    // SAFETY: descriptor fields are integers, raw pointers, and optional callbacks.
    let mut descriptor: UrpAstDescriptor = unsafe { zeroed() };
    descriptor.channel_set_direct_callbacks = Some(retain_direct);
    let mut retention = URP_AST_OK;
    let mut channel = Channel {
        _name: "usb".into(),
        descriptor: &descriptor,
        rust_channel: AtomicPtr::new(ptr::from_mut(&mut retention).cast()),
        control: ptr::null_mut(),
        dsp: ptr::null_mut(),
        format: ptr::null_mut(),
        sample_rate_hz: ADVANCED_RATE_HZ,
        frame_samples: 960,
        owner: AtomicPtr::new(ptr::null_mut()),
        worker: Mutex::new(None),
        delivery_stop: AtomicBool::new(false),
        service_failed: AtomicBool::new(false),
        jitter_pending: AtomicBool::new(false),
        pending_transmit: AtomicU64::new(0),
        direct: AtomicBool::new(false),
    };
    let mut value = 0u8;
    // SAFETY: the fake owner and its writable byte remain live through the call.
    let unsupported = unsafe {
        setoption(
            ptr::from_mut(&mut channel).cast(),
            12345,
            ptr::from_mut(&mut value).cast(),
            1,
        )
    };
    assert_eq!(unsupported, -1, "unsupported option must fail");
    // SAFETY: Linux provides a valid pointer to this thread's errno.
    assert_eq!(unsafe { *libc::__errno_location() }, libc::ENOSYS);
    for case in 0..9 {
        retention = if case == 7 { -1 } else { URP_AST_OK };
        channel
            .rust_channel
            .store(ptr::from_mut(&mut retention).cast(), Ordering::Release);
        let mut direct = DirectV2 {
            struct_size: size_of::<DirectV2>() as u32,
            abi_version: 2,
            receive_context: callbacks.receive_context,
            receive: callbacks.receive,
            transmit_context: callbacks.transmit_context,
            transmit: callbacks.transmit,
            accepted_abi_version: 0,
        };
        let mut length = size_of::<DirectV2>() as c_int;
        match case {
            0 => direct.struct_size -= 1,
            1 => direct.abi_version = 1,
            2 => direct.receive = None,
            3 => direct.transmit = None,
            4 => direct.receive_context = ptr::null_mut(),
            5 => direct.transmit_context = ptr::null_mut(),
            6 => length -= 1,
            _ => {}
        }
        // The host option must also accept byte-aligned Asterisk payloads.
        let mut storage = vec![0u8; size_of::<DirectV2>() + 1];
        // SAFETY: the allocation includes the byte offset and complete descriptor.
        let data = unsafe { storage.as_mut_ptr().add(1).cast::<DirectV2>() };
        // SAFETY: the byte buffer reserves a complete, possibly unaligned descriptor.
        unsafe { data.write_unaligned(direct) };
        // SAFETY: the fake owner, descriptor, and retention context remain live.
        let result = unsafe {
            setoption(
                ptr::from_mut(&mut channel).cast(),
                crate::URP_AST_OPTION_DIRECT_CALLBACKS,
                data.cast(),
                length,
            )
        };
        // SAFETY: setoption returned synchronously; the complete buffer remains live.
        let accepted = unsafe { data.read_unaligned().accepted_abi_version };
        assert_eq!(accepted, if case == 8 { 2 } else { 0 }, "case {case}");
        assert_eq!(result == 0, case == 8, "case {case}");
    }
}

#[test]
fn direct_call_starts_station_without_delivery_worker() {
    for direct in [true, false] {
        let events = RefCell::new(Vec::new());
        assert!(
            start_media(
                direct,
                || {
                    events.borrow_mut().push("start");
                    URP_AST_OK
                },
                || {
                    events.borrow_mut().push("delivery");
                    Ok(())
                },
                || {
                    events.borrow_mut().push("stop");
                },
            )
            .is_ok()
        );
        let expected: &[&str] = if direct {
            &["start"]
        } else {
            &["start", "delivery"]
        };
        assert_eq!(*events.borrow(), expected);
    }
}

#[test]
fn failed_delivery_start_stops_the_station() {
    let events = RefCell::new(Vec::new());
    assert!(
        start_media(
            false,
            || {
                events.borrow_mut().push("start");
                URP_AST_OK
            },
            || {
                events.borrow_mut().push("delivery");
                Err(())
            },
            || {
                events.borrow_mut().push("stop");
            },
        )
        .is_err()
    );
    assert_eq!(*events.borrow(), ["start", "delivery", "stop"]);
}

#[test]
fn direct_option_rejects_wrong_technology_and_payload_length() {
    let callbacks = crate::tests::direct_callbacks();
    let data = ptr::from_ref(&callbacks).cast_mut().cast();
    let length = size_of::<super::super::super::UrpAstDirectCallbacks>() as c_int;
    // SAFETY: complete copied descriptor remains live throughout validation.
    unsafe {
        assert!(direct_option(APP_RPT_RATE_HZ, data, length).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length - 1).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length + 1).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, ptr::null_mut(), length).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length).is_ok());
    }
}
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
fn channel_names_must_be_nonempty_and_fit_the_adapter_abi() {
    assert_eq!(channel_name_length(b""), None);
    assert_eq!(channel_name_length(b"alpha"), Some(5));
}

#[test]
fn reservation_failures_keep_the_existing_operator_diagnostics() {
    assert_eq!(
        reservation_failure_diagnostic("RadioPlus", b"alpha", URP_AST_CHANNEL_NOT_FOUND),
        (
            ffi::__LOG_WARNING as c_int,
            "RadioPlus/alpha: Rust channel was not configured".into()
        )
    );
    assert_eq!(
        reservation_failure_diagnostic("RadioPlusAdvanced", b"bravo", -6),
        (
            ffi::__LOG_ERROR as c_int,
            "RadioPlusAdvanced/bravo: Rust channel reservation failed (-6)".into()
        )
    );
}

#[test]
fn native_format_must_have_the_exact_required_rate() {
    let format = std::ptr::dangling_mut::<u8>();

    assert!(format_has_rate(format, 48_000, |_| 48_000));
    assert!(!format_has_rate(format, 48_000, |_| 44_100));
    assert!(!format_has_rate(ptr::null_mut::<u8>(), 48_000, |_| 48_000));
}

#[test]
fn failed_jitter_configuration_remains_pending_for_retry() {
    let pending = AtomicBool::new(true);
    assert_eq!(apply_pending_jitter(&pending, || Err(())), Err(()));
    assert!(pending.load(Ordering::Acquire));

    assert_eq!(apply_pending_jitter(&pending, || Ok(())), Ok(()));
    assert!(!pending.load(Ordering::Acquire));
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
fn forced_ctcss_accepts_only_one_optional_trailing_nul() {
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0"), Ok(1_000));
    assert_eq!(forced_ctcss_tenths_hz(b"100\0.0"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0x"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0\0"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"\0"), Err(()));
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
fn only_reload_admission_contention_defers_transmit() {
    let pending = AtomicU64::new(PendingTransmit::NONE.raw());
    let intent = PendingTransmit::new(true, 1_230);

    assert_eq!(
        finish_transmit_attempt(&pending, intent, TransmitAttempt::Deferred),
        0
    );
    assert_eq!(pending.load(Ordering::Acquire), intent.raw());

    pending.store(PendingTransmit::NONE.raw(), Ordering::Release);
    assert_eq!(
        finish_transmit_attempt(
            &pending,
            intent,
            TransmitAttempt::Completed(URP_AST_CHANNEL_BUSY)
        ),
        -1
    );
    assert_eq!(pending.load(Ordering::Acquire), PendingTransmit::NONE.raw());
}

#[test]
fn request_keeps_membership_frozen_through_initial_jitter_setup() {
    let events = RefCell::new(Vec::new());

    let succeeded = finish_request_jitter(
        || {
            events.borrow_mut().push("configure");
            Err(())
        },
        || events.borrow_mut().push("unlock"),
        || events.borrow_mut().push("release-membership"),
        || events.borrow_mut().push("hangup"),
    );

    assert!(!succeeded);
    assert_eq!(
        *events.borrow(),
        ["configure", "unlock", "release-membership", "hangup"]
    );
}

#[test]
fn hangup_releases_owner_before_waiting_for_membership() {
    let events = RefCell::new(Vec::new());

    let membership = handoff_hangup_lock(
        || events.borrow_mut().push("unpublish"),
        || events.borrow_mut().push("unlock-owner"),
        || {
            events.borrow_mut().push("lock-membership");
            17
        },
        || events.borrow_mut().push("relock-owner"),
    );

    assert_eq!(membership, 17);
    assert_eq!(
        *events.borrow(),
        [
            "unpublish",
            "unlock-owner",
            "lock-membership",
            "relock-owner"
        ]
    );
}

#[test]
fn unregister_rechecks_activity_after_request_construction_finishes() {
    let membership = Arc::new(Mutex::new(()));
    let active = Arc::new(AtomicUsize::new(0));
    let construction = membership.lock().expect("membership lock poisoned");
    let (started_sender, started_receiver) = sync_channel(0);
    let (result_sender, result_receiver) = sync_channel(0);
    let worker_membership = Arc::clone(&membership);
    let worker_active = Arc::clone(&active);

    let worker = std::thread::spawn(move || {
        started_sender.send(()).expect("start signal failed");
        let result = match lock_idle_membership(&worker_membership, &worker_active) {
            Ok(membership) => {
                drop(membership);
                Ok(())
            }
            Err(status) => Err(status),
        };
        result_sender.send(result).expect("result signal failed");
    });
    started_receiver.recv().expect("start signal failed");
    active.store(1, Ordering::Release);
    drop(construction);

    assert_eq!(
        result_receiver.recv().expect("result signal failed"),
        Err(URP_AST_CHANNEL_BUSY)
    );
    worker.join().expect("worker panicked");
}

#[test]
fn registration_gate_blocks_requests_until_rollback_finishes() {
    let membership = Arc::new(Mutex::new(()));
    let active = AtomicUsize::new(0);
    let registration =
        lock_idle_membership(&membership, &active).expect("registration gate failed");
    let (started_sender, started_receiver) = sync_channel(0);
    let (acquired_sender, acquired_receiver) = sync_channel(0);
    let request_membership = Arc::clone(&membership);

    let request = std::thread::spawn(move || {
        started_sender.send(()).expect("start signal failed");
        let _request = request_membership.lock().expect("membership lock poisoned");
        acquired_sender.send(()).expect("acquired signal failed");
    });
    started_receiver.recv().expect("start signal failed");
    assert_eq!(
        acquired_receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );

    drop(registration);
    acquired_receiver.recv().expect("acquired signal failed");
    request.join().expect("request panicked");
}

#[test]
fn channel_host_requires_every_operation_it_invokes() {
    assert!(descriptor_is_valid(crate::product_descriptor()));
    macro_rules! reject_missing {
        ($field:ident) => {{
            // SAFETY: the descriptor contains only plain ABI fields and
            // function pointers, so this copy owns no dropped resource.
            let mut incomplete = unsafe { std::ptr::read(crate::product_descriptor()) };
            incomplete.$field = None;
            assert!(!descriptor_is_valid(&incomplete));
        }};
    }
    for corrupt in [0_u8, 1] {
        // SAFETY: see the macro comment above.
        let mut incomplete = unsafe { std::ptr::read(crate::product_descriptor()) };
        if corrupt == 0 {
            incomplete.struct_size = 0;
        } else {
            incomplete.abi_version = 0;
        }
        assert!(!descriptor_is_valid(&incomplete));
    }
    reject_missing!(driver_reload);
    reject_missing!(driver_reload_finish);
    reject_missing!(driver_channel_name);
    reject_missing!(driver_active_channel);
    reject_missing!(driver_set_active_channel);
    reject_missing!(channel_reserve);
    reject_missing!(channel_start);
    reject_missing!(channel_stop);
    reject_missing!(channel_reload_prepare);
    reject_missing!(channel_reload_activate);
    reject_missing!(channel_reload_finish);
    reject_missing!(channel_write_voice);
    reject_missing!(channel_write_text);
    reject_missing!(channel_set_transmit);
    reject_missing!(channel_set_dtmf);
    reject_missing!(channel_set_echo);
    reject_missing!(channel_set_direct_callbacks);
    reject_missing!(channel_get_jitter_config);
    reject_missing!(channel_command);
    reject_missing!(channel_get_status);
    reject_missing!(channel_service);
    reject_missing!(channel_destroy);
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
