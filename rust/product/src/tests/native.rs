//! Native entry points use only released providers, never an Asterisk host.
use super::*;
use provider_support as fixture;

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
        abi_version: 1,
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
    }
}

fn create(config: &UrpNativeStationConfig) -> (c_int, *mut c_void) {
    let mut providers = fixture::manifest();
    // Native program PCM is pulled directly from the callback: no ASL rate/queue path.
    providers.ring = ptr::null();
    providers.samplerate = ptr::null();
    providers.rnnoise = ptr::null();
    let callbacks = direct_callbacks();
    let args = UrpNativeCreateArgs {
        struct_size: size_of::<UrpNativeCreateArgs>() as u32,
        abi_version: 1,
        config,
        providers: &providers,
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
