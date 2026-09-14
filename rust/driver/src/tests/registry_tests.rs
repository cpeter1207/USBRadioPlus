use super::*;

fn configuration(name: &str) -> DriverConfiguration {
    DriverConfiguration::parse("radio.conf", format!("[{name}]\n")).unwrap()
}

#[test]
fn replacement_is_complete_and_existing_snapshots_remain_valid() {
    let registry = ConfigurationRegistry::new(configuration("old"));
    let retained = registry.snapshot();

    let candidate = ConfigurationRegistry::prepare("radio.conf", "[new]\n").unwrap();

    assert!(registry.snapshot().channel("old").is_some());
    registry.publish(candidate);

    assert!(retained.channel("old").is_some());
    let active = registry.snapshot();
    assert!(active.channel("old").is_none());
    assert!(active.channel("new").is_some());
}

#[test]
fn rejected_candidate_does_not_change_the_active_generation() {
    let registry = ConfigurationRegistry::new(configuration("active"));

    assert_eq!(
        ConfigurationRegistry::prepare("radio.conf", "[hardware]\n"),
        Err(DriverConfigurationError::NoChannels)
    );
    assert!(registry.snapshot().channel("active").is_some());
}
