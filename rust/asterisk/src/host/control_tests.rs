//! Taskprocessor dispatch and typed-result boundary regression checks.

use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

use super::super::support;
use super::*;

static TRANSMIT: AtomicU32 = AtomicU32::new(0);

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
        ControlOperation::Direct(crate::tests::direct_callbacks()),
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

    let callbacks = crate::tests::direct_callbacks();
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
