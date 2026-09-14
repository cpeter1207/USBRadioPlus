use super::*;
use std::sync::atomic::{AtomicPtr, AtomicU32, Ordering};

static STARTS: AtomicU32 = AtomicU32::new(0);
static STOPS: AtomicU32 = AtomicU32::new(0);
static DESTROYS: AtomicU32 = AtomicU32::new(0);
static LAST_STEPS: AtomicU32 = AtomicU32::new(0);
static RECEIVES: AtomicU32 = AtomicU32::new(0);
static TRANSMITS: AtomicU32 = AtomicU32::new(0);
static PARTIAL_STREAM: AtomicPtr<u8> = AtomicPtr::new(std::ptr::null_mut());
static PARTIAL_MIXER: AtomicPtr<u32> = AtomicPtr::new(std::ptr::null_mut());

unsafe extern "C" fn stream_create(
    config: *const RawStreamConfig,
    output: *mut *mut OpaqueStream,
) -> c_int {
    // SAFETY: Tests call through the validated wrapper with live storage.
    let config = unsafe { &*config };
    assert_eq!(config.native_sample_rate_hz, NATIVE_RATE_HZ);
    assert_eq!(config.input_device_channels, 1);
    assert_eq!(config.output_device_channels, 2);
    assert!(config.receive_worker.is_some());
    assert!(config.transmit_worker.is_some());
    assert!(!config.receive_worker_context.is_null());
    assert!(!config.transmit_worker_context.is_null());
    if config.maximum_receive_frame_count == 99 {
        return -3;
    }
    let input = [0.0_f32; 2];
    let mut output_samples = [0.0_f32; 2];
    assert_eq!(
        // SAFETY: The test owns both live contexts and complete stereo buffers.
        unsafe { config.receive_worker.unwrap()(config.receive_worker_context, input.as_ptr(), 1) },
        0
    );
    assert_eq!(
        // SAFETY: The test owns the live context and writable stereo output.
        unsafe {
            config.transmit_worker.unwrap()(
                config.transmit_worker_context,
                output_samples.as_mut_ptr(),
                1,
            )
        },
        0
    );
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(7_u8)).cast() };
    RESULT_OK
}

unsafe extern "C" fn stream_start(_stream: *mut OpaqueStream) -> c_int {
    STARTS.fetch_add(1, Ordering::Relaxed);
    RESULT_OK
}

unsafe extern "C" fn stream_stop(_stream: *mut OpaqueStream) -> c_int {
    STOPS.fetch_add(1, Ordering::Relaxed);
    RESULT_OK
}

unsafe extern "C" fn stream_stats(
    _stream: *const OpaqueStream,
    stats: *mut RawStreamStatistics,
) -> c_int {
    // SAFETY: The wrapper provides complete writable storage.
    let stats = unsafe { &mut *stats };
    stats.abi_version = ABI_VERSION;
    stats.callback_count = 3;
    stats.callback_max_duration_ns = 41;
    stats.worker_failure_count = 7;
    RESULT_OK
}

unsafe extern "C" fn stream_timing(
    _stream: *const OpaqueStream,
    timing: *mut RawStreamTiming,
) -> c_int {
    // SAFETY: The wrapper provides complete writable storage.
    let timing = unsafe { &mut *timing };
    timing.abi_version = ABI_VERSION;
    timing.input_latency_seconds = 0.01;
    timing.output_latency_seconds = 0.02;
    timing.sample_rate_hz = 48_000.0;
    RESULT_OK
}

unsafe extern "C" fn stream_destroy(stream: *mut OpaqueStream) {
    if !stream.is_null() {
        // SAFETY: Test stream handles originate from Box::into_raw above.
        drop(unsafe { Box::from_raw(stream.cast::<u8>()) });
        DESTROYS.fetch_add(1, Ordering::Relaxed);
    }
}

unsafe extern "C" fn mixer_create(
    _config: *const RawMixerConfig,
    _output: *mut *mut OpaqueMixer,
) -> c_int {
    -5
}

unsafe extern "C" fn mixer_usb_create(
    config: *const RawUsbMixerConfig,
    output: *mut *mut OpaqueMixer,
) -> c_int {
    // SAFETY: The wrapper supplies a live setup object.
    let config = unsafe { &*config };
    assert_eq!(config.channel, 0);
    // SAFETY: The wrapper supplies a NUL-terminated element for this call.
    let element = unsafe { CStr::from_ptr(config.element) }.to_bytes();
    if element == b"missing" {
        return -5;
    }
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(500_u32)).cast() };
    if element == b"partial" {
        return -4;
    }
    RESULT_OK
}

unsafe extern "C" fn stream_create_partial_failure(
    _config: *const RawStreamConfig,
    output: *mut *mut OpaqueStream,
) -> c_int {
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = Box::into_raw(Box::new(7_u8)).cast() };
    -3
}

unsafe extern "C" fn stream_create_partial_without_destroy(
    _config: *const RawStreamConfig,
    output: *mut *mut OpaqueStream,
) -> c_int {
    let handle = Box::into_raw(Box::new(7_u8));
    PARTIAL_STREAM.store(handle, Ordering::Relaxed);
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = handle.cast() };
    -3
}

unsafe extern "C" fn mixer_create_partial_without_destroy(
    _config: *const RawUsbMixerConfig,
    output: *mut *mut OpaqueMixer,
) -> c_int {
    let handle = Box::into_raw(Box::new(500_u32));
    PARTIAL_MIXER.store(handle, Ordering::Relaxed);
    // SAFETY: The wrapper supplies writable handle storage.
    unsafe { *output = handle.cast() };
    -4
}

unsafe extern "C" fn mixer_range(
    _mixer: *const OpaqueMixer,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: Test wrapper supplies writable outputs.
    unsafe {
        *minimum = 0;
        *maximum = 2000;
    }
    RESULT_OK
}

unsafe extern "C" fn invalid_mixer_range(
    _mixer: *const OpaqueMixer,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: Test wrapper supplies writable outputs.
    unsafe {
        *minimum = 2;
        *maximum = 1;
    }
    RESULT_OK
}

unsafe extern "C" fn high_minimum_mixer_range(
    _mixer: *const OpaqueMixer,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: Test wrapper supplies writable outputs.
    unsafe {
        *minimum = 1500;
        *maximum = 2000;
    }
    RESULT_OK
}

unsafe extern "C" fn negative_maximum_mixer_range(
    _mixer: *const OpaqueMixer,
    minimum: *mut i64,
    maximum: *mut i64,
) -> c_int {
    // SAFETY: Test wrapper supplies writable outputs.
    unsafe {
        *minimum = -2;
        *maximum = -1;
    }
    RESULT_OK
}

unsafe extern "C" fn mixer_get_i64(_mixer: *const OpaqueMixer, value: *mut i64) -> c_int {
    // SAFETY: Test wrapper supplies writable output.
    unsafe { *value = 0 };
    RESULT_OK
}

unsafe extern "C" fn mixer_set_i64(_mixer: *mut OpaqueMixer, value: i64) -> c_int {
    LAST_STEPS.store(value as u32, Ordering::Relaxed);
    RESULT_OK
}

unsafe extern "C" fn mixer_get(mixer: *const OpaqueMixer, value: *mut u32) -> c_int {
    // SAFETY: Handle and output originate from the wrapper.
    unsafe { *value = *mixer.cast::<u32>() };
    RESULT_OK
}

unsafe extern "C" fn mixer_set(mixer: *mut OpaqueMixer, value: u32) -> c_int {
    // SAFETY: Handle originates from Box::into_raw and is exclusively borrowed.
    unsafe { *mixer.cast::<u32>() = value };
    RESULT_OK
}

unsafe extern "C" fn mixer_destroy(mixer: *mut OpaqueMixer) {
    if !mixer.is_null() {
        // SAFETY: Test mixer handles originate from Box::into_raw above.
        drop(unsafe { Box::from_raw(mixer.cast::<u32>()) });
    }
}

unsafe extern "C" fn device_resolve(
    _identity: *const RawDeviceIdentity,
    _selection: *mut RawDeviceSelection,
) -> c_int {
    -5
}

unsafe extern "C" fn device_select(
    selector: *const RawDeviceSelector,
    output: *mut RawDeviceMatch,
) -> c_int {
    // SAFETY: The wrapper supplies live request/result storage.
    let (selector, output) = unsafe { (&*selector, &mut *output) };
    let identifier = if selector.device_identifier.is_null() {
        &[][..]
    } else {
        // SAFETY: The wrapper supplies a live NUL-terminated identifier.
        unsafe { CStr::from_ptr(selector.device_identifier) }.to_bytes()
    };
    output.abi_version = ABI_VERSION;
    copy_c_string(&mut output.usb_interface_path, b"3-1:1.0");
    copy_c_string(&mut output.usb_serial, b"ABC");
    output.selection.struct_size = size_of::<RawDeviceSelection>() as u32;
    output.selection.abi_version = ABI_VERSION;
    output.selection.alsa_card_index = 2;
    output.selection.input_device_index = 4;
    output.selection.output_device_index = 5;
    match identifier {
        b"invalid-structure" => output.struct_size = 0,
        b"invalid-selection-structure" => output.selection.struct_size = 0,
        b"invalid-input" => output.selection.input_device_index = -1,
        b"invalid-output" => output.selection.output_device_index = -1,
        b"wrong-abi" => output.abi_version = ABI_VERSION + 1,
        b"empty-interface" => output.usb_interface_path.fill(0),
        b"unterminated-interface" => output.usb_interface_path.fill(b'x' as c_char),
        b"invalid-interface-utf8" => {
            output.usb_interface_path.fill(0);
            output.usb_interface_path[0] = -1;
        }
        b"invalid-serial-utf8" => {
            output.usb_serial.fill(0);
            output.usb_serial[0] = -1;
        }
        _ => {}
    }
    RESULT_OK
}

unsafe extern "C" fn mixer_paths(_interface: *const c_char, output: *mut RawMixerPaths) -> c_int {
    // SAFETY: The wrapper supplies writable result storage.
    let output = unsafe { &mut *output };
    output.abi_version = ABI_VERSION;
    output.rx_capture_path_count = 1;
    copy_c_string(&mut output.rx_capture_paths[0].element, b"Mic");
    output.rx_capture_paths[0].direction = MixerDirection::Capture as u32;
    output.rx_capture_paths[0].capabilities = 3;
    output.tx_playback_path_count = 1;
    copy_c_string(&mut output.tx_playback_paths[0].element, b"Speaker");
    output.tx_playback_paths[0].direction = MixerDirection::Playback as u32;
    output.tx_playback_paths[0].capabilities = 1;
    RESULT_OK
}

unsafe extern "C" fn empty_mixer_paths(
    interface: *const c_char,
    output: *mut RawMixerPaths,
) -> c_int {
    // SAFETY: The wrapper supplies a NUL-terminated interface and writable result.
    let (interface, output) = unsafe { (CStr::from_ptr(interface).to_bytes(), &mut *output) };
    output.abi_version = ABI_VERSION;
    if interface == b"small" {
        output.struct_size = 0;
    } else if interface == b"no-transmit" {
        output.rx_capture_path_count = 1;
    }
    RESULT_OK
}

fn copy_c_string<const N: usize>(output: &mut [c_char; N], value: &[u8]) {
    for (destination, source) in output.iter_mut().zip(value) {
        *destination = *source as c_char;
    }
}

unsafe extern "C" fn receive(_context: *mut c_void, _input: *const f32, _frames: u32) -> i32 {
    RECEIVES.fetch_add(1, Ordering::Relaxed);
    0
}

unsafe extern "C" fn transmit(_context: *mut c_void, _output: *mut f32, _frames: u32) -> i32 {
    TRANSMITS.fetch_add(1, Ordering::Relaxed);
    0
}

static VALID: Descriptor = Descriptor {
    struct_size: size_of::<Descriptor>() as u32,
    abi_version: ABI_VERSION,
    capability_name: CAPABILITY.as_ptr(),
    stream_create: Some(stream_create),
    stream_start: Some(stream_start),
    stream_stop: Some(stream_stop),
    stream_get_stats: Some(stream_stats),
    stream_destroy: Some(stream_destroy),
    mixer_create: Some(mixer_create),
    mixer_get_range_centibels: Some(mixer_range),
    mixer_get_centibels: Some(mixer_get_i64),
    mixer_set_centibels: Some(mixer_set_i64),
    mixer_destroy: Some(mixer_destroy),
    mixer_create_for_usb_interface: Some(mixer_usb_create),
    mixer_get_range_steps: Some(mixer_range),
    mixer_get_steps: Some(mixer_get_i64),
    mixer_set_steps: Some(mixer_set_i64),
    mixer_get_normalized: Some(mixer_get),
    mixer_set_normalized: Some(mixer_set),
    mixer_get_switch: Some(mixer_get),
    mixer_set_switch: Some(mixer_set),
    usb_device_resolve: Some(device_resolve),
    usb_device_select: Some(device_select),
    stream_get_timing: Some(stream_timing),
    cm119_mixer_paths_resolve: Some(mixer_paths),
};

// SAFETY: The test descriptor and all referenced storage are immutable/static.
unsafe impl Sync for Descriptor {}

fn provider(descriptor: &'static Descriptor) -> Result<AudioProvider, AudioError> {
    // SAFETY: The descriptor has static immutable lifetime.
    unsafe { AudioProvider::from_raw_descriptor(std::ptr::from_ref(descriptor).cast()) }
}

fn selector() -> DeviceSelector {
    DeviceSelector {
        policy: SelectionPolicy::Exact,
        identifier: Some("3-1".to_owned()),
        serial: None,
        input_channels: ChannelCount::Mono,
        output_channels: ChannelCount::Stereo,
    }
}

fn endpoints<'receive, 'transmit>(
    receive_context: &'receive mut u8,
    transmit_context: &'transmit mut u8,
) -> (
    ReceiveWorkerEndpoint<'receive>,
    TransmitWorkerEndpoint<'transmit>,
) {
    // SAFETY: The typed borrows keep both test contexts alive for the endpoints.
    let receive = unsafe {
        ReceiveWorkerEndpoint::from_raw(receive, NonNull::from(receive_context).cast::<c_void>())
    };
    // SAFETY: The transmit context has an independent live typed borrow.
    let transmit = unsafe {
        TransmitWorkerEndpoint::from_raw(transmit, NonNull::from(transmit_context).cast::<c_void>())
    };
    (receive, transmit)
}

fn stream_config(receive_maximum: u32, transmit_maximum: u32) -> StreamConfig {
    StreamConfig {
        maximum_receive_frame_count: receive_maximum,
        maximum_transmit_frame_count: transmit_maximum,
        input_device_index: 4,
        output_device_index: 5,
        input_channels: ChannelCount::Mono,
        output_channels: ChannelCount::Stereo,
    }
}

#[test]
fn descriptor_and_selection_validate_complete_results() {
    assert_eq!(
        // SAFETY: Null is explicitly rejected.
        unsafe { AudioProvider::from_raw_descriptor(std::ptr::null()) }.err(),
        Some(AudioError::IncompatibleAdapter)
    );
    let mut bad = VALID;
    bad.struct_size = (REQUIRED_DESCRIPTOR_SIZE - 1) as u32;
    assert_eq!(
        provider(Box::leak(Box::new(bad))).err(),
        Some(AudioError::IncompatibleAdapter)
    );
    bad = VALID;
    bad.abi_version = ABI_VERSION - 1;
    assert_eq!(
        provider(Box::leak(Box::new(bad))).err(),
        Some(AudioError::IncompatibleAdapter)
    );
    bad = VALID;
    bad.capability_name = std::ptr::null();
    assert_eq!(
        provider(Box::leak(Box::new(bad))).err(),
        Some(AudioError::IncompatibleAdapter)
    );
    let selected = provider(&VALID)
        .unwrap()
        .select_device(&selector())
        .unwrap();
    assert_eq!(selected.interface_path, "3-1:1.0");
    assert_eq!(selected.serial.as_deref(), Some("ABC"));
    assert_eq!(selected.alsa_card_index, 2);
}

#[test]
fn every_required_descriptor_entry_is_validated() {
    macro_rules! missing {
        ($field:ident) => {{
            let mut descriptor = VALID;
            descriptor.$field = None;
            assert!(!descriptor_complete(&descriptor));
        }};
    }
    missing!(stream_create);
    missing!(stream_start);
    missing!(stream_stop);
    missing!(stream_get_stats);
    missing!(stream_destroy);
    missing!(mixer_destroy);
    missing!(mixer_create_for_usb_interface);
    missing!(mixer_get_range_steps);
    missing!(mixer_get_steps);
    missing!(mixer_set_steps);
    missing!(mixer_get_normalized);
    missing!(mixer_set_normalized);
    missing!(mixer_get_switch);
    missing!(mixer_set_switch);
    missing!(usb_device_resolve);
    missing!(usb_device_select);
    missing!(stream_get_timing);
    missing!(cm119_mixer_paths_resolve);

    let mut descriptor = VALID;
    descriptor.stream_create = None;
    assert_eq!(
        provider(Box::leak(Box::new(descriptor))).err(),
        Some(AudioError::IncompatibleAdapter)
    );
}

#[test]
fn stream_lifecycle_timing_and_statistics_are_owned() {
    STARTS.store(0, Ordering::Relaxed);
    STOPS.store(0, Ordering::Relaxed);
    DESTROYS.store(0, Ordering::Relaxed);
    RECEIVES.store(0, Ordering::Relaxed);
    TRANSMITS.store(0, Ordering::Relaxed);
    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive, transmit) = endpoints(&mut receive_context, &mut transmit_context);
    let mut stream = provider(&VALID)
        .unwrap()
        .open_stream(stream_config(480, 960), receive, transmit)
        .unwrap();
    stream.start().unwrap();
    stream.start().unwrap();
    assert_eq!(stream.statistics().unwrap().callback_count, 3);
    assert_eq!(stream.statistics().unwrap().worker_failure_count, 7);
    assert_eq!(stream.timing().unwrap().sample_rate_hz, 48_000.0);
    stream.stop().unwrap();
    stream.stop().unwrap();
    drop(stream);
    assert_eq!(RECEIVES.load(Ordering::Relaxed), 1);
    assert_eq!(TRANSMITS.load(Ordering::Relaxed), 1);
    assert_eq!(STARTS.load(Ordering::Relaxed), 1);
    assert_eq!(STOPS.load(Ordering::Relaxed), 1);
    assert_eq!(DESTROYS.load(Ordering::Relaxed), 1);
}

#[test]
fn mixer_discovery_and_controls_preserve_semantics() {
    let provider = provider(&VALID).unwrap();
    let paths = provider.cm119_mixer_paths("3-1:1.0").unwrap();
    assert_eq!(paths.receive[0].element, "Mic");
    assert!(paths.receive[0].has_switch);
    assert_eq!(paths.transmit[0].element, "Speaker");
    let mut mixer = provider.open_mixer("3-1:1.0", &paths.receive[0]).unwrap();
    assert_eq!(mixer.normalized().unwrap(), 500);
    mixer.set_normalized(999).unwrap();
    assert_eq!(mixer.normalized().unwrap(), 999);
    mixer.set_enabled(false).unwrap();
    assert!(!mixer.enabled().unwrap());
    mixer.set_hardware_level(500).unwrap();
    assert_eq!(LAST_STEPS.load(Ordering::Relaxed), 1000);
    assert_eq!(mixer.set_normalized(1000), Err(AudioError::InvalidArgument));
    assert_eq!(
        mixer.set_hardware_level(1000),
        Err(AudioError::InvalidArgument)
    );
}

#[test]
fn invalid_policies_strings_and_adapter_results_are_rejected() {
    let provider = provider(&VALID).unwrap();
    let mut invalid = selector();
    invalid.identifier = None;
    assert_eq!(
        provider.select_device(&invalid),
        Err(AudioError::InvalidArgument)
    );
    invalid.policy = SelectionPolicy::Automatic;
    invalid.serial = Some("unexpected".to_owned());
    assert_eq!(
        provider.select_device(&invalid),
        Err(AudioError::InvalidArgument)
    );
    invalid.identifier = Some("unexpected".to_owned());
    invalid.serial = None;
    assert_eq!(
        provider.select_device(&invalid),
        Err(AudioError::InvalidArgument)
    );
    invalid.policy = SelectionPolicy::Exact;
    invalid.identifier = Some("bad\0id".to_owned());
    assert_eq!(
        provider.select_device(&invalid),
        Err(AudioError::InvalidArgument)
    );
    assert_eq!(
        provider.cm119_mixer_paths(""),
        Err(AudioError::InvalidArgument)
    );
    assert_eq!(
        provider.cm119_mixer_paths("bad\0path"),
        Err(AudioError::InvalidArgument)
    );

    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    assert_eq!(
        provider
            .open_stream(stream_config(0, 1), receive_endpoint, transmit_endpoint)
            .err(),
        Some(AudioError::InvalidArgument)
    );
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    assert_eq!(
        provider
            .open_stream(stream_config(1, 0), receive_endpoint, transmit_endpoint)
            .err(),
        Some(AudioError::InvalidArgument)
    );
}

#[test]
fn abi_two_failure_results_and_callback_lifetimes_are_owned() {
    let mut wrong_name = VALID;
    wrong_name.capability_name = c"wrong".as_ptr();
    assert_eq!(
        provider(Box::leak(Box::new(wrong_name))).err(),
        Some(AudioError::IncompatibleAdapter)
    );

    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    assert_eq!(
        provider(&VALID)
            .unwrap()
            .open_stream(stream_config(99, 1), receive_endpoint, transmit_endpoint,)
            .err(),
        Some(AudioError::PortAudio)
    );

    let mut partial = VALID;
    partial.stream_create = Some(stream_create_partial_failure);
    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    assert_eq!(
        provider(Box::leak(Box::new(partial)))
            .unwrap()
            .open_stream(stream_config(1, 1), receive_endpoint, transmit_endpoint,)
            .err(),
        Some(AudioError::PortAudio)
    );

    let mut without_destroy = VALID;
    without_destroy.stream_create = Some(stream_create_partial_without_destroy);
    without_destroy.stream_destroy = None;
    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    assert_eq!(
        AudioProvider {
            descriptor: Box::leak(Box::new(without_destroy)),
        }
        .open_stream(stream_config(1, 1), receive_endpoint, transmit_endpoint,)
        .err(),
        Some(AudioError::PortAudio)
    );
    let leaked = PARTIAL_STREAM.swap(std::ptr::null_mut(), Ordering::Relaxed);
    assert!(!leaked.is_null());
    // SAFETY: The helper transferred this otherwise-unowned allocation.
    drop(unsafe { Box::from_raw(leaked) });

    let (mut receive_context, mut transmit_context) = (0_u8, 0_u8);
    let (receive_endpoint, transmit_endpoint) =
        endpoints(&mut receive_context, &mut transmit_context);
    let mut started = provider(&VALID)
        .unwrap()
        .open_stream(stream_config(1, 1), receive_endpoint, transmit_endpoint)
        .unwrap();
    started.start().unwrap();
    drop(started);

    let handle = Box::into_raw(Box::new(7_u8));
    let mut no_lifecycle = VALID;
    no_lifecycle.stream_stop = None;
    no_lifecycle.stream_destroy = None;
    let stream = AudioStream {
        provider: AudioProvider {
            descriptor: Box::leak(Box::new(no_lifecycle)),
        },
        // SAFETY: `handle` is non-null and remains live until reclaimed below.
        handle: unsafe { NonNull::new_unchecked(handle.cast()) },
        started: true,
        _workers: PhantomData,
    };
    drop(stream);
    // SAFETY: No descriptor callback consumed the deliberately unowned handle.
    drop(unsafe { Box::from_raw(handle) });
}

#[test]
fn adapter_result_mapping_and_messages_are_complete() {
    assert_eq!(
        AudioError::IncompatibleAdapter.to_string(),
        "incompatible PortAudio/ALSA adapter"
    );
    let cases = [
        (
            -1,
            AudioError::InvalidArgument,
            "invalid audio-adapter argument",
        ),
        (-2, AudioError::NoMemory, "audio-adapter allocation failed"),
        (-3, AudioError::PortAudio, "PortAudio operation failed"),
        (-4, AudioError::Alsa, "ALSA mixer operation failed"),
        (
            -5,
            AudioError::Unsupported,
            "unsupported audio device or mixer path",
        ),
        (-6, AudioError::DeviceBusy, "audio device is already in use"),
        (
            1,
            AudioError::AdapterFailure,
            "audio adapter returned an unknown failure",
        ),
    ];
    for (raw, expected, message) in cases {
        assert_eq!(error_from_result(raw), expected);
        assert_eq!(expected.to_string(), message);
    }
    assert_eq!(map_result(-1), Err(AudioError::InvalidArgument));
    assert_eq!(
        validate_returned_abi(ABI_VERSION + 1),
        Err(AudioError::IncompatibleAdapter)
    );
}

#[test]
fn returned_device_validation_rejects_partial_or_mismatched_results() {
    let provider = provider(&VALID).unwrap();
    for identifier in [
        "invalid-structure",
        "invalid-selection-structure",
        "invalid-input",
        "invalid-output",
        "wrong-abi",
        "empty-interface",
        "unterminated-interface",
        "invalid-interface-utf8",
        "invalid-serial-utf8",
    ] {
        let mut request = selector();
        request.identifier = Some(identifier.to_owned());
        assert!(provider.select_device(&request).is_err());
    }
    let mut wrong_serial = selector();
    wrong_serial.serial = Some("WRONG".to_owned());
    assert_eq!(
        provider.select_device(&wrong_serial),
        Err(AudioError::AdapterFailure)
    );
    let automatic = DeviceSelector {
        policy: SelectionPolicy::Automatic,
        identifier: None,
        serial: None,
        input_channels: ChannelCount::Mono,
        output_channels: ChannelCount::Stereo,
    };
    assert!(provider.select_device(&automatic).is_ok());
    let serial_only = DeviceSelector {
        policy: SelectionPolicy::Exact,
        identifier: None,
        serial: Some("ABC".to_owned()),
        input_channels: ChannelCount::Mono,
        output_channels: ChannelCount::Stereo,
    };
    assert!(provider.select_device(&serial_only).is_ok());
}

#[test]
fn mixer_failures_release_partial_handles_and_validate_values() {
    let audio_provider = provider(&VALID).unwrap();
    let paths = audio_provider.cm119_mixer_paths("3-1:1.0").unwrap();
    assert_eq!(
        audio_provider.open_mixer("", &paths.receive[0]).err(),
        Some(AudioError::InvalidArgument)
    );
    assert_eq!(
        audio_provider
            .open_mixer("bad\0path", &paths.receive[0])
            .err(),
        Some(AudioError::InvalidArgument)
    );
    let mut path = paths.receive[0].clone();
    path.element.clear();
    assert_eq!(
        audio_provider.open_mixer("3-1:1.0", &path).err(),
        Some(AudioError::InvalidArgument)
    );
    path.element = "bad\0element".to_owned();
    assert_eq!(
        audio_provider.open_mixer("3-1:1.0", &path).err(),
        Some(AudioError::InvalidArgument)
    );
    path.element = "missing".to_owned();
    assert_eq!(
        audio_provider.open_mixer("3-1:1.0", &path).err(),
        Some(AudioError::Unsupported)
    );
    path.element = "partial".to_owned();
    assert_eq!(
        audio_provider.open_mixer("3-1:1.0", &path).err(),
        Some(AudioError::Alsa)
    );

    let mut invalid_range = VALID;
    invalid_range.mixer_get_range_steps = Some(invalid_mixer_range);
    let mut mixer = provider(Box::leak(Box::new(invalid_range)))
        .unwrap()
        .open_mixer("3-1:1.0", &paths.receive[0])
        .unwrap();
    assert_eq!(mixer.set_hardware_level(1), Err(AudioError::AdapterFailure));

    let mut high_minimum = VALID;
    high_minimum.mixer_get_range_steps = Some(high_minimum_mixer_range);
    let mut mixer = provider(Box::leak(Box::new(high_minimum)))
        .unwrap()
        .open_mixer("3-1:1.0", &paths.receive[0])
        .unwrap();
    assert_eq!(
        mixer.set_hardware_level(500),
        Err(AudioError::AdapterFailure)
    );

    let mut negative_maximum = VALID;
    negative_maximum.mixer_get_range_steps = Some(negative_maximum_mixer_range);
    let mut mixer = provider(Box::leak(Box::new(negative_maximum)))
        .unwrap()
        .open_mixer("3-1:1.0", &paths.receive[0])
        .unwrap();
    assert_eq!(
        mixer.set_hardware_level(500),
        Err(AudioError::AdapterFailure)
    );

    let mut without_destroy = VALID;
    without_destroy.mixer_create_for_usb_interface = Some(mixer_create_partial_without_destroy);
    without_destroy.mixer_destroy = None;
    assert_eq!(
        AudioProvider {
            descriptor: Box::leak(Box::new(without_destroy)),
        }
        .open_mixer("3-1:1.0", &paths.receive[0])
        .err(),
        Some(AudioError::Alsa)
    );
    let leaked = PARTIAL_MIXER.swap(std::ptr::null_mut(), Ordering::Relaxed);
    assert!(!leaked.is_null());
    // SAFETY: The helper transferred this otherwise-unowned allocation.
    drop(unsafe { Box::from_raw(leaked) });

    let mut mixer = provider(&VALID)
        .unwrap()
        .open_mixer("3-1:1.0", &paths.receive[0])
        .unwrap();
    mixer.set_enabled(true).unwrap();
    assert!(mixer.enabled().unwrap());
    // SAFETY: The test exclusively owns this fake u32-backed mixer handle.
    unsafe { mixer_set(mixer.handle.as_ptr(), 1000) };
    assert_eq!(mixer.normalized(), Err(AudioError::AdapterFailure));
    // SAFETY: The same exclusively owned fake handle accepts a test sentinel.
    unsafe { mixer_set(mixer.handle.as_ptr(), 2) };
    assert_eq!(mixer.enabled(), Err(AudioError::AdapterFailure));

    let handle = Box::into_raw(Box::new(500_u32));
    let mut no_destroy = VALID;
    no_destroy.mixer_destroy = None;
    let mixer = Mixer {
        provider: AudioProvider {
            descriptor: Box::leak(Box::new(no_destroy)),
        },
        // SAFETY: `handle` is non-null and remains live until reclaimed below.
        handle: unsafe { NonNull::new_unchecked(handle.cast()) },
    };
    drop(mixer);
    // SAFETY: No descriptor callback consumed the deliberately unowned handle.
    drop(unsafe { Box::from_raw(handle) });
}

#[test]
fn mixer_path_conversion_rejects_malformed_adapter_data() {
    let no_nul = [b'x' as c_char; MIXER_ELEMENT_CAPACITY];
    assert_eq!(fixed_c_string(&no_nul), Err(AudioError::AdapterFailure));
    let mut invalid_utf8 = [0; MIXER_ELEMENT_CAPACITY];
    invalid_utf8[0] = -1;
    assert_eq!(
        fixed_c_string(&invalid_utf8),
        Err(AudioError::AdapterFailure)
    );

    let mut paths = [RawMixerPath::default(); MIXER_PATH_CAPACITY];
    assert_eq!(
        convert_paths(&paths, 3, MixerDirection::Capture, 1),
        Err(AudioError::AdapterFailure)
    );
    copy_c_string(&mut paths[0].element, b"Mic");
    paths[0].capabilities = 1;
    paths[0].channel = MixerChannel::Right as u32;
    assert_eq!(
        convert_paths(&paths, 1, MixerDirection::Capture, 1).unwrap()[0].channel,
        MixerChannel::Right
    );
    paths[0].channel = 2;
    assert!(convert_paths(&paths, 1, MixerDirection::Capture, 1).is_err());
    paths[0].channel = 0;
    paths[0].direction = 2;
    assert!(convert_paths(&paths, 1, MixerDirection::Capture, 1).is_err());
    paths[0].direction = MixerDirection::Playback as u32;
    assert!(convert_paths(&paths, 1, MixerDirection::Capture, 1).is_err());
    paths[0].direction = MixerDirection::Capture as u32;
    paths[0].capabilities = 0;
    assert!(convert_paths(&paths, 1, MixerDirection::Capture, 1).is_err());
    paths[0].element.fill(0);
    paths[0].capabilities = 1;
    assert!(convert_paths(&paths, 1, MixerDirection::Capture, 1).is_err());

    let mut descriptor = VALID;
    descriptor.cm119_mixer_paths_resolve = Some(empty_mixer_paths);
    assert_eq!(
        provider(Box::leak(Box::new(descriptor)))
            .unwrap()
            .cm119_mixer_paths("3-1:1.0"),
        Err(AudioError::AdapterFailure)
    );
    assert_eq!(
        provider(Box::leak(Box::new(descriptor)))
            .unwrap()
            .cm119_mixer_paths("small"),
        Err(AudioError::AdapterFailure)
    );
    assert_eq!(
        provider(Box::leak(Box::new(descriptor)))
            .unwrap()
            .cm119_mixer_paths("no-transmit"),
        Err(AudioError::AdapterFailure)
    );
}
