//! Native maintenance C-contract checks using the existing station/provider fixture.

use super::*;
use std::collections::{BTreeMap, BTreeSet};

fn maintenance_station(eeprom_enabled: bool) -> *mut c_void {
    let raw = radio();
    let mut config = request(&raw);
    let source: &[u8] = if eeprom_enabled {
        b"[524950]\n[radio]\nhardware_eeprom_enabled=yes\n"
    } else {
        b"[524950]\n[radio]\nhardware_eeprom_enabled=no\n"
    };
    config.processing_configuration = source.as_ptr();
    config.processing_configuration_length = source.len() as u32;
    config.processing_node = b"524950".as_ptr();
    config.processing_node_length = 6;
    let (status, handle) = create(&config);
    assert_eq!(status, URP_AST_OK);
    handle
}

fn command_request(command: u32, target: u32, value: i64) -> UrpAstChannelCommand {
    UrpAstChannelCommand {
        struct_size: size_of::<UrpAstChannelCommand>() as u32,
        abi_version: ABI_VERSION,
        command,
        target,
        value,
        flags: 0,
        eeprom_words: [0; 64],
    }
}

unsafe extern "C" fn append_text(context: *mut c_void, text: *const u8, length: u32) {
    // SAFETY: the synchronous caller retains this output and the SDK's complete UTF-8 span.
    let (output, bytes) = unsafe {
        (
            &mut *context.cast::<String>(),
            std::slice::from_raw_parts(text, length as usize),
        )
    };
    output.push_str(std::str::from_utf8(bytes).unwrap());
}

#[test]
fn physical_status_text_matches_binary_values_and_the_public_measurement_keys() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let handle = maintenance_station(false);
    let api = product_descriptor();
    let mut physical = UrpNativePhysicalStatus {
        struct_size: size_of::<UrpNativePhysicalStatus>() as u32,
        abi_version: ABI_VERSION,
        ..UrpNativePhysicalStatus::default()
    };
    let mut text = String::new();
    // SAFETY: this single live station, complete output record and text context are retained.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_get_status.unwrap()(handle, &mut physical),
            URP_AST_OK
        );
        assert_eq!(
            api.native_status_text.unwrap()(
                handle,
                Some(append_text),
                ptr::from_mut(&mut text).cast()
            ),
            URP_AST_OK
        );
        api.native_destroy.unwrap()(handle);
    }
    assert_eq!(physical.running, 1);
    assert_eq!(physical.receive_input_peak, 0.5);
    assert_eq!(physical.transmit_output_rms, 0.125);
    assert_eq!(physical.input_latency_seconds, 0.01);
    assert_eq!(physical.output_latency_seconds, 0.02);
    assert_eq!(physical.native_sample_rate_hz, 48_000.0);
    assert_eq!(
        [
            physical.receive_mixer_level,
            physical.transmit_a_mixer_level,
            physical.transmit_b_mixer_level
        ],
        [750, 795, 245]
    );
    let values: BTreeMap<_, _> = text
        .lines()
        .map(|line| line.split_once('=').unwrap())
        .collect();
    let expected: BTreeSet<_> = [
        "running",
        "receive_input_peak",
        "receive_input_rms",
        "receive_output_peak",
        "receive_output_rms",
        "receive_input_rail_samples",
        "receive_output_rail_samples",
        "receive_ctcss_decoder_peak",
        "receive_rssi_peak",
        "receive_rssi_updated",
        "transmit_program_peak",
        "transmit_program_rms",
        "transmit_output_peak",
        "transmit_output_rms",
        "input_overflow_count",
        "output_underflow_count",
        "input_clip_sample_count",
        "output_clip_sample_count",
        "callback_last_duration_ns",
        "callback_max_duration_ns",
        "callback_last_start_delay_ns",
        "callback_max_start_delay_ns",
        "callback_late_start_count",
        "last_input_xrun_monotonic_ns",
        "last_output_xrun_monotonic_ns",
        "input_latency_seconds",
        "output_latency_seconds",
        "native_sample_rate_hz",
        "carrier_active",
        "subaudible_active",
        "receiver_keyed",
        "logical_ptt",
        "ctcss_decode_index",
        "dcs_valid",
        "receive_mixer_level",
        "transmit_a_mixer_level",
        "transmit_b_mixer_level",
    ]
    .into_iter()
    .collect();
    assert_eq!(text.lines().count(), 37);
    assert_eq!(values.keys().copied().collect::<BTreeSet<_>>(), expected);
    assert_eq!(
        values["receive_input_peak"].parse::<f32>().unwrap(),
        physical.receive_input_peak
    );
    assert_eq!(
        values["transmit_output_rms"].parse::<f32>().unwrap(),
        physical.transmit_output_rms
    );
    assert_eq!(
        values["input_latency_seconds"].parse::<f64>().unwrap(),
        physical.input_latency_seconds
    );
    assert_eq!(
        values["native_sample_rate_hz"].parse::<f64>().unwrap(),
        physical.native_sample_rate_hz
    );
    assert_eq!(values["ctcss_decode_index"], "-1");
    for (key, level) in [
        ("receive_mixer_level", 750),
        ("transmit_a_mixer_level", 795),
        ("transmit_b_mixer_level", 245),
    ] {
        assert_eq!(values[key].parse::<u32>().unwrap(), level);
    }
}

#[test]
fn physical_status_rejects_bad_outputs_and_reports_real_discovery_failure_without_partial_data() {
    #[repr(C, align(8))]
    struct ShortStatus {
        struct_size: u32,
        abi_version: u32,
    }
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let handle = maintenance_station(false);
    let api = product_descriptor();
    let mut physical = UrpNativePhysicalStatus {
        struct_size: size_of::<UrpNativePhysicalStatus>() as u32,
        abi_version: ABI_VERSION,
        running: 77,
        receive_mixer_level: 888,
        ..UrpNativePhysicalStatus::default()
    };
    let mut text = "unchanged".to_owned();
    // SAFETY: this station and every complete or explicitly short ABI output remain retained.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        fixture::set_failure(fixture::FAIL_MIXER_PATHS);
        assert_eq!(
            api.native_get_status.unwrap()(handle, &mut physical),
            URP_AST_SETUP_FAILED
        );
        assert_eq!((physical.running, physical.receive_mixer_level), (77, 888));
        assert_eq!(
            api.native_status_text.unwrap()(
                handle,
                Some(append_text),
                ptr::from_mut(&mut text).cast()
            ),
            URP_AST_SETUP_FAILED
        );
        assert_eq!(text, "unchanged");
        fixture::clear_failure();
        assert_eq!(
            api.native_get_status.unwrap()(handle, &mut physical),
            URP_AST_OK
        );
        assert_eq!(physical.receive_mixer_level, 750);
        let mut short = ShortStatus {
            struct_size: size_of::<ShortStatus>() as u32,
            abi_version: ABI_VERSION,
        };
        assert_eq!(
            api.native_get_status.unwrap()(handle, ptr::from_mut(&mut short).cast()),
            URP_AST_INCOMPATIBLE_ABI
        );
        assert_eq!((short.struct_size, short.abi_version), (8, ABI_VERSION));
        physical.abi_version = ABI_VERSION + 1;
        physical.running = 77;
        assert_eq!(
            api.native_get_status.unwrap()(handle, &mut physical),
            URP_AST_INCOMPATIBLE_ABI
        );
        assert_eq!(physical.running, 77);
        physical.abi_version = ABI_VERSION;
        assert_eq!(
            api.native_get_status.unwrap()(handle, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_get_status.unwrap()(ptr::null_mut(), &mut physical),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(physical.running, 77);
        assert_eq!(
            api.native_status_text.unwrap()(handle, None, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.native_status_text.unwrap()(
                ptr::null_mut(),
                Some(append_text),
                ptr::from_mut(&mut text).cast()
            ),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(text, "unchanged");
        assert_eq!(api.native_stop.unwrap()(handle), URP_AST_OK);
        assert_eq!(
            api.native_get_status.unwrap()(handle, &mut physical),
            URP_AST_OK
        );
        assert_eq!(physical.running, 0);
        api.native_destroy.unwrap()(handle);
    }
}

#[test]
fn typed_commands_validate_inputs_preserve_failures_and_share_actual_mixer_controls() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let handle = maintenance_station(false);
    let api = product_descriptor();
    // SAFETY: the unique started station and initialized mutable commands remain live.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        for (target, before, after) in [
            (URP_AST_MIXER_RECEIVE, 750, 222),
            (URP_AST_MIXER_TRANSMIT_A, 795, 333),
            (URP_AST_MIXER_TRANSMIT_B, 245, 444),
        ] {
            let mut command = command_request(URP_AST_COMMAND_GET_MIXER, target, -1);
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_OK
            );
            assert_eq!(command.value, before);
            command.command = URP_AST_COMMAND_SET_MIXER;
            command.value = after;
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_OK
            );
            command.command = URP_AST_COMMAND_GET_MIXER;
            command.value = -1;
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_OK
            );
            assert_eq!(command.value, after);
        }
        assert_eq!(fixture::mixer_levels(), [222, 333, 444]);
        for value in [-1, 1000, i64::MAX] {
            let mut command =
                command_request(URP_AST_COMMAND_SET_MIXER, URP_AST_MIXER_RECEIVE, value);
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_INVALID_ARGUMENT
            );
            assert_eq!(command.value, value);
            assert_eq!(fixture::mixer_levels(), [222, 333, 444]);
        }
        for operation in [
            URP_AST_COMMAND_SET_TEST_TONE,
            URP_AST_COMMAND_SET_CTCSS_INHIBIT,
            URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE,
        ] {
            for value in [1, 0, 2, -1] {
                let mut command = command_request(operation, 0, value);
                assert_eq!(
                    api.native_command.unwrap()(handle, &mut command),
                    if matches!(value, 0 | 1) {
                        URP_AST_OK
                    } else {
                        URP_AST_INVALID_ARGUMENT
                    }
                );
                assert_eq!(command.value, value);
            }
        }
        for (operation, target) in [
            (u32::MAX, 0),
            (URP_AST_COMMAND_GET_MIXER, u32::MAX),
            (URP_AST_COMMAND_SET_MIXER, u32::MAX),
        ] {
            let mut command = command_request(operation, target, 123);
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_INVALID_ARGUMENT
            );
            assert_eq!(command.value, 123);
        }
        for operation in [
            URP_AST_COMMAND_READ_EEPROM,
            URP_AST_COMMAND_WRITE_EEPROM,
            URP_AST_COMMAND_SAVE_TUNING_EEPROM,
        ] {
            let mut command = command_request(operation, 0, -123);
            command.flags = 0x80;
            command.eeprom_words = [7; 64];
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_SETUP_FAILED
            );
            assert_eq!(
                (command.value, command.flags, command.eeprom_words),
                (-123, 0x80, [7; 64])
            );
        }
        api.native_destroy.unwrap()(handle);
    }
}

#[test]
fn eeprom_commands_return_provider_results_and_keep_inputs_when_storage_fails() {
    let _guard = PROVIDER_TEST_LOCK.lock().unwrap();
    fixture::clear_failure();
    fixture::restored_mixer_levels([750, 795, 245]);
    let handle = maintenance_station(true);
    let api = product_descriptor();
    // SAFETY: this sole station and every complete mutable command are retained until destroy.
    unsafe {
        assert_eq!(api.native_start.unwrap()(handle), URP_AST_OK);
        let mut command = command_request(URP_AST_COMMAND_READ_EEPROM, 0, -1);
        assert_eq!(
            api.native_command.unwrap()(handle, &mut command),
            URP_AST_OK
        );
        assert_eq!(
            command.flags,
            URP_AST_EEPROM_CHECKSUM_VALID | URP_AST_EEPROM_MAGIC_VALID
        );
        assert_eq!(
            [
                command.eeprom_words[52],
                command.eeprom_words[53],
                command.eeprom_words[54],
                command.eeprom_words[59],
                command.eeprom_words[60]
            ],
            [321, 411, 512, 63, 654]
        );
        command.command = URP_AST_COMMAND_WRITE_EEPROM;
        command.flags = 0;
        command.eeprom_words[52] = 222;
        let written = command.eeprom_words;
        assert_eq!(
            api.native_command.unwrap()(handle, &mut command),
            URP_AST_OK
        );
        assert_eq!(
            command.flags,
            URP_AST_EEPROM_CHECKSUM_VALID | URP_AST_EEPROM_MAGIC_VALID
        );
        assert_eq!(command.eeprom_words, written);
        command.command = URP_AST_COMMAND_SAVE_TUNING_EEPROM;
        command.value = -1;
        assert_eq!(
            api.native_command.unwrap()(handle, &mut command),
            URP_AST_OK
        );
        assert_eq!(command.value, 1);
        fixture::set_failure(fixture::FAIL_EEPROM);
        for operation in [
            URP_AST_COMMAND_READ_EEPROM,
            URP_AST_COMMAND_WRITE_EEPROM,
            URP_AST_COMMAND_SAVE_TUNING_EEPROM,
        ] {
            let mut command = command_request(operation, 0, -123);
            command.flags = 0x80;
            command.eeprom_words = [7; 64];
            assert_eq!(
                api.native_command.unwrap()(handle, &mut command),
                URP_AST_SETUP_FAILED
            );
            assert_eq!(
                (command.value, command.flags, command.eeprom_words),
                (-123, 0x80, [7; 64])
            );
        }
        fixture::clear_failure();
        command.value = -1;
        assert_eq!(
            api.native_command.unwrap()(handle, &mut command),
            URP_AST_OK
        );
        assert_eq!(command.value, 1);
        api.native_destroy.unwrap()(handle);
    }
}
