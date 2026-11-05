use crate::media::tests::prepared;
use usbradioplus_core::{RemoteSignal, RemoteSignalSource as Source};
use usbradioplus_radio::{ReceiveSignaling, SubaudibleSource, TransmitSignaling};

fn state() -> usbradioplus_core::RemoteOperatingState {
    usbradioplus_core::RemoteOperatingState {
        mode: 1,
        encode: RemoteSignal {
            source: Source::Dsp,
            enabled: true,
            tone_tenths_hz: 1148,
            ..RemoteSignal::default()
        },
        decode: RemoteSignal {
            source: Source::Dsp,
            enabled: true,
            tone_tenths_hz: 1000,
            ..RemoteSignal::default()
        },
        ..Default::default()
    }
}

#[test]
fn remote_reload_keeps_operating_signaling_and_adopts_new_audio_gains() {
    use usbradioplus_core::{ConfigDocument, ResolvedRemoteConfiguration};
    let (original, _) = prepared(crate::ControllerTransport::RptAdvanced, 4).into_parts();
    let document = ConfigDocument::new(
        "[remote_base usb]\nenabled=yes\nbackend=parallel_channel\naudio_source=alsa\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory usb local]\nselector=1\nchannel=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n",
    );
    let definition = ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "usb",
        &original.configuration().station,
    )
    .unwrap()
    .unwrap();
    let initial = original.with_remote_configuration(Some(definition));
    assert!(initial.hardware().cm119.is_none());
    let current = initial
        .clone()
        .with_remote_carrier(usbradioplus_core::CarrierSource::External)
        .with_remote_signaling(&state(), SubaudibleSource::Disabled)
        .unwrap();
    let mut changed = initial.configuration().clone();
    changed.station.hardware.output_a_gain_db += 3.0;
    let next = initial
        .clone()
        .with_radio_configuration(changed, *initial.radio())
        .retain_remote_operating(&current)
        .unwrap();
    assert_eq!(
        next.radio().receive.signaling,
        current.radio().receive.signaling
    );
    assert_eq!(next.radio().qualification, current.radio().qualification);
    assert_eq!(
        next.configuration().station.hardware.output_a_gain_db,
        initial.configuration().station.hardware.output_a_gain_db + 3.0
    );
    assert!(next.remote_configuration().is_some());
}

#[test]
fn remote_signaling_prepares_independent_dsp_directions_and_preserves_levels() {
    let (original, _) = prepared(crate::ControllerTransport::RptAdvanced, 4).into_parts();
    let updated = original
        .clone()
        .with_remote_signaling(&state(), SubaudibleSource::Disabled)
        .unwrap();
    let ReceiveSignaling::Ctcss(receive) = updated.radio().receive.signaling else {
        panic!("missing decoder")
    };
    let TransmitSignaling::Ctcss(transmit) = updated.radio().transmit.signaling else {
        panic!("missing encoder")
    };
    assert_ne!(receive.tones.bits(), 0);
    assert_eq!(transmit.default_frequency_tenths_hz, 1148);
    assert_eq!(
        transmit.peak,
        crate::db_to_linear(original.configuration().station.ctcss.transmit_peak_dbfs)
    );
    assert_eq!(
        updated.radio().receive_input_gain,
        original.radio().receive_input_gain
    );
    assert_eq!(
        updated.radio().transmit.output_a,
        original.radio().transmit.output_a
    );
    assert_eq!(
        updated.radio().transmit.output_b,
        original.radio().transmit.output_b
    );
    assert_eq!(updated.processing().local, original.processing().local);
    assert_eq!(
        updated.processing().voice_telemetry,
        original.processing().voice_telemetry
    );
    assert_eq!(updated.hardware(), original.hardware());
    assert_eq!(
        updated.radio().generation_id,
        original.radio().generation_id + 1
    );
}

#[test]
fn remote_signaling_never_duplicates_rig_tones_or_fabricates_external_decode() {
    let (original, _) = prepared(crate::ControllerTransport::AppRpt, 4).into_parts();
    let mut requested = state();
    requested.encode.source = Source::Rig;
    requested.decode.source = Source::External;
    requested.decode.tone_tenths_hz = 0;
    requested.decode.dcs_code = 0o23;
    let updated = original
        .clone()
        .with_remote_signaling(&requested, SubaudibleSource::Parallel)
        .unwrap();
    assert_eq!(
        updated.radio().receive.signaling,
        ReceiveSignaling::Disabled
    );
    assert_eq!(
        updated.radio().transmit.signaling,
        TransmitSignaling::Disabled
    );
    assert_eq!(
        updated.radio().qualification.subaudible_source,
        SubaudibleSource::Parallel
    );
    assert!(
        original
            .clone()
            .with_remote_signaling(&requested, SubaudibleSource::Disabled)
            .is_err()
    );
    requested.decode.source = Source::Rig;
    assert!(
        original
            .with_remote_signaling(&requested, SubaudibleSource::Parallel)
            .is_err()
    );
}

#[test]
fn remote_signaling_mode_changes_preserve_profiles_and_conditioning_corners() {
    let (original, _) = prepared(crate::ControllerTransport::RptAdvanced, 4).into_parts();
    let mut requested = state();
    requested.mode = 3;
    requested.encode.enabled = false;
    requested.decode.enabled = false;
    let updated = original
        .clone()
        .with_remote_signaling(&requested, SubaudibleSource::Disabled)
        .unwrap();
    assert!(!updated.processing().receive_deemphasis);
    assert!(!updated.processing().transmit_preemphasis);
    assert!(
        !updated.radio().qualification.advanced_transport,
        "non-FM half-duplex receive must close during local transmission"
    );
    assert!(!updated.radio().qualification.radio_duplex);
    assert_eq!(
        updated.processing().receive_deemphasis_corner_hz,
        original.processing().receive_deemphasis_corner_hz
    );
    assert_eq!(
        updated.processing().transmit_preemphasis_corner_hz,
        original.processing().transmit_preemphasis_corner_hz
    );
    assert_eq!(
        updated.processing().receive_graph,
        original.processing().receive_graph
    );
    assert_eq!(
        updated.processing().transmit_graph,
        original.processing().transmit_graph
    );
    assert_eq!(
        updated.radio().receive.signaling,
        ReceiveSignaling::Disabled
    );
    assert_eq!(
        updated.radio().transmit.signaling,
        TransmitSignaling::Disabled
    );
    let restored = updated
        .with_remote_signaling(&state(), SubaudibleSource::Disabled)
        .unwrap();
    assert!(restored.radio().qualification.advanced_transport);
    let mut full_radio = *original.radio();
    full_radio.qualification.radio_duplex = true;
    let full = original
        .clone()
        .with_radio_configuration(original.configuration().clone(), full_radio)
        .with_remote_signaling(&requested, SubaudibleSource::Disabled)
        .unwrap();
    assert!(full.radio().qualification.radio_duplex);
    assert_eq!(
        restored.processing().receive_deemphasis,
        original.configuration().station.receive.audio_source
            == usbradioplus_core::ReceiveAudioSource::Flat
    );
    assert_eq!(
        restored.processing().transmit_preemphasis,
        original
            .configuration()
            .station
            .transmit
            .preemphasis_enabled
    );
}
