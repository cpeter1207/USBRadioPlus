//! Focused typed-CLI tests.

use super::*;
use crate::{
    URP_AST_CHANNEL_NOT_FOUND, URP_AST_COMMAND_READ_EEPROM, URP_AST_COMMAND_SET_TEST_TONE,
    URP_AST_COMMAND_WRITE_EEPROM, URP_AST_MIXER_RECEIVE, URP_AST_OK, URP_AST_SETUP_FAILED,
    UrpAstChannelCommand, UrpAstChannelStatus,
};

use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::Mutex;

use crate::host::channel::tests::{
    DriverCalls, RegistrationGuard, capture_command, capture_dtmf, capture_profile, capture_status,
    capture_transmit, lifecycle_descriptor, register_fixture,
};
use crate::host::support::{Fixture as HostFixture, with_state};

unsafe extern "C" {
    fn urp_test_float_format_result(result: c_int);
}

pub(in crate::host) fn set_registration_result(result: c_int) {
    CLI_CALLS.lock().unwrap().register_result = result;
}

#[test]
fn float_format_failures_preserve_the_fatal_buffer_guard() {
    for (result, precision) in [(-1, 9), (64, 17)] {
        // SAFETY: the native test seam affects only this thread's next float conversion.
        unsafe { urp_test_float_format_result(result) };
        assert!(std::panic::catch_unwind(|| c_float(0.5, precision)).is_err());
        assert_eq!(c_float(0.5, precision), "0.5");
    }
}

#[test]
fn active_entry_rejects_a_real_channel_name_exceeding_the_abi_length() {
    let _fixture = HostFixture::new();
    let name = crate::host::support::OversizedCString::new();
    let pointers = [c"radioplus".as_ptr(), c"active".as_ptr(), name.as_ptr()];
    // SAFETY: Asterisk's aggregate contains only integer and pointer fields.
    let mut arguments: ffi::ast_cli_args = unsafe { std::mem::zeroed() };
    arguments.argc = pointers.len() as c_int;
    arguments.argv = pointers.as_ptr();
    assert_eq!(
        // SAFETY: all three immutable C strings remain mapped throughout dispatch.
        unsafe { callback(0, ptr::null_mut(), 0, &mut arguments) },
        ffi::RESULT_SHOWUSAGE as *mut c_char
    );
    with_state(|state| assert!(state.messages.is_empty()));
}

#[derive(Default)]
struct CliCalls {
    selected: Vec<u8>,
    selection_result: i32,
    register_result: c_int,
    registered: Vec<usize>,
    unregistered: Vec<usize>,
}

static CLI_CALLS: Mutex<CliCalls> = Mutex::new(CliCalls {
    selected: Vec::new(),
    selection_result: URP_AST_OK,
    register_result: 0,
    registered: Vec::new(),
    unregistered: Vec::new(),
});

pub(in crate::host) fn assert_link_snapshot(
    name: &str,
    processed: u64,
    bypassed: u64,
    failed: u64,
) {
    let snapshot = ProductionBackend { fd: -1 }.link_status();
    assert_eq!(snapshot.len(), 1);
    assert_eq!(snapshot[0].name, name);
    assert_eq!(snapshot[0].processed_blocks, processed);
    assert_eq!(snapshot[0].bypassed_blocks, bypassed);
    assert_eq!(snapshot[0].failed_blocks, failed);
}

unsafe extern "C" fn active_name(
    _: *mut c_void,
    output: *mut u8,
    capacity: u32,
    length: *mut u32,
) -> i32 {
    let calls = CLI_CALLS.lock().unwrap();
    // SAFETY: the backend supplies a writable length and advertises its buffer size.
    unsafe {
        *length = calls.selected.len() as u32;
        if !output.is_null() {
            assert!(capacity as usize >= calls.selected.len());
            ptr::copy_nonoverlapping(calls.selected.as_ptr(), output, calls.selected.len());
        }
    }
    URP_AST_OK
}

unsafe extern "C" fn select_name(_: *mut c_void, name: *const u8, length: u32) -> i32 {
    let mut calls = CLI_CALLS.lock().unwrap();
    if calls.selection_result == URP_AST_OK {
        // SAFETY: the backend retains this byte-counted channel name through the call.
        calls.selected = unsafe { std::slice::from_raw_parts(name, length as usize) }.to_vec();
    }
    calls.selection_result
}

#[unsafe(no_mangle)]
unsafe extern "C" fn __ast_cli_register_multiple(
    entries: *mut ffi::ast_cli_entry,
    count: c_int,
    module: *mut ffi::ast_module,
) -> c_int {
    assert_eq!(count, 9);
    assert!(!module.is_null());
    // SAFETY: registration retains the nine-entry allocation until unregistration.
    for (index, entry) in unsafe { std::slice::from_raw_parts_mut(entries, count as usize) }
        .iter_mut()
        .enumerate()
    {
        // SAFETY: register installs a valid handler and static summary on every entry.
        unsafe {
            assert_eq!(CStr::from_ptr(entry.summary), SUMMARIES[index]);
            assert_eq!(
                entry.handler.unwrap()(entry, ffi::CLI_INIT, ptr::null_mut()),
                ffi::RESULT_SUCCESS as *mut c_char
            );
            assert_eq!(CStr::from_ptr(entry.command), COMMANDS[index]);
        }
    }
    let mut calls = CLI_CALLS.lock().unwrap();
    calls.registered.push(entries as usize);
    calls.register_result
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ast_cli_unregister_multiple(
    entries: *mut ffi::ast_cli_entry,
    count: c_int,
) -> c_int {
    assert_eq!(count, 9);
    // SAFETY: cleanup must unregister before freeing this initialized allocation.
    let first = unsafe { &*entries };
    // SAFETY: initialization installed a process-lifetime static command string.
    assert_eq!(unsafe { CStr::from_ptr(first.command) }, COMMANDS[0]);
    CLI_CALLS
        .lock()
        .unwrap()
        .unregistered
        .push(entries as usize);
    0
}

const _: unsafe extern "C" fn(*mut ffi::ast_cli_entry, c_int, *mut ffi::ast_module) -> c_int =
    ffi::__ast_cli_register_multiple;
const _: unsafe extern "C" fn(*mut ffi::ast_cli_entry, c_int, *mut ffi::ast_module) -> c_int =
    __ast_cli_register_multiple;
const _: unsafe extern "C" fn(*mut ffi::ast_cli_entry, c_int) -> c_int =
    ast_cli_unregister_multiple;

#[test]
fn concrete_cli_registration_initializes_all_entries_and_rolls_back_failures() {
    let _fixture = HostFixture::new();
    struct Cleanup;
    impl Drop for Cleanup {
        fn drop(&mut self) {
            unregister();
        }
    }
    let _cleanup = Cleanup;
    *CLI_CALLS.lock().unwrap() = CliCalls::default();
    unregister();
    assert_eq!(register(ptr::null_mut()), URP_AST_NOT_READY);
    let module = ptr::dangling_mut();
    CLI_CALLS.lock().unwrap().register_result = -1;
    assert_eq!(register(module), URP_AST_ASTERISK_FAILURE);
    assert!(REGISTERED.load(Ordering::Acquire).is_null());
    {
        let calls = CLI_CALLS.lock().unwrap();
        assert_eq!(calls.registered, calls.unregistered);
        assert_eq!(calls.registered.len(), 1);
    }
    CLI_CALLS.lock().unwrap().register_result = 0;
    assert_eq!(register(module), URP_AST_OK);
    assert_eq!(register(module), URP_AST_OK);
    assert_eq!(CLI_CALLS.lock().unwrap().registered.len(), 2);
    unregister();
    unregister();
    let calls = CLI_CALLS.lock().unwrap();
    assert_eq!(calls.registered, calls.unregistered);
}

#[test]
fn driver_name_copy_checks_each_abi_response_before_returning_utf8() {
    for (query, bytes, fill, expected) in [
        (-6, b"usb".as_slice(), 0, Err(-6)),
        (0, b"".as_slice(), 0, Err(URP_AST_ASTERISK_FAILURE)),
        (0, b"usb".as_slice(), -6, Err(-6)),
        (0, b"\xff".as_slice(), 0, Err(URP_AST_ASTERISK_FAILURE)),
        (0, b"usb".as_slice(), 0, Ok("usb".to_owned())),
    ] {
        let result = copy_driver_name(|output, capacity, length| {
            // SAFETY: copy_driver_name supplies this output length and exact buffer size.
            unsafe {
                *length = bytes.len() as u32;
                if output.is_null() {
                    assert_eq!(capacity, 0);
                    query
                } else {
                    assert_eq!(capacity as usize, bytes.len());
                    ptr::copy_nonoverlapping(bytes.as_ptr(), output, bytes.len());
                    fill
                }
            }
        });
        assert_eq!(result, expected);
    }
}

#[test]
fn concrete_backend_propagates_missing_driver_and_real_poll_readiness() {
    use std::io::Write;
    use std::os::fd::AsRawFd;
    use std::os::unix::net::UnixStream;

    let _fixture = HostFixture::new();
    let (reader, mut writer) = UnixStream::pair().unwrap();
    let mut backend = ProductionBackend {
        fd: reader.as_raw_fd(),
    };
    assert_eq!(backend.configured_channels(), (vec![], URP_AST_NOT_READY));
    assert_eq!(backend.active_channel(), Err(URP_AST_NOT_READY));
    assert_eq!(backend.select_active("usb"), URP_AST_NOT_READY);
    assert_eq!(backend.active_status().err(), Some(URP_AST_NOT_READY));
    assert_eq!(backend.set_echo(true), URP_AST_NOT_READY);
    assert_eq!(backend.set_transmit(true, 1230), URP_AST_NOT_READY);
    let mut command =
        parse_channel_command(&["radioplus", "channel", "command", "test-tone", "1"]).unwrap();
    assert_eq!(backend.channel_command(&mut command), URP_AST_NOT_READY);
    assert_eq!(backend.reload(), URP_AST_ASTERISK_FAILURE);
    let _ = backend.link_status();
    assert!(backend.wait_timed_out(0));
    writer.write_all(b"x").unwrap();
    assert!(!backend.wait_timed_out(0));
    assert!(!backend.wait_timed_out(u32::MAX));
    backend.emit("ignored\0suffix");
    backend.emit("operator output\n");
    with_state(|state| {
        assert_eq!(
            state.messages,
            [(1, reader.as_raw_fd(), "operator output\n".into())]
        )
    });
}

#[test]
fn concrete_backend_uses_registered_driver_and_live_channel_callbacks() {
    let _fixture = HostFixture::new();
    *CLI_CALLS.lock().unwrap() = CliCalls {
        selected: b"usb".to_vec(),
        ..CliCalls::default()
    };
    let mut descriptor = lifecycle_descriptor();
    descriptor.driver_channel_name = Some(capture_profile);
    descriptor.driver_active_channel = Some(active_name);
    descriptor.driver_set_active_channel = Some(select_name);
    descriptor.channel_get_status = Some(capture_status);
    descriptor.channel_set_echo = Some(capture_dtmf);
    descriptor.channel_set_transmit = Some(capture_transmit);
    descriptor.channel_command = Some(capture_command);
    let calls = Mutex::new(DriverCalls::default());
    calls.lock().unwrap().profiles = vec![b"usb".to_vec(), b"other".to_vec()];
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    let mut backend = ProductionBackend { fd: 41 };
    assert_eq!(
        backend.configured_channels(),
        (vec!["usb".into(), "other".into()], URP_AST_OK)
    );
    calls.lock().unwrap().query_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        backend.configured_channels(),
        (vec![], URP_AST_SETUP_FAILED)
    );
    calls.lock().unwrap().query_result = URP_AST_OK;
    assert_eq!(backend.active_channel(), Ok("usb".into()));
    let pointers = [c"radioplus".as_ptr(), c"active".as_ptr()];
    // SAFETY: Asterisk's aggregate contains only integers and pointers.
    let mut arguments: ffi::ast_cli_args = unsafe { std::mem::zeroed() };
    arguments.fd = backend.fd;
    arguments.argc = pointers.len() as c_int;
    arguments.argv = pointers.as_ptr();
    assert_eq!(
        // SAFETY: the arguments and both C strings remain live through dispatch.
        unsafe { callback(0, ptr::null_mut(), 0, &mut arguments) },
        ffi::RESULT_SUCCESS as *mut c_char
    );
    assert_eq!(backend.active_status().err(), Some(URP_AST_NOT_READY));
    assert_eq!(backend.set_echo(true), URP_AST_NOT_READY);
    let technology = with_state(|state| state.registered[0]) as *const ffi::ast_channel_tech;
    // SAFETY: registration retains this technology, capability, and requester.
    let owner = unsafe {
        (*technology).requester.unwrap()(
            (*technology).type_,
            (*technology).capabilities,
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        )
    };
    assert!(!owner.is_null());
    assert_eq!(backend.active_status().unwrap().1.running, 1);
    assert_eq!(backend.set_echo(true), URP_AST_OK);
    assert_eq!(backend.set_transmit(true, 1230), URP_AST_OK);
    let mut command = parse_channel_command(&[
        "radioplus",
        "channel",
        "command",
        "set-mixer",
        "receive",
        "10",
    ])
    .unwrap();
    assert_eq!(backend.channel_command(&mut command), URP_AST_OK);
    assert_eq!(command.value, 17);
    assert_eq!(calls.lock().unwrap().dtmf, [1]);
    assert_eq!(calls.lock().unwrap().transmit, [(1, 1230)]);
    calls.lock().unwrap().result = URP_AST_SETUP_FAILED;
    assert_eq!(backend.active_status().err(), Some(URP_AST_SETUP_FAILED));
    assert_eq!(backend.set_echo(false), URP_AST_SETUP_FAILED);
    assert_eq!(backend.set_transmit(false, 0), URP_AST_SETUP_FAILED);
    assert_eq!(backend.channel_command(&mut command), URP_AST_SETUP_FAILED);
    assert_eq!(backend.select_active("other"), URP_AST_OK);
    assert_eq!(backend.active_channel(), Ok("other".into()));
    CLI_CALLS.lock().unwrap().selection_result = URP_AST_SETUP_FAILED;
    assert_eq!(backend.select_active("usb"), URP_AST_SETUP_FAILED);
    assert_eq!(backend.active_channel(), Ok("other".into()));
}

#[test]
fn cli_c_entries_validate_metadata_arguments_and_map_dispositions() {
    let _fixture = HostFixture::new();
    for (index, handler) in HANDLERS.iter().enumerate() {
        // SAFETY: ast_cli_entry is initialized as the Asterisk registration API requires.
        let mut entry: ffi::ast_cli_entry = unsafe { std::mem::zeroed() };
        // SAFETY: initialization uses the live entry; null forms are explicitly rejected.
        unsafe {
            assert_eq!(
                handler(ptr::null_mut(), ffi::CLI_INIT, ptr::null_mut()),
                ffi::RESULT_FAILURE as *mut c_char
            );
            assert_eq!(
                handler(&mut entry, ffi::CLI_INIT, ptr::null_mut()),
                ffi::RESULT_SUCCESS as *mut c_char
            );
            assert_eq!(CStr::from_ptr(entry.command), COMMANDS[index]);
            assert_eq!(CStr::from_ptr(entry.usage), USAGES[index]);
            assert_eq!(
                handler(ptr::null_mut(), ffi::CLI_GENERATE, ptr::null_mut()),
                ffi::RESULT_SUCCESS as *mut c_char
            );
            assert_eq!(
                handler(ptr::null_mut(), 0, ptr::null_mut()),
                ffi::RESULT_FAILURE as *mut c_char
            );
        }
    }
    // SAFETY: all CLI argument fields accept zero initialization before assignment.
    let mut arguments: ffi::ast_cli_args = unsafe { std::mem::zeroed() };
    arguments.fd = 43;
    for count in [-1, 1] {
        arguments.argc = count;
        assert_eq!(
            // SAFETY: invalid count/null array is rejected without dereferencing.
            unsafe { callback(0, ptr::null_mut(), 0, &mut arguments) },
            ffi::RESULT_FAILURE as *mut c_char
        );
    }
    for pointer in [ptr::null(), c"\xff".as_ptr()] {
        arguments.argc = 1;
        arguments.argv = &raw const pointer;
        assert_eq!(
            // SAFETY: the array is initialized; null and invalid UTF-8 entries are rejected.
            unsafe { callback(0, ptr::null_mut(), 0, &mut arguments) },
            ffi::RESULT_FAILURE as *mut c_char
        );
    }
    for (index, words, expected) in [
        (0, vec![], ffi::RESULT_SHOWUSAGE),
        (0, vec![c"radioplus", c"active"], ffi::RESULT_FAILURE),
        (
            1,
            vec![c"radioplus", c"channel", c"list"],
            ffi::RESULT_FAILURE,
        ),
    ] {
        let pointers = words.iter().map(|word| word.as_ptr()).collect::<Vec<_>>();
        arguments.argc = pointers.len() as c_int;
        arguments.argv = pointers.as_ptr();
        assert_eq!(
            // SAFETY: the argument array and its C strings remain live through dispatch.
            unsafe { callback(index, ptr::null_mut(), 0, &mut arguments) },
            expected as *mut c_char
        );
    }
    with_state(|state| {
        assert!(
            state
                .messages
                .iter()
                .any(|(_, fd, message)| *fd == 43 && message.contains("No active USB radio"))
        )
    });
}

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
    transmit_results: VecDeque<i32>,
    command_results: VecDeque<i32>,
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
        self.events
            .push(if keyed { "transmit on" } else { "transmit off" });
        self.transmit.push((keyed, ctcss_tenths_hz));
        self.transmit_results
            .pop_front()
            .unwrap_or(self.transmit_result)
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
        self.command_results
            .pop_front()
            .unwrap_or(self.command_result)
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
    // SAFETY: negative count is rejected before the deliberately null pointer is read.
    assert!(unsafe { argument_pointers(std::ptr::null(), -1) }.is_none());
    let pointer = std::ptr::null();
    // SAFETY: pointer is one initialized argv entry.
    let one = unsafe { argument_pointers(&raw const pointer, 1) }.unwrap();
    assert_eq!(one, [std::ptr::null()]);
}

#[test]
fn cli_registration_rejects_a_null_module_owner() {
    assert!(!registration_owner_is_valid(std::ptr::null_mut()));
    assert!(registration_owner_is_valid(std::ptr::dangling_mut::<
        crate::ffi::ast_module,
    >()));
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
    assert_eq!(result, CliResult::success("USB Device Flash completed.\n"));
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

#[test]
fn command_usage_rejects_invalid_arity_and_numeric_arguments() {
    let mut fixture = Fixture::healthy();
    for definition in CLI_DEFINITIONS {
        assert_eq!(run(&mut fixture, definition.kind, &[]), CliResult::usage());
    }
    for (kind, args) in [
        (
            CliCommand::Active,
            vec!["radioplus", "active", "usb", "extra"],
        ),
        (
            CliCommand::ChannelStatus,
            vec!["radioplus", "channel", "status", "invalid"],
        ),
        (
            CliCommand::ChannelEcho,
            vec!["radioplus", "channel", "echo", "2"],
        ),
        (
            CliCommand::ChannelEcho,
            vec!["radioplus", "channel", "echo", "1", "extra"],
        ),
        (
            CliCommand::ChannelTransmit,
            vec!["radioplus", "channel", "transmit", "2"],
        ),
        (
            CliCommand::ChannelTransmit,
            vec!["radioplus", "channel", "transmit", "1", "bad"],
        ),
        (
            CliCommand::ChannelTransmit,
            vec!["radioplus", "channel", "transmit", "1", "0", "extra"],
        ),
    ] {
        assert_eq!(
            run(&mut fixture, kind, &args),
            CliResult::usage(),
            "{args:?}"
        );
    }
    for value in ["", "-1", "not-a-number", "18446744073709551616"] {
        assert_eq!(parse_unsigned(value, u32::MAX), None);
    }
    assert_eq!(parse_unsigned("4294967295", u32::MAX), Some(u32::MAX));
    assert_eq!(parse_unsigned("4294967296", u32::MAX), None);
}

#[test]
fn typed_commands_cover_every_operation_target_and_rejected_tail() {
    let mut fixture = Fixture::healthy();
    for (operation, tail, id, target, value) in [
        (
            "get-mixer",
            vec!["transmit-a"],
            URP_AST_COMMAND_GET_MIXER,
            URP_AST_MIXER_TRANSMIT_A,
            0,
        ),
        (
            "set-mixer",
            vec!["transmit-b", "999"],
            URP_AST_COMMAND_SET_MIXER,
            URP_AST_MIXER_TRANSMIT_B,
            999,
        ),
        ("test-tone", vec!["1"], URP_AST_COMMAND_SET_TEST_TONE, 0, 1),
        ("eeprom-read", vec![], URP_AST_COMMAND_READ_EEPROM, 0, 0),
        (
            "eeprom-save-tuning",
            vec![],
            URP_AST_COMMAND_SAVE_TUNING_EEPROM,
            0,
            0,
        ),
        (
            "ctcss-inhibit",
            vec!["0"],
            URP_AST_COMMAND_SET_CTCSS_INHIBIT,
            0,
            0,
        ),
        (
            "subaudible-override",
            vec!["1"],
            URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE,
            0,
            1,
        ),
    ] {
        let mut args = vec!["radioplus", "channel", "command", operation];
        args.extend(tail);
        assert_eq!(
            run(&mut fixture, CliCommand::ChannelCommand, &args).disposition,
            CliDisposition::Success
        );
        let command = fixture.commands.last().unwrap();
        assert_eq!(
            (command.command, command.target, command.value),
            (id, target, value)
        );
    }
    for (operation, tail) in [
        ("get-mixer", vec![]),
        ("get-mixer", vec!["unknown"]),
        ("set-mixer", vec!["receive"]),
        ("set-mixer", vec!["unknown", "1"]),
        ("test-tone", vec![]),
        ("eeprom-read", vec!["extra"]),
        ("eeprom-write", vec!["3"]),
        ("eeprom-save-tuning", vec!["extra"]),
        ("ctcss-inhibit", vec![]),
        ("ctcss-inhibit", vec!["2"]),
        ("subaudible-override", vec![]),
        ("subaudible-override", vec!["2"]),
        ("unknown", vec![]),
    ] {
        let mut args = vec!["radioplus", "channel", "command", operation];
        args.extend(tail);
        assert_eq!(
            run(&mut fixture, CliCommand::ChannelCommand, &args),
            CliResult::usage(),
            "{args:?}"
        );
    }
    assert_eq!(mixer_target("unknown"), None);
    let too_many = vec!["0"; 65].join(",");
    assert!(parse_eeprom_words(&too_many).is_none());
    let mut invalid_word = vec!["0"; 64];
    invalid_word[20] = "65536";
    assert!(parse_eeprom_words(&invalid_word.join(",")).is_none());
}

#[test]
fn operator_commands_preserve_failure_dispositions_and_immediate_status() {
    let mut fixture = Fixture::healthy();
    fixture.active = None;
    assert_eq!(
        run(&mut fixture, CliCommand::Active, &["radioplus", "active"]).disposition,
        CliDisposition::Failure
    );
    fixture.select_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::Active,
            &["radioplus", "active", "usb"]
        )
        .disposition,
        CliDisposition::Failure
    );
    fixture.status_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelEcho,
            &["radioplus", "channel", "echo"]
        ),
        CliResult::failure("")
    );
    fixture.echo_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelEcho,
            &["radioplus", "channel", "echo", "1"]
        ),
        CliResult::failure("")
    );
    fixture.transmit_result = URP_AST_SETUP_FAILED;
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelTransmit,
            &["radioplus", "channel", "transmit", "1"]
        ),
        CliResult::failure("")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ProcessingStats,
            &["radioplus", "processing", "stats"]
        ),
        CliResult::failure("")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelStatus,
            &["radioplus", "channel", "status", "follow"]
        ),
        CliResult::failure("")
    );
    assert_eq!(fixture.emitted, "Active channel status is unavailable.\n");

    let mut fixture = Fixture::healthy();
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ChannelTransmit,
            &["radioplus", "channel", "transmit", "0", "0"]
        ),
        CliResult::success("transmit_keyed=0\nforced_ctcss_tenths_hz=0\n")
    );
    let status = run(
        &mut fixture,
        CliCommand::ChannelStatus,
        &["radioplus", "channel", "status"],
    );
    assert_eq!(status.disposition, CliDisposition::Success);
    assert!(status.output.starts_with("status_abi="));
    assert!(fixture.emitted.is_empty());
    let stats = run(
        &mut fixture,
        CliCommand::ProcessingStats,
        &["radioplus", "processing", "stats"],
    );
    assert!(
        stats
            .output
            .ends_with("No USBRadioPlus link-processing hook is attached.\n")
    );
    assert_eq!(
        run(
            &mut fixture,
            CliCommand::ProcessingReload,
            &["radioplus", "processing", "reload"]
        ),
        CliResult::success("Configuration reloaded in place.\n")
    );
}

#[test]
fn flash_cleans_up_after_control_failures_and_interrupted_waits() {
    for (commands, transmit, waits, disposition) in [
        (vec![-6], vec![], vec![], CliDisposition::Failure),
        (vec![], vec![-6], vec![], CliDisposition::Failure),
        (vec![], vec![], vec![false], CliDisposition::Success),
        (vec![0, -6], vec![0, -6], vec![], CliDisposition::Failure),
        (vec![0, -6], vec![], vec![], CliDisposition::Failure),
        (vec![], vec![0, -6], vec![], CliDisposition::Failure),
        (vec![], vec![], vec![true, false], CliDisposition::Success),
    ] {
        let mut fixture = Fixture::healthy();
        fixture.command_results = commands.into();
        fixture.transmit_results = transmit.into();
        fixture.wait_results = waits.into();
        let result = run(
            &mut fixture,
            CliCommand::ChannelFlash,
            &["radioplus", "channel", "flash"],
        );
        assert_eq!(result.disposition, disposition);
        assert_eq!(fixture.transmit.last(), Some(&(false, 0)));
        assert_eq!(fixture.commands.last().unwrap().value, 0);
    }
}
