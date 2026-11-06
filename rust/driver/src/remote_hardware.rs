//! Explicit signal ownership selected once from actual physical capabilities.

use crate::RemoteRequestError;
use usbradioplus_core::{
    CarrierSource, RemoteBackend, RemoteEncoderOutput, ResolvedRemoteConfiguration,
};
use usbradioplus_radio::SubaudibleSource;
use usbradioplus_remote::Setting;
use usbradioplus_station::HardwarePlan;

/// One physical PTT provider for this stream generation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RemotePttSource {
    /// The selected rig's supported PTT operation.
    Rig,
    /// The selected CM119's dedicated PTT output.
    Usb,
    /// A configured parallel-port PTT assignment.
    Parallel,
}

/// Worker-local output routing. Audio only publishes demand; a remote owner
/// services one selected output, keeping every other dedicated PTT inactive.
#[derive(Clone, Copy)]
pub(crate) enum PhysicalPttControl {
    Ordinary,
    Remote {
        selected: Option<RemotePttSource>,
        key: bool,
    },
}
impl PhysicalPttControl {
    pub(crate) fn key_for(self, source: RemotePttSource, rendered: bool) -> bool {
        match self {
            Self::Ordinary => rendered,
            Self::Remote { selected, key } => selected == Some(source) && key,
        }
    }
}

/// Effective signal ownership. A failed observation never changes these choices.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RemotePhysicalWiring {
    pub(crate) external_encode: Option<ExternalEncoder>,
    /// The one provider permitted to drive PTT.
    pub ptt: RemotePttSource,
    /// Effective receive carrier input, including configured GPIO polarity.
    pub carrier: CarrierSource,
    /// The external carrier snapshot comes from Hamlib's actual DCD getter.
    pub rig_carrier: bool,
    /// Actual selected external tone qualifier, independent of rig DCD.
    pub external_decode: SubaudibleSource,
}

/// One admitted output on this radio's existing GPIO owner.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ExternalEncoder {
    pub bank: crate::SiteBank,
    pub mask: u8,
    pub inverted: bool,
}
impl ExternalEncoder {
    pub(crate) fn configured(config: &ResolvedRemoteConfiguration) -> Option<Self> {
        let (bank, mask) = match config.external_encode_output? {
            RemoteEncoderOutput::Usb(pin) => (crate::SiteBank::Cm119, 1 << (pin.get() - 1)),
            RemoteEncoderOutput::Parallel(pin) => (crate::SiteBank::Parallel, 1 << (pin.get() - 2)),
        };
        Some(Self {
            bank,
            mask,
            inverted: config.external_encode_inverted,
        })
    }
}

/// Inspect existing pin capabilities without opening a device or claiming I/O.
pub(crate) fn external_encoder(
    config: &ResolvedRemoteConfiguration,
    plan: &HardwarePlan,
) -> Result<Option<ExternalEncoder>, RemoteRequestError> {
    let unavailable = || RemoteRequestError::Rejected(-6);
    let Some(output) = ExternalEncoder::configured(config) else {
        return Ok(None);
    };
    let mask = output.mask;
    match output.bank {
        crate::SiteBank::Cm119 => {
            let plan = plan.cm119.as_ref().ok_or_else(unavailable)?;
            if plan.output_enable_mask & mask == 0 || plan.clip_led_mask & mask != 0 {
                return Err(unavailable());
            }
        }
        crate::SiteBank::Parallel => {
            let plan = plan.parallel.as_ref().ok_or_else(unavailable)?;
            let reserved = match config.backend {
                RemoteBackend::ParallelChannel => 0xf0,
                RemoteBackend::ParallelRtx => 0x1f,
                RemoteBackend::Hamlib => 0,
            };
            if plan.config.output_enable_mask & mask == 0 || (plan.ptt_mask | reserved) & mask != 0
            {
                return Err(unavailable());
            }
        }
    }
    Ok(Some(output))
}

/// Register representability only, never a claim about a radio's RF coverage.
/// RX selects the existing RTX encoder's reference and grid for both words.
pub(crate) fn rtx_frequencies_representable(receive: u64, transmit: u64) -> bool {
    let (step, intermediate) = if receive > 200_000_000 {
        (12_500, 21_400_000)
    } else {
        (5_000, 10_700_000)
    };
    receive
        .checked_sub(intermediate)
        .is_some_and(|value| value % step == 0 && value / step <= 262_143)
        && transmit % step == 0
        && transmit / step <= 262_143
}

impl RemotePhysicalWiring {
    /// Select once against actual admitted settings and hardware assignments.
    /// Missing explicit providers reject setup; they never acquire a fallback.
    pub fn select(
        config: &ResolvedRemoteConfiguration,
        plan: &HardwarePlan,
        inherited_carrier: CarrierSource,
        inherited_decode: SubaudibleSource,
        settings: &[Setting],
    ) -> Result<Self, RemoteRequestError> {
        let external_encode = external_encoder(config, plan)?;
        let unavailable = || RemoteRequestError::Rejected(-6);
        let rig_ptt = settings
            .iter()
            .any(|setting| setting.selector == 20 && setting.writable && setting.readable);
        let rig_carrier = settings
            .iter()
            .any(|setting| setting.selector == 21 && setting.readable);
        let usb = plan.cm119.is_some();
        let parallel_ptt = plan
            .parallel
            .as_ref()
            .is_some_and(|plan| plan.ptt_mask != 0);
        let ptt = match config.ptt_source.as_deref() {
            Some("hamlib") if rig_ptt => RemotePttSource::Rig,
            Some("usb") if usb => RemotePttSource::Usb,
            Some("parallel") if parallel_ptt => RemotePttSource::Parallel,
            None if rig_ptt => RemotePttSource::Rig,
            None if usb => RemotePttSource::Usb,
            None if parallel_ptt => RemotePttSource::Parallel,
            _ => return Err(unavailable()),
        };
        if config.backend != RemoteBackend::Hamlib {
            let parallel = plan.parallel.as_ref().ok_or_else(unavailable)?;
            let owned = if config.backend == RemoteBackend::ParallelChannel {
                0xf0
            } else {
                0x1f
            };
            if parallel.config.output_enable_mask & owned != owned
                || config.backend == RemoteBackend::ParallelChannel
                    && parallel.ptt_mask & owned != 0
                || config.backend == RemoteBackend::ParallelRtx
                    && (ptt != RemotePttSource::Parallel
                        || parallel.ptt_mask != 8
                        || plan.ptt_inverted)
            {
                return Err(unavailable());
            }
            for memory in &config.memories {
                let state = &memory.state;
                if config.backend == RemoteBackend::ParallelChannel {
                    if state.channel.is_none_or(|channel| channel > 15) {
                        return Err(unavailable());
                    }
                } else {
                    let transmit = state
                        .transmit_hz
                        .map_or(Some(0), |value| value.checked_add_signed(state.offset_hz))
                        .ok_or_else(unavailable)?;
                    if !rtx_frequencies_representable(state.receive_hz, transmit) {
                        return Err(unavailable());
                    }
                }
            }
        }
        let (carrier, rig_carrier) = match config.cor_source.as_deref() {
            Some("hamlib") | None if rig_carrier => (CarrierSource::External, true),
            Some("open") => (CarrierSource::Open, false),
            Some("dsp") => (CarrierSource::Dsp, false),
            Some("usb") => (
                if inherited_carrier == CarrierSource::UsbInverted {
                    CarrierSource::UsbInverted
                } else {
                    CarrierSource::Usb
                },
                false,
            ),
            Some("parallel") => (
                if inherited_carrier == CarrierSource::ParallelInverted {
                    CarrierSource::ParallelInverted
                } else {
                    CarrierSource::Parallel
                },
                false,
            ),
            None => (inherited_carrier, false),
            _ => return Err(unavailable()),
        };
        if matches!(carrier, CarrierSource::Usb | CarrierSource::UsbInverted) && !usb
            || matches!(
                carrier,
                CarrierSource::Parallel | CarrierSource::ParallelInverted
            ) && plan
                .parallel
                .as_ref()
                .is_none_or(|plan| plan.carrier_mask == 0)
            || carrier == CarrierSource::External && !rig_carrier
        {
            return Err(unavailable());
        }
        let external_decode = match inherited_decode {
            SubaudibleSource::Usb | SubaudibleSource::UsbInverted if usb => inherited_decode,
            SubaudibleSource::Parallel | SubaudibleSource::ParallelInverted
                if plan
                    .parallel
                    .as_ref()
                    .is_some_and(|plan| plan.ctcss_mask != 0) =>
            {
                inherited_decode
            }
            _ => SubaudibleSource::Disabled,
        };
        Ok(Self {
            external_encode,
            ptt,
            carrier,
            rig_carrier,
            external_decode,
        })
    }
}

#[cfg(test)]
#[path = "tests/remote_hardware_tests.rs"]
mod tests;
