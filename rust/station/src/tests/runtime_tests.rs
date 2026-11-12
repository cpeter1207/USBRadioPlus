use super::*;

fn remote_dsp_target() -> usbradioplus_core::RemoteOperatingState {
    use usbradioplus_core::{RemoteSignal, RemoteSignalSource};
    usbradioplus_core::RemoteOperatingState {
        mode: 1,
        decode: RemoteSignal {
            source: RemoteSignalSource::Dsp,
            enabled: true,
            tone_tenths_hz: 1000,
            ..Default::default()
        },
        ..Default::default()
    }
}

pub(crate) fn remote_dsp_render(runtime: &mut StationRuntime, transmit: bool) {
    let input = [0.0; 1920];
    assert_eq!(
        // SAFETY: the fixture has no background audio worker; this test is its sole RX owner.
        unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 960) },
        0
    );
    if transmit {
        let mut output = [0.0; 1920];
        assert_eq!(
            // SAFETY: this test exclusively owns the fixture's TX callback and full output span.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            0
        );
    }
}

#[test]
fn remote_dsp_prepares_off_owner_and_commits_after_both_callback_acknowledgments() {
    let (mut runtime, mut control) = runtime(40);
    runtime.start().unwrap();
    let before = control.plan().clone();
    let mut dsp = crate::RemoteDspControl::new();
    let port = dsp.port();
    let worker = std::thread::spawn(move || {
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::from_secs(2),
            || false,
        )
    });
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    while !worker.is_finished() {
        assert!(std::time::Instant::now() < deadline);
        dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
        remote_dsp_render(&mut runtime, true);
        std::thread::yield_now();
    }
    assert!(worker.join().unwrap().is_ok());
    assert_ne!(
        control.plan().radio().receive.signaling,
        before.radio().receive.signaling
    );
    assert_eq!(control.plan().hardware(), before.hardware());
    assert_eq!(
        control
            .plan()
            .configuration()
            .station
            .receive
            .cos_assignment,
        usbradioplus_core::CarrierSource::External
    );
    assert_eq!(
        control.plan().radio().qualification.carrier_source,
        usbradioplus_radio::CarrierSource::External
    );
    assert!(!dsp.is_busy());
}

#[test]
fn remote_dsp_cancelled_half_update_retains_storage_until_reverse_acknowledgments() {
    let (mut runtime, mut control) = runtime(40);
    runtime.start().unwrap();
    let before = control.plan().clone();
    let mut dsp = crate::RemoteDspControl::new();
    let port = dsp.port();
    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let cancelled = cancel.clone();
    let worker = std::thread::spawn(move || {
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::from_secs(2),
            || cancelled.load(std::sync::atomic::Ordering::Acquire),
        )
    });
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    while !dsp.awaiting_callbacks() {
        assert!(std::time::Instant::now() < deadline);
        dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
        std::thread::yield_now();
    }
    remote_dsp_render(&mut runtime, false);
    cancel.store(true, std::sync::atomic::Ordering::Release);
    assert!(matches!(
        worker.join().unwrap(),
        Err(crate::RemoteDspError::Cancelled)
    ));
    dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
    assert!(dsp.is_busy(), "a half-adopted update cannot be reclaimed");
    while dsp.is_busy() {
        assert!(std::time::Instant::now() < deadline);
        remote_dsp_render(&mut runtime, true);
        dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
    }
    assert_eq!(control.plan(), &before);
}

#[test]
fn remote_dsp_timeout_keeps_admission_bounded_and_control_retirement_terminal() {
    let control = crate::RemoteDspControl::new();
    let port = control.port();
    assert!(matches!(
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::ZERO,
            || false
        ),
        Err(crate::RemoteDspError::Timeout)
    ));
    assert!(matches!(
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::ZERO,
            || false
        ),
        Err(crate::RemoteDspError::Busy)
    ));
    drop(control);
    assert!(matches!(
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::ZERO,
            || false
        ),
        Err(crate::RemoteDspError::Stopped)
    ));
}

#[test]
fn remote_dsp_recaptures_a_revised_generation_without_overwriting_tuner_settings() {
    let (mut runtime, mut control) = runtime(40);
    runtime.start().unwrap();
    let mut dsp = crate::RemoteDspControl::new();
    let port = dsp.port();
    let (waiting, wait) = std::sync::mpsc::sync_channel(1);
    let (release, released) = std::sync::mpsc::sync_channel(1);
    let worker = std::thread::spawn(move || {
        let mut first = Some((waiting, released));
        port.apply(
            remote_dsp_target(),
            usbradioplus_radio::SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::External,
            std::time::Duration::from_secs(2),
            || {
                if let Some((waiting, released)) = first.take() {
                    waiting.send(()).unwrap();
                    released.recv().unwrap();
                }
                false
            },
        )
    });
    wait.recv_timeout(std::time::Duration::from_secs(1))
        .unwrap();
    dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
    let mut changed = control.plan().configuration().clone();
    changed.voice_telemetry.output_gain_db = -3.0;
    let candidate = StationPlan::new("usb", changed, control.plan().transport(), 41, 960).unwrap();
    let update = runtime
        .prepare_update(&control, candidate, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    remote_dsp_render(&mut runtime, true);
    runtime.finish_update(&mut control, true).unwrap();
    release.send(()).unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    while !worker.is_finished() {
        assert!(std::time::Instant::now() < deadline);
        dsp.pump(&mut runtime, &mut control, &factory(), radio_provider());
        remote_dsp_render(&mut runtime, true);
        std::thread::yield_now();
    }
    assert!(worker.join().unwrap().is_ok());
    assert_eq!(
        control
            .plan()
            .configuration()
            .voice_telemetry
            .output_gain_db,
        -3.0
    );
}

#[test]
fn remote_hardware_signals_preserve_gpio_and_require_selected_ptt_confirmation() {
    let state = SharedHardwareState::default();
    let gpio = HardwareInputs {
        carrier: true,
        subaudible: true,
        physical_ptt: true,
        ..HardwareInputs::default()
    };
    state.publish_inputs(gpio);
    assert!(state.transmit_controls().physical_ptt_applied);
    assert_eq!(state.receive_controls().external_carrier, None);
    for observed in [None, Some(false), Some(true)] {
        let signals = RemoteHardwareSignals {
            carrier: observed,
            ptt_selected: true,
            physical_ptt: observed,
        };
        state.publish_remote_signals(signals);
        assert_eq!(state.remote_signals(), signals);
        assert_eq!(state.inputs(), gpio);
        assert_eq!(state.receive_controls().external_carrier, observed);
        assert!(state.receive_controls().hardware_subaudible);
        assert!(!state.receive_controls().subaudible_override);
        assert_eq!(
            state.transmit_controls().physical_ptt_applied,
            observed == Some(true)
        );
    }
    state.publish_remote_signals(RemoteHardwareSignals::default());
    assert!(state.transmit_controls().physical_ptt_applied);
}

#[test]
fn remote_radio_plan_update_preserves_physical_and_processing_ownership() {
    for transport in [
        crate::ControllerTransport::AppRpt,
        crate::ControllerTransport::RptAdvanced,
    ] {
        let (plan, _) = prepared(transport, 24).into_parts();
        let mut exact = plan.processing().clone();
        exact.local = None;
        exact.receive_graph = "highpass=f=60".to_owned();
        exact.transmit_graph = "volume=-2dB".to_owned();
        let original = plan.with_explicit_processing(exact);
        let mut configuration = original.configuration().clone();
        configuration.station.ctcss.receive_frequencies.clear();
        let mut radio = *original.radio();
        radio.generation_id += 1;
        radio.receive.signaling = usbradioplus_radio::ReceiveSignaling::Disabled;
        let next = original
            .clone()
            .with_radio_configuration(configuration.clone(), radio);
        assert_eq!(next.configuration(), &configuration);
        assert_eq!(next.radio(), &radio);
        assert_eq!(next.channel(), original.channel());
        assert_eq!(next.transport(), original.transport());
        assert_eq!(next.hardware(), original.hardware());
        assert_eq!(next.program_ring(), original.program_ring());
        assert_eq!(next.processing(), original.processing());
    }
}

use std::cell::Cell;
use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;

use usbradioplus_asl3::{
    AsteriskPcmMode, ControllerPcmFrame, ConversionError, ConversionProgress, DeliveryAction,
    EchoConfiguration,
};
use usbradioplus_audio::{ChannelCount, SelectedDevice};
use usbradioplus_radio::{RadioError, RadioProvider};
use usbradioplus_runtime::NativeProcessingFactory;

use crate::ControllerSetup;
use crate::control::tests::{factory, resolved};
use crate::media::tests::{
    DESTROYED_SEVEN, RECEIVE_FAILING_GENERATION, RECEIVE_UPDATE_FAILING_GENERATION,
    TRANSMIT_FAILING_GENERATION, TRANSMIT_UPDATE_FAILING_GENERATION, prepared, radio_provider,
};
use crate::program::tests::provider as ring_provider;

#[test]
fn native_media_opens_without_any_asterisk_controller_or_ring_provider() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 7);
    let selected = selected(&prepared, 960);
    let (plan, processing) = prepared.into_parts();
    let mut received = 0_u32;
    let mut transmitted = 0_u32;
    unsafe extern "C" fn receive(
        context: *mut c_void,
        _: *const crate::DirectReceiveMetadata,
        _: *mut f32,
        frames: u32,
    ) -> i32 {
        // SAFETY: this test retains the u32 until synchronous runtime destruction.
        unsafe { *context.cast::<u32>() += frames };
        0
    }
    unsafe extern "C" fn transmit(
        context: *mut c_void,
        pcm: *mut f32,
        frames: u32,
        keyed: *mut u32,
        ctcss: *mut u32,
    ) -> i32 {
        // SAFETY: all callback spans and destinations belong to this test call.
        unsafe {
            *context.cast::<u32>() += frames;
            std::slice::from_raw_parts_mut(pcm, frames as usize).fill(0.25);
            *keyed = 1;
            *ctcss = 1;
        }
        0
    }
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut received).cast(),
        receive: Some(receive),
        transmit_context: ptr::from_mut(&mut transmitted).cast(),
        transmit: Some(transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    // SAFETY: callback storage outlives the synchronous runtime below.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let (mut runtime, _control) =
        StationRuntime::open_native(media, &selected, audio_provider()).unwrap();
    runtime.start().unwrap();
    runtime.stop().unwrap();
    drop(runtime);
    assert!(received > 0);
    assert!(transmitted > 0);
}

#[test]
fn feedback_binding_is_serial_stopped_only_and_does_not_retag_off_as_readback() {
    use crate::{
        FEEDBACK_ACQUISITION_VALID, FEEDBACK_DUPLEX_VALID, FEEDBACK_PHASE_VALID,
        FEEDBACK_READBACK_VALID, FEEDBACK_SERVICE_VALID, RadioFeedback, RadioFeedbackPort,
    };
    let (mut runtime, _) = runtime(20);
    let mut received = [RadioFeedback::default(); 8];
    let mut calls = 0usize;
    let mut context = (&mut received, &mut calls);
    unsafe extern "C" fn publish(context: *mut c_void, record: *const RadioFeedback) {
        let (records, calls) =
            // SAFETY: this fixture retains exact output slots through stopped unbind.
            unsafe { &mut *context.cast::<(&mut [RadioFeedback; 8], &mut usize)>() };
        // SAFETY: publication lends a complete immutable record for this call.
        records[**calls] = unsafe { *record };
        **calls += 1;
    }
    let port = RadioFeedbackPort {
        struct_size: size_of::<RadioFeedbackPort>() as u32,
        abi_version: 1,
        context: ptr::from_mut(&mut context).cast(),
        publish: Some(publish),
    };
    // SAFETY: context lives until this test unbinds after successful stop.
    unsafe { runtime.bind_feedback(Some(port)) }.unwrap();
    runtime.start().unwrap();
    // SAFETY: the rejected running bind retains no new context.
    assert!(unsafe { runtime.bind_feedback(None) }.is_err());
    let request = runtime.hardware.feedback_request().unwrap();
    runtime
        .hardware
        .publish_physical_feedback(request, Some(false), None, true);
    crate::allocation::without_heap(|| {
        remote_dsp_render(&mut runtime, true);
        remote_dsp_render(&mut runtime, true);
    });
    runtime.stop().unwrap();
    // SAFETY: successful stop has quiesced the callback, so None releases its borrow.
    unsafe { runtime.bind_feedback(None) }.unwrap();
    runtime.start().unwrap();
    runtime.stop().unwrap();
    assert_eq!(calls, 3);
    assert_eq!(received[0].flags, FEEDBACK_DUPLEX_VALID);
    assert_eq!(received[0].generation_id, 20);
    assert_eq!(received[1].flags & FEEDBACK_READBACK_VALID, 0);
    assert_ne!(received[1].flags & FEEDBACK_SERVICE_VALID, 0);
    assert_ne!(received[1].flags & FEEDBACK_PHASE_VALID, 0);
    assert_ne!(received[1].flags & FEEDBACK_ACQUISITION_VALID, 0);
    assert_eq!(received[1].acquisition_age_frames, 0);
    assert_eq!(received[2].acquisition_age_frames, 960);
}

#[test]
fn feedback_failed_start_keeps_context_bound_until_actual_stop_succeeds() {
    let (mut runtime, _) = runtime(20);
    let mut calls = 0usize;
    unsafe extern "C" fn publish(context: *mut c_void, _: *const crate::RadioFeedback) {
        // SAFETY: the test retains this counter until confirmed stop and unbind.
        unsafe { *context.cast::<usize>() += 1 };
    }
    let port = crate::RadioFeedbackPort {
        struct_size: size_of::<crate::RadioFeedbackPort>() as u32,
        abi_version: 1,
        context: ptr::from_mut(&mut calls).cast(),
        publish: Some(publish),
    };
    // SAFETY: the serial endpoint and context stay alive through eventual confirmed stop.
    unsafe { runtime.bind_feedback(Some(port)) }.unwrap();
    PARTIAL_START_ERROR.set(true);
    PARTIAL_STOP_ERROR.set(true);
    assert!(runtime.start().is_err());
    assert!(!runtime.started());
    assert!(
        runtime.running(),
        "a failed start cannot prove callback quiescence"
    );
    assert!(runtime.stop().is_err());
    // SAFETY: rejected stopped-only mutation must retain the original binding.
    assert!(unsafe { runtime.bind_feedback(None) }.is_err());
    let before = calls;
    let mut output = [0.0; 1920];
    assert_eq!(
        // SAFETY: the fake adapter remains active after failed start/stop; this test is its sole TX owner.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                960,
            )
        },
        0
    );
    assert_eq!(calls, before + 1);
    PARTIAL_STOP_ERROR.set(false);
    runtime.stop().unwrap();
    assert!(!runtime.running());
    // SAFETY: successful provider stop now permits releasing this borrowed context.
    unsafe { runtime.bind_feedback(None) }.unwrap();
    let before = calls;
    PARTIAL_START_ERROR.set(false);
    runtime.start().unwrap();
    assert!(runtime.started());
    runtime.stop().unwrap();
    assert_eq!(calls, before);
}

#[test]
fn feedback_forwards_only_existing_phases_and_exhausted_identity_stays_unknown() {
    use crate::media::tests::{TRANSMIT_INDEX_OVERRIDE, TRANSMIT_STATE_OVERRIDE};
    let (mut runtime, _) = runtime(20);
    let mut observed = crate::RadioFeedback::default();
    unsafe extern "C" fn publish(context: *mut c_void, record: *const crate::RadioFeedback) {
        // SAFETY: both records stay live for the bounded synchronous fixture call.
        unsafe { *context.cast::<crate::RadioFeedback>() = *record };
    }
    let mut port = crate::RadioFeedbackPort {
        struct_size: size_of::<crate::RadioFeedbackPort>() as u32,
        abi_version: 1,
        context: ptr::from_mut(&mut observed).cast(),
        publish: Some(publish),
    };
    // SAFETY: capture outlives confirmed stop and stopped unbind.
    unsafe { runtime.bind_feedback(Some(port)) }.unwrap();
    runtime.start().unwrap();
    for state in [0, 1, 2, 4, 5] {
        TRANSMIT_STATE_OVERRIDE.set(Some(state));
        runtime.hardware.publish_requests(ControllerRequests {
            transmit: matches!(state, 1 | 2 | 4),
            render_admitted: true,
            ..Default::default()
        });
        remote_dsp_render(&mut runtime, true);
        remote_dsp_render(&mut runtime, true);
        assert_eq!(observed.transmitter_state, state);
        assert_ne!(observed.flags & crate::FEEDBACK_PHASE_VALID, 0);
    }
    runtime.stop().unwrap();
    port.publish = None;
    // SAFETY: a missing publisher is a valid stopped no-op binding.
    unsafe { runtime.bind_feedback(Some(port)) }.unwrap();
    let before = observed;
    runtime.start().unwrap();
    assert_eq!(observed, before);
    // SAFETY: this fake adapter has no worker thread; the test owns its TX context.
    let context = unsafe { &mut *runtime._transmit_context.get() };
    context.feedback_record.request_id = u64::MAX;
    context.feedback_record.requested_ptt = 0;
    context.feedback_record.rendered_ptt = 0;
    runtime.hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..Default::default()
    });
    remote_dsp_render(&mut runtime, true);
    assert_eq!(runtime.hardware.feedback_request(), None);
    assert_eq!(
        // SAFETY: serial test access follows the completed fake callback above.
        unsafe { (*runtime._transmit_context.get()).feedback_record.flags },
        crate::FEEDBACK_DUPLEX_VALID
    );
    // SAFETY: the same serial fixture can prepare the source-position boundary.
    unsafe {
        (*runtime._transmit_context.get())
            .feedback_record
            .request_id = 0
    };
    TRANSMIT_INDEX_OVERRIDE.set(Some(u64::MAX));
    remote_dsp_render(&mut runtime, true);
    assert_eq!(runtime.hardware.feedback_request(), None);
    TRANSMIT_INDEX_OVERRIDE.set(None);
    TRANSMIT_STATE_OVERRIDE.set(None);
    runtime.stop().unwrap();
}

#[test]
fn native_media_rejects_an_incompatible_direct_callback_descriptor() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 7);
    let (plan, processing) = prepared.into_parts();
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: 0,
        receive_context: ptr::null_mut(),
        receive: Some(direct_receive),
        transmit_context: ptr::null_mut(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    assert!(matches!(
        // SAFETY: rejection occurs before callbacks are retained or invoked.
        unsafe {
            crate::NativeStationMedia::prepare(
                *plan.radio(),
                processing,
                radio_provider(),
                callbacks,
            )
        },
        Err(crate::StationMediaError::ControllerTransportMismatch)
    ));
}

#[test]
fn native_planned_media_rejects_mismatched_radio_before_audio_open() {
    CREATES.set(0);
    DESTROYED_SEVEN.set(0);
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 7);
    let selected = selected(&prepared, 960);
    let (plan, processing) = prepared.into_parts();
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::null_mut(),
        receive: Some(direct_receive),
        transmit_context: ptr::null_mut(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    // SAFETY: rejection occurs before either direct callback can be invoked.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let mut radio = *plan.radio();
    radio.generation_id += 1;
    let mismatched = StationPlan::native(
        "native",
        plan.configuration().clone(),
        plan.hardware().clone(),
        radio,
        plan.processing(),
    );
    let result =
        StationRuntime::open_native_planned(media, mismatched, &selected, audio_provider());
    assert_eq!(CREATES.get(), 0, "mismatched media must not open audio");
    assert!(matches!(result, Err(AudioError::InvalidArgument)));
    assert_eq!(DESTROYED_SEVEN.get(), 1, "rejected media must be released");
}

#[test]
fn native_failed_render_and_oversized_callback_clear_key_and_output() {
    for (generation, frames) in [(TRANSMIT_FAILING_GENERATION, 4), (24, 961)] {
        let prepared = prepared(crate::ControllerTransport::RptAdvanced, generation);
        let selected = selected(&prepared, 960);
        let (plan, processing) = prepared.into_parts();
        let mut capture = DirectCapture {
            tx_keyed: 1,
            ctcss_enabled: 1,
            ..DirectCapture::default()
        };
        let callbacks = crate::DirectCallbacks {
            struct_size: size_of::<crate::DirectCallbacks>() as u32,
            abi_version: crate::DirectCallbacks::ABI_VERSION,
            receive_context: ptr::from_mut(&mut capture).cast(),
            receive: Some(direct_receive),
            transmit_context: ptr::from_mut(&mut capture).cast(),
            transmit: Some(direct_transmit),
            metadata: None,
            accepted_abi_version: 0,
        };
        // SAFETY: this serial fixture retains callback storage through runtime teardown.
        let media = unsafe {
            crate::NativeStationMedia::prepare(
                *plan.radio(),
                processing,
                radio_provider(),
                callbacks,
            )
        }
        .unwrap();
        let (runtime, _control) =
            StationRuntime::open_native(media, &selected, audio_provider()).unwrap();
        assert_eq!(
            // SAFETY: the live native context rejects the null output before any span access.
            unsafe {
                transmit_callback(runtime._transmit_context.get().cast(), ptr::null_mut(), 4)
            },
            CALLBACK_FAILED
        );
        runtime
            .hardware
            .publish_transmit_result(transmit_result(true, 1000));
        let mut output = vec![1.0; frames * 2];
        assert_eq!(
            // SAFETY: the stopped runtime owns this context and advertised output span.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    frames as u32,
                )
            },
            CALLBACK_FAILED
        );
        assert!(output.iter().all(|sample| *sample == 0.0));
        assert!(!runtime.hardware.outputs().logical_ptt);
        assert_eq!(runtime.hardware.outputs().selected_ctcss_tenths_hz, None);
    }
}

#[test]
fn native_direct_transmit_failure_immediately_silences_and_unkeys() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 24);
    let selected = selected(&prepared, 960);
    let (plan, processing) = prepared.into_parts();
    let mut capture = DirectCapture {
        tx_status: -1,
        tx_keyed: 1,
        ctcss_enabled: 1,
        ..DirectCapture::default()
    };
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    // SAFETY: capture remains live through the stopped runtime's synchronous callback.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let (runtime, _control) =
        StationRuntime::open_native(media, &selected, audio_provider()).unwrap();
    runtime.hardware.publish_transmit_intent(true, false, false);
    let mut output = [1.0; 8];
    assert_eq!(
        // SAFETY: the stopped runtime owns the live context and exact stereo span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        CALLBACK_FAILED
    );
    assert_eq!(capture.transmit_calls, 1);
    assert_eq!(output, [0.0; 8]);
    assert!(!runtime.hardware.outputs().logical_ptt);
    assert_eq!(runtime.hardware.callback_statistics().transmit_failures, 1);
    assert!(!runtime.hardware.remote_activity().transmit_requested);
}

#[test]
fn native_manual_key_and_ctcss_intent_survive_direct_controller_results() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 24);
    let selected = selected(&prepared, 960);
    let (plan, processing) = prepared.into_parts();
    let mut capture = DirectCapture::default();
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    // SAFETY: capture stays live until the stopped runtime is destroyed.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let (runtime, _control) =
        StationRuntime::open_native(media, &selected, audio_provider()).unwrap();
    let tone = CtcssTone::from_tenths_hz(1_000).unwrap();
    for (manual_key, inhibited, controller_key, controller_ctcss, expected_inhibit) in [
        (true, false, 0, 0, false),
        (false, true, 1, 1, true),
        (false, false, 1, 0, true),
        (false, false, 1, 1, false),
    ] {
        runtime.hardware.publish_requests(ControllerRequests {
            transmit: manual_key,
            calibrated_test_tone: manual_key,
            forced_ctcss: manual_key.then_some(tone),
            ctcss_inhibit: inhibited,
            ..ControllerRequests::default()
        });
        capture.tx_keyed = controller_key;
        capture.ctcss_enabled = controller_ctcss;
        let mut output = [0.0; 8];
        assert_eq!(
            // SAFETY: the stopped runtime exclusively owns both callback contexts.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    4,
                )
            },
            CALLBACK_OK
        );
        assert_eq!(
            output,
            if expected_inhibit {
                [0.5; 8]
            } else {
                [-0.375; 8]
            }
        );
        assert!(runtime.hardware.outputs().logical_ptt);
    }
    // Session cleanup clears the manually published intent; the controller callback
    // still requests ordinary transmission and retains its CTCSS setting.
    runtime
        .hardware
        .publish_requests(ControllerRequests::default());
    capture.tx_keyed = 1;
    capture.ctcss_enabled = 1;
    let mut output = [0.0; 8];
    assert_eq!(
        // SAFETY: callback contexts and the stereo output span remain exclusively owned.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        CALLBACK_OK
    );
    assert!(runtime.hardware.outputs().logical_ptt);
    assert_eq!(runtime.hardware.requests(), ControllerRequests::default());
}

#[test]
fn native_planned_updates_share_acknowledgments_rollback_and_stream_ownership() {
    STARTS.set(0);
    STOPS.set(0);
    DESTROYS.set(0);
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 24);
    let selected = selected(&prepared, 960);
    let (plan, _) = prepared.into_parts();
    let mut exact = plan.processing().clone();
    exact.local = None;
    exact.voice_telemetry = None;
    exact.receive_graph = "highpass=f=60".to_owned();
    exact.transmit_graph = "volume=-2dB".to_owned();
    let processing = factory().prepare(&exact).unwrap();
    let plan = StationPlan::native(
        "native",
        plan.configuration().clone(),
        plan.hardware().clone(),
        *plan.radio(),
        exact.clone(),
    );
    let mut capture = DirectCapture {
        tx_keyed: 1,
        ctcss_enabled: 1,
        ..DirectCapture::default()
    };
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    // SAFETY: callback capture outlives synchronous stream shutdown.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let original = plan.clone();
    let (mut runtime, mut control) =
        StationRuntime::open_native_planned(media, plan, &selected, audio_provider()).unwrap();
    runtime.start().unwrap();
    let mut config = original.configuration().clone();
    config.local.input_gain_db = 3.0;
    exact.receive_graph = "highpass=f=120".to_owned();
    let candidate = StationPlan::new("native", config, original.transport(), 25, 960)
        .unwrap()
        .with_explicit_processing(exact);
    let update = runtime
        .prepare_update(&control, candidate, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    assert!(matches!(
        runtime.update_result(),
        Err(StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    assert!(matches!(
        runtime.update_result(),
        Err(StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, false);
    runtime.accept_update(&mut control).unwrap();
    assert_eq!(control.plan().configuration().local.input_gain_db, 3.0);
    assert_eq!(control.plan().processing().receive_graph, "highpass=f=120");
    assert!(matches!(
        runtime.finish_update(&mut control, false),
        Err(StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    runtime.finish_update(&mut control, false).unwrap();
    assert_eq!(control.plan(), &original);
    assert!(runtime.hardware.outputs().logical_ptt);
    assert_eq!((STARTS.get(), STOPS.get(), DESTROYS.get()), (1, 0, 0));
}

#[test]
fn stopped_pending_update_drains_both_owners_before_rollback_and_restart() {
    for partial in [None, Some(true), Some(false)] {
        let (mut runtime, mut control) = runtime(20);
        let original = control.plan().clone();
        runtime.start().unwrap();
        let candidate = StationPlan::new(
            "usb",
            original.configuration().clone(),
            original.transport(),
            21,
            960,
        )
        .unwrap();
        let update = runtime
            .prepare_update(&control, candidate, &factory(), radio_provider())
            .unwrap();
        runtime.begin_update(&control, update).unwrap();
        if let Some(receive) = partial {
            update_tick(&mut runtime, receive);
        }
        assert!(matches!(
            runtime.update_result(),
            Err(StationUpdateError::Pending)
        ));
        runtime.stop().unwrap();
        runtime.finish_update(&mut control, false).unwrap();
        assert_eq!(control.plan(), &original);
        runtime.start().unwrap();
        let candidate = StationPlan::new(
            "usb",
            original.configuration().clone(),
            original.transport(),
            22,
            960,
        )
        .unwrap();
        assert!(
            runtime
                .prepare_update(&control, candidate, &factory(), radio_provider())
                .is_ok()
        );
    }
}

impl StationRuntime {
    // Tests have no delivery task; join the production snapshot/preparation steps.
    pub(crate) fn prepare_update(
        &self,
        control: &StationControlHost,
        plan: StationPlan,
        factory: &NativeProcessingFactory,
        provider: RadioProvider,
    ) -> Result<StationUpdate, StationUpdateError> {
        self.snapshot_update(control, factory, provider)?
            .prepare(plan)
    }
}

#[test]
fn update_errors_explain_every_failure() {
    use crate::StationUpdateError as Error;

    assert_eq!(
        Error::Incompatible.to_string(),
        "setting requires device handoff"
    );
    assert_eq!(
        Error::Busy.to_string(),
        "another live update is awaiting completion"
    );
    assert_eq!(
        Error::Pending.to_string(),
        "audio callbacks have not acknowledged the live update"
    );
    assert_eq!(
        Error::Preparation(crate::StationPreparationError::Radio(
            RadioError::InvalidArgument,
        ))
        .to_string(),
        "radio preparation failed: invalid radio-session argument"
    );
    assert_eq!(
        Error::Radio(RadioError::InvalidArgument).to_string(),
        "invalid radio-session argument"
    );
}

const AUDIO_ABI: u32 = 2;
const AUDIO_OK: c_int = 0;
thread_local! {
    static PARTIAL_START_ERROR: Cell<bool> = const { Cell::new(false) };
    static PARTIAL_STOP_ERROR: Cell<bool> = const { Cell::new(false) };
    static CREATES: Cell<u32> = const { Cell::new(0) };
    static STARTS: Cell<u32> = const { Cell::new(0) };
    static STOPS: Cell<u32> = const { Cell::new(0) };
    static DESTROYS: Cell<u32> = const { Cell::new(0) };
    static OUTPUT_SAMPLE: Cell<u32> = const { Cell::new(0) };
    static CREATE_FAILS: Cell<bool> = const { Cell::new(false) };
    static STATISTICS_FAILS: Cell<bool> = const { Cell::new(false) };
}

type ReceiveWorker = unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32;
type TransmitWorker = unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32;

#[repr(C)]
struct RawStreamConfig {
    struct_size: u32,
    abi_version: u32,
    native_sample_rate_hz: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
    input_device_index: i32,
    output_device_index: i32,
    input_device_channels: u32,
    output_device_channels: u32,
    receive_worker: Option<ReceiveWorker>,
    receive_worker_context: *mut c_void,
    transmit_worker: Option<TransmitWorker>,
    transmit_worker_context: *mut c_void,
    extra_output_buffer_milliseconds: u32,
    extra_input_buffer_milliseconds: u32,
}

#[repr(C)]
#[derive(Default)]
struct RawStatistics {
    struct_size: u32,
    abi_version: u32,
    callback_count: u64,
    callback_frame_count: u64,
    oversized_callback_count: u64,
    worker_failure_count: u64,
    input_overflow_count: u64,
    output_underflow_count: u64,
    device_error_count: u64,
    input_clip_sample_count: u64,
    output_clip_sample_count: u64,
    input_peak: f32,
    input_rms: f32,
    output_peak: f32,
    output_rms: f32,
    last_portaudio_error: i32,
    callback_last_duration_ns: u64,
    callback_max_duration_ns: u64,
    callback_last_start_delay_ns: u64,
    callback_max_start_delay_ns: u64,
    callback_late_start_count: u64,
    callback_late_start_tolerance_ns: u64,
    last_input_xrun_monotonic_ns: u64,
    last_output_xrun_monotonic_ns: u64,
    callback_clock_error_count: u64,
    capture_callback_count: u64,
}

#[repr(C)]
#[derive(Default)]
struct RawTiming {
    struct_size: u32,
    abi_version: u32,
    input_latency_seconds: f64,
    output_latency_seconds: f64,
    sample_rate_hz: f64,
}

type StreamCreate = unsafe extern "C" fn(*const RawStreamConfig, *mut *mut c_void) -> c_int;
type StreamControl = unsafe extern "C" fn(*mut c_void) -> c_int;
type StreamStatistics = unsafe extern "C" fn(*const c_void, *mut RawStatistics) -> c_int;
type StreamTiming = unsafe extern "C" fn(*const c_void, *mut RawTiming) -> c_int;
type StreamDestroy = unsafe extern "C" fn(*mut c_void);
type Unused = unsafe extern "C" fn();

#[repr(C)]
struct AudioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    stream_create: Option<StreamCreate>,
    stream_start: Option<StreamControl>,
    stream_stop: Option<StreamControl>,
    stream_get_stats: Option<StreamStatistics>,
    stream_destroy: Option<StreamDestroy>,
    mixer_create: Option<Unused>,
    mixer_get_range_centibels: Option<Unused>,
    mixer_get_centibels: Option<Unused>,
    mixer_set_centibels: Option<Unused>,
    mixer_destroy: Option<Unused>,
    mixer_create_for_usb_interface: Option<Unused>,
    mixer_get_range_steps: Option<Unused>,
    mixer_get_steps: Option<Unused>,
    mixer_set_steps: Option<Unused>,
    mixer_get_normalized: Option<Unused>,
    mixer_set_normalized: Option<Unused>,
    mixer_get_switch: Option<Unused>,
    mixer_set_switch: Option<Unused>,
    usb_device_resolve: Option<Unused>,
    usb_device_select: Option<Unused>,
    stream_get_timing: Option<StreamTiming>,
    cm119_mixer_paths_resolve: Option<Unused>,
}

// SAFETY: the descriptor and its function pointers are immutable and static.
unsafe impl Sync for AudioDescriptor {}

struct FakeStream {
    active: bool,
    receive: ReceiveWorker,
    receive_context: *mut c_void,
    transmit: TransmitWorker,
    transmit_context: *mut c_void,
}

unsafe extern "C" fn stream_create(
    config: *const RawStreamConfig,
    output: *mut *mut c_void,
) -> c_int {
    CREATES.set(CREATES.get() + 1);
    if CREATE_FAILS.get() {
        return -1;
    }
    // SAFETY: the audio wrapper supplies both complete objects.
    let (Some(config), Some(output)) = (unsafe { config.as_ref() }, unsafe { output.as_mut() })
    else {
        return -1;
    };
    assert_eq!(config.native_sample_rate_hz, 48_000);
    assert_eq!(config.input_device_channels, 1);
    assert_eq!(config.output_device_channels, 2);
    assert_eq!(config.extra_input_buffer_milliseconds, 0);
    assert_eq!(config.extra_output_buffer_milliseconds, 0);
    let Some(receive) = config.receive_worker else {
        return -1;
    };
    let Some(transmit) = config.transmit_worker else {
        return -1;
    };
    *output = Box::into_raw(Box::new(FakeStream {
        active: false,
        receive,
        receive_context: config.receive_worker_context,
        transmit,
        transmit_context: config.transmit_worker_context,
    }))
    .cast();
    AUDIO_OK
}

unsafe extern "C" fn stream_start(stream: *mut c_void) -> c_int {
    STARTS.set(STARTS.get() + 1);
    // SAFETY: create returns this live FakeStream allocation.
    let stream = unsafe { &mut *stream.cast::<FakeStream>() };
    stream.active = true;
    let mut output = [f32::NAN; ADVANCED_FRAME_SAMPLES * 2];
    // SAFETY: both worker contexts and buffers remain live for these calls.
    let transmit = unsafe {
        (stream.transmit)(
            stream.transmit_context,
            output.as_mut_ptr(),
            ADVANCED_FRAME_SAMPLES as u32,
        )
    };
    OUTPUT_SAMPLE.set(output[0].to_bits());
    let input = [0.25_f32; ADVANCED_FRAME_SAMPLES * 2];
    // SAFETY: both worker contexts and buffers remain live for these calls.
    let receive = unsafe {
        (stream.receive)(
            stream.receive_context,
            input.as_ptr(),
            ADVANCED_FRAME_SAMPLES as u32,
        )
    };
    u32::from(transmit != AUDIO_OK || receive != AUDIO_OK || PARTIAL_START_ERROR.get()) as c_int
}

unsafe extern "C" fn stream_stop(stream: *mut c_void) -> c_int {
    STOPS.set(STOPS.get() + 1);
    // SAFETY: the stopped or possibly active fake stream is exclusively retained here.
    let stream = unsafe { &mut *stream.cast::<FakeStream>() };
    if PARTIAL_STOP_ERROR.get() && stream.active {
        return -1;
    }
    stream.active = false;
    AUDIO_OK
}

unsafe extern "C" fn stream_statistics(
    _stream: *const c_void,
    output: *mut RawStatistics,
) -> c_int {
    if STATISTICS_FAILS.get() {
        return -1;
    }
    // SAFETY: the wrapper supplies writable statistics storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    output.callback_count = 2;
    output.capture_callback_count = 1;
    AUDIO_OK
}

unsafe extern "C" fn stream_timing(_stream: *const c_void, output: *mut RawTiming) -> c_int {
    // SAFETY: the wrapper supplies writable timing storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    output.input_latency_seconds = 0.01;
    output.output_latency_seconds = 0.02;
    output.sample_rate_hz = 48_000.0;
    AUDIO_OK
}

unsafe extern "C" fn stream_destroy(stream: *mut c_void) {
    if !stream.is_null() {
        // SAFETY: create transferred this allocation to the stream owner.
        drop(unsafe { Box::from_raw(stream.cast::<FakeStream>()) });
        DESTROYS.set(DESTROYS.get() + 1);
    }
}

unsafe extern "C" fn unused() {}

static AUDIO_DESCRIPTOR: AudioDescriptor = AudioDescriptor {
    struct_size: size_of::<AudioDescriptor>() as u32,
    abi_version: AUDIO_ABI,
    capability_name: c"rptadv.portaudio-alsa-audio".as_ptr(),
    stream_create: Some(stream_create),
    stream_start: Some(stream_start),
    stream_stop: Some(stream_stop),
    stream_get_stats: Some(stream_statistics),
    stream_destroy: Some(stream_destroy),
    mixer_create: Some(unused),
    mixer_get_range_centibels: Some(unused),
    mixer_get_centibels: Some(unused),
    mixer_set_centibels: Some(unused),
    mixer_destroy: Some(unused),
    mixer_create_for_usb_interface: Some(unused),
    mixer_get_range_steps: Some(unused),
    mixer_get_steps: Some(unused),
    mixer_set_steps: Some(unused),
    mixer_get_normalized: Some(unused),
    mixer_set_normalized: Some(unused),
    mixer_get_switch: Some(unused),
    mixer_set_switch: Some(unused),
    usb_device_resolve: Some(unused),
    usb_device_select: Some(unused),
    stream_get_timing: Some(stream_timing),
    cm119_mixer_paths_resolve: Some(unused),
};

fn audio_provider() -> AudioProvider {
    // SAFETY: this immutable complete descriptor has process lifetime.
    unsafe { AudioProvider::from_raw_descriptor(ptr::from_ref(&AUDIO_DESCRIPTOR).cast()) }.unwrap()
}

fn selected(prepared: &crate::PreparedStation, maximum: u32) -> SelectedHardwarePlan {
    prepared
        .plan()
        .hardware()
        .select(
            &SelectedDevice {
                interface_path: Some("3-1:1.0".to_owned()),
                serial: None,
                alsa_card_index: 1,
                input_device_index: 2,
                output_device_index: 3,
                input_channels: ChannelCount::Mono,
                output_channels: ChannelCount::Stereo,
            },
            maximum,
        )
        .unwrap()
}

pub(crate) fn runtime(generation: u64) -> (StationRuntime, StationControlHost) {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, generation);
    let selected = selected(&prepared, ADVANCED_FRAME_SAMPLES as u32);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 4 },
        )
        .unwrap();
    StationRuntime::open(media, &selected, audio_provider()).unwrap()
}

fn media_with_maximum(
    transport: crate::ControllerTransport,
    generation: u64,
    maximum: u32,
) -> (crate::StationMedia, SelectedHardwarePlan) {
    let (channel, configuration) = resolved("[usb]\n");
    let prepared = crate::StationPlan::new(channel, configuration, transport, generation, maximum)
        .unwrap()
        .prepare(&factory())
        .unwrap();
    let selected = selected(&prepared, maximum);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 2 },
        )
        .unwrap();
    (media, selected)
}

fn transmit_result(logical_ptt: bool, tone: i32) -> TransmitResult {
    TransmitResult {
        generation_id: 1,
        first_sample_index: 0,
        frame_count: 1,
        logical_ptt,
        transmitter_state: usbradioplus_radio::TransmitterState::Active,
        selected_ctcss_tenths_hz: tone,
        program_peak: 0.0,
        program_rms: 0.0,
        program_rail_samples: 0,
        output_peak: 0.0,
        output_rms: 0.0,
        output_rail_samples: 0,
        periodic_status_due: false,
        program_ring: usbradioplus_radio::RingObservation::default(),
    }
}

#[test]
fn live_update_preserves_stream_ptt_and_sample_clock() {
    STARTS.set(0);
    STOPS.set(0);
    DESTROYS.set(0);
    let (mut runtime, mut control) = runtime(20);
    let original_rx = control.receive_loudness().unwrap().epoch;
    let original_tx = control.transmit_loudness().unwrap().epoch;
    let hardware = runtime.hardware_state();
    hardware.publish_inputs(HardwareInputs {
        carrier: true,
        subaudible: true,
        physical_ptt: true,
        ..HardwareInputs::default()
    });
    hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..ControllerRequests::default()
    });
    runtime.start().unwrap();
    let before = control.radio().radio().snapshot().unwrap();
    let mut configuration = control.plan().configuration().clone();
    configuration.local.input_gain_db = 3.0;
    configuration.voice_telemetry.output_gain_db = -1.0;
    let plan = StationPlan::new("usb", configuration, control.plan().transport(), 21, 960).unwrap();
    let update = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    assert!(matches!(
        runtime.update_result(),
        Err(crate::StationUpdateError::Pending)
    ));
    // Explicit ticks let the test observe each owner without a timing-dependent sleep.
    update_tick(&mut runtime, true);
    assert_ne!(control.receive_loudness().unwrap().epoch, original_rx);
    assert_eq!(control.transmit_loudness().unwrap().epoch, original_tx);
    assert!(matches!(
        runtime.update_result(),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, false);
    assert_ne!(control.transmit_loudness().unwrap().epoch, original_tx);
    runtime.accept_update(&mut control).unwrap();
    runtime.finish_update(&mut control, true).unwrap();
    let after = control.radio().radio().snapshot().unwrap();
    assert!(after.receive_frames > before.receive_frames);
    assert!(after.transmit_frames > before.transmit_frames);
    assert!(hardware.outputs().logical_ptt);
    assert_eq!((STARTS.get(), STOPS.get(), DESTROYS.get()), (1, 0, 0));
    assert_eq!(control.plan().configuration().local.input_gain_db, 3.0);
}

#[test]
fn programme_retirement_is_idempotent_serialized_and_generation_scoped() {
    let (mut runtime, mut control) = runtime(20);
    runtime.start().unwrap();
    let rx = control.receive_loudness().unwrap().epoch;
    let tx = control.transmit_loudness().unwrap().epoch;
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 0)
            .is_err()
    );
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 7),
        Err(crate::StationUpdateError::Pending)
    ));
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 7),
        Err(crate::StationUpdateError::Pending)
    ));
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 8),
        Err(crate::StationUpdateError::Busy)
    ));
    assert!(matches!(
        runtime.finish_update(&mut control, true),
        Err(crate::StationUpdateError::Busy)
    ));
    assert!(runtime.accept_update(&mut control).is_err());
    update_tick(&mut runtime, true);
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 7),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, false);
    assert!(matches!(
        runtime.accept_update(&mut control),
        Err(crate::StationUpdateError::Busy)
    ));
    assert_ne!(control.transmit_loudness().unwrap().epoch, tx);
    assert_eq!(control.receive_loudness().unwrap().epoch, rx);
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 7)
        .unwrap();
    let retired = control.transmit_loudness().unwrap().epoch;
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 7)
        .unwrap();
    assert_eq!(control.transmit_loudness().unwrap().epoch, retired);
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 6)
            .is_err()
    );
    let mut next = *control.plan().radio();
    next.generation_id = 21;
    let plan = control
        .plan()
        .clone()
        .with_radio_configuration(control.plan().configuration().clone(), next);
    let update = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 7),
        Err(crate::StationUpdateError::Busy)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    runtime.finish_update(&mut control, true).unwrap();
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 7)
            .is_err()
    );
}

#[test]
fn programme_retirement_failed_half_keeps_fresh_reverse_until_ack_and_never_acks_failure() {
    let (mut runtime, mut control) = runtime(20);
    runtime.start().unwrap();
    let old = control.transmit_loudness().unwrap().epoch;
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Pending)
    ));
    crate::media::tests::FAIL_NEXT_APPLY.set(true);
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Pending)
    ));
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 2),
        Err(crate::StationUpdateError::Busy)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 1)
            .is_err()
    );
    assert_ne!(control.transmit_loudness().unwrap().epoch, old);
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 1)
            .is_err()
    );
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 2),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 2)
        .unwrap();
}

#[test]
fn programme_retirement_checks_foreign_failed_start_stopped_and_preparation_ownership() {
    let (mut owned, mut control) = runtime(20);
    let (_foreign, mut foreign) = runtime(21);
    assert!(matches!(
        owned.retire_programme(&mut foreign, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Incompatible)
    ));
    crate::control::tests::fail_next_graph_preparation();
    assert!(matches!(
        owned.retire_programme(&mut control, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Preparation(_))
    ));
    assert!(
        owned
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 1)
            .is_err()
    );
    owned
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 2)
        .unwrap();
    PARTIAL_START_ERROR.set(true);
    assert!(owned.start().is_err());
    assert!(matches!(
        owned.retire_programme(&mut control, &factory(), radio_provider(), 20, 3),
        Err(crate::StationUpdateError::Busy)
    ));
    PARTIAL_START_ERROR.set(false);
    owned.stop().unwrap();
}

#[test]
fn programme_retirement_failed_stop_fences_new_work_and_retains_pending_cleanup() {
    let (mut runtime, mut control) = runtime(20);
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 19, 1)
            .is_err()
    );
    runtime.start().unwrap();
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Pending)
    ));
    PARTIAL_STOP_ERROR.set(true);
    assert!(runtime.stop().is_err());
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 1),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 1)
        .unwrap();
    // A completed same-ID poll allocates/prepares nothing even while fresh
    // retirement admission is fenced by the failed stop.
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 1)
        .unwrap();
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 2),
        Err(crate::StationUpdateError::Busy)
    ));
    PARTIAL_STOP_ERROR.set(false);
    runtime.stop().unwrap();
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 2)
        .unwrap();
    runtime.start().unwrap();
    PARTIAL_STOP_ERROR.set(true);
    assert!(runtime.stop().is_err());
    assert!(
        runtime
            .retire_programme(&mut control, &factory(), radio_provider(), 20, 3)
            .is_err()
    );
    PARTIAL_STOP_ERROR.set(false);
    runtime.start().unwrap();
    assert!(
        matches!(
            runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 3),
            Err(crate::StationUpdateError::Busy)
        ),
        "already-started no-op is not a new provider receipt"
    );
    runtime.stop().unwrap();
    runtime.start().unwrap();
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 3),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 3)
        .unwrap();
}

#[test]
fn programme_feedback_preserves_physical_prefix_and_requires_completed_matching_render() {
    assert_eq!(
        std::mem::offset_of!(crate::RadioFeedback, programme_buffering),
        112
    );
    assert_eq!(size_of::<crate::RadioFeedback>(), 184);
    let (mut runtime, mut control) = runtime(20);
    runtime.start().unwrap();
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 20, 9),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    // SAFETY: no fixture background worker exists; test exclusively owns TX.
    let before = unsafe { (*runtime._transmit_context.get()).feedback_record };
    assert_eq!(before.programme_retirement_id, 0);
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 20, 9)
        .unwrap();
    crate::allocation::without_heap(|| update_tick(&mut runtime, false));
    // SAFETY: the callback has returned to this fixture's sole TX owner.
    let committed = unsafe { (*runtime._transmit_context.get()).feedback_record };
    assert_eq!(committed.generation_id, 20);
    assert_eq!(committed.programme_retirement_id, 9);
    assert_eq!(
        committed.programme_buffering.flags, 0,
        "old provider stays unknown"
    );
    let mut radio = *control.plan().radio();
    radio.generation_id = 21;
    let next = control
        .plan()
        .clone()
        .with_radio_configuration(control.plan().configuration().clone(), radio);
    let update = runtime
        .prepare_update(&control, next, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    update_tick(&mut runtime, false);
    // SAFETY: no concurrent callback can access this fixture-owned record.
    let replaced = unsafe { (*runtime._transmit_context.get()).feedback_record };
    assert_eq!(
        replaced.generation_id, 20,
        "same-device update preserves physical session identity"
    );
    assert_eq!(replaced.programme_retirement_id, 0);
    let mut output = [0.0; 1920];
    assert_ne!(
        // SAFETY: test supplies its owned callback context and bounded output span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                0,
            )
        },
        0
    );
    // SAFETY: test owns TX after the failed synchronous callback returned.
    let failed = unsafe { (*runtime._transmit_context.get()).feedback_record };
    assert_eq!(failed.programme_retirement_id, 0);
    assert_eq!(failed.programme_buffering.flags, 0);
}

#[test]
fn disabled_receive_source_starts_a_new_meter_epoch() {
    STARTS.set(0);
    STOPS.set(0);
    DESTROYS.set(0);
    let (mut runtime, mut control) = runtime(20);
    let original_rx = control.receive_loudness().unwrap().epoch;
    let original_tx = control.transmit_loudness().unwrap().epoch;
    let hardware = runtime.hardware_state();
    hardware.publish_inputs(HardwareInputs {
        carrier: true,
        subaudible: true,
        physical_ptt: true,
        ..HardwareInputs::default()
    });
    hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..ControllerRequests::default()
    });
    runtime.start().unwrap();
    let before = control.radio().radio().snapshot().unwrap();
    let mut configuration = control.plan().configuration().clone();
    configuration.station.receive.audio_source = usbradioplus_core::ReceiveAudioSource::Disabled;
    let mut radio = *control.plan().radio();
    radio.generation_id = 21;
    radio.receive_input_gain = 0.0;
    let plan = control
        .plan()
        .clone()
        .with_radio_configuration(configuration, radio);
    let update = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    assert!(matches!(
        runtime.update_result(),
        Err(crate::StationUpdateError::Pending)
    ));
    // Explicit ticks let the test observe each owner without a timing-dependent sleep.
    update_tick(&mut runtime, true);
    assert_ne!(control.receive_loudness().unwrap().epoch, original_rx);
    assert_eq!(control.transmit_loudness().unwrap().epoch, original_tx);
    assert!(matches!(
        runtime.update_result(),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, false);
    assert_eq!(control.transmit_loudness().unwrap().epoch, original_tx);
    runtime.accept_update(&mut control).unwrap();
    runtime.finish_update(&mut control, true).unwrap();
    let after = control.radio().radio().snapshot().unwrap();
    assert!(after.receive_frames > before.receive_frames);
    assert!(after.transmit_frames > before.transmit_frames);
    assert!(hardware.outputs().logical_ptt);
    assert_eq!((STARTS.get(), STOPS.get(), DESTROYS.get()), (1, 0, 0));
    assert_eq!(control.plan().radio().receive_input_gain, 0.0);
}

#[test]
fn stopped_updates_rollback_without_starting_and_reject_stale_or_foreign_candidates() {
    STARTS.set(0);
    let (mut runtime, mut control) = runtime(20);
    let original = control.plan().clone();
    let make_plan = |gain| {
        let mut config = original.configuration().clone();
        config.local.input_gain_db = gain;
        StationPlan::new("usb", config, original.transport(), 21, 960).unwrap()
    };
    let first = runtime
        .prepare_update(&control, make_plan(1.0), &factory(), radio_provider())
        .unwrap();
    let stale = runtime
        .prepare_update(&control, make_plan(2.0), &factory(), radio_provider())
        .unwrap();
    let foreign = runtime
        .prepare_update(&control, make_plan(3.0), &factory(), radio_provider())
        .unwrap();
    let (mut other, other_control) = self::runtime(22);
    assert!(matches!(
        other.begin_update(&other_control, foreign),
        Err(crate::StationUpdateError::Incompatible)
    ));
    runtime.begin_update(&control, first).unwrap();
    runtime.accept_update(&mut control).unwrap();
    runtime.finish_update(&mut control, true).unwrap();
    assert!(matches!(
        runtime.begin_update(&control, stale),
        Err(crate::StationUpdateError::Busy)
    ));
    let second = runtime
        .prepare_update(&control, make_plan(4.0), &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, second).unwrap();
    runtime.accept_update(&mut control).unwrap();
    runtime.finish_update(&mut control, false).unwrap();
    assert_eq!(control.plan().configuration().local.input_gain_db, 1.0);
    assert_eq!(STARTS.get(), 0);
}

#[test]
fn update_control_ownership_and_in_flight_transaction_are_checked() {
    let (mut runtime, mut control) = runtime(20);
    let (_, mut foreign_control) = self::runtime(22);
    let original = control.plan().clone();
    let plan = StationPlan::new(
        "usb",
        original.configuration().clone(),
        original.transport(),
        21,
        960,
    )
    .unwrap();
    assert!(matches!(
        runtime.snapshot_update(&foreign_control, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Incompatible)
    ));
    let first = runtime
        .prepare_update(&control, plan.clone(), &factory(), radio_provider())
        .unwrap();
    assert_eq!(first.plan(), &plan);
    runtime.finish_update(&mut control, true).unwrap();
    assert!(matches!(
        runtime.begin_update(&foreign_control, first),
        Err(crate::StationUpdateError::Incompatible)
    ));
    let first = runtime
        .prepare_update(&control, plan.clone(), &factory(), radio_provider())
        .unwrap();
    let second = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, first).unwrap();
    assert!(matches!(
        runtime.begin_update(&control, second),
        Err(crate::StationUpdateError::Busy)
    ));
    assert!(matches!(
        runtime.accept_update(&mut foreign_control),
        Err(crate::StationUpdateError::Incompatible)
    ));
    assert!(matches!(
        runtime.finish_update(&mut foreign_control, true),
        Err(crate::StationUpdateError::Incompatible)
    ));
    runtime.finish_update(&mut control, true).unwrap();
}

#[test]
fn failed_callback_update_reports_radio_error_and_rolls_back() {
    for generation in [
        RECEIVE_UPDATE_FAILING_GENERATION,
        TRANSMIT_UPDATE_FAILING_GENERATION,
    ] {
        let (mut runtime, mut control) = runtime(20);
        runtime.start().unwrap();
        let original = control.plan().clone();
        let plan = StationPlan::new(
            "usb",
            original.configuration().clone(),
            original.transport(),
            generation,
            960,
        )
        .unwrap();
        let update = runtime
            .prepare_update(&control, plan, &factory(), radio_provider())
            .unwrap();
        runtime.begin_update(&control, update).unwrap();
        update_tick(&mut runtime, true);
        assert!(matches!(
            runtime.update_result(),
            Err(crate::StationUpdateError::Pending)
        ));
        update_tick(&mut runtime, false);
        assert!(matches!(
            runtime.update_result(),
            Err(crate::StationUpdateError::Radio(
                RadioError::InvalidArgument
            ))
        ));
        assert!(matches!(
            runtime.accept_update(&mut control),
            Err(crate::StationUpdateError::Radio(
                RadioError::InvalidArgument
            ))
        ));
        assert!(matches!(
            runtime.finish_update(&mut control, false),
            Err(crate::StationUpdateError::Pending)
        ));
        update_tick(&mut runtime, true);
        update_tick(&mut runtime, false);
        runtime.finish_update(&mut control, false).unwrap();
        assert_eq!(control.plan(), &original);
    }
}

#[test]
fn delayed_rollback_primes_restored_graph_before_exposing_its_stale_pcm() {
    crate::control::tests::simulate_graph_delay(true);
    let (mut runtime, mut control) = runtime(25);
    runtime.start().unwrap();
    let render = |runtime: &mut StationRuntime| {
        let mut output = [0.0; 1920];
        assert_eq!(
            // SAFETY: the fake provider has no live worker; this is the sole TX owner.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            0
        );
        output
    };
    render(&mut runtime);
    let original = control.plan().clone();
    let mut configuration = original.configuration().clone();
    configuration.voice_telemetry.output_gain_db = -3.0;
    let next = StationPlan::new("usb", configuration, original.transport(), 26, 960).unwrap();
    let update = runtime
        .prepare_update(&control, next, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    update_tick(&mut runtime, true);
    crate::media::tests::program_sample(0.5);
    for _ in 0..4 {
        render(&mut runtime);
    }
    runtime.accept_update(&mut control).unwrap();
    crate::media::tests::program_sample(0.75);
    for _ in 0..4 {
        render(&mut runtime);
    }
    assert!(matches!(
        runtime.finish_update(&mut control, false),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    let first = render(&mut runtime);
    assert_eq!(
        first[0], 0.75,
        "rollback must not expose the original graph's stopped 0.5 PCM"
    );
    runtime.finish_update(&mut control, false).unwrap();
    // Retired transaction ownership must not free the adopted reverse transition.
    for _ in 0..4 {
        assert!(render(&mut runtime).iter().all(|sample| *sample == 0.75));
    }
    assert_eq!(control.plan(), &original);
    crate::media::tests::program_sample(0.25);
    crate::control::tests::simulate_graph_delay(false);
}

fn update_tick(runtime: &mut StationRuntime, receive: bool) {
    if receive {
        let input = [0.25; 1920];
        assert_eq!(
            // SAFETY: the fake provider has no worker thread; this is the sole RX owner.
            unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 960) },
            0
        );
    } else {
        let mut output = [0.0; 1920];
        assert_eq!(
            // SAFETY: the fake provider has no worker thread; this is the sole TX owner.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            0
        );
    }
}

#[test]
fn partial_callback_adoption_stays_owned_through_nonblocking_rollback() {
    let (mut runtime, mut control) = runtime(20);
    runtime.start().unwrap();
    let original = control.plan().clone();
    let mut config = original.configuration().clone();
    config.local.input_gain_db = 2.0;
    let plan = StationPlan::new("usb", config, original.transport(), 21, 960).unwrap();
    let update = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    update_tick(&mut runtime, true);
    assert!(matches!(
        runtime.finish_update(&mut control, false),
        Err(crate::StationUpdateError::Pending)
    ));
    assert!(matches!(
        runtime.prepare_update(&control, original.clone(), &factory(), radio_provider()),
        Err(crate::StationUpdateError::Busy)
    ));
    update_tick(&mut runtime, false);
    assert!(matches!(
        runtime.finish_update(&mut control, false),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    update_tick(&mut runtime, false);
    assert!(matches!(
        runtime.accept_update(&mut control),
        Err(crate::StationUpdateError::Busy)
    ));
    assert!(matches!(
        runtime.finish_update(&mut control, true),
        Err(crate::StationUpdateError::Busy)
    ));
    runtime.finish_update(&mut control, false).unwrap();
    assert_eq!(control.plan(), &original);
}

#[test]
fn failed_update_preparation_and_incompatible_stream_leave_active_plan_untouched() {
    let (runtime, control) = runtime(20);
    let original = control.plan().clone();
    let invalid = StationPlan::new(
        "usb",
        original.configuration().clone(),
        original.transport(),
        99,
        960,
    )
    .unwrap();
    assert!(matches!(
        runtime.prepare_update(&control, invalid, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Radio(_))
    ));
    let incompatible = StationPlan::new(
        "usb",
        original.configuration().clone(),
        original.transport(),
        21,
        480,
    )
    .unwrap();
    assert!(matches!(
        runtime.prepare_update(&control, incompatible, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Incompatible)
    ));
    let mut different_hardware = original.configuration().clone();
    different_hardware.station.hardware.input_extra_buffer_ms += 1;
    let incompatible =
        StationPlan::new("usb", different_hardware, original.transport(), 21, 960).unwrap();
    assert!(matches!(
        runtime.prepare_update(&control, incompatible, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Incompatible)
    ));
    let incompatible = StationPlan::new(
        "usb",
        original.configuration().clone(),
        crate::ControllerTransport::AppRpt,
        21,
        960,
    )
    .unwrap();
    assert!(matches!(
        runtime.prepare_update(&control, incompatible, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Incompatible)
    ));
    let document = usbradioplus_core::ConfigDocument::new(
        "[remote_base usb]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\n[remote_memory usb local]\nselector=1\nrx_frequency_hz=146520000\nmode=FM\n",
    );
    let definition = usbradioplus_core::ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "usb",
        &original.configuration().station,
    )
    .unwrap()
    .unwrap();
    let incompatible = original.clone().with_remote_configuration(Some(definition));
    assert!(matches!(
        runtime.prepare_update(&control, incompatible, &factory(), radio_provider()),
        Err(crate::StationUpdateError::Incompatible)
    ));
    assert_eq!(control.plan(), &original);
}

#[test]
fn shared_hardware_state_round_trips_complete_atomic_snapshots() {
    let state = SharedHardwareState::default();
    let inputs = HardwareInputs {
        carrier: true,
        parallel_carrier: true,
        subaudible: true,
        parallel_subaudible: true,
        physical_ptt: true,
        cm119_gpio_mask: 0xa5,
        parallel_input_mask: 0x30,
    };
    let requests = ControllerRequests {
        transmit: true,
        render_admitted: true,
        ctcss_inhibit: true,
        calibrated_test_tone: true,
        subaudible_override: true,
        forced_ctcss: CtcssTone::from_tenths_hz(1_000),
    };
    state.publish_inputs(inputs);
    state.publish_requests(requests);
    assert_eq!(state.inputs(), inputs);
    assert_eq!(state.requests(), requests);
    assert_eq!(state.receive_controls().hardware_carrier, inputs.carrier);
    assert!(state.receive_controls().subaudible_override);
    assert_eq!(
        state.transmit_controls().external_ptt_request,
        requests.transmit
    );
    assert_eq!(state.transmit_controls().forced_ctcss_tenths_hz, 1_000);
    assert_eq!(state.outputs(), HardwareOutputs::default());
    assert_eq!(state.callback_statistics(), CallbackStatistics::default());
    assert_eq!(state.receive_clip_events(), 0);
    state.publish_receive_clipping(0);
    state.publish_receive_clipping(2);
    assert_eq!(state.receive_clip_events(), 1);
    let observation_result = ReceiveResult {
        raw_receiver_keyed: true,
        rf_burst_id: 0,
        vad_mode: usbradioplus_radio::VadMode::Off,
        vad_voice_active: false,
        vad_score: 0.0,
        vad_voice_sample_index: u64::MAX,
        vad_preroll_frames: 0,

        generation_id: 1,
        first_sample_index: 0,
        frame_count: 1,
        carrier_active: true,
        subaudible_active: true,
        receiver_keyed: true,
        ctcss_decoded: None,
        dcs_valid: false,
        rssi_peak: -123,
        rssi_updated: true,
        ctcss_decoder_peak: 2_400.0 / 32_768.0,
        input_peak: 0.75,
        input_rms: 0.25,
        input_rail_samples: 2,
        output_peak: 0.5,
        output_rms: 0.125,
        output_rail_samples: 1,
        periodic_status_due: false,
    };
    state.publish_receive_observation(observation_result);
    assert_eq!(
        state.receive_observation(),
        ReceiveObservation {
            input_peak: 0.75,
            input_rms: 0.25,
            input_rail_samples: 2,
            ctcss_decoder_peak: 2_400.0 / 32_768.0,
            output_peak: 0.5,
            output_rms: 0.125,
            output_rail_samples: 1,
            rssi_peak: -123,
            rssi_updated: true,
        }
    );

    state.publish_transmit_result(transmit_result(true, 1_000));
    assert_eq!(
        state.outputs(),
        HardwareOutputs {
            receiver_keyed: false,
            logical_ptt: true,
            selected_ctcss_tenths_hz: Some(1_000),
        }
    );
    let tone = state.take_transmit_ctcss().unwrap();
    assert_eq!(tone.tenths_hz(), 1_000);
    state.restore_transmit_ctcss(tone);
    assert_eq!(state.take_transmit_ctcss(), Some(tone));
    assert_eq!(state.take_transmit_ctcss(), None);

    state.publish_transmit_result(transmit_result(false, -1));
    assert_eq!(state.outputs(), HardwareOutputs::default());
}

#[test]
fn remote_radio_admission_keeps_requested_edges_separate_from_rendered_tails() {
    let state = SharedHardwareState::default();
    assert!(!state.remote_control().enabled);
    assert_eq!(state.remote_activity().transmit_epoch, 0);
    for (requested, epoch) in [(true, 1), (true, 1), (false, 1), (true, 2)] {
        state.publish_transmit_intent(requested, false, false);
        let activity = state.remote_activity();
        assert_eq!(activity.transmit_requested, requested);
        assert_eq!(activity.transmit_epoch, epoch);
    }
    state.publish_transmit_result(transmit_result(false, 0));
    assert!(state.remote_activity().transmit_requested);
    let control = RemoteRadioControl {
        enabled: true,
        receive_permitted: false,
        transmit_permitted: false,
        non_fm: true,
        local_unqualified_open: true,
    };
    state.publish_remote_control(control);
    assert_eq!(state.remote_control(), control);
    state.publish_receiver_keyed(true);
    assert!(state.remote_activity().local_receiving);
}

#[test]
fn remote_tune_intent_does_not_overwrite_controller_or_tuner_requests() {
    let state = SharedHardwareState::default();
    state.publish_remote_tune(true);
    assert!(state.transmit_controls().external_ptt_request);
    assert!(state.transmit_controls().calibrated_test_tone);
    assert!(state.transmit_controls().render_admitted);
    assert_eq!(state.requests(), ControllerRequests::default());
    let requests = ControllerRequests {
        transmit: true,
        ctcss_inhibit: true,
        calibrated_test_tone: true,
        ..ControllerRequests::default()
    };
    state.publish_requests(requests);
    state.publish_remote_tune(false);
    assert_eq!(state.requests(), requests);
    assert!(state.transmit_controls().external_ptt_request);
    assert!(state.transmit_controls().calibrated_test_tone);
    assert!(state.transmit_controls().ctcss_inhibit);
    state.publish_requests(ControllerRequests::default());
    assert!(!state.transmit_controls().external_ptt_request);
    assert!(!state.transmit_controls().calibrated_test_tone);
}

#[test]
fn remote_radio_callback_inhibition_mutes_routes_without_hiding_raw_activity() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 24);
    let selected = selected(&prepared, 960);
    let (plan, processing) = prepared.into_parts();
    let mut capture = DirectCapture {
        tx_keyed: 1,
        ctcss_enabled: 1,
        metadata_source: 1,
        ..DirectCapture::default()
    };
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: Some(direct_metadata),
        accepted_abi_version: 0,
    };
    // SAFETY: capture outlives these serial callbacks and synchronous teardown.
    let media = unsafe {
        crate::NativeStationMedia::prepare(*plan.radio(), processing, radio_provider(), callbacks)
    }
    .unwrap();
    let (runtime, _control) =
        StationRuntime::open_native(media, &selected, audio_provider()).unwrap();
    runtime.hardware.publish_remote_control(RemoteRadioControl {
        enabled: true,
        receive_permitted: false,
        transmit_permitted: false,
        non_fm: true,
        local_unqualified_open: false,
    });
    runtime.hardware.publish_inputs(HardwareInputs {
        carrier: true,
        subaudible: true,
        ..HardwareInputs::default()
    });
    let input = [0.5_f32; 8];
    assert_eq!(
        // SAFETY: the stopped runtime owns the context and exact stereo input span.
        unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 4) },
        CALLBACK_OK
    );
    assert_eq!(capture.keyed, 0);
    assert_eq!(capture.samples, [0.0; 4]);
    assert!(runtime.hardware.remote_activity().local_receiving);
    let mut output = [0.5_f32; 8];
    assert_eq!(
        // SAFETY: the stopped runtime owns the context and exact stereo output span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        CALLBACK_OK
    );
    assert_eq!(output, [0.0; 8]);
    assert!(!runtime.hardware.outputs().logical_ptt);
    assert!(runtime.hardware.remote_activity().transmit_requested);
    assert_eq!(runtime.hardware.remote_activity().transmit_epoch, 1);
    assert_eq!(capture.metadata_flags, 9);
    assert!(runtime.hardware.remote_activity().linked_receiving);
    assert!(runtime.hardware.remote_activity().ctcss_requested);
    runtime.hardware.publish_remote_control(RemoteRadioControl {
        enabled: true,
        receive_permitted: true,
        transmit_permitted: true,
        ..Default::default()
    });
    assert_eq!(
        // SAFETY: the stopped runtime owns this RX context and complete input span.
        unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 4) },
        CALLBACK_OK
    );
    assert_eq!(capture.keyed, 1);
    assert_eq!(capture.samples, [0.5; 4]);
    capture.tx_keyed = 0;
    assert_eq!(
        // SAFETY: the stopped runtime owns this TX context and complete output span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        CALLBACK_OK
    );
    assert_eq!(output, [0.0; 8], "a new permitted key must admit the burst");
    assert!(!runtime.hardware.outputs().logical_ptt);
    capture.tx_keyed = 1;
    // SAFETY: the same stopped runtime owns this callback and complete output span.
    let permitted = unsafe {
        transmit_callback(
            runtime._transmit_context.get().cast(),
            output.as_mut_ptr(),
            4,
        )
    };
    assert_eq!(permitted, CALLBACK_OK);
    assert_eq!(output, [-0.375; 8]);
    let calls = capture.transmit_calls;
    capture.metadata_source = 2;
    output.fill(1.0);
    assert_eq!(
        // SAFETY: the stopped runtime retains this TX context and writable stereo span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        CALLBACK_FAILED,
    );
    assert_eq!(
        capture.transmit_calls, calls,
        "invalid metadata never enters audio rendering"
    );
    assert_eq!(output, [0.0; 8]);
    assert!(!runtime.hardware.remote_activity().ctcss_requested);
    assert!(!runtime.hardware.remote_activity().linked_receiving);
}

#[test]
fn receive_observation_waits_for_an_in_progress_publication() {
    use std::thread;
    use std::time::Duration;

    let state = SharedHardwareState::default();
    state
        .0
        .receive_observation_sequence
        .store(1, Ordering::Release);
    let publisher = state.clone();
    let handle = thread::spawn(move || {
        thread::sleep(Duration::from_millis(1));
        publisher
            .0
            .receive_observation_sequence
            .store(2, Ordering::Release);
    });
    assert_eq!(state.receive_observation(), ReceiveObservation::default());
    handle.join().unwrap();

    assert_eq!(
        stable_observation(0, 2, ReceiveObservation::default()),
        None
    );
}

#[test]
fn runtime_runs_separate_callbacks_and_exposes_lifecycle_and_observation() {
    fn require_send<T: Send>() {}
    require_send::<StationControlHost>();

    STARTS.set(0);
    STOPS.set(0);
    DESTROYS.set(0);
    let (mut runtime, mut control) = runtime(20);
    assert!(!runtime.running());
    let hardware = runtime.hardware_state();
    assert_eq!(control.hardware_state().inputs(), HardwareInputs::default());
    hardware.publish_inputs(HardwareInputs {
        carrier: true,
        subaudible: true,
        physical_ptt: true,
        ..HardwareInputs::default()
    });
    hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..ControllerRequests::default()
    });

    runtime.start().unwrap();
    runtime.start().unwrap();
    assert!(runtime.running());
    assert_eq!(STARTS.get(), 1);
    assert_eq!(f32::from_bits(OUTPUT_SAMPLE.get()), 0.25);
    assert_eq!(hardware.receive_observation().input_peak, 0.5);
    assert_eq!(hardware.receive_observation().rssi_peak, 16_384);
    assert!(hardware.receive_observation().rssi_updated);
    assert_eq!(
        hardware.outputs(),
        HardwareOutputs {
            receiver_keyed: true,
            logical_ptt: true,
            selected_ctcss_tenths_hz: Some(1_000),
        }
    );
    assert_eq!(
        hardware.callback_statistics(),
        CallbackStatistics {
            receive_calls: 1,
            transmit_calls: 1,
            receive_failures: 0,
            transmit_failures: 0,
        }
    );
    assert_eq!(control.plan().channel(), "usb");
    assert_eq!(
        control.radio().radio().snapshot().unwrap().receive_frames,
        960
    );
    let mut voice = ControllerPcmFrame::silence(AsteriskPcmMode::Advanced);
    assert!(matches!(
        control.controller().next_action(&mut voice),
        Some(DeliveryAction::ReceiverKey { .. })
    ));
    let statistics = runtime.statistics().unwrap();
    assert_eq!(statistics.audio.callback_count, 2);
    assert_eq!(statistics.callbacks, hardware.callback_statistics());
    assert_eq!(runtime.timing().unwrap().output_latency_seconds, 0.02);
    runtime.stop().unwrap();
    runtime.stop().unwrap();
    assert!(!runtime.running());
    assert_eq!(STOPS.get(), 1);
    runtime.suspend().unwrap();
    runtime.suspend().unwrap();
    assert_eq!(DESTROYS.get(), 1);
    assert_eq!(runtime.statistics(), Err(AudioError::InvalidArgument));
    assert_eq!(runtime.timing(), Err(AudioError::InvalidArgument));
    CREATE_FAILS.set(true);
    assert_eq!(runtime.reopen(), Err(AudioError::InvalidArgument));
    CREATE_FAILS.set(false);
    runtime.reopen().unwrap();
    runtime.reopen().unwrap();
    assert_eq!(STARTS.get(), 1, "reopening leaves callbacks stopped");
    runtime.start().unwrap();
    assert_eq!(STARTS.get(), 2);
    assert_eq!(hardware.callback_statistics().receive_calls, 2);
    assert_eq!(hardware.callback_statistics().transmit_calls, 2);
    assert!(runtime.statistics().is_ok());
    assert!(runtime.timing().is_ok());
    drop(runtime);
    assert_eq!(STOPS.get(), 2);
    assert_eq!(DESTROYS.get(), 2);
}

#[derive(Default)]
struct DirectCapture {
    samples: [f32; 4],
    keyed: u32,
    ctcss_enabled: u32,
    rx_status: c_int,
    tx_keyed: u32,
    tx_status: c_int,
    receive_calls: u32,
    transmit_calls: u32,
    metadata_flags: u32,
    metadata_source: u32,
    metadata_calls: u32,
    fail_metadata_after: u32,
    receive_metadata: Option<crate::DirectReceiveMetadata>,
}

unsafe extern "C" fn direct_metadata(context: *mut c_void, flags: u32, source: *mut u32) -> c_int {
    // SAFETY: the test retains this exclusive TX context and writable output.
    unsafe {
        let capture = &mut *context.cast::<DirectCapture>();
        capture.metadata_flags = flags;
        capture.metadata_calls += 1;
        *source = capture.metadata_source;
        if capture.fail_metadata_after != 0 && capture.metadata_calls > capture.fail_metadata_after
        {
            return -1;
        }
    }
    0
}

unsafe extern "C" fn direct_receive(
    context: *mut c_void,
    metadata: *const crate::DirectReceiveMetadata,
    samples: *mut f32,
    count: u32,
) -> c_int {
    // SAFETY: the test owns the context and exact mono span for this call.
    let (capture, samples, metadata) = unsafe {
        (
            &mut *context.cast::<DirectCapture>(),
            std::slice::from_raw_parts_mut(samples, count as usize),
            &*metadata,
        )
    };
    capture.samples.copy_from_slice(&samples[..4]);
    capture.keyed = metadata.receiver_keyed;
    capture.receive_metadata = Some(*metadata);
    capture.receive_calls += 1;
    samples.fill(0.0);
    capture.rx_status
}

unsafe extern "C" fn direct_transmit(
    context: *mut c_void,
    samples: *mut f32,
    count: u32,
    keyed: *mut u32,
    ctcss_enabled: *mut u32,
) -> c_int {
    // SAFETY: the test owns the context, output controls and exact mono span.
    let capture = unsafe { &mut *context.cast::<DirectCapture>() };
    // SAFETY: the callback contract supplies count writable samples and a key result.
    unsafe {
        std::slice::from_raw_parts_mut(samples, count as usize).fill(-0.375);
        *keyed = capture.tx_keyed;
        *ctcss_enabled = capture.ctcss_enabled;
    }
    capture.transmit_calls += 1;
    capture.tx_status
}

#[test]
fn direct_transmit_failure_or_invalid_key_immediately_silences_and_unkeys() {
    for (status, keyed, ctcss_enabled) in [(-1, 1, 1), (0, 2, 1), (0, 1, 2)] {
        let mut capture = DirectCapture {
            tx_keyed: 1,
            ctcss_enabled: 1,
            ..DirectCapture::default()
        };
        let (mut media, selected) =
            media_with_maximum(crate::ControllerTransport::RptAdvanced, 24, 960);
        let callbacks = crate::DirectCallbacks {
            struct_size: size_of::<crate::DirectCallbacks>() as u32,
            abi_version: crate::DirectCallbacks::ABI_VERSION,
            receive_context: ptr::from_mut(&mut capture).cast(),
            receive: Some(direct_receive),
            transmit_context: ptr::from_mut(&mut capture).cast(),
            transmit: Some(direct_transmit),
            metadata: None,
            accepted_abi_version: 0,
        };
        // SAFETY: the context outlives the runtime and calls below are serial.
        unsafe { media.set_direct_callbacks(callbacks) }.unwrap();
        let (runtime, mut control) =
            StationRuntime::open(media, &selected, audio_provider()).unwrap();
        runtime.hardware.publish_requests(ControllerRequests {
            transmit: true,
            calibrated_test_tone: true,
            ..ControllerRequests::default()
        });
        let mut output = [0.0; 8];
        // SAFETY: the stopped stream owns this context and exact stereo span.
        let success = unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        };
        assert_eq!(success, CALLBACK_OK);
        assert!(runtime.hardware.outputs().logical_ptt);
        assert_eq!(output, [-0.375; 8]);
        capture.tx_status = status;
        capture.tx_keyed = keyed;
        capture.ctcss_enabled = ctcss_enabled;
        assert_eq!(
            (capture.tx_status, capture.tx_keyed, capture.ctcss_enabled),
            (status, keyed, ctcss_enabled)
        );
        // SAFETY: the stopped stream owns this context and exact stereo span.
        let failure = unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        };
        assert_eq!(failure, CALLBACK_FAILED);
        assert_eq!(output, [0.0; 8]);
        assert!(!runtime.hardware.outputs().logical_ptt);
        assert_eq!(runtime.hardware.outputs().selected_ctcss_tenths_hz, None);
        assert_eq!(runtime.hardware.callback_statistics().transmit_failures, 1);
        assert_eq!(
            control.radio().radio().snapshot().unwrap().transmit_frames,
            8
        );
    }
}

#[test]
fn direct_callbacks_bypass_asterisk_and_stage_audio_and_key_in_the_same_render() {
    let mut capture = DirectCapture {
        tx_keyed: 1,
        ctcss_enabled: 1,
        ..DirectCapture::default()
    };
    let (mut media, selected) =
        media_with_maximum(crate::ControllerTransport::RptAdvanced, 24, 960);
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: None,
        accepted_abi_version: 0,
    };
    let mut legacy = prepared(crate::ControllerTransport::AppRpt, 25)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(FailingConverter),
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .unwrap();
    assert!(matches!(
        // SAFETY: capture outlives these stopped owners; rejection cannot invoke callbacks.
        unsafe { legacy.set_direct_callbacks(callbacks) },
        Err(crate::StationMediaError::ControllerTransportMismatch)
    ));
    drop(legacy);
    let mut invalid = callbacks;
    invalid.abi_version = 0;
    assert!(matches!(
        // SAFETY: capture outlives these stopped owners; the descriptor is rejected.
        unsafe { media.set_direct_callbacks(invalid) },
        Err(crate::StationMediaError::ControllerTransportMismatch)
    ));
    // SAFETY: capture outlives the stopped runtime and calls below are serial.
    unsafe { media.set_direct_callbacks(callbacks) }.unwrap();
    assert!(matches!(
        // SAFETY: the same live contexts remain owned here; attachment is already complete.
        unsafe { media.set_direct_callbacks(callbacks) },
        Err(crate::StationMediaError::ControllerTransportMismatch)
    ));
    let (mut runtime, mut control) =
        StationRuntime::open(media, &selected, audio_provider()).unwrap();
    runtime.hardware.publish_inputs(HardwareInputs {
        carrier: true,
        subaudible: true,
        ..HardwareInputs::default()
    });
    let input = [0.1, 0.9, 0.2, 0.8, 0.3, 0.7, 0.4, 0.6];
    assert_eq!(
        // SAFETY: stopped runtime uniquely owns these contexts and spans are exact.
        unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 4) },
        0
    );
    assert_eq!(capture.samples, [0.1, 0.2, 0.3, 0.4]);
    assert_eq!(capture.keyed, 1);
    assert_eq!(capture.receive_calls, 1);
    assert!(runtime.hardware.outputs().receiver_keyed);
    capture.rx_status = -7;
    assert_eq!(capture.rx_status, -7);
    assert_eq!(
        // SAFETY: the stopped runtime retains this live context and exact stereo span.
        unsafe { receive_callback(runtime._receive_context.get().cast(), input.as_ptr(), 4) },
        CALLBACK_FAILED
    );
    assert_eq!(capture.receive_calls, 2);
    assert_eq!(runtime.hardware.callback_statistics().receive_calls, 2);
    assert_eq!(runtime.hardware.callback_statistics().receive_failures, 1);
    capture.rx_status = 0;
    assert_eq!(capture.rx_status, 0);
    let invalid = [f32::NAN; 8];
    assert_eq!(
        // SAFETY: this stopped runtime owns the context and the exact stereo span.
        unsafe { receive_callback(runtime._receive_context.get().cast(), invalid.as_ptr(), 4) },
        CALLBACK_FAILED
    );
    assert_eq!(
        capture.receive_calls, 3,
        "source failure must cancel the previous direct admission"
    );
    let cancelled = capture.receive_metadata.unwrap();
    assert!(cancelled.status < 0);
    assert_eq!(
        (cancelled.raw_receiver_keyed, cancelled.receiver_keyed),
        (0, 0)
    );
    assert_eq!(capture.samples, [0.0; 4]);
    let mut voice = ControllerPcmFrame::silence(AsteriskPcmMode::Advanced);
    assert!(control.controller().next_action(&mut voice).is_none());
    let mut output = [0.0; 8];
    assert_eq!(
        // SAFETY: stopped runtime uniquely owns these contexts and spans are exact.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        0
    );
    assert_eq!(capture.transmit_calls, 1);
    assert!(runtime.hardware.outputs().logical_ptt);
    // The radio fixture exercises the actual bound program port for generation 24.
    assert_eq!(output, [-0.375; 8]);
    capture.ctcss_enabled = 0;
    assert_eq!(
        // SAFETY: the same stopped contexts and exact writable span remain valid.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        0
    );
    assert_eq!(output, [0.5; 8]);
    assert!(runtime.hardware.outputs().logical_ptt);
    capture.ctcss_enabled = 1;
    capture.tx_keyed = 0;
    assert_eq!(capture.tx_keyed, 0);
    runtime.hardware.publish_requests(ControllerRequests {
        transmit: true,
        ..ControllerRequests::default()
    });
    assert_eq!(
        // SAFETY: same stopped contexts and exact writable span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        0
    );
    assert!(runtime.hardware.outputs().logical_ptt);
    runtime
        .hardware
        .publish_requests(ControllerRequests::default());
    assert_eq!(
        // SAFETY: same stopped contexts and exact writable span.
        unsafe {
            transmit_callback(
                runtime._transmit_context.get().cast(),
                output.as_mut_ptr(),
                4,
            )
        },
        0
    );
    assert!(!runtime.hardware.outputs().logical_ptt);
    STOPS.set(0);
    DESTROYS.set(0);
    runtime.start().unwrap();
    runtime.stop().unwrap();
    assert_eq!(STOPS.get(), 1);
    drop(runtime);
    assert_eq!(DESTROYS.get(), 1);
    // Context remains owned here until both callback owners have been destroyed.
    assert_eq!(capture.receive_calls, 4);
    assert_eq!(capture.transmit_calls, 5);
}

#[test]
fn callbacks_process_the_exact_independent_frame_counts() {
    let (runtime, mut control) = runtime(14);
    // SAFETY: this test invokes each stopped stream context serially.
    let receive_context = unsafe { NonNull::new_unchecked(runtime._receive_context.get()) }.cast();
    let transmit_context = {
        // SAFETY: this test invokes each stopped stream context serially.
        unsafe { NonNull::new_unchecked(runtime._transmit_context.get()) }.cast()
    };
    let input = [0.25_f32; 14];
    let mut output = [f32::NAN; 26];
    assert_eq!(
        // SAFETY: the input contains exactly seven canonical stereo frames.
        unsafe { receive_callback(receive_context.as_ptr(), input.as_ptr(), 7) },
        CALLBACK_OK
    );
    assert_eq!(
        // SAFETY: the output contains exactly thirteen canonical stereo frames.
        unsafe { transmit_callback(transmit_context.as_ptr(), output.as_mut_ptr(), 13) },
        CALLBACK_OK
    );
    assert!(output.iter().all(|sample| *sample == 0.25));
    let snapshot = control.radio().radio().snapshot().unwrap();
    assert_eq!(snapshot.receive_frames, 7);
    assert_eq!(snapshot.transmit_frames, 13);
}

#[test]
fn callbacks_fail_safely_for_invalid_spans_and_radio_failures() {
    assert_eq!(
        // SAFETY: null is an explicitly tested rejected FFI argument.
        unsafe { receive_callback(ptr::null_mut(), ptr::null(), 0) },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: null is an explicitly tested rejected FFI argument.
        unsafe { transmit_callback(ptr::null_mut(), ptr::null_mut(), 0) },
        CALLBACK_FAILED
    );

    let (mut receive_failure, _) = runtime(RECEIVE_FAILING_GENERATION);
    assert_eq!(receive_failure.start(), Err(AudioError::AdapterFailure));
    assert_eq!(
        receive_failure
            .hardware_state()
            .callback_statistics()
            .receive_failures,
        1
    );

    let (mut transmit_failure, _) = runtime(TRANSMIT_FAILING_GENERATION);
    assert_eq!(transmit_failure.start(), Err(AudioError::AdapterFailure));
    assert_eq!(
        transmit_failure
            .hardware_state()
            .callback_statistics()
            .transmit_failures,
        1
    );

    let (valid, _) = runtime(8);
    // SAFETY: this test serially invokes the stopped runtime's owned contexts.
    let receive_context = unsafe { NonNull::new_unchecked(valid._receive_context.get()) }.cast();
    // SAFETY: this test serially invokes the stopped runtime's owned contexts.
    let transmit_context = unsafe { NonNull::new_unchecked(valid._transmit_context.get()) }.cast();
    let input = [0.0_f32; 2];
    let mut output = [1.0_f32; 2];
    assert_eq!(
        // SAFETY: the context is live; null input is intentionally rejected.
        unsafe { receive_callback(receive_context.as_ptr(), ptr::null(), 1) },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: the context/input are live; zero frames are intentionally rejected.
        unsafe { receive_callback(receive_context.as_ptr(), input.as_ptr(), 0) },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: validation rejects the oversized span before reading input.
        unsafe {
            receive_callback(
                receive_context.as_ptr(),
                input.as_ptr(),
                ADVANCED_FRAME_SAMPLES as u32 + 1,
            )
        },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: the context is live; null output is intentionally rejected.
        unsafe { transmit_callback(transmit_context.as_ptr(), ptr::null_mut(), 1) },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: the context/output are live; zero frames are intentionally rejected.
        unsafe { transmit_callback(transmit_context.as_ptr(), output.as_mut_ptr(), 0) },
        CALLBACK_FAILED
    );
    assert_eq!(
        // SAFETY: validation rejects the oversized span before writing output.
        unsafe {
            transmit_callback(
                transmit_context.as_ptr(),
                output.as_mut_ptr(),
                ADVANCED_FRAME_SAMPLES as u32 + 1,
            )
        },
        CALLBACK_FAILED
    );
}

#[test]
fn metadata_maps_qualification_ctcss_voter_and_transmit_status() {
    let state = SharedHardwareState::default();
    state.publish_transmit_result(transmit_result(true, 1_000));
    let mut tones = [None; CTCSS_TONE_COUNT];
    tones[0] = CtcssTone::from_tenths_hz(670);
    let metadata = receive_metadata(
        ReceiveResult {
            raw_receiver_keyed: true,
            rf_burst_id: 0,
            vad_mode: usbradioplus_radio::VadMode::Off,
            vad_voice_active: false,
            vad_score: 0.0,
            vad_voice_sample_index: u64::MAX,
            vad_preroll_frames: 0,

            generation_id: 1,
            first_sample_index: 0,
            frame_count: 1,
            carrier_active: true,
            subaudible_active: true,
            receiver_keyed: true,
            ctcss_decoded: usbradioplus_radio::CtcssToneIndex::new(0),
            dcs_valid: true,
            rssi_peak: 16_384,
            rssi_updated: true,
            ctcss_decoder_peak: 0.0,
            input_peak: 0.0,
            input_rms: 0.0,
            input_rail_samples: 0,
            output_peak: 0.0,
            output_rms: 0.0,
            output_rail_samples: 0,
            periodic_status_due: false,
        },
        &tones,
        true,
        &state,
    );
    assert_eq!(metadata.controller_ctcss.unwrap().tenths_hz(), 670);
    assert!(metadata.qualification.receiver_keyed);
    assert!(metadata.qualification.dcs_valid);
    assert!(matches!(
        metadata.statuses[0],
        Some(ReceiveStatus::TransmitCtcssReady(_))
    ));
    assert_eq!(metadata.statuses[1], Some(ReceiveStatus::VoterRssi(499)));
    assert_eq!(voter_rssi(i16::MIN), 1_000);
    assert_eq!(voter_rssi(i16::MAX), 0);
}

#[test]
fn open_rejects_stream_bounds_that_station_or_asl3_cannot_accept() {
    let cases = [
        (960, 0, 960),
        (960, 960, 0),
        (960, ADVANCED_FRAME_SAMPLES as u32 + 1, 960),
        (480, 960, 480),
        (480, 480, 960),
    ];
    for (station_maximum, receive_maximum, transmit_maximum) in cases {
        let (media, mut selected) = media_with_maximum(
            crate::ControllerTransport::RptAdvanced,
            u64::from(station_maximum),
            station_maximum,
        );
        selected.stream.maximum_receive_frame_count = receive_maximum;
        selected.stream.maximum_transmit_frame_count = transmit_maximum;
        assert_eq!(
            StationRuntime::open(media, &selected, audio_provider()).err(),
            Some(AudioError::InvalidArgument)
        );
    }
}

#[test]
fn runtime_propagates_audio_open_and_statistics_failures() {
    let prepared = prepared(crate::ControllerTransport::RptAdvanced, 12);
    let selected = selected(&prepared, ADVANCED_FRAME_SAMPLES as u32);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 2 },
        )
        .unwrap();
    CREATE_FAILS.set(true);
    assert_eq!(
        StationRuntime::open(media, &selected, audio_provider()).err(),
        Some(AudioError::InvalidArgument)
    );
    CREATE_FAILS.set(false);

    let (runtime, _) = runtime(13);
    STATISTICS_FAILS.set(true);
    assert_eq!(runtime.statistics(), Err(AudioError::InvalidArgument));
    STATISTICS_FAILS.set(false);
}

struct FailingConverter;

impl usbradioplus_asl3::AppRptConverter for FailingConverter {
    fn process(
        &mut self,
        _input: &[f32],
        _output: &mut [f32],
    ) -> Result<ConversionProgress, ConversionError> {
        Err(ConversionError)
    }
}

#[test]
fn receive_callback_restores_ctcss_status_when_controller_publication_fails() {
    let prepared = prepared(crate::ControllerTransport::AppRpt, 10);
    let selected = selected(&prepared, ADVANCED_FRAME_SAMPLES as u32);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(FailingConverter),
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .unwrap();
    let (mut runtime, _) = StationRuntime::open(media, &selected, audio_provider()).unwrap();
    runtime
        .hardware_state()
        .publish_requests(ControllerRequests {
            transmit: true,
            ..ControllerRequests::default()
        });
    assert_eq!(runtime.start(), Err(AudioError::AdapterFailure));
    assert_eq!(
        runtime.hardware_state().take_transmit_ctcss(),
        CtcssTone::from_tenths_hz(1_000)
    );
}

#[test]
fn receive_failure_without_pending_transmit_ctcss_needs_no_restore() {
    let prepared = prepared(crate::ControllerTransport::AppRpt, 11);
    let selected = selected(&prepared, ADVANCED_FRAME_SAMPLES as u32);
    let media = prepared
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(FailingConverter),
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .unwrap();
    let (mut runtime, _) = StationRuntime::open(media, &selected, audio_provider()).unwrap();
    assert_eq!(runtime.start(), Err(AudioError::AdapterFailure));
    assert_eq!(runtime.hardware_state().take_transmit_ctcss(), None);
}

#[test]
fn metadata_omits_voter_status_when_it_is_not_reportable() {
    let result = |receiver_keyed, rssi_updated| ReceiveResult {
        raw_receiver_keyed: receiver_keyed,
        rf_burst_id: 0,
        vad_mode: usbradioplus_radio::VadMode::Off,
        vad_voice_active: false,
        vad_score: 0.0,
        vad_voice_sample_index: u64::MAX,
        vad_preroll_frames: 0,

        generation_id: 1,
        first_sample_index: 0,
        frame_count: 1,
        carrier_active: true,
        subaudible_active: true,
        receiver_keyed,
        ctcss_decoded: None,
        dcs_valid: false,
        rssi_peak: 0,
        rssi_updated,
        ctcss_decoder_peak: 0.0,
        input_peak: 0.0,
        input_rms: 0.0,
        input_rail_samples: 0,
        output_peak: 0.0,
        output_rms: 0.0,
        output_rail_samples: 0,
        periodic_status_due: false,
    };
    for (voter_reporting, receiver_keyed, rssi_updated) in [
        (false, true, true),
        (true, false, true),
        (true, true, false),
    ] {
        let metadata = receive_metadata(
            result(receiver_keyed, rssi_updated),
            &[None; CTCSS_TONE_COUNT],
            voter_reporting,
            &SharedHardwareState::default(),
        );
        assert_eq!(metadata.statuses, [None, None]);
    }
}

#[test]
fn direct_remote_metadata_failure_after_render_silences_the_current_frame() {
    let (mut media, selected) =
        media_with_maximum(crate::ControllerTransport::RptAdvanced, 24, 960);
    let mut capture = DirectCapture {
        tx_keyed: 1,
        ctcss_enabled: 1,
        fail_metadata_after: 1,
        ..Default::default()
    };
    let callbacks = crate::DirectCallbacks {
        struct_size: size_of::<crate::DirectCallbacks>() as u32,
        abi_version: crate::DirectCallbacks::ABI_VERSION,
        receive_context: ptr::from_mut(&mut capture).cast(),
        receive: Some(direct_receive),
        transmit_context: ptr::from_mut(&mut capture).cast(),
        transmit: Some(direct_transmit),
        metadata: Some(direct_metadata),
        accepted_abi_version: 0,
    };
    // SAFETY: this stopped fixture retains the exclusive callback state until teardown.
    unsafe { media.set_direct_callbacks(callbacks) }.unwrap();
    let (runtime, _control) = StationRuntime::open(media, &selected, audio_provider()).unwrap();
    let mut output = [1.0; 8];
    // SAFETY: this test is the sole callback owner and lends a complete stereo span.
    let result = unsafe {
        transmit_callback(
            runtime._transmit_context.get().cast(),
            output.as_mut_ptr(),
            4,
        )
    };
    assert_eq!(result, CALLBACK_FAILED);
    assert_eq!(capture.transmit_calls, 1);
    assert_eq!(capture.metadata_calls, 2);
    assert_eq!(output, [0.0; 8]);
    assert!(!runtime.hardware.outputs().logical_ptt);
}

#[test]
fn programme_fence_survives_same_device_adoption_and_waits_for_rendered_retirement() {
    struct Permit {
        answer: u32,
        calls: usize,
    }
    unsafe extern "C" fn permit(context: *mut c_void, generation: u64, frames: u32) -> u32 {
        // SAFETY: the test retains this probe through synchronous runtime destruction.
        let probe = unsafe { &mut *context.cast::<Permit>() };
        assert_eq!((generation, frames), (25, 960));
        probe.calls += 1;
        probe.answer
    }
    fn render(runtime: &mut StationRuntime) -> [f32; 1920] {
        let mut output = [0.0; 1920];
        assert_eq!(
            // SAFETY: the fake audio provider has no background worker; this test is sole TX owner.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            0
        );
        output
    }
    let (mut runtime, mut control) = runtime(25); // Existing fixture executes its actual graph port.
    let hardware = runtime.hardware_state();
    hardware.publish_requests(ControllerRequests {
        transmit: true,
        render_admitted: true,
        ..ControllerRequests::default()
    });
    let mut probe = Permit {
        answer: 0,
        calls: 0,
    };
    let port = usbradioplus_runtime::ProgrammeFence {
        context: ptr::from_mut(&mut probe).cast(),
        permit,
    };
    // SAFETY: probe stays live through confirmed stop/unbind; no real hardware is opened.
    unsafe { runtime.bind_programme_fence(Some(port)) }.unwrap();
    runtime.start().unwrap();
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    assert!(
        hardware.outputs().logical_ptt,
        "programme suppression must not rewrite signaling intent"
    );
    assert!(matches!(
        runtime.retire_programme(&mut control, &factory(), radio_provider(), 25, 11),
        Err(crate::StationUpdateError::Pending)
    ));
    update_tick(&mut runtime, true);
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    runtime
        .retire_programme(&mut control, &factory(), radio_provider(), 25, 11)
        .unwrap();
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    assert_eq!(
        // SAFETY: no concurrent callback; this is the just-completed render's record.
        unsafe {
            (*runtime._transmit_context.get())
                .feedback_record
                .programme_retirement_id
        },
        11
    );
    probe.answer = 1;
    assert!(render(&mut runtime).iter().any(|v| *v != 0.0));
    probe.answer = 0;
    let mut config = control.plan().configuration().clone();
    config.voice_telemetry.output_gain_db = -3.0;
    let plan = StationPlan::new("usb", config, control.plan().transport(), 25, 960).unwrap();
    let update = runtime
        .prepare_update(&control, plan, &factory(), radio_provider())
        .unwrap();
    runtime.begin_update(&control, update).unwrap();
    update_tick(&mut runtime, true);
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    runtime.finish_update(&mut control, true).unwrap();
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    // SAFETY: running unbind is rejected and cannot release the active context.
    assert!(unsafe { runtime.bind_programme_fence(None) }.is_err());
    runtime.stop().unwrap();
    runtime.start().unwrap();
    assert!(render(&mut runtime).iter().all(|v| *v == 0.0));
    assert!(probe.calls >= 8);
    runtime.stop().unwrap();
    // SAFETY: successful stop quiesced both callbacks before releasing the probe.
    unsafe { runtime.bind_programme_fence(None) }.unwrap();
    runtime.start().unwrap();
    assert!(render(&mut runtime).iter().any(|v| *v != 0.0));
    runtime.stop().unwrap();
}

#[test]
fn programme_fence_stays_closed_through_failed_forward_and_reverse_adoption() {
    unsafe extern "C" fn deny(raw: *mut c_void, generation: u64, frames: u32) -> u32 {
        assert_eq!((generation, frames), (25, 960));
        // SAFETY: this serial fixture retains the counter through stopped unbind.
        unsafe { *raw.cast::<u32>() += 1 };
        0
    }
    fn render(runtime: &mut StationRuntime) {
        let mut output = [1.0; 1920];
        assert_eq!(
            // SAFETY: fake audio has no background owner; the fixture lends exact stereo output.
            unsafe {
                transmit_callback(
                    runtime._transmit_context.get().cast(),
                    output.as_mut_ptr(),
                    960,
                )
            },
            0
        );
        assert!(output.iter().all(|sample| *sample == 0.0));
    }
    for fail_receive in [true, false] {
        let (mut runtime, mut control) = runtime(25);
        runtime
            .hardware_state()
            .publish_requests(ControllerRequests {
                transmit: true,
                render_admitted: true,
                ..ControllerRequests::default()
            });
        let mut calls = 0_u32;
        // SAFETY: this counter/code remains live through stopped unbind; no hardware opens.
        unsafe {
            runtime.bind_programme_fence(Some(usbradioplus_runtime::ProgrammeFence {
                context: ptr::from_mut(&mut calls).cast(),
                permit: deny,
            }))
        }
        .unwrap();
        runtime.start().unwrap();
        render(&mut runtime);
        let original_epoch = control.transmit_loudness().unwrap().epoch;
        assert!(matches!(
            runtime.retire_programme(&mut control, &factory(), radio_provider(), 25, 11),
            Err(crate::StationUpdateError::Pending)
        ));
        crate::media::tests::FAIL_NEXT_APPLY.set(true);
        if fail_receive {
            update_tick(&mut runtime, true);
            render(&mut runtime);
        } else {
            render(&mut runtime);
            update_tick(&mut runtime, true);
        }
        assert!(matches!(
            runtime.retire_programme(&mut control, &factory(), radio_provider(), 25, 11),
            Err(crate::StationUpdateError::Pending)
        ));
        update_tick(&mut runtime, true);
        render(&mut runtime);
        assert!(
            runtime
                .retire_programme(&mut control, &factory(), radio_provider(), 25, 11)
                .is_err()
        );
        render(&mut runtime);
        assert_ne!(control.transmit_loudness().unwrap().epoch, original_epoch);
        assert_eq!(
            // SAFETY: the fixture is the only callback owner; this is completed-render feedback.
            unsafe {
                (*runtime._transmit_context.get())
                    .feedback_record
                    .programme_retirement_id
            },
            0
        );
        assert!(
            calls >= 4,
            "both forward and reverse renders must invoke the same gate"
        );
        runtime.stop().unwrap();
        // SAFETY: successful stop quiesced all borrowers before releasing the counter.
        unsafe { runtime.bind_programme_fence(None) }.unwrap();
    }
}
