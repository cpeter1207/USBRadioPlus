use super::*;
use crate::remote_request::tests::configuration;
use crate::test_support::setting;
use usbradioplus_core::CarrierSource;
use usbradioplus_radio::SubaudibleSource;
fn plan() -> HardwarePlan {
    let config = crate::DriverConfiguration::parse("test", "[usb]\n").unwrap();
    usbradioplus_station::hardware_plan(config.channel("usb").unwrap().config())
}

#[test]
fn external_encoder_requires_an_unshared_configured_output() {
    use usbradioplus_core::{RemoteEncoderOutput, radio_control::GpioPin};
    let mut config = configuration();
    let mut plan = plan();
    config.external_encode_output = Some(RemoteEncoderOutput::Usb(GpioPin::new(1).unwrap()));
    plan.cm119.as_mut().unwrap().output_enable_mask = 1;
    let settings = [setting(20, true, true)];
    let select = |plan: &HardwarePlan| {
        RemotePhysicalWiring::select(
            &config,
            plan,
            CarrierSource::Dsp,
            SubaudibleSource::Dsp,
            &settings,
        )
    };
    assert!(select(&plan).unwrap().external_encode.is_some());
    plan.cm119.as_mut().unwrap().clip_led_mask = 1;
    assert!(select(&plan).is_err());
    plan.cm119.as_mut().unwrap().clip_led_mask = 0;
    plan.cm119.as_mut().unwrap().output_enable_mask = 0;
    assert!(select(&plan).is_err());
}

#[test]
fn remote_parallel_setup_checks_owned_pins_and_exact_register_grid() {
    use usbradioplus_core::RemoteBackend;
    let mut source = String::from("[usb]\n[hardware]\n");
    for pin in 2..=9 {
        source.push_str(&format!(
            "hardware_parallel_pin_{pin}_assignment={}\n",
            if pin == 5 { "ptt" } else { "out0" }
        ));
    }
    let station = crate::DriverConfiguration::parse("test", &source).unwrap();
    let plan = usbradioplus_station::hardware_plan(station.channel("usb").unwrap().config());
    let mut config = configuration();
    config.ptt_source = Some("parallel".into());
    config.backend = RemoteBackend::ParallelRtx;
    let select = |config: &ResolvedRemoteConfiguration, plan: &HardwarePlan| {
        RemotePhysicalWiring::select(config, plan, CarrierSource::Dsp, SubaudibleSource::Dsp, &[])
    };
    assert!(
        select(&config, &plan).is_ok(),
        "{:?} {:?} {:?}",
        select(&config, &plan),
        plan,
        config.cor_source
    );
    config.memories[0].state.receive_hz += 1;
    assert!(
        select(&config, &plan).is_err(),
        "off-grid RTX must not be acknowledged"
    );
    config.memories[0].state.receive_hz -= 1;
    for (mask, ptt, inverted) in [(0x1e, 8, false), (0xff, 4, false), (0xff, 8, true)] {
        let mut invalid = plan.clone();
        invalid.parallel.as_mut().unwrap().config.output_enable_mask = mask;
        invalid.parallel.as_mut().unwrap().ptt_mask = ptt;
        invalid.ptt_inverted = inverted;
        assert!(select(&config, &invalid).is_err());
    }
    config.backend = RemoteBackend::ParallelChannel;
    config.memories[0].state.channel = Some(15);
    assert!(select(&config, &plan).is_ok());
    config.memories[0].state.channel = Some(16);
    assert!(select(&config, &plan).is_err());
    config.memories[0].state.channel = Some(15);
    let mut conflict = plan.clone();
    conflict.parallel.as_mut().unwrap().ptt_mask = 0x80;
    assert!(select(&config, &conflict).is_err());
}

#[test]
fn remote_wiring_prefers_supported_rig_signals_without_runtime_fallback() {
    let config = configuration();
    let mut plan = plan();
    let settings = [setting(20, true, true), setting(21, true, false)];
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan,
        CarrierSource::Dsp,
        SubaudibleSource::Dsp,
        &settings,
    )
    .unwrap();
    assert_eq!(wiring.ptt, RemotePttSource::Rig);
    assert_eq!(wiring.carrier, CarrierSource::External);
    assert!(wiring.rig_carrier);
    plan.cm119 = None;
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan,
        CarrierSource::Dsp,
        SubaudibleSource::Disabled,
        &settings,
    )
    .unwrap();
    assert_eq!(wiring.ptt, RemotePttSource::Rig);
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &[]
        )
        .is_err()
    );
}

#[test]
fn explicit_open_receive_is_not_a_disabled_carrier_source() {
    let mut config = configuration();
    config.cor_source = Some("open".into());
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan(),
        CarrierSource::Dsp,
        SubaudibleSource::Disabled,
        &[],
    )
    .unwrap();
    assert_eq!(wiring.carrier.to_string(), "open");
    assert!(!wiring.rig_carrier);
}

#[test]
fn remote_wiring_rejects_missing_explicit_sources_and_keeps_qualification_separate() {
    let mut config = configuration();
    let mut plan = plan();
    let settings = [setting(20, true, true), setting(21, true, false)];
    config.ptt_source = Some("usb".into());
    config.cor_source = Some("usb".into());
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan,
        CarrierSource::UsbInverted,
        SubaudibleSource::UsbInverted,
        &settings,
    )
    .unwrap();
    assert_eq!(wiring.ptt, RemotePttSource::Usb);
    assert_eq!(wiring.carrier, CarrierSource::UsbInverted);
    assert!(!wiring.rig_carrier);
    assert_eq!(wiring.external_decode, SubaudibleSource::UsbInverted);
    plan.cm119 = None;
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &settings
        )
        .is_err()
    );
    config.ptt_source = Some("hamlib".into());
    config.cor_source = Some("open".into());
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan,
        CarrierSource::Dsp,
        SubaudibleSource::Disabled,
        &settings,
    )
    .unwrap();
    assert_eq!(wiring.carrier, CarrierSource::Open);
    assert_eq!(wiring.external_decode, SubaudibleSource::Disabled);
    config.cor_source = Some("hamlib".into());
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &[setting(20, false, true)]
        )
        .is_err()
    );
}

#[test]
fn remote_ptt_routing_never_mirrors_to_an_unselected_output() {
    assert!(PhysicalPttControl::Ordinary.key_for(RemotePttSource::Usb, true));
    assert!(PhysicalPttControl::Ordinary.key_for(RemotePttSource::Parallel, true));
    for (selected, key) in [
        (None, false),
        (Some(RemotePttSource::Rig), true),
        (Some(RemotePttSource::Usb), true),
        (Some(RemotePttSource::Parallel), true),
        (Some(RemotePttSource::Usb), false),
    ] {
        let routing = PhysicalPttControl::Remote { selected, key };
        assert_eq!(
            routing.key_for(RemotePttSource::Usb, true),
            key && selected == Some(RemotePttSource::Usb)
        );
        assert_eq!(
            routing.key_for(RemotePttSource::Parallel, true),
            key && selected == Some(RemotePttSource::Parallel)
        );
    }
}
