//! Focused reload-transaction tests.

use super::reload::*;
use crate::URP_AST_OK;

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
