//! Typed provider DTO decoded on the control owner. Role presence is independent of stage enablement.

use crate::native_processing::Profiles;
use serde::Deserialize;
use usbradioplus_core::*;

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub(super) struct Settings {
    station: Station,
    local: Option<Role>,
    link: Option<Role>,
    voice_telemetry: Option<Role>,
    pub(super) remote: Option<ResolvedRemoteConfiguration>,
}

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Station {
    channel_enabled: Option<bool>,
    eeprom_enabled: bool,
    voter_reporting: u32,
    gpio_usb_port_path: String,
    parallel_port: ParallelPortConfig,
    receive: ReceiveMetadata,
    transmit: TransmitMetadata,
    ctcss: Option<CtcssConfig>,
    dcs: Option<DcsConfig>,
    duplex: DuplexMetadata,
    diagnostics: DiagnosticsConfig,
}

// The released radio template already owns squelch, VAD, qualification and
// transmitter timing. Only station-only metadata is accepted here.
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct ReceiveMetadata {
    frequency_hz: u32,
    squelch_delay_ms: u16,
}
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct TransmitMetadata {
    frequency_hz: u32,
}
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct DuplexMetadata {
    local_repeat_level: u16,
}

// Group members are the provider's actual typed DSP settings. Omitting a role
// retains the legacy absence behavior. An empty role selects its shipped chain.
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Role {
    enabled: Option<bool>,
    rnnoise_enabled: Option<bool>,
    input_gain_db: Option<f64>,
    output_gain_db: Option<f64>,
    stage_order: Option<Vec<ProcessingStage>>,
    receive: Option<RoleReceive>,
    equalizer: Option<Equalizer>,
    deesser: Option<Deesser>,
    agc: Option<Agc>,
    expander: Option<Expander>,
    compressor: Option<Compressor>,
    limiter: Option<Limiter>,
    peak_limiter: Option<PeakLimiter>,
    transmit_tail: Option<TransmitTail>,
}

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct RoleReceive {
    bandpass_enabled: Option<bool>,
    bandpass_highpass_hz: Option<f64>,
    bandpass_lowpass_hz: Option<f64>,
    pl_filter: Option<PlFilter>,
    notch_width_hz: Option<f64>,
    highpass_hz: Option<f64>,
}

// Preserve the selected role's shipped values for every omitted field.
macro_rules! overlay {
    ($target:expr, $source:expr, $($field:ident),+ $(,)?) => {
        $(if let Some(value) = $source.$field { $target.$field = value; })+
    };
}

impl Role {
    fn chain(self, role: ChainRole) -> Result<ProcessingChain, &'static str> {
        let mut chain = ProcessingChain::shipped(role);
        if let Some(receive) = self.receive {
            overlay!(
                chain.receive,
                receive,
                bandpass_enabled,
                bandpass_highpass_hz,
                bandpass_lowpass_hz,
                pl_filter,
                notch_width_hz,
                highpass_hz
            );
        }
        overlay!(
            chain,
            self,
            enabled,
            rnnoise_enabled,
            input_gain_db,
            output_gain_db,
            equalizer,
            deesser,
            agc,
            expander,
            compressor,
            limiter,
            peak_limiter,
            transmit_tail
        );
        if let Some(stages) = self.stage_order {
            chain.stage_order = StageOrder::from_stages(stages).map_err(|_| "stage_order")?;
        }
        chain.validate().map_err(|e| match e {
            ProcessingConfigError::Range { field, .. } => field,
            ProcessingConfigError::Ordering { upper, .. } => upper,
            ProcessingConfigError::StageOrder(_) => "stage_order",
            ProcessingConfigError::StageNotAllowed(_) => "role",
            ProcessingConfigError::InvalidOversample => "oversample",
        })?;
        Ok(chain)
    }
}

impl Station {
    fn resolve(self) -> Result<StationConfig, &'static str> {
        if self.voter_reporting > i32::MAX as u32 {
            return Err("voter_reporting");
        }
        if self.gpio_usb_port_path.contains('\0') {
            return Err("gpio_usb_port_path");
        }
        if self.parallel_port.device.contains('\0') || self.parallel_port.base_address == 0 {
            return Err("parallel_port");
        }
        let rx = &self.receive;
        let tx = &self.transmit;
        if rx.squelch_delay_ms > 511 {
            return Err("squelch_delay_ms");
        }
        if rx.frequency_hz > i32::MAX as u32 || tx.frequency_hz > i32::MAX as u32 {
            return Err("frequency_hz");
        }
        if self.duplex.local_repeat_level > 999 {
            return Err("local_repeat_level");
        }
        if self.diagnostics.status_publication_interval_ms == 0 {
            return Err("status_publication_interval_ms");
        }
        let ctcss = self.ctcss.unwrap_or_default();
        for frequencies in [&ctcss.receive_frequencies, &ctcss.transmit_frequencies] {
            if frequencies.is_empty() || frequencies.len() > CtcssTone::supported().len() {
                return Err("ctcss_frequencies");
            }
        }
        if ctcss.receive_relax > 1 {
            return Err("receive_relax");
        }
        if !(-90.0..=0.0).contains(&ctcss.transmit_peak_dbfs) {
            return Err("transmit_peak_dbfs");
        }
        if ctcss.phase_shift_degrees <= 0.0 {
            return Err("phase_shift_degrees");
        }
        if !(40..=32767).contains(&ctcss.tail_duration_ms) {
            return Err("tail_duration_ms");
        }
        if ctcss.tail_frequency_hz <= 0.0 {
            return Err("tail_frequency_hz");
        }
        let dcs = self.dcs.unwrap_or_default();
        if dcs.receive_code.value() > 511 || dcs.transmit_code.value() > 511 {
            return Err("dcs_code");
        }
        if !(150..=200).contains(&dcs.turnoff_duration_ms) {
            return Err("turnoff_duration_ms");
        }
        if !(-90.0..=0.0).contains(&dcs.peak_dbfs) {
            return Err("peak_dbfs");
        }
        Ok(StationConfig {
            channel_enabled: self.channel_enabled.unwrap_or(true),
            hardware: HardwareConfig {
                eeprom_enabled: self.eeprom_enabled,
                voter_reporting: self.voter_reporting,
                gpio_usb_port_path: self.gpio_usb_port_path,
                parallel_port: self.parallel_port,
                ..Default::default()
            },
            receive: ReceiveConfig {
                frequency_hz: rx.frequency_hz,
                squelch_delay_ms: rx.squelch_delay_ms,
                ..Default::default()
            },
            transmit: TransmitConfig {
                frequency_hz: tx.frequency_hz,
                ..Default::default()
            },
            ctcss,
            dcs,
            duplex: DuplexConfig {
                local_repeat_level: self.duplex.local_repeat_level,
                ..Default::default()
            },
            diagnostics: self.diagnostics,
            ..Default::default()
        })
    }
}

pub(super) struct Semantic {
    pub(super) ctcss_supplied: bool,
    pub(super) dcs_supplied: bool,
    pub(super) channel: ChannelConfiguration,
    pub(super) profiles: Profiles,
    pub(super) remote: Option<ResolvedRemoteConfiguration>,
}

/// Maximum size of either provider settings record in bytes.
pub(super) const MAX_SETTINGS_BYTES: u32 = 1_048_576;

pub(super) fn parse<T: serde::de::DeserializeOwned>(text: &str) -> Result<T, &'static str> {
    // Struct Serde implementations also permit positional sequences. A DTO
    // record always has named members, including records inside memory lists.
    let shape: serde_json::Value = serde_json::from_str(text).map_err(|_| "settings")?;
    if !shape.is_object() || !valid_shape(&shape, "") {
        return Err("settings");
    }
    serde_json::from_str(text).map_err(|_| "settings")
}

/// Only the link role is meaningful without a station.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct LinkSettings {
    link: Option<Role>,
}

impl LinkSettings {
    pub(super) fn resolve(self) -> Result<Option<ProcessingChain>, &'static str> {
        self.link
            .map(|role| role.chain(ChainRole::Link))
            .transpose()
    }
}

impl Settings {
    pub(super) fn resolve(self, raw_output_gain: i32) -> Result<Semantic, &'static str> {
        let profiles = Profiles {
            local: self
                .local
                .map(|s| s.chain(ChainRole::LocalReceive))
                .transpose()?,
            link: self.link.map(|s| s.chain(ChainRole::Link)).transpose()?,
            voice_telemetry: self
                .voice_telemetry
                .map(|s| s.chain(ChainRole::VoiceTelemetry))
                .transpose()?,
            warnings: Vec::new(),
        };
        let ctcss_supplied = self.station.ctcss.is_some();
        let dcs_supplied = self.station.dcs.is_some();
        let channel = ChannelConfiguration {
            station: self.station.resolve()?,
            local: profiles.local.clone().unwrap_or_else(|| {
                let mut chain = ProcessingChain::shipped(ChainRole::LocalReceive);
                chain.output_gain_db = f64::from(raw_output_gain);
                chain
            }),
            link: profiles
                .link
                .clone()
                .unwrap_or_else(|| ProcessingChain::shipped(ChainRole::Link)),
            voice_telemetry: profiles
                .voice_telemetry
                .clone()
                .unwrap_or_else(|| ProcessingChain::shipped(ChainRole::VoiceTelemetry)),
        };
        if let Some(remote) = &self.remote {
            validate_remote(remote)?;
        }
        Ok(Semantic {
            ctcss_supplied,
            dcs_supplied,
            channel,
            profiles,
            remote: self.remote,
        })
    }
}

fn valid_shape(value: &serde_json::Value, key: &str) -> bool {
    use serde_json::Value;
    if value.is_null() {
        // Optional remote operating values distinguish "unset" from a request.
        // Pin/preset array slots are checked by their typed array deserializers.
        return matches!(
            key,
            "remote"
                | "ptt_source"
                | "cor_source"
                | "external_encode_output"
                | "transmit_hz"
                | "maximum_power_mw"
                | "offset_magnitude_hz"
                | "power_mw"
                | "channel"
                | "priority"
                | "control_source"
        );
    }
    if key == "encode" && value.get("control_source").is_some() {
        return false;
    }
    if matches!(key, "local" | "link" | "voice_telemetry") && !value.is_object() {
        return false;
    }
    if key == "hamlib_options" {
        return value
            .as_object()
            .is_some_and(|map| map.values().all(Value::is_string));
    }
    if matches!(
        key,
        "polarity"
            | "pl_filter"
            | "layout"
            | "source"
            | "control_source"
            | "backend"
            | "audio_source"
            | "dtmf_access"
    ) {
        return value.is_string();
    }
    match value {
        Value::Array(items) => match key {
            "memories" | "scan_lists" => items
                .iter()
                .all(|item| item.is_object() && valid_shape(item, "")),
            "receive_ranges" | "transmit_ranges" | "receive_only_ranges" => {
                items.iter().all(|item| {
                    item.as_array()
                        .is_some_and(|pair| pair.len() == 2 && pair.iter().all(Value::is_u64))
                })
            }
            "stage_order"
            | "modes"
            | "receive_frequencies"
            | "transmit_frequencies"
            | "members"
            | "power_presets_mw"
            | "scan_presets_ms"
            | "output_assignments"
            | "input_assignments" => items
                .iter()
                .all(|item| !item.is_array() && !item.is_object()),
            _ => false,
        },
        Value::Object(members) => members.iter().all(|(key, value)| valid_shape(value, key)),
        _ => true,
    }
}

fn validate_remote(r: &ResolvedRemoteConfiguration) -> Result<(), &'static str> {
    if r.backend == RemoteBackend::Hamlib
        && (!r.hamlib_model.starts_with("RIG_MODEL_")
            || r.hamlib_model.len() <= 10
            || !r
                .hamlib_model
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_'))
    {
        return Err("hamlib_model");
    }
    if r.hamlib_options.iter().any(|(k, v)| {
        k.is_empty()
            || k.contains('\0')
            || v.contains('\0')
            || matches!(k.as_str(), "timeout" | "retry" | "timeout_retry")
    }) {
        return Err("hamlib_options");
    }
    if !(1..=10000).contains(&r.timeout_ms) {
        return Err("timeout_ms");
    }
    if r.retries > 10 {
        return Err("retries");
    }
    if r.timeout_retries > 100 {
        return Err("timeout_retries");
    }
    if r.poll_interval_ms == 0 {
        return Err("poll_interval_ms");
    }
    if !(1..=5000).contains(&r.tune_duration_ms) {
        return Err("tune_duration_ms");
    }
    if !(1..=63).contains(&r.dtmf_prefix.len())
        || !r.dtmf_prefix.bytes().all(|b| b.is_ascii_digit())
    {
        return Err("dtmf_prefix");
    }
    if r.ptt_source
        .as_deref()
        .is_some_and(|s| !matches!(s, "hamlib" | "usb" | "parallel"))
    {
        return Err("ptt_source");
    }
    if r.cor_source
        .as_deref()
        .is_some_and(|s| !matches!(s, "hamlib" | "usb" | "parallel" | "open" | "dsp"))
    {
        return Err("cor_source");
    }
    if r.scan_presets_ms.contains(&Some(0)) {
        return Err("scan_presets_ms");
    }
    if r.receive_ranges.is_empty()
        || r.receive_ranges
            .iter()
            .chain(&r.transmit_ranges)
            .chain(&r.receive_only_ranges)
            .any(|&(a, b)| a == 0 || a > b)
    {
        return Err("ranges");
    }
    if r.modes.is_empty() || r.modes.iter().any(|m| !(1..=12).contains(m)) {
        return Err("modes");
    }
    for m in &r.memories {
        let s = &m.state;
        if m.selector == 0
            || m.name.is_empty()
            || m.name.contains('\0')
            || s.receive_hz == 0
            || s.channel.is_some_and(|c| c > 15)
            || !(1..=12).contains(&s.mode)
            || s.receive_vfo > 5
            || s.transmit_vfo > 5
        {
            return Err("memories");
        }
        for signal in [s.encode, s.decode] {
            if signal.dcs_code > 511
                || signal.tone_tenths_hz != 0
                    && (!(500..=3000).contains(&signal.tone_tenths_hz) || signal.dcs_code != 0)
            {
                return Err("signal");
            }
        }
    }
    for s in &r.scan_lists {
        if s.name.is_empty()
            || s.name.contains('\0')
            || s.selector == 0
            || s.members.is_empty()
            || s.dwell_ms == 0
            || s.dwell_ms.checked_add(s.settle_ms).is_none()
            || s.priority.is_some_and(|p| !s.members.contains(&p))
        {
            return Err("scan_lists");
        }
    }
    Ok(())
}
