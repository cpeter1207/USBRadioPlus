//! One remote generation on the existing physical service worker.

use super::*;
use crate::{
    RemoteDevice, RemoteOwner, RemotePhysicalWiring, RemoteQueue, RemoteSetup, RemoteSourceLease,
};
use usbradioplus_core::{CarrierSource, RemoteBackend};
use usbradioplus_radio::SubaudibleSource;
use usbradioplus_remote::{
    CallError, HamlibOperating, OperatingIo, SelectedCapabilities, operating_configuration, raw,
};
use usbradioplus_station::{RemoteHardwareSignals, RemoteRadioControl};

/// All thread-affine contexts remain in the worker that creates this value.
pub(super) struct RemoteGeneration {
    setup: Arc<RemoteSetup>,
    owner: RemoteOwner,
    rig: Option<HamlibOperating>,
    wiring: RemotePhysicalWiring,
    queue: Arc<RemoteQueue>,
    current: raw::RptAdvRemoteState,
    signal_poll: Option<u64>,
    encode_gate: Option<bool>,
}

fn failure(_: ServiceFailure) -> CallError {
    // Hardware operation diagnostics retain the exact GPIO operation separately.
    CallError(-1101)
}

fn dsp_failure(error: usbradioplus_station::RemoteDspError) -> CallError {
    use usbradioplus_station::RemoteDspError;
    // Bridge-owned causes are distinct from portable engine and Hamlib codes:
    // hardware1101, stopped DSP owner1102, DSP deadline1103, DSP update1104.
    CallError(match error {
        RemoteDspError::Busy => -9,
        RemoteDspError::Cancelled => -10,
        RemoteDspError::Stopped => -1102,
        RemoteDspError::Timeout => -1103,
        RemoteDspError::Update(_) => -1104,
    })
}

fn cover(range: raw::rptadv_hamlib_range) -> raw::RptAdvRemoteCoverage {
    raw::RptAdvRemoteCoverage {
        minimum: range.minimum_hz,
        maximum: range.maximum_hz,
        mode_mask: range.modes,
        flags: u32::from(range.minimum_power_mw >= 0)
            | (u32::from(range.maximum_power_mw >= 0) << 1),
        minimum_power_mw: range.minimum_power_mw.max(0) as u32,
        maximum_power_mw: range.maximum_power_mw.max(0) as u32,
    }
}

impl RemoteGeneration {
    pub(super) fn open(
        setup: impl Into<Arc<RemoteSetup>>,
        queue: Arc<RemoteQueue>,
        worker: &ServiceWorker,
        devices: &mut ServiceDevices,
        outputs: &mut OutputState,
    ) -> Result<Self, CallError> {
        let setup = setup.into();
        worker.state.publish_remote_control(RemoteRadioControl {
            enabled: true,
            ..Default::default()
        });
        worker.ptt_control.set(PhysicalPttControl::Remote {
            selected: None,
            key: false,
        });
        let mut rig = setup.open_hamlib_on_owner()?;
        let prepared = (|| {
            let settings = rig
                .as_ref()
                .map_or_else(Vec::new, |rig| rig.settings().to_vec());
            let wiring = RemotePhysicalWiring::select(
                setup.configuration(),
                setup.hardware(),
                setup.inherited_carrier(),
                setup.external_decode(),
                &settings,
            )
            .map_err(|_| CallError(-6))?;
            let mut caps = SelectedCapabilities {
                manual_tune: true,
                encode_sources: 1 << 1,
                decode_sources: 1 << 1,
                decode_control_sources: 1 << 1,
                ..SelectedCapabilities::default()
            };
            if wiring.external_decode != SubaudibleSource::Disabled {
                caps.decode_sources |= 1 << 2;
                caps.decode_control_sources |= 1 << 2;
            }
            if let Some(rig) = &mut rig {
                caps.receive = rig.rig().coverage(false)?.into_iter().map(cover).collect();
                caps.transmit = rig.rig().coverage(true)?.into_iter().map(cover).collect();
                caps.modes = caps
                    .receive
                    .iter()
                    .fold(0, |mask, range| mask | range.mode_mask);
                caps.readback_fields = rig.readback_fields();
                let writable = |selector| {
                    settings
                        .iter()
                        .any(|setting| setting.selector == selector && setting.writable)
                };
                caps.power_mw = writable(11);
                if writable(12) && writable(16) {
                    caps.encode_sources |= 1 << 3;
                }
                if writable(13) && writable(17) {
                    caps.decode_control_sources |= 1 << 3;
                }
            } else {
                // Channel frequency metadata is declared by its configuration. The
                // backend applies only a configured channel; it cannot tune a VFO.
                caps.modes = 1 << 1;
                let ranges = |ranges: &[(u64, u64)]| {
                    ranges
                        .iter()
                        .map(|&(minimum, maximum)| raw::RptAdvRemoteCoverage {
                            minimum,
                            maximum,
                            mode_mask: 1 << 1,
                            flags: 0,
                            minimum_power_mw: 0,
                            maximum_power_mw: 0,
                        })
                        .collect()
                };
                caps.receive = ranges(&setup.configuration().receive_ranges);
                caps.transmit = ranges(&setup.configuration().transmit_ranges);
            }
            caps.scan_modes = match wiring.carrier {
                CarrierSource::Disabled | CarrierSource::Open => 0,
                CarrierSource::Dsp => caps.modes & ((1 << 1) | (1 << 9) | (1 << 10)),
                _ => caps.modes,
            };
            let owner = setup.open_on_owner(&caps, settings, Arc::clone(&queue))?;
            let current = owner.snapshot()?.requested;
            Ok((owner, wiring, current))
        })();
        let (owner, wiring, current) = match prepared {
            Ok(prepared) => prepared,
            Err(error) => {
                queue.retire();
                // Never drop an opened rig after failed admission until all
                // outputs are proven safe. Retry on this same owning worker.
                loop {
                    let gpio = service_remote_ptt(devices, outputs, worker).map_err(|error| {
                        worker.statistics.failure(error);
                        failure(error)
                    });
                    let cat = match &mut rig {
                        Some(rig)
                            if rig
                                .settings()
                                .iter()
                                .any(|setting| setting.selector == 20 && setting.writable) =>
                        {
                            rig.rig().set(20, 0)
                        }
                        _ => Ok(()),
                    };
                    if gpio.and(cat).is_ok() {
                        break;
                    }
                    thread::park_timeout(RETRY_INTERVAL);
                }
                return Err(error);
            }
        };
        worker.state.publish_remote_control(RemoteRadioControl {
            enabled: true,
            ..Default::default()
        });
        worker.ptt_control.set(PhysicalPttControl::Remote {
            selected: Some(wiring.ptt),
            key: false,
        });
        worker.state.publish_remote_signals(RemoteHardwareSignals {
            ptt_selected: true,
            ..Default::default()
        });
        Ok(Self {
            setup,
            owner,
            rig,
            wiring,
            queue,
            current,
            signal_poll: None,
            encode_gate: None,
        })
    }
    pub(super) fn wiring(&self) -> RemotePhysicalWiring {
        self.wiring
    }
    pub(super) fn snapshot(&self) -> Result<raw::RptAdvRemoteSnapshot, CallError> {
        self.owner.snapshot()
    }

    /// Advance only on the existing physical worker. Admission is published
    /// before blocking I/O and reopened only after the engine verifies its result.
    pub(super) fn service(
        &mut self,
        worker: &ServiceWorker,
        devices: &mut ServiceDevices,
        outputs: &mut OutputState,
    ) -> Result<(), CallError> {
        let source = self.owner.pending_source();
        let activity = worker.state.remote_activity();
        let physical = worker.state.remote_signals();
        let flags = (u32::from(activity.local_receiving) * raw::RPTADV_REMOTE_LOCAL_RX)
            | (u32::from(activity.linked_receiving) * raw::RPTADV_REMOTE_LINK_RX)
            | (u32::from(activity.transmit_requested) * raw::RPTADV_REMOTE_REQUEST_PTT)
            | (u32::from(worker.state.outputs().logical_ptt) * raw::RPTADV_REMOTE_RENDERED_PTT)
            | (u32::from(physical.physical_ptt == Some(true)) * raw::RPTADV_REMOTE_PHYSICAL_PTT);
        let wiring = self.wiring();
        let mut backend = PhysicalBackend {
            setup: &self.setup,
            rig: &mut self.rig,
            wiring,
            worker,
            devices,
            outputs,
            current: &mut self.current,
            encode_gate: &mut self.encode_gate,
            queue: &self.queue,
            source,
        };
        let result = self.owner.service(
            worker.now_ms(),
            raw::RptAdvRemoteActivity {
                struct_size: std::mem::size_of::<raw::RptAdvRemoteActivity>() as u32,
                flags,
            },
            &mut backend,
        );
        let snapshot = self.snapshot()?;
        let allowed = result.is_ok() && snapshot.flags & raw::RPTADV_REMOTE_RECEIVE_PERMITTED != 0;
        worker.state.publish_remote_control(RemoteRadioControl {
            enabled: true,
            receive_permitted: allowed,
            transmit_permitted: result.is_ok()
                && snapshot.flags & raw::RPTADV_REMOTE_INHIBITED == 0,
            non_fm: snapshot.flags & raw::RPTADV_REMOTE_APPLIED != 0
                && !matches!(snapshot.applied.mode, 1 | 9 | 10),
        });
        if let Err(error) = result {
            return Err(if snapshot.failure_code != 0 {
                CallError(snapshot.failure_code)
            } else {
                error
            });
        }
        if self.signal_poll.is_none_or(|last| {
            worker.now_ms().saturating_sub(last) >= self.setup.configuration().poll_interval_ms
        }) {
            self.signal_poll = Some(worker.now_ms());
            if self.wiring.rig_carrier {
                let carrier = self.rig.as_mut().ok_or(CallError(-6))?.rig().get(21);
                let mut signals = worker.state.remote_signals();
                signals.carrier = carrier.as_ref().ok().map(|value| *value != 0);
                worker.state.publish_remote_signals(signals);
                carrier?;
            }
        }
        let encode = self.current.encode;
        if encode.source == 3 || encode.control_source == 3 {
            let enabled = encode.enabled != 0
                && activity.ctcss_requested
                && worker.state.remote_control().transmit_permitted;
            if self.encode_gate != Some(enabled) {
                self.rig
                    .as_mut()
                    .ok_or(CallError(-6))?
                    .rig()
                    .set(16, i64::from(enabled))?;
                self.encode_gate = Some(enabled);
            }
        }
        Ok(())
    }

    /// Confirm every nonselected bank inactive as well as the selected provider.
    /// The rig remains owned here on failure; cleanup may not consume it first.
    pub(super) fn unkey(
        &mut self,
        worker: &ServiceWorker,
        devices: &mut ServiceDevices,
        outputs: &mut OutputState,
    ) -> Result<(), CallError> {
        worker.state.publish_remote_control(RemoteRadioControl {
            enabled: true,
            ..Default::default()
        });
        worker.state.publish_remote_tune(false);
        worker.ptt_control.set(PhysicalPttControl::Remote {
            selected: Some(self.wiring.ptt),
            key: false,
        });
        worker.state.publish_remote_signals(RemoteHardwareSignals {
            ptt_selected: true,
            ..Default::default()
        });
        let gpio = service_remote_ptt(devices, outputs, worker).map_err(|error| {
            worker.statistics.failure(error);
            failure(error)
        });
        let rig = if self.wiring.ptt == RemotePttSource::Rig {
            self.rig.as_mut().map_or(Ok(()), |rig| rig.rig().set(20, 0))
        } else {
            Ok(())
        };
        gpio.and(rig)?;
        worker.state.publish_remote_signals(RemoteHardwareSignals {
            ptt_selected: true,
            physical_ptt: Some(false),
            ..Default::default()
        });
        Ok(())
    }
    pub(super) fn retire(
        &mut self,
        worker: &ServiceWorker,
        devices: &mut ServiceDevices,
        outputs: &mut OutputState,
    ) -> Result<(), CallError> {
        self.owner.retire()?;
        self.unkey(worker, devices, outputs)?;
        if let Some(mut rig) = self.rig.take() {
            rig.rig().close()?;
        }
        Ok(())
    }
}

/// Stack borrow into the dynamic engine. No station/control pointer or foreign
/// callback escapes one synchronous service call.
struct PhysicalBackend<'a> {
    setup: &'a RemoteSetup,
    rig: &'a mut Option<HamlibOperating>,
    wiring: RemotePhysicalWiring,
    worker: &'a ServiceWorker,
    devices: &'a mut ServiceDevices,
    outputs: &'a mut OutputState,
    current: &'a mut raw::RptAdvRemoteState,
    encode_gate: &'a mut Option<bool>,
    queue: &'a RemoteQueue,
    source: Option<RemoteSourceLease>,
}
impl PhysicalBackend<'_> {
    fn cancelled(&self) -> bool {
        self.worker.stop.load(Ordering::Acquire)
            || self.queue.has_urgent()
            || self
                .source
                .as_ref()
                .is_some_and(|source| !source.source().valid)
    }
}
impl OperatingIo for PhysicalBackend<'_> {
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
        let worker = self.worker;
        let queue = self.queue;
        let source = self.source.as_ref();
        self.rig
            .as_mut()
            .ok_or(CallError(-6))?
            .read(self.current, || {
                worker.stop.load(Ordering::Acquire)
                    || queue.has_urgent()
                    || source.is_some_and(|source| !source.source().valid)
            })
    }
    fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        self.worker
            .state
            .publish_remote_control(RemoteRadioControl {
                enabled: true,
                ..Default::default()
            });
        if self.cancelled() {
            return Err(CallError(-10));
        }
        let worker = self.worker;
        let queue = self.queue;
        let source = self.source.as_ref();
        let cancelled = || {
            worker.stop.load(Ordering::Acquire)
                || queue.has_urgent()
                || source.is_some_and(|source| !source.source().valid)
        };
        if let Some(rig) = &mut self.rig {
            rig.apply(state, cancelled)?;
            *self.encode_gate = None;
        } else {
            // These transports have FM/channel/frequency control only. Never
            // acknowledge an RF control the existing hardware cannot program.
            if state.mode != 1
                || state.bandwidth_hz != 0
                || state.receive_vfo != 0
                || state.transmit_vfo != 0
                || state.rit_hz != 0
                || state.xit_hz != 0
                || state.flags & (2 | 16 | 32) != 0
            {
                return Err(CallError(-6));
            }
            match self.setup.configuration().backend {
                RemoteBackend::ParallelChannel => {
                    if state.flags & 4 == 0
                        || !self.setup.configuration().memories.iter().any(|memory| {
                            memory.state.channel == Some(state.channel as u8)
                                && memory.state.receive_hz == state.receive_hz
                                && memory.state.transmit_hz.unwrap_or(0) == state.transmit_hz
                                && memory.state.offset_hz == state.offset_hz
                        })
                    {
                        return Err(CallError(-6));
                    }
                    self.outputs
                        .select_channel(
                            state.channel.try_into().map_err(|_| CallError(-6))?,
                            self.devices,
                        )
                        .map_err(failure)?;
                }
                RemoteBackend::ParallelRtx => {
                    let transmit = state
                        .transmit_hz
                        .checked_add_signed(state.offset_hz)
                        .ok_or(CallError(-6))?;
                    self.outputs.configure_remote_radio(RemoteRadio {
                        receive_hz: state.receive_hz.try_into().map_err(|_| CallError(-6))?,
                        transmit_hz: transmit.try_into().map_err(|_| CallError(-6))?,
                        receive_ctcss: None,
                        transmit_ctcss: None,
                        high_power: false,
                    });
                    self.outputs
                        .service_remote_radio(self.devices, false)
                        .map_err(failure)?;
                }
                RemoteBackend::Hamlib => return Err(CallError(-6)),
            }
        }
        self.setup
            .dsp()
            .apply(
                operating_configuration(state)?,
                self.wiring.external_decode,
                self.wiring.carrier,
                CONTROL_RESPONSE_TIMEOUT,
                cancelled,
            )
            .map_err(dsp_failure)?;
        *self.current = *state;
        Ok(())
    }
    fn set_ptt(&mut self, keyed: bool) -> Result<(), CallError> {
        self.worker
            .state
            .publish_remote_signals(RemoteHardwareSignals {
                ptt_selected: true,
                physical_ptt: None,
                ..self.worker.state.remote_signals()
            });
        self.worker.ptt_control.set(PhysicalPttControl::Remote {
            selected: Some(self.wiring.ptt),
            key: keyed,
        });
        let gpio = service_remote_ptt(self.devices, self.outputs, self.worker).map_err(|error| {
            self.worker.statistics.failure(error);
            failure(error)
        });
        if keyed && gpio.is_err() {
            return gpio;
        }
        let rig = if self.wiring.ptt == RemotePttSource::Rig {
            self.rig
                .as_mut()
                .ok_or(CallError(-6))?
                .rig()
                .set(20, i64::from(keyed))
        } else {
            Ok(())
        };
        gpio.and(rig)?;
        self.worker
            .state
            .publish_remote_signals(RemoteHardwareSignals {
                physical_ptt: Some(keyed),
                ..self.worker.state.remote_signals()
            });
        Ok(())
    }
    fn set_tune(&mut self, enabled: bool) -> Result<(), CallError> {
        self.worker.state.publish_remote_tune(enabled);
        Ok(())
    }
    fn monotonic_ms(&self) -> u64 {
        self.worker.now_ms()
    }
    fn transmit_epoch(&self) -> u64 {
        self.worker.state.remote_activity().transmit_epoch
    }
}
impl RemoteDevice for PhysicalBackend<'_> {
    fn get_setting(
        &mut self,
        selector: u32,
        snapshot: &raw::RptAdvRemoteSnapshot,
    ) -> Result<i64, CallError> {
        let rig = self.rig.as_mut().ok_or(CallError(-6))?.rig();
        if selector == 11 {
            let state = snapshot.observed;
            if snapshot.observed_fields & (2 | 4 | 64 | 256) != (2 | 4 | 64 | 256) {
                return Err(CallError(-6));
            }
            let transmit = state
                .transmit_hz
                .checked_add_signed(state.offset_hz)
                .and_then(|value| {
                    value.checked_add_signed(if state.flags & 32 != 0 {
                        state.xit_hz
                    } else {
                        0
                    })
                })
                .ok_or(CallError(-6))?;
            rig.get_power_mw(transmit, i64::from(state.mode))
                .map(i64::from)
        } else {
            rig.get(selector)
        }
    }
    fn set_setting(&mut self, selector: u32, value: i64) -> Result<(), CallError> {
        self.rig
            .as_mut()
            .ok_or(CallError(-6))?
            .rig()
            .set(selector, value)
    }
}
