//! Typed command-line parsing for `usbradioplus-tune`.

use std::ffi::OsString;
use std::fmt;
use std::path::PathBuf;

/// Default unified `USBRadioPlus` configuration path.
pub const DEFAULT_CONFIG_PATH: &str = "/etc/asterisk/usbradioplus.conf";
/// Default path used to serialize interactive tuning sessions.
pub const DEFAULT_LOCK_PATH: &str = "/run/lock/usbradioplus-tune.lock";
/// Command-line help shown by the Rust tuner executable.
pub const HELP: &str = "\
Interactive USBRadioPlus radio and processing tuner\n\
\n\
Usage: usbradioplus-tune [OPTIONS]\n\
\n\
Options:\n\
      --check          Validate prerequisites and configuration\n\
      --offline        With --check, do not require whiptail or Asterisk\n\
      --config PATH    USBRadioPlus configuration path\n\
      --lock PATH      Menu lock-file path\n\
  -n, --node NODE      Select the active RadioPlus channel\n\
  -h, --help           Print help\n";

/// Fully parsed tuner command-line arguments.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Arguments {
    /// Whether prerequisite and configuration validation was requested.
    pub check: bool,
    /// Whether validation must avoid live Asterisk and Whiptail dependencies.
    pub offline: bool,
    /// Unified `USBRadioPlus` configuration path.
    pub config_path: PathBuf,
    /// Exclusive interactive-session lock path.
    pub lock_path: PathBuf,
    /// Explicitly selected `RadioPlus` channel, if supplied.
    pub node: Option<String>,
}

impl Default for Arguments {
    fn default() -> Self {
        Self {
            check: false,
            offline: false,
            config_path: PathBuf::from(DEFAULT_CONFIG_PATH),
            lock_path: PathBuf::from(DEFAULT_LOCK_PATH),
            node: None,
        }
    }
}

/// Requested command-line action.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParseOutcome {
    /// Execute the tuner with the supplied arguments.
    Run(Arguments),
    /// Print command-line help without running the tuner.
    Help,
}

/// Failure to parse the tuner command line.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ArgumentError {
    /// An option requiring a following value did not receive one.
    MissingValue(&'static str),
    /// A channel name was not valid Unicode.
    InvalidNode,
    /// An unsupported option or positional argument was supplied.
    Unexpected(OsString),
}

impl fmt::Display for ArgumentError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::MissingValue(option) => write!(formatter, "{option} requires a value"),
            Self::InvalidNode => formatter.write_str("--node requires a valid UTF-8 channel name"),
            Self::Unexpected(argument) => {
                write!(
                    formatter,
                    "unrecognized argument: {}",
                    argument.to_string_lossy()
                )
            }
        }
    }
}

impl std::error::Error for ArgumentError {}

impl Arguments {
    /// Parse arguments excluding the executable name.
    ///
    /// # Errors
    ///
    /// Returns [`ArgumentError`] when an argument is unknown, a required value
    /// is absent, or a channel name is not valid Unicode.
    pub fn parse<I, S>(arguments: I) -> Result<ParseOutcome, ArgumentError>
    where
        I: IntoIterator<Item = S>,
        S: Into<OsString>,
    {
        Self::parse_owned(arguments.into_iter().map(Into::into).collect())
    }

    fn parse_owned(arguments: Vec<OsString>) -> Result<ParseOutcome, ArgumentError> {
        let mut parsed = Self::default();
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            match argument.to_str() {
                Some("--check") => parsed.check = true,
                Some("--offline") => parsed.offline = true,
                Some("-h" | "--help") => return Ok(ParseOutcome::Help),
                Some("--config") => {
                    parsed.config_path = PathBuf::from(required_value(&mut arguments, "--config")?);
                }
                Some("--lock") => {
                    parsed.lock_path = PathBuf::from(required_value(&mut arguments, "--lock")?);
                }
                Some("-n" | "--node") => {
                    parsed.node = Some(node_value(required_value(&mut arguments, "--node")?)?);
                }
                Some(text) if text.starts_with("--config=") => {
                    parsed.config_path = PathBuf::from(&text["--config=".len()..]);
                }
                Some(text) if text.starts_with("--lock=") => {
                    parsed.lock_path = PathBuf::from(&text["--lock=".len()..]);
                }
                Some(text) if text.starts_with("--node=") => {
                    parsed.node = Some(text["--node=".len()..].to_owned());
                }
                _ => return Err(ArgumentError::Unexpected(argument)),
            }
        }
        Ok(ParseOutcome::Run(parsed))
    }
}

fn required_value<I>(arguments: &mut I, option: &'static str) -> Result<OsString, ArgumentError>
where
    I: Iterator<Item = OsString>,
{
    arguments.next().ok_or(ArgumentError::MissingValue(option))
}

fn node_value(value: OsString) -> Result<String, ArgumentError> {
    value.into_string().map_err(|_| ArgumentError::InvalidNode)
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    #[test]
    fn defaults_match_the_existing_utility() {
        assert_eq!(
            Arguments::parse(std::iter::empty::<&str>()),
            Ok(ParseOutcome::Run(Arguments::default()))
        );
    }

    #[test]
    fn parses_every_supported_spelling() {
        let outcome = Arguments::parse([
            "--check",
            "--offline",
            "--config",
            "/tmp/radio.conf",
            "--lock=/tmp/radio.lock",
            "-n",
            "524950",
        ])
        .unwrap();
        let ParseOutcome::Run(arguments) = outcome else {
            panic!("normal options must request execution");
        };
        assert!(arguments.check);
        assert!(arguments.offline);
        assert_eq!(arguments.config_path, PathBuf::from("/tmp/radio.conf"));
        assert_eq!(arguments.lock_path, PathBuf::from("/tmp/radio.lock"));
        assert_eq!(arguments.node.as_deref(), Some("524950"));

        let outcome = Arguments::parse([
            "--config=/tmp/other.conf",
            "--lock",
            "/tmp/other.lock",
            "--node=usb",
        ])
        .unwrap();
        let ParseOutcome::Run(arguments) = outcome else {
            panic!("normal options must request execution");
        };
        assert_eq!(arguments.config_path, PathBuf::from("/tmp/other.conf"));
        assert_eq!(arguments.lock_path, PathBuf::from("/tmp/other.lock"));
        assert_eq!(arguments.node.as_deref(), Some("usb"));
    }

    #[test]
    fn later_values_replace_earlier_values_like_argparse() {
        let ParseOutcome::Run(arguments) = Arguments::parse([
            "--config=first",
            "--config",
            "second",
            "--node=first",
            "-n",
            "second",
        ])
        .unwrap() else {
            panic!("normal options must request execution");
        };
        assert_eq!(arguments.config_path, PathBuf::from("second"));
        assert_eq!(arguments.node.as_deref(), Some("second"));
    }

    #[test]
    fn help_short_circuits_execution() {
        assert_eq!(Arguments::parse(["--help"]), Ok(ParseOutcome::Help));
        assert_eq!(Arguments::parse(["-h"]), Ok(ParseOutcome::Help));
    }

    #[test]
    fn reports_missing_and_unexpected_arguments() {
        assert_eq!(
            Arguments::parse(["--config"]),
            Err(ArgumentError::MissingValue("--config"))
        );
        assert_eq!(
            Arguments::parse(["--lock"]),
            Err(ArgumentError::MissingValue("--lock"))
        );
        assert_eq!(
            Arguments::parse(["--node"]),
            Err(ArgumentError::MissingValue("--node"))
        );
        assert_eq!(
            Arguments::parse(["file.conf"]),
            Err(ArgumentError::Unexpected(OsString::from("file.conf")))
        );
        assert!(ArgumentError::InvalidNode.to_string().contains("UTF-8"));
    }

    #[test]
    fn path_options_accept_non_unicode_values() {
        let non_unicode = non_unicode_value();
        let ParseOutcome::Run(arguments) =
            Arguments::parse([OsString::from("--config"), non_unicode.clone()]).unwrap()
        else {
            panic!("normal options must request execution");
        };
        assert_eq!(arguments.config_path.as_os_str(), non_unicode.as_os_str());

        #[cfg(unix)]
        assert_eq!(
            Arguments::parse([OsString::from("--node"), non_unicode]),
            Err(ArgumentError::InvalidNode)
        );
    }

    #[cfg(unix)]
    fn non_unicode_value() -> OsString {
        use std::os::unix::ffi::OsStringExt;
        OsString::from_vec(vec![b'/', b't', b'm', b'p', b'/', 0xff])
    }

    #[cfg(not(unix))]
    fn non_unicode_value() -> OsString {
        OsString::from("non-unicode-path-test")
    }

    #[test]
    fn display_messages_identify_the_bad_argument() {
        let missing = ArgumentError::MissingValue("--lock");
        assert_eq!(missing.to_string(), "--lock requires a value");
        let unexpected = ArgumentError::Unexpected(OsString::from("--bad"));
        assert_eq!(unexpected.to_string(), "unrecognized argument: --bad");
        assert!(std::error::Error::source(&unexpected).is_none());
        assert_eq!(
            OsStr::new(DEFAULT_CONFIG_PATH),
            Arguments::default().config_path
        );
    }
}
