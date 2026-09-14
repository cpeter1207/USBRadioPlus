//! Typed configuration browsing and editing menus.

use std::collections::BTreeMap;
use std::fmt;
use std::time::Duration;

use usbradioplus_core::{
    CarrierSource, ChainRole, ConfigDocument, ConfigError, CtcssTone, DcsCode,
    HardwareOutputAssignment, ReceiveAudioSource, ResolutionWarningKind, ResolvedProfile,
    ResolvedStationConfig, SignalingMethod, StageOrder,
};

const PCM_SCALE: f32 = 32_768.0;
const RX_NOISE_TARGET: f32 = 27_000.0 / PCM_SCALE;
const RX_NOISE_TOLERANCE: f32 = 2_750.0 / PCM_SCALE;
const RX_VOICE_TARGET: f32 = 7_200.0 / PCM_SCALE;
const RX_VOICE_TOLERANCE: f32 = 360.0 / PCM_SCALE;
const RX_CTCSS_TARGET: f32 = 2_400.0 / PCM_SCALE;
const RX_CTCSS_TOLERANCE: f32 = 100.0 / PCM_SCALE;

#[derive(Clone, Copy)]
enum CalibrationMeasurement {
    ReceiveOutput,
    ReceiveCtcss,
}

#[derive(Clone, Copy)]
struct GainCalibration {
    section: &'static str,
    key: &'static str,
    label: &'static str,
    target: f32,
    tolerance: f32,
    minimum: f32,
    maximum: f32,
    initial: f32,
    base_multiplier: f32,
    observation: Duration,
    measurement: CalibrationMeasurement,
}

#[derive(Clone, Copy)]
enum TransmitCalibration {
    Voice,
    Ctcss,
    Auxiliary,
}

struct TransmitCalibrationPlan {
    section: &'static str,
    key: &'static str,
    label: String,
    test_tone: bool,
    forced_ctcss: Option<CtcssTone>,
}

fn mixer_gain_db(level: u32) -> f32 {
    20.0 * ((level.max(1) as f32) / 500.0).log10()
}

fn dbfs(level: f32) -> f32 {
    if level > 0.0 {
        20.0 * level.log10()
    } else {
        -96.0
    }
}

fn format_decimal(value: f32) -> String {
    if value.abs() < 0.000_000_5 {
        return "0".to_owned();
    }
    let text = format!("{value:.6}");
    text.trim_end_matches('0').trim_end_matches('.').to_owned()
}

use crate::repository::ConfigRepository;
#[cfg(test)]
use crate::ui::DialogBackend;
use crate::ui::{AccessibleUi, ChoiceItem, UiError};

/// Runtime action required after a configuration edit.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ApplyMode {
    /// Reload the active module configuration in place.
    Reload,
    /// Restart Asterisk because construction-time state changed.
    Restart,
}

/// Inclusive bounds and display units for an integer setting.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IntegerEditor {
    /// Optional inclusive minimum.
    pub minimum: Option<i64>,
    /// Optional inclusive maximum.
    pub maximum: Option<i64>,
    /// Unit appended to limits and displayed values.
    pub units: String,
}

/// Bounds and display units for a floating-point setting.
#[derive(Clone, Debug, PartialEq)]
pub struct FloatEditor {
    /// Optional minimum.
    pub minimum: Option<f64>,
    /// Optional inclusive maximum.
    pub maximum: Option<f64>,
    /// Whether a supplied minimum is exclusive.
    pub exclusive_minimum: bool,
    /// Unit appended to limits and displayed values.
    pub units: String,
}

/// Typed editor used by one configuration option.
#[derive(Clone, Debug, PartialEq)]
pub enum SettingKind {
    /// On/off radiolist stored as `yes` or `no`.
    OnOff,
    /// Enumerated radiolist containing value/description pairs.
    Enumeration(Vec<ChoiceItem>),
    /// Enumeration which may remove its explicit assignment and inherit.
    OptionalEnumeration(Vec<ChoiceItem>),
    /// Integer input with optional bounds.
    Integer(IntegerEditor),
    /// Floating-point input with optional bounds.
    Float(FloatEditor),
    /// Decibel gain input with optional bounds.
    Gain(FloatEditor),
    /// Text input checked according to its declared syntax.
    Text(TextEditor),
}

/// Syntax accepted by a text-input setting.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TextSyntax {
    /// Text which may be empty to request automatic selection.
    Optional,
    /// Any nonempty text.
    NonEmpty,
    /// A comma-separated list of exact native CTCSS tones.
    CtcssToneList,
    /// One exact native CTCSS tone.
    CtcssTone,
    /// Three octal digits followed by normal or inverted polarity.
    DcsCode,
    /// A comma-separated optional-processing stage order.
    StageOrder,
}

const INHERIT_VALUE: &str = "__inherit__";
const HARDWARE_IDENTITY_DEFAULTS: [(&str, &str); 3] = [
    ("hardware_device_identifier", ""),
    ("hardware_serial", ""),
    ("hardware_gpio_usb_port_path", ""),
];
const PROFILE_SELECTORS: [(&str, &str, &str); 11] = [
    ("asterisk_profile", "Asterisk profile", "asterisk"),
    ("hardware_profile", "Hardware profile", "hardware"),
    ("receive_profile", "Receive profile", "receive"),
    ("transmit_profile", "Transmit profile", "transmit"),
    ("ctcss_profile", "CTCSS profile", "ctcss"),
    ("dcs_profile", "DCS profile", "dcs"),
    ("duplex_profile", "Duplex profile", "duplex"),
    ("diagnostics_profile", "Diagnostics profile", "diagnostics"),
    ("local_profile", "Local receiver profile", "local"),
    ("link_profile", "Linked audio profile", "link"),
    (
        "voice_telemetry_profile",
        "Voice + telemetry profile",
        "voice_telemetry",
    ),
];

/// Prompt and syntax for a text-input setting.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TextEditor {
    /// Short input instruction displayed above the current value.
    pub instruction: String,
    /// Syntax validated before the file is changed.
    pub syntax: TextSyntax,
}

/// One editable configuration setting.
#[derive(Clone, Debug, PartialEq)]
pub struct SettingSpec {
    /// Configuration assignment name.
    pub key: String,
    /// Operator-facing setting label.
    pub label: String,
    /// Concrete fallback shown when the assignment is absent.
    pub default_value: String,
    /// Typed editor and validation policy.
    pub kind: SettingKind,
    /// Runtime action required after changing this setting.
    pub apply_mode: ApplyMode,
}

/// One navigable configuration section.
#[derive(Clone, Debug, PartialEq)]
pub struct SectionSpec {
    /// Flat section kind, such as `hardware` or `local`.
    pub section: String,
    /// Operator-facing menu title.
    pub title: String,
    /// Ordered settings shown in this section.
    pub settings: Vec<SettingSpec>,
}

/// One navigation page in the typed configuration hierarchy.
#[derive(Clone, Debug, PartialEq)]
pub struct MenuPage {
    /// Stable identifier used to retain this page's cursor position.
    pub id: String,
    /// Operator-facing page title.
    pub title: String,
    /// Ordered submenus and setting groups.
    pub entries: Vec<MenuEntry>,
}

/// One entry in a configuration navigation page.
#[derive(Clone, Debug, PartialEq)]
pub enum MenuEntry {
    /// Descend into another navigation page.
    Page(MenuPage),
    /// Open one group of settings that share a target section.
    Section(SectionSpec),
    /// Run a bounded live control or status action.
    Action(ActionSpec),
}

/// Live operation exposed beside configuration menus.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LiveAction {
    /// Show processing-chain measurements.
    ProcessingStatistics,
    /// Show complete radio and processing configuration.
    ActiveConfiguration,
    /// Show combined radio, signal, meter, and native-stream status.
    RadioStatus,
    /// Key the transmitter for three short calibration bursts.
    FlashTransmitter,
    /// Automatically calibrate discriminator noise and DSP squelch.
    CalibrateReceiveNoise,
    /// Automatically calibrate receive voice with the documented reference signal.
    CalibrateReceiveVoice,
    /// Automatically calibrate the receive CTCSS decoder input level.
    CalibrateReceiveCtcss,
    /// Display continuously updated signaling and audio measurements.
    ContinuousRadioMeters,
    /// Persist live radio tuning values and enabled EEPROM storage.
    SaveRadioTuning,
}

/// One coherent live measurement snapshot used by radio calibration.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RadioStatus {
    /// Raw capture peak on the normalized PCM scale.
    pub receive_input_peak: f32,
    /// Raw capture RMS on the normalized PCM scale.
    pub receive_input_rms: f32,
    /// Processed local-receive peak on the normalized PCM scale.
    pub receive_output_peak: f32,
    /// Processed local-receive RMS on the normalized PCM scale.
    pub receive_output_rms: f32,
    /// Post-decoder-gain CTCSS half peak-to-peak on the normalized PCM scale.
    pub receive_ctcss_decoder_peak: f32,
    /// Latest native discriminator-noise measurement.
    pub receive_rssi_peak: i32,
    /// Whether the current snapshot completed an RSSI integration window.
    pub receive_rssi_updated: bool,
    /// Raw capture samples at a PCM rail in the latest callback.
    pub receive_input_rail_samples: u64,
    /// Current normalized CM119 receive mixer level.
    pub receive_mixer_level: u32,
}

/// One user-adjustable CM119 hardware mixer.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwareMixer {
    /// Receiver capture level.
    Receive,
    /// First playback output.
    TransmitA,
    /// Second playback output.
    TransmitB,
}

/// Action owned by either the tuner session or its runtime-control port.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MenuAction {
    /// Dispatch one semantic operation to the live radio adapter.
    Live(LiveAction),
    /// Select named configuration profiles for the active channel.
    SelectProfiles,
    /// Exchange USB-device ownership with another configured channel.
    SwapUsbDevice,
    /// Enable or disable app_rpt echo playback.
    ToggleEcho,
    /// Key a calibrated test tone while editing the routed voice output gain.
    AdjustTransmitVoice,
    /// Key the configured transmit CTCSS tone while editing its deviation level.
    AdjustTransmitCtcss,
    /// Key a calibrated test tone while editing the routed auxiliary output gain.
    AdjustAuxiliaryOutput,
    /// Mark the current configuration as the session restore point.
    SaveSession,
    /// Write an independent timestamped backup.
    BackupConfiguration,
    /// Restore the most recently saved session state.
    DiscardSessionChanges,
}

/// One operator-facing live menu action.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ActionSpec {
    /// Menu text describing the operation.
    pub label: String,
    /// Title used to display the operation result.
    pub result_title: String,
    /// Typed live operation dispatched by the control boundary.
    pub action: MenuAction,
}

impl MenuEntry {
    fn title(&self) -> &str {
        match self {
            Self::Page(page) => &page.title,
            Self::Section(section) => &section.title,
            Self::Action(action) => &action.label,
        }
    }
}

/// Durable configuration operations consumed by the menu state machine.
pub trait ConfigurationStore {
    /// Read the complete current configuration.
    ///
    /// # Errors
    ///
    /// Returns a storage-specific message when reading fails.
    fn read_config(&self) -> Result<String, String>;
    /// Atomically replace the complete current configuration.
    ///
    /// # Errors
    ///
    /// Returns a storage-specific message when replacement fails.
    fn write_config(&self, contents: &str) -> Result<(), String>;
    /// Create an independent backup and return its display path.
    ///
    /// # Errors
    ///
    /// Returns a storage-specific message when backup creation fails.
    fn backup_config(&self) -> Result<String, String>;
}

impl ConfigurationStore for ConfigRepository {
    fn read_config(&self) -> Result<String, String> {
        self.read().map_err(|error| error.to_string())
    }

    fn write_config(&self, contents: &str) -> Result<(), String> {
        self.write_atomic(contents)
            .map_err(|error| error.to_string())
    }

    fn backup_config(&self) -> Result<String, String> {
        self.create_backup()
            .map(|path| path.display().to_string())
            .map_err(|error| error.to_string())
    }
}

/// Runtime application boundary kept separate from file mutation.
pub trait ConfigurationApplier {
    /// Apply a completed file edit using the selected runtime action.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when the new generation cannot be applied.
    fn apply(&mut self, mode: ApplyMode) -> Result<(), String>;
}

/// Active-channel boundary used by named-channel selection.
pub trait ChannelControl {
    /// Return the currently active `RadioPlus` channel when one is available.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when active state cannot be queried.
    fn active_channel(&mut self) -> Result<Option<String>, String>;
    /// Make one configured `RadioPlus` channel active.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when selection fails.
    fn select_channel(&mut self, channel: &str) -> Result<(), String>;
    /// Read one coherent live status snapshot.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when status is unavailable.
    fn radio_status(&mut self) -> Result<RadioStatus, String>;
    /// Set one normalized hardware mixer level.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when the mixer cannot be changed.
    fn set_hardware_mixer(&mut self, mixer: HardwareMixer, level: u32) -> Result<(), String>;
    /// Enable or disable the calibrated transmitter test tone.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when the tone state cannot be changed.
    fn set_test_tone(&mut self, enabled: bool) -> Result<(), String>;
    /// Key or unkey the transmitter, optionally selecting a forced CTCSS tone.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when PTT cannot be changed.
    fn set_transmit(&mut self, keyed: bool, forced_ctcss: Option<CtcssTone>) -> Result<(), String>;
    /// Persist the active station's live tuning values to its EEPROM.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when EEPROM storage is unavailable or fails.
    fn save_current_tuning_to_eeprom(&mut self) -> Result<String, String>;
    /// Wait between bounded control-plane calibration observations.
    fn wait(&mut self, duration: std::time::Duration);
    /// Run one typed live operation against the active channel.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when the operation is unavailable or fails.
    fn perform(&mut self, action: LiveAction) -> Result<String, String>;
    /// Return whether echo playback is currently enabled.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when echo state cannot be queried.
    fn echo_enabled(&mut self) -> Result<bool, String>;
    /// Enable or disable echo playback and return an operator-facing result.
    ///
    /// # Errors
    ///
    /// Returns a controller-specific message when echo state cannot be changed.
    fn set_echo_enabled(&mut self, enabled: bool) -> Result<String, String>;
}

/// Failure that prevents the menu session from continuing safely.
#[derive(Debug)]
pub enum MenuError {
    /// The dialog backend failed.
    Ui(UiError),
    /// Configuration structure could not be resolved or edited.
    Configuration(ConfigError),
    /// Durable configuration storage failed.
    Storage(String),
    /// Live module or service control failed.
    Control(String),
}

impl fmt::Display for MenuError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ui(error) => write!(formatter, "user interface failed: {error}"),
            Self::Configuration(error) => write!(formatter, "configuration failed: {error}"),
            Self::Storage(error) => write!(formatter, "configuration storage failed: {error}"),
            Self::Control(error) => write!(formatter, "live control failed: {error}"),
        }
    }
}

impl std::error::Error for MenuError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Ui(error) => Some(error),
            Self::Configuration(error) => Some(error),
            Self::Storage(_) | Self::Control(_) => None,
        }
    }
}

impl From<UiError> for MenuError {
    fn from(error: UiError) -> Self {
        Self::Ui(error)
    }
}

impl From<ConfigError> for MenuError {
    fn from(error: ConfigError) -> Self {
        Self::Configuration(error)
    }
}

/// Stateful typed menu browser for one tuning session.
pub struct TunerApp {
    ui: AccessibleUi,
    store: Box<dyn ConfigurationStore>,
    applier: Box<dyn ConfigurationApplier>,
    channels: Box<dyn ChannelControl>,
    selected_channel: Option<String>,
    top_cursor: String,
    page_cursors: BTreeMap<String, String>,
    section_cursors: BTreeMap<String, String>,
    saved_config: Option<String>,
    restart_changed: bool,
}

impl TunerApp {
    /// Construct a menu session with an optional command-line channel selection.
    pub fn new(
        ui: AccessibleUi,
        store: impl ConfigurationStore + 'static,
        applier: impl ConfigurationApplier + 'static,
        channels: impl ChannelControl + 'static,
        selected_channel: Option<String>,
    ) -> Self {
        Self {
            ui,
            store: Box::new(store),
            applier: Box::new(applier),
            channels: Box::new(channels),
            selected_channel,
            top_cursor: "1".to_owned(),
            page_cursors: BTreeMap::new(),
            section_cursors: BTreeMap::new(),
            saved_config: None,
            restart_changed: false,
        }
    }

    /// Browse a hierarchical configuration catalog until the operator selects Exit.
    ///
    /// # Errors
    ///
    /// Returns [`MenuError`] when configuration, UI, storage, or control fails.
    pub fn browse_tree(&mut self, root: &MenuPage) -> Result<(), MenuError> {
        self.saved_config = Some(self.store.read_config().map_err(MenuError::Storage)?);
        self.restart_changed = false;
        self.initialize_channel()?;
        loop {
            self.browse_page(root, true)?;
            if self.finish_session()? {
                return Ok(());
            }
        }
    }

    fn browse_page(&mut self, page: &MenuPage, top_level: bool) -> Result<(), MenuError> {
        let mut cursor = if top_level {
            self.top_cursor.clone()
        } else {
            self.page_cursors
                .get(&page.id)
                .cloned()
                .unwrap_or_else(|| "1".to_owned())
        };
        loop {
            let mut items = page
                .entries
                .iter()
                .enumerate()
                .map(|(index, entry)| ChoiceItem::new((index + 1).to_string(), entry.title()))
                .collect::<Vec<_>>();
            if top_level {
                items.push(ChoiceItem::new("C", "Select active radio channel"));
            }
            let channel = self.selected_channel.as_deref().unwrap_or("flat defaults");
            let prompt = if top_level {
                format!("{}\nCurrent channel: {channel}", page.title)
            } else {
                page.title.clone()
            };
            let choice = if top_level {
                self.ui.top_navigation(prompt, &cursor, items)?
            } else {
                self.ui.navigation(prompt, &cursor, items)?
            };
            let Some(choice) = choice else {
                if top_level {
                    self.top_cursor = cursor;
                } else {
                    self.page_cursors.insert(page.id.clone(), cursor);
                }
                return Ok(());
            };
            cursor.clone_from(&choice);
            if top_level {
                self.top_cursor.clone_from(&cursor);
            } else {
                self.page_cursors.insert(page.id.clone(), cursor.clone());
            }
            if top_level && choice.eq_ignore_ascii_case("C") {
                self.choose_channel()?;
                continue;
            }
            let Some(index) = menu_index(&choice, page.entries.len()) else {
                self.ui
                    .message("Invalid selection", "Select a listed configuration group.")?;
                continue;
            };
            match &page.entries[index] {
                MenuEntry::Page(child) => self.browse_page(child, false)?,
                MenuEntry::Section(section) => self.browse_section(section)?,
                MenuEntry::Action(action) => self.run_menu_action(action)?,
            }
        }
    }

    fn run_menu_action(&mut self, action: &ActionSpec) -> Result<(), MenuError> {
        match action.action {
            MenuAction::Live(live) => self.run_live_action(action, live),
            MenuAction::SelectProfiles => self.select_profiles(),
            MenuAction::SwapUsbDevice => self.swap_usb_device(),
            MenuAction::ToggleEcho => self.toggle_echo(),
            MenuAction::AdjustTransmitVoice => self.adjust_transmit(TransmitCalibration::Voice),
            MenuAction::AdjustTransmitCtcss => self.adjust_transmit(TransmitCalibration::Ctcss),
            MenuAction::AdjustAuxiliaryOutput => {
                self.adjust_transmit(TransmitCalibration::Auxiliary)
            }
            MenuAction::SaveSession => self.save_session(),
            MenuAction::BackupConfiguration => self.backup_configuration(),
            MenuAction::DiscardSessionChanges => self.discard_session_changes(true).map(|_| ()),
        }
    }

    fn run_live_action(&mut self, action: &ActionSpec, live: LiveAction) -> Result<(), MenuError> {
        let result = match live {
            LiveAction::ActiveConfiguration => self.complete_active_configuration(),
            LiveAction::CalibrateReceiveNoise => self.calibrate_receive_noise(),
            LiveAction::CalibrateReceiveVoice => self.calibrate_receive_voice(),
            LiveAction::CalibrateReceiveCtcss => self.calibrate_receive_ctcss(),
            _ => self.channels.perform(live),
        };
        match result {
            Ok(output) => self.ui.message(&action.result_title, output)?,
            Err(error) => self.ui.message(
                "Live operation failed",
                format!("{}\n\n{error}", action.label),
            )?,
        }
        Ok(())
    }

    fn complete_active_configuration(&mut self) -> Result<String, String> {
        let channel = self.calibration_channel()?;
        let document = ConfigDocument::new(self.store.read_config()?);
        let mut effective = BTreeMap::<String, BTreeMap<String, String>>::new();
        effective.insert(channel.clone(), document.explicit_values(&channel));
        let catalog = crate::catalog::configuration_catalog();
        let mut sections = Vec::new();
        collect_sections(&catalog, &mut sections);
        for section in sections {
            let (target, values) = self
                .section_values(&document, &section.section)
                .map_err(|error| error.to_string())?;
            let assignments = effective.entry(target).or_default();
            for setting in &section.settings {
                assignments.insert(
                    setting.key.clone(),
                    values
                        .get(&setting.key)
                        .cloned()
                        .unwrap_or_else(|| setting.default_value.clone()),
                );
            }
        }
        let mut output = format!("Active channel: {channel}\n");
        for (section, assignments) in effective {
            output.push_str(&format!("\n[{section}]\n"));
            for (key, value) in assignments {
                output.push_str(&format!("{key} = {value}\n"));
            }
        }
        output.push_str("\nLive state\n");
        output.push_str(&self.channels.perform(LiveAction::RadioStatus)?);
        Ok(output)
    }

    fn calibrate_receive_noise(&mut self) -> Result<String, String> {
        let channel = self.calibration_channel()?;
        let original = self.store.read_config()?;
        let document = ConfigDocument::new(original.clone());
        let station = ResolvedStationConfig::from_document(&document, "configuration", &channel)
            .map_err(|error| error.to_string())?;
        if station.config().receive.audio_source != ReceiveAudioSource::Flat {
            return Err("RX noise calibration requires receive_audio_source = flat".to_owned());
        }

        let mut level = 2_u32;
        let mut best = None;
        let mut last = RadioStatus::default();
        let mut attempts = 0_u32;
        for attempt in 0..48_u32 {
            attempts = attempt + 1;
            self.channels
                .set_hardware_mixer(HardwareMixer::Receive, level)?;
            self.channels.wait(Duration::from_millis(100));
            self.channels.wait(Duration::from_millis(400));
            last = self.channels.radio_status()?;
            let measured = last.receive_input_peak.max(1.0 / PCM_SCALE);
            if last.receive_input_rail_samples == 0
                && measured <= 1.0
                && best.is_none_or(|(_, peak): (u32, f32)| measured > peak)
            {
                best = Some((level, measured));
            }
            let in_range = (measured - RX_NOISE_TARGET).abs() <= RX_NOISE_TOLERANCE;
            if in_range && attempt > 5 {
                best = Some((level, measured));
                break;
            }
            level = if attempt <= 2 {
                ((level as f32 * RX_NOISE_TARGET / measured).round() as u32).clamp(1, 999)
            } else if measured < RX_NOISE_TARGET - RX_NOISE_TOLERANCE {
                level.saturating_add(1).min(999)
            } else if measured > RX_NOISE_TARGET + RX_NOISE_TOLERANCE {
                level.saturating_sub(1).max(1)
            } else {
                level
            };
        }

        let Some((level, measured)) = best else {
            return Err(format!(
                "RX noise calibration failed after {attempts} attempts; no unclipped mixer setting was measured"
            ));
        };
        self.channels
            .set_hardware_mixer(HardwareMixer::Receive, level)?;
        if station.config().receive.cos_assignment == CarrierSource::Dsp {
            let mut updated = false;
            for _ in 0..20 {
                self.channels.wait(Duration::from_millis(20));
                last = self.channels.radio_status()?;
                if last.receive_rssi_updated {
                    updated = true;
                    break;
                }
            }
            if !updated {
                return Err("DSP noise measurement did not complete during calibration".to_owned());
            }
        }

        let mut updated = document;
        let hardware = updated
            .resolved_section(&channel, "hardware")
            .map_err(|error| error.to_string())?;
        updated
            .set_value(
                &hardware,
                "hardware_input_gain_db",
                &format_decimal(mixer_gain_db(level)),
            )
            .map_err(|error| error.to_string())?;
        let mut squelch = None;
        if station.config().receive.cos_assignment == CarrierSource::Dsp {
            let normalized = (((32_767_i64 - i64::from(last.receive_rssi_peak)) * 999) / 32_767)
                .clamp(0, 999) as u16;
            let threshold = normalized.saturating_add(150).min(999);
            let receive = updated
                .resolved_section(&channel, "receive")
                .map_err(|error| error.to_string())?;
            updated
                .set_value(&receive, "squelch_level", &threshold.to_string())
                .map_err(|error| error.to_string())?;
            squelch = Some((normalized, threshold));
        }
        ResolvedStationConfig::from_document(&updated, "configuration", &channel)
            .map_err(|error| error.to_string())?;
        self.apply_generation(&original, updated.as_str(), ApplyMode::Reload)?;

        let peak_dbfs = dbfs(measured);
        let rms_dbfs = dbfs(last.receive_input_rms);
        let mut result = format!(
            "RX noise calibration complete.\n\nAttempts: {attempts}\nHardware input: {level}/999\nPeak: {:.0} codes ({peak_dbfs:.1} dBFS)\nRMS: {:.0} codes ({rms_dbfs:.1} dBFS)",
            measured * PCM_SCALE,
            last.receive_input_rms * PCM_SCALE,
        );
        if let Some((noise, threshold)) = squelch {
            result.push_str(&format!(
                "\nDSP noise level: {noise}/999\nDSP squelch: {threshold}/999"
            ));
            let noise_peak = last.receive_rssi_peak.max(1) as f32;
            if measured * PCM_SCALE / (noise_peak / 10.0) > 26.0 {
                result.push_str(
                    "\n\nWarning: insufficient high-frequency discriminator noise; the input may be de-emphasized or too quiet for reliable DSP carrier detection.",
                );
            }
        }
        Ok(result)
    }

    fn calibrate_receive_voice(&mut self) -> Result<String, String> {
        self.calibrate_receive_gain(GainCalibration {
            section: "local",
            key: "input_gain_db",
            label: "RX voice",
            target: RX_VOICE_TARGET,
            tolerance: RX_VOICE_TOLERANCE,
            minimum: 0.1,
            maximum: 5.0,
            initial: 1.0,
            base_multiplier: 2.0,
            observation: Duration::from_millis(1_000),
            measurement: CalibrationMeasurement::ReceiveOutput,
        })
    }

    fn calibrate_receive_ctcss(&mut self) -> Result<String, String> {
        self.calibrate_receive_gain(GainCalibration {
            section: "ctcss",
            key: "receive_decoder_gain_db",
            label: "RX CTCSS",
            target: RX_CTCSS_TARGET,
            tolerance: RX_CTCSS_TOLERANCE,
            minimum: 0.1,
            maximum: 8.0,
            initial: 1.0,
            base_multiplier: 1.0,
            observation: Duration::from_millis(500),
            measurement: CalibrationMeasurement::ReceiveCtcss,
        })
    }

    fn calibrate_receive_gain(&mut self, calibration: GainCalibration) -> Result<String, String> {
        let channel = self.calibration_channel()?;
        let original = self.store.read_config()?;
        let mut current = original.clone();
        let target_section = ConfigDocument::new(original.clone())
            .resolved_section(&channel, calibration.section)
            .map_err(|error| error.to_string())?;
        let mut multiplier = calibration.initial;
        let mut measured = 0.0_f32;
        let mut attempts = 0_u32;

        for attempt in 0..12_u32 {
            attempts = attempt + 1;
            let mut candidate = ConfigDocument::new(current.clone());
            let gain_db = 20.0 * (calibration.base_multiplier * multiplier).log10();
            candidate
                .set_value(&target_section, calibration.key, &format_decimal(gain_db))
                .map_err(|error| error.to_string())?;
            self.validate_candidate(&candidate, calibration.section, calibration.key)?;
            if let Err(error) =
                self.apply_generation(&current, candidate.as_str(), ApplyMode::Reload)
            {
                return self.restore_failed_calibration(&current, &original, error);
            }
            current = candidate.as_str().to_owned();
            self.channels.wait(Duration::from_millis(10));
            self.channels.wait(calibration.observation);
            let status = match self.channels.radio_status() {
                Ok(status) => status,
                Err(error) => return self.restore_failed_calibration(&current, &original, error),
            };
            measured = match calibration.measurement {
                CalibrationMeasurement::ReceiveOutput => status.receive_output_peak,
                CalibrationMeasurement::ReceiveCtcss => status.receive_ctcss_decoder_peak,
            }
            .max(1.0 / PCM_SCALE);
            let in_range = (measured - calibration.target).abs() <= calibration.tolerance;
            if in_range && attempt > 4 {
                return Ok(format!(
                    "{} calibration complete.\n\nAttempts: {attempts}\nGain: {} dB\nPeak: {:.0} codes ({:.1} dBFS)",
                    calibration.label,
                    format_decimal(gain_db),
                    measured * PCM_SCALE,
                    dbfs(measured),
                ));
            }
            multiplier = (multiplier * calibration.target / measured)
                .clamp(calibration.minimum, calibration.maximum);
        }

        self.restore_failed_calibration(
            &current,
            &original,
            format!(
                "{} calibration failed after {attempts} attempts; the last peak was {:.0} codes ({:.1} dBFS)",
                calibration.label,
                measured * PCM_SCALE,
                dbfs(measured),
            ),
        )
    }

    fn calibration_channel(&self) -> Result<String, String> {
        self.selected_channel
            .clone()
            .ok_or_else(|| "select a radio channel before calibrating it".to_owned())
    }

    fn restore_failed_calibration(
        &mut self,
        current: &str,
        original: &str,
        error: String,
    ) -> Result<String, String> {
        if current == original {
            return Err(error);
        }
        match self.apply_generation(current, original, ApplyMode::Reload) {
            Ok(()) => Err(format!(
                "{error}\n\nThe previous configuration was restored."
            )),
            Err(rollback) => Err(format!(
                "{error}\n\nRestoring the previous configuration failed: {rollback}"
            )),
        }
    }

    fn save_session(&mut self) -> Result<(), MenuError> {
        self.saved_config = Some(self.store.read_config().map_err(MenuError::Storage)?);
        self.restart_changed = false;
        self.ui.message(
            "Configuration saved",
            "Current processing settings are saved as this session's restore point.",
        )?;
        Ok(())
    }

    fn backup_configuration(&mut self) -> Result<(), MenuError> {
        let path = self.store.backup_config().map_err(MenuError::Storage)?;
        self.ui
            .message("Configuration backup", format!("Backup created:\n{path}"))?;
        Ok(())
    }

    fn discard_session_changes(&mut self, confirm: bool) -> Result<bool, MenuError> {
        let current = self.store.read_config().map_err(MenuError::Storage)?;
        let saved = self.saved_config.clone().unwrap_or_else(|| current.clone());
        if current == saved {
            return Ok(true);
        }
        if confirm
            && !self.ui.confirm(
                "Restore configuration",
                "Restore the configuration from the last save?",
                "Restore",
                "Cancel",
            )?
        {
            return Ok(false);
        }
        let mode = if self.restart_changed {
            ApplyMode::Restart
        } else {
            ApplyMode::Reload
        };
        if self.apply_edit(&current, &saved, mode)? {
            self.restart_changed = false;
            Ok(true)
        } else {
            Ok(false)
        }
    }

    fn finish_session(&mut self) -> Result<bool, MenuError> {
        let current = self.store.read_config().map_err(MenuError::Storage)?;
        if self.saved_config.as_deref() == Some(current.as_str()) {
            return Ok(true);
        }
        let choice = self.ui.decision(
            "Live processing settings have changed.",
            "save",
            vec![
                ChoiceItem::new("save", "Save changes and exit"),
                ChoiceItem::new("discard", "Discard changes and exit"),
            ],
            "Continue editing",
        )?;
        match choice.as_deref() {
            Some("save") => {
                self.saved_config = Some(current);
                self.restart_changed = false;
                Ok(true)
            }
            Some("discard") => self.discard_session_changes(false),
            _ => Ok(false),
        }
    }

    fn swap_usb_device(&mut self) -> Result<(), MenuError> {
        let Some(active) = self.selected_channel.clone() else {
            self.ui.message(
                "USB-device swap",
                "Select a configured named radio channel before swapping devices.",
            )?;
            return Ok(());
        };
        let mut document = self.document()?;
        let mut channels = document.configured_channels();
        channels.retain(|channel| !channel.eq_ignore_ascii_case(&active));
        channels.sort_by_key(|channel| channel.to_ascii_lowercase());
        channels.dedup_by(|left, right| left.eq_ignore_ascii_case(right));
        let Some(default) = channels.first().cloned() else {
            self.ui.message(
                "USB-device swap",
                "No eligible USB radio channels were found.",
            )?;
            return Ok(());
        };
        let choices = channels
            .into_iter()
            .map(|channel| ChoiceItem::new(&channel, format!("Radio channel {channel}")))
            .collect();
        let Some(selected) = self
            .ui
            .selection("USB radio channel to swap", &default, choices)?
        else {
            return Ok(());
        };
        let original = document.as_str().to_owned();
        if let Err(error) = swap_hardware_identity(&mut document, &active, &selected) {
            self.ui.message("USB-device swap failed", error)?;
            return Ok(());
        }
        for channel in [&active, &selected] {
            if let Err(error) =
                ResolvedStationConfig::from_document(&document, "configuration", channel)
            {
                self.ui
                    .message("USB-device swap failed", error.to_string())?;
                return Ok(());
            }
        }
        if !self.apply_edit(&original, document.as_str(), ApplyMode::Restart)? {
            return Ok(());
        }
        self.restart_changed = true;
        self.channels
            .select_channel(&active)
            .map_err(MenuError::Control)?;
        self.ui.message(
            "USB-device swap",
            format!("USB-device assignments for {active} and {selected} were exchanged."),
        )?;
        Ok(())
    }

    fn toggle_echo(&mut self) -> Result<(), MenuError> {
        let enabled = match self.channels.echo_enabled() {
            Ok(enabled) => enabled,
            Err(error) => {
                self.ui.message("Echo mode failed", error)?;
                return Ok(());
            }
        };
        let current = if enabled { "yes" } else { "no" };
        let Some(selected) = self.ui.selection(
            format!("Echo mode\nCurrent: {}", if enabled { "On" } else { "Off" }),
            current,
            vec![ChoiceItem::new("yes", "On"), ChoiceItem::new("no", "Off")],
        )?
        else {
            return Ok(());
        };
        match self.channels.set_echo_enabled(selected == "yes") {
            Ok(output) => self.ui.message("Echo mode", output)?,
            Err(error) => self.ui.message("Echo mode failed", error)?,
        }
        Ok(())
    }

    fn adjust_transmit(&mut self, kind: TransmitCalibration) -> Result<(), MenuError> {
        let plan = match self.transmit_calibration_plan(kind) {
            Ok(plan) => plan,
            Err(error) => {
                self.ui
                    .message(format!("{} unavailable", calibration_title(kind)), error)?;
                return Ok(());
            }
        };
        self.adjust_keyed_setting(
            plan.section,
            plan.key,
            &plan.label,
            plan.test_tone,
            plan.forced_ctcss,
        )
    }

    fn transmit_calibration_plan(
        &self,
        kind: TransmitCalibration,
    ) -> Result<TransmitCalibrationPlan, String> {
        let station = self.station_for_calibration()?;
        let outputs = (
            station.hardware.output_a_assignment,
            station.hardware.output_b_assignment,
        );
        match kind {
            TransmitCalibration::Voice => Ok(TransmitCalibrationPlan {
                section: "hardware",
                key: routed_output_gain(outputs, |assignment| {
                    matches!(
                        assignment,
                        HardwareOutputAssignment::Voice | HardwareOutputAssignment::VoiceCtcss
                    )
                })
                .ok_or_else(|| {
                    "Assign voice or voice_ctcss to a hardware output before calibrating it."
                        .to_owned()
                })?,
                label: "Hardware voice output gain; transmitter is keyed with the calibrated 1 kHz tone".to_owned(),
                test_tone: true,
                forced_ctcss: None,
            }),
            TransmitCalibration::Auxiliary => Ok(TransmitCalibrationPlan {
                section: "hardware",
                key: routed_output_gain(outputs, |assignment| {
                    assignment == HardwareOutputAssignment::AuxiliaryVoice
                })
                .ok_or_else(|| {
                    "Assign auxvoice to a hardware output before calibrating it.".to_owned()
                })?,
                label: "Hardware auxiliary output gain; transmitter is keyed with the calibrated 1 kHz tone".to_owned(),
                test_tone: true,
                forced_ctcss: None,
            }),
            TransmitCalibration::Ctcss => {
                if station.transmit.signaling_method != SignalingMethod::Ctcss {
                    return Err(
                        "Select CTCSS transmit signaling before calibrating it.".to_owned(),
                    );
                }
                let tone = station.ctcss.transmit_default;
                Ok(TransmitCalibrationPlan {
                    section: "ctcss",
                    key: "transmit_peak_dbfs",
                    label: format!("Transmit CTCSS peak; transmitter is keyed with {tone} Hz"),
                    test_tone: false,
                    forced_ctcss: Some(tone),
                })
            }
        }
    }

    fn station_for_calibration(&self) -> Result<usbradioplus_core::StationConfig, String> {
        let channel = self.calibration_channel()?;
        let document = ConfigDocument::new(self.store.read_config()?);
        ResolvedStationConfig::from_document(&document, "configuration", &channel)
            .map(ResolvedStationConfig::into_config)
            .map_err(|error| error.to_string())
    }

    fn adjust_keyed_setting(
        &mut self,
        section_name: &str,
        key: &str,
        label: &str,
        test_tone: bool,
        forced_ctcss: Option<CtcssTone>,
    ) -> Result<(), MenuError> {
        if test_tone {
            if let Err(error) = self.channels.set_test_tone(true) {
                self.ui.message("Calibration unavailable", error)?;
                return Ok(());
            }
        }
        if let Err(error) = self.channels.set_transmit(true, forced_ctcss) {
            if test_tone {
                let _ = self.channels.set_test_tone(false);
            }
            self.ui.message("Calibration unavailable", error)?;
            return Ok(());
        }

        let edit = self.edit_catalog_setting(section_name, key, label);
        let unkey = self.channels.set_transmit(false, None);
        let tone_off = test_tone
            .then(|| self.channels.set_test_tone(false))
            .transpose();
        edit?;
        if let Err(error) = unkey.and(tone_off) {
            self.ui.message("Calibration cleanup failed", error)?;
        }
        Ok(())
    }

    fn edit_catalog_setting(
        &mut self,
        section_name: &str,
        key: &str,
        label: &str,
    ) -> Result<(), MenuError> {
        let catalog = crate::catalog::configuration_catalog();
        let mut sections = Vec::new();
        collect_sections(&catalog, &mut sections);
        let section = sections
            .into_iter()
            .find(|section| {
                section.section == section_name
                    && section.settings.iter().any(|setting| setting.key == key)
            })
            .ok_or_else(|| MenuError::Control(format!("tuner catalog omits {section_name}.{key}")))?
            .clone();
        let mut setting = section
            .settings
            .iter()
            .find(|setting| setting.key == key)
            .expect("section was selected by this setting")
            .clone();
        setting.label = label.to_owned();
        self.edit_setting(&section, &setting)
    }

    fn select_profiles(&mut self) -> Result<(), MenuError> {
        let Some(channel) = self.selected_channel.clone() else {
            self.ui.message(
                "Profile selection",
                "Select a configured named radio channel before choosing profiles.",
            )?;
            return Ok(());
        };
        let mut cursor = "1".to_owned();
        loop {
            let mut document = self.document()?;
            let configured = document.explicit_values(&channel);
            let items = PROFILE_SELECTORS
                .iter()
                .enumerate()
                .map(|(index, (key, label, _))| {
                    let current = configured
                        .get(*key)
                        .cloned()
                        .unwrap_or_else(|| format!("channel default ({channel})"));
                    ChoiceItem::new((index + 1).to_string(), format!("{label}: {current}"))
                })
                .collect();
            let Some(choice) = self.ui.navigation(
                format!("Profile selection for RadioPlus/{channel}"),
                &cursor,
                items,
            )?
            else {
                return Ok(());
            };
            cursor.clone_from(&choice);
            let Some(index) = menu_index(&choice, PROFILE_SELECTORS.len()) else {
                self.ui
                    .message("Invalid selection", "Select a listed profile type.")?;
                continue;
            };
            let (key, label, section) = PROFILE_SELECTORS[index];
            let names = profile_names(&document, section);
            let mut choices = vec![ChoiceItem::new(
                INHERIT_VALUE,
                format!("Use channel default ({channel})"),
            )];
            choices.extend(
                names
                    .iter()
                    .map(|name| ChoiceItem::new(name, format!("[{section} {name}]"))),
            );
            let current = configured
                .get(key)
                .filter(|name| names.iter().any(|candidate| candidate == *name))
                .map_or(INHERIT_VALUE, String::as_str);
            let Some(selected) =
                self.ui
                    .selection(format!("{label}\nCurrent: {current}"), current, choices)?
            else {
                continue;
            };
            let original = document.as_str().to_owned();
            if selected == INHERIT_VALUE {
                document.remove_value(&channel, key)?;
            } else {
                document.set_value(&channel, key, &selected)?;
            }
            if self.apply_edit(&original, document.as_str(), ApplyMode::Restart)? {
                self.restart_changed = true;
            }
        }
    }

    fn initialize_channel(&mut self) -> Result<(), MenuError> {
        let document = self.document()?;
        let configured = document.configured_channels();
        if let Some(channel) = self.selected_channel.as_ref() {
            if let Some(canonical) = find_channel(&configured, channel) {
                let canonical = canonical.to_owned();
                self.channels
                    .select_channel(&canonical)
                    .map_err(MenuError::Control)?;
                self.selected_channel = Some(canonical);
                return Ok(());
            }
            return Err(ConfigError::MissingChannel(channel.clone()).into());
        }
        let active = self.channels.active_channel().unwrap_or(None);
        self.selected_channel = active
            .and_then(|channel| find_channel(&configured, &channel).map(str::to_owned))
            .or_else(|| configured.first().cloned());
        Ok(())
    }

    fn choose_channel(&mut self) -> Result<(), MenuError> {
        let document = self.document()?;
        let channels = document.configured_channels();
        if channels.is_empty() {
            self.ui
                .message("Radio channel", "No named radio channels are configured.")?;
            return Ok(());
        }
        let current = self
            .selected_channel
            .as_deref()
            .and_then(|channel| find_channel(&channels, channel))
            .map_or_else(|| channels[0].clone(), str::to_owned);
        let items = channels
            .iter()
            .map(|channel| ChoiceItem::new(channel, format!("RadioPlus/{channel}")))
            .collect();
        let Some(selected) = self.ui.selection(
            format!("Radio channel\nCurrent: {current}"),
            &current,
            items,
        )?
        else {
            return Ok(());
        };
        if let Err(error) = self.channels.select_channel(&selected) {
            self.ui.message(
                "Radio channel",
                format!("Unable to select {selected}.\n\n{error}"),
            )?;
            return Ok(());
        }
        self.selected_channel = Some(selected);
        Ok(())
    }

    fn browse_section(&mut self, section: &SectionSpec) -> Result<(), MenuError> {
        let cursor_key = format!("{}:{}", section.section, section.title);
        let cursor = self
            .section_cursors
            .entry(cursor_key.clone())
            .or_insert_with(|| "1".to_owned())
            .clone();
        let mut cursor = cursor;
        loop {
            let document = self.document()?;
            let (_target, values) = self.section_values(&document, &section.section)?;
            let visible = visible_settings(section, &values);
            let items = visible
                .settings
                .iter()
                .enumerate()
                .map(|(index, setting)| {
                    let value = values
                        .get(&setting.key)
                        .map_or(setting.default_value.as_str(), String::as_str);
                    ChoiceItem::new(
                        (index + 1).to_string(),
                        format!("{}: {}", setting.label, display_value(&setting.kind, value)),
                    )
                })
                .collect();
            let choice = self.ui.navigation(&section.title, &cursor, items)?;
            let Some(choice) = choice else {
                self.section_cursors.insert(cursor_key, cursor);
                return Ok(());
            };
            cursor.clone_from(&choice);
            self.section_cursors
                .insert(cursor_key.clone(), cursor.clone());
            let Some(index) = menu_index(&choice, visible.settings.len()) else {
                self.ui
                    .message("Invalid selection", "Select a listed setting.")?;
                continue;
            };
            self.edit_setting(section, visible.settings[index])?;
        }
    }

    fn edit_setting(
        &mut self,
        section: &SectionSpec,
        setting: &SettingSpec,
    ) -> Result<(), MenuError> {
        let original = self.store.read_config().map_err(MenuError::Storage)?;
        let mut document = ConfigDocument::new(&original);
        let (target, values) = self.section_values(&document, &section.section)?;
        let current = values
            .get(&setting.key)
            .map_or(setting.default_value.as_str(), String::as_str);
        let Some(value) = self.prompt_setting(setting, current)? else {
            return Ok(());
        };
        if matches!(setting.kind, SettingKind::OptionalEnumeration(_)) && value == INHERIT_VALUE {
            document.remove_value(&target, &setting.key)?;
        } else {
            document.set_value(&target, &setting.key, &value)?;
        }
        if let Err(message) = self.validate_candidate(&document, &target, &setting.key) {
            self.ui.message("Invalid configuration", message)?;
            return Ok(());
        }
        if self.apply_edit(&original, document.as_str(), setting.apply_mode)? {
            self.restart_changed |= setting.apply_mode == ApplyMode::Restart;
        }
        Ok(())
    }

    fn prompt_setting(
        &mut self,
        setting: &SettingSpec,
        current: &str,
    ) -> Result<Option<String>, MenuError> {
        match &setting.kind {
            SettingKind::OnOff => self.prompt_on_off(setting, current),
            SettingKind::Enumeration(choices) => {
                self.prompt_enumeration(setting, current, choices, false)
            }
            SettingKind::OptionalEnumeration(choices) => {
                self.prompt_enumeration(setting, current, choices, true)
            }
            SettingKind::Integer(editor) => self.prompt_integer(setting, current, editor),
            SettingKind::Float(editor) => {
                self.prompt_float(setting, current, editor, "Decimal value")
            }
            SettingKind::Gain(editor) => self.prompt_float(setting, current, editor, "Gain"),
            SettingKind::Text(editor) => self.prompt_text(setting, current, editor),
        }
    }

    fn prompt_on_off(
        &mut self,
        setting: &SettingSpec,
        current: &str,
    ) -> Result<Option<String>, MenuError> {
        let current = if parse_on(current) { "yes" } else { "no" };
        let selected = self.ui.selection(
            format!(
                "{}\nCurrent: {}",
                setting.label,
                if current == "yes" { "On" } else { "Off" }
            ),
            current,
            vec![ChoiceItem::new("yes", "On"), ChoiceItem::new("no", "Off")],
        )?;
        if matches!(selected.as_deref(), Some("yes" | "no") | None) {
            Ok(selected)
        } else {
            self.ui.message("Invalid value", "Select On or Off.")?;
            Ok(None)
        }
    }

    fn prompt_enumeration(
        &mut self,
        setting: &SettingSpec,
        current: &str,
        choices: &[ChoiceItem],
        optional: bool,
    ) -> Result<Option<String>, MenuError> {
        let mut choices = choices.to_vec();
        if optional {
            choices.insert(
                0,
                ChoiceItem::new(INHERIT_VALUE, "Use inherited/default value"),
            );
        }
        let selected_current = if optional && !choices.iter().any(|choice| choice.value == current)
        {
            INHERIT_VALUE
        } else {
            current
        };
        let selected = self.ui.selection(
            format!("{}\nCurrent: {current}", setting.label),
            selected_current,
            choices.clone(),
        )?;
        if selected
            .as_ref()
            .is_some_and(|value| !choices.iter().any(|choice| choice.value == *value))
        {
            self.ui
                .message("Invalid value", "Select one of the listed values.")?;
            Ok(None)
        } else {
            Ok(selected)
        }
    }

    fn prompt_integer(
        &mut self,
        setting: &SettingSpec,
        current: &str,
        editor: &IntegerEditor,
    ) -> Result<Option<String>, MenuError> {
        let prompt = integer_prompt(&setting.label, current, editor);
        let Some(entered) = self.ui.input(prompt, current)? else {
            return Ok(None);
        };
        self.show_validation(validate_integer(&entered, editor).map(|value| value.to_string()))
    }

    fn prompt_float(
        &mut self,
        setting: &SettingSpec,
        current: &str,
        editor: &FloatEditor,
        type_name: &str,
    ) -> Result<Option<String>, MenuError> {
        let prompt = numeric_prompt(
            &setting.label,
            type_name,
            current,
            editor.minimum,
            editor.maximum,
            editor.exclusive_minimum,
            &editor.units,
        );
        let Some(entered) = self.ui.input(prompt, current)? else {
            return Ok(None);
        };
        self.show_validation(validate_float(&entered, editor).map(format_float))
    }

    fn prompt_text(
        &mut self,
        setting: &SettingSpec,
        current: &str,
        editor: &TextEditor,
    ) -> Result<Option<String>, MenuError> {
        let prompt = format!(
            "{}\n{}\nCurrent: {current}",
            setting.label, editor.instruction
        );
        let Some(entered) = self.ui.input(prompt, current)? else {
            return Ok(None);
        };
        self.show_validation(validate_text(&entered, editor.syntax))
    }

    fn show_validation(
        &mut self,
        validated: Result<String, String>,
    ) -> Result<Option<String>, MenuError> {
        match validated {
            Ok(value) => Ok(Some(value)),
            Err(message) => {
                self.ui.message("Invalid value", message)?;
                Ok(None)
            }
        }
    }

    fn validate_candidate(
        &self,
        document: &ConfigDocument,
        target: &str,
        key: &str,
    ) -> Result<(), String> {
        let Some(channel) = self.selected_channel.as_deref() else {
            return Ok(());
        };
        if let Some(role) = processing_role(target) {
            let resolved = ResolvedProfile::from_document(document, "configuration", channel, role)
                .map_err(|error| error.to_string())?;
            reject_setting_warning(resolved.warnings(), target, key)
        } else {
            let resolved = ResolvedStationConfig::from_document(document, "configuration", channel)
                .map_err(|error| error.to_string())?;
            reject_setting_warning(resolved.warnings(), target, key)
        }
    }

    fn apply_edit(
        &mut self,
        original: &str,
        updated: &str,
        mode: ApplyMode,
    ) -> Result<bool, MenuError> {
        self.store
            .write_config(updated)
            .map_err(MenuError::Storage)?;
        if let Err(message) = self.apply_written_generation(original, mode) {
            self.ui.message("Configuration apply failed", message)?;
            return Ok(false);
        }
        if mode == ApplyMode::Restart {
            self.ui.message(
                "Configuration saved",
                "The setting was saved and Asterisk was restarted.",
            )?;
        }
        Ok(true)
    }

    fn apply_generation(
        &mut self,
        original: &str,
        updated: &str,
        mode: ApplyMode,
    ) -> Result<(), String> {
        self.store.write_config(updated)?;
        self.apply_written_generation(original, mode)
    }

    fn apply_written_generation(&mut self, original: &str, mode: ApplyMode) -> Result<(), String> {
        if let Err(apply_error) = self.applier.apply(mode) {
            self.store.write_config(original)?;
            return Err(self.applier.apply(mode).err().map_or_else(
                || format!("Apply failed: {apply_error}\n\nThe previous configuration was restored."),
                |rollback_error| format!(
                    "Apply failed: {apply_error}\n\nThe previous file was restored, but reapplying it failed: {rollback_error}"
                ),
            ));
        }
        Ok(())
    }

    fn document(&self) -> Result<ConfigDocument, MenuError> {
        self.store
            .read_config()
            .map(ConfigDocument::new)
            .map_err(MenuError::Storage)
    }

    fn section_values(
        &self,
        document: &ConfigDocument,
        section: &str,
    ) -> Result<(String, BTreeMap<String, String>), MenuError> {
        let Some(channel) = self.selected_channel.as_deref() else {
            return Ok((section.to_owned(), document.explicit_values(section)));
        };
        let target = document.resolved_section(channel, section)?;
        let mut values = document.resolved_values(channel, section)?;
        if let Some(role) = processing_role(section) {
            if let Ok(resolved) =
                ResolvedProfile::from_document(document, "configuration", channel, role)
            {
                apply_warning_fallbacks(&mut values, resolved.warnings());
            }
        } else if let Ok(resolved) =
            ResolvedStationConfig::from_document(document, "configuration", channel)
        {
            apply_warning_fallbacks(&mut values, resolved.warnings());
        }
        Ok((target, values))
    }
}

fn swap_hardware_identity(
    document: &mut ConfigDocument,
    first_channel: &str,
    second_channel: &str,
) -> Result<(), String> {
    let first_target = document
        .resolved_section(first_channel, "hardware")
        .map_err(|error| error.to_string())?;
    let second_target = document
        .resolved_section(second_channel, "hardware")
        .map_err(|error| error.to_string())?;
    if first_target.eq_ignore_ascii_case(&second_target) {
        return Err(format!(
            "{first_channel} and {second_channel} use the same hardware profile [{first_target}]."
        ));
    }
    for (channel, target) in [
        (first_channel, &first_target),
        (second_channel, &second_target),
    ] {
        let shared = document.configured_channels().into_iter().any(|candidate| {
            !candidate.eq_ignore_ascii_case(channel)
                && document
                    .resolved_section(&candidate, "hardware")
                    .is_ok_and(|resolved| resolved.eq_ignore_ascii_case(target))
        });
        if shared {
            return Err(format!(
                "Cannot swap {channel}: hardware profile [{target}] is shared by another channel."
            ));
        }
    }
    let first = document
        .resolved_values(first_channel, "hardware")
        .map_err(|error| error.to_string())?;
    let second = document
        .resolved_values(second_channel, "hardware")
        .map_err(|error| error.to_string())?;
    for (key, default_value) in HARDWARE_IDENTITY_DEFAULTS {
        document
            .set_value(
                &first_target,
                key,
                second.get(key).map_or(default_value, String::as_str),
            )
            .map_err(|error| error.to_string())?;
        document
            .set_value(
                &second_target,
                key,
                first.get(key).map_or(default_value, String::as_str),
            )
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn collect_sections<'a>(page: &'a MenuPage, sections: &mut Vec<&'a SectionSpec>) {
    for entry in &page.entries {
        match entry {
            MenuEntry::Page(child) => collect_sections(child, sections),
            MenuEntry::Section(section) => sections.push(section),
            MenuEntry::Action(_) => {}
        }
    }
}

struct VisibleSettings<'a> {
    settings: Vec<&'a SettingSpec>,
}

fn visible_settings<'a>(
    section: &'a SectionSpec,
    values: &BTreeMap<String, String>,
) -> VisibleSettings<'a> {
    let stage = if section.title.contains("Compressor") {
        Some("compressor")
    } else if section.title.contains("Limiter") && !section.title.contains("Final") {
        Some("limiter")
    } else {
        None
    };
    let settings = section
        .settings
        .iter()
        .filter(|setting| {
            let Some(stage) = stage else {
                return true;
            };
            let mode_key = format!("{stage}_bands");
            let multiband = values
                .get(&mode_key)
                .or_else(|| {
                    section
                        .settings
                        .iter()
                        .find(|item| item.key == mode_key)
                        .map(|item| &item.default_value)
                })
                .is_some_and(|value| value == "3");
            let band_prefixes = [
                format!("{stage}_low_"),
                format!("{stage}_mid_"),
                format!("{stage}_high_"),
            ];
            let band_specific = band_prefixes
                .iter()
                .any(|prefix| setting.key.starts_with(prefix));
            let full_band = setting.key.starts_with(&format!("{stage}_"))
                && !band_specific
                && setting.key != mode_key
                && !setting.key.contains("crossover")
                && !setting.key.contains("sidechain");
            (!band_specific && !full_band)
                || (multiband && band_specific)
                || (!multiband && full_band)
        })
        .collect();
    VisibleSettings { settings }
}

fn processing_role(section: &str) -> Option<ChainRole> {
    let kind = section.split_once(' ').map_or(section, |(kind, _)| kind);
    match kind {
        "local" => Some(ChainRole::LocalReceive),
        "link" => Some(ChainRole::Link),
        "voice_telemetry" => Some(ChainRole::VoiceTelemetry),
        _ => None,
    }
}

fn reject_setting_warning(
    warnings: &[usbradioplus_core::ResolutionWarning],
    target: &str,
    key: &str,
) -> Result<(), String> {
    warnings
        .iter()
        .find(|warning| {
            warning.section.eq_ignore_ascii_case(target)
                && warning.name.eq_ignore_ascii_case(key)
                && warning.kind != ResolutionWarningKind::UnknownOption
        })
        .map_or(Ok(()), |warning| Err(warning.to_string()))
}

fn apply_warning_fallbacks(
    values: &mut BTreeMap<String, String>,
    warnings: &[usbradioplus_core::ResolutionWarning],
) {
    for warning in warnings {
        if warning.kind != ResolutionWarningKind::UnknownOption
            && values
                .get(&warning.name)
                .is_some_and(|value| value == &warning.supplied_value)
        {
            values.insert(warning.name.clone(), warning.fallback.clone());
        }
    }
}

fn menu_index(choice: &str, count: usize) -> Option<usize> {
    choice
        .parse::<usize>()
        .ok()
        .and_then(|value| value.checked_sub(1))
        .filter(|index| *index < count)
}

const fn calibration_title(kind: TransmitCalibration) -> &'static str {
    match kind {
        TransmitCalibration::Voice => "TX voice calibration",
        TransmitCalibration::Ctcss => "TX CTCSS calibration",
        TransmitCalibration::Auxiliary => "Auxiliary output calibration",
    }
}

fn routed_output_gain(
    outputs: (HardwareOutputAssignment, HardwareOutputAssignment),
    accepts: impl Fn(HardwareOutputAssignment) -> bool,
) -> Option<&'static str> {
    if accepts(outputs.0) {
        Some("hardware_output_a_gain_db")
    } else if accepts(outputs.1) {
        Some("hardware_output_b_gain_db")
    } else {
        None
    }
}

fn find_channel<'a>(channels: &'a [String], requested: &str) -> Option<&'a str> {
    channels
        .iter()
        .find(|channel| channel.eq_ignore_ascii_case(requested))
        .map(String::as_str)
}

fn profile_names(document: &ConfigDocument, kind: &str) -> Vec<String> {
    let mut names = document
        .section_names()
        .into_iter()
        .filter_map(|section| {
            let (section_kind, name) = section.split_once(' ')?;
            (section_kind.eq_ignore_ascii_case(kind) && !name.trim().is_empty())
                .then(|| name.trim().to_owned())
        })
        .collect::<Vec<_>>();
    names.sort_by_key(|name| name.to_ascii_lowercase());
    names.dedup_by(|left, right| left.eq_ignore_ascii_case(right));
    names
}

fn parse_on(value: &str) -> bool {
    matches!(
        value.trim().to_ascii_lowercase().as_str(),
        "yes" | "true" | "1" | "on"
    )
}

fn display_value(kind: &SettingKind, value: &str) -> String {
    match kind {
        SettingKind::OnOff => {
            if parse_on(value) {
                "On".to_owned()
            } else {
                "Off".to_owned()
            }
        }
        SettingKind::Integer(editor) => with_units(value, &editor.units),
        SettingKind::Float(editor) | SettingKind::Gain(editor) => with_units(value, &editor.units),
        SettingKind::Enumeration(_)
        | SettingKind::OptionalEnumeration(_)
        | SettingKind::Text(_) => value.to_owned(),
    }
}

fn validate_text(entered: &str, syntax: TextSyntax) -> Result<String, String> {
    let value = entered.trim();
    if value.is_empty() {
        if syntax == TextSyntax::Optional {
            return Ok(String::new());
        }
        return Err("The value cannot be empty.".to_owned());
    }
    match syntax {
        TextSyntax::Optional | TextSyntax::NonEmpty => {}
        TextSyntax::CtcssToneList => {
            for tone in value.split(',') {
                tone.trim()
                    .parse::<CtcssTone>()
                    .map_err(|error| error.to_string())?;
            }
        }
        TextSyntax::CtcssTone => {
            value
                .parse::<CtcssTone>()
                .map_err(|error| error.to_string())?;
        }
        TextSyntax::DcsCode => {
            return value
                .parse::<DcsCode>()
                .map(|code| code.to_string())
                .map_err(|error| error.to_string());
        }
        TextSyntax::StageOrder => {
            StageOrder::parse(value, &[]).map_err(|error| error.to_string())?;
        }
    }
    Ok(value.to_owned())
}

fn with_units(value: &str, units: &str) -> String {
    if units.is_empty() {
        value.to_owned()
    } else {
        format!("{value} {units}")
    }
}

fn numeric_prompt(
    label: &str,
    type_name: &str,
    current: &str,
    minimum: Option<f64>,
    maximum: Option<f64>,
    exclusive_minimum: bool,
    units: &str,
) -> String {
    let suffix = units_suffix(units);
    let limits = match (minimum, maximum) {
        (Some(minimum), Some(maximum)) if exclusive_minimum => {
            format!(
                "\nRange: > {} to {}{suffix}",
                format_float(minimum),
                format_float(maximum)
            )
        }
        (Some(minimum), Some(maximum)) => format!(
            "\nRange: {} to {}{suffix}",
            format_float(minimum),
            format_float(maximum)
        ),
        (Some(minimum), None) if exclusive_minimum => {
            format!("\nMust be greater than {}{suffix}", format_float(minimum))
        }
        (Some(minimum), None) => {
            format!("\nMinimum: {}{suffix}", format_float(minimum))
        }
        (None, Some(maximum)) => {
            format!("\nMaximum: {}{suffix}", format_float(maximum))
        }
        (None, None) => String::new(),
    };
    format!("{label}\n{type_name}; current: {current}{limits}")
}

fn integer_prompt(label: &str, current: &str, editor: &IntegerEditor) -> String {
    let suffix = units_suffix(&editor.units);
    let limits = match (editor.minimum, editor.maximum) {
        (Some(minimum), Some(maximum)) => format!("\nRange: {minimum} to {maximum}{suffix}"),
        (Some(minimum), None) => format!("\nMinimum: {minimum}{suffix}"),
        (None, Some(maximum)) => format!("\nMaximum: {maximum}{suffix}"),
        (None, None) => String::new(),
    };
    format!("{label}\nInteger; current: {current}{limits}")
}

fn validate_integer(entered: &str, editor: &IntegerEditor) -> Result<i64, String> {
    let value = parse_integer(entered).ok_or_else(|| "Enter an integer.".to_owned())?;
    if editor.minimum.is_some_and(|minimum| value < minimum)
        || editor.maximum.is_some_and(|maximum| value > maximum)
    {
        return Err(integer_range_error(editor));
    }
    Ok(value)
}

fn parse_integer(entered: &str) -> Option<i64> {
    let entered = entered.trim();
    let (negative, unsigned) = match entered.as_bytes().first() {
        Some(b'-') => (true, &entered[1..]),
        Some(b'+') => (false, &entered[1..]),
        _ => (false, entered),
    };
    let (radix, digits) = match unsigned.get(..2) {
        Some("0x" | "0X") => (16, &unsigned[2..]),
        Some("0o" | "0O") => (8, &unsigned[2..]),
        Some("0b" | "0B") => (2, &unsigned[2..]),
        _ => (10, unsigned),
    };
    if digits.is_empty() {
        return None;
    }
    let magnitude = i128::from_str_radix(digits, radix).ok()?;
    let signed = if negative { -magnitude } else { magnitude };
    i64::try_from(signed).ok()
}

fn integer_range_error(editor: &IntegerEditor) -> String {
    let suffix = units_suffix(&editor.units);
    match (editor.minimum, editor.maximum) {
        (Some(minimum), Some(maximum)) => {
            format!("Value must be between {minimum} and {maximum}{suffix}.")
        }
        (Some(minimum), None) => format!("Value must be at least {minimum}{suffix}."),
        (None, Some(maximum)) => format!("Value must be no more than {maximum}{suffix}."),
        (None, None) => "Enter a valid value.".to_owned(),
    }
}

fn units_suffix(units: &str) -> String {
    if units.is_empty() {
        String::new()
    } else {
        format!(" {units}")
    }
}

fn validate_float(entered: &str, editor: &FloatEditor) -> Result<f64, String> {
    let value = entered
        .trim()
        .parse::<f64>()
        .map_err(|_| "Enter a numeric value.".to_owned())?;
    if !value.is_finite() {
        return Err("Enter a finite numeric value.".to_owned());
    }
    let below = editor.minimum.is_some_and(|minimum| {
        if editor.exclusive_minimum {
            value <= minimum
        } else {
            value < minimum
        }
    });
    if below || editor.maximum.is_some_and(|maximum| value > maximum) {
        return Err(range_error(
            editor.minimum,
            editor.maximum,
            editor.exclusive_minimum,
            &editor.units,
        ));
    }
    Ok(value)
}

fn range_error(
    minimum: Option<f64>,
    maximum: Option<f64>,
    exclusive_minimum: bool,
    units: &str,
) -> String {
    let suffix = units_suffix(units);
    match (minimum, maximum) {
        (Some(minimum), Some(maximum)) if exclusive_minimum => format!(
            "Value must be greater than {} and no more than {}{suffix}.",
            format_float(minimum),
            format_float(maximum)
        ),
        (Some(minimum), Some(maximum)) => format!(
            "Value must be between {} and {}{suffix}.",
            format_float(minimum),
            format_float(maximum)
        ),
        (Some(minimum), None) if exclusive_minimum => {
            format!(
                "Value must be greater than {}{suffix}.",
                format_float(minimum)
            )
        }
        (Some(minimum), None) => {
            format!("Value must be at least {}{suffix}.", format_float(minimum))
        }
        (None, Some(maximum)) => {
            format!(
                "Value must be no more than {}{suffix}.",
                format_float(maximum)
            )
        }
        (None, None) => "Enter a valid value.".to_owned(),
    }
}

fn format_float(value: f64) -> String {
    value.to_string()
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;
    use crate::ui::{DialogRequest, DialogResponse};
    use std::cell::RefCell;
    use std::collections::VecDeque;
    use std::rc::Rc;

    impl TunerApp {
        fn selected_channel(&self) -> Option<&str> {
            self.selected_channel.as_deref()
        }

        fn browse(&mut self, sections: &[SectionSpec]) -> Result<(), MenuError> {
            let page = MenuPage {
                id: "root".to_owned(),
                title: "USBRadioPlus configuration".to_owned(),
                entries: sections.iter().cloned().map(MenuEntry::Section).collect(),
            };
            self.initialize_channel()?;
            self.browse_page(&page, true)
        }
    }

    #[derive(Clone, Default)]
    struct SharedStore(Rc<RefCell<StoreState>>);

    #[derive(Default)]
    struct StoreState {
        text: String,
        writes: Vec<String>,
        fail_read: bool,
        fail_write: bool,
        fail_backup: bool,
        backups: Vec<String>,
    }

    impl SharedStore {
        fn with_text(text: &str) -> Self {
            Self(Rc::new(RefCell::new(StoreState {
                text: text.to_owned(),
                ..StoreState::default()
            })))
        }
    }

    impl ConfigurationStore for SharedStore {
        fn read_config(&self) -> Result<String, String> {
            if self.0.borrow().fail_read {
                return Err("read failed".to_owned());
            }
            Ok(self.0.borrow().text.clone())
        }

        fn write_config(&self, contents: &str) -> Result<(), String> {
            let mut state = self.0.borrow_mut();
            if state.fail_write {
                return Err("write failed".to_owned());
            }
            state.text = contents.to_owned();
            state.writes.push(contents.to_owned());
            Ok(())
        }

        fn backup_config(&self) -> Result<String, String> {
            let mut state = self.0.borrow_mut();
            if state.fail_backup {
                return Err("backup failed".to_owned());
            }
            let path = format!("/tmp/config.bak.{}", state.backups.len());
            let text = state.text.clone();
            state.backups.push(text);
            Ok(path)
        }
    }

    #[derive(Clone, Default)]
    struct SharedApplier(Rc<RefCell<ApplyState>>);

    #[derive(Default)]
    struct ApplyState {
        results: VecDeque<Result<(), String>>,
        calls: Vec<ApplyMode>,
    }

    impl ConfigurationApplier for SharedApplier {
        fn apply(&mut self, mode: ApplyMode) -> Result<(), String> {
            let mut state = self.0.borrow_mut();
            state.calls.push(mode);
            state.results.pop_front().unwrap_or(Ok(()))
        }
    }

    #[derive(Clone, Default)]
    struct SharedChannels(Rc<RefCell<ChannelState>>);

    struct ChannelState {
        active: Option<String>,
        selections: Vec<String>,
        failure: Option<String>,
        radio_status: VecDeque<Result<RadioStatus, String>>,
        mixer_changes: Vec<(HardwareMixer, u32)>,
        test_tone_changes: Vec<bool>,
        test_tone_results: VecDeque<Result<(), String>>,
        transmit_changes: Vec<(bool, Option<CtcssTone>)>,
        transmit_results: VecDeque<Result<(), String>>,
        waits: Vec<Duration>,
        action_results: VecDeque<Result<String, String>>,
        actions: Vec<LiveAction>,
        echo: Result<bool, String>,
        echo_changes: Vec<bool>,
        echo_results: VecDeque<Result<String, String>>,
    }

    impl Default for ChannelState {
        fn default() -> Self {
            Self {
                active: None,
                selections: Vec::new(),
                failure: None,
                radio_status: VecDeque::new(),
                mixer_changes: Vec::new(),
                test_tone_changes: Vec::new(),
                test_tone_results: VecDeque::new(),
                transmit_changes: Vec::new(),
                transmit_results: VecDeque::new(),
                waits: Vec::new(),
                action_results: VecDeque::new(),
                actions: Vec::new(),
                echo: Ok(false),
                echo_changes: Vec::new(),
                echo_results: VecDeque::new(),
            }
        }
    }

    impl ChannelControl for SharedChannels {
        fn active_channel(&mut self) -> Result<Option<String>, String> {
            Ok(self.0.borrow().active.clone())
        }

        fn select_channel(&mut self, channel: &str) -> Result<(), String> {
            let mut state = self.0.borrow_mut();
            if let Some(error) = &state.failure {
                return Err(error.clone());
            }
            state.selections.push(channel.to_owned());
            state.active = Some(channel.to_owned());
            Ok(())
        }

        fn radio_status(&mut self) -> Result<RadioStatus, String> {
            self.0
                .borrow_mut()
                .radio_status
                .pop_front()
                .unwrap_or_else(|| Err("status unavailable".to_owned()))
        }

        fn set_hardware_mixer(&mut self, mixer: HardwareMixer, level: u32) -> Result<(), String> {
            self.0.borrow_mut().mixer_changes.push((mixer, level));
            Ok(())
        }

        fn set_test_tone(&mut self, enabled: bool) -> Result<(), String> {
            let mut state = self.0.borrow_mut();
            state.test_tone_changes.push(enabled);
            state.test_tone_results.pop_front().unwrap_or(Ok(()))
        }

        fn set_transmit(
            &mut self,
            keyed: bool,
            forced_ctcss: Option<CtcssTone>,
        ) -> Result<(), String> {
            let mut state = self.0.borrow_mut();
            state.transmit_changes.push((keyed, forced_ctcss));
            state.transmit_results.pop_front().unwrap_or(Ok(()))
        }

        fn save_current_tuning_to_eeprom(&mut self) -> Result<String, String> {
            Ok("EEPROM tuning saved.".to_owned())
        }

        fn wait(&mut self, duration: Duration) {
            self.0.borrow_mut().waits.push(duration);
        }

        fn perform(&mut self, action: LiveAction) -> Result<String, String> {
            let mut state = self.0.borrow_mut();
            state.actions.push(action);
            state
                .action_results
                .pop_front()
                .unwrap_or_else(|| Err("action unavailable".to_owned()))
        }

        fn echo_enabled(&mut self) -> Result<bool, String> {
            self.0.borrow().echo.clone()
        }

        fn set_echo_enabled(&mut self, enabled: bool) -> Result<String, String> {
            let mut state = self.0.borrow_mut();
            state.echo_changes.push(enabled);
            state
                .echo_results
                .pop_front()
                .unwrap_or_else(|| Ok("Echo mode updated.".to_owned()))
        }
    }

    #[derive(Clone, Default)]
    struct ScriptedBackend(Rc<RefCell<ScriptState>>);

    #[derive(Default)]
    struct ScriptState {
        responses: VecDeque<Result<DialogResponse, UiError>>,
        requests: Vec<DialogRequest>,
    }

    impl ScriptedBackend {
        fn new(responses: impl IntoIterator<Item = DialogResponse>) -> Self {
            Self::with_results(responses.into_iter().map(Ok))
        }

        fn with_results(
            responses: impl IntoIterator<Item = Result<DialogResponse, UiError>>,
        ) -> Self {
            Self(Rc::new(RefCell::new(ScriptState {
                responses: responses.into_iter().collect(),
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

    fn section() -> SectionSpec {
        SectionSpec {
            section: "hardware".to_owned(),
            title: "CM119 hardware".to_owned(),
            settings: vec![
                SettingSpec {
                    key: "enabled".to_owned(),
                    label: "Enabled".to_owned(),
                    default_value: "no".to_owned(),
                    kind: SettingKind::OnOff,
                    apply_mode: ApplyMode::Reload,
                },
                SettingSpec {
                    key: "assignment".to_owned(),
                    label: "Assignment".to_owned(),
                    default_value: "off".to_owned(),
                    kind: SettingKind::Enumeration(vec![
                        ChoiceItem::new("off", "Disabled"),
                        ChoiceItem::new("voice", "Voice"),
                    ]),
                    apply_mode: ApplyMode::Restart,
                },
                SettingSpec {
                    key: "depth".to_owned(),
                    label: "Queue depth".to_owned(),
                    default_value: "2".to_owned(),
                    kind: SettingKind::Integer(IntegerEditor {
                        minimum: Some(1),
                        maximum: Some(32),
                        units: "frames".to_owned(),
                    }),
                    apply_mode: ApplyMode::Restart,
                },
                SettingSpec {
                    key: "ratio".to_owned(),
                    label: "Ratio".to_owned(),
                    default_value: "1".to_owned(),
                    kind: SettingKind::Float(FloatEditor {
                        minimum: Some(0.0),
                        maximum: Some(10.0),
                        exclusive_minimum: true,
                        units: String::new(),
                    }),
                    apply_mode: ApplyMode::Reload,
                },
                SettingSpec {
                    key: "gain".to_owned(),
                    label: "Input gain".to_owned(),
                    default_value: "0".to_owned(),
                    kind: SettingKind::Gain(FloatEditor {
                        minimum: Some(-30.0),
                        maximum: Some(30.0),
                        exclusive_minimum: false,
                        units: "dB".to_owned(),
                    }),
                    apply_mode: ApplyMode::Reload,
                },
            ],
        }
    }

    const fn config() -> &'static str {
        "[one]\n[hardware]\nenabled = no\n[hardware one]\nassignment = off\n\
         [two]\n[hardware two]\nassignment = voice\n"
    }

    #[test]
    fn every_typed_editor_applies_and_section_focus_tracks_the_last_choice() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Accepted("2".to_owned()),
            DialogResponse::Accepted("voice".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("3".to_owned()),
            DialogResponse::Accepted("0x10".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("4".to_owned()),
            DialogResponse::Accepted("1.25".to_owned()),
            DialogResponse::Accepted("5".to_owned()),
            DialogResponse::Accepted("-6".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let channels = SharedChannels::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            applier.clone(),
            channels,
            Some("one".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        let text = &store.0.borrow().text;
        assert!(text.contains("enabled = yes"));
        assert!(text.contains("assignment = voice"));
        assert!(text.contains("depth = 16"));
        assert!(text.contains("ratio = 1.25"));
        assert!(text.contains("gain = -6"));
        assert_eq!(
            applier.0.borrow().calls,
            [
                ApplyMode::Reload,
                ApplyMode::Restart,
                ApplyMode::Restart,
                ApplyMode::Reload,
                ApplyMode::Reload
            ]
        );
        let requests = &backend.0.borrow().requests;
        let section_defaults = requests
            .iter()
            .filter_map(|request| match request {
                DialogRequest::Menu {
                    prompt,
                    default_item,
                    ..
                } if prompt == "CM119 hardware" => Some(default_item.as_str()),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(section_defaults, ["1", "1", "2", "3", "4", "5"]);
    }

    #[test]
    fn invalid_numeric_input_never_writes_or_applies() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("3".to_owned()),
            DialogResponse::Accepted("99".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("4".to_owned()),
            DialogResponse::Accepted("NaN".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        assert!(store.0.borrow().writes.is_empty());
        assert!(applier.0.borrow().calls.is_empty());
    }

    #[test]
    fn categorical_editors_reject_values_not_returned_by_their_lists() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("maybe".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("2".to_owned()),
            DialogResponse::Accepted("unlisted".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        assert!(store.0.borrow().writes.is_empty());
        assert!(applier.0.borrow().calls.is_empty());
    }

    #[test]
    fn apply_failure_restores_the_file_and_reapplies_the_old_generation() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        applier
            .0
            .borrow_mut()
            .results
            .push_back(Err("new generation rejected".to_owned()));
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(store.0.borrow().text, config());
        assert_eq!(store.0.borrow().writes.len(), 2);
        assert_eq!(
            applier.0.borrow().calls,
            [ApplyMode::Reload, ApplyMode::Reload]
        );
        assert!(backend.0.borrow().requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { title, body }
                if title == "Configuration apply failed" && body.contains("restored")
        )));
    }

    #[test]
    fn rollback_reapply_failure_is_visible_but_the_old_file_remains() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        applier.0.borrow_mut().results = VecDeque::from([
            Err("new generation rejected".to_owned()),
            Err("old generation rejected".to_owned()),
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            applier,
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(store.0.borrow().text, config());
        assert!(backend.0.borrow().requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { body, .. }
                if body.contains("reapplying it failed: old generation rejected")
        )));
    }

    #[test]
    fn named_channel_selection_uses_active_fallback_and_preserves_top_focus() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("C".to_owned()),
            DialogResponse::Accepted("two".to_owned()),
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let channels = SharedChannels::default();
        channels.0.borrow_mut().active = Some("one".to_owned());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store,
            SharedApplier::default(),
            channels.clone(),
            None,
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(app.selected_channel(), Some("two"));
        assert_eq!(channels.0.borrow().selections, ["two"]);
        let requests = &backend.0.borrow().requests;
        assert!(matches!(
            &requests[1],
            DialogRequest::Selection { default_item, .. } if default_item == "one"
        ));
        assert!(matches!(
            &requests[2],
            DialogRequest::Menu {
                default_item,
                cancel_label,
                ..
            } if default_item == "C" && cancel_label == "Exit"
        ));
    }

    #[test]
    fn explicit_channel_is_canonicalized_or_rejected_before_browsing() {
        let backend = ScriptedBackend::new([DialogResponse::Cancelled]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("ONE".to_owned()),
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(app.selected_channel(), Some("one"));

        let backend = ScriptedBackend::new(std::iter::empty());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("missing".to_owned()),
        );
        assert!(matches!(
            app.browse(&[section()]),
            Err(MenuError::Configuration(ConfigError::MissingChannel(channel)))
                if channel == "missing"
        ));
    }

    #[test]
    fn failed_or_impossible_channel_selection_does_not_change_the_target() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("C".to_owned()),
            DialogResponse::Accepted("two".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(config());
        let channels = SharedChannels::default();
        channels.0.borrow_mut().active = Some("one".to_owned());
        channels.0.borrow_mut().failure = Some("module rejected channel".to_owned());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store,
            SharedApplier::default(),
            channels,
            None,
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(app.selected_channel(), Some("one"));
        assert!(backend.0.borrow().requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { body, .. } if body.contains("module rejected channel")
        )));

        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("C".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text("[hardware]\nenabled = no\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        app.browse(&[section()]).unwrap();
        assert_eq!(app.selected_channel(), None);
        assert!(backend.0.borrow().requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { body, .. } if body.contains("No named radio channels")
        )));
    }

    #[test]
    fn store_and_structural_failures_are_returned_to_the_session_owner() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
        ]);
        let store = SharedStore::with_text(config());
        store.0.borrow_mut().fail_write = true;
        let error = MenuError::Storage("failed".to_owned());
        assert!(error.to_string().contains("storage"));
        assert!(std::error::Error::source(&error).is_none());
        let config_error = MenuError::Configuration(ConfigError::InvalidValue);
        assert!(std::error::Error::source(&config_error).is_some());
        let ui_error = MenuError::Ui(UiError::message("failed"));
        assert!(std::error::Error::source(&ui_error).is_some());

        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store,
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        assert!(
            matches!(app.browse(&[section()]), Err(MenuError::Storage(error)) if error == "write failed")
        );
    }

    #[test]
    fn numeric_helpers_cover_radices_bounds_and_prompt_forms() {
        assert_eq!(parse_integer("-0x10"), Some(-16));
        assert_eq!(parse_integer("+0o10"), Some(8));
        assert_eq!(parse_integer("0b10"), Some(2));
        assert_eq!(parse_integer(""), None);
        assert_eq!(parse_integer("0x"), None);
        assert_eq!(parse_integer("999999999999999999999999"), None);
        assert!(
            numeric_prompt("X", "Integer", "1", Some(0.0), None, false, "ms").contains("Minimum")
        );
        assert!(numeric_prompt("X", "Float", "1", Some(0.0), None, true, "").contains("greater"));
        assert!(numeric_prompt("X", "Float", "1", None, Some(2.0), false, "").contains("Maximum"));
        assert!(numeric_prompt("X", "Float", "1", None, None, false, "").ends_with("current: 1"));
        assert_eq!(menu_index("0", 2), None);
        assert_eq!(menu_index("3", 2), None);
        assert_eq!(menu_index("bad", 2), None);
        assert_eq!(with_units("1", ""), "1");
    }

    #[test]
    fn helper_functions_cover_every_supported_shape() {
        assert_eq!(parse_integer("10"), Some(10));
        assert_eq!(parse_integer("0X10"), Some(16));
        assert_eq!(parse_integer("0O10"), Some(8));
        assert_eq!(parse_integer("0B10"), Some(2));
        assert_eq!(parse_integer("-9223372036854775808"), Some(i64::MIN));
        assert_eq!(parse_integer("9223372036854775808"), None);

        for (minimum, maximum, expected) in [
            (Some(1), Some(2), "between"),
            (Some(1), None, "at least"),
            (None, Some(2), "no more"),
            (None, None, "valid"),
        ] {
            let editor = IntegerEditor {
                minimum,
                maximum,
                units: "frames".to_owned(),
            };
            assert!(integer_prompt("Depth", "1", &editor).contains("Integer"));
            assert!(integer_range_error(&editor).contains(expected));
        }

        for (minimum, maximum, exclusive, expected) in [
            (Some(1.0), Some(2.0), true, "greater"),
            (Some(1.0), Some(2.0), false, "between"),
            (Some(1.0), None, true, "greater"),
            (Some(1.0), None, false, "at least"),
            (None, Some(2.0), false, "no more"),
            (None, None, false, "valid"),
        ] {
            assert!(range_error(minimum, maximum, exclusive, "dB").contains(expected));
        }
    }

    #[test]
    fn text_profile_and_warning_helpers_cover_every_shape() {
        assert_eq!(
            validate_text(" name ", TextSyntax::NonEmpty),
            Ok("name".to_owned())
        );
        assert_eq!(validate_text("  ", TextSyntax::Optional), Ok(String::new()));
        assert!(validate_text("", TextSyntax::NonEmpty).is_err());
        assert!(validate_text("100.0", TextSyntax::CtcssTone).is_ok());
        assert!(validate_text("101.0", TextSyntax::CtcssTone).is_err());
        assert_eq!(
            processing_role("local profile"),
            Some(ChainRole::LocalReceive)
        );
        assert_eq!(processing_role("link"), Some(ChainRole::Link));
        assert_eq!(
            processing_role("voice_telemetry"),
            Some(ChainRole::VoiceTelemetry)
        );
        assert_eq!(processing_role("hardware"), None);
        assert_eq!(find_channel(&["Radio".to_owned()], "radio"), Some("Radio"));
        assert_eq!(find_channel(&["Radio".to_owned()], "other"), None);

        let document = ConfigDocument::new(
            "[local Zulu]\n[LOCAL alpha]\n[local zulu]\n[local ]\n[hardware site]\n",
        );
        assert_eq!(profile_names(&document, "local"), ["alpha", "Zulu"]);

        let warning = usbradioplus_core::ResolutionWarning {
            kind: ResolutionWarningKind::InvalidValue,
            source: "test".to_owned(),
            section: "hardware one".to_owned(),
            name: "gain".to_owned(),
            supplied_value: "bad".to_owned(),
            fallback: "0".to_owned(),
            reason: "invalid".to_owned(),
        };
        assert!(
            reject_setting_warning(std::slice::from_ref(&warning), "HARDWARE ONE", "GAIN").is_err()
        );
        assert!(reject_setting_warning(std::slice::from_ref(&warning), "hardware", "gain").is_ok());
        let unknown = usbradioplus_core::ResolutionWarning {
            kind: ResolutionWarningKind::UnknownOption,
            ..warning.clone()
        };
        assert!(reject_setting_warning(&[unknown.clone()], "hardware one", "gain").is_ok());
        let mut values = BTreeMap::from([("gain".to_owned(), "bad".to_owned())]);
        apply_warning_fallbacks(&mut values, std::slice::from_ref(&warning));
        assert_eq!(values["gain"], "0");
        values.insert("gain".to_owned(), "different".to_owned());
        apply_warning_fallbacks(&mut values, &[warning, unknown]);
        assert_eq!(values["gain"], "different");
    }

    #[test]
    fn display_helpers_cover_every_setting_kind() {
        for (kind, expected) in [
            (SettingKind::OnOff, "On".to_owned()),
            (
                SettingKind::Integer(IntegerEditor {
                    minimum: None,
                    maximum: None,
                    units: "ms".to_owned(),
                }),
                "1 ms".to_owned(),
            ),
            (
                SettingKind::Float(FloatEditor {
                    minimum: None,
                    maximum: None,
                    exclusive_minimum: false,
                    units: "Hz".to_owned(),
                }),
                "1 Hz".to_owned(),
            ),
            (SettingKind::Enumeration(Vec::new()), "1".to_owned()),
            (SettingKind::OptionalEnumeration(Vec::new()), "1".to_owned()),
            (
                SettingKind::Text(TextEditor {
                    instruction: String::new(),
                    syntax: TextSyntax::NonEmpty,
                }),
                "1".to_owned(),
            ),
        ] {
            assert_eq!(display_value(&kind, "1"), expected);
        }
        assert_eq!(display_value(&SettingKind::OnOff, "no"), "Off");
    }

    #[test]
    fn compressor_and_limiter_visibility_follows_band_layout() {
        let setting = |key: &str| SettingSpec {
            key: key.to_owned(),
            label: key.to_owned(),
            default_value: "0".to_owned(),
            kind: SettingKind::OnOff,
            apply_mode: ApplyMode::Reload,
        };
        for title in ["Compressor", "Limiter"] {
            let prefix = title.to_ascii_lowercase();
            let section = SectionSpec {
                section: "local".to_owned(),
                title: title.to_owned(),
                settings: vec![
                    setting(&format!("{prefix}_bands")),
                    setting(&format!("{prefix}_threshold_dbfs")),
                    setting(&format!("{prefix}_low_threshold_dbfs")),
                    setting(&format!("{prefix}_crossover_low_hz")),
                    setting(&format!("{prefix}_sidechain_highpass_hz")),
                    setting("unrelated"),
                ],
            };
            let full = visible_settings(&section, &BTreeMap::new());
            assert!(
                full.settings
                    .iter()
                    .any(|item| item.key == format!("{prefix}_threshold_dbfs"))
            );
            assert!(
                !full
                    .settings
                    .iter()
                    .any(|item| item.key == format!("{prefix}_low_threshold_dbfs"))
            );
            let multiband = visible_settings(
                &section,
                &BTreeMap::from([(format!("{prefix}_bands"), "3".to_owned())]),
            );
            assert!(
                multiband
                    .settings
                    .iter()
                    .any(|item| item.key == format!("{prefix}_low_threshold_dbfs"))
            );
            assert!(
                !multiband
                    .settings
                    .iter()
                    .any(|item| item.key == format!("{prefix}_threshold_dbfs"))
            );
        }

        let ordinary = section();
        assert_eq!(
            visible_settings(&ordinary, &BTreeMap::new()).settings.len(),
            ordinary.settings.len()
        );
    }

    #[test]
    fn nested_navigation_retains_page_and_section_focus() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("2".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let root = MenuPage {
            id: "root".to_owned(),
            title: "Root".to_owned(),
            entries: vec![MenuEntry::Page(MenuPage {
                id: "child".to_owned(),
                title: "Child".to_owned(),
                entries: vec![
                    MenuEntry::Section(section()),
                    MenuEntry::Section(SectionSpec {
                        section: "receive".to_owned(),
                        title: "Receiver".to_owned(),
                        settings: vec![],
                    }),
                ],
            })],
        };
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse_tree(&root).unwrap();
        let child_defaults = backend
            .0
            .borrow()
            .requests
            .iter()
            .filter_map(|request| match request {
                DialogRequest::Menu {
                    prompt,
                    default_item,
                    ..
                } if prompt == "Child" => Some(default_item.clone()),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(child_defaults, ["1", "2", "2"]);
    }

    #[test]
    fn core_resolvers_reject_cross_field_edits_before_storage() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("100".to_owned()),
            DialogResponse::Accepted(String::new()),
        ]);
        let store = SharedStore::with_text(
            "[one]\n[local]\nreceive_bandpass_highpass_hz = 200\n\
             receive_bandpass_lowpass_hz = 5000\n",
        );
        let applier = SharedApplier::default();
        let setting = SettingSpec {
            key: "receive_bandpass_lowpass_hz".to_owned(),
            label: "High cutoff".to_owned(),
            default_value: "5000".to_owned(),
            kind: SettingKind::Float(FloatEditor {
                minimum: Some(20.0),
                maximum: Some(6_000.0),
                exclusive_minimum: false,
                units: "Hz".to_owned(),
            }),
            apply_mode: ApplyMode::Reload,
        };
        let section = SectionSpec {
            section: "local".to_owned(),
            title: "Filters".to_owned(),
            settings: vec![setting.clone()],
        };
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.edit_setting(&section, &setting).unwrap();
        assert!(store.0.borrow().writes.is_empty());
        assert!(applier.0.borrow().calls.is_empty());
        assert!(backend.0.borrow().requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { title, body }
                if title == "Invalid configuration" && body.contains("receive_bandpass")
        )));
    }

    #[test]
    fn resolver_fallbacks_are_displayed_and_optional_assignments_can_be_removed() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted(INHERIT_VALUE.to_owned()),
            DialogResponse::Accepted(String::new()),
        ]);
        let store = SharedStore::with_text(
            "[one]\nhardware_profile = site\n[hardware]\nhardware_input_gain_db = invalid\n\
             [hardware site]\nhardware_parallel_pin_2_assignment = out1\n",
        );
        let setting = SettingSpec {
            key: "hardware_parallel_pin_2_assignment".to_owned(),
            label: "Parallel pin 2".to_owned(),
            default_value: "unconfigured".to_owned(),
            kind: SettingKind::OptionalEnumeration(vec![
                ChoiceItem::new("out0", "Low"),
                ChoiceItem::new("out1", "High"),
            ]),
            apply_mode: ApplyMode::Restart,
        };
        let section = SectionSpec {
            section: "hardware".to_owned(),
            title: "Parallel".to_owned(),
            settings: vec![setting.clone()],
        };
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        let document = app.document().unwrap();
        let (_, values) = app.section_values(&document, "hardware").unwrap();
        assert_eq!(
            values.get("hardware_input_gain_db").map(String::as_str),
            Some("0")
        );
        app.edit_setting(&section, &setting).unwrap();
        assert!(
            !store
                .0
                .borrow()
                .text
                .contains("hardware_parallel_pin_2_assignment")
        );
    }

    #[test]
    fn typed_text_editors_use_core_syntax_and_canonicalize_dcs() {
        assert_eq!(
            validate_text(" 023i ", TextSyntax::DcsCode),
            Ok("023I".to_owned())
        );
        assert!(validate_text("089N", TextSyntax::DcsCode).is_err());
        assert!(validate_text("100.0, 103.5", TextSyntax::CtcssToneList).is_ok());
        assert!(validate_text("100.0, 101.0", TextSyntax::CtcssToneList).is_err());
        assert!(validate_text("equalizer,agc", TextSyntax::StageOrder).is_ok());
        assert!(validate_text("equalizer,equalizer", TextSyntax::StageOrder).is_err());
    }

    #[test]
    fn rx_noise_calibration_sets_hardware_gain_and_dsp_squelch() {
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let channels = SharedChannels::default();
        let initial = RadioStatus {
            receive_input_peak: RX_NOISE_TARGET * 2.0 / 500.0,
            receive_input_rms: 0.001,
            receive_rssi_peak: 20_000,
            receive_rssi_updated: true,
            ..RadioStatus::default()
        };
        let settled = RadioStatus {
            receive_input_peak: RX_NOISE_TARGET,
            receive_input_rms: 0.25,
            receive_rssi_peak: 20_000,
            receive_rssi_updated: true,
            receive_mixer_level: 500,
            ..RadioStatus::default()
        };
        let mut observations = VecDeque::from([Ok(initial)]);
        observations.extend((0..7).map(|_| Ok(settled)));
        channels.0.borrow_mut().radio_status = observations;
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            store.clone(),
            applier.clone(),
            channels.clone(),
            Some("one".to_owned()),
        );

        let report = app.calibrate_receive_noise().unwrap();

        assert!(report.contains("Hardware input: 500/999"));
        assert!(report.contains("Peak: 27000 codes"));
        assert!(store.0.borrow().text.contains("hardware_input_gain_db = 0"));
        assert!(store.0.borrow().text.contains("squelch_level = 539"));
        assert_eq!(applier.0.borrow().calls, [ApplyMode::Reload]);
        let state = channels.0.borrow();
        assert_eq!(
            state.mixer_changes.first(),
            Some(&(HardwareMixer::Receive, 2))
        );
        assert_eq!(
            state.mixer_changes.last(),
            Some(&(HardwareMixer::Receive, 500))
        );
        assert_eq!(state.waits.len(), 15);
    }

    #[test]
    fn rx_noise_calibration_covers_adjustment_and_failure_policies() {
        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = VecDeque::from([
            Ok(RadioStatus {
                receive_input_peak: RX_NOISE_TARGET / 2.0,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: 0.95,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: RX_NOISE_TARGET / 2.0,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: RX_NOISE_TARGET / 2.0,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: 0.95,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: RX_NOISE_TARGET,
                ..RadioStatus::default()
            }),
            Ok(RadioStatus {
                receive_input_peak: RX_NOISE_TARGET,
                ..RadioStatus::default()
            }),
        ]);
        let store =
            SharedStore::with_text(&format!("{}[receive]\ncos_assignment = no\n", config()));
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            store.clone(),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        let report = app.calibrate_receive_noise().unwrap();
        assert!(!report.contains("DSP squelch"));
        assert!(!store.0.borrow().text.contains("squelch_level"));

        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = (0..48)
            .map(|_| {
                Ok(RadioStatus {
                    receive_input_peak: 1.0,
                    receive_input_rail_samples: 1,
                    ..RadioStatus::default()
                })
            })
            .collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_noise()
                .unwrap_err()
                .contains("failed after 48 attempts")
        );

        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = (0..48)
            .map(|_| {
                Ok(RadioStatus {
                    receive_input_peak: 1.01,
                    ..RadioStatus::default()
                })
            })
            .collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_noise()
                .unwrap_err()
                .contains("no unclipped mixer setting")
        );

        let channels = SharedChannels::default();
        let settled = RadioStatus {
            receive_input_peak: RX_NOISE_TARGET,
            receive_rssi_updated: false,
            ..RadioStatus::default()
        };
        channels.0.borrow_mut().radio_status = (0..27).map(|_| Ok(settled)).collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_noise()
                .unwrap_err()
                .contains("DSP noise measurement did not complete")
        );
    }

    #[test]
    fn rx_noise_calibration_warns_for_insufficient_discriminator_noise() {
        let channels = SharedChannels::default();
        let settled = RadioStatus {
            receive_input_peak: RX_NOISE_TARGET,
            receive_rssi_peak: 1,
            receive_rssi_updated: true,
            ..RadioStatus::default()
        };
        channels.0.borrow_mut().radio_status = (0..8).map(|_| Ok(settled)).collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_noise()
                .unwrap()
                .contains("insufficient high-frequency discriminator noise")
        );
    }

    #[test]
    fn complete_active_configuration_resolves_defaults_profiles_and_live_state() {
        let channels = SharedChannels::default();
        channels.0.borrow_mut().action_results =
            VecDeque::from([Ok("running=1\nreceiver_keyed=0\n".to_owned())]);
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels.clone(),
            Some("one".to_owned()),
        );

        let output = app.complete_active_configuration().unwrap();

        assert!(output.starts_with("Active channel: one\n"));
        assert!(output.contains("[hardware one]\n"));
        assert!(output.contains("hardware_input_gain_db = 0\n"));
        assert!(output.contains("Live state\nrunning=1\n"));
        assert_eq!(channels.0.borrow().actions, [LiveAction::RadioStatus]);
    }

    #[test]
    fn rx_voice_and_ctcss_calibration_store_the_established_gains() {
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = (0..6)
            .map(|_| {
                Ok(RadioStatus {
                    receive_output_peak: RX_VOICE_TARGET,
                    ..RadioStatus::default()
                })
            })
            .collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            store.clone(),
            applier.clone(),
            channels.clone(),
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_voice()
                .unwrap()
                .contains("7200 codes")
        );
        assert!(store.0.borrow().text.contains("input_gain_db = 6.0206"));

        channels.0.borrow_mut().radio_status = (0..6)
            .map(|_| {
                Ok(RadioStatus {
                    receive_ctcss_decoder_peak: RX_CTCSS_TARGET,
                    ..RadioStatus::default()
                })
            })
            .collect();
        assert!(
            app.calibrate_receive_ctcss()
                .unwrap()
                .contains("2400 codes")
        );
        assert!(
            store
                .0
                .borrow()
                .text
                .contains("receive_decoder_gain_db = 0")
        );
        assert_eq!(applier.0.borrow().calls.len(), 12);
    }

    #[test]
    fn transmit_calibrations_key_the_required_signal_and_edit_typed_levels() {
        let voice_store =
            SharedStore::with_text("[one]\n[hardware]\nhardware_output_a_assignment = voice\n");
        let voice_channels = SharedChannels::default();
        let mut voice = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                "-3".to_owned(),
            )])),
            voice_store.clone(),
            SharedApplier::default(),
            voice_channels.clone(),
            Some("one".to_owned()),
        );
        voice
            .run_menu_action(&ActionSpec {
                label: "Voice".to_owned(),
                result_title: "Voice".to_owned(),
                action: MenuAction::AdjustTransmitVoice,
            })
            .unwrap();
        assert!(
            voice_store
                .0
                .borrow()
                .text
                .contains("hardware_output_a_gain_db = -3")
        );
        assert_eq!(voice_channels.0.borrow().test_tone_changes, [true, false]);
        assert_eq!(
            voice_channels.0.borrow().transmit_changes,
            [(true, None), (false, None)]
        );

        let auxiliary_store = SharedStore::with_text(
            "[one]\n[hardware]\nhardware_output_a_assignment = off\n\
             hardware_output_b_assignment = auxvoice\n",
        );
        let auxiliary_channels = SharedChannels::default();
        let mut auxiliary = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                "-6".to_owned(),
            )])),
            auxiliary_store.clone(),
            SharedApplier::default(),
            auxiliary_channels.clone(),
            Some("one".to_owned()),
        );
        auxiliary
            .run_menu_action(&ActionSpec {
                label: "Auxiliary".to_owned(),
                result_title: "Auxiliary".to_owned(),
                action: MenuAction::AdjustAuxiliaryOutput,
            })
            .unwrap();
        assert!(
            auxiliary_store
                .0
                .borrow()
                .text
                .contains("hardware_output_b_gain_db = -6")
        );
        assert_eq!(
            auxiliary_channels.0.borrow().test_tone_changes,
            [true, false]
        );

        let ctcss_store = SharedStore::with_text(
            "[one]\n[hardware]\nhardware_output_a_assignment = ctcss\n\
             [transmit]\nsignaling_method = ctcss\n\
             [ctcss]\ntransmit_default_hz = 100.0\n",
        );
        let ctcss_channels = SharedChannels::default();
        let mut ctcss = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                "-30".to_owned(),
            )])),
            ctcss_store.clone(),
            SharedApplier::default(),
            ctcss_channels.clone(),
            Some("one".to_owned()),
        );
        ctcss
            .run_menu_action(&ActionSpec {
                label: "CTCSS".to_owned(),
                result_title: "CTCSS".to_owned(),
                action: MenuAction::AdjustTransmitCtcss,
            })
            .unwrap();
        assert!(
            ctcss_store
                .0
                .borrow()
                .text
                .contains("transmit_peak_dbfs = -30")
        );
        let tone = CtcssTone::from_tenths_hz(1_000).unwrap();
        assert!(ctcss_channels.0.borrow().test_tone_changes.is_empty());
        assert_eq!(
            ctcss_channels.0.borrow().transmit_changes,
            [(true, Some(tone)), (false, None)]
        );
    }

    #[test]
    fn transmit_calibration_explains_unavailable_configuration() {
        let backend = ScriptedBackend::new(std::iter::repeat_n(
            DialogResponse::Accepted(String::new()),
            3,
        ));
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text(
                "[one]\n[hardware]\nhardware_output_a_assignment = off\n\
                 hardware_output_b_assignment = off\n",
            ),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        for kind in [
            TransmitCalibration::Voice,
            TransmitCalibration::Ctcss,
            TransmitCalibration::Auxiliary,
        ] {
            app.adjust_transmit(kind).unwrap();
        }
        let requests = &backend.0.borrow().requests;
        assert_eq!(requests.len(), 3);
        assert!(matches!(
            &requests[0],
            DialogRequest::Message { title, .. }
                if title.contains("TX voice calibration unavailable")
        ));
        assert!(matches!(
            &requests[1],
            DialogRequest::Message { title, .. }
                if title.contains("TX CTCSS calibration unavailable")
        ));
        assert!(matches!(
            &requests[2],
            DialogRequest::Message { title, .. }
                if title.contains("Auxiliary output calibration unavailable")
        ));

        let unreadable = SharedStore::with_text("[one]\n");
        unreadable.0.borrow_mut().fail_read = true;
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                String::new(),
            )])),
            unreadable,
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.adjust_transmit(TransmitCalibration::Voice).unwrap();

        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                String::new(),
            )])),
            SharedStore::with_text("[one]\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        app.adjust_transmit(TransmitCalibration::Voice).unwrap();
    }

    #[test]
    fn transmit_calibration_reports_runtime_failures_and_always_cleans_up() {
        let tone_failure = SharedChannels::default();
        tone_failure
            .0
            .borrow_mut()
            .test_tone_results
            .push_back(Err("tone failed".to_owned()));
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                String::new(),
            )])),
            SharedStore::with_text("[one]\n"),
            SharedApplier::default(),
            tone_failure.clone(),
            Some("one".to_owned()),
        );
        app.adjust_transmit(TransmitCalibration::Voice).unwrap();
        assert!(tone_failure.0.borrow().transmit_changes.is_empty());

        let key_failure = SharedChannels::default();
        key_failure
            .0
            .borrow_mut()
            .transmit_results
            .push_back(Err("key failed".to_owned()));
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                String::new(),
            )])),
            SharedStore::with_text("[one]\n"),
            SharedApplier::default(),
            key_failure.clone(),
            Some("one".to_owned()),
        );
        app.adjust_transmit(TransmitCalibration::Voice).unwrap();
        assert_eq!(key_failure.0.borrow().test_tone_changes, [true, false]);

        let cleanup_failure = SharedChannels::default();
        cleanup_failure.0.borrow_mut().test_tone_results =
            VecDeque::from([Ok(()), Err("stop tone failed".to_owned())]);
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([
                DialogResponse::Accepted("-3".to_owned()),
                DialogResponse::Accepted(String::new()),
            ])),
            SharedStore::with_text("[one]\n"),
            SharedApplier::default(),
            cleanup_failure.clone(),
            Some("one".to_owned()),
        );
        app.adjust_transmit(TransmitCalibration::Voice).unwrap();
        assert_eq!(cleanup_failure.0.borrow().transmit_changes.len(), 2);

        let edit_failure = SharedChannels::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::with_results([Err(UiError::message(
                "dialog failed",
            ))])),
            SharedStore::with_text("[one]\n"),
            SharedApplier::default(),
            edit_failure.clone(),
            Some("one".to_owned()),
        );
        assert!(matches!(
            app.adjust_transmit(TransmitCalibration::Voice),
            Err(MenuError::Ui(_))
        ));
        assert_eq!(edit_failure.0.borrow().test_tone_changes, [true, false]);
        assert_eq!(edit_failure.0.borrow().transmit_changes.len(), 2);

        assert!(
            app.edit_catalog_setting("hardware", "missing", "Missing")
                .unwrap_err()
                .to_string()
                .contains("tuner catalog omits hardware.missing")
        );
    }

    #[test]
    fn calibration_rejects_wrong_source_and_restores_after_runtime_failure() {
        let speaker = SharedStore::with_text("[one]\n[receive]\naudio_source = speaker\n");
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            speaker,
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_noise()
                .unwrap_err()
                .contains("requires receive_audio_source = flat")
        );

        let store = SharedStore::with_text(config());
        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = VecDeque::from([Err("meter failed".to_owned())]);
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            store.clone(),
            applier.clone(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_voice()
                .unwrap_err()
                .contains("previous configuration was restored")
        );
        assert_eq!(store.0.borrow().text, config());
        assert_eq!(applier.0.borrow().calls, [ApplyMode::Reload; 2]);
        assert_eq!(format_decimal(-0.0), "0");
        assert_eq!(dbfs(0.0), -96.0);
        assert!(mixer_gain_db(0).is_finite());
    }

    #[test]
    fn receive_gain_calibration_reports_apply_exhaustion_and_rollback_failures() {
        let applier = SharedApplier::default();
        applier
            .0
            .borrow_mut()
            .results
            .push_back(Err("candidate rejected".to_owned()));
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            applier,
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        let error = app.calibrate_receive_voice().unwrap_err();
        assert!(error.contains("candidate rejected"));
        assert!(error.contains("previous configuration was restored"));

        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status =
            (0..12).map(|_| Ok(RadioStatus::default())).collect();
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(
            app.calibrate_receive_voice()
                .unwrap_err()
                .contains("failed after 12 attempts")
        );

        let channels = SharedChannels::default();
        channels.0.borrow_mut().radio_status = VecDeque::from([Err("meter failed".to_owned())]);
        let applier = SharedApplier::default();
        applier.0.borrow_mut().results =
            VecDeque::from([Ok(()), Err("rollback rejected".to_owned())]);
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::default()),
            SharedStore::with_text(config()),
            applier,
            channels,
            Some("one".to_owned()),
        );
        let error = app.calibrate_receive_voice().unwrap_err();
        assert!(error.contains("Restoring the previous configuration failed"));
        assert!(error.contains("rollback rejected"));
    }

    #[test]
    fn live_calibration_actions_report_missing_channel_without_runtime_commands() {
        let backend = ScriptedBackend::new((0..4).map(|_| DialogResponse::Accepted(String::new())));
        let channels = SharedChannels::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels.clone(),
            None,
        );
        for live in [
            LiveAction::ActiveConfiguration,
            LiveAction::CalibrateReceiveNoise,
            LiveAction::CalibrateReceiveVoice,
            LiveAction::CalibrateReceiveCtcss,
        ] {
            app.run_live_action(
                &ActionSpec {
                    label: "Calibration".to_owned(),
                    result_title: "Result".to_owned(),
                    action: MenuAction::Live(live),
                },
                live,
            )
            .unwrap();
        }
        assert!(channels.0.borrow().actions.is_empty());
        assert_eq!(backend.0.borrow().requests.len(), 4);
    }

    #[test]
    fn typed_live_actions_display_success_and_failure_without_losing_focus() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let channels = SharedChannels::default();
        channels.0.borrow_mut().action_results =
            VecDeque::from([Ok("levels".to_owned()), Err("not running".to_owned())]);
        let page = MenuPage {
            id: "root".to_owned(),
            title: "Root".to_owned(),
            entries: vec![MenuEntry::Action(ActionSpec {
                label: "Show status".to_owned(),
                result_title: "Status".to_owned(),
                action: MenuAction::Live(LiveAction::RadioStatus),
            })],
        };
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels.clone(),
            Some("one".to_owned()),
        );
        app.browse_tree(&page).unwrap();
        assert_eq!(
            channels.0.borrow().actions,
            [LiveAction::RadioStatus, LiveAction::RadioStatus]
        );
        let requests = &backend.0.borrow().requests;
        assert!(requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { title, body } if title == "Status" && body == "levels"
        )));
        assert!(requests.iter().any(|request| matches!(
            request,
            DialogRequest::Message { title, body }
                if title == "Live operation failed" && body.contains("not running")
        )));
        let defaults = requests
            .iter()
            .filter_map(|request| match request {
                DialogRequest::Menu { default_item, .. } => Some(default_item.as_str()),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(defaults, ["1", "1", "1"]);
    }

    #[test]
    fn profile_selection_writes_named_profiles_and_can_restore_inheritance() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("2".to_owned()),
            DialogResponse::Accepted("site".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("2".to_owned()),
            DialogResponse::Accepted(INHERIT_VALUE.to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text(
            "[one]\n[hardware]\n[hardware zulu]\n[hardware site]\n[local warm]\n",
        );
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );

        app.select_profiles().unwrap();

        assert!(!store.0.borrow().text.contains("hardware_profile"));
        assert_eq!(applier.0.borrow().calls, [ApplyMode::Restart; 2]);
        let selections = backend
            .0
            .borrow()
            .requests
            .iter()
            .filter_map(|request| match request {
                DialogRequest::Selection { items, .. } => Some(
                    items
                        .iter()
                        .map(|item| item.value.clone())
                        .collect::<Vec<_>>(),
                ),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(selections[0], [INHERIT_VALUE, "site", "zulu"]);
    }

    #[test]
    fn saved_session_backup_and_discard_restore_the_expected_generation() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted(String::new()),
        ]);
        let store = SharedStore::with_text(config());
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );

        app.backup_configuration().unwrap();
        app.save_session().unwrap();
        store.0.borrow_mut().text.push_str("# changed\n");
        app.restart_changed = true;
        assert!(app.discard_session_changes(true).unwrap());
        assert_eq!(store.0.borrow().text, config());
        assert_eq!(store.0.borrow().backups, [config()]);
        assert_eq!(applier.0.borrow().calls, [ApplyMode::Restart]);
        assert!(app.finish_session().unwrap());
    }

    #[test]
    fn exit_decision_can_continue_editing_or_save_without_an_extra_apply() {
        let backend = ScriptedBackend::new([
            DialogResponse::Cancelled,
            DialogResponse::Accepted("save".to_owned()),
        ]);
        let store = SharedStore::with_text(config());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store,
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.saved_config = Some("older\n".to_owned());

        assert!(!app.finish_session().unwrap());
        assert!(app.finish_session().unwrap());
        assert_eq!(app.saved_config.as_deref(), Some(config()));
    }

    #[test]
    fn usb_swap_is_a_validated_config_edit_and_echo_uses_live_control() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("two".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Accepted(String::new()),
        ]);
        let channels = SharedChannels::default();
        channels.0.borrow_mut().echo = Ok(false);
        let store = SharedStore::with_text(
            "[one]\nhardware_profile = one\n[hardware one]\n\
             hardware_device_identifier = first\nhardware_serial = A\n\
             hardware_gpio_usb_port_path = 1-1\n\
             [two]\nhardware_profile = two\n[hardware two]\n\
             hardware_device_identifier = second\nhardware_serial = B\n\
             hardware_gpio_usb_port_path = 2-1\n",
        );
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            store.clone(),
            SharedApplier::default(),
            channels.clone(),
            Some("one".to_owned()),
        );

        app.swap_usb_device().unwrap();
        app.toggle_echo().unwrap();

        let state = channels.0.borrow();
        assert_eq!(state.selections, ["one"]);
        assert_eq!(state.echo_changes, [true]);
        let document = ConfigDocument::new(store.0.borrow().text.clone());
        let first = document.resolved_values("one", "hardware").unwrap();
        let second = document.resolved_values("two", "hardware").unwrap();
        assert_eq!(first["hardware_device_identifier"], "second");
        assert_eq!(first["hardware_serial"], "B");
        assert_eq!(first["hardware_gpio_usb_port_path"], "2-1");
        assert_eq!(second["hardware_device_identifier"], "first");
        let requests = &backend.0.borrow().requests;
        assert!(matches!(
            &requests[0],
            DialogRequest::Selection { default_item, items, .. }
                if default_item == "two" && items.len() == 1
        ));
        assert!(matches!(
            &requests[3],
            DialogRequest::Selection { default_item, .. } if default_item == "no"
        ));
    }

    #[test]
    fn usb_swap_recovers_from_restart_failure_and_rejects_shared_profiles() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("two".to_owned()),
            DialogResponse::Accepted(String::new()),
        ]);
        let store = SharedStore::with_text(
            "[one]\nhardware_profile = one\n[hardware one]\n\
             hardware_device_identifier = first\n\
             [two]\nhardware_profile = two\n[hardware two]\n\
             hardware_device_identifier = second\n",
        );
        let applier = SharedApplier::default();
        applier
            .0
            .borrow_mut()
            .results
            .extend([Err("restart failed".to_owned()), Ok(())]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store,
            applier.clone(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.swap_usb_device().unwrap();
        assert_eq!(
            applier.0.borrow().calls,
            [ApplyMode::Restart, ApplyMode::Restart]
        );

        let mut document = ConfigDocument::new(
            "[one]\nhardware_profile = north\n\
             [two]\nhardware_profile = south\n\
             [three]\nhardware_profile = north\n\
             [hardware north]\nhardware_device_identifier = first\n\
             [hardware south]\nhardware_device_identifier = second\n",
        );
        assert!(
            swap_hardware_identity(&mut document, "one", "two")
                .unwrap_err()
                .contains("shared by another channel")
        );
    }

    #[test]
    fn error_types_have_complete_semantics() {
        let ui = MenuError::from(UiError::message("broken"));
        assert_eq!(ui.to_string(), "user interface failed: broken");
        assert!(std::error::Error::source(&ui).is_some());
        let configuration = MenuError::from(ConfigError::MissingChannel("one".to_owned()));
        assert!(configuration.to_string().contains("configuration failed"));
        assert!(std::error::Error::source(&configuration).is_some());
        for error in [
            MenuError::Storage("disk".to_owned()),
            MenuError::Control("runtime".to_owned()),
        ] {
            assert!(!error.to_string().is_empty());
            assert!(std::error::Error::source(&error).is_none());
        }
    }

    #[test]
    fn repository_implements_the_tuner_storage_port() {
        let root = std::env::temp_dir().join(format!(
            "usbradioplus-tune-menu-store-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("radio.conf");
        std::fs::write(&path, "old\n").unwrap();
        let repository = ConfigRepository::new(&path, std::iter::empty::<std::path::PathBuf>());
        assert_eq!(
            ConfigurationStore::read_config(&repository).unwrap(),
            "old\n"
        );
        ConfigurationStore::write_config(&repository, "new\n").unwrap();
        let backup = ConfigurationStore::backup_config(&repository).unwrap();
        assert!(backup.contains("radio.conf.bak."));

        let missing = ConfigRepository::new(
            root.join("missing/radio.conf"),
            std::iter::empty::<std::path::PathBuf>(),
        );
        assert!(ConfigurationStore::read_config(&missing).is_err());
        assert!(ConfigurationStore::write_config(&missing, "x").is_err());
        assert!(ConfigurationStore::backup_config(&missing).is_err());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn menu_action_dispatch_covers_every_session_and_control_operation() {
        let backend = ScriptedBackend::new(std::iter::repeat_n(
            DialogResponse::Accepted(String::new()),
            6,
        ));
        let channels = SharedChannels::default();
        channels.0.borrow_mut().echo = Err("unavailable".to_owned());
        let store = SharedStore::with_text(config());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store,
            SharedApplier::default(),
            channels,
            None,
        );
        app.saved_config = Some(config().to_owned());
        for action in [
            MenuAction::Live(LiveAction::RadioStatus),
            MenuAction::SelectProfiles,
            MenuAction::SwapUsbDevice,
            MenuAction::ToggleEcho,
            MenuAction::SaveSession,
            MenuAction::BackupConfiguration,
            MenuAction::DiscardSessionChanges,
        ] {
            app.run_menu_action(&ActionSpec {
                label: "Action".to_owned(),
                result_title: "Result".to_owned(),
                action,
            })
            .unwrap();
        }
    }

    #[test]
    fn cancelled_and_failed_live_helpers_leave_the_session_usable() {
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Accepted(
                String::new(),
            )])),
            SharedStore::with_text("[one]\n[hardware]\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.swap_usb_device().unwrap();

        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Cancelled])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.swap_usb_device().unwrap();

        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([
                DialogResponse::Accepted("two".to_owned()),
                DialogResponse::Accepted(String::new()),
            ])),
            SharedStore::with_text("[one]\n[two]\n[hardware]\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.swap_usb_device().unwrap();

        let channels = SharedChannels::default();
        channels.0.borrow_mut().echo = Ok(true);
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Cancelled])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        app.toggle_echo().unwrap();

        let channels = SharedChannels::default();
        {
            let mut state = channels.0.borrow_mut();
            state.echo = Ok(true);
            state.echo_results = VecDeque::from([Err("echo failed".to_owned())]);
        }
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([
                DialogResponse::Accepted("no".to_owned()),
                DialogResponse::Accepted(String::new()),
            ])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        app.toggle_echo().unwrap();
    }

    #[test]
    fn invalid_menu_choices_and_flat_defaults_are_non_destructive() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("bad".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let root = MenuPage {
            id: "root".to_owned(),
            title: "Root".to_owned(),
            entries: Vec::new(),
        };
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text("[hardware]\nenabled = no\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        app.browse_tree(&root).unwrap();

        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text("[hardware]\nenabled = no\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        app.browse(&[section()]).unwrap();
        let document = app.document().unwrap();
        let (target, values) = app.section_values(&document, "hardware").unwrap();
        assert_eq!(target, "hardware");
        assert_eq!(values["enabled"], "no");
        assert!(
            app.validate_candidate(&document, "hardware", "enabled")
                .is_ok()
        );
    }

    #[test]
    fn prompts_can_be_cancelled_without_validation() {
        let backend = ScriptedBackend::new(std::iter::repeat_n(DialogResponse::Cancelled, 4));
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        for setting in &section().settings[2..] {
            assert_eq!(
                app.prompt_setting(setting, &setting.default_value).unwrap(),
                None
            );
        }
        let text = SettingSpec {
            key: "name".to_owned(),
            label: "Name".to_owned(),
            default_value: "radio".to_owned(),
            kind: SettingKind::Text(TextEditor {
                instruction: "Enter a name".to_owned(),
                syntax: TextSyntax::NonEmpty,
            }),
            apply_mode: ApplyMode::Reload,
        };
        assert_eq!(app.prompt_setting(&text, "radio").unwrap(), None);
    }

    #[test]
    fn discard_cancellation_and_reload_paths_are_distinct() {
        let backend = ScriptedBackend::new([DialogResponse::Cancelled]);
        let store = SharedStore::with_text("changed\n");
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        app.saved_config = Some("saved\n".to_owned());
        assert!(!app.discard_session_changes(true).unwrap());
        assert_eq!(store.0.borrow().text, "changed\n");

        let backend = ScriptedBackend::new([DialogResponse::Accepted("discard".to_owned())]);
        let store = SharedStore::with_text("changed\n");
        let applier = SharedApplier::default();
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier.clone(),
            SharedChannels::default(),
            None,
        );
        app.saved_config = Some("saved\n".to_owned());
        assert!(app.finish_session().unwrap());
        assert_eq!(store.0.borrow().text, "saved\n");
        assert_eq!(applier.0.borrow().calls, [ApplyMode::Reload]);
    }

    #[test]
    fn continuing_an_exit_returns_to_the_same_configuration_tree() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("yes".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
            DialogResponse::Accepted("save".to_owned()),
        ]);
        let root = MenuPage {
            id: "root".to_owned(),
            title: "USBRadioPlus configuration".to_owned(),
            entries: vec![MenuEntry::Section(section())],
        };
        let store = SharedStore::with_text(config());
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );

        app.browse_tree(&root).unwrap();

        assert!(store.0.borrow().text.contains("enabled = yes"));
    }

    #[test]
    fn failed_discard_keeps_the_changed_generation_active() {
        let backend = ScriptedBackend::new([DialogResponse::Accepted(String::new())]);
        let store = SharedStore::with_text("changed\n");
        let applier = SharedApplier::default();
        applier.0.borrow_mut().results =
            VecDeque::from([Err("saved generation rejected".to_owned()), Ok(())]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier,
            SharedChannels::default(),
            None,
        );
        app.saved_config = Some("saved\n".to_owned());

        assert!(!app.discard_session_changes(false).unwrap());
        assert_eq!(store.0.borrow().text, "changed\n");
    }

    #[test]
    fn cancelled_and_invalid_nested_selections_preserve_navigation() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("99".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Cancelled,
            DialogResponse::Cancelled,
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text("[one]\n[local]\n[local clean]\n"),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.select_profiles().unwrap();

        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("99".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.browse_section(&section()).unwrap();

        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Cancelled])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.choose_channel().unwrap();
        assert_eq!(app.selected_channel(), Some("one"));
    }

    #[test]
    fn prompt_defaults_validation_and_ui_failures_are_explicit() {
        let optional = SettingSpec {
            key: "assignment".to_owned(),
            label: "Assignment".to_owned(),
            default_value: "off".to_owned(),
            kind: SettingKind::OptionalEnumeration(vec![ChoiceItem::new("off", "Disabled")]),
            apply_mode: ApplyMode::Reload,
        };
        let backend = ScriptedBackend::new([
            DialogResponse::Cancelled,
            DialogResponse::Accepted(" station ".to_owned()),
        ]);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend.clone()),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        assert_eq!(app.prompt_setting(&optional, "unknown").unwrap(), None);
        backend
            .0
            .borrow_mut()
            .responses
            .push_front(Ok(DialogResponse::Accepted("off".to_owned())));
        assert_eq!(
            app.prompt_setting(&optional, "unknown").unwrap(),
            Some("off".to_owned())
        );
        backend
            .0
            .borrow_mut()
            .responses
            .push_front(Ok(DialogResponse::Accepted("off".to_owned())));
        let optional_section = SectionSpec {
            section: "hardware".to_owned(),
            title: "Optional assignment".to_owned(),
            settings: vec![optional.clone()],
        };
        app.edit_setting(&optional_section, &optional).unwrap();
        let text = SettingSpec {
            key: "name".to_owned(),
            label: "Name".to_owned(),
            default_value: "radio".to_owned(),
            kind: SettingKind::Text(TextEditor {
                instruction: "Enter a name".to_owned(),
                syntax: TextSyntax::NonEmpty,
            }),
            apply_mode: ApplyMode::Reload,
        };
        assert_eq!(
            app.prompt_setting(&text, "radio").unwrap(),
            Some("station".to_owned())
        );
        assert!(matches!(
            &backend.0.borrow().requests[0],
            DialogRequest::Selection { default_item, .. } if default_item == INHERIT_VALUE
        ));

        let failing = ScriptedBackend::with_results([Err(UiError::message("selection failed"))]);
        let mut app = TunerApp::new(
            AccessibleUi::new(failing),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        assert!(matches!(
            app.prompt_setting(&section().settings[0], "yes"),
            Err(MenuError::Ui(_))
        ));

        let editor = FloatEditor {
            minimum: Some(0.0),
            maximum: Some(2.0),
            exclusive_minimum: true,
            units: String::new(),
        };
        assert!(validate_float("0", &editor).is_err());
        assert!(validate_float("3", &editor).is_err());
        assert!(validate_float("not-a-number", &editor).is_err());
        let integer = IntegerEditor {
            minimum: Some(1),
            maximum: Some(2),
            units: String::new(),
        };
        assert!(validate_integer("0", &integer).is_err());
        assert!(validate_integer("3", &integer).is_err());
        assert!(validate_integer("not-an-integer", &integer).is_err());

        let gain = &section().settings[4];
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Cancelled])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        assert_eq!(app.prompt_setting(gain, "-3").unwrap(), None);
    }

    #[test]
    fn processing_candidate_and_resolution_failure_fallbacks_are_safe() {
        let app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new(std::iter::empty())),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        let document = ConfigDocument::new("[one]\n[local]\ninput_gain_db = 0\n");
        assert!(
            app.validate_candidate(&document, "local", "input_gain_db")
                .is_ok()
        );

        let ambiguous = ConfigDocument::new(
            "[one]\nlocal_profile = one\nLOCAL_PROFILE = two\n[local one]\n[local two]\n",
        );
        assert!(app.section_values(&ambiguous, "local").is_ok());
        let ambiguous = ConfigDocument::new(
            "[one]\nhardware_profile = one\nHARDWARE_PROFILE = two\n[hardware one]\n[hardware two]\n",
        );
        assert!(app.section_values(&ambiguous, "hardware").is_ok());
        assert!(
            app.validate_candidate(&ambiguous, "hardware", "hardware_input_gain_db")
                .is_err()
        );
    }

    #[test]
    fn failed_profile_apply_and_echo_dialog_errors_are_non_destructive() {
        let backend = ScriptedBackend::new([
            DialogResponse::Accepted("1".to_owned()),
            DialogResponse::Accepted("clean".to_owned()),
            DialogResponse::Accepted(String::new()),
            DialogResponse::Cancelled,
        ]);
        let store = SharedStore::with_text("[one]\n[local]\n[local clean]\n");
        let applier = SharedApplier::default();
        applier
            .0
            .borrow_mut()
            .results
            .push_back(Err("profile rejected".to_owned()));
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            store.clone(),
            applier,
            SharedChannels::default(),
            Some("one".to_owned()),
        );
        app.select_profiles().unwrap();
        assert!(!store.0.borrow().text.contains("asterisk_profile"));

        let backend = ScriptedBackend::with_results([Err(UiError::message("dialog failed"))]);
        let channels = SharedChannels::default();
        channels.0.borrow_mut().echo = Ok(false);
        let mut app = TunerApp::new(
            AccessibleUi::new(backend),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            channels,
            Some("one".to_owned()),
        );
        assert!(matches!(app.toggle_echo(), Err(MenuError::Ui(_))));

        let final_limiter = SectionSpec {
            section: "voice_telemetry".to_owned(),
            title: "Final Limiter".to_owned(),
            settings: Vec::new(),
        };
        assert!(
            visible_settings(&final_limiter, &BTreeMap::new())
                .settings
                .is_empty()
        );
    }

    #[test]
    fn unset_session_and_channel_defaults_require_no_external_action() {
        let mut app = TunerApp::new(
            AccessibleUi::new(ScriptedBackend::new([DialogResponse::Cancelled])),
            SharedStore::with_text(config()),
            SharedApplier::default(),
            SharedChannels::default(),
            None,
        );
        assert!(app.discard_session_changes(false).unwrap());
        app.choose_channel().unwrap();
        assert_eq!(app.selected_channel(), None);
    }
}
