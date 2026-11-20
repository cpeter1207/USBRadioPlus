//! Exercise station/radio ABI boundaries before a device or native graph is constructed.
use super::*;
use usbradioplus_core::CtcssTone;

type RadioChange = fn(&mut RawSessionConfig);
type StationChange = fn(&mut UrpNativeStationConfig);

fn ctcss() -> RawSessionConfig {
    let mut raw = standalone_radio();
    let index = CtcssTone::from_tenths_hz(1000).unwrap().table_index();
    raw.receive.ctcss_enabled = 1;
    raw.receive.ctcss_tone_mask = 1 << index;
    raw.qualification.subaudible_source = 3;
    raw.transmit.ctcss_transmit_enabled = 1;
    raw.transmit.default_ctcss_frequency_tenths_hz = 1000;
    raw.transmit.mapped_ctcss_frequency_tenths_hz[index] = 1000;
    raw.transmit.ctcss_peak = 0.1;
    raw.transmit.ctcss_turnoff_duration_ms = 150;
    raw.transmit.ctcss_turnoff_phase_shift_degrees = 120.0;
    raw.transmit.ctcss_turnoff_tail_tone_hz = 55.0;
    raw.transmit.output_a_route = 3;
    raw
}

#[test]
fn physical_and_legacy_spans_cannot_override_typed_owners() {
    let cases: &[(StationChange, &str)] = &[
        (
            |s| s.device_identifier_length = 1_048_577,
            "device_identifier",
        ),
        (|s| s.usb_serial_length = 1_048_577, "device_identifier"),
        (|s| s.processing_node_length = 1, "processing_configuration"),
        (|s| s.receive_graph_length = 1, "graph"),
        (|s| s.transmit_graph_length = 1, "graph"),
        (|s| s.gpio_output_enable_mask = 255, "gpio_masks"),
        (|s| s.gpio_output_initial_mask = 1, "gpio_masks"),
        (|s| s.clip_led_mask = 4, "gpio_masks"),
    ];
    for (change, token) in cases {
        let failure = prepare(&standalone_radio(), b"{}", change).err().unwrap();
        assert_eq!(failure.1, *token);
    }
    let failure = prepare(&standalone_radio(), b"{}", |s| s.usb_serial_length = 1)
        .err()
        .unwrap();
    assert_eq!(failure, (Status::InvalidArgument.code(), String::new()));
}

#[test]
fn radio_level_timing_and_transmit_calibration_errors_return_static_fields() {
    let cases: &[(RadioChange, &str)] = &[
        (|r| r.receive_input_gain = 100.0, "receive_input_gain"),
        (|r| r.receive.squelch_open_level = 32768, "squelch_level"),
        (|r| r.receive.squelch_hysteresis = 32768, "squelch_level"),
        (
            |r| r.qualification.rx_on_delay_blocks = 3001,
            "delay_frames",
        ),
        (
            |r| r.qualification.tx_off_delay_blocks = 3001,
            "delay_frames",
        ),
        (|r| r.transmit.tx_settle_time_ms = -1, "transmit_timing"),
        (
            |r| r.transmit.receiver_blanking_ms = 32768,
            "transmit_timing",
        ),
        (|r| r.transmit.output_a_tone_gain = f32::NAN, "output_route"),
        (
            |r| r.transmit.output_b_tone_bias = f32::INFINITY,
            "output_route",
        ),
        (
            |r| r.receive.ctcss_decoder_gain = f32::NAN,
            "receive_decoder_gain_db",
        ),
        (
            |r| r.receive.ctcss_decoder_gain = 0.0,
            "receive_decoder_gain_db",
        ),
        (
            |r| r.receive.ctcss_decoder_gain = 0.0001,
            "receive_decoder_gain_db",
        ),
        (|r| r.receive.ctcss_tone_mask = 0, "receive_frequencies"),
        (|r| r.qualification.subaudible_source = 0, "receive_source"),
        (|r| r.transmit.ctcss_peak = 1.1, "transmit_ctcss"),
        (
            |r| r.transmit.ctcss_turnoff_duration_ms = 39,
            "transmit_ctcss",
        ),
        (
            |r| r.transmit.ctcss_turnoff_phase_shift_degrees = f64::NAN,
            "transmit_ctcss",
        ),
        (
            |r| r.transmit.ctcss_turnoff_phase_shift_degrees = 0.0,
            "transmit_ctcss",
        ),
        (
            |r| r.transmit.ctcss_turnoff_tail_tone_hz = f64::INFINITY,
            "transmit_ctcss",
        ),
        (
            |r| r.transmit.ctcss_turnoff_tail_tone_hz = 0.0,
            "transmit_ctcss",
        ),
    ];
    for (change, field) in cases {
        let mut raw = ctcss();
        change(&mut raw);
        assert_eq!(prepare(&raw, b"{}", |_| {}).err().unwrap().1, *field);
    }
    for (peak, duration, field) in [(1.1, 150, "transmit_dcs"), (0.1, 201, "transmit_dcs")] {
        let mut raw = standalone_radio();
        raw.transmit.dcs_transmit_enabled = 1;
        raw.transmit.dcs_peak = peak;
        raw.transmit.dcs_turnoff_duration_ms = duration;
        assert_eq!(prepare(&raw, b"{}", |_| {}).err().unwrap().1, field);
    }
}

#[test]
fn carrier_sources_native_routes_and_vad_bind_to_station_metadata() {
    use usbradioplus_core::{CarrierSource, HardwareOutputAssignment, ReceiveAudioSource};
    let sources = [
        (7, CarrierSource::External),
        (3, CarrierSource::Usb),
        (4, CarrierSource::UsbInverted),
        (5, CarrierSource::Parallel),
        (6, CarrierSource::ParallelInverted),
    ];
    for (index, source) in sources {
        let mut raw = standalone_radio();
        raw.receive_input_gain = 0.0;
        raw.qualification.carrier_source = index;
        raw.receive.vad_mode = 2;
        raw.transmit.output_a_route = 4;
        raw.transmit.output_b_route = 2;
        let owner = prepare(&raw, b"{}", |_| {}).unwrap();
        // SAFETY: the immutable request is retained by owner.
        let req = unsafe { crate::native_configuration::prepared(owner.0).unwrap() };
        let station = &req.configuration.station;
        assert_eq!(station.receive.cos_assignment, source);
        assert_eq!(
            station.hardware.output_a_assignment,
            HardwareOutputAssignment::AuxiliaryVoice
        );
        assert_eq!(
            station.hardware.output_b_assignment,
            HardwareOutputAssignment::Ctcss
        );
        assert_eq!(station.receive.audio_source, ReceiveAudioSource::Disabled);
    }
    for (source, mode, allowed) in [(0, 2, false), (8, 1, false), (3, 1, true)] {
        let mut raw = standalone_radio();
        raw.qualification.carrier_source = source;
        raw.receive.vad_mode = mode;
        assert_eq!(
            prepare(&raw, b"{}", |_| {}).is_ok(),
            allowed,
            "source={source}, mode={mode}"
        );
    }
}

#[test]
fn active_ctcss_projects_dormant_or_inferred_ordered_metadata() {
    let mut raw = ctcss();
    for mode in [2, 3] {
        raw.transmit.tone_off_mode = mode;
        let owner = prepare(&raw, b"{}", |_| {}).unwrap();
        // SAFETY: the prepared owner remains live.
        let req = unsafe { crate::native_configuration::prepared(owner.0).unwrap() };
        assert_eq!(
            req.configuration.station.ctcss.transmit_default,
            CtcssTone::from_tenths_hz(1000).unwrap()
        );
    }
    raw.receive.ctcss_enabled = 0;
    raw.qualification.subaudible_source = 0;
    let owner = prepare(&raw, b"{}", |_| {}).unwrap();
    // SAFETY: the prepared owner remains live.
    let req = unsafe { crate::native_configuration::prepared(owner.0).unwrap() };
    assert_eq!(
        req.configuration.station.ctcss.transmit_frequencies,
        [CtcssTone::from_tenths_hz(1000).unwrap()]
    );
    raw.transmit.ctcss_transmit_enabled = 0;
    for source in [1, 2, 4, 5] {
        raw.qualification.subaudible_source = source;
        prepare(&raw, b"{}", |_| {}).unwrap();
    }
}

#[test]
fn conflicting_active_signals_are_rejected_before_publishing_an_owner() {
    let reject = |raw, dto: &[u8], token| {
        assert_eq!(prepare(&raw, dto, |_| {}).err().unwrap().1, token);
    };
    // Supplied ordered tones must agree with both active signaling and the qualification source.
    let mut raw = ctcss();
    reject(raw, br#"{"station":{"ctcss":{}}}"#, "receive_signaling");
    raw.receive.ctcss_relax = 1;
    reject(
        raw,
        br#"{"station":{"ctcss":{"receive_relax":1}}}"#,
        "transmit_signaling",
    );
    raw.transmit.default_ctcss_frequency_tenths_hz = 1011;
    reject(raw, b"{}", "ctcss_frequencies");

    let mut raw = standalone_radio();
    raw.receive.dcs_enabled = 1;
    raw.qualification.subaudible_source = 1;
    reject(raw, b"{}", "receive_signaling");
    raw.receive.dcs_enabled = 0;
    raw.qualification.subaudible_source = 0;
    raw.transmit.dcs_transmit_enabled = 1;
    raw.transmit.dcs_code = 0o23;
    raw.transmit.dcs_peak = 0.1;
    raw.transmit.dcs_turnoff_duration_ms = 180;
    raw.transmit.dcs_turnoff_enabled = 1;
    raw.transmit.output_a_route = 3;
    reject(raw, br#"{"station":{"dcs":{}}}"#, "transmit_signaling");
}
