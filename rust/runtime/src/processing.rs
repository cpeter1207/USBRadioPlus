//! Control-plane ownership of prepared native processing providers.

use std::cell::UnsafeCell;
use std::ffi::{CString, c_int, c_void};
use std::fmt;
use std::ptr::NonNull;
use std::slice;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

use usbradioplus_core::{
    ChainRole, CtcssTone, GraphDescriptionError, GraphDescriptionFactory, NativeStreamSpec,
    PlFilter, ProcessingChain, ProcessingConfigError, StreamSpecError,
};
use usbradioplus_ffmpeg::{GraphError, GraphProvider, PreparedGraph};
use usbradioplus_radio::{CTCSS_TONE_COUNT, ProcessorPort, ProgramRingPort, SessionPorts};
use usbradioplus_rnnoise::{DenoiseError, DenoiseProvider, DenoiseStream};

const GRAPH_WARMUP_BLOCKS: usize = 8;
// One millisecond at the fixed native 48 kHz rate. The prime margin covers the
// causal IIR tail after alimiter; the same short span smooths the final handoff.
const GRAPH_TRANSITION_FRAMES: usize = 48;
// DenoiseStream deliberately withholds its first two live 480-sample frames.
const DENOISE_STARTUP_FRAMES: usize = 960;
const PORT_OK: c_int = 0;
const PORT_ERROR: c_int = -1;

/// Failure to validate or prepare one processing generation.
#[derive(Debug)]
pub enum ProcessingRuntimeError {
    /// A processing chain does not have its required source role.
    IncorrectChainRole,
    /// A graph description unexpectedly contains an interior NUL byte.
    InvalidGraphDescription,
    /// Typed processing validation failed.
    Configuration(ProcessingConfigError),
    /// FFmpeg graph-description construction failed.
    GraphDescription(GraphDescriptionError),
    /// The external FFmpeg adapter rejected setup.
    GraphAdapter(GraphError),
    /// The external RNNoise adapter rejected setup.
    DenoiseAdapter(DenoiseError),
    /// The fixed native stream specification is invalid.
    Stream(StreamSpecError),
}

impl fmt::Display for ProcessingRuntimeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncorrectChainRole => formatter.write_str("processing chain has the wrong role"),
            Self::InvalidGraphDescription => {
                formatter.write_str("FFmpeg graph contains an interior NUL byte")
            }
            Self::Configuration(error) => write!(formatter, "invalid processing settings: {error}"),
            Self::GraphDescription(error) => write!(formatter, "invalid FFmpeg graph: {error}"),
            Self::GraphAdapter(error) => write!(formatter, "FFmpeg adapter setup failed: {error}"),
            Self::DenoiseAdapter(error) => {
                write!(formatter, "RNNoise adapter setup failed: {error}")
            }
            Self::Stream(error) => write!(formatter, "invalid native stream: {error:?}"),
        }
    }
}

impl std::error::Error for ProcessingRuntimeError {}

impl From<ProcessingConfigError> for ProcessingRuntimeError {
    fn from(error: ProcessingConfigError) -> Self {
        Self::Configuration(error)
    }
}

impl From<GraphDescriptionError> for ProcessingRuntimeError {
    fn from(error: GraphDescriptionError) -> Self {
        Self::GraphDescription(error)
    }
}

impl From<GraphError> for ProcessingRuntimeError {
    fn from(error: GraphError) -> Self {
        Self::GraphAdapter(error)
    }
}

impl From<DenoiseError> for ProcessingRuntimeError {
    fn from(error: DenoiseError) -> Self {
        Self::DenoiseAdapter(error)
    }
}

impl From<StreamSpecError> for ProcessingRuntimeError {
    fn from(error: StreamSpecError) -> Self {
        Self::Stream(error)
    }
}

/// Immutable graph settings used to prepare one runtime generation.
#[derive(Clone, Debug, PartialEq)]
pub struct NativeProcessingPlan {
    /// Local-receiver processing settings.
    pub local: ProcessingChain,
    /// Final voice/telemetry and transmitter settings.
    pub voice_telemetry: ProcessingChain,
    /// Whether flat discriminator audio requires de-emphasis.
    pub deemphasis_enabled: bool,
    /// Receiver de-emphasis corner in Hz.
    pub deemphasis_corner_hz: f64,
    /// Whether transmitter pre-emphasis is active.
    pub preemphasis_enabled: bool,
    /// Transmitter pre-emphasis corner in Hz.
    pub preemphasis_corner_hz: f64,
}

/// Reusable control-plane factory for prepared processing generations.
#[derive(Clone)]
pub struct NativeProcessingFactory {
    descriptions: GraphDescriptionFactory,
    graph_provider: GraphProvider,
    denoise_provider: DenoiseProvider,
    stream: NativeStreamSpec,
}

impl NativeProcessingFactory {
    /// Construct a factory from process-lifetime adapter capabilities.
    pub fn new(
        graph_provider: GraphProvider,
        denoise_provider: DenoiseProvider,
        agc_plugin_path: impl Into<String>,
        maximum_frame_count: u32,
    ) -> Result<Self, ProcessingRuntimeError> {
        Ok(Self {
            descriptions: GraphDescriptionFactory::new(agc_plugin_path)?,
            graph_provider,
            denoise_provider,
            stream: NativeStreamSpec::new(48_000, maximum_frame_count)?,
        })
    }

    /// Borrow the validated graph-description factory for related processing setup.
    pub fn graph_descriptions(&self) -> &GraphDescriptionFactory {
        &self.descriptions
    }

    /// Prepare and warm every processor before publication to audio owners.
    pub fn prepare(
        &self,
        plan: &NativeProcessingPlan,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        validate_plan(plan)?;
        let maximum = self.stream.maximum_frame_count();
        let mut notches = std::array::from_fn(|_| None);
        let mut tail_notch = None;
        if plan.local.receive.pl_filter == PlFilter::DecodedToneNotch {
            for tone in CtcssTone::supported() {
                notches[tone.table_index()] = Some(self.prepare_graph(
                    self.descriptions.decoded_tone_notch(
                        f64::from(tone.as_hz()),
                        plan.local.receive.notch_width_hz,
                    )?,
                    maximum,
                )?);
            }
            // A 55 Hz tail closes CTCSS decode before the regular decoded-tone
            // port can be selected, so prepare its fixed rejection graph here.
            tail_notch = Some(
                self.prepare_graph(
                    self.descriptions
                        .decoded_tone_notch(55.0, plan.local.receive.notch_width_hz)?,
                    maximum,
                )?,
            );
        }

        Ok(ProcessingGeneration {
            receive_deemphasis: self.prepare_graph(
                self.descriptions
                    .receive_deemphasis(plan.deemphasis_enabled, plan.deemphasis_corner_hz)?,
                maximum,
            )?,
            receive_filter: self
                .prepare_graph(self.descriptions.receive_filter(&plan.local)?, maximum)?,
            receive_ctcss_notch: notches,
            receive_ctcss_tail_notch: tail_notch,
            receive_noise_reduction: plan
                .local
                .rnnoise_enabled
                .then(|| self.prepare_denoise(false))
                .transpose()?,
            receive_dynamics: self
                .prepare_graph(self.descriptions.dynamics(&plan.local)?, maximum)?,
            transmit_program: self.prepare_graph(
                self.descriptions.transmitter(
                    &plan.voice_telemetry,
                    plan.preemphasis_enabled,
                    plan.preemphasis_corner_hz,
                )?,
                maximum,
            )?,
            transmit_dcs_normal_filter: self
                .prepare_graph(self.descriptions.dcs_filter(false), maximum)?,
            transmit_dcs_turnoff_filter: self
                .prepare_graph(self.descriptions.dcs_filter(true), maximum)?,
        })
    }

    /// Prepare a replacement for the same serial receive and transmit owners.
    /// Unchanged stages retain their exact histories without being warmed again.
    /// New stages are prepared and silence-warmed on the control plane, then
    /// primed with real input behind the audible predecessor at handoff.
    ///
    /// # Safety
    /// `previous` must have been prepared with the same factory configuration
    /// and `previous_plan`.
    /// Old and replacement ports must retain the same sole RX or TX owner;
    /// they must never process a shared stage concurrently. Replacements are
    /// intended for a callback-boundary handoff, not a second radio session.
    pub unsafe fn prepare_replacement(
        &self,
        plan: &NativeProcessingPlan,
        previous_plan: &NativeProcessingPlan,
        previous: &ProcessingGeneration,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        if plan == previous_plan {
            // The admission contract supplies the already-validated old plan.
            // In particular, a control snapshot need not format the notch bank.
            return Ok(previous.clone_for_owner());
        }
        validate_plan(plan)?;
        let maximum = self.stream.maximum_frame_count();
        let mut notches = std::array::from_fn(|_| None);
        let mut tail_notch = None;
        if plan.local.receive.pl_filter == PlFilter::DecodedToneNotch {
            for tone in CtcssTone::supported() {
                let description = self.descriptions.decoded_tone_notch(
                    f64::from(tone.as_hz()),
                    plan.local.receive.notch_width_hz,
                )?;
                notches[tone.table_index()] = Some(
                    if let Some(old) = &previous.receive_ctcss_notch[tone.table_index()] {
                        self.replacement_graph(
                            description,
                            self.descriptions.decoded_tone_notch(
                                f64::from(tone.as_hz()),
                                previous_plan.local.receive.notch_width_hz,
                            )?,
                            old,
                            0,
                        )?
                    } else {
                        self.prepare_graph(description, maximum)?
                    },
                );
            }
            let description = self
                .descriptions
                .decoded_tone_notch(55.0, plan.local.receive.notch_width_hz)?;
            tail_notch = Some(if let Some(old) = &previous.receive_ctcss_tail_notch {
                self.replacement_graph(
                    description,
                    self.descriptions
                        .decoded_tone_notch(55.0, previous_plan.local.receive.notch_width_hz)?,
                    old,
                    0,
                )?
            } else {
                self.prepare_graph(description, maximum)?
            });
        }
        Ok(ProcessingGeneration {
            receive_deemphasis: self.replacement_graph(
                self.descriptions
                    .receive_deemphasis(plan.deemphasis_enabled, plan.deemphasis_corner_hz)?,
                self.descriptions.receive_deemphasis(
                    previous_plan.deemphasis_enabled,
                    previous_plan.deemphasis_corner_hz,
                )?,
                &previous.receive_deemphasis,
                0,
            )?,
            receive_filter: self.replacement_graph(
                self.descriptions.receive_filter(&plan.local)?,
                self.descriptions.receive_filter(&previous_plan.local)?,
                &previous.receive_filter,
                0,
            )?,
            receive_ctcss_notch: notches,
            receive_ctcss_tail_notch: tail_notch,
            receive_noise_reduction: if plan.local.rnnoise_enabled {
                Some(match &previous.receive_noise_reduction {
                    Some(old) => old.clone(),
                    None => self.prepare_denoise(true)?,
                })
            } else {
                None
            },
            receive_dynamics: self.replacement_graph(
                self.descriptions.dynamics(&plan.local)?,
                self.descriptions.dynamics(&previous_plan.local)?,
                &previous.receive_dynamics,
                0,
            )?,
            transmit_program: self.replacement_graph(
                self.descriptions.transmitter(
                    &plan.voice_telemetry,
                    plan.preemphasis_enabled,
                    plan.preemphasis_corner_hz,
                )?,
                self.descriptions.transmitter(
                    &previous_plan.voice_telemetry,
                    previous_plan.preemphasis_enabled,
                    previous_plan.preemphasis_corner_hz,
                )?,
                &previous.transmit_program,
                transmit_lookahead_frames(plan),
            )?,
            transmit_dcs_normal_filter: previous.transmit_dcs_normal_filter.clone(),
            transmit_dcs_turnoff_filter: previous.transmit_dcs_turnoff_filter.clone(),
        })
    }

    /// Prepare an inverse handoff to retained original processors without
    /// resetting or warming their state. Only changed stages receive a handoff.
    ///
    /// # Safety
    /// Both generations and `target_plan` must belong to this factory's same
    /// sole RX/TX owners. `current` must be the immediately preceding forward
    /// update, with no intervening update. A rejected forward half may leave its
    /// original owner active instead; the inverse also handles that case.
    pub unsafe fn prepare_restore(
        &self,
        target_plan: &NativeProcessingPlan,
        target: &ProcessingGeneration,
        current: &ProcessingGeneration,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        validate_plan(target_plan)?;
        let restore = |target: &SharedGraph, current: &SharedGraph, lookahead| {
            if target.0.primary.pointer() == current.0.primary.pointer() {
                Ok(target.clone())
            } else {
                self.transition_graph(target.0.primary.clone(), current, lookahead)
            }
        };
        let restore_optional =
            |target: &Option<SharedGraph>, current: &Option<SharedGraph>| match (target, current) {
                (Some(target), Some(current)) => restore(target, current, 0).map(Some),
                _ => Ok(target.clone()),
            };
        let mut notches = std::array::from_fn(|_| None);
        for ((notch, target), current) in notches
            .iter_mut()
            .zip(&target.receive_ctcss_notch)
            .zip(&current.receive_ctcss_notch)
        {
            *notch = restore_optional(target, current)?;
        }
        Ok(ProcessingGeneration {
            receive_deemphasis: restore(
                &target.receive_deemphasis,
                &current.receive_deemphasis,
                0,
            )?,
            receive_filter: restore(&target.receive_filter, &current.receive_filter, 0)?,
            receive_ctcss_notch: notches,
            receive_ctcss_tail_notch: restore_optional(
                &target.receive_ctcss_tail_notch,
                &current.receive_ctcss_tail_notch,
            )?,
            receive_noise_reduction: target.receive_noise_reduction.as_ref().map(|target| {
                if current.receive_noise_reduction.is_none() {
                    // Framing may have stopped while disabled. Reset it on the
                    // first callback, retaining the original prepared model.
                    SharedDenoise::new(target.0.stream.clone(), true)
                } else {
                    target.clone()
                }
            }),
            receive_dynamics: restore(&target.receive_dynamics, &current.receive_dynamics, 0)?,
            transmit_program: restore(
                &target.transmit_program,
                &current.transmit_program,
                transmit_lookahead_frames(target_plan),
            )?,
            transmit_dcs_normal_filter: target.transmit_dcs_normal_filter.clone(),
            transmit_dcs_turnoff_filter: target.transmit_dcs_turnoff_filter.clone(),
        })
    }

    fn replacement_graph(
        &self,
        description: String,
        previous_description: String,
        previous: &SharedGraph,
        lookahead_frames: usize,
    ) -> Result<SharedGraph, ProcessingRuntimeError> {
        if description == previous_description {
            Ok(previous.clone())
        } else {
            let maximum = self.stream.maximum_frame_count();
            let graph = self.prepare_warmed_graph(description, maximum)?;
            self.transition_graph(SharedProcessor::new(graph), previous, lookahead_frames)
        }
    }

    fn prepare_denoise(&self, activate: bool) -> Result<SharedDenoise, ProcessingRuntimeError> {
        Ok(SharedDenoise::new(
            SharedProcessor::new(self.denoise_provider.prepare()?),
            activate,
        ))
    }

    fn transition_graph(
        &self,
        primary: SharedProcessor<PreparedGraph>,
        previous: &SharedGraph,
        lookahead_frames: usize,
    ) -> Result<SharedGraph, ProcessingRuntimeError> {
        let maximum = self.stream.maximum_frame_count() as usize;
        let audible = previous.0.audible.load(Ordering::Acquire);
        let current = previous
            .lease_for(audible)
            .ok_or(GraphError::InvalidArgument)?;
        // Until adoption, the old owner can either keep its audible predecessor
        // or finish fading to its primary. Retain those two flat graphs and
        // select at the first callback, not during this potentially slow setup.
        let pending =
            (current.pointer() != previous.0.primary.pointer()).then(|| previous.0.primary.clone());
        Ok(SharedGraph(Arc::new(GraphStage {
            primary,
            previous: Some(GraphPredecessor {
                current: current.clone(),
                pending,
            }),
            audible: Arc::clone(&previous.0.audible),
            transition: Some(UnsafeCell::new(GraphTransition {
                output: vec![0.0; maximum].into_boxed_slice(),
                prime_remaining: (lookahead_frames + GRAPH_TRANSITION_FRAMES).max(maximum),
                faded: 0,
                use_pending: None,
            })),
            complete: AtomicBool::new(false),
        })))
    }

    fn prepare_graph(
        &self,
        description: String,
        maximum_frame_count: u32,
    ) -> Result<SharedGraph, ProcessingRuntimeError> {
        let primary =
            SharedProcessor::new(self.prepare_warmed_graph(description, maximum_frame_count)?);
        let audible = Arc::new(AtomicPtr::new(primary.pointer().as_ptr()));
        Ok(SharedGraph(Arc::new(GraphStage {
            primary,
            previous: None,
            audible,
            transition: None,
            complete: AtomicBool::new(true),
        })))
    }

    fn prepare_warmed_graph(
        &self,
        description: String,
        maximum_frame_count: u32,
    ) -> Result<PreparedGraph, ProcessingRuntimeError> {
        let description = CString::new(description)
            .map_err(|_| ProcessingRuntimeError::InvalidGraphDescription)?;
        let mut graph = self
            .graph_provider
            .prepare(&description, maximum_frame_count)?;
        let silence = vec![0.0; maximum_frame_count as usize];
        let mut output = vec![0.0; maximum_frame_count as usize];
        graph.warm_up(&silence, &mut output, GRAPH_WARMUP_BLOCKS)?;
        Ok(graph)
    }
}

/// Stable storage shared only across successive generations of the same owner.
struct SharedProcessor<T>(Arc<UnsafeCell<T>>);

impl<T> SharedProcessor<T> {
    fn new(processor: T) -> Self {
        Self(Arc::new(UnsafeCell::new(processor)))
    }

    fn pointer(&self) -> NonNull<T> {
        // SAFETY: the Arc allocation retains its initialized processor.
        unsafe { NonNull::new_unchecked(self.0.get()) }
    }
}

impl<T> Clone for SharedProcessor<T> {
    fn clone(&self) -> Self {
        Self(Arc::clone(&self.0))
    }
}

// SAFETY: only the assigned RX or TX callback mutates this processor. Control
// only clones/drops ownership; unsafe replacement admission preserves that owner.
unsafe impl<T: Send> Send for SharedProcessor<T> {}
// SAFETY: shared access exposes no reference to T. Its raw ports remain assigned
// to the same serial audio owner across the unsafe replacement handoff.
unsafe impl<T: Send> Sync for SharedProcessor<T> {}

/// Immutable leases and publication state; only the assigned owner accesses the
/// transition's scratch/counters or mutates either selected native graph.
struct GraphStage {
    primary: SharedProcessor<PreparedGraph>,
    previous: Option<GraphPredecessor>,
    audible: Arc<AtomicPtr<PreparedGraph>>,
    transition: Option<UnsafeCell<GraphTransition>>,
    complete: AtomicBool,
}

struct GraphPredecessor {
    current: SharedProcessor<PreparedGraph>,
    pending: Option<SharedProcessor<PreparedGraph>>,
}

struct GraphTransition {
    output: Box<[f32]>,
    prime_remaining: usize,
    faded: usize,
    use_pending: Option<bool>,
}

// SAFETY: control reads only immutable leases and the atomic audible pointer.
// The same sole RX/TX owner mutates all graph and transition state across ports.
unsafe impl Sync for GraphStage {}

#[derive(Clone)]
struct SharedGraph(Arc<GraphStage>);

impl SharedGraph {
    fn pointer(&self) -> NonNull<GraphStage> {
        NonNull::from(self.0.as_ref())
    }

    fn lease_for(&self, pointer: *mut PreparedGraph) -> Option<&SharedProcessor<PreparedGraph>> {
        if self.0.primary.pointer().as_ptr() == pointer {
            return Some(&self.0.primary);
        }
        let previous = self.0.previous.as_ref()?;
        if previous.current.pointer().as_ptr() == pointer {
            return Some(&previous.current);
        }
        previous
            .pending
            .as_ref()
            .filter(|graph| graph.pointer().as_ptr() == pointer)
    }
}

struct DenoiseStage {
    stream: SharedProcessor<DenoiseStream>,
    // None preserves ordinary startup/burst behavior. Some is a one-shot live
    // enable bridge, accessed only by the serial RX owner.
    activation: UnsafeCell<Option<usize>>,
}

// SAFETY: control only clones the immutable model lease; RX alone mutates the
// model and activation counter, including across replacement/restore ports.
unsafe impl Sync for DenoiseStage {}

#[derive(Clone)]
struct SharedDenoise(Arc<DenoiseStage>);

impl SharedDenoise {
    fn new(stream: SharedProcessor<DenoiseStream>, activate: bool) -> Self {
        Self(Arc::new(DenoiseStage {
            stream,
            activation: UnsafeCell::new(activate.then_some(0)),
        }))
    }

    fn pointer(&self) -> NonNull<DenoiseStage> {
        NonNull::from(self.0.as_ref())
    }
}

/// Stable ownership of all processors borrowed by one radio-core generation.
pub struct ProcessingGeneration {
    receive_deemphasis: SharedGraph,
    receive_filter: SharedGraph,
    receive_ctcss_notch: [Option<SharedGraph>; CTCSS_TONE_COUNT],
    receive_ctcss_tail_notch: Option<SharedGraph>,
    receive_noise_reduction: Option<SharedDenoise>,
    receive_dynamics: SharedGraph,
    transmit_program: SharedGraph,
    transmit_dcs_normal_filter: SharedGraph,
    transmit_dcs_turnoff_filter: SharedGraph,
}

impl ProcessingGeneration {
    // Private: safe public Clone would permit two independent audio owners.
    fn clone_for_owner(&self) -> Self {
        Self {
            receive_deemphasis: self.receive_deemphasis.clone(),
            receive_filter: self.receive_filter.clone(),
            receive_ctcss_notch: self.receive_ctcss_notch.clone(),
            receive_ctcss_tail_notch: self.receive_ctcss_tail_notch.clone(),
            receive_noise_reduction: self.receive_noise_reduction.clone(),
            receive_dynamics: self.receive_dynamics.clone(),
            transmit_program: self.transmit_program.clone(),
            transmit_dcs_normal_filter: self.transmit_dcs_normal_filter.clone(),
            transmit_dcs_turnoff_filter: self.transmit_dcs_turnoff_filter.clone(),
        }
    }

    /// Borrow all prepared processors together with the caller-owned program ring.
    ///
    /// The returned ports keep this generation mutably borrowed until the
    /// complete radio session and its split endpoints have been dropped.
    pub fn session_ports<'a>(&'a mut self, program_ring: ProgramRingPort<'a>) -> SessionPorts<'a> {
        let Self {
            receive_deemphasis,
            receive_filter,
            receive_ctcss_notch,
            receive_ctcss_tail_notch,
            receive_noise_reduction,
            receive_dynamics,
            transmit_program,
            transmit_dcs_normal_filter,
            transmit_dcs_turnoff_filter,
        } = self;
        let mut notch_ports = std::array::from_fn(|_| ProcessorPort::passthrough());
        for (port, graph) in notch_ports.iter_mut().zip(receive_ctcss_notch) {
            if let Some(graph) = graph {
                *port = graph_port(graph);
            }
        }
        let tail_notch_port = receive_ctcss_tail_notch
            .as_mut()
            .map_or_else(ProcessorPort::passthrough, graph_port);
        SessionPorts {
            receive_deemphasis: graph_port(receive_deemphasis),
            receive_filter: graph_port(receive_filter),
            receive_ctcss_notch: notch_ports,
            receive_noise_reduction: receive_noise_reduction
                .as_mut()
                .map_or_else(ProcessorPort::passthrough, denoise_port),
            receive_dynamics: graph_port(receive_dynamics),
            transmit_program: graph_port(transmit_program),
            transmit_dcs_normal_filter: graph_port(transmit_dcs_normal_filter),
            transmit_dcs_turnoff_filter: graph_port(transmit_dcs_turnoff_filter),
            program_ring,
            receive_ctcss_tail_notch: tail_notch_port,
        }
    }
}

fn validate_plan(plan: &NativeProcessingPlan) -> Result<(), ProcessingRuntimeError> {
    if plan.local.role != ChainRole::LocalReceive
        || plan.voice_telemetry.role != ChainRole::VoiceTelemetry
    {
        return Err(ProcessingRuntimeError::IncorrectChainRole);
    }
    plan.local.validate()?;
    plan.voice_telemetry.validate()?;
    Ok(())
}

fn transmit_lookahead_frames(plan: &NativeProcessingPlan) -> usize {
    if plan.voice_telemetry.transmit_tail.limiter_enabled {
        (plan.voice_telemetry.transmit_tail.lookahead_ms * 48.0).ceil() as usize
    } else {
        0
    }
}

fn graph_port(graph: &mut SharedGraph) -> ProcessorPort<'_> {
    // SAFETY: the port borrow retains stable storage. Replacement generations
    // may share it only under the same serial owner, as required at admission.
    unsafe { ProcessorPort::from_raw(graph.pointer().cast::<c_void>(), graph_process, None, None) }
}

fn denoise_port(stream: &mut SharedDenoise) -> ProcessorPort<'_> {
    // SAFETY: the port retains stable storage; process and bypass remain under
    // the same serial receive owner, including across replacement generations.
    unsafe {
        ProcessorPort::from_raw(
            stream.pointer().cast::<c_void>(),
            denoise_process,
            Some(denoise_bypass),
            None,
        )
    }
}

unsafe extern "C" fn graph_process(
    context: *mut c_void,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
) -> c_int {
    let (Some(context), false, false, false) = (
        NonNull::new(context.cast::<GraphStage>()),
        input.is_null(),
        output.is_null(),
        frame_count == 0,
    ) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port retains the immutable stage. Only this serial owner
    // accesses its inner graph/scratch state and the exact disjoint sample spans.
    let (stage, input, output) = unsafe {
        (
            context.as_ref(),
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    // SAFETY: replacement admission preserves this graph's sole audio owner.
    let graph = unsafe { &mut *stage.primary.pointer().as_ptr() };
    if graph.process_block(input, output).is_err() {
        return PORT_ERROR;
    }
    if stage.complete.load(Ordering::Relaxed) {
        return PORT_OK;
    }
    let (Some(previous), Some(transition)) = (&stage.previous, &stage.transition) else {
        return PORT_ERROR;
    };
    // SAFETY: control never accesses these callback-owned scratch/counters.
    let transition = unsafe { &mut *transition.get() };
    let use_pending = match transition.use_pending {
        Some(selected) => selected,
        None => {
            let audible = stage.audible.load(Ordering::Acquire);
            if stage.primary.pointer().as_ptr() == audible {
                // The inverse target may have finished its own earlier fade
                // after inverse preparation. It is already current and warm.
                stage.complete.store(true, Ordering::Relaxed);
                return PORT_OK;
            }
            let selected = if previous.current.pointer().as_ptr() == audible {
                false
            } else if previous
                .pending
                .as_ref()
                .is_some_and(|graph| graph.pointer().as_ptr() == audible)
            {
                true
            } else {
                return PORT_ERROR;
            };
            transition.use_pending = Some(selected);
            selected
        }
    };
    let previous = if use_pending {
        let Some(previous) = &previous.pending else {
            return PORT_ERROR;
        };
        previous
    } else {
        &previous.current
    };
    if previous.pointer() == stage.primary.pointer() {
        // An inverse adopted before its forward fade needs no handoff: its
        // original primary was still audible and has already run exactly once.
        stage.complete.store(true, Ordering::Relaxed);
        return PORT_OK;
    }
    // SAFETY: this flat, distinct predecessor retains the same sole owner.
    let previous = unsafe { &mut *previous.pointer().as_ptr() };
    let Some(prior_output) = transition.output.get_mut(..output.len()) else {
        return PORT_ERROR;
    };
    if previous.process_block(input, prior_output).is_err() {
        return PORT_ERROR;
    }
    for (sample, prior) in output.iter_mut().zip(prior_output) {
        if transition.prime_remaining > 0 {
            transition.prime_remaining -= 1;
            *sample = *prior;
        } else if transition.faded < GRAPH_TRANSITION_FRAMES {
            transition.faded += 1;
            let blend = transition.faded as f32 / GRAPH_TRANSITION_FRAMES as f32;
            *sample = *prior * (1.0 - blend) + *sample * blend;
        }
    }
    if transition.faded == GRAPH_TRANSITION_FRAMES {
        // Leases and scratch remain in place for control-side reclamation.
        stage
            .audible
            .store(stage.primary.pointer().as_ptr(), Ordering::Release);
        stage.complete.store(true, Ordering::Release);
    }
    PORT_OK
}

unsafe extern "C" fn denoise_process(
    context: *mut c_void,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
) -> c_int {
    let (Some(context), false, false, false) = (
        NonNull::new(context.cast::<DenoiseStage>()),
        input.is_null(),
        output.is_null(),
        frame_count == 0,
    ) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port guarantees the concrete context type, exclusive
    // callback access, and disjoint spans of exactly frame_count.
    let (stage, input, output) = unsafe {
        (
            context.as_ref(),
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    // SAFETY: replacement admission preserves sole RX access to model/framing
    // and the optional one-shot activation counter. Control never reads either.
    let (stream, activation) = unsafe {
        (
            &mut *stage.stream.pointer().as_ptr(),
            &mut *stage.activation.get(),
        )
    };
    if *activation == Some(0) {
        // Restoring an enabled model after a disabled interval discards queued
        // stale PCM, but retains the model history just like ordinary bypass.
        stream.bypass();
    }
    output.copy_from_slice(input);
    if stream.process(output).is_err() {
        return PORT_ERROR;
    }
    if let Some(processed) = activation {
        for (sample, dry) in output.iter_mut().zip(input) {
            if *processed < DENOISE_STARTUP_FRAMES {
                *sample = *dry;
            } else if *processed < DENOISE_STARTUP_FRAMES + GRAPH_TRANSITION_FRAMES {
                let blend = (*processed - DENOISE_STARTUP_FRAMES + 1) as f32
                    / GRAPH_TRANSITION_FRAMES as f32;
                *sample = *dry * (1.0 - blend) + *sample * blend;
            }
            *processed += 1;
        }
        if *processed >= DENOISE_STARTUP_FRAMES + GRAPH_TRANSITION_FRAMES {
            *activation = None;
        }
    }
    PORT_OK
}

unsafe extern "C" fn denoise_bypass(context: *mut c_void, _frame_count: u32) -> c_int {
    let Some(context) = NonNull::new(context.cast::<DenoiseStage>()) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port guarantees the concrete context type and sole
    // receive-owner access while bypass state is advanced.
    let stage = unsafe { context.as_ref() };
    // SAFETY: only this serial RX owner touches model/framing and activation.
    let (stream, activation) = unsafe {
        (
            &mut *stage.stream.pointer().as_ptr(),
            &mut *stage.activation.get(),
        )
    };
    stream.bypass();
    if activation.is_some() {
        *activation = Some(0);
    }
    PORT_OK
}

#[cfg(test)]
#[path = "tests/processing_tests.rs"]
mod tests;
