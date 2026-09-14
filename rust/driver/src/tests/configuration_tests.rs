use super::*;

#[test]
fn resolves_named_channels_once_in_file_order() {
    let configuration = DriverConfiguration::parse(
        "/etc/asterisk/usbradioplus.conf",
        "[hardware]\nhardware_input_gain_db = -3\n\
         [local warm]\ninput_gain_db = 2\n\
         [North]\nlocal_profile = warm\n\
         [south]\nchannel_enabled = no\n",
    )
    .unwrap();

    assert_eq!(configuration.channels().len(), 2);
    assert_eq!(configuration.channels()[0].channel(), "North");
    assert_eq!(
        configuration.channels()[0]
            .config()
            .station
            .hardware
            .input_gain_db,
        -3.0
    );
    assert_eq!(
        configuration.channels()[0].config().local.input_gain_db,
        2.0
    );
    assert!(!configuration.channels()[1].config().station.channel_enabled);
    assert_eq!(configuration.channel("NORTH").unwrap().channel(), "North");
    assert!(configuration.channel("missing").is_none());
}

#[test]
fn rejects_empty_and_invalid_generations_without_partial_state() {
    let empty = DriverConfiguration::parse("radio.conf", "[hardware]\n").unwrap_err();
    assert_eq!(empty, DriverConfigurationError::NoChannels);
    assert_eq!(
        empty.to_string(),
        "configuration contains no radio channels"
    );
    assert!(std::error::Error::source(&empty).is_none());

    let invalid = DriverConfiguration::parse(
        "radio.conf",
        "[first]\n[second]\n[receive second]\nsignaling_method = ctcss\n\
         [ctcss second]\nreceive_source = no\n",
    )
    .unwrap_err();
    assert!(matches!(
        invalid,
        DriverConfigurationError::Channel { ref name, .. } if name == "second"
    ));
    assert!(
        invalid
            .to_string()
            .starts_with("channel [second] is invalid:")
    );
    assert!(std::error::Error::source(&invalid).is_some());
}

#[test]
fn collects_unknown_section_and_resolved_option_warnings() {
    let configuration = DriverConfiguration::parse(
        "radio.conf",
        "[mystery site]\nvalue = one\n[hardware]\nhardware_input_gain_db = loud\n[usb]\n",
    )
    .unwrap();
    assert_eq!(configuration.warnings().len(), 2);
    assert_eq!(
        configuration.warnings()[0].kind,
        ResolutionWarningKind::UnknownSection
    );
    assert_eq!(configuration.warnings()[0].section, "mystery site");
    assert_eq!(
        configuration.warnings()[1].kind,
        ResolutionWarningKind::InvalidValue
    );
}
