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
    assert!(
        bridge
            .settings()
            .iter()
            .any(|setting| { setting.selector == 1 && setting.readable && setting.writable })
    );
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
        "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nhamlib_option_ptt_type=None\nstartup_memory=one\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 one]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\nctcss_encode_source=auto\nctcss_encode_hz=114.8\nctcss_decode_source=auto\nctcss_decode_hz=114.8\n",
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
    assert!(
        rig.settings()
            .unwrap()
            .iter()
            .any(|setting| { setting.selector == 20 && !setting.readable })
    );
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

#[test]
fn provider_admission_rejects_every_missing_operation_and_bad_identity() {
    let remote = RemoteProvider::load().unwrap();
    let hamlib = HamlibProvider::load().unwrap();
    for offset in [
        offset_of!(raw::rptadv_hamlib_api, model_count),
        offset_of!(raw::rptadv_hamlib_api, model_name),
        offset_of!(raw::rptadv_hamlib_api, open),
        offset_of!(raw::rptadv_hamlib_api, close),
        offset_of!(raw::rptadv_hamlib_api, destroy),
        offset_of!(raw::rptadv_hamlib_api, setting),
        offset_of!(raw::rptadv_hamlib_api, choice),
        offset_of!(raw::rptadv_hamlib_api, get),
        offset_of!(raw::rptadv_hamlib_api, set),
        offset_of!(raw::rptadv_hamlib_api, transport_budget_ms),
        offset_of!(raw::rptadv_hamlib_api, range),
        offset_of!(raw::rptadv_hamlib_api, set_mode_width),
        offset_of!(raw::rptadv_hamlib_api, set_split_vfos),
        offset_of!(raw::rptadv_hamlib_api, set_power_mw),
        offset_of!(raw::rptadv_hamlib_api, set_split_mode_width),
        offset_of!(raw::rptadv_hamlib_api, get_power_mw),
    ] {
        let mut api = *hamlib.api();
        // SAFETY: each offset selects one nullable function pointer in this owned table.
        unsafe {
            (&raw mut api)
                .cast::<u8>()
                .add(offset)
                .cast::<usize>()
                .write(0)
        };
        // SAFETY: the complete descriptor remains readable for admission.
        assert!(unsafe { admit_hamlib(&api) }.is_err());
    }
    for name in [std::ptr::null(), c"wrong.provider".as_ptr()] {
        let mut api = *remote.api();
        api.capability_name = name;
        // SAFETY: both tested names are null or a static C string.
        assert!(unsafe { admit_remote(&api) }.is_err());
        let mut api = *hamlib.api();
        api.name = name;
        // SAFETY: both tested names are null or a static C string.
        assert!(unsafe { admit_hamlib(&api) }.is_err());
    }
    let mut api = *hamlib.api();
    api.abi_version += 1;
    // SAFETY: the complete descriptor is readable despite the unsupported version.
    assert!(unsafe { admit_hamlib(&api) }.is_err());
    // SAFETY: admission rejects null/misaligned addresses before dereferencing them.
    unsafe {
        assert!(admit_remote(std::ptr::null()).is_err());
        assert!(admit_remote(std::ptr::dangling::<u8>().cast()).is_err());
        assert!(admit_hamlib(std::ptr::null()).is_err());
        assert!(admit_hamlib(std::ptr::dangling::<u8>().cast()).is_err());
    }
    assert_eq!(
        ProviderError::Incompatible.to_string(),
        "incompatible remote radio provider"
    );
    let error = RemoteProvider::load_from("/missing-remote-provider.so")
        .err()
        .unwrap();
    assert!(!error.to_string().is_empty());
    assert_eq!(
        CallError(-42).to_string(),
        "remote radio operation failed (-42)"
    );
    assert!(hamlib.has_model("RIG_MODEL_DUMMY").unwrap());
    assert!(!hamlib.has_model("RIG_MODEL_NOT_INSTALLED").unwrap());
}

#[test]
fn state_projection_preserves_optional_fields_and_rejects_bad_provider_values() {
    use crate::configuration::{operating_configuration, operating_state};
    use usbradioplus_core::{RemoteOperatingState, RemoteSignal, RemoteSignalSource};
    let mut state = RemoteOperatingState {
        receive_hz: 146_520_000,
        transmit_hz: Some(146_520_000),
        power_mw: Some(5000),
        channel: Some(15),
        xit_enabled: true,
        encode: RemoteSignal {
            source: RemoteSignalSource::External,
            enabled: true,
            inverted: true,
            dcs_code: 0o245,
            ..Default::default()
        },
        decode: RemoteSignal {
            source: RemoteSignalSource::Rig,
            control_source: Some(RemoteSignalSource::External),
            enabled: true,
            tone_tenths_hz: 1148,
            ..Default::default()
        },
        ..Default::default()
    };
    let caps = SelectedCapabilities::default();
    let projected = operating_state(&state, &caps).unwrap();
    assert_eq!(operating_configuration(&projected).unwrap(), state);
    for mutate in [
        |s: &mut raw::RptAdvRemoteState| s.channel = 16,
        |s: &mut raw::RptAdvRemoteState| s.encode.source = 4,
        |s: &mut raw::RptAdvRemoteState| s.decode.control_source = 4,
        |s: &mut raw::RptAdvRemoteState| s.encode.enabled = 2,
        |s: &mut raw::RptAdvRemoteState| s.decode.inverted = 2,
    ] {
        let mut bad = projected;
        mutate(&mut bad);
        assert_eq!(operating_configuration(&bad).err(), Some(CallError(-1003)));
    }
    state.encode.source = RemoteSignalSource::Automatic;
    assert_eq!(operating_state(&state, &caps).err(), Some(CallError(-6)));
    state.encode.enabled = false;
    assert_eq!(operating_state(&state, &caps).unwrap().encode.source, 0);
    state.encode.source = RemoteSignalSource::External;
    state.decode.control_source = Some(RemoteSignalSource::Automatic);
    assert_eq!(operating_state(&state, &caps).err(), Some(CallError(-6)));
}

fn resolved_configuration() -> usbradioplus_core::ResolvedRemoteConfiguration {
    use usbradioplus_core::{ConfigDocument, ResolvedRemoteConfiguration, StationConfig};
    ResolvedRemoteConfiguration::from_document(
        &ConfigDocument::new(concat!(
            "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\n",
            "hamlib_model=RIG_MODEL_DUMMY\nstartup_memory=one\nallowed_modes=FM\n",
            "allowed_rx_ranges_hz=144000000-148000000\n",
            "allowed_tx_ranges_hz=144000000-148000000\n",
            "[remote_memory 524950 one]\nselector=1\nrx_frequency_hz=146520000\n",
            "tx_frequency_hz=146520000\nmode=FM\n"
        )),
        "fixture",
        "524950",
        &StationConfig::default(),
    )
    .unwrap()
    .unwrap()
}

fn selected_capabilities() -> SelectedCapabilities {
    let coverage = raw::RptAdvRemoteCoverage {
        minimum: 144_000_000,
        maximum: 148_000_000,
        mode_mask: 2,
        flags: 0,
        minimum_power_mw: 0,
        maximum_power_mw: 0,
    };
    SelectedCapabilities {
        receive: vec![coverage],
        transmit: vec![coverage],
        modes: 2,
        scan_modes: 2,
        manual_tune: true,
        encode_sources: 14,
        decode_sources: 14,
        decode_control_sources: 14,
        ..Default::default()
    }
}

#[test]
fn configured_client_copies_scan_definitions_and_validates_policy_before_open() {
    use usbradioplus_core::{RemoteScanList, RemoteSignalSource};
    let provider = std::sync::Arc::new(RemoteProvider::load().unwrap());
    let mut config = resolved_configuration();
    config.memories[0].state.encode.source = RemoteSignalSource::Dsp;
    config.memories[0].state.decode.source = RemoteSignalSource::Dsp;
    RemoteSession::validate_policy(std::sync::Arc::clone(&provider), &config).unwrap();
    config.memories[0].state.encode.source = RemoteSignalSource::Automatic;
    config.memories[0].state.decode.source = RemoteSignalSource::Automatic;
    config.memories[0].state.decode.control_source = Some(RemoteSignalSource::Automatic);
    config.scan_lists.push(RemoteScanList {
        selector: 1,
        name: "local".into(),
        members: vec![1],
        dwell_ms: 1000,
        resume_delay_ms: 100,
        settle_ms: 10,
        priority: Some(1),
    });
    RemoteSession::validate_policy(std::sync::Arc::clone(&provider), &config).unwrap();
    let mut session =
        RemoteSession::configured(provider, &config, &selected_capabilities()).unwrap();
    let mut backend = OperatingFixture {
        state: session.configured_memory(1).unwrap(),
        keyed: false,
        tune: false,
        fail: false,
    };
    let activity = raw::RptAdvRemoteActivity {
        struct_size: size_of::<raw::RptAdvRemoteActivity>() as u32,
        flags: 0,
    };
    session.advance_with(0, &activity, &mut backend).unwrap();
    // SAFETY: the request is a scalar-only C record.
    let mut request: raw::RptAdvRemoteRequest = unsafe { std::mem::zeroed() };
    request.struct_size = size_of::<raw::RptAdvRemoteRequest>() as u32;
    request.abi_version = 1;
    request.kind = raw::RptAdvRemoteRequestKind_RPTADV_REMOTE_SCAN_START;
    request.selector = 1;
    request.direction = 1;
    session.request(&request, 1).unwrap();
    session.advance_with(1, &activity, &mut backend).unwrap();
    assert_eq!(session.scan_snapshot().unwrap().unwrap().list, 1);
    config.modes = vec![64];
    assert_eq!(
        RemoteSession::configured(
            std::sync::Arc::new(RemoteProvider::load().unwrap()),
            &config,
            &selected_capabilities()
        )
        .err(),
        Some(CallError(-4))
    );
}

thread_local! {
    static CATALOG: std::cell::RefCell<Vec<(u32, u32)>> = const { std::cell::RefCell::new(Vec::new()) };
    static METADATA_FAULT: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    static GET_VALUES: std::cell::RefCell<std::collections::BTreeMap<u32, i64>> = const { std::cell::RefCell::new(std::collections::BTreeMap::new()) };
    static SET_VALUES: std::cell::RefCell<Vec<(u32, i64)>> = const { std::cell::RefCell::new(Vec::new()) };
    static GET_FAILURE: std::cell::Cell<Option<u32>> = const { std::cell::Cell::new(None) };
}

unsafe extern "C" fn catalog_setting(
    _: *mut std::ffi::c_void,
    index: usize,
    out: *mut raw::rptadv_hamlib_setting,
) -> i32 {
    if METADATA_FAULT.get() == 5 {
        return -71;
    }
    let Some((selector, access)) = CATALOG.with_borrow(|values| values.get(index).copied()) else {
        return 1;
    };
    // SAFETY: settings lends a complete writable record for each catalog item.
    let out = unsafe { &mut *out };
    out.selector = selector;
    out.access = access;
    out.name = raw::rptadv_hamlib_string {
        data: c"fixture".as_ptr(),
        size: 7,
    };
    out.value_kind = 3;
    out.minimum = i64::MIN;
    out.maximum = i64::MAX;
    match METADATA_FAULT.get() {
        1 => out.struct_size = 0,
        2 => out.name.data = std::ptr::null(),
        3 => out.access = 4,
        4 => {
            out.name = raw::rptadv_hamlib_string {
                data: b"\xff".as_ptr().cast(),
                size: 1,
            }
        }
        6 => out.choice_count = 1,
        _ => {}
    }
    0
}

unsafe extern "C" fn catalog_choice(
    _: *mut std::ffi::c_void,
    _: u32,
    _: usize,
    _: *mut i64,
) -> i32 {
    -72
}
unsafe extern "C" fn catalog_get(_: *mut std::ffi::c_void, selector: u32, out: *mut i64) -> i32 {
    if GET_FAILURE.get() == Some(selector) {
        return -73;
    }
    // SAFETY: the caller lends a complete writable scalar.
    unsafe {
        out.write(GET_VALUES.with_borrow(|values| values.get(&selector).copied().unwrap_or(0)))
    };
    0
}
unsafe extern "C" fn catalog_set(_: *mut std::ffi::c_void, selector: u32, value: i64) -> i32 {
    SET_VALUES.with_borrow_mut(|values| values.push((selector, value)));
    GET_VALUES.with_borrow_mut(|values| values.insert(selector, value));
    0
}
unsafe extern "C" fn catalog_power(_: *mut std::ffi::c_void, _: u64, _: i64, out: *mut u32) -> i32 {
    if GET_FAILURE.get() == Some(11) {
        return -73;
    }
    // SAFETY: the caller lends a complete writable scalar.
    unsafe { out.write(5000) };
    0
}
unsafe extern "C" fn invalid_range(
    _: *mut std::ffi::c_void,
    _: u32,
    _: usize,
    out: *mut raw::rptadv_hamlib_range,
) -> i32 {
    // SAFETY: coverage lends one complete writable range.
    unsafe { (*out).struct_size = 0 };
    0
}
unsafe extern "C" fn failed_range(
    _: *mut std::ffi::c_void,
    _: u32,
    _: usize,
    _: *mut raw::rptadv_hamlib_range,
) -> i32 {
    -74
}
unsafe extern "C" fn failed_model(_: usize, _: *mut raw::rptadv_hamlib_string) -> i32 {
    -75
}
unsafe extern "C" fn invalid_model(_: usize, out: *mut raw::rptadv_hamlib_string) -> i32 {
    // SAFETY: has_model lends a complete writable name span.
    unsafe {
        *out = raw::rptadv_hamlib_string {
            data: std::ptr::null(),
            size: 0,
        }
    };
    0
}
unsafe extern "C" fn oversized_model(_: usize, out: *mut raw::rptadv_hamlib_string) -> i32 {
    // SAFETY: has_model rejects the oversized span before reading its contents.
    unsafe {
        *out = raw::rptadv_hamlib_string {
            data: c"fixture".as_ptr(),
            size: isize::MAX as usize + 1,
        }
    };
    0
}

fn rig_with_catalog(
    settings: &[(u32, u32)],
    edit: impl FnOnce(&mut raw::rptadv_hamlib_api),
) -> HamlibRig {
    CATALOG.with_borrow_mut(|values| *values = settings.to_vec());
    METADATA_FAULT.set(0);
    GET_FAILURE.set(None);
    SET_VALUES.with_borrow_mut(Vec::clear);
    GET_VALUES.with_borrow_mut(|values| {
        *values = [
            (1, 146_520_000),
            (2, 146_940_000),
            (3, 1),
            (4, 12000),
            (5, 1),
            (6, 2),
            (7, 1),
            (8, 600_000),
            (9, 125),
            (10, -250),
            (22, 1),
            (23, 12000),
            (307, 1),
            (308, 1),
        ]
        .into_iter()
        .collect()
    });
    let mut provider = HamlibProvider::load().unwrap();
    provider.api.setting = Some(catalog_setting);
    provider.api.choice = Some(catalog_choice);
    provider.api.get = Some(catalog_get);
    provider.api.set = Some(catalog_set);
    provider.api.get_power_mw = Some(catalog_power);
    edit(&mut provider.api);
    // SAFETY: initialization contains only scalar fields and nullable spans.
    let mut config: raw::rptadv_hamlib_config = unsafe { std::mem::zeroed() };
    config.struct_size = size_of::<raw::rptadv_hamlib_config>();
    config.abi_version = 1;
    config.model = raw::rptadv_hamlib_string {
        data: c"RIG_MODEL_DUMMY".as_ptr(),
        size: 15,
    };
    config.timeout_ms = 100;
    // SAFETY: the real released Dummy opens/closes the context; only selected
    // catalog/getter functions are substituted by this thread-local provider fixture.
    unsafe { HamlibRig::open(std::sync::Arc::new(provider), &config) }.unwrap()
}

fn complete_catalog() -> Vec<(u32, u32)> {
    [
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 22, 23, 307, 308,
    ]
    .into_iter()
    .map(|selector| (selector, 3))
    .collect()
}

#[test]
fn dependency_catalog_and_model_contract_failures_preserve_the_first_status() {
    for fault in 1..=6 {
        let rig = rig_with_catalog(&[(1, 3)], |_| {});
        METADATA_FAULT.set(fault);
        let expected = match fault {
            5 => -71,
            6 => -72,
            _ => -1003,
        };
        assert_eq!(rig.settings().err(), Some(CallError(expected)));
    }
    let rig = rig_with_catalog(&[], |api| api.range = Some(invalid_range));
    assert_eq!(rig.coverage(false).err(), Some(CallError(-1003)));
    let rig = rig_with_catalog(&[], |api| api.range = Some(failed_range));
    assert_eq!(rig.coverage(true).err(), Some(CallError(-74)));
    let mut provider = HamlibProvider::load().unwrap();
    provider.api.model_name = Some(failed_model);
    assert_eq!(provider.has_model("fixture"), Err(CallError(-75)));
    provider.api.model_name = Some(invalid_model);
    assert_eq!(provider.has_model("fixture"), Err(CallError(-1003)));
    provider.api.model_name = Some(oversized_model);
    assert_eq!(provider.has_model("fixture"), Err(CallError(-1003)));
}

fn complete_state() -> raw::RptAdvRemoteState {
    // SAFETY: the operating record contains only scalar fields.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    state.receive_hz = 146_520_000;
    state.transmit_hz = 146_940_000;
    state.mode = 1;
    state.bandwidth_hz = 12000;
    state.receive_vfo = 1;
    state.transmit_vfo = 2;
    state.offset_hz = 600_000;
    state.rit_hz = 125;
    state.xit_hz = -250;
    state.power_mw = 5000;
    state.flags = 1 | 2 | 8 | 16 | 32;
    state.encode.source = 3;
    state.encode.enabled = 1;
    state.encode.ctcss_tenths_hz = 1148;
    state.decode.source = 2;
    state.decode.control_source = 3;
    state.decode.enabled = 1;
    state.decode.ctcss_tenths_hz = 1000;
    state
}

#[test]
fn operating_readback_uses_only_actual_complete_getters_and_rejects_bad_observations() {
    let mut bridge = HamlibOperating::new(rig_with_catalog(&complete_catalog(), |_| {})).unwrap();
    let baseline = complete_state();
    let observed = bridge.read(&baseline, || false).unwrap();
    assert_eq!(observed.power_mw, 5000);
    assert_eq!(observed.flags & (16 | 32), 16 | 32);
    assert_eq!(bridge.readback_fields() & 512, 512);
    for stop in 1..=15 {
        let mut calls = 0;
        assert_eq!(
            bridge
                .read(&baseline, || {
                    calls += 1;
                    calls == stop
                })
                .err(),
            Some(CallError(-10))
        );
    }
    for selector in [1, 2, 3, 4, 5, 6] {
        GET_VALUES.with_borrow_mut(|values| values.insert(selector, -1));
        assert_eq!(
            bridge.read(&baseline, || false).err(),
            Some(CallError(-1003))
        );
        GET_VALUES.with_borrow_mut(|values| {
            values.insert(
                selector,
                match selector {
                    1 => 146_520_000,
                    2 => 146_940_000,
                    4 => 12000,
                    6 => 2,
                    _ => 1,
                },
            )
        });
    }
    for selector in [22, 23] {
        GET_VALUES.with_borrow_mut(|values| values.insert(selector, -1));
        assert_eq!(bridge.read(&baseline, || false).err(), Some(CallError(-11)));
        GET_VALUES.with_borrow_mut(|values| {
            values.insert(selector, if selector == 22 { 1 } else { 12000 })
        });
    }
    for selector in [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 307, 308] {
        GET_FAILURE.set(Some(selector));
        assert_eq!(bridge.read(&baseline, || false).err(), Some(CallError(-73)));
    }
    GET_FAILURE.set(None);
    GET_VALUES.with_borrow_mut(|values| {
        values.insert(2, 1);
        values.insert(8, -2);
    });
    assert_eq!(
        bridge.read(&baseline, || false).err(),
        Some(CallError(-1003))
    );

    let mut bridge = HamlibOperating::new(rig_with_catalog(&[], |_| {})).unwrap();
    assert_eq!(bridge.readback_fields(), 0);
    assert_eq!(
        bridge.read(&baseline, || false).unwrap().receive_hz,
        baseline.receive_hz
    );
    let rig = rig_with_catalog(&[(1, 3)], |_| {});
    METADATA_FAULT.set(5);
    assert_eq!(
        HamlibOperating::new(rig).err().map(|error| error.0),
        Some(-71)
    );
}

#[test]
fn operating_apply_checks_independent_capabilities_before_any_write() {
    let state = complete_state();
    for required in [
        1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 16, 17, 22, 23, 307, 308,
    ] {
        let mut catalog = complete_catalog();
        catalog.retain(|(selector, _)| *selector != required);
        let mut bridge = HamlibOperating::new(rig_with_catalog(&catalog, |_| {})).unwrap();
        assert_eq!(
            bridge.apply(&state, || false),
            Err(CallError(-6)),
            "selector {required}"
        );
        SET_VALUES.with_borrow(|writes| assert!(writes.is_empty()));
    }
    let mut bridge = HamlibOperating::new(rig_with_catalog(&complete_catalog(), |_| {})).unwrap();
    bridge.apply(&state, || false).unwrap();
    SET_VALUES.with_borrow(|writes| {
        assert!(writes.contains(&(12, 1148)));
        assert!(writes.contains(&(13, 1000)));
        assert!(writes.contains(&(16, 1)));
        assert!(writes.contains(&(17, 1)));
    });
    for stop in 1..=16 {
        let mut calls = 0;
        assert_eq!(
            bridge.apply(&state, || {
                calls += 1;
                calls == stop
            }),
            Err(CallError(-10))
        );
    }
    let mut without_xit = state;
    without_xit.flags &= !32;
    bridge.apply(&without_xit, || false).unwrap();
    for mutate in [
        |s: &mut raw::RptAdvRemoteState| s.flags |= 4,
        |s: &mut raw::RptAdvRemoteState| s.flags &= !8,
        |s: &mut raw::RptAdvRemoteState| s.encode.inverted = 1,
    ] {
        let mut rejected = state;
        mutate(&mut rejected);
        assert_eq!(bridge.apply(&rejected, || false), Err(CallError(-6)));
    }
    let mut simple = state;
    simple.flags = 1;
    simple.receive_vfo = 0;
    simple.transmit_hz = simple.receive_hz;
    simple.offset_hz = 0;
    simple.rit_hz = 0;
    simple.xit_hz = 0;
    simple.encode = raw::RptAdvRemoteSignal {
        source: 1,
        control_source: 0,
        enabled: 0,
        ctcss_tenths_hz: 0,
        dcs_code: 0,
        inverted: 0,
    };
    simple.decode = simple.encode;
    let mut bridge =
        HamlibOperating::new(rig_with_catalog(&[(1, 3), (3, 3), (4, 3)], |_| {})).unwrap();
    bridge.apply(&simple, || false).unwrap();
    simple.flags = 0;
    bridge.apply(&simple, || false).unwrap();
    simple.flags = 1;
    let mut bridge = HamlibOperating::new(rig_with_catalog(&complete_catalog(), |_| {})).unwrap();
    simple.encode.source = 3;
    simple.decode.control_source = 2;
    bridge.apply(&simple, || false).unwrap();
    GET_VALUES.with_borrow_mut(|values| {
        values.insert(7, 0);
        values.insert(307, 0);
        values.insert(308, 0);
    });
    let observed = bridge.read(&simple, || false).unwrap();
    SET_VALUES.with_borrow_mut(Vec::clear);
    bridge.apply(&observed, || false).unwrap();
    SET_VALUES.with_borrow(|writes| {
        assert!(
            !writes
                .iter()
                .any(|(selector, _)| matches!(selector, 9 | 10))
        )
    });
}

thread_local! {
    static OPEN_STATUS: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    static OPEN_HANDLE: std::cell::Cell<bool> = const { std::cell::Cell::new(true) };
    static DESTROYED: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
    static BACKEND_RESULTS: std::cell::RefCell<Vec<i64>> = const { std::cell::RefCell::new(Vec::new()) };
}

unsafe fn probe_open(out: *mut *mut std::ffi::c_void) -> i32 {
    if OPEN_HANDLE.get() {
        // SAFETY: each open lends one writable handle slot and transfers this allocation.
        unsafe { out.write(Box::into_raw(Box::new(1_u8)).cast()) };
    }
    OPEN_STATUS.get()
}
unsafe extern "C" fn probe_hamlib_open(
    _: *const raw::rptadv_hamlib_config,
    out: *mut *mut std::ffi::c_void,
) -> i32 {
    // SAFETY: the wrapper supplies its unique writable handle slot.
    unsafe { probe_open(out) }
}
unsafe extern "C" fn probe_remote_open(
    _: *const raw::RptAdvRemoteSetup,
    out: *mut *mut std::ffi::c_void,
) -> i32 {
    // SAFETY: the wrapper supplies its unique writable handle slot.
    unsafe { probe_open(out) }
}
unsafe extern "C" fn probe_destroy(context: *mut std::ffi::c_void) {
    // SAFETY: probe_open transfers exactly this allocation to its matching destroy.
    drop(unsafe { Box::from_raw(context.cast::<u8>()) });
    DESTROYED.set(DESTROYED.get() + 1);
}

#[test]
fn transferred_partial_contexts_are_destroyed_and_successful_null_handles_are_rejected() {
    for (code, handle, expected) in [(-84, true, -84), (-85, false, -85), (0, false, -1003)] {
        OPEN_STATUS.set(code);
        OPEN_HANDLE.set(handle);
        DESTROYED.set(0);
        let mut provider = HamlibProvider::load().unwrap();
        provider.api.open = Some(probe_hamlib_open);
        provider.api.destroy = Some(probe_destroy);
        // SAFETY: scalar initialization supplies readable configuration storage.
        let config = unsafe { std::mem::zeroed() };
        // SAFETY: the fixture reads no borrowed spans and owns any transferred handle.
        let result = unsafe { HamlibRig::open(std::sync::Arc::new(provider), &config) };
        assert_eq!(result.err(), Some(CallError(expected)));
        assert_eq!(DESTROYED.get(), usize::from(handle));
        DESTROYED.set(0);
        let mut provider = RemoteProvider::load().unwrap();
        provider.api.create = Some(probe_remote_open);
        provider.api.destroy = Some(probe_destroy);
        // SAFETY: scalar initialization supplies readable setup storage.
        let setup = unsafe { std::mem::zeroed() };
        // SAFETY: the fixture reads no borrowed spans and owns any transferred handle.
        let result = unsafe { RemoteSession::open(std::sync::Arc::new(provider), &setup) };
        assert_eq!(result.err(), Some(CallError(expected)));
        assert_eq!(DESTROYED.get(), usize::from(handle));
    }
    assert!(RemoteProvider::load_from("libc.so.6").is_err());
    assert!(HamlibProvider::load_from("libc.so.6").is_err());
}

unsafe extern "C" fn probe_advance(
    _: *mut std::ffi::c_void,
    _: u64,
    _: *const raw::RptAdvRemoteActivity,
    backend: *const raw::RptAdvRemoteBackend,
) -> i32 {
    // SAFETY: advance_with lends this complete table for the synchronous call.
    let backend = unsafe { &*backend };
    // SAFETY: the operating record contains only scalar fields.
    let mut state: raw::RptAdvRemoteState = unsafe { std::mem::zeroed() };
    let invalid = std::ptr::dangling_mut::<u8>().cast();
    // SAFETY: valid storage is retained through every synchronous callback. Null,
    // misaligned and non-boolean values exercise the callback's public C rejection.
    let results = unsafe {
        vec![
            i64::from(backend.read_state.unwrap()(backend.context, &mut state)),
            i64::from(backend.apply_state.unwrap()(backend.context, &state)),
            i64::from(backend.set_ptt.unwrap()(backend.context, 1)),
            i64::from(backend.set_tune.unwrap()(backend.context, 1)),
            backend.monotonic_ms.unwrap()(backend.context) as i64,
            backend.transmit_epoch.unwrap()(backend.context) as i64,
            i64::from(backend.read_state.unwrap()(
                backend.context,
                std::ptr::null_mut(),
            )),
            i64::from(backend.read_state.unwrap()(backend.context, invalid)),
            i64::from(backend.apply_state.unwrap()(
                backend.context,
                std::ptr::null(),
            )),
            i64::from(backend.apply_state.unwrap()(backend.context, invalid)),
            i64::from(backend.set_ptt.unwrap()(backend.context, 2)),
            i64::from(backend.set_tune.unwrap()(backend.context, 2)),
            i64::from(backend.read_state.unwrap()(
                std::ptr::null_mut(),
                &mut state,
            )),
            i64::from(backend.read_state.unwrap()(invalid.cast(), &mut state)),
            backend.monotonic_ms.unwrap()(std::ptr::null_mut()) as i64,
            backend.transmit_epoch.unwrap()(invalid.cast()) as i64,
        ]
    };
    BACKEND_RESULTS.with_borrow_mut(|stored| *stored = results);
    0
}

struct FailedOperatingIo;
impl OperatingIo for FailedOperatingIo {
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
        Err(CallError(-21))
    }
    fn apply_state(&mut self, _: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        Err(CallError(-22))
    }
    fn set_ptt(&mut self, _: bool) -> Result<(), CallError> {
        Err(CallError(-23))
    }
    fn set_tune(&mut self, _: bool) -> Result<(), CallError> {
        Err(CallError(-24))
    }
    fn monotonic_ms(&self) -> u64 {
        42
    }
    fn transmit_epoch(&self) -> u64 {
        7
    }
}

#[test]
fn borrowed_operating_callbacks_preserve_errors_and_reject_invalid_c_arguments() {
    OPEN_STATUS.set(0);
    OPEN_HANDLE.set(true);
    let mut provider = RemoteProvider::load().unwrap();
    provider.api.create = Some(probe_remote_open);
    provider.api.destroy = Some(probe_destroy);
    provider.api.advance = Some(probe_advance);
    let mut session =
        // SAFETY: this fixture does not borrow setup spans or retain host callbacks.
        unsafe { RemoteSession::open(std::sync::Arc::new(provider), &std::mem::zeroed()) }.unwrap();
    let activity = raw::RptAdvRemoteActivity {
        struct_size: size_of::<raw::RptAdvRemoteActivity>() as u32,
        flags: 0,
    };
    session
        .advance_with(0, &activity, &mut FailedOperatingIo)
        .unwrap();
    BACKEND_RESULTS.with_borrow(|results| {
        assert_eq!(&results[..6], &[-21, -22, -23, -24, 42, 7]);
        assert_eq!(&results[6..14], &[-1003; 8]);
        assert_eq!(&results[14..], &[0, 0]);
    });
    let mut io = OperatingFixture {
        state: complete_state(),
        keyed: false,
        tune: false,
        fail: false,
    };
    session.advance_with(1, &activity, &mut io).unwrap();
    BACKEND_RESULTS.with_borrow(|results| assert_eq!(&results[..6], &[0; 6]));
    assert!(io.keyed && io.tune);
}
