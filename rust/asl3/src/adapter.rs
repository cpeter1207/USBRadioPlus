//! Complete media and compatibility state for one ASL3 channel.

use std::fmt;

use crate::PublishOutcome;
use crate::control::{ControlAction, ControlMessage, ControlSnapshot, CtcssTone};
use crate::handoff::{Consumer, HandoffError, HandoffObservation, LatestHandoff, Producer};
use crate::pcm::{
    ADVANCED_FRAME_SAMPLES, APP_RPT_FRAME_SAMPLES, AsteriskPcmMode, ControllerPcmFrame,
    FrameAssembler, PcmBoundaryError, f32_to_s16, s16_to_f32,
};

const NATIVE_FRAME_SAMPLES: usize = ADVANCED_FRAME_SAMPLES;
const STATUS_CAPACITY: usize = 16;
const CTCSS_TONE_COUNT: u8 = 38;
/// Largest retained legacy echo recording, matching the established 20-second limit.
pub const MAX_ECHO_FRAMES: u16 = 1_000;

/// Counts returned by one streaming 48-to-8-kHz conversion operation.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ConversionProgress {
    /// Native input samples consumed from the submitted prefix.
    pub input_used: usize,
    /// App-rate samples generated into the output prefix.
    pub output_generated: usize,
}

/// Failure reported by the prepared released sample-rate adapter port.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ConversionError;

impl fmt::Display for ConversionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("app_rpt sample-rate conversion failed")
    }
}

impl std::error::Error for ConversionError {}

/// Owned port to one prepared released mono sample-rate converter.
///
/// Product composition must back this port with the released samplerate
/// adapter. The operation runs in the native callback and therefore must not
/// allocate, lock, log, or perform I/O.
pub trait AppRptConverter: Send {
    /// Convert a continuing native-rate input prefix into app_rpt-rate output.
    fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
    ) -> Result<ConversionProgress, ConversionError>;
}

/// Failure reported by the sole prepared program-ring producer port.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProgramProducerError;

impl fmt::Display for ProgramProducerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("program-ring producer failed")
    }
}

impl std::error::Error for ProgramProducerError {}

/// Owned producer port for the sole rate-adjusting transmitter program ring.
///
/// Implementations must preserve chronological-prefix acceptance and must not
/// add another audio input or ring. `push` runs on the audio path and must use
/// prepared storage without allocating, locking, logging, or performing I/O.
pub trait ProgramProducer: Send {
    /// Append a chronological prefix with the latest delivered RX state.
    ///
    /// The released ring does not report source consumption through its rate
    /// converter. The implementation therefore updates one atomic current-state
    /// snapshot when at least one sample is accepted, and returns that snapshot
    /// with subsequent renders. Zero acceptance and errors leave it unchanged.
    fn push(
        &mut self,
        input: &[f32],
        receive: ReceiveQualification,
    ) -> Result<usize, ProgramProducerError>;
}

/// Valid zero-based index into the radio core's fixed CTCSS table.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssToneIndex(u8);

impl CtcssToneIndex {
    /// Construct one of the 38 fixed decoded-tone indexes.
    pub const fn new(index: u8) -> Option<Self> {
        if index < CTCSS_TONE_COUNT {
            Some(Self(index))
        } else {
            None
        }
    }

    /// Zero-based table index.
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Sample-associated receiver state returned by the native radio callback.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ReceiveQualification {
    /// Resolved carrier state.
    pub carrier_active: bool,
    /// Resolved CTCSS/DCS or external subaudible state.
    pub subaudible_active: bool,
    /// Fully qualified receiver state.
    pub receiver_keyed: bool,
    /// Decoded CTCSS table index, when one is aligned with this span.
    pub ctcss_decoded: Option<CtcssToneIndex>,
    /// Whether the configured DCS code is aligned and valid for this span.
    pub dcs_valid: bool,
}

/// Legacy echo setup allocated before audio processing starts.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EchoConfiguration {
    enabled: bool,
    maximum_frames: u16,
}

impl EchoConfiguration {
    #[cfg(test)]
    const fn new(enabled: bool, maximum_frames: u16) -> Option<Self> {
        if maximum_frames <= MAX_ECHO_FRAMES {
            Some(Self {
                enabled,
                maximum_frames,
            })
        } else {
            None
        }
    }

    /// Disable echo without retaining audio storage.
    pub const fn disabled() -> Self {
        Self {
            enabled: false,
            maximum_frames: 0,
        }
    }
}

impl Default for EchoConfiguration {
    fn default() -> Self {
        Self {
            enabled: false,
            maximum_frames: MAX_ECHO_FRAMES,
        }
    }
}

/// A status message latched with native callback progress.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReceiveStatus {
    /// Transmit CTCSS became ready for its controller text notification.
    TransmitCtcssReady(CtcssTone),
    /// Legacy voter RSSI report in the established zero-through-1000 scale.
    VoterRssi(u16),
}

/// Receiver state accompanying one native callback span.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ReceiveMetadata {
    /// Native callback qualification aligned with these samples.
    pub qualification: ReceiveQualification,
    /// Controller-facing qualified receive CTCSS frequency, if present.
    pub controller_ctcss: Option<CtcssTone>,
    /// At most one new event of each retained status kind.
    pub statuses: [Option<ReceiveStatus>; 2],
}

#[derive(Clone, Copy)]
struct StatusJournal {
    events: [ReceiveStatus; STATUS_CAPACITY],
    produced: u64,
}

impl StatusJournal {
    const fn new() -> Self {
        Self {
            events: [ReceiveStatus::VoterRssi(0); STATUS_CAPACITY],
            produced: 0,
        }
    }

    fn push(&mut self, status: ReceiveStatus) {
        self.events[self.produced as usize % STATUS_CAPACITY] = status;
        self.produced = self.produced.wrapping_add(1);
    }

    fn oldest_sequence(self) -> u64 {
        self.produced.saturating_sub(STATUS_CAPACITY as u64)
    }

    fn get(self, sequence: u64) -> ReceiveStatus {
        self.events[sequence as usize % STATUS_CAPACITY]
    }
}

#[derive(Clone, Copy)]
struct ReceivePacket {
    frame: ControllerPcmFrame,
    qualification: ReceiveQualification,
    controller_ctcss: Option<CtcssTone>,
    statuses: StatusJournal,
}

enum ReceiveEgress {
    AppRpt(Box<dyn AppRptConverter>),
    Advanced,
}

/// Sole native-callback owner for receiver conversion and publication.
pub struct ReceivePublisher {
    mode: AsteriskPcmMode,
    egress: ReceiveEgress,
    assembler: FrameAssembler,
    native_remainder: usize,
    converted: [f32; NATIVE_FRAME_SAMPLES],
    quantized: [i16; NATIVE_FRAME_SAMPLES],
    statuses: StatusJournal,
    handoff: Producer<ReceivePacket>,
}

/// Result of one bounded callback publication attempt.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct PublishReport {
    /// Whether this callback completed one controller frame.
    pub completed_frame: bool,
    /// Handoff result when a frame was completed.
    pub handoff: Option<PublishOutcome>,
}

/// ASL3 setup failure before callback publication starts.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AdapterSetupError {
    /// The requested callback handoff depth is invalid.
    Handoff(HandoffError),
}

impl fmt::Display for AdapterSetupError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Handoff(error) => error.fmt(formatter),
        }
    }
}

impl std::error::Error for AdapterSetupError {}

impl From<HandoffError> for AdapterSetupError {
    fn from(error: HandoffError) -> Self {
        Self::Handoff(error)
    }
}

/// Media operation rejected at the safe adapter boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AdapterError {
    /// A native callback block was empty or exceeded the prepared bound.
    InvalidNativeFrame,
    /// The sample-rate adapter failed.
    SampleRate(ConversionError),
    /// The sample-rate adapter consumed no input despite ample output space.
    ConversionStalled,
    /// The sample-rate adapter returned counts beyond the supplied buffers.
    InvalidConversionResult,
    /// Fixed-frame assembly rejected a bounded operation.
    Pcm(PcmBoundaryError),
    /// A controller frame belongs to the other interface.
    WrongInterface,
    /// The program ring rejected an operation.
    ProgramRing(ProgramProducerError),
    /// The program ring reported accepting beyond the submitted span.
    InvalidProgramRingResult,
}

impl fmt::Display for AdapterError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidNativeFrame => formatter.write_str("invalid native receive frame"),
            Self::SampleRate(error) => error.fmt(formatter),
            Self::ConversionStalled => formatter.write_str("sample-rate conversion stalled"),
            Self::InvalidConversionResult => {
                formatter.write_str("sample-rate converter returned invalid counts")
            }
            Self::Pcm(error) => error.fmt(formatter),
            Self::WrongInterface => formatter.write_str("controller PCM interface mismatch"),
            Self::ProgramRing(error) => error.fmt(formatter),
            Self::InvalidProgramRingResult => {
                formatter.write_str("program ring accepted beyond submitted audio")
            }
        }
    }
}

impl std::error::Error for AdapterError {}

impl From<ConversionError> for AdapterError {
    fn from(error: ConversionError) -> Self {
        Self::SampleRate(error)
    }
}

impl From<PcmBoundaryError> for AdapterError {
    fn from(error: PcmBoundaryError) -> Self {
        Self::Pcm(error)
    }
}

impl From<ProgramProducerError> for AdapterError {
    fn from(error: ProgramProducerError) -> Self {
        Self::ProgramRing(error)
    }
}

impl ReceivePublisher {
    /// Convert one arbitrary bounded native mono span and publish at most one
    /// complete 20 ms controller frame without allocating or waiting.
    pub fn publish(
        &mut self,
        native_mono: &[f32],
        metadata: ReceiveMetadata,
    ) -> Result<PublishReport, AdapterError> {
        if native_mono.is_empty() || native_mono.len() > NATIVE_FRAME_SAMPLES {
            return Err(AdapterError::InvalidNativeFrame);
        }
        for status in metadata.statuses.into_iter().flatten() {
            self.statuses.push(status);
        }

        let mut offset = 0;
        let mut report = PublishReport::default();
        while offset < native_mono.len() {
            let count =
                (NATIVE_FRAME_SAMPLES - self.native_remainder).min(native_mono.len() - offset);
            self.convert_span(&native_mono[offset..offset + count])?;
            self.native_remainder += count;
            offset += count;
            if self.native_remainder == NATIVE_FRAME_SAMPLES {
                self.native_remainder = 0;
                let packet = ReceivePacket {
                    frame: self.take_frame(metadata.qualification.receiver_keyed)?,
                    qualification: metadata.qualification,
                    controller_ctcss: metadata.controller_ctcss,
                    statuses: self.statuses,
                };
                report.completed_frame = true;
                report.handoff = Some(self.handoff.push(packet));
            }
        }
        Ok(report)
    }

    /// Best-effort callback-handoff diagnostics.
    pub fn observe(&self) -> HandoffObservation {
        self.handoff.observe()
    }

    fn convert_span(&mut self, input: &[f32]) -> Result<(), AdapterError> {
        match &mut self.egress {
            ReceiveEgress::Advanced => {
                for (output, input) in self.quantized[..input.len()].iter_mut().zip(input) {
                    *output = f32_to_s16(*input);
                }
                self.assembler.append(&self.quantized[..input.len()])?;
            }
            ReceiveEgress::AppRpt(converter) => {
                let mut used = 0;
                while used < input.len() {
                    let result = converter.process(&input[used..], &mut self.converted)?;
                    if result.input_used > input.len() - used
                        || result.output_generated > self.converted.len()
                    {
                        return Err(AdapterError::InvalidConversionResult);
                    }
                    if result.input_used == 0 {
                        return Err(AdapterError::ConversionStalled);
                    }
                    used += result.input_used;
                    for (output, converted) in self.quantized[..result.output_generated]
                        .iter_mut()
                        .zip(&self.converted)
                    {
                        *output = f32_to_s16(*converted);
                    }
                    self.assembler
                        .append(&self.quantized[..result.output_generated])?;
                }
            }
        }
        Ok(())
    }

    fn take_frame(&mut self, keyed: bool) -> Result<ControllerPcmFrame, AdapterError> {
        match self.mode {
            AsteriskPcmMode::AppRpt => {
                let mut samples = [0; APP_RPT_FRAME_SAMPLES];
                self.assembler.take_padded(&mut samples)?;
                if !keyed {
                    samples.fill(0);
                }
                Ok(ControllerPcmFrame::app_rpt(samples))
            }
            AsteriskPcmMode::Advanced => {
                let mut samples = [0; ADVANCED_FRAME_SAMPLES];
                self.assembler.take_padded(&mut samples)?;
                if !keyed {
                    samples.fill(0);
                }
                Ok(ControllerPcmFrame::advanced(samples))
            }
        }
    }
}

#[derive(Clone, Copy)]
enum DeliveryPhase {
    ReceiverState,
    Status,
    Voice,
}

#[derive(Clone, Copy)]
struct PendingDelivery {
    packet: ReceivePacket,
    phase: DeliveryPhase,
}

/// One typed operation for the thin Asterisk shim.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum DeliveryAction {
    /// Queue an Asterisk radio-key control frame.
    ReceiverKey {
        /// Qualified receive CTCSS frequency for the frame payload.
        ctcss: Option<CtcssTone>,
    },
    /// Queue an Asterisk radio-unkey control frame.
    ReceiverUnkey,
    /// Queue the fixed signed-16 frame written to the caller's voice workspace.
    Voice,
    /// Queue the retained `cstx=` notification.
    TransmitCtcssReady(CtcssTone),
    /// Queue the retained `R value` voter notification.
    VoterRssi(u16),
}

/// Cumulative bounded status-journal delivery diagnostics.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct StatusObservation {
    /// Next event sequence expected by the delivery owner.
    pub next_sequence: u64,
    /// Events skipped after exceeding the fixed journal capacity.
    pub discarded: u64,
}

/// Kind of DTMF frame returned by Asterisk's legacy detector.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DtmfEventKind {
    /// Start of one detected digit.
    Begin,
    /// End of one detected digit.
    End,
}

/// One detector result translated out of the Asterisk frame representation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DtmfEvent {
    /// Detector frame kind.
    pub kind: DtmfEventKind,
    /// ASCII digit code supplied by the detector.
    pub digit: u8,
}

/// Compatibility decision returned to the Asterisk shim.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DtmfAction {
    /// The shim should not run the legacy detector for this interface or mode.
    PassVoice,
    /// Forward the first begin frame for this digit.
    ForwardBegin(u8),
    /// Forward an end frame with an owned monotonic duration.
    ForwardEnd {
        /// ASCII digit code.
        digit: u8,
        /// Elapsed duration in milliseconds.
        duration_ms: u64,
    },
    /// Replace the pseudo-digit frame with an Asterisk null frame.
    MutePseudoDigit,
    /// Free a repeated begin frame without queueing it.
    SuppressRepeatedBegin,
}

#[derive(Clone, Copy)]
enum EchoState {
    Disabled,
    Recording,
    Playing,
}

#[derive(Clone)]
struct LegacyEcho {
    state: EchoState,
    samples: Box<[i16]>,
    length: usize,
    playback: usize,
}

impl LegacyEcho {
    fn new(configuration: EchoConfiguration) -> Self {
        let sample_capacity = usize::from(configuration.maximum_frames) * APP_RPT_FRAME_SAMPLES;
        Self {
            state: if configuration.enabled {
                EchoState::Recording
            } else {
                EchoState::Disabled
            },
            samples: vec![0; sample_capacity].into_boxed_slice(),
            length: 0,
            playback: 0,
        }
    }

    fn enabled(&self) -> bool {
        !matches!(self.state, EchoState::Disabled)
    }

    fn playing(&self) -> bool {
        matches!(self.state, EchoState::Playing)
    }

    fn set_enabled(&mut self, enabled: bool) {
        self.clear();
        self.state = if enabled {
            EchoState::Recording
        } else {
            EchoState::Disabled
        };
    }

    fn clear(&mut self) {
        self.samples[..self.length].fill(0);
        self.length = 0;
        self.playback = 0;
    }

    fn receive(&mut self, frame: &ControllerPcmFrame, keyed: bool) {
        if !matches!(self.state, EchoState::Recording) {
            return;
        }
        if keyed && frame.mode() == AsteriskPcmMode::AppRpt {
            let samples = frame.samples();
            let copied = samples.len().min(self.samples.len() - self.length);
            self.samples[self.length..self.length + copied].copy_from_slice(&samples[..copied]);
            self.length += copied;
        } else if self.length != 0 {
            self.state = EchoState::Playing;
            self.playback = 0;
        }
    }

    fn finish_playback_frame(&mut self, count: usize) {
        self.playback += count;
        if self.playback >= self.length {
            self.clear();
            self.state = EchoState::Recording;
        }
    }
}

#[derive(Clone, Copy)]
struct DtmfState {
    enabled: bool,
    active_since_ms: Option<u64>,
}

impl DtmfState {
    const fn disabled() -> Self {
        Self {
            enabled: false,
            active_since_ms: None,
        }
    }

    const fn enabled() -> Self {
        Self {
            enabled: true,
            active_since_ms: None,
        }
    }

    fn handle(&mut self, event: DtmfEvent, now_ms: u64) -> DtmfAction {
        if !self.enabled {
            return DtmfAction::PassVoice;
        }
        if event.digit == b'm' || event.digit == b'u' {
            return DtmfAction::MutePseudoDigit;
        }
        match event.kind {
            DtmfEventKind::Begin if self.active_since_ms.is_some() => {
                DtmfAction::SuppressRepeatedBegin
            }
            DtmfEventKind::Begin => {
                self.active_since_ms = Some(now_ms);
                DtmfAction::ForwardBegin(event.digit)
            }
            DtmfEventKind::End => DtmfAction::ForwardEnd {
                digit: event.digit,
                duration_ms: self
                    .active_since_ms
                    .take()
                    .map_or(0, |started| now_ms.saturating_sub(started)),
            },
        }
    }
}

/// Source selected for one program-ring write.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ProgramSource {
    /// Continuous controller program audio.
    Controller,
    /// One frame of retained legacy echo playback.
    LegacyEcho,
}

/// Result of one program-ring write.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProgramWrite {
    /// Selected source for this controller tick.
    pub source: ProgramSource,
    /// Submitted source samples.
    pub submitted_samples: usize,
    /// Samples accepted by the bounded ring.
    pub accepted_samples: usize,
    /// Whether this write drained the echo recording.
    pub echo_completed: bool,
}

/// Serialized non-real-time owner for controller delivery and program audio.
pub struct ControllerState {
    mode: AsteriskPcmMode,
    receive: Consumer<ReceivePacket>,
    pending: Option<PendingDelivery>,
    delivered_keyed: bool,
    status: StatusObservation,
    program: Box<dyn ProgramProducer>,
    program_f32: [f32; ADVANCED_FRAME_SAMPLES],
    echo: LegacyEcho,
    dtmf: DtmfState,
    control: ControlSnapshot,
    latest_receive: ReceiveQualification,
}

/// Controller-owned state retained while an active station generation is replaced.
///
/// This opaque value carries only state whose lifetime is independent of the
/// retired native callbacks. Receive frames must be delivered before capture
/// because their handoff remains paired with the retiring publisher.
#[derive(Clone)]
pub struct ControllerReloadState {
    mode: AsteriskPcmMode,
    delivered_keyed: bool,
    echo: LegacyEcho,
    dtmf: DtmfState,
    control: ControlSnapshot,
    latest_receive: ReceiveQualification,
}

impl ControllerState {
    /// Prepare the ordinary 8 kHz app_rpt interface and split callback from
    /// serialized delivery ownership.
    pub fn prepare_app_rpt(
        converter: Box<dyn AppRptConverter>,
        program: Box<dyn ProgramProducer>,
        handoff_slots: usize,
        echo: EchoConfiguration,
    ) -> Result<(ReceivePublisher, Self), AdapterSetupError> {
        Self::prepare(
            AsteriskPcmMode::AppRpt,
            ReceiveEgress::AppRpt(converter),
            program,
            handoff_slots,
            echo,
        )
    }

    /// Prepare the native fixed-48-kHz rpt_advanced interface and split
    /// callback from serialized delivery ownership.
    pub fn prepare_advanced(
        program: Box<dyn ProgramProducer>,
        handoff_slots: usize,
    ) -> Result<(ReceivePublisher, Self), AdapterSetupError> {
        Self::prepare(
            AsteriskPcmMode::Advanced,
            ReceiveEgress::Advanced,
            program,
            handoff_slots,
            EchoConfiguration::disabled(),
        )
    }

    fn prepare(
        mode: AsteriskPcmMode,
        egress: ReceiveEgress,
        program: Box<dyn ProgramProducer>,
        handoff_slots: usize,
        echo: EchoConfiguration,
    ) -> Result<(ReceivePublisher, Self), AdapterSetupError> {
        let (publisher, consumer) = LatestHandoff::split(handoff_slots)?;
        Ok((
            ReceivePublisher {
                mode,
                egress,
                assembler: FrameAssembler::new(mode),
                native_remainder: 0,
                converted: [0.0; NATIVE_FRAME_SAMPLES],
                quantized: [0; NATIVE_FRAME_SAMPLES],
                statuses: StatusJournal::new(),
                handoff: publisher,
            },
            Self {
                mode,
                receive: consumer,
                pending: None,
                delivered_keyed: false,
                status: StatusObservation::default(),
                program,
                program_f32: [0.0; ADVANCED_FRAME_SAMPLES],
                echo: LegacyEcho::new(echo),
                dtmf: if mode == AsteriskPcmMode::AppRpt {
                    DtmfState::enabled()
                } else {
                    DtmfState::disabled()
                },
                control: ControlSnapshot::default(),
                latest_receive: ReceiveQualification::default(),
            },
        ))
    }

    /// Return the next ordered Asterisk operation without blocking.
    ///
    /// Receiver edges precede associated status messages, which precede voice.
    pub fn next_action(&mut self, voice: &mut ControllerPcmFrame) -> Option<DeliveryAction> {
        loop {
            if self.pending.is_none() {
                self.pending = self.receive.pop().map(|packet| PendingDelivery {
                    packet,
                    phase: DeliveryPhase::ReceiverState,
                });
            }
            let pending = self.pending.as_mut()?;
            match pending.phase {
                DeliveryPhase::ReceiverState => {
                    pending.phase = DeliveryPhase::Status;
                    self.latest_receive = pending.packet.qualification;
                    if self.delivered_keyed != pending.packet.qualification.receiver_keyed {
                        self.delivered_keyed = pending.packet.qualification.receiver_keyed;
                        return Some(if pending.packet.qualification.receiver_keyed {
                            DeliveryAction::ReceiverKey {
                                ctcss: pending.packet.controller_ctcss,
                            }
                        } else {
                            DeliveryAction::ReceiverUnkey
                        });
                    }
                }
                DeliveryPhase::Status => {
                    let oldest = pending.packet.statuses.oldest_sequence();
                    if self.status.next_sequence < oldest {
                        self.status.discarded += oldest - self.status.next_sequence;
                        self.status.next_sequence = oldest;
                    }
                    if self.status.next_sequence < pending.packet.statuses.produced {
                        let event = pending.packet.statuses.get(self.status.next_sequence);
                        self.status.next_sequence += 1;
                        return Some(match event {
                            ReceiveStatus::TransmitCtcssReady(tone) => {
                                DeliveryAction::TransmitCtcssReady(tone)
                            }
                            ReceiveStatus::VoterRssi(value) => DeliveryAction::VoterRssi(value),
                        });
                    }
                    pending.phase = DeliveryPhase::Voice;
                }
                DeliveryPhase::Voice => {
                    let packet = pending.packet;
                    self.pending = None;
                    self.echo
                        .receive(&packet.frame, packet.qualification.receiver_keyed);
                    *voice = packet.frame;
                    return Some(DeliveryAction::Voice);
                }
            }
        }
    }

    /// Convert and append one continuous controller frame to the sole program
    /// ring, substituting one legacy echo frame only while echo is playing.
    pub fn write_program(
        &mut self,
        frame: &ControllerPcmFrame,
    ) -> Result<ProgramWrite, AdapterError> {
        if frame.mode() != self.mode {
            return Err(AdapterError::WrongInterface);
        }
        let (source, sample_count) = if self.echo.playing() {
            (ProgramSource::LegacyEcho, APP_RPT_FRAME_SAMPLES)
        } else {
            (ProgramSource::Controller, frame.samples().len())
        };
        for index in 0..sample_count {
            let sample = if source == ProgramSource::LegacyEcho {
                self.echo.samples[self.echo.playback + index]
            } else {
                frame.samples()[index]
            };
            self.program_f32[index] = s16_to_f32(sample);
        }
        let accepted = self
            .program
            .push(&self.program_f32[..sample_count], self.latest_receive)?;
        if accepted > sample_count {
            return Err(AdapterError::InvalidProgramRingResult);
        }
        let echo_completed = source == ProgramSource::LegacyEcho
            && self.echo.playback + sample_count >= self.echo.length;
        if source == ProgramSource::LegacyEcho {
            self.echo.finish_playback_frame(sample_count);
        }
        Ok(ProgramWrite {
            source,
            submitted_samples: sample_count,
            accepted_samples: accepted,
            echo_completed,
        })
    }

    /// Apply one radio-specific controller message and return its typed product action.
    pub fn apply_control(&mut self, message: ControlMessage) -> ControlAction {
        self.control.apply(message)
    }

    /// Current controller-owned radio-control state.
    pub const fn control_snapshot(&self) -> ControlSnapshot {
        self.control
    }

    /// Latest receiver qualification associated with controller delivery and program input.
    pub const fn latest_receive_qualification(&self) -> ReceiveQualification {
        self.latest_receive
    }

    /// Enable or disable app_rpt legacy DTMF detection.
    ///
    /// rpt_advanced always owns its digit policy and remains disabled here.
    pub fn set_dtmf_detection(&mut self, enabled: bool) {
        if self.mode == AsteriskPcmMode::AppRpt {
            self.dtmf.enabled = enabled;
            if !enabled {
                self.dtmf.active_since_ms = None;
            }
        }
    }

    /// Whether the C shim should invoke its unavoidable Asterisk DTMF detector.
    pub const fn dtmf_detection_enabled(&self) -> bool {
        self.dtmf.enabled
    }

    /// Apply a translated legacy detector result using a monotonic timestamp.
    pub fn handle_dtmf(&mut self, event: DtmfEvent, now_ms: u64) -> DtmfAction {
        self.dtmf.handle(event, now_ms)
    }

    /// Enable or disable legacy echo, clearing any recording or playback.
    ///
    /// This has no effect on rpt_advanced, which owns its own echo behavior.
    pub fn set_echo_enabled(&mut self, enabled: bool) {
        if self.mode == AsteriskPcmMode::AppRpt {
            self.echo.set_enabled(enabled);
        }
    }

    /// Whether app_rpt echo recording is enabled or playing.
    pub fn echo_enabled(&self) -> bool {
        self.echo.enabled()
    }

    /// Whether controller audio is currently suppressed for echo playback.
    pub fn echo_playing(&self) -> bool {
        self.echo.playing()
    }

    /// Best-effort callback-handoff diagnostics.
    pub fn handoff_observation(&self) -> HandoffObservation {
        self.receive.observe()
    }

    /// Cumulative status-journal delivery diagnostics.
    pub const fn status_observation(&self) -> StatusObservation {
        self.status
    }

    /// Capture state that remains meaningful across a prepared hot reload.
    ///
    /// The serialized owner must first drain [`Self::next_action`] so no
    /// packet belonging to the retiring receive publisher is abandoned.
    #[must_use]
    pub fn reload_state(&self) -> Option<ControllerReloadState> {
        if self.pending.is_some() || self.receive.observe().available != 0 {
            return None;
        }
        Some(ControllerReloadState {
            mode: self.mode,
            delivered_keyed: self.delivered_keyed,
            echo: self.echo.clone(),
            dtmf: self.dtmf,
            control: self.control,
            latest_receive: self.latest_receive,
        })
    }

    /// Restore controller state after replacing one compatible station generation.
    ///
    /// A state from a different Asterisk PCM interface is ignored because its
    /// echo and DTMF semantics are not interchangeable.
    pub fn restore_reload_state(&mut self, state: ControllerReloadState) {
        if self.mode != state.mode {
            return;
        }
        self.delivered_keyed = state.delivered_keyed;
        self.echo = state.echo;
        self.dtmf = state.dtmf;
        self.control = state.control;
        self.latest_receive = state.latest_receive;
    }
}

#[cfg(test)]
#[path = "adapter/tests.rs"]
mod tests;
