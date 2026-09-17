use super::*;

/// Validated process-lifetime ABI-4 radio capability.
#[derive(Clone, Copy)]
pub struct RadioProvider {
    functions: Functions,
}

impl RadioProvider {
    /// Validate and copy the required prefix of a process-lifetime descriptor.
    ///
    /// # Safety
    ///
    /// A null or misaligned `raw_descriptor` is rejected without dereferencing
    /// it. Any non-null, correctly aligned pointer must expose a readable header,
    /// capability C string, and all advertised descriptor bytes. Those bytes
    /// must remain immutable, and the shared object containing every advertised
    /// function must remain loaded while any returned provider or session
    /// exists. Each function must implement the documented ABI contract,
    /// including its threading, ownership, and no-unwind requirements.
    pub unsafe fn from_raw_descriptor(raw_descriptor: *const c_void) -> Result<Self, RadioError> {
        if raw_descriptor.is_null() || (raw_descriptor as usize) % align_of::<RawDescriptor>() != 0
        {
            return Err(RadioError::IncompatibleAdapter);
        }
        let header = raw_descriptor.cast::<RawDescriptorHeader>();
        // SAFETY: the caller guarantees a correctly aligned readable header.
        let header = unsafe { ptr::read(header) };
        if (header.struct_size as usize) < REQUIRED_DESCRIPTOR_SIZE
            || header.abi_version != ABI_VERSION
            || header.capability_name.is_null()
        {
            return Err(RadioError::IncompatibleAdapter);
        }
        // SAFETY: the caller guarantees the advertised descriptor bytes and C
        // string are readable for the process lifetime.
        if unsafe { CStr::from_ptr(header.capability_name) } != CAPABILITY {
            return Err(RadioError::IncompatibleAdapter);
        }
        // SAFETY: the validated size covers the complete ABI-4 prefix.
        let descriptor = unsafe { ptr::read(raw_descriptor.cast::<RawDescriptor>()) };
        let (
            Some(create),
            Some(warm),
            Some(receive),
            Some(transmit),
            Some(snapshot),
            Some(pop_receive_event),
            Some(pop_transmit_event),
            Some(destroy),
        ) = (
            descriptor.session_create,
            descriptor.session_warm,
            descriptor.session_receive,
            descriptor.session_transmit,
            descriptor.session_snapshot,
            descriptor.session_pop_receive_event,
            descriptor.session_pop_transmit_event,
            descriptor.session_destroy,
        )
        else {
            return Err(RadioError::IncompatibleAdapter);
        };
        Ok(Self {
            functions: Functions {
                create,
                warm,
                receive,
                transmit,
                snapshot,
                pop_receive_event,
                pop_transmit_event,
                destroy,
            },
        })
    }

    /// Create, preallocate, and warm one stopped session on the control plane.
    ///
    /// The borrowed ports remain owned by the caller and must outlive all three
    /// endpoints. Neither live callback is entered before [`PreparedSession::split`].
    pub fn prepare<'a>(
        self,
        config: &SessionConfig,
        ports: SessionPorts<'a>,
    ) -> Result<PreparedSession<'a>, RadioError> {
        validate_config(config)?;
        let raw_config = config.as_raw();
        let raw_ports = ports.as_raw();
        let mut session = ptr::null_mut();
        // SAFETY: the complete raw values remain live for the synchronous
        // creation call, and validation established every function address.
        let create_result =
            unsafe { (self.functions.create)(&raw_config, &raw_ports, &mut session) };
        if create_result != RESULT_OK {
            if let Some(partial) = NonNull::new(session) {
                // SAFETY: a non-null partial handle was returned by this exact
                // descriptor and is no longer reachable after this call.
                unsafe { (self.functions.destroy)(partial.as_ptr()) };
            }
            return Err(error_from_result(create_result));
        }
        let session = NonNull::new(session).ok_or(RadioError::AdapterFailure)?;
        let inner = SessionInner {
            functions: self.functions,
            session,
            generation_id: config.generation_id,
            maximum_receive_frames: config.maximum_receive_frame_count,
            maximum_transmit_frames: config.maximum_transmit_frame_count,
            _ports: PhantomData,
        };
        // SAFETY: the newly created stopped handle is exclusively owned here;
        // warming is a synchronous control-plane operation.
        map_result(unsafe { (inner.functions.warm)(inner.session.as_ptr()) })?;
        Ok(PreparedSession {
            inner: Arc::new(inner),
        })
    }
}

struct SessionInner<'a> {
    functions: Functions,
    session: NonNull<OpaqueSession>,
    generation_id: u64,
    maximum_receive_frames: u32,
    maximum_transmit_frames: u32,
    _ports: PhantomData<&'a mut c_void>,
}

impl Drop for SessionInner<'_> {
    fn drop(&mut self) {
        // SAFETY: Arc finalization proves the prepared session is no longer
        // reachable by either stopped callback endpoint or its observer.
        unsafe { (self.functions.destroy)(self.session.as_ptr()) };
    }
}

// SAFETY: the ABI permits the complete session ownership group to move between
// threads; borrowed-port Send requirements are part of their unsafe binding.
unsafe impl Send for SessionInner<'_> {}
// SAFETY: ABI 4 explicitly permits one receive owner, one transmit owner, and
// lock-free control observation concurrently; safe construction creates only
// those owners, and their mutable operations require exclusive endpoint access.
unsafe impl Sync for SessionInner<'_> {}

/// Warmed session whose persistent owners have not yet been assigned.
pub struct PreparedSession<'a> {
    inner: Arc<SessionInner<'a>>,
}

impl<'a> PreparedSession<'a> {
    /// Assign exactly one receive owner, transmit owner, and control observer.
    #[must_use]
    pub fn split(
        self,
    ) -> (
        ReceiveEndpoint<'a>,
        TransmitEndpoint<'a>,
        ControlObserver<'a>,
    ) {
        let receive = ReceiveEndpoint {
            inner: Arc::clone(&self.inner),
            _not_sync: Cell::new(()),
        };
        let transmit = TransmitEndpoint {
            inner: Arc::clone(&self.inner),
            _not_sync: Cell::new(()),
        };
        let observer = ControlObserver {
            inner: self.inner,
            _not_sync: Cell::new(()),
        };
        (receive, transmit, observer)
    }
}

/// Sole serial receive callback endpoint.
///
/// The endpoint is `Send` so it can move to its assigned callback thread, but
/// is deliberately not `Sync` and cannot be cloned.
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<usbradioplus_radio::ReceiveEndpoint<'static>>();
/// ```
pub struct ReceiveEndpoint<'a> {
    inner: Arc<SessionInner<'a>>,
    _not_sync: Cell<()>,
}

impl ReceiveEndpoint<'_> {
    /// Process one exact canonical-stereo capture span into mono receive PCM.
    ///
    /// `input` contains two interleaved normalized-F32 samples per frame and
    /// `output` contains exactly one sample per frame. On every wrapper- or
    /// adapter-detected failure, the complete output slice is silent. This
    /// method does not allocate, lock, block, log, or panic.
    pub fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        controls: ReceiveControls,
    ) -> Result<ReceiveResult, RadioError> {
        output.fill(0.0);
        let frames = receive_frame_count(input, output.len(), self.inner.maximum_receive_frames)?;
        let raw_controls = controls.as_raw();
        let mut raw_result = RawReceiveResult::default();
        // SAFETY: slices prove exact readable/writable bounds, this unique
        // endpoint serializes the receive worker, and the session is Arc-live.
        let status = unsafe {
            (self.inner.functions.receive)(
                self.inner.session.as_ptr(),
                input.as_ptr(),
                output.as_mut_ptr(),
                frames,
                &raw_controls,
                &mut raw_result,
            )
        };
        if let Err(error) = map_result(status) {
            output.fill(0.0);
            return Err(error);
        }
        match receive_result(raw_result, self.inner.generation_id, frames) {
            Ok(result) if output.iter().all(|sample| sample.is_finite()) => Ok(result),
            Ok(_) | Err(_) => {
                output.fill(0.0);
                Err(RadioError::AdapterFailure)
            }
        }
    }
}

// SAFETY: the endpoint owns the ABI's sole serial receive capability and its
// unsafe port bindings promise that their contexts can move with this owner.
unsafe impl Send for ReceiveEndpoint<'_> {}

/// Sole serial transmit callback endpoint.
///
/// The endpoint is `Send` so it can move to its assigned callback thread, but
/// is deliberately not `Sync` and cannot be cloned.
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<usbradioplus_radio::TransmitEndpoint<'static>>();
/// ```
pub struct TransmitEndpoint<'a> {
    inner: Arc<SessionInner<'a>>,
    _not_sync: Cell<()>,
}

impl TransmitEndpoint<'_> {
    /// Render one exact canonical-stereo normalized-F32 playback span.
    ///
    /// `output` contains exactly two interleaved samples per native frame. On
    /// every wrapper- or adapter-detected failure, the complete output slice is
    /// silent. This method does not allocate, lock, block, log, or panic.
    pub fn render(
        &mut self,
        output: &mut [f32],
        controls: TransmitControls,
    ) -> Result<TransmitResult, RadioError> {
        output.fill(0.0);
        let frames = transmit_frame_count(output.len(), self.inner.maximum_transmit_frames)?;
        let raw_controls = controls.as_raw();
        let mut raw_result = RawTransmitResult::default();
        // SAFETY: the slice proves the exact writable bound, this unique
        // endpoint serializes the transmit worker, and the session is Arc-live.
        let status = unsafe {
            (self.inner.functions.transmit)(
                self.inner.session.as_ptr(),
                output.as_mut_ptr(),
                frames,
                &raw_controls,
                &mut raw_result,
            )
        };
        if let Err(error) = map_result(status) {
            output.fill(0.0);
            return Err(error);
        }
        match transmit_result(raw_result, self.inner.generation_id, frames) {
            Ok(result) if output.iter().all(|sample| sample.is_finite()) => Ok(result),
            Ok(_) | Err(_) => {
                output.fill(0.0);
                Err(RadioError::AdapterFailure)
            }
        }
    }
}

// SAFETY: the endpoint owns the ABI's sole serial transmit capability and its
// unsafe port bindings promise that their contexts can move with this owner.
unsafe impl Send for TransmitEndpoint<'_> {}

/// Sole control-plane observer for snapshots and the two event queues.
///
/// It may move between control threads, but is not `Sync` because each event
/// queue has exactly one serial consumer.
pub struct ControlObserver<'a> {
    inner: Arc<SessionInner<'a>>,
    _not_sync: Cell<()>,
}

impl ControlObserver<'_> {
    /// Read the latest independently published lock-free diagnostic values.
    pub fn snapshot(&self) -> Result<SessionSnapshot, RadioError> {
        let mut raw = RawSnapshot::default();
        // SAFETY: the observer and session are live; the ABI permits this
        // read-only snapshot concurrently with both callback owners.
        map_result(unsafe {
            (self.inner.functions.snapshot)(self.inner.session.as_ptr(), &mut raw)
        })?;
        session_snapshot(raw, self.inner.generation_id)
    }

    /// Pop the next receive-owner event without waiting.
    pub fn pop_receive_event(&mut self) -> Result<Option<RadioEvent>, RadioError> {
        self.pop_event(EventOwner::Receive)
    }

    /// Pop the next transmit-owner event without waiting.
    pub fn pop_transmit_event(&mut self) -> Result<Option<RadioEvent>, RadioError> {
        self.pop_event(EventOwner::Transmit)
    }

    fn pop_event(&mut self, owner: EventOwner) -> Result<Option<RadioEvent>, RadioError> {
        let mut raw = RawEvent::default();
        let pop = match owner {
            EventOwner::Receive => self.inner.functions.pop_receive_event,
            EventOwner::Transmit => self.inner.functions.pop_transmit_event,
        };
        // SAFETY: mutable observer access serializes this queue consumer while
        // the ABI permits its callback owner to publish concurrently.
        match unsafe { pop(self.inner.session.as_ptr(), &mut raw) } {
            0 => Ok(None),
            1 => event(raw, self.inner.generation_id, owner).map(Some),
            _ => Err(RadioError::AdapterFailure),
        }
    }
}

// SAFETY: the observer owns both serial event consumers and can be transferred
// between control threads; Cell prevents concurrent sharing in safe Rust.
unsafe impl Send for ControlObserver<'_> {}

#[derive(Clone, Copy)]
pub(super) enum EventOwner {
    Receive,
    Transmit,
}

pub(super) fn validate_config(config: &SessionConfig) -> Result<(), RadioError> {
    let (ctcss, dcs) = match config.transmit.signaling {
        TransmitSignaling::Disabled => {
            (CtcssTransmitConfig::default(), DcsTransmitConfig::default())
        }
        TransmitSignaling::Ctcss(value) => (value, DcsTransmitConfig::default()),
        TransmitSignaling::Dcs(value) => (CtcssTransmitConfig::default(), value),
    };
    if config.maximum_receive_frame_count == 0
        || config.maximum_transmit_frame_count == 0
        || config.publication_interval_milliseconds == 0
        || !config.receive_input_gain.is_finite()
        || !config.receive.ctcss_decoder_gain.is_finite()
        || !ctcss.peak.is_finite()
        || !ctcss.turnoff_phase_shift_degrees.is_finite()
        || !ctcss.turnoff_tail_tone_hz.is_finite()
        || ctcss
            .mapped_frequencies_tenths_hz
            .iter()
            .any(|frequency| *frequency < 0)
        || !dcs.peak.is_finite()
        || !config.transmit.output_a.tone_gain.is_finite()
        || !config.transmit.output_a.tone_bias.is_finite()
        || !config.transmit.output_b.tone_gain.is_finite()
        || !config.transmit.output_b.tone_bias.is_finite()
    {
        return Err(RadioError::InvalidArgument);
    }
    Ok(())
}

pub(super) fn receive_frame_count(
    input: &[f32],
    output_length: usize,
    maximum: u32,
) -> Result<u32, RadioError> {
    if input.is_empty()
        || input.len() % CANONICAL_CHANNELS != 0
        || input.len() / CANONICAL_CHANNELS != output_length
        || input.iter().any(|sample| !sample.is_finite())
    {
        return Err(RadioError::InvalidArgument);
    }
    bounded_frame_count(output_length, maximum)
}

pub(super) fn transmit_frame_count(output_length: usize, maximum: u32) -> Result<u32, RadioError> {
    if output_length == 0 || output_length % CANONICAL_CHANNELS != 0 {
        return Err(RadioError::InvalidArgument);
    }
    bounded_frame_count(output_length / CANONICAL_CHANNELS, maximum)
}

pub(super) fn bounded_frame_count(frames: usize, maximum: u32) -> Result<u32, RadioError> {
    let frames = u32::try_from(frames).map_err(|_| RadioError::FrameCountExceeded)?;
    if frames > maximum {
        Err(RadioError::FrameCountExceeded)
    } else {
        Ok(frames)
    }
}

pub(super) fn bool_from_raw(value: u32) -> Result<bool, RadioError> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(RadioError::AdapterFailure),
    }
}

pub(super) fn ctcss_from_raw(value: i32) -> Result<Option<CtcssToneIndex>, RadioError> {
    if value < 0 {
        return Ok(None);
    }
    let index = u8::try_from(value).map_err(|_| RadioError::AdapterFailure)?;
    CtcssToneIndex::new(index)
        .map(Some)
        .ok_or(RadioError::AdapterFailure)
}

pub(super) fn ring_is_valid(ring: RingObservation) -> bool {
    ring.ratio.is_finite()
}

pub(super) fn receive_result(
    raw: RawReceiveResult,
    generation_id: u64,
    frames: u32,
) -> Result<ReceiveResult, RadioError> {
    if raw.generation_id != generation_id
        || raw.frame_count != frames
        || ![
            raw.input_peak,
            raw.input_rms,
            raw.ctcss_decoder_peak,
            raw.output_peak,
            raw.output_rms,
        ]
        .iter()
        .all(|value| value.is_finite())
    {
        return Err(RadioError::AdapterFailure);
    }
    Ok(ReceiveResult {
        generation_id: raw.generation_id,
        first_sample_index: raw.first_sample_index,
        frame_count: raw.frame_count,
        carrier_active: bool_from_raw(raw.carrier_active)?,
        subaudible_active: bool_from_raw(raw.subaudible_active)?,
        receiver_keyed: bool_from_raw(raw.receiver_keyed)?,
        ctcss_decoded: ctcss_from_raw(raw.ctcss_decoded_index)?,
        dcs_valid: bool_from_raw(raw.dcs_valid)?,
        rssi_peak: raw.rssi_peak,
        rssi_updated: bool_from_raw(raw.rssi_updated)?,
        ctcss_decoder_peak: raw.ctcss_decoder_peak,
        input_peak: raw.input_peak,
        input_rms: raw.input_rms,
        input_rail_samples: raw.input_rail_samples,
        output_peak: raw.output_peak,
        output_rms: raw.output_rms,
        output_rail_samples: raw.output_rail_samples,
        periodic_status_due: bool_from_raw(raw.periodic_status_due)?,
    })
}

pub(super) fn transmitter_state(value: i32) -> Result<TransmitterState, RadioError> {
    match value {
        0 => Ok(TransmitterState::Idle),
        1 => Ok(TransmitterState::Active),
        2 => Ok(TransmitterState::ToneOff),
        4 => Ok(TransmitterState::Finishing),
        5 => Ok(TransmitterState::Complete),
        _ => Err(RadioError::AdapterFailure),
    }
}

pub(super) fn transmit_result(
    raw: RawTransmitResult,
    generation_id: u64,
    frames: u32,
) -> Result<TransmitResult, RadioError> {
    if raw.generation_id != generation_id
        || raw.frame_count != frames
        || ![
            raw.program_peak,
            raw.program_rms,
            raw.output_peak,
            raw.output_rms,
        ]
        .iter()
        .all(|value| value.is_finite())
        || !ring_is_valid(raw.program_ring)
    {
        return Err(RadioError::AdapterFailure);
    }
    Ok(TransmitResult {
        generation_id: raw.generation_id,
        first_sample_index: raw.first_sample_index,
        frame_count: raw.frame_count,
        logical_ptt: bool_from_raw(raw.logical_ptt)?,
        transmitter_state: transmitter_state(raw.transmitter_state)?,
        selected_ctcss_tenths_hz: raw.selected_ctcss_tenths_hz,
        program_peak: raw.program_peak,
        program_rms: raw.program_rms,
        program_rail_samples: raw.program_rail_samples,
        output_peak: raw.output_peak,
        output_rms: raw.output_rms,
        output_rail_samples: raw.output_rail_samples,
        periodic_status_due: bool_from_raw(raw.periodic_status_due)?,
        program_ring: raw.program_ring,
    })
}

pub(super) fn event(
    raw: RawEvent,
    generation_id: u64,
    owner: EventOwner,
) -> Result<RadioEvent, RadioError> {
    if raw.generation_id != generation_id {
        return Err(RadioError::AdapterFailure);
    }
    let value = match (owner, raw.kind) {
        (EventOwner::Receive, 1) => EventValue::Carrier(bool_from_event(raw.value)?),
        (EventOwner::Receive, 2) => EventValue::Subaudible(bool_from_event(raw.value)?),
        (EventOwner::Receive, 3) => EventValue::ReceiverKeyed(bool_from_event(raw.value)?),
        (EventOwner::Receive, 4) => EventValue::CtcssDecode(ctcss_from_raw(raw.value)?),
        (EventOwner::Receive, 5) => EventValue::DcsDecode(bool_from_event(raw.value)?),
        (EventOwner::Transmit, 6) => EventValue::Ptt(bool_from_event(raw.value)?),
        (EventOwner::Transmit, 7) => EventValue::CtcssTransmit(raw.value),
        (EventOwner::Transmit, 8) => EventValue::ReceiverBlanking(
            u32::try_from(raw.value).map_err(|_| RadioError::AdapterFailure)?,
        ),
        (EventOwner::Receive | EventOwner::Transmit, 9) if raw.value == 1 => {
            EventValue::ProviderFailure
        }
        _ => return Err(RadioError::AdapterFailure),
    };
    Ok(RadioEvent {
        generation_id: raw.generation_id,
        sample_index: raw.sample_index,
        value,
    })
}

pub(super) fn bool_from_event(value: i32) -> Result<bool, RadioError> {
    u32::try_from(value)
        .map_err(|_| RadioError::AdapterFailure)
        .and_then(bool_from_raw)
}

pub(super) fn session_snapshot(
    raw: RawSnapshot,
    generation_id: u64,
) -> Result<SessionSnapshot, RadioError> {
    if raw.generation_id != generation_id
        || ![
            raw.receive_input_peak,
            raw.receive_input_rms,
            raw.receive_ctcss_decoder_peak,
            raw.receive_output_peak,
            raw.receive_output_rms,
            raw.transmit_program_peak,
            raw.transmit_program_rms,
            raw.transmit_output_peak,
            raw.transmit_output_rms,
        ]
        .iter()
        .all(|value| value.is_finite())
        || !ring_is_valid(raw.program_ring)
    {
        return Err(RadioError::AdapterFailure);
    }
    Ok(SessionSnapshot {
        generation_id: raw.generation_id,
        receive_frames: raw.receive_frames,
        transmit_frames: raw.transmit_frames,
        receive_input_peak: raw.receive_input_peak,
        receive_input_rms: raw.receive_input_rms,
        receive_ctcss_decoder_peak: raw.receive_ctcss_decoder_peak,
        receive_output_peak: raw.receive_output_peak,
        receive_output_rms: raw.receive_output_rms,
        transmit_program_peak: raw.transmit_program_peak,
        transmit_program_rms: raw.transmit_program_rms,
        transmit_output_peak: raw.transmit_output_peak,
        transmit_output_rms: raw.transmit_output_rms,
        receive_input_rail_samples: raw.receive_input_rail_samples,
        receive_output_rail_samples: raw.receive_output_rail_samples,
        transmit_program_rail_samples: raw.transmit_program_rail_samples,
        transmit_output_rail_samples: raw.transmit_output_rail_samples,
        provider_failures: raw.provider_failures,
        receive_event_drops: raw.receive_event_drops,
        transmit_event_drops: raw.transmit_event_drops,
        carrier_active: bool_from_raw(raw.carrier_active)?,
        subaudible_active: bool_from_raw(raw.subaudible_active)?,
        receiver_keyed: bool_from_raw(raw.receiver_keyed)?,
        logical_ptt: bool_from_raw(raw.logical_ptt)?,
        ctcss_decoded: ctcss_from_raw(raw.ctcss_decoded_index)?,
        dcs_valid: bool_from_raw(raw.dcs_valid)?,
        program_ring: raw.program_ring,
    })
}

pub(super) fn map_result(result: c_int) -> Result<(), RadioError> {
    if result == RESULT_OK {
        Ok(())
    } else {
        Err(error_from_result(result))
    }
}

pub(super) fn error_from_result(result: c_int) -> RadioError {
    match result {
        RESULT_INVALID_ARGUMENT => RadioError::InvalidArgument,
        RESULT_PROVIDER_FAILED => RadioError::ProviderFailed,
        RESULT_FRAME_COUNT_EXCEEDED => RadioError::FrameCountExceeded,
        RESULT_UNSUPPORTED => RadioError::Unsupported,
        RESULT_NOT_READY => RadioError::NotReady,
        RESULT_BUSY => RadioError::Busy,
        _ => RadioError::AdapterFailure,
    }
}
