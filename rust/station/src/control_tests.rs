use super::*;

use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;

use usbradioplus_core::{
    ChainRole, ChannelConfiguration, ConfigDocument, ResolvedChannelConfiguration,
};
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_rnnoise::DenoiseProvider;

#[repr(C)]
struct TestGraphConfig {
    struct_size: u32,
    abi_version: u32,
    sample_rate_hz: u32,
    maximum_frame_count: u32,
    filter_description: *const c_char,
}

type GraphCreate = unsafe extern "C" fn(*const TestGraphConfig, *mut *mut c_void) -> c_int;
type GraphStreamingProcess =
    unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, u32, *mut u32, *mut u32) -> c_int;

#[repr(C)]
struct TestGraphDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<GraphCreate>,
    process: Option<GraphStreamingProcess>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    process_block: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32) -> c_int>,
}

// SAFETY: This immutable test descriptor and its capability name are static.
unsafe impl Sync for TestGraphDescriptor {}

unsafe extern "C" fn graph_create(
    _config: *const TestGraphConfig,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    0
}

unsafe extern "C" fn graph_process(
    _state: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
) -> c_int {
    // SAFETY: The graph wrapper supplies distinct spans of frame_count samples.
    unsafe {
        ptr::copy_nonoverlapping(input, output, frame_count as usize);
    }
    0
}

unsafe extern "C" fn graph_destroy(state: *mut c_void) {
    if !state.is_null() {
        // SAFETY: Every non-null state was allocated once by graph_create.
        drop(unsafe { Box::from_raw(state.cast::<()>()) });
    }
}

static GRAPH_DESCRIPTOR: TestGraphDescriptor = TestGraphDescriptor {
    struct_size: size_of::<TestGraphDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.ffmpeg".as_ptr(),
    create: Some(graph_create),
    process: None,
    destroy: Some(graph_destroy),
    process_block: Some(graph_process),
};

#[repr(C)]
struct TestDenoiseDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    create: Option<unsafe extern "C" fn(u32, u32, u32, *mut *mut c_void) -> c_int>,
    process:
        Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, *mut f32, *mut f32) -> c_int>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: This immutable test descriptor and its capability name are static.
unsafe impl Sync for TestDenoiseDescriptor {}

unsafe extern "C" fn denoise_create(
    _sample_rate_hz: u32,
    _channels: u32,
    _frame_count: u32,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: The wrapper supplies one writable handle destination.
    unsafe { *output = Box::into_raw(Box::new(())).cast() };
    0
}

unsafe extern "C" fn denoise_process(
    _state: *mut c_void,
    input: *const f32,
    frame_count: u32,
    output: *mut f32,
    probability: *mut f32,
) -> c_int {
    // SAFETY: The wrapper supplies distinct spans and one writable probability.
    unsafe {
        ptr::copy_nonoverlapping(input, output, frame_count as usize);
        *probability = 0.0;
    }
    0
}

unsafe extern "C" fn denoise_destroy(state: *mut c_void) {
    if !state.is_null() {
        // SAFETY: Every non-null state was allocated once by denoise_create.
        drop(unsafe { Box::from_raw(state.cast::<()>()) });
    }
}

static DENOISE_DESCRIPTOR: TestDenoiseDescriptor = TestDenoiseDescriptor {
    struct_size: size_of::<TestDenoiseDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"rptadv.rnnoise".as_ptr(),
    create: Some(denoise_create),
    process: Some(denoise_process),
    destroy: Some(denoise_destroy),
};

pub(crate) fn resolved(text: &str) -> (String, ChannelConfiguration) {
    let resolved = ResolvedChannelConfiguration::from_document(
        &ConfigDocument::new(text),
        "radio.conf",
        "usb",
    )
    .unwrap();
    (resolved.channel().to_owned(), resolved.into_config())
}

pub(crate) fn factory() -> NativeProcessingFactory {
    // SAFETY: Both static descriptors reproduce the process-lifetime ABIs.
    let graph = unsafe {
        GraphProvider::from_raw_descriptor(ptr::from_ref(&GRAPH_DESCRIPTOR).cast()).unwrap()
    };
    // SAFETY: Both static descriptors reproduce the process-lifetime ABIs.
    let denoise = unsafe {
        DenoiseProvider::from_raw_descriptor(ptr::from_ref(&DENOISE_DESCRIPTOR).cast()).unwrap()
    };
    NativeProcessingFactory::new(graph, denoise, "agc.so", 960).unwrap()
}

#[test]
fn plan_preserves_resolution_and_derives_every_station_request() {
    let (channel, configuration) = resolved("[usb]\n");
    let plan =
        StationPlan::new(channel, configuration, ControllerTransport::AppRpt, 9, 960).unwrap();

    assert_eq!(plan.channel(), "usb");
    assert_eq!(plan.configuration().station.hardware.input_gain_db, 0.0);
    assert_eq!(
        plan.hardware().audio_selector.output_channels,
        usbradioplus_audio::ChannelCount::Stereo
    );
    assert_eq!(plan.program_ring().input_rate_hz, 8_000);
    assert_eq!(plan.radio().generation_id, 9);
    assert_eq!(plan.processing().local.role, ChainRole::LocalReceive);
}

#[test]
fn prepared_generation_keeps_its_exact_immutable_plan() {
    let (channel, configuration) = resolved("[usb]\n");
    let plan = StationPlan::new(
        channel,
        configuration,
        ControllerTransport::RptAdvanced,
        12,
        960,
    )
    .unwrap();
    let expected = plan.clone();
    let prepared = plan.prepare(&factory()).unwrap();

    assert_eq!(prepared.plan(), &expected);
    let (returned, _processing) = prepared.into_parts();
    assert_eq!(returned, expected);
}

#[test]
fn preparation_errors_are_typed_and_keep_existing_state_outside_the_candidate() {
    let (channel, mut invalid) = resolved("[usb]\n");
    invalid.local.role = ChainRole::Link;
    let error = StationPlan::new(channel, invalid, ControllerTransport::AppRpt, 1, 960)
        .unwrap()
        .prepare(&factory())
        .err()
        .unwrap();
    assert!(matches!(error, StationPreparationError::Processing(_)));
    assert!(
        error
            .to_string()
            .starts_with("processing preparation failed:")
    );
    assert!(std::error::Error::source(&error).is_some());

    let (channel, mut invalid) = resolved("[usb]\n");
    invalid.station.transmit.settle_ms = u32::MAX;
    let error =
        StationPlan::new(channel, invalid, ControllerTransport::AppRpt, 1, 960).unwrap_err();
    assert!(matches!(error, StationPreparationError::Radio(_)));
    assert_eq!(
        error.to_string(),
        "radio preparation failed: invalid radio-session argument"
    );
    assert!(std::error::Error::source(&error).is_some());

    let (channel, configuration) = resolved("[usb]\n");
    assert!(matches!(
        StationPlan::new(channel, configuration, ControllerTransport::AppRpt, 1, 0),
        Err(StationPreparationError::Radio(RadioError::InvalidArgument))
    ));
}
