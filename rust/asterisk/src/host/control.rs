//! Taskprocessor serialization for non-real-time channel operations.

use std::ffi::c_void;
use std::sync::mpsc::{SyncSender, sync_channel};

use std::mem::size_of;

use super::super::{
    URP_AST_ASTERISK_FAILURE, URP_AST_OK, UrpAstChannelCommand, UrpAstChannelStatus,
    UrpAstDescriptor, UrpAstJitterConfig, ffi,
};

const SOURCE_FILE: &std::ffi::CStr = c"usbradioplus-rust-host";
const SOURCE_FUNCTION: &std::ffi::CStr = c"run_control";

/// One operation which must run on the channel's Asterisk taskprocessor.
pub(super) enum ControlOperation {
    Start,
    Stop,
    ReloadPrepare,
    ReloadActivate,
    ReloadFinish(bool),
    Text(Vec<u8>),
    Transmit { keyed: bool, ctcss_tenths_hz: u32 },
    Dtmf(bool),
    Echo(bool),
    Jitter,
    Command(UrpAstChannelCommand),
    Status,
    Service,
    Destroy,
}

/// Typed result returned by one serialized operation.
pub(super) enum ControlResult {
    Status(i32),
    Jitter(i32, UrpAstJitterConfig),
    Command(i32, UrpAstChannelCommand),
    ChannelStatus(i32, UrpAstChannelStatus),
}

struct Task {
    descriptor: usize,
    channel: usize,
    operation: ControlOperation,
    reply: SyncSender<ControlResult>,
}

fn status(result: Option<i32>) -> i32 {
    result.unwrap_or(URP_AST_ASTERISK_FAILURE)
}

unsafe fn execute(
    descriptor: *const UrpAstDescriptor,
    channel: *mut c_void,
    operation: ControlOperation,
) -> ControlResult {
    // SAFETY: installation validates the process-lifetime descriptor before a
    // channel can submit work.
    let descriptor = unsafe { &*descriptor };
    match operation {
        ControlOperation::Start => ControlResult::Status(status(
            descriptor
                .channel_start
                .map(|call| unsafe { call(channel) }),
        )),
        ControlOperation::Stop => ControlResult::Status(status(
            descriptor.channel_stop.map(|call| unsafe { call(channel) }),
        )),
        ControlOperation::ReloadPrepare => ControlResult::Status(status(
            descriptor
                .channel_reload_prepare
                .map(|call| unsafe { call(channel) }),
        )),
        ControlOperation::ReloadActivate => ControlResult::Status(status(
            descriptor
                .channel_reload_activate
                .map(|call| unsafe { call(channel) }),
        )),
        ControlOperation::ReloadFinish(commit) => ControlResult::Status(status(
            descriptor
                .channel_reload_finish
                .map(|call| unsafe { call(channel, u32::from(commit)) }),
        )),
        ControlOperation::Text(text) => ControlResult::Status(status(
            descriptor
                .channel_write_text
                .map(|call| unsafe { call(channel, text.as_ptr(), text.len() as u32) }),
        )),
        ControlOperation::Transmit {
            keyed,
            ctcss_tenths_hz,
        } => ControlResult::Status(status(
            descriptor
                .channel_set_transmit
                .map(|call| unsafe { call(channel, u32::from(keyed), ctcss_tenths_hz) }),
        )),
        ControlOperation::Dtmf(enabled) => ControlResult::Status(status(
            descriptor
                .channel_set_dtmf
                .map(|call| unsafe { call(channel, u32::from(enabled)) }),
        )),
        ControlOperation::Echo(enabled) => ControlResult::Status(status(
            descriptor
                .channel_set_echo
                .map(|call| unsafe { call(channel, u32::from(enabled)) }),
        )),
        ControlOperation::Jitter => {
            let mut output = UrpAstJitterConfig {
                struct_size: size_of::<UrpAstJitterConfig>() as u32,
                abi_version: super::super::ABI_VERSION,
                enabled: 0,
                maximum_size_ms: 0,
                resync_threshold_ms: 0,
                implementation: 0,
                logging_enabled: 0,
                force_enabled: 0,
                target_extra_ms: 0,
                video_sync_enabled: 0,
            };
            let result = status(
                descriptor
                    .channel_get_jitter_config
                    .map(|call| unsafe { call(channel, &raw mut output) }),
            );
            ControlResult::Jitter(result, output)
        }
        ControlOperation::Command(mut command) => {
            let result = status(
                descriptor
                    .channel_command
                    .map(|call| unsafe { call(channel, &raw mut command) }),
            );
            ControlResult::Command(result, command)
        }
        ControlOperation::Status => {
            let mut output = UrpAstChannelStatus {
                struct_size: size_of::<UrpAstChannelStatus>() as u32,
                abi_version: super::super::ABI_VERSION,
                ..UrpAstChannelStatus::default()
            };
            let result = status(
                descriptor
                    .channel_get_status
                    .map(|call| unsafe { call(channel, &raw mut output) }),
            );
            ControlResult::ChannelStatus(result, output)
        }
        ControlOperation::Service => ControlResult::Status(status(
            descriptor
                .channel_service
                .map(|call| unsafe { call(channel) }),
        )),
        ControlOperation::Destroy => {
            if let Some(call) = descriptor.channel_destroy {
                // SAFETY: the serialized destroy is the final use of this
                // Rust-owned channel handle.
                unsafe { call(channel) };
                ControlResult::Status(URP_AST_OK)
            } else {
                ControlResult::Status(URP_AST_ASTERISK_FAILURE)
            }
        }
    }
}

unsafe extern "C" fn execute_task(opaque: *mut c_void) -> i32 {
    // SAFETY: run_control transfers exactly one Box<Task> to this callback.
    let task = unsafe { Box::from_raw(opaque.cast::<Task>()) };
    let result = unsafe {
        execute(
            task.descriptor as *const UrpAstDescriptor,
            task.channel as *mut c_void,
            task.operation,
        )
    };
    let _ = task.reply.send(result);
    0
}

/// Submit and synchronously await one taskprocessor-owned control operation.
pub(super) unsafe fn run_control(
    taskprocessor: *mut ffi::ast_taskprocessor,
    descriptor: *const UrpAstDescriptor,
    channel: *mut c_void,
    operation: ControlOperation,
) -> ControlResult {
    let (reply, receive) = sync_channel(0);
    let task = Box::new(Task {
        descriptor: descriptor as usize,
        channel: channel as usize,
        operation,
        reply,
    });
    let opaque = Box::into_raw(task).cast::<c_void>();
    // SAFETY: the taskprocessor takes ownership of opaque only when push
    // succeeds and invokes execute_task exactly once.
    if unsafe {
        ffi::__ast_taskprocessor_push(
            taskprocessor,
            Some(execute_task),
            opaque,
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
        )
    } != 0
    {
        // SAFETY: a failed push leaves ownership with this caller.
        drop(unsafe { Box::from_raw(opaque.cast::<Task>()) });
        return ControlResult::Status(URP_AST_ASTERISK_FAILURE);
    }
    receive
        .recv()
        .unwrap_or(ControlResult::Status(URP_AST_ASTERISK_FAILURE))
}

#[cfg(test)]
mod tests {
    use std::ptr;
    use std::sync::atomic::{AtomicU32, Ordering};

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
        let result = unsafe {
            execute(
                &raw const descriptor,
                ptr::null_mut(),
                ControlOperation::Start,
            )
        };
        assert!(matches!(result, ControlResult::Status(17)));

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
}
