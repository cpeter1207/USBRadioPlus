use super::*;
use std::mem::{offset_of, size_of};

#[test]
fn remote_dynamic_client_admits_only_complete_current_contracts() {
    assert!(RemoteProvider::load_from("/missing-rptadv-fixture.so").is_err());
    let provider = RemoteProvider::load().unwrap();
    assert_eq!(provider.api().abi_version, 1);
    let good = *provider.api();
    for offset in [
        offset_of!(raw::RptAdvRemoteApi, create),
        offset_of!(raw::RptAdvRemoteApi, destroy),
        offset_of!(raw::RptAdvRemoteApi, configured_memory),
        offset_of!(raw::RptAdvRemoteApi, request),
        offset_of!(raw::RptAdvRemoteApi, advance),
        offset_of!(raw::RptAdvRemoteApi, snapshot),
        offset_of!(raw::RptAdvRemoteApi, take_completion),
        offset_of!(raw::RptAdvRemoteApi, retire),
        offset_of!(raw::RptAdvRemoteApi, scan_snapshot),
    ] {
        let mut bad = good;
        // SAFETY: each listed field is a nullable C function pointer in this owned record.
        unsafe {
            (&raw mut bad)
                .cast::<u8>()
                .add(offset)
                .cast::<usize>()
                .write(0);
        }
        // SAFETY: the owned record remains readable for descriptor admission.
        assert!(unsafe { admit_remote(&bad) }.is_err());
    }
    for (size, abi) in [
        (0, 1),
        (size_of::<raw::RptAdvRemoteApi>() as u32 - 1, 1),
        (good.struct_size, 2),
    ] {
        let mut bad = good;
        bad.struct_size = size;
        bad.abi_version = abi;
        // SAFETY: the complete record is readable even when its advertised prefix is rejected.
        assert!(unsafe { admit_remote(&bad) }.is_err());
    }
}

#[test]
fn hamlib_dynamic_client_preserves_its_separate_boundary() {
    assert!(HamlibProvider::load_from("/missing-hamlib-fixture.so").is_err());
    let provider = HamlibProvider::load().unwrap();
    let api = provider.api();
    assert_eq!(api.abi_version, 1);
    // SAFETY: complete admitted immutable descriptor remains loaded by provider.
    assert!(unsafe { api.model_count.unwrap()() } > 0);
    let mut bad = *api;
    bad.struct_size -= 1;
    // SAFETY: the complete owned record remains readable during admission.
    assert!(unsafe { admit_hamlib(&bad) }.is_err());
    bad = *api;
    bad.get = None;
    // SAFETY: the complete owned record remains readable during admission.
    assert!(unsafe { admit_hamlib(&bad) }.is_err());
}

#[test]
fn hamlib_owned_client_keeps_one_thread_and_complete_state_calls() {
    let provider = std::sync::Arc::new(HamlibProvider::load().unwrap());
    // SAFETY: the C configuration contains only scalar fields and nullable pointers.
    let mut config: raw::rptadv_hamlib_config = unsafe { std::mem::zeroed() };
    config.struct_size = size_of::<raw::rptadv_hamlib_config>();
    config.abi_version = 1;
    config.model = raw::rptadv_hamlib_string {
        data: b"RIG_MODEL_DUMMY".as_ptr().cast(),
        size: 15,
    };
    config.timeout_ms = 100;
    // SAFETY: all configuration spans remain readable through the synchronous open.
    let mut rig = unsafe { HamlibRig::open(provider, &config) }.unwrap();
    rig.set(1, 146_520_000).unwrap();
    rig.set_mode_width(3, 2400).unwrap();
    rig.set_split_vfos(1, true, 2).unwrap();
    rig.set_power_mw(5000, 146_520_000, 1).unwrap();
    assert_eq!(rig.get(1), Ok(146_520_000));
    assert_eq!(rig.get(3), Ok(3));
    assert!(
        rig.settings()
            .unwrap()
            .iter()
            .any(|setting| setting.selector == 1)
    );
    assert!(!rig.coverage(false).unwrap().is_empty());
    assert_eq!(rig.transport_budget_ms(), Ok(100));
    let settings = rig.settings().unwrap();
    let af = settings
        .iter()
        .find(|setting| setting.selector == 100)
        .unwrap();
    assert_eq!((af.value_kind, af.units, af.decimal_places), (4, 6, 6));
    let tone = settings
        .iter()
        .find(|setting| setting.selector == 12)
        .unwrap();
    assert_eq!(
        (tone.value_kind, tone.units, tone.decimal_places),
        (4, 1, 1)
    );
    rig.close().unwrap();
    assert!(rig.get(1).is_err());
}

#[test]
fn remote_owned_client_copies_setup_and_destroys_before_unloading_provider() {
    let provider = std::sync::Arc::new(RemoteProvider::load().unwrap());
    let ranges = [raw::RptAdvRemoteRange {
        minimum: 144_000_000,
        maximum: 148_000_000,
    }];
    let coverage = [raw::RptAdvRemoteCoverage {
        minimum: 144_000_000,
        maximum: 148_000_000,
        mode_mask: 2,
        flags: 0,
        minimum_power_mw: 0,
        maximum_power_mw: 0,
    }];
    // SAFETY: state has only scalar fields and zero selects disabled signaling.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.receive_hz = 146_520_000;
    state.transmit_hz = state.receive_hz;
    state.flags = 1;
    state.mode = 1;
    let memory = [raw::RptAdvRemoteMemory {
        selector: 1,
        name: b"local".as_ptr().cast(),
        name_length: 5,
        state,
    }];
    // SAFETY: all remaining setup fields are nullable spans or scalars.
    let mut setup: raw::RptAdvRemoteSetup = unsafe { std::mem::zeroed() };
    setup.struct_size = size_of::<raw::RptAdvRemoteSetup>() as u32;
    setup.abi_version = 1;
    setup.limits.receive = raw::RptAdvRemoteRanges {
        data: ranges.as_ptr(),
        length: 1,
    };
    setup.limits.transmit = setup.limits.receive;
    setup.limits.mode_mask = 2;
    setup.capabilities.receive = raw::RptAdvRemoteCoverages {
        data: coverage.as_ptr(),
        length: 1,
    };
    setup.capabilities.transmit = setup.capabilities.receive;
    setup.capabilities.mode_mask = 2;
    setup.memories = raw::RptAdvRemoteMemories {
        data: memory.as_ptr(),
        length: 1,
    };
    setup.startup_memory = 1;
    // SAFETY: every setup span remains readable through synchronous copying.
    let mut session = unsafe { RemoteSession::open(provider, &setup) }.unwrap();
    assert_eq!(
        session.configured_memory(1).unwrap().receive_hz,
        state.receive_hz
    );
    assert!(session.snapshot().unwrap().flags & raw::RPTADV_REMOTE_PENDING != 0);
    assert!(session.take_completion().unwrap().is_none());
    assert!(session.scan_snapshot().unwrap().is_none());
    session.retire().unwrap();
    assert!(session.configured_memory(99).is_err());
}

#[test]
fn configured_remote_client_resolves_available_signal_providers_once() {
    use usbradioplus_core::{ConfigDocument, ResolvedRemoteConfiguration, StationConfig};
    let document = ConfigDocument::new(
        "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=one\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 one]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\nctcss_encode_source=auto\nctcss_encode_hz=114.8\nctcss_decode_source=auto\nctcss_decode_hz=114.8\n",
    );
    let config = ResolvedRemoteConfiguration::from_document(
        &document,
        "test",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap();
    let coverage = raw::RptAdvRemoteCoverage {
        minimum: 144_000_000,
        maximum: 148_000_000,
        mode_mask: 2,
        flags: 0,
        minimum_power_mw: 0,
        maximum_power_mw: 0,
    };
    let caps = SelectedCapabilities {
        receive: vec![coverage],
        transmit: vec![coverage],
        modes: 2,
        encode_sources: (1 << 1) | (1 << 3),
        decode_sources: 1 << 1,
        decode_control_sources: (1 << 1) | (1 << 3),
        ..SelectedCapabilities::default()
    };
    let session = RemoteSession::configured(
        std::sync::Arc::new(RemoteProvider::load().unwrap()),
        &config,
        &caps,
    )
    .unwrap();
    let memory = session.configured_memory(1).unwrap();
    assert_eq!(memory.encode.source, 3);
    assert_eq!(
        memory.decode.source, 1,
        "Hamlib busy indication is not qualified tone decode"
    );
    assert_eq!(memory.encode.ctcss_tenths_hz, 1148);
    assert_eq!(
        config.memories[0].state.encode.source,
        usbradioplus_core::RemoteSignalSource::Automatic
    );
}
