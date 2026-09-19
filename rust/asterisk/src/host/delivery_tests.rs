//! Regression coverage of copied frames, detector results, and frame ownership.

use super::super::channel::tests::{DriverCalls, callback_channel, callback_descriptor};
use super::super::support::{DspOutput, Fixture, with_state};
use super::*;
use std::sync::Mutex;

#[test]
fn voice_delivery_validates_owner_and_copies_complete_pcm_frames() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (mut channel, mut owner) = callback_channel(&descriptor, &calls);
    let context = ptr::from_mut(&mut *channel).cast();
    let samples = [321i16; 160];
    // SAFETY: every non-null context and PCM span remains live throughout these calls.
    unsafe {
        assert_eq!(
            queue_voice(
                ptr::null_mut(),
                ptr::null_mut(),
                samples.as_ptr(),
                160,
                8_000
            ),
            -1
        );
        for (data, count, rate) in [
            (ptr::null(), 160, 8_000),
            (samples.as_ptr(), 159, 8_000),
            (samples.as_ptr(), 160, 48_000),
        ] {
            assert_eq!(queue_voice(ptr::null_mut(), context, data, count, rate), -1);
        }
        assert_eq!(
            queue_voice(ptr::null_mut(), context, samples.as_ptr(), 160, 8_000),
            0
        );
        with_state(|state| assert!(state.queued.is_empty()));
        owner.state = ffi::AST_STATE_UP as c_int;
        with_state(|state| state.trylock_result = 1);
        assert_eq!(
            queue_voice(ptr::null_mut(), context, samples.as_ptr(), 160, 8_000),
            0
        );
        with_state(|state| {
            state.trylock_result = 0;
            state.queue_result = 7;
        });
        assert_eq!(
            queue_voice(ptr::null_mut(), context, samples.as_ptr(), 160, 8_000),
            7
        );
    }
    with_state(|state| {
        assert_eq!(state.queued.len(), 1);
        let frame = &state.queued[0];
        assert_eq!(frame.kind, ffi::AST_FRAME_VOICE);
        assert_eq!(frame.samples, 160);
        assert_eq!(
            frame.bytes,
            samples
                .iter()
                .flat_map(|sample| sample.to_ne_bytes())
                .collect::<Vec<_>>()
        );
        assert_eq!(state.unlocks, 2);
    });
}

#[test]
fn control_delivery_copies_tones_digits_and_null_frames() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (mut channel, _owner) = callback_channel(&descriptor, &calls);
    let context = ptr::from_mut(&mut *channel).cast();
    let cases = [
        (
            URP_AST_CONTROL_RECEIVER_KEY,
            1_005,
            0,
            ffi::AST_FRAME_CONTROL,
            ffi::AST_CONTROL_RADIO_KEY as c_int,
            0,
            b"100.5\0".as_slice(),
        ),
        (
            URP_AST_CONTROL_RECEIVER_KEY,
            0,
            0,
            ffi::AST_FRAME_CONTROL,
            ffi::AST_CONTROL_RADIO_KEY as c_int,
            0,
            b"".as_slice(),
        ),
        (
            URP_AST_CONTROL_RECEIVER_UNKEY,
            0,
            0,
            ffi::AST_FRAME_CONTROL,
            ffi::AST_CONTROL_RADIO_UNKEY as c_int,
            0,
            b"".as_slice(),
        ),
        (
            URP_AST_CONTROL_DTMF_BEGIN,
            i32::from(b'4'),
            23,
            ffi::AST_FRAME_DTMF_BEGIN,
            i32::from(b'4'),
            23,
            b"".as_slice(),
        ),
        (
            URP_AST_CONTROL_DTMF_END,
            i32::from(b'#'),
            u64::MAX,
            ffi::AST_FRAME_DTMF_END,
            i32::from(b'#'),
            libc::c_long::MAX,
            b"".as_slice(),
        ),
        (
            URP_AST_CONTROL_NULL,
            0,
            0,
            ffi::AST_FRAME_NULL,
            0,
            0,
            b"".as_slice(),
        ),
    ];
    // SAFETY: the channel and its stable owner outlive each synchronous callback.
    unsafe {
        assert_eq!(
            queue_control(ptr::null_mut(), ptr::null_mut(), URP_AST_CONTROL_NULL, 0, 0),
            -1
        );
        assert_eq!(queue_control(ptr::null_mut(), context, u32::MAX, 0, 0), -1);
        with_state(|state| state.trylock_result = 1);
        assert_eq!(
            queue_control(ptr::null_mut(), context, URP_AST_CONTROL_NULL, 0, 0),
            0
        );
        with_state(|state| {
            state.trylock_result = 0;
            state.queue_result = 9;
        });
        for (kind, value, duration, ..) in cases {
            assert_eq!(
                queue_control(ptr::null_mut(), context, kind, value, duration),
                9
            );
        }
    }
    with_state(|state| {
        assert_eq!(state.queued.len(), cases.len());
        for (frame, (_, _, _, kind, subclass, duration, bytes)) in state.queued.iter().zip(cases) {
            assert_eq!(
                (frame.kind, frame.subclass, frame.duration),
                (kind, subclass, duration)
            );
            assert_eq!(frame.bytes, bytes);
        }
        assert_eq!(state.unlocks, cases.len());
    });
}

#[test]
fn text_delivery_checks_lengths_and_owns_its_terminator() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (mut channel, _owner) = callback_channel(&descriptor, &calls);
    let context = ptr::from_mut(&mut *channel).cast();
    let text = b"link status";
    // SAFETY: invalid spans are rejected before access; valid text remains readable.
    unsafe {
        assert_eq!(
            queue_text(ptr::null_mut(), ptr::null_mut(), text.as_ptr(), 11),
            -1
        );
        assert_eq!(queue_text(ptr::null_mut(), context, ptr::null(), 1), -1);
        assert_eq!(
            queue_text(ptr::null_mut(), context, text.as_ptr(), c_int::MAX as u32),
            -1
        );
        with_state(|state| state.trylock_result = 1);
        assert_eq!(queue_text(ptr::null_mut(), context, text.as_ptr(), 11), 0);
        with_state(|state| {
            state.trylock_result = 0;
            state.queue_result = 3;
        });
        assert_eq!(queue_text(ptr::null_mut(), context, text.as_ptr(), 11), 3);
        assert_eq!(queue_text(ptr::null_mut(), context, ptr::null(), 0), 3);
        assert_eq!(queue_text(ptr::null_mut(), context, text.as_ptr(), 0), 3);
    }
    with_state(|state| {
        assert_eq!(
            state
                .queued
                .iter()
                .map(|frame| frame.bytes.as_slice())
                .collect::<Vec<_>>(),
            [b"link status\0".as_slice(), b"\0", b"\0"]
        );
        assert!(
            state
                .queued
                .iter()
                .all(|frame| frame.kind == ffi::AST_FRAME_TEXT)
        );
        assert_eq!(state.unlocks, 3);
    });
}

#[test]
fn detector_checks_the_boundary_and_frees_only_independent_frames() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (mut channel, _owner) = callback_channel(&descriptor, &calls);
    let context = ptr::from_mut(&mut *channel).cast();
    let mut samples = [200i16; 160];
    let mut result = UrpAstDtmfResult::default();
    // SAFETY: fixture DSP handles are never dereferenced; valid PCM/result spans outlive calls.
    unsafe {
        assert_eq!(
            analyze_dtmf(
                ptr::null_mut(),
                ptr::null_mut(),
                samples.as_mut_ptr(),
                160,
                8_000,
                &raw mut result
            ),
            -1
        );
        assert_eq!(
            analyze_dtmf(
                ptr::null_mut(),
                context,
                samples.as_mut_ptr(),
                160,
                8_000,
                &raw mut result
            ),
            -1
        );
        channel.dsp = ptr::dangling_mut();
        for (data, count, rate, output) in [
            (ptr::null_mut(), 160, 8_000, &raw mut result),
            (samples.as_mut_ptr(), 160, 8_000, ptr::null_mut()),
            (samples.as_mut_ptr(), 159, 8_000, &raw mut result),
            (samples.as_mut_ptr(), 160, 48_000, &raw mut result),
        ] {
            assert_eq!(
                analyze_dtmf(ptr::null_mut(), context, data, count, rate, output),
                -1
            );
        }
        result.struct_size = 0;
        assert_eq!(
            analyze_dtmf(
                ptr::null_mut(),
                context,
                samples.as_mut_ptr(),
                160,
                8_000,
                &raw mut result
            ),
            -1
        );
        result = UrpAstDtmfResult::default();
        with_state(|state| state.trylock_result = 1);
        assert_eq!(
            analyze_dtmf(
                ptr::null_mut(),
                context,
                samples.as_mut_ptr(),
                160,
                8_000,
                &raw mut result
            ),
            0
        );
        with_state(|state| {
            state.trylock_result = 0;
            state.dsp_output = DspOutput::Null;
        });
        assert_eq!(
            analyze_dtmf(
                ptr::null_mut(),
                context,
                samples.as_mut_ptr(),
                160,
                8_000,
                &raw mut result
            ),
            -1
        );
        for (mode, expected_status, expected_kind, expected_frees) in [
            (DspOutput::Input, 0, URP_AST_DTMF_NONE, 0),
            (
                DspOutput::Allocated {
                    kind: ffi::AST_FRAME_TEXT,
                    digit: 0,
                },
                0,
                URP_AST_DTMF_NONE,
                1,
            ),
            (
                DspOutput::Allocated {
                    kind: ffi::AST_FRAME_DTMF_BEGIN,
                    digit: i32::from(b'7'),
                },
                1,
                URP_AST_DTMF_BEGIN,
                2,
            ),
            (
                DspOutput::Allocated {
                    kind: ffi::AST_FRAME_DTMF_END,
                    digit: i32::from(b'7'),
                },
                1,
                URP_AST_DTMF_END,
                3,
            ),
        ] {
            with_state(|state| state.dsp_output = mode);
            assert_eq!(
                analyze_dtmf(
                    ptr::null_mut(),
                    context,
                    samples.as_mut_ptr(),
                    160,
                    8_000,
                    &raw mut result
                ),
                expected_status
            );
            assert_eq!(result.event_kind, expected_kind);
            if expected_status == 1 {
                assert_eq!(result.digit, b'7');
            }
            with_state(|state| assert_eq!(state.freed_frames, expected_frees));
        }
    }
    with_state(|state| {
        let input = state.dsp_input.as_ref().unwrap();
        assert_eq!((input.kind, input.samples), (ffi::AST_FRAME_VOICE, 160));
        assert_eq!(
            input.bytes,
            samples
                .iter()
                .flat_map(|sample| sample.to_ne_bytes())
                .collect::<Vec<_>>()
        );
        assert_eq!(state.unlocks, 5);
    });
}

#[test]
fn operations_are_stable_and_logging_preserves_level_and_length() {
    let _fixture = Fixture::new();
    let operations = usbradioplus_asterisk_channel_host_operations();
    assert_eq!(operations, usbradioplus_asterisk_channel_host_operations());
    // SAFETY: operations has process lifetime and each message span is readable.
    unsafe {
        let operations = &*operations;
        assert_eq!(
            operations.struct_size as usize,
            size_of::<UrpAstOperations>()
        );
        assert_eq!(operations.abi_version, ABI_VERSION);
        assert!(operations.queue_voice.is_some());
        assert!(operations.queue_control.is_some());
        assert!(operations.queue_text.is_some());
        assert!(operations.analyze_dtmf.is_some());
        super::super::support::urp_test_fail_clock_read();
        assert_eq!(
            operations.monotonic_milliseconds.unwrap()(ptr::null_mut()),
            0
        );
        assert!(operations.monotonic_milliseconds.unwrap()(ptr::null_mut()) > 0);
        let logger = operations.log.unwrap();
        logger(ptr::null_mut(), URP_AST_LOG_ERROR, ptr::null(), 0);
        for level in [URP_AST_LOG_WARNING, URP_AST_LOG_ERROR, 999] {
            logger(ptr::null_mut(), level, b"bounded ignored".as_ptr(), 7);
        }
    }
    with_state(|state| {
        assert_eq!(state.messages.len(), 3);
        for (message, level) in
            state
                .messages
                .iter()
                .zip([ffi::__LOG_WARNING, ffi::__LOG_ERROR, ffi::__LOG_NOTICE])
        {
            assert_eq!(message.1, level as c_int);
            assert_eq!(message.2, "bounded\n");
        }
    });
}

#[test]
fn control_events_keep_the_existing_asterisk_frame_contract() {
    for kind in [URP_AST_CONTROL_DTMF_BEGIN, URP_AST_CONTROL_DTMF_END] {
        for digit in [-1, 0, 256] {
            assert_eq!(control_frame(kind, digit, 20), Err(()));
        }
    }
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
