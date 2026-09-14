use super::*;

#[test]
fn complete_channel_uses_flat_and_selected_profiles() {
    let document = ConfigDocument::new(
        "[hardware]\nhardware_input_gain_db = -3\n\
         [local]\ninput_gain_db = 1\n\
         [local hill]\ninput_gain_db = 2\n\
         [usb]\nlocal_profile = hill\n",
    );
    let mut resolved =
        ResolvedChannelConfiguration::from_document(&document, "radio.conf", "usb").unwrap();
    assert_eq!(resolved.channel(), "usb");
    assert_eq!(resolved.config().station.hardware.input_gain_db, -3.0);
    assert_eq!(resolved.config().local.input_gain_db, 2.0);
    assert_eq!(resolved.config().link.role, ChainRole::Link);
    assert_eq!(
        resolved.config().voice_telemetry.role,
        ChainRole::VoiceTelemetry
    );
    assert!(resolved.warnings().is_empty());
    resolved.config_mut().station.hardware.input_gain_db = -6.0;
    assert_eq!(resolved.config().station.hardware.input_gain_db, -6.0);
    assert_eq!(resolved.into_config().local.input_gain_db, 2.0);
}

#[test]
fn complete_channel_combines_warnings_and_propagates_errors() {
    let document = ConfigDocument::new(
        "[hardware]\nhardware_input_gain_db = loud\n\
         [local]\ninput_gain_db = louder\n\
         [usb]\n",
    );
    let resolved =
        ResolvedChannelConfiguration::from_document(&document, "radio.conf", "usb").unwrap();
    assert_eq!(resolved.warnings().len(), 2);

    let missing = ResolvedChannelConfiguration::from_document(&document, "radio.conf", "missing");
    assert!(matches!(
        &missing,
        Err(ChannelConfigurationError::Station(_))
    ));
    let error = missing.unwrap_err();
    assert!(!error.to_string().is_empty());
    assert!(std::error::Error::source(&error).is_some());

    let invalid_profile = ConfigDocument::new(
        "[local]\nstage_order = equalizer\n\
         [local broken]\nagc_enabled = yes\n\
         [usb]\nlocal_profile = broken\n",
    );
    let result = ResolvedChannelConfiguration::from_document(&invalid_profile, "radio.conf", "usb");
    assert!(matches!(
        result,
        Err(ChannelConfigurationError::Processing(_))
    ));
    let error = result.unwrap_err();
    assert!(!error.to_string().is_empty());
    assert!(std::error::Error::source(&error).is_some());
}
