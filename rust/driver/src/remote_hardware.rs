//! Explicit signal ownership selected once from actual physical capabilities.

use crate::RemoteRequestError;
use usbradioplus_core::{CarrierSource, RemoteBackend, ResolvedRemoteConfiguration};
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
    /// The one provider permitted to drive PTT.
    pub ptt: RemotePttSource,
    /// Effective receive carrier input, including configured GPIO polarity.
    pub carrier: CarrierSource,
    /// The external carrier snapshot comes from Hamlib's actual DCD getter.
    pub rig_carrier: bool,
    /// Actual selected external tone qualifier, independent of rig DCD.
    pub external_decode: SubaudibleSource,
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
            ptt,
            carrier,
            rig_carrier,
            external_decode,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::remote_request::tests::configuration;
    use usbradioplus_core::CarrierSource;
    use usbradioplus_radio::SubaudibleSource;
    fn plan() -> HardwarePlan {
        let config = crate::DriverConfiguration::parse("test", "[usb]\n").unwrap();
        usbradioplus_station::hardware_plan(config.channel("usb").unwrap().config())
    }

    #[test]
    fn remote_parallel_setup_checks_owned_pins_and_exact_register_grid() {
        use usbradioplus_core::RemoteBackend;
        let mut source = String::from("[usb]\n[hardware]\n");
        for pin in 2..=9 {
            source.push_str(&format!(
                "hardware_parallel_pin_{pin}_assignment={}\n",
                if pin == 5 { "ptt" } else { "out0" }
            ));
        }
        let station = crate::DriverConfiguration::parse("test", &source).unwrap();
        let plan = usbradioplus_station::hardware_plan(station.channel("usb").unwrap().config());
        let mut config = configuration();
        config.ptt_source = Some("parallel".into());
        config.backend = RemoteBackend::ParallelRtx;
        let select = |config: &ResolvedRemoteConfiguration, plan: &HardwarePlan| {
            RemotePhysicalWiring::select(
                config,
                plan,
                CarrierSource::Dsp,
                SubaudibleSource::Dsp,
                &[],
            )
        };
        assert!(
            select(&config, &plan).is_ok(),
            "{:?} {:?} {:?}",
            select(&config, &plan),
            plan,
            config.cor_source
        );
        config.memories[0].state.receive_hz += 1;
        assert!(
            select(&config, &plan).is_err(),
            "off-grid RTX must not be acknowledged"
        );
        config.memories[0].state.receive_hz -= 1;
        for (mask, ptt, inverted) in [(0x1e, 8, false), (0xff, 4, false), (0xff, 8, true)] {
            let mut invalid = plan.clone();
            invalid.parallel.as_mut().unwrap().config.output_enable_mask = mask;
            invalid.parallel.as_mut().unwrap().ptt_mask = ptt;
            invalid.ptt_inverted = inverted;
            assert!(select(&config, &invalid).is_err());
        }
        config.backend = RemoteBackend::ParallelChannel;
        config.memories[0].state.channel = Some(15);
        assert!(select(&config, &plan).is_ok());
        config.memories[0].state.channel = Some(16);
        assert!(select(&config, &plan).is_err());
        config.memories[0].state.channel = Some(15);
        let mut conflict = plan.clone();
        conflict.parallel.as_mut().unwrap().ptt_mask = 0x80;
        assert!(select(&config, &conflict).is_err());
    }

    fn setting(selector: u32, readable: bool, writable: bool) -> usbradioplus_remote::Setting {
        usbradioplus_remote::Setting {
            selector,
            name: "test".into(),
            readable,
            writable,
            value_kind: 1,
            units: 0,
            decimal_places: 0,
            minimum: 0,
            maximum: 1,
            choices: vec![],
        }
    }

    #[test]
    fn remote_wiring_prefers_supported_rig_signals_without_runtime_fallback() {
        let config = configuration();
        let mut plan = plan();
        let settings = [setting(20, true, true), setting(21, true, false)];
        let wiring = RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Dsp,
            &settings,
        )
        .unwrap();
        assert_eq!(wiring.ptt, RemotePttSource::Rig);
        assert_eq!(wiring.carrier, CarrierSource::External);
        assert!(wiring.rig_carrier);
        plan.cm119 = None;
        let wiring = RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &settings,
        )
        .unwrap();
        assert_eq!(wiring.ptt, RemotePttSource::Rig);
        assert!(
            RemotePhysicalWiring::select(
                &config,
                &plan,
                CarrierSource::Dsp,
                SubaudibleSource::Disabled,
                &[]
            )
            .is_err()
        );
    }

    #[test]
    fn explicit_open_receive_is_not_a_disabled_carrier_source() {
        let mut config = configuration();
        config.cor_source = Some("open".into());
        let wiring = RemotePhysicalWiring::select(
            &config,
            &plan(),
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &[],
        )
        .unwrap();
        assert_eq!(wiring.carrier.to_string(), "open");
        assert!(!wiring.rig_carrier);
    }

    #[test]
    fn remote_wiring_rejects_missing_explicit_sources_and_keeps_qualification_separate() {
        let mut config = configuration();
        let mut plan = plan();
        let settings = [setting(20, true, true), setting(21, true, false)];
        config.ptt_source = Some("usb".into());
        config.cor_source = Some("usb".into());
        let wiring = RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::UsbInverted,
            SubaudibleSource::UsbInverted,
            &settings,
        )
        .unwrap();
        assert_eq!(wiring.ptt, RemotePttSource::Usb);
        assert_eq!(wiring.carrier, CarrierSource::UsbInverted);
        assert!(!wiring.rig_carrier);
        assert_eq!(wiring.external_decode, SubaudibleSource::UsbInverted);
        plan.cm119 = None;
        assert!(
            RemotePhysicalWiring::select(
                &config,
                &plan,
                CarrierSource::Dsp,
                SubaudibleSource::Disabled,
                &settings
            )
            .is_err()
        );
        config.ptt_source = Some("hamlib".into());
        config.cor_source = Some("open".into());
        let wiring = RemotePhysicalWiring::select(
            &config,
            &plan,
            CarrierSource::Dsp,
            SubaudibleSource::Disabled,
            &settings,
        )
        .unwrap();
        assert_eq!(wiring.carrier, CarrierSource::Open);
        assert_eq!(wiring.external_decode, SubaudibleSource::Disabled);
        config.cor_source = Some("hamlib".into());
        assert!(
            RemotePhysicalWiring::select(
                &config,
                &plan,
                CarrierSource::Dsp,
                SubaudibleSource::Disabled,
                &[setting(20, false, true)]
            )
            .is_err()
        );
    }

    #[test]
    fn remote_ptt_routing_never_mirrors_to_an_unselected_output() {
        assert!(PhysicalPttControl::Ordinary.key_for(RemotePttSource::Usb, true));
        assert!(PhysicalPttControl::Ordinary.key_for(RemotePttSource::Parallel, true));
        for (selected, key) in [
            (None, false),
            (Some(RemotePttSource::Rig), true),
            (Some(RemotePttSource::Usb), true),
            (Some(RemotePttSource::Parallel), true),
            (Some(RemotePttSource::Usb), false),
        ] {
            let routing = PhysicalPttControl::Remote { selected, key };
            assert_eq!(
                routing.key_for(RemotePttSource::Usb, true),
                key && selected == Some(RemotePttSource::Usb)
            );
            assert_eq!(
                routing.key_for(RemotePttSource::Parallel, true),
                key && selected == Some(RemotePttSource::Parallel)
            );
        }
    }
}
