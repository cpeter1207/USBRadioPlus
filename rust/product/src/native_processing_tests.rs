use super::*;
use crate::{URP_AST_LINK_DIRECTION_READ, URP_AST_LINK_DIRECTION_WRITE, URP_AST_OK};
use std::ffi::c_void;

fn args(configuration: &str) -> UrpNativeProcessingArgs {
    UrpNativeProcessingArgs {
        struct_size: std::mem::size_of::<UrpNativeProcessingArgs>() as u32,
        abi_version: 1,
        configuration: configuration.as_ptr(),
        configuration_length: configuration.len() as u32,
        node: b"524950".as_ptr(),
        node_length: 6,
    }
}

#[test]
fn absent_roles_preserve_raw_graphs_and_explicit_roles_share_usb_resolution() {
    let empty = Profiles::resolve("", "").unwrap();
    assert!(empty.local.is_none());
    assert!(empty.link.is_none());
    assert!(empty.voice_telemetry.is_none());
    let configuration = "[524950]\nlink_profile=network\n[local]\ninput_gain_db=-3\n[local 524950]\noutput_gain_db=-2\n[link]\nlimiter_enabled=yes\nlimiter_bands=3\nlimiter_low_threshold_dbfs=-10\nlimiter_mid_threshold_dbfs=-10\nlimiter_high_threshold_dbfs=-10\n[link network]\noutput_gain_db=0\n[voice_telemetry]\ninput_gain_db=6\n";
    let profiles = Profiles::resolve(configuration, "524950").unwrap();
    let document = ConfigDocument::new(configuration);
    for (role, actual) in [
        (ChainRole::LocalReceive, profiles.local),
        (ChainRole::Link, profiles.link),
        (ChainRole::VoiceTelemetry, profiles.voice_telemetry),
    ] {
        assert_eq!(
            actual.unwrap(),
            ResolvedProfile::from_document(&document, SOURCE, "524950", role)
                .unwrap()
                .into_chain()
        );
    }
    assert!(profiles.warnings.is_empty());
    let other_node =
        Profiles::resolve("[524950]\n[link 999]\nlimiter_enabled=yes\n", "524950").unwrap();
    assert!(other_node.link.is_none());
}

#[test]
fn native_profile_gains_replace_the_raw_path_without_double_application() {
    let mut radio = SessionConfig::new(1, 960, 960);
    radio.receive_input_gain = 0.125;
    let mut processing = ExplicitNativeProcessingPlan {
        local: None,
        voice_telemetry: None,
        receive_input_gain: 1.0,
        receive_graph: "volume=-6dB".to_owned(),
        transmit_graph: "volume=6dB".to_owned(),
        receive_deemphasis: true,
        receive_deemphasis_corner_hz: 300.0,
        transmit_preemphasis: false,
        transmit_preemphasis_corner_hz: 300.0,
        receive_output_gain_db: -9,
        receive_ctcss_mask: 0,
        transmit_dcs: false,
    };
    Profiles::resolve("", "")
        .unwrap()
        .apply(&mut radio, &mut processing);
    assert_eq!(radio.receive_input_gain, 0.125);
    assert_eq!(processing.receive_input_gain, radio.receive_input_gain);
    assert!(processing.local.is_none());
    let configuration = "[524950]\n[local]\ninput_gain_db=-6\noutput_gain_db=-3\nlimiter_enabled=invalid\n[voice_telemetry]\ninput_gain_db=2\n";
    Profiles::resolve(configuration, "524950")
        .unwrap()
        .apply(&mut radio, &mut processing);
    assert!((radio.receive_input_gain - 10.0_f32.powf(-6.0 / 20.0)).abs() < 1e-6);
    assert_eq!(processing.receive_input_gain, radio.receive_input_gain);
    assert_eq!(processing.local.as_ref().unwrap().output_gain_db, -3.0);
    assert_eq!(
        processing.voice_telemetry.as_ref().unwrap().input_gain_db,
        2.0
    );
    radio.receive_input_gain = 0.0;
    Profiles::resolve("[524950]\n[local]\ninput_gain_db=6\n", "524950")
        .unwrap()
        .apply(&mut radio, &mut processing);
    assert_eq!(
        radio.receive_input_gain, 0.0,
        "disabled receive must stay muted"
    );
    assert_eq!(processing.receive_input_gain, 0.0);
}

#[test]
fn validation_reports_shared_fallback_diagnostics_and_rejects_unsafe_profiles() {
    unsafe extern "C" fn diagnostic(context: *mut c_void, text: *const u8, length: u32) {
        // SAFETY: validate supplies a readable UTF-8 diagnostic during this call.
        let value = unsafe { std::slice::from_raw_parts(text, length as usize) };
        // SAFETY: this test retains the exclusively borrowed vector through validate.
        let values = unsafe { &mut *context.cast::<Vec<String>>() };
        values.push(std::str::from_utf8(value).unwrap().to_owned());
    }
    let configuration =
        "[524950]\nlink_profile=missing\n[link]\nlimiter_enabled=invalid\nunknown_setting=1\n";
    let mut warnings = Vec::<String>::new();
    assert_eq!(
        // SAFETY: request strings and callback context remain valid for this call.
        unsafe {
            validate(
                &args(configuration),
                Some(diagnostic),
                std::ptr::from_mut(&mut warnings).cast(),
            )
        },
        URP_AST_OK
    );
    assert_eq!(warnings.len(), 3);
    assert!(warnings.iter().all(|value| value.contains(SOURCE)));
    assert_eq!(
        // SAFETY: this valid request is retained and intentionally has no diagnostic callback.
        unsafe { validate(&args(configuration), None, std::ptr::null_mut()) },
        URP_AST_OK
    );
    for configuration in [
        "[524950]\n[link]\nlimiter_low_crossover_hz=2000\nlimiter_high_crossover_hz=100\n",
        "[524950]\n[link]\ninput_gain_db=0\nINPUT_GAIN_DB=2\n",
        "[link]\nlimiter_enabled=yes\n",
    ] {
        assert_ne!(
            // SAFETY: each request owns readable UTF-8 strings and no callback is supplied.
            unsafe { validate(&args(configuration), None, std::ptr::null_mut()) },
            URP_AST_OK
        );
    }
}

#[test]
fn native_f32_link_uses_shared_preparation_counters_and_direction_rules() {
    let _guard = crate::tests::PROVIDER_TEST_LOCK.lock().unwrap();
    crate::tests::provider_support::clear_failure();
    let configuration = "[524950]\n[link]\nlimiter_enabled=yes\nlimiter_bands=3\nlimiter_low_threshold_dbfs=-10\nlimiter_mid_threshold_dbfs=-10\nlimiter_high_threshold_dbfs=-10\n";
    let api = crate::product_descriptor();
    let providers = crate::tests::provider_support::manifest();
    let mut output = std::ptr::dangling_mut();
    // SAFETY: requests, descriptor and output storage remain live; the graph is serially owned.
    unsafe {
        assert_eq!(
            link_prepare(&args(configuration), providers.ffmpeg, 960, &mut output),
            URP_AST_OK
        );
        assert!(!output.is_null());
        let mut samples = [0.125_f32, -0.25];
        assert_eq!(
            link_process_f32(
                output,
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                samples.as_mut_ptr(),
                2
            ),
            URP_AST_OK
        );
        assert_eq!(samples, [0.125, -0.25]);
        assert_eq!(
            link_process_f32(
                output,
                URP_AST_LINK_DIRECTION_WRITE,
                48_000,
                samples.as_mut_ptr(),
                2
            ),
            URP_AST_OK
        );
        let mut observation = crate::UrpAstLinkObservation {
            struct_size: std::mem::size_of::<crate::UrpAstLinkObservation>() as u32,
            abi_version: 1,
            ..Default::default()
        };
        assert_eq!(
            api.link_observe.unwrap()(output, &mut observation),
            URP_AST_OK
        );
        assert_eq!(observation.processed_blocks, 1);
        assert_eq!(observation.bypassed_blocks, 1);
        assert_ne!(
            link_process_f32(output, 99, 48_000, samples.as_mut_ptr(), 2),
            URP_AST_OK
        );
        assert_ne!(
            link_process_f32(
                output,
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                std::ptr::null_mut(),
                2
            ),
            URP_AST_OK
        );
        let storage = [0_u64; 2];
        assert_ne!(
            link_process_f32(
                output,
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                storage
                    .as_ptr()
                    .cast::<u8>()
                    .wrapping_add(1)
                    .cast_mut()
                    .cast(),
                1,
            ),
            URP_AST_OK
        );
        api.link_destroy.unwrap()(output);
        output = std::ptr::dangling_mut();
        assert_eq!(
            link_prepare(&args("[524950]\n"), providers.ffmpeg, 960, &mut output),
            URP_AST_OK
        );
        assert!(!output.is_null());
        samples = [0.125, -0.25];
        assert_eq!(
            link_process_f32(
                output,
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                samples.as_mut_ptr(),
                2
            ),
            URP_AST_OK
        );
        assert_eq!(samples, [0.125, -0.25]);
        assert_eq!(
            api.link_observe.unwrap()(output, &mut observation),
            URP_AST_OK
        );
        assert_ne!(observation.loudness_epoch, 0);
        assert_eq!(observation.loudness_sample_count, 2);
        assert_eq!(
            link_process_f32(
                output,
                crate::URP_AST_LINK_DIRECTION_SILENCE,
                48_000,
                samples.as_mut_ptr(),
                2
            ),
            URP_AST_OK
        );
        assert_eq!(samples, [0.0, 0.0]);
        assert_eq!(
            api.link_observe.unwrap()(output, &mut observation),
            URP_AST_OK
        );
        assert_eq!(observation.loudness_sample_count, 4);
        api.link_destroy.unwrap()(output);
    }
}

#[test]
fn processing_entry_points_reject_bad_requests_and_leave_no_graph_handle() {
    let configuration = "[524950]\n[link]\nlimiter_enabled=yes\n";
    let mut request = args(configuration);
    let mut output = std::ptr::dangling_mut();
    // SAFETY: deliberately null or malformed records retain readable headers and outputs.
    unsafe {
        assert_ne!(
            validate(std::ptr::null(), None, std::ptr::null_mut()),
            URP_AST_OK
        );
        request.abi_version = 2;
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request = args(configuration);
        request.struct_size = 8;
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request = args(configuration);
        request.configuration = std::ptr::null();
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request = args(configuration);
        request.configuration = b"\xff".as_ptr();
        request.configuration_length = 1;
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request.configuration = [0_u8].as_ptr();
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request = args(configuration);
        request.node = std::ptr::null();
        assert_ne!(validate(&request, None, std::ptr::null_mut()), URP_AST_OK);
        request = args(configuration);
        assert_ne!(
            link_prepare(&request, std::ptr::null(), 960, &mut output),
            URP_AST_OK
        );
        assert!(output.is_null());
        for frames in [0, 4097] {
            output = std::ptr::dangling_mut();
            assert_ne!(
                link_prepare(&request, std::ptr::null(), frames, &mut output),
                URP_AST_OK
            );
            assert!(output.is_null());
        }
        assert_ne!(
            link_prepare(&request, std::ptr::null(), 960, std::ptr::null_mut()),
            URP_AST_OK
        );
        assert_ne!(
            link_process_f32(
                std::ptr::null_mut(),
                URP_AST_LINK_DIRECTION_READ,
                48_000,
                std::ptr::null_mut(),
                0
            ),
            URP_AST_OK
        );
    }
}
