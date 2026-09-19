//! Taskprocessor serialization for non-real-time channel operations.

use std::ffi::c_void;
use std::sync::mpsc::{SyncSender, sync_channel};

use std::mem::size_of;

use super::super::{
    URP_AST_ASTERISK_FAILURE, URP_AST_OK, UrpAstDescriptor, UrpAstJitterConfig, ffi,
};
use super::super::{UrpAstChannelCommand, UrpAstChannelStatus};

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
    Direct(super::super::UrpAstDirectCallbacks),
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

impl ControlResult {
    /// Return the operation status without discarding its typed output payload.
    pub(super) fn status(&self) -> i32 {
        match self {
            Self::Status(status)
            | Self::Jitter(status, _)
            | Self::Command(status, _)
            | Self::ChannelStatus(status, _) => *status,
        }
    }
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
    // SAFETY: the caller keeps the validated descriptor and channel live while
    // its taskprocessor serializes every operation, with Destroy the final use.
    // Owned text/callback values and initialized output buffers remain valid for
    // each synchronous call. The attaching owner retains callback contexts/code
    // until synchronous stream shutdown; this dispatch only copies the binding.
    unsafe {
        let descriptor = &*descriptor;
        match operation {
            ControlOperation::Start => {
                ControlResult::Status(status(descriptor.channel_start.map(|call| call(channel))))
            }
            ControlOperation::Stop => {
                ControlResult::Status(status(descriptor.channel_stop.map(|call| call(channel))))
            }
            ControlOperation::ReloadPrepare => ControlResult::Status(status(
                descriptor.channel_reload_prepare.map(|call| call(channel)),
            )),
            ControlOperation::ReloadActivate => ControlResult::Status(status(
                descriptor.channel_reload_activate.map(|call| call(channel)),
            )),
            ControlOperation::ReloadFinish(commit) => ControlResult::Status(status(
                descriptor
                    .channel_reload_finish
                    .map(|call| call(channel, u32::from(commit))),
            )),
            ControlOperation::Text(text) => ControlResult::Status(status(
                descriptor
                    .channel_write_text
                    .map(|call| call(channel, text.as_ptr(), text.len() as u32)),
            )),
            ControlOperation::Transmit {
                keyed,
                ctcss_tenths_hz,
            } => ControlResult::Status(status(
                descriptor
                    .channel_set_transmit
                    .map(|call| call(channel, u32::from(keyed), ctcss_tenths_hz)),
            )),
            ControlOperation::Dtmf(enabled) => ControlResult::Status(status(
                descriptor
                    .channel_set_dtmf
                    .map(|call| call(channel, u32::from(enabled))),
            )),
            ControlOperation::Echo(enabled) => ControlResult::Status(status(
                descriptor
                    .channel_set_echo
                    .map(|call| call(channel, u32::from(enabled))),
            )),
            ControlOperation::Direct(callbacks) => ControlResult::Status(status(
                descriptor
                    .channel_set_direct_callbacks
                    .map(|call| call(channel, &callbacks)),
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
                        .map(|call| call(channel, &raw mut output)),
                );
                ControlResult::Jitter(result, output)
            }
            ControlOperation::Command(mut command) => {
                let result = status(
                    descriptor
                        .channel_command
                        .map(|call| call(channel, &raw mut command)),
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
                        .map(|call| call(channel, &raw mut output)),
                );
                ControlResult::ChannelStatus(result, output)
            }
            ControlOperation::Service => {
                ControlResult::Status(status(descriptor.channel_service.map(|call| call(channel))))
            }
            ControlOperation::Destroy => {
                if let Some(call) = descriptor.channel_destroy {
                    call(channel);
                    ControlResult::Status(URP_AST_OK)
                } else {
                    ControlResult::Status(URP_AST_ASTERISK_FAILURE)
                }
            }
        }
    }
}

unsafe extern "C" fn execute_task(opaque: *mut c_void) -> i32 {
    // SAFETY: run_control transfers exactly one Box<Task> to this callback.
    let task = unsafe { Box::from_raw(opaque.cast::<Task>()) };
    // SAFETY: run_control waits for this task before releasing its live channel
    // and descriptor; the taskprocessor serializes access to the owned operation.
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
#[path = "control_tests.rs"]
mod tests;
