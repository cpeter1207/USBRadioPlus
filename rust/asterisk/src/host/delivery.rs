//! Asterisk frame translation for Rust-owned station delivery.

use std::ffi::{c_int, c_void};
use std::mem::{size_of, zeroed};
use std::ptr;
use std::sync::OnceLock;
use std::sync::atomic::Ordering;
use std::time::Duration;

use super::channel::{
    Channel, configure_pending_jitter, lock_owner, replay_transmit, service, unlock_owner,
};
use crate::{
    ABI_VERSION, URP_AST_CHANNEL_BUSY, URP_AST_CONTROL_DTMF_BEGIN, URP_AST_CONTROL_DTMF_END,
    URP_AST_CONTROL_NULL, URP_AST_CONTROL_RECEIVER_KEY, URP_AST_CONTROL_RECEIVER_UNKEY,
    URP_AST_DTMF_BEGIN, URP_AST_DTMF_END, URP_AST_DTMF_NONE, URP_AST_LOG_ERROR,
    URP_AST_LOG_WARNING, URP_AST_OK, UrpAstDtmfResult, UrpAstOperations, ffi,
};

const SOURCE_FILE: &std::ffi::CStr = c"usbradioplus-rust-host";
const SOURCE_FUNCTION: &std::ffi::CStr = c"delivery";
static OPERATIONS: OnceLock<usize> = OnceLock::new();

#[derive(Debug, PartialEq, Eq)]
pub(super) enum ControlFrame {
    ReceiverKey(Option<String>),
    ReceiverUnkey,
    DtmfBegin {
        digit: u8,
        duration_ms: libc::c_long,
    },
    DtmfEnd {
        digit: u8,
        duration_ms: libc::c_long,
    },
    Null,
}

pub(super) fn control_frame(kind: u32, value: i32, duration_ms: u64) -> Result<ControlFrame, ()> {
    match kind {
        URP_AST_CONTROL_RECEIVER_KEY => Ok(ControlFrame::ReceiverKey(
            (value > 0).then(|| format!("{}.{:01}", value / 10, value % 10)),
        )),
        URP_AST_CONTROL_RECEIVER_UNKEY => Ok(ControlFrame::ReceiverUnkey),
        URP_AST_CONTROL_DTMF_BEGIN | URP_AST_CONTROL_DTMF_END
            if (1..=i32::from(u8::MAX)).contains(&value) =>
        {
            let digit = value as u8;
            let duration_ms = libc::c_long::try_from(duration_ms).unwrap_or(libc::c_long::MAX);
            if kind == URP_AST_CONTROL_DTMF_BEGIN {
                Ok(ControlFrame::DtmfBegin { digit, duration_ms })
            } else {
                Ok(ControlFrame::DtmfEnd { digit, duration_ms })
            }
        }
        URP_AST_CONTROL_NULL => Ok(ControlFrame::Null),
        _ => Err(()),
    }
}

/// Return the Asterisk operations table which must be injected at driver creation.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_asterisk_channel_host_operations() -> *const UrpAstOperations {
    let address = OPERATIONS.get_or_init(|| {
        Box::into_raw(Box::new(UrpAstOperations {
            struct_size: size_of::<UrpAstOperations>() as u32,
            abi_version: ABI_VERSION,
            application_context: ptr::null_mut(),
            queue_voice: Some(queue_voice),
            queue_control: Some(queue_control),
            queue_text: Some(queue_text),
            analyze_dtmf: Some(analyze_dtmf),
            monotonic_milliseconds: Some(monotonic_milliseconds),
            log: Some(log),
        })) as usize
    });
    *address as *const UrpAstOperations
}

unsafe fn channel<'a>(context: *mut c_void) -> Option<&'a Channel> {
    // SAFETY: the Rust driver returns only the stable context supplied when
    // its channel was reserved.
    unsafe { context.cast::<Channel>().as_ref() }
}

unsafe extern "C" fn queue_voice(
    _application_context: *mut c_void,
    channel_context: *mut c_void,
    samples: *const i16,
    sample_count: u32,
    sample_rate_hz: u32,
) -> c_int {
    // SAFETY: the Rust driver returns the reserved channel context.
    let Some(channel) = (unsafe { channel(channel_context) }) else {
        return -1;
    };
    if samples.is_null()
        || sample_count != channel.frame_samples
        || sample_rate_hz != channel.sample_rate_hz
    {
        return -1;
    }
    // SAFETY: the locked owner remains stable until explicitly unlocked.
    let Some(owner) = (unsafe { lock_owner(channel) }) else {
        return 0;
    };
    // SAFETY: owner remains locked and live.
    if unsafe { ffi::ast_channel_state(owner) } != ffi::AST_STATE_UP {
        // SAFETY: owner was locked above.
        unsafe { unlock_owner(owner) };
        return 0;
    }
    // SAFETY: zero is the documented empty initialization for an ast_frame.
    let mut frame: ffi::ast_frame = unsafe { zeroed() };
    frame.frametype = ffi::AST_FRAME_VOICE;
    frame.subclass.__bindgen_anon_1.format = channel.format;
    frame.samples = sample_count as c_int;
    frame.datalen = (sample_count as usize * size_of::<i16>()) as c_int;
    frame.data.ptr = samples.cast_mut().cast();
    frame.src = SOURCE_FUNCTION.as_ptr();
    // SAFETY: Asterisk copies the complete frame while owner is locked.
    let result = unsafe { ffi::ast_queue_frame(owner, &raw mut frame) };
    // SAFETY: owner was locked above.
    unsafe { unlock_owner(owner) };
    result
}

unsafe extern "C" fn queue_control(
    _application_context: *mut c_void,
    channel_context: *mut c_void,
    kind: u32,
    value: i32,
    duration_ms: u64,
) -> c_int {
    // SAFETY: the Rust driver returns the reserved channel context.
    let Some(channel) = (unsafe { channel(channel_context) }) else {
        return -1;
    };
    let Ok(control) = control_frame(kind, value, duration_ms) else {
        return -1;
    };
    // SAFETY: the locked owner remains stable until explicitly unlocked.
    let Some(owner) = (unsafe { lock_owner(channel) }) else {
        return 0;
    };
    // SAFETY: zero is the documented empty initialization for an ast_frame.
    let mut frame: ffi::ast_frame = unsafe { zeroed() };
    frame.src = SOURCE_FUNCTION.as_ptr();
    let tone;
    match control {
        ControlFrame::ReceiverKey(value) => {
            frame.frametype = ffi::AST_FRAME_CONTROL;
            frame.subclass.integer = ffi::AST_CONTROL_RADIO_KEY as c_int;
            if let Some(value) = value {
                tone = std::ffi::CString::new(value).expect("numeric tone cannot contain NUL");
                frame.data.ptr = tone.as_ptr().cast_mut().cast();
                frame.datalen = tone.as_bytes_with_nul().len() as c_int;
            }
        }
        ControlFrame::ReceiverUnkey => {
            frame.frametype = ffi::AST_FRAME_CONTROL;
            frame.subclass.integer = ffi::AST_CONTROL_RADIO_UNKEY as c_int;
        }
        ControlFrame::DtmfBegin { digit, duration_ms } => {
            frame.frametype = ffi::AST_FRAME_DTMF_BEGIN;
            frame.subclass.integer = c_int::from(digit);
            frame.len = duration_ms;
        }
        ControlFrame::DtmfEnd { digit, duration_ms } => {
            frame.frametype = ffi::AST_FRAME_DTMF_END;
            frame.subclass.integer = c_int::from(digit);
            frame.len = duration_ms;
        }
        ControlFrame::Null => {
            // SAFETY: ast_null_frame is a process-lifetime value copied here.
            frame = unsafe { ffi::ast_null_frame };
        }
    }
    // SAFETY: Asterisk copies the frame while owner is locked.
    let result = unsafe { ffi::ast_queue_frame(owner, &raw mut frame) };
    // SAFETY: owner was locked above.
    unsafe { unlock_owner(owner) };
    result
}

unsafe extern "C" fn queue_text(
    _application_context: *mut c_void,
    channel_context: *mut c_void,
    text: *const u8,
    text_length: u32,
) -> c_int {
    // SAFETY: the Rust driver returns the reserved channel context.
    let Some(channel) = (unsafe { channel(channel_context) }) else {
        return -1;
    };
    if (text.is_null() && text_length != 0) || text_length >= c_int::MAX as u32 {
        return -1;
    }
    // SAFETY: the locked owner remains stable until explicitly unlocked.
    let Some(owner) = (unsafe { lock_owner(channel) }) else {
        return 0;
    };
    let mut terminated = Vec::with_capacity(text_length as usize + 1);
    if text_length != 0 {
        // SAFETY: the ABI promises text_length readable bytes.
        terminated
            .extend_from_slice(unsafe { std::slice::from_raw_parts(text, text_length as usize) });
    }
    terminated.push(0);
    // SAFETY: zero is the documented empty initialization for an ast_frame.
    let mut frame: ffi::ast_frame = unsafe { zeroed() };
    frame.frametype = ffi::AST_FRAME_TEXT;
    frame.src = SOURCE_FUNCTION.as_ptr();
    frame.data.ptr = terminated.as_mut_ptr().cast();
    frame.datalen = terminated.len() as c_int;
    // SAFETY: Asterisk copies the frame while storage and owner remain live.
    let result = unsafe { ffi::ast_queue_frame(owner, &raw mut frame) };
    // SAFETY: owner was locked above.
    unsafe { unlock_owner(owner) };
    result
}

unsafe extern "C" fn analyze_dtmf(
    _application_context: *mut c_void,
    channel_context: *mut c_void,
    samples: *mut i16,
    sample_count: u32,
    sample_rate_hz: u32,
    result: *mut UrpAstDtmfResult,
) -> c_int {
    // SAFETY: the Rust driver returns the reserved channel context.
    let Some(channel) = (unsafe { channel(channel_context) }) else {
        return -1;
    };
    if channel.dsp.is_null()
        || samples.is_null()
        || result.is_null()
        || sample_count != channel.frame_samples
        || sample_rate_hz != 8_000
    {
        return -1;
    }
    // SAFETY: result is writable ABI storage supplied by the Rust driver.
    if unsafe { (*result).struct_size } < size_of::<UrpAstDtmfResult>() as u32 {
        return -1;
    }
    // SAFETY: the locked owner remains stable until explicitly unlocked.
    let Some(owner) = (unsafe { lock_owner(channel) }) else {
        return 0;
    };
    // SAFETY: zero is the documented empty initialization for an ast_frame.
    let mut input: ffi::ast_frame = unsafe { zeroed() };
    input.frametype = ffi::AST_FRAME_VOICE;
    input.subclass.__bindgen_anon_1.format = channel.format;
    input.samples = sample_count as c_int;
    input.datalen = (sample_count as usize * size_of::<i16>()) as c_int;
    input.data.ptr = samples.cast();
    input.src = SOURCE_FUNCTION.as_ptr();
    // SAFETY: DSP, owner, and input remain live and owner is locked.
    let detected = unsafe { ffi::ast_dsp_process(owner, channel.dsp, &raw mut input) };
    if detected.is_null() {
        // SAFETY: owner was locked above.
        unsafe { unlock_owner(owner) };
        return -1;
    }
    let mut event = 0;
    // SAFETY: detected points to a live Asterisk frame.
    let detected_frame = unsafe { &*detected };
    if detected_frame.frametype == ffi::AST_FRAME_DTMF_BEGIN
        || detected_frame.frametype == ffi::AST_FRAME_DTMF_END
    {
        // SAFETY: result is valid writable ABI storage.
        unsafe {
            (*result).event_kind = if detected_frame.frametype == ffi::AST_FRAME_DTMF_BEGIN {
                URP_AST_DTMF_BEGIN
            } else {
                URP_AST_DTMF_END
            };
            (*result).digit = detected_frame.subclass.integer as u8;
        }
        event = 1;
    } else {
        // SAFETY: result is valid writable ABI storage.
        unsafe { (*result).event_kind = URP_AST_DTMF_NONE };
    }
    if !ptr::eq(detected, &raw mut input) {
        // SAFETY: DSP returned an independently allocated frame.
        unsafe { ffi::ast_frame_free(detected, 1) };
    }
    // SAFETY: owner was locked above.
    unsafe { unlock_owner(owner) };
    event
}

unsafe extern "C" fn monotonic_milliseconds(_application_context: *mut c_void) -> u64 {
    let mut now = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: now is writable timespec storage.
    if unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut now) } != 0 {
        0
    } else {
        (now.tv_sec as u64)
            .saturating_mul(1_000)
            .saturating_add((now.tv_nsec as u64) / 1_000_000)
    }
}

unsafe extern "C" fn log(
    _application_context: *mut c_void,
    level: u32,
    message: *const u8,
    message_length: u32,
) {
    if message.is_null() {
        return;
    }
    let level = if level == URP_AST_LOG_WARNING {
        ffi::__LOG_WARNING
    } else if level == URP_AST_LOG_ERROR {
        ffi::__LOG_ERROR
    } else {
        ffi::__LOG_NOTICE
    };
    // SAFETY: the precision bounds the readable message span and all other
    // arguments match Asterisk's public variadic logging API.
    unsafe {
        ffi::ast_log(
            level as c_int,
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
            c"%.*s\n".as_ptr(),
            message_length.min(c_int::MAX as u32) as c_int,
            message.cast::<i8>(),
        )
    };
}

pub(super) unsafe fn run_worker(channel: *const Channel) {
    // SAFETY: the spawning channel joins this worker before freeing itself.
    let channel = unsafe { &*channel };
    while !channel.delivery_stop.load(Ordering::Acquire) {
        let transmit = replay_transmit(channel);
        if transmit != URP_AST_OK && transmit != URP_AST_CHANNEL_BUSY {
            channel.service_failed.store(true, Ordering::Release);
            break;
        }
        if transmit == URP_AST_OK {
            configure_pending_jitter(channel);
            let delivered = service(channel);
            if delivered != URP_AST_OK && delivered != URP_AST_CHANNEL_BUSY {
                channel.service_failed.store(true, Ordering::Release);
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn control_events_keep_the_existing_asterisk_frame_contract() {
        assert_eq!(
            control_frame(URP_AST_CONTROL_RECEIVER_KEY, 1_000, 0),
            Ok(ControlFrame::ReceiverKey(Some("100.0".into())))
        );
        assert_eq!(
            control_frame(URP_AST_CONTROL_RECEIVER_KEY, 0, 0),
            Ok(ControlFrame::ReceiverKey(None))
        );
        assert_eq!(
            control_frame(URP_AST_CONTROL_RECEIVER_UNKEY, 0, 0),
            Ok(ControlFrame::ReceiverUnkey)
        );
        assert_eq!(
            control_frame(URP_AST_CONTROL_DTMF_BEGIN, i32::from(b'1'), 20),
            Ok(ControlFrame::DtmfBegin {
                digit: b'1',
                duration_ms: 20,
            })
        );
        assert_eq!(
            control_frame(URP_AST_CONTROL_DTMF_END, i32::from(b'#'), u64::MAX),
            Ok(ControlFrame::DtmfEnd {
                digit: b'#',
                duration_ms: libc::c_long::MAX,
            })
        );
        assert_eq!(
            control_frame(URP_AST_CONTROL_NULL, 0, 0),
            Ok(ControlFrame::Null)
        );
        assert_eq!(control_frame(URP_AST_CONTROL_DTMF_BEGIN, 0, 0), Err(()));
        assert_eq!(control_frame(999, 0, 0), Err(()));
    }
}
