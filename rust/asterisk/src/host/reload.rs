//! Serialized, all-or-nothing configuration reload coordination.

use std::sync::Mutex;

use crate::URP_AST_OK;

use std::ffi::{CStr, CString, c_int};
use std::path::PathBuf;

use super::{channel, link};
use crate::{URP_AST_ASTERISK_FAILURE, ffi};

/// Complete configuration input selected before a reload transaction begins.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct ReloadConfiguration {
    /// Path reported to configuration diagnostics.
    pub(super) source: String,
    /// Complete configuration-file bytes.
    pub(super) text: Vec<u8>,
}

/// One failed reload phase and its portable status.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct ReloadFailure {
    /// Stable phase name for diagnostics.
    pub(super) phase: &'static str,
    /// Portable adapter status returned by that phase.
    pub(super) status: i32,
    /// Frozen channel index for a channel-specific phase.
    pub(super) channel: Option<usize>,
}

/// Host operations used by the transaction coordinator.
///
/// `set_control_gate(true)` freezes channel membership and rejects ordinary
/// control admission until the matching `false` call.
pub(super) trait ReloadOperations {
    /// Read the complete selected configuration.
    fn read_configuration(&mut self) -> Result<ReloadConfiguration, i32>;
    /// Close or reopen ordinary control admission.
    fn set_control_gate(&mut self, blocked: bool);
    /// Number of membership-frozen live radio channels.
    fn channel_count(&self) -> usize;
    /// Stage the driver generation.
    fn driver_prepare(&mut self, configuration: &ReloadConfiguration) -> i32;
    /// Prepare one live channel against the staged generation.
    fn channel_prepare(&mut self, index: usize) -> i32;
    /// Prepare every attached incoming-link graph.
    fn link_prepare(&mut self) -> i32;
    /// Adopt one prepared channel while retaining rollback ownership.
    fn channel_activate(&mut self, index: usize) -> i32;
    /// Publish or discard the staged driver generation.
    fn driver_finish(&mut self, commit: bool) -> i32;
    /// Publish or discard every staged incoming-link graph.
    fn link_finish(&mut self, commit: bool);
    /// Commit or roll back one live channel.
    fn channel_finish(&mut self, index: usize, commit: bool) -> i32;
    /// Defer Asterisk jitter reconfiguration to the channel owner.
    fn mark_jitter_pending(&mut self, index: usize);
    /// Report degraded cleanup without replacing the original failure.
    fn log_error(&mut self, phase: &'static str, status: i32, channel: Option<usize>);
}

/// Serializes complete reload transactions.
pub(super) struct ReloadCoordinator {
    transaction: Mutex<()>,
}

impl ReloadCoordinator {
    /// Construct an idle reload coordinator.
    pub(super) const fn new() -> Self {
        Self {
            transaction: Mutex::new(()),
        }
    }

    /// Prepare every candidate, then commit all of them or roll all of them back.
    pub(super) fn reload(
        &self,
        operations: &mut impl ReloadOperations,
    ) -> Result<(), ReloadFailure> {
        let _transaction = self
            .transaction
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let configuration = operations
            .read_configuration()
            .map_err(|status| ReloadFailure {
                phase: "configuration read",
                status,
                channel: None,
            })?;
        operations.set_control_gate(true);
        let channels = operations.channel_count();
        let result = self.reload_gated(operations, &configuration, channels);
        operations.set_control_gate(false);
        result
    }

    fn reload_gated(
        &self,
        operations: &mut impl ReloadOperations,
        configuration: &ReloadConfiguration,
        channels: usize,
    ) -> Result<(), ReloadFailure> {
        let status = operations.driver_prepare(configuration);
        if status != URP_AST_OK {
            return Err(ReloadFailure {
                phase: "driver prepare",
                status,
                channel: None,
            });
        }

        let result = (|| {
            for index in 0..channels {
                check(
                    "channel prepare",
                    operations.channel_prepare(index),
                    Some(index),
                )?;
            }
            check("link prepare", operations.link_prepare(), None)?;
            for index in 0..channels {
                check(
                    "channel activate",
                    operations.channel_activate(index),
                    Some(index),
                )?;
            }
            check("driver commit", operations.driver_finish(true), None)
        })();

        if let Err(failure) = result {
            operations.link_finish(false);
            for index in 0..channels {
                let status = operations.channel_finish(index, false);
                if status != URP_AST_OK {
                    operations.log_error("channel rollback", status, Some(index));
                }
            }
            let status = operations.driver_finish(false);
            if status != URP_AST_OK {
                operations.log_error("driver rollback", status, None);
            }
            return Err(failure);
        }

        operations.link_finish(true);
        for index in 0..channels {
            let status = operations.channel_finish(index, true);
            if status != URP_AST_OK {
                operations.log_error("channel commit", status, Some(index));
            }
        }
        for index in 0..channels {
            operations.mark_jitter_pending(index);
        }
        Ok(())
    }
}

fn check(phase: &'static str, status: i32, channel: Option<usize>) -> Result<(), ReloadFailure> {
    if status == URP_AST_OK {
        Ok(())
    } else {
        Err(ReloadFailure {
            phase,
            status,
            channel,
        })
    }
}

static RELOAD_COORDINATOR: ReloadCoordinator = ReloadCoordinator::new();

struct ProductionOperations {
    driver: channel::DriverContext,
    channels: Option<channel::LiveChannelsGuard>,
    links: Option<link::LinkReload>,
}

impl ProductionOperations {
    fn new(driver: channel::DriverContext) -> Self {
        Self {
            driver,
            channels: None,
            links: None,
        }
    }

    fn channel(&self, index: usize) -> &channel::Channel {
        self.channels
            .as_ref()
            .expect("reload control gate is closed")
            .iter()
            .nth(index)
            .expect("reload channel index came from the frozen membership")
    }

    fn descriptor(&self) -> &crate::UrpAstDescriptor {
        // SAFETY: channel registration validated this process-lifetime descriptor.
        unsafe { &*self.driver.descriptor }
    }
}

impl ReloadOperations for ProductionOperations {
    fn read_configuration(&mut self) -> Result<ReloadConfiguration, i32> {
        read_configuration()
    }

    fn set_control_gate(&mut self, blocked: bool) {
        if blocked {
            self.channels = Some(channel::lock_live_channels());
        } else {
            self.channels
                .as_mut()
                .expect("reload control gate is closed")
                .reopen_control();
        }
    }

    fn channel_count(&self) -> usize {
        self.channels
            .as_ref()
            .expect("reload control gate is closed")
            .iter()
            .count()
    }

    fn driver_prepare(&mut self, configuration: &ReloadConfiguration) -> i32 {
        let call = self
            .descriptor()
            .driver_reload
            .expect("registration validated driver_reload");
        // SAFETY: the driver and descriptor remain installed; both byte spans
        // remain live for the duration of this synchronous call.
        unsafe {
            call(
                self.driver.driver,
                configuration.source.as_ptr(),
                configuration.source.len() as u32,
                configuration.text.as_ptr(),
                configuration.text.len() as u32,
            )
        }
    }

    fn channel_prepare(&mut self, index: usize) -> i32 {
        channel::reload_prepare(self.channel(index))
    }

    fn link_prepare(&mut self) -> i32 {
        let profile = self
            .channels
            .as_ref()
            .expect("reload control gate is closed")
            .first_profile(self.driver);
        match link::reload_prepare(profile.as_deref()) {
            Ok(reload) => {
                self.links = Some(reload);
                URP_AST_OK
            }
            Err(link::LinkHostError::Asterisk) => URP_AST_ASTERISK_FAILURE,
            Err(link::LinkHostError::Product(status)) => status,
        }
    }

    fn channel_activate(&mut self, index: usize) -> i32 {
        channel::reload_activate(self.channel(index))
    }

    fn driver_finish(&mut self, commit: bool) -> i32 {
        let call = self
            .descriptor()
            .driver_reload_finish
            .expect("registration validated driver_reload_finish");
        // SAFETY: the driver and descriptor remain installed through reload.
        unsafe { call(self.driver.driver, u32::from(commit)) }
    }

    fn link_finish(&mut self, commit: bool) {
        if let Some(reload) = self.links.take() {
            reload.finish(commit);
        }
    }

    fn channel_finish(&mut self, index: usize, commit: bool) -> i32 {
        channel::reload_finish(self.channel(index), commit)
    }

    fn mark_jitter_pending(&mut self, index: usize) {
        channel::mark_jitter_pending(self.channel(index));
    }

    fn log_error(&mut self, phase: &'static str, status: i32, channel: Option<usize>) {
        let channel = channel.map(|index| self.channel(index).name());
        log_error(&reload_failure_diagnostic(phase, status, channel));
    }
}

/// Read the complete selected configuration through Asterisk's path policy.
pub(super) fn read_configuration() -> Result<ReloadConfiguration, i32> {
    // SAFETY: Asterisk initializes its immutable configuration-directory
    // pointer before loading channel modules.
    let directory = unsafe { ffi::ast_config_AST_CONFIG_DIR };
    if directory.is_null() {
        return Err(URP_AST_ASTERISK_FAILURE);
    }
    // SAFETY: the Asterisk path is a process-lifetime NUL-terminated string.
    let directory = unsafe { CStr::from_ptr(directory) }.to_string_lossy();
    let source = PathBuf::from(directory.as_ref())
        .join("usbradioplus.conf")
        .to_string_lossy()
        .into_owned();
    if source.len() > u32::MAX as usize {
        log_error("USBRadioPlus configuration path exceeds the adapter ABI");
        return Err(URP_AST_ASTERISK_FAILURE);
    }
    let path = CString::new(source.as_bytes()).map_err(|_| URP_AST_ASTERISK_FAILURE)?;
    // SAFETY: path is NUL terminated and Asterisk returns either null or one
    // allocation owned by Asterisk.
    let text = unsafe { ffi::ast_read_textfile(path.as_ptr()) };
    if text.is_null() {
        log_error(&format!("Unable to read {source}"));
        return Err(URP_AST_ASTERISK_FAILURE);
    }
    // SAFETY: ast_read_textfile returned a NUL-terminated allocation.
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes().to_vec();
    // SAFETY: this is the allocation returned by ast_read_textfile and it is
    // no longer borrowed after copying its contents.
    unsafe { ffi::ast_free_ptr(text.cast()) };
    if bytes.len() > u32::MAX as usize {
        log_error(&format!("{source} exceeds the adapter ABI"));
        return Err(URP_AST_ASTERISK_FAILURE);
    }
    Ok(ReloadConfiguration {
        source,
        text: bytes,
    })
}

/// Atomically replace the active configuration generation.
pub(super) fn reload() -> i32 {
    let Some(driver) = channel::driver_context() else {
        return URP_AST_ASTERISK_FAILURE;
    };
    let mut operations = ProductionOperations::new(driver);
    match RELOAD_COORDINATOR.reload(&mut operations) {
        Ok(()) => URP_AST_OK,
        Err(failure) => {
            operations.log_error(failure.phase, failure.status, failure.channel);
            failure.status
        }
    }
}

pub(super) fn reload_failure_diagnostic(phase: &str, status: i32, channel: Option<&str>) -> String {
    match channel {
        Some(channel) => {
            format!("{channel}: configuration reload {phase} failed ({status})")
        }
        None => format!("Configuration reload {phase} failed ({status})"),
    }
}

fn log_error(message: &str) {
    // SAFETY: message length bounds the readable byte span and all variadic
    // arguments match Asterisk's public logging declaration.
    unsafe {
        ffi::ast_log(
            ffi::__LOG_ERROR as c_int,
            c"usbradioplus-rust-host".as_ptr(),
            0,
            c"reload".as_ptr(),
            c"%.*s\n".as_ptr(),
            message.len().min(c_int::MAX as usize) as c_int,
            message.as_ptr().cast::<i8>(),
        )
    };
}
