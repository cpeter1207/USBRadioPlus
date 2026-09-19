//! Focused reload-transaction tests.

use super::*;
use crate::URP_AST_OK;

use crate::host::channel::tests::{
    DriverCalls, RegistrationGuard, capture_profile, lifecycle_descriptor, register_fixture,
};
use crate::host::support::{Fixture as HostFixture, with_state};
use std::ffi::c_void;
use std::ptr;

#[derive(Default)]
struct NativeReloadCalls {
    source: Vec<u8>,
    text: Vec<u8>,
    driver_result: i32,
    prepare_result: i32,
    activate_result: i32,
    commit_result: i32,
    cleanup_result: i32,
    finishes: Vec<u32>,
    channel_finishes: Vec<u32>,
}

static RELOAD_CALLS: Mutex<NativeReloadCalls> = Mutex::new(NativeReloadCalls {
    source: Vec::new(),
    text: Vec::new(),
    driver_result: 0,
    prepare_result: 0,
    activate_result: 0,
    commit_result: 0,
    cleanup_result: 0,
    finishes: Vec::new(),
    channel_finishes: Vec::new(),
});

unsafe extern "C" fn native_prepare(
    _: *mut c_void,
    source: *const u8,
    source_len: u32,
    text: *const u8,
    text_len: u32,
) -> i32 {
    let mut calls = RELOAD_CALLS.lock().unwrap();
    // SAFETY: the reload backend retains both complete byte spans through this call.
    unsafe {
        calls.source = std::slice::from_raw_parts(source, source_len as usize).to_vec();
        calls.text = std::slice::from_raw_parts(text, text_len as usize).to_vec();
    }
    calls.driver_result
}

unsafe extern "C" fn native_finish(_: *mut c_void, commit: u32) -> i32 {
    let mut calls = RELOAD_CALLS.lock().unwrap();
    calls.finishes.push(commit);
    if commit != 0 {
        calls.commit_result
    } else {
        calls.cleanup_result
    }
}

unsafe extern "C" fn channel_prepare(_: *mut c_void) -> i32 {
    RELOAD_CALLS.lock().unwrap().prepare_result
}

unsafe extern "C" fn channel_activate(_: *mut c_void) -> i32 {
    RELOAD_CALLS.lock().unwrap().activate_result
}

unsafe extern "C" fn channel_finish(_: *mut c_void, commit: u32) -> i32 {
    let mut calls = RELOAD_CALLS.lock().unwrap();
    calls.channel_finishes.push(commit);
    calls.cleanup_result
}

#[test]
fn configuration_lengths_share_the_complete_abi_bound_and_diagnostics() {
    let _fixture = HostFixture::new();
    for subject in [
        "USBRadioPlus configuration path",
        "/test-config/usbradioplus.conf",
    ] {
        for length in [0, u32::MAX as usize] {
            assert_eq!(validate_configuration_length(length, subject), Ok(()));
        }
        assert_eq!(
            validate_configuration_length(u32::MAX as usize + 1, subject),
            Err(URP_AST_ASTERISK_FAILURE)
        );
    }
    with_state(|state| {
        assert_eq!(state.messages.len(), 2);
        assert_eq!(
            state.messages[0].2,
            "USBRadioPlus configuration path exceeds the adapter ABI\n"
        );
        assert_eq!(
            state.messages[1].2,
            "/test-config/usbradioplus.conf exceeds the adapter ABI\n"
        );
    });
}

#[test]
fn configuration_read_uses_asterisk_directory_and_frees_owned_text() {
    let _fixture = HostFixture::new();
    // SAFETY: this test owns the shared host guard and restores the global immediately.
    unsafe {
        let directory = ffi::ast_config_AST_CONFIG_DIR;
        crate::host::support::ast_config_AST_CONFIG_DIR = ptr::null();
        let missing = read_configuration();
        crate::host::support::ast_config_AST_CONFIG_DIR = directory;
        assert_eq!(missing, Err(URP_AST_ASTERISK_FAILURE));
    }
    assert_eq!(read_configuration(), Err(URP_AST_ASTERISK_FAILURE));
    with_state(|state| state.configuration_text = Some(c"[usb]\n".to_owned()));
    assert_eq!(
        read_configuration(),
        Ok(ReloadConfiguration {
            source: "/test-config/usbradioplus.conf".into(),
            text: b"[usb]\n".to_vec(),
        })
    );
    with_state(|state| {
        assert_eq!(
            state.configuration_paths,
            ["/test-config/usbradioplus.conf"; 2]
        );
        assert_eq!(state.freed_texts, 1);
        assert!(
            state.messages[0]
                .2
                .contains("Unable to read /test-config/usbradioplus.conf")
        );
    });
}

#[test]
fn concrete_reload_preserves_backend_results_and_reopens_admission() {
    let _fixture = HostFixture::new();
    assert_eq!(reload(), URP_AST_ASTERISK_FAILURE);
    let mut descriptor = lifecycle_descriptor();
    descriptor.driver_reload = Some(native_prepare);
    descriptor.driver_reload_finish = Some(native_finish);
    descriptor.driver_channel_name = Some(capture_profile);
    descriptor.channel_reload_prepare = Some(channel_prepare);
    descriptor.channel_reload_activate = Some(channel_activate);
    descriptor.channel_reload_finish = Some(channel_finish);
    let calls = Mutex::new(DriverCalls::default());
    calls.lock().unwrap().profiles.push(b"usb".to_vec());
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    let technology = with_state(|state| state.registered[0]) as *const ffi::ast_channel_tech;
    // SAFETY: registration pins the complete technology table and its capability.
    let owner = unsafe {
        let technology = &*technology;
        technology.requester.unwrap()(
            c"RadioPlus".as_ptr(),
            technology.capabilities,
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        )
    };
    assert!(!owner.is_null());
    assert_eq!(reload(), URP_AST_ASTERISK_FAILURE);
    with_state(|state| state.configuration_text = Some(c"[usb]\n".to_owned()));
    *RELOAD_CALLS.lock().unwrap() = NativeReloadCalls::default();
    // An absent scanner rejects preparation and rolls back the already prepared channel.
    assert_eq!(reload(), URP_AST_ASTERISK_FAILURE);
    assert_eq!(RELOAD_CALLS.lock().unwrap().finishes, [0]);
    let _links = link::tests::RunningFixture::new(false);
    for case in 0..6 {
        let expected = {
            let mut calls = RELOAD_CALLS.lock().unwrap();
            *calls = NativeReloadCalls::default();
            match case {
                0 => {
                    calls.driver_result = -11;
                    -11
                }
                1 => {
                    calls.prepare_result = -12;
                    calls.cleanup_result = -19;
                    -12
                }
                2 => {
                    calls.activate_result = -13;
                    -13
                }
                3 => {
                    calls.commit_result = -14;
                    -14
                }
                4 => {
                    calls.cleanup_result = -19;
                    URP_AST_OK
                }
                _ => URP_AST_OK,
            }
        };
        assert_eq!(reload(), expected);
        let recorded = RELOAD_CALLS.lock().unwrap();
        assert_eq!(recorded.source, b"/test-config/usbradioplus.conf");
        assert_eq!(recorded.text, b"[usb]\n");
        let expected_finishes: &[u32] = match case {
            0 => &[],
            3 => &[1, 0],
            4 | 5 => &[1],
            _ => &[0],
        };
        assert_eq!(recorded.finishes, expected_finishes);
        assert_eq!(
            recorded.channel_finishes,
            match case {
                0 => vec![],
                4 | 5 => vec![1],
                _ => vec![0],
            }
        );
        drop(recorded);
        assert_eq!(
            channel::with_live_channel("usb", channel::service),
            Some(URP_AST_OK)
        );
    }
    with_state(|state| {
        assert!(state.messages.iter().any(|message| {
            message
                .2
                .contains("usb: configuration reload channel rollback failed (-19)")
        }));
        assert!(state.messages.iter().any(|message| {
            message
                .2
                .contains("Configuration reload driver rollback failed (-19)")
        }));
        assert!(state.messages.iter().any(|message| {
            message
                .2
                .contains("usb: configuration reload channel commit failed (-19)")
        }));
    });
    let before = with_state(|state| state.jitter.len());
    channel::with_live_channel("usb", channel::configure_pending_jitter).unwrap();
    assert_eq!(with_state(|state| state.jitter.len()), before + 1);
}

#[test]
fn concrete_reload_returns_link_product_failure_without_publishing() {
    let _fixture = HostFixture::new();
    let mut descriptor = lifecycle_descriptor();
    descriptor.driver_channel_name = Some(capture_profile);
    let calls = Mutex::new(DriverCalls::default());
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    let links = link::tests::RunningFixture::new(true);
    links.preparation_result(-17);
    // A configured live profile is required to stage the eligible link.
    calls.lock().unwrap().profiles.push(b"usb".to_vec());
    let technology = with_state(|state| state.registered[0]) as *const ffi::ast_channel_tech;
    // SAFETY: the guard retains the technology and driver until this owner is hung up.
    let owner = unsafe {
        let technology = &*technology;
        technology.requester.unwrap()(
            c"RadioPlus".as_ptr(),
            technology.capabilities,
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        )
    };
    assert!(!owner.is_null());
    let mut operations = ProductionOperations::new(channel::driver_context().unwrap());
    operations.set_control_gate(true);
    assert_eq!(operations.channel_count(), 1);
    assert_eq!(operations.link_prepare(), -17);
    operations.link_finish(false);
    operations.set_control_gate(false);
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Event {
    Read,
    Gate(bool),
    DriverPrepare,
    ChannelPrepare(usize),
    LinkPrepare,
    ChannelActivate(usize),
    DriverFinish(bool),
    LinkFinish(bool),
    ChannelFinish(usize, bool),
    Jitter(usize),
    Error(&'static str, i32, Option<usize>),
}

struct Fixture {
    events: Vec<Event>,
    channels: usize,
    failure: Option<(Event, i32)>,
}

impl Fixture {
    fn new(channels: usize) -> Self {
        Self {
            events: Vec::new(),
            channels,
            failure: None,
        }
    }

    fn result(&mut self, event: Event) -> i32 {
        self.events.push(event.clone());
        self.failure
            .as_ref()
            .filter(|(failed, _)| *failed == event)
            .map_or(URP_AST_OK, |(_, status)| *status)
    }
}

impl ReloadOperations for Fixture {
    fn read_configuration(&mut self) -> Result<ReloadConfiguration, i32> {
        self.events.push(Event::Read);
        Ok(ReloadConfiguration {
            source: "/etc/asterisk/usbradioplus.conf".into(),
            text: b"[alpha]\n".to_vec(),
        })
    }

    fn set_control_gate(&mut self, blocked: bool) {
        self.events.push(Event::Gate(blocked));
    }

    fn channel_count(&self) -> usize {
        self.channels
    }

    fn driver_prepare(&mut self, _: &ReloadConfiguration) -> i32 {
        self.result(Event::DriverPrepare)
    }

    fn channel_prepare(&mut self, index: usize) -> i32 {
        self.result(Event::ChannelPrepare(index))
    }

    fn link_prepare(&mut self) -> i32 {
        self.result(Event::LinkPrepare)
    }

    fn channel_activate(&mut self, index: usize) -> i32 {
        self.result(Event::ChannelActivate(index))
    }

    fn driver_finish(&mut self, commit: bool) -> i32 {
        self.result(Event::DriverFinish(commit))
    }

    fn link_finish(&mut self, commit: bool) {
        self.events.push(Event::LinkFinish(commit));
    }

    fn channel_finish(&mut self, index: usize, commit: bool) -> i32 {
        self.result(Event::ChannelFinish(index, commit))
    }

    fn mark_jitter_pending(&mut self, index: usize) {
        self.events.push(Event::Jitter(index));
    }

    fn log_error(&mut self, phase: &'static str, status: i32, channel: Option<usize>) {
        self.events.push(Event::Error(phase, status, channel));
    }
}

#[test]
fn successful_reload_publishes_only_after_every_candidate_is_ready() {
    let coordinator = ReloadCoordinator::new();
    let mut fixture = Fixture::new(2);

    assert_eq!(coordinator.reload(&mut fixture), Ok(()));
    assert_eq!(
        fixture.events,
        vec![
            Event::Read,
            Event::Gate(true),
            Event::DriverPrepare,
            Event::ChannelPrepare(0),
            Event::ChannelPrepare(1),
            Event::LinkPrepare,
            Event::ChannelActivate(0),
            Event::ChannelActivate(1),
            Event::DriverFinish(true),
            Event::LinkFinish(true),
            Event::ChannelFinish(0, true),
            Event::ChannelFinish(1, true),
            Event::Jitter(0),
            Event::Jitter(1),
            Event::Gate(false),
        ]
    );
}

#[test]
fn failed_activation_rolls_back_every_channel_before_reopening_control() {
    let coordinator = ReloadCoordinator::new();
    let mut fixture = Fixture::new(2);
    fixture.failure = Some((Event::ChannelActivate(1), -6));

    assert_eq!(
        coordinator.reload(&mut fixture),
        Err(ReloadFailure {
            phase: "channel activate",
            status: -6,
            channel: Some(1),
        })
    );
    assert_eq!(
        fixture.events,
        vec![
            Event::Read,
            Event::Gate(true),
            Event::DriverPrepare,
            Event::ChannelPrepare(0),
            Event::ChannelPrepare(1),
            Event::LinkPrepare,
            Event::ChannelActivate(0),
            Event::ChannelActivate(1),
            Event::LinkFinish(false),
            Event::ChannelFinish(0, false),
            Event::ChannelFinish(1, false),
            Event::DriverFinish(false),
            Event::Gate(false),
        ]
    );
}

#[test]
fn rollback_degradation_is_logged_without_hiding_the_original_failure() {
    let coordinator = ReloadCoordinator::new();
    let mut fixture = Fixture::new(1);
    fixture.failure = Some((Event::ChannelPrepare(0), -7));
    // A second fixture implementation behavior is unnecessary: changing the
    // selected failure after prepare is represented by its finish method.
    struct Degraded(Fixture);
    impl ReloadOperations for Degraded {
        fn read_configuration(&mut self) -> Result<ReloadConfiguration, i32> {
            self.0.read_configuration()
        }
        fn set_control_gate(&mut self, blocked: bool) {
            self.0.set_control_gate(blocked);
        }
        fn channel_count(&self) -> usize {
            self.0.channel_count()
        }
        fn driver_prepare(&mut self, config: &ReloadConfiguration) -> i32 {
            self.0.driver_prepare(config)
        }
        fn channel_prepare(&mut self, index: usize) -> i32 {
            self.0.channel_prepare(index)
        }
        fn link_prepare(&mut self) -> i32 {
            self.0.link_prepare()
        }
        fn channel_activate(&mut self, index: usize) -> i32 {
            self.0.channel_activate(index)
        }
        fn driver_finish(&mut self, commit: bool) -> i32 {
            self.0.events.push(Event::DriverFinish(commit));
            if commit { URP_AST_OK } else { -8 }
        }
        fn link_finish(&mut self, commit: bool) {
            self.0.link_finish(commit);
        }
        fn channel_finish(&mut self, index: usize, commit: bool) -> i32 {
            self.0.events.push(Event::ChannelFinish(index, commit));
            if commit { URP_AST_OK } else { -9 }
        }
        fn mark_jitter_pending(&mut self, index: usize) {
            self.0.mark_jitter_pending(index);
        }
        fn log_error(&mut self, phase: &'static str, status: i32, channel: Option<usize>) {
            self.0.log_error(phase, status, channel);
        }
    }
    let mut degraded = Degraded(fixture);

    assert_eq!(
        coordinator.reload(&mut degraded),
        Err(ReloadFailure {
            phase: "channel prepare",
            status: -7,
            channel: Some(0),
        })
    );
    assert!(
        degraded
            .0
            .events
            .contains(&Event::Error("channel rollback", -9, Some(0)))
    );
    assert!(
        degraded
            .0
            .events
            .contains(&Event::Error("driver rollback", -8, None))
    );
}

#[test]
fn channel_reload_diagnostic_names_the_failed_channel() {
    assert_eq!(
        reload_failure_diagnostic("channel prepare", -7, Some("alpha")),
        "alpha: configuration reload channel prepare failed (-7)"
    );
    assert_eq!(
        reload_failure_diagnostic("driver prepare", -8, None),
        "Configuration reload driver prepare failed (-8)"
    );
}
