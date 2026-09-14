use super::*;

use std::error::Error as _;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::mem::size_of;
use std::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

use usbradioplus_asl3::{ControlMessage, CtcssTone, GpioPin, ParallelPin, RemoteRadio};
use usbradioplus_core::{ConfigDocument, ResolvedChannelConfiguration};
use usbradioplus_station::hardware_plan;

use crate::test_support::{graph_provider, sample_rate_adapter, station_providers};
use crate::{ControllerConfiguration, StationFactory};

const AUDIO_ABI: u32 = 2;
const GPIO_ABI: u32 = 1;
const OK: c_int = 0;
static TEST_LOCK: Mutex<()> = Mutex::new(());
static GPIO_INPUTS: AtomicU32 = AtomicU32::new(0);
static PARALLEL_INPUTS: AtomicU32 = AtomicU32::new(0);
static PTT: AtomicU32 = AtomicU32::new(0);
static FAILURE: AtomicU32 = AtomicU32::new(0);
static MIXER_PATHS_MODE: AtomicU32 = AtomicU32::new(0);
static DEVICE_INTERFACE_MODE: AtomicU32 = AtomicU32::new(0);
static EEPROM_FLAGS: AtomicU32 = AtomicU32::new(3);

const FAIL_AUDIO_START: u32 = 1;
const FAIL_AUDIO_SELECT: u32 = 2;
const FAIL_AUDIO_PATHS: u32 = 3;
const FAIL_MIXER_OPEN: u32 = 4;
const FAIL_MIXER_LEVEL: u32 = 5;
const FAIL_MIXER_SWITCH: u32 = 6;
const FAIL_AUDIO_STREAM_OPEN: u32 = 7;
const FAIL_AUDIO_STATISTICS: u32 = 8;
const FAIL_AUDIO_TIMING: u32 = 9;
const FAIL_CM119_OPEN: u32 = 10;
const FAIL_PARALLEL_OPEN: u32 = 11;
const FAIL_CM119_PUBLISH: u32 = 12;
const FAIL_CM119_SERVICE: u32 = 13;
const FAIL_CM119_INPUTS: u32 = 14;
const FAIL_CM119_STATISTICS: u32 = 15;
const FAIL_EEPROM_READ: u32 = 16;
const FAIL_EEPROM_WRITE: u32 = 17;
const FAIL_CM119_PULSE: u32 = 18;
const FAIL_PARALLEL_PUBLISH: u32 = 20;
const FAIL_PARALLEL_SERVICE: u32 = 21;
const FAIL_PARALLEL_INPUTS: u32 = 22;
const FAIL_PARALLEL_STATISTICS: u32 = 23;
const FAIL_PARALLEL_PULSE: u32 = 24;
const FAIL_SELECT_CHANNEL: u32 = 25;
const FAIL_PROGRAM_REMOTE: u32 = 26;
const FAIL_CLEAR_REMOTE: u32 = 27;
const FAIL_OPEN_TX: u32 = 40;
const FAIL_OPEN_SIDETONE: u32 = 41;
const FAIL_OPEN_COMPATIBILITY: u32 = 42;
const FAIL_LEVEL_TX_A: u32 = 43;
const FAIL_LEVEL_TX_B: u32 = 44;
const FAIL_SWITCH_TRANSMIT: u32 = 45;
const FAIL_SWITCH_COMPATIBILITY: u32 = 46;

fn fail_once(code: u32) -> bool {
    FAILURE
        .compare_exchange(code, 0, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
}

fn reset_test_state() {
    GPIO_INPUTS.store(0, Ordering::Release);
    PARALLEL_INPUTS.store(0, Ordering::Release);
    PTT.store(0, Ordering::Release);
    FAILURE.store(0, Ordering::Release);
    MIXER_PATHS_MODE.store(0, Ordering::Release);
    DEVICE_INTERFACE_MODE.store(0, Ordering::Release);
    EEPROM_FLAGS.store(3, Ordering::Release);
}

fn test_guard() -> std::sync::MutexGuard<'static, ()> {
    TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[test]
fn hardware_errors_and_operations_are_complete_and_diagnostic() {
    let operations = [
        (HardwareOperation::OpenCm119, "open CM119"),
        (HardwareOperation::OpenParallel, "open parallel port"),
        (HardwareOperation::PublishCm119, "publish CM119 outputs"),
        (HardwareOperation::ServiceCm119, "service CM119"),
        (HardwareOperation::ReadCm119, "read CM119 inputs"),
        (
            HardwareOperation::ReadCm119Statistics,
            "read CM119 statistics",
        ),
        (
            HardwareOperation::PublishParallel,
            "publish parallel outputs",
        ),
        (HardwareOperation::ServiceParallel, "service parallel port"),
        (HardwareOperation::ReadParallel, "read parallel inputs"),
        (
            HardwareOperation::ReadParallelStatistics,
            "read parallel statistics",
        ),
        (HardwareOperation::PulseCm119, "schedule CM119 pulse"),
        (HardwareOperation::PulseParallel, "schedule parallel pulse"),
        (HardwareOperation::ReadEeprom, "read CM119 EEPROM"),
        (HardwareOperation::WriteEeprom, "write CM119 EEPROM"),
        (HardwareOperation::SelectChannel, "select parallel channel"),
        (
            HardwareOperation::ProgramRemoteRadio,
            "program parallel remote radio",
        ),
        (
            HardwareOperation::ClearRemoteRadioTransmit,
            "release parallel remote-radio transmitter",
        ),
        (
            HardwareOperation::ApplySidetone,
            "apply CM119 hardware local repeat",
        ),
    ];
    assert_eq!(HardwareOperation::from_u8(0), None);
    assert_eq!(HardwareOperation::from_u8(19), None);
    for (index, (operation, description)) in operations.into_iter().enumerate() {
        assert_eq!(HardwareOperation::from_u8(index as u8 + 1), Some(operation));
        assert_eq!(operation.to_string(), description);
    }

    let audio = HardwareStationError::from(AudioError::Alsa);
    assert_eq!(
        audio.to_string(),
        "audio hardware failed: ALSA mixer operation failed"
    );
    assert!(audio.source().is_some());
    let plan = HardwareStationError::from(HardwarePlanError::ChannelLayoutMismatch);
    assert!(plan.to_string().starts_with("hardware identity failed:"));
    assert!(plan.source().is_some());
    let gpio = HardwareStationError::Gpio {
        operation: HardwareOperation::ReadCm119,
        source: GpioError::Usb,
    };
    assert_eq!(
        gpio.to_string(),
        "read CM119 inputs failed: CM119 USB operation failed"
    );
    assert!(gpio.source().is_some());
    let simple = [
        (
            HardwareStationError::UnavailableOutput,
            "output is not configured for control",
        ),
        (
            HardwareStationError::PulseTooLong,
            "output pulse duration is too long",
        ),
        (
            HardwareStationError::ControlQueueFull,
            "hardware control queue is full",
        ),
        (
            HardwareStationError::NotRunning,
            "hardware service is not running",
        ),
        (
            HardwareStationError::EepromDisabled,
            "EEPROM tuning storage is disabled",
        ),
        (
            HardwareStationError::ControlTimeout,
            "hardware control request timed out",
        ),
        (
            HardwareStationError::WorkerPanicked,
            "hardware service thread panicked",
        ),
    ];
    for (error, description) in simple {
        assert_eq!(error.to_string(), description);
        assert!(error.source().is_none());
    }

    let converted = HardwareStationError::from(ServiceFailure(
        HardwareOperation::ServiceParallel,
        GpioError::Io,
    ));
    assert!(matches!(
        converted,
        HardwareStationError::Gpio {
            operation: HardwareOperation::ServiceParallel,
            source: GpioError::Io
        }
    ));
}

#[test]
fn mixer_db_scale_preserves_the_established_midpoint() {
    assert_eq!(mixer_level(-30.0), 16);
    assert_eq!(mixer_level(0.0), 500);
    assert_eq!(mixer_level(30.0), 999);
}

#[test]
fn hardware_local_repeat_is_app_rpt_only_and_tracks_receiver_state() {
    let _guard = test_guard();
    assert_eq!(
        hardware_local_repeat_level(ControllerTransport::AppRpt, 750),
        750
    );
    assert_eq!(
        hardware_local_repeat_level(ControllerTransport::RptAdvanced, 750),
        0
    );

    let channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());
    let provider = audio_provider();
    let selected = provider.select_device(&plan.audio_selector).unwrap();
    let paths = provider
        .cm119_mixer_paths(&selected.interface_path)
        .unwrap()
        .sidetone;
    let mixers = open_paths(provider, &selected.interface_path, &paths).unwrap();
    let mut sidetone = HardwareSidetone::new(paths, mixers, 750).unwrap().unwrap();
    assert_eq!(sidetone.mixers[0].normalized().unwrap(), 0);
    assert!(!sidetone.mixers[0].enabled().unwrap());
    sidetone.apply(true).unwrap();
    sidetone.apply(true).unwrap();
    assert_eq!(sidetone.mixers[0].normalized().unwrap(), 750);
    assert!(sidetone.mixers[0].enabled().unwrap());
    sidetone.apply(false).unwrap();
    assert_eq!(sidetone.mixers[0].normalized().unwrap(), 0);
    assert!(!sidetone.mixers[0].enabled().unwrap());

    assert!(
        HardwareSidetone::new(Vec::new(), Vec::new(), 0)
            .unwrap()
            .is_none()
    );
    assert!(matches!(
        HardwareSidetone::new(Vec::new(), Vec::new(), 1),
        Err(AudioError::Unsupported)
    ));
    assert!(matches!(
        HardwareSidetone::new(Vec::new(), Vec::new(), 1_000),
        Err(AudioError::InvalidArgument)
    ));
}

#[test]
fn hardware_preflight_and_mixers_report_each_adapter_boundary_failure() {
    let _guard = test_guard();
    reset_test_state();
    let channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());

    FAILURE.store(FAIL_AUDIO_SELECT, Ordering::Release);
    assert!(matches!(
        HardwareStation::preflight(&plan, false, audio_provider(), gpio_provider()),
        Err(HardwareStationError::Audio(_))
    ));

    let mut mismatched = plan.clone();
    mismatched.cm119.required_usb_port_path = Some("another-device".into());
    assert!(matches!(
        HardwareStation::preflight(&mismatched, false, audio_provider(), gpio_provider()),
        Err(HardwareStationError::Plan(
            HardwarePlanError::UsbIdentityMismatch
        ))
    ));

    FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
    assert!(matches!(
        HardwareStation::preflight(&plan, true, audio_provider(), gpio_provider()),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::OpenCm119,
            ..
        })
    ));
    FAILURE.store(FAIL_EEPROM_READ, Ordering::Release);
    let absent =
        HardwareStation::preflight(&plan, true, audio_provider(), gpio_provider()).unwrap();
    let mut without_tuning = channel.clone().into_config();
    assert!(!absent.apply_startup_tuning(&mut without_tuning));
    EEPROM_FLAGS.store(1, Ordering::Release);
    let invalid =
        HardwareStation::preflight(&plan, true, audio_provider(), gpio_provider()).unwrap();
    assert!(!invalid.apply_startup_tuning(&mut without_tuning));
    EEPROM_FLAGS.store(2, Ordering::Release);
    let invalid =
        HardwareStation::preflight(&plan, true, audio_provider(), gpio_provider()).unwrap();
    assert!(!invalid.apply_startup_tuning(&mut without_tuning));
    EEPROM_FLAGS.store(3, Ordering::Release);

    let selected = audio_provider()
        .select_device(&plan.audio_selector)
        .unwrap();
    let config = &channel.config().station.hardware;
    FAILURE.store(FAIL_AUDIO_PATHS, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    MIXER_PATHS_MODE.store(1, Ordering::Release);
    assert!(matches!(
        HardwareMixers::open(audio_provider(), &selected, config, None, 0),
        Err(AudioError::Unsupported)
    ));
    MIXER_PATHS_MODE.store(0, Ordering::Release);
    FAILURE.store(FAIL_MIXER_OPEN, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    for fault in [FAIL_OPEN_TX, FAIL_OPEN_SIDETONE, FAIL_OPEN_COMPATIBILITY] {
        FAILURE.store(fault, Ordering::Release);
        assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    }
    FAILURE.store(FAIL_MIXER_LEVEL, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    for fault in [FAIL_LEVEL_TX_A, FAIL_LEVEL_TX_B] {
        FAILURE.store(fault, Ordering::Release);
        assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    }
    FAILURE.store(FAIL_MIXER_SWITCH, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    FAILURE.store(FAIL_SWITCH_TRANSMIT, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    FAILURE.store(FAIL_SWITCH_COMPATIBILITY, Ordering::Release);
    assert!(HardwareMixers::open(audio_provider(), &selected, config, None, 0).is_err());
    MIXER_PATHS_MODE.store(4, Ordering::Release);
    let (mut mixers, _) =
        HardwareMixers::open(audio_provider(), &selected, config, None, 0).unwrap();
    FAILURE.store(FAIL_MIXER_LEVEL, Ordering::Release);
    assert!(mixers.set_level(HardwareMixer::Receive, 500).is_err());

    MIXER_PATHS_MODE.store(2, Ordering::Release);
    assert!(matches!(
        HardwareMixers::open(audio_provider(), &selected, config, None, 500),
        Err(AudioError::Unsupported)
    ));
    MIXER_PATHS_MODE.store(0, Ordering::Release);
    let paths = audio_provider()
        .cm119_mixer_paths(&selected.interface_path)
        .unwrap()
        .sidetone;
    let sidetone_mixers = open_paths(audio_provider(), &selected.interface_path, &paths).unwrap();
    FAILURE.store(FAIL_MIXER_LEVEL, Ordering::Release);
    assert!(HardwareSidetone::new(paths, sidetone_mixers, 500).is_err());

    let paths = audio_provider()
        .cm119_mixer_paths(&selected.interface_path)
        .unwrap()
        .sidetone;
    let sidetone_mixers = open_paths(audio_provider(), &selected.interface_path, &paths).unwrap();
    let mut sidetone = HardwareSidetone::new(paths, sidetone_mixers, 500)
        .unwrap()
        .unwrap();
    FAILURE.store(FAIL_MIXER_SWITCH, Ordering::Release);
    assert!(sidetone.apply(true).is_err());
}

#[test]
fn hardware_station_composition_start_and_diagnostics_propagate_failures() {
    let _guard = test_guard();
    reset_test_state();

    let channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());
    let mut bad_selection = audio_provider()
        .select_device(&plan.audio_selector)
        .unwrap();
    bad_selection.input_channels = usbradioplus_audio::ChannelCount::Stereo;
    let bad_preflight = HardwarePreflight::new(bad_selection, None);
    assert!(matches!(
        HardwareStation::open_preflighted(
            station_media(channel),
            audio_provider(),
            gpio_provider(),
            960,
            bad_preflight,
        ),
        Err(HardwareStationError::Plan(
            HardwarePlanError::ChannelLayoutMismatch
        ))
    ));

    let mut channel = resolved_hardware_channel();
    let preflight = HardwareStation::preflight(
        &hardware_plan(channel.config()),
        true,
        audio_provider(),
        gpio_provider(),
    )
    .unwrap();
    preflight.apply_startup_tuning(channel.config_mut());
    FAILURE.store(FAIL_MIXER_OPEN, Ordering::Release);
    assert!(matches!(
        HardwareStation::open_preflighted(
            station_media(channel),
            audio_provider(),
            gpio_provider(),
            960,
            preflight,
        ),
        Err(HardwareStationError::Audio(_))
    ));

    let mut channel = resolved_hardware_channel();
    let preflight = HardwareStation::preflight(
        &hardware_plan(channel.config()),
        true,
        audio_provider(),
        gpio_provider(),
    )
    .unwrap();
    preflight.apply_startup_tuning(channel.config_mut());
    FAILURE.store(FAIL_AUDIO_STREAM_OPEN, Ordering::Release);
    assert!(matches!(
        HardwareStation::open_preflighted(
            station_media(channel),
            audio_provider(),
            gpio_provider(),
            960,
            preflight,
        ),
        Err(HardwareStationError::Audio(_))
    ));

    let mut service_start_failure = open_hardware_station();
    FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
    assert!(matches!(
        service_start_failure.start(),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::OpenCm119,
            ..
        })
    ));

    let mut station = open_hardware_station();
    FAILURE.store(FAIL_AUDIO_START, Ordering::Release);
    assert!(matches!(
        station.start(),
        Err(HardwareStationError::Audio(_))
    ));
    station.start().unwrap();
    FAILURE.store(FAIL_MIXER_LEVEL, Ordering::Release);
    assert!(matches!(
        station.set_mixer_level(HardwareMixer::Receive, 500),
        Err(HardwareStationError::Audio(_))
    ));
    FAILURE.store(FAIL_EEPROM_WRITE, Ordering::Release);
    assert!(matches!(
        station.save_current_tuning_to_eeprom(),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::WriteEeprom,
            ..
        })
    ));
    FAILURE.store(FAIL_AUDIO_STATISTICS, Ordering::Release);
    assert!(matches!(
        station.diagnostics(),
        Err(HardwareStationError::Audio(_))
    ));
    FAILURE.store(FAIL_AUDIO_TIMING, Ordering::Release);
    assert!(matches!(
        station.diagnostics(),
        Err(HardwareStationError::Audio(_))
    ));
    station.stop().unwrap();
}

#[test]
fn configured_frequencies_seed_parallel_remote_radio_programming() {
    let mut configuration = resolved_hardware_channel().into_config();
    assert_eq!(configured_remote_radio(&configuration), None);
    configuration.station.receive.frequency_hz = 145_110_000;
    configuration.station.transmit.frequency_hz = 144_510_000;
    assert_eq!(
        configured_remote_radio(&configuration),
        Some(RemoteRadio {
            receive_hz: 145_110_000,
            transmit_hz: 144_510_000,
            receive_ctcss: None,
            transmit_ctcss: None,
            high_power: false,
        })
    );
}

#[test]
fn persistent_outputs_and_parallel_ptt_are_independent() {
    let mut value = 0x02;
    apply_persistent(&mut value, 0x04, OutputRequest::Active);
    assert_eq!(value, 0x06);
    apply_persistent(&mut value, 0x02, OutputRequest::Inactive);
    assert_eq!(value, 0x04);
    apply_persistent(&mut value, 0x08, OutputRequest::Pulse(1));
    assert_eq!(value, 0x04);

    let plan = ParallelPlan {
        config: usbradioplus_gpio::ParallelConfig {
            transport: usbradioplus_gpio::ParallelTransport::Automatic,
            ppdev_path: None,
            raw_io_base: 0,
            output_enable_mask: 0x0f,
            output_initial_mask: 0,
        },
        ptt_mask: 0x01,
        input_mask: 0,
        carrier_mask: 0,
        ctcss_mask: 0,
    };
    assert_eq!(parallel_output(&plan, 0x06, false, false), 0x06);
    assert_eq!(parallel_output(&plan, 0x06, true, false), 0x07);
    assert_eq!(parallel_output(&plan, 0x06, false, true), 0x07);
}

#[test]
fn hardware_input_mapping_requires_online_devices() {
    let plan = ParallelPlan {
        config: usbradioplus_gpio::ParallelConfig {
            transport: usbradioplus_gpio::ParallelTransport::Automatic,
            ppdev_path: None,
            raw_io_base: 0,
            output_enable_mask: 0,
            output_initial_mask: 0,
        },
        ptt_mask: 0,
        input_mask: 0,
        carrier_mask: 0x20,
        ctcss_mask: 0x10,
    };
    let mapped = hardware_inputs(
        Cm119Inputs {
            online: true,
            cor_active: true,
            ctcss_active: true,
            ..Cm119Inputs::default()
        },
        Some(&plan),
        Some(ParallelInputs {
            online: true,
            status_mask: 0x30,
        }),
        Cm119Statistics {
            online: true,
            ptt_applied: true,
            ..Cm119Statistics::default()
        },
    );
    assert_eq!(
        mapped,
        HardwareInputs {
            carrier: true,
            parallel_carrier: true,
            subaudible: true,
            parallel_subaudible: true,
            physical_ptt: true,
            cm119_gpio_mask: 0,
            parallel_input_mask: 0x30,
        }
    );
    assert_eq!(
        hardware_inputs(
            Cm119Inputs::default(),
            Some(&plan),
            Some(ParallelInputs {
                online: false,
                status_mask: 0x30,
            }),
            Cm119Statistics::default(),
        ),
        HardwareInputs::default()
    );
}

#[repr(C)]
struct AudioStreamConfig {
    struct_size: u32,
    abi_version: u32,
    native_sample_rate_hz: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
    input_device_index: i32,
    output_device_index: i32,
    input_device_channels: u32,
    output_device_channels: u32,
    receive_worker: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32>,
    receive_context: *mut c_void,
    transmit_worker: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32>,
    transmit_context: *mut c_void,
}

#[repr(C)]
#[derive(Default)]
struct AudioStatistics {
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
struct AudioTiming {
    struct_size: u32,
    abi_version: u32,
    input_latency_seconds: f64,
    output_latency_seconds: f64,
    sample_rate_hz: f64,
}

#[repr(C)]
struct AudioMixerConfig {
    struct_size: u32,
    card: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct UsbMixerConfig {
    struct_size: u32,
    interface: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct AudioDeviceIdentity {
    struct_size: u32,
    interface: *const c_char,
    serial: *const c_char,
    input_channels: u32,
    output_channels: u32,
}

#[repr(C)]
#[derive(Default)]
struct AudioDeviceSelection {
    struct_size: u32,
    abi_version: u32,
    alsa_card_index: u32,
    input_device_index: i32,
    output_device_index: i32,
}

#[repr(C)]
struct AudioDeviceSelector {
    struct_size: u32,
    policy: u32,
    identifier: *const c_char,
    serial: *const c_char,
    input_channels: u32,
    output_channels: u32,
}

#[repr(C)]
struct AudioDeviceMatch {
    struct_size: u32,
    abi_version: u32,
    interface: [c_char; 256],
    serial: [c_char; 256],
    selection: AudioDeviceSelection,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AudioMixerPath {
    element: [c_char; 64],
    element_index: u32,
    channel: u32,
    direction: u32,
    capabilities: u32,
}

impl Default for AudioMixerPath {
    fn default() -> Self {
        Self {
            element: [0; 64],
            element_index: 0,
            channel: 0,
            direction: 0,
            capabilities: 0,
        }
    }
}

#[repr(C)]
#[derive(Default)]
struct AudioMixerPaths {
    struct_size: u32,
    abi_version: u32,
    receive_count: u32,
    transmit_count: u32,
    sidetone_count: u32,
    compatibility_count: u32,
    receive: [AudioMixerPath; 2],
    transmit: [AudioMixerPath; 2],
    sidetone: [AudioMixerPath; 2],
    compatibility: [AudioMixerPath; 2],
}

#[repr(C)]
struct AudioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability: *const c_char,
    stream_create:
        Option<unsafe extern "C" fn(*const AudioStreamConfig, *mut *mut c_void) -> c_int>,
    stream_start: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    stream_stop: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    stream_statistics: Option<unsafe extern "C" fn(*const c_void, *mut AudioStatistics) -> c_int>,
    stream_destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    mixer_create: Option<unsafe extern "C" fn(*const AudioMixerConfig, *mut *mut c_void) -> c_int>,
    mixer_range_centibels: Option<unsafe extern "C" fn(*const c_void, *mut i64, *mut i64) -> c_int>,
    mixer_get_centibels: Option<unsafe extern "C" fn(*const c_void, *mut i64) -> c_int>,
    mixer_set_centibels: Option<unsafe extern "C" fn(*mut c_void, i64) -> c_int>,
    mixer_destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    mixer_usb_create:
        Option<unsafe extern "C" fn(*const UsbMixerConfig, *mut *mut c_void) -> c_int>,
    mixer_range_steps: Option<unsafe extern "C" fn(*const c_void, *mut i64, *mut i64) -> c_int>,
    mixer_get_steps: Option<unsafe extern "C" fn(*const c_void, *mut i64) -> c_int>,
    mixer_set_steps: Option<unsafe extern "C" fn(*mut c_void, i64) -> c_int>,
    mixer_get_normalized: Option<unsafe extern "C" fn(*const c_void, *mut u32) -> c_int>,
    mixer_set_normalized: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    mixer_get_switch: Option<unsafe extern "C" fn(*const c_void, *mut u32) -> c_int>,
    mixer_set_switch: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    device_resolve: Option<
        unsafe extern "C" fn(*const AudioDeviceIdentity, *mut AudioDeviceSelection) -> c_int,
    >,
    device_select:
        Option<unsafe extern "C" fn(*const AudioDeviceSelector, *mut AudioDeviceMatch) -> c_int>,
    stream_timing: Option<unsafe extern "C" fn(*const c_void, *mut AudioTiming) -> c_int>,
    mixer_paths: Option<unsafe extern "C" fn(*const c_char, *mut AudioMixerPaths) -> c_int>,
}

// SAFETY: the test descriptor and all referenced functions are immutable.
unsafe impl Sync for AudioDescriptor {}

struct FakeStream {
    receive: unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32,
    receive_context: *mut c_void,
    transmit: unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32,
    transmit_context: *mut c_void,
}

struct FakeMixer {
    normalized: u32,
    enabled: u32,
    kind: u32,
}

unsafe extern "C" fn audio_stream_create(
    config: *const AudioStreamConfig,
    output: *mut *mut c_void,
) -> c_int {
    if fail_once(FAIL_AUDIO_STREAM_OPEN) {
        return -1;
    }
    // SAFETY: the validated wrapper supplies complete live objects.
    let (config, output) = unsafe { (&*config, &mut *output) };
    *output = Box::into_raw(Box::new(FakeStream {
        receive: config.receive_worker.unwrap(),
        receive_context: config.receive_context,
        transmit: config.transmit_worker.unwrap(),
        transmit_context: config.transmit_context,
    }))
    .cast();
    OK
}

unsafe extern "C" fn audio_stream_start(stream: *mut c_void) -> c_int {
    if fail_once(FAIL_AUDIO_START) {
        return -1;
    }
    // SAFETY: the handle originates from audio_stream_create.
    let stream = unsafe { &mut *stream.cast::<FakeStream>() };
    let input = [0.25_f32; 1_920];
    let mut output = [0.0_f32; 1_920];
    // SAFETY: both contexts and canonical 20 ms buffers are live for these calls.
    let receive = unsafe { (stream.receive)(stream.receive_context, input.as_ptr(), 960) };
    // SAFETY: both contexts and canonical 20 ms buffers are live for these calls.
    let transmit = unsafe { (stream.transmit)(stream.transmit_context, output.as_mut_ptr(), 960) };
    if receive == 0 && transmit == 0 {
        OK
    } else {
        -1
    }
}

unsafe extern "C" fn audio_stream_control(_stream: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn audio_stream_statistics(
    _stream: *const c_void,
    output: *mut AudioStatistics,
) -> c_int {
    if fail_once(FAIL_AUDIO_STATISTICS) {
        return -1;
    }
    // SAFETY: the wrapper supplies complete writable storage.
    unsafe {
        (*output).abi_version = AUDIO_ABI;
        (*output).callback_count = 2;
    }
    OK
}

unsafe extern "C" fn audio_stream_timing(
    _stream: *const c_void,
    output: *mut AudioTiming,
) -> c_int {
    if fail_once(FAIL_AUDIO_TIMING) {
        return -1;
    }
    // SAFETY: the wrapper supplies complete writable storage.
    unsafe {
        (*output).abi_version = AUDIO_ABI;
        (*output).input_latency_seconds = 0.01;
        (*output).output_latency_seconds = 0.02;
        (*output).sample_rate_hz = 48_000.0;
    }
    OK
}

unsafe extern "C" fn audio_stream_destroy(stream: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by create.
    drop(unsafe { Box::from_raw(stream.cast::<FakeStream>()) });
}

unsafe extern "C" fn unused_audio_mixer_create(
    _config: *const AudioMixerConfig,
    _output: *mut *mut c_void,
) -> c_int {
    -1
}

unsafe extern "C" fn audio_mixer_create(
    config: *const UsbMixerConfig,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: the wrapper supplies a complete live configuration.
    let name = unsafe { CStr::from_ptr((*config).element) }.to_bytes();
    let kind = match name {
        b"rx" => 1,
        b"tx-a" => 2,
        b"tx-b" => 3,
        b"side" => 4,
        b"compat" => 5,
        _ => 0,
    };
    if fail_once(FAIL_MIXER_OPEN)
        || (kind == 2 && fail_once(FAIL_OPEN_TX))
        || (kind == 4 && fail_once(FAIL_OPEN_SIDETONE))
        || (kind == 5 && fail_once(FAIL_OPEN_COMPATIBILITY))
    {
        return -1;
    }
    // SAFETY: the wrapper supplies writable handle storage.
    unsafe {
        *output = Box::into_raw(Box::new(FakeMixer {
            normalized: 500,
            enabled: 0,
            kind,
        }))
        .cast();
    }
    OK
}

unsafe extern "C" fn audio_mixer_range(
    _mixer: *const c_void,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: the wrapper supplies writable range fields.
    unsafe {
        *minimum = 0;
        *maximum = 2_000;
    }
    OK
}

unsafe extern "C" fn audio_mixer_get_i64(mixer: *const c_void, value: *mut i64) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = i64::from((*mixer.cast::<FakeMixer>()).normalized) * 2 };
    OK
}

unsafe extern "C" fn audio_mixer_set_i64(mixer: *mut c_void, value: i64) -> c_int {
    // SAFETY: the handle originates from audio_mixer_create.
    let kind = unsafe { (*mixer.cast::<FakeMixer>()).kind };
    if fail_once(FAIL_MIXER_LEVEL)
        || (kind == 2 && fail_once(FAIL_LEVEL_TX_A))
        || (kind == 3 && fail_once(FAIL_LEVEL_TX_B))
    {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).normalized = (value / 2) as u32 };
    OK
}

unsafe extern "C" fn audio_mixer_get(mixer: *const c_void, value: *mut u32) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).normalized };
    OK
}

unsafe extern "C" fn audio_mixer_set(mixer: *mut c_void, value: u32) -> c_int {
    if fail_once(FAIL_MIXER_LEVEL) {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).normalized = value };
    OK
}

unsafe extern "C" fn audio_mixer_get_switch(mixer: *const c_void, value: *mut u32) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).enabled };
    OK
}

unsafe extern "C" fn audio_mixer_set_switch(mixer: *mut c_void, value: u32) -> c_int {
    // SAFETY: the handle originates from audio_mixer_create.
    let kind = unsafe { (*mixer.cast::<FakeMixer>()).kind };
    if fail_once(FAIL_MIXER_SWITCH)
        || (matches!(kind, 2 | 3) && fail_once(FAIL_SWITCH_TRANSMIT))
        || (kind == 5 && fail_once(FAIL_SWITCH_COMPATIBILITY))
    {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).enabled = value };
    OK
}

unsafe extern "C" fn audio_mixer_destroy(mixer: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by create.
    drop(unsafe { Box::from_raw(mixer.cast::<FakeMixer>()) });
}

unsafe extern "C" fn audio_device_resolve(
    _identity: *const AudioDeviceIdentity,
    _selection: *mut AudioDeviceSelection,
) -> c_int {
    -1
}

unsafe extern "C" fn audio_device_select(
    _selector: *const AudioDeviceSelector,
    output: *mut AudioDeviceMatch,
) -> c_int {
    if fail_once(FAIL_AUDIO_SELECT) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    if DEVICE_INTERFACE_MODE.load(Ordering::Acquire) == 0 {
        copy_c_string(&mut output.interface, b"3-1:1.0");
    } else {
        copy_c_string(&mut output.interface, b"4-1:1.0");
    }
    copy_c_string(&mut output.serial, b"ABC");
    output.selection = AudioDeviceSelection {
        struct_size: size_of::<AudioDeviceSelection>() as u32,
        abi_version: AUDIO_ABI,
        alsa_card_index: 1,
        input_device_index: 2,
        output_device_index: 3,
    };
    OK
}

unsafe extern "C" fn audio_mixer_paths(
    _interface: *const c_char,
    output: *mut AudioMixerPaths,
) -> c_int {
    if fail_once(FAIL_AUDIO_PATHS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    output.receive_count = 1;
    let mode = MIXER_PATHS_MODE.load(Ordering::Acquire);
    output.transmit_count = if mode == 1 { 1 } else { 2 };
    output.sidetone_count = 1;
    output.compatibility_count = 1;
    set_mixer_path(&mut output.receive[0], b"rx", 0, 3);
    set_mixer_path(&mut output.transmit[0], b"tx-a", 1, 3);
    set_mixer_path(&mut output.transmit[1], b"tx-b", 1, 3);
    set_mixer_path(&mut output.sidetone[0], b"side", 1, 3);
    set_mixer_path(&mut output.compatibility[0], b"compat", 1, 2);
    if mode == 2 {
        output.sidetone[0].capabilities = 1;
    } else if mode == 4 {
        output.receive[0].capabilities = 1;
    }
    OK
}

fn set_mixer_path(path: &mut AudioMixerPath, name: &[u8], direction: u32, capabilities: u32) {
    copy_c_string(&mut path.element, name);
    path.direction = direction;
    path.capabilities = capabilities;
}

fn copy_c_string<const N: usize>(output: &mut [c_char; N], value: &[u8]) {
    for (destination, source) in output.iter_mut().zip(value) {
        *destination = *source as c_char;
    }
}

static AUDIO_DESCRIPTOR: AudioDescriptor = AudioDescriptor {
    struct_size: size_of::<AudioDescriptor>() as u32,
    abi_version: AUDIO_ABI,
    capability: c"rptadv.portaudio-alsa-audio".as_ptr(),
    stream_create: Some(audio_stream_create),
    stream_start: Some(audio_stream_start),
    stream_stop: Some(audio_stream_control),
    stream_statistics: Some(audio_stream_statistics),
    stream_destroy: Some(audio_stream_destroy),
    mixer_create: Some(unused_audio_mixer_create),
    mixer_range_centibels: Some(audio_mixer_range),
    mixer_get_centibels: Some(audio_mixer_get_i64),
    mixer_set_centibels: Some(audio_mixer_set_i64),
    mixer_destroy: Some(audio_mixer_destroy),
    mixer_usb_create: Some(audio_mixer_create),
    mixer_range_steps: Some(audio_mixer_range),
    mixer_get_steps: Some(audio_mixer_get_i64),
    mixer_set_steps: Some(audio_mixer_set_i64),
    mixer_get_normalized: Some(audio_mixer_get),
    mixer_set_normalized: Some(audio_mixer_set),
    mixer_get_switch: Some(audio_mixer_get_switch),
    mixer_set_switch: Some(audio_mixer_set_switch),
    device_resolve: Some(audio_device_resolve),
    device_select: Some(audio_device_select),
    stream_timing: Some(audio_stream_timing),
    mixer_paths: Some(audio_mixer_paths),
};

#[repr(C)]
struct GpioCm119Outputs {
    struct_size: u32,
    abi_version: u32,
    ptt_asserted: u32,
    gpio_output_mask: u32,
}

#[repr(C)]
struct GpioCm119Pulse {
    struct_size: u32,
    abi_version: u32,
    ptt_invert: u32,
    gpio_invert_mask: u32,
    duration_ms: u32,
    ptt_cancel: u32,
    gpio_cancel_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioCm119Inputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    cor_active: u32,
    ctcss_active: u32,
    gpio_input_mask: u32,
    hid_report: [u8; 4],
}

#[repr(C)]
#[derive(Default)]
struct GpioCm119Statistics {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    usb_error_count: u64,
    ptt_applied: u32,
    online: u32,
    last_usb_error: i32,
    eeprom_read_count: u64,
    eeprom_write_count: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct GpioEeprom {
    struct_size: u32,
    abi_version: u32,
    checksum_valid: u32,
    magic_valid: u32,
    words: [u16; 64],
}

#[repr(C)]
struct GpioParallelOutputs {
    struct_size: u32,
    abi_version: u32,
    output_mask: u32,
    pulse_mask: u32,
    duration_ms: u32,
    cancel_pulse: u32,
}

#[repr(C)]
struct GpioParallelPulse {
    struct_size: u32,
    abi_version: u32,
    invert_mask: u32,
    duration_ms: u32,
    cancel_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioParallelInputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    status_mask: u32,
}

#[repr(C)]
#[derive(Default)]
struct GpioParallelStatistics {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    io_error_count: u64,
    online: u32,
    last_io_error: i32,
    applied_output_mask: u32,
}

type GpioOpen = unsafe extern "C" fn(*const c_void, *mut *mut c_void) -> c_int;
type GpioPublish = unsafe extern "C" fn(*mut c_void, *const GpioCm119Outputs) -> c_int;
type GpioControl = unsafe extern "C" fn(*mut c_void) -> c_int;
type GpioInputs = unsafe extern "C" fn(*const c_void, *mut GpioCm119Inputs) -> c_int;
type GpioStatistics = unsafe extern "C" fn(*const c_void, *mut GpioCm119Statistics) -> c_int;
type GpioEepromIo = unsafe extern "C" fn(*mut c_void, *mut GpioEeprom) -> c_int;
type ParallelPublish = unsafe extern "C" fn(*mut c_void, *const GpioParallelOutputs) -> c_int;
type RawParallelInputs = unsafe extern "C" fn(*const c_void, *mut GpioParallelInputs) -> c_int;
type ParallelStatistics = unsafe extern "C" fn(*const c_void, *mut GpioParallelStatistics) -> c_int;

#[repr(C)]
struct GpioDescriptor {
    struct_size: u32,
    abi_version: u32,
    capability: *const c_char,
    probe: Option<unsafe extern "C" fn(*const c_void, *mut c_void) -> c_int>,
    open: Option<GpioOpen>,
    publish: Option<GpioPublish>,
    service: Option<GpioControl>,
    inputs: Option<GpioInputs>,
    statistics: Option<GpioStatistics>,
    close: Option<unsafe extern "C" fn(*mut c_void)>,
    discover: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    read_eeprom: Option<GpioEepromIo>,
    write_eeprom: Option<GpioEepromIo>,
    parallel_open: Option<GpioOpen>,
    parallel_publish: Option<ParallelPublish>,
    parallel_service: Option<GpioControl>,
    parallel_write: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    parallel_inputs: Option<RawParallelInputs>,
    parallel_statistics: Option<ParallelStatistics>,
    parallel_close: Option<unsafe extern "C" fn(*mut c_void)>,
    old_gpio_pulse: Option<unsafe extern "C" fn() -> c_int>,
    old_parallel_pulse: Option<unsafe extern "C" fn() -> c_int>,
    gpio_pulse: Option<unsafe extern "C" fn(*mut c_void, *const GpioCm119Pulse) -> c_int>,
    parallel_pulse: Option<unsafe extern "C" fn(*mut c_void, *const GpioParallelPulse) -> c_int>,
    set_channel: Option<unsafe extern "C" fn(*mut c_void, u8) -> c_int>,
    program_rtx: Option<unsafe extern "C" fn(*mut c_void, u32, u32, u32, u32) -> c_int>,
    clear_rtx: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
}

// SAFETY: the test descriptor and all referenced functions are immutable.
unsafe impl Sync for GpioDescriptor {}

unsafe extern "C" fn gpio_probe(_config: *const c_void, _output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_discover(_output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_open(_config: *const c_void, output: *mut *mut c_void) -> c_int {
    if fail_once(FAIL_CM119_OPEN) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(0_u8)).cast() };
    OK
}

unsafe extern "C" fn parallel_open(_config: *const c_void, output: *mut *mut c_void) -> c_int {
    if fail_once(FAIL_PARALLEL_OPEN) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(0_u8)).cast() };
    OK
}

unsafe extern "C" fn gpio_publish(_device: *mut c_void, outputs: *const GpioCm119Outputs) -> c_int {
    if fail_once(FAIL_CM119_PUBLISH) {
        return -1;
    }
    // SAFETY: the wrapper supplies a complete output snapshot.
    PTT.store(unsafe { (*outputs).ptt_asserted }, Ordering::Release);
    OK
}

unsafe extern "C" fn gpio_service(_device: *mut c_void) -> c_int {
    if fail_once(FAIL_CM119_SERVICE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_service(_device: *mut c_void) -> c_int {
    if fail_once(FAIL_PARALLEL_SERVICE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn gpio_inputs(_device: *const c_void, output: *mut GpioCm119Inputs) -> c_int {
    if fail_once(FAIL_CM119_INPUTS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioCm119Inputs {
            struct_size: size_of::<GpioCm119Inputs>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            cor_active: 1,
            ctcss_active: 1,
            gpio_input_mask: GPIO_INPUTS.load(Ordering::Acquire),
            hid_report: [0; 4],
        };
    }
    OK
}

unsafe extern "C" fn gpio_statistics(
    _device: *const c_void,
    output: *mut GpioCm119Statistics,
) -> c_int {
    if fail_once(FAIL_CM119_STATISTICS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioCm119Statistics {
            struct_size: size_of::<GpioCm119Statistics>() as u32,
            abi_version: GPIO_ABI,
            input_read_count: 1,
            output_apply_count: 1,
            ptt_applied: PTT.load(Ordering::Acquire),
            online: 1,
            ..GpioCm119Statistics::default()
        };
    }
    OK
}

unsafe extern "C" fn gpio_close(device: *mut c_void) {
    // SAFETY: the handle is the unique allocation returned by open.
    drop(unsafe { Box::from_raw(device.cast::<u8>()) });
}

unsafe extern "C" fn gpio_read_eeprom(_device: *mut c_void, image: *mut GpioEeprom) -> c_int {
    if fail_once(FAIL_EEPROM_READ) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable image storage.
    unsafe {
        (*image).struct_size = size_of::<GpioEeprom>() as u32;
        (*image).abi_version = GPIO_ABI;
        let flags = EEPROM_FLAGS.load(Ordering::Acquire);
        (*image).checksum_valid = flags & 1;
        (*image).magic_valid = (flags >> 1) & 1;
        (*image).words[52] = 321;
        (*image).words[53] = 411;
        (*image).words[54] = 512;
        (*image).words[59] = 63;
        (*image).words[60] = 654;
    }
    OK
}

unsafe extern "C" fn gpio_write_eeprom(_device: *mut c_void, image: *mut GpioEeprom) -> c_int {
    if fail_once(FAIL_EEPROM_WRITE) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable image storage.
    unsafe {
        (*image).checksum_valid = 1;
        (*image).magic_valid = 1;
    }
    OK
}

unsafe extern "C" fn gpio_pulse(_device: *mut c_void, _pulse: *const GpioCm119Pulse) -> c_int {
    if fail_once(FAIL_CM119_PULSE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_publish(
    _device: *mut c_void,
    _outputs: *const GpioParallelOutputs,
) -> c_int {
    if fail_once(FAIL_PARALLEL_PUBLISH) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_inputs(
    _device: *const c_void,
    output: *mut GpioParallelInputs,
) -> c_int {
    if fail_once(FAIL_PARALLEL_INPUTS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioParallelInputs {
            struct_size: size_of::<GpioParallelInputs>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            status_mask: PARALLEL_INPUTS.load(Ordering::Acquire),
        };
    }
    OK
}

unsafe extern "C" fn parallel_statistics(
    _device: *const c_void,
    output: *mut GpioParallelStatistics,
) -> c_int {
    if fail_once(FAIL_PARALLEL_STATISTICS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable snapshot storage.
    unsafe {
        *output = GpioParallelStatistics {
            struct_size: size_of::<GpioParallelStatistics>() as u32,
            abi_version: GPIO_ABI,
            online: 1,
            ..GpioParallelStatistics::default()
        };
    }
    OK
}

unsafe extern "C" fn parallel_write(_device: *mut c_void, _value: u32) -> c_int {
    OK
}

unsafe extern "C" fn parallel_pulse(
    _device: *mut c_void,
    _pulse: *const GpioParallelPulse,
) -> c_int {
    if fail_once(FAIL_PARALLEL_PULSE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_set_channel(_device: *mut c_void, _channel: u8) -> c_int {
    if fail_once(FAIL_SELECT_CHANNEL) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_program_rtx(
    _device: *mut c_void,
    _receive_hz: u32,
    _transmit_hz: u32,
    _transmitting: u32,
    _high_power: u32,
) -> c_int {
    if fail_once(FAIL_PROGRAM_REMOTE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn parallel_control(_device: *mut c_void) -> c_int {
    if fail_once(FAIL_CLEAR_REMOTE) {
        return -1;
    }
    OK
}

unsafe extern "C" fn unused_gpio_pulse() -> c_int {
    OK
}

static GPIO_DESCRIPTOR: GpioDescriptor = GpioDescriptor {
    struct_size: size_of::<GpioDescriptor>() as u32,
    abi_version: GPIO_ABI,
    capability: c"rptadv.cm119-hid-gpio".as_ptr(),
    probe: Some(gpio_probe),
    open: Some(gpio_open),
    publish: Some(gpio_publish),
    service: Some(gpio_service),
    inputs: Some(gpio_inputs),
    statistics: Some(gpio_statistics),
    close: Some(gpio_close),
    discover: Some(gpio_discover),
    read_eeprom: Some(gpio_read_eeprom),
    write_eeprom: Some(gpio_write_eeprom),
    parallel_open: Some(parallel_open),
    parallel_publish: Some(parallel_publish),
    parallel_service: Some(parallel_service),
    parallel_write: Some(parallel_write),
    parallel_inputs: Some(parallel_inputs),
    parallel_statistics: Some(parallel_statistics),
    parallel_close: Some(gpio_close),
    old_gpio_pulse: Some(unused_gpio_pulse),
    old_parallel_pulse: Some(unused_gpio_pulse),
    gpio_pulse: Some(gpio_pulse),
    parallel_pulse: Some(parallel_pulse),
    set_channel: Some(parallel_set_channel),
    program_rtx: Some(parallel_program_rtx),
    clear_rtx: Some(parallel_control),
};

fn audio_provider() -> AudioProvider {
    // SAFETY: the descriptor and every referenced function have process lifetime.
    unsafe { AudioProvider::from_raw_descriptor(ptr::from_ref(&AUDIO_DESCRIPTOR).cast()) }.unwrap()
}

fn gpio_provider() -> GpioAdapter {
    // SAFETY: the descriptor and every referenced function have process lifetime.
    unsafe { GpioAdapter::from_raw(ptr::from_ref(&GPIO_DESCRIPTOR).cast()) }.unwrap()
}

fn resolved_hardware_channel() -> ResolvedChannelConfiguration {
    let document = ConfigDocument::new(
        "[usb]\n\
         [hardware]\n\
         hardware_eeprom_enabled = yes\n\
         hardware_gpio_1_mode = out0\n\
         hardware_gpio_2_mode = in\n\
         hardware_clip_led_gpio = 1\n\
         hardware_parallel_port_device = /dev/parport0\n\
         hardware_parallel_pin_2_assignment = out0\n\
         hardware_parallel_pin_10_assignment = in\n",
    );
    ResolvedChannelConfiguration::from_document(&document, "radio.conf", "usb").unwrap()
}

fn station_media(channel: ResolvedChannelConfiguration) -> StationMedia {
    StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap()
    .prepare(
        channel,
        1,
        ControllerConfiguration::RptAdvanced { handoff_slots: 2 },
    )
    .unwrap()
}

fn selected_hardware(with_parallel: bool) -> (HardwarePlan, SelectedHardwarePlan) {
    let channel = resolved_hardware_channel();
    let mut plan = hardware_plan(channel.config());
    if !with_parallel {
        plan.parallel = None;
    }
    let selected = audio_provider()
        .select_device(&plan.audio_selector)
        .unwrap();
    let selected = plan.select(&selected, 960).unwrap();
    (plan, selected)
}

fn service_worker(
    plan: &HardwarePlan,
    selected: &SelectedHardwarePlan,
    state: SharedHardwareState,
) -> (
    ServiceWorker,
    SyncSender<ServiceCommand>,
    Receiver<HardwareInputEvent>,
) {
    let (command_sender, command_receiver) = sync_channel(CONTROL_QUEUE_CAPACITY);
    let (input_sender, input_receiver) = sync_channel(INPUT_EVENT_QUEUE_CAPACITY);
    (
        ServiceWorker {
            provider: gpio_provider(),
            selected: selected.clone(),
            parallel: plan.parallel.clone(),
            state,
            statistics: Arc::new(ServiceStatistics::new(plan.parallel.is_some())),
            stop: Arc::new(AtomicBool::new(false)),
            receiver: command_receiver,
            cm119_input_mask: plan.cm119.input_mask,
            clip_led_mask: plan.cm119.clip_led_mask,
            input_sender,
        },
        command_sender,
        input_receiver,
    )
}

fn hardware_service(eeprom_enabled: bool, with_parallel: bool) -> HardwareService {
    let (plan, selected) = selected_hardware(with_parallel);
    HardwareService::new(
        gpio_provider(),
        &plan,
        selected,
        SharedHardwareState::default(),
        HardwareServiceSetup {
            eeprom_enabled,
            sidetone: None,
            initial_remote_radio: None,
        },
    )
}

fn open_hardware_station() -> HardwareStation {
    let mut channel = resolved_hardware_channel();
    let preflight = HardwareStation::preflight(
        &hardware_plan(channel.config()),
        true,
        audio_provider(),
        gpio_provider(),
    )
    .unwrap();
    preflight.apply_startup_tuning(channel.config_mut());
    HardwareStation::open_preflighted(
        station_media(channel),
        audio_provider(),
        gpio_provider(),
        960,
        preflight,
    )
    .unwrap()
}

fn failed_operation(result: Result<(), ServiceFailure>) -> HardwareOperation {
    match result {
        Err(ServiceFailure(operation, _)) => operation,
        Ok(()) => panic!("operation unexpectedly succeeded"),
    }
}

fn remote_radio(receive_hz: u32) -> RemoteRadio {
    RemoteRadio {
        receive_hz,
        transmit_hz: receive_hz,
        receive_ctcss: None,
        transmit_ctcss: None,
        high_power: false,
    }
}

#[test]
fn service_devices_and_output_state_cover_all_hardware_operations() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(true);

    FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
    let operation = match ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()) {
        Err(ServiceFailure(operation, _)) => operation,
        Ok(_) => panic!("CM119 open unexpectedly succeeded"),
    };
    assert_eq!(operation, HardwareOperation::OpenCm119);
    FAILURE.store(FAIL_PARALLEL_OPEN, Ordering::Release);
    let operation = match ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()) {
        Err(ServiceFailure(operation, _)) => operation,
        Ok(_) => panic!("parallel open unexpectedly succeeded"),
    };
    assert_eq!(operation, HardwareOperation::OpenParallel);

    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs
        .apply(OutputCommand::Cm119(1, OutputRequest::Active), &devices)
        .unwrap();
    outputs
        .apply(OutputCommand::Cm119(1, OutputRequest::Inactive), &devices)
        .unwrap();
    outputs
        .apply(OutputCommand::Cm119(1, OutputRequest::Pulse(2)), &devices)
        .unwrap();
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    assert_eq!(
        failed_operation(
            outputs.apply(OutputCommand::Cm119(1, OutputRequest::Pulse(2)), &devices,)
        ),
        HardwareOperation::PulseCm119
    );
    outputs
        .apply(OutputCommand::Parallel(1, OutputRequest::Active), &devices)
        .unwrap();
    outputs
        .apply(
            OutputCommand::Parallel(1, OutputRequest::Inactive),
            &devices,
        )
        .unwrap();
    outputs
        .apply(
            OutputCommand::Parallel(1, OutputRequest::Pulse(2)),
            &devices,
        )
        .unwrap();
    FAILURE.store(FAIL_PARALLEL_PULSE, Ordering::Release);
    assert_eq!(
        failed_operation(outputs.apply(
            OutputCommand::Parallel(1, OutputRequest::Pulse(2)),
            &devices,
        )),
        HardwareOperation::PulseParallel
    );

    outputs.select_channel(3, &mut devices).unwrap();
    FAILURE.store(FAIL_SELECT_CHANNEL, Ordering::Release);
    assert_eq!(
        failed_operation(outputs.select_channel(2, &mut devices)),
        HardwareOperation::SelectChannel
    );
    let (_, selected_without_parallel) = selected_hardware(false);
    let mut devices_without_parallel =
        ServiceDevices::open(gpio_provider(), &selected_without_parallel, None).unwrap();
    assert_eq!(
        failed_operation(outputs.select_channel(1, &mut devices_without_parallel)),
        HardwareOperation::SelectChannel
    );

    let mut no_remote = OutputState::new(&selected, plan.parallel.as_ref(), None);
    no_remote.service_remote_radio(&mut devices, false).unwrap();
    no_remote.configure_remote_radio(remote_radio(146_520_000));
    no_remote.service_remote_radio(&mut devices, true).unwrap();
    no_remote.service_remote_radio(&mut devices, true).unwrap();
    no_remote.service_remote_radio(&mut devices, false).unwrap();
    no_remote.remote_radio_dirty = true;
    FAILURE.store(FAIL_PROGRAM_REMOTE, Ordering::Release);
    assert_eq!(
        failed_operation(no_remote.service_remote_radio(&mut devices, false)),
        HardwareOperation::ProgramRemoteRadio
    );

    no_remote.configure_remote_radio(remote_radio(0));
    no_remote.remote_radio_ptt = None;
    no_remote.service_remote_radio(&mut devices, false).unwrap();
    no_remote.remote_radio_dirty = true;
    no_remote.remote_radio_ptt = Some(true);
    no_remote.service_remote_radio(&mut devices, true).unwrap();
    no_remote.remote_radio_dirty = true;
    no_remote.remote_radio_ptt = Some(true);
    no_remote.service_remote_radio(&mut devices, false).unwrap();
    no_remote.remote_radio_dirty = true;
    no_remote.remote_radio_ptt = Some(true);
    FAILURE.store(FAIL_CLEAR_REMOTE, Ordering::Release);
    assert_eq!(
        failed_operation(no_remote.service_remote_radio(&mut devices, false)),
        HardwareOperation::ClearRemoteRadioTransmit
    );
    assert_eq!(
        failed_operation(no_remote.service_remote_radio(&mut devices_without_parallel, false)),
        HardwareOperation::ProgramRemoteRadio
    );

    let mut clip = OutputState::new(&selected, plan.parallel.as_ref(), None);
    clip.service_clip_led(&devices, 1, 0).unwrap();
    clip.service_clip_led(&devices, 0, 1).unwrap();
    clip.clip_led_until = Some(Instant::now() + CLIP_LED_HOLD);
    clip.service_clip_led(&devices, 1, 2).unwrap();
    clip.clip_led_until = None;
    clip.service_clip_led(&devices, 1, 3).unwrap();
    clip.clip_led_until = None;
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    assert_eq!(
        failed_operation(clip.service_clip_led(&devices, 1, 4)),
        HardwareOperation::PulseCm119
    );

    let state = HardwareTransientState {
        cm119_output_mask: u8::MAX,
        parallel_output_mask: u8::MAX,
        remote_radio: Some(remote_radio(145_000_000)),
    };
    outputs.restore(state, &selected, plan.parallel.as_ref());
    outputs.restore(state, &selected_without_parallel, None);
    devices.fail_safe_unkey(plan.parallel.as_ref(), &outputs);
    outputs.remote_radio = Some(remote_radio(145_000_000));
    devices.fail_safe_unkey(plan.parallel.as_ref(), &outputs);
    devices_without_parallel.fail_safe_unkey(None, &outputs);
}

#[test]
fn one_service_cycle_identifies_each_adapter_failure() {
    let _guard = test_guard();
    reset_test_state();
    let failures = [
        (FAIL_CM119_PUBLISH, HardwareOperation::PublishCm119),
        (FAIL_CM119_SERVICE, HardwareOperation::ServiceCm119),
        (FAIL_CM119_INPUTS, HardwareOperation::ReadCm119),
        (
            FAIL_CM119_STATISTICS,
            HardwareOperation::ReadCm119Statistics,
        ),
        (FAIL_PARALLEL_PUBLISH, HardwareOperation::PublishParallel),
        (FAIL_PARALLEL_SERVICE, HardwareOperation::ServiceParallel),
        (FAIL_PARALLEL_INPUTS, HardwareOperation::ReadParallel),
        (
            FAIL_PARALLEL_STATISTICS,
            HardwareOperation::ReadParallelStatistics,
        ),
    ];
    for (fault, expected) in failures {
        let (plan, selected) = selected_hardware(true);
        let state = SharedHardwareState::default();
        let (worker, _command_sender, _input_receiver) = service_worker(&plan, &selected, state);
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        let mut inputs = InputTracker::default();
        FAILURE.store(fault, Ordering::Release);
        assert_eq!(
            failed_operation(service_once(
                &mut devices,
                &mut outputs,
                &mut inputs,
                None,
                &worker,
            )),
            expected
        );
    }

    let (plan, selected) = selected_hardware(false);
    let state = SharedHardwareState::default();
    let (worker, _command_sender, _input_receiver) = service_worker(&plan, &selected, state);
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    service_once(
        &mut devices,
        &mut outputs,
        &mut InputTracker::default(),
        None,
        &worker,
    )
    .unwrap();

    let paths = audio_provider()
        .cm119_mixer_paths(&selected.cm119.usb_port_path)
        .unwrap()
        .sidetone;
    let mixers = open_paths(audio_provider(), &selected.cm119.usb_port_path, &paths).unwrap();
    let mut sidetone = HardwareSidetone::new(paths, mixers, 500).unwrap().unwrap();
    sidetone.applied = None;
    FAILURE.store(FAIL_MIXER_SWITCH, Ordering::Release);
    service_once(
        &mut devices,
        &mut outputs,
        &mut InputTracker::default(),
        Some(&mut sidetone),
        &worker,
    )
    .unwrap();
    assert_eq!(
        worker.statistics.snapshot().last_failure,
        Some(HardwareOperation::ApplySidetone)
    );

    let (plan, selected) = selected_hardware(true);
    let state = SharedHardwareState::default();
    let (worker, _command_sender, _input_receiver) = service_worker(&plan, &selected, state);
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.clip_events_seen = u64::MAX;
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    assert_eq!(
        failed_operation(service_once(
            &mut devices,
            &mut outputs,
            &mut InputTracker::default(),
            None,
            &worker,
        )),
        HardwareOperation::PulseCm119
    );
    outputs.clip_events_seen = 0;
    outputs.configure_remote_radio(remote_radio(146_520_000));
    FAILURE.store(FAIL_PROGRAM_REMOTE, Ordering::Release);
    assert_eq!(
        failed_operation(service_once(
            &mut devices,
            &mut outputs,
            &mut InputTracker::default(),
            None,
            &worker,
        )),
        HardwareOperation::ProgramRemoteRadio
    );
}

#[test]
fn service_worker_recovers_devices_and_stops_from_each_state() {
    let _guard = test_guard();
    reset_test_state();

    let (plan, selected) = selected_hardware(false);
    let (worker, _command_sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    worker.stop.store(true, Ordering::Release);
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    assert!(
        worker
            .run(
                devices,
                OutputState::new(&selected, None, None),
                InputTracker::default(),
                None,
            )
            .is_none()
    );

    let (plan, selected) = selected_hardware(false);
    let (worker, _command_sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let stop = Arc::clone(&worker.stop);
    let statistics = Arc::clone(&worker.statistics);
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    let handle = std::thread::spawn(move || {
        worker.run(
            devices,
            OutputState::new(&selected, None, None),
            InputTracker::default(),
            None,
        )
    });
    while statistics.snapshot().service_failures == 0 {
        std::thread::yield_now();
    }
    stop.store(true, Ordering::Release);
    handle.thread().unpark();
    assert!(handle.join().unwrap().is_none());

    let (plan, selected) = selected_hardware(false);
    let (worker, _command_sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let stop = Arc::clone(&worker.stop);
    let statistics = Arc::clone(&worker.statistics);
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    let handle = std::thread::spawn(move || {
        worker.run(
            devices,
            OutputState::new(&selected, None, None),
            InputTracker::default(),
            None,
        )
    });
    while statistics.snapshot().service_failures == 0 {
        std::thread::yield_now();
    }
    FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
    handle.thread().unpark();
    while statistics.snapshot().service_failures < 2 {
        std::thread::yield_now();
    }
    handle.thread().unpark();
    while statistics.snapshot().service_cycles < 3 {
        std::thread::yield_now();
    }
    stop.store(true, Ordering::Release);
    handle.thread().unpark();
    assert!(handle.join().unwrap().is_none());
}

#[test]
fn service_commands_and_bounded_responses_cover_each_message() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(true);
    let (worker, sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);

    sender
        .send(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Active,
        )))
        .unwrap();
    let (read_sender, read_receiver) = sync_channel(1);
    sender
        .send(ServiceCommand::ReadEeprom(read_sender))
        .unwrap();
    let (write_sender, write_receiver) = sync_channel(1);
    sender
        .send(ServiceCommand::WriteEeprom(
            EepromImage::default(),
            write_sender,
        ))
        .unwrap();
    sender.send(ServiceCommand::SelectChannel(2)).unwrap();
    sender
        .send(ServiceCommand::ConfigureRemoteRadio(remote_radio(
            146_520_000,
        )))
        .unwrap();
    let (snapshot_sender, snapshot_receiver) = sync_channel(1);
    sender
        .send(ServiceCommand::Snapshot(snapshot_sender))
        .unwrap();
    sender
        .send(ServiceCommand::Restore(HardwareTransientState {
            cm119_output_mask: 1,
            parallel_output_mask: 2,
            remote_radio: None,
        }))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        plan.parallel.as_ref(),
    )
    .unwrap();
    assert!(read_receiver.recv().unwrap().is_ok());
    assert!(write_receiver.recv().unwrap().unwrap().magic_valid);
    assert_eq!(snapshot_receiver.recv().unwrap().cm119_output_mask(), 1);
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        plan.parallel.as_ref(),
    )
    .unwrap();
    drop(sender);
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        plan.parallel.as_ref(),
    )
    .unwrap();

    let (ok_sender, ok_receiver) = sync_channel(1);
    ok_sender.send(Ok::<_, GpioError>(7_u8)).unwrap();
    assert_eq!(
        receive_response(ok_receiver, HardwareOperation::ReadEeprom).unwrap(),
        7
    );
    let (error_sender, error_receiver) = sync_channel(1);
    error_sender.send(Err::<u8, _>(GpioError::Usb)).unwrap();
    assert!(matches!(
        receive_response(error_receiver, HardwareOperation::ReadEeprom),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::ReadEeprom,
            source: GpioError::Usb
        })
    ));
    let (disconnected_sender, disconnected_receiver) = sync_channel::<u8>(1);
    drop(disconnected_sender);
    assert!(matches!(
        receive_value(disconnected_receiver),
        Err(HardwareStationError::NotRunning)
    ));
    let (disconnected_sender, disconnected_receiver) = sync_channel::<Result<u8, GpioError>>(1);
    drop(disconnected_sender);
    assert!(matches!(
        receive_response(disconnected_receiver, HardwareOperation::ReadEeprom),
        Err(HardwareStationError::NotRunning)
    ));
    let (_timeout_sender, timeout_receiver) = sync_channel::<u8>(1);
    assert!(matches!(
        receive_value(timeout_receiver),
        Err(HardwareStationError::ControlTimeout)
    ));

    let (worker, sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    sender
        .send(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Pulse(1),
        )))
        .unwrap();
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    assert_eq!(
        failed_operation(service_commands(
            &worker.receiver,
            &mut OutputState::new(&selected, plan.parallel.as_ref(), None),
            &mut devices,
            &selected,
            plan.parallel.as_ref(),
        )),
        HardwareOperation::PulseCm119
    );

    let (worker, sender, _input_receiver) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    sender.send(ServiceCommand::SelectChannel(1)).unwrap();
    FAILURE.store(FAIL_SELECT_CHANNEL, Ordering::Release);
    assert_eq!(
        failed_operation(service_commands(
            &worker.receiver,
            &mut OutputState::new(&selected, plan.parallel.as_ref(), None),
            &mut devices,
            &selected,
            plan.parallel.as_ref(),
        )),
        HardwareOperation::SelectChannel
    );
}

#[test]
fn hardware_service_reports_lifecycle_queue_and_eeprom_boundaries() {
    let _guard = test_guard();
    reset_test_state();

    let mut disabled = hardware_service(false, false);
    assert!(matches!(
        disabled.read_eeprom(),
        Err(HardwareStationError::EepromDisabled)
    ));
    assert!(matches!(
        disabled.write_eeprom(&mut EepromImage::default()),
        Err(HardwareStationError::EepromDisabled)
    ));
    assert!(matches!(
        disabled.read_eeprom_for_preflight(),
        Err(HardwareStationError::NotRunning)
    ));
    assert!(matches!(
        disabled.transient_state(),
        Err(HardwareStationError::NotRunning)
    ));
    assert_eq!(disabled.take_input_event(), None);
    assert!(matches!(
        disabled.queue_service(ServiceCommand::SelectChannel(1)),
        Err(HardwareStationError::UnavailableOutput)
    ));
    assert!(matches!(
        disabled.queue_service(ServiceCommand::ConfigureRemoteRadio(remote_radio(1))),
        Err(HardwareStationError::UnavailableOutput)
    ));
    disabled
        .restore_transient_state(HardwareTransientState {
            cm119_output_mask: 1,
            parallel_output_mask: 2,
            remote_radio: None,
        })
        .unwrap();

    FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
    assert!(matches!(
        disabled.start(),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::OpenCm119,
            ..
        })
    ));
    let mut parallel_open_failure = hardware_service(true, true);
    FAILURE.store(FAIL_PARALLEL_OPEN, Ordering::Release);
    assert!(matches!(
        parallel_open_failure.start(),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::OpenParallel,
            ..
        })
    ));
    let mut initial_service_failure = hardware_service(true, true);
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    assert!(matches!(
        initial_service_failure.start(),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::PublishCm119,
            ..
        })
    ));

    let mut service = hardware_service(true, true);
    service.start().unwrap();
    service.start().unwrap();
    service
        .restore_transient_state(HardwareTransientState {
            cm119_output_mask: 1,
            parallel_output_mask: 2,
            remote_radio: None,
        })
        .unwrap();
    FAILURE.store(FAIL_EEPROM_READ, Ordering::Release);
    assert_eq!(service.read_eeprom_for_preflight().unwrap(), None);
    FAILURE.store(FAIL_EEPROM_WRITE, Ordering::Release);
    assert!(matches!(
        service.write_eeprom(&mut EepromImage::default()),
        Err(HardwareStationError::Gpio {
            operation: HardwareOperation::WriteEeprom,
            ..
        })
    ));
    service.stop().unwrap();
    service.stop().unwrap();

    let mut queue = hardware_service(true, false);
    let (sender, receiver) = sync_channel(1);
    queue.sender = Some(sender);
    queue.worker = Some(std::thread::spawn(|| None));
    queue
        .queue_service(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Active,
        )))
        .unwrap();
    assert!(matches!(
        queue.queue_service(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Active
        ))),
        Err(HardwareStationError::ControlQueueFull)
    ));
    drop(receiver);
    queue.stop().unwrap();

    let mut disconnected = hardware_service(true, false);
    let (sender, receiver) = sync_channel(1);
    drop(receiver);
    disconnected.sender = Some(sender);
    disconnected.worker = Some(std::thread::spawn(|| None));
    assert!(matches!(
        disconnected.queue_service(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Active
        ))),
        Err(HardwareStationError::NotRunning)
    ));
    disconnected.stop().unwrap();

    let mut panicked = hardware_service(true, false);
    panicked.worker = Some(std::thread::spawn(|| -> Option<HardwareSidetone> {
        panic!("test worker failure")
    }));
    assert!(matches!(
        panicked.stop(),
        Err(HardwareStationError::WorkerPanicked)
    ));
}

#[test]
fn input_edges_and_statistics_cover_online_offline_and_overflow_states() {
    let _guard = test_guard();
    reset_test_state();
    let statistics = ServiceStatistics::new(true);
    let (sender, receiver) = sync_channel(32);
    let plan = ParallelPlan {
        config: usbradioplus_gpio::ParallelConfig {
            transport: usbradioplus_gpio::ParallelTransport::Automatic,
            ppdev_path: None,
            raw_io_base: 0,
            output_enable_mask: 0,
            output_initial_mask: 0,
        },
        ptt_mask: 0,
        input_mask: 0x78,
        carrier_mask: 0,
        ctcss_mask: 0,
    };
    let mut tracker = InputTracker::default();
    tracker.publish(
        Cm119Inputs::default(),
        u8::MAX,
        None,
        Some(&plan),
        &sender,
        &statistics,
    );
    tracker.publish(
        Cm119Inputs {
            online: true,
            gpio_input_mask: u8::MAX,
            ..Cm119Inputs::default()
        },
        u8::MAX,
        Some(ParallelInputs {
            online: true,
            status_mask: 0x78,
        }),
        Some(&plan),
        &sender,
        &statistics,
    );
    let events: Vec<_> = receiver.try_iter().collect();
    assert_eq!(events.len(), 12);
    for pin in [10, 12, 13, 15] {
        assert!(
            events
                .iter()
                .any(|event| { event.input == HardwareInput::Parallel(pin) && event.active })
        );
    }
    tracker.publish(
        Cm119Inputs::default(),
        u8::MAX,
        Some(ParallelInputs::default()),
        Some(&plan),
        &sender,
        &statistics,
    );

    let (full_sender, _full_receiver) = sync_channel(1);
    let mut previous = Some(0);
    publish_changed_inputs(
        &mut previous,
        3,
        3,
        HardwareInput::Parallel,
        &full_sender,
        &statistics,
    );
    let dropped = statistics.snapshot().dropped_input_events;
    assert_eq!(dropped, 1);
    let (disconnected_sender, disconnected_receiver) = sync_channel(1);
    drop(disconnected_receiver);
    publish_changed_inputs(
        &mut Some(0),
        1,
        1,
        HardwareInput::Parallel,
        &disconnected_sender,
        &statistics,
    );
    assert_eq!(statistics.snapshot().dropped_input_events, dropped);

    let cm119 = Cm119Statistics {
        input_read_count: 1,
        output_apply_count: 2,
        usb_error_count: 3,
        ptt_applied: true,
        online: true,
        last_usb_error: -4,
        eeprom_read_count: 5,
        eeprom_write_count: 6,
    };
    statistics.success(cm119);
    let parallel = usbradioplus_gpio::ParallelStatistics {
        input_read_count: 7,
        output_apply_count: 8,
        io_error_count: 9,
        online: true,
        last_io_error: -10,
        applied_output_mask: 11,
    };
    statistics.publish_parallel(parallel);
    statistics.failure(ServiceFailure(
        HardwareOperation::ReadParallel,
        GpioError::Io,
    ));
    let snapshot = statistics.snapshot();
    assert_eq!(snapshot.cm119, cm119);
    assert_eq!(snapshot.parallel, Some(parallel));
    assert_eq!(snapshot.last_failure, Some(HardwareOperation::ReadParallel));
    assert_eq!(snapshot.service_cycles, 2);
    assert_eq!(snapshot.service_failures, 1);

    let without_parallel = ServiceStatistics::new(false).snapshot();
    assert_eq!(without_parallel.parallel, None);
    assert_eq!(without_parallel.last_failure, None);
}

#[test]
fn eeprom_tuning_validates_encodes_and_applies_current_scales() {
    assert!(EepromTuning::new(1_000, 0, 0, 0, 0).is_none());
    assert!(EepromTuning::new(0, 1_000, 0, 0, 0).is_none());
    assert!(EepromTuning::new(0, 0, 1_000, 0, 0).is_none());
    assert!(EepromTuning::new(0, 0, 0, 1_000, 0).is_none());
    assert!(EepromTuning::new(0, 0, 0, 0, 1_000).is_none());

    let tuning = EepromTuning::new(0, 999, 500, 0, 999).unwrap();
    assert_eq!(tuning.receive_mixer_level(), 0);
    assert_eq!(tuning.transmit_a_mixer_level(), 999);
    assert_eq!(tuning.transmit_b_mixer_level(), 500);
    assert_eq!(tuning.transmit_ctcss_level(), 0);
    assert_eq!(tuning.receive_squelch_level(), 999);
    let mut configuration = resolved_hardware_channel().into_config();
    tuning.apply_to(&mut configuration);
    assert_eq!(configuration.station.hardware.input_gain_db, -30.0);
    assert!(configuration.station.hardware.output_a_gain_db > 6.0);
    assert_eq!(configuration.station.hardware.output_b_gain_db, 0.0);
    assert_eq!(configuration.station.ctcss.transmit_peak_dbfs, -90.0);
    assert_eq!(configuration.station.receive.squelch_level, 999);

    let mut image = tuning.encode();
    assert_eq!(EepromTuning::decode(&image), None);
    image.magic_valid = true;
    assert_eq!(EepromTuning::decode(&image), None);
    image.checksum_valid = true;
    assert_eq!(EepromTuning::decode(&image), Some(tuning));
    image.words[EEPROM_TUNING_START_WORD + EEPROM_RECEIVE_MIXER_OFFSET] = 1_000;
    assert_eq!(EepromTuning::decode(&image), None);

    assert_eq!(ctcss_dbfs_to_level(ctcss_level_to_dbfs(63)), 63);
}

#[test]
fn hardware_station_preflights_runs_controls_and_stops_cleanly() {
    let _guard = test_guard();
    reset_test_state();

    let mut channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());
    let no_eeprom =
        HardwareStation::preflight(&plan, false, audio_provider(), gpio_provider()).unwrap();
    assert!(!no_eeprom.apply_startup_tuning(channel.config_mut()));
    let preflight =
        HardwareStation::preflight(&plan, true, audio_provider(), gpio_provider()).unwrap();
    assert!(preflight.apply_startup_tuning(channel.config_mut()));
    assert_eq!(channel.config().station.receive.squelch_level, 654);

    let mut station = HardwareStation::open_preflighted(
        station_media(channel),
        audio_provider(),
        gpio_provider(),
        960,
        preflight,
    )
    .unwrap();
    assert_eq!(station.mixer_level(HardwareMixer::Receive).unwrap(), 321);
    station
        .set_mixer_level(HardwareMixer::Receive, 222)
        .unwrap();
    station
        .set_mixer_level(HardwareMixer::TransmitA, 333)
        .unwrap();
    station
        .set_mixer_level(HardwareMixer::TransmitB, 444)
        .unwrap();
    assert_eq!(station.mixer_level(HardwareMixer::Receive).unwrap(), 222);
    assert_eq!(station.mixer_level(HardwareMixer::TransmitA).unwrap(), 333);
    assert_eq!(station.mixer_level(HardwareMixer::TransmitB).unwrap(), 444);

    station.start().unwrap();
    station.start().unwrap();
    let mut replacement_configuration = resolved_hardware_channel().into_config();
    let without_eeprom = station
        .preflight_replacement(&plan, false, audio_provider(), gpio_provider())
        .unwrap();
    assert!(!without_eeprom.apply_startup_tuning(&mut replacement_configuration));
    EEPROM_FLAGS.store(1, Ordering::Release);
    let invalid_eeprom = station
        .preflight_replacement(&plan, true, audio_provider(), gpio_provider())
        .unwrap();
    assert!(!invalid_eeprom.apply_startup_tuning(&mut replacement_configuration));
    EEPROM_FLAGS.store(3, Ordering::Release);
    DEVICE_INTERFACE_MODE.store(1, Ordering::Release);
    let different_device = station
        .preflight_replacement(&plan, false, audio_provider(), gpio_provider())
        .unwrap();
    assert!(!different_device.apply_startup_tuning(&mut replacement_configuration));
    DEVICE_INTERFACE_MODE.store(0, Ordering::Release);
    let replacement = station
        .preflight_replacement(&plan, true, audio_provider(), gpio_provider())
        .unwrap();
    assert!(replacement.apply_startup_tuning(&mut replacement_configuration));
    assert_eq!(replacement_configuration.station.receive.squelch_level, 654);
    let forced = CtcssTone::from_tenths_hz(1_000).unwrap();
    station
        .apply_control(ControlMessage::TransmitKey(Some(forced)))
        .unwrap();
    station
        .apply_control(ControlMessage::ReceiveCtcss(false))
        .unwrap();
    station
        .apply_control(ControlMessage::TransmitCtcss(false))
        .unwrap();
    let _ = station.control();
    station
        .apply_control(ControlMessage::SelectChannel(2))
        .unwrap();
    station
        .apply_control(ControlMessage::Gpio(
            GpioPin::new(1).unwrap(),
            OutputRequest::Active,
        ))
        .unwrap();
    station
        .apply_control(ControlMessage::Parallel(
            ParallelPin::new(2).unwrap(),
            OutputRequest::Active,
        ))
        .unwrap();
    station
        .apply_control(ControlMessage::RemoteRadio(remote_radio(146_520_000)))
        .unwrap();
    let requests = station.hardware_state().requests();
    assert!(requests.transmit);
    assert!(requests.render_admitted);
    assert!(requests.subaudible_override);
    assert!(requests.ctcss_inhibit);
    assert_eq!(requests.forced_ctcss, Some(forced));

    station.set_calibrated_test_tone(true);
    assert!(station.hardware_state().requests().calibrated_test_tone);
    station
        .set_gpio(GpioPin::new(1).unwrap(), OutputRequest::Active)
        .unwrap();
    station
        .set_parallel(ParallelPin::new(2).unwrap(), OutputRequest::Pulse(1))
        .unwrap();
    station.select_channel(3).unwrap();
    station
        .configure_remote_radio(RemoteRadio {
            receive_hz: 146_520_000,
            transmit_hz: 146_520_000,
            receive_ctcss: None,
            transmit_ctcss: None,
            high_power: false,
        })
        .unwrap();
    std::thread::sleep(Duration::from_millis(20));
    let transient = station.transient_state().unwrap();
    assert_eq!(transient.cm119_output_mask() & 0x01, 0x01);
    assert_eq!(transient.parallel_output_mask() & 0xf0, 0xc0);
    assert_eq!(
        transient.remote_radio(),
        Some(RemoteRadio {
            receive_hz: 146_520_000,
            transmit_hz: 146_520_000,
            receive_ctcss: None,
            transmit_ctcss: None,
            high_power: false,
        })
    );

    let mut eeprom = station.read_eeprom().unwrap();
    eeprom.words[52] = 456;
    station.write_eeprom(&mut eeprom).unwrap();
    assert!(eeprom.magic_valid);
    assert_eq!(
        station.save_current_tuning_to_eeprom().unwrap(),
        EepromTuning {
            receive_mixer_level: 222,
            transmit_a_mixer_level: 333,
            transmit_b_mixer_level: 444,
            transmit_ctcss_level: 63,
            receive_squelch_level: 654,
        }
    );
    GPIO_INPUTS.store(0x02, Ordering::Release);
    PARALLEL_INPUTS.store(0x40, Ordering::Release);
    std::thread::sleep(Duration::from_millis(20));
    let events = [station.take_input_event(), station.take_input_event()];
    assert!(events.contains(&Some(HardwareInputEvent {
        input: HardwareInput::Cm119(GpioPin::new(2).unwrap()),
        active: true,
    })));
    assert!(events.contains(&Some(HardwareInputEvent {
        input: HardwareInput::Parallel(10),
        active: true,
    })));

    let diagnostics = station.diagnostics().unwrap();
    assert!(diagnostics.hardware.service_cycles > 0);
    assert_eq!(diagnostics.runtime.audio.callback_count, 2);
    assert_eq!(diagnostics.timing.sample_rate_hz, 48_000.0);
    assert_eq!(diagnostics.receive.input_peak, 0.5);
    station
        .apply_control(ControlMessage::TransmitUnkey)
        .unwrap();
    assert!(!station.hardware_state().requests().transmit);
    std::thread::sleep(Duration::from_millis(20));
    let transient = station.transient_state().unwrap();
    station.stop().unwrap();
    let stopped_replacement = station
        .preflight_replacement(&plan, false, audio_provider(), gpio_provider())
        .unwrap();
    assert!(!stopped_replacement.apply_startup_tuning(&mut replacement_configuration));
    station.restore_transient_state(transient).unwrap();
    station.start().unwrap();
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(station.transient_state().unwrap(), transient);
    station.stop().unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 0);
    assert!(matches!(
        station.set_gpio(GpioPin::new(1).unwrap(), OutputRequest::Active),
        Err(HardwareStationError::NotRunning)
    ));
    assert!(matches!(
        station.set_gpio(GpioPin::new(2).unwrap(), OutputRequest::Active),
        Err(HardwareStationError::UnavailableOutput)
    ));
    assert!(matches!(
        station.set_gpio(GpioPin::new(1).unwrap(), OutputRequest::Pulse(u32::MAX)),
        Err(HardwareStationError::PulseTooLong)
    ));
}
