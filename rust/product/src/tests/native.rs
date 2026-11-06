//! Native entry points use only released providers, never an Asterisk host.
use super::*;
use provider_support as fixture;

#[path = "native_maintenance.rs"]
mod maintenance_contract;

#[test]
fn descriptor_exposes_owned_native_configuration_resolution() {
    let prefix = std::mem::offset_of!(UrpAstDescriptor, link_process_f32)
        + std::mem::size_of::<Option<unsafe extern "C" fn()>>();
    let minimum = prefix + 3 * std::mem::size_of::<Option<unsafe extern "C" fn()>>();
    assert!(
        (DESCRIPTOR.struct_size as usize) >= minimum,
        "the shared native resolver, visitor and destroy operations are missing"
    );
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawReceiveConfig {
    noise_filter_profile: u32,
    squelch_open_level: u32,
    squelch_hysteresis: u32,
    ctcss_decoder_gain: f32,
    vox_threshold: i32,
    vox_hang_ms: i32,
    ctcss_enabled: u32,
    ctcss_tone_mask: u64,
    ctcss_relax: u32,
    dcs_enabled: u32,
    dcs_code: i32,
    dcs_inverted: u32,
    cpu_saver_enabled: u32,
    native_squelch_delay_frames: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawQualificationConfig {
    carrier_source: u32,
    subaudible_source: u32,
    subaudible_override: u32,
    advanced_transport: u32,
    radio_duplex: u32,
    rx_on_delay_blocks: u32,
    tx_off_delay_blocks: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawTransmitConfig {
    ctcss_transmit_enabled: u32,
    default_ctcss_frequency_tenths_hz: i32,
    mapped_ctcss_frequency_tenths_hz: [i32; 38],
    tone_off_mode: u32,
    ctcss_turnoff_duration_ms: i32,
    ctcss_turnoff_phase_shift_degrees: f64,
    ctcss_turnoff_tail_tone_hz: f64,
    dcs_transmit_enabled: u32,
    dcs_turnoff_enabled: u32,
    dcs_turnoff_duration_ms: i32,
    receiver_blanking_ms: i32,
    tx_settle_time_ms: i32,
    cpu_saver_enabled: u32,
    dcs_code: i32,
    dcs_inverted: u32,
    dcs_peak: f32,
    ctcss_peak: f32,
    output_a_route: u32,
    output_b_route: u32,
    output_a_tone_gain: f32,
    output_a_tone_bias: f32,
    output_b_tone_gain: f32,
    output_b_tone_bias: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawSessionConfig {
    struct_size: u32,
    abi_version: u32,
    generation_id: u64,
    native_sample_rate_hz: u32,
    interleaved_channels: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
    publication_interval_ms: u32,
    receive_channel: u32,
    receive_input_gain: f32,
    receive: RawReceiveConfig,
    qualification: RawQualificationConfig,
    transmit: RawTransmitConfig,
}

fn radio() -> RawSessionConfig {
    // SAFETY: the fixture consists solely of integers, floats and numeric arrays.
    let mut config: RawSessionConfig = unsafe { std::mem::zeroed() };
    config.struct_size = size_of::<RawSessionConfig>() as u32;
    config.abi_version = 4;
    config.generation_id = 1;
    config.native_sample_rate_hz = 48_000;
    config.interleaved_channels = 2;
    config.maximum_receive_frame_count = 960;
    config.maximum_transmit_frame_count = 960;
    config.publication_interval_ms = 50;
    config.receive_input_gain = 1.0;
    config.receive.ctcss_decoder_gain = 1.0;
    config.receive.vox_threshold = 1000;
    config.receive.vox_hang_ms = 500;
    config.qualification.advanced_transport = 1;
    config.qualification.radio_duplex = 1;
    config.transmit.output_a_route = 1;
    config.transmit.output_a_tone_gain = 1.0;
    config.transmit.output_b_tone_gain = 1.0;
    config
}

fn request(radio: &RawSessionConfig) -> UrpNativeStationConfig {
    UrpNativeStationConfig {
        struct_size: size_of::<UrpNativeStationConfig>() as u32,
        abi_version: URP_NATIVE_CONFIG_ABI_VERSION,
        radio: ptr::from_ref(radio).cast(),
        device_selection: 1,
        device_identifier: ptr::null(),
        device_identifier_length: 0,
        usb_serial: ptr::null(),
        usb_serial_length: 0,
        input_device_channels: 2,
        output_device_channels: 2,
        input_extra_buffer_ms: 0,
        output_extra_buffer_ms: 0,
        cm119_profile: 0,
        ptt_inverted: 0,
        gpio_output_enable_mask: 0,
        gpio_output_initial_mask: 0,
        clip_led_mask: 0,
        receive_graph: b"anull".as_ptr(),
        receive_graph_length: 5,
        transmit_graph: b"anull".as_ptr(),
        transmit_graph_length: 5,
        receive_deemphasis: 0,
        receive_output_gain_db: -6,
        hardware_gain_mask: 0,
        hardware_input_gain_db: 0.0,
        hardware_output_a_gain_db: 0.0,
        hardware_output_b_gain_db: 0.0,
        processing_configuration: ptr::null(),
        processing_configuration_length: 0,
        processing_node: ptr::null(),
        processing_node_length: 0,
        transmit_preemphasis: 0,
        transmit_preemphasis_corner_hz: 300.0,
        receive_deemphasis_corner_hz: 300.0,
    }
}

fn create(config: &UrpNativeStationConfig) -> (c_int, *mut c_void) {
    create_pointer(config)
}

fn create_pointer(config: *const UrpNativeStationConfig) -> (c_int, *mut c_void) {
    let mut providers = fixture::manifest();
    // Native program PCM is pulled directly from the callback: no ASL rate/queue path.
    providers.ring = ptr::null();
    providers.samplerate = ptr::null();
    providers.rnnoise = ptr::null();
    create_with_providers(config, &providers)
}

fn create_with_providers(
    config: *const UrpNativeStationConfig,
    providers: &UrpAstProviderManifest,
) -> (c_int, *mut c_void) {
    let callbacks = direct_callbacks();
    let args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config,
        providers,
        callbacks: &callbacks,
        generation_id: 8,
        maximum_frames: 960,
    };
    let mut handle = ptr::dangling_mut();
    // SAFETY: all request spans live through creation; callbacks use no borrowed state.
    let status = unsafe { product_descriptor().native_create.unwrap()(&args, &mut handle) };
    (status, handle)
}

#[test]
fn native_create_accepts_the_current_processing_configuration_abi() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let mut config = request(&raw);
    config.abi_version = 3;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    // SAFETY: successful creation transfers the uniquely owned native handle.
    unsafe { product_descriptor().native_destroy.unwrap()(handle) };
}

#[test]
fn remote_native_requires_metadata_before_opening_any_device() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let mut config = request(&raw);
    let source = b"[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n";
    config.processing_configuration = source.as_ptr();
    config.processing_configuration_length = source.len() as u32;
    config.processing_node = b"524950".as_ptr();
    config.processing_node_length = 6;
    let providers = fixture::manifest();
    let mut callbacks = direct_callbacks();
    callbacks.metadata = None;
    let args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &config,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 8,
        maximum_frames: 960,
    };
    let mut handle = ptr::null_mut();
    // SAFETY: complete borrowed records and byte spans live through this call.
    let result = unsafe { product_descriptor().native_create.unwrap()(&args, &mut handle) };
    assert_eq!(result, URP_AST_INCOMPATIBLE_ABI);
    assert!(handle.is_null());
    let invalid = std::str::from_utf8(source)
        .unwrap()
        .replace("RIG_MODEL_DUMMY", "RIG_MODEL_NOT_REAL");
    let mut bad = config;
    bad.processing_configuration = invalid.as_ptr();
    bad.processing_configuration_length = invalid.len() as u32;
    callbacks.metadata = Some(remote_metadata);
    let mut next = args;
    next.config = &bad;
    assert_eq!(
        // SAFETY: the malformed model name is UTF-8, and all complete spans stay borrowed.
        unsafe { product_descriptor().native_create.unwrap()(&next, &mut handle) },
        URP_AST_SETUP_FAILED
    );
    assert!(handle.is_null());
}

#[test]
fn native_service_advances_only_the_existing_control_owner() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let (status, handle) = create(&request(&raw));
    assert_eq!(status, URP_AST_OK);
    let api = product_descriptor();
    // SAFETY: creation transfers this sole live handle; the null case retains no owner.
    unsafe {
        assert_eq!(api.native_service.unwrap()(handle), URP_AST_OK);
        assert_eq!(api.native_activation_status.unwrap()(handle), URP_AST_OK);
        let operation = usbradioplus_core::remote_operation::RemoteOperation::new(
            usbradioplus_core::remote_operation::RemoteOperationKind::Status,
        );
        let mut ticket = ptr::dangling_mut();
        assert_eq!(
            api.native_remote_submit.unwrap()(handle, &operation, 0, &mut ticket),
            URP_AST_NOT_READY
        );
        assert!(ticket.is_null());
        assert_eq!(api.native_quiesce.unwrap()(handle), URP_AST_OK);
        assert_eq!(api.native_quiesce.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_quiesce.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_service.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        api.native_destroy.unwrap()(handle);
    }
}

unsafe extern "C" fn remote_metadata(_: *mut c_void, _: u32, active: *mut u32) -> c_int {
    // SAFETY: the callback lends one writable activity result through return.
    unsafe { active.write(0) };
    0
}

unsafe extern "C" fn remote_text(context: *mut c_void, data: *const u8, length: u32) {
    // SAFETY: the test lends its String and the synchronous provider lends UTF-8.
    let (out, bytes) = unsafe {
        (
            &mut *context.cast::<String>(),
            std::slice::from_raw_parts(data, length as usize),
        )
    };
    out.push_str(std::str::from_utf8(bytes).unwrap());
}

#[test]
fn native_remote_activation_submission_and_generation_guards_share_the_owner() {
    use std::time::{Duration, Instant};
    use usbradioplus_core::remote_operation::{RemoteOperation as O, RemoteOperationKind as K};
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::periodic_audio(true);
    let raw = radio();
    let mut config = request(&raw);
    let source = "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nhamlib_option_ptt_type=RIG\nptt_source=usb\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\npower_w=5\nmode=FM\n";
    config.processing_configuration = source.as_ptr();
    config.processing_configuration_length = source.len() as u32;
    config.processing_node = b"524950".as_ptr();
    config.processing_node_length = 6;
    let providers = fixture::manifest();
    let mut callbacks = direct_callbacks();
    callbacks.metadata = Some(remote_metadata);
    let mut args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &config,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 8,
        maximum_frames: 960,
    };
    let api = product_descriptor();
    let mut handle = ptr::null_mut();
    // SAFETY: all borrowed config spans and callbacks remain live until joined destruction.
    unsafe {
        assert_eq!(api.native_create.unwrap()(&args, &mut handle), URP_AST_OK);
        assert_eq!(
            api.native_activation_status.unwrap()(handle),
            URP_AST_RELOAD_PENDING
        );
        let mut output = ptr::dangling_mut();
        let mut operation = O::new(K::Status);
        assert_eq!(
            api.native_remote_submit.unwrap()(handle, &operation, 4, &mut output),
            URP_AST_INVALID_ARGUMENT
        );
        assert!(output.is_null());
        operation.operation = u32::MAX;
        assert_eq!(
            api.native_remote_submit.unwrap()(handle, &operation, 0, &mut output),
            URP_AST_NOT_READY
        );
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        let deadline = Instant::now() + Duration::from_secs(5);
        while api.native_activation_status.unwrap()(handle) == URP_AST_RELOAD_PENDING {
            assert_eq!(api.native_service.unwrap()(handle), URP_AST_OK);
            assert!(
                Instant::now() < deadline,
                "native remote activation must finish"
            );
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(api.native_activation_status.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_remote_submit.unwrap()(handle, &operation, 0, &mut output),
            URP_AST_INVALID_ARGUMENT
        );
        // Hold one actual auxiliary command at its source-unkey gate. This
        // bounds the queue deterministically without stalling a device thread.
        let mut auxiliary = O::new(K::SetSetting);
        auxiliary.selector = 100;
        auxiliary.magnitude = 500_000;
        auxiliary.decimal_places = 6;
        assert_eq!(
            api.native_remote_submit.unwrap()(handle, &auxiliary, 2, &mut output),
            URP_AST_OK
        );
        let mut tickets = vec![output];
        operation = O::new(K::Status);
        loop {
            let result = api.native_remote_submit.unwrap()(handle, &operation, 0, &mut output);
            if result == URP_AST_CHANNEL_BUSY {
                assert!(output.is_null());
                break;
            }
            assert_eq!(result, URP_AST_OK);
            tickets.push(output);
            assert!(tickets.len() < 256, "remote queue must be bounded");
        }
        for ticket in tickets {
            api.remote_destroy.unwrap()(ticket);
        }
        loop {
            let result = api.native_remote_submit.unwrap()(handle, &operation, 1, &mut output);
            if result != URP_AST_CHANNEL_BUSY {
                assert_eq!(result, URP_AST_OK);
                break;
            }
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(2));
        }
        let mut json = String::new();
        loop {
            assert_eq!(api.native_service.unwrap()(handle), URP_AST_OK);
            let result = api.remote_take.unwrap()(
                output,
                Some(remote_text),
                ptr::from_mut(&mut json).cast(),
            );
            if result != URP_AST_RELOAD_PENDING {
                assert_eq!(result, URP_AST_OK);
                break;
            }
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(json.contains("status"));
        api.remote_destroy.unwrap()(output);
        let changed = source.replace("146520000", "146940000");
        let mut next = config;
        next.processing_configuration = changed.as_ptr();
        next.processing_configuration_length = changed.len() as u32;
        args.config = &next;
        args.generation_id = 9;
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut output),
            URP_AST_INVALID_CONFIGURATION
        );
        assert!(output.is_null());
        while api.native_quiesce.unwrap()(handle) == URP_AST_RELOAD_PENDING {
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(2));
        }
        api.native_destroy.unwrap()(handle);
    }
    fixture::periodic_audio(false);
}

#[test]
fn native_configuration_visit_preserves_shared_gpio_identity_fallback_through_create() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let source =
        b"[1000]\nhardware_profile=site\n[hardware site]\nhardware_gpio_usb_port_path=3-1:1.0\n";
    let args = UrpNativeProcessingArgs {
        struct_size: size_of::<UrpNativeProcessingArgs>() as u32,
        abi_version: 1,
        configuration: source.as_ptr(),
        configuration_length: source.len() as u32,
        node: b"1000".as_ptr(),
        node_length: 4,
    };
    struct Result {
        status: c_int,
        handle: *mut c_void,
        identifier: String,
    }
    unsafe extern "C" fn visit(
        context: *mut c_void,
        request: *const UrpNativeStationConfig,
    ) -> c_int {
        // SAFETY: the caller retains this context and the SDK borrows a complete request for this call.
        let (result, request) = unsafe { (&mut *context.cast::<Result>(), &*request) };
        // SAFETY: the SDK borrows this identifier through the synchronous visitor.
        result.identifier = unsafe {
            crate::native::text(request.device_identifier, request.device_identifier_length)
        }
        .unwrap_or_default();
        (result.status, result.handle) = create(request);
        0
    }
    let api = product_descriptor();
    let mut owner = ptr::null_mut();
    let mut result = Result {
        status: -99,
        handle: ptr::null_mut(),
        identifier: String::new(),
    };
    // SAFETY: all source/owner/context storage is live; each successful handle is consumed once.
    unsafe {
        assert_eq!(
            api.native_configuration_resolve.unwrap()(&args, None, ptr::null_mut(), &mut owner),
            URP_AST_OK
        );
        assert_eq!(
            api.native_configuration_visit.unwrap()(
                owner,
                Some(visit),
                ptr::from_mut(&mut result).cast()
            ),
            URP_AST_OK
        );
        api.native_configuration_destroy.unwrap()(owner);
        if !result.handle.is_null() {
            api.native_destroy.unwrap()(result.handle);
        }
    }
    assert_eq!(
        result.status, URP_AST_OK,
        "the shared audio selector must survive the inspection request"
    );
    assert_eq!(result.identifier, "3-1:1.0");
    assert_eq!(fixture::audio_streams(), 0);
}

#[test]
fn native_physical_commands_snapshot_and_manual_cleanup_share_the_owner() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let raw = radio();
    let (status, handle) = create(&request(&raw));
    assert_eq!(status, URP_AST_OK);
    // SAFETY: this initialized command contains only scalar ABI fields.
    let mut command: UrpAstChannelCommand = unsafe { std::mem::zeroed() };
    command.struct_size = size_of::<UrpAstChannelCommand>() as u32;
    command.abi_version = ABI_VERSION;
    command.command = URP_AST_COMMAND_GET_MIXER;
    command.target = URP_AST_MIXER_RECEIVE;
    // SAFETY: the unique stopped native handle and command live for all calls.
    unsafe {
        assert_eq!(
            crate::native::command(handle, &mut command),
            URP_AST_NOT_READY
        );
        assert_eq!(crate::native::start(handle), URP_AST_OK);
        assert_eq!(crate::native::command(handle, &mut command), URP_AST_OK);
        assert_eq!(command.value, 750);
        command.command = URP_AST_COMMAND_SET_MIXER;
        command.value = 222;
        assert_eq!(crate::native::command(handle, &mut command), URP_AST_OK);
        let physical = crate::native::snapshot(handle).unwrap();
        assert_eq!(physical.running, 1);
        assert_eq!(physical.receive_mixer_level, 222);
        unsafe extern "C" fn append(context: *mut c_void, text: *const u8, length: u32) {
            // SAFETY: the synchronous call retains this owned output and span.
            let (output, text) = unsafe {
                (
                    &mut *context.cast::<String>(),
                    std::slice::from_raw_parts(text, length as usize),
                )
            };
            output.push_str(std::str::from_utf8(text).unwrap());
        }
        let mut processing = String::new();
        assert_eq!(
            product_descriptor().native_processing_status_text.unwrap()(
                handle,
                Some(append),
                ptr::from_mut(&mut processing).cast()
            ),
            URP_AST_OK
        );
        assert!(processing.contains("radio_receive_calls=1"));
        assert!(processing.contains("radio_transmit_calls=1"));
        assert!(processing.contains("radio_receive_failures=0"));
        assert!(processing.contains("transmit_loudness_meter_enabled=0\n"));
        assert!(processing.contains("transmit_integrated_lufs=unavailable\n"));
        assert!(processing.contains("transmit_true_peak_dbtp=unavailable\n"));
        assert!(processing.contains("transmit_loudness_sample_count=0\n"));
        assert_eq!(crate::native::set_transmit(handle, 1, 1_000), URP_AST_OK);
        command.command = URP_AST_COMMAND_SET_TEST_TONE;
        command.value = 1;
        assert_eq!(crate::native::command(handle, &mut command), URP_AST_OK);
        assert_eq!(crate::native::clear_manual(handle), URP_AST_OK);
        assert_eq!(crate::native::stop(handle), URP_AST_OK);
        assert_eq!(
            crate::native::set_transmit(handle, 1, 1_000),
            URP_AST_NOT_READY
        );
        crate::native::destroy(handle);
    }
}

#[test]
fn native_full_source_enables_shared_eeprom_preflight_before_preparation() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let raw = radio();
    let source = b"[524950]\n[radio]\nhardware_eeprom_enabled=yes\nparallel_port_device=/dev/parport0\nparallel_pin_2_assignment=out0\nparallel_pin_10_assignment=in\n";
    let mut config = request(&raw);
    config.processing_configuration = source.as_ptr();
    config.processing_configuration_length = source.len() as u32;
    config.processing_node = b"524950".as_ptr();
    config.processing_node_length = 6;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    assert_eq!(fixture::mixer_levels(), [750, 795, 245]);
    // SAFETY: native creation transferred this single handle and fixture providers.
    unsafe {
        assert_eq!(crate::native::start(handle), URP_AST_OK);
        assert_eq!(fixture::mixer_levels(), [321, 411, 512]);
        let mut parallel = UrpAstSiteSnapshot {
            struct_size: size_of::<UrpAstSiteSnapshot>() as u32,
            abi_version: ABI_VERSION,
            ..UrpAstSiteSnapshot::default()
        };
        assert_eq!(
            product_descriptor().native_site_snapshot.unwrap()(handle, 1, &mut parallel),
            URP_AST_OK
        );
        assert_eq!(parallel.output_mask, 1);
        // A physical read reaches the same live owner used by preflight.
        let mut command: UrpAstChannelCommand = std::mem::zeroed();
        command.struct_size = size_of::<UrpAstChannelCommand>() as u32;
        command.abi_version = ABI_VERSION;
        command.command = URP_AST_COMMAND_READ_EEPROM;
        assert_eq!(crate::native::command(handle, &mut command), URP_AST_OK);
        assert_eq!(command.eeprom_words[60], 654);
        crate::native::destroy(handle);
    }
}

#[test]
fn native_prepared_updates_commit_and_rollback_without_reopening_audio() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::exclusive_audio(true);
    fixture::periodic_audio(true);
    fixture::restored_mixer_levels([750, 795, 245]);
    let raw = radio();
    let config = request(&raw);
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    let providers = fixture::manifest();
    let callbacks = direct_callbacks();
    let mut next = config;
    next.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_INPUT;
    next.hardware_input_gain_db = 3.0;
    let mut args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &next,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 9,
        maximum_frames: 960,
    };
    // SAFETY: all request spans, providers and native handles remain live and
    // control operations are serialized while the independent callbacks run.
    unsafe {
        assert_eq!(crate::native::start(handle), URP_AST_OK);
        let base = next;
        type StructuralCase = (&'static str, fn(&mut UrpNativeStationConfig));
        let structural: &[StructuralCase] = &[
            ("audio selector", |c| {
                c.device_selection = 0;
                c.device_identifier = b"another-device".as_ptr();
                c.device_identifier_length = 14;
            }),
            ("HID serial", |c| {
                c.usb_serial = b"other".as_ptr();
                c.usb_serial_length = 5;
            }),
            ("channel layout", |c| c.input_device_channels = 1),
            ("audio buffering", |c| c.input_extra_buffer_ms = 1),
            ("CM119 wiring", |c| c.cm119_profile = 1),
        ];
        for (name, change) in structural {
            next = base;
            change(&mut next);
            args.config = &next;
            let mut update = 1_usize as *mut c_void;
            assert_eq!(
                product_descriptor().native_prepare_update.unwrap()(handle, &args, &mut update),
                URP_AST_INVALID_CONFIGURATION,
                "{name} requires controlled device handoff"
            );
            assert!(update.is_null(), "{name} transferred no update owner");
            assert_eq!(fixture::audio_lifecycle(), (1, 0));
        }
        next = base;
        for (commit, gain) in [(true, 3.0), (false, 6.0)] {
            next.hardware_input_gain_db = gain;
            args.config = &next;
            let mut update = ptr::null_mut();
            assert_eq!(
                crate::native::prepare_update(handle, &args, &mut update),
                URP_AST_OK
            );
            assert!(!update.is_null());
            assert_eq!(crate::native::begin_update(handle, update), URP_AST_OK);
            crate::native::destroy_update(update);
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
            loop {
                let result = crate::native::accept_update(handle);
                if result == URP_AST_OK {
                    break;
                }
                assert_eq!(result, URP_AST_RELOAD_PENDING);
                assert!(std::time::Instant::now() < deadline);
                std::thread::yield_now();
            }
            loop {
                let result = crate::native::finish_update(handle, u32::from(commit));
                if result == URP_AST_OK {
                    break;
                }
                assert_eq!(result, URP_AST_RELOAD_PENDING);
                assert!(std::time::Instant::now() < deadline);
                std::thread::yield_now();
            }
            assert_eq!(fixture::audio_lifecycle(), (1, 0));
        }
        assert_eq!(fixture::mixer_levels(), [706, 795, 245]);
        crate::native::destroy(handle);
    }
    fixture::periodic_audio(false);
    fixture::exclusive_audio(false);
}

#[test]
fn native_stop_rolls_back_an_unacknowledged_update_before_restart() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let raw = radio();
    let config = request(&raw);
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    let providers = fixture::manifest();
    let callbacks = direct_callbacks();
    let mut next = config;
    next.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_INPUT;
    next.hardware_input_gain_db = 3.0;
    let args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &next,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 9,
        maximum_frames: 960,
    };
    // SAFETY: fixture callbacks run synchronously at start and the stopped
    // runtime retains both callback contexts while rollback adopts its ports.
    unsafe {
        assert_eq!(crate::native::start(handle), URP_AST_OK);
        let mut update = ptr::null_mut();
        assert_eq!(
            crate::native::prepare_update(handle, &args, &mut update),
            URP_AST_OK
        );
        assert_eq!(crate::native::begin_update(handle, update), URP_AST_OK);
        crate::native::destroy_update(update);
        assert_eq!(fixture::mixer_levels()[0], 706);
        assert_eq!(crate::native::stop(handle), URP_AST_OK);
        assert_eq!(fixture::mixer_levels()[0], 750);
        assert_eq!(crate::native::start(handle), URP_AST_OK);
        let mut replacement = ptr::null_mut();
        assert_eq!(
            crate::native::prepare_update(handle, &args, &mut replacement),
            URP_AST_OK
        );
        crate::native::destroy_update(replacement);
        crate::native::destroy(handle);
    }
}

#[test]
fn native_update_admission_preserves_running_state_for_bad_busy_and_stale_requests() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::periodic_audio(false);
    fixture::exclusive_audio(true);
    fixture::restored_mixer_levels([750, 795, 245]);
    let raw = radio();
    let config = request(&raw);
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    let providers = fixture::manifest();
    let callbacks = direct_callbacks();
    let mut next = config;
    let mut args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &next,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 9,
        maximum_frames: 960,
    };
    let api = product_descriptor();
    // SAFETY: this test retains all request spans and serializes the unique native owner/tokens.
    unsafe {
        let mut output = ptr::dangling_mut();
        assert_eq!(
            api.native_prepare_update.unwrap()(ptr::null_mut(), &args, &mut output),
            URP_AST_INVALID_ARGUMENT
        );
        assert!(output.is_null());
        assert_eq!(
            api.native_begin_update.unwrap()(handle, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_begin_update.unwrap()(ptr::null_mut(), ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_accept_update.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_finish_update.unwrap()(ptr::null_mut(), 0),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_clear_manual.unwrap()(ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_set_transmit.unwrap()(ptr::null_mut(), 0, 0),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut output),
            URP_AST_NOT_READY
        );
        assert!(output.is_null());
        assert_eq!(api.native_finish_update.unwrap()(handle, 0), URP_AST_OK);
        api.native_destroy_update.unwrap()(ptr::null_mut());
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(api.native_set_transmit.unwrap()(handle, 0, 0), URP_AST_OK);
        for (keyed, tone) in [(2, 0), (1, u32::MAX), (1, 450)] {
            assert_eq!(
                api.native_set_transmit.unwrap()(handle, keyed, tone),
                URP_AST_INVALID_ARGUMENT
            );
        }
        for (generation, maximum) in [(0, 960), (9, 959)] {
            args.generation_id = generation;
            args.maximum_frames = maximum;
            output = ptr::dangling_mut();
            assert_eq!(
                api.native_prepare_update.unwrap()(handle, &args, &mut output),
                URP_AST_INVALID_ARGUMENT
            );
            assert!(output.is_null());
        }
        args.generation_id = 9;
        args.maximum_frames = 960;
        let eeprom = b"[1000]\n[radio]\nhardware_eeprom_enabled=yes\n";
        next.processing_configuration = eeprom.as_ptr();
        next.processing_configuration_length = eeprom.len() as u32;
        next.processing_node = b"1000".as_ptr();
        next.processing_node_length = 4;
        args.config = &next;
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut output),
            URP_AST_INVALID_CONFIGURATION
        );
        assert!(output.is_null());
        next = config;
        next.receive_graph = b"volume=0.5".as_ptr();
        next.receive_graph_length = 10;
        args.config = &next;
        fixture::set_failure(fixture::FAIL_GRAPH);
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut output),
            URP_AST_SETUP_FAILED
        );
        assert!(output.is_null());
        fixture::clear_failure();
        next = config;
        next.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_INPUT;
        next.hardware_input_gain_db = 3.0;
        args.config = &next;
        let mut first = ptr::null_mut();
        let mut stale = ptr::null_mut();
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut first),
            URP_AST_OK
        );
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut stale),
            URP_AST_OK
        );
        assert_eq!(api.native_begin_update.unwrap()(handle, first), URP_AST_OK);
        assert_eq!(
            api.native_prepare_update.unwrap()(handle, &args, &mut output),
            URP_AST_CHANNEL_BUSY
        );
        assert!(output.is_null());
        assert_eq!(
            api.native_begin_update.unwrap()(handle, stale),
            URP_AST_CHANNEL_BUSY
        );
        assert_eq!(
            api.native_accept_update.unwrap()(handle),
            URP_AST_RELOAD_PENDING
        );
        assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_begin_update.unwrap()(handle, stale),
            URP_AST_NOT_READY
        );
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_begin_update.unwrap()(handle, stale),
            URP_AST_CHANNEL_BUSY
        );
        assert_eq!(
            api.native_begin_update.unwrap()(handle, first),
            URP_AST_INVALID_ARGUMENT
        );
        api.native_destroy_update.unwrap()(first);
        api.native_destroy_update.unwrap()(stale);
        assert_eq!(fixture::audio_lifecycle(), (1, 0));
        assert_eq!(fixture::mixer_levels(), [750, 795, 245]);
        api.native_destroy.unwrap()(handle);
    }
    fixture::exclusive_audio(false);
}

fn wait_for_native_update(mut operation: impl FnMut() -> c_int) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    loop {
        let result = operation();
        if result == URP_AST_OK {
            return;
        }
        assert_eq!(result, URP_AST_RELOAD_PENDING);
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
}

#[test]
fn native_updates_retain_eeprom_tuning_when_raw_processing_changes() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let document = usbradioplus_core::ConfigDocument::new("[native]\n");
    let mut channel =
        usbradioplus_core::ResolvedNativeConfiguration::from_document(&document, "test", "native")
            .unwrap()
            .into_config();
    channel.station.receive.squelch_level = 654;
    let matching_squelch = usbradioplus_station::native_radio_session_config(&channel, 8, 960)
        .unwrap()
        .receive
        .squelch_open_level;
    fixture::periodic_audio(true);
    for squelch in [0, matching_squelch] {
        fixture::exclusive_audio(true);
        fixture::restored_mixer_levels([750, 795, 245]);
        let mut raw = radio();
        raw.receive.squelch_open_level = squelch;
        let mut config = request(&raw);
        let source = b"[1000]\n[radio]\nhardware_eeprom_enabled=yes\n";
        config.processing_configuration = source.as_ptr();
        config.processing_configuration_length = source.len() as u32;
        config.processing_node = b"1000".as_ptr();
        config.processing_node_length = 4;
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_OK);
        let providers = fixture::manifest();
        let callbacks = direct_callbacks();
        let mut next = config;
        next.receive_graph = b"volume=0.5".as_ptr();
        next.receive_graph_length = 10;
        let args = UrpNativeCreateArgs {
            struct_size: size_of::<UrpNativeCreateArgs>() as u32,
            abi_version: 1,
            config: &next,
            providers: &providers,
            callbacks: &callbacks,
            generation_id: 9,
            maximum_frames: 960,
        };
        let api = product_descriptor();
        // SAFETY: the serialized owner retains request/providers while its independent callbacks run.
        unsafe {
            assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
            let before = fixture::mixer_levels();
            let mut update = ptr::null_mut();
            assert_eq!(
                api.native_prepare_update.unwrap()(handle, &args, &mut update),
                URP_AST_OK
            );
            assert_eq!(api.native_begin_update.unwrap()(handle, update), URP_AST_OK);
            api.native_destroy_update.unwrap()(update);
            wait_for_native_update(|| api.native_accept_update.unwrap()(handle));
            wait_for_native_update(|| api.native_finish_update.unwrap()(handle, 1));
            assert_eq!(fixture::mixer_levels(), before);
            assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
            assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
            assert_eq!(
                fixture::mixer_levels(),
                before,
                "committed EEPROM intentions survive restart"
            );
            assert_eq!(fixture::audio_lifecycle(), (1, 0));
            api.native_destroy.unwrap()(handle);
        }
        fixture::exclusive_audio(false);
    }
    fixture::periodic_audio(false);
}

#[test]
fn native_create_uses_shared_profiles_and_requires_enabled_noise_reduction() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let text = b"[524950]\n[local]\ninput_gain_db=-3\noutput_gain_db=-2\nrnnoise_enabled=yes\nlimiter_enabled=invalid\n[voice_telemetry]\ninput_gain_db=0\noutput_gain_db=-1\n";
    let mut config = request(&raw);
    config.processing_configuration = text.as_ptr();
    config.processing_configuration_length = text.len() as u32;
    config.processing_node = b"524950".as_ptr();
    config.processing_node_length = 6;
    let (status, handle) = create(&config);
    assert_eq!(
        status, URP_AST_SETUP_FAILED,
        "enabled RNNoise cannot be bypassed"
    );
    assert!(handle.is_null());
    let mut providers = fixture::manifest();
    let (status, handle) = create_with_providers(&config, &providers);
    assert_eq!(status, URP_AST_OK);
    // SAFETY: native creation returned one exclusively owned, stopped station.
    unsafe { product_descriptor().native_destroy.unwrap()(handle) };
    providers.rnnoise = providers.ffmpeg;
    let (status, handle) = create_with_providers(&config, &providers);
    assert_eq!(status, URP_AST_INCOMPATIBLE_ABI);
    assert!(handle.is_null());
    let invalid =
        b"[524950]\n[link]\nlimiter_low_crossover_hz=2000\nlimiter_high_crossover_hz=100\n";
    config.processing_configuration = invalid.as_ptr();
    config.processing_configuration_length = invalid.len() as u32;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_INVALID_CONFIGURATION);
    assert!(handle.is_null());
}

#[test]
fn native_start_without_configured_gains_preserves_mixer_controls() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    fixture::set_failure(fixture::FAIL_MIXER_PATHS);
    let raw = radio();
    let (status, handle) = create(&request(&raw));
    assert_eq!(status, URP_AST_OK);
    let api = product_descriptor();
    // SAFETY: creation transferred one stopped station with live provider fixtures.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(fixture::mixer_levels(), [750, 795, 245]);
        assert_eq!(fixture::audio_start_mixer_levels(), [750, 795, 245]);
        assert_eq!(fixture::mixer_other_writes(), 0);
        api.native_destroy.unwrap()(handle);
    }
    fixture::clear_failure();
}

#[test]
fn native_start_reapplies_only_configured_gains_before_audio_callbacks() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let api = product_descriptor();
    for (mask, expected) in [
        (7, [500, 999, 706]),
        (URP_NATIVE_HARDWARE_GAIN_INPUT, [500, 795, 245]),
        (URP_NATIVE_HARDWARE_GAIN_OUTPUT_A, [750, 999, 245]),
        (URP_NATIVE_HARDWARE_GAIN_OUTPUT_B, [750, 795, 706]),
    ] {
        fixture::restored_mixer_levels([750, 795, 245]);
        let mut config = request(&raw);
        config.hardware_gain_mask = mask;
        config.hardware_output_a_gain_db = 6.012;
        config.hardware_output_b_gain_db = 3.0;
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_OK);
        assert_eq!(fixture::mixer_levels(), [750, 795, 245]);
        // SAFETY: successful creation returns one stopped station with live fixtures.
        unsafe {
            for _ in 0..2 {
                assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
                assert_eq!(fixture::mixer_levels(), expected);
                assert_eq!(fixture::audio_start_mixer_levels(), expected);
                assert_eq!(fixture::mixer_other_writes(), 0);
                fixture::set_failure(fixture::FAIL_MIXER);
                assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
                fixture::clear_failure();
                assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
                fixture::restored_mixer_levels([750, 795, 245]);
            }
            api.native_destroy.unwrap()(handle);
        }
    }
}

#[test]
fn native_mixer_failures_leave_audio_stopped_and_allow_start_retry() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    let raw = radio();
    let api = product_descriptor();
    for (failure, mask, expected) in [
        (
            fixture::FAIL_MIXER_PATHS,
            URP_NATIVE_HARDWARE_GAIN_OUTPUT_B,
            [750, 795, 706],
        ),
        (
            fixture::FAIL_MIXER_OPEN,
            URP_NATIVE_HARDWARE_GAIN_OUTPUT_B,
            [750, 795, 706],
        ),
        (
            fixture::FAIL_MIXER,
            URP_NATIVE_HARDWARE_GAIN_OUTPUT_B,
            [750, 795, 706],
        ),
        (
            fixture::FAIL_MIXER_OUTPUT_B_PATH,
            URP_NATIVE_HARDWARE_GAIN_OUTPUT_B,
            [750, 795, 706],
        ),
        (
            fixture::FAIL_MIXER_INPUT_PATH,
            URP_NATIVE_HARDWARE_GAIN_INPUT,
            [500, 795, 245],
        ),
        (
            fixture::FAIL_START,
            URP_NATIVE_HARDWARE_GAIN_OUTPUT_B,
            [750, 795, 706],
        ),
    ] {
        fixture::clear_failure();
        fixture::restored_mixer_levels([750, 795, 245]);
        let mut config = request(&raw);
        config.hardware_gain_mask = mask;
        config.hardware_output_b_gain_db = 3.0;
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_OK);
        fixture::set_failure(failure);
        // SAFETY: the failed start retains this unique station for a retry.
        unsafe {
            assert_eq!(api.native_start.unwrap()(handle), URP_AST_SETUP_FAILED);
            assert_eq!(fixture::audio_start_mixer_levels(), [0; 3]);
            assert!(!fixture::ptt_state().0);
            fixture::clear_failure();
            assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
            assert_eq!(fixture::mixer_levels(), expected);
            api.native_destroy.unwrap()(handle);
        }
    }
}

#[test]
fn native_failed_audio_stop_does_not_reapply_gains_while_callbacks_remain_started() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let mut config = request(&raw);
    config.hardware_gain_mask = 7;
    config.hardware_output_a_gain_db = 6.012;
    config.hardware_output_b_gain_db = 3.0;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    let api = product_descriptor();
    // SAFETY: the failed stop retains the unique station and its live callbacks.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        fixture::set_failure(fixture::FAIL_STOP);
        assert_eq!(api.native_stop.unwrap()(handle), URP_AST_SETUP_FAILED);
        fixture::restored_mixer_levels([750, 795, 245]);
        fixture::set_failure(fixture::FAIL_MIXER);
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(fixture::mixer_levels(), [750, 795, 245]);
        assert_eq!(fixture::audio_start_mixer_levels(), [0; 3]);
        fixture::clear_failure();
        api.native_destroy.unwrap()(handle);
    }
}

#[test]
fn native_create_rejects_old_or_short_configurations_before_reading_the_gain_tail() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    #[repr(C, align(8))]
    struct Header {
        struct_size: u32,
        abi_version: u32,
    }
    for abi_version in [1, URP_NATIVE_CONFIG_ABI_VERSION] {
        let header = Header {
            struct_size: size_of::<Header>() as u32,
            abi_version,
        };
        let (status, handle) = create_pointer(ptr::from_ref(&header).cast());
        assert_eq!(status, URP_AST_INCOMPATIBLE_ABI);
        assert!(handle.is_null());
    }
    let (status, handle) = create_pointer(ptr::null());
    assert_eq!(status, URP_AST_INVALID_ARGUMENT);
    assert!(handle.is_null());
    let storage = [0_u64; 2];
    let (status, handle) = create_pointer(storage.as_ptr().cast::<u8>().wrapping_add(1).cast());
    assert_eq!(status, URP_AST_INVALID_ARGUMENT);
    assert!(handle.is_null());
    let raw = radio();
    let mut config = request(&raw);
    for version in [1, 2, 4] {
        config.abi_version = version;
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_INCOMPATIBLE_ABI);
        assert!(handle.is_null());
    }
}

/// Cache publication can legitimately reject a concurrent control read as busy.
/// Retry only that admission status; successful writes are never submitted twice.
fn retry_site_busy(mut operation: impl FnMut() -> c_int) -> c_int {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    loop {
        let result = operation();
        if result != URP_AST_CHANNEL_BUSY {
            return result;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "site cache did not admit a coherent read"
        );
        std::thread::yield_now();
    }
}

#[test]
fn native_site_bridge_rejects_reserved_and_stale_writes_and_acknowledges_service() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let mut config = request(&raw);
    config.gpio_output_enable_mask = 0x21;
    config.clip_led_mask = 1;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    let api = product_descriptor();
    let mut snapshot = UrpAstSiteSnapshot {
        struct_size: size_of::<UrpAstSiteSnapshot>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstSiteSnapshot::default()
    };
    // SAFETY: serialized native owner and fully initialized ABI records live until destroy.
    unsafe {
        assert_eq!(
            retry_site_busy(|| api.native_site_snapshot.unwrap()(handle, 0, &mut snapshot)),
            URP_AST_NOT_READY
        );
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            retry_site_busy(|| api.native_site_snapshot.unwrap()(handle, 0, &mut snapshot)),
            URP_AST_OK
        );
        assert_eq!(snapshot.output_mask, 0x21);
        assert_eq!(snapshot.reserved_mask, 1);
        assert_eq!(snapshot.readback_valid_mask, 0);
        let old_generation = snapshot.generation;
        let mut binding = UrpAstSiteBinding {
            struct_size: size_of::<UrpAstSiteBinding>() as u32,
            abi_version: ABI_VERSION,
            generation: old_generation,
            output_mask: 1,
            initial_high_mask: 1,
            safe_high_mask: 0,
        };
        assert_eq!(
            retry_site_busy(|| api.native_site_bind.unwrap()(handle, 0, &binding)),
            URP_AST_NOT_READY
        );
        binding.output_mask = 0x20;
        binding.initial_high_mask = 0x20;
        assert_eq!(
            retry_site_busy(|| api.native_site_bind.unwrap()(handle, 0, &binding)),
            URP_AST_OK
        );
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        loop {
            let result =
                retry_site_busy(|| api.native_site_snapshot.unwrap()(handle, 0, &mut snapshot));
            if result == URP_AST_OK
                && snapshot.generation != old_generation
                && snapshot.serviced_generation == snapshot.generation
            {
                break;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "binding was not serviced"
            );
            std::thread::yield_now();
        }
        assert_eq!(snapshot.claimed_mask, 0x20);
        assert_eq!(snapshot.serviced_high_mask & 0x20, 0x20);
        let request = UrpAstSiteRequest {
            struct_size: size_of::<UrpAstSiteRequest>() as u32,
            abi_version: ABI_VERSION,
            generation: old_generation,
            output_mask: 0x20,
            high_mask: 0,
        };
        assert_eq!(
            retry_site_busy(|| api.native_site_publish.unwrap()(handle, 0, &request)),
            URP_AST_NOT_READY
        );
        let request = UrpAstSiteRequest {
            generation: snapshot.generation,
            ..request
        };
        assert_eq!(
            retry_site_busy(|| api.native_site_publish.unwrap()(handle, 0, &request)),
            URP_AST_OK
        );
        assert_eq!(
            api.native_site_snapshot.unwrap()(handle, 1, &mut snapshot),
            URP_AST_NOT_READY
        );
        assert_eq!(
            api.native_site_snapshot.unwrap()(handle, 99, &mut snapshot),
            URP_AST_INVALID_ARGUMENT
        );
        snapshot.struct_size = 8;
        assert_eq!(
            retry_site_busy(|| api.native_site_snapshot.unwrap()(handle, 0, &mut snapshot)),
            URP_AST_INCOMPATIBLE_ABI
        );
        assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
        api.native_destroy.unwrap()(handle);
    }
}

#[test]
fn native_create_rejects_invalid_callback_and_frame_contracts() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    let raw = radio();
    let config = request(&raw);
    let providers = fixture::manifest();
    let mut callbacks = direct_callbacks();
    let mut args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config: &config,
        providers: &providers,
        callbacks: &callbacks,
        generation_id: 8,
        maximum_frames: 960,
    };
    let mut output = ptr::dangling_mut();
    let create = product_descriptor().native_create.unwrap();
    // SAFETY: every request and advertised descriptor remains readable for the call.
    unsafe {
        args.callbacks = ptr::null();
        assert_eq!(create(&args, &mut output), URP_AST_INVALID_ARGUMENT);
        assert!(output.is_null());

        args.callbacks = &callbacks;
        callbacks.struct_size -= 1;
        assert_eq!(create(&args, &mut output), URP_AST_INCOMPATIBLE_ABI);
        assert!(output.is_null());
        callbacks.struct_size += 1;

        args.generation_id = 0;
        assert_eq!(create(&args, &mut output), URP_AST_INVALID_ARGUMENT);
        args.generation_id = 8;
        args.maximum_frames = 0;
        assert_eq!(create(&args, &mut output), URP_AST_INVALID_ARGUMENT);
        args.maximum_frames = 4097;
        assert_eq!(create(&args, &mut output), URP_AST_INVALID_ARGUMENT);
        args.maximum_frames = 960;
        callbacks.receive = None;
        assert_eq!(create(&args, &mut output), URP_AST_INVALID_ARGUMENT);
        assert!(output.is_null());
    }
}

#[test]
fn native_create_rejects_invalid_selection_and_processing_fields() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    let raw = radio();
    type Case = (&'static str, fn(&mut UrpNativeStationConfig));
    let cases: &[Case] = &[
        ("input buffer", |c| c.input_extra_buffer_ms = 501),
        ("output buffer", |c| c.output_extra_buffer_ms = 501),
        ("ptt inversion", |c| c.ptt_inverted = 2),
        ("deemphasis", |c| c.receive_deemphasis = 2),
        ("output gain", |c| c.receive_output_gain_db = 31),
        ("hardware gain mask", |c| c.hardware_gain_mask = 8),
        ("capture gain below range", |c| {
            c.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_INPUT;
            c.hardware_input_gain_db = -30.01;
        }),
        ("output-A gain above range", |c| {
            c.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_OUTPUT_A;
            c.hardware_output_a_gain_db = 30.01;
        }),
        ("output-B gain NaN", |c| {
            c.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_OUTPUT_B;
            c.hardware_output_b_gain_db = f64::NAN;
        }),
        ("capture infinite gain", |c| {
            c.hardware_gain_mask = URP_NATIVE_HARDWARE_GAIN_INPUT;
            c.hardware_input_gain_db = f64::INFINITY;
        }),
        ("gpio enable", |c| c.gpio_output_enable_mask = 256),
        ("gpio initial", |c| c.gpio_output_initial_mask = 256),
        ("clip gpio range", |c| c.clip_led_mask = 256),
        ("multiple clip gpios", |c| c.clip_led_mask = 3),
        ("receive graph", |c| c.receive_graph_length = 0),
        ("transmit graph", |c| c.transmit_graph_length = 0),
        ("transmit emphasis flag", |c| c.transmit_preemphasis = 2),
        ("transmit emphasis NaN", |c| {
            c.transmit_preemphasis_corner_hz = f64::NAN
        }),
        ("transmit emphasis zero", |c| {
            c.transmit_preemphasis_corner_hz = 0.0
        }),
        ("transmit emphasis high", |c| {
            c.transmit_preemphasis_corner_hz = 501.0
        }),
        ("receive emphasis NaN", |c| {
            c.receive_deemphasis_corner_hz = f64::NAN
        }),
        ("receive emphasis zero", |c| {
            c.receive_deemphasis_corner_hz = 0.0
        }),
        ("receive emphasis high", |c| {
            c.receive_deemphasis_corner_hz = 501.0
        }),
        ("selection policy", |c| c.device_selection = 2),
        ("exact selection", |c| c.device_selection = 0),
        ("input channels", |c| c.input_device_channels = 3),
        ("output channels", |c| c.output_device_channels = 0),
        ("cm119 profile", |c| c.cm119_profile = 4),
    ];
    for (case, configure) in cases {
        let mut config = request(&raw);
        configure(&mut config);
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_INVALID_CONFIGURATION, "{case}");
        assert!(handle.is_null(), "{case}");
    }

    let mut config = request(&raw);
    config.device_identifier = b"usb\0unexpected".as_ptr();
    config.device_identifier_length = b"usb\0unexpected".len() as u32;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_INVALID_CONFIGURATION);
    assert!(handle.is_null());
}

#[test]
fn native_create_accepts_released_device_profiles_and_exact_selectors() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let api = product_descriptor();
    for profile in 0..=3 {
        let mut config = request(&raw);
        config.cm119_profile = profile;
        config.gpio_output_enable_mask = 1;
        config.gpio_output_initial_mask = 1;
        let (status, handle) = create(&config);
        assert_eq!(status, URP_AST_OK, "CM119 profile {profile}");
        // SAFETY: successful creation transfers one owned station handle.
        unsafe { api.native_destroy.unwrap()(handle) };
    }
    let mut config = request(&raw);
    config.input_device_channels = 1;
    config.output_device_channels = 1;
    config.device_selection = 0;
    config.device_identifier = b"input-device".as_ptr();
    config.device_identifier_length = b"input-device".len() as u32;
    let (status, handle) = create(&config);
    assert_eq!(
        status, URP_AST_OK,
        "exact device identifier and mono channels"
    );
    // SAFETY: successful creation transfers one owned station handle.
    unsafe { api.native_destroy.unwrap()(handle) };

    config.device_identifier_length = 0;
    config.usb_serial = b"ABC".as_ptr();
    config.usb_serial_length = 3;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK, "exact USB serial");
    // SAFETY: successful creation transfers one owned station handle.
    unsafe {
        api.native_destroy.unwrap()(handle);
        api.native_destroy.unwrap()(ptr::null_mut());
    }
    assert_eq!(fixture::audio_streams(), 0);
}

#[test]
fn native_create_accepts_dsp_and_usb_ctcss_sources() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let mut raw = radio();
    raw.receive.ctcss_enabled = 1;
    raw.receive.ctcss_tone_mask = 1;
    for source in [3, 1] {
        raw.qualification.subaudible_source = source;
        let (status, handle) = create(&request(&raw));
        assert_eq!(status, URP_AST_OK, "subaudible source {source}");
        // SAFETY: successful creation transfers one owned station handle.
        unsafe { product_descriptor().native_destroy.unwrap()(handle) };
    }
    assert_eq!(fixture::audio_streams(), 0);
}

#[test]
fn native_lifecycle_restarts_without_asl_services_or_mixer_writes() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::exclusive_audio(true);
    fixture::set_failure(fixture::FAIL_MIXER);
    let raw = radio();
    let (status, handle) = create(&request(&raw));
    assert_eq!(status, URP_AST_OK);
    assert!(!handle.is_null());
    assert_eq!(fixture::audio_streams(), 1);
    let api = product_descriptor();
    // SAFETY: creation transferred one live handle; callbacks remain valid through destroy.
    unsafe {
        for _ in 0..2 {
            assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
            assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
        }
        api.native_destroy.unwrap()(handle);
    }
    fixture::clear_failure();
    assert_eq!(fixture::audio_streams(), 0);
    assert_eq!(fixture::audio_lifecycle(), (1, 1));
    fixture::exclusive_audio(false);
}

#[test]
fn native_partial_preparation_releases_device_and_clears_output() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::exclusive_audio(true);
    let raw = radio();
    for failure in [
        fixture::FAIL_GRAPH,
        fixture::FAIL_PREFLIGHT,
        fixture::FAIL_OPEN,
    ] {
        fixture::set_failure(failure);
        let (status, handle) = create(&request(&raw));
        assert_ne!(status, URP_AST_OK);
        assert!(handle.is_null());
        assert_eq!(fixture::audio_streams(), 0);
    }
    fixture::clear_failure();
    fixture::exclusive_audio(false);
}

#[test]
fn native_failed_start_or_stop_retains_handle_for_retry() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    let raw = radio();
    let (status, handle) = create(&request(&raw));
    assert_eq!(status, URP_AST_OK);
    let api = product_descriptor();
    // SAFETY: no callback worker is concurrent, and failed operations retain ownership.
    unsafe {
        fixture::set_failure(fixture::FAIL_START);
        assert_ne!(api.native_start.unwrap()(handle), URP_AST_OK);
        fixture::clear_failure();
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        fixture::set_failure(fixture::FAIL_STOP);
        assert_ne!(api.native_stop.unwrap()(handle), URP_AST_OK);
        fixture::clear_failure();
        assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
        api.native_destroy.unwrap()(handle);
    }
    assert_eq!(fixture::audio_streams(), 0);
}
