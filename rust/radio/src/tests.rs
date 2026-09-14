use super::*;
use crate::session::{
    EventOwner, bool_from_raw, bounded_frame_count, ctcss_from_raw, error_from_result, event,
    map_result, receive_frame_count, receive_result, ring_is_valid, session_snapshot,
    transmit_frame_count, transmit_result, transmitter_state, validate_config,
};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};

const CREATE_FAILURE_GENERATION: u64 = 9_001;
const WARM_FAILURE_GENERATION: u64 = 9_002;
const NULL_SUCCESS_GENERATION: u64 = 9_003;
const RECEIVE_FAILURE_GENERATION: u64 = 9_004;
const TRANSMIT_FAILURE_GENERATION: u64 = 9_005;
const MALFORMED_GENERATION: u64 = 9_006;
const NONFINITE_OUTPUT_GENERATION: u64 = 9_007;
const SNAPSHOT_FAILURE_GENERATION: u64 = 9_008;
const INVALID_POP_COUNT_GENERATION: u64 = 9_009;
const NULL_FAILURE_GENERATION: u64 = 9_010;

const CAPABILITY_BYTES: &[u8] = b"rptadv.radio-core\0";
const WRONG_CAPABILITY_BYTES: &[u8] = b"not.radio-core\0";

#[derive(Default)]
struct FakeLifecycle {
    warm_calls: AtomicUsize,
    destroy_calls: AtomicUsize,
}

struct FakeSession {
    config: RawSessionConfig,
    lifecycle: *const FakeLifecycle,
    warmed: AtomicBool,
    receive_calls: AtomicU64,
    transmit_calls: AtomicU64,
    receive_event_pending: AtomicBool,
    transmit_event_pending: AtomicBool,
}

unsafe extern "C" fn fake_create(
    config: *const RawSessionConfig,
    ports: *const RawSessionPorts,
    output: *mut *mut OpaqueSession,
) -> c_int {
    if config.is_null() || ports.is_null() || output.is_null() {
        return RESULT_INVALID_ARGUMENT;
    }
    // SAFETY: all pointers were checked and the wrapper supplies complete
    // values for this synchronous fake descriptor call.
    let (config, lifecycle) = unsafe {
        output.write(ptr::null_mut());
        (
            ptr::read(config),
            (*ports).receive_deemphasis.context.cast::<FakeLifecycle>(),
        )
    };
    match config.generation_id {
        NULL_SUCCESS_GENERATION => return RESULT_OK,
        NULL_FAILURE_GENERATION => return RESULT_PROVIDER_FAILED,
        _ => {}
    }
    let session = Box::new(FakeSession {
        config,
        lifecycle,
        warmed: AtomicBool::new(false),
        receive_calls: AtomicU64::new(0),
        transmit_calls: AtomicU64::new(0),
        receive_event_pending: AtomicBool::new(false),
        transmit_event_pending: AtomicBool::new(false),
    });
    // SAFETY: output is a checked writable destination and now owns the box.
    unsafe { output.write(Box::into_raw(session).cast::<OpaqueSession>()) };
    if config.generation_id == CREATE_FAILURE_GENERATION {
        RESULT_PROVIDER_FAILED
    } else {
        RESULT_OK
    }
}

unsafe extern "C" fn fake_warm(session: *mut OpaqueSession) -> c_int {
    let Some(session) = NonNull::new(session.cast::<FakeSession>()) else {
        return RESULT_INVALID_ARGUMENT;
    };
    // SAFETY: fake_create returns this exact allocation type and it remains
    // exclusively owned by the wrapper while warm runs.
    let session = unsafe { session.as_ref() };
    if !session.lifecycle.is_null() {
        // SAFETY: the test retains the lifecycle through session destruction.
        unsafe {
            (*session.lifecycle)
                .warm_calls
                .fetch_add(1, Ordering::Relaxed)
        };
    }
    if session.config.generation_id == WARM_FAILURE_GENERATION {
        return RESULT_PROVIDER_FAILED;
    }
    session.warmed.store(true, Ordering::Release);
    RESULT_OK
}

unsafe extern "C" fn fake_receive(
    session: *mut OpaqueSession,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
    controls: *const RawReceiveInput,
    result: *mut RawReceiveResult,
) -> c_int {
    if session.is_null()
        || input.is_null()
        || output.is_null()
        || controls.is_null()
        || result.is_null()
        || frame_count == 0
    {
        return RESULT_INVALID_ARGUMENT;
    }
    // SAFETY: fake_create returns FakeSession and the wrapper supplies exact
    // slices and complete control/result values for the synchronous call.
    let session = unsafe { &*session.cast::<FakeSession>() };
    if !session.warmed.load(Ordering::Acquire) {
        return RESULT_NOT_READY;
    }
    if frame_count > session.config.maximum_receive_frame_count {
        return RESULT_FRAME_COUNT_EXCEEDED;
    }
    // SAFETY: the wrapper guarantees two input samples and one output sample
    // for each declared frame.
    let (input, output) = unsafe {
        (
            std::slice::from_raw_parts(input, frame_count as usize * CANONICAL_CHANNELS),
            std::slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    if session.config.generation_id == RECEIVE_FAILURE_GENERATION {
        output.fill(0.75);
        return RESULT_PROVIDER_FAILED;
    }
    for (frame, sample) in output.iter_mut().enumerate() {
        *sample = input[frame * CANONICAL_CHANNELS];
    }
    if session.config.generation_id == NONFINITE_OUTPUT_GENERATION {
        output[0] = f32::NAN;
    }
    let calls = session.receive_calls.fetch_add(1, Ordering::Relaxed) + 1;
    session.receive_event_pending.store(true, Ordering::Release);
    let malformed = session.config.generation_id == MALFORMED_GENERATION;
    // SAFETY: result is a checked writable destination.
    unsafe {
        result.write(RawReceiveResult {
            generation_id: session.config.generation_id,
            first_sample_index: (calls - 1) * u64::from(frame_count),
            frame_count,
            carrier_active: if malformed {
                2
            } else {
                (*controls).hardware_carrier
            },
            subaudible_active: 0,
            receiver_keyed: (*controls).hardware_carrier | (*controls).subaudible_override,
            ctcss_decoded_index: -1,
            dcs_valid: 0,
            rssi_peak: 12,
            rssi_updated: 1,
            ctcss_decoder_peak: 2_400.0 / 32_768.0,
            input_peak: 0.5,
            input_rms: 0.25,
            input_rail_samples: 0,
            output_peak: 0.5,
            output_rms: 0.25,
            output_rail_samples: 0,
            periodic_status_due: 0,
        });
    }
    RESULT_OK
}

unsafe extern "C" fn fake_transmit(
    session: *mut OpaqueSession,
    output: *mut f32,
    frame_count: u32,
    controls: *const RawTransmitInput,
    result: *mut RawTransmitResult,
) -> c_int {
    if session.is_null()
        || output.is_null()
        || controls.is_null()
        || result.is_null()
        || frame_count == 0
    {
        return RESULT_INVALID_ARGUMENT;
    }
    // SAFETY: fake_create returns FakeSession and the wrapper supplies exact
    // output and complete control/result values for the synchronous call.
    let session = unsafe { &*session.cast::<FakeSession>() };
    if !session.warmed.load(Ordering::Acquire) {
        return RESULT_NOT_READY;
    }
    if frame_count > session.config.maximum_transmit_frame_count {
        return RESULT_FRAME_COUNT_EXCEEDED;
    }
    // SAFETY: the wrapper guarantees two output samples per declared frame.
    let output = unsafe {
        std::slice::from_raw_parts_mut(output, frame_count as usize * CANONICAL_CHANNELS)
    };
    if session.config.generation_id == TRANSMIT_FAILURE_GENERATION {
        output.fill(0.75);
        return RESULT_BUSY;
    }
    output.fill(0.25);
    if session.config.generation_id == NONFINITE_OUTPUT_GENERATION {
        output[0] = f32::NAN;
    }
    let calls = session.transmit_calls.fetch_add(1, Ordering::Relaxed) + 1;
    session
        .transmit_event_pending
        .store(true, Ordering::Release);
    let malformed = session.config.generation_id == MALFORMED_GENERATION;
    // SAFETY: result is a checked writable destination.
    unsafe {
        result.write(RawTransmitResult {
            generation_id: session.config.generation_id,
            first_sample_index: (calls - 1) * u64::from(frame_count),
            frame_count,
            logical_ptt: (*controls).external_ptt_request,
            transmitter_state: if malformed { 3 } else { 1 },
            selected_ctcss_tenths_hz: if (*controls).forced_ctcss_tenths_hz == 0 {
                1_000
            } else {
                (*controls).forced_ctcss_tenths_hz
            },
            program_peak: 0.25,
            program_rms: 0.25,
            program_rail_samples: 0,
            output_peak: 0.25,
            output_rms: 0.25,
            output_rail_samples: 0,
            periodic_status_due: 0,
            program_ring: RingObservation {
                ratio: 1.0,
                ..RingObservation::default()
            },
        });
    }
    RESULT_OK
}

unsafe extern "C" fn fake_snapshot(
    session: *const OpaqueSession,
    output: *mut RawSnapshot,
) -> c_int {
    if session.is_null() || output.is_null() {
        return RESULT_INVALID_ARGUMENT;
    }
    // SAFETY: fake_create returns FakeSession and output is checked writable.
    let session = unsafe { &*session.cast::<FakeSession>() };
    if session.config.generation_id == SNAPSHOT_FAILURE_GENERATION {
        return RESULT_UNSUPPORTED;
    }
    let malformed = session.config.generation_id == MALFORMED_GENERATION;
    // SAFETY: output is a checked writable destination.
    unsafe {
        output.write(RawSnapshot {
            generation_id: session.config.generation_id,
            receive_frames: session.receive_calls.load(Ordering::Relaxed),
            transmit_frames: session.transmit_calls.load(Ordering::Relaxed),
            receive_ctcss_decoder_peak: 2_400.0 / 32_768.0,
            logical_ptt: u32::from(malformed) * 2,
            ctcss_decoded_index: -1,
            program_ring: RingObservation {
                ratio: 1.0,
                ..RingObservation::default()
            },
            ..RawSnapshot::default()
        });
    }
    RESULT_OK
}

unsafe extern "C" fn fake_pop_receive_event(
    session: *const OpaqueSession,
    output: *mut RawEvent,
) -> u32 {
    if session.is_null() || output.is_null() {
        return 0;
    }
    // SAFETY: fake_create returns FakeSession and output is checked writable.
    let session = unsafe { &*session.cast::<FakeSession>() };
    if session.config.generation_id == INVALID_POP_COUNT_GENERATION {
        return 2;
    }
    if !session.receive_event_pending.swap(false, Ordering::AcqRel) {
        return 0;
    }
    // SAFETY: output is a checked writable destination.
    unsafe {
        output.write(RawEvent {
            generation_id: session.config.generation_id,
            sample_index: session.receive_calls.load(Ordering::Relaxed),
            kind: if session.config.generation_id == MALFORMED_GENERATION {
                99
            } else {
                1
            },
            value: 1,
        });
    }
    1
}

unsafe extern "C" fn fake_pop_transmit_event(
    session: *const OpaqueSession,
    output: *mut RawEvent,
) -> u32 {
    if session.is_null() || output.is_null() {
        return 0;
    }
    // SAFETY: fake_create returns FakeSession and output is checked writable.
    let session = unsafe { &*session.cast::<FakeSession>() };
    if !session.transmit_event_pending.swap(false, Ordering::AcqRel) {
        return 0;
    }
    // SAFETY: output is a checked writable destination.
    unsafe {
        output.write(RawEvent {
            generation_id: session.config.generation_id,
            sample_index: session.transmit_calls.load(Ordering::Relaxed),
            kind: 6,
            value: 1,
        });
    }
    1
}

unsafe extern "C" fn fake_destroy(session: *mut OpaqueSession) {
    if session.is_null() {
        return;
    }
    // SAFETY: the wrapper transfers each fake_create allocation here once.
    let session = unsafe { Box::from_raw(session.cast::<FakeSession>()) };
    if !session.lifecycle.is_null() {
        // SAFETY: the test retains the lifecycle through this destruction.
        unsafe {
            (*session.lifecycle)
                .destroy_calls
                .fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn valid_descriptor() -> RawDescriptor {
    RawDescriptor {
        struct_size: size_of::<RawDescriptor>() as u32,
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY_BYTES.as_ptr().cast::<c_char>(),
        session_create: Some(fake_create),
        session_warm: Some(fake_warm),
        session_receive: Some(fake_receive),
        session_transmit: Some(fake_transmit),
        session_snapshot: Some(fake_snapshot),
        session_pop_receive_event: Some(fake_pop_receive_event),
        session_pop_transmit_event: Some(fake_pop_transmit_event),
        session_destroy: Some(fake_destroy),
    }
}

fn provider() -> RadioProvider {
    let descriptor = valid_descriptor();
    // SAFETY: the descriptor and C string are readable for validation, while
    // all copied function addresses have static test-binary lifetime.
    unsafe { RadioProvider::from_raw_descriptor(ptr::from_ref(&descriptor).cast()) }.unwrap()
}

fn ports(lifecycle: &FakeLifecycle) -> SessionPorts<'_> {
    let mut ports = SessionPorts::default();
    ports.receive_deemphasis.raw.context = ptr::from_ref(lifecycle).cast_mut().cast();
    ports
}

fn prepare(
    generation_id: u64,
    lifecycle: &FakeLifecycle,
    maximum_frames: u32,
) -> Result<PreparedSession<'_>, RadioError> {
    provider().prepare(
        &SessionConfig::new(generation_id, maximum_frames, maximum_frames),
        ports(lifecycle),
    )
}

#[test]
fn descriptor_validation_requires_the_complete_abi3_table() {
    // SAFETY: null is explicitly accepted as an incompatible descriptor.
    let null_result = unsafe { RadioProvider::from_raw_descriptor(ptr::null()) };
    assert_eq!(null_result.err(), Some(RadioError::IncompatibleAdapter));

    for defect in 0..11 {
        let mut descriptor = valid_descriptor();
        match defect {
            0 => descriptor.struct_size = (REQUIRED_DESCRIPTOR_SIZE - 1) as u32,
            1 => descriptor.abi_version = ABI_VERSION - 1,
            2 => descriptor.capability_name = ptr::null(),
            3 => {
                descriptor.capability_name = WRONG_CAPABILITY_BYTES.as_ptr().cast::<c_char>();
            }
            4 => descriptor.session_create = None,
            5 => descriptor.session_warm = None,
            6 => descriptor.session_receive = None,
            7 => descriptor.session_transmit = None,
            8 => descriptor.session_snapshot = None,
            9 => descriptor.session_pop_receive_event = None,
            10 => descriptor.session_pop_transmit_event = None,
            _ => unreachable!(),
        }
        // SAFETY: this local descriptor remains readable for the validation
        // call and its non-null C string has static lifetime.
        let result =
            unsafe { RadioProvider::from_raw_descriptor(ptr::from_ref(&descriptor).cast()) };
        assert_eq!(result.err(), Some(RadioError::IncompatibleAdapter));
    }

    let mut descriptor = valid_descriptor();
    descriptor.session_destroy = None;
    // SAFETY: this local descriptor remains readable for validation.
    let missing_destroy =
        unsafe { RadioProvider::from_raw_descriptor(ptr::from_ref(&descriptor).cast()) };
    assert_eq!(missing_destroy.err(), Some(RadioError::IncompatibleAdapter));
}

#[test]
fn high_level_configuration_maps_to_the_flat_abi() {
    let tone = CtcssToneIndex::new(37).unwrap();
    let mut config = SessionConfig::new(44, 160, 240);
    config.publication_interval_milliseconds = 75;
    config.receive_channel = ReceiveChannel::Second;
    config.receive_input_gain = 1.25;
    config.receive.noise_filter_profile = NoiseFilterProfile::Alternate;
    config.receive.squelch_open_level = 31;
    config.receive.squelch_hysteresis = 7;
    config.receive.ctcss_decoder_gain = 2.0;
    config.receive.vox_threshold = 123;
    config.receive.vox_hang_milliseconds = 456;
    config.receive.signaling = ReceiveSignaling::Ctcss(CtcssReceiveConfig {
        tones: CtcssToneMask::EMPTY.with(tone),
        relaxed: true,
    });
    config.qualification.carrier_source = CarrierSource::Vox;
    config.qualification.subaudible_source = SubaudibleSource::Dsp;
    config.transmit.signaling = TransmitSignaling::Ctcss(CtcssTransmitConfig {
        default_frequency_tenths_hz: 1_000,
        mapped_frequencies_tenths_hz: [1_234; CTCSS_TONE_COUNT],
        peak: 0.2,
        turnoff_duration_milliseconds: 80,
        turnoff_phase_shift_degrees: 180.0,
        turnoff_tail_tone_hz: 55.0,
    });
    config.transmit.output_a = OutputConfig {
        route: OutputRoute::Composite,
        tone_gain: 0.5,
        tone_bias: 0.1,
    };

    let raw = config.as_raw();
    assert_eq!(raw.struct_size as usize, size_of::<RawSessionConfig>());
    assert_eq!(raw.abi_version, ABI_VERSION);
    assert_eq!(raw.native_sample_rate_hz, NATIVE_SAMPLE_RATE_HZ);
    assert_eq!(raw.interleaved_channels as usize, CANONICAL_CHANNELS);
    assert_eq!(raw.receive.noise_filter_profile, 1);
    assert_eq!(raw.receive.ctcss_tone_mask, 1_u64 << 37);
    assert_eq!(raw.receive.dcs_enabled, 0);
    assert_eq!(raw.qualification.carrier_source, 2);
    assert_eq!(raw.transmit.ctcss_transmit_enabled, 1);
    assert_eq!(raw.transmit.mapped_ctcss_frequency_tenths_hz[0], 1_234);
    assert_eq!(raw.transmit.dcs_turnoff_enabled, 0);
    assert_eq!(raw.transmit.output_a_route, 3);
}

#[test]
fn rust_layout_matches_the_abi3_c_header() {
    assert_eq!(
        (
            size_of::<RawReceiveConfig>(),
            align_of::<RawReceiveConfig>()
        ),
        (64, 8)
    );
    assert_eq!(offset_of!(RawReceiveConfig, ctcss_tone_mask), 32);
    assert_eq!(
        offset_of!(RawReceiveConfig, native_squelch_delay_frames),
        60
    );
    assert_eq!(
        (
            size_of::<RawTransmitConfig>(),
            align_of::<RawTransmitConfig>()
        ),
        (248, 8)
    );
    assert_eq!(offset_of!(RawTransmitConfig, tone_off_mode), 160);
    assert_eq!(
        offset_of!(RawTransmitConfig, ctcss_turnoff_phase_shift_degrees),
        168
    );
    assert_eq!(offset_of!(RawTransmitConfig, dcs_transmit_enabled), 184);
    assert_eq!(offset_of!(RawTransmitConfig, output_b_tone_bias), 244);
    assert_eq!(
        (
            size_of::<RawSessionConfig>(),
            align_of::<RawSessionConfig>()
        ),
        (392, 8)
    );
    assert_eq!(offset_of!(RawSessionConfig, receive), 48);
    assert_eq!(offset_of!(RawSessionConfig, qualification), 112);
    assert_eq!(offset_of!(RawSessionConfig, transmit), 144);
    assert_eq!(size_of::<RawSessionPorts>(), 1_472);
    assert_eq!(offset_of!(RawSessionPorts, receive_ctcss_notch), 72);
    assert_eq!(offset_of!(RawSessionPorts, receive_noise_reduction), 1_288);
    assert_eq!(offset_of!(RawSessionPorts, transmit_program), 1_352);
    assert_eq!(
        offset_of!(RawSessionPorts, transmit_dcs_normal_filter),
        1_384
    );
    assert_eq!(
        offset_of!(RawSessionPorts, transmit_dcs_turnoff_filter),
        1_416
    );
    assert_eq!(offset_of!(RawSessionPorts, program_ring), 1_448);
    assert_eq!(size_of::<RawDescriptor>(), 80);
    assert_eq!(size_of::<RawReceiveInput>(), 20);
    assert_eq!(size_of::<RawTransmitInput>(), 24);
    assert_eq!(size_of::<RawReceiveResult>(), 96);
    assert_eq!(size_of::<RawTransmitResult>(), 120);
    assert_eq!(size_of::<RawEvent>(), 24);
    assert_eq!(size_of::<RawSnapshot>(), 192);
    assert_eq!(size_of::<RingObservation>(), 48);
    assert_eq!(size_of::<ProgramRingResult>(), 64);
}

#[test]
fn lifecycle_warms_splits_and_destroys_after_every_owner_stops() {
    let lifecycle = FakeLifecycle::default();
    let prepared = prepare(42, &lifecycle, 8).unwrap();
    assert_eq!(lifecycle.warm_calls.load(Ordering::Relaxed), 1);
    let (mut receive, mut transmit, mut observer) = prepared.split();

    assert_eq!(observer.pop_receive_event().unwrap(), None);
    let mut receive_output = [9.0; 2];
    let receive_result = receive
        .process(
            &[0.25, 0.0, -0.5, 0.0],
            &mut receive_output,
            ReceiveControls {
                hardware_carrier: true,
                subaudible_override: true,
                ..ReceiveControls::default()
            },
        )
        .unwrap();
    assert_eq!(receive_output, [0.25, -0.5]);
    assert!(receive_result.carrier_active);
    assert_eq!(receive_result.generation_id, 42);
    assert_eq!(receive_result.ctcss_decoder_peak, 2_400.0 / 32_768.0);

    let mut transmit_output = [9.0; 4];
    let transmit_result = transmit
        .render(
            &mut transmit_output,
            TransmitControls {
                external_ptt_request: true,
                forced_ctcss_tenths_hz: 1_234,
                ..TransmitControls::default()
            },
        )
        .unwrap();
    assert_eq!(transmit_output, [0.25; 4]);
    assert_eq!(transmit_result.selected_ctcss_tenths_hz, 1_234);
    assert_eq!(transmit_result.transmitter_state, TransmitterState::Active);
    let snapshot = observer.snapshot().unwrap();
    assert_eq!(snapshot.receive_frames, 1);
    assert_eq!(snapshot.transmit_frames, 1);
    assert_eq!(snapshot.receive_ctcss_decoder_peak, 2_400.0 / 32_768.0);
    assert_eq!(
        observer.pop_receive_event().unwrap().unwrap().value,
        EventValue::Carrier(true)
    );
    assert_eq!(
        observer.pop_transmit_event().unwrap().unwrap().value,
        EventValue::Ptt(true)
    );

    drop(observer);
    assert_eq!(lifecycle.destroy_calls.load(Ordering::Relaxed), 0);
    drop(receive);
    assert_eq!(lifecycle.destroy_calls.load(Ordering::Relaxed), 0);
    drop(transmit);
    assert_eq!(lifecycle.destroy_calls.load(Ordering::Relaxed), 1);
}

#[test]
fn callback_boundaries_reject_bad_spans_without_entering_the_adapter() {
    let lifecycle = FakeLifecycle::default();
    let (mut receive, mut transmit, observer) = prepare(50, &lifecycle, 2).unwrap().split();

    let mut receive_output = [8.0; 2];
    assert_eq!(
        receive.process(&[0.0, 0.0], &mut receive_output, ReceiveControls::default()),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(receive_output, [0.0; 2]);
    assert_eq!(
        receive.process(
            &[0.0, 0.0, f32::NAN, 0.0],
            &mut receive_output,
            ReceiveControls::default(),
        ),
        Err(RadioError::InvalidArgument)
    );
    let mut too_many_receive = [8.0; 3];
    assert_eq!(
        receive.process(&[0.0; 6], &mut too_many_receive, ReceiveControls::default(),),
        Err(RadioError::FrameCountExceeded)
    );
    assert_eq!(too_many_receive, [0.0; 3]);

    let mut odd_transmit = [8.0; 3];
    assert_eq!(
        transmit.render(&mut odd_transmit, TransmitControls::default()),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(odd_transmit, [0.0; 3]);
    let mut too_many_transmit = [8.0; 6];
    assert_eq!(
        transmit.render(&mut too_many_transmit, TransmitControls::default()),
        Err(RadioError::FrameCountExceeded)
    );
    assert_eq!(too_many_transmit, [0.0; 6]);
    let snapshot = observer.snapshot().unwrap();
    assert_eq!(snapshot.receive_frames, 0);
    assert_eq!(snapshot.transmit_frames, 0);
}

#[test]
fn adapter_failures_are_mapped_and_output_is_forced_silent() {
    let receive_lifecycle = FakeLifecycle::default();
    let (mut receive, transmit, observer) =
        prepare(RECEIVE_FAILURE_GENERATION, &receive_lifecycle, 2)
            .unwrap()
            .split();
    let mut output = [9.0; 2];
    assert_eq!(
        receive.process(&[0.0; 4], &mut output, ReceiveControls::default()),
        Err(RadioError::ProviderFailed)
    );
    assert_eq!(output, [0.0; 2]);
    drop((receive, transmit, observer));

    let transmit_lifecycle = FakeLifecycle::default();
    let (receive, mut transmit, observer) =
        prepare(TRANSMIT_FAILURE_GENERATION, &transmit_lifecycle, 2)
            .unwrap()
            .split();
    let mut output = [9.0; 4];
    assert_eq!(
        transmit.render(&mut output, TransmitControls::default()),
        Err(RadioError::Busy)
    );
    assert_eq!(output, [0.0; 4]);
    drop((receive, transmit, observer));
}

#[test]
fn partial_creation_warm_failure_and_null_success_are_cleaned_up() {
    let partial = FakeLifecycle::default();
    assert_eq!(
        prepare(CREATE_FAILURE_GENERATION, &partial, 2).err(),
        Some(RadioError::ProviderFailed)
    );
    assert_eq!(partial.warm_calls.load(Ordering::Relaxed), 0);
    assert_eq!(partial.destroy_calls.load(Ordering::Relaxed), 1);

    let warm = FakeLifecycle::default();
    assert_eq!(
        prepare(WARM_FAILURE_GENERATION, &warm, 2).err(),
        Some(RadioError::ProviderFailed)
    );
    assert_eq!(warm.warm_calls.load(Ordering::Relaxed), 1);
    assert_eq!(warm.destroy_calls.load(Ordering::Relaxed), 1);

    let null = FakeLifecycle::default();
    assert_eq!(
        prepare(NULL_SUCCESS_GENERATION, &null, 2).err(),
        Some(RadioError::AdapterFailure)
    );
    assert_eq!(null.destroy_calls.load(Ordering::Relaxed), 0);

    let null_failure = FakeLifecycle::default();
    assert_eq!(
        prepare(NULL_FAILURE_GENERATION, &null_failure, 2).err(),
        Some(RadioError::ProviderFailed)
    );
    assert_eq!(null_failure.destroy_calls.load(Ordering::Relaxed), 0);
}

#[test]
fn receive_and_transmit_endpoints_progress_concurrently() {
    fn assert_send<T: Send>() {}
    assert_send::<ReceiveEndpoint<'static>>();
    assert_send::<TransmitEndpoint<'static>>();
    assert_send::<ControlObserver<'static>>();

    let lifecycle = FakeLifecycle::default();
    let (receive, transmit, observer) = prepare(77, &lifecycle, 8).unwrap().split();
    let (receive, transmit) = std::thread::scope(|scope| {
        let receive_thread = scope.spawn(move || {
            let mut receive = receive;
            let mut output = [0.0; 4];
            for _ in 0..1_000 {
                receive
                    .process(&[0.1; 8], &mut output, ReceiveControls::default())
                    .unwrap();
            }
            receive
        });
        let transmit_thread = scope.spawn(move || {
            let mut transmit = transmit;
            let mut output = [0.0; 8];
            for _ in 0..1_000 {
                transmit
                    .render(&mut output, TransmitControls::default())
                    .unwrap();
            }
            transmit
        });
        (
            receive_thread.join().unwrap(),
            transmit_thread.join().unwrap(),
        )
    });
    let snapshot = observer.snapshot().unwrap();
    assert_eq!(snapshot.receive_frames, 1_000);
    assert_eq!(snapshot.transmit_frames, 1_000);
    drop((receive, transmit, observer));
    assert_eq!(lifecycle.destroy_calls.load(Ordering::Relaxed), 1);
}

#[test]
fn malformed_results_snapshots_and_events_are_rejected() {
    let lifecycle = FakeLifecycle::default();
    let (mut receive, mut transmit, mut observer) = prepare(MALFORMED_GENERATION, &lifecycle, 2)
        .unwrap()
        .split();
    let mut receive_output = [9.0; 2];
    assert_eq!(
        receive.process(&[0.0; 4], &mut receive_output, ReceiveControls::default(),),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(receive_output, [0.0; 2]);
    assert_eq!(
        observer.pop_receive_event(),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(observer.snapshot(), Err(RadioError::AdapterFailure));

    let mut transmit_output = [9.0; 4];
    assert_eq!(
        transmit.render(&mut transmit_output, TransmitControls::default()),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(transmit_output, [0.0; 4]);
}

#[test]
fn local_configuration_validation_precedes_creation() {
    let lifecycle = FakeLifecycle::default();
    let mut config = SessionConfig::new(88, 0, 2);
    assert_eq!(
        provider().prepare(&config, ports(&lifecycle)).err(),
        Some(RadioError::InvalidArgument)
    );
    config.maximum_receive_frame_count = 2;
    config.receive.ctcss_decoder_gain = f32::INFINITY;
    assert_eq!(
        provider().prepare(&config, ports(&lifecycle)).err(),
        Some(RadioError::InvalidArgument)
    );
    config.receive.ctcss_decoder_gain = 1.0;
    config.transmit.signaling = TransmitSignaling::Ctcss(CtcssTransmitConfig {
        mapped_frequencies_tenths_hz: [-1; CTCSS_TONE_COUNT],
        ..CtcssTransmitConfig::default()
    });
    assert_eq!(
        provider().prepare(&config, ports(&lifecycle)).err(),
        Some(RadioError::InvalidArgument)
    );
    assert_eq!(lifecycle.warm_calls.load(Ordering::Relaxed), 0);
    assert_eq!(lifecycle.destroy_calls.load(Ordering::Relaxed), 0);
}

#[test]
fn typed_values_and_borrowed_ports_cover_the_narrow_public_surface() {
    let errors = [
        (
            RadioError::IncompatibleAdapter,
            "incompatible radio-core adapter",
        ),
        (
            RadioError::InvalidArgument,
            "invalid radio-session argument",
        ),
        (RadioError::ProviderFailed, "radio-session provider failed"),
        (
            RadioError::FrameCountExceeded,
            "radio-session frame bound exceeded",
        ),
        (
            RadioError::Unsupported,
            "unsupported radio-session stream shape",
        ),
        (RadioError::NotReady, "radio session is not warmed"),
        (RadioError::Busy, "radio-session endpoint is already busy"),
        (
            RadioError::AdapterFailure,
            "radio-core adapter returned invalid data",
        ),
    ];
    for (error, expected) in errors {
        assert_eq!(error.to_string(), expected);
    }

    assert_eq!(CtcssToneIndex::new(CTCSS_TONE_COUNT as u8), None);
    let tone = CtcssToneIndex::new(5).unwrap();
    assert_eq!(tone.get(), 5);
    assert_eq!(
        CtcssToneMask::from_bits(1_u64 << 5).unwrap().bits(),
        1_u64 << 5
    );
    assert_eq!(
        CtcssToneMask::from_bits(1_u64 << CTCSS_TONE_COUNT),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(CtcssToneMask::EMPTY.with(tone).bits(), 1_u64 << 5);

    let observation = RingObservation {
        ratio: 1.0,
        ..RingObservation::default()
    };
    let ring_result = ProgramRingResult::new(observation, true, Some(tone), true);
    assert_eq!(ring_result.observation, observation);
    assert_eq!(ring_result.receiver_keyed, 1);
    assert_eq!(ring_result.ctcss_decoded_index, 5);
    assert_eq!(ring_result.dcs_valid, 1);
    assert_eq!(ProgramRingResult::default().ctcss_decoded_index, -1);

    unsafe extern "C" fn process(
        _context: *mut c_void,
        _input: *const f32,
        _output: *mut f32,
        _frames: u32,
    ) -> c_int {
        RESULT_OK
    }
    unsafe extern "C" fn bypass(_context: *mut c_void, _frames: u32) -> c_int {
        RESULT_OK
    }
    unsafe extern "C" fn warm(_context: *mut c_void, _frames: u32) -> c_int {
        RESULT_OK
    }
    unsafe extern "C" fn render(
        _context: *mut c_void,
        _output: *mut f32,
        _frames: u32,
        _result: *mut ProgramRingResult,
    ) -> c_int {
        RESULT_OK
    }

    let mut context = 0_u8;
    let mut normal_dcs_context = 0_u8;
    let mut turnoff_dcs_context = 0_u8;
    let context = NonNull::from(&mut context).cast::<c_void>();
    let normal_dcs_context = NonNull::from(&mut normal_dcs_context).cast::<c_void>();
    let turnoff_dcs_context = NonNull::from(&mut turnoff_dcs_context).cast::<c_void>();
    // SAFETY: the local context outlives these unexecuted test bindings and
    // the callbacks accept its opaque address without dereferencing it.
    let processor = unsafe { ProcessorPort::from_raw(context, process, Some(bypass), Some(warm)) };
    // SAFETY: the same lifetime and callback argument apply to this binding.
    let program_ring = unsafe { ProgramRingPort::from_raw(context, render, Some(warm)) };
    // SAFETY: both local contexts outlive these unexecuted test bindings.
    let normal_dcs = unsafe { ProcessorPort::from_raw(normal_dcs_context, process, None, None) };
    // SAFETY: the local context outlives this unexecuted test binding.
    let turnoff_dcs = unsafe { ProcessorPort::from_raw(turnoff_dcs_context, process, None, None) };
    assert_eq!(processor.raw.context, context.as_ptr());
    assert!(processor.raw.process_f32.is_some());
    assert!(processor.raw.bypass.is_some());
    assert!(processor.raw.warm.is_some());
    assert!(program_ring.raw.render_f32.is_some());
    assert!(program_ring.raw.warm.is_some());
    assert!(ProcessorPort::default().raw.context.is_null());
    assert!(ProgramRingPort::default().raw.context.is_null());

    let mut ports = SessionPorts::default();
    ports.receive_ctcss_notch[5] = processor;
    ports.transmit_dcs_normal_filter = normal_dcs;
    ports.transmit_dcs_turnoff_filter = turnoff_dcs;
    ports.program_ring = program_ring;
    let raw = ports.as_raw();
    assert!(raw.receive_ctcss_notch[5].process_f32.is_some());
    assert!(raw.receive_ctcss_notch[4].process_f32.is_none());
    assert_eq!(
        raw.transmit_dcs_normal_filter.context,
        normal_dcs_context.as_ptr()
    );
    assert_eq!(
        raw.transmit_dcs_turnoff_filter.context,
        turnoff_dcs_context.as_ptr()
    );
    assert!(raw.program_ring.render_f32.is_some());
}

#[test]
fn internal_observation_validation_accepts_every_valid_variant_and_rejects_bad_data() {
    assert_eq!(bool_from_raw(0), Ok(false));
    assert_eq!(bool_from_raw(1), Ok(true));
    assert_eq!(bool_from_raw(2), Err(RadioError::AdapterFailure));
    assert_eq!(ctcss_from_raw(-1), Ok(None));
    assert_eq!(ctcss_from_raw(0), Ok(CtcssToneIndex::new(0)));
    assert_eq!(ctcss_from_raw(-2), Ok(None));
    assert_eq!(
        ctcss_from_raw(CTCSS_TONE_COUNT as i32),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(ctcss_from_raw(256), Err(RadioError::AdapterFailure));

    for (raw, expected) in [
        (0, Ok(TransmitterState::Idle)),
        (1, Ok(TransmitterState::Active)),
        (2, Ok(TransmitterState::ToneOff)),
        (4, Ok(TransmitterState::Finishing)),
        (5, Ok(TransmitterState::Complete)),
        (3, Err(RadioError::AdapterFailure)),
    ] {
        assert_eq!(transmitter_state(raw), expected);
    }

    let receive_raw = RawReceiveResult {
        generation_id: 10,
        frame_count: 2,
        carrier_active: 1,
        subaudible_active: 0,
        receiver_keyed: 1,
        ctcss_decoded_index: -1,
        dcs_valid: 0,
        rssi_updated: 1,
        periodic_status_due: 0,
        ..RawReceiveResult::default()
    };
    let receive = receive_result(receive_raw, 10, 2).unwrap();
    assert!(receive.carrier_active);
    assert_eq!(receive.ctcss_decoded, None);
    assert_eq!(receive.ctcss_decoder_peak, 0.0);
    let mut malformed_receive = receive_raw;
    malformed_receive.generation_id = 11;
    assert_eq!(
        receive_result(malformed_receive, 10, 2),
        Err(RadioError::AdapterFailure)
    );
    malformed_receive = receive_raw;
    malformed_receive.frame_count = 3;
    assert_eq!(
        receive_result(malformed_receive, 10, 2),
        Err(RadioError::AdapterFailure)
    );
    malformed_receive = receive_raw;
    malformed_receive.output_rms = f32::NAN;
    assert_eq!(
        receive_result(malformed_receive, 10, 2),
        Err(RadioError::AdapterFailure)
    );

    let ring = RingObservation {
        ratio: 1.0,
        ..RingObservation::default()
    };
    assert!(ring_is_valid(ring));
    assert!(!ring_is_valid(RingObservation {
        ratio: f64::NAN,
        ..ring
    }));
    let transmit_raw = RawTransmitResult {
        generation_id: 10,
        frame_count: 2,
        logical_ptt: 1,
        transmitter_state: 5,
        periodic_status_due: 0,
        program_ring: ring,
        ..RawTransmitResult::default()
    };
    assert_eq!(
        transmit_result(transmit_raw, 10, 2)
            .unwrap()
            .transmitter_state,
        TransmitterState::Complete
    );
    let mut malformed_transmit = transmit_raw;
    malformed_transmit.generation_id = 11;
    assert_eq!(
        transmit_result(malformed_transmit, 10, 2),
        Err(RadioError::AdapterFailure)
    );
    malformed_transmit = transmit_raw;
    malformed_transmit.frame_count = 3;
    assert_eq!(
        transmit_result(malformed_transmit, 10, 2),
        Err(RadioError::AdapterFailure)
    );
    malformed_transmit = transmit_raw;
    malformed_transmit.output_peak = f32::NAN;
    assert_eq!(
        transmit_result(malformed_transmit, 10, 2),
        Err(RadioError::AdapterFailure)
    );
    malformed_transmit = transmit_raw;
    malformed_transmit.program_ring.ratio = f64::NAN;
    assert_eq!(
        transmit_result(malformed_transmit, 10, 2),
        Err(RadioError::AdapterFailure)
    );

    let snapshot_raw = RawSnapshot {
        generation_id: 10,
        ctcss_decoded_index: -1,
        program_ring: ring,
        ..RawSnapshot::default()
    };
    let snapshot = session_snapshot(snapshot_raw, 10).unwrap();
    assert_eq!(snapshot.ctcss_decoded, None);
    assert_eq!(snapshot.receive_ctcss_decoder_peak, 0.0);
    let mut malformed_snapshot = snapshot_raw;
    malformed_snapshot.generation_id = 11;
    assert_eq!(
        session_snapshot(malformed_snapshot, 10),
        Err(RadioError::AdapterFailure)
    );
    malformed_snapshot = snapshot_raw;
    malformed_snapshot.transmit_output_rms = f32::NAN;
    assert_eq!(
        session_snapshot(malformed_snapshot, 10),
        Err(RadioError::AdapterFailure)
    );
    malformed_snapshot = snapshot_raw;
    malformed_snapshot.program_ring.ratio = f64::NAN;
    assert_eq!(
        session_snapshot(malformed_snapshot, 10),
        Err(RadioError::AdapterFailure)
    );
}

#[test]
fn event_and_status_mapping_is_exhaustive() {
    let expected = [
        (EventOwner::Receive, 1, 1, EventValue::Carrier(true)),
        (EventOwner::Receive, 2, 0, EventValue::Subaudible(false)),
        (EventOwner::Receive, 3, 1, EventValue::ReceiverKeyed(true)),
        (EventOwner::Receive, 4, -1, EventValue::CtcssDecode(None)),
        (EventOwner::Receive, 5, 1, EventValue::DcsDecode(true)),
        (EventOwner::Transmit, 6, 0, EventValue::Ptt(false)),
        (
            EventOwner::Transmit,
            7,
            1_000,
            EventValue::CtcssTransmit(1_000),
        ),
        (
            EventOwner::Transmit,
            8,
            25,
            EventValue::ReceiverBlanking(25),
        ),
        (EventOwner::Receive, 9, 1, EventValue::ProviderFailure),
        (EventOwner::Transmit, 9, 1, EventValue::ProviderFailure),
    ];
    for (owner, kind, value, expected_value) in expected {
        let parsed = event(
            RawEvent {
                generation_id: 4,
                sample_index: 9,
                kind,
                value,
            },
            4,
            owner,
        )
        .unwrap();
        assert_eq!(parsed.value, expected_value);
    }

    for (owner, kind, value, generation_id) in [
        (EventOwner::Receive, 1, 1, 5),
        (EventOwner::Transmit, 1, 1, 4),
        (EventOwner::Receive, 6, 1, 4),
        (EventOwner::Receive, 1, 2, 4),
        (EventOwner::Receive, 1, -1, 4),
        (EventOwner::Receive, 4, 38, 4),
        (EventOwner::Transmit, 8, -1, 4),
        (EventOwner::Receive, 9, 0, 4),
        (EventOwner::Transmit, 9, 0, 4),
    ] {
        assert_eq!(
            event(
                RawEvent {
                    generation_id,
                    sample_index: 0,
                    kind,
                    value,
                },
                4,
                owner,
            ),
            Err(RadioError::AdapterFailure)
        );
    }

    assert_eq!(map_result(RESULT_OK), Ok(()));
    for (status, expected) in [
        (RESULT_INVALID_ARGUMENT, RadioError::InvalidArgument),
        (RESULT_PROVIDER_FAILED, RadioError::ProviderFailed),
        (RESULT_FRAME_COUNT_EXCEEDED, RadioError::FrameCountExceeded),
        (RESULT_UNSUPPORTED, RadioError::Unsupported),
        (RESULT_NOT_READY, RadioError::NotReady),
        (RESULT_BUSY, RadioError::Busy),
        (-99, RadioError::AdapterFailure),
    ] {
        assert_eq!(map_result(status), Err(expected));
        assert_eq!(error_from_result(status), expected);
    }
}

#[test]
fn all_local_frame_and_configuration_guards_are_exercised() {
    assert_eq!(bounded_frame_count(2, 2), Ok(2));
    assert_eq!(
        bounded_frame_count(3, 2),
        Err(RadioError::FrameCountExceeded)
    );
    assert_eq!(
        bounded_frame_count(u32::MAX as usize + 1, u32::MAX),
        Err(RadioError::FrameCountExceeded)
    );
    assert_eq!(
        receive_frame_count(&[], 0, 1),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(
        receive_frame_count(&[0.0], 0, 1),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(
        receive_frame_count(&[0.0, 0.0], 0, 1),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(
        receive_frame_count(&[f32::NAN, 0.0], 1, 1),
        Err(RadioError::InvalidArgument)
    );
    assert_eq!(receive_frame_count(&[0.0, 0.0], 1, 1), Ok(1));
    assert_eq!(transmit_frame_count(0, 1), Err(RadioError::InvalidArgument));
    assert_eq!(transmit_frame_count(1, 1), Err(RadioError::InvalidArgument));
    assert_eq!(transmit_frame_count(2, 1), Ok(1));

    let valid = SessionConfig::new(1, 1, 1);
    assert_eq!(validate_config(&valid), Ok(()));
    let mut invalid = valid;
    invalid.maximum_receive_frame_count = 0;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.maximum_transmit_frame_count = 0;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.publication_interval_milliseconds = 0;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.receive_input_gain = f32::NAN;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.receive.ctcss_decoder_gain = f32::NAN;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));

    let valid_ctcss = TransmitSignaling::Ctcss(CtcssTransmitConfig::default());
    invalid = valid;
    invalid.transmit.signaling = valid_ctcss;
    let TransmitSignaling::Ctcss(ref mut ctcss) = invalid.transmit.signaling else {
        unreachable!();
    };
    ctcss.peak = f32::NAN;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.transmit.signaling = valid_ctcss;
    let TransmitSignaling::Ctcss(ref mut ctcss) = invalid.transmit.signaling else {
        unreachable!();
    };
    ctcss.turnoff_phase_shift_degrees = f64::NAN;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.transmit.signaling = valid_ctcss;
    let TransmitSignaling::Ctcss(ref mut ctcss) = invalid.transmit.signaling else {
        unreachable!();
    };
    ctcss.turnoff_tail_tone_hz = f64::NAN;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    invalid = valid;
    invalid.transmit.signaling = valid_ctcss;
    let TransmitSignaling::Ctcss(ref mut ctcss) = invalid.transmit.signaling else {
        unreachable!();
    };
    ctcss.mapped_frequencies_tenths_hz[37] = -1;
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));

    invalid = valid;
    invalid.transmit.signaling = TransmitSignaling::Dcs(DcsTransmitConfig {
        peak: f32::NAN,
        ..DcsTransmitConfig::default()
    });
    assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    for selector in 0..4 {
        invalid = valid;
        match selector {
            0 => invalid.transmit.output_a.tone_gain = f32::NAN,
            1 => invalid.transmit.output_a.tone_bias = f32::NAN,
            2 => invalid.transmit.output_b.tone_gain = f32::NAN,
            3 => invalid.transmit.output_b.tone_bias = f32::NAN,
            _ => unreachable!(),
        }
        assert_eq!(validate_config(&invalid), Err(RadioError::InvalidArgument));
    }
}

#[test]
fn dcs_configuration_and_remaining_adapter_failure_paths_are_typed() {
    let mut config = SessionConfig::new(1, 2, 2);
    config.receive.signaling = ReceiveSignaling::Dcs(DcsReceiveConfig {
        code: 245,
        inverted: true,
    });
    config.transmit.signaling = TransmitSignaling::Dcs(DcsTransmitConfig {
        code: 431,
        inverted: true,
        peak: 0.25,
        turnoff_enabled: true,
        turnoff_duration_milliseconds: 125,
    });
    let raw = config.as_raw();
    assert_eq!(raw.receive.ctcss_enabled, 0);
    assert_eq!(raw.receive.dcs_enabled, 1);
    assert_eq!(raw.receive.dcs_code, 245);
    assert_eq!(raw.receive.dcs_inverted, 1);
    assert_eq!(raw.transmit.ctcss_transmit_enabled, 0);
    assert_eq!(raw.transmit.dcs_transmit_enabled, 1);
    assert_eq!(raw.transmit.dcs_code, 431);
    assert_eq!(raw.transmit.dcs_inverted, 1);

    // SAFETY: address one is intentionally misaligned and validation rejects
    // it before dereferencing it.
    let misaligned =
        unsafe { RadioProvider::from_raw_descriptor(ptr::without_provenance::<c_void>(1)) };
    assert_eq!(misaligned.err(), Some(RadioError::IncompatibleAdapter));

    let nonfinite = FakeLifecycle::default();
    let (mut receive, mut transmit, observer) = prepare(NONFINITE_OUTPUT_GENERATION, &nonfinite, 2)
        .unwrap()
        .split();
    let mut receive_output = [1.0; 2];
    assert_eq!(
        receive.process(&[0.0; 4], &mut receive_output, ReceiveControls::default()),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(receive_output, [0.0; 2]);
    let mut transmit_output = [1.0; 4];
    assert_eq!(
        transmit.render(&mut transmit_output, TransmitControls::default()),
        Err(RadioError::AdapterFailure)
    );
    assert_eq!(transmit_output, [0.0; 4]);
    drop((receive, transmit, observer));

    let snapshot_failure = FakeLifecycle::default();
    let (receive, transmit, observer) = prepare(SNAPSHOT_FAILURE_GENERATION, &snapshot_failure, 2)
        .unwrap()
        .split();
    assert_eq!(observer.snapshot(), Err(RadioError::Unsupported));
    drop((receive, transmit, observer));

    let invalid_pop = FakeLifecycle::default();
    let (receive, transmit, mut observer) = prepare(INVALID_POP_COUNT_GENERATION, &invalid_pop, 2)
        .unwrap()
        .split();
    assert_eq!(
        observer.pop_receive_event(),
        Err(RadioError::AdapterFailure)
    );
    drop((receive, transmit, observer));
}
