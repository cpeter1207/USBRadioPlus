//! Focused typed-CLI tests.

use super::cli::*;
use crate::{
    URP_AST_CHANNEL_NOT_FOUND, URP_AST_COMMAND_READ_EEPROM, URP_AST_COMMAND_SET_TEST_TONE,
    URP_AST_COMMAND_WRITE_EEPROM, URP_AST_MIXER_RECEIVE, URP_AST_OK, URP_AST_SETUP_FAILED,
    UrpAstChannelCommand, UrpAstChannelStatus,
};

use std::cell::RefCell;
use std::collections::VecDeque;

#[derive(Default)]
struct Fixture {
    channels: Vec<String>,
    channels_result: i32,
    active: Option<String>,
    status: UrpAstChannelStatus,
    status_result: i32,
    select_result: i32,
    echo_result: i32,
    transmit_result: i32,
    command_result: i32,
    reload_result: i32,
    commands: Vec<UrpAstChannelCommand>,
    echo: Vec<bool>,
    transmit: Vec<(bool, u32)>,
    waits: Vec<u32>,
    wait_results: VecDeque<bool>,
    status_reads: usize,
    links: Vec<LinkStatus>,
    emitted: String,
    events: Vec<&'static str>,
}

impl Fixture {
    fn healthy() -> Self {
        Self {
            channels: vec!["alpha".into(), "bravo".into()],
            active: Some("alpha".into()),
            status: UrpAstChannelStatus {
                transport: 1,
                running: 1,
                echo_enabled: 1,
                dtmf_enabled: 1,
                receive_input_peak: 0.5,
                ring_ratio: 1.0,
                ctcss_decode_index: -1,
                ..UrpAstChannelStatus::default()
            },
            ..Self::default()
        }
    }
}

impl CliBackend for Fixture {
    fn configured_channels(&mut self) -> (Vec<String>, i32) {
        (self.channels.clone(), self.channels_result)
    }
    fn active_channel(&mut self) -> Result<String, i32> {
        self.active.clone().ok_or(URP_AST_CHANNEL_NOT_FOUND)
    }
    fn select_active(&mut self, name: &str) -> i32 {
        if self.select_result == URP_AST_OK {
            self.active = Some(name.into());
        }
        self.select_result
    }
    fn active_status(&mut self) -> Result<(String, UrpAstChannelStatus), i32> {
        self.status_reads += 1;
        if self.status_result == URP_AST_OK {
            self.active_channel().map(|name| (name, self.status))
        } else {
            Err(self.status_result)
        }
    }
    fn set_echo(&mut self, enabled: bool) -> i32 {
        self.echo.push(enabled);
        self.echo_result
    }
    fn set_transmit(&mut self, keyed: bool, ctcss_tenths_hz: u32) -> i32 {
        self.events.push(if keyed { "transmit on" } else { "transmit off" });
        self.transmit.push((keyed, ctcss_tenths_hz));
        self.transmit_result
    }
    fn channel_command(&mut self, command: &mut UrpAstChannelCommand) -> i32 {
        if command.command == URP_AST_COMMAND_SET_TEST_TONE {
            self.events.push(if command.value == 0 {
                "tone off"
            } else {
                "tone on"
            });
        }
        if command.command == URP_AST_COMMAND_READ_EEPROM {
            command.flags = 3;
            for (index, word) in command.eeprom_words.iter_mut().enumerate() {
                *word = index as u16;
            }
        }
        self.commands.push(*command);
        self.command_result
    }
    fn reload(&mut self) -> i32 {
        self.reload_result
    }
    fn link_status(&mut self) -> Vec<LinkStatus> {
        self.links.clone()
    }
    fn wait_timed_out(&mut self, milliseconds: u32) -> bool {
        self.waits.push(milliseconds);
        self.wait_results.pop_front().unwrap_or(true)
    }
    fn emit(&mut self, output: &str) {
        self.events.push("emit");
        self.emitted.push_str(output);
    }
}

fn run(fixture: &mut Fixture, command: CliCommand, args: &[&str]) -> CliResult {
    execute(fixture, command, args)
}

#[test]
fn generated_asterisk_cli_constants_match_the_supported_abi() {
    assert_eq!(crate::ffi::CLI_INIT, -2);
    assert_eq!(crate::ffi::CLI_GENERATE, -3);
    assert_eq!(crate::ffi::RESULT_SUCCESS, 0);
    assert_eq!(crate::ffi::RESULT_SHOWUSAGE, 1);
    assert_eq!(crate::ffi::RESULT_FAILURE, 2);
}

#[test]
fn empty_cli_argument_array_does_not_require_a_nonnull_pointer() {
    // SAFETY: a zero-length array has no backing storage to validate.
    let empty = unsafe { argument_pointers(std::ptr::null(), 0) }.unwrap();
    assert!(empty.is_empty());
    // SAFETY: these invalid shapes are rejected before any pointer is read.
    assert!(unsafe { argument_pointers(std::ptr::null(), 1) }.is_none());
    assert!(unsafe { argument_pointers(std::ptr::null(), -1) }.is_none());
    let pointer = std::ptr::null();
    // SAFETY: pointer is one initialized argv entry.
    let one = unsafe { argument_pointers(&raw const pointer, 1) }.unwrap();
    assert_eq!(one, [std::ptr::null()]);
}

#[test]
fn cli_registration_rejects_a_null_module_owner() {
    assert!(!registration_owner_is_valid(std::ptr::null_mut()));
    assert!(registration_owner_is_valid(
        std::ptr::dangling_mut::<crate::ffi::ast_module>()
    ));
}

#[test]
fn failed_bulk_registration_unregisters_before_releasing_storage() {
    struct Allocation<'a>(&'a RefCell<Vec<&'static str>>);

    impl Drop for Allocation<'_> {
        fn drop(&mut self) {
            self.0.borrow_mut().push("drop");
        }
    }

    let events = RefCell::new(Vec::new());
    let entries = Box::into_raw(Box::new(Allocation(&events)));
    // SAFETY: entries came from Box::into_raw and the test unregister closure
    // records completion without retaining the allocation.
    unsafe {
        rollback_registration(entries, |_| {
            events.borrow_mut().push("unregister");
        });
    }
    assert_eq!(*events.borrow(), ["unregister", "drop"]);
}

#[test]
fn command_definitions_preserve_every_spelling_usage_and_summary() {
    let actual = CLI_DEFINITIONS
        .iter()
        .map(|definition| (definition.command, definition.usage, definition.summary))
        .collect::<Vec<_>>();
    assert_eq!(
        actual,
        vec![
            (
                "radioplus active",
                "Usage: radioplus active [channel-name]\n",
                "Select the USBRadioPlus tuning channel"
            ),
            (
                "radioplus channel list",
                "Usage: radioplus channel list\n",
                "List configured USBRadioPlus channels"
            ),
            (
                "radioplus channel status",
                "Usage: radioplus channel status [follow]\n",
                "Show typed USBRadioPlus channel status"
            ),
            (
                "radioplus channel echo",
                "Usage: radioplus channel echo [0|1]\n",
                "Read or change channel echo"
            ),
            (
                "radioplus channel transmit",
                "Usage: radioplus channel transmit <0|1> [ctcss-tenths-hz]\n",
                "Key or unkey the channel transmitter"
            ),
            (
                "radioplus channel flash",
                "Usage: radioplus channel flash\n",
                "Flash the channel transmitter"
            ),
            (
                "radioplus channel command",
                CHANNEL_COMMAND_USAGE,
                "Run a typed USBRadioPlus hardware command"
            ),
            (
                "radioplus processing stats",
                "Usage: radioplus processing stats\n",
                "Show USBRadioPlus processing statistics"
            ),
            (
                "radioplus processing reload",
                "Usage: radioplus processing reload\n",
                "Reload USBRadioPlus configuration"
            ),
        ]
    );
}

#[test]
fn active_list_echo_and_transmit_keep_the_existing_contract() {
    let mut fixture = Fixture::healthy();
    assert_eq!(
        run(&mut fixture, CliCommand::Active, &["radioplus", "active"]),
        CliResult::success("Active USB Radio device is [alpha].\n")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::Active,
            &["radioplus", "active", "bravo"]
        ),
        CliResult::success("Active radio set to [bravo].\n")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelList,
            &["radioplus", "channel", "list"]
        ),
        CliResult::success("alpha,bravo\n")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelEcho,
            &["radioplus", "channel", "echo"]
        ),
        CliResult::success("echo_enabled=1\n")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelEcho,
            &["radioplus", "channel", "echo", "0"]
        ),
        CliResult::success("echo_enabled=0\n")
    );
    assert_eq!(fixture.echo, [false]);
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelTransmit,
            &["radioplus", "channel", "transmit", "1", "1230"]
        ),
        CliResult::success("transmit_keyed=1\nforced_ctcss_tenths_hz=1230\n")
    );
    assert_eq!(fixture.transmit, [(true, 1230)]);
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelTransmit,
            &["radioplus", "channel", "transmit", "0", "1"]
        )
        .disposition,
        CliDisposition::ShowUsage
    );
}

#[test]
fn channel_list_keeps_names_emitted_before_a_later_enumeration_failure() {
    let mut fixture = Fixture::healthy();
    fixture.channels_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelList,
            &["radioplus", "channel", "list"]
        ),
        CliResult::failure("alpha,bravo\n")
    );
}

#[test]
fn hardware_commands_validate_bounds_and_round_trip_complete_eeprom_images() {
    let mut fixture = Fixture::healthy();
    let words = (0_u16..64)
        .map(|word| word.to_string())
        .collect::<Vec<_>>()
        .join(",");
    let result = run(
        &mut fixture,
        CliCommand::ChannelCommand,
        &[
            "radioplus",
            "channel",
            "command",
            "eeprom-write",
            "3",
            &words,
        ],
    );
    assert_eq!(result.disposition, CliDisposition::Success);
    assert_eq!(result.output, format!("flags=3\nwords={words}\n"));
    let command = fixture.commands.last().unwrap();
    assert_eq!(command.command, URP_AST_COMMAND_WRITE_EEPROM);
    assert_eq!(command.eeprom_words[0], 0);
    assert_eq!(command.eeprom_words[63], 63);

    let result = run(
        &mut fixture,
        CliCommand::ChannelCommand,
        &["radioplus", "channel", "command", "get-mixer", "receive"],
    );
    assert_eq!(result, CliResult::success("value=0\n"));
    assert_eq!(
        fixture.commands.last().unwrap().target,
        URP_AST_MIXER_RECEIVE
    );

    for args in [
        vec![
            "radioplus",
            "channel",
            "command",
            "set-mixer",
            "receive",
            "1000",
        ],
        vec![
            "radioplus",
            "channel",
            "command",
            "eeprom-write",
            "4",
            &words,
        ],
        vec![
            "radioplus",
            "channel",
            "command",
            "eeprom-write",
            "3",
            "1,2",
        ],
        vec!["radioplus", "channel", "command", "test-tone", "2"],
    ] {
        assert_eq!(
            run(&mut fixture, CliCommand::ChannelCommand, &args).disposition,
            CliDisposition::ShowUsage
        );
    }
}

#[test]
fn status_and_follow_emit_every_machine_readable_key() {
    let mut fixture = Fixture::healthy();
    fixture.wait_results = [true, false].into();
    let result = run(
        &mut fixture,
        CliCommand::ChannelStatus,
        &["radioplus", "channel", "status", "follow"],
    );
    assert_eq!(result.disposition, CliDisposition::Success);
    assert_eq!(fixture.status_reads, 2);
    assert_eq!(fixture.waits, [1000, 1000]);
    assert!(result.output.is_empty());
    let keys = fixture
        .emitted
        .lines()
        .take(54)
        .map(|line| line.split_once('=').unwrap().0)
        .collect::<Vec<_>>();
    assert_eq!(keys, STATUS_KEYS);
    assert_eq!(fixture.emitted.matches("status_abi=").count(), 2);
}

#[test]
fn flash_uses_three_one_second_bursts_with_established_spacing_and_cleanup() {
    let mut fixture = Fixture::healthy();
    let result = run(
        &mut fixture,
        CliCommand::ChannelFlash,
        &["radioplus", "channel", "flash"],
    );
    assert_eq!(
        result,
        CliResult::success("USB Device Flash completed.\n")
    );
    assert_eq!(fixture.emitted, "USB Device Flash starting.\n");
    assert_eq!(fixture.events.first(), Some(&"emit"));
    assert_eq!(fixture.events.get(1), Some(&"tone on"));
    assert_eq!(fixture.waits, [1000, 1500, 1000, 1500, 1000]);
    assert_eq!(
        fixture.transmit,
        [
            (true, 0),
            (false, 0),
            (true, 0),
            (false, 0),
            (true, 0),
            (false, 0),
            (false, 0),
        ]
    );
    assert_eq!(
        fixture
            .commands
            .iter()
            .map(|command| (command.command, command.value))
            .collect::<Vec<_>>(),
        [
            (URP_AST_COMMAND_SET_TEST_TONE, 1),
            (URP_AST_COMMAND_SET_TEST_TONE, 0),
            (URP_AST_COMMAND_SET_TEST_TONE, 1),
            (URP_AST_COMMAND_SET_TEST_TONE, 0),
            (URP_AST_COMMAND_SET_TEST_TONE, 1),
            (URP_AST_COMMAND_SET_TEST_TONE, 0),
            (URP_AST_COMMAND_SET_TEST_TONE, 0),
        ]
    );
}

#[test]
fn reload_statistics_and_failures_preserve_operator_output() {
    let mut fixture = Fixture::healthy();
    fixture.links.push(LinkStatus {
        name: "IAX2/506316-1".into(),
        processed_blocks: 7,
        bypassed_blocks: 2,
        failed_blocks: 1,
    });
    let result = run(
        &mut fixture,
        CliCommand::ProcessingStats,
        &["radioplus", "processing", "stats"],
    );
    assert!(
        result
            .output
            .contains("IAX2/506316-1/link: processed=7 bypassed=2 failed=1 blocks\n")
    );

    fixture.reload_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ProcessingReload,
            &["radioplus", "processing", "reload"]
        ),
        CliResult::failure("Configuration reload failed; existing settings retained.\n")
    );
    fixture.command_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelCommand,
            &["radioplus", "channel", "command", "eeprom-read"]
        ),
        CliResult::failure("Channel command failed (-6).\n")
    );
    fixture.status_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelStatus,
            &["radioplus", "channel", "status"]
        ),
        CliResult::failure("Active channel status is unavailable.\n")
    );
}
