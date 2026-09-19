//! Control-plane ownership of prepared native processing providers.

use std::ffi::{CString, c_int, c_void};
use std::fmt;
use std::ptr::NonNull;
use std::slice;

use usbradioplus_core::{
    ChainRole, CtcssTone, GraphDescriptionError, GraphDescriptionFactory, NativeStreamSpec,
    PlFilter, ProcessingChain, ProcessingConfigError, StreamSpecError,
};
use usbradioplus_ffmpeg::{GraphError, GraphProvider, PreparedGraph};
use usbradioplus_radio::{CTCSS_TONE_COUNT, ProcessorPort, ProgramRingPort, SessionPorts};
use usbradioplus_rnnoise::{DenoiseError, DenoiseProvider, DenoiseStream};

const GRAPH_WARMUP_BLOCKS: usize = 8;
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
        let mut notches: [Option<PreparedGraph>; CTCSS_TONE_COUNT] = std::array::from_fn(|_| None);
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
                .then(|| self.denoise_provider.prepare())
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

    fn prepare_graph(
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

/// Stable ownership of all processors borrowed by one radio-core generation.
pub struct ProcessingGeneration {
    receive_deemphasis: PreparedGraph,
    receive_filter: PreparedGraph,
    receive_ctcss_notch: [Option<PreparedGraph>; CTCSS_TONE_COUNT],
    receive_ctcss_tail_notch: Option<PreparedGraph>,
    receive_noise_reduction: Option<DenoiseStream>,
    receive_dynamics: PreparedGraph,
    transmit_program: PreparedGraph,
    transmit_dcs_normal_filter: PreparedGraph,
    transmit_dcs_turnoff_filter: PreparedGraph,
}

impl ProcessingGeneration {
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

fn graph_port(graph: &mut PreparedGraph) -> ProcessorPort<'_> {
    // SAFETY: The port borrow prevents moving or dropping the graph;
    // `graph_process` is the sole callback operation on this graph instance.
    unsafe {
        ProcessorPort::from_raw(
            NonNull::from(graph).cast::<c_void>(),
            graph_process,
            None,
            None,
        )
    }
}

fn denoise_port(stream: &mut DenoiseStream) -> ProcessorPort<'_> {
    // SAFETY: The port borrow prevents moving or dropping the stream;
    // the process and bypass callbacks remain serialized by the receive owner.
    unsafe {
        ProcessorPort::from_raw(
            NonNull::from(stream).cast::<c_void>(),
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
    let (Some(mut context), false, false, false) = (
        NonNull::new(context.cast::<PreparedGraph>()),
        input.is_null(),
        output.is_null(),
        frame_count == 0,
    ) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port guarantees the concrete context type, exclusive
    // callback access, and readable/writable spans of exactly frame_count.
    let (graph, input, output) = unsafe {
        (
            context.as_mut(),
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    match graph.process_block(input, output) {
        Ok(()) => PORT_OK,
        Err(_) => PORT_ERROR,
    }
}

unsafe extern "C" fn denoise_process(
    context: *mut c_void,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
) -> c_int {
    let (Some(mut context), false, false, false) = (
        NonNull::new(context.cast::<DenoiseStream>()),
        input.is_null(),
        output.is_null(),
        frame_count == 0,
    ) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port guarantees the concrete context type, exclusive
    // callback access, and disjoint spans of exactly frame_count.
    let (stream, input, output) = unsafe {
        (
            context.as_mut(),
            slice::from_raw_parts(input, frame_count as usize),
            slice::from_raw_parts_mut(output, frame_count as usize),
        )
    };
    output.copy_from_slice(input);
    match stream.process(output) {
        Ok(()) => PORT_OK,
        Err(_) => PORT_ERROR,
    }
}

unsafe extern "C" fn denoise_bypass(context: *mut c_void, _frame_count: u32) -> c_int {
    let Some(mut context) = NonNull::new(context.cast::<DenoiseStream>()) else {
        return PORT_ERROR;
    };
    // SAFETY: The bound port guarantees the concrete context type and sole
    // receive-owner access while bypass state is advanced.
    unsafe { context.as_mut() }.bypass();
    PORT_OK
}

#[cfg(test)]
#[path = "tests/processing_tests.rs"]
mod tests;
