use super::*;

fn tone(value: u16) -> CtcssTone {
    CtcssTone::from_tenths_hz(value).unwrap()
}

#[test]
fn typed_ranges_reject_invalid_values() {
    assert_eq!(CtcssTone::from_tenths_hz(499), None);
    assert_eq!(CtcssTone::from_tenths_hz(500).unwrap().tenths_hz(), 500);
    assert_eq!(CtcssTone::from_tenths_hz(3_000).unwrap().tenths_hz(), 3_000);
    assert_eq!(CtcssTone::from_tenths_hz(3_001), None);

    assert_eq!(GpioPin::new(0), None);
    assert_eq!(GpioPin::new(1).unwrap().get(), 1);
    assert_eq!(GpioPin::new(8).unwrap().get(), 8);
    assert_eq!(GpioPin::new(9), None);

    assert_eq!(ParallelPin::new(1), None);
    assert_eq!(ParallelPin::new(2).unwrap().get(), 2);
    assert_eq!(ParallelPin::new(9).unwrap().get(), 9);
    assert_eq!(ParallelPin::new(10), None);
}

#[test]
fn retained_text_commands_parse_to_typed_messages() {
    assert_eq!(
        parse_controller_text(" SETCHAN 255\n"),
        Ok(ControlMessage::SelectChannel(255))
    );
    assert_eq!(
        parse_controller_text("RXCTCSS 0"),
        Ok(ControlMessage::ReceiveCtcss(false))
    );
    assert_eq!(
        parse_controller_text("TXCTCSS 1"),
        Ok(ControlMessage::TransmitCtcss(true))
    );
    assert_eq!(
        parse_controller_text("GPIO 8 0"),
        Ok(ControlMessage::Gpio(
            GpioPin::new(8).unwrap(),
            OutputRequest::Inactive
        ))
    );
    assert_eq!(
        parse_controller_text("GPIO 1 1"),
        Ok(ControlMessage::Gpio(
            GpioPin::new(1).unwrap(),
            OutputRequest::Active
        ))
    );
    assert_eq!(
        parse_controller_text("PP 9 11"),
        Ok(ControlMessage::Parallel(
            ParallelPin::new(9).unwrap(),
            OutputRequest::Pulse(10)
        ))
    );
    assert_eq!(
        parse_controller_text("SETFREQ 146.520000 146.940001 0.0 100.0 H"),
        Ok(ControlMessage::RemoteRadio(RemoteRadio {
            receive_hz: 146_520_000,
            transmit_hz: 146_940_001,
            receive_ctcss: None,
            transmit_ctcss: Some(tone(1_000)),
            high_power: true,
        }))
    );
    assert_eq!(
        parse_controller_text("SETFREQ 1 2 67.0 71.9 L"),
        Ok(ControlMessage::RemoteRadio(RemoteRadio {
            receive_hz: 1_000_000,
            transmit_hz: 2_000_000,
            receive_ctcss: Some(tone(670)),
            transmit_ctcss: Some(tone(719)),
            high_power: false,
        }))
    );
}

#[test]
fn text_parser_reports_exact_failure_classes() {
    assert_eq!(
        parse_controller_text(""),
        Err(ControlParseError::WrongFieldCount)
    );
    assert_eq!(
        parse_controller_text("BOGUS"),
        Err(ControlParseError::UnknownCommand)
    );
    for text in [
        "SETCHAN",
        "SETCHAN 1 extra",
        "RXCTCSS",
        "GPIO 1",
        "PP 2",
        "SETFREQ 1 2 67.0 67.0",
    ] {
        assert_eq!(
            parse_controller_text(text),
            Err(ControlParseError::WrongFieldCount),
            "{text}"
        );
    }
    for text in [
        "SETCHAN nope",
        "RXCTCSS 2",
        "TXCTCSS -1",
        "GPIO 0 1",
        "GPIO 1 -1",
        "PP 10 1",
        "PP nope 1",
        "SETFREQ 1 2 3 4",
        "SETFREQ nope 2 67.0 67.0 H",
        "SETFREQ nan 2 67.0 67.0 H",
        "SETFREQ -1 2 67.0 67.0 H",
        "SETFREQ 5000 2 67.0 67.0 H",
        "SETFREQ 1 2 nope 67.0 H",
        "SETFREQ 1 2 NaN 67.0 H",
        "SETFREQ 1 2 -67.0 67.0 H",
        "SETFREQ 1 2 6554.0 67.0 H",
        "SETFREQ 1 2 40.0 67.0 H",
        "SETFREQ 1 2 67.0 67.0 X",
    ] {
        assert_eq!(
            parse_controller_text(text),
            Err(ControlParseError::InvalidField),
            "{text}"
        );
    }
    for error in [
        ControlParseError::UnknownCommand,
        ControlParseError::WrongFieldCount,
        ControlParseError::InvalidField,
    ] {
        assert!(!error.to_string().is_empty());
    }
}

#[test]
fn control_snapshot_owns_state_and_maps_every_action() {
    let mut state = ControlSnapshot::default();
    assert_eq!(
        state,
        ControlSnapshot {
            transmit_keyed: false,
            forced_ctcss: None,
            receive_ctcss_enabled: true,
            transmit_ctcss_enabled: true,
        }
    );
    assert_eq!(
        state.apply(ControlMessage::TransmitKey(Some(tone(1_000)))),
        ControlAction::SetTransmit {
            keyed: true,
            forced_ctcss: Some(tone(1_000)),
        }
    );
    assert!(state.transmit_keyed);
    assert_eq!(state.forced_ctcss, Some(tone(1_000)));
    assert_eq!(
        state.apply(ControlMessage::TransmitUnkey),
        ControlAction::SetTransmit {
            keyed: false,
            forced_ctcss: None,
        }
    );
    assert_eq!(
        state.apply(ControlMessage::SelectChannel(7)),
        ControlAction::SelectChannel(7)
    );
    assert_eq!(
        state.apply(ControlMessage::ReceiveCtcss(false)),
        ControlAction::SetReceiveCtcss(false)
    );
    assert_eq!(
        state.apply(ControlMessage::TransmitCtcss(false)),
        ControlAction::SetTransmitCtcss(false)
    );
    let gpio = ControlMessage::Gpio(GpioPin::new(2).unwrap(), OutputRequest::Active);
    assert_eq!(
        state.apply(gpio),
        ControlAction::SetGpio(GpioPin::new(2).unwrap(), OutputRequest::Active)
    );
    let parallel = ControlMessage::Parallel(ParallelPin::new(3).unwrap(), OutputRequest::Inactive);
    assert_eq!(
        state.apply(parallel),
        ControlAction::SetParallel(ParallelPin::new(3).unwrap(), OutputRequest::Inactive)
    );
    let remote = RemoteRadio {
        receive_hz: 1,
        transmit_hz: 2,
        receive_ctcss: None,
        transmit_ctcss: None,
        high_power: false,
    };
    assert_eq!(
        state.apply(ControlMessage::RemoteRadio(remote)),
        ControlAction::ConfigureRemoteRadio(remote)
    );
}
