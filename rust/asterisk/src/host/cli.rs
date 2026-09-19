//! Typed USBRadioPlus CLI parsing and output.

use std::ffi::{CStr, c_char, c_int};
use std::fmt::Write;
use std::mem::size_of;

use std::ffi::CString;
use std::ptr;
use std::sync::atomic::{AtomicPtr, Ordering};

use super::{channel, link, reload};
use crate::ffi;
use crate::{URP_AST_ASTERISK_FAILURE, URP_AST_CHANNEL_NOT_FOUND, URP_AST_NOT_READY};

use crate::{
    ABI_VERSION, URP_AST_COMMAND_GET_MIXER, URP_AST_COMMAND_READ_EEPROM,
    URP_AST_COMMAND_SAVE_TUNING_EEPROM, URP_AST_COMMAND_SET_CTCSS_INHIBIT,
    URP_AST_COMMAND_SET_MIXER, URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE,
    URP_AST_COMMAND_SET_TEST_TONE, URP_AST_COMMAND_WRITE_EEPROM, URP_AST_EEPROM_CHECKSUM_VALID,
    URP_AST_EEPROM_MAGIC_VALID, URP_AST_MIXER_RECEIVE, URP_AST_MIXER_TRANSMIT_A,
    URP_AST_MIXER_TRANSMIT_B, URP_AST_OK, URP_AST_SETUP_FAILED, UrpAstChannelCommand,
    UrpAstChannelStatus,
};

/// Complete usage text for the typed hardware-command entry.
pub(super) const CHANNEL_COMMAND_USAGE: &str = "Usage: radioplus channel command get-mixer <receive|transmit-a|transmit-b>\n\
       radioplus channel command set-mixer <receive|transmit-a|transmit-b> <0-999>\n\
       radioplus channel command test-tone <0|1>\n\
       radioplus channel command eeprom-read\n\
       radioplus channel command eeprom-write <flags> <64-comma-separated-words>\n\
       radioplus channel command eeprom-save-tuning\n\
       radioplus channel command ctcss-inhibit <0|1>\n\
       radioplus channel command subaudible-override <0|1>\n";

/// Stable command identity used by Asterisk callback wrappers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum CliCommand {
    /// Select or report the tuning channel.
    Active,
    /// List configured radio channels.
    ChannelList,
    /// Show one or continuous status snapshots.
    ChannelStatus,
    /// Read or change retained echo mode.
    ChannelEcho,
    /// Key or unkey the active transmitter.
    ChannelTransmit,
    /// Generate three calibration bursts.
    ChannelFlash,
    /// Execute one typed hardware primitive.
    ChannelCommand,
    /// Show channel and incoming-link processing statistics.
    ProcessingStats,
    /// Reload configuration in place.
    ProcessingReload,
}

/// Static Asterisk CLI registration metadata.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct CliDefinition {
    /// Stable command spelling.
    pub(super) command: &'static str,
    /// Usage returned for malformed arguments.
    pub(super) usage: &'static str,
    /// One-line Asterisk command summary.
    pub(super) summary: &'static str,
    /// Typed dispatch identity.
    pub(super) kind: CliCommand,
}

/// Complete typed command surface used by operators and the tune utility.
pub(super) const CLI_DEFINITIONS: [CliDefinition; 9] = [
    CliDefinition {
        command: "radioplus active",
        usage: "Usage: radioplus active [channel-name]\n",
        summary: "Select the USBRadioPlus tuning channel",
        kind: CliCommand::Active,
    },
    CliDefinition {
        command: "radioplus channel list",
        usage: "Usage: radioplus channel list\n",
        summary: "List configured USBRadioPlus channels",
        kind: CliCommand::ChannelList,
    },
    CliDefinition {
        command: "radioplus channel status",
        usage: "Usage: radioplus channel status [follow]\n",
        summary: "Show typed USBRadioPlus channel status",
        kind: CliCommand::ChannelStatus,
    },
    CliDefinition {
        command: "radioplus channel echo",
        usage: "Usage: radioplus channel echo [0|1]\n",
        summary: "Read or change channel echo",
        kind: CliCommand::ChannelEcho,
    },
    CliDefinition {
        command: "radioplus channel transmit",
        usage: "Usage: radioplus channel transmit <0|1> [ctcss-tenths-hz]\n",
        summary: "Key or unkey the channel transmitter",
        kind: CliCommand::ChannelTransmit,
    },
    CliDefinition {
        command: "radioplus channel flash",
        usage: "Usage: radioplus channel flash\n",
        summary: "Flash the channel transmitter",
        kind: CliCommand::ChannelFlash,
    },
    CliDefinition {
        command: "radioplus channel command",
        usage: CHANNEL_COMMAND_USAGE,
        summary: "Run a typed USBRadioPlus hardware command",
        kind: CliCommand::ChannelCommand,
    },
    CliDefinition {
        command: "radioplus processing stats",
        usage: "Usage: radioplus processing stats\n",
        summary: "Show USBRadioPlus processing statistics",
        kind: CliCommand::ProcessingStats,
    },
    CliDefinition {
        command: "radioplus processing reload",
        usage: "Usage: radioplus processing reload\n",
        summary: "Reload USBRadioPlus configuration",
        kind: CliCommand::ProcessingReload,
    },
];

/// Result category translated to Asterisk's CLI result pointer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum CliDisposition {
    /// Command completed.
    Success,
    /// Command was well formed but failed.
    Failure,
    /// Arguments were malformed and Asterisk should show usage.
    ShowUsage,
}

/// Buffered output and disposition for one command invocation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct CliResult {
    /// Asterisk-facing result category.
    pub(super) disposition: CliDisposition,
    /// Complete operator output.
    pub(super) output: String,
}

impl CliResult {
    /// Construct a successful result.
    pub(super) fn success(output: impl Into<String>) -> Self {
        Self {
            disposition: CliDisposition::Success,
            output: output.into(),
        }
    }

    /// Construct a failed result.
    pub(super) fn failure(output: impl Into<String>) -> Self {
        Self {
            disposition: CliDisposition::Failure,
            output: output.into(),
        }
    }

    fn usage() -> Self {
        Self {
            disposition: CliDisposition::ShowUsage,
            output: String::new(),
        }
    }
}

/// One attached incoming-link observation rendered by processing statistics.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct LinkStatus {
    /// Asterisk channel name.
    pub(super) name: String,
    /// Successfully processed blocks.
    pub(super) processed_blocks: u64,
    /// Bypassed blocks.
    pub(super) bypassed_blocks: u64,
    /// Failed blocks.
    pub(super) failed_blocks: u64,
}

/// Operations supplied by the Rust Asterisk host.
pub(super) trait CliBackend {
    /// Read every channel name reached and the terminal enumeration status.
    fn configured_channels(&mut self) -> (Vec<String>, i32);
    /// Read the selected tuning channel.
    fn active_channel(&mut self) -> Result<String, i32>;
    /// Select a tuning channel.
    fn select_active(&mut self, name: &str) -> i32;
    /// Read active-channel identity and status atomically with teardown exclusion.
    fn active_status(&mut self) -> Result<(String, UrpAstChannelStatus), i32>;
    /// Change echo mode on the active live channel.
    fn set_echo(&mut self, enabled: bool) -> i32;
    /// Change transmitter state on the active live channel.
    fn set_transmit(&mut self, keyed: bool, ctcss_tenths_hz: u32) -> i32;
    /// Run one typed hardware command on the active live channel.
    fn channel_command(&mut self, command: &mut UrpAstChannelCommand) -> i32;
    /// Run the serialized reload transaction.
    fn reload(&mut self) -> i32;
    /// Snapshot attached incoming-link observations.
    fn link_status(&mut self) -> Vec<LinkStatus>;
    /// Wait for input; true means the complete interval elapsed.
    fn wait_timed_out(&mut self, milliseconds: u32) -> bool;
    /// Emit one status snapshot immediately for a continuous command.
    fn emit(&mut self, output: &str);
}

/// Execute one already-selected CLI entry.
pub(super) fn execute(
    backend: &mut impl CliBackend,
    command: CliCommand,
    args: &[&str],
) -> CliResult {
    match command {
        CliCommand::Active => active(backend, args),
        CliCommand::ChannelList => channel_list(backend, args),
        CliCommand::ChannelStatus => channel_status(backend, args),
        CliCommand::ChannelEcho => channel_echo(backend, args),
        CliCommand::ChannelTransmit => channel_transmit(backend, args),
        CliCommand::ChannelFlash => channel_flash(backend, args),
        CliCommand::ChannelCommand => channel_command(backend, args),
        CliCommand::ProcessingStats => processing_stats(backend, args),
        CliCommand::ProcessingReload => processing_reload(backend, args),
    }
}

fn active(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    match args {
        [_, _] => match backend.active_channel() {
            Ok(name) => CliResult::success(format!("Active USB Radio device is [{name}].\n")),
            Err(status) => CliResult::failure(format!(
                "No active USB radio channel is available ({status}).\n"
            )),
        },
        [_, _, name] if name.len() <= u32::MAX as usize => {
            let status = backend.select_active(name);
            if status == URP_AST_OK {
                CliResult::success(format!("Active radio set to [{name}].\n"))
            } else {
                CliResult::failure(format!(
                    "Unable to select USB radio channel {name} ({status}).\n"
                ))
            }
        }
        _ => CliResult::usage(),
    }
}

fn channel_list(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    if args.len() != 3 {
        return CliResult::usage();
    }
    let (channels, status) = backend.configured_channels();
    let output = format!("{}\n", channels.join(","));
    if status == URP_AST_OK {
        CliResult::success(output)
    } else {
        CliResult::failure(output)
    }
}

fn channel_status(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    let follow = match args {
        [_, _, _] => false,
        [_, _, _, follow] if follow.eq_ignore_ascii_case("follow") => true,
        _ => return CliResult::usage(),
    };
    let mut output = String::new();
    loop {
        let Ok((name, status)) = backend.active_status() else {
            let unavailable = "Active channel status is unavailable.\n";
            if follow {
                backend.emit(unavailable);
            } else {
                output.push_str(unavailable);
            }
            return CliResult::failure(output);
        };
        let snapshot = format_status(&name, &status);
        if follow {
            backend.emit(&snapshot);
        } else {
            output.push_str(&snapshot);
        }
        if !follow || !backend.wait_timed_out(1_000) {
            return CliResult::success(output);
        }
    }
}

fn channel_echo(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    match args {
        [_, _, _] => match backend.active_status() {
            Ok((_, status)) => {
                CliResult::success(format!("echo_enabled={}\n", status.echo_enabled))
            }
            Err(_) => CliResult::failure(""),
        },
        [_, _, _, enabled] => {
            let Some(enabled) = parse_unsigned(enabled, 1) else {
                return CliResult::usage();
            };
            if backend.set_echo(enabled != 0) == URP_AST_OK {
                CliResult::success(format!("echo_enabled={enabled}\n"))
            } else {
                CliResult::failure("")
            }
        }
        _ => CliResult::usage(),
    }
}

fn channel_transmit(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    let [_, _, _, keyed, rest @ ..] = args else {
        return CliResult::usage();
    };
    if rest.len() > 1 {
        return CliResult::usage();
    }
    let Some(keyed) = parse_unsigned(keyed, 1) else {
        return CliResult::usage();
    };
    let ctcss = match rest.first() {
        None => 0,
        Some(value) => match parse_unsigned(value, u32::MAX) {
            Some(value) => value,
            None => return CliResult::usage(),
        },
    };
    if keyed == 0 && ctcss != 0 {
        return CliResult::usage();
    }
    if backend.set_transmit(keyed != 0, ctcss) != URP_AST_OK {
        return CliResult::failure("");
    }
    CliResult::success(format!(
        "transmit_keyed={keyed}\nforced_ctcss_tenths_hz={ctcss}\n"
    ))
}

fn channel_command(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    let Some(mut command) = parse_channel_command(args) else {
        return CliResult::usage();
    };
    let status = backend.channel_command(&mut command);
    if status != URP_AST_OK {
        return CliResult::failure(format!("Channel command failed ({status}).\n"));
    }
    if matches!(
        command.command,
        URP_AST_COMMAND_READ_EEPROM | URP_AST_COMMAND_WRITE_EEPROM
    ) {
        CliResult::success(format_eeprom(&command))
    } else {
        CliResult::success(format!("value={}\n", command.value))
    }
}

fn parse_channel_command(args: &[&str]) -> Option<UrpAstChannelCommand> {
    let [_, _, _, operation, tail @ ..] = args else {
        return None;
    };
    let mut command = UrpAstChannelCommand {
        struct_size: size_of::<UrpAstChannelCommand>() as u32,
        abi_version: ABI_VERSION,
        command: 0,
        target: 0,
        value: 0,
        flags: 0,
        eeprom_words: [0; 64],
    };
    if operation.eq_ignore_ascii_case("get-mixer") && tail.len() == 1 {
        command.command = URP_AST_COMMAND_GET_MIXER;
        command.target = mixer_target(tail[0])?;
    } else if operation.eq_ignore_ascii_case("set-mixer") && tail.len() == 2 {
        command.command = URP_AST_COMMAND_SET_MIXER;
        command.target = mixer_target(tail[0])?;
        command.value = i64::from(parse_unsigned(tail[1], 999)?);
    } else if operation.eq_ignore_ascii_case("test-tone") && tail.len() == 1 {
        command.command = URP_AST_COMMAND_SET_TEST_TONE;
        command.value = i64::from(parse_unsigned(tail[0], 1)?);
    } else if operation.eq_ignore_ascii_case("eeprom-read") && tail.is_empty() {
        command.command = URP_AST_COMMAND_READ_EEPROM;
    } else if operation.eq_ignore_ascii_case("eeprom-write") && tail.len() == 2 {
        command.command = URP_AST_COMMAND_WRITE_EEPROM;
        command.flags = parse_unsigned(
            tail[0],
            URP_AST_EEPROM_CHECKSUM_VALID | URP_AST_EEPROM_MAGIC_VALID,
        )?;
        command.eeprom_words = parse_eeprom_words(tail[1])?;
    } else if operation.eq_ignore_ascii_case("eeprom-save-tuning") && tail.is_empty() {
        command.command = URP_AST_COMMAND_SAVE_TUNING_EEPROM;
    } else if operation.eq_ignore_ascii_case("ctcss-inhibit") && tail.len() == 1 {
        command.command = URP_AST_COMMAND_SET_CTCSS_INHIBIT;
        command.value = i64::from(parse_unsigned(tail[0], 1)?);
    } else if operation.eq_ignore_ascii_case("subaudible-override") && tail.len() == 1 {
        command.command = URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE;
        command.value = i64::from(parse_unsigned(tail[0], 1)?);
    } else {
        return None;
    }
    Some(command)
}

fn channel_flash(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    if args.len() != 3 {
        return CliResult::usage();
    }
    backend.emit("USB Device Flash starting.\n");
    let mut status = URP_AST_OK;
    for burst in 0..3 {
        status = set_test_tone(backend, true);
        if status == URP_AST_OK {
            status = backend.set_transmit(true, 0);
        }
        if status != URP_AST_OK || !backend.wait_timed_out(1_000) {
            break;
        }
        status = backend.set_transmit(false, 0);
        let tone = set_test_tone(backend, false);
        if tone != URP_AST_OK && status == URP_AST_OK {
            status = URP_AST_SETUP_FAILED;
        }
        if status != URP_AST_OK || burst == 2 || !backend.wait_timed_out(1_500) {
            break;
        }
    }
    let _ = backend.set_transmit(false, 0);
    let _ = set_test_tone(backend, false);
    if status == URP_AST_OK {
        CliResult::success("USB Device Flash completed.\n")
    } else {
        CliResult::failure("")
    }
}

fn processing_stats(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    if args.len() != 3 {
        return CliResult::usage();
    }
    let Ok((name, status)) = backend.active_status() else {
        return CliResult::failure("");
    };
    let mut output = format_status(&name, &status);
    let links = backend.link_status();
    if links.is_empty() {
        output.push_str("No USBRadioPlus link-processing hook is attached.\n");
    } else {
        for link in links {
            writeln!(
                output,
                "{}/link: processed={} bypassed={} failed={} blocks",
                link.name, link.processed_blocks, link.bypassed_blocks, link.failed_blocks
            )
            .expect("writing to String cannot fail");
        }
    }
    CliResult::success(output)
}

fn processing_reload(backend: &mut impl CliBackend, args: &[&str]) -> CliResult {
    if args.len() != 3 {
        return CliResult::usage();
    }
    if backend.reload() == URP_AST_OK {
        CliResult::success("Configuration reloaded in place.\n")
    } else {
        CliResult::failure("Configuration reload failed; existing settings retained.\n")
    }
}

fn set_test_tone(backend: &mut impl CliBackend, enabled: bool) -> i32 {
    let mut command = UrpAstChannelCommand {
        struct_size: size_of::<UrpAstChannelCommand>() as u32,
        abi_version: ABI_VERSION,
        command: URP_AST_COMMAND_SET_TEST_TONE,
        target: 0,
        value: i64::from(enabled),
        flags: 0,
        eeprom_words: [0; 64],
    };
    backend.channel_command(&mut command)
}

fn parse_unsigned(text: &str, maximum: u32) -> Option<u32> {
    if text.is_empty() || text.starts_with('-') {
        return None;
    }
    text.parse::<u64>()
        .ok()
        .filter(|value| *value <= u64::from(maximum))
        .map(|value| value as u32)
}

fn mixer_target(name: &str) -> Option<u32> {
    if name.eq_ignore_ascii_case("receive") {
        Some(URP_AST_MIXER_RECEIVE)
    } else if name.eq_ignore_ascii_case("transmit-a") {
        Some(URP_AST_MIXER_TRANSMIT_A)
    } else if name.eq_ignore_ascii_case("transmit-b") {
        Some(URP_AST_MIXER_TRANSMIT_B)
    } else {
        None
    }
}

fn parse_eeprom_words(text: &str) -> Option<[u16; 64]> {
    let mut words = [0; 64];
    let mut values = text.split(',');
    for word in &mut words {
        *word = parse_unsigned(values.next()?, u16::MAX.into())? as u16;
    }
    values.next().is_none().then_some(words)
}

fn format_eeprom(command: &UrpAstChannelCommand) -> String {
    let words = command
        .eeprom_words
        .iter()
        .map(u16::to_string)
        .collect::<Vec<_>>()
        .join(",");
    format!("flags={}\nwords={words}\n", command.flags)
}

/// Status keys kept in output order for tune-utility compatibility.
pub(super) const STATUS_KEYS: [&str; 54] = [
    "status_abi",
    "channel",
    "transport",
    "running",
    "echo_enabled",
    "dtmf_enabled",
    "receive_handoff_available",
    "receive_handoff_discarded",
    "program_handoff_available",
    "program_handoff_discarded",
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
    "ring_occupancy_frames",
    "ring_reserve_frames",
    "ring_target_frames",
    "ring_capacity_frames",
    "ring_ratio",
    "ring_underrun_samples",
    "ring_overrun_samples",
    "ring_concealment_samples",
    "carrier_active",
    "subaudible_active",
    "receiver_keyed",
    "logical_ptt",
    "ctcss_decode_index",
    "dcs_valid",
    "receive_mixer_level",
    "transmit_a_mixer_level",
    "transmit_b_mixer_level",
];

fn format_status(name: &str, status: &UrpAstChannelStatus) -> String {
    let values = [
        ABI_VERSION.to_string(),
        name.to_owned(),
        status.transport.to_string(),
        status.running.to_string(),
        status.echo_enabled.to_string(),
        status.dtmf_enabled.to_string(),
        status.receive_handoff_available.to_string(),
        status.receive_handoff_discarded.to_string(),
        status.program_handoff_available.to_string(),
        status.program_handoff_discarded.to_string(),
        c_float(f64::from(status.receive_input_peak), 9),
        c_float(f64::from(status.receive_input_rms), 9),
        c_float(f64::from(status.receive_output_peak), 9),
        c_float(f64::from(status.receive_output_rms), 9),
        status.receive_input_rail_samples.to_string(),
        status.receive_output_rail_samples.to_string(),
        c_float(f64::from(status.receive_ctcss_decoder_peak), 9),
        status.receive_rssi_peak.to_string(),
        status.receive_rssi_updated.to_string(),
        c_float(f64::from(status.transmit_program_peak), 9),
        c_float(f64::from(status.transmit_program_rms), 9),
        c_float(f64::from(status.transmit_output_peak), 9),
        c_float(f64::from(status.transmit_output_rms), 9),
        status.input_overflow_count.to_string(),
        status.output_underflow_count.to_string(),
        status.input_clip_sample_count.to_string(),
        status.output_clip_sample_count.to_string(),
        status.callback_last_duration_ns.to_string(),
        status.callback_max_duration_ns.to_string(),
        status.callback_last_start_delay_ns.to_string(),
        status.callback_max_start_delay_ns.to_string(),
        status.callback_late_start_count.to_string(),
        status.last_input_xrun_monotonic_ns.to_string(),
        status.last_output_xrun_monotonic_ns.to_string(),
        c_float(status.input_latency_seconds, 17),
        c_float(status.output_latency_seconds, 17),
        c_float(status.native_sample_rate_hz, 17),
        status.ring_occupancy_frames.to_string(),
        status.ring_reserve_frames.to_string(),
        status.ring_target_frames.to_string(),
        status.ring_capacity_frames.to_string(),
        c_float(status.ring_ratio, 17),
        status.ring_underrun_samples.to_string(),
        status.ring_overrun_samples.to_string(),
        status.ring_concealment_samples.to_string(),
        status.carrier_active.to_string(),
        status.subaudible_active.to_string(),
        status.receiver_keyed.to_string(),
        status.logical_ptt.to_string(),
        status.ctcss_decode_index.to_string(),
        status.dcs_valid.to_string(),
        status.receive_mixer_level.to_string(),
        status.transmit_a_mixer_level.to_string(),
        status.transmit_b_mixer_level.to_string(),
    ];
    let mut output = String::new();
    for (key, value) in STATUS_KEYS.iter().zip(values) {
        writeln!(output, "{key}={value}").expect("writing to String cannot fail");
    }
    output
}

fn c_float(value: f64, precision: u32) -> String {
    unsafe extern "C" {
        fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    }
    let format = if precision == 9 { c"%.9g" } else { c"%.17g" };
    let mut buffer = [0 as c_char; 64];
    // SAFETY: buffer is writable, format contains one double conversion, and
    // the bounded result is checked before reading it as a C string.
    let length = unsafe { snprintf(buffer.as_mut_ptr(), buffer.len(), format.as_ptr(), value) };
    assert!(length >= 0 && length < buffer.len() as c_int);
    // SAFETY: snprintf wrote a NUL-terminated string within buffer.
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

struct ProductionBackend {
    fd: c_int,
}

impl ProductionBackend {
    fn driver(&self) -> Result<channel::DriverContext, i32> {
        channel::driver_context().ok_or(URP_AST_NOT_READY)
    }

    fn channel_name(&self, index: u32) -> Result<String, i32> {
        let driver = self.driver()?;
        // SAFETY: channel registration validated this process-lifetime descriptor.
        let descriptor = unsafe { &*driver.descriptor };
        let call = descriptor
            .driver_channel_name
            .expect("registration validated driver_channel_name");
        copy_driver_name(|output, capacity, length| {
            // SAFETY: the driver is installed and output describes the supplied span.
            unsafe { call(driver.driver, index, output, capacity, length) }
        })
    }

    fn selected_name(&self) -> Result<String, i32> {
        let driver = self.driver()?;
        // SAFETY: channel registration validated this process-lifetime descriptor.
        let descriptor = unsafe { &*driver.descriptor };
        let call = descriptor
            .driver_active_channel
            .expect("registration validated driver_active_channel");
        copy_driver_name(|output, capacity, length| {
            // SAFETY: the driver is installed and output describes the supplied span.
            unsafe { call(driver.driver, output, capacity, length) }
        })
    }

    fn with_active<T>(&self, operation: impl FnOnce(&channel::Channel) -> T) -> Result<T, i32> {
        let name = self.selected_name()?;
        channel::with_live_channel(&name, operation).ok_or(URP_AST_NOT_READY)
    }
}

impl CliBackend for ProductionBackend {
    fn configured_channels(&mut self) -> (Vec<String>, i32) {
        let mut channels = Vec::new();
        // Each named section consumes multiple bytes of the project's u32-sized
        // configuration, so the installed driver reaches END before index overflow.
        let mut index = 0_u32;
        loop {
            match self.channel_name(index) {
                Ok(name) => channels.push(name),
                Err(URP_AST_CHANNEL_NOT_FOUND) => return (channels, URP_AST_OK),
                Err(status) => return (channels, status),
            }
            index += 1;
        }
    }

    fn active_channel(&mut self) -> Result<String, i32> {
        self.selected_name()
    }

    fn select_active(&mut self, name: &str) -> i32 {
        let Ok(driver) = self.driver() else {
            return URP_AST_NOT_READY;
        };
        // SAFETY: channel registration validated this process-lifetime descriptor.
        let descriptor = unsafe { &*driver.descriptor };
        let call = descriptor
            .driver_set_active_channel
            .expect("registration validated driver_set_active_channel");
        // SAFETY: the driver remains installed and name is a byte-counted span.
        unsafe { call(driver.driver, name.as_ptr(), name.len() as u32) }
    }

    fn active_status(&mut self) -> Result<(String, UrpAstChannelStatus), i32> {
        let name = self.selected_name()?;
        let mut status = UrpAstChannelStatus {
            struct_size: size_of::<UrpAstChannelStatus>() as u32,
            abi_version: ABI_VERSION,
            ..UrpAstChannelStatus::default()
        };
        let result = channel::with_live_channel(&name, |selected| {
            channel::read_status(selected, &mut status)
        })
        .ok_or(URP_AST_NOT_READY)?;
        if result == URP_AST_OK {
            Ok((name, status))
        } else {
            Err(result)
        }
    }

    fn set_echo(&mut self, enabled: bool) -> i32 {
        self.with_active(|selected| channel::set_echo(selected, enabled))
            .unwrap_or_else(|status| status)
    }

    fn set_transmit(&mut self, keyed: bool, ctcss_tenths_hz: u32) -> i32 {
        self.with_active(|selected| channel::set_transmit(selected, keyed, ctcss_tenths_hz))
            .unwrap_or_else(|status| status)
    }

    fn channel_command(&mut self, command: &mut UrpAstChannelCommand) -> i32 {
        self.with_active(|selected| channel::run_command(selected, command))
            .unwrap_or_else(|status| status)
    }

    fn reload(&mut self) -> i32 {
        reload::reload()
    }

    fn link_status(&mut self) -> Vec<LinkStatus> {
        link::statistics()
            .into_iter()
            .map(|status| LinkStatus {
                name: status.asterisk_channel.into(),
                processed_blocks: status.observation.processed_blocks,
                bypassed_blocks: status.observation.bypassed_blocks,
                failed_blocks: status.observation.failed_blocks,
            })
            .collect()
    }

    fn wait_timed_out(&mut self, milliseconds: u32) -> bool {
        let mut input = libc::pollfd {
            fd: self.fd,
            events: libc::POLLIN,
            revents: 0,
        };
        // SAFETY: input is one initialized poll descriptor.
        unsafe {
            libc::poll(
                &raw mut input,
                1,
                milliseconds.min(c_int::MAX as u32) as c_int,
            ) == 0
        }
    }

    fn emit(&mut self, output: &str) {
        let Ok(output) = CString::new(output) else {
            return;
        };
        // SAFETY: output is NUL terminated and the format has one matching
        // string argument.
        unsafe { ffi::ast_cli(self.fd, c"%s".as_ptr(), output.as_ptr()) };
    }
}

fn copy_driver_name(call: impl Fn(*mut u8, u32, *mut u32) -> i32) -> Result<String, i32> {
    let mut length = 0;
    let status = call(ptr::null_mut(), 0, &raw mut length);
    if status != URP_AST_OK {
        return Err(status);
    }
    if length == 0 {
        return Err(URP_AST_ASTERISK_FAILURE);
    }
    let mut name = vec![0_u8; length as usize];
    let status = call(name.as_mut_ptr(), length, &raw mut length);
    if status != URP_AST_OK {
        return Err(status);
    }
    name.truncate(length as usize);
    String::from_utf8(name).map_err(|_| URP_AST_ASTERISK_FAILURE)
}

static COMMANDS: [&CStr; 9] = [
    c"radioplus active",
    c"radioplus channel list",
    c"radioplus channel status",
    c"radioplus channel echo",
    c"radioplus channel transmit",
    c"radioplus channel flash",
    c"radioplus channel command",
    c"radioplus processing stats",
    c"radioplus processing reload",
];

static USAGES: [&CStr; 9] = [
    c"Usage: radioplus active [channel-name]\n",
    c"Usage: radioplus channel list\n",
    c"Usage: radioplus channel status [follow]\n",
    c"Usage: radioplus channel echo [0|1]\n",
    c"Usage: radioplus channel transmit <0|1> [ctcss-tenths-hz]\n",
    c"Usage: radioplus channel flash\n",
    c"Usage: radioplus channel command get-mixer <receive|transmit-a|transmit-b>\n       radioplus channel command set-mixer <receive|transmit-a|transmit-b> <0-999>\n       radioplus channel command test-tone <0|1>\n       radioplus channel command eeprom-read\n       radioplus channel command eeprom-write <flags> <64-comma-separated-words>\n       radioplus channel command eeprom-save-tuning\n       radioplus channel command ctcss-inhibit <0|1>\n       radioplus channel command subaudible-override <0|1>\n",
    c"Usage: radioplus processing stats\n",
    c"Usage: radioplus processing reload\n",
];

static SUMMARIES: [&CStr; 9] = [
    c"Select the USBRadioPlus tuning channel",
    c"List configured USBRadioPlus channels",
    c"Show typed USBRadioPlus channel status",
    c"Read or change channel echo",
    c"Key or unkey the channel transmitter",
    c"Flash the channel transmitter",
    c"Run a typed USBRadioPlus hardware command",
    c"Show USBRadioPlus processing statistics",
    c"Reload USBRadioPlus configuration",
];

static REGISTERED: AtomicPtr<ffi::ast_cli_entry> = AtomicPtr::new(ptr::null_mut());

unsafe extern "C" {
    fn ast_cli_unregister_multiple(entries: *mut ffi::ast_cli_entry, count: c_int) -> c_int;
}

unsafe extern "C" fn active_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(0, entry, command, args) }
}

unsafe extern "C" fn list_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(1, entry, command, args) }
}

unsafe extern "C" fn status_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(2, entry, command, args) }
}

unsafe extern "C" fn echo_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(3, entry, command, args) }
}

unsafe extern "C" fn transmit_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(4, entry, command, args) }
}

unsafe extern "C" fn flash_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(5, entry, command, args) }
}

unsafe extern "C" fn command_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(6, entry, command, args) }
}

unsafe extern "C" fn stats_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(7, entry, command, args) }
}

unsafe extern "C" fn reload_callback(
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    // SAFETY: Asterisk invokes CLI handlers with its live entry and arguments.
    unsafe { callback(8, entry, command, args) }
}

const HANDLERS: [unsafe extern "C" fn(
    *mut ffi::ast_cli_entry,
    c_int,
    *mut ffi::ast_cli_args,
) -> *mut c_char; 9] = [
    active_callback,
    list_callback,
    status_callback,
    echo_callback,
    transmit_callback,
    flash_callback,
    command_callback,
    stats_callback,
    reload_callback,
];

unsafe fn callback(
    index: usize,
    entry: *mut ffi::ast_cli_entry,
    command: c_int,
    args: *mut ffi::ast_cli_args,
) -> *mut c_char {
    if command == ffi::CLI_INIT {
        if entry.is_null() {
            return ffi::RESULT_FAILURE as *mut c_char;
        }
        // SAFETY: Asterisk supplied the mutable registered entry.
        unsafe {
            (*entry).command = COMMANDS[index].as_ptr().cast_mut();
            (*entry).usage = USAGES[index].as_ptr();
        }
        return ffi::RESULT_SUCCESS as *mut c_char;
    }
    if command == ffi::CLI_GENERATE {
        return ffi::RESULT_SUCCESS as *mut c_char;
    }
    if args.is_null() {
        return ffi::RESULT_FAILURE as *mut c_char;
    }
    // SAFETY: Asterisk supplied one initialized argument structure.
    let arguments = unsafe { &*args };
    // SAFETY: Asterisk promises `argv` contains `argc` entries when nonempty.
    let Some(pointers) = (unsafe { argument_pointers(arguments.argv, arguments.argc) }) else {
        return ffi::RESULT_FAILURE as *mut c_char;
    };
    let mut owned = Vec::with_capacity(pointers.len());
    for pointer in pointers {
        if pointer.is_null() {
            return ffi::RESULT_FAILURE as *mut c_char;
        }
        // SAFETY: each argv entry is a NUL-terminated Asterisk string.
        let Ok(argument) = unsafe { CStr::from_ptr(*pointer) }.to_str() else {
            return ffi::RESULT_FAILURE as *mut c_char;
        };
        owned.push(argument);
    }
    let mut backend = ProductionBackend { fd: arguments.fd };
    let result = execute(&mut backend, CLI_DEFINITIONS[index].kind, &owned);
    if !result.output.is_empty() {
        backend.emit(&result.output);
    }
    match result.disposition {
        CliDisposition::Success => ffi::RESULT_SUCCESS as *mut c_char,
        CliDisposition::Failure => ffi::RESULT_FAILURE as *mut c_char,
        CliDisposition::ShowUsage => ffi::RESULT_SHOWUSAGE as *mut c_char,
    }
}

/// Interpret Asterisk's optional argument array without constructing an empty
/// Rust slice from a null pointer.
///
/// # Safety
///
/// A non-null `argv` must identify at least `argc` initialized pointers.
pub(super) unsafe fn argument_pointers<'a>(
    argv: *const *const c_char,
    argc: c_int,
) -> Option<&'a [*const c_char]> {
    if argc < 0 {
        None
    } else if argc == 0 {
        Some(&[])
    } else if argv.is_null() {
        None
    } else {
        // SAFETY: the caller supplies the readable pointer array promised above.
        Some(unsafe { std::slice::from_raw_parts(argv, argc as usize) })
    }
}

pub(super) fn registration_owner_is_valid(module_self: *mut ffi::ast_module) -> bool {
    !module_self.is_null()
}

/// Unregister a partially published group before releasing its backing storage.
///
/// # Safety
///
/// `entries` must come from exactly one [`Box::into_raw`] call. `unregister`
/// must stop every external user of that allocation before it returns.
pub(super) unsafe fn rollback_registration<T>(entries: *mut T, unregister: impl FnOnce(*mut T)) {
    unregister(entries);
    // SAFETY: the caller transfers the unique Box allocation after unregister
    // has removed every externally visible reference.
    drop(unsafe { Box::from_raw(entries) });
}

/// Register every USBRadioPlus CLI entry as one all-or-nothing group.
pub(super) fn register(module_self: *mut ffi::ast_module) -> i32 {
    if !REGISTERED.load(Ordering::Acquire).is_null() {
        return URP_AST_OK;
    }
    if !registration_owner_is_valid(module_self) {
        return URP_AST_NOT_READY;
    }
    // SAFETY: ast_cli_entry is a C aggregate whose all-zero state is the
    // initialization expected by AST_CLI_DEFINE before registration.
    let mut entries: Box<[ffi::ast_cli_entry; 9]> = Box::new(unsafe { std::mem::zeroed() });
    for (index, entry) in entries.iter_mut().enumerate() {
        entry.summary = SUMMARIES[index].as_ptr();
        entry.handler = Some(HANDLERS[index]);
    }
    let raw = Box::into_raw(entries);
    // SAFETY: raw names nine stable entries; Asterisk initializes and retains
    // them until the matching unregister call.
    if unsafe { ffi::__ast_cli_register_multiple(raw.cast(), 9, module_self) } != 0 {
        // Asterisk registers every entry independently and reports the OR of
        // their results, so a group failure may leave some entries published.
        // SAFETY: raw is the unique allocation above; bulk unregistration
        // removes every successful partial registration before it is freed.
        unsafe {
            rollback_registration(raw, |entries| {
                ast_cli_unregister_multiple(entries.cast(), 9);
            });
        }
        return URP_AST_ASTERISK_FAILURE;
    }
    REGISTERED.store(raw.cast(), Ordering::Release);
    URP_AST_OK
}

/// Unregister and release every CLI entry registered by [`register`].
pub(super) fn unregister() {
    let entries = REGISTERED.swap(ptr::null_mut(), Ordering::AcqRel);
    if entries.is_null() {
        return;
    }
    // SAFETY: entries names the exact nine-entry allocation retained after a
    // successful registration. Unregistration releases Asterisk's metadata.
    unsafe {
        ast_cli_unregister_multiple(entries, 9);
        drop(Box::from_raw(entries.cast::<[ffi::ast_cli_entry; 9]>()));
    }
}

#[cfg(test)]
#[path = "cli_tests.rs"]
pub(in crate::host) mod tests;
