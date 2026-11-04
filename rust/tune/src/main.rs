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

use usbradioplus_tune::arguments::{Arguments, Backend, HELP, ParseOutcome};
use usbradioplus_tune::catalog::{configuration_catalog, standalone_configuration_catalog};
use usbradioplus_tune::configuration::NativeConfiguration;
use usbradioplus_tune::control::{AsteriskRuntime, SessionLock};
use usbradioplus_tune::menus::TunerApp;
use usbradioplus_tune::policy::{
    RuntimeEnvironment, SystemEnvironment, validate_backend_check_environment,
    validate_backend_interactive_environment,
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
    let mut repository = ConfigRepository::new(&arguments.config_path, sample_candidates);
    if arguments.backend == Backend::Asterisk {
        repository
            .ensure_exists()
            .map_err(|error| error.to_string())?;
    } else if arguments.check {
        #[cfg(unix)]
        if !arguments.offline {
            repository = standalone_repository(&arguments)?.1;
        }
        repository.read().map_err(|error| error.to_string())?;
    }
    if arguments.check {
        validate_backend_check_environment(environment, arguments.offline, arguments.backend)
            .map_err(|error| error.to_string())?;
        println!("usbradioplus-tune: configuration storage and prerequisites are valid");
        return Ok(());
    }
    validate_backend_interactive_environment(environment, arguments.backend)
        .map_err(|error| error.to_string())?;
    start_session(&arguments, repository)
}

fn interactive_session(
    arguments: &Arguments,
    repository: ConfigRepository,
    backend: WhiptailBackend,
) -> Result<(), String> {
    let _lock = if arguments.backend == Backend::Standalone {
        SessionLock::acquire_service(&arguments.lock_path)
    } else {
        SessionLock::acquire(&arguments.lock_path)
    }
    .map_err(|error| error.to_string())?;
    if arguments.backend == Backend::Standalone {
        #[cfg(unix)]
        {
            let (runtime, repository) = standalone_repository(arguments)?;
            return TunerApp::with_configuration(
                AccessibleUi::new(backend),
                repository,
                runtime.clone(),
                runtime,
                arguments.node.clone(),
                NativeConfiguration,
            )
            .browse_tree(&standalone_configuration_catalog())
            .map_err(|error| error.to_string());
        }
        #[cfg(not(unix))]
        return Err("standalone control requires Unix-domain sockets".to_owned());
    }
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

#[cfg(unix)]
fn standalone_repository(
    arguments: &Arguments,
) -> Result<
    (
        usbradioplus_tune::standalone::StandaloneRuntime,
        ConfigRepository,
    ),
    String,
> {
    let runtime =
        usbradioplus_tune::standalone::StandaloneRuntime::connect(&arguments.socket_path)?;
    let path = runtime.configuration_path()?;
    let server_path = path
        .canonicalize()
        .map_err(|error| format!("resolve service configuration {}: {error}", path.display()))?;
    if arguments.config_explicit {
        let supplied = arguments.config_path.canonicalize().map_err(|error| {
            format!(
                "resolve selected configuration {}: {error}",
                arguments.config_path.display()
            )
        })?;
        if supplied != server_path {
            return Err(format!(
                "selected configuration {} differs from service configuration {}",
                supplied.display(),
                server_path.display()
            ));
        }
    }
    let repository = ConfigRepository::new(server_path, std::iter::empty::<PathBuf>());
    repository.read().map_err(|error| error.to_string())?;
    Ok((runtime, repository))
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
    fn standalone_offline_check_requires_existing_native_storage() {
        let directory = TestDirectory::new();
        let config = directory.0.join("native.conf");
        let sample = directory.0.join("asl.sample");
        fs::write(&sample, "[hardware]\n").unwrap();
        let mut no_session =
            |_: &Arguments, _: ConfigRepository| panic!("offline check must not start a session");
        let arguments = vec![
            "--backend=standalone".into(),
            "--check".into(),
            "--offline".into(),
            "--config".into(),
            config.clone().into_os_string(),
        ];
        assert!(
            run_with(
                arguments.clone(),
                &FakeEnvironment::default(),
                vec![sample.clone()],
                &mut no_session
            )
            .is_err()
        );
        assert!(!config.exists());
        fs::write(&config, "[1000]\n[radio]\n").unwrap();
        let mut environment = FakeEnvironment::default();
        environment.commands.clear();
        assert!(run_with(arguments, &environment, vec![sample], &mut no_session).is_ok());
        assert_eq!(fs::read_to_string(config).unwrap(), "[1000]\n[radio]\n");
    }

    #[cfg(unix)]
    #[test]
    fn standalone_startup_uses_service_path_and_rejects_other_edit_destinations() {
        use std::io::{Read, Write};
        use std::os::unix::net::UnixListener;
        let directory = TestDirectory::new();
        let config = directory.0.join("service.conf");
        let other = directory.0.join("other.conf");
        let socket = directory.0.join("radio.sock");
        fs::write(&config, "[1000]\n").unwrap();
        fs::write(&other, "untouched\n").unwrap();
        let server_path = config.canonicalize().unwrap();
        let listener = UnixListener::bind(&socket).unwrap();
        let response =
            serde_json::to_vec(&serde_json::json!({"version":1,"ok":true,"result":server_path}))
                .unwrap();
        let handle = std::thread::spawn(move || {
            for _ in 0..4 {
                let (mut stream, _) = listener.accept().unwrap();
                let mut prefix = [0; 4];
                stream.read_exact(&mut prefix).unwrap();
                let mut request = vec![0; u32::from_be_bytes(prefix) as usize];
                stream.read_exact(&mut request).unwrap();
                let request: serde_json::Value = serde_json::from_slice(&request).unwrap();
                assert_eq!(
                    request,
                    serde_json::json!({"version":1,"operation":"configuration-path"})
                );
                stream
                    .write_all(&(response.len() as u32).to_be_bytes())
                    .unwrap();
                stream.write_all(&response).unwrap();
            }
        });
        let mut arguments = Arguments {
            backend: Backend::Standalone,
            socket_path: socket,
            ..Arguments::default()
        };
        let (_, repository) = standalone_repository(&arguments).unwrap();
        assert_eq!(repository.path(), server_path);
        arguments.config_explicit = true;
        arguments.config_path = other.clone();
        assert!(
            standalone_repository(&arguments)
                .unwrap_err()
                .contains("differs from service configuration")
        );
        arguments.config_path = directory.0.join("missing.conf");
        assert!(
            standalone_repository(&arguments)
                .unwrap_err()
                .contains("resolve selected configuration")
        );
        arguments.config_path = config;
        assert!(standalone_repository(&arguments).is_ok());
        assert_eq!(fs::read_to_string(other).unwrap(), "untouched\n");
        handle.join().unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn coverage_native_online_check_and_interactive_dispatch_use_service_storage() {
        use std::io::{Read, Write};
        use std::os::unix::net::UnixListener;
        let directory = TestDirectory::new();
        let config = directory.0.join("service.conf");
        let socket = directory.0.join("radio.sock");
        fs::write(&config, "[1000]\n[radio]\n").unwrap();
        let listener = UnixListener::bind(&socket).unwrap();
        let server_path = config.canonicalize().unwrap();
        let handle = std::thread::spawn(move || {
            let mut operations = Vec::new();
            for _ in 0..2 {
                let (mut stream, _) = listener.accept().unwrap();
                loop {
                    let mut prefix = [0; 4];
                    match stream.read_exact(&mut prefix) {
                        Ok(()) => {}
                        Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => break,
                        Err(error) => panic!("control socket: {error}"),
                    }
                    let mut request = vec![0; u32::from_be_bytes(prefix) as usize];
                    stream.read_exact(&mut request).unwrap();
                    let request: serde_json::Value = serde_json::from_slice(&request).unwrap();
                    let operation = request["operation"].as_str().unwrap();
                    let result = match operation {
                        "configuration-path" => serde_json::json!(server_path),
                        "nodes" => serde_json::json!(["1000"]),
                        other => panic!("unexpected startup operation {other}"),
                    };
                    operations.push(operation.to_owned());
                    let response = serde_json::to_vec(
                        &serde_json::json!({"version":1,"ok":true,"result":result}),
                    )
                    .unwrap();
                    stream
                        .write_all(&(response.len() as u32).to_be_bytes())
                        .unwrap();
                    stream.write_all(&response).unwrap();
                }
            }
            operations
        });
        let common = vec![
            "--backend=standalone".into(),
            "--socket".into(),
            socket.into_os_string(),
        ];
        let mut check = common.clone();
        check.push("--check".into());
        run_with(check, &FakeEnvironment::default(), vec![], &mut |_, _| {
            panic!("check must not enter UI")
        })
        .unwrap();
        let mut interactive = common;
        interactive.extend([
            "--lock".into(),
            directory.0.join("tune.lock").into_os_string(),
            "--node=1000".into(),
        ]);
        run_with(
            interactive,
            &FakeEnvironment::default(),
            vec![],
            &mut |arguments, repository| {
                interactive_session(arguments, repository, WhiptailBackend::new("/bin/false"))
            },
        )
        .unwrap();
        assert_eq!(
            handle.join().unwrap(),
            ["configuration-path", "configuration-path", "nodes"]
        );
        assert_eq!(fs::read_to_string(config).unwrap(), "[1000]\n[radio]\n");
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
