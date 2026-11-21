//! Regression coverage at the typed preparation descriptor boundary.
use super::*;

struct Owner(*mut c_void);
impl Drop for Owner {
    fn drop(&mut self) {
        // SAFETY: successful preparation transfers this unique live owner.
        unsafe { crate::native_configuration::destroy(self.0) }
    }
}
impl Owner {
    fn same(&self, other: &Self) -> bool {
        let mut result = 2;
        assert_eq!(
            // SAFETY: both owners and the writable result stay live throughout this call.
            unsafe { (crate::preparation::descriptor().equal)(self.0, other.0, &mut result) },
            0
        );
        result == 1
    }
}

fn prepare(
    raw: &RawSessionConfig,
    dto: &[u8],
    configure: impl FnOnce(&mut UrpNativeStationConfig),
) -> Result<Owner, (c_int, String)> {
    prepare_with(
        crate::usbradioplus_preparation_descriptor(),
        raw,
        dto,
        configure,
    )
}

fn prepare_with(
    descriptor: *const crate::UrpPreparationDescriptor,
    raw: &RawSessionConfig,
    dto: &[u8],
    configure: impl FnOnce(&mut UrpNativeStationConfig),
) -> Result<Owner, (c_int, String)> {
    unsafe extern "C" fn diagnostic(context: *mut c_void, field: *const u8, length: u32) {
        // SAFETY: prepare lends the field and this caller retains its exclusive string.
        unsafe { *context.cast::<String>() = crate::native::text(field, length).unwrap() };
    }
    let mut field = String::new();
    let mut station = request(raw);
    station.receive_graph_length = 0;
    station.transmit_graph_length = 0;
    configure(&mut station);
    let args = crate::preparation::UrpTypedPreparation {
        struct_size: size_of::<crate::preparation::UrpTypedPreparation>() as u32,
        abi_version: 1,
        station: &station,
        settings: dto.as_ptr(),
        settings_length: dto.len() as u32,
    };
    let mut owner = ptr::null_mut();
    // SAFETY: the readable template and all borrowed spans stay live until return.
    let status = unsafe {
        ((*descriptor).prepare)(
            &args,
            Some(diagnostic),
            ptr::from_mut(&mut field).cast(),
            &mut owner,
        )
    };
    if status == 0 {
        assert!(!owner.is_null());
        Ok(Owner(owner))
    } else {
        assert!(owner.is_null());
        Err((status, field))
    }
}

fn standalone_radio() -> RawSessionConfig {
    let mut raw = radio();
    raw.receive.vad_external_delay = 1;
    raw
}

#[link(name = "rptadvradio")]
unsafe extern "C" {
    fn rptadv_radio_descriptor() -> *const c_void;
}

#[test]
fn inactive_vox_controls_are_canonical_before_real_software_radio_admission() {
    let raw = standalone_radio();
    let plain = prepare(&raw, b"{}", |_| {}).unwrap();
    let mut legacy = raw;
    legacy.receive.vox_threshold = i32::MAX;
    legacy.receive.vox_hang_ms = -1;
    let candidate = prepare(&legacy, b"{}", |_| {}).unwrap();
    assert!(plain.same(&candidate));
    // SAFETY: the test retains the candidate and the linked immutable provider throughout preparation.
    let (radio, provider) = unsafe {
        (
            &crate::native_configuration::prepared(candidate.0)
                .unwrap()
                .radio,
            usbradioplus_radio::RadioProvider::from_raw_descriptor(rptadv_radio_descriptor())
                .unwrap(),
        )
    };
    provider
        .prepare(radio, usbradioplus_radio::SessionPorts::default())
        .unwrap();
}

#[test]
fn native_delay_and_retired_cpu_overrides_never_reach_radio_owner() {
    for (external, frames, rx_cpu, tx_cpu, controller) in [
        (1, 1, 0, 0, 1),
        (0, 48000, 0, 0, 1),
        (0, 0, 0, 0, 1),
        (1, 0, 1, 0, 1),
        (1, 0, 0, 1, 1),
        (1, 0, 0, 0, 0),
    ] {
        let mut raw = standalone_radio();
        raw.qualification.advanced_transport = controller;
        raw.receive.vad_external_delay = external;
        raw.receive.native_squelch_delay_frames = frames;
        raw.receive.cpu_saver_enabled = rx_cpu;
        raw.transmit.cpu_saver_enabled = tx_cpu;
        assert!(
            prepare(&raw, b"{}", |_| {}).is_err(),
            "{external}/{frames}/{rx_cpu}/{tx_cpu}"
        );
    }
}
#[test]
fn explicit_local_profile_overrides_fallback_gain_in_equality() {
    for (dto, equal) in [
        (br#"{"local":{"output_gain_db":-2}}"#.as_slice(), true),
        (b"{}".as_slice(), false),
    ] {
        let raw = standalone_radio();
        let a = prepare(&raw, dto, |r| r.receive_output_gain_db = 0).unwrap();
        let b = prepare(&raw, dto, |r| r.receive_output_gain_db = -10).unwrap();
        assert_eq!(a.same(&b), equal);
    }
}
#[test]
fn exact_selector_uses_the_resolved_topology_identity() {
    let raw = standalone_radio();
    let dto = br#"{"station":{"gpio_usb_port_path":"1-5"}}"#;
    let topology = prepare(&raw, dto, |r| r.device_selection = 0).unwrap();
    let named = prepare(&raw, dto, |r| {
        r.device_selection = 0;
        r.device_identifier = b"1-5".as_ptr();
        r.device_identifier_length = 3;
    })
    .unwrap();
    assert!(topology.same(&named));
    assert!(prepare(&raw, b"{}", |r| r.device_selection = 0).is_err());
}
#[test]
fn present_role_scalars_and_groups_reject_null() {
    let raw = standalone_radio();
    for dto in [
        br#"{"local":{"enabled":null}}"#.as_slice(),
        br#"{"local":{"input_gain_db":null}}"#,
        br#"{"link":{"receive":{"bandpass_enabled":null}}}"#,
        br#"{"voice_telemetry":{"compressor":null}}"#,
        br#"{"station":{"channel_enabled":null}}"#,
    ] {
        assert!(
            prepare(&raw, dto, |_| {}).is_err(),
            "{}",
            std::str::from_utf8(dto).unwrap()
        );
    }
    prepare(
        &raw,
        br#"{"local":{},"station":{"parallel_port":{"input_assignments":[null,null,null,null]}}}"#,
        |_| {},
    )
    .unwrap();
}

#[path = "radio_bounds.rs"]
mod radio_bounds;
#[path = "settings_bounds.rs"]
mod settings_bounds;

#[test]
fn physical_preparation_keeps_legacy_api_and_operating_policy_independent() {
    let raw = standalone_radio();
    let table = crate::usbradioplus_physical_preparation_descriptor();
    let configure = |station: &mut UrpNativeStationConfig| {
        station.device_selection = 0;
        station.device_identifier = b"hw:3,0".as_ptr();
        station.device_identifier_length = 6;
    };
    let settings = br#"{"physical":{"ordinary":{"audio_source":"alsa","ptt_source":"parallel"},"parallel_carrier_inverted":false,"parallel_ctcss_inverted":false},"station":{"parallel_port":{"output_assignments":["push_to_talk",null,null,null,null,null,null,null]}}}"#;
    assert!(prepare(&raw, settings, configure).is_err());
    assert!(prepare_with(table, &raw, b"{}", configure).is_err());
    let owner = prepare_with(table, &raw, settings, configure).unwrap();
    // SAFETY: this unique immutable owner stays alive through the borrow.
    let request = unsafe { crate::native_configuration::prepared(owner.0).unwrap() };
    assert!(request.remote.is_none());
    assert!(request.hardware.cm119.is_none());
    assert_eq!(
        request.hardware.ordinary_ptt,
        usbradioplus_station::OrdinaryPtt::Parallel
    );
    assert!(prepare_with(table, &raw, settings, |_| {}).is_err());
    assert!(
        prepare_with(table, &raw, settings, |s| {
            configure(s);
            s.hardware_gain_mask = 1;
        })
        .is_err()
    );
    for name in [
        "hw:7",
        "hw:07,0",
        "default",
        "plughw:7,0",
        "hw:+7,0",
        "hw:7,",
        "hw:4294967296",
    ] {
        let candidate = prepare_with(table, &raw, settings, |s| {
            configure(s);
            s.device_identifier = name.as_ptr();
            s.device_identifier_length = name.len() as u32;
        });
        assert_eq!(
            candidate.is_ok(),
            matches!(name, "hw:7" | "hw:07,0"),
            "{name}"
        );
    }
    let mut conflicting = raw;
    conflicting.qualification.carrier_source = 4;
    assert!(prepare_with(table, &conflicting, settings, configure).is_err());
}

#[test]
fn physical_descriptor_checks_request_size_version_and_initializes_failures() {
    let table = crate::usbradioplus_physical_preparation_descriptor();
    for (size, version) in [(0, 1), (size_of::<crate::UrpTypedPreparation>() as u32, 2)] {
        let args = crate::UrpTypedPreparation {
            struct_size: size,
            abi_version: version,
            station: ptr::null(),
            settings: ptr::null(),
            settings_length: 0,
        };
        let mut output = ptr::dangling_mut::<c_void>();
        // SAFETY: a readable header and exclusive output are supplied; the rejected record has no spans.
        let status = unsafe { ((*table).prepare)(&args, None, ptr::null_mut(), &mut output) };
        assert_ne!(status, 0);
        assert!(output.is_null());
    }
}

#[test]
fn physical_inverted_chip_receive_requires_live_acquisition() {
    use usbradioplus_radio::{RadioProvider, SessionPorts};
    let settings = br#"{"physical":{"ordinary":{"audio_source":"cm119","ptt_source":"usb"},"parallel_carrier_inverted":false,"parallel_ctcss_inverted":false}}"#;
    for (carrier, tone) in [(4, 0), (8, 2)] {
        let mut raw = standalone_radio();
        raw.qualification.carrier_source = carrier;
        raw.qualification.subaudible_source = tone;
        let owner = prepare_with(
            crate::usbradioplus_physical_preparation_descriptor(),
            &raw,
            settings,
            |_| {},
        )
        .unwrap();
        // SAFETY: the typed candidate and linked radio descriptor remain live through the callback.
        let (request, provider) = unsafe {
            (
                crate::native_configuration::prepared(owner.0).unwrap(),
                RadioProvider::from_raw_descriptor(rptadv_radio_descriptor()).unwrap(),
            )
        };
        let (mut rx, _, _) = provider
            .prepare(&request.radio, SessionPorts::default())
            .unwrap()
            .split();
        let state = usbradioplus_station::SharedHardwareState::default();
        let first = rx
            .process(&[0.; 1920], &mut [0.; 960], state.receive_controls())
            .unwrap();
        assert!(
            !first.receiver_keyed,
            "receiver keyed before its first physical sample"
        );
        for online in [false, true, false, true] {
            let mut inputs = usbradioplus_station::HardwareInputs::default();
            inputs.observe_inverted_cm119(
                request.hardware.inverted_cm119,
                online.then_some([false, false]),
            );
            state.publish_sampled_inputs(inputs, std::time::Instant::now());
            let controls = state.receive_controls();
            let r = rx.process(&[0.; 1920], &mut [0.; 960], controls).unwrap();
            assert_eq!(
                r.receiver_keyed, online,
                "carrier {carrier}, tone {tone}, online {online}"
            );
        }
    }
}
