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
        ((*crate::usbradioplus_preparation_descriptor()).prepare)(
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
