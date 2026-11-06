//! Taskprocessor dispatch and typed-result boundary regression checks.

use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

use super::super::support;
use super::*;

static TRANSMIT: AtomicU32 = AtomicU32::new(0);

#[test]
fn processing_text_copies_synchronous_chunks_and_preserves_status() {
    unsafe extern "C" fn text(
        channel: *mut c_void,
        sink: Option<unsafe extern "C" fn(*mut c_void, *const u8, u32)>,
        context: *mut c_void,
    ) -> i32 {
        let sink = sink.unwrap();
        let bytes = b"transmit_integrated_lufs=-23.05\n";
        // SAFETY: dispatch retains the sink and output for this complete call.
        unsafe {
            sink(context, ptr::null(), 0);
            sink(context, bytes.as_ptr(), bytes.len() as u32);
            *channel.cast::<i32>()
        }
    }
    let mut descriptor = descriptor();
    // SAFETY: the complete descriptor is retained and its optional entry is absent.
    let empty = unsafe {
        execute(
            &descriptor,
            ptr::null_mut(),
            ControlOperation::ProcessingText,
        )
    };
    assert!(matches!(empty, ControlResult::ProcessingText(0, ref bytes) if bytes.is_empty()));
    assert_eq!(empty.status(), URP_AST_OK);
    descriptor.channel_processing_status_text = Some(text);
    for mut expected in [URP_AST_OK, -37] {
        // SAFETY: the synchronous callback borrows this live status and output.
        let result = unsafe {
            execute(
                &descriptor,
                (&mut expected as *mut i32).cast(),
                ControlOperation::ProcessingText,
            )
        };
        assert_eq!(result.status(), expected);
        assert!(
            matches!(result, ControlResult::ProcessingText(_, ref bytes) if bytes == b"transmit_integrated_lufs=-23.05\n")
        );
    }
}

#[test]
fn remote_control_dispatch_preserves_pending_and_ticket_ownership() {
    unsafe extern "C" fn pending(_: *mut c_void) -> i32 {
        1
    }
    let mut descriptor = descriptor();
    descriptor.channel_quiesce = Some(pending);
    // SAFETY: zero initializes the pointer-free request and empty result ticket.
    let mut request: crate::UrpAstRemoteControl = unsafe { std::mem::zeroed() };
    request.operation = crate::URP_AST_REMOTE_QUIESCE;
    // SAFETY: the complete retained descriptor's fake callback ignores its context.
    let result = unsafe {
        execute(
            &descriptor,
            ptr::null_mut(),
            ControlOperation::Remote(request),
        )
    };
    assert_eq!(result.status(), 1);
    assert!(matches!(result, ControlResult::Remote(1, _)));
    for expected in [0, 1, -123] {
        unsafe extern "C" fn activation(_: *mut c_void) -> i32 {
            TRANSMIT.load(Ordering::Acquire) as i32
        }
        TRANSMIT.store(expected as u32, Ordering::Release);
        descriptor.channel_activation_status = Some(activation);
        request.operation = crate::URP_AST_REMOTE_READY;
        // SAFETY: the complete descriptor and request remain borrowed for dispatch.
        let result = unsafe {
            execute(
                &descriptor,
                ptr::null_mut(),
                ControlOperation::Remote(request),
            )
        };
        assert_eq!(result.status(), expected);
    }
}

unsafe extern "C" fn start(_channel: *mut c_void) -> i32 {
    17
}

unsafe extern "C" fn transmit(_channel: *mut c_void, keyed: u32, ctcss_tenths_hz: u32) -> i32 {
    TRANSMIT.store((keyed << 31) | ctcss_tenths_hz, Ordering::Release);
    URP_AST_OK
}

fn descriptor() -> UrpAstDescriptor {
    // SAFETY: a zero function-pointer option is None, and all raw-pointer
    // fields may be null in this focused dispatch fixture.
    let mut descriptor: UrpAstDescriptor = unsafe { std::mem::zeroed() };
    descriptor.channel_start = Some(start);
    descriptor.channel_set_transmit = Some(transmit);
    descriptor
}

#[test]
fn operation_dispatch_uses_the_selected_descriptor_entry() {
    let descriptor = descriptor();
    // SAFETY: this live fixture descriptor's Start callback ignores the
    // channel pointer and is invoked synchronously on this test thread.
    let result = unsafe {
        execute(
            &raw const descriptor,
            ptr::null_mut(),
            ControlOperation::Start,
        )
    };
    assert!(matches!(result, ControlResult::Status(17)));

    // SAFETY: the live fixture's Transmit callback ignores the channel
    // pointer and only writes the test atomic from this synchronous call.
    let result = unsafe {
        execute(
            &raw const descriptor,
            ptr::null_mut(),
            ControlOperation::Transmit {
                keyed: true,
                ctcss_tenths_hz: 1_230,
            },
        )
    };
    assert!(matches!(result, ControlResult::Status(URP_AST_OK)));
    assert_eq!(TRANSMIT.load(Ordering::Acquire), (1 << 31) | 1_230);
}

#[test]
fn missing_descriptor_entry_reports_host_failure() {
    let descriptor = descriptor();
    // SAFETY: the descriptor remains live and its missing Echo entry is
    // rejected without dereferencing or invoking the null channel pointer.
    let result = unsafe {
        execute(
            &raw const descriptor,
            ptr::null_mut(),
            ControlOperation::Echo(true),
        )
    };
    assert!(matches!(
        result,
        ControlResult::Status(URP_AST_ASTERISK_FAILURE)
    ));
}

#[test]
fn every_missing_operation_reports_host_failure() {
    // SAFETY: descriptor fields are integers, pointers, and optional callbacks.
    let descriptor: UrpAstDescriptor = unsafe { std::mem::zeroed() };
    // SAFETY: the command contains only integers and a fixed integer array.
    let command: UrpAstChannelCommand = unsafe { std::mem::zeroed() };
    for operation in [
        ControlOperation::Start,
        ControlOperation::Stop,
        ControlOperation::ReloadPrepare,
        ControlOperation::ReloadActivate,
        ControlOperation::ReloadFinish(true),
        ControlOperation::Text(vec![1]),
        ControlOperation::Transmit {
            keyed: true,
            ctcss_tenths_hz: 1_000,
        },
        ControlOperation::Dtmf(true),
        ControlOperation::Echo(true),
        ControlOperation::Direct(support::product_support::direct_callbacks()),
        ControlOperation::Site(crate::UrpAstSiteControl {
            operation: 0,
            ..Default::default()
        }),
        ControlOperation::Site(crate::UrpAstSiteControl {
            operation: 1,
            ..Default::default()
        }),
        ControlOperation::Site(crate::UrpAstSiteControl {
            operation: 2,
            ..Default::default()
        }),
        ControlOperation::Site(crate::UrpAstSiteControl {
            operation: 3,
            ..Default::default()
        }),
        ControlOperation::Jitter,
        ControlOperation::Command(command),
        ControlOperation::Status,
        ControlOperation::Service,
        ControlOperation::Destroy,
    ] {
        // SAFETY: absent callbacks cannot dereference the null channel context.
        let result = unsafe { execute(&descriptor, ptr::null_mut(), operation) };
        assert_eq!(result.status(), URP_AST_ASTERISK_FAILURE);
    }
}

unsafe extern "C" fn record_call(context: *mut c_void) -> i32 {
    // SAFETY: dispatch tests supply one exclusively borrowed counter.
    unsafe { *context.cast::<u32>() += 1 };
    17
}

unsafe extern "C" fn record_flag(context: *mut c_void, value: u32) -> i32 {
    // SAFETY: dispatch tests supply one exclusively borrowed flag.
    unsafe { *context.cast::<u32>() = value };
    17
}

unsafe extern "C" fn record_text(context: *mut c_void, text: *const u8, length: u32) -> i32 {
    // SAFETY: execute keeps the owned text live and the test owns the output vector.
    unsafe {
        *context.cast::<Vec<u8>>() = std::slice::from_raw_parts(text, length as usize).to_vec();
    }
    17
}

unsafe extern "C" fn record_direct(
    context: *mut c_void,
    callbacks: *const crate::UrpAstDirectCallbacks,
) -> i32 {
    // SAFETY: execute lends the complete descriptor for this synchronous call.
    unsafe { *context.cast::<u32>() = (*callbacks).abi_version };
    17
}

unsafe extern "C" fn record_destroy(context: *mut c_void) {
    // SAFETY: the same caller-owned counter is used by record_call.
    unsafe { record_call(context) };
}

#[test]
fn present_callbacks_receive_owned_inputs_and_return_status() {
    let mut observed = 0_u32;
    let context = ptr::from_mut(&mut observed).cast();
    for operation in [
        ControlOperation::Start,
        ControlOperation::Stop,
        ControlOperation::ReloadPrepare,
        ControlOperation::ReloadActivate,
        ControlOperation::Service,
    ] {
        // SAFETY: descriptor members are nullable pointers, callback options, and integers.
        let mut descriptor: UrpAstDescriptor = unsafe { std::mem::zeroed() };
        match operation {
            ControlOperation::Start => descriptor.channel_start = Some(record_call),
            ControlOperation::Stop => descriptor.channel_stop = Some(record_call),
            ControlOperation::ReloadPrepare => {
                descriptor.channel_reload_prepare = Some(record_call)
            }
            ControlOperation::ReloadActivate => {
                descriptor.channel_reload_activate = Some(record_call)
            }
            ControlOperation::Service => descriptor.channel_service = Some(record_call),
            _ => unreachable!(),
        }
        observed = 0;
        assert!(matches!(
            // SAFETY: the selected callback only mutates the live counter.
            unsafe { execute(&descriptor, context, operation) },
            ControlResult::Status(17)
        ));
        assert_eq!(observed, 1);
    }
    for enabled in [false, true] {
        for operation in [
            ControlOperation::ReloadFinish(enabled),
            ControlOperation::Dtmf(enabled),
            ControlOperation::Echo(enabled),
        ] {
            // SAFETY: this fixture uses only nullable fields and integers.
            let mut descriptor: UrpAstDescriptor = unsafe { std::mem::zeroed() };
            match operation {
                ControlOperation::ReloadFinish(_) => {
                    descriptor.channel_reload_finish = Some(record_flag)
                }
                ControlOperation::Dtmf(_) => descriptor.channel_set_dtmf = Some(record_flag),
                ControlOperation::Echo(_) => descriptor.channel_set_echo = Some(record_flag),
                _ => unreachable!(),
            }
            observed = 9;
            assert!(matches!(
                // SAFETY: the selected callback only mutates the live flag.
                unsafe { execute(&descriptor, context, operation) },
                ControlResult::Status(17)
            ));
            assert_eq!(observed, u32::from(enabled));
        }
    }

    let mut descriptor = descriptor();
    descriptor.channel_write_text = Some(record_text);
    let mut text = Vec::<u8>::new();
    assert!(matches!(
        // SAFETY: record_text copies into the live test vector before execute returns.
        unsafe {
            execute(
                &descriptor,
                ptr::from_mut(&mut text).cast(),
                ControlOperation::Text(b"T 123.0".to_vec()),
            )
        },
        ControlResult::Status(17)
    ));
    assert_eq!(text, b"T 123.0");

    let callbacks = support::product_support::direct_callbacks();
    let version = callbacks.abi_version;
    descriptor.channel_set_direct_callbacks = Some(record_direct);
    assert!(matches!(
        // SAFETY: the context and copied descriptor remain live through dispatch.
        unsafe { execute(&descriptor, context, ControlOperation::Direct(callbacks)) },
        ControlResult::Status(17)
    ));
    assert_eq!(observed, version);

    descriptor.channel_destroy = Some(record_destroy);
    observed = 0;
    assert!(matches!(
        // SAFETY: this fixture destroy records invocation without freeing the counter.
        unsafe { execute(&descriptor, context, ControlOperation::Destroy) },
        ControlResult::Status(URP_AST_OK)
    ));
    assert_eq!(observed, 1);
}

unsafe extern "C" fn jitter_result(_: *mut c_void, output: *mut UrpAstJitterConfig) -> i32 {
    // SAFETY: execute supplies an initialized, exclusively borrowed output record.
    let output = unsafe { &mut *output };
    if output.struct_size != size_of::<UrpAstJitterConfig>() as u32
        || output.abi_version != crate::ABI_VERSION
    {
        return -1;
    }
    output.enabled = 1;
    output.maximum_size_ms = 160;
    17
}

unsafe extern "C" fn command_result(_: *mut c_void, output: *mut UrpAstChannelCommand) -> i32 {
    // SAFETY: execute lends its owned command exclusively until this call returns.
    let output = unsafe { &mut *output };
    output.value += 1;
    17
}

unsafe extern "C" fn status_result(_: *mut c_void, output: *mut UrpAstChannelStatus) -> i32 {
    // SAFETY: execute initializes the complete output record before invoking us.
    let output = unsafe { &mut *output };
    if output.struct_size != size_of::<UrpAstChannelStatus>() as u32
        || output.abi_version != crate::ABI_VERSION
    {
        return -1;
    }
    output.running = 1;
    17
}

#[test]
fn typed_results_preserve_callback_updates() {
    let mut descriptor = descriptor();
    descriptor.channel_get_jitter_config = Some(jitter_result);
    descriptor.channel_command = Some(command_result);
    descriptor.channel_get_status = Some(status_result);
    // SAFETY: callbacks do not inspect context and receive live initialized outputs.
    let jitter = unsafe { execute(&descriptor, ptr::null_mut(), ControlOperation::Jitter) };
    let ControlResult::Jitter(17, jitter) = jitter else {
        panic!("wrong jitter result")
    };
    assert_eq!((jitter.enabled, jitter.maximum_size_ms), (1, 160));
    // SAFETY: every command field is an integer.
    let mut command: UrpAstChannelCommand = unsafe { std::mem::zeroed() };
    command.value = 42;
    // SAFETY: command_result only changes the owned command.
    let command = unsafe {
        execute(
            &descriptor,
            ptr::null_mut(),
            ControlOperation::Command(command),
        )
    };
    let ControlResult::Command(17, command) = command else {
        panic!("wrong command result")
    };
    assert_eq!(command.value, 43);
    // SAFETY: status_result only changes the initialized output.
    let status = unsafe { execute(&descriptor, ptr::null_mut(), ControlOperation::Status) };
    let ControlResult::ChannelStatus(17, status) = status else {
        panic!("wrong status result")
    };
    assert_eq!(status.running, 1);
}

#[test]
fn taskprocessor_failure_retains_ownership_and_success_returns_callback_result() {
    let _fixture = support::Fixture::new();
    let descriptor = descriptor();
    for (push_result, expected) in [(-1, URP_AST_ASTERISK_FAILURE), (0, 17)] {
        support::with_state(|state| state.taskprocessor_push_result = push_result);
        // SAFETY: fixture task submission ignores its processor pointer; the live
        // descriptor stays borrowed until its asynchronous reply is received.
        let result = unsafe {
            run_control(
                ptr::null_mut(),
                &descriptor,
                ptr::null_mut(),
                ControlOperation::Start,
            )
        };
        assert!(matches!(result, ControlResult::Status(code) if code == expected));
    }
    assert_eq!(
        support::with_state(|state| state.taskprocessor_push_calls),
        2
    );
}

#[test]
fn remote_dispatch_validates_ticket_transfer_and_cleans_partial_results() {
    use std::cell::Cell;
    thread_local! { static MODE: Cell<i32> = const { Cell::new(0) }; static DROPS: Cell<u32> = const { Cell::new(0) }; }
    unsafe extern "C" fn submit(
        _: *mut c_void,
        _: *const crate::UrpRemoteOperation,
        _: u32,
        out: *mut *mut c_void,
    ) -> i32 {
        // SAFETY: execute lends one complete writable ticket pointer.
        unsafe {
            *out = if MODE.get() == 1 {
                ptr::null_mut()
            } else {
                ptr::dangling_mut::<u8>().cast()
            };
        }
        if MODE.get() == 2 { -42 } else { 0 }
    }
    unsafe extern "C" fn source(_: *mut c_void, _: u32) -> i32 {
        0
    }
    unsafe extern "C" fn take(
        _: *mut c_void,
        _: crate::UrpNativeProcessingDiagnostic,
        _: *mut c_void,
    ) -> i32 {
        1
    }
    unsafe extern "C" fn destroy(_: *mut c_void) {
        DROPS.set(DROPS.get() + 1);
    }
    unsafe extern "C" fn service(_: *mut c_void) -> i32 {
        17
    }
    let mut table = descriptor();
    table.channel_service = Some(service);
    table.channel_remote_submit = Some(submit);
    table.remote_source = Some(source);
    table.remote_take = Some(take);
    table.remote_destroy = Some(destroy);
    DROPS.set(0);
    for mode in 0..5 {
        MODE.set(mode);
        // SAFETY: the envelope is scalar data and nullable pointers.
        let mut request: crate::UrpAstRemoteControl = unsafe { std::mem::zeroed() };
        request.operation = crate::URP_AST_REMOTE_SUBMIT;
        if mode == 3 {
            table.remote_source = None;
        }
        if mode == 4 {
            request.operation = 0;
        }
        // SAFETY: all fixture functions are synchronous and retain no pointers.
        let result = unsafe { execute(&table, ptr::null_mut(), ControlOperation::Remote(request)) };
        let ControlResult::Remote(status, response) = result else {
            panic!("remote result expected")
        };
        match mode {
            0 => {
                assert_eq!(status, 0);
                assert!(!response.ticket.context.is_null());
                assert!(response.ticket.source.is_some() && response.ticket.take.is_some());
                // SAFETY: the successful response transfers this exact fake ticket.
                unsafe { response.ticket.destroy.unwrap()(response.ticket.context) };
            }
            2 => assert_eq!(status, -42),
            _ => assert_eq!(status, URP_AST_ASTERISK_FAILURE),
        }
        assert_eq!(DROPS.get(), if mode < 2 { 1 } else { 2 });
    }
    // SAFETY: the complete table remains borrowed and service ignores the handle.
    let result = unsafe { execute(&table, ptr::null_mut(), ControlOperation::Service) };
    assert_eq!(result.status(), 17);
    // SAFETY: zero initializes a complete scalar envelope.
    let mut request: crate::UrpAstRemoteControl = unsafe { std::mem::zeroed() };
    request.operation = crate::URP_AST_REMOTE_SERVICE;
    // SAFETY: the service callback is the same synchronous retained fixture.
    let result = unsafe { execute(&table, ptr::null_mut(), ControlOperation::Remote(request)) };
    assert_eq!(result.status(), 17);
}
