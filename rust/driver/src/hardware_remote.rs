//! One remote generation on the existing physical service worker.

use super::*;
use crate::{
    RemoteDevice, RemoteOwner, RemotePhysicalWiring, RemoteQueue, RemoteSetup, RemoteSourceLease,
};
use usbradioplus_core::{CarrierSource, RemoteBackend};
use usbradioplus_radio::SubaudibleSource;
use usbradioplus_remote::{
    CallError, HamlibOperating, OperatingIo, SelectedCapabilities, Setting,
    operating_configuration, raw,
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

/// Unknown physical PTT cannot release half-duplex non-FM receive. FM retains
/// its ordinary DSP qualification and genuine full-duplex radios remain open.
fn receive_allowed(engine: bool, mode: u32, duplex: bool, physical: Option<bool>) -> bool {
    engine && (matches!(mode, 1 | 9 | 10) || duplex || physical == Some(false))
}

fn physical_busy(physical: Option<bool>) -> bool {
    physical != Some(false)
}

/// Re-read cancellation between native calls; do not snapshot it before CAT I/O.
fn physical_cancelled(
    worker: &ServiceWorker,
    queue: &RemoteQueue,
    source: Option<&RemoteSourceLease>,
) -> bool {
    worker.stop.load(Ordering::Acquire)
        || queue.has_urgent()
        || source.is_some_and(|source| !source.source().valid)
}

fn confirm_ptt(actual: i64, keyed: bool) -> Result<(), CallError> {
    if (actual != 0) == keyed {
        Ok(())
    } else {
        Err(CallError(-1101))
    }
}

fn first_cause(current: CallError, recorded: i32) -> CallError {
    if recorded == 0 {
        current
    } else {
        CallError(recorded)
    }
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

fn writable(settings: &[Setting], selector: u32) -> bool {
    settings
        .iter()
        .any(|setting| setting.selector == selector && setting.writable)
}

/// Tone programming and enable controls are independent backend capabilities.
fn add_rig_controls(caps: &mut SelectedCapabilities, settings: &[Setting]) {
    caps.power_mw = writable(settings, 11);
    if writable(settings, 12) && writable(settings, 16) {
        caps.encode_sources |= 1 << 3;
    }
    if writable(settings, 13) && writable(settings, 17) {
        caps.decode_control_sources |= 1 << 3;
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
        let encoder =
            crate::remote_hardware::external_encoder(setup.configuration(), setup.hardware())
                .map_err(|_| CallError(-6))?;
        outputs.bind_encoder(encoder, devices).map_err(failure)?;
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
            if wiring.external_encode.is_some() {
                caps.encode_sources |= 1 << 2;
            }
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
                add_rig_controls(&mut caps, &settings);
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
                    let cat = rig
                        .as_mut()
                        .filter(|rig| writable(rig.settings(), 20))
                        .map_or(Ok(()), |rig| rig.rig().set(20, 0));
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
        let mut flags = (u32::from(activity.local_receiving) * raw::RPTADV_REMOTE_LOCAL_RX)
            | (u32::from(activity.linked_receiving) * raw::RPTADV_REMOTE_LINK_RX)
            | (u32::from(activity.transmit_requested) * raw::RPTADV_REMOTE_REQUEST_PTT)
            | (u32::from(worker.state.outputs().logical_ptt) * raw::RPTADV_REMOTE_RENDERED_PTT)
            | (u32::from(physical_busy(physical.physical_ptt)) * raw::RPTADV_REMOTE_PHYSICAL_PTT);
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
        // The engine's cached serviced state cannot establish physical safety
        // after an observation failure. Retry confirmation on this same owner.
        if physical.physical_ptt.is_none() {
            backend.set_ptt(false)?;
            flags &= !raw::RPTADV_REMOTE_PHYSICAL_PTT;
        }
        let result = self.owner.service(
            worker.now_ms(),
            raw::RptAdvRemoteActivity {
                struct_size: std::mem::size_of::<raw::RptAdvRemoteActivity>() as u32,
                flags,
            },
            &mut backend,
        );
        if result.is_ok()
            && self.signal_poll.is_none_or(|last| {
                worker.now_ms().saturating_sub(last) >= self.setup.configuration().poll_interval_ms
            })
        {
            if wiring.rig_carrier {
                let carrier = backend.rig.as_mut().ok_or(CallError(-6))?.rig().get(21);
                let mut signals = worker.state.remote_signals();
                signals.carrier = carrier.as_ref().ok().map(|value| *value != 0);
                worker.state.publish_remote_signals(signals);
                carrier?;
            }
            if wiring.ptt == RemotePttSource::Rig {
                backend.transmit_idle()?;
            }
            // Slow exchanges count toward service time, not the next poll's gap.
            self.signal_poll = Some(worker.now_ms());
        }
        let snapshot = self.snapshot()?;
        let allowed = receive_allowed(
            result.is_ok() && snapshot.flags & raw::RPTADV_REMOTE_RECEIVE_PERMITTED != 0,
            snapshot.applied.mode,
            self.setup.physical_duplex(),
            worker.state.remote_signals().physical_ptt,
        );
        worker.state.publish_remote_control(RemoteRadioControl {
            enabled: true,
            receive_permitted: allowed,
            transmit_permitted: result.is_ok()
                && snapshot.flags & raw::RPTADV_REMOTE_INHIBITED == 0,
            non_fm: snapshot.flags & raw::RPTADV_REMOTE_APPLIED != 0
                && !matches!(snapshot.applied.mode, 1 | 9 | 10),
            local_unqualified_open: snapshot.flags & raw::RPTADV_REMOTE_APPLIED != 0
                && self.wiring.carrier == CarrierSource::Open
                && snapshot.applied.decode.enabled == 0,
        });
        if let Err(error) = result {
            outputs.set_encoder(false);
            return Err(first_cause(error, snapshot.failure_code));
        }
        let encode = self.current.encode;
        outputs.set_encoder(
            encode.source == 2
                && encode.enabled != 0
                && activity.ctcss_requested
                && worker.state.remote_control().transmit_permitted,
        );
        if encode.source == 3 {
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
        // An allowed front-panel observation can set APPLIED while startup's
        // RF/DSP transaction is still pending. Only a completed commit is ready.
        if snapshot.flags & (raw::RPTADV_REMOTE_APPLIED | raw::RPTADV_REMOTE_PENDING)
            == raw::RPTADV_REMOTE_APPLIED
        {
            self.queue.mark_ready();
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
        outputs.set_encoder(false);
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
            let rig = self.rig.as_mut().ok_or(CallError(-6))?.rig();
            rig.set(20, 0)
                .and_then(|()| confirm_ptt(rig.get(20)?, false))
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
        // Hamlib close consumes its handle even on error. Taking it below is
        // possible only after confirmed OFF, so a retry is already safe/done.
        // Parallel backends have no rig from birth and still need GPIO unkey.
        if self.setup.configuration().backend == RemoteBackend::Hamlib && self.rig.is_none() {
            return Ok(());
        }
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
impl OperatingIo for PhysicalBackend<'_> {
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError> {
        let worker = self.worker;
        let queue = self.queue;
        let source = self.source.as_ref();
        self.rig
            .as_mut()
            .ok_or(CallError(-6))?
            .read(self.current, || physical_cancelled(worker, queue, source))
    }
    fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        self.worker
            .state
            .publish_remote_control(RemoteRadioControl {
                enabled: true,
                ..Default::default()
            });
        if physical_cancelled(self.worker, self.queue, self.source.as_ref()) {
            return Err(CallError(-10));
        }
        let worker = self.worker;
        let queue = self.queue;
        let source = self.source.as_ref();
        let cancelled = || physical_cancelled(worker, queue, source);
        if let Some(rig) = &mut self.rig {
            rig.apply(state, cancelled)?;
            *self.encode_gate = None;
        } else {
            // These transports have FM/channel/frequency control only. Never
            // acknowledge an RF control the existing hardware cannot program.
            // FM is already enforced by the admitted parallel capability mask.
            if state.bandwidth_hz != 0
                || state.receive_vfo != 0
                || state.transmit_vfo != 0
                || state.rit_hz != 0
                || state.xit_hz != 0
                || state.flags & (2 | 16 | 32) != 0
            {
                return Err(CallError(-6));
            }
            // A live Hamlib generation always owns a rig; only parallel remains.
            if self.setup.configuration().backend == RemoteBackend::ParallelChannel {
                if !self.setup.configuration().memories.iter().any(|memory| {
                    memory.state.channel == Some(state.channel as u8)
                        && memory.state.receive_hz == state.receive_hz
                        && memory.state.transmit_hz.unwrap_or(0) == state.transmit_hz
                        && memory.state.offset_hz == state.offset_hz
                }) {
                    return Err(CallError(-6));
                }
                self.outputs
                    .select_channel(state.channel as u8, self.devices)
                    .map_err(failure)?;
            } else {
                let transmit = if state.flags & 1 == 0 {
                    0
                } else {
                    state
                        .transmit_hz
                        .checked_add_signed(state.offset_hz)
                        .expect("Core admitted the effective transmit frequency")
                };
                if !crate::remote_hardware::rtx_frequencies_representable(
                    state.receive_hz,
                    transmit,
                ) {
                    return Err(CallError(-6));
                }
                self.outputs
                    .configure_remote_radio(RemoteRadio {
                        // The verified RTX register range fits u32.
                        receive_hz: state.receive_hz as u32,
                        transmit_hz: transmit as u32,
                        receive_ctcss: None,
                        transmit_ctcss: None,
                        high_power: false,
                    })
                    .map_err(failure)?;
                self.outputs
                    .service_remote_radio(self.devices, false)
                    .map_err(failure)?;
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
        if !keyed {
            self.outputs.set_encoder(false);
        }
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
            let rig = self.rig.as_mut().ok_or(CallError(-6))?.rig();
            rig.set(20, i64::from(keyed))
                .and_then(|()| confirm_ptt(rig.get(20)?, keyed))
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
    fn transmit_idle(&mut self) -> Result<bool, CallError> {
        if self.wiring.ptt != RemotePttSource::Rig {
            return Ok(self.worker.state.remote_signals().physical_ptt == Some(false));
        }
        let active = self
            .rig
            .as_mut()
            .ok_or(CallError(-6))?
            .rig()
            .get(20)
            .map(|value| value != 0);
        self.worker
            .state
            .publish_remote_signals(RemoteHardwareSignals {
                physical_ptt: active.as_ref().ok().copied(),
                ..self.worker.state.remote_signals()
            });
        active.map(|active| !active)
    }
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

#[cfg(test)]
#[path = "tests/hardware_remote_tests.rs"]
mod tests;
