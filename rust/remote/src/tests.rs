use super::*;
use std::mem::{offset_of, size_of};

#[test]
fn remote_operating_state_projection_retains_split_and_signal_ownership() {
    use crate::configuration::{operating_configuration, operating_state};
    use usbradioplus_core::{RemoteOperatingState, RemoteSignal, RemoteSignalSource};
    let state = RemoteOperatingState {
        receive_hz: 146520000,
        transmit_hz: Some(146520000),
        split: true,
        rit_enabled: true,
        rit_hz: 125,
        mode: 1,
        encode: RemoteSignal {
            enabled: true,
            source: RemoteSignalSource::Dsp,
            tone_tenths_hz: 1148,
            ..Default::default()
        },
        ..Default::default()
    };
    let caps = SelectedCapabilities {
        encode_sources: 2,
        ..Default::default()
    };
    let mut projected = operating_state(&state, &caps).unwrap();
    assert_eq!(projected.flags, 1 | 8 | 16);
    assert_eq!(operating_configuration(&projected).unwrap(), state);
    projected.flags |= 64;
    assert!(operating_configuration(&projected).is_err());
}

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
    let mut rig = dummy_rig();
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

fn dummy_rig() -> HamlibRig {
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
    unsafe { HamlibRig::open(provider, &config) }.unwrap()
}

#[test]
fn hamlib_operating_bridge_applies_complete_state_and_reports_real_readback() {
    let mut bridge = HamlibOperating::new(dummy_rig()).unwrap();
    // SAFETY: the complete operating record contains only scalar fields.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.receive_hz = 146520000;
    state.transmit_hz = state.receive_hz;
    state.flags = 1;
    state.mode = 1;
    state.encode.source = 1;
    state.decode.source = 1;
    bridge.apply(&state, || false).unwrap();
    let readback = bridge.read(&state, || false).unwrap();
    assert_eq!(readback.receive_hz, state.receive_hz);
    assert_eq!(readback.mode, 1);
    assert!(readback.bandwidth_hz > 0);
    assert_ne!(bridge.readback_fields() & 1, 0);
    state.flags |= 8;
    state.receive_vfo = 1;
    state.transmit_vfo = 2;
    state.transmit_hz = 146940000;
    bridge.apply(&state, || false).unwrap();
    let readback = bridge.read(&state, || false).unwrap();
    assert_eq!(readback.transmit_hz, state.transmit_hz);
    assert_ne!(readback.flags & 8, 0);
}

#[test]
fn hamlib_incremental_offsets_are_observed_separately_from_enable_state() {
    let mut bridge = HamlibOperating::new(dummy_rig()).unwrap();
    // SAFETY: all fields are fixed-width scalar state.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.receive_hz = 146520000;
    state.transmit_hz = state.receive_hz;
    state.flags = 1 | 16;
    state.mode = 1;
    state.rit_hz = 125;
    state.xit_hz = -250;
    bridge.apply(&state, || false).unwrap();
    let observed = bridge.read(&state, || false).unwrap();
    assert_eq!((observed.rit_hz, observed.xit_hz), (125, -250));
    assert_eq!(observed.flags & 48, 16);
    assert_eq!(bridge.rig().get(307), Ok(1));
    assert_eq!(bridge.rig().get(308), Ok(0));
    assert_eq!(bridge.readback_fields() & (3 << 14), 3 << 14);
}

#[test]
fn hamlib_operating_bridge_rejects_unsupported_signaling_before_retune() {
    let mut bridge = HamlibOperating::new(dummy_rig()).unwrap();
    bridge.rig().set(1, 146520000).unwrap();
    // SAFETY: the complete operating record contains only scalar fields.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.receive_hz = 146940000;
    state.transmit_hz = state.receive_hz;
    state.mode = 1;
    state.flags = 1;
    state.encode.source = 3;
    state.encode.enabled = 1;
    state.encode.dcs_code = 0o023;
    assert_eq!(bridge.apply(&state, || false), Err(CallError(-6)));
    assert_eq!(bridge.rig().get(1), Ok(146520000));
    state.encode.enabled = 0;
    state.encode.source = 0;
    assert_eq!(bridge.apply(&state, || true), Err(CallError(-10)));
    assert_eq!(bridge.rig().get(1), Ok(146520000));
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
    let mut backend = OperatingFixture {
        state,
        keyed: false,
        tune: false,
        fail: false,
    };
    let activity = raw::RptAdvRemoteActivity {
        struct_size: size_of::<raw::RptAdvRemoteActivity>() as u32,
        flags: 0,
    };
    session.advance_with(0, &activity, &mut backend).unwrap();
    assert!(!backend.keyed);
    assert_eq!(backend.state.receive_hz, state.receive_hz);
    assert_eq!(
        session.configured_memory(1).unwrap().receive_hz,
        state.receive_hz
    );
    assert!(session.snapshot().unwrap().flags & raw::RPTADV_REMOTE_APPLIED != 0);
    assert!(session.take_completion().unwrap().is_none());
    assert!(session.scan_snapshot().unwrap().is_none());
    // SAFETY: request and nested state contain only scalar fields.
    let mut request: raw::RptAdvRemoteRequest = unsafe { std::mem::zeroed() };
    request.struct_size = size_of::<raw::RptAdvRemoteRequest>() as u32;
    request.abi_version = 1;
    request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_RECALL;
    request.selector = 1;
    let id = session.request(&request, 1).unwrap();
    backend.fail = true;
    assert_eq!(
        session.advance_with(1, &activity, &mut backend),
        Err(CallError(-1003))
    );
    assert_eq!(session.snapshot().unwrap().failure_code, -1003);
    let completion = session.take_completion().unwrap().unwrap();
    assert_eq!((completion.request_id, completion.result), (id, -8));
    backend.fail = false;
    session.retire().unwrap();
    assert_eq!(
        session.advance_with(1, &activity, &mut backend),
        Err(CallError(-10))
    );
    assert!(session.configured_memory(99).is_err());
}

struct OperatingFixture {
    state: raw::RptAdvRemoteState,
    keyed: bool,
    tune: bool,
    fail: bool,
}
impl OperatingIo for OperatingFixture {
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
        Ok(self.state)
    }
    fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        assert!(!self.fail, "controlled dependency panic");
        self.state = *state;
        Ok(())
    }
    fn set_ptt(&mut self, keyed: bool) -> Result<(), CallError> {
        self.keyed = keyed;
        Ok(())
    }
    fn set_tune(&mut self, enabled: bool) -> Result<(), CallError> {
        self.tune = enabled;
        Ok(())
    }
    fn monotonic_ms(&self) -> u64 {
        0
    }
    fn transmit_epoch(&self) -> u64 {
        0
    }
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
    let mut rig = HamlibRig::configured(
        std::sync::Arc::new(HamlibProvider::load().unwrap()),
        &config,
    )
    .unwrap();
    rig.set(1, 146_520_000).unwrap();
    assert_eq!(rig.get(1), Ok(146_520_000));
    assert_eq!(
        rig.transport_budget_ms(),
        Ok(u64::from(config.timeout_ms)
            * u64::from(config.retries + 1)
            * u64::from(config.timeout_retries + 1))
    );
    rig.close().unwrap();
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
