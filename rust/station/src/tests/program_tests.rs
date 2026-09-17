use super::*;
use std::cell::Cell;
use std::collections::VecDeque;
use std::ffi::{c_char, c_void};
use std::mem::size_of;
use std::ptr;

const OK: c_int = 0;
const INVALID: c_int = -1;

thread_local! {
    static OBSERVE_FAILS: Cell<bool> = const { Cell::new(false) };
}

#[repr(C)]
struct FakeConfig {
    struct_size: u32,
    abi_version: u32,
    capacity_samples: u64,
    input_rate_hz: u32,
    output_rate_hz: u32,
    quality: u32,
}

#[repr(C)]
#[derive(Default)]
struct FakeObservation {
    struct_size: u32,
    abi_version: u32,
    capacity_samples: u64,
    available_samples: u64,
    reserve_samples: u64,
    filtered_occupancy_samples: u64,
    target_samples: u64,
    ratio_correction_ppm: i32,
    reserved: u32,
    discarded_samples: u64,
    missing_samples: u64,
    consecutive_shortfall_samples: u64,
    shortfall_average_milli: u64,
    adapter_error_count: u64,
}

type Create = unsafe extern "C" fn(*const FakeConfig, *mut *mut c_void) -> c_int;
type Destroy = unsafe extern "C" fn(*mut c_void);
type PushSample = unsafe extern "C" fn(*mut c_void, f32, *mut bool) -> c_int;
type Push = unsafe extern "C" fn(*mut c_void, *const f32, u64, *mut u64) -> c_int;
type RenderSample = unsafe extern "C" fn(*mut c_void, *mut f32, u64, *mut bool) -> c_int;
type Render = unsafe extern "C" fn(*mut c_void, *mut f32, u64, u64, u64, *mut u64) -> c_int;
type Observe = unsafe extern "C" fn(*const c_void, *mut FakeObservation) -> c_int;

#[repr(C)]
struct FakeDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<Create>,
    destroy: Option<Destroy>,
    push_sample: Option<PushSample>,
    push: Option<Push>,
    render_sample: Option<RenderSample>,
    render: Option<Render>,
    observe: Option<Observe>,
}

// SAFETY: The immutable descriptor contains only process-lifetime function pointers.
unsafe impl Sync for FakeDescriptor {}

struct FakeRing {
    capacity: usize,
    samples: VecDeque<f32>,
    reserve: u64,
    target: u64,
    discarded: u64,
    missing: u64,
}

unsafe extern "C" fn fake_create(config: *const FakeConfig, output: *mut *mut c_void) -> c_int {
    // SAFETY: The test client supplies complete synchronous ABI arguments.
    let (Some(config), Some(output)) = (unsafe { config.as_ref() }, unsafe { output.as_mut() })
    else {
        return INVALID;
    };
    let ring = Box::new(FakeRing {
        capacity: config.capacity_samples as usize,
        samples: VecDeque::with_capacity(config.capacity_samples as usize),
        reserve: 0,
        target: 0,
        discarded: 0,
        missing: 0,
    });
    *output = Box::into_raw(ring).cast();
    OK
}

unsafe extern "C" fn fake_destroy(ring: *mut c_void) {
    if !ring.is_null() {
        // SAFETY: fake_create allocated this exact handle and transfers it once.
        drop(unsafe { Box::from_raw(ring.cast::<FakeRing>()) });
    }
}

unsafe extern "C" fn fake_push_sample(
    ring: *mut c_void,
    sample: f32,
    accepted: *mut bool,
) -> c_int {
    // SAFETY: The test client supplies the live fake handle and result pointer.
    let (Some(ring), Some(accepted)) = (unsafe { ring.cast::<FakeRing>().as_mut() }, unsafe {
        accepted.as_mut()
    }) else {
        return INVALID;
    };
    *accepted = ring.samples.len() < ring.capacity;
    if *accepted {
        ring.samples.push_back(sample);
    } else {
        ring.discarded += 1;
    }
    OK
}

unsafe extern "C" fn fake_push(
    ring: *mut c_void,
    input: *const f32,
    count: u64,
    accepted: *mut u64,
) -> c_int {
    // SAFETY: The test client supplies the live fake handle and result pointer.
    let (Some(ring), Some(accepted)) = (unsafe { ring.cast::<FakeRing>().as_mut() }, unsafe {
        accepted.as_mut()
    }) else {
        return INVALID;
    };
    // SAFETY: The test client supplies exactly count readable samples.
    let input = unsafe { std::slice::from_raw_parts(input, count as usize) };
    let available = ring.capacity.saturating_sub(ring.samples.len());
    let count = available.min(input.len());
    ring.samples.extend(&input[..count]);
    ring.discarded += (input.len() - count) as u64;
    *accepted = count as u64;
    OK
}

unsafe extern "C" fn fake_render_sample(
    ring: *mut c_void,
    output: *mut f32,
    target: u64,
    real: *mut bool,
) -> c_int {
    // SAFETY: The test client supplies one live handle.
    let ring = unsafe { ring.cast::<FakeRing>().as_mut() };
    // SAFETY: The test client supplies one writable sample.
    let output = unsafe { output.as_mut() };
    // SAFETY: The test client supplies one writable result.
    let real = unsafe { real.as_mut() };
    let (Some(ring), Some(output), Some(real)) = (ring, output, real) else {
        return INVALID;
    };
    ring.target = target;
    *real = ring.samples.front().is_some();
    *output = ring.samples.pop_front().unwrap_or_else(|| {
        ring.missing += 1;
        0.0
    });
    OK
}

unsafe extern "C" fn fake_render(
    ring: *mut c_void,
    output: *mut f32,
    count: u64,
    reserve: u64,
    target: u64,
    real: *mut u64,
) -> c_int {
    // SAFETY: The test client supplies the live fake handle and result pointer.
    let (Some(ring), Some(real)) = (unsafe { ring.cast::<FakeRing>().as_mut() }, unsafe {
        real.as_mut()
    }) else {
        return INVALID;
    };
    ring.reserve = reserve;
    ring.target = target;
    // SAFETY: The test client supplies exactly count writable samples.
    let output = unsafe { std::slice::from_raw_parts_mut(output, count as usize) };
    *real = 0;
    for sample in output {
        if let Some(value) = ring.samples.pop_front() {
            *sample = value;
            *real += 1;
        } else {
            *sample = 0.0;
            ring.missing += 1;
        }
    }
    OK
}

unsafe extern "C" fn fake_observe(ring: *const c_void, output: *mut FakeObservation) -> c_int {
    if OBSERVE_FAILS.get() {
        return INVALID;
    }
    // SAFETY: The test client supplies the live fake handle and snapshot pointer.
    let (Some(ring), Some(output)) = (unsafe { ring.cast::<FakeRing>().as_ref() }, unsafe {
        output.as_mut()
    }) else {
        return INVALID;
    };
    *output = FakeObservation {
        struct_size: size_of::<FakeObservation>() as u32,
        abi_version: 2,
        capacity_samples: ring.capacity as u64,
        available_samples: ring.samples.len() as u64,
        reserve_samples: ring.reserve,
        target_samples: ring.target,
        discarded_samples: ring.discarded,
        missing_samples: ring.missing,
        ..FakeObservation::default()
    };
    OK
}

static FAKE_DESCRIPTOR: FakeDescriptor = FakeDescriptor {
    struct_size: size_of::<FakeDescriptor>() as u32,
    abi_version: 2,
    capability_name: c"rptadv.rate-adjusting-pcm-ring.f32".as_ptr(),
    create: Some(fake_create),
    destroy: Some(fake_destroy),
    push_sample: Some(fake_push_sample),
    push: Some(fake_push),
    render_sample: Some(fake_render_sample),
    render: Some(fake_render),
    observe: Some(fake_observe),
};

pub(crate) fn provider() -> RingProvider {
    // SAFETY: FAKE_DESCRIPTOR and its capability string have process lifetime.
    unsafe { RingProvider::from_raw_descriptor(ptr::from_ref(&FAKE_DESCRIPTOR).cast()) }.unwrap()
}

#[test]
fn interface_plans_keep_the_same_real_time_policy() {
    let app_rpt = program_ring_plan(ControllerTransport::AppRpt);
    assert_eq!(app_rpt.input_rate_hz, 8_000);
    assert_eq!(app_rpt.output_rate_hz, 48_000);
    assert_eq!(app_rpt.reserve_samples, 160);
    assert_eq!(app_rpt.target_samples, 320);
    assert_eq!(app_rpt.capacity_samples, 640);

    let advanced = program_ring_plan(ControllerTransport::RptAdvanced);
    assert_eq!(advanced.input_rate_hz, 48_000);
    assert_eq!(advanced.output_rate_hz, 48_000);
    assert_eq!(advanced.reserve_samples, 960);
    assert_eq!(advanced.target_samples, 1_920);
    assert_eq!(advanced.capacity_samples, 3_840);
}

#[test]
fn qualification_packing_preserves_every_field_and_rejects_bad_tones() {
    let expected = ReceiveQualification {
        carrier_active: true,
        subaudible_active: true,
        receiver_keyed: true,
        ctcss_decoded: AslCtcssToneIndex::new(37),
        dcs_valid: true,
    };
    assert_eq!(unpack_qualification(pack_qualification(expected)), expected);
    assert_eq!(unpack_qualification(63 << TONE_SHIFT).ctcss_decoded, None);
}

#[test]
fn radio_observation_maps_units_ratio_and_large_counts() {
    let output = radio_observation(
        RingObservation {
            capacity_samples: u64::MAX,
            available_samples: 7,
            reserve_samples: 8,
            filtered_occupancy_samples: 9,
            target_samples: 10,
            ratio_correction_ppm: 125,
            discarded_samples: 11,
            missing_samples: 12,
            consecutive_shortfall_samples: 13,
            shortfall_average_milli: 14,
            adapter_error_count: 15,
        },
        6.0,
    );
    assert_eq!(output.capacity_frames, u32::MAX);
    assert_eq!(output.occupancy_frames, 7);
    assert_eq!(output.reserve_frames, 8);
    assert_eq!(output.target_frames, 10);
    assert_eq!(output.overrun_samples, 11);
    assert_eq!(output.underrun_samples, 12);
    assert_eq!(output.concealment_samples, 12);
    assert!((output.ratio - 6.000_75).abs() < f64::EPSILON);
}

#[test]
fn setup_error_preserves_the_ring_cause() {
    let error = ProgramRingSetupError(RingError::NoMemory);
    assert!(error.to_string().contains("unable to allocate PCM ring"));
    assert_eq!(error.0, RingError::NoMemory);
}

#[test]
fn prepared_owner_pair_moves_audio_and_latest_accepted_state() {
    let plan = program_ring_plan(ControllerTransport::RptAdvanced);
    let (mut producer, mut consumer) = prepare_program_ring(provider(), plan).unwrap();
    let qualification = ReceiveQualification {
        receiver_keyed: true,
        ctcss_decoded: AslCtcssToneIndex::new(5),
        dcs_valid: true,
        ..ReceiveQualification::default()
    };
    assert_eq!(producer.push(&[0.25, -0.5], qualification).unwrap(), 2);
    let mut output = [9.0; 3];
    let (observation, received) = consumer.render(&mut output).unwrap();
    assert_eq!(output, [0.25, -0.5, 0.0]);
    assert_eq!(received, qualification);
    assert_eq!(observation.reserve_samples, plan.reserve_samples);
    assert_eq!(observation.target_samples, plan.target_samples);
    assert_eq!(observation.missing_samples, 1);
}

#[test]
fn direct_program_uses_current_block_and_preserves_ring_and_receive_qualification() {
    let (mut producer, mut consumer) = prepare_program_ring(
        provider(),
        program_ring_plan(ControllerTransport::RptAdvanced),
    )
    .unwrap();
    producer
        .push(&[0.9, 0.8], ReceiveQualification::default())
        .unwrap();
    let source = consumer.direct_source();
    // SAFETY: this test serializes all accesses to the stopped direct source.
    unsafe {
        source.prepare(2);
    }
    let qualification = ReceiveQualification {
        receiver_keyed: true,
        ctcss_decoded: AslCtcssToneIndex::new(5),
        dcs_valid: true,
        ..ReceiveQualification::default()
    };
    consumer
        .qualification
        .store(pack_qualification(qualification), Ordering::Release);
    // SAFETY: no source render overlaps this staging call.
    unsafe {
        source.stage(&[0.125, -0.25]);
    }
    let mut output = [0.0; 2];
    OBSERVE_FAILS.set(true);
    let (_, actual) = consumer.render(&mut output).unwrap();
    OBSERVE_FAILS.set(false);
    assert_eq!(output, [0.125, -0.25]);
    assert_eq!(actual, qualification);
    // Direct rendering must not advance or even observe the rate-adjusting ring.
    assert_eq!(consumer.ring.observe().unwrap().available_samples, 2);
    // SAFETY: no source render overlaps this staging call.
    unsafe {
        source.stage(&[-0.5, 0.75]);
    }
    consumer.render(&mut output).unwrap();
    assert_eq!(output, [-0.5, 0.75]);
    assert_eq!(consumer.ring.observe().unwrap().available_samples, 2);
}

#[test]
fn direct_source_stages_while_radio_exclusively_owns_the_consumer() {
    let (_, mut consumer) = prepare_program_ring(
        provider(),
        program_ring_plan(ControllerTransport::RptAdvanced),
    )
    .unwrap();
    let source = consumer.direct_source();
    let prepared = crate::media::tests::prepared(ControllerTransport::RptAdvanced, 24);
    let ports = usbradioplus_radio::SessionPorts {
        program_ring: consumer.radio_port(),
        ..usbradioplus_radio::SessionPorts::default()
    };
    let (_, mut transmit, _) = crate::media::tests::radio_provider()
        .prepare(prepared.plan().radio(), ports)
        .unwrap()
        .split();
    // No further borrow of consumer is possible while transmit retains its port.
    // SAFETY: this stopped, single-threaded test serializes preparation, staging
    // and radio rendering; the separately owned source stays alive throughout.
    unsafe {
        source.prepare(4);
    }
    for block in [[0.1, 0.2, 0.3, 0.4], [-0.1, -0.2, -0.3, -0.4]] {
        // SAFETY: no radio render overlaps this source mutation.
        unsafe {
            source.stage(&block);
        }
        let mut output = [0.0; 8];
        transmit
            .render(
                &mut output,
                usbradioplus_radio::TransmitControls {
                    render_admitted: true,
                    ..usbradioplus_radio::TransmitControls::default()
                },
            )
            .unwrap();
        assert_eq!(
            output,
            [
                block[0], block[0], block[1], block[1], block[2], block[2], block[3], block[3]
            ]
        );
    }
}

#[test]
fn rejected_write_does_not_replace_the_latest_accepted_state() {
    let plan = ProgramRingPlan {
        capacity_samples: 512,
        input_rate_hz: 48_000,
        output_rate_hz: 48_000,
        reserve_samples: 1,
        target_samples: 2,
    };
    let (mut producer, consumer) = prepare_program_ring(provider(), plan).unwrap();
    let accepted = ReceiveQualification {
        carrier_active: true,
        ..ReceiveQualification::default()
    };
    assert_eq!(producer.push(&vec![0.0; 512], accepted).unwrap(), 512);
    assert_eq!(
        producer
            .push(
                &[1.0],
                ReceiveQualification {
                    dcs_valid: true,
                    ..ReceiveQualification::default()
                }
            )
            .unwrap(),
        0
    );
    assert_eq!(
        unpack_qualification(consumer.qualification.load(Ordering::Acquire)),
        accepted
    );
}

#[test]
fn radio_callback_validates_pointers_and_renders_the_exact_span() {
    // SAFETY: Null arguments intentionally exercise callback validation.
    let invalid = unsafe { render_program(ptr::null_mut(), ptr::null_mut(), 0, ptr::null_mut()) };
    assert_eq!(invalid, PROVIDER_FAILED);
    let plan = program_ring_plan(ControllerTransport::RptAdvanced);
    let (mut producer, mut consumer) = prepare_program_ring(provider(), plan).unwrap();
    producer
        .push(&[0.1, 0.2], ReceiveQualification::default())
        .unwrap();
    let mut output = [0.0; 2];
    let mut result = ProgramRingResult::default();
    // SAFETY: Every pointer names a live value of the exact callback type and size.
    let status = unsafe {
        render_program(
            ptr::from_mut(&mut consumer).cast(),
            output.as_mut_ptr(),
            output.len() as u32,
            &mut result,
        )
    };
    assert_eq!(status, PROVIDER_OK);
    assert_eq!(output, [0.1, 0.2]);
}

#[test]
fn radio_callback_reports_observation_failure_after_rendering() {
    let plan = program_ring_plan(ControllerTransport::RptAdvanced);
    let (mut producer, mut consumer) = prepare_program_ring(provider(), plan).unwrap();
    producer
        .push(&[0.1, 0.2], ReceiveQualification::default())
        .unwrap();
    let mut output = [1.0; 2];
    let mut result = ProgramRingResult::default();
    OBSERVE_FAILS.set(true);
    // SAFETY: every pointer names a live value of the exact callback type and size.
    let status = unsafe {
        render_program(
            ptr::from_mut(&mut consumer).cast(),
            output.as_mut_ptr(),
            output.len() as u32,
            &mut result,
        )
    };
    OBSERVE_FAILS.set(false);
    assert_eq!(status, PROVIDER_FAILED);
    assert_eq!(output, [0.1, 0.2]);
}
