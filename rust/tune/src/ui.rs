//! Consistent screen-reader-friendly Whiptail user interface.

use std::ffi::OsString;
use std::fmt;
use std::io;
#[cfg(unix)]
use std::io::Read;
use std::io::Write;
#[cfg(unix)]
use std::os::fd::AsRawFd;
use std::process::{Command, Stdio};
use std::time::Duration;

/// One tagged item in a navigation or selection list.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ChoiceItem {
    /// Stable value returned when this item is selected.
    pub value: String,
    /// Operator-facing description of this item.
    pub description: String,
}

impl ChoiceItem {
    /// Construct one owned choice.
    pub fn new(value: impl Into<String>, description: impl Into<String>) -> Self {
        Self {
            value: value.into(),
            description: description.into(),
        }
    }
}

/// One request sent through the dialog backend.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DialogRequest {
    /// Tagged menu used to navigate sections.
    Menu {
        /// Prompt displayed above the entries.
        prompt: String,
        /// Item that receives initial keyboard focus.
        default_item: String,
        /// Ordered tagged menu entries.
        items: Vec<ChoiceItem>,
        /// Label for the cancel/back button.
        cancel_label: String,
    },
    /// Single-selection radiolist used for booleans and enumerations.
    Selection {
        /// Setting label and current value.
        prompt: String,
        /// Item that receives initial keyboard focus.
        default_item: String,
        /// Ordered allowed values.
        items: Vec<ChoiceItem>,
    },
    /// Text entry used for integer and floating-point settings.
    Input {
        /// Setting label, current value, type, units, and limits.
        prompt: String,
        /// Initial editable text.
        initial: String,
    },
    /// Informational or validation message.
    Message {
        /// Message title.
        title: String,
        /// Operator-facing message body.
        body: String,
    },
    /// Binary confirmation with action-specific button labels.
    Confirmation {
        /// Question displayed to the operator.
        prompt: String,
        /// Label for the affirmative button.
        accept_label: String,
        /// Label for the negative button.
        cancel_label: String,
    },
}

/// Backend result from one modal dialog.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DialogResponse {
    /// The operator accepted the dialog, optionally returning a value.
    Accepted(String),
    /// The operator selected Cancel, Back, or Exit.
    Cancelled,
}

/// Failure to launch or communicate with a dialog backend.
#[derive(Debug)]
pub struct UiError {
    message: String,
    source: Option<io::Error>,
}

impl UiError {
    /// Construct a backend error without an operating-system cause.
    pub fn message(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
            source: None,
        }
    }

    fn process(operation: &'static str, source: io::Error) -> Self {
        Self {
            message: operation.to_owned(),
            source: Some(source),
        }
    }
}

impl fmt::Display for UiError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.source {
            Some(source) => write!(formatter, "{}: {source}", self.message),
            None => formatter.write_str(&self.message),
        }
    }
}

impl std::error::Error for UiError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        self.source
            .as_ref()
            .map(|source| source as &(dyn std::error::Error + 'static))
    }
}

/// Backend capable of displaying one typed modal dialog.
pub trait DialogBackend {
    /// Display the request and return the operator's response.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the backend cannot display or read the dialog.
    fn show(&mut self, request: &DialogRequest) -> Result<DialogResponse, UiError>;

    /// Present live text without retaining an unbounded snapshot history.
    ///
    /// Backends without a live surface may show one modal message and stop.
    ///
    /// # Errors
    /// Returns an error when the output surface is unavailable.
    fn live_status(&mut self, title: &str, body: &str) -> Result<(), UiError> {
        self.show(&DialogRequest::Message {
            title: title.to_owned(),
            body: body.to_owned(),
        })
        .map(|_| ())
    }

    /// Wait at most the supplied duration; return true when live output should stop.
    ///
    /// # Errors
    /// Returns an error when input cannot be observed safely.
    fn wait_for_live_exit(&mut self, _timeout: Duration) -> Result<bool, UiError> {
        Ok(true)
    }
}

/// Accessible high-level UI whose widgets use one button and focus convention.
pub struct AccessibleUi {
    backend: Box<dyn DialogBackend>,
}

impl AccessibleUi {
    /// Wrap one dialog backend.
    pub fn new(backend: impl DialogBackend + 'static) -> Self {
        Self {
            backend: Box::new(backend),
        }
    }

    /// Present an accessible live-text header or snapshot.
    ///
    /// # Errors
    /// Returns the live surface's output error.
    pub fn live_status(&mut self, title: &str, body: &str) -> Result<(), UiError> {
        self.backend.live_status(title, body)
    }

    /// Wait for cancellation or the next bounded live refresh interval.
    ///
    /// # Errors
    /// Returns the live surface's input error.
    pub fn wait_for_live_exit(&mut self, timeout: Duration) -> Result<bool, UiError> {
        self.backend.wait_for_live_exit(timeout)
    }

    /// Show a section-navigation menu using Select and Back.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn navigation(
        &mut self,
        prompt: impl Into<String>,
        default_item: impl Into<String>,
        items: Vec<ChoiceItem>,
    ) -> Result<Option<String>, UiError> {
        self.menu(prompt, default_item, items, "Back")
    }

    /// Show the top-level navigation menu using Select and Exit.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn top_navigation(
        &mut self,
        prompt: impl Into<String>,
        default_item: impl Into<String>,
        items: Vec<ChoiceItem>,
    ) -> Result<Option<String>, UiError> {
        self.menu(prompt, default_item, items, "Exit")
    }

    /// Show a short decision menu whose cancel button keeps the current context.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn decision(
        &mut self,
        prompt: impl Into<String>,
        default_item: impl Into<String>,
        items: Vec<ChoiceItem>,
        cancel_label: impl Into<String>,
    ) -> Result<Option<String>, UiError> {
        self.menu(prompt, default_item, items, cancel_label)
    }

    /// Show an on/off or enumeration radiolist using Apply and Cancel.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn selection(
        &mut self,
        prompt: impl Into<String>,
        default_item: impl Into<String>,
        items: Vec<ChoiceItem>,
    ) -> Result<Option<String>, UiError> {
        let response = self.backend.show(&DialogRequest::Selection {
            prompt: prompt.into(),
            default_item: default_item.into(),
            items,
        })?;
        Ok(response_value(response))
    }

    /// Show numeric entry using Apply and Cancel.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn input(
        &mut self,
        prompt: impl Into<String>,
        initial: impl Into<String>,
    ) -> Result<Option<String>, UiError> {
        let mut initial = initial.into();
        if initial.starts_with('-') {
            initial.insert(0, ' ');
        }
        let response = self.backend.show(&DialogRequest::Input {
            prompt: prompt.into(),
            initial,
        })?;
        Ok(response_value(response))
    }

    /// Show an informational or validation message with a Close button.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn message(
        &mut self,
        title: impl Into<String>,
        body: impl Into<String>,
    ) -> Result<(), UiError> {
        self.backend.show(&DialogRequest::Message {
            title: title.into(),
            body: body.into(),
        })?;
        Ok(())
    }

    /// Ask for explicit confirmation using operator-facing action labels.
    ///
    /// # Errors
    ///
    /// Returns [`UiError`] when the dialog backend fails.
    pub fn confirm(
        &mut self,
        title: impl AsRef<str>,
        prompt: impl AsRef<str>,
        accept_label: impl Into<String>,
        cancel_label: impl Into<String>,
    ) -> Result<bool, UiError> {
        let response = self.backend.show(&DialogRequest::Confirmation {
            prompt: format!("{}\n{}", title.as_ref(), prompt.as_ref()),
            accept_label: accept_label.into(),
            cancel_label: cancel_label.into(),
        })?;
        Ok(matches!(response, DialogResponse::Accepted(_)))
    }

    fn menu(
        &mut self,
        prompt: impl Into<String>,
        default_item: impl Into<String>,
        items: Vec<ChoiceItem>,
        cancel_label: impl Into<String>,
    ) -> Result<Option<String>, UiError> {
        let response = self.backend.show(&DialogRequest::Menu {
            prompt: prompt.into(),
            default_item: default_item.into(),
            items,
            cancel_label: cancel_label.into(),
        })?;
        Ok(response_value(response))
    }
}

fn response_value(response: DialogResponse) -> Option<String> {
    match response {
        DialogResponse::Accepted(value) => Some(value.trim().to_owned()),
        DialogResponse::Cancelled => None,
    }
}

/// Production backend that invokes Whiptail directly without a shell.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WhiptailBackend {
    program: OsString,
    backtitle: String,
}

impl Default for WhiptailBackend {
    fn default() -> Self {
        Self::new("whiptail")
    }
}

impl WhiptailBackend {
    /// Construct a backend using the selected Whiptail executable.
    pub fn new(program: impl Into<OsString>) -> Self {
        Self {
            program: program.into(),
            backtitle: "USBRadioPlus processing".to_owned(),
        }
    }

    fn arguments(&self, request: &DialogRequest) -> Vec<OsString> {
        let mut arguments = strings(["--backtitle", self.backtitle.as_str()]);
        match request {
            DialogRequest::Menu {
                prompt,
                default_item,
                items,
                cancel_label,
            } => {
                arguments.extend(strings([
                    "--ok-button",
                    "Select",
                    "--cancel-button",
                    cancel_label,
                    "--default-item",
                    default_item,
                    "--menu",
                    prompt,
                    "20",
                    "78",
                    "12",
                ]));
                append_items(&mut arguments, items, None);
            }
            DialogRequest::Selection {
                prompt,
                default_item,
                items,
            } => {
                let height = items.len().saturating_add(8).clamp(11, 22);
                arguments.extend(strings([
                    "--ok-button",
                    "Apply",
                    "--cancel-button",
                    "Cancel",
                    "--default-item",
                    default_item,
                    "--radiolist",
                    prompt,
                    &height.to_string(),
                    "72",
                    &items.len().to_string(),
                ]));
                append_items(&mut arguments, items, Some(default_item));
            }
            DialogRequest::Input { prompt, initial } => {
                arguments.extend(strings([
                    "--ok-button",
                    "Apply",
                    "--cancel-button",
                    "Cancel",
                    "--inputbox",
                    prompt,
                    "11",
                    "70",
                    initial,
                ]));
            }
            DialogRequest::Message { title, body } => {
                arguments.extend(strings([
                    "--ok-button",
                    "Close",
                    "--title",
                    title,
                    "--msgbox",
                    body,
                    "12",
                    "70",
                ]));
            }
            DialogRequest::Confirmation {
                prompt,
                accept_label,
                cancel_label,
            } => {
                arguments.extend(strings([
                    "--yes-button",
                    accept_label,
                    "--no-button",
                    cancel_label,
                    "--yesno",
                    prompt,
                    "8",
                    "70",
                ]));
            }
        }
        arguments
    }
}

impl DialogBackend for WhiptailBackend {
    fn show(&mut self, request: &DialogRequest) -> Result<DialogResponse, UiError> {
        let output = Command::new(&self.program)
            .args(self.arguments(request))
            .stdout(dialog_screen())
            .stderr(Stdio::piped())
            .output()
            .map_err(|error| UiError::process("launch whiptail", error))?;
        if output.status.success() {
            Ok(DialogResponse::Accepted(
                String::from_utf8_lossy(&output.stderr).trim().to_owned(),
            ))
        } else {
            Ok(DialogResponse::Cancelled)
        }
    }

    fn live_status(&mut self, title: &str, body: &str) -> Result<(), UiError> {
        write_live_status(&mut io::stdout().lock(), title, body)
            .map_err(|error| UiError::process("write live radio status", error))
    }

    fn wait_for_live_exit(&mut self, timeout: Duration) -> Result<bool, UiError> {
        #[cfg(unix)]
        {
            wait_for_live_input(&mut io::stdin().lock(), timeout)
                .map_err(|error| UiError::process("wait for live radio input", error))
        }
        #[cfg(not(unix))]
        {
            let _ = timeout;
            Err(UiError::message("live radio input requires Unix"))
        }
    }
}

fn write_live_status(output: &mut impl Write, title: &str, body: &str) -> io::Result<()> {
    if !title.is_empty() {
        writeln!(output, "{title}")?;
    }
    writeln!(output, "{body}")?;
    output.flush()
}

#[cfg(unix)]
fn wait_for_live_input(input: &mut (impl Read + AsRawFd), timeout: Duration) -> io::Result<bool> {
    #[repr(C)]
    struct PollFd {
        fd: i32,
        events: i16,
        revents: i16,
    }
    unsafe extern "C" {
        fn poll(fds: *mut PollFd, count: usize, timeout_ms: i32) -> i32;
    }
    let mut descriptor = PollFd {
        fd: input.as_raw_fd(),
        events: 1,
        revents: 0,
    };
    let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
    // SAFETY: this is one initialized pollfd with a descriptor borrowed from
    // the live input owner. Debian's nfds_t is an unsigned machine word.
    let ready = unsafe { poll(&raw mut descriptor, 1, timeout_ms) };
    if ready < 0 {
        return Err(io::Error::last_os_error());
    }
    if ready == 0 {
        return Ok(false);
    }
    let mut line = [0; 256];
    input.read(&mut line).map(|_| true)
}

fn strings<const N: usize>(values: [&str; N]) -> Vec<OsString> {
    values.into_iter().map(OsString::from).collect()
}

fn append_items(arguments: &mut Vec<OsString>, items: &[ChoiceItem], selected: Option<&str>) {
    for item in items {
        arguments.push(OsString::from(&item.value));
        arguments.push(OsString::from(&item.description));
        if let Some(selected) = selected {
            arguments.push(OsString::from(if item.value == selected {
                "ON"
            } else {
                "OFF"
            }));
        }
    }
}

fn dialog_screen() -> Stdio {
    Stdio::inherit()
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::VecDeque;
    use std::ffi::OsStr;
    use std::fs;
    use std::rc::Rc;

    #[derive(Default)]
    struct ScriptState {
        responses: VecDeque<Result<DialogResponse, UiError>>,
        requests: Vec<DialogRequest>,
    }

    #[derive(Clone, Default)]
    struct ScriptedBackend(Rc<RefCell<ScriptState>>);

    impl ScriptedBackend {
        fn with_responses(responses: impl IntoIterator<Item = DialogResponse>) -> Self {
            Self(Rc::new(RefCell::new(ScriptState {
                responses: responses.into_iter().map(Ok).collect(),
                requests: Vec::new(),
            })))
        }
    }

    impl DialogBackend for ScriptedBackend {
        fn show(&mut self, request: &DialogRequest) -> Result<DialogResponse, UiError> {
            let mut state = self.0.borrow_mut();
            state.requests.push(request.clone());
            state
                .responses
                .pop_front()
                .unwrap_or_else(|| Err(UiError::message("script exhausted")))
        }
    }

    #[test]
    fn live_status_writes_plain_snapshots_without_escape_sequences_or_history() {
        let mut output = Vec::new();
        write_live_status(&mut output, "Live radio meters", "Press Enter to stop.").unwrap();
        write_live_status(&mut output, "", "receive_input_peak=0.25").unwrap();
        assert_eq!(
            String::from_utf8(output).unwrap(),
            "Live radio meters\nPress Enter to stop.\nreceive_input_peak=0.25\n"
        );
        struct FailedOutput;
        impl Write for FailedOutput {
            fn write(&mut self, _buffer: &[u8]) -> io::Result<usize> {
                Err(io::Error::other("output unavailable"))
            }
            fn flush(&mut self) -> io::Result<()> {
                Err(io::Error::other("flush unavailable"))
            }
        }
        assert!(write_live_status(&mut FailedOutput, "Meters", "status").is_err());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn coverage_whiptail_live_port_with_process_input_and_interrupted_poll() {
        const CHILD_MODE: &str = "USBRADIOPLUS_TEST_LIVE_PORT";
        if let Ok(mode) = std::env::var(CHILD_MODE) {
            let mut backend = WhiptailBackend::default();
            backend
                .live_status("Meters", "receive_input_peak=0.25")
                .unwrap();
            if mode == "eof" {
                assert!(backend.wait_for_live_exit(Duration::from_secs(1)).unwrap());
                return;
            }
            use std::sync::{
                Arc,
                atomic::{AtomicBool, Ordering},
            };
            unsafe extern "C" {
                fn signal(number: i32, handler: extern "C" fn(i32)) -> usize;
                fn pthread_self() -> usize;
                fn pthread_kill(thread: usize, number: i32) -> i32;
            }
            extern "C" fn handle_signal(_: i32) {}
            // SAFETY: the no-op handler has the required C signal-handler ABI;
            // only this isolated child process installs it.
            unsafe {
                signal(10, handle_signal);
            }
            // SAFETY: pthread_self returns the caller's live thread identifier.
            let thread = unsafe { pthread_self() };
            let done = Arc::new(AtomicBool::new(false));
            let stop = Arc::clone(&done);
            let sender = std::thread::spawn(move || {
                while !stop.load(Ordering::Acquire) {
                    std::thread::sleep(Duration::from_millis(1));
                    // SAFETY: the test thread remains alive until this sender joins.
                    assert_eq!(unsafe { pthread_kill(thread, 10) }, 0);
                }
            });
            let result = backend.wait_for_live_exit(Duration::from_secs(2));
            done.store(true, Ordering::Release);
            sender.join().unwrap();
            let error = result.unwrap_err();
            assert!(error.to_string().contains("wait for live radio input"));
            assert_eq!(
                std::error::Error::source(&error)
                    .unwrap()
                    .downcast_ref::<io::Error>()
                    .unwrap()
                    .kind(),
                io::ErrorKind::Interrupted
            );
            return;
        }
        for mode in ["eof", "interrupt"] {
            let mut child = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "ui::tests::coverage_whiptail_live_port_with_process_input_and_interrupted_poll", "--nocapture"])
                .env(CHILD_MODE, mode)
                .stdin(if mode == "eof" { Stdio::null() } else { Stdio::piped() })
                .stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap();
            // Keep the interrupt child's input pipe open until it finishes polling.
            let _input = child.stdin.take();
            let output = child.wait_with_output().unwrap();
            assert!(
                output.status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            assert!(
                String::from_utf8_lossy(&output.stdout)
                    .contains("Meters\nreceive_input_peak=0.25\n")
            );
        }
    }

    #[test]
    #[cfg(unix)]
    fn live_input_wait_is_bounded_consumes_cancellation_and_stops_on_eof() {
        use std::os::unix::net::UnixStream;
        let (mut input, mut peer) = UnixStream::pair().unwrap();
        assert!(!wait_for_live_input(&mut input, Duration::from_millis(1)).unwrap());
        peer.write_all(b"stop\n").unwrap();
        assert!(wait_for_live_input(&mut input, Duration::from_secs(1)).unwrap());
        assert!(!wait_for_live_input(&mut input, Duration::ZERO).unwrap());
        drop(peer);
        // A concurrent child launch can briefly inherit the peer between fork
        // and exec; allow its close-on-exec descriptor to reach EOF.
        assert!(wait_for_live_input(&mut input, Duration::from_secs(1)).unwrap());
    }

    #[test]
    fn live_port_defaults_keep_modal_backends_usable() {
        let backend = ScriptedBackend::with_responses([DialogResponse::Accepted(String::new())]);
        let mut ui = AccessibleUi::new(backend.clone());
        ui.live_status("Meters", "snapshot").unwrap();
        assert!(ui.wait_for_live_exit(Duration::from_secs(1)).unwrap());
        assert!(
            matches!(&backend.0.borrow().requests[0], DialogRequest::Message { title, body } if title == "Meters" && body == "snapshot")
        );
    }

    #[test]
    fn accessible_widgets_use_one_button_and_focus_convention() {
        let backend = ScriptedBackend::with_responses([
            DialogResponse::Accepted("2\n".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Accepted("-6.0".to_owned()),
            DialogResponse::Cancelled,
        ]);
        let mut ui = AccessibleUi::new(backend.clone());
        assert_eq!(
            ui.navigation("Sections", "2", vec![ChoiceItem::new("2", "Hardware")])
                .unwrap(),
            Some("2".to_owned())
        );
        assert_eq!(
            ui.selection("Enabled", "yes", vec![ChoiceItem::new("yes", "On")])
                .unwrap(),
            Some("yes".to_owned())
        );
        assert_eq!(ui.input("Gain", "-3.0").unwrap(), Some("-6.0".to_owned()));
        ui.message("Error", "Invalid").unwrap();
        let state = backend.0.borrow();
        assert!(matches!(
            &state.requests[0],
            DialogRequest::Menu {
                default_item,
                cancel_label,
                ..
            } if default_item == "2" && cancel_label == "Back"
        ));
        assert!(matches!(
            &state.requests[2],
            DialogRequest::Input { initial, .. } if initial == " -3.0"
        ));
        assert!(matches!(&state.requests[3], DialogRequest::Message { .. }));
    }

    #[test]
    fn negative_input_is_presented_without_becoming_a_whiptail_option() {
        let backend = ScriptedBackend::with_responses([DialogResponse::Cancelled]);
        let mut ui = AccessibleUi::new(backend.clone());
        assert_eq!(ui.input("Gain", "-3.0").unwrap(), None);
        assert!(matches!(
            &backend.0.borrow().requests[0],
            DialogRequest::Input { initial, .. } if initial == " -3.0"
        ));
    }

    #[test]
    fn cancellation_and_backend_failures_remain_distinct() {
        let backend = ScriptedBackend(Rc::new(RefCell::new(ScriptState {
            responses: VecDeque::from([
                Ok(DialogResponse::Cancelled),
                Err(UiError::message("broken backend")),
            ]),
            requests: Vec::new(),
        })));
        let mut ui = AccessibleUi::new(backend.clone());
        assert_eq!(ui.input("Value", "0").unwrap(), None);
        let error = ui.input("Value", "0").unwrap_err();
        assert_eq!(error.to_string(), "broken backend");
        assert!(std::error::Error::source(&error).is_none());
    }

    #[test]
    fn decisions_and_confirmations_preserve_action_specific_labels() {
        let backend = ScriptedBackend::with_responses([
            DialogResponse::Accepted("discard".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let mut ui = AccessibleUi::new(backend.clone());
        assert_eq!(
            ui.decision(
                "Changed",
                "save",
                vec![ChoiceItem::new("discard", "Discard")],
                "Continue editing",
            )
            .unwrap(),
            Some("discard".to_owned())
        );
        assert!(
            ui.confirm("Restore", "Restore saved values?", "Restore", "Cancel")
                .unwrap()
        );
        assert!(
            !ui.confirm("Restore", "Restore saved values?", "Restore", "Cancel")
                .unwrap()
        );
        let state = backend.0.borrow();
        assert!(matches!(
            &state.requests[0],
            DialogRequest::Menu { cancel_label, .. } if cancel_label == "Continue editing"
        ));
        assert!(matches!(
            &state.requests[1],
            DialogRequest::Confirmation { accept_label, cancel_label, .. }
                if accept_label == "Restore" && cancel_label == "Cancel"
        ));
    }

    #[test]
    fn whiptail_arguments_encode_accessibility_contract() {
        let backend = WhiptailBackend::new("whiptail-test");
        for request in [
            DialogRequest::Menu {
                prompt: "Menu".to_owned(),
                default_item: "one".to_owned(),
                items: vec![ChoiceItem::new("one", "First")],
                cancel_label: "Back".to_owned(),
            },
            DialogRequest::Input {
                prompt: "Value".to_owned(),
                initial: "1".to_owned(),
            },
            DialogRequest::Message {
                title: "Notice".to_owned(),
                body: "Body".to_owned(),
            },
        ] {
            assert!(!backend.arguments(&request).is_empty());
        }
        let arguments = backend.arguments(&DialogRequest::Selection {
            prompt: "Mode\nCurrent: one".to_owned(),
            default_item: "one".to_owned(),
            items: vec![
                ChoiceItem::new("one", "First"),
                ChoiceItem::new("two", "Second"),
            ],
        });
        let text = arguments
            .iter()
            .map(|argument| argument.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        assert_eq!(text[..2].join("|"), "--backtitle|USBRadioPlus processing");
        assert!(
            text.windows(2)
                .any(|pair| pair.join("|") == "--ok-button|Apply")
        );
        assert!(
            text.windows(2)
                .any(|pair| pair.join("|") == "--cancel-button|Cancel")
        );
        assert!(
            text.windows(2)
                .any(|pair| pair.join("|") == "--default-item|one")
        );
        assert_eq!(
            text[text.len() - 6..].join("|"),
            "one|First|ON|two|Second|OFF"
        );

        let confirmation = backend.arguments(&DialogRequest::Confirmation {
            prompt: "Restore?".to_owned(),
            accept_label: "Restore".to_owned(),
            cancel_label: "Cancel".to_owned(),
        });
        assert_eq!(
            confirmation
                .iter()
                .map(|value| value.to_string_lossy())
                .collect::<Vec<_>>()
                .join("|"),
            "--backtitle|USBRadioPlus processing|--yes-button|Restore|--no-button|Cancel|--yesno|Restore?|8|70"
        );
    }

    #[test]
    fn whiptail_process_response_maps_success_cancel_and_launch_error() {
        let directory = test_directory("backend");
        let success = directory.join("success");
        let cancel = directory.join("cancel");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::write(&success, "#!/bin/sh\nprintf 'chosen\\n' >&2\n").unwrap();
            fs::write(&cancel, "#!/bin/sh\nexit 1\n").unwrap();
            fs::set_permissions(&success, fs::Permissions::from_mode(0o700)).unwrap();
            fs::set_permissions(&cancel, fs::Permissions::from_mode(0o700)).unwrap();
            let request = DialogRequest::Input {
                prompt: "Value".to_owned(),
                initial: "0".to_owned(),
            };
            assert_eq!(
                WhiptailBackend::new(&success).show(&request).unwrap(),
                DialogResponse::Accepted("chosen".to_owned())
            );
            assert_eq!(
                WhiptailBackend::new(&cancel).show(&request).unwrap(),
                DialogResponse::Cancelled
            );
            assert!(
                WhiptailBackend::new(directory.join("missing"))
                    .show(&request)
                    .is_err()
            );
        }
        fs::remove_dir_all(directory).unwrap();
    }

    fn test_directory(label: &str) -> std::path::PathBuf {
        let path = std::env::temp_dir().join(format!(
            "usbradioplus-tune-ui-{label}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn ui_error_with_io_source_reports_both_layers() {
        let error = UiError::process("operation", io::Error::other("cause"));
        assert_eq!(error.to_string(), "operation: cause");
        assert!(std::error::Error::source(&error).is_some());
        assert_eq!(OsStr::new("whiptail"), WhiptailBackend::default().program);
    }
}
