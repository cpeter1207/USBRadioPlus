//! Consistent screen-reader-friendly Whiptail user interface.

use std::ffi::OsString;
use std::fmt;
use std::io;
use std::process::{Command, Stdio};

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
