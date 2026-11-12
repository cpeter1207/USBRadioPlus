//! Control-owned replacement storage; callbacks only take a pointer and acknowledge it.

use super::*;
use std::fmt;
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicPtr};
use std::sync::{Mutex, Weak};
use usbradioplus_radio::{PreparedUpdate, ProgramRingPort, RadioError, RadioProvider};
use usbradioplus_runtime::{NativeProcessingFactory, ProcessingGeneration};

/// Preparation or adoption failure. Pending updates retain all borrowed storage.
#[derive(Debug)]
pub enum StationUpdateError {
    /// Device, stream bounds, or transport require the existing device handoff.
    Incompatible,
    /// A prior transaction still owns the bounded update slots.
    Busy,
    /// One callback has not yet acknowledged its update.
    Pending,
    /// A processing graph could not be prepared off the audio thread.
    Preparation(crate::StationPreparationError),
    /// The radio provider rejected an update without changing that owner.
    Radio(RadioError),
}

impl fmt::Display for StationUpdateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Incompatible => f.write_str("setting requires device handoff"),
            Self::Busy => f.write_str("another live update is awaiting completion"),
            Self::Pending => f.write_str("audio callbacks have not acknowledged the live update"),
            Self::Preparation(error) => error.fmt(f),
            Self::Radio(error) => error.fmt(f),
        }
    }
}

impl std::error::Error for StationUpdateError {}

/// Prepared forward and rollback settings; neither opens or stops hardware.
pub struct StationUpdate {
    plan: StationPlan,
    previous: StationPlan,
    forward: UpdateOwner,
    reverse: UpdateOwner,
    source: Weak<UpdateMailbox>,
    revision: u64,
    programme_retirement: bool,
}

impl StationUpdate {
    /// Effective settings adopted by this update.
    pub const fn plan(&self) -> &StationPlan {
        &self.plan
    }
}

/// Stable processor leases captured briefly by the control owner. Expensive graph
/// construction can then run on another thread while media delivery continues.
pub struct StationUpdatePreparation {
    previous: StationPlan,
    processing: ProcessingGeneration,
    factory: NativeProcessingFactory,
    provider: RadioProvider,
    source: Weak<UpdateMailbox>,
    revision: u64,
}

impl StationUpdatePreparation {
    /// Exact generation captured by the original control owner. Build replacements
    /// from this plan so concurrent tuning/reloads cannot overwrite newer settings.
    pub const fn plan(&self) -> &StationPlan {
        &self.previous
    }

    /// Prepare forward/rollback updates without accessing a live station owner.
    pub fn prepare(self, plan: StationPlan) -> Result<StationUpdate, StationUpdateError> {
        let plan = plan
            .retain_remote_operating(&self.previous)
            .map_err(StationUpdateError::Preparation)?;
        // StationPlan::new gives RX and TX the same immutable frame maximum.
        if plan.hardware() != self.previous.hardware()
            || plan.remote_configuration() != self.previous.remote_configuration()
            || plan.transport() != self.previous.transport()
            || plan.radio().maximum_receive_frame_count
                != self.previous.radio().maximum_receive_frame_count
        {
            return Err(StationUpdateError::Incompatible);
        }
        // SAFETY: the private lease is bound to its original mailbox and revision.
        // Only that station's serial RX/TX owners may ever adopt these ports.
        let forward = unsafe {
            self.factory.prepare_replacement(
                plan.processing(),
                self.previous.processing(),
                &self.processing,
            )
        }
        .map_err(|error| StationUpdateError::Preparation(error.into()))?;
        // SAFETY: both leases belong to these same serial owners. Restored graphs
        // are primed from the audible forward path without warming live state.
        let reverse = unsafe {
            self.factory
                .prepare_restore(self.previous.processing(), &self.processing, &forward)
        }
        .map_err(|error| StationUpdateError::Preparation(error.into()))?;
        let forward = UpdateOwner::prepare(&plan, forward, self.provider)?;
        let reverse = UpdateOwner::prepare(&self.previous, reverse, self.provider)?;
        Ok(StationUpdate {
            plan,
            previous: self.previous,
            forward,
            reverse,
            source: self.source,
            revision: self.revision,
            programme_retirement: false,
        })
    }

    fn prepare_programme_retirement(self) -> Result<StationUpdate, StationUpdateError> {
        // Both possible successors have fresh TX histories. A failed forward
        // adoption must never restore the retired predecessor programme.
        let prepare = || {
            // SAFETY: these leases are restricted to the original mailbox/revision.
            unsafe {
                self.factory
                    .prepare_programme_retirement(self.previous.processing(), &self.processing)
            }
            .map_err(|error| StationUpdateError::Preparation(error.into()))
        };
        let forward = UpdateOwner::prepare(&self.previous, prepare()?, self.provider)?;
        let reverse = UpdateOwner::prepare(&self.previous, prepare()?, self.provider)?;
        Ok(StationUpdate {
            plan: self.previous.clone(),
            previous: self.previous,
            forward,
            reverse,
            source: self.source,
            revision: self.revision,
            programme_retirement: true,
        })
    }
}

struct UpdateOwner {
    physical_duplex: bool,
    radio: PreparedUpdate<'static>,
    processing: Box<ProcessingGeneration>,
    controller_ctcss: [Option<CtcssTone>; CTCSS_TONE_COUNT],
    voter_reporting: bool,
}

impl UpdateOwner {
    fn prepare(
        plan: &StationPlan,
        processing: ProcessingGeneration,
        provider: RadioProvider,
    ) -> Result<Self, StationUpdateError> {
        let mut processing = Box::new(processing);
        let ports = processing.session_ports(ProgramRingPort::silence());
        let radio = provider
            .prepare_update(plan.radio(), ports)
            .map_err(StationUpdateError::Radio)?;
        // SAFETY: the stable processing box follows radio in drop order. The
        // mailbox retains this owner until callbacks stop or adopt its successor.
        let radio =
            unsafe { std::mem::transmute::<PreparedUpdate<'_>, PreparedUpdate<'static>>(radio) };
        Ok(Self {
            radio,
            physical_duplex: plan.radio().qualification.radio_duplex,
            processing,
            controller_ctcss: controller_ctcss_table(plan),
            voter_reporting: plan.configuration().station.hardware.voter_reporting != 0,
        })
    }
}

struct Transaction {
    candidate: Box<StationUpdate>,
    reversing: bool,
}

#[derive(Default)]
struct UpdateStorage {
    // Keep the adopted owner, including reverse transition storage, until both
    // callbacks adopt its successor. Raw callback ports borrow this allocation.
    current: Option<UpdateOwner>,
    transaction: Option<Transaction>,
    revision: u64,
    retirement: Option<ProgrammeRetirement>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RetirementOutcome {
    Pending,
    Failed,
    Complete,
}

#[derive(Clone, Copy)]
struct ProgrammeRetirement {
    request: u64,
    revision: u64,
    outcome: RetirementOutcome,
}

/// Only control operations lock storage. RX/TX exclusively use their atomic slot.
/// The observer retains a lease too, so borrowed processor storage outlives the
/// radio session even if the runtime is destroyed before its control host.
#[derive(Default)]
pub(super) struct UpdateMailbox {
    storage: Mutex<UpdateStorage>,
    receive: AtomicPtr<UpdateOwner>,
    transmit: AtomicPtr<UpdateOwner>,
    // Either audio owner can adopt before the control-plane commit.
    receive_processing: AtomicPtr<ProcessingGeneration>,
    transmit_processing: AtomicPtr<ProcessingGeneration>,
    receive_result: AtomicI32,
    transmit_result: AtomicI32,
    retirement_ack: crate::feedback::Slot<2>,
}

impl UpdateMailbox {
    pub(super) fn retired_request(&self, generation: u64) -> u64 {
        self.retirement_ack
            .read()
            .filter(|value| value[0] == generation)
            .map_or(0, |value| value[1])
    }

    /// Called only between operations of the original serial TX owner.
    pub(super) unsafe fn programme_observation(
        &self,
        station: &StationTransmit,
    ) -> (usbradioplus_core::ProgrammeBuffering, u64) {
        // SAFETY: storage retains every adopted owner through successor ACKs;
        // this sole TX owner cannot acknowledge or reclaim itself during read.
        let processing = unsafe { self.transmit_processing.load(Ordering::Acquire).as_ref() }
            .unwrap_or_else(|| station.processing());
        // SAFETY: caller is the sole TX graph owner between its process calls.
        (
            // SAFETY: caller is the sole TX graph owner between process calls.
            unsafe { processing.transmit_programme_buffering() },
            processing
                .transmit_loudness()
                .expect("prepared TX meter")
                .sample_count,
        )
    }

    /// Apply only on the original serial TX owner after successor adoption.
    pub(super) unsafe fn bind_programme_fence(
        &self,
        station: &StationTransmit,
        port: Option<usbradioplus_runtime::ProgrammeFence>,
        generation: u64,
    ) {
        // SAFETY: adopted storage is retained until this owner acknowledges;
        // neither reclamation nor a concurrent TX call can occur in this read.
        let processing = unsafe { self.transmit_processing.load(Ordering::Acquire).as_ref() }
            .unwrap_or_else(|| station.processing());
        // SAFETY: forwarded to the same sole TX owner between graph operations.
        unsafe { processing.set_transmit_programme_fence(port, generation) };
    }

    fn publish(&self, owner: &UpdateOwner) {
        self.receive_result.store(0, Ordering::Relaxed);
        self.transmit_result.store(0, Ordering::Relaxed);
        let pointer = ptr::from_ref(owner).cast_mut();
        self.receive.store(pointer, Ordering::Release);
        self.transmit.store(pointer, Ordering::Release);
    }

    fn result(&self) -> Result<(), StationUpdateError> {
        let rx = self.receive_result.load(Ordering::Acquire);
        let tx = self.transmit_result.load(Ordering::Acquire);
        if rx == 0 || tx == 0 {
            return Err(StationUpdateError::Pending);
        }
        if rx < 0 || tx < 0 {
            return Err(StationUpdateError::Radio(RadioError::InvalidArgument));
        }
        Ok(())
    }

    pub(super) fn receive(
        &self,
        station: &mut StationReceive,
        ctcss: &mut [Option<CtcssTone>; CTCSS_TONE_COUNT],
        voter: &mut bool,
    ) {
        let pointer = self.receive.swap(ptr::null_mut(), Ordering::Acquire);
        if pointer.is_null() {
            return;
        }
        // SAFETY: control retains the stable owner until this final release ACK.
        let owner = unsafe { &*pointer };
        let result = station.radio().apply_update(&owner.radio);
        if result.is_ok() {
            *ctcss = owner.controller_ctcss;
            *voter = owner.voter_reporting;
            self.receive_processing.store(
                ptr::from_ref(owner.processing.as_ref()).cast_mut(),
                Ordering::Release,
            );
        }
        self.receive_result
            .store(if result.is_ok() { 1 } else { -1 }, Ordering::Release);
    }

    pub(super) fn transmit(&self, station: &mut StationTransmit) {
        let pointer = self.transmit.swap(ptr::null_mut(), Ordering::Acquire);
        if pointer.is_null() {
            return;
        }
        // SAFETY: control retains the stable owner until this final release ACK.
        let owner = unsafe { &*pointer };
        let result = station.radio().apply_update(&owner.radio);
        if result.is_ok() {
            station.physical_duplex = owner.physical_duplex;
            self.transmit_processing.store(
                ptr::from_ref(owner.processing.as_ref()).cast_mut(),
                Ordering::Release,
            );
        }
        self.transmit_result
            .store(if result.is_ok() { 1 } else { -1 }, Ordering::Release);
    }
}

impl StationControlHost {
    /// Read the mandatory output meter from the generation actually adopted by RX.
    pub fn receive_loudness(&self) -> Option<usbradioplus_core::OutputLoudnessSnapshot> {
        self.output_loudness(
            &self._updates.receive_processing,
            ProcessingGeneration::receive_loudness,
        )
    }

    /// Read the mandatory output meter from the generation actually adopted by TX.
    pub fn transmit_loudness(&self) -> Option<usbradioplus_core::OutputLoudnessSnapshot> {
        self.output_loudness(
            &self._updates.transmit_processing,
            ProcessingGeneration::transmit_loudness,
        )
    }

    fn output_loudness(
        &self,
        owner: &AtomicPtr<ProcessingGeneration>,
        read: fn(&ProcessingGeneration) -> Option<usbradioplus_core::OutputLoudnessSnapshot>,
    ) -> Option<usbradioplus_core::OutputLoudnessSnapshot> {
        let _storage = self
            ._updates
            .storage
            .lock()
            .expect("update storage poisoned");
        let pointer = owner.load(Ordering::Acquire);
        // SAFETY: storage retains each adopted owner until its successor is
        // acknowledged; this control-plane lock prevents reclamation during read.
        let processing = unsafe { pointer.as_ref() }.unwrap_or_else(|| self.control.processing());
        read(processing)
    }
}

impl StationRuntime {
    /// Whether explicit programme retirement currently owns the existing update
    /// transaction. Later queued remote work must not block this owner's polls.
    pub fn programme_retirement_pending(&self) -> bool {
        self.updates
            .storage
            .lock()
            .expect("update storage poisoned")
            .transaction
            .as_ref()
            .is_some_and(|transaction| transaction.candidate.programme_retirement)
    }

    /// Explicitly retire only TX programme history through the existing update
    /// owners. A repeated nonzero request polls one transaction; newer requests
    /// wait for its completion. Failed/pending work retains every borrowed owner.
    /// Completion is scoped to the current radio lifecycle generation.
    pub fn retire_programme(
        &mut self,
        control: &mut StationControlHost,
        factory: &NativeProcessingFactory,
        provider: RadioProvider,
        expected_generation: u64,
        request: u64,
    ) -> Result<(), StationUpdateError> {
        if request == 0
            || !Arc::ptr_eq(&self.updates, &control._updates)
            || expected_generation != self.generation_id
        {
            return Err(StationUpdateError::Incompatible);
        }
        if self.running && !self.started() {
            return Err(StationUpdateError::Busy);
        }
        let generation = self.generation_id;
        let prepare = {
            let mut storage = self
                .updates
                .storage
                .lock()
                .expect("update storage poisoned");
            if storage
                .transaction
                .as_ref()
                .is_some_and(|transaction| !transaction.candidate.programme_retirement)
            {
                return Err(StationUpdateError::Busy);
            }
            if let Some(previous) = storage.retirement {
                if request < previous.request
                    || (request == previous.request
                        && previous.outcome == RetirementOutcome::Complete
                        && storage.revision != previous.revision)
                {
                    return Err(StationUpdateError::Incompatible);
                }
                if request == previous.request {
                    match previous.outcome {
                        RetirementOutcome::Complete => return Ok(()),
                        RetirementOutcome::Failed if storage.transaction.is_none() => {
                            return Err(StationUpdateError::Radio(RadioError::InvalidArgument));
                        }
                        _ => {}
                    }
                }
            }
            let same = storage
                .retirement
                .is_some_and(|previous| previous.request == request);
            if storage.transaction.is_some() && !same {
                return Err(StationUpdateError::Busy);
            }
            if !same {
                if !self.programme_retirement_ready {
                    return Err(StationUpdateError::Busy);
                }
                storage.retirement = Some(ProgrammeRetirement {
                    request,
                    revision: storage.revision,
                    outcome: RetirementOutcome::Pending,
                });
                self.updates.retirement_ack.publish([0, 0]);
            }
            !same
        };
        if prepare {
            let result = self
                .snapshot_update(control, factory, provider)
                .and_then(StationUpdatePreparation::prepare_programme_retirement)
                .and_then(|update| self.begin_update(control, update));
            if let Err(error) = result {
                self.updates
                    .storage
                    .lock()
                    .expect("update storage poisoned")
                    .retirement
                    .as_mut()
                    .expect("retirement request")
                    .outcome = RetirementOutcome::Failed;
                return Err(error);
            }
        }
        let failed = self
            .updates
            .storage
            .lock()
            .expect("update storage poisoned")
            .retirement
            .expect("retirement request")
            .outcome
            == RetirementOutcome::Failed;
        if !failed {
            match self.update_result() {
                Err(StationUpdateError::Pending) => return Err(StationUpdateError::Pending),
                Err(_) => {
                    self.updates
                        .storage
                        .lock()
                        .expect("update storage poisoned")
                        .retirement
                        .as_mut()
                        .expect("retirement request")
                        .outcome = RetirementOutcome::Failed;
                }
                Ok(()) => {
                    self.finish_update_inner(control, true, true)?;
                    let mut storage = self
                        .updates
                        .storage
                        .lock()
                        .expect("update storage poisoned");
                    let revision = storage.revision;
                    let retirement = storage.retirement.as_mut().expect("retirement request");
                    retirement.outcome = RetirementOutcome::Complete;
                    retirement.revision = revision;
                    self.updates.retirement_ack.publish([generation, request]);
                    return Ok(());
                }
            }
        }
        self.finish_update_inner(control, false, true)?;
        Err(StationUpdateError::Radio(RadioError::InvalidArgument))
    }

    /// Capture immutable policy and processor leases without building new graphs.
    pub fn snapshot_update(
        &self,
        control: &StationControlHost,
        factory: &NativeProcessingFactory,
        provider: RadioProvider,
    ) -> Result<StationUpdatePreparation, StationUpdateError> {
        if !Arc::ptr_eq(&self.updates, &control._updates) {
            return Err(StationUpdateError::Incompatible);
        }
        let storage = self
            .updates
            .storage
            .lock()
            .expect("update storage poisoned");
        if storage.transaction.is_some() {
            return Err(StationUpdateError::Busy);
        }
        let current = storage
            .current
            .as_ref()
            .map_or_else(|| control.control.processing(), |update| &update.processing);
        // SAFETY: unchanged descriptions only clone leases; no live port is run.
        // Private mailbox/revision admission enforces the original serial owners.
        let processing = unsafe {
            factory.prepare_replacement(
                control.plan.processing(),
                control.plan.processing(),
                current,
            )
        }
        .map_err(|error| StationUpdateError::Preparation(error.into()))?;
        Ok(StationUpdatePreparation {
            previous: control.plan.clone(),
            processing,
            factory: factory.clone(),
            provider,
            source: Arc::downgrade(&self.updates),
            revision: storage.revision,
        })
    }

    /// Publish preallocated settings; only the original station may adopt them.
    pub fn begin_update(
        &mut self,
        control: &StationControlHost,
        update: StationUpdate,
    ) -> Result<(), StationUpdateError> {
        if !Weak::ptr_eq(&update.source, &Arc::downgrade(&self.updates))
            || !Arc::ptr_eq(&self.updates, &control._updates)
        {
            return Err(StationUpdateError::Incompatible);
        }
        control
            .control
            .validate_update(&update.forward.radio)
            .map_err(StationUpdateError::Radio)?;
        control
            .control
            .validate_update(&update.reverse.radio)
            .map_err(StationUpdateError::Radio)?;
        let mut storage = self
            .updates
            .storage
            .lock()
            .expect("update storage poisoned");
        if storage.transaction.is_some() || storage.revision != update.revision {
            return Err(StationUpdateError::Busy);
        }
        self.updates.retirement_ack.publish([0, 0]);
        storage.transaction = Some(Transaction {
            candidate: Box::new(update),
            reversing: false,
        });
        self.updates.publish(
            &storage
                .transaction
                .as_ref()
                .expect("installed update")
                .candidate
                .forward,
        );
        self.apply_stopped_update();
        Ok(())
    }

    /// Poll both callback acknowledgments without waiting or invoking a callback.
    pub fn update_result(&self) -> Result<(), StationUpdateError> {
        self.updates.result()
    }

    /// Publish the new effective plan after both owners have adopted it.
    pub fn accept_update(
        &self,
        control: &mut StationControlHost,
    ) -> Result<(), StationUpdateError> {
        if !Arc::ptr_eq(&self.updates, &control._updates) {
            return Err(StationUpdateError::Incompatible);
        }
        self.update_result()?;
        let storage = self
            .updates
            .storage
            .lock()
            .expect("update storage poisoned");
        let transaction = storage
            .transaction
            .as_ref()
            .ok_or(StationUpdateError::Busy)?;
        if transaction.reversing || transaction.candidate.programme_retirement {
            return Err(StationUpdateError::Busy);
        }
        control.plan = transaction.candidate.plan.clone();
        Ok(())
    }

    /// Commit or roll back without stopping audio, clearing PCM, or changing PTT.
    /// A stalled callback leaves the transaction bounded and safely retained.
    pub fn finish_update(
        &mut self,
        control: &mut StationControlHost,
        commit: bool,
    ) -> Result<(), StationUpdateError> {
        self.finish_update_inner(control, commit, false)
    }

    fn finish_update_inner(
        &mut self,
        control: &mut StationControlHost,
        commit: bool,
        retirement_owner: bool,
    ) -> Result<(), StationUpdateError> {
        if !Arc::ptr_eq(&self.updates, &control._updates) {
            return Err(StationUpdateError::Incompatible);
        }
        // Successful stop quiesces both owners. Drain any forward half before
        // deciding whether its retained reverse generation can be retired.
        self.apply_stopped_update();
        let mut storage = self
            .updates
            .storage
            .lock()
            .expect("update storage poisoned");
        let Some(transaction) = storage.transaction.as_mut() else {
            return Ok(());
        };
        if transaction.candidate.programme_retirement != retirement_owner {
            return Err(StationUpdateError::Busy);
        }
        if commit {
            self.updates.result()?;
            if transaction.reversing {
                return Err(StationUpdateError::Busy);
            }
            control.plan = transaction.candidate.plan.clone();
            storage.current = storage
                .transaction
                .take()
                .map(|transaction| transaction.candidate.forward);
            storage.revision = storage.revision.wrapping_add(1);
            return Ok(());
        }
        match self.updates.result() {
            Err(StationUpdateError::Pending) => return Err(StationUpdateError::Pending),
            result if transaction.reversing => {
                result?;
                control.plan = transaction.candidate.previous.clone();
                storage.current = storage
                    .transaction
                    .take()
                    .map(|transaction| transaction.candidate.reverse);
                storage.revision = storage.revision.wrapping_add(1);
                return Ok(());
            }
            _ => {}
        }
        transaction.reversing = true;
        // Both owners must adopt the retained reverse ports, even if a forward
        // half rejected the update. Reapplying its original configuration keeps
        // state intact and prevents any port from borrowing the retired owner.
        self.updates.publish(&transaction.candidate.reverse);
        self.apply_stopped_update();
        self.updates.result()?;
        control.plan = transaction.candidate.previous.clone();
        storage.current = storage
            .transaction
            .take()
            .map(|transaction| transaction.candidate.reverse);
        storage.revision = storage.revision.wrapping_add(1);
        Ok(())
    }

    fn apply_stopped_update(&self) {
        if self.running {
            return;
        }
        // SAFETY: successful stop synchronously quiesces both callback owners;
        // before first start they have never been published to a running stream.
        let receive = unsafe { &mut *self._receive_context.get() };
        // SAFETY: the independently owned TX callback is quiescent too.
        let transmit = unsafe { &mut *self._transmit_context.get() };
        self.updates.receive(
            &mut receive.station,
            &mut receive.controller_ctcss,
            &mut receive.voter_reporting,
        );
        self.updates.transmit(&mut transmit.station);
    }
}
