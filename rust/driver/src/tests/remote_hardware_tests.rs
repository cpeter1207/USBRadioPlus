use super::*;
use crate::remote_request::tests::configuration;
use crate::test_support::setting;
use usbradioplus_core::CarrierSource;
use usbradioplus_radio::SubaudibleSource;
fn plan() -> HardwarePlan {
    let config = crate::DriverConfiguration::parse("test", "[usb]\n").unwrap();
    usbradioplus_station::hardware_plan(config.channel("usb").unwrap().config())
}

fn parallel_plan() -> HardwarePlan {
    let mut source = String::from("[usb]\n[hardware]\n");
    for pin in 2..=9 {
        source.push_str(&format!(
            "hardware_parallel_pin_{pin}_assignment={}\n",
            if pin == 5 { "ptt" } else { "out0" }
        ));
    }
    source.push_str(
        "hardware_parallel_pin_10_assignment=cor\nhardware_parallel_pin_12_assignment=ctcss\n",
    );
    let station = crate::DriverConfiguration::parse("test", &source).unwrap();
    usbradioplus_station::hardware_plan(station.channel("usb").unwrap().config())
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
    let plan = parallel_plan();
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

#[test]
fn external_parallel_encoder_respects_enable_ptt_and_backend_owned_pins() {
    use usbradioplus_core::{RemoteBackend, RemoteEncoderOutput, radio_control::ParallelPin};
    for (backend, pin, allowed) in [
        (RemoteBackend::Hamlib, 2, true),
        (RemoteBackend::Hamlib, 5, false),
        (RemoteBackend::ParallelChannel, 2, true),
        (RemoteBackend::ParallelChannel, 6, false),
        (RemoteBackend::ParallelRtx, 7, true),
        (RemoteBackend::ParallelRtx, 2, false),
    ] {
        let mut config = configuration();
        config.backend = backend;
        config.external_encode_output = Some(RemoteEncoderOutput::Parallel(
            ParallelPin::new(pin).unwrap(),
        ));
        let mut plan = parallel_plan();
        let output = external_encoder(&config, &plan);
        assert_eq!(output.is_ok(), allowed, "{backend:?} pin{pin}");
        if allowed {
            let output = output.unwrap().unwrap();
            assert_eq!(output.bank, crate::SiteBank::Parallel);
            assert_eq!(output.mask, 1 << (pin - 2));
            plan.parallel.as_mut().unwrap().config.output_enable_mask &= !output.mask;
            assert!(external_encoder(&config, &plan).is_err());
        }
        plan.parallel = None;
        assert!(external_encoder(&config, &plan).is_err());
    }
}

#[test]
fn rtx_grid_admission_covers_both_reference_bands_and_transmit_word_limits() {
    assert!(rtx_frequencies_representable(433_925_000, 433_925_000));
    assert!(!rtx_frequencies_representable(433_925_000, 433_925_001));
    assert!(!rtx_frequencies_representable(433_925_000, 3_276_800_000));
    assert!(rtx_frequencies_representable(200_000_000, 200_000_000));
    assert!(!rtx_frequencies_representable(10_000_000, 0));
}

#[test]
fn remote_wiring_uses_available_parallel_qualifiers_and_rejects_missing_carriers() {
    let mut config = configuration();
    config.cor_source = Some("parallel".into());
    config.memories[0].state.decode.source = usbradioplus_core::RemoteSignalSource::External;
    config.memories[0].state.decode.enabled = true;
    config.memories[0].state.decode.tone_tenths_hz = 1148;
    let mut plan = parallel_plan();
    plan.cm119 = None;
    assert_ne!(plan.parallel.as_ref().unwrap().ctcss_mask, 0);
    for (carrier, decode) in [
        (CarrierSource::Parallel, SubaudibleSource::Parallel),
        (
            CarrierSource::ParallelInverted,
            SubaudibleSource::ParallelInverted,
        ),
    ] {
        let wiring = RemotePhysicalWiring::select(&config, &plan, carrier, decode, &[]).unwrap();
        assert_eq!(wiring.ptt, RemotePttSource::Parallel);
        assert_eq!(wiring.carrier, carrier);
        assert_eq!(wiring.external_decode, decode);
    }
    // Subsequent capability probes do not request decode qualification.
    config.memories[0].state.decode.enabled = false;
    // A read-only or unobserved PTT operation cannot replace the selected output.
    config.ptt_source = Some("hamlib".into());
    for capability in [
        setting(20, true, false),
        setting(20, false, true),
        setting(21, true, false),
    ] {
        assert!(
            RemotePhysicalWiring::select(
                &config,
                &plan,
                CarrierSource::Dsp,
                SubaudibleSource::Disabled,
                &[capability]
            )
            .is_err()
        );
    }
    let settings = [setting(20, true, true)];
    plan.parallel.as_mut().unwrap().ctcss_mask = 0;
    let wiring = RemotePhysicalWiring::select(
        &config,
        &plan,
        CarrierSource::Parallel,
        SubaudibleSource::Parallel,
        &settings,
    )
    .unwrap();
    assert_eq!(wiring.external_decode, SubaudibleSource::Disabled);
    plan.parallel.as_mut().unwrap().carrier_mask = 0;
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Parallel,
            SubaudibleSource::Disabled,
            &settings
        )
        .is_err()
    );
    plan.parallel = None;
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Parallel,
            SubaudibleSource::Disabled,
            &settings
        )
        .is_err()
    );
    config.cor_source = Some("usb".into());
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
    config.cor_source = Some("hamlib".into());
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
    config.cor_source = None;
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::External,
            SubaudibleSource::Disabled,
            &settings
        )
        .is_err()
    );
    config.cor_source = Some("open".into());
    for decode in [
        SubaudibleSource::Usb,
        SubaudibleSource::UsbInverted,
        SubaudibleSource::Parallel,
    ] {
        assert_eq!(
            RemotePhysicalWiring::select(&config, &plan, CarrierSource::Dsp, decode, &settings)
                .unwrap()
                .external_decode,
            SubaudibleSource::Disabled
        );
    }
    plan = self::plan();
    config.cor_source = Some("usb".into());
    assert_eq!(
        RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Usb,
            &settings
        )
        .unwrap()
        .carrier,
        CarrierSource::Usb
    );
    let mut parallel = parallel_plan();
    parallel.cm119 = None;
    parallel.parallel.as_mut().unwrap().ptt_mask = 0;
    config.ptt_source = Some("parallel".into());
    config.cor_source = Some("open".into());
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &parallel,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &[]
        )
        .is_err()
    );
    parallel.parallel.as_mut().unwrap().ptt_mask = 8;
    config.backend = RemoteBackend::ParallelRtx;
    config.ptt_source = Some("hamlib".into());
    assert!(
        RemotePhysicalWiring::select(
            &config,
            &parallel,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &settings
        )
        .is_err(),
        "RTX must own the parallel PTT transaction"
    );
}
