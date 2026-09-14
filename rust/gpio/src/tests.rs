use super::*;
use std::sync::Mutex;
use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

static TEST_LOCK: Mutex<()> = Mutex::new(());
static RESULT: AtomicI32 = AtomicI32::new(RESULT_OK);
static RETURN_MODE: AtomicU32 = AtomicU32::new(0);
static CM119_CLOSES: AtomicU32 = AtomicU32::new(0);
static PARALLEL_CLOSES: AtomicU32 = AtomicU32::new(0);

#[derive(Default)]
struct FakeCm119 {
    outputs: Cm119Outputs,
    pulse: Cm119Pulse,
    service_count: u64,
}

#[derive(Default)]
struct FakeParallel {
    outputs: ParallelOutputs,
    pulse: ParallelPulse,
    service_count: u64,
    direct_data: u8,
    binary_channel: u8,
    rtx: Option<ParallelRtx>,
}

unsafe extern "C" fn device_probe(
    config: *const RawDeviceConfig,
    info: *mut RawDeviceInfo,
) -> c_int {
    let result = RESULT.load(Ordering::Relaxed);
    if result != RESULT_OK {
        return result;
    }
    // SAFETY: the tested wrapper supplies valid call-lifetime structures.
    let (config, info) = unsafe { (&*config, &mut *info) };
    if config.usb_port_path.is_null() {
        return -1;
    }
    populate_info(info, true, b"CM119-A");
    result
}

unsafe extern "C" fn device_open(
    _config: *const RawDeviceConfig,
    device: *mut *mut OpaqueCm119Device,
) -> c_int {
    let result = RESULT.load(Ordering::Relaxed);
    if result != RESULT_OK {
        return result;
    }
    // SAFETY: the tested wrapper supplies writable handle storage.
    unsafe {
        *device = Box::into_raw(Box::new(FakeCm119::default())).cast::<OpaqueCm119Device>();
    }
    result
}

unsafe extern "C" fn device_publish(
    device: *mut OpaqueCm119Device,
    outputs: *const RawCm119Outputs,
) -> c_int {
    // SAFETY: handles and actions originate from the tested wrapper.
    let (device, outputs) = unsafe { (&mut *device.cast::<FakeCm119>(), &*outputs) };
    device.outputs = Cm119Outputs {
        ptt_asserted: outputs.ptt_asserted != 0,
        gpio_output_mask: outputs.gpio_output_mask as u8,
    };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_schedule(
    device: *mut OpaqueCm119Device,
    pulse: *const RawCm119ScheduledPulse,
) -> c_int {
    // SAFETY: handles and actions originate from the tested wrapper.
    let (device, pulse) = unsafe { (&mut *device.cast::<FakeCm119>(), &*pulse) };
    device.pulse = Cm119Pulse {
        invert_ptt: pulse.ptt_invert != 0,
        invert_gpio_mask: pulse.gpio_invert_mask as u8,
        duration_milliseconds: pulse.pulse_duration_milliseconds,
        cancel_ptt: pulse.ptt_cancel != 0,
        cancel_gpio_mask: pulse.gpio_cancel_mask as u8,
    };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_service(device: *mut OpaqueCm119Device) -> c_int {
    // SAFETY: the handle originates from `device_open`.
    unsafe { (*device.cast::<FakeCm119>()).service_count += 1 };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_inputs(
    _device: *const OpaqueCm119Device,
    inputs: *mut RawCm119Inputs,
) -> c_int {
    // SAFETY: the tested wrapper supplies writable snapshot storage.
    unsafe {
        (*inputs).struct_size = size_of::<RawCm119Inputs>() as u32;
        (*inputs).abi_version = ABI_VERSION;
        (*inputs).online = 1;
        (*inputs).cor_active = 1;
        (*inputs).ctcss_active = 0;
        (*inputs).gpio_input_mask = if RETURN_MODE.load(Ordering::Relaxed) == 3 {
            256
        } else {
            0xa5
        };
        (*inputs).hid_report = [1, 2, 3, 4];
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_stats(
    device: *const OpaqueCm119Device,
    stats: *mut RawCm119Stats,
) -> c_int {
    // SAFETY: the handle and snapshot storage originate from the tested wrapper.
    let service_count = unsafe { (*device.cast::<FakeCm119>()).service_count };
    // SAFETY: `stats` is writable for this call.
    unsafe {
        *stats = RawCm119Stats {
            struct_size: size_of::<RawCm119Stats>() as u32,
            abi_version: ABI_VERSION,
            input_read_count: service_count,
            output_apply_count: 2,
            usb_error_count: 3,
            ptt_applied: 1,
            online: 1,
            last_usb_error: -7,
            eeprom_read_count: 8,
            eeprom_write_count: 9,
        };
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_close(device: *mut OpaqueCm119Device) {
    // SAFETY: this is the unique allocation returned by `device_open`.
    drop(unsafe { Box::from_raw(device.cast::<FakeCm119>()) });
    CM119_CLOSES.fetch_add(1, Ordering::Relaxed);
}

unsafe extern "C" fn device_discover(list: *mut RawDeviceList) -> c_int {
    // SAFETY: the tested wrapper supplies writable list storage.
    let list = unsafe { &mut *list };
    list.struct_size = size_of::<RawDeviceList>() as u32;
    list.abi_version = ABI_VERSION;
    list.matching_device_count = 3;
    list.returned_device_count = 2;
    populate_info(&mut list.devices[0], true, b"FIRST");
    populate_info(&mut list.devices[1], true, b"");
    match RETURN_MODE.load(Ordering::Relaxed) {
        1 => list.struct_size = 0,
        2 => list.returned_device_count = (DEVICE_CAPACITY + 1) as u32,
        4 => list.matching_device_count = 1,
        _ => {}
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_read_eeprom(
    _device: *mut OpaqueCm119Device,
    image: *mut RawEepromImage,
) -> c_int {
    // SAFETY: the tested wrapper supplies writable image storage.
    unsafe {
        (*image).struct_size = size_of::<RawEepromImage>() as u32;
        (*image).abi_version = ABI_VERSION;
        (*image).checksum_valid = 1;
        (*image).magic_valid = 1;
        (*image).words[51] = 34329;
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn device_write_eeprom(
    _device: *mut OpaqueCm119Device,
    image: *mut RawEepromImage,
) -> c_int {
    // SAFETY: the tested wrapper supplies writable image storage.
    unsafe {
        (*image).checksum_valid = 1;
        (*image).magic_valid = 1;
        (*image).words[63] = 0x55aa;
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_open(
    _config: *const RawParallelConfig,
    device: *mut *mut OpaqueParallelDevice,
) -> c_int {
    let result = RESULT.load(Ordering::Relaxed);
    if result == RESULT_OK {
        // SAFETY: the tested wrapper supplies writable handle storage.
        unsafe {
            *device =
                Box::into_raw(Box::new(FakeParallel::default())).cast::<OpaqueParallelDevice>();
        }
    }
    result
}

unsafe extern "C" fn parallel_publish(
    device: *mut OpaqueParallelDevice,
    outputs: *const RawParallelOutputs,
) -> c_int {
    // SAFETY: handles and actions originate from the tested wrapper.
    let (device, outputs) = unsafe { (&mut *device.cast::<FakeParallel>(), &*outputs) };
    device.outputs = ParallelOutputs {
        output_mask: outputs.output_mask as u8,
        pulse_mask: outputs.pulse_mask as u8,
        pulse_duration_milliseconds: outputs.pulse_duration_milliseconds,
        cancel_pulse: outputs.cancel_pulse != 0,
    };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_schedule(
    device: *mut OpaqueParallelDevice,
    pulse: *const RawParallelScheduledPulse,
) -> c_int {
    // SAFETY: handles and actions originate from the tested wrapper.
    let (device, pulse) = unsafe { (&mut *device.cast::<FakeParallel>(), &*pulse) };
    device.pulse = ParallelPulse {
        invert_mask: pulse.invert_mask as u8,
        duration_milliseconds: pulse.pulse_duration_milliseconds,
        cancel_mask: pulse.cancel_mask as u8,
    };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_service(device: *mut OpaqueParallelDevice) -> c_int {
    // SAFETY: the handle originates from `parallel_open`.
    unsafe { (*device.cast::<FakeParallel>()).service_count += 1 };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_write(device: *mut OpaqueParallelDevice, data: u32) -> c_int {
    // SAFETY: the handle originates from `parallel_open`.
    unsafe { (*device.cast::<FakeParallel>()).direct_data = data as u8 };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_set_binary_channel(
    device: *mut OpaqueParallelDevice,
    channel: u8,
) -> c_int {
    // SAFETY: the handle originates from `parallel_open`.
    unsafe { (*device.cast::<FakeParallel>()).binary_channel = channel };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_program_rtx(
    device: *mut OpaqueParallelDevice,
    receive_hz: u32,
    transmit_hz: u32,
    transmitting: u32,
    high_power: u32,
) -> c_int {
    // SAFETY: the handle originates from `parallel_open`.
    unsafe {
        (*device.cast::<FakeParallel>()).rtx = Some(ParallelRtx {
            receive_hz,
            transmit_hz,
            transmitting: transmitting != 0,
            high_power: high_power != 0,
        })
    };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_clear_rtx_transmit(device: *mut OpaqueParallelDevice) -> c_int {
    // SAFETY: the handle originates from `parallel_open`.
    if let Some(request) = unsafe { &mut (*device.cast::<FakeParallel>()).rtx } {
        request.transmitting = false;
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_inputs(
    _device: *const OpaqueParallelDevice,
    inputs: *mut RawParallelInputs,
) -> c_int {
    // SAFETY: the tested wrapper supplies writable snapshot storage.
    unsafe {
        (*inputs).struct_size = size_of::<RawParallelInputs>() as u32;
        (*inputs).abi_version = ABI_VERSION;
        (*inputs).online = 1;
        (*inputs).status_mask = if RETURN_MODE.load(Ordering::Relaxed) == 3 {
            300
        } else {
            0x87
        };
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_stats(
    device: *const OpaqueParallelDevice,
    stats: *mut RawParallelStats,
) -> c_int {
    // SAFETY: the handle and storage originate from the tested wrapper.
    let service_count = unsafe { (*device.cast::<FakeParallel>()).service_count };
    // SAFETY: `stats` is writable for this call.
    unsafe {
        *stats = RawParallelStats {
            struct_size: size_of::<RawParallelStats>() as u32,
            abi_version: ABI_VERSION,
            input_read_count: service_count,
            output_apply_count: 2,
            io_error_count: 3,
            online: 1,
            last_io_error: -5,
            applied_output_mask: 0x42,
        };
    }
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn parallel_close(device: *mut OpaqueParallelDevice) {
    // SAFETY: this is the unique allocation returned by `parallel_open`.
    drop(unsafe { Box::from_raw(device.cast::<FakeParallel>()) });
    PARALLEL_CLOSES.fetch_add(1, Ordering::Relaxed);
}

unsafe extern "C" fn unused_extension() -> c_int {
    RESULT_OK
}

fn descriptor() -> Descriptor {
    Descriptor {
        struct_size: size_of::<Descriptor>() as u32,
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY.as_ptr(),
        device_probe: Some(device_probe),
        device_open: Some(device_open),
        device_publish_outputs: Some(device_publish),
        device_service: Some(device_service),
        device_get_inputs: Some(device_inputs),
        device_get_stats: Some(device_stats),
        device_close: Some(device_close),
        device_discover: Some(device_discover),
        device_read_eeprom: Some(device_read_eeprom),
        device_write_eeprom: Some(device_write_eeprom),
        parallel_open: Some(parallel_open),
        parallel_publish_outputs: Some(parallel_publish),
        parallel_service: Some(parallel_service),
        parallel_control_write_data: Some(parallel_write),
        parallel_get_inputs: Some(parallel_inputs),
        parallel_get_stats: Some(parallel_stats),
        parallel_close: Some(parallel_close),
        device_publish_inverting_pulse: Some(unused_extension),
        parallel_publish_inverting_pulse: Some(unused_extension),
        device_schedule_inverting_pulse: Some(device_schedule),
        parallel_schedule_inverting_pulse: Some(parallel_schedule),
        parallel_set_binary_channel: Some(parallel_set_binary_channel),
        parallel_program_rtx: Some(parallel_program_rtx),
        parallel_clear_rtx_transmit: Some(parallel_clear_rtx_transmit),
    }
}

fn adapter(raw: &Descriptor) -> GpioAdapter {
    // SAFETY: the local descriptor and its functions live throughout each test.
    unsafe { GpioAdapter::from_raw(ptr::from_ref(raw).cast::<c_void>()).unwrap() }
}

fn cm119_config() -> Cm119Config {
    Cm119Config {
        usb_port_path: "3-1.2".into(),
        vendor_id: 0,
        product_id: 0,
        profile: Cm119Profile::DudeUsb,
        ptt_inverted: false,
        output_enable_mask: 0xf0,
        output_initial_mask: 0x20,
    }
}

fn populate_info(info: &mut RawDeviceInfo, present: bool, serial: &[u8]) {
    *info = RawDeviceInfo::default();
    info.present = u32::from(present);
    info.vendor_id = 0x0d8c;
    info.product_id = 0x013c;
    info.usb_bus = 3;
    info.usb_port_number_count = 2;
    info.usb_port_numbers[..2].copy_from_slice(&[1, 2]);
    for (destination, source) in info.serial.iter_mut().zip(serial) {
        *destination = *source as c_char;
    }
}

fn reset() {
    RESULT.store(RESULT_OK, Ordering::Relaxed);
    RETURN_MODE.store(0, Ordering::Relaxed);
    CM119_CLOSES.store(0, Ordering::Relaxed);
    PARALLEL_CLOSES.store(0, Ordering::Relaxed);
}

#[test]
fn cm119_lifecycle_and_snapshots_are_typed() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();
    let raw = descriptor();
    let adapter = adapter(&raw);

    let list = adapter.discover().unwrap();
    assert_eq!(list.matching_device_count, 3);
    assert_eq!(list.devices.len(), 2);
    assert_eq!(list.devices[0].serial.as_deref(), Some("FIRST"));
    assert_eq!(list.devices[1].serial, None);

    let info = adapter.probe(&cm119_config()).unwrap();
    assert!(info.present);
    assert_eq!(info.usb_port_numbers, [1, 2]);
    assert_eq!(info.serial.as_deref(), Some("CM119-A"));

    let mut device = adapter.open_cm119(&cm119_config()).unwrap();
    device
        .publish_outputs(Cm119Outputs {
            ptt_asserted: true,
            gpio_output_mask: 0x41,
        })
        .unwrap();
    device
        .schedule_pulse(Cm119Pulse {
            invert_ptt: true,
            invert_gpio_mask: 2,
            duration_milliseconds: 125,
            ..Cm119Pulse::default()
        })
        .unwrap();
    device.service().unwrap();
    assert_eq!(
        device.inputs().unwrap(),
        Cm119Inputs {
            online: true,
            cor_active: true,
            ctcss_active: false,
            gpio_input_mask: 0xa5,
            hid_report: [1, 2, 3, 4],
        }
    );
    assert_eq!(
        device.statistics().unwrap(),
        Cm119Statistics {
            input_read_count: 1,
            output_apply_count: 2,
            usb_error_count: 3,
            ptt_applied: true,
            online: true,
            last_usb_error: -7,
            eeprom_read_count: 8,
            eeprom_write_count: 9,
        }
    );
    let mut image = device.read_eeprom().unwrap();
    assert!(image.checksum_valid && image.magic_valid);
    assert_eq!(image.words[51], 34329);
    device.write_eeprom(&mut image).unwrap();
    assert_eq!(image.words[63], 0x55aa);
    drop(device);
    assert_eq!(CM119_CLOSES.load(Ordering::Relaxed), 1);
}

#[test]
fn parallel_lifecycle_and_snapshots_are_typed() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();
    let raw = descriptor();
    let adapter = adapter(&raw);
    let mut device = adapter
        .open_parallel(&ParallelConfig {
            transport: ParallelTransport::Ppdev,
            ppdev_path: Some("/dev/parport0".into()),
            raw_io_base: 0,
            output_enable_mask: 0xff,
            output_initial_mask: 0x10,
        })
        .unwrap();
    device
        .publish_outputs(ParallelOutputs {
            output_mask: 0x22,
            pulse_mask: 0x04,
            pulse_duration_milliseconds: 50,
            cancel_pulse: false,
        })
        .unwrap();
    device
        .schedule_pulse(ParallelPulse {
            invert_mask: 0x08,
            duration_milliseconds: 75,
            cancel_mask: 0,
        })
        .unwrap();
    device.write_data(0x56).unwrap();
    device.set_binary_channel(0x0d).unwrap();
    let rtx = ParallelRtx {
        receive_hz: 146_520_000,
        transmit_hz: 147_120_000,
        transmitting: true,
        high_power: true,
    };
    device.program_rtx(rtx).unwrap();
    // SAFETY: the fake handle remains live and exclusively owned here.
    let fake = unsafe { &*device.device.as_ptr().cast::<FakeParallel>() };
    assert_eq!(fake.binary_channel, 0x0d);
    assert_eq!(fake.rtx, Some(rtx));
    device.clear_rtx_transmit().unwrap();
    // SAFETY: the fake handle remains live and exclusively owned here.
    let transmitting = unsafe { (*device.device.as_ptr().cast::<FakeParallel>()).rtx }
        .unwrap()
        .transmitting;
    assert!(!transmitting);
    device.service().unwrap();
    assert_eq!(
        device.inputs().unwrap(),
        ParallelInputs {
            online: true,
            status_mask: 0x87,
        }
    );
    assert_eq!(
        device.statistics().unwrap(),
        ParallelStatistics {
            input_read_count: 1,
            output_apply_count: 2,
            io_error_count: 3,
            online: true,
            last_io_error: -5,
            applied_output_mask: 0x42,
        }
    );
    drop(device);
    assert_eq!(PARALLEL_CLOSES.load(Ordering::Relaxed), 1);
}

#[test]
fn optional_parallel_compatibility_operations_report_unsupported() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();
    let mut raw = descriptor();
    raw.parallel_set_binary_channel = None;
    raw.parallel_program_rtx = None;
    raw.parallel_clear_rtx_transmit = None;
    let mut device = adapter(&raw)
        .open_parallel(&ParallelConfig {
            transport: ParallelTransport::RawIo,
            ppdev_path: None,
            raw_io_base: 0x378,
            output_enable_mask: 0xff,
            output_initial_mask: 0,
        })
        .unwrap();
    assert_eq!(device.set_binary_channel(1), Err(GpioError::Unsupported));
    assert_eq!(
        device.program_rtx(ParallelRtx {
            receive_hz: 1,
            transmit_hz: 2,
            transmitting: false,
            high_power: false,
        }),
        Err(GpioError::Unsupported)
    );
    assert_eq!(device.clear_rtx_transmit(), Err(GpioError::Unsupported));
}

#[test]
fn descriptor_and_adapter_failures_are_rejected() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();
    // SAFETY: null is intentionally supplied to test validation.
    let result = unsafe { GpioAdapter::from_raw(ptr::null()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
    let mut raw = descriptor();
    raw.struct_size = 0;
    // SAFETY: `raw` is a readable but deliberately incompatible descriptor.
    let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
    raw = descriptor();
    raw.abi_version = 9;
    // SAFETY: `raw` is a readable but deliberately incompatible descriptor.
    let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
    raw = descriptor();
    raw.capability_name = c"wrong".as_ptr();
    // SAFETY: `raw` is a readable but deliberately incompatible descriptor.
    let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
    raw = descriptor();
    raw.device_probe = None;
    // SAFETY: `raw` is a readable but deliberately incomplete descriptor.
    let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
    assert!(!GpioError::IncompatibleAdapter.to_string().is_empty());

    for (code, expected) in [
        (-1, GpioError::InvalidArgument),
        (-2, GpioError::NoMemory),
        (-3, GpioError::Usb),
        (-4, GpioError::Unsupported),
        (-5, GpioError::Io),
        (-99, GpioError::AdapterFailure),
    ] {
        assert_eq!(map_result(code), Err(expected));
        assert!(!expected.to_string().is_empty());
    }
}

#[test]
fn descriptor_and_returned_values_reject_every_invalid_abi_form() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();

    let mut info = RawDeviceInfo::default();
    info.usb_port_number_count = (info.usb_port_numbers.len() + 1) as u32;
    assert_eq!(device_info_from_raw(&info), Err(GpioError::InvalidArgument));
    assert_eq!(
        string_from_array(&[0xff_u8 as c_char, 0]),
        Err(GpioError::InvalidArgument)
    );
    assert_eq!(
        validate_returned(
            size_of::<RawDeviceInfo>() as u32,
            ABI_VERSION + 1,
            size_of::<RawDeviceInfo>(),
        ),
        Err(GpioError::InvalidArgument)
    );

    let mut raw = descriptor();
    raw.capability_name = ptr::null();
    // SAFETY: `raw` is readable but intentionally lacks a required header field.
    let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));

    macro_rules! assert_missing_function_is_rejected {
        ($field:ident) => {{
            let mut incomplete = descriptor();
            incomplete.$field = None;
            // SAFETY: `incomplete` remains readable for the validation call.
            let result = unsafe { GpioAdapter::from_raw(ptr::from_ref(&incomplete).cast()) };
            assert!(matches!(result, Err(GpioError::IncompatibleAdapter)));
        }};
    }

    assert_missing_function_is_rejected!(device_probe);
    assert_missing_function_is_rejected!(device_open);
    assert_missing_function_is_rejected!(device_publish_outputs);
    assert_missing_function_is_rejected!(device_service);
    assert_missing_function_is_rejected!(device_get_inputs);
    assert_missing_function_is_rejected!(device_get_stats);
    assert_missing_function_is_rejected!(device_close);
    assert_missing_function_is_rejected!(device_discover);
    assert_missing_function_is_rejected!(device_read_eeprom);
    assert_missing_function_is_rejected!(device_write_eeprom);
    assert_missing_function_is_rejected!(parallel_open);
    assert_missing_function_is_rejected!(parallel_publish_outputs);
    assert_missing_function_is_rejected!(parallel_service);
    assert_missing_function_is_rejected!(parallel_control_write_data);
    assert_missing_function_is_rejected!(parallel_get_inputs);
    assert_missing_function_is_rejected!(parallel_get_stats);
    assert_missing_function_is_rejected!(parallel_close);
    assert_missing_function_is_rejected!(device_schedule_inverting_pulse);
    assert_missing_function_is_rejected!(parallel_schedule_inverting_pulse);
}

#[test]
fn invalid_arguments_and_returned_values_fail_safely() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset();
    let raw = descriptor();
    let adapter = adapter(&raw);

    let mut bad = cm119_config();
    bad.usb_port_path.clear();
    assert!(matches!(
        adapter.probe(&bad),
        Err(GpioError::InvalidArgument)
    ));
    bad.usb_port_path = "bad\0path".into();
    assert!(matches!(
        adapter.open_cm119(&bad),
        Err(GpioError::InvalidArgument)
    ));
    assert!(matches!(
        adapter.open_parallel(&ParallelConfig {
            transport: ParallelTransport::Automatic,
            ppdev_path: Some("bad\0path".into()),
            raw_io_base: 0x378,
            output_enable_mask: 1,
            output_initial_mask: 0,
        }),
        Err(GpioError::InvalidArgument)
    ));

    let mut cm119 = adapter.open_cm119(&cm119_config()).unwrap();
    assert_eq!(
        cm119.schedule_pulse(Cm119Pulse {
            invert_ptt: true,
            cancel_ptt: true,
            ..Cm119Pulse::default()
        }),
        Err(GpioError::InvalidArgument)
    );
    assert_eq!(
        cm119.schedule_pulse(Cm119Pulse {
            invert_gpio_mask: 1,
            cancel_gpio_mask: 1,
            ..Cm119Pulse::default()
        }),
        Err(GpioError::InvalidArgument)
    );
    assert_eq!(cm119.schedule_pulse(Cm119Pulse::default()), Ok(()));
    RETURN_MODE.store(3, Ordering::Relaxed);
    assert_eq!(cm119.inputs(), Err(GpioError::InvalidArgument));
    RETURN_MODE.store(0, Ordering::Relaxed);
    RESULT.store(-3, Ordering::Relaxed);
    assert_eq!(cm119.service(), Err(GpioError::Usb));
    RESULT.store(RESULT_OK, Ordering::Relaxed);

    let mut parallel = adapter
        .open_parallel(&ParallelConfig {
            transport: ParallelTransport::RawIo,
            ppdev_path: None,
            raw_io_base: 0x378,
            output_enable_mask: 0xff,
            output_initial_mask: 0,
        })
        .unwrap();
    assert_eq!(
        parallel.schedule_pulse(ParallelPulse {
            invert_mask: 1,
            cancel_mask: 1,
            ..ParallelPulse::default()
        }),
        Err(GpioError::InvalidArgument)
    );
    RETURN_MODE.store(3, Ordering::Relaxed);
    assert_eq!(parallel.inputs(), Err(GpioError::InvalidArgument));
    RETURN_MODE.store(0, Ordering::Relaxed);
    RESULT.store(-5, Ordering::Relaxed);
    assert_eq!(parallel.service(), Err(GpioError::Io));
    RESULT.store(RESULT_OK, Ordering::Relaxed);

    RETURN_MODE.store(1, Ordering::Relaxed);
    assert_eq!(adapter.discover(), Err(GpioError::InvalidArgument));
    RETURN_MODE.store(2, Ordering::Relaxed);
    assert_eq!(adapter.discover(), Err(GpioError::InvalidArgument));
    RETURN_MODE.store(4, Ordering::Relaxed);
    assert_eq!(adapter.discover(), Err(GpioError::InvalidArgument));
}
