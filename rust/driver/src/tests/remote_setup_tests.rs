use super::*;
use crate::{DriverConfiguration, remote_request::tests::configuration};
use usbradioplus_core::{CarrierSource, RemoteBackend};

fn plan() -> usbradioplus_station::HardwarePlan {
    let config = DriverConfiguration::parse("test", "[usb]\n").unwrap();
    usbradioplus_station::hardware_plan(config.channel("usb").unwrap().config())
}
#[test]
fn remote_setup_admits_selected_libraries_before_creating_worker_contexts() {
    let config = configuration();
    let (setup, control) = RemoteSetup::prepare(
        config,
        plan(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    assert!(setup.hamlib.is_some());
    assert_eq!(setup.configuration().hamlib_model, "RIG_MODEL_DUMMY");
    fn send<T: Send>(_: &T) {}
    send(&setup);
    drop(control);
    let mut config = configuration();
    config.backend = RemoteBackend::ParallelChannel;
    config.hamlib_model = "unused model".into();
    let (setup, _control) = RemoteSetup::prepare(
        config,
        plan(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    assert!(setup.hamlib.is_none());
}
#[test]
fn remote_setup_rejects_unknown_enumerated_models_without_opening_devices() {
    let mut config = configuration();
    config.hamlib_model = "RIG_MODEL_NOT_REAL".into();
    assert!(matches!(
        RemoteSetup::prepare(
            config,
            plan(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default()
        ),
        Err(RemoteSetupError::Call(usbradioplus_remote::CallError(
            -1001
        )))
    ));
}
#[test]
fn remote_setup_generic_audio_has_no_cm119_owner() {
    let mut config = configuration();
    config.audio_source = usbradioplus_core::RemoteAudioSource::Alsa;
    let (setup, _control) = RemoteSetup::prepare(
        config,
        plan(),
        CarrierSource::Dsp,
        usbradioplus_radio::QualificationConfig::default(),
    )
    .unwrap();
    assert!(setup.hardware().cm119.is_none());
}

#[test]
fn remote_setup_errors_format_the_selected_cause_without_transport_options() {
    assert_eq!(
        RemoteSetupError::Provider(ProviderError::Incompatible).to_string(),
        "incompatible remote radio provider"
    );
    assert!(
        RemoteSetupError::Call(CallError(-1001))
            .to_string()
            .contains("-1001")
    );
}
