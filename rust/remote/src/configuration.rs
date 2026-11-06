//! Copy shared resolved configuration across the released operating-engine ABI.

use crate::{CallError, HamlibProvider, HamlibRig, RemoteProvider, RemoteSession, raw};
use std::{mem::size_of, sync::Arc};
use usbradioplus_core::{
    RemoteOperatingState, RemoteSignal, RemoteSignalSource, ResolvedRemoteConfiguration,
};

/// Actual capabilities assembled from the selected dependency and DSP/GPIO owners.
/// These records carry no device handle, borrowed string, or operating policy.
#[derive(Default)]
pub struct SelectedCapabilities {
    /// Per-range receive coverage, preserving each range's modes/power.
    pub receive: Vec<raw::RptAdvRemoteCoverage>,
    /// Per-range transmit coverage.
    pub transmit: Vec<raw::RptAdvRemoteCoverage>,
    /// One shifted by each supported canonical mode.
    pub modes: u64,
    /// Actually observable complete state fields.
    pub readback_fields: u64,
    /// Modes with usable selected receive qualification for scanning.
    pub scan_modes: u64,
    /// Absolute RF power can be established.
    pub power_mw: bool,
    /// Existing calibrated test tone can be stopped by this owner.
    pub manual_tune: bool,
    /// Available encoder source bits.
    pub encode_sources: u32,
    /// Actual decoder qualification source bits, excluding generic Hamlib busy.
    pub decode_sources: u32,
    /// Independent decoder programming providers.
    pub decode_control_sources: u32,
}

impl HamlibRig {
    /// Copy the shared resolver's symbolic model, named transport options and
    /// finite retry settings into the dependency adapter on this physical owner.
    pub fn configured(
        provider: Arc<HamlibProvider>,
        config: &ResolvedRemoteConfiguration,
    ) -> Result<Self, CallError> {
        let span = |value: &str| raw::rptadv_hamlib_string {
            data: value.as_ptr().cast(),
            size: value.len(),
        };
        let options: Vec<_> = config
            .hamlib_options
            .iter()
            .map(|(name, value)| raw::rptadv_hamlib_option {
                name: span(name),
                value: span(value),
            })
            .collect();
        let setup = raw::rptadv_hamlib_config {
            struct_size: size_of::<raw::rptadv_hamlib_config>(),
            abi_version: 1,
            reserved: 0,
            model: span(&config.hamlib_model),
            options: options.as_ptr(),
            option_count: options.len(),
            timeout_ms: config.timeout_ms,
            retries: config.retries,
            timeout_retries: config.timeout_retries,
            reserved2: 0,
        };
        // SAFETY: every borrowed string/option remains readable through open;
        // the dependency copies its configuration and retains no client pointers.
        unsafe { Self::open(provider, &setup) }
    }
}

fn mode_mask(modes: &[u32]) -> Result<u64, CallError> {
    modes.iter().try_fold(0, |mask, mode| {
        1_u64
            .checked_shl(*mode)
            .map(|bit| mask | bit)
            .ok_or(CallError(-4))
    })
}

fn source(source: RemoteSignalSource, available: u32, enabled: bool) -> Result<u32, CallError> {
    if source != RemoteSignalSource::Automatic {
        return Ok(source as u32);
    }
    [3, 1, 2]
        .into_iter()
        .find(|source| available & (1 << source) != 0)
        .or_else(|| (!enabled).then_some(0))
        .ok_or(CallError(-6))
}

fn signal(
    value: RemoteSignal,
    available: u32,
    control: u32,
) -> Result<raw::RptAdvRemoteSignal, CallError> {
    Ok(raw::RptAdvRemoteSignal {
        source: source(value.source, available, value.enabled)?,
        control_source: value
            .control_source
            .map(|selected| source(selected, control, value.enabled))
            .transpose()?
            .unwrap_or(0),
        enabled: u32::from(value.enabled),
        ctcss_tenths_hz: value.tone_tenths_hz,
        dcs_code: value.dcs_code,
        inverted: u32::from(value.inverted),
    })
}

/// Convert a complete resolved live state; automatic choices are made only
/// against admitted capabilities, never as a runtime fallback after failure.
pub fn operating_state(
    value: &RemoteOperatingState,
    caps: &SelectedCapabilities,
) -> Result<raw::RptAdvRemoteState, CallError> {
    Ok(raw::RptAdvRemoteState {
        receive_hz: value.receive_hz,
        transmit_hz: value.transmit_hz.unwrap_or(0),
        offset_hz: value.offset_hz,
        rit_hz: value.rit_hz,
        xit_hz: value.xit_hz,
        flags: u32::from(value.transmit_hz.is_some())
            | (u32::from(value.power_mw.is_some()) << 1)
            | (u32::from(value.channel.is_some()) << 2)
            | (u32::from(value.split) << 3)
            | (u32::from(value.rit_enabled) << 4)
            | (u32::from(value.xit_enabled) << 5),
        mode: value.mode,
        bandwidth_hz: value.bandwidth_hz,
        receive_vfo: value.receive_vfo,
        transmit_vfo: value.transmit_vfo,
        power_mw: value.power_mw.unwrap_or(0),
        channel: u32::from(value.channel.unwrap_or(0)),
        encode: signal(value.encode, caps.encode_sources, caps.encode_sources)?,
        decode: signal(
            value.decode,
            caps.decode_sources,
            caps.decode_control_sources,
        )?,
    })
}

/// Copy a validated engine state into the existing DSP configuration model.
/// Source selection has already completed; automatic providers cannot reappear.
pub fn operating_configuration(
    value: &raw::RptAdvRemoteState,
) -> Result<RemoteOperatingState, CallError> {
    let source = |value| match value {
        0 => Ok(RemoteSignalSource::Disabled),
        1 => Ok(RemoteSignalSource::Dsp),
        2 => Ok(RemoteSignalSource::External),
        3 => Ok(RemoteSignalSource::Rig),
        _ => Err(CallError(-1003)),
    };
    let signal = |signal: raw::RptAdvRemoteSignal| {
        if signal.enabled > 1 || signal.inverted > 1 {
            return Err(CallError(-1003));
        }
        Ok(RemoteSignal {
            source: source(signal.source)?,
            control_source: (signal.control_source != 0)
                .then(|| source(signal.control_source))
                .transpose()?,
            enabled: signal.enabled != 0,
            tone_tenths_hz: signal.ctcss_tenths_hz,
            dcs_code: signal.dcs_code,
            inverted: signal.inverted != 0,
        })
    };
    if value.flags & !63 != 0 || value.flags & 4 != 0 && value.channel > 15 {
        return Err(CallError(-1003));
    }
    Ok(RemoteOperatingState {
        receive_hz: value.receive_hz,
        transmit_hz: (value.flags & 1 != 0).then_some(value.transmit_hz),
        power_mw: (value.flags & 2 != 0).then_some(value.power_mw),
        channel: (value.flags & 4 != 0).then_some(value.channel as u8),
        split: value.flags & 8 != 0,
        mode: value.mode,
        bandwidth_hz: value.bandwidth_hz,
        receive_vfo: value.receive_vfo,
        transmit_vfo: value.transmit_vfo,
        offset_hz: value.offset_hz,
        rit_hz: value.rit_hz,
        xit_hz: value.xit_hz,
        rit_enabled: value.flags & 16 != 0,
        xit_enabled: value.flags & 32 != 0,
        encode: signal(value.encode)?,
        decode: signal(value.decode)?,
    })
}

fn ranges(values: &[(u64, u64)]) -> Vec<raw::RptAdvRemoteRange> {
    values
        .iter()
        .map(|(minimum, maximum)| raw::RptAdvRemoteRange {
            minimum: *minimum,
            maximum: *maximum,
        })
        .collect()
}

impl RemoteSession {
    /// Validate configuration-only RF policy before a host releases its current
    /// radio. These permissive synthetic capabilities establish no hardware fact;
    /// the physical owner must still admit actual capabilities before activation.
    pub fn validate_policy(
        provider: Arc<RemoteProvider>,
        config: &ResolvedRemoteConfiguration,
    ) -> Result<(), CallError> {
        let mut policy = config.clone();
        for memory in &mut policy.memories {
            for signal in [&mut memory.state.encode, &mut memory.state.decode] {
                // An automatic provider is unresolved here. DSP avoids assuming
                // a particular rig's signaling limits during pure validation.
                if signal.source == RemoteSignalSource::Automatic {
                    signal.source = RemoteSignalSource::Dsp;
                }
                if signal.control_source == Some(RemoteSignalSource::Automatic) {
                    signal.control_source = Some(RemoteSignalSource::Dsp);
                }
            }
        }
        let coverage = raw::RptAdvRemoteCoverage {
            minimum: 1,
            maximum: u64::MAX,
            mode_mask: 0x1ffe,
            flags: 0,
            minimum_power_mw: 0,
            maximum_power_mw: 0,
        };
        let caps = SelectedCapabilities {
            receive: vec![coverage],
            transmit: vec![coverage],
            modes: 0x1ffe,
            scan_modes: 0x1ffe,
            power_mw: true,
            manual_tune: true,
            encode_sources: 14,
            decode_sources: 14,
            decode_control_sources: 14,
            ..Default::default()
        };
        Self::configured(provider, &policy, &caps).map(drop)
    }

    /// Copy the existing shared resolver's definitions into the dynamic engine.
    /// The engine alone validates RF policy, memories and scan membership.
    pub fn configured(
        provider: Arc<RemoteProvider>,
        config: &ResolvedRemoteConfiguration,
        caps: &SelectedCapabilities,
    ) -> Result<Self, CallError> {
        let receive = ranges(&config.receive_ranges);
        let transmit = ranges(&config.transmit_ranges);
        let receive_only = ranges(&config.receive_only_ranges);
        let memories = config
            .memories
            .iter()
            .map(|memory| {
                Ok(raw::RptAdvRemoteMemory {
                    selector: memory.selector,
                    name: memory.name.as_ptr().cast(),
                    name_length: memory.name.len(),
                    state: operating_state(&memory.state, caps)?,
                })
            })
            .collect::<Result<Vec<_>, CallError>>()?;
        let scans: Vec<_> = config
            .scan_lists
            .iter()
            .map(|scan| raw::RptAdvRemoteScan {
                selector: scan.selector,
                name: scan.name.as_ptr().cast(),
                name_length: scan.name.len(),
                members: scan.members.as_ptr(),
                member_count: scan.members.len(),
                dwell_ms: scan.dwell_ms,
                resume_delay_ms: scan.resume_delay_ms,
                settle_ms: scan.settle_ms,
                priority: scan.priority.unwrap_or(0),
            })
            .collect();
        let setup = raw::RptAdvRemoteSetup {
            struct_size: size_of::<raw::RptAdvRemoteSetup>() as u32,
            abi_version: 1,
            limits: raw::RptAdvRemoteLimits {
                receive: raw::RptAdvRemoteRanges {
                    data: receive.as_ptr(),
                    length: receive.len(),
                },
                transmit: raw::RptAdvRemoteRanges {
                    data: transmit.as_ptr(),
                    length: transmit.len(),
                },
                receive_only: raw::RptAdvRemoteRanges {
                    data: receive_only.as_ptr(),
                    length: receive_only.len(),
                },
                mode_mask: mode_mask(&config.modes)?,
                flags: u32::from(config.maximum_power_mw.is_some()) << 1,
                maximum_power_mw: config.maximum_power_mw.unwrap_or(0),
            },
            capabilities: raw::RptAdvRemoteCapabilities {
                receive: raw::RptAdvRemoteCoverages {
                    data: caps.receive.as_ptr(),
                    length: caps.receive.len(),
                },
                transmit: raw::RptAdvRemoteCoverages {
                    data: caps.transmit.as_ptr(),
                    length: caps.transmit.len(),
                },
                mode_mask: caps.modes,
                readback_fields: caps.readback_fields,
                scan_mode_mask: caps.scan_modes,
                flags: (u32::from(caps.power_mw) << 1) | (u32::from(caps.manual_tune) << 3),
                encode_sources: caps.encode_sources,
                decode_sources: caps.decode_sources,
                decode_control_sources: caps.decode_control_sources,
            },
            memories: raw::RptAdvRemoteMemories {
                data: memories.as_ptr(),
                length: memories.len(),
            },
            scan_lists: raw::RptAdvRemoteScans {
                data: scans.as_ptr(),
                length: scans.len(),
            },
            startup_memory: config.startup_memory,
            poll_interval_ms: config.poll_interval_ms,
        };
        // SAFETY: every array and UTF-8 name is retained through the synchronous
        // create; the dynamic engine copies them and retains no host pointers.
        unsafe { Self::open(provider, &setup) }
    }
}
