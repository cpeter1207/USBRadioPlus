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
pub(super) static FAILURE: AtomicU32 = AtomicU32::new(0);
static MIXER_PATHS_MODE: AtomicU32 = AtomicU32::new(0);
static DEVICE_INTERFACE_MODE: AtomicU32 = AtomicU32::new(0);
static EEPROM_FLAGS: AtomicU32 = AtomicU32::new(3);
static EEPROM_WRITES: AtomicU32 = AtomicU32::new(0);
static NATIVE_IDENTITY_MODE: AtomicU32 = AtomicU32::new(0);
static MIXER_LEVELS: [AtomicU32; 3] = [const { AtomicU32::new(500) }; 3];
static MIXER_LEVEL_WRITES: AtomicU32 = AtomicU32::new(0);
static MIXER_SWITCH_WRITES: AtomicU32 = AtomicU32::new(0);
static TUNING_IO_CALLS: AtomicU32 = AtomicU32::new(0);
static GPIO_OPENS: AtomicU32 = AtomicU32::new(0);
static GPIO_CLOSES: AtomicU32 = AtomicU32::new(0);
static SHUTDOWN_PULSE_STAGE: AtomicU32 = AtomicU32::new(0);
static SHUTDOWN_SERVICE_FAILURES: AtomicU32 = AtomicU32::new(0);
static PARALLEL_APPLIED_MASK: AtomicU32 = AtomicU32::new(0);
static GPIO_INITIAL_BYTE: AtomicU32 = AtomicU32::new(0);
static GPIO_PUBLISHES: AtomicU32 = AtomicU32::new(0);
static GPIO_ONLINE: AtomicU32 = AtomicU32::new(1);
static GPIO_APPLIED_PTT: AtomicU32 = AtomicU32::new(u32::MAX);
static GPIO_FINAL_BYTE: AtomicU32 = AtomicU32::new(0);
static PARALLEL_FINAL_BYTE: AtomicU32 = AtomicU32::new(0);
static PARALLEL_CLOSE_BLOCKED: AtomicU32 = AtomicU32::new(0);
static PARALLEL_RAW_CLOSES: AtomicU32 = AtomicU32::new(0);
static PARALLEL_REMOTE_RECEIVE: AtomicU32 = AtomicU32::new(0);

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
pub(super) const FAIL_CM119_STATISTICS: u32 = 15;
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
const FAIL_PARALLEL_CLOSE: u32 = 28;
const FAIL_CM119_CAPABILITIES: u32 = 29;
const FAIL_PARALLEL_CAPABILITIES: u32 = 30;
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

pub(super) fn reset_test_state() {
    GPIO_INPUTS.store(0, Ordering::Release);
    PARALLEL_INPUTS.store(0, Ordering::Release);
    PTT.store(0, Ordering::Release);
    FAILURE.store(0, Ordering::Release);
    MIXER_PATHS_MODE.store(0, Ordering::Release);
    DEVICE_INTERFACE_MODE.store(0, Ordering::Release);
    EEPROM_FLAGS.store(3, Ordering::Release);
    EEPROM_WRITES.store(0, Ordering::Release);
    NATIVE_IDENTITY_MODE.store(0, Ordering::Release);
    for level in &MIXER_LEVELS {
        level.store(500, Ordering::Release);
    }
    MIXER_LEVEL_WRITES.store(0, Ordering::Release);
    MIXER_SWITCH_WRITES.store(0, Ordering::Release);
    TUNING_IO_CALLS.store(0, Ordering::Release);
    GPIO_OPENS.store(0, Ordering::Release);
    GPIO_CLOSES.store(0, Ordering::Release);
    SHUTDOWN_PULSE_STAGE.store(0, Ordering::Release);
    SHUTDOWN_SERVICE_FAILURES.store(0, Ordering::Release);
    PARALLEL_APPLIED_MASK.store(0, Ordering::Release);
    GPIO_INITIAL_BYTE.store(0, Ordering::Release);
    GPIO_PUBLISHES.store(0, Ordering::Release);
    GPIO_ONLINE.store(1, Ordering::Release);
    GPIO_APPLIED_PTT.store(u32::MAX, Ordering::Release);
    GPIO_FINAL_BYTE.store(0, Ordering::Release);
    PARALLEL_FINAL_BYTE.store(0, Ordering::Release);
    PARALLEL_CLOSE_BLOCKED.store(0, Ordering::Release);
    PARALLEL_RAW_CLOSES.store(0, Ordering::Release);
    PARALLEL_REMOTE_RECEIVE.store(0, Ordering::Release);
}

pub(super) fn test_guard() -> std::sync::MutexGuard<'static, ()> {
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
        (
            HardwareOperation::ConfirmCm119Ptt,
            "confirm serviced CM119 PTT",
        ),
        (
            HardwareOperation::ConfirmParallelPtt,
            "confirm serviced parallel PTT",
        ),
        (HardwareOperation::RemoteControl, "service remote radio"),
    ];
    assert_eq!(HardwareOperation::from_u8(0), None);
    assert_eq!(HardwareOperation::from_u8(22), None);
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
    let gpio_probe = HardwareStationError::GpioProbe(GpioError::Usb);
    assert!(
        gpio_probe
            .to_string()
            .starts_with("probe CM119 identity failed:")
    );
    assert!(gpio_probe.source().is_some());
    let missing_gpio = HardwareStationError::GpioNotPresent;
    assert_eq!(
        missing_gpio.to_string(),
        "selected CM119 is not present on GPIO"
    );
    assert!(missing_gpio.source().is_none());
    let update = HardwareStationError::Update(StationUpdateError::Busy);
    assert_eq!(
        update.to_string(),
        "station update failed: another live update is awaiting completion"
    );
    assert!(update.source().is_some());
    let simple = [
        (
            HardwareStationError::Site(SiteError::Fault),
            "site hardware request failed: Fault",
        ),
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
fn configured_hardware_gains_validate_before_adapter_access_and_skip_omitted_controls() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    let selected = audio_provider()
        .select_device(&plan.audio_selector)
        .unwrap();
    FAILURE.store(FAIL_AUDIO_PATHS, Ordering::Release);
    for gain in [-30.01, 30.01, f64::NAN, f64::INFINITY] {
        assert!(matches!(
            HardwareMixers::apply_native_gains(
                audio_provider(),
                selected.interface_path.as_deref().unwrap(),
                [Some(gain), None, None],
                None
            ),
            Err(AudioError::InvalidArgument)
        ));
    }
    HardwareMixers::apply_native_gains(
        audio_provider(),
        selected.interface_path.as_deref().unwrap(),
        [None; 3],
        None,
    )
    .unwrap();
    assert_eq!(FAILURE.swap(0, Ordering::AcqRel), FAIL_AUDIO_PATHS);
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), 0);
    HardwareMixers::apply_native_gains(
        audio_provider(),
        selected.interface_path.as_deref().unwrap(),
        [Some(3.0), None, Some(-3.0)],
        None,
    )
    .unwrap();
    let configured = HardwareMixers::native(audio_provider(), &selected).unwrap();
    assert_eq!(
        configured.levels,
        [mixer_level(3.0), 500, mixer_level(-3.0)]
    );
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), 2);
    assert_eq!(MIXER_SWITCH_WRITES.load(Ordering::Acquire), 0);
    for (mode, expected) in [
        (1, AudioError::Unsupported),
        (3, AudioError::AdapterFailure),
    ] {
        MIXER_PATHS_MODE.store(mode, Ordering::Release);
        assert_eq!(
            HardwareMixers::native(audio_provider(), &selected).err(),
            Some(expected)
        );
    }
    reset_test_state();
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
        .cm119_mixer_paths(selected.interface_path.as_deref().unwrap())
        .unwrap()
        .sidetone;
    let mixers = open_paths(
        provider,
        selected.interface_path.as_deref().unwrap(),
        &paths,
    )
    .unwrap();
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
    mismatched.cm119.as_mut().unwrap().required_usb_port_path = Some("another-device".into());
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
        .cm119_mixer_paths(selected.interface_path.as_deref().unwrap())
        .unwrap()
        .sidetone;
    let sidetone_mixers = open_paths(
        audio_provider(),
        selected.interface_path.as_deref().unwrap(),
        &paths,
    )
    .unwrap();
    FAILURE.store(FAIL_MIXER_LEVEL, Ordering::Release);
    assert!(HardwareSidetone::new(paths, sidetone_mixers, 500).is_err());

    let paths = audio_provider()
        .cm119_mixer_paths(selected.interface_path.as_deref().unwrap())
        .unwrap()
        .sidetone;
    let sidetone_mixers = open_paths(
        audio_provider(),
        selected.interface_path.as_deref().unwrap(),
        &paths,
    )
    .unwrap();
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
    alsa_device_resolve:
        Option<unsafe extern "C" fn(*const c_char, u32, u32, *mut AudioDeviceSelection) -> c_int>,
}

// SAFETY: the test descriptor and all referenced functions are immutable.
unsafe impl Sync for AudioDescriptor {}

struct FakeStream {
    receive: unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32,
    receive_context: *mut c_void,
    transmit: unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32,
    transmit_context: *mut c_void,
}
static LAST_STREAM: std::sync::atomic::AtomicPtr<c_void> =
    std::sync::atomic::AtomicPtr::new(ptr::null_mut());

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
    LAST_STREAM.store(*output, Ordering::Release);
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
    TUNING_IO_CALLS.fetch_add(1, Ordering::AcqRel);
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
            normalized: if (1..=3).contains(&kind) {
                MIXER_LEVELS[kind as usize - 1].load(Ordering::Acquire)
            } else {
                500
            },
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
    MIXER_LEVEL_WRITES.fetch_add(1, Ordering::AcqRel);
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
    if (1..=3).contains(&kind) {
        MIXER_LEVELS[kind as usize - 1].store((value / 2) as u32, Ordering::Release);
    }
    OK
}

unsafe extern "C" fn audio_mixer_get(mixer: *const c_void, value: *mut u32) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).normalized };
    OK
}

unsafe extern "C" fn audio_mixer_set(mixer: *mut c_void, value: u32) -> c_int {
    MIXER_LEVEL_WRITES.fetch_add(1, Ordering::AcqRel);
    if fail_once(FAIL_MIXER_LEVEL) {
        return -1;
    }
    // SAFETY: the handle is exclusively owned by the wrapper.
    unsafe { (*mixer.cast::<FakeMixer>()).normalized = value };
    // SAFETY: the uniquely owned fake handle supplies its selected hardware path.
    let kind = unsafe { (*mixer.cast::<FakeMixer>()).kind };
    if (1..=3).contains(&kind) {
        MIXER_LEVELS[kind as usize - 1].store(value, Ordering::Release);
    }
    OK
}

unsafe extern "C" fn audio_mixer_get_switch(mixer: *const c_void, value: *mut u32) -> c_int {
    // SAFETY: both pointers originate from the wrapper.
    unsafe { *value = (*mixer.cast::<FakeMixer>()).enabled };
    OK
}

unsafe extern "C" fn audio_mixer_set_switch(mixer: *mut c_void, value: u32) -> c_int {
    MIXER_SWITCH_WRITES.fetch_add(1, Ordering::AcqRel);
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
    if !matches!(NATIVE_IDENTITY_MODE.load(Ordering::Acquire), 5 | 6) {
        copy_c_string(&mut output.serial, b"ABC");
    }
    output.selection = audio_device_selection();
    OK
}

fn audio_device_selection() -> AudioDeviceSelection {
    AudioDeviceSelection {
        struct_size: size_of::<AudioDeviceSelection>() as u32,
        abi_version: AUDIO_ABI,
        alsa_card_index: 1,
        input_device_index: 2,
        output_device_index: 3,
    }
}

unsafe extern "C" fn audio_alsa_device_resolve(
    _name: *const c_char,
    _input_channels: u32,
    _output_channels: u32,
    selected: *mut AudioDeviceSelection,
) -> c_int {
    if fail_once(FAIL_AUDIO_SELECT) {
        return -1;
    }
    // SAFETY: the audio wrapper supplies writable storage for the complete
    // selection. This endpoint reports ALSA indices, with no USB/HID identity.
    unsafe { selected.write(audio_device_selection()) };
    OK
}

unsafe extern "C" fn audio_mixer_paths(
    _interface: *const c_char,
    output: *mut AudioMixerPaths,
) -> c_int {
    TUNING_IO_CALLS.fetch_add(1, Ordering::AcqRel);
    if fail_once(FAIL_AUDIO_PATHS) {
        return -1;
    }
    // SAFETY: the wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = AUDIO_ABI;
    let mode = MIXER_PATHS_MODE.load(Ordering::Acquire);
    output.receive_count = if mode == 3 { 0 } else { 1 };
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
    alsa_device_resolve: Some(audio_alsa_device_resolve),
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
#[derive(Clone, Copy)]
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
    context_operations: [Option<unsafe extern "C" fn()>; 5],
    device_capabilities: Option<
        unsafe extern "C" fn(*const c_void, *mut usbradioplus_gpio::GpioCapabilities) -> c_int,
    >,
    parallel_capabilities: Option<
        unsafe extern "C" fn(*const c_void, *mut usbradioplus_gpio::GpioCapabilities) -> c_int,
    >,
    interlock_operations: [Option<unsafe extern "C" fn()>; 8],
    parallel_close_outputs: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
}

// SAFETY: the test descriptor and all referenced functions are immutable.
unsafe impl Sync for GpioDescriptor {}

#[repr(C)]
struct GpioDeviceConfig {
    struct_size: u32,
    abi_version: u32,
    usb_port_path: *const c_char,
    vendor_id: u16,
    product_id: u16,
    profile: u32,
    ptt_inverted: u32,
    output_enable_mask: u32,
    output_initial_mask: u32,
}

#[repr(C)]
struct GpioDeviceInfo {
    struct_size: u32,
    abi_version: u32,
    present: u32,
    vendor_id: u16,
    product_id: u16,
    usb_bus: u32,
    usb_port_number_count: u32,
    usb_port_numbers: [u8; 7],
    serial: [c_char; 128],
}

unsafe extern "C" fn gpio_probe(config: *const c_void, output: *mut c_void) -> c_int {
    // SAFETY: the validated wrapper supplies matching complete ABI records.
    let (config, output) = unsafe {
        (
            &*config.cast::<GpioDeviceConfig>(),
            &mut *output.cast::<GpioDeviceInfo>(),
        )
    };
    let mode = NATIVE_IDENTITY_MODE.load(Ordering::Acquire);
    // SAFETY: the synchronous probe borrows a live NUL-terminated USB path.
    if unsafe { CStr::from_ptr(config.usb_port_path) }.to_bytes() != b"3-1" || mode == 4 {
        return -4;
    }
    output.abi_version = if mode == 7 { 99 } else { GPIO_ABI };
    output.present = u32::from(mode != 1);
    output.vendor_id = 0x0d8c;
    output.product_id = 0x000c;
    output.usb_bus = 3;
    output.usb_port_number_count = 1;
    output.usb_port_numbers[0] = 1;
    if !matches!(mode, 3 | 6) {
        copy_c_string(
            &mut output.serial,
            if mode == 2 { b"OTHER" } else { b"ABC" },
        );
    }
    OK
}

unsafe extern "C" fn gpio_discover(_output: *mut c_void) -> c_int {
    OK
}

unsafe extern "C" fn gpio_open(config: *const c_void, output: *mut *mut c_void) -> c_int {
    GPIO_OPENS.fetch_add(1, Ordering::AcqRel);
    GPIO_INITIAL_BYTE.store(
        // SAFETY: the wrapper supplies a complete live CM119 configuration record.
        unsafe { (*config.cast::<GpioDeviceConfig>()).output_initial_mask },
        Ordering::Release,
    );
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
    GPIO_PUBLISHES.fetch_add(1, Ordering::AcqRel);
    if fail_once(FAIL_CM119_PUBLISH) {
        return -1;
    }
    // SAFETY: the wrapper supplies a complete output snapshot.
    PTT.store(unsafe { (*outputs).ptt_asserted }, Ordering::Release);
    // SAFETY: same complete borrowed output record.
    GPIO_FINAL_BYTE.store(unsafe { (*outputs).gpio_output_mask }, Ordering::Release);
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
            online: GPIO_ONLINE.load(Ordering::Acquire),
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
            ptt_applied: match GPIO_APPLIED_PTT.load(Ordering::Acquire) {
                u32::MAX => PTT.load(Ordering::Acquire),
                applied => applied,
            },
            online: 1,
            ..GpioCm119Statistics::default()
        };
    }
    OK
}

unsafe extern "C" fn gpio_close(device: *mut c_void) {
    GPIO_CLOSES.fetch_add(1, Ordering::AcqRel);
    // SAFETY: the handle is the unique allocation returned by open.
    drop(unsafe { Box::from_raw(device.cast::<u8>()) });
}

unsafe extern "C" fn parallel_close_outputs(device: *mut c_void, output: u32) -> c_int {
    if PARALLEL_CLOSE_BLOCKED.load(Ordering::Acquire) != 0 || fail_once(FAIL_PARALLEL_CLOSE) {
        return -5;
    }
    PARALLEL_FINAL_BYTE.store(output, Ordering::Release);
    // SAFETY: a successful final write consumes the unique handle.
    unsafe { gpio_close(device) };
    OK
}

unsafe extern "C" fn parallel_close(device: *mut c_void) {
    PARALLEL_RAW_CLOSES.fetch_add(1, Ordering::AcqRel);
    PARALLEL_FINAL_BYTE.store(0, Ordering::Release);
    // SAFETY: ordinary close consumes the unique handle and drives all data low.
    unsafe { gpio_close(device) };
}

unsafe extern "C" fn gpio_read_eeprom(_device: *mut c_void, image: *mut GpioEeprom) -> c_int {
    TUNING_IO_CALLS.fetch_add(1, Ordering::AcqRel);
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
    EEPROM_WRITES.fetch_add(1, Ordering::AcqRel);
    TUNING_IO_CALLS.fetch_add(1, Ordering::AcqRel);
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
            applied_output_mask: PARALLEL_APPLIED_MASK.load(Ordering::Acquire),
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
    receive_hz: u32,
    _transmit_hz: u32,
    _transmitting: u32,
    _high_power: u32,
) -> c_int {
    if fail_once(FAIL_PROGRAM_REMOTE) {
        return -1;
    }
    PARALLEL_REMOTE_RECEIVE.store(receive_hz, Ordering::Release);
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

unsafe extern "C" fn gpio_capabilities(
    _device: *const c_void,
    output: *mut usbradioplus_gpio::GpioCapabilities,
) -> c_int {
    if fail_once(FAIL_CM119_CAPABILITIES) {
        return -1;
    }
    // SAFETY: the wrapper supplies a complete capability result.
    unsafe {
        *output = usbradioplus_gpio::GpioCapabilities {
            struct_size: size_of::<usbradioplus_gpio::GpioCapabilities>() as u32,
            inputs: 3,
            outputs: 1,
            input_pin_mask: 0xfb,
            output_pin_mask: 0xfb,
            ..usbradioplus_gpio::GpioCapabilities::default()
        };
    }
    OK
}
unsafe extern "C" fn parallel_capabilities(
    _device: *const c_void,
    output: *mut usbradioplus_gpio::GpioCapabilities,
) -> c_int {
    if fail_once(FAIL_PARALLEL_CAPABILITIES) {
        return -1;
    }
    // SAFETY: the wrapper supplies a complete capability result.
    unsafe {
        *output = usbradioplus_gpio::GpioCapabilities {
            struct_size: size_of::<usbradioplus_gpio::GpioCapabilities>() as u32,
            input_pin_mask: 0xf800,
            output_pin_mask: 0xff,
            ..usbradioplus_gpio::GpioCapabilities::default()
        };
    }
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
    parallel_close: Some(parallel_close),
    old_gpio_pulse: Some(unused_gpio_pulse),
    old_parallel_pulse: Some(unused_gpio_pulse),
    gpio_pulse: Some(gpio_pulse),
    parallel_pulse: Some(parallel_pulse),
    set_channel: Some(parallel_set_channel),
    program_rtx: Some(parallel_program_rtx),
    clear_rtx: Some(parallel_control),
    context_operations: [None; 5],
    device_capabilities: Some(gpio_capabilities),
    parallel_capabilities: Some(parallel_capabilities),
    interlock_operations: [None; 8],
    parallel_close_outputs: Some(parallel_close_outputs),
};

fn audio_provider() -> AudioProvider {
    // SAFETY: the descriptor and every referenced function have process lifetime.
    unsafe { AudioProvider::from_raw_descriptor(ptr::from_ref(&AUDIO_DESCRIPTOR).cast()) }.unwrap()
}

pub(super) fn gpio_provider() -> GpioAdapter {
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

#[test]
fn live_update_compatibility_and_startup_tuning_preserve_explicit_choices() {
    let _guard = test_guard();
    reset_test_state();
    let channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());
    let preflight =
        HardwareStation::preflight(&plan, false, audio_provider(), gpio_provider()).unwrap();
    let mut station = HardwareStation::open_preflighted(
        station_media(channel),
        audio_provider(),
        gpio_provider(),
        960,
        preflight,
    )
    .unwrap();
    let current = station.control().plan().clone();
    let candidate = |configuration, transport| {
        StationPlan::new("usb", configuration, transport, 2, 960).unwrap()
    };
    assert!(station.supports_live_update(&current));
    assert!(!station.supports_live_update(&candidate(
        current.configuration().clone(),
        ControllerTransport::AppRpt,
    )));
    let mut changed = current.configuration().clone();
    changed.station.hardware.input_extra_buffer_ms += 1;
    assert!(!station.supports_live_update(&candidate(changed, current.transport())));
    let mut changed = current.configuration().clone();
    changed.station.hardware.eeprom_enabled = false;
    assert!(!station.supports_live_update(&candidate(changed, current.transport())));
    let mut changed = current.configuration().clone();
    changed.station.receive.frequency_hz = 146_520_000;
    changed.station.transmit.frequency_hz = 146_520_000;
    assert!(!station.supports_live_update(&candidate(changed, current.transport())));

    let previous = current.configuration().clone();
    let mut explicit = previous.clone();
    explicit.station.hardware.input_gain_db += 1.0;
    explicit.station.receive.squelch_level += 1;
    explicit.station.ctcss.transmit_peak_dbfs -= 1.0;
    station.preserve_startup_tuning(&previous, &mut explicit);
    assert_eq!(
        explicit.station.hardware.input_gain_db,
        previous.station.hardware.input_gain_db + 1.0
    );
    assert_eq!(
        explicit.station.receive.squelch_level,
        previous.station.receive.squelch_level + 1
    );
    assert_eq!(
        explicit.station.ctcss.transmit_peak_dbfs,
        previous.station.ctcss.transmit_peak_dbfs - 1.0
    );
    let mut unchanged = previous.clone();
    station.preserve_startup_tuning(&previous, &mut unchanged);
    station.finish_update(true).unwrap();

    let factory = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let update = factory
        .update_snapshot(&station)
        .unwrap()
        .prepare(current.clone())
        .unwrap();
    station.begin_update(update, [true, false, false]).unwrap();
    station.finish_update(true).unwrap();
    let mut changed = current.configuration().clone();
    changed.station.hardware.input_gain_db += 1.0;
    let update = factory
        .update_snapshot(&station)
        .unwrap()
        .prepare(candidate(changed, current.transport()))
        .unwrap();
    station.begin_update(update, [false; 3]).unwrap();
    station.finish_update(true).unwrap();
    let update = factory
        .update_snapshot(&station)
        .unwrap()
        .prepare(station.control().plan().clone())
        .unwrap();
    station.begin_update(update, [false; 3]).unwrap();
    station.finish_update(false).unwrap();
}

#[test]
fn app_rpt_local_repeat_change_requires_hardware_handoff() {
    let _guard = test_guard();
    reset_test_state();
    let channel = resolved_hardware_channel();
    let plan = hardware_plan(channel.config());
    let preflight =
        HardwareStation::preflight(&plan, false, audio_provider(), gpio_provider()).unwrap();
    let factory = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let mut station = HardwareStation::open_preflighted(
        factory
            .prepare(
                channel,
                1,
                ControllerConfiguration::AppRpt {
                    handoff_slots: 2,
                    echo: usbradioplus_asl3::EchoConfiguration::disabled(),
                },
            )
            .unwrap(),
        audio_provider(),
        gpio_provider(),
        960,
        preflight,
    )
    .unwrap();
    let current = station.control().plan().clone();
    let mut changed = current.configuration().clone();
    changed.station.duplex.local_repeat_level += 1;
    let changed = StationPlan::new("usb", changed, current.transport(), 2, 960).unwrap();
    assert!(!station.supports_live_update(&changed));
}

pub(super) fn selected_hardware(with_parallel: bool) -> (HardwarePlan, SelectedHardwarePlan) {
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

#[test]
fn remote_generation_uses_actual_caps_and_confirms_selected_unkey() {
    use crate::{RemoteQueue, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::CarrierSource;
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let (setup, _control) = RemoteSetup::prepare(
        configuration(),
        plan.clone(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    let state = SharedHardwareState::default();
    let (worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    let mut generation = remote::RemoteGeneration::open(
        setup,
        Arc::new(RemoteQueue::new(4)),
        &worker,
        &mut devices,
        &mut outputs,
    )
    .unwrap();
    assert_eq!(generation.wiring().ptt, RemotePttSource::Rig);
    generation
        .unkey(&worker, &mut devices, &mut outputs)
        .unwrap();
    assert_eq!(state.remote_signals().physical_ptt, Some(false));
    assert!(state.remote_signals().ptt_selected);
    assert!(!state.remote_control().receive_permitted);
    generation
        .retire(&worker, &mut devices, &mut outputs)
        .unwrap();
    assert_eq!(
        generation.retire(&worker, &mut devices, &mut outputs),
        Ok(()),
        "a consumed CAT handle must not leave retirement stuck"
    );
}

#[test]
fn remote_generation_keeps_audio_closed_when_dsp_owner_is_gone() {
    use crate::{RemoteQueue, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::CarrierSource;
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let (setup, control) = RemoteSetup::prepare(
        configuration(),
        plan.clone(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    drop(control);
    let state = SharedHardwareState::default();
    let (worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    let mut generation = remote::RemoteGeneration::open(
        setup,
        Arc::new(RemoteQueue::new(4)),
        &worker,
        &mut devices,
        &mut outputs,
    )
    .unwrap();
    assert_eq!(
        generation.service(&worker, &mut devices, &mut outputs),
        Err(usbradioplus_remote::CallError(-1102))
    );
    let policy = state.remote_control();
    assert!(policy.enabled && !policy.receive_permitted && !policy.transmit_permitted);
    assert_ne!(generation.snapshot().unwrap().failure_code, 0);
    generation
        .retire(&worker, &mut devices, &mut outputs)
        .unwrap();
}

#[test]
fn rejected_remote_generation_retries_physical_off_and_returns_its_original_cause() {
    use crate::{RemoteQueue, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::{CarrierSource, RemoteBackend};
    let _guard = test_guard();
    for backend in [RemoteBackend::Hamlib, RemoteBackend::ParallelChannel] {
        reset_test_state();
        let (mut plan, selected) = selected_hardware(backend == RemoteBackend::ParallelChannel);
        let mut configuration = configuration();
        configuration.backend = backend;
        configuration.ptt_source = Some("usb".into());
        if backend == RemoteBackend::Hamlib {
            // The rig opens, then explicit unavailable COR rejects admission.
            configuration.cor_source = Some("parallel".into());
        } else {
            plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
            configuration.memories[0].state.channel = Some(7);
            // Policy permits USB, but the selected channel hardware is FM-only.
            configuration.memories[0].state.mode = 3;
        }
        let (setup, _control) = RemoteSetup::prepare(
            configuration,
            plan.clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        let state = SharedHardwareState::default();
        let (worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        FAILURE.store(FAIL_CM119_STATISTICS, Ordering::Release);
        let error = remote::RemoteGeneration::open(
            setup,
            Arc::new(RemoteQueue::new(4)),
            &worker,
            &mut devices,
            &mut outputs,
        )
        .err();
        assert_eq!(
            error,
            Some(usbradioplus_remote::CallError(
                if backend == RemoteBackend::Hamlib {
                    -6
                } else {
                    -2
                }
            ))
        );
        assert_eq!(FAILURE.load(Ordering::Acquire), 0);
        assert_eq!(PTT.load(Ordering::Acquire), 0);
        assert!(!state.remote_control().transmit_permitted);
    }
}

#[test]
fn remote_station_services_cat_and_adopts_both_callback_graphs() {
    use crate::{RemoteReply, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::{
        CarrierSource,
        remote_operation::{RemoteOperation, RemoteOperationKind},
    };
    let _guard = test_guard();
    reset_test_state();
    let mut station = open_hardware_station();
    let factory = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let mut configuration = configuration();
    // The dummy rig starts with RFPOWER zero, outside its declared TX range.
    // Set an explicit memory power just as an actual configured radio requires.
    configuration.memories[0].state.power_mw = Some(5000);
    configuration.ptt_source = Some("usb".into());
    let (setup, control) = RemoteSetup::prepare(
        configuration,
        station.control().plan().hardware().clone(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    station.bind_remote(setup, control).unwrap();
    assert_eq!(station.maintenance.service.remote_ready(), Ok(false));
    station.start().unwrap();
    let state = station.maintenance.hardware_state();
    let deadline = Instant::now() + Duration::from_secs(5);
    while (!state.remote_control().receive_permitted
        || station.maintenance.service.remote_ready() != Ok(true))
        && Instant::now() < deadline
    {
        factory.service_remote(&mut station);
        // SAFETY: the guarded fake stream and its callback contexts remain owned by station.
        let result = unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) };
        assert_eq!(result, OK);
        thread::sleep(Duration::from_millis(2));
    }
    assert_eq!(station.maintenance.service.remote_error(), 0);
    assert!(state.remote_control().receive_permitted);
    assert_eq!(station.maintenance.service.remote_ready(), Ok(true));
    let mut operation = RemoteOperation::new(RemoteOperationKind::Frequency);
    operation.magnitude = 146_940_000;
    operation.flags = 2;
    let ticket = station
        .maintenance
        .service
        .remote_submit(operation, true, false)
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    let result = loop {
        factory.service_remote(&mut station);
        // SAFETY: same guarded fake stream; neither callback context has retired.
        let result = unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) };
        assert_eq!(result, OK);
        if let Some(result) = ticket.take_completion() {
            break result;
        }
        assert!(Instant::now() < deadline, "remote command did not complete");
        thread::sleep(Duration::from_millis(2));
    };
    if let Err(error) = result {
        panic!("operating change failed: {error:?}");
    }
    assert!(matches!(result, Ok(RemoteReply::Applied)));
    assert_eq!(station.maintenance.service.remote_error(), 0);
    // Lose physical confirmation after the engine has cached a serviced OFF.
    // A subsequent RF change must wait, even if the logical PTT never changed.
    GPIO_APPLIED_PTT.store(1, Ordering::Release);
    state.publish_remote_signals(usbradioplus_station::RemoteHardwareSignals {
        ptt_selected: true,
        physical_ptt: None,
        ..Default::default()
    });
    operation.magnitude = 146_530_000;
    let waiting = station
        .maintenance
        .service
        .remote_submit(operation, true, false)
        .unwrap();
    let deadline = Instant::now() + Duration::from_millis(150);
    let early = loop {
        factory.service_remote(&mut station);
        // SAFETY: both fake callback contexts remain owned by the running station.
        let result = unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) };
        assert_eq!(result, OK);
        if let Some(completion) = waiting.take_completion() {
            break Some(completion);
        }
        if Instant::now() >= deadline {
            break None;
        }
        thread::sleep(Duration::from_millis(2));
    };
    GPIO_APPLIED_PTT.store(u32::MAX, Ordering::Release);
    assert!(early.is_none(), "retuned with physical PTT unknown");
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        factory.service_remote(&mut station);
        // SAFETY: same live fake stream and callback contexts.
        let result = unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) };
        assert_eq!(result, OK);
        if let Some(completion) = waiting.take_completion() {
            assert!(matches!(completion, Ok(RemoteReply::Applied)));
            break;
        }
        assert!(
            Instant::now() < deadline,
            "did not recover after confirmed OFF"
        );
        thread::sleep(Duration::from_millis(2));
    }
    station.stop().unwrap();
    // The deliberately failed physical-OFF confirmation remains the first cause.
    assert_eq!(station.maintenance.service.remote_ready(), Err(-1101));
}

fn remote_station_command(
    station: &mut HardwareStation,
    factory: &StationFactory,
    operation: usbradioplus_core::remote_operation::RemoteOperation,
) -> Result<crate::RemoteReply, crate::RemoteRequestError> {
    let deadline = Instant::now() + Duration::from_secs(5);
    while station.maintenance.service.remote_ready() == Ok(false) {
        factory.service_remote(station);
        assert_eq!(
            // SAFETY: the fixture retains the fake stream and both callback contexts.
            unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) },
            OK
        );
        assert!(
            Instant::now() < deadline,
            "remote activation did not finish"
        );
        thread::sleep(Duration::from_millis(2));
    }
    assert_eq!(station.maintenance.service.remote_ready(), Ok(true));
    let ticket = station
        .maintenance
        .service
        .remote_submit(operation, true, false)
        .unwrap();
    loop {
        factory.service_remote(station);
        assert_eq!(
            // SAFETY: the fixture retains the fake stream and both callback contexts.
            unsafe { audio_stream_start(LAST_STREAM.load(Ordering::Acquire)) },
            OK
        );
        if let Some(result) = ticket.take_completion() {
            return result;
        }
        let state = station.maintenance.hardware_state();
        let activity = state.remote_activity();
        assert!(
            Instant::now() < deadline,
            "operation {} stalled: fault={} local={} linked={} requested={} physical={:?}",
            operation.operation,
            station.maintenance.service.remote_error(),
            activity.local_receiving,
            activity.linked_receiving,
            activity.transmit_requested,
            state.remote_signals().physical_ptt,
        );
        thread::sleep(Duration::from_millis(2));
    }
}

#[test]
fn remote_station_observes_power_changes_modes_and_bounds_manual_tune_with_both_ptt_sources() {
    use crate::{RemoteReply, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::{
        CarrierSource,
        remote_operation::{RemoteOperation as O, RemoteOperationKind as K},
    };
    let _guard = test_guard();
    crate::test_support::follow_radio_ptt_for_current_test();
    for ptt_source in ["usb", "hamlib"] {
        reset_test_state();
        let mut station = open_hardware_station();
        let factory = StationFactory::new(
            station_providers(graph_provider(), sample_rate_adapter()),
            "agc.so",
            960,
        )
        .unwrap();
        let mut configuration = configuration();
        configuration.memories[0].state.power_mw = Some(5000);
        configuration.ptt_source = Some(ptt_source.into());
        configuration.tune_duration_ms = 50;
        let (setup, control) = RemoteSetup::prepare(
            configuration,
            station.control().plan().hardware().clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        station.bind_remote(setup, control).unwrap();
        station.start().unwrap();
        for selector in [1, 11] {
            let mut operation = O::new(K::GetSetting);
            operation.selector = selector;
            let result = remote_station_command(&mut station, &factory, operation)
                .unwrap_or_else(|error| panic!("setting {selector}: {error:?}"));
            assert!(matches!(result, RemoteReply::Value { .. }));
        }
        for (kind, selector) in [
            (K::PowerPreset, 2),
            (K::Mode, 3),
            (K::Mode, 1),
            (K::Tune, 0),
        ] {
            let mut operation = O::new(kind);
            operation.selector = selector;
            let result = remote_station_command(&mut station, &factory, operation)
                .unwrap_or_else(|error| panic!("{kind:?}: {error:?}"));
            assert!(matches!(result, RemoteReply::Applied));
        }
        station.stop().unwrap();
        assert_eq!(
            station
                .maintenance
                .hardware_state()
                .remote_signals()
                .physical_ptt,
            Some(false)
        );
    }
}

#[test]
fn remote_station_gates_rig_and_external_encode_independently_of_decode_programming() {
    use crate::{RemoteReply, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::{
        CarrierSource, RemoteEncoderOutput, RemoteSignal, RemoteSignalSource as S,
        remote_operation::{RemoteOperation as O, RemoteOperationKind as K},
    };
    let _guard = test_guard();
    for case in 0..6 {
        reset_test_state();
        let source = ConfigDocument::new("[usb]\n[hardware]\nhardware_gpio_6_mode=out0\n");
        let channel =
            ResolvedChannelConfiguration::from_document(&source, "radio.conf", "usb").unwrap();
        let mut station = open_configured_hardware_station(channel);
        let factory = StationFactory::new(
            station_providers(graph_provider(), sample_rate_adapter()),
            "agc.so",
            960,
        )
        .unwrap();
        let mut configuration = configuration();
        configuration.memories[0].state.power_mw = Some(5000);
        configuration.poll_interval_ms = 1;
        configuration.cor_source = Some(
            if matches!(case, 0 | 4) {
                "hamlib"
            } else {
                "open"
            }
            .into(),
        );
        configuration.memories[0].state.encode = RemoteSignal {
            source: match case {
                0 | 4 => S::Rig,
                3 => S::Dsp,
                _ => S::External,
            },
            enabled: case < 4,
            tone_tenths_hz: 1148,
            ..RemoteSignal::default()
        };
        if matches!(case, 1 | 2 | 5) {
            configuration.external_encode_output =
                Some(RemoteEncoderOutput::Usb(GpioPin::new(6).unwrap()));
            configuration.external_encode_inverted = case == 2;
        }
        if case != 3 {
            configuration.memories[0].state.decode = RemoteSignal {
                source: S::External,
                control_source: (case == 0).then_some(S::Rig),
                enabled: true,
                tone_tenths_hz: 1148,
                ..RemoteSignal::default()
            };
        }
        let (setup, control) = RemoteSetup::prepare(
            configuration,
            station.control().plan().hardware().clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig {
                subaudible_source: if case == 3 {
                    usbradioplus_radio::SubaudibleSource::Disabled
                } else {
                    usbradioplus_radio::SubaudibleSource::Usb
                },
                ..usbradioplus_radio::QualificationConfig::default()
            },
        )
        .unwrap();
        station.bind_remote(setup, control).unwrap();
        station.start().unwrap();
        for requested in [false, true, true, false] {
            station.set_ctcss_inhibited(!requested);
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                let reply = remote_station_command(&mut station, &factory, O::new(K::Status))
                    .unwrap_or_else(|error| panic!("tone case {case}: {error:?}"));
                let RemoteReply::Status { snapshot, .. } = reply else {
                    panic!("status response expected");
                };
                assert_eq!(snapshot.applied.encode.enabled, u32::from(case < 4));
                let observed = match case {
                    1 | 2 | 5 => {
                        (GPIO_FINAL_BYTE.load(Ordering::Acquire) & 0x20 != 0)
                            == ((requested && case < 4) != (case == 2))
                    }
                    _ => {
                        station
                            .maintenance
                            .hardware_state()
                            .remote_activity()
                            .ctcss_requested
                            == requested
                    }
                };
                if observed {
                    break;
                }
                assert!(
                    Instant::now() < deadline,
                    "encode case {case} did not follow {requested}"
                );
            }
        }
        assert_eq!(
            station
                .maintenance
                .hardware_state()
                .remote_control()
                .local_unqualified_open,
            case == 3
        );
        station.stop().unwrap();
        if matches!(case, 1 | 2 | 5) {
            assert_eq!(
                GPIO_FINAL_BYTE.load(Ordering::Acquire) & 0x20 != 0,
                case == 2
            );
        }
    }
}

#[test]
fn native_selection_preserves_physical_layout_profile_and_identity_without_tuning() {
    use usbradioplus_audio::{ChannelCount, SelectionPolicy};
    use usbradioplus_gpio::Cm119Profile;

    let _guard = test_guard();
    reset_test_state();
    for policy in [SelectionPolicy::Automatic, SelectionPolicy::Exact] {
        for profile in [
            Cm119Profile::DudeUsb,
            Cm119Profile::SphUsb,
            Cm119Profile::Nhrc,
            Cm119Profile::Custom,
        ] {
            for input in [ChannelCount::Mono, ChannelCount::Stereo] {
                for output in [ChannelCount::Mono, ChannelCount::Stereo] {
                    let (mut plan, _) = selected_hardware(false);
                    plan.audio_selector.policy = policy;
                    plan.audio_selector.identifier =
                        (policy == SelectionPolicy::Exact).then(|| "hw:1,0".to_owned());
                    plan.audio_selector.serial =
                        (policy == SelectionPolicy::Exact).then(|| "ABC".to_owned());
                    plan.audio_selector.input_channels = input;
                    plan.audio_selector.output_channels = output;
                    plan.cm119.as_mut().unwrap().profile = profile;
                    plan.ptt_inverted = true;
                    plan.cm119.as_mut().unwrap().output_enable_mask = 0x21;
                    plan.cm119.as_mut().unwrap().output_initial_mask = 0x20;
                    plan.cm119.as_mut().unwrap().clip_led_mask = 1;
                    let (device, bound) =
                        select_native_hardware(&plan, audio_provider(), gpio_provider(), 480)
                            .unwrap();
                    assert_eq!(device.interface_path.as_deref(), Some("3-1:1.0"));
                    assert_eq!(device.serial.as_deref(), Some("ABC"));
                    assert_eq!(bound.stream.input_channels, input);
                    assert_eq!(bound.stream.output_channels, output);
                    assert_eq!(bound.stream.maximum_receive_frame_count, 480);
                    assert_eq!(bound.cm119.as_ref().unwrap().usb_port_path, "3-1");
                    assert_eq!(
                        (
                            bound.cm119.as_ref().unwrap().vendor_id,
                            bound.cm119.as_ref().unwrap().product_id
                        ),
                        (0x0d8c, 0x000c)
                    );
                    assert_eq!(bound.cm119.as_ref().unwrap().profile, profile);
                    assert!(bound.cm119.as_ref().unwrap().ptt_inverted);
                    assert_eq!(bound.cm119.as_ref().unwrap().output_enable_mask, 0x21);
                    assert_eq!(bound.cm119.as_ref().unwrap().output_initial_mask, 0x20);
                }
            }
        }
    }
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
}

#[test]
fn remote_parallel_composition_rejects_unprogrammable_fields_and_preserves_io_failures() {
    use crate::{RemoteQueue, RemoteSetup, remote_request::tests::configuration};
    use usbradioplus_core::{CarrierSource, RemoteBackend};
    use usbradioplus_remote::CallError;
    let _guard = test_guard();
    for case in 0..11 {
        reset_test_state();
        let (mut plan, selected) = selected_hardware(true);
        plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
        let mut configuration = configuration();
        configuration.backend = RemoteBackend::ParallelChannel;
        configuration.ptt_source = Some("usb".into());
        let memory = &mut configuration.memories[0].state;
        memory.channel = Some(7);
        match case {
            1 => memory.bandwidth_hz = 2400,
            2 => memory.receive_vfo = 1,
            3 => memory.transmit_vfo = 1,
            4 => memory.rit_hz = 10,
            5 => memory.xit_hz = 10,
            6 => memory.split = true,
            7 => memory.rit_enabled = true,
            8 => memory.xit_enabled = true,
            9 => {
                memory.transmit_hz = None;
                memory.power_mw = Some(5000);
            }
            _ => {}
        }
        let (setup, control) = RemoteSetup::prepare(
            configuration,
            plan.clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        // A valid hardware write reaches the missing DSP owner; unsupported
        // hardware fields and failed writes must stop before that exchange.
        drop(control);
        let state = SharedHardwareState::default();
        let (worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        let mut generation = remote::RemoteGeneration::open(
            setup,
            Arc::new(RemoteQueue::new(4)),
            &worker,
            &mut devices,
            &mut outputs,
        )
        .unwrap();
        if case == 10 {
            FAILURE.store(FAIL_SELECT_CHANNEL, Ordering::Release);
        }
        assert_eq!(
            generation.service(&worker, &mut devices, &mut outputs),
            Err(CallError(match case {
                0 | 6 => -1102,
                10 => -1101,
                _ => -6,
            })),
            "channel case {case}"
        );
        assert!(!state.remote_control().transmit_permitted);
        generation
            .retire(&worker, &mut devices, &mut outputs)
            .unwrap();
    }
    for case in 0..3 {
        reset_test_state();
        let (mut plan, selected) = selected_hardware(true);
        plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
        plan.parallel.as_mut().unwrap().ptt_mask = 8;
        let mut configuration = configuration();
        configuration.backend = RemoteBackend::ParallelRtx;
        configuration.ptt_source = Some("parallel".into());
        if case == 1 {
            configuration.memories[0].state.transmit_hz = None;
        }
        let (setup, control) = RemoteSetup::prepare(
            configuration,
            plan.clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        drop(control);
        let state = SharedHardwareState::default();
        let (worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        let mut generation = remote::RemoteGeneration::open(
            setup,
            Arc::new(RemoteQueue::new(4)),
            &worker,
            &mut devices,
            &mut outputs,
        )
        .unwrap();
        if case == 2 {
            FAILURE.store(FAIL_PROGRAM_REMOTE, Ordering::Release);
        }
        assert_eq!(
            generation.service(&worker, &mut devices, &mut outputs),
            Err(CallError(if case == 2 { -1101 } else { -1102 })),
            "RTX case {case}"
        );
        assert!(!state.remote_control().transmit_permitted);
        generation
            .retire(&worker, &mut devices, &mut outputs)
            .unwrap();
    }
}

#[test]
fn native_selection_rejects_absent_mismatched_and_failed_identity_probes() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    for mode in [1, 2, 4, 7] {
        NATIVE_IDENTITY_MODE.store(mode, Ordering::Release);
        assert!(select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).is_err());
    }
    for (mode, serial) in [(3, Some("ABC")), (5, Some("ABC")), (6, None)] {
        NATIVE_IDENTITY_MODE.store(mode, Ordering::Release);
        let (device, _) =
            select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).unwrap();
        assert_eq!(device.serial.as_deref(), serial);
    }
    NATIVE_IDENTITY_MODE.store(0, Ordering::Release);
    FAILURE.store(FAIL_AUDIO_SELECT, Ordering::Release);
    assert!(select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).is_err());
    let mut exact = plan;
    exact.audio_selector.policy = usbradioplus_audio::SelectionPolicy::Exact;
    exact.audio_selector.serial = Some("OTHER".to_owned());
    assert!(select_native_hardware(&exact, audio_provider(), gpio_provider(), 960).is_err());
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
}

#[test]
fn remote_parallel_is_available_to_native_without_another_owner() {
    use usbradioplus_core::radio_control::ParallelRadioRequest;

    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(true);
    let (preflight, selected) =
        HardwarePreflight::native(&plan, false, audio_provider(), gpio_provider(), 960).unwrap();
    let mut maintenance = HardwareMaintenance::native(
        &plan,
        preflight,
        selected,
        SharedHardwareState::default(),
        audio_provider(),
        gpio_provider(),
        [None; 3],
    )
    .unwrap();
    maintenance.start().unwrap();
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    maintenance
        .remote_radio_request(ParallelRadioRequest::SelectChannel(3))
        .unwrap();
    let settings = remote_radio(146_520_000);
    maintenance
        .remote_radio_request(ParallelRadioRequest::Configure(settings))
        .unwrap();
    let snapshot = maintenance.transient_state().unwrap();
    assert_eq!(snapshot.parallel_output_mask() & 0xf0, 0xc0);
    assert_eq!(snapshot.remote_radio(), Some(settings));
    let deadline = Instant::now() + Duration::from_millis(100);
    while PARALLEL_REMOTE_RECEIVE.load(Ordering::Acquire) != settings.receive_hz
        && Instant::now() < deadline
    {
        thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
        PARALLEL_REMOTE_RECEIVE.load(Ordering::Acquire),
        settings.receive_hz
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    maintenance.stop().unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 0);
}

#[test]
fn native_service_publishes_inputs_and_unkeys_without_mixer_or_eeprom_io() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    let (_, selected) =
        select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).unwrap();
    let state = SharedHardwareState::default();
    GPIO_INPUTS.store(0x02, Ordering::Release);
    PTT.store(1, Ordering::Release);
    let mut service = HardwareService::native(&plan, selected, state.clone(), gpio_provider());
    service.start().unwrap();
    let deadline = Instant::now() + Duration::from_millis(100);
    while !state.inputs().carrier && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(1));
    }
    let inputs = state.inputs();
    assert!(inputs.carrier && inputs.subaudible);
    assert_eq!(inputs.cm119_gpio_mask, 0x02);
    assert!(matches!(
        service.read_eeprom(),
        Err(HardwareStationError::EepromDisabled)
    ));
    assert_eq!(service.statistics().cm119.input_read_count, 1);
    service.stop().unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 0);
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
}

#[test]
fn generic_audio_service_keeps_parallel_ptt_independent_of_absent_cm119() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, mut selected) = selected_hardware(true);
    plan.cm119 = None;
    plan.audio_selector.policy = usbradioplus_audio::SelectionPolicy::Exact;
    plan.audio_selector.identifier = Some("hw:1,0".into());
    plan.audio_selector.serial = None;
    selected.cm119 = None;
    selected.ptt_inverted = false;
    let (_, bound) = select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).unwrap();
    assert!(bound.cm119.is_none());
    let parallel = plan.parallel.as_mut().unwrap();
    parallel.ptt_mask = 4;
    parallel.config.output_enable_mask |= 4;
    let state = SharedHardwareState::default();
    let (worker, _, _) = service_worker(&plan, &selected, state.clone());
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.prepare_site(&devices, &worker).unwrap();
    assert!(devices.cm119.is_none());
    assert_eq!(
        devices
            .cm119(HardwareOperation::ReadEeprom)
            .err()
            .unwrap()
            .1,
        GpioError::Unsupported
    );
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert!(
        !state.inputs().physical_ptt,
        "queued demand is not applied PTT"
    );
    PARALLEL_APPLIED_MASK.store(4, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert!(state.inputs().physical_ptt);
    service_native_once(&mut devices, &mut outputs, &worker, false).unwrap();
    assert!(
        state.inputs().physical_ptt,
        "unkey intent cannot erase serviced feedback"
    );
    PARALLEL_APPLIED_MASK.store(0, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, false).unwrap();
    assert!(!state.inputs().physical_ptt);
    assert_eq!(worker.statistics.snapshot().cm119.input_read_count, 0);
    assert_eq!(outputs.site[0].snapshot().output_mask, 0);
    assert_eq!(GPIO_PUBLISHES.load(Ordering::Acquire), 0);
    devices
        .close_outputs(plan.parallel.as_ref(), &mut outputs)
        .unwrap();
}

#[test]
fn native_maintenance_reuses_preflight_mixers_eeprom_and_live_device_owner() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(true);
    let (preflight, selected) =
        HardwarePreflight::native(&plan, true, audio_provider(), gpio_provider(), 960).unwrap();
    let mut configuration = resolved_hardware_channel().into_config();
    assert!(preflight.apply_startup_tuning(&mut configuration));
    assert_eq!(configuration.station.receive.squelch_level, 654);
    assert_eq!(EEPROM_WRITES.load(Ordering::Acquire), 0);
    let state = SharedHardwareState::default();
    // Native startup must not acquire sidetone/compatibility mixer paths.
    FAILURE.store(FAIL_OPEN_SIDETONE, Ordering::Release);
    let mut maintenance = HardwareMaintenance::native(
        &plan,
        preflight,
        selected,
        state.clone(),
        audio_provider(),
        gpio_provider(),
        [None; 3],
    )
    .unwrap();
    assert_eq!(FAILURE.swap(0, Ordering::AcqRel), FAIL_OPEN_SIDETONE);
    assert_eq!(EEPROM_WRITES.load(Ordering::Acquire), 0);
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), 0);
    assert!(matches!(
        maintenance.read_eeprom(),
        Err(HardwareStationError::NotRunning)
    ));
    maintenance.start().unwrap();
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
        321
    );
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    let replacement = maintenance
        .preflight_replacement(&plan, true, audio_provider(), gpio_provider(), 960)
        .unwrap();
    assert!(replacement.0.apply_startup_tuning(&mut configuration));
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    maintenance
        .set_mixer_level(HardwareMixer::Receive, 222)
        .unwrap();
    maintenance
        .set_mixer_level(HardwareMixer::TransmitA, 333)
        .unwrap();
    let tuning = maintenance
        .save_current_tuning_to_eeprom(&configuration)
        .unwrap();
    assert_eq!(tuning.receive_mixer_level(), 222);
    assert_eq!(tuning.transmit_a_mixer_level(), 333);
    assert_eq!(EEPROM_WRITES.load(Ordering::Acquire), 1);
    maintenance.stop().unwrap();
    maintenance.start().unwrap();
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
        222
    );
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::TransmitA).unwrap(),
        333
    );
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    let mut next = configuration.clone();
    next.station.hardware.input_gain_db = 3.0;
    maintenance
        .begin_mixer_update(&configuration, &next, [false; 3])
        .unwrap();
    assert!(matches!(
        maintenance.begin_mixer_update(&configuration, &next, [false; 3]),
        Err(HardwareStationError::Update(StationUpdateError::Busy))
    ));
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
        mixer_level(3.0)
    );
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::TransmitA).unwrap(),
        333
    );
    maintenance.finish_mixer_update(false).unwrap();
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
        222
    );
    maintenance
        .begin_mixer_update(&configuration, &next, [false; 3])
        .unwrap();
    maintenance.finish_mixer_update(true).unwrap();
    assert_eq!(
        maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
        mixer_level(3.0)
    );
    let mut image = maintenance.read_eeprom().unwrap();
    maintenance.write_eeprom(&mut image).unwrap();
    maintenance.set_transmit_request(true, true, CtcssTone::from_tenths_hz(1_000));
    maintenance.set_calibrated_test_tone(true);
    maintenance.set_ctcss_inhibited(true);
    maintenance.set_subaudible_override(true);
    assert!(state.requests().transmit && state.requests().calibrated_test_tone);
    assert!(state.requests().ctcss_inhibit && state.requests().subaudible_override);
    maintenance.clear_manual_requests();
    assert_eq!(
        state.requests(),
        usbradioplus_station::ControllerRequests::default()
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    maintenance.stop().unwrap();
    assert!(matches!(
        maintenance.read_eeprom(),
        Err(HardwareStationError::NotRunning)
    ));
}

#[test]
fn native_maintenance_rejects_changed_identity_and_invalid_gains_before_acquisition() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    let (preflight, selected) =
        HardwarePreflight::native(&plan, false, audio_provider(), gpio_provider(), 960).unwrap();
    for field in 0..3 {
        let mut changed = selected.clone();
        match field {
            0 => changed.stream.input_device_index += 1,
            1 => changed.stream.output_device_index += 1,
            _ => changed.cm119.as_mut().unwrap().usb_port_path = "another-device".to_owned(),
        }
        assert!(matches!(
            HardwareMaintenance::native(
                &plan,
                preflight.clone(),
                changed,
                SharedHardwareState::default(),
                audio_provider(),
                gpio_provider(),
                [None; 3],
            ),
            Err(HardwareStationError::Plan(
                HardwarePlanError::UsbIdentityMismatch
            ))
        ));
    }
    for gain in [-30.01, 30.01, f64::NAN, f64::INFINITY] {
        assert!(matches!(
            HardwareMaintenance::native(
                &plan,
                preflight.clone(),
                selected.clone(),
                SharedHardwareState::default(),
                audio_provider(),
                gpio_provider(),
                [Some(gain), None, None],
            ),
            Err(HardwareStationError::Audio(AudioError::InvalidArgument))
        ));
    }
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), 0);
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), 0);
}

#[test]
fn committed_native_gain_intents_replace_startup_tuning_without_rewriting_running_mixers() {
    let _guard = test_guard();
    for eeprom_enabled in [true, false] {
        reset_test_state();
        let (plan, _) = selected_hardware(false);
        let (preflight, selected) = HardwarePreflight::native(
            &plan,
            eeprom_enabled,
            audio_provider(),
            gpio_provider(),
            960,
        )
        .unwrap();
        let mut configuration = resolved_hardware_channel().into_config();
        configuration.station.hardware.eeprom_enabled = eeprom_enabled;
        configuration.station.hardware.input_gain_db = 3.0;
        configuration.station.hardware.output_a_gain_db = -6.0;
        configuration.station.hardware.output_b_gain_db = 0.0;
        let mut maintenance = HardwareMaintenance::native(
            &plan,
            preflight,
            selected,
            SharedHardwareState::default(),
            audio_provider(),
            gpio_provider(),
            [None; 3],
        )
        .unwrap();
        maintenance.start().unwrap();
        let writes = MIXER_LEVEL_WRITES.load(Ordering::Acquire);
        maintenance.adopt_native_gain_intents([Some(-12.0), None, Some(6.0)], &configuration);
        assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), writes);
        assert_eq!(
            maintenance.mixer_level(HardwareMixer::Receive).unwrap(),
            if eeprom_enabled { 321 } else { 500 }
        );
        maintenance.stop().unwrap();
        maintenance.start().unwrap();
        let expected = if eeprom_enabled {
            [mixer_level(3.0), mixer_level(-6.0), mixer_level(0.0)]
        } else {
            [mixer_level(-12.0), 500, mixer_level(6.0)]
        };
        for (mixer, level) in [
            HardwareMixer::Receive,
            HardwareMixer::TransmitA,
            HardwareMixer::TransmitB,
        ]
        .into_iter()
        .zip(expected)
        {
            assert_eq!(maintenance.mixer_level(mixer).unwrap(), level);
        }
        let opens = GPIO_OPENS.load(Ordering::Acquire);
        let writes = MIXER_LEVEL_WRITES.load(Ordering::Acquire);
        maintenance.start().unwrap();
        assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
        assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), writes);
        assert_eq!(MIXER_SWITCH_WRITES.load(Ordering::Acquire), 0);
        maintenance.stop().unwrap();
    }
}

#[test]
fn shared_maintenance_preserves_asl_mixers_when_native_gain_intents_are_inapplicable() {
    let _guard = test_guard();
    reset_test_state();
    let mut station = open_hardware_station();
    let configuration = station.configuration().clone();
    let writes = MIXER_LEVEL_WRITES.load(Ordering::Acquire);
    let maintenance = station.maintenance_mut();
    maintenance.adopt_native_gain_intents([Some(-12.0); 3], &configuration);
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), writes);
    for (mixer, level) in [
        (HardwareMixer::Receive, 321),
        (HardwareMixer::TransmitA, 411),
        (HardwareMixer::TransmitB, 512),
    ] {
        assert_eq!(maintenance.mixer_level(mixer).unwrap(), level);
    }
    maintenance.start().unwrap();
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    maintenance.start().unwrap();
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    maintenance.stop().unwrap();
}

#[test]
fn native_mixer_snapshot_retries_failed_discovery_and_reuses_its_retained_controls() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    let (preflight, selected) =
        HardwarePreflight::native(&plan, false, audio_provider(), gpio_provider(), 960).unwrap();
    let mut maintenance = HardwareMaintenance::native(
        &plan,
        preflight,
        selected,
        SharedHardwareState::default(),
        audio_provider(),
        gpio_provider(),
        [None; 3],
    )
    .unwrap();
    maintenance.start().unwrap();
    for (mode, expected) in [
        (1, AudioError::Unsupported),
        (3, AudioError::AdapterFailure),
    ] {
        MIXER_PATHS_MODE.store(mode, Ordering::Release);
        assert!(matches!(
            maintenance.mixer_levels(),
            Err(HardwareStationError::Audio(error)) if error == expected
        ));
    }
    MIXER_PATHS_MODE.store(0, Ordering::Release);
    for (mixer, level) in MIXER_LEVELS.iter().zip([222, 333, 444]) {
        mixer.store(level, Ordering::Release);
    }
    let discoveries = TUNING_IO_CALLS.load(Ordering::Acquire);
    assert_eq!(maintenance.mixer_levels().unwrap(), [222, 333, 444]);
    let initialized = TUNING_IO_CALLS.load(Ordering::Acquire);
    assert!(initialized > discoveries);
    assert_eq!(maintenance.mixer_levels().unwrap(), [222, 333, 444]);
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), initialized);
    assert_eq!(MIXER_LEVEL_WRITES.load(Ordering::Acquire), 0);
    maintenance.stop().unwrap();
}

#[test]
fn native_preflight_disabled_invalid_and_unreadable_eeprom_preserve_file_tuning() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, _) = selected_hardware(false);
    let original = resolved_hardware_channel().into_config();
    let (disabled, _) =
        HardwarePreflight::native(&plan, false, audio_provider(), gpio_provider(), 960).unwrap();
    let mut configuration = original.clone();
    assert!(!disabled.apply_startup_tuning(&mut configuration));
    assert_eq!(configuration, original);
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
    for flags in [0, 1, 2] {
        EEPROM_FLAGS.store(flags, Ordering::Release);
        let (invalid, _) =
            HardwarePreflight::native(&plan, true, audio_provider(), gpio_provider(), 960).unwrap();
        assert!(!invalid.apply_startup_tuning(&mut configuration));
        assert_eq!(configuration, original);
    }
    FAILURE.store(FAIL_EEPROM_READ, Ordering::Release);
    let (unreadable, _) =
        HardwarePreflight::native(&plan, true, audio_provider(), gpio_provider(), 960).unwrap();
    assert!(!unreadable.apply_startup_tuning(&mut configuration));
    assert_eq!(configuration, original);
    assert_eq!(EEPROM_WRITES.load(Ordering::Acquire), 0);
}

#[test]
fn native_service_keeps_its_handle_and_recovers_promptly_from_poll_failure() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let state = SharedHardwareState::default();
    let mut service = HardwareService::native(&plan, selected, state.clone(), gpio_provider());
    FAILURE.store(FAIL_CM119_SERVICE, Ordering::Release);
    service
        .start()
        .expect("a native poll failure must not reject activation");
    let deadline = Instant::now() + Duration::from_millis(100);
    while !state.inputs().carrier && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(1));
    }
    service.stop().unwrap();
    assert!(
        service.statistics().service_cycles > 0,
        "native polling must recover without the 500ms reconnect delay"
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), 1);
}

#[test]
fn native_cycle_gates_ptt_on_polled_inputs_and_keeps_physical_feedback_and_clip_pulses() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let state = SharedHardwareState::default();
    let (mut worker, _sender, _events) = service_worker(&plan, &selected, state.clone());
    assert_eq!(worker.interval(), Duration::from_millis(5));
    worker.native = true;
    assert_eq!(worker.interval(), Duration::from_millis(2));
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);

    GPIO_APPLIED_PTT.store(0, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 1);
    assert!(
        !state.inputs().physical_ptt,
        "queued PTT is not physical PTT"
    );
    assert!(state.inputs().carrier);
    let publishes = GPIO_PUBLISHES.load(Ordering::Acquire);
    GPIO_APPLIED_PTT.store(1, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert!(state.inputs().physical_ptt);
    assert_eq!(GPIO_PUBLISHES.load(Ordering::Acquire), publishes);

    outputs.cm119 ^= 0x20;
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(GPIO_PUBLISHES.load(Ordering::Acquire), publishes + 1);
    assert_eq!(PTT.load(Ordering::Acquire), 1);

    GPIO_ONLINE.store(0, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 0);
    assert_eq!(state.inputs(), HardwareInputs::default());
    GPIO_ONLINE.store(1, Ordering::Release);

    for fault in [FAIL_CM119_SERVICE, FAIL_CM119_INPUTS, FAIL_CM119_STATISTICS] {
        service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
        assert_eq!(PTT.load(Ordering::Acquire), 1);
        FAILURE.store(fault, Ordering::Release);
        assert!(service_native_once(&mut devices, &mut outputs, &worker, true).is_err());
        assert_eq!(PTT.load(Ordering::Acquire), 0);
        assert_eq!(state.inputs(), HardwareInputs::default());
    }
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    assert!(service_native_once(&mut devices, &mut outputs, &worker, true).is_err());
    assert_eq!(outputs.native_outputs, None);
    assert!(!state.inputs().physical_ptt);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(PTT.load(Ordering::Acquire), 1);

    outputs.clip_events_seen = u64::MAX;
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(FAILURE.load(Ordering::Acquire), 0);
    assert!(outputs.clip_led_until.is_none());
    outputs.clip_events_seen = u64::MAX;
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert!(outputs.clip_led_until.is_some());
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), 1);
}

#[test]
fn remote_unkey_requires_serviced_outputs_and_preserves_site_baselines() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, mut selected) = selected_hardware(false);
    selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
    let (worker, _sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    worker.ptt_control.set(PhysicalPttControl::Remote {
        selected: Some(RemotePttSource::Usb),
        key: false,
    });
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.cm119 = 0x20;
    GPIO_APPLIED_PTT.store(1, Ordering::Release);
    assert!(service_remote_ptt(&mut devices, &mut outputs, &worker).is_err());
    assert_eq!(
        PTT.load(Ordering::Acquire),
        0,
        "published off is not yet proof of service"
    );
    assert_eq!(GPIO_FINAL_BYTE.load(Ordering::Acquire), 0x20);
    GPIO_APPLIED_PTT.store(0, Ordering::Release);
    service_remote_ptt(&mut devices, &mut outputs, &worker).unwrap();
    assert!(!outputs.native_outputs.unwrap().ptt_asserted);
    assert_eq!(outputs.cm119, 0x20);
    FAILURE.store(FAIL_CM119_SERVICE, Ordering::Release);
    assert!(service_remote_ptt(&mut devices, &mut outputs, &worker).is_err());
}

#[test]
fn remote_parallel_unkey_confirms_every_inverted_ptt_bit() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, mut selected) = selected_hardware(true);
    plan.parallel.as_mut().unwrap().ptt_mask = 3;
    plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
    selected.cm119 = None;
    selected.ptt_inverted = true;
    let (worker, _sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    worker.ptt_control.set(PhysicalPttControl::Remote {
        selected: Some(RemotePttSource::Parallel),
        key: false,
    });
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.parallel = 0x10;
    PARALLEL_APPLIED_MASK.store(1, Ordering::Release);
    assert!(service_remote_ptt(&mut devices, &mut outputs, &worker).is_err());
    PARALLEL_APPLIED_MASK.store(0x13, Ordering::Release);
    service_remote_ptt(&mut devices, &mut outputs, &worker).unwrap();
    assert_eq!(outputs.parallel, 0x10);
}

#[test]
fn site_owner_spare_writes_share_native_handle_and_report_read_failure() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, mut selected) = selected_hardware(false);
    selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
    selected.cm119.as_mut().unwrap().output_initial_mask = 1;
    plan.cm119.as_mut().unwrap().input_mask = 0x02;
    plan.cm119.as_mut().unwrap().clip_led_mask = 1;
    let (mut worker, sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    worker.native = true;
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.prepare_site(&devices, &worker).unwrap();
    let initial = outputs.site[0].snapshot();
    assert_eq!(initial.output_mask, 0x21);
    assert_eq!(initial.reserved_mask & 1, 1);
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    sender
        .send(ServiceCommand::SiteBind(
            SiteBank::Cm119,
            SiteDigitalBinding {
                generation: initial.generation,
                output_mask: 1,
                initial_high_mask: 0,
                safe_high_mask: 0,
            },
        ))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        None,
    )
    .unwrap();
    assert_eq!(GPIO_PUBLISHES.load(Ordering::Acquire), 0);
    assert_eq!(outputs.cm119, 1);
    sender
        .send(ServiceCommand::SiteBind(
            SiteBank::Cm119,
            SiteDigitalBinding {
                generation: initial.generation,
                output_mask: 0x20,
                initial_high_mask: 0x20,
                safe_high_mask: 0,
            },
        ))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        None,
    )
    .unwrap();
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(outputs.native_outputs.unwrap().gpio_output_mask, 0x21);
    assert_eq!(PTT.load(Ordering::Acquire), 1);
    let snapshot = outputs.site[0].snapshot();
    assert_eq!(snapshot.input_quality, SiteReadQuality::Valid);
    assert_eq!(snapshot.readback_valid_mask, 0);
    assert_eq!(snapshot.submitted_high_mask, 0x21);
    assert_eq!(snapshot.submitted_generation, snapshot.generation);
    assert_ne!(
        snapshot.serviced_generation, snapshot.generation,
        "queueing an output is not servicing it"
    );
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    assert_eq!(
        outputs.site[0].snapshot().serviced_generation,
        snapshot.generation
    );
    sender
        .send(ServiceCommand::SitePublish(
            SiteBank::Cm119,
            SiteDigitalRequest {
                generation: initial.generation,
                output_mask: 0x20,
                high_mask: 0,
            },
        ))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        None,
    )
    .unwrap();
    assert_eq!(
        outputs.cm119, 0x21,
        "stale queued work cannot change hardware intent"
    );
    outputs.restore(
        HardwareTransientState {
            cm119_output_mask: 0,
            parallel_output_mask: 0,
            remote_radio: None,
            site_safe_high: [0; 2],
        },
        &selected,
        None,
    );
    assert_eq!(
        outputs.cm119 & 0x20,
        0x20,
        "ordinary state restoration cannot reset claimed site pins"
    );
    let observed_at = outputs.site[0].snapshot().observed_at_ms;
    FAILURE.store(FAIL_CM119_INPUTS, Ordering::Release);
    assert!(service_native_once(&mut devices, &mut outputs, &worker, true).is_err());
    let failed = outputs.site[0].snapshot();
    assert_eq!(failed.input_quality, SiteReadQuality::Fault);
    assert_eq!(failed.observed_at_ms, observed_at);
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
}

#[test]
fn site_owner_parallel_native_acquisition_and_commands_preserve_reserved_pins() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, selected) = selected_hardware(true);
    let parallel = plan.parallel.as_mut().unwrap();
    parallel.config.output_enable_mask = 0xff;
    parallel.ptt_mask = 0x01;
    parallel.input_mask = 0x80;
    let (mut worker, sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    worker.native = true;
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.prepare_site(&devices, &worker).unwrap();
    PARALLEL_INPUTS.store(0x80, Ordering::Release);
    let generation = outputs.site[1].snapshot().generation;
    sender
        .send(ServiceCommand::SiteBind(
            SiteBank::Parallel,
            SiteDigitalBinding {
                generation,
                output_mask: 0x10,
                initial_high_mask: 0x10,
                safe_high_mask: 0,
            },
        ))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        plan.parallel.as_ref(),
    )
    .unwrap();
    service_native_once(&mut devices, &mut outputs, &worker, true).unwrap();
    let snapshot = outputs.site[1].snapshot();
    assert_eq!(snapshot.input_quality, SiteReadQuality::Valid);
    assert_eq!(snapshot.input_high_mask, 0x8000);
    assert_eq!(snapshot.submitted_high_mask, 0x11);
    assert_eq!(snapshot.readback_valid_mask, 0);
    sender.send(ServiceCommand::SelectChannel(1)).unwrap();
    sender
        .send(ServiceCommand::ConfigureRemoteRadio(remote_radio(
            146_520_000,
        )))
        .unwrap();
    for operation in [
        HardwareOperation::SelectChannel,
        HardwareOperation::ProgramRemoteRadio,
    ] {
        assert!(matches!(
            service_commands(
                &worker.receiver,
                &mut outputs,
                &mut devices,
                &selected,
                plan.parallel.as_ref(),
            ),
            Err(ServiceFailure(actual, GpioError::Unsupported)) if actual == operation
        ));
    }
    assert_eq!(
        outputs.parallel, 0x10,
        "legacy channel commands cannot overwrite site pins"
    );
    assert!(outputs.remote_radio.is_none());
    outputs.restore(
        HardwareTransientState {
            cm119_output_mask: 0,
            parallel_output_mask: 0,
            remote_radio: Some(remote_radio(146_520_000)),
            site_safe_high: [0; 2],
        },
        &selected,
        plan.parallel.as_ref(),
    );
    assert!(
        outputs.remote_radio.is_none(),
        "restoring radio state must not steal a site pin"
    );
    outputs
        .apply(
            OutputCommand::Parallel(0x10, OutputRequest::Inactive),
            &devices,
        )
        .unwrap();
    assert_eq!(
        outputs.parallel, 0x10,
        "ordinary writes cannot overwrite site ownership"
    );
    assert_eq!(
        outputs.site[1].snapshot().last_command_error,
        Some(SiteError::Unavailable)
    );
    outputs.site_publish(
        SiteBank::Parallel,
        SiteDigitalRequest {
            generation: snapshot.generation,
            output_mask: 0x10,
            high_mask: 0,
        },
    );
    assert_eq!(
        outputs.parallel, 0,
        "current owner can change its own output"
    );
    FAILURE.store(FAIL_PARALLEL_INPUTS, Ordering::Release);
    assert!(service_native_once(&mut devices, &mut outputs, &worker, false).is_err());
    let failed = outputs.site[1].snapshot();
    assert_eq!(failed.observed_at_ms, snapshot.observed_at_ms);
    assert_eq!(failed.input_quality, SiteReadQuality::Fault);
}

#[test]
fn site_owner_cached_age_advances_even_without_worker_progress() {
    let _guard = test_guard();
    reset_test_state();
    let mut service = hardware_service(false, false);
    assert!(matches!(
        service.site_snapshot(SiteBank::Cm119),
        Err(HardwareStationError::NotRunning)
    ));
    service.worker = Some(thread::spawn(ServiceExit::default));
    assert!(matches!(
        service.site_snapshot(SiteBank::Parallel),
        Err(HardwareStationError::Site(SiteError::Unavailable))
    ));
    service.site[0].publish(
        SiteDigitalSnapshot {
            observed_at_ms: Some(10),
            input_quality: SiteReadQuality::Valid,
            ..SiteDigitalSnapshot::default()
        },
        20,
    );
    thread::sleep(Duration::from_millis(25));
    let snapshot = service.site_snapshot(SiteBank::Cm119).unwrap();
    assert_eq!(snapshot.observed_at_ms, Some(10));
    assert!(
        snapshot.snapshot_at_ms >= 25,
        "a cached read cannot freeze acquisition age"
    );
    service.stop().unwrap();
    let mut parallel = hardware_service(false, true);
    parallel.worker = Some(thread::spawn(ServiceExit::default));
    assert!(parallel.site_snapshot(SiteBank::Parallel).is_ok());
    parallel.stop().unwrap();
}

#[test]
fn site_owner_empty_binding_does_not_schedule_a_pulse() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let (worker, _sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.prepare_site(&devices, &worker).unwrap();
    let generation = outputs.site[0].snapshot().generation;
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    outputs
        .site_bind(
            SiteBank::Cm119,
            SiteDigitalBinding {
                generation,
                output_mask: 0,
                initial_high_mask: 0,
                safe_high_mask: 0,
            },
            &devices,
        )
        .unwrap();
    assert_ne!(outputs.site[0].snapshot().generation, generation);
    assert_eq!(outputs.site[0].snapshot().last_command_error, None);
    assert_eq!(FAILURE.swap(0, Ordering::AcqRel), FAIL_CM119_PULSE);
}

#[test]
fn site_owner_generation_exhaustion_rejects_binding_without_mutation() {
    // Exhaust the process-wide token source in an isolated test process only.
    const FLAG: &str = "URP_TEST_GENERATION_EXHAUSTED";
    if std::env::var_os(FLAG).is_none() {
        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "hardware_host::tests::site_owner_generation_exhaustion_rejects_binding_without_mutation"])
            .env(FLAG, "1").status().unwrap();
        assert!(status.success());
        return;
    }
    let (plan, mut selected) = selected_hardware(false);
    selected.cm119.as_mut().unwrap().output_enable_mask = 0x20;
    let (worker, _sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.prepare_site(&devices, &worker).unwrap();
    let generation = outputs.site[0].snapshot().generation;
    crate::hardware_site::NEXT_GENERATION.store(u64::MAX, Ordering::Relaxed);
    outputs
        .site_bind(
            SiteBank::Cm119,
            SiteDigitalBinding {
                generation,
                output_mask: 0x20,
                initial_high_mask: 0x20,
                safe_high_mask: 0,
            },
            &devices,
        )
        .unwrap();
    assert_eq!(outputs.cm119 & 0x20, 0);
    let snapshot = outputs.site[0].snapshot();
    assert_eq!(snapshot.generation, generation);
    assert_eq!(snapshot.claimed_mask, 0);
    assert_eq!(
        snapshot.last_command_error,
        Some(SiteError::StaleGeneration)
    );
    assert_eq!(SiteBankState::new(0, 0x20, 0, 0).snapshot().generation, 0);
}

#[test]
fn site_owner_recovery_retains_failed_close_and_retires_failed_reopen() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, selected) = selected_hardware(true);
    plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
    let (worker, sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let devices = ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.prepare_site(&devices, &worker).unwrap();
    outputs
        .site_bind(
            SiteBank::Parallel,
            SiteDigitalBinding {
                generation: outputs.site[1].snapshot().generation,
                output_mask: 0x10,
                initial_high_mask: 0,
                safe_high_mask: 0x10,
            },
            &devices,
        )
        .unwrap();
    let generation = outputs.site[1].snapshot().generation;
    let cache = Arc::clone(&worker.site[1]);
    let statistics = Arc::clone(&worker.statistics);
    let stop = Arc::clone(&worker.stop);
    PARALLEL_CLOSE_BLOCKED.store(1, Ordering::Release);
    FAILURE.store(FAIL_CM119_SERVICE, Ordering::Release);
    let handle = thread::spawn(move || worker.run(devices, outputs, InputTracker::default(), None));
    let wait = |condition: &dyn Fn() -> bool| {
        let deadline = Instant::now() + Duration::from_secs(3);
        while !condition() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(1));
        }
        condition()
    };
    let close_failed = wait(&|| statistics.snapshot().service_failures >= 2);
    let opens_while_blocked = GPIO_OPENS.load(Ordering::Acquire);
    for command in [
        ServiceCommand::SiteBind(
            SiteBank::Parallel,
            SiteDigitalBinding {
                generation,
                output_mask: 0x10,
                initial_high_mask: 0,
                safe_high_mask: 0,
            },
        ),
        ServiceCommand::SitePublish(
            SiteBank::Parallel,
            SiteDigitalRequest {
                generation,
                output_mask: 0x10,
                high_mask: 0,
            },
        ),
        ServiceCommand::Output(OutputCommand::Parallel(0x10, OutputRequest::Inactive)),
    ] {
        sender.send(command).unwrap();
    }
    FAILURE.store(FAIL_CM119_CAPABILITIES, Ordering::Release);
    PARALLEL_CLOSE_BLOCKED.store(0, Ordering::Release);
    handle.thread().unpark();
    let reopen_failed = wait(&|| statistics.snapshot().service_failures >= 3);
    let recovered = wait(&|| {
        cache.read().is_ok_and(|snapshot| {
            snapshot.generation != 0
                && snapshot.generation != generation
                && snapshot.serviced_generation == snapshot.generation
        })
    });
    let snapshot = cache.read().unwrap();
    stop.store(true, Ordering::Release);
    handle.thread().unpark();
    let exit = handle.join().unwrap();
    assert!(close_failed && reopen_failed && recovered);
    assert_eq!(
        opens_while_blocked, 1,
        "failed safe close must retain the device"
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), 3);
    assert_eq!(snapshot.claimed_mask, 0);
    assert_eq!(snapshot.serviced_high_mask & 0x10, 0x10);
    assert_eq!(PARALLEL_FINAL_BYTE.load(Ordering::Acquire), 0x10);
    assert!(exit.failure.is_none());
}

#[test]
fn site_owner_binding_cancels_prior_pulses_before_claiming_output() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, mut selected) = selected_hardware(false);
    selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
    let (worker, sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.prepare_site(&devices, &worker).unwrap();
    let initial = outputs.site[0].snapshot();
    let binding = SiteDigitalBinding {
        generation: initial.generation,
        output_mask: 0x20,
        initial_high_mask: 0x20,
        safe_high_mask: 0,
    };
    sender
        .send(ServiceCommand::SiteBind(SiteBank::Cm119, binding))
        .unwrap();
    FAILURE.store(FAIL_CM119_PULSE, Ordering::Release);
    assert!(
        service_commands(
            &worker.receiver,
            &mut outputs,
            &mut devices,
            &selected,
            None
        )
        .is_err()
    );
    assert_eq!(outputs.site[0].snapshot().generation, initial.generation);
    assert_eq!(
        outputs.site[0].snapshot().last_command_error,
        Some(SiteError::Fault)
    );
    assert_eq!(outputs.cm119 & 0x20, 0);
    sender
        .send(ServiceCommand::SiteBind(SiteBank::Cm119, binding))
        .unwrap();
    service_commands(
        &worker.receiver,
        &mut outputs,
        &mut devices,
        &selected,
        None,
    )
    .unwrap();
    assert_eq!(outputs.site[0].snapshot().claimed_mask, 0x20);
    assert_eq!(outputs.cm119 & 0x20, 0x20);
}

#[test]
fn site_owner_start_uses_safe_transfer_before_open_and_preserves_it_for_retry() {
    let _guard = test_guard();
    reset_test_state();
    let mut service = hardware_service(false, true);
    service.selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
    service.selected.cm119.as_mut().unwrap().output_initial_mask = 1;
    let state = HardwareTransientState {
        cm119_output_mask: 0x21,
        parallel_output_mask: 0,
        remote_radio: None,
        site_safe_high: [0x20, 0],
    };
    service.pending_transient = Some(state);
    FAILURE.store(FAIL_PARALLEL_OPEN, Ordering::Release);
    assert!(service.start().is_err());
    assert_eq!(GPIO_INITIAL_BYTE.load(Ordering::Acquire), 0x21);
    assert_eq!(service.pending_transient, Some(state));
    service.start().unwrap();
    assert_eq!(GPIO_INITIAL_BYTE.load(Ordering::Acquire), 0x21);
    service.stop().unwrap();
}

#[test]
fn site_owner_failed_start_retires_safely_and_retains_failed_cleanup() {
    let _guard = test_guard();
    for fault in [
        FAIL_CM119_CAPABILITIES,
        FAIL_PARALLEL_CAPABILITIES,
        FAIL_CM119_PUBLISH,
    ] {
        for blocked in [false, true] {
            reset_test_state();
            let mut service = hardware_service(false, true);
            service.selected.cm119.as_mut().unwrap().ptt_inverted = true;
            let parallel = service.parallel.as_mut().unwrap();
            parallel.config.output_enable_mask = 0xff;
            parallel.ptt_mask = 1;
            let state = HardwareTransientState {
                cm119_output_mask: 0,
                parallel_output_mask: 0x10,
                remote_radio: None,
                site_safe_high: [0, 0x10],
            };
            service.pending_transient = Some(state);
            PARALLEL_CLOSE_BLOCKED.store(u32::from(blocked), Ordering::Release);
            FAILURE.store(fault, Ordering::Release);
            assert!(service.start().is_err());
            assert_eq!(service.pending_transient, Some(state));
            assert_eq!(PARALLEL_RAW_CLOSES.load(Ordering::Acquire), 0);
            assert_eq!(service.retiring.is_some(), blocked);
            if blocked {
                let opens = GPIO_OPENS.load(Ordering::Acquire);
                assert!(service.start().is_err());
                assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
                PARALLEL_CLOSE_BLOCKED.store(0, Ordering::Release);
                service.stop().unwrap();
            }
            assert_eq!(PARALLEL_FINAL_BYTE.load(Ordering::Acquire), 0x11);
            service.start().unwrap();
            assert!(service.pending_transient.is_none());
            service.stop().unwrap();
            assert_eq!(PARALLEL_FINAL_BYTE.load(Ordering::Acquire), 0x11);
            assert_eq!(PARALLEL_RAW_CLOSES.load(Ordering::Acquire), 0);
        }
    }
}

#[test]
fn site_owner_stop_applies_safe_levels_and_rejects_queued_generation() {
    let _guard = test_guard();
    reset_test_state();
    for native in [false, true] {
        let (mut plan, mut selected) = selected_hardware(true);
        selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
        selected.cm119.as_mut().unwrap().output_initial_mask = 1;
        plan.cm119.as_mut().unwrap().clip_led_mask = 1;
        let parallel = plan.parallel.as_mut().unwrap();
        parallel.config.output_enable_mask = 0xff;
        parallel.ptt_mask = 1;
        let (mut worker, sender, _events) =
            service_worker(&plan, &selected, SharedHardwareState::default());
        worker.native = native;
        let devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        outputs.prepare_site(&devices, &worker).unwrap();
        for (bank, mask) in [(SiteBank::Cm119, 0x20), (SiteBank::Parallel, 0x10)] {
            outputs
                .site_bind(
                    bank,
                    SiteDigitalBinding {
                        generation: outputs.site[bank.index()].snapshot().generation,
                        output_mask: mask,
                        initial_high_mask: 0,
                        safe_high_mask: mask,
                    },
                    &devices,
                )
                .unwrap();
            sender
                .send(ServiceCommand::SitePublish(
                    bank,
                    SiteDigitalRequest {
                        generation: outputs.site[bank.index()].snapshot().generation,
                        output_mask: mask,
                        high_mask: 0,
                    },
                ))
                .unwrap();
        }
        let caches = worker.site.clone();
        worker.stop.store(true, Ordering::Release);
        worker.run(devices, outputs, InputTracker::default(), None);
        assert_eq!(GPIO_FINAL_BYTE.load(Ordering::Acquire), 0x21);
        assert_eq!(PARALLEL_FINAL_BYTE.load(Ordering::Acquire), 0x10);
        assert_eq!(PTT.load(Ordering::Acquire), 0);
        for (cache, safe) in caches.iter().zip([0x21, 0x10]) {
            let snapshot = cache.read().unwrap();
            assert_eq!(snapshot.generation, 0, "retired owners accept no requests");
            assert_eq!(snapshot.requested_high_mask, safe);
            assert_eq!(snapshot.submitted_high_mask, safe);
            assert_ne!(snapshot.input_quality, SiteReadQuality::Valid);
        }
    }
}

#[test]
fn site_owner_stop_failure_retains_port_until_safe_write_succeeds() {
    let _guard = test_guard();
    reset_test_state();
    let mut service = hardware_service(false, true);
    service.start().unwrap();
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    FAILURE.store(FAIL_PARALLEL_CLOSE, Ordering::Release);
    assert!(
        service.stop().is_err(),
        "failed handoff must not report success"
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    FAILURE.store(FAIL_PARALLEL_CLOSE, Ordering::Release);
    assert!(
        service.start().is_err(),
        "failed safe close must block replacement"
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    service.stop().unwrap();
    service.start().unwrap();
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens + 1);
    service.stop().unwrap();
}

#[test]
fn hardware_stop_poll_retains_a_busy_physical_owner_without_waiting() {
    let _guard = test_guard();
    reset_test_state();
    let mut service = hardware_service(false, true);
    let (release, wait) = std::sync::mpsc::channel();
    service.worker = Some(thread::spawn(move || {
        wait.recv().unwrap();
        ServiceExit {
            sidetone: None,
            retiring: None,
            failure: None,
        }
    }));
    let before = Instant::now();
    assert!(!service.poll_stop().unwrap());
    assert!(before.elapsed() < Duration::from_millis(100));
    assert!(service.worker.is_some());
    assert!(service.stop.load(Ordering::Acquire));
    release.send(()).unwrap();
    while !service.worker.as_ref().unwrap().is_finished() {
        thread::yield_now();
    }
    assert!(service.poll_stop().unwrap());
    assert!(service.worker.is_none());
    assert!(service.poll_stop().unwrap());
}

#[test]
fn site_owner_recovery_opens_at_safe_levels_without_replaying_departed_commands() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, mut selected) = selected_hardware(false);
    selected.cm119.as_mut().unwrap().output_enable_mask = 0x20;
    selected.cm119.as_mut().unwrap().output_initial_mask = 0;
    let (worker, sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    let mut outputs = OutputState::new(&selected, None, None);
    outputs.prepare_site(&devices, &worker).unwrap();
    outputs
        .site_bind(
            SiteBank::Cm119,
            SiteDigitalBinding {
                generation: outputs.site[0].snapshot().generation,
                output_mask: 0x20,
                initial_high_mask: 0,
                safe_high_mask: 0x20,
            },
            &devices,
        )
        .unwrap();
    let old_generation = outputs.site[0].snapshot().generation;
    let cache = Arc::clone(&worker.site[0]);
    let stop = Arc::clone(&worker.stop);
    FAILURE.store(FAIL_CM119_SERVICE, Ordering::Release);
    let handle = thread::spawn(move || worker.run(devices, outputs, InputTracker::default(), None));
    let deadline = Instant::now() + Duration::from_secs(2);
    let retired = loop {
        if let Ok(snapshot) = cache.read() {
            if snapshot.generation == 0 && snapshot.requested_high_mask == 0x20 {
                break true;
            }
        }
        if Instant::now() >= deadline {
            break false;
        }
        thread::sleep(Duration::from_millis(1));
    };
    // Fault recovery must apply the safe byte before its 500 ms reconnect backoff.
    let deadline = Instant::now() + Duration::from_millis(250);
    while GPIO_FINAL_BYTE.load(Ordering::Acquire) != 0x20 && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(1));
    }
    let safe_before_backoff = GPIO_FINAL_BYTE.load(Ordering::Acquire) == 0x20;
    sender
        .send(ServiceCommand::SitePublish(
            SiteBank::Cm119,
            SiteDigitalRequest {
                generation: old_generation,
                output_mask: 0x20,
                high_mask: 0,
            },
        ))
        .unwrap();
    handle.thread().unpark();
    let deadline = Instant::now() + Duration::from_secs(2);
    let recovered = loop {
        if let Ok(snapshot) = cache.read() {
            if snapshot.generation != 0
                && snapshot.generation != old_generation
                && snapshot.serviced_generation == snapshot.generation
            {
                break Some(snapshot);
            }
        }
        if Instant::now() >= deadline {
            break None;
        }
        thread::sleep(Duration::from_millis(1));
    };
    stop.store(true, Ordering::Release);
    handle.thread().unpark();
    let exit = handle.join().unwrap();
    assert!(retired, "failed owner must invalidate its generation");
    assert!(
        safe_before_backoff,
        "safe outputs must precede retry backoff"
    );
    let recovered = recovered.expect("replacement owner must become ready");
    assert_eq!(recovered.serviced_high_mask, 0x20);
    assert_eq!(
        recovered.claimed_mask, 0,
        "new generation requires a fresh binding"
    );
    assert_eq!(GPIO_FINAL_BYTE.load(Ordering::Acquire), 0x20);
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), 2);
    assert!(exit.failure.is_none());
}

#[test]
fn site_owner_replacement_restores_safe_levels_not_retired_intent() {
    let _guard = test_guard();
    reset_test_state();
    for safe_high in [false, true] {
        let (mut plan, mut selected) = selected_hardware(true);
        selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
        selected.cm119.as_mut().unwrap().output_initial_mask = 1;
        let parallel = plan.parallel.as_mut().unwrap();
        parallel.config.output_enable_mask = 0xff;
        parallel.config.output_initial_mask = 2;
        parallel.ptt_mask = 1;
        let (worker, _sender, _events) =
            service_worker(&plan, &selected, SharedHardwareState::default());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        outputs.prepare_site(&devices, &worker).unwrap();
        for (bank, mask) in [(SiteBank::Cm119, 0x20), (SiteBank::Parallel, 0x10)] {
            outputs
                .site_bind(
                    bank,
                    SiteDigitalBinding {
                        generation: outputs.site[bank.index()].snapshot().generation,
                        output_mask: mask,
                        initial_high_mask: if safe_high { 0 } else { mask },
                        safe_high_mask: if safe_high { mask } else { 0 },
                    },
                    &devices,
                )
                .unwrap();
        }
        let transient = outputs.snapshot();
        outputs.retire_site();
        devices
            .close_outputs(plan.parallel.as_ref(), &mut outputs)
            .unwrap();
        drop(devices);
        let mut replacement = OutputState::new(&selected, plan.parallel.as_ref(), None);
        replacement.restore(transient, &selected, plan.parallel.as_ref());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        replacement.prepare_site(&devices, &worker).unwrap();
        assert_eq!(replacement.cm119, if safe_high { 0x21 } else { 1 });
        assert_eq!(replacement.parallel, if safe_high { 0x12 } else { 2 });
        assert_eq!(replacement.site[0].snapshot().claimed_mask, 0);
        assert_eq!(replacement.site[1].snapshot().claimed_mask, 0);
        replacement.retire_site();
        devices
            .close_outputs(plan.parallel.as_ref(), &mut replacement)
            .unwrap();
        assert_eq!(
            PARALLEL_FINAL_BYTE.load(Ordering::Acquire),
            if safe_high { 0x10 } else { 0 }
        );
    }
}

#[test]
fn site_owner_recovery_preserves_safe_high_before_rebinding() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, selected) = selected_hardware(true);
    let parallel = plan.parallel.as_mut().unwrap();
    parallel.config.output_enable_mask = 0xff;
    parallel.ptt_mask = 1;
    let (worker, _sender, _events) =
        service_worker(&plan, &selected, SharedHardwareState::default());
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.prepare_site(&devices, &worker).unwrap();
    outputs
        .site_bind(
            SiteBank::Parallel,
            SiteDigitalBinding {
                generation: outputs.site[1].snapshot().generation,
                output_mask: 0x10,
                initial_high_mask: 0,
                safe_high_mask: 0x10,
            },
            &devices,
        )
        .unwrap();
    for _ in 0..2 {
        outputs.retire_site();
        devices
            .close_outputs(plan.parallel.as_ref(), &mut outputs)
            .unwrap();
        assert_eq!(PARALLEL_FINAL_BYTE.load(Ordering::Acquire), 0x10);
        drop(devices);
        devices = ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        outputs.prepare_site(&devices, &worker).unwrap();
        assert_eq!(outputs.site[1].snapshot().claimed_mask, 0);
    }
}

#[test]
fn site_owner_stop_clears_unclaimed_parallel_bits_and_deasserts_inverted_ptt() {
    let _guard = test_guard();
    reset_test_state();
    for inverted in [false, true] {
        let (mut plan, mut selected) = selected_hardware(true);
        selected.cm119.as_mut().unwrap().ptt_inverted = inverted;
        let parallel = plan.parallel.as_mut().unwrap();
        parallel.config.output_enable_mask = 0xff;
        parallel.config.output_initial_mask = 0x20;
        parallel.ptt_mask = 1;
        let (worker, _sender, _events) =
            service_worker(&plan, &selected, SharedHardwareState::default());
        let devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        outputs.prepare_site(&devices, &worker).unwrap();
        worker.stop.store(true, Ordering::Release);
        let exit = worker.run(devices, outputs, InputTracker::default(), None);
        assert!(exit.failure.is_none());
        assert_eq!(
            PARALLEL_FINAL_BYTE.load(Ordering::Acquire),
            u32::from(inverted)
        );
    }
}

#[test]
fn external_encoder_pin_stays_reserved_after_device_recovery() {
    let _guard = test_guard();
    for bank in [SiteBank::Cm119, SiteBank::Parallel] {
        reset_test_state();
        let (mut plan, mut selected) = selected_hardware(true);
        selected.cm119.as_mut().unwrap().output_enable_mask = 0x21;
        plan.cm119.as_mut().unwrap().output_enable_mask = 0x21;
        plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
        plan.parallel.as_mut().unwrap().ptt_mask = 1;
        let (worker, _sender, _events) =
            service_worker(&plan, &selected, SharedHardwareState::default());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        outputs.prepare_site(&devices, &worker).unwrap();
        outputs
            .bind_encoder(
                Some(crate::remote_hardware::ExternalEncoder {
                    bank,
                    mask: 0x20,
                    inverted: false,
                }),
                &devices,
            )
            .unwrap();
        assert_ne!(
            outputs.site[bank.index()].snapshot().reserved_mask & 0x20,
            0
        );

        // Follow the same safe close/reopen/site rebuild as ServiceWorker::run.
        outputs.retire_site();
        devices
            .close_outputs(plan.parallel.as_ref(), &mut outputs)
            .unwrap();
        drop(devices);
        let devices = ServiceDevices::open_at_outputs(
            gpio_provider(),
            &selected,
            plan.parallel.as_ref(),
            &outputs,
        )
        .unwrap();
        outputs.prepare_site(&devices, &worker).unwrap();
        let snapshot = outputs.site[bank.index()].snapshot();
        outputs
            .site_bind(
                bank,
                SiteDigitalBinding {
                    generation: snapshot.generation,
                    output_mask: 0x20,
                    initial_high_mask: 0x20,
                    safe_high_mask: 0,
                },
                &devices,
            )
            .unwrap();
        let snapshot = outputs.site[bank.index()].snapshot();
        assert_eq!(snapshot.claimed_mask & 0x20, 0);
        assert_eq!(snapshot.last_command_error, Some(SiteError::Unavailable));
        assert_ne!(snapshot.reserved_mask & 0x20, 0);
    }
}

#[test]
fn external_encoder_owns_its_pin_and_closes_inactive_for_both_polarities() {
    let _guard = test_guard();
    reset_test_state();
    for bank in [SiteBank::Cm119, SiteBank::Parallel] {
        for inverted in [false, true] {
            let (mut plan, selected) = selected_hardware(true);
            plan.parallel.as_mut().unwrap().config.output_enable_mask = 255;
            plan.parallel.as_mut().unwrap().ptt_mask = 1;
            let mut devices =
                ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
            let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
            outputs.site[bank.index()] = SiteBankState::new(0, 255, 0, 0);
            let encoder = crate::remote_hardware::ExternalEncoder {
                bank,
                mask: 2,
                inverted,
            };
            outputs.bind_encoder(Some(encoder), &devices).unwrap();
            outputs.set_encoder(true);
            let bits = if bank == SiteBank::Cm119 {
                outputs.cm119
            } else {
                outputs.parallel
            };
            assert_eq!(bits & 2 != 0, !inverted);
            let command = if bank == SiteBank::Cm119 {
                OutputCommand::Cm119(2, OutputRequest::Pulse(1))
            } else {
                OutputCommand::Parallel(2, OutputRequest::Pulse(1))
            };
            assert!(outputs.apply(command, &devices).is_err());
            outputs.retire_site();
            let bits = if bank == SiteBank::Cm119 {
                outputs.cm119
            } else {
                outputs.parallel
            };
            assert_eq!(bits & 2 != 0, inverted);
            devices
                .close_outputs(plan.parallel.as_ref(), &mut outputs)
                .unwrap();
            if bank == SiteBank::Parallel {
                assert_eq!(
                    PARALLEL_FINAL_BYTE.load(Ordering::Acquire) & 2 != 0,
                    inverted
                );
            }
        }
    }
}

pub(super) fn service_worker(
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
            native: false,
            ptt_control: Cell::new(PhysicalPttControl::Ordinary),
            provider: gpio_provider(),
            selected: selected.clone(),
            parallel: plan.parallel.clone(),
            state,
            statistics: Arc::new(ServiceStatistics::new(plan.parallel.is_some())),
            stop: Arc::new(AtomicBool::new(false)),
            receiver: command_receiver,
            cm119_input_mask: plan.cm119.as_ref().map_or(0, |cm119| cm119.input_mask),
            clip_led_mask: plan.cm119.as_ref().map_or(0, |cm119| cm119.clip_led_mask),
            input_sender,
            clock: Instant::now(),
            site: std::array::from_fn(|_| Arc::new(SiteCache::default())),
            remote: None,
            remote_queue: None,
            remote_error: Arc::new(AtomicI32::new(0)),
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
    open_configured_hardware_station(resolved_hardware_channel())
}

fn open_configured_hardware_station(mut channel: ResolvedChannelConfiguration) -> HardwareStation {
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
fn reserved_parallel_radio_pins_report_failure_instead_of_acknowledging_no_write() {
    let (plan, selected) = selected_hardware(true);
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    outputs.site[1] = SiteBankState::new(0, 255, 0, 0);
    let generation = outputs.site[1].snapshot().generation;
    let mut byte = 0;
    outputs.site[1]
        .bind(
            SiteDigitalBinding {
                generation,
                output_mask: 16,
                initial_high_mask: 0,
                safe_high_mask: 0,
            },
            &mut byte,
        )
        .unwrap();
    let mut devices = ServiceDevices {
        cm119: None,
        parallel: None,
        ptt_inverted: false,
    };
    assert!(outputs.select_channel(1, &mut devices).is_err());
    assert!(
        outputs
            .configure_remote_radio(remote_radio(146520000))
            .is_err()
    );
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
    no_remote
        .configure_remote_radio(remote_radio(146_520_000))
        .unwrap();
    no_remote.service_remote_radio(&mut devices, true).unwrap();
    no_remote.service_remote_radio(&mut devices, true).unwrap();
    no_remote.service_remote_radio(&mut devices, false).unwrap();
    no_remote.remote_radio_dirty = true;
    FAILURE.store(FAIL_PROGRAM_REMOTE, Ordering::Release);
    assert_eq!(
        failed_operation(no_remote.service_remote_radio(&mut devices, false)),
        HardwareOperation::ProgramRemoteRadio
    );

    no_remote.configure_remote_radio(remote_radio(0)).unwrap();
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
        site_safe_high: [0; 2],
    };
    outputs.restore(state, &selected, plan.parallel.as_ref());
    outputs.restore(state, &selected_without_parallel, None);
    outputs.remote_radio = Some(remote_radio(145_000_000));
    devices
        .close_outputs(plan.parallel.as_ref(), &mut outputs)
        .unwrap();
    devices
        .close_outputs(plan.parallel.as_ref(), &mut outputs)
        .unwrap();
    devices_without_parallel
        .close_outputs(None, &mut outputs)
        .unwrap();
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
        .cm119_mixer_paths(&selected.cm119.as_ref().unwrap().usb_port_path)
        .unwrap()
        .sidetone;
    let mixers = open_paths(
        audio_provider(),
        &selected.cm119.as_ref().unwrap().usb_port_path,
        &paths,
    )
    .unwrap();
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
    outputs
        .configure_remote_radio(remote_radio(146_520_000))
        .unwrap();
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
            .sidetone
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
    assert!(handle.join().unwrap().sidetone.is_none());

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
    assert!(handle.join().unwrap().sidetone.is_none());
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
            site_safe_high: [0; 2],
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
            site_safe_high: [0; 2],
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
            site_safe_high: [0; 2],
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
    queue.worker = Some(std::thread::spawn(ServiceExit::default));
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
    disconnected.worker = Some(std::thread::spawn(ServiceExit::default));
    assert!(matches!(
        disconnected.queue_service(ServiceCommand::Output(OutputCommand::Cm119(
            1,
            OutputRequest::Active
        ))),
        Err(HardwareStationError::NotRunning)
    ));
    disconnected.stop().unwrap();

    let mut panicked = hardware_service(true, false);
    panicked.worker = Some(std::thread::spawn(|| -> ServiceExit {
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
    statistics.success(Some(cm119));
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
    assert_eq!(station.receive_observation(), diagnostics.receive);
    station
        .apply_control(ControlMessage::TransmitUnkey)
        .unwrap();
    let requests = station.hardware_state().requests();
    assert!(!requests.transmit);
    assert!(requests.render_admitted);
    std::thread::sleep(Duration::from_millis(20));
    let transient = station.transient_state().unwrap();
    station.stop().unwrap();
    let stopped_replacement = station
        .preflight_replacement(&plan, false, audio_provider(), gpio_provider())
        .unwrap();
    assert!(!stopped_replacement.apply_startup_tuning(&mut replacement_configuration));
    station.stage_transient_state(transient);
    station.start().unwrap();
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(station.transient_state().unwrap(), transient);
    station.restore_transient_state(transient).unwrap();
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

#[test]
fn native_mixer_open_errors_and_generic_audio_keep_device_io_separate() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, mut selected) = selected_hardware(false);
    let audio_selected = audio_provider()
        .select_device(&plan.audio_selector)
        .unwrap();
    for failure in [FAIL_MIXER_OPEN, FAIL_OPEN_TX] {
        FAILURE.store(failure, Ordering::Release);
        assert!(HardwareMixers::native(audio_provider(), &audio_selected).is_err());
        assert_eq!(FAILURE.load(Ordering::Acquire), 0);
    }
    selected.cm119 = None;
    let opens = GPIO_OPENS.load(Ordering::Acquire);
    let io = TUNING_IO_CALLS.load(Ordering::Acquire);
    assert!(
        read_startup_eeprom(&selected, true, gpio_provider())
            .unwrap()
            .is_none()
    );
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), io);
    let mut generic = plan;
    generic.cm119 = None;
    let (worker, _, _) = service_worker(&generic, &selected, SharedHardwareState::default());
    let mut outputs = OutputState::new(&selected, None, None);
    let mut devices =
        ServiceDevices::open_at_outputs(gpio_provider(), &selected, None, &outputs).unwrap();
    service_once(
        &mut devices,
        &mut outputs,
        &mut InputTracker::default(),
        None,
        &worker,
    )
    .unwrap();
    assert_eq!(GPIO_OPENS.load(Ordering::Acquire), opens);
    assert!(!worker.state.inputs().physical_ptt);
}

#[test]
fn generic_alsa_station_omits_cm119_mixers_in_the_legacy_host() {
    let _guard = test_guard();
    reset_test_state();
    let providers = station_providers(graph_provider(), sample_rate_adapter());
    let processing =
        NativeProcessingFactory::new(providers.graph, providers.denoise, "agc.so", 960).unwrap();
    let mut remote = crate::remote_request::tests::configuration();
    remote.audio_source = usbradioplus_core::RemoteAudioSource::Alsa;
    let mut configuration = resolved_hardware_channel().into_config();
    configuration.station.hardware.device_identifier = "hw:1,0".into();
    configuration.station.hardware.serial.clear();
    let plan = StationPlan::new(
        "usb",
        configuration,
        ControllerTransport::RptAdvanced,
        1,
        960,
    )
    .unwrap()
    .with_remote_configuration(Some(remote));
    let selected = audio_provider()
        .select_alsa_device(&plan.hardware().audio_selector)
        .unwrap();
    assert!(selected.interface_path.is_none());
    let media = plan
        .prepare(&processing)
        .unwrap()
        .bind_media(
            providers.ring,
            providers.radio,
            usbradioplus_station::ControllerSetup::RptAdvanced { handoff_slots: 2 },
        )
        .unwrap();
    let station = HardwareStation::open_preflighted(
        media,
        audio_provider(),
        gpio_provider(),
        960,
        HardwarePreflight::new(selected, None),
    )
    .unwrap();
    assert!(station.maintenance.mixers.borrow().is_none());
    assert_eq!(TUNING_IO_CALLS.load(Ordering::Acquire), 0);
}

#[test]
fn external_encoder_host_admission_blocks_only_its_own_bank_and_pin() {
    use usbradioplus_core::{CarrierSource, RemoteEncoderOutput};
    let _guard = test_guard();
    reset_test_state();
    for bank in [SiteBank::Cm119, SiteBank::Parallel] {
        let (mut plan, mut selected) = selected_hardware(true);
        plan.cm119.as_mut().unwrap().output_enable_mask = 0x25;
        selected.cm119.as_mut().unwrap().output_enable_mask = 0x25;
        plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
        plan.parallel.as_mut().unwrap().ptt_mask = 1;
        let mut config = crate::remote_request::tests::configuration();
        config.external_encode_output = Some(match bank {
            SiteBank::Cm119 => RemoteEncoderOutput::Usb(GpioPin::new(6).unwrap()),
            SiteBank::Parallel => RemoteEncoderOutput::Parallel(ParallelPin::new(7).unwrap()),
        });
        let (setup, _control) = crate::RemoteSetup::prepare(
            config,
            plan.clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        let encoder = crate::remote_hardware::ExternalEncoder::configured(setup.configuration());
        let mut service = HardwareService::new(
            gpio_provider(),
            &plan,
            selected.clone(),
            SharedHardwareState::default(),
            HardwareServiceSetup {
                eeprom_enabled: false,
                sidetone: None,
                initial_remote_radio: None,
            },
        );
        service.bind_remote(setup).unwrap();
        let (worker, _, _) = service_worker(&plan, &selected, SharedHardwareState::default());
        let mut devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        outputs.prepare_site(&devices, &worker).unwrap();
        outputs.bind_encoder(encoder, &devices).unwrap();
        for (command, reserved) in [
            (
                OutputCommand::Cm119(0x20, OutputRequest::Active),
                bank == SiteBank::Cm119,
            ),
            (
                OutputCommand::Parallel(0x20, OutputRequest::Active),
                bank == SiteBank::Parallel,
            ),
            (OutputCommand::Cm119(4, OutputRequest::Active), false),
            (OutputCommand::Parallel(4, OutputRequest::Active), false),
        ] {
            assert!(
                matches!(
                    service.queue(command),
                    Err(HardwareStationError::UnavailableOutput)
                ) == reserved
            );
            assert_eq!(outputs.apply(command, &devices).is_err(), reserved);
        }
        devices
            .close_outputs(plan.parallel.as_ref(), &mut outputs)
            .unwrap();
    }
}

#[test]
fn safe_close_reports_cm119_publish_failure_and_still_closes_parallel() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(true);
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    assert_eq!(
        failed_operation(devices.close_outputs(plan.parallel.as_ref(), &mut outputs)),
        HardwareOperation::PublishCm119
    );
    assert!(devices.parallel.is_none());
    assert_eq!(FAILURE.load(Ordering::Acquire), 0);
    devices
        .close_outputs(plan.parallel.as_ref(), &mut outputs)
        .unwrap();
}

unsafe extern "C" fn gpio_statistics_offline(
    device: *const c_void,
    output: *mut GpioCm119Statistics,
) -> c_int {
    // SAFETY: forward the wrapper's live handle and writable ABI snapshot.
    let result = unsafe { gpio_statistics(device, output) };
    if result == OK {
        // SAFETY: a successful original callback initialized the complete output.
        unsafe {
            (*output).online = 0;
        }
    }
    result
}
unsafe extern "C" fn parallel_statistics_offline(
    device: *const c_void,
    output: *mut GpioParallelStatistics,
) -> c_int {
    // SAFETY: forward the wrapper's live handle and writable ABI snapshot.
    let result = unsafe { parallel_statistics(device, output) };
    if result == OK {
        // SAFETY: a successful original callback initialized the complete output.
        unsafe {
            (*output).online = 0;
        }
    }
    result
}
fn offline_statistics_provider() -> GpioAdapter {
    let descriptor = GpioDescriptor {
        statistics: Some(gpio_statistics_offline),
        parallel_statistics: Some(parallel_statistics_offline),
        ..GPIO_DESCRIPTOR
    };
    // SAFETY: from_raw copies the complete table; its C string and every function
    // address remain process-owned. No pointer to this stack table is retained.
    unsafe { GpioAdapter::from_raw(ptr::from_ref(&descriptor).cast()) }.unwrap()
}

#[test]
fn physical_unkey_requires_online_input_and_independent_online_statistics() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let (worker, _, _) = service_worker(&plan, &selected, SharedHardwareState::default());
    let mut outputs = OutputState::new(&selected, None, None);
    let mut devices = ServiceDevices::open(gpio_provider(), &selected, None).unwrap();
    GPIO_ONLINE.store(0, Ordering::Release);
    assert_eq!(
        failed_operation(service_remote_ptt(&mut devices, &mut outputs, &worker)),
        HardwareOperation::ConfirmCm119Ptt
    );
    GPIO_ONLINE.store(1, Ordering::Release);
    service_remote_ptt(&mut devices, &mut outputs, &worker).unwrap();
    let mut offline = ServiceDevices::open(offline_statistics_provider(), &selected, None).unwrap();
    assert_eq!(
        failed_operation(service_remote_ptt(&mut offline, &mut outputs, &worker)),
        HardwareOperation::ConfirmCm119Ptt
    );
    let (mut plan, mut selected) = selected_hardware(true);
    selected.cm119 = None;
    plan.cm119 = None;
    plan.parallel.as_mut().unwrap().ptt_mask = 8;
    let (worker, _, _) = service_worker(&plan, &selected, SharedHardwareState::default());
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    let mut devices = ServiceDevices::open(
        offline_statistics_provider(),
        &selected,
        plan.parallel.as_ref(),
    )
    .unwrap();
    assert_eq!(
        failed_operation(service_remote_ptt(&mut devices, &mut outputs, &worker)),
        HardwareOperation::ConfirmParallelPtt
    );
}

#[test]
fn parallel_only_input_reports_serviced_ptt_and_never_invents_offline_feedback() {
    let _guard = test_guard();
    reset_test_state();
    for (mask, inverted, applied, expected) in [
        (0, false, 0, false),
        (8, false, 8, true),
        (8, true, 0, true),
        (8, false, 0, false),
    ] {
        let (mut plan, mut selected) = selected_hardware(true);
        plan.cm119 = None;
        selected.cm119 = None;
        selected.ptt_inverted = inverted;
        plan.parallel.as_mut().unwrap().ptt_mask = mask;
        let (worker, _, _) = service_worker(&plan, &selected, SharedHardwareState::default());
        let devices =
            ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
        PARALLEL_APPLIED_MASK.store(applied, Ordering::Release);
        assert_eq!(
            collected_inputs(&devices, &worker, None, None)
                .unwrap()
                .physical_ptt,
            expected
        );
        FAILURE.store(FAIL_PARALLEL_STATISTICS, Ordering::Release);
        assert!(matches!(
            collected_inputs(&devices, &worker, None, None),
            Err(ServiceFailure(HardwareOperation::ReadParallelStatistics, _))
        ));
        let offline = ServiceDevices::open(
            offline_statistics_provider(),
            &selected,
            plan.parallel.as_ref(),
        )
        .unwrap();
        assert!(
            !collected_inputs(&offline, &worker, None, None)
                .unwrap()
                .physical_ptt
        );
    }
}

#[test]
fn remote_maintenance_parallel_requests_report_busy_and_retired_owners() {
    use usbradioplus_core::{CarrierSource, RemoteBackend};
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, _) = selected_hardware(true);
    plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
    let (audio_selected, selected) =
        select_native_hardware(&plan, audio_provider(), gpio_provider(), 960).unwrap();
    let mut maintenance = HardwareMaintenance::native(
        &plan,
        HardwarePreflight::new(audio_selected, None),
        selected,
        SharedHardwareState::default(),
        audio_provider(),
        gpio_provider(),
        [None; 3],
    )
    .unwrap();
    let mut config = crate::remote_request::tests::configuration();
    config.backend = RemoteBackend::ParallelChannel;
    config.ptt_source = Some("usb".into());
    config.memories[0].state.channel = Some(7);
    let (setup, control) = crate::RemoteSetup::prepare(
        config.clone(),
        plan.clone(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    let (replacement, _replacement_control) = crate::RemoteSetup::prepare(
        config,
        plan,
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    maintenance.bind_remote(setup).unwrap();
    maintenance.start().unwrap();
    assert!(matches!(
        maintenance.bind_remote(replacement),
        Err(HardwareStationError::ControlQueueFull)
    ));
    let deadline = Instant::now() + Duration::from_secs(5);
    while !control.is_busy() && Instant::now() < deadline {
        thread::yield_now();
    }
    assert!(
        control.is_busy(),
        "worker must be waiting for startup DSP adoption"
    );
    assert_eq!(maintenance.service.remote_error(), 0);
    for _ in 0..CONTROL_QUEUE_CAPACITY {
        maintenance
            .remote_radio_request(ParallelRadioRequest::SelectChannel(7))
            .unwrap();
    }
    assert!(matches!(
        maintenance.remote_radio_request(ParallelRadioRequest::SelectChannel(7)),
        Err(HardwareStationError::ControlQueueFull)
    ));
    maintenance.stop().unwrap();
    assert!(matches!(
        maintenance.remote_radio_request(ParallelRadioRequest::SelectChannel(7)),
        Err(HardwareStationError::NotRunning)
    ));
    let station = open_hardware_station();
    assert!(matches!(
        station.remote_radio_request(ParallelRadioRequest::SelectChannel(7)),
        Err(HardwareStationError::NotRunning)
    ));
}

#[test]
fn partial_safe_close_keeps_closed_parallel_bank_unkeyed_without_reviving_it() {
    let _guard = test_guard();
    reset_test_state();
    let (mut plan, selected) = selected_hardware(true);
    plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
    plan.parallel.as_mut().unwrap().ptt_mask = 8;
    let (worker, _, _) = service_worker(&plan, &selected, SharedHardwareState::default());
    worker.ptt_control.set(PhysicalPttControl::Remote {
        selected: Some(RemotePttSource::Parallel),
        key: false,
    });
    let mut devices =
        ServiceDevices::open(gpio_provider(), &selected, plan.parallel.as_ref()).unwrap();
    let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
    FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    assert_eq!(
        failed_operation(devices.close_outputs(plan.parallel.as_ref(), &mut outputs)),
        HardwareOperation::PublishCm119
    );
    assert!(devices.cm119.is_some());
    assert!(devices.parallel.is_none());
    // This partial state is produced by the actual two-bank safe-close API:
    // the parallel final OFF write succeeded while the CM119 write must retry.
    service_remote_ptt(&mut devices, &mut outputs, &worker).unwrap();
    assert!(
        devices.parallel.is_none(),
        "unkey must not reopen a safely closed bank"
    );
    worker.ptt_control.set(PhysicalPttControl::Remote {
        selected: Some(RemotePttSource::Parallel),
        key: true,
    });
    assert!(
        service_remote_ptt(&mut devices, &mut outputs, &worker).is_err(),
        "an absent selected output must never acknowledge a key request"
    );
    assert_eq!(PTT.load(Ordering::Acquire), 0);
    devices
        .close_outputs(plan.parallel.as_ref(), &mut outputs)
        .unwrap();
}

fn await_hardware_condition(condition: impl Fn() -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_secs(3);
    while !condition() && Instant::now() < deadline {
        thread::yield_now();
    }
    condition()
}

fn worker_remote_setup(
    plan: &HardwarePlan,
) -> (crate::RemoteSetup, usbradioplus_station::RemoteDspControl) {
    let mut config = crate::remote_request::tests::configuration();
    config.memories[0].state.power_mw = Some(5000);
    config.ptt_source = Some("usb".into());
    crate::RemoteSetup::prepare(
        config,
        plan.clone(),
        usbradioplus_core::CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap()
}

struct ReleaseWorkerFaults;
impl Drop for ReleaseWorkerFaults {
    fn drop(&mut self) {
        GPIO_APPLIED_PTT.store(u32::MAX, Ordering::Release);
        SHUTDOWN_PULSE_STAGE.store(0, Ordering::Release);
        SHUTDOWN_SERVICE_FAILURES.store(0, Ordering::Release);
        FAILURE.store(0, Ordering::Release);
    }
}

#[test]
fn remote_worker_retries_initial_and_final_physical_off_confirmation() {
    let _guard = test_guard();
    reset_test_state();
    let (plan, selected) = selected_hardware(false);
    let mut service = HardwareService::native(
        &plan,
        selected,
        SharedHardwareState::default(),
        gpio_provider(),
    );
    // This guard clears persistent injected feedback before HardwareService::drop
    // requests/join safe shutdown, including an assertion failure in this test.
    let _release = ReleaseWorkerFaults;
    let (setup, _control) = worker_remote_setup(&plan);
    service.bind_remote(setup).unwrap();
    GPIO_APPLIED_PTT.store(1, Ordering::Release);
    service.start().unwrap();
    let initial_failure = await_hardware_condition(|| service.remote_error() == -1101);
    let worker = service.worker.as_ref().unwrap().thread().clone();
    let queue = Arc::clone(service.remote_queue.as_ref().unwrap());
    service.stop.store(true, Ordering::Release);
    worker.unpark();
    let retired = await_hardware_condition(|| {
        matches!(
            queue.submit(
                RemoteOperation::new(
                    usbradioplus_core::remote_operation::RemoteOperationKind::Status
                ),
                true,
                false
            ),
            Err(RemoteRequestError::Stopped)
        )
    });
    let failures = service.statistics().service_failures;
    worker.unpark();
    let retirement_failure =
        await_hardware_condition(|| service.statistics().service_failures > failures);
    GPIO_APPLIED_PTT.store(0, Ordering::Release);
    worker.unpark();
    service.stop().unwrap();
    assert!(initial_failure && retired && retirement_failure);
    assert_eq!(PTT.load(Ordering::Acquire), 0);
}

unsafe extern "C" fn shutdown_publish_fault_on_second_pulse(
    device: *mut c_void,
    pulse: *const GpioCm119Pulse,
) -> c_int {
    if SHUTDOWN_PULSE_STAGE.load(Ordering::Acquire) != 0
        && SHUTDOWN_PULSE_STAGE.fetch_add(1, Ordering::AcqRel) == 2
    {
        // The first pulse belongs to confirmed remote unkey. The next belongs
        // to final safe close, where the existing one-shot publish fault applies.
        FAILURE.store(FAIL_CM119_PUBLISH, Ordering::Release);
    }
    // SAFETY: preserve the live handle and complete borrowed pulse contract.
    unsafe { gpio_pulse(device, pulse) }
}

unsafe extern "C" fn shutdown_service_fault(device: *mut c_void) -> c_int {
    if SHUTDOWN_SERVICE_FAILURES
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |remaining| {
            remaining.checked_sub(1)
        })
        .is_ok()
    {
        return -1;
    }
    // SAFETY: forward the retained device handle without changing its ownership.
    unsafe { gpio_service(device) }
}

fn shutdown_fault_provider() -> GpioAdapter {
    let descriptor = GpioDescriptor {
        gpio_pulse: Some(shutdown_publish_fault_on_second_pulse),
        service: Some(shutdown_service_fault),
        ..GPIO_DESCRIPTOR
    };
    // SAFETY: the wrapper copies the table; all callback and string addresses
    // are immutable test-binary objects and remain loaded for every owner.
    unsafe { GpioAdapter::from_raw(ptr::from_ref(&descriptor).cast()) }.unwrap()
}

#[test]
fn legacy_remote_worker_reopens_only_to_finish_safe_shutdown_and_retries_close() {
    let _guard = test_guard();
    for physical_off_stuck in [false, true] {
        reset_test_state();
        let (plan, selected) = selected_hardware(false);
        let mut service = HardwareService::new(
            shutdown_fault_provider(),
            &plan,
            selected,
            SharedHardwareState::default(),
            HardwareServiceSetup {
                eeprom_enabled: false,
                sidetone: None,
                initial_remote_radio: None,
            },
        );
        let _release = ReleaseWorkerFaults;
        let (setup, control) = worker_remote_setup(&plan);
        service.bind_remote(setup).unwrap();
        service.start().unwrap();
        let startup_waiting = await_hardware_condition(|| control.is_busy());
        drop(control);
        let servicing = await_hardware_condition(|| {
            service.remote_error() == -1102 && service.statistics().service_cycles >= 2
        });
        let worker = service.worker.as_ref().unwrap().thread().clone();
        let before_fault = service.statistics().service_failures;
        GPIO_APPLIED_PTT.store(u32::from(physical_off_stuck), Ordering::Release);
        // A stopped DSP owner may attempt safe unkey before the regular device
        // poll. Fail both consecutive services so the normal poll also fails and
        // enters legacy retirement. Physical OFF either succeeds immediately or
        // remains unavailable until this test restores the device feedback.
        SHUTDOWN_SERVICE_FAILURES.store(2, Ordering::Release);
        worker.unpark();
        let retirement_started = await_hardware_condition(|| {
            SHUTDOWN_SERVICE_FAILURES.load(Ordering::Acquire) == 0
                && service.statistics().service_failures
                    >= before_fault + 3 + 2 * u64::from(physical_off_stuck)
        });
        GPIO_APPLIED_PTT.store(0, Ordering::Release);
        worker.unpark();
        let closed = await_hardware_condition(|| GPIO_CLOSES.load(Ordering::Acquire) >= 1);
        let before_reopen = service.statistics().service_failures;
        service.stop.store(true, Ordering::Release);
        FAILURE.store(FAIL_CM119_OPEN, Ordering::Release);
        worker.unpark();
        let reopen_failed = await_hardware_condition(|| {
            FAILURE.load(Ordering::Acquire) == 0
                && service.statistics().service_failures > before_reopen
        });
        // No regular service occurs after stop. Reopen's remote unkey is pulse1;
        // final close's pulse2 injects a failed safe write, followed by a retry.
        let before_close = service.statistics().service_failures;
        SHUTDOWN_PULSE_STAGE.store(1, Ordering::Release);
        worker.unpark();
        let close_failed = await_hardware_condition(|| {
            SHUTDOWN_PULSE_STAGE.load(Ordering::Acquire) >= 3
                && service.statistics().service_failures > before_close
        });
        worker.unpark();
        service.stop().unwrap();
        assert!(startup_waiting, "startup must request DSP adoption");
        assert!(
            servicing,
            "stopped DSP must leave the GPIO worker servicing"
        );
        assert!(
            retirement_started,
            "service outage must reach safe retirement"
        );
        assert!(closed, "recovery must close the retired device");
        assert!(
            reopen_failed,
            "shutdown must retry its failed device reopen"
        );
        assert!(
            close_failed,
            "final safe write must fail and retain ownership"
        );
        assert!(SHUTDOWN_PULSE_STAGE.load(Ordering::Acquire) >= 4);
        assert_eq!(PTT.load(Ordering::Acquire), 0);
    }
}
