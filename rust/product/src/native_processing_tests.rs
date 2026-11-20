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
fn link_buffering_query_validates_output_before_reading_or_mutating_the_link() {
    let _guard = crate::tests::PROVIDER_TEST_LOCK.lock().unwrap();
    crate::tests::provider_support::clear_failure();
    let api = crate::product_descriptor();
    let providers = crate::tests::provider_support::manifest();
    let mut link = std::ptr::null_mut();
    // SAFETY: retained fake provider, valid request, and unique output owner.
    unsafe {
        assert_eq!(link_prepare(&args(""), providers.ffmpeg, 960, &mut link), 0);
        let query = api.link_programme_buffering.unwrap();
        assert_ne!(query(std::ptr::null_mut(), std::ptr::null_mut()), 0);
        let mut unaligned = [0x5a_u64; 10];
        let before = unaligned;
        assert_ne!(
            query(link, unaligned.as_mut_ptr().cast::<u8>().add(1).cast()),
            0
        );
        assert_eq!(unaligned, before);
        let mut output = usbradioplus_core::ProgrammeBuffering {
            flags: 0xffff,
            ..Default::default()
        };
        for defect in 0..4 {
            output.struct_size = if defect == 0 { 8 } else { 64 };
            output.abi_version = if defect == 1 { 2 } else { 1 };
            output.reserved = u32::from(defect == 2);
            let before = output;
            let pointer = if defect == 3 {
                std::ptr::null_mut()
            } else {
                link
            };
            assert_ne!(query(pointer, &mut output), 0);
            assert_eq!(output, before);
        }
        #[repr(C)]
        struct Larger {
            value: usbradioplus_core::ProgrammeBuffering,
            tail: u64,
        }
        let mut larger = Larger {
            value: Default::default(),
            tail: 0xface,
        };
        larger.value.struct_size = std::mem::size_of::<Larger>() as u32;
        assert_eq!(query(link, &mut larger.value), 0);
        assert_eq!(larger.tail, 0xface);
        assert_eq!(
            larger.value,
            usbradioplus_core::ProgrammeBuffering::default()
        );
        api.link_destroy.unwrap()(link);
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

#[test]
fn independent_link_role_retains_presence_effective_gains_and_negotiated_metering() {
    #[link(name = "rptadv_ffmpeg_adapter")]
    unsafe extern "C" {
        fn rptadv_ffmpeg_adapter_descriptor() -> *const c_void;
    }

    // SAFETY: the product and FFmpeg descriptors stay loaded through all destroys.
    // Empty, admitted and rejected spans live through their synchronous calls.
    unsafe {
        let api = &*crate::usbradioplus_link_preparation_descriptor();
        let product = crate::product_descriptor();
        let resolve = |text: &str| {
            let mut owner = std::ptr::null_mut();
            assert_eq!(
                (api.prepare)(
                    text.as_ptr(),
                    text.len() as u32,
                    None,
                    std::ptr::null_mut(),
                    &mut owner
                ),
                0
            );
            assert!(!owner.is_null());
            owner
        };
        let absent = resolve("{}");
        let shipped = resolve(r#"{"link":{}}"#);
        let disabled = resolve(r#"{"link":{"enabled":false}}"#);
        let attenuated = resolve(&format!(
            r#"{{"link":{{"enabled":false,"output_gain_db":{}}}}}"#,
            -6
        ));
        let same = resolve(r#"{"link":{"output_gain_db":-6.0,"enabled":false}}"#);
        for (left, right, expected) in [
            (absent, shipped, 0),
            (shipped, disabled, 0),
            (disabled, attenuated, 0),
            (attenuated, same, 1),
        ] {
            let mut equal = 2;
            assert_eq!((api.equal)(left, right, &mut equal), 0);
            assert_eq!(equal, expected);
        }

        let ffmpeg = rptadv_ffmpeg_adapter_descriptor();
        let mut peer = std::ptr::dangling_mut();
        for (rate, frames) in [(0, 160), (8_000, 0), (8_000, 4097)] {
            assert_ne!(
                (api.link_prepare)(absent, ffmpeg, rate, frames, &mut peer),
                0
            );
            assert!(peer.is_null());
        }
        for (owner, rate, reference) in [
            (absent, 8_000, 0.25_f32),
            (attenuated, 16_000, 0.25 * 10.0_f32.powf(-6.0 / 20.0)),
        ] {
            assert_eq!((api.link_prepare)(owner, ffmpeg, rate, 160, &mut peer), 0);
            (api.destroy)(owner);
            let mut pcm = [0.25_f32; 160];
            let process = product.link_process_f32.unwrap();
            assert_eq!(
                process(
                    peer,
                    URP_AST_LINK_DIRECTION_READ,
                    rate,
                    pcm.as_mut_ptr(),
                    160
                ),
                0
            );
            assert!((pcm[159] - reference).abs() < 0.0001, "{pcm:?}");
            let mut observation = crate::UrpAstLinkObservation {
                struct_size: size_of::<crate::UrpAstLinkObservation>() as u32,
                abi_version: 1,
                ..Default::default()
            };
            assert_eq!(product.link_observe.unwrap()(peer, &mut observation), 0);
            assert_eq!(observation.processed_blocks, 1);
            assert_eq!(observation.loudness_sample_count, 160);
            product.link_destroy.unwrap()(peer);
        }
        for owner in [shipped, disabled, same, std::ptr::null_mut()] {
            (api.destroy)(owner);
        }
    }
}

#[test]
fn independent_link_role_rejects_station_data_and_invalid_profiles_before_graph_creation() {
    unsafe extern "C" fn diagnostic(context: *mut c_void, text: *const u8, length: u32) {
        // SAFETY: the callback borrows its field bytes and exclusively owns its output for this call.
        unsafe {
            *context.cast::<Vec<u8>>() = std::slice::from_raw_parts(text, length as usize).into();
        }
    }
    // SAFETY: all provided spans are readable for their admitted length. Oversize
    // lengths must be rejected before reading. Results are writable and aligned.
    unsafe {
        let api = &*crate::usbradioplus_link_preparation_descriptor();
        let mut owner = std::ptr::dangling_mut();
        let mut field = Vec::<u8>::new();
        for text in [
            r#"{"station":{}}"#,
            r#"{"remote":null}"#,
            r#"{"local":{}}"#,
            r#"{"voice_telemetry":{}}"#,
            r#"{"link":null}"#,
            r#"{"link":{"enabled":null}}"#,
            r#"{"link":{"stage_order":["agc","agc"]}}"#,
            r#"{"link":{"output_gain_db":999}}"#,
            r#"{"link":{},"link":{}}"#,
            "[]",
        ] {
            field.clear();
            assert_ne!(
                (api.prepare)(
                    text.as_ptr(),
                    text.len() as u32,
                    Some(diagnostic),
                    std::ptr::from_mut(&mut field).cast(),
                    &mut owner
                ),
                0,
                "{text}"
            );
            assert!(owner.is_null());
            assert!(!field.is_empty());
        }
        for (pointer, length) in [
            (std::ptr::null(), 4),
            (b"{}".as_ptr(), 0),
            (b"{}".as_ptr(), 1_048_577),
            (b"\xff".as_ptr(), 1),
        ] {
            assert_ne!(
                (api.prepare)(pointer, length, None, std::ptr::null_mut(), &mut owner),
                0
            );
            assert!(owner.is_null());
        }
        assert_ne!(
            (api.prepare)(
                b"{}".as_ptr(),
                2,
                None,
                std::ptr::null_mut(),
                std::ptr::null_mut()
            ),
            0
        );
        let mut storage = [0_u64; 2];
        let misaligned = storage.as_mut_ptr().cast::<u8>().add(1);
        assert_ne!(
            (api.prepare)(
                b"{}".as_ptr(),
                2,
                None,
                std::ptr::null_mut(),
                misaligned.cast()
            ),
            0
        );
        let mut peer = std::ptr::null_mut();
        assert_ne!(
            (api.link_prepare)(misaligned.cast(), std::ptr::null(), 8000, 160, &mut peer),
            0
        );
        assert_ne!(
            (api.link_prepare)(owner, std::ptr::null(), 8000, 160, misaligned.cast()),
            0
        );
        assert_ne!((api.equal)(owner, owner, misaligned.cast()), 0);
        assert_eq!(storage, [0; 2]);
        let mut equal = 7;
        assert_ne!((api.equal)(owner, owner, &mut equal), 0);
        assert_eq!(equal, 7);
        assert_ne!((api.equal)(owner, owner, std::ptr::null_mut()), 0);
        let mut peer = std::ptr::dangling_mut();
        assert_ne!(
            (api.link_prepare)(owner, std::ptr::null(), 8_000, 160, &mut peer),
            0
        );
        assert!(peer.is_null());
    }
}
