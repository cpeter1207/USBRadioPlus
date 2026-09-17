use super::*;

use std::cell::RefCell;
use std::ffi::CStr;

#[path = "tests/provider_support.rs"]
mod provider_support;

static PROVIDER_TEST_LOCK: Mutex<()> = Mutex::new(());

unsafe extern "C" fn direct_receive_noop(_: *mut c_void, _: u32, _: *mut f32, _: u32) -> c_int {
    0
}
unsafe extern "C" fn direct_transmit_noop(
    _: *mut c_void,
    _: *mut f32,
    _: u32,
    _: *mut u32,
) -> c_int {
    0
}

pub(crate) fn direct_callbacks() -> UrpAstDirectCallbacks {
    UrpAstDirectCallbacks {
        struct_size: size_of::<UrpAstDirectCallbacks>() as u32,
        abi_version: 1,
        receive_context: NonNull::<u8>::dangling().as_ptr().cast(),
        receive: Some(direct_receive_noop),
        transmit_context: NonNull::<u8>::dangling().as_ptr().cast(),
        transmit: Some(direct_transmit_noop),
    }
}

#[test]
fn direct_attachment_validates_boundary_and_survives_prepared_reload() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let driver = create_driver(&context, b"[usb]\n");
    // SAFETY: the test owns all handles and no-op callbacks never dereference contexts.
    unsafe {
        let app = reserve_channel(driver, URP_AST_TRANSPORT_APP_RPT);
        assert_ne!(
            channel_set_direct_callbacks(app, &direct_callbacks()),
            URP_AST_OK
        );
        channel_destroy(app);
        let advanced = reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED);
        assert_ne!(
            channel_set_direct_callbacks(advanced, ptr::null()),
            URP_AST_OK
        );
        for field in 0..6 {
            let mut invalid = direct_callbacks();
            match field {
                0 => invalid.struct_size -= 1,
                1 => invalid.abi_version = 2,
                2 => invalid.receive = None,
                3 => invalid.transmit = None,
                4 => invalid.receive_context = ptr::null_mut(),
                _ => invalid.transmit_context = ptr::null_mut(),
            }
            assert_ne!(channel_set_direct_callbacks(advanced, &invalid), URP_AST_OK);
        }
        assert_eq!(
            channel_set_direct_callbacks(advanced, &direct_callbacks()),
            URP_AST_OK
        );
        assert_ne!(
            channel_set_direct_callbacks(advanced, &direct_callbacks()),
            URP_AST_OK
        );
        assert_eq!(stage_reload(driver, b"[usb]\n"), URP_AST_OK);
        assert_eq!(channel_reload_prepare(advanced), URP_AST_OK);
        let (_, control) = channel_control(advanced).unwrap();
        // Duplicate rejection proves the replacement already carries its direct binding.
        assert!(
            control
                .reload
                .as_mut()
                .unwrap()
                .media
                .set_direct_callbacks(direct_callbacks())
                .is_err()
        );
        assert_eq!(channel_reload_activate(advanced), URP_AST_OK);
        assert_eq!(channel_reload_finish(advanced, 1), URP_AST_OK);
        assert_eq!(driver_reload_finish(driver, 1), URP_AST_OK);
        assert_eq!(channel_start(advanced), URP_AST_OK);
        assert_eq!(channel_stop(advanced), URP_AST_OK);
        assert_ne!(
            channel_set_direct_callbacks(advanced, &direct_callbacks()),
            URP_AST_OK
        );
        channel_destroy(advanced);
        let late = reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED);
        assert_eq!(channel_start(late), URP_AST_OK);
        assert_ne!(
            channel_set_direct_callbacks(late, &direct_callbacks()),
            URP_AST_OK
        );
        assert_eq!(channel_stop(late), URP_AST_OK);
        assert_ne!(
            channel_set_direct_callbacks(late, &direct_callbacks()),
            URP_AST_OK
        );
        channel_destroy(late);
        driver_destroy(driver);
    }
}

#[derive(Default)]
struct Calls {
    voice: Vec<(usize, u32)>,
    control: Vec<(u32, i32, u64)>,
    text: Vec<String>,
    logs: Vec<(u32, String)>,
    dtmf_return: c_int,
    dtmf: UrpAstDtmfResult,
    now_ms: u64,
    fail_queue: bool,
    mute_sample: bool,
}

unsafe fn calls<'a>(context: *mut c_void) -> &'a RefCell<Calls> {
    // SAFETY: every test callback receives its live RefCell as context.
    unsafe { &*context.cast::<RefCell<Calls>>() }
}

unsafe extern "C" fn queue_voice(
    application: *mut c_void,
    _channel: *mut c_void,
    _samples: *const i16,
    sample_count: u32,
    rate: u32,
) -> c_int {
    // SAFETY: the table was constructed with this test context.
    let calls = unsafe { calls(application) };
    let mut calls = calls.borrow_mut();
    calls.voice.push((sample_count as usize, rate));
    -c_int::from(calls.fail_queue)
}

unsafe extern "C" fn queue_control(
    application: *mut c_void,
    _channel: *mut c_void,
    kind: u32,
    value: i32,
    duration_ms: u64,
) -> c_int {
    // SAFETY: the table was constructed with this test context.
    let calls = unsafe { calls(application) };
    let mut calls = calls.borrow_mut();
    calls.control.push((kind, value, duration_ms));
    -c_int::from(calls.fail_queue)
}

unsafe extern "C" fn queue_text(
    application: *mut c_void,
    _channel: *mut c_void,
    text: *const u8,
    length: u32,
) -> c_int {
    // SAFETY: the adapter supplies one readable byte span for this call.
    let text = unsafe { std::slice::from_raw_parts(text, length as usize) };
    // SAFETY: the table was constructed with this test context.
    let calls = unsafe { calls(application) };
    let mut calls = calls.borrow_mut();
    calls
        .text
        .push(std::str::from_utf8(text).unwrap().to_owned());
    -c_int::from(calls.fail_queue)
}

unsafe extern "C" fn analyze_dtmf(
    application: *mut c_void,
    _channel: *mut c_void,
    samples: *mut i16,
    sample_count: u32,
    _rate: u32,
    result: *mut UrpAstDtmfResult,
) -> c_int {
    // SAFETY: the table was constructed with this test context.
    let calls = unsafe { calls(application) }.borrow();
    if calls.mute_sample && sample_count != 0 {
        // SAFETY: the adapter supplies sample_count writable samples.
        unsafe { samples.write(0) };
    }
    // SAFETY: the adapter supplies writable result storage.
    unsafe { result.write(calls.dtmf) };
    calls.dtmf_return
}

unsafe extern "C" fn monotonic(application: *mut c_void) -> u64 {
    // SAFETY: the table was constructed with this test context.
    unsafe { calls(application) }.borrow().now_ms
}

unsafe extern "C" fn log(application: *mut c_void, level: u32, message: *const u8, length: u32) {
    // SAFETY: the adapter supplies one readable byte span for this call.
    let message = unsafe { std::slice::from_raw_parts(message, length as usize) };
    // SAFETY: the table was constructed with this test context.
    unsafe { calls(application) }
        .borrow_mut()
        .logs
        .push((level, std::str::from_utf8(message).unwrap().to_owned()));
}

fn raw_operations(context: &RefCell<Calls>) -> UrpAstOperations {
    UrpAstOperations {
        struct_size: size_of::<UrpAstOperations>() as u32,
        abi_version: ABI_VERSION,
        application_context: ptr::from_ref(context).cast_mut().cast(),
        queue_voice: Some(queue_voice),
        queue_control: Some(queue_control),
        queue_text: Some(queue_text),
        analyze_dtmf: Some(analyze_dtmf),
        monotonic_milliseconds: Some(monotonic),
        log: Some(log),
    }
}

fn operations(context: &RefCell<Calls>) -> Operations {
    let raw = raw_operations(context);
    // SAFETY: the local table is read and copied during this call.
    unsafe { Operations::from_raw(ptr::from_ref(&raw)) }.unwrap()
}

fn reserve_args(transport: u32) -> UrpAstChannelReserveArgs {
    reserve_args_for(c"usb", transport)
}

fn reserve_args_for(name: &CStr, transport: u32) -> UrpAstChannelReserveArgs {
    UrpAstChannelReserveArgs {
        struct_size: size_of::<UrpAstChannelReserveArgs>() as u32,
        abi_version: ABI_VERSION,
        channel_name: name.as_ptr().cast(),
        channel_name_length: name.to_bytes().len() as u32,
        transport,
        channel_context: ptr::null_mut(),
    }
}

fn create_driver_result(
    context: &RefCell<Calls>,
    config: &[u8],
    plugin: &[u8],
) -> (c_int, *mut c_void) {
    let operations = raw_operations(context);
    let providers = provider_support::manifest();
    let source = b"usbradioplus.conf";
    let args = UrpAstDriverCreateArgs {
        struct_size: size_of::<UrpAstDriverCreateArgs>() as u32,
        abi_version: ABI_VERSION,
        config_source: source.as_ptr(),
        config_source_length: source.len() as u32,
        config_text: config.as_ptr(),
        config_text_length: config.len() as u32,
        agc_plugin_path: plugin.as_ptr(),
        agc_plugin_path_length: plugin.len() as u32,
        operations: ptr::from_ref(&operations),
        providers: ptr::from_ref(&providers),
    };
    let mut driver = ptr::null_mut();
    // SAFETY: all descriptor storage is process-lived and all argument spans
    // remain live for this synchronous constructor.
    let status = unsafe { driver_create(&args, &mut driver) };
    (status, driver)
}

fn create_driver(context: &RefCell<Calls>, config: &[u8]) -> *mut c_void {
    let (status, driver) = create_driver_result(context, config, b"agc.so");
    assert_eq!(status, URP_AST_OK);
    assert!(!driver.is_null());
    driver
}

unsafe fn reserve_channel(driver: *mut c_void, transport: u32) -> *mut c_void {
    let args = reserve_args(transport);
    // SAFETY: the caller supplies a live driver and the local arguments remain live.
    unsafe { reserve_channel_with_args(driver, &args) }
}

unsafe fn reserve_channel_with_args(
    driver: *mut c_void,
    args: &UrpAstChannelReserveArgs,
) -> *mut c_void {
    let mut channel = ptr::null_mut();
    // SAFETY: the caller supplies a live driver and the local arguments remain live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_reserve(driver, args, &mut channel) },
        URP_AST_OK
    );
    assert!(!channel.is_null());
    channel
}

unsafe fn stage_reload(driver: *mut c_void, config: &[u8]) -> c_int {
    let source = b"reload.conf";
    // SAFETY: the caller keeps the live driver and both byte spans valid for this call.
    unsafe {
        driver_reload(
            driver,
            source.as_ptr(),
            source.len() as u32,
            config.as_ptr(),
            config.len() as u32,
        )
    }
}

unsafe fn reload_channels(driver: *mut c_void, channels: &[*mut c_void], config: &[u8]) -> c_int {
    // SAFETY: the caller supplies live handles serialized exactly as the Rust host does.
    let mut status = unsafe { stage_reload(driver, config) };
    if status != URP_AST_OK {
        return status;
    }
    for channel in channels {
        // SAFETY: every channel is live and owned by this serialized test.
        status = unsafe { channel_reload_prepare(*channel) };
        if status != URP_AST_OK {
            break;
        }
    }
    if status == URP_AST_OK {
        for channel in channels {
            // SAFETY: every channel was prepared above.
            status = unsafe { channel_reload_activate(*channel) };
            if status != URP_AST_OK {
                break;
            }
        }
    }
    if status == URP_AST_OK {
        // SAFETY: every channel adopted its candidate, so publication cannot fail.
        status = unsafe { driver_reload_finish(driver, 1) };
        if status == URP_AST_OK {
            for channel in channels {
                // SAFETY: every channel adopted its candidate above.
                assert_eq!(unsafe { channel_reload_finish(*channel, 1) }, URP_AST_OK);
            }
            return URP_AST_OK;
        }
    }
    for channel in channels {
        // SAFETY: rollback is idempotent for channels which did not prepare or activate.
        let _ = unsafe { channel_reload_finish(*channel, 0) };
    }
    // SAFETY: the staged driver candidate has not been published.
    let _ = unsafe { driver_reload_finish(driver, 0) };
    status
}

fn command(kind: u32, target: u32, value: i64) -> UrpAstChannelCommand {
    UrpAstChannelCommand {
        struct_size: size_of::<UrpAstChannelCommand>() as u32,
        abi_version: ABI_VERSION,
        command: kind,
        target,
        value,
        flags: 0,
        eeprom_words: [0; 64],
    }
}

#[test]
fn descriptor_exposes_one_complete_versioned_boundary() {
    let descriptor = product_descriptor();
    assert_eq!(
        descriptor.struct_size as usize,
        size_of::<UrpAstDescriptor>()
    );
    assert_eq!(descriptor.abi_version, ABI_VERSION);
    // SAFETY: the static descriptor owns a NUL-terminated capability string.
    let capability = unsafe { CStr::from_ptr(descriptor.capability_name) };
    assert_eq!(capability, CAPABILITY);
    assert!(descriptor.driver_create.is_some());
    assert!(descriptor.driver_reload.is_some());
    assert!(descriptor.driver_reload_finish.is_some());
    assert!(descriptor.driver_channel_name.is_some());
    assert!(descriptor.driver_active_channel.is_some());
    assert!(descriptor.driver_set_active_channel.is_some());
    assert!(descriptor.driver_destroy.is_some());
    assert!(descriptor.link_prepare.is_some());
    assert!(descriptor.link_prepare_reload.is_some());
    assert!(descriptor.link_process.is_some());
    assert!(descriptor.link_observe.is_some());
    assert!(descriptor.link_destroy.is_some());
    assert!(descriptor.channel_reserve.is_some());
    assert!(descriptor.channel_start.is_some());
    assert!(descriptor.channel_stop.is_some());
    assert!(descriptor.channel_reload_prepare.is_some());
    assert!(descriptor.channel_reload_activate.is_some());
    assert!(descriptor.channel_reload_finish.is_some());
    assert!(descriptor.channel_write_voice.is_some());
    assert!(descriptor.channel_write_text.is_some());
    assert!(descriptor.channel_set_transmit.is_some());
    assert!(descriptor.channel_set_dtmf.is_some());
    assert!(descriptor.channel_set_echo.is_some());
    assert!(descriptor.channel_get_jitter_config.is_some());
    assert!(descriptor.channel_command.is_some());
    assert!(descriptor.channel_get_status.is_some());
    assert!(descriptor.channel_service.is_some());
    assert!(descriptor.channel_destroy.is_some());
}

#[test]
fn incoming_link_boundary_prepares_processes_observes_and_destroys() {
    const CONFIG: &[u8] = b"[usb]\n[link]\nenabled = yes\n";
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let driver = create_driver(&context, CONFIG);
    let mut link = ptr::null_mut();

    // SAFETY: the driver, name, and output storage remain live for this setup call.
    assert_eq!(
        // SAFETY: the complete arguments remain live for this setup call.
        unsafe { link_prepare(driver, c"usb".as_ptr().cast(), 3, 8_000, 160, &mut link) },
        URP_AST_OK
    );
    assert!(!link.is_null());
    let mut samples = [123_i16; 160];
    // SAFETY: this test is the sole callback owner of link and its writable frame.
    assert_eq!(
        // SAFETY: the link and writable frame remain exclusively owned here.
        unsafe {
            link_process(
                link,
                URP_AST_LINK_DIRECTION_READ,
                8_000,
                samples.as_mut_ptr(),
                samples.len() as u32,
            )
        },
        URP_AST_OK
    );
    assert_eq!(samples, [123; 160]);
    // SAFETY: write direction and wrong rate are intentional bypass cases.
    assert_eq!(
        // SAFETY: the link and writable frame remain exclusively owned here.
        unsafe {
            link_process(
                link,
                URP_AST_LINK_DIRECTION_WRITE,
                8_000,
                samples.as_mut_ptr(),
                samples.len() as u32,
            )
        },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the link and writable frame remain exclusively owned here.
        unsafe {
            link_process(
                link,
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                samples.as_mut_ptr(),
                samples.len() as u32,
            )
        },
        URP_AST_OK
    );
    let mut observation = UrpAstLinkObservation {
        struct_size: size_of::<UrpAstLinkObservation>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstLinkObservation::default()
    };
    // SAFETY: callback processing has quiesced and initialized output storage is writable.
    assert_eq!(unsafe { link_observe(link, &mut observation) }, URP_AST_OK);
    assert_eq!(observation.processed_blocks, 1);
    assert_eq!(observation.bypassed_blocks, 2);
    assert_eq!(observation.failed_blocks, 0);

    provider_support::set_failure(provider_support::FAIL_GRAPH);
    let mut failed_link = ptr::null_mut();
    // SAFETY: graph construction failure is injected with otherwise complete arguments.
    assert_eq!(
        // SAFETY: the complete arguments remain live for this setup call.
        unsafe {
            link_prepare(
                driver,
                c"usb".as_ptr().cast(),
                3,
                8_000,
                160,
                &mut failed_link,
            )
        },
        URP_AST_SETUP_FAILED
    );
    assert!(failed_link.is_null());
    provider_support::clear_failure();

    // A candidate graph failure leaves both the published driver and live link untouched.
    assert_eq!(
        // SAFETY: the driver remains live through this staged transaction.
        unsafe { stage_reload(driver, CONFIG) },
        URP_AST_OK
    );
    provider_support::set_failure(provider_support::FAIL_GRAPH);
    assert_eq!(
        // SAFETY: the staged driver, name, and output storage remain live.
        unsafe {
            link_prepare_reload(
                driver,
                c"usb".as_ptr().cast(),
                3,
                8_000,
                160,
                &mut failed_link,
            )
        },
        URP_AST_SETUP_FAILED
    );
    provider_support::clear_failure();
    assert_eq!(
        // SAFETY: the retained link and writable frame remain exclusively owned here.
        unsafe {
            link_process(
                link,
                URP_AST_LINK_DIRECTION_READ,
                8_000,
                samples.as_mut_ptr(),
                samples.len() as u32,
            )
        },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: this live driver still owns the unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );

    // SAFETY: invalid inputs are rejected before their pointers are dereferenced.
    assert_eq!(
        // SAFETY: the live frame is valid; only the direction is deliberately invalid.
        unsafe { link_process(link, 99, 8_000, samples.as_mut_ptr(), 160) },
        URP_AST_INVALID_ARGUMENT
    );
    assert_eq!(
        // SAFETY: null PCM is deliberately rejected before dereference.
        unsafe {
            link_process(
                link,
                URP_AST_LINK_DIRECTION_READ,
                8_000,
                ptr::null_mut(),
                160,
            )
        },
        URP_AST_INVALID_ARGUMENT
    );
    assert_eq!(
        // SAFETY: null link is deliberately rejected before dereference.
        unsafe { link_observe(ptr::null_mut(), &mut observation) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: link and driver are paired live handles consumed exactly once.
    unsafe {
        link_destroy(link);
        driver_destroy(driver);
    }

    let disabled = b"[usb]\n[link]\nenabled = no\n";
    let driver = create_driver(&context, disabled);
    link = 1_usize as *mut c_void;
    // SAFETY: complete arguments are live; disabled and missing profiles are explicit cases.
    assert_eq!(
        // SAFETY: complete arguments remain live for this setup call.
        unsafe { link_prepare(driver, c"usb".as_ptr().cast(), 3, 8_000, 160, &mut link) },
        URP_AST_NOT_READY
    );
    assert!(link.is_null());
    assert_eq!(
        // SAFETY: complete arguments remain live for this setup call.
        unsafe { link_prepare(driver, c"other".as_ptr().cast(), 5, 8_000, 160, &mut link) },
        URP_AST_CHANNEL_NOT_FOUND
    );
    // Disabled processing is rejected before graph construction.
    provider_support::set_failure(provider_support::FAIL_GRAPH);
    assert_eq!(
        // SAFETY: complete arguments remain live for this setup call.
        unsafe { link_prepare(driver, c"usb".as_ptr().cast(), 3, 8_000, 160, &mut link) },
        URP_AST_NOT_READY
    );
    provider_support::clear_failure();
    // SAFETY: null destruction is explicitly harmless and driver is consumed once.
    unsafe {
        link_destroy(ptr::null_mut());
        driver_destroy(driver);
    }

    let tuning_inactive = b"[usb]\nchannel_enabled = no\n[link]\nenabled = yes\n";
    let driver = create_driver(&context, tuning_inactive);
    link = ptr::null_mut();
    assert_eq!(
        // SAFETY: channel_enabled selects only the tuning target; the enabled link remains usable.
        unsafe { link_prepare(driver, c"usb".as_ptr().cast(), 3, 8_000, 160, &mut link) },
        URP_AST_OK
    );
    assert!(!link.is_null());
    // SAFETY: link and driver are paired live handles consumed exactly once.
    unsafe {
        link_destroy(link);
        driver_destroy(driver);
    }
}

#[test]
fn common_abi_and_text_validation_reject_invalid_inputs() {
    // SAFETY: null is deliberately supplied to test rejection before dereference.
    let null_abi = unsafe { copy_abi::<UrpAstOperations>(ptr::null()) };
    assert!(matches!(null_abi, Err(Status::InvalidArgument)));
    let context = RefCell::new(Calls::default());
    let aligned = raw_operations(&context);
    // SAFETY: the deliberately misaligned address must be rejected before dereference.
    let misaligned_abi = unsafe {
        copy_abi::<UrpAstOperations>(ptr::from_ref(&aligned).cast::<u8>().wrapping_add(1).cast())
    };
    assert!(matches!(misaligned_abi, Err(Status::InvalidArgument)));
    let mut raw = raw_operations(&context);
    raw.struct_size -= 1;
    // SAFETY: the readable table advertises a deliberately short size.
    let short_abi = unsafe { copy_abi(ptr::from_ref(&raw)) };
    assert!(matches!(short_abi, Err(Status::IncompatibleAbi)));
    raw.struct_size += 1;
    raw.abi_version += 1;
    // SAFETY: the readable table advertises a deliberately wrong version.
    let wrong_abi = unsafe { copy_abi(ptr::from_ref(&raw)) };
    assert!(matches!(wrong_abi, Err(Status::IncompatibleAbi)));
    // SAFETY: null is deliberately supplied to test rejection before dereference.
    let null_text = unsafe { required_utf8(ptr::null(), 1) };
    assert!(matches!(null_text, Err(Status::InvalidArgument)));
    // SAFETY: zero length is deliberately supplied and rejected before dereference.
    let empty_text = unsafe { required_utf8(c"".as_ptr().cast(), 0) };
    assert!(matches!(empty_text, Err(Status::InvalidArgument)));
    let invalid = [0xff];
    // SAFETY: invalid is a live one-byte span for the duration of the call.
    let invalid_text = unsafe { required_utf8(invalid.as_ptr(), 1) };
    assert!(matches!(invalid_text, Err(Status::InvalidArgument)));
    // SAFETY: the C literal contains the requested two readable UTF-8 bytes.
    let valid = unsafe { required_utf8(c"ok".as_ptr().cast(), 2) }.unwrap();
    assert_eq!(valid, "ok");
    assert!(matches!(
        output_pointer(ptr::null_mut()),
        Err(Status::InvalidArgument)
    ));
    let mut output = 1_usize as *mut c_void;
    output_pointer(ptr::from_mut(&mut output)).unwrap();
    assert!(output.is_null());
}

#[test]
fn operations_translate_callbacks_and_reject_bad_dtmf_results() {
    let context = RefCell::new(Calls {
        now_ms: 123,
        ..Calls::default()
    });
    let operations = operations(&context);
    let frame = ControllerPcmFrame::app_rpt([7; usbradioplus_asl3::APP_RPT_FRAME_SAMPLES]);
    operations
        .queue_voice(11, frame.samples(), frame.mode().sample_rate_hz())
        .unwrap();
    operations.queue_control(11, 2, 3, 4).unwrap();
    operations.queue_text(11, "status").unwrap();
    operations.log(URP_AST_LOG_INFO, "message");
    assert_eq!(operations.monotonic_milliseconds(), 123);
    let mut samples = frame.samples().to_vec();
    assert_eq!(
        operations.analyze_dtmf(11, &mut samples, 8_000).unwrap(),
        None
    );
    {
        let mut calls = context.borrow_mut();
        calls.dtmf_return = 1;
        calls.dtmf.event_kind = URP_AST_DTMF_BEGIN;
        calls.dtmf.digit = b'5';
        calls.mute_sample = true;
    }
    assert_eq!(
        operations.analyze_dtmf(11, &mut samples, 8_000).unwrap(),
        Some(DtmfEvent {
            kind: DtmfEventKind::Begin,
            digit: b'5'
        })
    );
    assert_eq!(samples[0], 0);
    context.borrow_mut().dtmf.event_kind = URP_AST_DTMF_END;
    assert_eq!(
        operations
            .analyze_dtmf(11, &mut samples, 8_000)
            .unwrap()
            .unwrap()
            .kind,
        DtmfEventKind::End
    );
    context.borrow_mut().dtmf.event_kind = 99;
    assert!(matches!(
        operations.analyze_dtmf(11, &mut samples, 8_000),
        Err(Status::AsteriskFailure)
    ));
    context.borrow_mut().dtmf.struct_size = 0;
    assert!(matches!(
        operations.analyze_dtmf(11, &mut samples, 8_000),
        Err(Status::AsteriskFailure)
    ));
    context.borrow_mut().dtmf_return = -1;
    assert!(matches!(
        operations.analyze_dtmf(11, &mut samples, 8_000),
        Err(Status::AsteriskFailure)
    ));
    context.borrow_mut().fail_queue = true;
    assert!(matches!(
        operations.queue_voice(11, frame.samples(), 8_000),
        Err(Status::AsteriskFailure)
    ));
    assert!(matches!(
        operations.queue_control(11, 1, 0, 0),
        Err(Status::AsteriskFailure)
    ));
    assert!(matches!(
        operations.queue_text(11, "x"),
        Err(Status::AsteriskFailure)
    ));
    let calls = context.borrow();
    assert_eq!(calls.voice[0], (160, 8_000));
    assert_eq!(calls.control[0], (2, 3, 4));
    assert_eq!(calls.text[0], "status");
    assert_eq!(calls.logs[0], (URP_AST_LOG_INFO, "message".to_owned()));
}

#[test]
fn operations_require_every_callback() {
    let context = RefCell::new(Calls::default());
    let valid = raw_operations(&context);
    let missing = [
        UrpAstOperations {
            queue_voice: None,
            ..valid
        },
        UrpAstOperations {
            queue_control: None,
            ..valid
        },
        UrpAstOperations {
            queue_text: None,
            ..valid
        },
        UrpAstOperations {
            analyze_dtmf: None,
            ..valid
        },
        UrpAstOperations {
            monotonic_milliseconds: None,
            ..valid
        },
        UrpAstOperations { log: None, ..valid },
    ];
    for raw in missing {
        // SAFETY: each local table is readable and copied during the call.
        let result = unsafe { Operations::from_raw(ptr::from_ref(&raw)) };
        assert!(matches!(result, Err(Status::IncompatibleAbi)));
    }
}

#[test]
fn transport_configuration_is_explicit_and_bounded() {
    let app = reserve_args(URP_AST_TRANSPORT_APP_RPT);
    let (mode, configuration) = controller_configuration(app.transport).unwrap();
    assert_eq!(mode, AsteriskPcmMode::AppRpt);
    assert!(matches!(
        configuration,
        ControllerConfiguration::AppRpt {
            handoff_slots: 3,
            ..
        }
    ));
    let advanced = reserve_args(URP_AST_TRANSPORT_RPT_ADVANCED);
    let (mode, configuration) = controller_configuration(advanced.transport).unwrap();
    assert_eq!(mode, AsteriskPcmMode::Advanced);
    assert_eq!(
        configuration,
        ControllerConfiguration::RptAdvanced { handoff_slots: 3 }
    );
    let unknown = reserve_args(99);
    assert!(matches!(
        controller_configuration(unknown.transport),
        Err(Status::InvalidArgument)
    ));
}

#[test]
fn exported_operations_contain_panics_and_reject_null_handles() {
    assert_eq!(ffi_status(|| panic!("contained")), URP_AST_INTERNAL_FAILURE);
    // SAFETY: all calls deliberately pass null handles to exercise validation.
    unsafe {
        assert_eq!(
            driver_reload(ptr::null_mut(), ptr::null(), 0, ptr::null(), 0),
            -1
        );
        assert_eq!(driver_reload_finish(ptr::null_mut(), 0), -1);
        assert_eq!(
            driver_channel_name(ptr::null_mut(), 0, ptr::null_mut(), 0, ptr::null_mut()),
            -1
        );
        assert_eq!(
            driver_active_channel(ptr::null_mut(), ptr::null_mut(), 0, ptr::null_mut()),
            -1
        );
        assert_eq!(
            driver_set_active_channel(ptr::null_mut(), ptr::null(), 0),
            -1
        );
        assert_eq!(channel_start(ptr::null_mut()), -1);
        assert_eq!(channel_stop(ptr::null_mut()), -1);
        assert_eq!(channel_reload_prepare(ptr::null_mut()), -1);
        assert_eq!(channel_reload_activate(ptr::null_mut()), -1);
        assert_eq!(channel_reload_finish(ptr::null_mut(), 0), -1);
        assert_eq!(channel_write_voice(ptr::null_mut(), ptr::null(), 0), -1);
        assert_eq!(channel_write_text(ptr::null_mut(), ptr::null(), 0), -1);
        assert_eq!(channel_set_transmit(ptr::null_mut(), 0, 0), -1);
        assert_eq!(channel_set_dtmf(ptr::null_mut(), 0), -1);
        assert_eq!(channel_set_echo(ptr::null_mut(), 0), -1);
        assert_eq!(
            channel_get_jitter_config(ptr::null_mut(), ptr::null_mut()),
            -1
        );
        assert_eq!(channel_command(ptr::null_mut(), ptr::null_mut()), -1);
        assert_eq!(channel_get_status(ptr::null_mut(), ptr::null_mut()), -1);
        assert_eq!(channel_service(ptr::null_mut()), -1);
        driver_destroy(ptr::null_mut());
        channel_destroy(ptr::null_mut());
    }
}

#[test]
fn create_validates_configuration_before_provider_manifest() {
    let context = RefCell::new(Calls::default());
    let operations = raw_operations(&context);
    let providers = UrpAstProviderManifest {
        struct_size: size_of::<UrpAstProviderManifest>() as u32,
        abi_version: ABI_VERSION,
        ffmpeg: ptr::null(),
        rnnoise: ptr::null(),
        ring: ptr::null(),
        radio: ptr::null(),
        samplerate: ptr::null(),
        audio: ptr::null(),
        gpio: ptr::null(),
    };
    let invalid = b"[general]\n";
    let source = b"usbradioplus.conf";
    let plugin = b"agc.so";
    let mut args = UrpAstDriverCreateArgs {
        struct_size: size_of::<UrpAstDriverCreateArgs>() as u32,
        abi_version: ABI_VERSION,
        config_source: source.as_ptr(),
        config_source_length: source.len() as u32,
        config_text: invalid.as_ptr(),
        config_text_length: invalid.len() as u32,
        agc_plugin_path: plugin.as_ptr(),
        agc_plugin_path_length: plugin.len() as u32,
        operations: ptr::from_ref(&operations),
        providers: ptr::from_ref(&providers),
    };
    let mut output = 1_usize as *mut c_void;
    // SAFETY: every non-null pointer refers to live test storage.
    assert_eq!(unsafe { driver_create(&args, &mut output) }, -3);
    assert!(output.is_null());
    assert_eq!(context.borrow().logs.len(), 1);

    let valid = b"[usb]\n";
    args.config_text = valid.as_ptr();
    args.config_text_length = valid.len() as u32;
    // SAFETY: every non-null pointer refers to live test storage.
    assert_eq!(unsafe { driver_create(&args, &mut output) }, -2);
    assert!(output.is_null());

    // SAFETY: output is deliberately null to exercise validation.
    assert_eq!(unsafe { driver_create(&args, ptr::null_mut()) }, -1);
}

#[test]
fn every_status_has_the_documented_integer_value() {
    assert_eq!(Status::InvalidArgument.code(), -1);
    assert_eq!(Status::IncompatibleAbi.code(), -2);
    assert_eq!(Status::InvalidConfiguration.code(), -3);
    assert_eq!(Status::ChannelNotFound.code(), -4);
    assert_eq!(Status::ChannelBusy.code(), -5);
    assert_eq!(Status::SetupFailed.code(), -6);
    assert_eq!(Status::NotReady.code(), -7);
    assert_eq!(Status::AsteriskFailure.code(), -8);
    assert_eq!(Status::InternalFailure.code(), -9);
}

#[test]
fn typed_tuning_and_jitter_helpers_are_strict() {
    assert_eq!(
        URP_AST_CTCSS_CALIBRATION_TARGET,
        usbradioplus_radio::CTCSS_CALIBRATION_TARGET
    );
    assert_eq!(
        mixer(URP_AST_MIXER_RECEIVE).unwrap(),
        HardwareMixer::Receive
    );
    assert_eq!(
        mixer(URP_AST_MIXER_TRANSMIT_A).unwrap(),
        HardwareMixer::TransmitA
    );
    assert_eq!(
        mixer(URP_AST_MIXER_TRANSMIT_B).unwrap(),
        HardwareMixer::TransmitB
    );
    assert!(matches!(mixer(0), Err(Status::InvalidArgument)));
    assert!(!switch(0).unwrap());
    assert!(switch(1).unwrap());
    assert!(matches!(switch(2), Err(Status::InvalidArgument)));
    let mut image = EepromImage {
        checksum_valid: true,
        magic_valid: false,
        words: [0; 64],
    };
    assert_eq!(eeprom_flags(&image), URP_AST_EEPROM_CHECKSUM_VALID);
    image.magic_valid = true;
    assert_eq!(
        eeprom_flags(&image),
        URP_AST_EEPROM_CHECKSUM_VALID | URP_AST_EEPROM_MAGIC_VALID
    );

    let fixed = jitter_config(&AsteriskConfig::default());
    assert_eq!(fixed.implementation, URP_AST_JITTER_FIXED);
    assert_eq!(fixed.maximum_size_ms, 200);
    let adaptive = jitter_config(&AsteriskConfig {
        jitter_buffer_implementation: JitterBufferImplementation::Adaptive,
        ..AsteriskConfig::default()
    });
    assert_eq!(adaptive.implementation, URP_AST_JITTER_ADAPTIVE);

    let mut output = UrpAstJitterConfig {
        struct_size: size_of::<UrpAstJitterConfig>() as u32,
        abi_version: ABI_VERSION,
        enabled: 0,
        maximum_size_ms: 0,
        resync_threshold_ms: 0,
        implementation: 0,
        logging_enabled: 0,
        force_enabled: 0,
        target_extra_ms: 0,
        video_sync_enabled: 0,
    };
    // SAFETY: output is initialized writable storage for this exact ABI type.
    unsafe { write_abi(&mut output, fixed) }.unwrap();
    assert_eq!(output.maximum_size_ms, 200);
    output.struct_size -= 1;
    // SAFETY: the initialized short header is deliberately rejected before writing.
    let result = unsafe { write_abi(&mut output, fixed) };
    assert!(matches!(result, Err(Status::IncompatibleAbi)));
}

#[test]
fn channel_enabled_selects_tuning_target_without_filtering_channels() {
    let configuration = DriverConfiguration::parse(
        "usbradioplus.conf",
        "[first]\nchannel_enabled = yes\n[disabled]\nchannel_enabled = no\n[last]\nchannel_enabled = yes\n",
    )
    .unwrap();
    assert_eq!(configuration.channels().len(), 3);
    assert_eq!(
        configured_active_channel(&configuration).as_deref(),
        Some("last")
    );

    let mut reservations = ReservationState::new(configured_active_channel(&configuration));
    assert!(reservations.reserve("disabled".to_owned()));
    assert_eq!(reservations.active.as_deref(), Some("disabled"));
    assert!(reservations.reserve("last".to_owned()));
    assert_eq!(reservations.active.as_deref(), Some("last"));
    assert!(reservations.reserve("other".to_owned()));
    reservations.release("other");
    assert_eq!(reservations.active.as_deref(), Some("last"));
    reservations.release("last");
    assert_eq!(reservations.active, None);
    reservations.reload(Some("disabled".to_owned()));
    assert_eq!(reservations.active.as_deref(), Some("disabled"));
    reservations.reload(Some("missing".to_owned()));
    assert_eq!(reservations.active.as_deref(), Some("disabled"));
}

#[test]
fn byte_output_supports_size_query_and_bounded_copy() {
    let mut length = 0;
    // SAFETY: length is writable and null with zero capacity requests size only.
    unsafe { write_bytes(b"radio", ptr::null_mut(), 0, &mut length) }.unwrap();
    assert_eq!(length, 5);
    let mut output = [0_u8; 5];
    // SAFETY: output and length are live writable storage of advertised size.
    unsafe { write_bytes(b"radio", output.as_mut_ptr(), 5, &mut length) }.unwrap();
    assert_eq!(&output, b"radio");
    // SAFETY: the deliberately short output is rejected before copying.
    let short = unsafe { write_bytes(b"radio", output.as_mut_ptr(), 4, &mut length) };
    assert!(matches!(short, Err(Status::InvalidArgument)));
    // SAFETY: nonzero capacity with no output pointer is deliberately rejected.
    let missing = unsafe { write_bytes(b"radio", ptr::null_mut(), 1, &mut length) };
    assert!(matches!(missing, Err(Status::InvalidArgument)));
    // SAFETY: missing length storage is rejected before it can be written.
    let missing_length = unsafe { write_bytes(b"radio", ptr::null_mut(), 0, ptr::null_mut()) };
    assert!(matches!(missing_length, Err(Status::InvalidArgument)));
    // SAFETY: deliberately misaligned length storage is rejected before dereference.
    let misaligned_length = unsafe {
        write_bytes(
            b"radio",
            ptr::null_mut(),
            0,
            ptr::from_mut(&mut length).cast::<u8>().add(1).cast(),
        )
    };
    assert!(matches!(misaligned_length, Err(Status::InvalidArgument)));
}

#[test]
fn ordinary_hardware_inputs_use_the_established_asterisk_text_format() {
    assert_eq!(
        hardware_input_text(HardwareInputEvent {
            input: HardwareInput::Cm119(usbradioplus_asl3::GpioPin::new(3).unwrap()),
            active: true,
        }),
        "GPIO3 1\n"
    );
    assert_eq!(
        hardware_input_text(HardwareInputEvent {
            input: HardwareInput::Parallel(12),
            active: false,
        }),
        "PP12 0\n"
    );
}

#[test]
fn resolved_configuration_warnings_are_logged_on_create_and_reload() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let driver = create_driver(
        &context,
        b"[hardware]\nhardware_input_gain_db = loud\n[usb]\n",
    );
    assert!(context.borrow().logs.contains(&(
        URP_AST_LOG_WARNING,
        "usbradioplus.conf [hardware]: hardware_input_gain_db=\"loud\" requires a finite number from -30 through 30; using 0".to_owned(),
    )));

    context.borrow_mut().logs.clear();
    let source = b"reload.conf";
    let config = b"[mystery site]\nvalue = one\n[local]\nnot_a_setting = value\n[usb]\n";
    // SAFETY: the driver and both byte spans remain live for this synchronous call.
    assert_eq!(
        // SAFETY: all pointers remain live for this synchronous reload.
        unsafe {
            driver_reload(
                driver,
                source.as_ptr(),
                source.len() as u32,
                config.as_ptr(),
                config.len() as u32,
            )
        },
        URP_AST_OK
    );
    assert!(context.borrow().logs.contains(&(
        URP_AST_LOG_WARNING,
        "reload.conf [local]: not_a_setting=\"value\" is unknown and was ignored; using ignored"
            .to_owned(),
    )));
    assert!(
        context.borrow().logs.contains(&(
            URP_AST_LOG_WARNING,
            "reload.conf [mystery site]: section=\"mystery site\" is not recognized; using ignored"
                .to_owned(),
        ))
    );
    // SAFETY: no live channel depends on this valid staged generation.
    assert_eq!(unsafe { driver_reload_finish(driver, 1) }, URP_AST_OK);

    // SAFETY: this driver was created above and has not yet been destroyed.
    unsafe { driver_destroy(driver) };
}

#[test]
fn complete_driver_channel_lifecycle_crosses_the_c_boundary() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    const CONFIG: &[u8] = b"[usb]\n\
        [hardware]\n\
        hardware_eeprom_enabled = yes\n\
        hardware_gpio_1_mode = out0\n\
        hardware_gpio_2_mode = in\n\
        hardware_clip_led_gpio = 1\n\
        hardware_parallel_port_device = /dev/parport0\n\
        hardware_parallel_pin_2_assignment = out0\n\
        hardware_parallel_pin_10_assignment = in\n";
    let context = RefCell::new(Calls::default());
    let driver = create_driver(&context, CONFIG);

    let mut length = 0;
    // SAFETY: driver is live and length is writable.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_channel_name(driver, 0, ptr::null_mut(), 0, &mut length) },
        URP_AST_OK
    );
    assert_eq!(length, 3);
    let mut name = [0; 3];
    // SAFETY: driver is live and name has its advertised capacity.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_channel_name(driver, 0, name.as_mut_ptr(), 3, &mut length) },
        URP_AST_OK
    );
    assert_eq!(&name, b"usb");
    // SAFETY: driver is live; index one is deliberately out of range.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_channel_name(driver, 1, ptr::null_mut(), 0, &mut length) },
        URP_AST_CHANNEL_NOT_FOUND
    );
    // SAFETY: no channel is reserved yet.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_active_channel(driver, ptr::null_mut(), 0, &mut length) },
        URP_AST_CHANNEL_NOT_FOUND
    );

    // SAFETY: driver remains live until the matching destruction below.
    let channel = unsafe { reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED) };
    let mut duplicate = ptr::null_mut();
    let args = reserve_args(URP_AST_TRANSPORT_RPT_ADVANCED);
    // SAFETY: all arguments are live; the existing reservation causes rejection.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_reserve(driver, &args, &mut duplicate) },
        URP_AST_CHANNEL_BUSY
    );
    assert!(duplicate.is_null());
    // SAFETY: the selected reservation now exists.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_active_channel(driver, name.as_mut_ptr(), 3, &mut length) },
        URP_AST_OK
    );
    // SAFETY: driver and channel-name bytes are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_set_active_channel(driver, b"USB".as_ptr(), 3) },
        URP_AST_OK
    );
    // SAFETY: driver and missing name bytes are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { driver_set_active_channel(driver, b"none".as_ptr(), 4) },
        URP_AST_CHANNEL_NOT_FOUND
    );

    let mut status = UrpAstChannelStatus {
        struct_size: size_of::<UrpAstChannelStatus>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstChannelStatus::default()
    };
    // SAFETY: channel and initialized output storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_get_status(channel, &mut status) },
        URP_AST_OK
    );
    assert_eq!(status.running, 0);
    let mut prestart = command(URP_AST_COMMAND_GET_MIXER, URP_AST_MIXER_RECEIVE, 0);
    // SAFETY: commands requiring opened hardware reject this prepared channel.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_command(channel, &mut prestart) },
        URP_AST_NOT_READY
    );
    // SAFETY: control requiring opened hardware rejects this prepared channel.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_set_transmit(channel, 1, 0) },
        URP_AST_NOT_READY
    );
    // SAFETY: prepared media owns both configuration switches.
    assert_eq!(unsafe { channel_set_dtmf(channel, 1) }, URP_AST_OK);
    // SAFETY: prepared media owns both configuration switches.
    assert_eq!(unsafe { channel_set_echo(channel, 1) }, URP_AST_OK);

    let mut jitter = UrpAstJitterConfig {
        struct_size: size_of::<UrpAstJitterConfig>() as u32,
        abi_version: ABI_VERSION,
        enabled: 0,
        maximum_size_ms: 0,
        resync_threshold_ms: 0,
        implementation: 0,
        logging_enabled: 0,
        force_enabled: 0,
        target_extra_ms: 0,
        video_sync_enabled: 0,
    };
    // SAFETY: channel and initialized output storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_get_jitter_config(channel, &mut jitter) },
        URP_AST_OK
    );
    assert_eq!(jitter.implementation, URP_AST_JITTER_FIXED);

    let samples = [1_i16; usbradioplus_asl3::ADVANCED_FRAME_SAMPLES];
    // SAFETY: channel and complete advanced frame are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_voice(channel, samples.as_ptr(), samples.len() as u32) },
        URP_AST_OK
    );
    // SAFETY: channel is live and the short count is rejected before reading.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_voice(channel, samples.as_ptr(), 1) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: a null span with the exact count is rejected before reading.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_voice(channel, ptr::null(), samples.len() as u32) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: the serialized service endpoint is exclusively owned by this test.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);

    // Reload before hardware binding exercises prepared-generation replacement.
    let reloaded = [
        CONFIG,
        b"[asterisk]\nasterisk_jitter_buffer_implementation = adaptive\n",
    ]
    .concat();
    // A staged candidate is invisible to ordinary service and jitter queries.
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this transaction.
        unsafe { stage_reload(driver, &reloaded) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel and initialized output storage remain valid.
        unsafe { channel_get_jitter_config(channel, &mut jitter) },
        URP_AST_OK
    );
    assert_eq!(jitter.implementation, URP_AST_JITTER_FIXED);
    assert_eq!(
        // SAFETY: ordinary service owns this live channel control endpoint.
        unsafe { channel_service(channel) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel is serialized and the candidate is staged.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the prepared channel is exclusively serialized here.
        unsafe { channel_reload_activate(channel) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the sole live channel has adopted the staged generation.
        unsafe { driver_reload_finish(driver, 1) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: commit drops only detached rollback ownership.
        unsafe { channel_reload_finish(channel, 1) },
        URP_AST_OK
    );
    // SAFETY: the query only reads the generation adopted above.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_get_jitter_config(channel, &mut jitter) },
        URP_AST_OK
    );
    assert_eq!(jitter.implementation, URP_AST_JITTER_ADAPTIVE);

    // SAFETY: the prepared channel owns its provider endpoints exclusively.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);
    // SAFETY: a repeated start is supported by the opened provider.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);
    // SAFETY: channel is live and provider callbacks completed during start.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);

    for (target, value) in [
        (URP_AST_MIXER_RECEIVE, 401),
        (URP_AST_MIXER_TRANSMIT_A, 402),
        (URP_AST_MIXER_TRANSMIT_B, 403),
    ] {
        let mut set = command(URP_AST_COMMAND_SET_MIXER, target, value);
        // SAFETY: channel and command storage are live.
        assert_eq!(unsafe { channel_command(channel, &mut set) }, URP_AST_OK);
        let mut get = command(URP_AST_COMMAND_GET_MIXER, target, 0);
        // SAFETY: channel and command storage are live.
        assert_eq!(unsafe { channel_command(channel, &mut get) }, URP_AST_OK);
        assert_eq!(get.value, value);
    }
    for value in [0, 1] {
        let mut tone = command(URP_AST_COMMAND_SET_TEST_TONE, 0, value);
        // SAFETY: channel and command storage are live.
        assert_eq!(unsafe { channel_command(channel, &mut tone) }, URP_AST_OK);
        let mut inhibit = command(URP_AST_COMMAND_SET_CTCSS_INHIBIT, 0, value);
        // SAFETY: channel and command storage are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_command(channel, &mut inhibit) },
            URP_AST_OK
        );
        let mut override_ = command(URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE, 0, value);
        // SAFETY: channel and command storage are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_command(channel, &mut override_) },
            URP_AST_OK
        );
    }
    let mut read = command(URP_AST_COMMAND_READ_EEPROM, 0, 0);
    // SAFETY: channel and command storage are live.
    assert_eq!(unsafe { channel_command(channel, &mut read) }, URP_AST_OK);
    assert_ne!(read.flags, 0);
    read.command = URP_AST_COMMAND_WRITE_EEPROM;
    // SAFETY: channel and command storage are live.
    assert_eq!(unsafe { channel_command(channel, &mut read) }, URP_AST_OK);
    let mut save = command(URP_AST_COMMAND_SAVE_TUNING_EEPROM, 0, 0);
    // SAFETY: channel and command storage are live.
    assert_eq!(unsafe { channel_command(channel, &mut save) }, URP_AST_OK);
    assert_eq!(save.value, 1);

    // SAFETY: channel is live; forced tone is invalid on an unkey request.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_set_transmit(channel, 0, 1_000) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: channel is live; this is not a configured CTCSS frequency.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_set_transmit(channel, 1, 1) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: channel is live; both valid transmit transitions are serialized.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_set_transmit(channel, 1, 1_000) },
        URP_AST_OK
    );
    // SAFETY: channel is live; both valid transmit transitions are serialized.
    assert_eq!(unsafe { channel_set_transmit(channel, 0, 0) }, URP_AST_OK);

    for text in [b"RXCTCSS 0".as_slice(), b"TXCTCSS 1", b"GPIO 1 1"] {
        // SAFETY: channel and control-text span are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_write_text(channel, text.as_ptr(), text.len() as u32) },
            URP_AST_OK
        );
    }
    // SAFETY: invalid text is rejected without changing controller state.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_text(channel, b"BOGUS".as_ptr(), 5) },
        URP_AST_INVALID_ARGUMENT
    );
    // SAFETY: this syntactically valid request targets an unconfigured GPIO.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_text(channel, b"GPIO 8 1".as_ptr(), 8) },
        URP_AST_SETUP_FAILED
    );
    // SAFETY: channel and initialized output storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_get_status(channel, &mut status) },
        URP_AST_OK
    );
    assert_eq!(status.running, 1);
    assert_eq!(status.native_sample_rate_hz, 48_000.0);
    assert_eq!(status.receive_mixer_level, 401);

    // Reload while running exercises provider replacement and retained state.
    // SAFETY: this test serializes the complete one-channel transaction.
    assert_eq!(
        // SAFETY: both handles remain live and exclusively serialized here.
        unsafe { reload_channels(driver, &[channel], CONFIG) },
        URP_AST_OK
    );
    // SAFETY: service owns pending-delivery work exclusively.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);
    // SAFETY: repeated stopping is explicitly harmless.
    assert_eq!(unsafe { channel_stop(channel) }, URP_AST_OK);
    // SAFETY: repeated stopping is explicitly harmless.
    assert_eq!(unsafe { channel_stop(channel) }, URP_AST_OK);
    // SAFETY: channel and driver were returned by their paired constructors.
    unsafe {
        channel_destroy(channel);
        driver_destroy(driver);
    }
}

#[test]
fn app_rpt_delivery_translates_every_controller_action() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let driver = create_driver(&context, b"[usb]\n");
    // SAFETY: driver remains live until the paired destruction below.
    let channel = unsafe { reserve_channel(driver, URP_AST_TRANSPORT_APP_RPT) };
    let updated = b"[usb]\n[diagnostics]\ndiagnostic_trace_level = 1\n";
    // SAFETY: this test serializes the complete one-channel transaction.
    assert_eq!(
        // SAFETY: both handles remain live and exclusively serialized here.
        unsafe { reload_channels(driver, &[channel], updated) },
        URP_AST_OK
    );
    // SAFETY: service drains the already-adopted app_rpt generation.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);
    // SAFETY: this test is the sole serialized control owner.
    let (channel_ref, control) = unsafe { channel_control(channel) }.unwrap();
    let tone = CtcssTone::from_tenths_hz(1_000).unwrap();
    for action in [
        DeliveryAction::ReceiverKey { ctcss: None },
        DeliveryAction::ReceiverKey { ctcss: Some(tone) },
        DeliveryAction::ReceiverUnkey,
        DeliveryAction::TransmitCtcssReady(tone),
        DeliveryAction::VoterRssi(42),
    ] {
        deliver(channel_ref, control, action).unwrap();
    }

    control.voice = ControllerPcmFrame::app_rpt([7; usbradioplus_asl3::APP_RPT_FRAME_SAMPLES]);
    control.controller().unwrap().set_dtmf_detection(false);
    deliver(channel_ref, control, DeliveryAction::Voice).unwrap();
    control.controller().unwrap().set_dtmf_detection(true);
    deliver(channel_ref, control, DeliveryAction::Voice).unwrap();

    {
        let mut calls = context.borrow_mut();
        calls.dtmf_return = 1;
        calls.dtmf.struct_size = size_of::<UrpAstDtmfResult>() as u32;
        calls.dtmf.event_kind = URP_AST_DTMF_BEGIN;
        calls.dtmf.digit = b'u';
    }
    deliver_voice(channel_ref, control).unwrap();
    context.borrow_mut().dtmf.digit = b'5';
    context.borrow_mut().now_ms = 100;
    deliver_voice(channel_ref, control).unwrap();
    context.borrow_mut().dtmf.digit = b'6';
    context.borrow_mut().now_ms = 120;
    deliver_voice(channel_ref, control).unwrap();
    context.borrow_mut().dtmf.event_kind = URP_AST_DTMF_END;
    context.borrow_mut().dtmf.digit = b'5';
    context.borrow_mut().now_ms = 140;
    deliver_voice(channel_ref, control).unwrap();
    deliver_dtmf_action(operations(&context), 0, &[1], 8_000, DtmfAction::PassVoice).unwrap();

    // The adapter writes fixed app_rpt frames through its own converter path.
    let samples = [2_i16; usbradioplus_asl3::APP_RPT_FRAME_SAMPLES];
    // SAFETY: channel and complete app_rpt frame are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_write_voice(channel, samples.as_ptr(), samples.len() as u32) },
        URP_AST_OK
    );
    let mut status = UrpAstChannelStatus {
        struct_size: size_of::<UrpAstChannelStatus>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstChannelStatus::default()
    };
    // SAFETY: channel and initialized output storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_get_status(channel, &mut status) },
        URP_AST_OK
    );
    assert_eq!(status.transport, URP_AST_TRANSPORT_APP_RPT);

    let calls = context.borrow();
    assert!(calls.voice.iter().any(|voice| *voice == (160, 8_000)));
    assert!(
        calls
            .control
            .iter()
            .any(|control| control.0 == URP_AST_CONTROL_DTMF_BEGIN)
    );
    assert!(
        calls
            .control
            .iter()
            .any(|control| control.0 == URP_AST_CONTROL_DTMF_END)
    );
    assert!(
        calls
            .control
            .iter()
            .any(|control| control.0 == URP_AST_CONTROL_NULL)
    );
    drop(calls);
    // SAFETY: channel and driver were returned by their paired constructors.
    unsafe {
        channel_destroy(channel);
        driver_destroy(driver);
    }
}

#[test]
fn provider_failures_are_reported_and_reload_keeps_the_live_generation() {
    const CONFIG: &[u8] = b"[usb]\n\
        [hardware]\n\
        hardware_eeprom_enabled = yes\n\
        hardware_gpio_1_mode = out0\n\
        hardware_gpio_2_mode = in\n\
        hardware_parallel_port_device = /dev/parport0\n\
        hardware_parallel_pin_2_assignment = out0\n\
        hardware_parallel_pin_10_assignment = in\n";
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    provider_support::set_inputs(0, 0);
    let context = RefCell::new(Calls::default());

    let (status, failed_driver) = create_driver_result(&context, CONFIG, b"bad\npath");
    assert_eq!(status, URP_AST_SETUP_FAILED);
    assert!(failed_driver.is_null());

    let driver = create_driver(&context, CONFIG);
    let invalid = b"[general]\n";
    let source = b"reload.conf";
    // SAFETY: driver and both byte spans are live; configuration is deliberately invalid.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe {
            driver_reload(
                driver,
                source.as_ptr(),
                source.len() as u32,
                invalid.as_ptr(),
                invalid.len() as u32,
            )
        },
        URP_AST_INVALID_CONFIGURATION
    );
    for failure in [
        provider_support::FAIL_PREFLIGHT,
        provider_support::FAIL_GRAPH,
    ] {
        provider_support::set_failure(failure);
        let args = reserve_args(URP_AST_TRANSPORT_RPT_ADVANCED);
        let mut failed_channel = ptr::null_mut();
        // SAFETY: driver and complete reservation arguments are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_reserve(driver, &args, &mut failed_channel) },
            URP_AST_SETUP_FAILED
        );
        assert!(failed_channel.is_null());
        provider_support::clear_failure();
    }

    // A failed open consumes the prepared generation; a later start is not ready.
    // SAFETY: driver is live until paired destruction below.
    let failed_open = unsafe { reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED) };
    provider_support::set_failure(provider_support::FAIL_OPEN);
    // SAFETY: channel is live and exclusively controlled by this test.
    assert_eq!(unsafe { channel_start(failed_open) }, URP_AST_SETUP_FAILED);
    provider_support::clear_failure();
    // SAFETY: both prepared resources were consumed by the failed open.
    assert_eq!(unsafe { channel_start(failed_open) }, URP_AST_NOT_READY);
    // SAFETY: channel was returned by channel_reserve.
    unsafe { channel_destroy(failed_open) };

    // SAFETY: the released reservation may be acquired again.
    let channel = unsafe { reserve_channel(driver, URP_AST_TRANSPORT_RPT_ADVANCED) };
    provider_support::set_failure(provider_support::FAIL_START);
    // SAFETY: channel is live and exclusively controlled by this test.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_SETUP_FAILED);
    provider_support::clear_failure();
    // SAFETY: the opened station remains available after a provider start failure.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);
    // SAFETY: stop prepares the already-opened hardware branch for another start.
    assert_eq!(unsafe { channel_stop(channel) }, URP_AST_OK);
    provider_support::set_failure(provider_support::FAIL_START);
    // SAFETY: repeated provider start failures are reported identically.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_SETUP_FAILED);
    provider_support::clear_failure();
    // SAFETY: restore the live state for subsequent controls.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);

    provider_support::set_failure(provider_support::FAIL_STOP);
    // SAFETY: provider stop failures are mapped at the C boundary.
    assert_eq!(unsafe { channel_stop(channel) }, URP_AST_SETUP_FAILED);
    provider_support::clear_failure();
    // SAFETY: recover any partially stopped provider state before reload tests.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);

    let mut status_output = UrpAstChannelStatus {
        struct_size: size_of::<UrpAstChannelStatus>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstChannelStatus::default()
    };
    for failure in [
        provider_support::FAIL_STATISTICS,
        provider_support::FAIL_TIMING,
        provider_support::FAIL_SNAPSHOT,
    ] {
        provider_support::set_failure(failure);
        // SAFETY: channel and initialized output storage are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_get_status(channel, &mut status_output) },
            URP_AST_SETUP_FAILED
        );
        provider_support::clear_failure();
    }

    provider_support::set_failure(provider_support::FAIL_MIXER);
    let mut mixer = command(URP_AST_COMMAND_SET_MIXER, URP_AST_MIXER_RECEIVE, 222);
    // SAFETY: channel and command storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_command(channel, &mut mixer) },
        URP_AST_SETUP_FAILED
    );
    provider_support::clear_failure();
    provider_support::set_failure(provider_support::FAIL_EEPROM);
    let mut eeprom = command(URP_AST_COMMAND_READ_EEPROM, 0, 0);
    // SAFETY: channel and command storage are live.
    assert_eq!(
        // SAFETY: this test controls every argument supplied to the C boundary.
        unsafe { channel_command(channel, &mut eeprom) },
        URP_AST_SETUP_FAILED
    );
    provider_support::clear_failure();

    for mut invalid in [
        command(99, 0, 0),
        command(URP_AST_COMMAND_GET_MIXER, 99, 0),
        command(URP_AST_COMMAND_SET_MIXER, URP_AST_MIXER_RECEIVE, -1),
        command(URP_AST_COMMAND_SET_MIXER, URP_AST_MIXER_RECEIVE, 1_000),
        command(URP_AST_COMMAND_SET_TEST_TONE, 0, 2),
    ] {
        // SAFETY: channel and complete command storage are live.
        assert_eq!(
            // SAFETY: this test controls every argument supplied to the C boundary.
            unsafe { channel_command(channel, &mut invalid) },
            URP_AST_INVALID_ARGUMENT
        );
    }

    // Exercise both typed event forms through the sole hardware-service consumer.
    provider_support::set_inputs(0, 0x40);
    std::thread::sleep(std::time::Duration::from_millis(10));
    provider_support::set_inputs(0x02, 0x40);
    std::thread::sleep(std::time::Duration::from_millis(10));
    provider_support::set_inputs(0, 0x40);
    std::thread::sleep(std::time::Duration::from_millis(10));
    // SAFETY: service forwards every input edge to Asterisk.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);
    assert!(
        context
            .borrow()
            .text
            .iter()
            .any(|text| text.starts_with("PP"))
    );
    assert!(
        context
            .borrow()
            .text
            .iter()
            .any(|text| text.starts_with("GPIO"))
    );

    let reloads = [
        [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 1\n"].concat(),
        [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 2\n"].concat(),
        [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 3\n"].concat(),
        [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 4\n"].concat(),
    ];
    for (configuration, failure) in reloads.iter().zip([
        provider_support::FAIL_PREFLIGHT,
        provider_support::FAIL_GRAPH,
        provider_support::FAIL_OPEN,
        provider_support::FAIL_START_ONCE,
    ]) {
        // SAFETY: staging is private until the channel successfully adopts it.
        assert_eq!(
            // SAFETY: the driver and candidate bytes remain live for this call.
            unsafe { stage_reload(driver, configuration) },
            URP_AST_OK
        );
        provider_support::set_failure(failure);
        // SAFETY: preparation catches preflight/graph failures before hardware mutation.
        let prepared = unsafe { channel_reload_prepare(channel) };
        if matches!(
            failure,
            provider_support::FAIL_PREFLIGHT | provider_support::FAIL_GRAPH
        ) {
            assert_eq!(prepared, URP_AST_SETUP_FAILED);
        } else {
            assert_eq!(prepared, URP_AST_OK);
            // SAFETY: open/start failures restore the retained running generation.
            assert_eq!(
                // SAFETY: the channel is live and this test owns its control endpoint.
                unsafe { channel_reload_activate(channel) },
                URP_AST_SETUP_FAILED
            );
        }
        // SAFETY: rollback is idempotent even when candidate construction was consumed.
        assert_eq!(
            // SAFETY: the channel remains live and serialized after the failed phase.
            unsafe { channel_reload_finish(channel, 0) },
            URP_AST_OK
        );
        assert_eq!(
            // SAFETY: the driver still owns its unpublished candidate.
            unsafe { driver_reload_finish(driver, 0) },
            URP_AST_OK
        );
        provider_support::clear_failure();
        // SAFETY: service has no reload side effect and the old generation remains live.
        assert_eq!(
            // SAFETY: the retained channel remains live and serialized here.
            unsafe { channel_service(channel) },
            URP_AST_OK,
            "provider failure {failure}: {:?}",
            context.borrow().logs
        );
    }

    // Publishing one frame during stop makes activation fail and immediately restores old state.
    let final_reload = [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 5\n"].concat();
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this call.
        unsafe { stage_reload(driver, &final_reload) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel is serialized and the driver has a candidate.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_OK
    );
    context.borrow_mut().fail_queue = true;
    provider_support::set_failure(provider_support::PUBLISH_ON_STOP);
    // SAFETY: the injected post-stop frame reaches the failing Asterisk queue during activation.
    assert_eq!(
        // SAFETY: the channel is live and this test owns its control endpoint.
        unsafe { channel_reload_activate(channel) },
        URP_AST_ASTERISK_FAILURE
    );
    assert_eq!(
        // SAFETY: the failed activation left a live, serialized channel.
        unsafe { channel_reload_finish(channel, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns its unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    context.borrow_mut().fail_queue = false;
    provider_support::clear_failure();
    // SAFETY: service drains the retained old generation without retrying reload.
    assert_eq!(
        // SAFETY: the retained channel remains live and serialized here.
        unsafe { channel_service(channel) },
        URP_AST_OK
    );

    // A replacement stop failure leaves the old generation recoverable.
    let stop_reload = [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 55\n"].concat();
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this call.
        unsafe { stage_reload(driver, &stop_reload) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel is serialized and the driver has a candidate.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_OK
    );
    provider_support::set_failure(provider_support::FAIL_STOP);
    // SAFETY: the provider failure is contained by the activation boundary.
    assert_eq!(
        // SAFETY: the channel is live and this test owns its control endpoint.
        unsafe { channel_reload_activate(channel) },
        URP_AST_SETUP_FAILED
    );
    assert_eq!(
        // SAFETY: the failed activation left a live, serialized channel.
        unsafe { channel_reload_finish(channel, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns its unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    provider_support::clear_failure();
    // SAFETY: restart is harmless if activation already restored the old generation.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);
    // SAFETY: no lazy reload is attempted here.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);

    // A stopped hardware generation reloads without restoring or restarting it.
    // SAFETY: the running channel is stopped normally.
    assert_eq!(unsafe { channel_stop(channel) }, URP_AST_OK);
    let stopped_reload = [CONFIG, b"[diagnostics]\ndiagnostic_trace_level = 6\n"].concat();
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this call.
        unsafe { stage_reload(driver, &stopped_reload) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live stopped channel is serialized and has a driver candidate.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_OK
    );
    provider_support::set_failure(provider_support::FAIL_OPEN);
    // SAFETY: stopped-generation open failure does not restart the old generation.
    assert_eq!(
        // SAFETY: the channel is live and this test owns its control endpoint.
        unsafe { channel_reload_activate(channel) },
        URP_AST_SETUP_FAILED
    );
    assert_eq!(
        // SAFETY: the failed activation left a live, serialized channel.
        unsafe { channel_reload_finish(channel, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns its unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    provider_support::clear_failure();
    // SAFETY: stopped-generation replacement succeeds as one explicit transaction.
    assert_eq!(
        // SAFETY: both handles remain live and exclusively serialized here.
        unsafe { reload_channels(driver, &[channel], &stopped_reload) },
        URP_AST_OK
    );

    // Removing a reserved channel rejects the complete transaction.
    let removed = b"[other]\n";
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this call.
        unsafe { stage_reload(driver, removed) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel is serialized and the candidate is staged.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_INVALID_CONFIGURATION
    );
    assert_eq!(
        // SAFETY: abort is idempotent after the preparation rejection.
        unsafe { channel_reload_finish(channel, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns its rejected unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    // SAFETY: service owns the retained live generation and performs no refresh.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);
    assert!(
        context
            .borrow()
            .logs
            .iter()
            .any(|(_, message)| message.contains("cannot remove a reserved channel"))
    );

    // SAFETY: channel is live and currently stopped.
    assert_eq!(unsafe { channel_start(channel) }, URP_AST_OK);
    // SAFETY: this test is the sole serialized control owner.
    let (_, control) = unsafe { channel_control(channel) }.unwrap();
    control.hardware.as_mut().unwrap().stop().unwrap();
    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this call.
        unsafe { stage_reload(driver, &final_reload) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the live channel is serialized and the candidate is staged.
        unsafe { channel_reload_prepare(channel) },
        URP_AST_OK
    );
    // SAFETY: explicit activation surfaces the stopped-service snapshot failure.
    assert_eq!(
        // SAFETY: the channel is live and this test owns its control endpoint.
        unsafe { channel_reload_activate(channel) },
        URP_AST_SETUP_FAILED
    );
    assert_eq!(
        // SAFETY: the failed activation left a live, serialized channel.
        unsafe { channel_reload_finish(channel, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns its unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    // SAFETY: ordinary service remains a pure delivery operation.
    assert_eq!(unsafe { channel_service(channel) }, URP_AST_OK);

    // SAFETY: channel and driver were returned by their paired constructors.
    unsafe {
        channel_destroy(channel);
        driver_destroy(driver);
    }
    provider_support::clear_failure();
    provider_support::set_inputs(0, 0);
}

#[test]
fn multi_channel_activation_failure_rolls_every_owner_back_before_publication() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    provider_support::clear_failure();
    let context = RefCell::new(Calls::default());
    let original = b"[one]\n[two]\n";
    let candidate = b"[one]\n[two]\n[three]\n[diagnostics]\ndiagnostic_trace_level = 1\n";
    let driver = create_driver(&context, original);
    let one_args = reserve_args_for(c"one", URP_AST_TRANSPORT_RPT_ADVANCED);
    let two_args = reserve_args_for(c"two", URP_AST_TRANSPORT_RPT_ADVANCED);
    // SAFETY: each named reservation and handle remains live through cleanup.
    let one = unsafe { reserve_channel_with_args(driver, &one_args) };
    // SAFETY: the second named reservation also remains live through cleanup.
    let two = unsafe { reserve_channel_with_args(driver, &two_args) };
    assert_eq!(
        // SAFETY: the first channel is live and exclusively controlled here.
        unsafe { channel_start(one) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the second channel is live and exclusively controlled here.
        unsafe { channel_start(two) },
        URP_AST_OK
    );

    assert_eq!(
        // SAFETY: the driver and candidate bytes remain live for this transaction.
        unsafe { stage_reload(driver, candidate) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the first channel is live and the driver candidate is staged.
        unsafe { channel_reload_prepare(one) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the second channel is live and the driver candidate is staged.
        unsafe { channel_reload_prepare(two) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the first prepared channel is exclusively serialized here.
        unsafe { channel_reload_activate(one) },
        URP_AST_OK
    );
    provider_support::set_failure(provider_support::FAIL_OPEN);
    assert_eq!(
        // SAFETY: the second prepared channel is exclusively serialized here.
        unsafe { channel_reload_activate(two) },
        URP_AST_SETUP_FAILED
    );
    provider_support::clear_failure();

    assert_eq!(
        // SAFETY: the adopted first channel remains live for rollback.
        unsafe { channel_reload_finish(one, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: rollback is idempotent for the failed second activation.
        unsafe { channel_reload_finish(two, 0) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the driver still owns the unpublished candidate.
        unsafe { driver_reload_finish(driver, 0) },
        URP_AST_OK
    );
    let mut length = 0;
    assert_eq!(
        // SAFETY: the live driver and writable length storage remain valid.
        unsafe { driver_channel_name(driver, 2, ptr::null_mut(), 0, &mut length) },
        URP_AST_CHANNEL_NOT_FOUND
    );
    let mut status = UrpAstChannelStatus {
        struct_size: size_of::<UrpAstChannelStatus>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstChannelStatus::default()
    };
    assert_eq!(
        // SAFETY: the first channel and output storage remain live.
        unsafe { channel_get_status(one, &mut status) },
        URP_AST_OK
    );
    assert_eq!(status.running, 1);
    assert_eq!(
        // SAFETY: the second channel and output storage remain live.
        unsafe { channel_get_status(two, &mut status) },
        URP_AST_OK
    );
    assert_eq!(status.running, 1);
    assert_eq!(
        // SAFETY: the first channel remains live and serialized here.
        unsafe { channel_service(one) },
        URP_AST_OK
    );
    assert_eq!(
        // SAFETY: the second channel remains live and serialized here.
        unsafe { channel_service(two) },
        URP_AST_OK
    );

    // SAFETY: both channels are destroyed before their shared live driver.
    unsafe {
        channel_destroy(one);
        channel_destroy(two);
        driver_destroy(driver);
    }
}
