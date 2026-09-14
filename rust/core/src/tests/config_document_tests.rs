use super::*;

const CONFIG: &str = "; retained comment\n\
[usb]\n\
hardware_profile = site\n\
\n\
[hardware]\n\
hardware_input_gain_db = 0.0\n\
hardware_output_a_gain_db = -1.0 ; retained suffix\n\
\n\
[hardware site]\n\
hardware_input_gain_db = 3.0\n\
\n\
[receive]\n\
audio_source = flat\n";

#[test]
fn channels_and_profiles_resolve_without_losing_flat_defaults() {
    let document = ConfigDocument::new(CONFIG);
    assert_eq!(document.configured_channels(), ["usb"]);
    assert_eq!(document.resolved_section("usb", "general").unwrap(), "usb");
    assert_eq!(
        document.resolved_section("usb", "hardware").unwrap(),
        "hardware site"
    );
    assert_eq!(
        document.resolved_section("usb", "receive").unwrap(),
        "receive"
    );
    let hardware = document.resolved_values("usb", "hardware").unwrap();
    assert_eq!(hardware["hardware_input_gain_db"], "3.0");
    assert_eq!(hardware["hardware_output_a_gain_db"], "-1.0");
}

#[test]
fn explicit_values_ignore_comments_and_keep_last_assignment() {
    let document = ConfigDocument::new(
        "[hardware]\n; gain = 7\ngain = 1 # note\ninvalid-name = 2\ngain = 3\n",
    );
    assert_eq!(
        document.explicit_values("HARDWARE"),
        BTreeMap::from([("gain".to_owned(), "3".to_owned())])
    );
}

#[test]
fn setting_and_removing_values_preserves_unrelated_text() {
    let mut document = ConfigDocument::new(CONFIG);
    document
        .set_value("hardware", "hardware_output_a_gain_db", "2.5")
        .unwrap();
    assert!(
        document
            .as_str()
            .contains("hardware_output_a_gain_db = 2.5\n")
    );
    assert!(document.as_str().contains("; retained comment\n"));
    document
        .set_value("hardware site", "hardware_serial", "CM119")
        .unwrap();
    assert!(
        document
            .as_str()
            .contains("hardware_input_gain_db = 3.0\n\nhardware_serial = CM119\n[receive]")
    );
    assert!(
        document
            .remove_value("hardware site", "hardware_serial")
            .unwrap()
    );
    assert!(
        !document
            .remove_value("hardware site", "hardware_serial")
            .unwrap()
    );
}

#[test]
fn missing_flat_and_scoped_sections_can_be_created_but_channels_cannot() {
    let mut document = ConfigDocument::new("[usb]\nchannel_enabled = yes");
    document.set_value("link", "chain_enabled", "no").unwrap();
    document
        .set_value("voice_telemetry site", "output_gain_db", "0.0")
        .unwrap();
    assert!(
        document
            .as_str()
            .contains("\n\n[link]\nchain_enabled = no\n")
    );
    assert!(document.as_str().contains("[voice_telemetry site]\n"));
    assert_eq!(
        document.set_value("missing", "channel_enabled", "yes"),
        Err(ConfigError::MissingChannel("missing".to_owned()))
    );
}

#[test]
fn invalid_names_values_and_missing_profiles_are_contextual() {
    let mut document = ConfigDocument::new(CONFIG);
    assert_eq!(
        document.set_value("", "gain", "0"),
        Err(ConfigError::InvalidSectionName)
    );
    assert_eq!(
        document.set_value("hardware", "bad-name", "0"),
        Err(ConfigError::InvalidOptionName)
    );
    assert_eq!(
        document.set_value("hardware", "gain", "0\n1"),
        Err(ConfigError::InvalidValue)
    );
    assert_eq!(
        document.resolved_section("missing", "hardware"),
        Err(ConfigError::MissingChannel("missing".to_owned()))
    );
    let document = ConfigDocument::new("[usb]\nhardware_profile = absent\n");
    assert_eq!(
        document.resolved_section("usb", "hardware"),
        Err(ConfigError::MissingProfile("hardware absent".to_owned()))
    );
}

#[test]
fn windows_line_endings_are_retained_for_replaced_values() {
    let mut document = ConfigDocument::new("[hardware]\r\ngain = 0\r\n");
    document.set_value("hardware", "gain", "1").unwrap();
    assert_eq!(document.as_str(), "[hardware]\r\ngain = 1\r\n");

    let mut document = ConfigDocument::new("[hardware]\ngain = 0\n");
    document.set_value("hardware", "gain", "1").unwrap();
    assert_eq!(document.as_str(), "[hardware]\ngain = 1\n");

    let mut document = ConfigDocument::new("[hardware]\ngain = 0");
    document.set_value("hardware", "gain", "1").unwrap();
    assert_eq!(document.as_str(), "[hardware]\ngain = 1");
}

#[test]
fn all_document_boundaries_are_explicit_and_contextual() {
    let mut document = ConfigDocument::new("[usb]\nvalue = channel\n[hardware]\nvalue = flat\n");
    assert_eq!(
        document.resolved_values("usb", "general").unwrap(),
        BTreeMap::from([("value".to_owned(), "channel".to_owned())])
    );
    assert_eq!(
        document.resolved_values("usb", "hardware").unwrap(),
        BTreeMap::from([("value".to_owned(), "flat".to_owned())])
    );
    assert_eq!(
        document.resolved_section("]", "hardware"),
        Err(ConfigError::InvalidSectionName)
    );
    assert_eq!(
        document.resolved_section("usb", "]"),
        Err(ConfigError::InvalidSectionName)
    );
    assert_eq!(
        document.resolved_values("missing", "hardware"),
        Err(ConfigError::MissingChannel("missing".to_owned()))
    );

    document.set_value("usb", "added", "one").unwrap();
    assert!(document.as_str().contains("added = one\n["));

    let mut empty = ConfigDocument::default();
    empty.set_value("hardware", "gain", "0").unwrap();
    assert_eq!(empty.as_str(), "[hardware]\ngain = 0\n");

    let mut unterminated = ConfigDocument::new("; comment");
    unterminated.set_value("hardware", "gain", "0").unwrap();
    assert!(unterminated.as_str().starts_with("; comment\n\n[hardware]"));

    let mut separated = ConfigDocument::new("; comment\n\n");
    separated.set_value("hardware", "gain", "0").unwrap();
    assert_eq!(separated.as_str(), "; comment\n\n[hardware]\ngain = 0\n");

    for error in [
        ConfigError::InvalidSectionName,
        ConfigError::InvalidOptionName,
        ConfigError::InvalidValue,
        ConfigError::MissingChannel("usb".to_owned()),
        ConfigError::MissingProfile("hardware site".to_owned()),
    ] {
        assert!(!error.to_string().is_empty());
    }
    assert_eq!(
        document.remove_value("]", "gain"),
        Err(ConfigError::InvalidSectionName)
    );
    assert_eq!(
        document.remove_value("hardware", ""),
        Err(ConfigError::InvalidOptionName)
    );
    assert!(ConfigDocument::default().section_names().is_empty());
    assert!(section_header("[").is_none());
    assert!(section_header("[]").is_none());
}

#[test]
fn unknown_scoped_sections_are_reported_without_confusing_named_channels() {
    let document = ConfigDocument::new(
        "[usb]\n[hardware site]\n[mystery site]\n[MYSTERY other]\n[local warm]\n",
    );
    assert_eq!(
        document.unknown_scoped_sections(),
        ["mystery site", "MYSTERY other"]
    );
    assert_eq!(document.configured_channels(), ["usb"]);
}
