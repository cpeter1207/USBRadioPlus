use super::*;
use usbradioplus_core::{
    CarrierSource as ConfigCarrierSource, CtcssSource, CtcssTurnoffMode, HardwareOutputAssignment,
    RadioDuplexMode, ReceiveAudioSource, SignalingMethod,
};

fn channel() -> ChannelConfiguration {
    ChannelConfiguration {
        station: Default::default(),
        local: usbradioplus_core::ProcessingChain::shipped(
            usbradioplus_core::ChainRole::LocalReceive,
        ),
        link: usbradioplus_core::ProcessingChain::shipped(usbradioplus_core::ChainRole::Link),
        voice_telemetry: usbradioplus_core::ProcessingChain::shipped(
            usbradioplus_core::ChainRole::VoiceTelemetry,
        ),
    }
}

#[test]
fn defaults_translate_to_the_complete_app_rpt_session() {
    let mut input = channel();
    input.station.receive.noise_filter_type = 1;
    let output = radio_session_config(&input, ControllerTransport::AppRpt, 7, 960).unwrap();
    assert_eq!(output.generation_id, 7);
    assert_eq!(output.maximum_receive_frame_count, 960);
    assert_eq!(output.maximum_transmit_frame_count, 960);
    assert_eq!(output.publication_interval_milliseconds, 50);
    assert_eq!(output.receive_input_gain, 1.0);
    assert_eq!(
        output.receive.noise_filter_profile,
        NoiseFilterProfile::Alternate
    );
    assert_eq!(output.receive.squelch_open_level, 16_350);
    assert_eq!(output.receive.squelch_hysteresis, 3_000);
    assert!(matches!(
        output.receive.signaling,
        ReceiveSignaling::Disabled
    ));
    assert_eq!(output.qualification.carrier_source, CarrierSource::DspNoise);
    assert_eq!(
        output.qualification.subaudible_source,
        SubaudibleSource::Disabled
    );
    assert!(!output.qualification.advanced_transport);
    assert!(!output.qualification.radio_duplex);
    assert!(matches!(
        output.transmit.signaling,
        TransmitSignaling::Disabled
    ));
    assert_eq!(output.transmit.output_a.route, OutputRoute::Composite);
    assert_eq!(output.transmit.output_b.route, OutputRoute::Disabled);
}

#[test]
fn all_carrier_and_external_subaudible_sources_translate_exactly() {
    let pairs = [
        (ConfigCarrierSource::Disabled, CarrierSource::Disabled),
        (ConfigCarrierSource::Dsp, CarrierSource::DspNoise),
        (ConfigCarrierSource::Vox, CarrierSource::Vox),
        (ConfigCarrierSource::Usb, CarrierSource::Usb),
        (ConfigCarrierSource::UsbInverted, CarrierSource::UsbInverted),
        (ConfigCarrierSource::Parallel, CarrierSource::Parallel),
        (
            ConfigCarrierSource::ParallelInverted,
            CarrierSource::ParallelInverted,
        ),
    ];
    for (configured, expected) in pairs {
        assert_eq!(carrier_source(configured), expected);
    }

    let pairs = [
        (CtcssSource::Disabled, SubaudibleSource::Disabled),
        (CtcssSource::Usb, SubaudibleSource::Usb),
        (CtcssSource::UsbInverted, SubaudibleSource::UsbInverted),
        (CtcssSource::Dsp, SubaudibleSource::Dsp),
        (CtcssSource::Parallel, SubaudibleSource::Parallel),
        (
            CtcssSource::ParallelInverted,
            SubaudibleSource::ParallelInverted,
        ),
    ];
    for (configured, expected) in pairs {
        let mut input = channel();
        input.station.receive.signaling_method = SignalingMethod::Ctcss;
        input.station.ctcss.receive_source = configured;
        assert_eq!(subaudible_source(&input), expected);
    }
    let mut input = channel();
    input.station.receive.signaling_method = SignalingMethod::Dcs;
    assert_eq!(subaudible_source(&input), SubaudibleSource::Dsp);

    input.station.receive.signaling_method = SignalingMethod::Ctcss;
    input.station.ctcss.receive_source = CtcssSource::Usb;
    assert!(matches!(
        receive_signaling(&input).unwrap(),
        ReceiveSignaling::Disabled
    ));
}

#[test]
fn ctcss_session_keeps_tone_map_levels_and_tail_policy() {
    let mut input = channel();
    input.station.receive.signaling_method = SignalingMethod::Ctcss;
    input.station.transmit.signaling_method = SignalingMethod::Ctcss;
    input.station.ctcss.receive_source = CtcssSource::Dsp;
    input.station.ctcss.receive_frequencies = vec!["100.0".parse().unwrap()];
    input.station.ctcss.transmit_frequencies = vec!["123.0".parse().unwrap()];
    input.station.ctcss.receive_relax = 1;
    input.station.ctcss.receive_decoder_gain_db = 6.0;
    input.station.ctcss.transmit_peak_dbfs = -20.0;
    input.station.ctcss.turnoff_mode = CtcssTurnoffMode::TailTone;
    input.station.duplex.radio_mode = RadioDuplexMode::Full;
    let output = radio_session_config(&input, ControllerTransport::RptAdvanced, 1, 480).unwrap();
    let ReceiveSignaling::Ctcss(receive) = output.receive.signaling else {
        panic!("CTCSS receive configuration expected");
    };
    assert_ne!(receive.tones.bits(), 0);
    assert!(receive.relaxed);
    assert!((output.receive.ctcss_decoder_gain - 1.995_262_4).abs() < 0.000_01);
    assert!(output.qualification.advanced_transport);
    assert!(output.qualification.radio_duplex);
    let TransmitSignaling::Ctcss(transmit) = output.transmit.signaling else {
        panic!("CTCSS transmit configuration expected");
    };
    assert_eq!(transmit.mapped_frequencies_tenths_hz[11], 1_230);
    assert!((transmit.peak - 0.1).abs() < f32::EPSILON);
    assert_eq!(output.transmit.tone_off_mode, ToneOffMode::TailTone);
}

#[test]
fn dcs_session_and_every_route_translate_without_compatibility_state() {
    let mut input = channel();
    input.station.receive.signaling_method = SignalingMethod::Dcs;
    input.station.transmit.signaling_method = SignalingMethod::Dcs;
    input.station.dcs.receive_code = "125I".parse().unwrap();
    input.station.dcs.transmit_code = "754N".parse().unwrap();
    let output = radio_session_config(&input, ControllerTransport::AppRpt, 1, 960).unwrap();
    assert!(matches!(
        output.receive.signaling,
        ReceiveSignaling::Dcs(DcsReceiveConfig {
            code: 0o125,
            inverted: true
        })
    ));
    assert!(matches!(
        output.transmit.signaling,
        TransmitSignaling::Dcs(DcsTransmitConfig {
            code: 0o754,
            inverted: false,
            ..
        })
    ));

    for (assignment, route) in [
        (HardwareOutputAssignment::Off, OutputRoute::Disabled),
        (HardwareOutputAssignment::Voice, OutputRoute::Voice),
        (HardwareOutputAssignment::Ctcss, OutputRoute::Tone),
        (HardwareOutputAssignment::VoiceCtcss, OutputRoute::Composite),
        (
            HardwareOutputAssignment::AuxiliaryVoice,
            OutputRoute::AuxiliaryVoice,
        ),
    ] {
        assert_eq!(output_config(assignment).route, route);
    }
}

#[test]
fn disabled_audio_is_silent_and_settle_overflow_is_rejected() {
    let mut input = channel();
    input.station.receive.audio_source = ReceiveAudioSource::Disabled;
    input.local.input_gain_db = 12.0;
    assert_eq!(
        radio_session_config(&input, ControllerTransport::AppRpt, 1, 960)
            .unwrap()
            .receive_input_gain,
        0.0
    );
    input.station.transmit.settle_ms = u32::MAX;
    assert_eq!(
        radio_session_config(&input, ControllerTransport::AppRpt, 1, 960),
        Err(RadioError::InvalidArgument)
    );
}

#[test]
fn all_ctcss_turnoff_modes_translate() {
    for (configured, expected) in [
        (CtcssTurnoffMode::None, ToneOffMode::None),
        (CtcssTurnoffMode::PhaseShift, ToneOffMode::PhaseShift),
        (CtcssTurnoffMode::ToneRemove, ToneOffMode::ToneRemove),
        (CtcssTurnoffMode::TailTone, ToneOffMode::TailTone),
    ] {
        assert_eq!(tone_off_mode(configured), expected);
    }
}

#[test]
fn native_processing_uses_only_the_local_and_transmit_chains() {
    let mut input = channel();
    input.station.receive.audio_source = ReceiveAudioSource::Flat;
    input.station.hardware.deemphasis_corner_hz = 275.0;
    input.station.transmit.preemphasis_enabled = false;
    input.station.hardware.preemphasis_corner_hz = 325.0;
    input.local.input_gain_db = 3.0;
    input.link.input_gain_db = 9.0;
    input.voice_telemetry.input_gain_db = -2.0;

    let result = native_processing_plan(&input);

    assert_eq!(result.local, input.local);
    assert_eq!(result.voice_telemetry, input.voice_telemetry);
    assert!(result.deemphasis_enabled);
    assert_eq!(result.deemphasis_corner_hz, 275.0);
    assert!(!result.preemphasis_enabled);
    assert_eq!(result.preemphasis_corner_hz, 325.0);

    input.station.receive.audio_source = ReceiveAudioSource::Speaker;
    assert!(!native_processing_plan(&input).deemphasis_enabled);
}
