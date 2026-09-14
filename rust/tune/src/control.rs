//! Interactive-session locking and typed Asterisk runtime control.

use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io;
#[cfg(unix)]
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use usbradioplus_core::CtcssTone;

use crate::menus::{
    ApplyMode, ChannelControl, ConfigurationApplier, HardwareMixer, LiveAction, RadioStatus,
};

/// Live tuner control implemented through Asterisk's local command interface.
///
/// This is intentionally the only tuner component which knows Asterisk's CLI
/// spelling. The menu itself continues to operate on typed semantic actions.
#[derive(Clone, Debug)]
pub struct AsteriskRuntime {
    asterisk: PathBuf,
    systemctl: PathBuf,
}

impl Default for AsteriskRuntime {
    fn default() -> Self {
        Self {
            asterisk: PathBuf::from("asterisk"),
            systemctl: PathBuf::from("systemctl"),
        }
    }
}

impl AsteriskRuntime {
    /// Use explicit Asterisk and service-manager executable paths.
    ///
    /// The packaged utility uses [`Default`]; this constructor keeps command
    /// execution deterministic for alternate filesystem layouts and tests.
    pub fn new(asterisk: impl Into<PathBuf>, systemctl: impl Into<PathBuf>) -> Self {
        Self {
            asterisk: asterisk.into(),
            systemctl: systemctl.into(),
        }
    }

    fn run_asterisk(&self, command: &str) -> Result<String, String> {
        command_output(Command::new(&self.asterisk).args(["-rx", command]).output())
    }

    fn run_systemctl_restart(&self) -> Result<String, String> {
        command_output(
            Command::new(&self.systemctl)
                .args(["restart", "asterisk"])
                .output(),
        )
    }

    fn channel_status(&self) -> Result<String, String> {
        self.run_asterisk("radioplus channel status")
    }
}

impl ConfigurationApplier for AsteriskRuntime {
    fn apply(&mut self, mode: ApplyMode) -> Result<(), String> {
        match mode {
            ApplyMode::Reload => {
                let output = self.run_asterisk("radioplus processing reload")?;
                if output.to_ascii_lowercase().contains("reloaded in place") {
                    Ok(())
                } else {
                    Err(format!("processing reload failed\n\n{output}"))
                }
            }
            ApplyMode::Restart => {
                let restart = self.run_systemctl_restart()?;
                let health = self.run_asterisk("module show like chan_usbradioplus")?;
                if health.contains("chan_usbradioplus.so") && health.contains("Running") {
                    Ok(())
                } else {
                    Err(format!(
                        "Asterisk restart did not leave USBRadioPlus running\n\n{restart}{health}"
                    ))
                }
            }
        }
    }
}

impl ChannelControl for AsteriskRuntime {
    fn active_channel(&mut self) -> Result<Option<String>, String> {
        let output = self.run_asterisk("radioplus active")?;
        Ok(bracketed_value(&output).map(str::to_owned))
    }

    fn select_channel(&mut self, channel: &str) -> Result<(), String> {
        let output = self.run_asterisk(&format!("radioplus active {channel}"))?;
        if output.to_ascii_lowercase().contains("set to") {
            Ok(())
        } else {
            Err(if output.trim().is_empty() {
                format!("unable to select radio channel {channel}")
            } else {
                output
            })
        }
    }

    fn radio_status(&mut self) -> Result<RadioStatus, String> {
        let output = self.channel_status()?;
        Ok(RadioStatus {
            receive_input_peak: parsed_value(&output, "receive_input_peak")?,
            receive_input_rms: parsed_value(&output, "receive_input_rms")?,
            receive_output_peak: parsed_value(&output, "receive_output_peak")?,
            receive_output_rms: parsed_value(&output, "receive_output_rms")?,
            receive_ctcss_decoder_peak: parsed_value(&output, "receive_ctcss_decoder_peak")?,
            receive_rssi_peak: parsed_value(&output, "receive_rssi_peak")?,
            receive_rssi_updated: parsed_switch(&output, "receive_rssi_updated")?,
            receive_input_rail_samples: parsed_value(&output, "receive_input_rail_samples")?,
            receive_mixer_level: parsed_value(&output, "receive_mixer_level")?,
        })
    }

    fn set_hardware_mixer(&mut self, mixer: HardwareMixer, level: u32) -> Result<(), String> {
        let target = match mixer {
            HardwareMixer::Receive => "receive",
            HardwareMixer::TransmitA => "transmit-a",
            HardwareMixer::TransmitB => "transmit-b",
        };
        self.run_asterisk(&format!(
            "radioplus channel command set-mixer {target} {level}"
        ))?;
        Ok(())
    }

    fn set_test_tone(&mut self, enabled: bool) -> Result<(), String> {
        self.run_asterisk(&format!(
            "radioplus channel command test-tone {}",
            u8::from(enabled)
        ))?;
        Ok(())
    }

    fn set_transmit(&mut self, keyed: bool, forced_ctcss: Option<CtcssTone>) -> Result<(), String> {
        let mut command = format!("radioplus channel transmit {}", u8::from(keyed));
        if let Some(tone) = forced_ctcss {
            command.push_str(&format!(" {}", tone.tenths_hz()));
        }
        self.run_asterisk(&command)?;
        Ok(())
    }

    fn save_current_tuning_to_eeprom(&mut self) -> Result<String, String> {
        self.run_asterisk("radioplus channel command eeprom-save-tuning")
    }

    fn wait(&mut self, duration: std::time::Duration) {
        std::thread::sleep(duration);
    }

    fn perform(&mut self, action: LiveAction) -> Result<String, String> {
        match action {
            LiveAction::ProcessingStatistics => self.run_asterisk("radioplus processing stats"),
            LiveAction::ActiveConfiguration | LiveAction::RadioStatus => self.channel_status(),
            LiveAction::FlashTransmitter => self.run_asterisk("radioplus channel flash"),
            LiveAction::CalibrateReceiveNoise
            | LiveAction::CalibrateReceiveVoice
            | LiveAction::CalibrateReceiveCtcss => Err(
                "the Rust tuner must compose this operation from typed live controls".to_owned(),
            ),
            LiveAction::SaveRadioTuning => self.save_current_tuning_to_eeprom(),
            LiveAction::ContinuousRadioMeters => {
                self.run_asterisk("radioplus channel status follow")
            }
        }
    }

    fn echo_enabled(&mut self) -> Result<bool, String> {
        let output = self.run_asterisk("radioplus channel echo")?;
        match key_value(&output, "echo_enabled") {
            "0" => Ok(false),
            "1" => Ok(true),
            _ => Err("active radio returned an invalid echo state".to_owned()),
        }
    }

    fn set_echo_enabled(&mut self, enabled: bool) -> Result<String, String> {
        self.run_asterisk(&format!("radioplus channel echo {}", u8::from(enabled)))
    }
}

fn key_value<'a>(output: &'a str, key: &str) -> &'a str {
    output
        .lines()
        .filter_map(|line| line.split_once('='))
        .find_map(|(found, value)| (found.trim() == key).then_some(value.trim()))
        .unwrap_or_default()
}

fn parsed_value<T>(output: &str, key: &str) -> Result<T, String>
where
    T: std::str::FromStr,
{
    key_value(output, key)
        .parse()
        .map_err(|_| format!("active radio returned an invalid {key} value"))
}

fn parsed_switch(output: &str, key: &str) -> Result<bool, String> {
    match key_value(output, key) {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => Err(format!("active radio returned an invalid {key} value")),
    }
}

fn command_output(result: io::Result<Output>) -> Result<String, String> {
    let output = result.map_err(|error| format!("start command: {error}"))?;
    let mut text = String::from_utf8_lossy(&output.stdout).into_owned();
    text.push_str(&String::from_utf8_lossy(&output.stderr));
    if output.status.success() {
        Ok(text)
    } else if text.trim().is_empty() {
        Err(format!("command exited with {}", output.status))
    } else {
        Err(text)
    }
}

fn bracketed_value(output: &str) -> Option<&str> {
    let start = output.find('[')? + 1;
    let end = output[start..].find(']')? + start;
    (end > start).then(|| &output[start..end])
}

/// Held, nonblocking advisory lock for one interactive tuning session.
#[derive(Debug)]
pub struct SessionLock {
    _file: File,
    path: PathBuf,
}

impl SessionLock {
    /// Create and exclusively lock the selected session file.
    ///
    /// # Errors
    ///
    /// Returns [`SessionLockError::AlreadyRunning`] when another tuner owns the
    /// lock, or [`SessionLockError::Io`] when the path cannot be created or locked.
    pub fn acquire(path: &Path) -> Result<Self, SessionLockError> {
        let path = path.to_path_buf();
        let parent = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(parent).map_err(|source| SessionLockError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
        let file = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(&path)
            .map_err(|source| SessionLockError::Io {
                path: path.clone(),
                source,
            })?;
        lock_file(&file).map_err(|source| lock_error(&path, source))?;
        Ok(Self { _file: file, path })
    }

    /// Return the lock-file path retained by this session.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }
}

fn lock_error(path: &Path, source: io::Error) -> SessionLockError {
    if source.kind() == io::ErrorKind::WouldBlock {
        SessionLockError::AlreadyRunning(path.to_path_buf())
    } else {
        SessionLockError::Io {
            path: path.to_path_buf(),
            source,
        }
    }
}

/// Failure to acquire the interactive-session lock.
#[derive(Debug)]
pub enum SessionLockError {
    /// Another process already owns the advisory lock.
    AlreadyRunning(PathBuf),
    /// Lock-file creation or locking failed.
    Io {
        /// File or directory involved in the failure.
        path: PathBuf,
        /// Operating-system error.
        source: io::Error,
    },
}

impl fmt::Display for SessionLockError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AlreadyRunning(_) => {
                formatter.write_str("another USBRadioPlus tuning menu is already running")
            }
            Self::Io { path, source } => {
                write!(
                    formatter,
                    "access session lock {}: {source}",
                    path.display()
                )
            }
        }
    }
}

impl std::error::Error for SessionLockError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io { source, .. } => Some(source),
            Self::AlreadyRunning(_) => None,
        }
    }
}

#[cfg(unix)]
fn lock_file(file: &File) -> io::Result<()> {
    const LOCK_EX: i32 = 2;
    const LOCK_NB: i32 = 4;
    unsafe extern "C" {
        fn flock(fd: i32, operation: i32) -> i32;
    }
    // SAFETY: `file` owns a valid open descriptor for this call's duration;
    // `flock` neither retains the descriptor nor dereferences process memory.
    let result = unsafe { flock(file.as_raw_fd(), LOCK_EX | LOCK_NB) };
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[cfg(not(unix))]
fn lock_file(_file: &File) -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "session locking is supported only on Unix",
    ))
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;
    use std::sync::atomic::{AtomicU64, Ordering};

    use super::*;

    static LOCK_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    #[cfg(unix)]
    fn executable(directory: &Path, name: &str, source: &str) -> PathBuf {
        let path = directory.join(name);
        fs::write(&path, source).unwrap();
        let mut permissions = fs::metadata(&path).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&path, permissions).unwrap();
        path
    }

    #[cfg(unix)]
    #[test]
    fn asterisk_runtime_maps_every_semantic_operation() {
        let defaults = AsteriskRuntime::default();
        assert_eq!(defaults.asterisk, PathBuf::from("asterisk"));
        assert_eq!(defaults.systemctl, PathBuf::from("systemctl"));
        let root = std::env::temp_dir().join(format!(
            "usbradioplus-tune-runtime-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        let script = executable(
            &root,
            "control",
            "#!/bin/sh\n\
             case \"$*\" in\n\
             '-rx radioplus processing reload') echo 'Configuration reloaded in place' ;;\n\
             '-rx module show like chan_usbradioplus') echo 'chan_usbradioplus.so Running' ;;\n\
             '-rx radioplus active') echo 'Active USB Radio device is [radio].' ;;\n\
             '-rx radioplus active radio') echo 'Active radio set to [radio]' ;;\n\
             '-rx radioplus channel echo') echo 'echo_enabled=1' ;;\n\
             '-rx radioplus channel status') printf '%s\\n' \\
               'receive_input_peak=0.75' \\
               'receive_input_rms=0.25' \\
               'receive_output_peak=0.5' \\
               'receive_output_rms=0.125' \\
               'receive_ctcss_decoder_peak=0.0625' \\
               'receive_rssi_peak=1234' \\
               'receive_rssi_updated=1' \\
               'receive_input_rail_samples=3' \\
               'receive_mixer_level=625' ;;\n\
             *) printf '%s\\n' \"$*\" ;;\n\
             esac\n",
        );
        let mut runtime = AsteriskRuntime::new(&script, &script);

        assert_eq!(runtime.active_channel().unwrap().as_deref(), Some("radio"));
        runtime.select_channel("radio").unwrap();
        assert!(runtime.echo_enabled().unwrap());
        assert!(!runtime.set_echo_enabled(false).unwrap().is_empty());
        assert!(!runtime.set_echo_enabled(true).unwrap().is_empty());
        assert_eq!(
            runtime.radio_status().unwrap(),
            RadioStatus {
                receive_input_peak: 0.75,
                receive_input_rms: 0.25,
                receive_output_peak: 0.5,
                receive_output_rms: 0.125,
                receive_ctcss_decoder_peak: 0.0625,
                receive_rssi_peak: 1234,
                receive_rssi_updated: true,
                receive_input_rail_samples: 3,
                receive_mixer_level: 625,
            }
        );
        runtime
            .set_hardware_mixer(HardwareMixer::Receive, 625)
            .unwrap();
        runtime
            .set_hardware_mixer(HardwareMixer::TransmitA, 500)
            .unwrap();
        runtime
            .set_hardware_mixer(HardwareMixer::TransmitB, 750)
            .unwrap();
        runtime.set_test_tone(true).unwrap();
        runtime.set_test_tone(false).unwrap();
        runtime.set_transmit(true, None).unwrap();
        runtime
            .set_transmit(true, CtcssTone::from_tenths_hz(1_000))
            .unwrap();
        runtime.set_transmit(false, None).unwrap();
        runtime.wait(std::time::Duration::ZERO);
        runtime.apply(ApplyMode::Reload).unwrap();
        runtime.apply(ApplyMode::Restart).unwrap();
        for action in [
            LiveAction::ProcessingStatistics,
            LiveAction::ActiveConfiguration,
            LiveAction::RadioStatus,
            LiveAction::FlashTransmitter,
            LiveAction::ContinuousRadioMeters,
        ] {
            assert!(!runtime.perform(action).unwrap().is_empty());
        }
        for action in [
            LiveAction::CalibrateReceiveNoise,
            LiveAction::CalibrateReceiveVoice,
            LiveAction::CalibrateReceiveCtcss,
        ] {
            assert!(runtime.perform(action).is_err());
        }
        assert!(
            !runtime
                .perform(LiveAction::SaveRadioTuning)
                .unwrap()
                .is_empty()
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn forced_ctcss_is_sent_as_tenths_of_a_hertz() {
        let root = std::env::temp_dir().join(format!(
            "usbradioplus-tune-forced-ctcss-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        let script = executable(
            &root,
            "control",
            "#!/bin/sh\n[ \"$*\" = '-rx radioplus channel transmit 1 1000' ]\n",
        );
        let mut runtime = AsteriskRuntime::new(&script, &script);
        runtime
            .set_transmit(true, CtcssTone::from_tenths_hz(1_000))
            .unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn asterisk_runtime_reports_command_and_response_failures() {
        let root = std::env::temp_dir().join(format!(
            "usbradioplus-tune-runtime-errors-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        let empty = executable(&root, "empty", "#!/bin/sh\nexit 0\n");
        let failed = executable(
            &root,
            "failed",
            "#!/bin/sh\nprintf 'failed output' >&2\nexit 1\n",
        );
        let failed_empty = executable(&root, "failed-empty", "#!/bin/sh\nexit 1\n");
        let malformed = executable(&root, "malformed", "#!/bin/sh\necho 'not a state'\n");
        let not_running = executable(
            &root,
            "not-running",
            "#!/bin/sh\necho 'chan_usbradioplus.so Stopped'\n",
        );
        let bad_echo = executable(
            &root,
            "bad-echo",
            "#!/bin/sh\necho '0,1,x,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22'\n",
        );

        let mut runtime = AsteriskRuntime::new(&malformed, &empty);
        assert_eq!(runtime.active_channel().unwrap(), None);
        assert!(runtime.select_channel("missing").is_err());
        assert!(runtime.apply(ApplyMode::Reload).is_err());
        assert!(runtime.apply(ApplyMode::Restart).is_err());
        assert!(runtime.echo_enabled().is_err());
        assert!(runtime.radio_status().is_err());
        runtime.asterisk = bad_echo;
        assert!(runtime.echo_enabled().is_err());
        runtime.asterisk = empty.clone();
        assert!(runtime.select_channel("missing").is_err());
        runtime.systemctl = failed.clone();
        assert!(runtime.apply(ApplyMode::Restart).is_err());
        runtime.systemctl = empty.clone();
        runtime.asterisk = not_running;
        assert!(runtime.apply(ApplyMode::Restart).is_err());

        assert!(command_output(Err(io::Error::other("unavailable"))).is_err());
        assert!(command_output(Command::new(&failed).output()).is_err());
        assert!(command_output(Command::new(&failed_empty).output()).is_err());
        assert_eq!(bracketed_value("[]"), None);
        assert_eq!(bracketed_value("missing"), None);
        assert!(parsed_switch("state=maybe", "state").is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn session_lock_is_nonblocking_and_released_with_its_owner() {
        let path = std::env::temp_dir().join(format!(
            "usbradioplus-tune-lock-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        let lock = SessionLock::acquire(&path).expect("first lock must succeed");
        assert_eq!(lock.path(), path);
        let error = SessionLock::acquire(&path).expect_err("second lock must fail");
        assert!(matches!(error, SessionLockError::AlreadyRunning(_)));
        assert_eq!(
            error.to_string(),
            "another USBRadioPlus tuning menu is already running"
        );
        assert!(std::error::Error::source(&error).is_none());
        drop(lock);
        assert!(SessionLock::acquire(&path).is_ok());
        fs::remove_file(path).expect("test lock cleanup must succeed");
    }

    #[test]
    fn session_lock_reports_filesystem_and_lock_errors() {
        let root = std::env::temp_dir().join(format!(
            "usbradioplus-tune-lock-errors-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();

        let parent_file = root.join("not-a-directory");
        fs::write(&parent_file, "x").unwrap();
        let error = SessionLock::acquire(&parent_file.join("lock")).unwrap_err();
        assert!(matches!(error, SessionLockError::Io { .. }));
        assert!(error.to_string().contains("access session lock"));
        assert!(std::error::Error::source(&error).is_some());

        let error = SessionLock::acquire(&root).unwrap_err();
        assert!(matches!(error, SessionLockError::Io { .. }));

        let path = root.join("lock");
        assert!(matches!(
            lock_error(&path, io::Error::other("failed")),
            SessionLockError::Io { .. }
        ));
        assert!(matches!(
            lock_error(&path, io::Error::from(io::ErrorKind::WouldBlock)),
            SessionLockError::AlreadyRunning(_)
        ));
        let relative = PathBuf::from(format!(
            "usbradioplus-relative-lock-{}-{}",
            std::process::id(),
            LOCK_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        let lock = SessionLock::acquire(&relative).unwrap();
        drop(lock);
        fs::remove_file(relative).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
