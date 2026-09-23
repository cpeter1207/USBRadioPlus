use super::*;
use usbradioplus_core::{ChainRole, HardwareConfig, ProcessingChain, StationConfig};

fn channel(hardware: HardwareConfig) -> ChannelConfiguration {
    ChannelConfiguration {
        station: StationConfig {
            hardware,
            ..StationConfig::default()
        },
        local: ProcessingChain::shipped(ChainRole::LocalReceive),
        link: ProcessingChain::shipped(ChainRole::Link),
        voice_telemetry: ProcessingChain::shipped(ChainRole::VoiceTelemetry),
    }
}

fn selected_device() -> SelectedDevice {
    SelectedDevice {
        interface_path: "3-1".to_owned(),
        serial: Some("CM119".to_owned()),
        alsa_card_index: 2,
        input_device_index: 4,
        output_device_index: 5,
        input_channels: ChannelCount::Mono,
        output_channels: ChannelCount::Stereo,
    }
}

#[test]
fn defaults_select_automatically_and_prepare_no_optional_outputs() {
    let config = HardwareConfig::default();
    assert_eq!(config.input_extra_buffer_ms, 0);
    assert_eq!(config.output_extra_buffer_ms, 0);
    let plan = hardware_plan(&channel(config));

    assert_eq!(plan.audio_selector.policy, SelectionPolicy::Automatic);
    assert_eq!(plan.audio_selector.identifier, None);
    assert_eq!(plan.audio_selector.serial, None);
    assert_eq!(plan.audio_selector.input_channels, ChannelCount::Mono);
    assert_eq!(plan.audio_selector.output_channels, ChannelCount::Stereo);
    assert_eq!(plan.cm119.profile, Cm119Profile::DudeUsb);
    assert_eq!(plan.cm119.output_enable_mask, 0);
    assert_eq!(plan.cm119.output_initial_mask, 0);
    assert_eq!(plan.parallel, None);
}

#[test]
fn identity_uses_audio_identifier_then_checks_gpio_path_and_serial() {
    let config = HardwareConfig {
        device_identifier: "hw:2,0".to_owned(),
        serial: "CM119".to_owned(),
        gpio_usb_port_path: "3-1".to_owned(),
        input_extra_buffer_ms: 20,
        output_extra_buffer_ms: 35,
        ..HardwareConfig::default()
    };
    let plan = hardware_plan(&channel(config));

    assert_eq!(plan.audio_selector.policy, SelectionPolicy::Exact);
    assert_eq!(plan.audio_selector.identifier.as_deref(), Some("hw:2,0"));
    assert_eq!(plan.audio_selector.serial.as_deref(), Some("CM119"));
    assert_eq!(plan.cm119.required_usb_port_path.as_deref(), Some("3-1"));

    let selected = selected_device();
    let bound = plan.select(&selected, 960).unwrap();
    assert_eq!(bound.stream.maximum_receive_frame_count, 960);
    assert_eq!(bound.stream.maximum_transmit_frame_count, 960);
    assert_eq!(bound.stream.input_device_index, 4);
    assert_eq!(bound.stream.output_device_index, 5);
    assert_eq!(bound.stream.extra_input_buffer_milliseconds, 20);
    assert_eq!(bound.stream.extra_output_buffer_milliseconds, 35);
    assert_eq!(bound.cm119.usb_port_path, "3-1");
    assert_eq!(bound.cm119.vendor_id, 0);
    assert_eq!(bound.cm119.product_id, 0);

    let mut mismatch = selected.clone();
    mismatch.interface_path = "3-2".to_owned();
    assert_eq!(
        plan.select(&mismatch, 960),
        Err(HardwarePlanError::UsbIdentityMismatch)
    );
    mismatch.interface_path = "3-1".to_owned();
    mismatch.serial = Some("other".to_owned());
    assert_eq!(
        plan.select(&mismatch, 960),
        Err(HardwarePlanError::SerialMismatch)
    );
}

#[test]
fn gpio_topology_can_supply_the_exact_audio_identity() {
    let config = HardwareConfig {
        gpio_usb_port_path: "3-1".to_owned(),
        ..HardwareConfig::default()
    };
    let plan = hardware_plan(&channel(config));

    assert_eq!(plan.audio_selector.policy, SelectionPolicy::Exact);
    assert_eq!(plan.audio_selector.identifier.as_deref(), Some("3-1"));
}

#[test]
fn dude_usb_keeps_only_ordinary_outputs_and_clip_led() {
    let config = HardwareConfig {
        gpio_modes: [
            GpioMode::OutputHigh,
            GpioMode::OutputLow,
            GpioMode::OutputHigh,
            GpioMode::Input,
            GpioMode::OutputHigh,
            GpioMode::OutputLow,
            GpioMode::Input,
            GpioMode::Input,
        ],
        clip_led_gpio: Some(8),
        ..HardwareConfig::default()
    };
    let plan = hardware_plan(&channel(config));

    assert_eq!(plan.cm119.output_enable_mask, 0xb3);
    assert_eq!(plan.cm119.output_initial_mask, 0x11);
}

#[test]
fn sph_usb_rejects_nonordinary_outputs_including_dedicated_ptt() {
    let config = HardwareConfig {
        interface_type: HardwareInterfaceType::SphUsb,
        gpio_modes: [GpioMode::OutputHigh; 8],
        ..HardwareConfig::default()
    };
    let plan = hardware_plan(&channel(config));

    assert_eq!(plan.cm119.profile, Cm119Profile::SphUsb);
    assert_eq!(plan.cm119.output_enable_mask, 1);
    assert_eq!(plan.cm119.output_initial_mask, 1);
}

#[test]
fn binding_rejects_a_noncanonical_channel_layout() {
    let plan = hardware_plan(&channel(HardwareConfig::default()));
    let mut selected = selected_device();
    selected.input_channels = ChannelCount::Stereo;

    assert_eq!(
        plan.select(&selected, 960),
        Err(HardwarePlanError::ChannelLayoutMismatch)
    );
    selected.input_channels = ChannelCount::Mono;
    selected.output_channels = ChannelCount::Mono;
    assert_eq!(
        plan.select(&selected, 960),
        Err(HardwarePlanError::ChannelLayoutMismatch)
    );
}

#[test]
fn binding_errors_have_specific_messages() {
    let cases = [
        (
            HardwarePlanError::ChannelLayoutMismatch,
            "selected audio device is not mono capture with stereo playback",
        ),
        (
            HardwarePlanError::UsbIdentityMismatch,
            "selected audio device does not match the configured GPIO USB path",
        ),
        (
            HardwarePlanError::SerialMismatch,
            "selected audio device does not match the configured USB serial number",
        ),
    ];

    for (error, expected) in cases {
        assert_eq!(error.to_string(), expected);
    }
}

#[test]
fn parallel_assignments_map_to_adapter_and_signal_masks() {
    let mut config = HardwareConfig::default();
    config.parallel_port.device = "/dev/parport1".to_owned();
    config.parallel_port.base_address = 0x400;
    config.parallel_port.output_assignments[0] = Some(ParallelOutputAssignment::Low);
    config.parallel_port.output_assignments[1] = Some(ParallelOutputAssignment::High);
    config.parallel_port.output_assignments[2] = Some(ParallelOutputAssignment::PushToTalk);
    config.parallel_port.input_assignments = [
        Some(ParallelInputAssignment::Input),
        Some(ParallelInputAssignment::Carrier),
        Some(ParallelInputAssignment::Ctcss),
        None,
    ];

    let plan = hardware_plan(&channel(config));
    let parallel = plan.parallel.unwrap();
    assert_eq!(parallel.config.transport, ParallelTransport::Automatic);
    assert_eq!(parallel.config.ppdev_path.as_deref(), Some("/dev/parport1"));
    assert_eq!(parallel.config.raw_io_base, 0x400);
    assert_eq!(parallel.config.output_enable_mask, 0x07);
    assert_eq!(parallel.config.output_initial_mask, 0x02);
    assert_eq!(parallel.ptt_mask, 0x04);
    assert_eq!(parallel.input_mask, 0x40);
    assert_eq!(parallel.carrier_mask, 0x20);
    assert_eq!(parallel.ctcss_mask, 0x10);
}
