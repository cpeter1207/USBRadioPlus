use super::*;

use std::error::Error;
use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};

use usbradioplus_asl3::{
    ADVANCED_FRAME_SAMPLES, APP_RPT_FRAME_SAMPLES, AdapterError, AsteriskPcmMode,
    ControllerPcmFrame, ConversionError, ConversionProgress, ProgramSource, ReceiveMetadata,
};
use usbradioplus_radio::{ReceiveControls, RingObservation, SessionSnapshot, TransmitControls};

use crate::control::tests::{factory, resolved};
use crate::program::tests::provider as ring_provider;

const RADIO_OK: c_int = 0;
const RADIO_FAILED: c_int = -1;
const FAILING_GENERATION: u64 = 99;
pub(crate) const RECEIVE_FAILING_GENERATION: u64 = 97;
pub(crate) const TRANSMIT_FAILING_GENERATION: u64 = 98;
const ABI_VERSION: u32 = 4;
const CHANNELS: usize = 2;
static DESTROYED_SEVEN: AtomicUsize = AtomicUsize::new(0);
static DESTROYED_FAILURE: AtomicUsize = AtomicUsize::new(0);

#[repr(C)]
#[derive(Clone, Copy)]
struct FakeConfigPrefix {
    struct_size: u32,
    abi_version: u32,
    generation_id: u64,
    native_sample_rate_hz: u32,
    interleaved_channels: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FakeReceiveResult {
    generation_id: u64,
    first_sample_index: u64,
    frame_count: u32,
    carrier_active: u32,
    subaudible_active: u32,
    receiver_keyed: u32,
    ctcss_decoded_index: i32,
    dcs_valid: u32,
    rssi_peak: i16,
    rssi_updated: u32,
    ctcss_decoder_peak: f32,
    input_peak: f32,
    input_rms: f32,
    input_rail_samples: u64,
    output_peak: f32,
    output_rms: f32,
    output_rail_samples: u64,
    periodic_status_due: u32,
}

#[repr(C)]
struct FakeReceiveControls {
    hardware_carrier: u32,
    parallel_carrier: u32,
    hardware_subaudible: u32,
    parallel_subaudible: u32,
    subaudible_override: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FakeTransmitResult {
    generation_id: u64,
    first_sample_index: u64,
    frame_count: u32,
    logical_ptt: u32,
    transmitter_state: i32,
    selected_ctcss_tenths_hz: i32,
    program_peak: f32,
    program_rms: f32,
    program_rail_samples: u64,
    output_peak: f32,
    output_rms: f32,
    output_rail_samples: u64,
    periodic_status_due: u32,
    program_ring: RingObservation,
}

#[repr(C)]
struct FakeTransmitControls {
    external_ptt_request: u32,
    physical_ptt_applied: u32,
    render_admitted: u32,
    ctcss_inhibit: u32,
    calibrated_test_tone: u32,
    forced_ctcss_tenths_hz: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FakeSnapshot {
    generation_id: u64,
    receive_frames: u64,
    transmit_frames: u64,
    receive_input_peak: f32,
    receive_input_rms: f32,
    receive_ctcss_decoder_peak: f32,
    receive_output_peak: f32,
    receive_output_rms: f32,
    transmit_program_peak: f32,
    transmit_program_rms: f32,
    transmit_output_peak: f32,
    transmit_output_rms: f32,
    receive_input_rail_samples: u64,
    receive_output_rail_samples: u64,
    transmit_program_rail_samples: u64,
    transmit_output_rail_samples: u64,
    provider_failures: u64,
    receive_event_drops: u64,
    transmit_event_drops: u64,
    carrier_active: u32,
    subaudible_active: u32,
    receiver_keyed: u32,
    logical_ptt: u32,
    ctcss_decoded_index: i32,
    dcs_valid: u32,
    program_ring: RingObservation,
}

type Create =
    unsafe extern "C" fn(*const FakeConfigPrefix, *const c_void, *mut *mut c_void) -> c_int;
type Warm = unsafe extern "C" fn(*mut c_void) -> c_int;
type Receive = unsafe extern "C" fn(
    *mut c_void,
    *const f32,
    *mut f32,
    u32,
    *const c_void,
    *mut FakeReceiveResult,
) -> c_int;
type Transmit = unsafe extern "C" fn(
    *mut c_void,
    *mut f32,
    u32,
    *const c_void,
    *mut FakeTransmitResult,
) -> c_int;
type Snapshot = unsafe extern "C" fn(*const c_void, *mut FakeSnapshot) -> c_int;
type Pop = unsafe extern "C" fn(*const c_void, *mut c_void) -> u32;
type Destroy = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
struct FakeDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<Create>,
    warm: Option<Warm>,
    receive: Option<Receive>,
    transmit: Option<Transmit>,
    snapshot: Option<Snapshot>,
    pop_receive: Option<Pop>,
    pop_transmit: Option<Pop>,
    destroy: Option<Destroy>,
}

// SAFETY: the immutable descriptor contains only process-lifetime functions.
unsafe impl Sync for FakeDescriptor {}

struct FakeSession {
    generation_id: u64,
    maximum_receive: u32,
    maximum_transmit: u32,
    receive_frames: u64,
    transmit_frames: u64,
}

unsafe extern "C" fn create(
    config: *const FakeConfigPrefix,
    _ports: *const c_void,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies a complete config prefix and handle destination.
    let (Some(config), Some(output)) = (unsafe { config.as_ref() }, unsafe { output.as_mut() })
    else {
        return RADIO_FAILED;
    };
    let session = Box::new(FakeSession {
        generation_id: config.generation_id,
        maximum_receive: config.maximum_receive_frame_count,
        maximum_transmit: config.maximum_transmit_frame_count,
        receive_frames: 0,
        transmit_frames: 0,
    });
    *output = Box::into_raw(session).cast();
    if config.generation_id == FAILING_GENERATION {
        RADIO_FAILED
    } else {
        RADIO_OK
    }
}

unsafe extern "C" fn warm(session: *mut c_void) -> c_int {
    if session.is_null() {
        RADIO_FAILED
    } else {
        RADIO_OK
    }
}

unsafe extern "C" fn receive(
    session: *mut c_void,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
    controls: *const c_void,
    result: *mut FakeReceiveResult,
) -> c_int {
    // SAFETY: the wrapper supplies this fake's live handle.
    let session = unsafe { session.cast::<FakeSession>().as_mut() };
    // SAFETY: the wrapper supplies this fake's result destination.
    let result = unsafe { result.as_mut() };
    let (Some(session), Some(result)) = (session, result) else {
        return RADIO_FAILED;
    };
    if input.is_null()
        || output.is_null()
        || controls.is_null()
        || frame_count > session.maximum_receive
        || session.generation_id == RECEIVE_FAILING_GENERATION
    {
        return RADIO_FAILED;
    }
    // SAFETY: the wrapper supplies the receive-control ABI object.
    let controls = unsafe { &*controls.cast::<FakeReceiveControls>() };
    // SAFETY: the wrapper supplies stereo input and mono output for frame_count.
    let (input, output) = unsafe {
        (
            std::slice::from_raw_parts(input, frame_count as usize * CHANNELS),
            std::slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    for (frame, sample) in output.iter_mut().enumerate() {
        *sample = input[frame * CHANNELS];
    }
    *result = FakeReceiveResult {
        generation_id: session.generation_id,
        first_sample_index: session.receive_frames,
        frame_count,
        carrier_active: controls.hardware_carrier,
        subaudible_active: controls.hardware_subaudible,
        receiver_keyed: controls.hardware_carrier & controls.hardware_subaudible,
        ctcss_decoded_index: if controls.hardware_subaudible != 0 {
            0
        } else {
            -1
        },
        rssi_peak: 16_384,
        rssi_updated: controls.hardware_carrier,
        input_peak: 0.5,
        input_rms: 0.25,
        output_peak: 0.5,
        output_rms: 0.25,
        ..FakeReceiveResult::default()
    };
    session.receive_frames += u64::from(frame_count);
    RADIO_OK
}

unsafe extern "C" fn transmit(
    session: *mut c_void,
    output: *mut f32,
    frame_count: u32,
    controls: *const c_void,
    result: *mut FakeTransmitResult,
) -> c_int {
    // SAFETY: the wrapper supplies this fake's live handle.
    let session = unsafe { session.cast::<FakeSession>().as_mut() };
    // SAFETY: the wrapper supplies this fake's result destination.
    let result = unsafe { result.as_mut() };
    let (Some(session), Some(result)) = (session, result) else {
        return RADIO_FAILED;
    };
    if output.is_null()
        || controls.is_null()
        || frame_count > session.maximum_transmit
        || session.generation_id == TRANSMIT_FAILING_GENERATION
    {
        return RADIO_FAILED;
    }
    // SAFETY: the wrapper supplies the transmit-control ABI object.
    let controls = unsafe { &*controls.cast::<FakeTransmitControls>() };
    // SAFETY: the wrapper supplies stereo output for frame_count.
    unsafe {
        std::slice::from_raw_parts_mut(output, frame_count as usize * CHANNELS).fill(0.25);
    }
    *result = FakeTransmitResult {
        generation_id: session.generation_id,
        first_sample_index: session.transmit_frames,
        frame_count,
        logical_ptt: controls.external_ptt_request,
        transmitter_state: 1,
        selected_ctcss_tenths_hz: if controls.external_ptt_request != 0 {
            1_000
        } else {
            -1
        },
        program_peak: 0.25,
        program_rms: 0.25,
        output_peak: 0.25,
        output_rms: 0.25,
        program_ring: RingObservation {
            ratio: 1.0,
            ..RingObservation::default()
        },
        ..FakeTransmitResult::default()
    };
    session.transmit_frames += u64::from(frame_count);
    RADIO_OK
}

unsafe extern "C" fn snapshot(session: *const c_void, output: *mut FakeSnapshot) -> c_int {
    // SAFETY: the wrapper supplies this fake's live handle.
    let session = unsafe { session.cast::<FakeSession>().as_ref() };
    // SAFETY: the wrapper supplies this fake's snapshot destination.
    let output = unsafe { output.as_mut() };
    let (Some(session), Some(output)) = (session, output) else {
        return RADIO_FAILED;
    };
    *output = FakeSnapshot {
        generation_id: session.generation_id,
        receive_frames: session.receive_frames,
        transmit_frames: session.transmit_frames,
        ctcss_decoded_index: -1,
        program_ring: RingObservation {
            ratio: 1.0,
            ..RingObservation::default()
        },
        ..FakeSnapshot::default()
    };
    RADIO_OK
}

unsafe extern "C" fn pop(_session: *const c_void, _output: *mut c_void) -> u32 {
    0
}

unsafe extern "C" fn destroy(session: *mut c_void) {
    if !session.is_null() {
        // SAFETY: create transfers each allocation here exactly once.
        let session = unsafe { Box::from_raw(session.cast::<FakeSession>()) };
        match session.generation_id {
            7 => &DESTROYED_SEVEN,
            FAILING_GENERATION => &DESTROYED_FAILURE,
            _ => return,
        }
        .fetch_add(1, Ordering::Relaxed);
    }
}

static DESCRIPTOR: FakeDescriptor = FakeDescriptor {
    struct_size: size_of::<FakeDescriptor>() as u32,
    abi_version: ABI_VERSION,
    capability_name: c"rptadv.radio-core".as_ptr(),
    create: Some(create),
    warm: Some(warm),
    receive: Some(receive),
    transmit: Some(transmit),
    snapshot: Some(snapshot),
    pop_receive: Some(pop),
    pop_transmit: Some(pop),
    destroy: Some(destroy),
};

pub(crate) fn radio_provider() -> RadioProvider {
    // SAFETY: the immutable descriptor and functions have process lifetime.
    unsafe { RadioProvider::from_raw_descriptor(ptr::from_ref(&DESCRIPTOR).cast()) }.unwrap()
}

struct Downsampler;

impl AppRptConverter for Downsampler {
    fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
    ) -> Result<ConversionProgress, ConversionError> {
        let generated = (input.len() / 6).min(output.len());
        for (target, source) in output
            .iter_mut()
            .zip(input.iter().step_by(6))
            .take(generated)
        {
            *target = *source;
        }
        Ok(ConversionProgress {
            input_used: generated * 6,
            output_generated: generated,
        })
    }
}

pub(crate) fn prepared(transport: ControllerTransport, generation_id: u64) -> PreparedStation {
    let (channel, configuration) = resolved("[usb]\n");
    StationPlan::new(channel, configuration, transport, generation_id, 960)
        .unwrap()
        .prepare(&factory())
        .unwrap()
}

fn require_send<T: Send>() {}

#[test]
fn advanced_binding_exposes_all_serial_owners_and_keeps_resources_alive() {
    require_send::<StationMedia>();
    require_send::<StationReceive>();
    require_send::<StationTransmit>();
    require_send::<StationControl>();
    DESTROYED_SEVEN.store(0, Ordering::Relaxed);
    let media = prepared(ControllerTransport::RptAdvanced, 7)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 4 },
        )
        .unwrap();
    assert_eq!(media.plan.transport(), ControllerTransport::RptAdvanced);
    let StationMedia {
        plan,
        mut receive,
        mut transmit,
        mut control,
        mut controller,
    } = media;
    assert_eq!(plan.channel(), "usb");
    let frame = ControllerPcmFrame::advanced([1_000; ADVANCED_FRAME_SAMPLES]);
    let written = controller.write_program(&frame).unwrap();
    assert_eq!(written.source, ProgramSource::Controller);
    assert_eq!(written.accepted_samples, ADVANCED_FRAME_SAMPLES);

    let input = [0.5, -0.5, 0.25, -0.25];
    let mut mono = [0.0; 2];
    let received = receive
        .radio()
        .process(&input, &mut mono, ReceiveControls::default())
        .unwrap();
    assert_eq!(mono, [0.5, 0.25]);
    assert_eq!(received.frame_count, 2);
    assert!(
        !receive
            .controller()
            .publish(&mono, ReceiveMetadata::default())
            .unwrap()
            .completed_frame
    );

    let mut output = [0.0; 4];
    transmit
        .radio()
        .render(&mut output, TransmitControls::default())
        .unwrap();
    assert_eq!(output, [0.25; 4]);
    let observation: SessionSnapshot = control.radio().snapshot().unwrap();
    assert_eq!(observation.receive_frames, 2);
    assert_eq!(observation.transmit_frames, 2);

    drop(receive);
    drop(transmit);
    assert_eq!(DESTROYED_SEVEN.load(Ordering::Relaxed), 0);
    drop(control);
    assert_eq!(DESTROYED_SEVEN.load(Ordering::Relaxed), 1);
}

#[test]
fn app_rpt_binding_uses_the_supplied_converter_and_echo_policy() {
    let media = prepared(ControllerTransport::AppRpt, 8)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(Downsampler),
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .unwrap();
    let mut parts = media;
    let input = vec![0.25; ADVANCED_FRAME_SAMPLES * CHANNELS];
    let mut mono = [0.0; ADVANCED_FRAME_SAMPLES];
    parts
        .receive
        .radio()
        .process(&input, &mut mono, ReceiveControls::default())
        .unwrap();
    let report = parts
        .receive
        .controller()
        .publish(&mono, ReceiveMetadata::default())
        .unwrap();
    assert!(report.completed_frame);
    let mut delivered = ControllerPcmFrame::silence(AsteriskPcmMode::AppRpt);
    assert!(parts.controller.next_action(&mut delivered).is_some());
    assert_eq!(delivered.samples().len(), APP_RPT_FRAME_SAMPLES);
}

#[test]
fn binding_rejects_mismatch_controller_setup_and_provider_failures() {
    let mismatch = prepared(ControllerTransport::RptAdvanced, 10)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(Downsampler),
                handoff_slots: 2,
                echo: EchoConfiguration::default(),
            },
        )
        .err()
        .unwrap();
    assert!(matches!(
        mismatch,
        StationMediaError::ControllerTransportMismatch
    ));
    assert!(mismatch.source().is_none());
    assert_eq!(
        mismatch.to_string(),
        "controller setup does not match the station transport"
    );

    let handoff = prepared(ControllerTransport::RptAdvanced, 11)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 1 },
        )
        .err()
        .unwrap();
    assert!(matches!(handoff, StationMediaError::Controller(_)));
    assert!(handoff.source().is_some());

    DESTROYED_FAILURE.store(0, Ordering::Relaxed);
    let radio = prepared(ControllerTransport::RptAdvanced, FAILING_GENERATION)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::RptAdvanced { handoff_slots: 2 },
        )
        .err()
        .unwrap();
    assert!(matches!(radio, StationMediaError::Radio(_)));
    assert!(radio.to_string().starts_with("radio session setup failed:"));
    assert!(radio.source().is_some());
    assert_eq!(DESTROYED_FAILURE.load(Ordering::Relaxed), 1);

    let ring = StationMediaError::from(ProgramRingSetupError(
        usbradioplus_ring::RingError::NoMemory,
    ));
    assert!(matches!(ring, StationMediaError::ProgramRing(_)));
    assert!(ring.source().is_some());
    assert!(
        ring.to_string()
            .contains("unable to prepare transmitter program ring")
    );

    let controller = StationMediaError::from(usbradioplus_asl3::AdapterSetupError::Handoff(
        usbradioplus_asl3::HandoffError::InvalidSlotCount,
    ));
    assert!(matches!(controller, StationMediaError::Controller(_)));
    assert!(controller.source().is_some());
    assert_eq!(
        controller.to_string(),
        "invalid callback handoff slot count"
    );

    let wrong_frame = ControllerPcmFrame::advanced([0; ADVANCED_FRAME_SAMPLES]);
    let mut app = prepared(ControllerTransport::AppRpt, 12)
        .bind_media(
            ring_provider(),
            radio_provider(),
            ControllerSetup::AppRpt {
                converter: Box::new(Downsampler),
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .unwrap();
    assert_eq!(
        app.controller.write_program(&wrong_frame),
        Err(AdapterError::WrongInterface)
    );
}
