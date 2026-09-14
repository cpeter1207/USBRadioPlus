//! Rust entry point for `USBRadioPlus` configuration tuning.

#![allow(
    clippy::literal_string_with_formatting_args,
    reason = "Clippy misclassifies the conditional coverage feature attribute"
)]
#![cfg_attr(coverage, feature(coverage_attribute))]

use std::env;
use std::ffi::OsString;
use std::path::PathBuf;
use std::process::ExitCode;

use usbradioplus_tune::arguments::{Arguments, HELP, ParseOutcome};
use usbradioplus_tune::catalog::configuration_catalog;
use usbradioplus_tune::control::{AsteriskRuntime, SessionLock};
use usbradioplus_tune::menus::TunerApp;
use usbradioplus_tune::policy::{
    RuntimeEnvironment, SystemEnvironment, validate_check_environment,
    validate_interactive_environment,
};
use usbradioplus_tune::repository::ConfigRepository;
use usbradioplus_tune::ui::{AccessibleUi, WhiptailBackend};

fn main() -> ExitCode {
    exit_code(run())
}

fn exit_code(result: Result<(), String>) -> ExitCode {
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("usbradioplus-tune: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let mut start_session = start_system_session;
    run_with(
        env::args_os().skip(1).collect(),
        &SystemEnvironment,
        sample_candidates(),
        &mut start_session,
    )
}

fn start_system_session(arguments: &Arguments, repository: ConfigRepository) -> Result<(), String> {
    interactive_session(arguments, repository, WhiptailBackend::default())
}

fn run_with(
    raw_arguments: Vec<OsString>,
    environment: &dyn RuntimeEnvironment,
    sample_candidates: Vec<PathBuf>,
    start_session: &mut dyn FnMut(&Arguments, ConfigRepository) -> Result<(), String>,
) -> Result<(), String> {
    let arguments = match Arguments::parse(raw_arguments).map_err(|error| error.to_string())? {
        ParseOutcome::Help => {
            print!("{HELP}");
            return Ok(());
        }
        ParseOutcome::Run(arguments) => arguments,
    };
    let repository = ConfigRepository::new(&arguments.config_path, sample_candidates);
    repository
        .ensure_exists()
        .map_err(|error| error.to_string())?;
    if arguments.check {
        validate_check_environment(environment, arguments.offline)
            .map_err(|error| error.to_string())?;
        println!("usbradioplus-tune: configuration storage and prerequisites are valid");
        return Ok(());
    }
    validate_interactive_environment(environment).map_err(|error| error.to_string())?;
    start_session(&arguments, repository)
}

fn interactive_session(
    arguments: &Arguments,
    repository: ConfigRepository,
    backend: WhiptailBackend,
) -> Result<(), String> {
    let _lock = SessionLock::acquire(&arguments.lock_path).map_err(|error| error.to_string())?;
    let runtime = AsteriskRuntime::default();
    let mut application = TunerApp::new(
        AccessibleUi::new(backend),
        repository,
        runtime.clone(),
        runtime,
        arguments.node.clone(),
    );
    application
        .browse_tree(&configuration_catalog())
        .map_err(|error| error.to_string())
}

fn sample_candidates() -> Vec<PathBuf> {
    vec![
        PathBuf::from("/usr/share/usbradioplus/usbradioplus.conf.sample"),
        PathBuf::from("/usr/share/doc/usbradioplus/usbradioplus.conf.sample"),
        PathBuf::from("/usr/local/share/doc/usbradioplus/usbradioplus.conf.sample"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../examples/usbradioplus.conf.sample"),
    ]
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::collections::BTreeSet;
    use std::fs;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEST_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct FakeEnvironment {
        uid: u32,
        commands: BTreeSet<String>,
        terminals: [bool; 3],
        terminal_type: Option<String>,
    }

    impl Default for FakeEnvironment {
        fn default() -> Self {
            Self {
                uid: 0,
                commands: ["whiptail", "asterisk"].map(str::to_owned).into(),
                terminals: [true; 3],
                terminal_type: Some("xterm".to_owned()),
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
            self.terminal_type.clone()
        }
    }

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new() -> Self {
            let path = env::temp_dir().join(format!(
                "usbradioplus-tune-main-{}-{}",
                std::process::id(),
                TEST_SEQUENCE.fetch_add(1, Ordering::Relaxed)
            ));
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn source_tree_sample_is_the_final_development_fallback() {
        let candidates = sample_candidates();
        assert!(candidates.last().unwrap().is_file());
        assert!(
            candidates.iter().all(|candidate| candidate
                .extension()
                .and_then(|value| value.to_str())
                != Some("gz"))
        );
    }

    #[test]
    fn command_runner_covers_help_checks_and_interactive_sessions() {
        let directory = TestDirectory::new();
        let config = directory.0.join("radio.conf");
        let sample = directory.0.join("sample.conf");
        let lock = directory.0.join("menu.lock");
        std::fs::write(&sample, "[hardware]\n").unwrap();
        let mut sessions = 0;
        let mut session = |arguments: &Arguments, repository: ConfigRepository| {
            sessions += 1;
            interactive_session(arguments, repository, WhiptailBackend::new("/bin/false"))
        };

        assert!(
            run_with(
                vec!["--help".into()],
                &FakeEnvironment::default(),
                vec![],
                &mut session
            )
            .is_ok()
        );
        assert!(
            run_with(
                vec!["--bad".into()],
                &FakeEnvironment::default(),
                vec![],
                &mut session
            )
            .is_err()
        );
        assert!(
            run_with(
                vec![
                    "--check".into(),
                    "--offline".into(),
                    "--config".into(),
                    config.clone().into_os_string(),
                ],
                &FakeEnvironment::default(),
                vec![sample.clone()],
                &mut session,
            )
            .is_ok()
        );
        assert!(
            run_with(
                vec![
                    "--config".into(),
                    config.into_os_string(),
                    "--lock".into(),
                    lock.into_os_string(),
                ],
                &FakeEnvironment::default(),
                vec![sample],
                &mut session,
            )
            .is_ok()
        );
        assert_eq!(sessions, 1);
    }

    #[test]
    fn interactive_session_reports_lock_and_ui_failures() {
        let directory = TestDirectory::new();
        let config = directory.0.join("radio.conf");
        fs::write(&config, "[hardware]\n").unwrap();

        let blocked_parent = directory.0.join("not-a-directory");
        fs::write(&blocked_parent, "file").unwrap();
        let blocked_arguments = Arguments {
            config_path: config.clone(),
            lock_path: blocked_parent.join("menu.lock"),
            ..Arguments::default()
        };
        assert!(
            interactive_session(
                &blocked_arguments,
                ConfigRepository::new(&config, Vec::<PathBuf>::new()),
                WhiptailBackend::new("/bin/false"),
            )
            .is_err()
        );
        assert!(
            start_system_session(
                &blocked_arguments,
                ConfigRepository::new(&config, Vec::<PathBuf>::new()),
            )
            .is_err()
        );

        let arguments = Arguments {
            config_path: config.clone(),
            lock_path: directory.0.join("menu.lock"),
            ..Arguments::default()
        };
        assert!(
            interactive_session(
                &arguments,
                ConfigRepository::new(&config, Vec::<PathBuf>::new()),
                WhiptailBackend::new(directory.0.join("missing-whiptail")),
            )
            .is_err()
        );
    }

    #[test]
    fn runner_reports_storage_policy_session_and_exit_failures() {
        let directory = TestDirectory::new();
        let missing = directory.0.join("missing.conf");
        let mut session = |_arguments: &Arguments, _repository: ConfigRepository| {
            Err("session failed".to_owned())
        };
        assert!(
            run_with(
                vec!["--config".into(), missing.clone().into_os_string()],
                &FakeEnvironment::default(),
                vec![],
                &mut session,
            )
            .is_err()
        );

        std::fs::write(&missing, "[hardware]\n").unwrap();
        let missing_commands = FakeEnvironment {
            commands: BTreeSet::new(),
            ..FakeEnvironment::default()
        };
        assert!(
            run_with(
                vec![
                    "--check".into(),
                    "--config".into(),
                    missing.clone().into_os_string(),
                ],
                &missing_commands,
                vec![],
                &mut session,
            )
            .is_err()
        );
        let environment = FakeEnvironment {
            uid: 1000,
            ..FakeEnvironment::default()
        };
        assert!(
            run_with(
                vec!["--config".into(), missing.clone().into_os_string()],
                &environment,
                vec![],
                &mut session,
            )
            .is_err()
        );
        assert!(
            run_with(
                vec!["--config".into(), missing.into_os_string()],
                &FakeEnvironment::default(),
                vec![],
                &mut session,
            )
            .is_err()
        );
        assert_eq!(exit_code(Ok(())), ExitCode::SUCCESS);
        assert_eq!(exit_code(Err("expected".to_owned())), ExitCode::FAILURE);
        assert_eq!(main(), ExitCode::FAILURE);
    }
}
