//! One remote-base configuration resolver for channel and standalone hosts.

use crate::{ConfigDocument, CtcssSource, DcsCode, DcsPolarity, SignalingMethod, StationConfig};
use crate::{ResolutionWarning, ResolutionWarningKind};
use std::{collections::BTreeMap, fmt};

/// Explicit physical control backend; parallel operations retain their existing owner.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemoteBackend {
    /// Released Hamlib adapter and one rig per node.
    Hamlib,
    /// Existing four-bit parallel channel selection.
    ParallelChannel,
    /// Existing two-word parallel RTX programming.
    ParallelRtx,
}

/// Physical audio admission; both paths use the shared PortAudio adapter.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemoteAudioSource {
    /// CM119 audio plus its existing HID/mixer capabilities.
    Cm119,
    /// Generic onboard USB sound, without assumed CM119 HID.
    Alsa,
}

/// Remote operating command authorization; status remains read-only and public.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemoteDtmfAccess {
    /// Any command source may operate the rig.
    Open,
    /// Existing source-specific administrative unlock is required.
    Locked,
    /// DTMF operating requests are disabled.
    Disabled,
}

/// Configured signal provider, resolved once against physical capabilities.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum RemoteSignalSource {
    /// Signal is not used.
    #[default]
    Disabled = 0,
    /// Shared DSP generates or decodes the signal.
    Dsp = 1,
    /// Existing external encoder/decoder and hardware signaling.
    External = 2,
    /// Radio's own supported encoder/decoder through Hamlib.
    Rig = 3,
    /// Select a supported provider at setup, preferring Hamlib; no runtime fallback.
    Automatic = 4,
}

/// One independently configured signaling direction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RemoteSignal {
    /// Provider, with automatic resolved before creating the shared engine.
    pub source: RemoteSignalSource,
    /// Optional decoder-programming provider, separate from actual qualification.
    /// Absent follows `source`; encoders never use this override.
    pub control_source: Option<RemoteSignalSource>,
    /// Configured enablement, distinct from activity-scoped physical encode gating.
    pub enabled: bool,
    /// CTCSS in tenths of hertz; zero selects DCS/no tone.
    pub tone_tenths_hz: u32,
    /// Actual nine-bit DCS value.
    pub dcs_code: u32,
    /// Inverted DCS polarity.
    pub inverted: bool,
}

/// Complete owned operating configuration; no hardware or runtime handle is retained.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RemoteOperatingState {
    /// Base receive VFO frequency in hertz.
    pub receive_hz: u64,
    /// Base transmit VFO frequency, or receive-only.
    pub transmit_hz: Option<u64>,
    /// Shared mode selector: FM=1, AM=2, USB=3, LSB=4, through packet LSB=12.
    pub mode: u32,
    /// Passband width; zero requests normal width.
    pub bandwidth_hz: u32,
    /// Shared receive VFO selector, current=0 through memory=5.
    pub receive_vfo: u32,
    /// Shared transmit VFO selector.
    pub transmit_vfo: u32,
    /// Split-VFO routing, independent of repeater shift and VFO values.
    pub split: bool,
    /// Signed repeater shift in hertz.
    pub offset_hz: i64,
    /// Receive incremental tuning in hertz.
    pub rit_hz: i64,
    /// Apply RIT; a stored offset alone is not an enabled RF shift.
    pub rit_enabled: bool,
    /// Transmit incremental tuning in hertz.
    pub xit_hz: i64,
    /// Apply XIT independently of RIT.
    pub xit_enabled: bool,
    /// Configured RF power in milliwatts, when established.
    pub power_mw: Option<u32>,
    /// Independent encode request.
    pub encode: RemoteSignal,
    /// Independent decode request.
    pub decode: RemoteSignal,
    /// Optional four-bit parallel channel number.
    pub channel: Option<u8>,
}

/// Configuration-only named memory.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemoteMemory {
    /// Stable positive DTMF selector.
    pub selector: u32,
    /// Configured name, not an identifier generated from an array index.
    pub name: String,
    /// Complete requested operating state.
    pub state: RemoteOperatingState,
}

/// Configuration-only named scan list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemoteScanList {
    /// Stable positive DTMF selector.
    pub selector: u32,
    /// Configured name.
    pub name: String,
    /// Ordered configured memory selectors.
    pub members: Vec<u32>,
    /// Dwell after completed tuning and settling.
    pub dwell_ms: u64,
    /// Quiet hold after qualified activity.
    pub resume_delay_ms: u64,
    /// Model-specific settling after tune completion.
    pub settle_ms: u64,
    /// Optional configured priority member.
    pub priority: Option<u32>,
}

/// Shared resolved settings. RF admission itself belongs to the remote-base DSO.
#[derive(Clone, PartialEq, Eq)]
pub struct ResolvedRemoteConfiguration {
    /// One explicit control backend.
    pub backend: RemoteBackend,
    /// Audio device capabilities required by this composition.
    pub audio_source: RemoteAudioSource,
    /// Canonical named model, never a numeric model value.
    pub hamlib_model: String,
    /// Named native transport options. Diagnostic formatting must not expose values.
    pub hamlib_options: BTreeMap<String, String>,
    /// Positive port timeout, milliseconds.
    pub timeout_ms: u32,
    /// Finite transaction retries.
    pub retries: u32,
    /// Finite additional read-timeout retries.
    pub timeout_retries: u32,
    /// Monotonic rig-state poll interval.
    pub poll_interval_ms: u64,
    /// Optional explicit PTT provider: rig, usb, or parallel.
    pub ptt_source: Option<String>,
    /// Optional explicit COR provider: rig, dsp, usb, parallel, or open.
    pub cor_source: Option<String>,
    /// Inclusive permitted receive intervals.
    pub receive_ranges: Vec<(u64, u64)>,
    /// Inclusive permitted transmit intervals.
    pub transmit_ranges: Vec<(u64, u64)>,
    /// Explicitly transmit-prohibited intervals.
    pub receive_only_ranges: Vec<(u64, u64)>,
    /// Permitted shared mode selectors.
    pub modes: Vec<u32>,
    /// Optional established transmit power ceiling.
    pub maximum_power_mw: Option<u32>,
    /// Configuration-only memory definitions.
    pub memories: Vec<RemoteMemory>,
    /// Configuration-only scan definitions.
    pub scan_lists: Vec<RemoteScanList>,
    /// Startup memory selector.
    pub startup_memory: u32,
    /// Administrative DTMF policy.
    pub dtmf_access: RemoteDtmfAccess,
    /// Remote command family, excluding the initial star.
    pub dtmf_prefix: String,
    /// Configured magnitude for familiar plus/minus offset commands.
    pub offset_magnitude_hz: Option<u64>,
    /// Optional low/medium/high RF-power presets, never fabricated levels.
    pub power_presets_mw: [Option<u32>; 3],
    /// Optional slow/medium/fast scan dwell presets.
    pub scan_presets_ms: [Option<u64>; 3],
    /// Maximum duration of the admitted manual tune operation.
    pub tune_duration_ms: u64,
    /// Shared unknown-value/inheritance diagnostics, with transport values redacted.
    pub warnings: Vec<ResolutionWarning>,
}

impl fmt::Debug for ResolvedRemoteConfiguration {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        out.debug_struct("ResolvedRemoteConfiguration")
            .field("backend", &self.backend)
            .field("model", &self.hamlib_model)
            .field("transport_option_names", &self.hamlib_options.keys())
            .field("memories", &self.memories)
            .field("scan_lists", &self.scan_lists)
            .finish_non_exhaustive()
    }
}

/// Unsafe or incomplete enabled configuration; its supplied value is not echoed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemoteConfigurationError {
    /// Source file or logical document name.
    pub source: String,
    /// Section containing the invalid setting.
    pub section: String,
    /// Setting requiring correction.
    pub key: String,
}

impl fmt::Display for RemoteConfigurationError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            out,
            "{} [{}]: invalid or missing remote-base {}",
            self.source, self.section, self.key
        )
    }
}
impl std::error::Error for RemoteConfigurationError {}

struct Values<'a> {
    source: &'a str,
    section: String,
    values: BTreeMap<String, String>,
}

impl Values<'_> {
    fn unknown(&self, allowed: &[&str], transport: bool) -> Vec<ResolutionWarning> {
        self.values
            .keys()
            .filter(|key| {
                !(allowed.contains(&key.as_str()) || transport && key.starts_with("hamlib_option_"))
            })
            .map(|key| {
                warning(
                    self.source,
                    &self.section,
                    key,
                    ResolutionWarningKind::UnknownOption,
                    "ignored",
                )
            })
            .collect()
    }
    fn error(&self, key: &str) -> RemoteConfigurationError {
        RemoteConfigurationError {
            source: self.source.into(),
            section: self.section.clone(),
            key: key.into(),
        }
    }
    fn text(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str)
    }
    fn required(&self, key: &str) -> Result<&str, RemoteConfigurationError> {
        self.text(key)
            .filter(|value| !value.is_empty())
            .ok_or_else(|| self.error(key))
    }
    fn number<T: std::str::FromStr>(
        &self,
        key: &str,
        default: T,
    ) -> Result<T, RemoteConfigurationError> {
        self.text(key).map_or(Ok(default), |value| {
            value.parse().map_err(|_| self.error(key))
        })
    }
    fn optional<T: std::str::FromStr>(
        &self,
        key: &str,
    ) -> Result<Option<T>, RemoteConfigurationError> {
        self.text(key)
            .map(|value| value.parse().map_err(|_| self.error(key)))
            .transpose()
    }
    fn scaled(&self, key: &str, places: usize) -> Result<Option<u32>, RemoteConfigurationError> {
        self.text(key)
            .map(|value| scaled(value, places).ok_or_else(|| self.error(key)))
            .transpose()
    }
}

fn scaled(value: &str, places: usize) -> Option<u32> {
    let (whole, fraction) = value.split_once('.').unwrap_or((value, ""));
    if whole.is_empty()
        || !whole.bytes().all(|byte| byte.is_ascii_digit())
        || fraction.len() > places
        || !fraction.bytes().all(|byte| byte.is_ascii_digit())
    {
        return None;
    }
    whole
        .parse::<u32>()
        .ok()?
        .checked_mul(10_u32.pow(places as u32))?
        .checked_add(if fraction.is_empty() {
            0
        } else {
            fraction
                .parse::<u32>()
                .ok()?
                .checked_mul(10_u32.pow((places - fraction.len()) as u32))?
        })
}

fn bool_value(value: &str) -> Option<bool> {
    match value {
        "yes" => Some(true),
        "no" => Some(false),
        _ => None,
    }
}

fn ranges(values: &Values<'_>, key: &str) -> Result<Vec<(u64, u64)>, RemoteConfigurationError> {
    let Some(text) = values.text(key) else {
        return Ok(Vec::new());
    };
    text.split(',')
        .map(|part| {
            let (low, high) = part
                .trim()
                .split_once('-')
                .ok_or_else(|| values.error(key))?;
            let low = low.trim().parse::<u64>().map_err(|_| values.error(key))?;
            let high = high.trim().parse::<u64>().map_err(|_| values.error(key))?;
            if low == 0 || low > high {
                return Err(values.error(key));
            }
            Ok((low, high))
        })
        .collect()
}

fn mode(value: &str) -> Option<u32> {
    [
        "FM", "AM", "USB", "LSB", "CW", "CWR", "RTTY", "RTTYR", "WFM", "PKTFM", "PKTUSB", "PKTLSB",
    ]
    .iter()
    .position(|name| value.eq_ignore_ascii_case(name))
    .map(|index| index as u32 + 1)
}

fn source(value: &str) -> Option<RemoteSignalSource> {
    match value {
        "disabled" => Some(RemoteSignalSource::Disabled),
        "dsp" => Some(RemoteSignalSource::Dsp),
        "external" => Some(RemoteSignalSource::External),
        "hamlib" => Some(RemoteSignalSource::Rig),
        "auto" => Some(RemoteSignalSource::Automatic),
        _ => None,
    }
}

fn signal(
    values: &Values<'_>,
    encode: bool,
    inherited: RemoteSignal,
) -> Result<RemoteSignal, RemoteConfigurationError> {
    let direction = if encode { "encode" } else { "decode" };
    let tone_key = format!("ctcss_{direction}_hz");
    let code_key = format!("dcs_{direction}_code");
    let source_key = format!("ctcss_{direction}_source");
    let dcs_source_key = format!("dcs_{direction}_source");
    let selected_source = values
        .text(&source_key)
        .or_else(|| values.text(&dcs_source_key));
    if values.text(&source_key).is_some() && values.text(&dcs_source_key).is_some() {
        return Err(values.error(&source_key));
    }
    let mut result = inherited;
    if !encode {
        if let Some(value) = values.text("decode_control_source") {
            result.control_source =
                Some(source(value).ok_or_else(|| values.error("decode_control_source"))?);
        }
    }
    if let Some(tone) = values.scaled(&tone_key, 1)? {
        if !(500..=3000).contains(&tone) || values.text(&code_key).is_some() {
            return Err(values.error(&tone_key));
        }
        result.tone_tenths_hz = tone;
        result.dcs_code = 0;
        result.inverted = false;
        if selected_source.is_none() {
            result.source = RemoteSignalSource::Automatic;
        }
        result.enabled = true;
    }
    if let Some(code) = values.text(&code_key) {
        let code = code
            .parse::<DcsCode>()
            .map_err(|_| values.error(&code_key))?;
        result.dcs_code = u32::from(code.value());
        result.inverted = code.polarity() == DcsPolarity::Inverted;
        result.tone_tenths_hz = 0;
        if selected_source.is_none() {
            result.source = RemoteSignalSource::Automatic;
        }
        result.enabled = true;
    }
    if let Some(selected) = selected_source {
        result.source = source(selected).ok_or_else(|| values.error(&source_key))?;
        result.enabled = result.source != RemoteSignalSource::Disabled;
        if result.source == RemoteSignalSource::External
            && values.text(&tone_key).is_none()
            && values.text(&code_key).is_none()
        {
            result.tone_tenths_hz = 0;
            result.dcs_code = 0;
            result.inverted = false;
        }
    }
    Ok(result)
}

fn station_signal(station: &StationConfig, encode: bool) -> RemoteSignal {
    let method = if encode {
        station.transmit.signaling_method
    } else {
        station.receive.signaling_method
    };
    let source = if encode {
        RemoteSignalSource::Dsp
    } else {
        match station.ctcss.receive_source {
            CtcssSource::Disabled => RemoteSignalSource::Disabled,
            CtcssSource::Dsp => RemoteSignalSource::Dsp,
            _ => RemoteSignalSource::External,
        }
    };
    match method {
        SignalingMethod::Ctcss => RemoteSignal {
            source,
            enabled: source != RemoteSignalSource::Disabled,
            tone_tenths_hz: station_tone(station, encode),
            ..RemoteSignal::default()
        },
        SignalingMethod::Dcs => {
            let code = if encode {
                station.dcs.transmit_code
            } else {
                station.dcs.receive_code
            };
            RemoteSignal {
                source,
                enabled: source != RemoteSignalSource::Disabled,
                dcs_code: u32::from(code.value()),
                inverted: code.polarity() == DcsPolarity::Inverted,
                ..RemoteSignal::default()
            }
        }
        _ => RemoteSignal {
            tone_tenths_hz: station_tone(station, encode),
            ..RemoteSignal::default()
        },
    }
}

impl ResolvedRemoteConfiguration {
    /// Resolve enabled remote sections without device access or rewriting the source.
    /// Missing/disabled remote settings preserve the ordinary radio composition.
    pub fn from_document(
        document: &ConfigDocument,
        source: &str,
        node: &str,
        station: &StationConfig,
    ) -> Result<Option<Self>, RemoteConfigurationError> {
        let mut warnings = Vec::new();
        let mut global = defaults(BASE_DEFAULTS);
        overlay(document, source, "remote_base", &mut global, &mut warnings)?;
        overlay(
            document,
            source,
            &format!("remote_base {node}"),
            &mut global,
            &mut warnings,
        )?;
        let values = Values {
            source,
            section: format!("remote_base {node}"),
            values: global,
        };
        if !bool_value(values.text("enabled").unwrap_or("no"))
            .ok_or_else(|| values.error("enabled"))?
        {
            return Ok(None);
        }
        let backend = match values.required("backend")? {
            "hamlib" => RemoteBackend::Hamlib,
            "parallel_channel" => RemoteBackend::ParallelChannel,
            "parallel_rtx" => RemoteBackend::ParallelRtx,
            _ => return Err(values.error("backend")),
        };
        let audio_source = match values.text("audio_source").unwrap_or("cm119") {
            "cm119" => RemoteAudioSource::Cm119,
            "alsa" => RemoteAudioSource::Alsa,
            _ => return Err(values.error("audio_source")),
        };
        let hamlib_model = values.text("hamlib_model").unwrap_or_default().to_owned();
        if backend == RemoteBackend::Hamlib
            && (!hamlib_model.starts_with("RIG_MODEL_")
                || hamlib_model.len() <= 10
                || !hamlib_model
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_'))
        {
            return Err(values.error("hamlib_model"));
        }
        let mut hamlib_options = BTreeMap::new();
        if let Some(device) = values.text("hamlib_device") {
            hamlib_options.insert("rig_pathname".into(), device.into());
        }
        for (key, value) in &values.values {
            if let Some(option) = key.strip_prefix("hamlib_option_") {
                if option.is_empty()
                    || matches!(
                        option,
                        "timeout" | "retry" | "timeout_retry" | "rig_pathname"
                    )
                {
                    return Err(values.error(key));
                }
                hamlib_options.insert(option.into(), value.clone());
            }
        }
        let timeout_ms = values.number("timeout_ms", 500)?;
        let retries = values.number("retries", 2)?;
        let timeout_retries = values.number("timeout_retries", 0)?;
        let poll_interval_ms = values.number("poll_interval_ms", 500_u64)?;
        if !(1..=10_000).contains(&timeout_ms)
            || retries > 10
            || timeout_retries > 100
            || poll_interval_ms == 0
        {
            return Err(values.error("transport/poll interval"));
        }
        let modes = values
            .required("allowed_modes")?
            .split(',')
            .map(|name| mode(name.trim()).ok_or_else(|| values.error("allowed_modes")))
            .collect::<Result<Vec<_>, _>>()?;
        let receive_ranges = ranges(&values, "allowed_rx_ranges_hz")?;
        if receive_ranges.is_empty() {
            return Err(values.error("allowed_rx_ranges_hz"));
        }
        let encode = signal(&values, true, station_signal(station, true))?;
        let decode = signal(&values, false, station_signal(station, false))?;
        let mut memories = Vec::<RemoteMemory>::new();
        warnings.extend(values.unknown(BASE_KEYS, true));
        for section in document.section_names() {
            let mut words = section.splitn(3, char::is_whitespace);
            if !words
                .next()
                .is_some_and(|kind| kind.eq_ignore_ascii_case("remote_memory"))
                || words.next() != Some(node)
            {
                continue;
            }
            let name = words
                .next()
                .filter(|name| !name.is_empty())
                .ok_or_else(|| values.error("memory name"))?
                .to_owned();
            let mut memory_values = defaults(MEMORY_DEFAULTS);
            overlay(
                document,
                source,
                &section,
                &mut memory_values,
                &mut warnings,
            )?;
            let memory = Values {
                source,
                section: section.clone(),
                values: memory_values,
            };
            let selector = memory.number("selector", 0)?;
            if selector == 0 || memories.iter().any(|old| old.selector == selector) {
                return Err(memory.error("selector"));
            }
            let receive_hz = memory.number("rx_frequency_hz", 0)?;
            if receive_hz == 0 {
                return Err(memory.error("rx_frequency_hz"));
            }
            let transmit_hz = match memory.text("tx_frequency_hz") {
                None | Some("none") => None,
                Some(value) => Some(
                    value
                        .parse::<u64>()
                        .map_err(|_| memory.error("tx_frequency_hz"))?,
                ),
            };
            let mode =
                mode(memory.text("mode").unwrap_or("FM")).ok_or_else(|| memory.error("mode"))?;
            let channel = memory.optional::<u8>("channel")?;
            if channel.is_some_and(|value| value > 15) {
                return Err(memory.error("channel"));
            }
            warnings.extend(memory.unknown(MEMORY_KEYS, false));
            memories.push(RemoteMemory {
                selector,
                name,
                state: RemoteOperatingState {
                    receive_hz,
                    transmit_hz,
                    mode,
                    bandwidth_hz: memory.number("bandwidth_hz", 0)?,
                    receive_vfo: vfo(&memory, "receive_vfo")?,
                    transmit_vfo: vfo(&memory, "transmit_vfo")?,
                    split: memory
                        .text("split")
                        .and_then(bool_value)
                        .unwrap_or(transmit_hz.is_some_and(|transmit| transmit != receive_hz)),
                    offset_hz: memory.number("offset_hz", 0)?,
                    rit_hz: memory.number("rit_hz", 0)?,
                    xit_hz: memory.number("xit_hz", 0)?,
                    rit_enabled: memory
                        .text("rit_enabled")
                        .and_then(bool_value)
                        .unwrap_or(memory.number::<i64>("rit_hz", 0)? != 0),
                    xit_enabled: memory
                        .text("xit_enabled")
                        .and_then(bool_value)
                        .unwrap_or(memory.number::<i64>("xit_hz", 0)? != 0),
                    power_mw: memory.scaled("power_w", 3)?,
                    encode: signal(&memory, true, encode)?,
                    decode: signal(&memory, false, decode)?,
                    channel,
                },
            });
        }
        let lookup = |name: &str| {
            memories
                .iter()
                .find(|memory| memory.name == name)
                .map(|memory| memory.selector)
        };
        let startup_memory = lookup(values.required("startup_memory")?)
            .ok_or_else(|| values.error("startup_memory"))?;
        let mut scan_lists = Vec::<RemoteScanList>::new();
        for section in document.section_names() {
            let mut words = section.splitn(3, char::is_whitespace);
            if !words
                .next()
                .is_some_and(|kind| kind.eq_ignore_ascii_case("remote_scan"))
                || words.next() != Some(node)
            {
                continue;
            }
            let name = words
                .next()
                .filter(|name| !name.is_empty())
                .ok_or_else(|| values.error("scan name"))?
                .to_owned();
            let mut scan_values = defaults(SCAN_DEFAULTS);
            overlay(document, source, &section, &mut scan_values, &mut warnings)?;
            let scan = Values {
                source,
                section: section.clone(),
                values: scan_values,
            };
            let selector = scan.number("selector", 0)?;
            if selector == 0 || scan_lists.iter().any(|old| old.selector == selector) {
                return Err(scan.error("selector"));
            }
            let members = scan
                .required("members")?
                .split(',')
                .map(|name| lookup(name.trim()).ok_or_else(|| scan.error("members")))
                .collect::<Result<Vec<_>, _>>()?;
            let priority = scan
                .text("priority")
                .map(|name| lookup(name).ok_or_else(|| scan.error("priority")))
                .transpose()?;
            let dwell_ms = scan.number("dwell_ms", 500_u64)?;
            let settle_ms = scan.number("settle_ms", 0_u64)?;
            if dwell_ms == 0
                || dwell_ms.checked_add(settle_ms).is_none()
                || priority.is_some_and(|member| !members.contains(&member))
            {
                return Err(scan.error("timing/priority"));
            }
            warnings.extend(scan.unknown(SCAN_KEYS, false));
            scan_lists.push(RemoteScanList {
                selector,
                name,
                members,
                dwell_ms,
                settle_ms,
                resume_delay_ms: scan.number("resume_delay_ms", 2000)?,
                priority,
            });
        }
        let dtmf_access = match values.text("dtmf_access").unwrap_or("locked") {
            "open" => RemoteDtmfAccess::Open,
            "locked" => RemoteDtmfAccess::Locked,
            "disabled" => RemoteDtmfAccess::Disabled,
            _ => return Err(values.error("dtmf_access")),
        };
        let dtmf_prefix = values.text("dtmf_prefix").unwrap_or("6").to_owned();
        if dtmf_prefix.is_empty() || !dtmf_prefix.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err(values.error("dtmf_prefix"));
        }
        let tune_duration_ms = values.number("tune_duration_ms", 1000)?;
        if !(1..=5000).contains(&tune_duration_ms) {
            return Err(values.error("tune_duration_ms"));
        }
        let ptt_source = values.text("ptt_source").map(str::to_owned);
        let cor_source = values.text("cor_source").map(str::to_owned);
        if ptt_source
            .as_deref()
            .is_some_and(|source| !matches!(source, "hamlib" | "usb" | "parallel"))
            || cor_source.as_deref().is_some_and(|source| {
                !matches!(source, "hamlib" | "dsp" | "usb" | "parallel" | "open")
            })
        {
            return Err(values.error("ptt/cor source"));
        }
        Ok(Some(Self {
            backend,
            audio_source,
            hamlib_model,
            hamlib_options,
            timeout_ms,
            retries,
            timeout_retries,
            poll_interval_ms,
            ptt_source,
            cor_source,
            receive_ranges,
            transmit_ranges: ranges(&values, "allowed_tx_ranges_hz")?,
            receive_only_ranges: ranges(&values, "receive_only_ranges_hz")?,
            modes,
            maximum_power_mw: values.scaled("max_power_w", 3)?,
            memories,
            scan_lists,
            startup_memory,
            dtmf_access,
            dtmf_prefix,
            offset_magnitude_hz: values.optional("offset_magnitude_hz")?,
            power_presets_mw: [
                values.scaled("power_low_w", 3)?,
                values.scaled("power_medium_w", 3)?,
                values.scaled("power_high_w", 3)?,
            ],
            scan_presets_ms: [
                values.optional("scan_slow_ms")?,
                values.optional("scan_medium_ms")?,
                values.optional("scan_fast_ms")?,
            ],
            tune_duration_ms,
            warnings,
        }))
    }
}

fn vfo(values: &Values<'_>, key: &str) -> Result<u32, RemoteConfigurationError> {
    match values.text(key).unwrap_or("current") {
        "current" => Ok(0),
        "A" => Ok(1),
        "B" => Ok(2),
        "main" => Ok(3),
        "sub" => Ok(4),
        "memory" => Ok(5),
        _ => Err(values.error(key)),
    }
}

fn section_values(
    document: &ConfigDocument,
    source: &str,
    section: &str,
) -> Result<BTreeMap<String, String>, RemoteConfigurationError> {
    let mut normalized = BTreeMap::new();
    for (key, value) in document.explicit_values(section) {
        let key = key.to_ascii_lowercase();
        if normalized
            .insert(key.clone(), value.clone())
            .is_some_and(|old| old != value)
        {
            return Err(RemoteConfigurationError {
                source: source.into(),
                section: section.into(),
                key,
            });
        }
    }
    Ok(normalized)
}

fn station_tone(station: &StationConfig, encode: bool) -> u32 {
    if encode {
        u32::from(station.ctcss.transmit_default.tenths_hz())
    } else {
        station
            .ctcss
            .receive_frequencies
            .first()
            .map_or(0, |tone| u32::from(tone.tenths_hz()))
    }
}

fn defaults(values: &[(&str, &str)]) -> BTreeMap<String, String> {
    values
        .iter()
        .map(|(name, value)| ((*name).into(), (*value).into()))
        .collect()
}

fn warning(
    source: &str,
    section: &str,
    key: &str,
    kind: ResolutionWarningKind,
    fallback: &str,
) -> ResolutionWarning {
    ResolutionWarning {
        source: source.into(),
        section: section.into(),
        name: key.into(),
        kind,
        supplied_value: "<redacted>".into(),
        fallback: fallback.into(),
        reason: "remote-base setting was not applied".into(),
    }
}

fn overlay(
    document: &ConfigDocument,
    source: &str,
    section: &str,
    inherited: &mut BTreeMap<String, String>,
    warnings: &mut Vec<ResolutionWarning>,
) -> Result<(), RemoteConfigurationError> {
    for (key, value) in section_values(document, source, section)? {
        let optional_default = key.starts_with("ctcss_")
            || key.starts_with("dcs_")
            || matches!(
                key.as_str(),
                "power_low_w"
                    | "power_medium_w"
                    | "power_high_w"
                    | "scan_slow_ms"
                    | "scan_medium_ms"
                    | "scan_fast_ms"
            );
        if !valid_value(&key, &value) && (inherited.contains_key(&key) || optional_default) {
            warnings.push(warning(
                source,
                section,
                &key,
                ResolutionWarningKind::InvalidValue,
                "inherited/default",
            ));
        } else {
            inherited.insert(key, value);
        }
    }
    Ok(())
}

fn valid_value(key: &str, value: &str) -> bool {
    match key {
        "enabled" | "split" | "rit_enabled" | "xit_enabled" => bool_value(value).is_some(),
        "audio_source" => matches!(value, "cm119" | "alsa"),
        "backend" => matches!(value, "hamlib" | "parallel_channel" | "parallel_rtx"),
        "dtmf_access" => matches!(value, "open" | "locked" | "disabled"),
        "dtmf_prefix" => !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()),
        "timeout_ms" => value
            .parse::<u32>()
            .is_ok_and(|value| (1..=10000).contains(&value)),
        "retries" => value.parse::<u32>().is_ok_and(|value| value <= 10),
        "timeout_retries" => value.parse::<u32>().is_ok_and(|value| value <= 100),
        "poll_interval_ms" | "dwell_ms" | "scan_slow_ms" | "scan_medium_ms" | "scan_fast_ms" => {
            value.parse::<u64>().is_ok_and(|value| value > 0)
        }
        "resume_delay_ms" | "settle_ms" => value.parse::<u64>().is_ok(),
        "tune_duration_ms" => value
            .parse::<u64>()
            .is_ok_and(|value| (1..=5000).contains(&value)),
        "mode" => mode(value).is_some(),
        "receive_vfo" | "transmit_vfo" => {
            matches!(value, "current" | "A" | "B" | "main" | "sub" | "memory")
        }
        "bandwidth_hz" => value.parse::<u32>().is_ok(),
        "offset_hz" | "rit_hz" | "xit_hz" => value.parse::<i64>().is_ok(),
        "ctcss_encode_hz" | "ctcss_decode_hz" => {
            scaled(value, 1).is_some_and(|value| (500..=3000).contains(&value))
        }
        "dcs_encode_code" | "dcs_decode_code" => value.parse::<DcsCode>().is_ok(),
        "ctcss_encode_source"
        | "ctcss_decode_source"
        | "dcs_encode_source"
        | "dcs_decode_source" => self::source(value).is_some(),
        "decode_control_source" => self::source(value).is_some(),
        "power_low_w" | "power_medium_w" | "power_high_w" | "max_power_w" | "power_w" => {
            scaled(value, 3).is_some()
        }
        _ => true,
    }
}

const BASE_DEFAULTS: &[(&str, &str)] = &[
    ("enabled", "no"),
    ("audio_source", "cm119"),
    ("dtmf_access", "locked"),
    ("dtmf_prefix", "6"),
    ("timeout_ms", "500"),
    ("retries", "2"),
    ("timeout_retries", "0"),
    ("poll_interval_ms", "500"),
    ("tune_duration_ms", "1000"),
];
const MEMORY_DEFAULTS: &[(&str, &str)] = &[
    ("mode", "FM"),
    ("bandwidth_hz", "0"),
    ("receive_vfo", "current"),
    ("transmit_vfo", "current"),
    ("offset_hz", "0"),
    ("rit_hz", "0"),
    ("xit_hz", "0"),
];
const SCAN_DEFAULTS: &[(&str, &str)] = &[
    ("dwell_ms", "500"),
    ("resume_delay_ms", "2000"),
    ("settle_ms", "0"),
];

const BASE_KEYS: &[&str] = &[
    "enabled",
    "backend",
    "hamlib_model",
    "hamlib_device",
    "audio_source",
    "startup_memory",
    "allowed_rx_ranges_hz",
    "allowed_tx_ranges_hz",
    "receive_only_ranges_hz",
    "allowed_modes",
    "max_power_w",
    "timeout_ms",
    "retries",
    "timeout_retries",
    "poll_interval_ms",
    "ptt_source",
    "cor_source",
    "dtmf_access",
    "dtmf_prefix",
    "offset_magnitude_hz",
    "power_low_w",
    "power_medium_w",
    "power_high_w",
    "scan_slow_ms",
    "scan_medium_ms",
    "scan_fast_ms",
    "tune_duration_ms",
    "ctcss_encode_hz",
    "ctcss_decode_hz",
    "ctcss_encode_source",
    "ctcss_decode_source",
    "dcs_encode_code",
    "dcs_decode_code",
    "dcs_encode_source",
    "dcs_decode_source",
    "decode_control_source",
];
const MEMORY_KEYS: &[&str] = &[
    "selector",
    "rx_frequency_hz",
    "tx_frequency_hz",
    "mode",
    "power_w",
    "bandwidth_hz",
    "receive_vfo",
    "transmit_vfo",
    "split",
    "offset_hz",
    "rit_hz",
    "xit_hz",
    "rit_enabled",
    "xit_enabled",
    "channel",
    "ctcss_encode_hz",
    "ctcss_decode_hz",
    "ctcss_encode_source",
    "ctcss_decode_source",
    "dcs_encode_code",
    "dcs_decode_code",
    "dcs_encode_source",
    "dcs_decode_source",
    "decode_control_source",
];
const SCAN_KEYS: &[&str] = &[
    "selector",
    "members",
    "dwell_ms",
    "settle_ms",
    "resume_delay_ms",
    "priority",
];
