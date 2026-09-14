//! Testable prerequisite and terminal policy for the tuning utility.

use std::env;
use std::fmt;
use std::io::{self, IsTerminal};
use std::path::Path;

/// Operating-system information required by startup policy.
pub trait RuntimeEnvironment {
    /// Return the effective numeric user identifier.
    fn effective_user_id(&self) -> u32;
    /// Report whether an executable can be invoked through `PATH`.
    fn command_available(&self, command: &str) -> bool;
    /// Report whether standard input is attached to a terminal.
    fn stdin_is_terminal(&self) -> bool;
    /// Report whether standard output is attached to a terminal.
    fn stdout_is_terminal(&self) -> bool;
    /// Report whether standard error is attached to a terminal.
    fn stderr_is_terminal(&self) -> bool;
    /// Return the terminal type supplied through `TERM`.
    fn terminal_type(&self) -> Option<String>;
}

/// Runtime environment backed by the host operating system.
#[derive(Clone, Copy, Debug, Default)]
pub struct SystemEnvironment;

impl RuntimeEnvironment for SystemEnvironment {
    fn effective_user_id(&self) -> u32 {
        effective_user_id()
    }

    fn command_available(&self, command: &str) -> bool {
        command_available(command)
    }

    fn stdin_is_terminal(&self) -> bool {
        io::stdin().is_terminal()
    }

    fn stdout_is_terminal(&self) -> bool {
        io::stdout().is_terminal()
    }

    fn stderr_is_terminal(&self) -> bool {
        io::stderr().is_terminal()
    }

    fn terminal_type(&self) -> Option<String> {
        env::var("TERM").ok()
    }
}

/// Startup policy failure presented to the operator.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PolicyError {
    /// Interactive tuning was attempted without effective root privileges.
    RootRequired,
    /// One or more required executables could not be found.
    MissingCommands(Vec<String>),
    /// One or more standard streams are not attached to a terminal.
    InteractiveTerminalRequired,
    /// `TERM` cannot describe a usable menu terminal.
    UnusableTerminalType,
}

impl fmt::Display for PolicyError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::RootRequired => {
                formatter.write_str("Run this menu with sudo: sudo usbradioplus-tune")
            }
            Self::MissingCommands(commands) => write!(formatter, "Missing: {}", commands.join(", ")),
            Self::InteractiveTerminalRequired => {
                formatter.write_str("usbradioplus-tune requires an interactive terminal.")
            }
            Self::UnusableTerminalType => formatter.write_str(
                "The terminal type is not usable for a menu (TERM is unset or dumb). Start it from the same terminal in which asl-menu works.",
            ),
        }
    }
}

impl std::error::Error for PolicyError {}

/// Validate the dependencies required by `--check`.
///
/// # Errors
///
/// Returns [`PolicyError::MissingCommands`] when online checking cannot find
/// every required executable.
pub fn validate_check_environment<E: RuntimeEnvironment + ?Sized>(
    environment: &E,
    offline: bool,
) -> Result<(), PolicyError> {
    if offline {
        return Ok(());
    }
    let missing = ["whiptail", "asterisk"]
        .into_iter()
        .filter(|command| !environment.command_available(command))
        .map(str::to_owned)
        .collect::<Vec<_>>();
    if missing.is_empty() {
        Ok(())
    } else {
        Err(PolicyError::MissingCommands(missing))
    }
}

/// Validate privileges and terminal prerequisites for an interactive session.
///
/// # Errors
///
/// Returns [`PolicyError`] when the process lacks root privileges, Whiptail,
/// three terminal streams, or a usable terminal type.
pub fn validate_interactive_environment<E: RuntimeEnvironment + ?Sized>(
    environment: &E,
) -> Result<(), PolicyError> {
    if environment.effective_user_id() != 0 {
        return Err(PolicyError::RootRequired);
    }
    if !environment.command_available("whiptail") {
        return Err(PolicyError::MissingCommands(vec!["whiptail".to_owned()]));
    }
    if !environment.stdin_is_terminal()
        || !environment.stdout_is_terminal()
        || !environment.stderr_is_terminal()
    {
        return Err(PolicyError::InteractiveTerminalRequired);
    }
    let usable = environment.terminal_type().is_some_and(|terminal| {
        !matches!(
            terminal.to_ascii_lowercase().as_str(),
            "" | "dumb" | "unknown"
        )
    });
    if !usable {
        return Err(PolicyError::UnusableTerminalType);
    }
    Ok(())
}

fn command_available(command: &str) -> bool {
    let path = Path::new(command);
    if path.components().count() > 1 {
        return executable(path);
    }
    env::var_os("PATH").is_some_and(|value| {
        env::split_paths(&value).any(|directory| executable(&directory.join(command)))
    })
}

fn executable(path: &Path) -> bool {
    let Ok(metadata) = path.metadata() else {
        return false;
    };
    if !metadata.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        metadata.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

#[cfg(unix)]
fn effective_user_id() -> u32 {
    unsafe extern "C" {
        fn geteuid() -> u32;
    }
    // SAFETY: `geteuid` has no arguments, returns a value, and has no ownership contract.
    unsafe { geteuid() }
}

#[cfg(not(unix))]
fn effective_user_id() -> u32 {
    u32::MAX
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::collections::BTreeSet;
    use std::sync::Mutex;

    static ENVIRONMENT_LOCK: Mutex<()> = Mutex::new(());

    #[derive(Clone)]
    struct FakeEnvironment {
        uid: u32,
        commands: BTreeSet<String>,
        terminals: [bool; 3],
        term: Option<String>,
    }

    impl Default for FakeEnvironment {
        fn default() -> Self {
            Self {
                uid: 0,
                commands: ["whiptail", "asterisk"].map(str::to_owned).into(),
                terminals: [true; 3],
                term: Some("xterm-256color".to_owned()),
            }
        }
    }

    impl RuntimeEnvironment for FakeEnvironment {
        fn effective_user_id(&self) -> u32 {
            self.uid
        }

        fn command_available(&self, command: &str) -> bool {
            self.commands.contains(command)
        }

        fn stdin_is_terminal(&self) -> bool {
            self.terminals[0]
        }

        fn stdout_is_terminal(&self) -> bool {
            self.terminals[1]
        }

        fn stderr_is_terminal(&self) -> bool {
            self.terminals[2]
        }

        fn terminal_type(&self) -> Option<String> {
            self.term.clone()
        }
    }

    #[test]
    fn offline_check_has_no_external_prerequisites() {
        let environment = FakeEnvironment {
            commands: BTreeSet::new(),
            ..FakeEnvironment::default()
        };
        assert_eq!(validate_check_environment(&environment, true), Ok(()));
    }

    #[test]
    fn online_check_reports_every_missing_command_in_order() {
        assert_eq!(
            validate_check_environment(&FakeEnvironment::default(), false),
            Ok(())
        );
        let environment = FakeEnvironment {
            commands: BTreeSet::new(),
            ..FakeEnvironment::default()
        };
        assert_eq!(
            validate_check_environment(&environment, false),
            Err(PolicyError::MissingCommands(vec![
                "whiptail".to_owned(),
                "asterisk".to_owned()
            ]))
        );
        let environment = FakeEnvironment {
            commands: ["whiptail"].map(str::to_owned).into(),
            ..FakeEnvironment::default()
        };
        assert_eq!(
            validate_check_environment(&environment, false),
            Err(PolicyError::MissingCommands(vec!["asterisk".to_owned()]))
        );
    }

    #[test]
    fn healthy_interactive_environment_is_accepted() {
        assert_eq!(
            validate_interactive_environment(&FakeEnvironment::default()),
            Ok(())
        );
    }

    #[test]
    fn interactive_checks_follow_operator_facing_order() {
        let environment = FakeEnvironment {
            uid: 1000,
            commands: BTreeSet::new(),
            terminals: [false; 3],
            term: None,
        };
        assert_eq!(
            validate_interactive_environment(&environment),
            Err(PolicyError::RootRequired)
        );
        let environment = FakeEnvironment {
            commands: BTreeSet::new(),
            ..FakeEnvironment::default()
        };
        assert_eq!(
            validate_interactive_environment(&environment),
            Err(PolicyError::MissingCommands(vec!["whiptail".to_owned()]))
        );
        for index in 0..3 {
            let mut environment = FakeEnvironment::default();
            environment.terminals[index] = false;
            assert_eq!(
                validate_interactive_environment(&environment),
                Err(PolicyError::InteractiveTerminalRequired)
            );
        }
    }

    #[test]
    fn unusable_terminal_names_are_rejected_case_insensitively() {
        for term in [
            None,
            Some(String::new()),
            Some("DUMB".to_owned()),
            Some("unknown".to_owned()),
        ] {
            let environment = FakeEnvironment {
                term,
                ..FakeEnvironment::default()
            };
            assert_eq!(
                validate_interactive_environment(&environment),
                Err(PolicyError::UnusableTerminalType)
            );
        }
    }

    #[test]
    fn policy_error_messages_match_the_existing_interface() {
        assert!(
            PolicyError::RootRequired
                .to_string()
                .starts_with("Run this menu")
        );
        assert_eq!(
            PolicyError::MissingCommands(vec!["one".to_owned(), "two".to_owned()]).to_string(),
            "Missing: one, two"
        );
        assert!(
            PolicyError::InteractiveTerminalRequired
                .to_string()
                .contains("interactive")
        );
        assert!(
            PolicyError::UnusableTerminalType
                .to_string()
                .contains("TERM")
        );
        assert!(std::error::Error::source(&PolicyError::RootRequired).is_none());
    }

    #[test]
    fn executable_requires_a_regular_executable_file() {
        let directory = temporary_directory("policy");
        let file = directory.join("tool");
        std::fs::write(&file, b"tool").unwrap();
        assert!(!executable(&directory));
        #[cfg(unix)]
        assert!(!executable(&file));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&file, std::fs::Permissions::from_mode(0o700)).unwrap();
        }
        assert!(executable(&file));
        assert!(!executable(&directory.join("missing")));
        std::fs::remove_dir_all(directory).unwrap();
    }

    fn temporary_directory(label: &str) -> std::path::PathBuf {
        let path = env::temp_dir().join(format!(
            "usbradioplus-tune-{label}-{}-{}",
            std::process::id(),
            std::thread::current().name().unwrap_or("unnamed")
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn system_environment_queries_every_host_property() {
        let _guard = ENVIRONMENT_LOCK.lock().unwrap();
        let environment = SystemEnvironment;
        let _ = environment.effective_user_id();
        let _ = environment.stdin_is_terminal();
        let _ = environment.stdout_is_terminal();
        let _ = environment.stderr_is_terminal();
        let _ = environment.terminal_type();
        assert!(environment.command_available("/bin/sh"));
        assert!(!environment.command_available("/definitely/missing/usbradioplus-command"));
        assert!(!environment.command_available("definitely-missing-usbradioplus-command"));

        let saved_path = env::var_os("PATH");
        // SAFETY: this test serializes its environment mutation, and no other
        // tuner test reads PATH through production policy.
        unsafe { env::remove_var("PATH") };
        assert!(!environment.command_available("sh"));
        if let Some(saved_path) = saved_path {
            // SAFETY: see the serialized mutation above.
            unsafe { env::set_var("PATH", saved_path) };
        }
    }
}
