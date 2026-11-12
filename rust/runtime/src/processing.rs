//! Control-plane ownership of prepared native processing providers.

use std::cell::UnsafeCell;
use std::ffi::{CString, c_int, c_void};
use std::fmt;
use std::ptr::NonNull;
use std::slice;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

use usbradioplus_core::{
    ChainRole, CtcssTone, GraphDescriptionError, GraphDescriptionFactory, GraphRecipe,
    NativeStreamSpec, OutputLoudnessError, OutputLoudnessMeter, OutputLoudnessObserver,
    OutputLoudnessSnapshot, PROGRAMME_CURRENT_BOUND_VALID, PROGRAMME_PREPARED_BOUND_VALID,
    PlFilter, ProcessingChain, ProcessingConfigError, ProgrammeBuffering, ProgrammeBufferingPlan,
    StreamSpecError,
};
use usbradioplus_ffmpeg::{GraphError, GraphProvider, PreparedGraph};
use usbradioplus_radio::{CTCSS_TONE_COUNT, ProcessorPort, ProgramRingPort, SessionPorts};
use usbradioplus_rnnoise::{DenoiseError, DenoiseProvider, DenoiseStream};

const GRAPH_WARMUP_BLOCKS: usize = 8;
// One millisecond at the fixed native 48 kHz rate smooths the final handoff.
const GRAPH_TRANSITION_FRAMES: usize = 48;
// Measured 4x limiter/SRC and 20th-order bandpass settling at 2.5 kHz needs up
// to 224 native frames beyond lookahead for one-sample public blocks.
const LIMITER_PRIME_MARGIN_FRAMES: usize = 256;
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
    /// A mandatory output meter rejected setup.
    Loudness(OutputLoudnessError),
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
            Self::Loudness(error) => write!(formatter, "output meter setup failed: {error}"),
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

/// Native frontend profiles with exact raw-graph fallbacks for unconfigured roles.
#[derive(Clone, Debug, PartialEq)]
pub struct ExplicitNativeProcessingPlan {
    /// Shared local profile; replaces the raw receive graph and output gain when present.
    pub local: Option<ProcessingChain>,
    /// Shared voice/telemetry profile; replaces the raw transmit graph when present.
    pub voice_telemetry: Option<ProcessingChain>,
    /// Effective linear receiver input gain applied by the radio session.
    pub receive_input_gain: f32,
    /// Exact configured mono receive graph when no local profile is selected.
    pub receive_graph: String,
    /// Exact configured mono transmit graph when no voice/telemetry profile is selected.
    pub transmit_graph: String,
    /// Apply receiver de-emphasis before receive gain/filtering.
    pub receive_deemphasis: bool,
    /// Receiver de-emphasis corner in Hz.
    pub receive_deemphasis_corner_hz: f64,
    /// Apply transmitter pre-emphasis when a voice/telemetry profile is selected.
    pub transmit_preemphasis: bool,
    /// Transmitter pre-emphasis corner in Hz for the shared voice/telemetry profile.
    pub transmit_preemphasis_corner_hz: f64,
    /// Gain after raw receive filtering, in decibels; unused with a local profile.
    pub receive_output_gain_db: i32,
    /// CTCSS table entries requiring decoded-tone rejection.
    pub receive_ctcss_mask: u64,
    /// Prepare normal and turnoff DCS shaping.
    pub transmit_dcs: bool,
}

impl From<&NativeProcessingPlan> for ExplicitNativeProcessingPlan {
    fn from(plan: &NativeProcessingPlan) -> Self {
        Self {
            local: Some(plan.local.clone()),
            voice_telemetry: Some(plan.voice_telemetry.clone()),
            receive_input_gain: 10.0_f32.powf(plan.local.input_gain_db as f32 / 20.0),
            receive_graph: String::new(),
            transmit_graph: String::new(),
            receive_deemphasis: plan.deemphasis_enabled,
            receive_deemphasis_corner_hz: plan.deemphasis_corner_hz,
            transmit_preemphasis: plan.preemphasis_enabled,
            transmit_preemphasis_corner_hz: plan.preemphasis_corner_hz,
            receive_output_gain_db: 0,
            receive_ctcss_mask: (1_u64 << CTCSS_TONE_COUNT) - 1,
            transmit_dcs: true,
        }
    }
}

impl From<NativeProcessingPlan> for ExplicitNativeProcessingPlan {
    fn from(plan: NativeProcessingPlan) -> Self {
        Self::from(&plan)
    }
}

impl From<&ExplicitNativeProcessingPlan> for ExplicitNativeProcessingPlan {
    fn from(plan: &ExplicitNativeProcessingPlan) -> Self {
        plan.clone()
    }
}

impl ExplicitNativeProcessingPlan {
    fn notch_settings(&self) -> (u64, f64) {
        match &self.local {
            Some(local) if local.receive.pl_filter == PlFilter::DecodedToneNotch => {
                (self.receive_ctcss_mask, local.receive.notch_width_hz)
            }
            Some(_) => (0, 10.0),
            None => (self.receive_ctcss_mask, 10.0),
        }
    }

    fn receive_filter(
        &self,
        descriptions: &GraphDescriptionFactory,
    ) -> Result<String, GraphDescriptionError> {
        self.local.as_ref().map_or_else(
            || Ok(self.receive_graph.clone()),
            |local| descriptions.receive_filter(local),
        )
    }

    fn receive_dynamics(
        &self,
        descriptions: &GraphDescriptionFactory,
    ) -> Result<String, GraphDescriptionError> {
        match &self.local {
            Some(local) => descriptions.dynamics(local),
            None if self.receive_output_gain_db == 0 => Ok("anull".to_owned()),
            None => Ok(format!("volume={}dB", self.receive_output_gain_db)),
        }
    }

    fn transmitter(
        &self,
        descriptions: &GraphDescriptionFactory,
    ) -> Result<GraphRecipe, GraphDescriptionError> {
        self.voice_telemetry.as_ref().map_or_else(
            || {
                Ok(GraphRecipe {
                    description: self.transmit_graph.clone(),
                    buffering: ProgrammeBufferingPlan::Unknown,
                })
            },
            |voice| {
                descriptions.transmitter_recipe(
                    voice,
                    self.transmit_preemphasis,
                    self.transmit_preemphasis_corner_hz,
                )
            },
        )
    }

    fn dcs_filter(&self, descriptions: &GraphDescriptionFactory, turnoff: bool) -> String {
        if self.transmit_dcs {
            descriptions.dcs_filter(turnoff)
        } else {
            "anull".to_owned()
        }
    }

    fn rnnoise_enabled(&self) -> bool {
        self.local
            .as_ref()
            .is_some_and(|local| local.rnnoise_enabled)
    }
}

/// Reusable control-plane factory for prepared processing generations.
#[derive(Clone)]
pub struct NativeProcessingFactory {
    descriptions: GraphDescriptionFactory,
    graph_provider: GraphProvider,
    denoise_provider: Option<DenoiseProvider>,
    stream: NativeStreamSpec,
}

impl NativeProcessingFactory {
    /// Prepare native profiles through the shared USBRadioPlus graph builders.
    ///
    /// Unconfigured roles retain their exact raw graph behavior. Configured roles
    /// replace those graphs rather than stacking their filtering and gains. RNNoise
    /// requires its provider only when enabled by the selected local profile.
    pub fn prepare_explicit(
        provider: GraphProvider,
        denoise_provider: Option<DenoiseProvider>,
        agc_plugin_path: &str,
        plan: &ExplicitNativeProcessingPlan,
        maximum_frame_count: u32,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        let descriptions = GraphDescriptionFactory::new(agc_plugin_path)?;
        Self::prepare_explicit_graphs(
            provider,
            denoise_provider,
            &descriptions,
            plan,
            maximum_frame_count,
        )
    }

    fn prepare_explicit_graphs(
        provider: GraphProvider,
        denoise_provider: Option<DenoiseProvider>,
        descriptions: &GraphDescriptionFactory,
        plan: &ExplicitNativeProcessingPlan,
        maximum_frame_count: u32,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        NativeStreamSpec::new(48_000, maximum_frame_count)?;
        validate_plan(plan)?;
        let prepare = |description| prepare_graph(provider, description, maximum_frame_count);
        let (notch_mask, notch_width) = plan.notch_settings();
        let mut notches = std::array::from_fn(|_| None);
        for tone in CtcssTone::supported() {
            if notch_mask & (1 << tone.table_index()) != 0 {
                notches[tone.table_index()] = Some(prepare(
                    descriptions.decoded_tone_notch(f64::from(tone.as_hz()), notch_width)?,
                )?);
            }
        }
        let tail = (notch_mask != 0)
            .then(|| prepare(descriptions.decoded_tone_notch(55.0, notch_width)?))
            .transpose()?;
        ProcessingGeneration {
            receive_deemphasis: prepare(
                descriptions.receive_deemphasis(
                    plan.receive_deemphasis,
                    plan.receive_deemphasis_corner_hz,
                )?,
            )?,
            receive_filter: prepare(plan.receive_filter(descriptions)?)?,
            receive_ctcss_notch: notches,
            receive_ctcss_tail_notch: tail,
            receive_noise_reduction: if plan.rnnoise_enabled() {
                Some(prepare_denoise(
                    denoise_provider.ok_or(DenoiseError::IncompatibleAdapter)?,
                    false,
                )?)
            } else {
                None
            },
            receive_dynamics: prepare(plan.receive_dynamics(descriptions)?)?,
            transmit_program: prepare_recipe(
                provider,
                plan.transmitter(descriptions)?,
                maximum_frame_count,
            )?,
            receive_loudness: None,
            receive_input_gain: 0.0,
            transmit_loudness: None,
            transmit_dcs_normal_filter: prepare(plan.dcs_filter(descriptions, false))?,
            transmit_dcs_turnoff_filter: prepare(plan.dcs_filter(descriptions, true))?,
        }
        .with_output_meters(plan, maximum_frame_count, None)
    }

    /// Construct a factory from process-lifetime adapter capabilities.
    pub fn new(
        graph_provider: GraphProvider,
        denoise_provider: DenoiseProvider,
        agc_plugin_path: impl Into<String>,
        maximum_frame_count: u32,
    ) -> Result<Self, ProcessingRuntimeError> {
        Self::new_explicit(
            graph_provider,
            Some(denoise_provider),
            agc_plugin_path,
            maximum_frame_count,
        )
    }

    /// Construct a reusable native factory; RNNoise is required only by enabled profiles.
    pub fn new_explicit(
        graph_provider: GraphProvider,
        denoise_provider: Option<DenoiseProvider>,
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
        plan: impl Into<ExplicitNativeProcessingPlan>,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        Self::prepare_explicit_graphs(
            self.graph_provider,
            self.denoise_provider,
            &self.descriptions,
            &plan.into(),
            self.stream.maximum_frame_count(),
        )
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
        plan: impl Into<ExplicitNativeProcessingPlan>,
        previous_plan: impl Into<ExplicitNativeProcessingPlan>,
        previous: &ProcessingGeneration,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        let plan = plan.into();
        let previous_plan = previous_plan.into();
        if plan == previous_plan {
            // A control snapshot retains validated leases without formatting graphs.
            return Ok(previous.clone_for_owner());
        }
        validate_plan(&plan)?;
        let maximum = self.stream.maximum_frame_count();
        let (notch_mask, notch_width) = plan.notch_settings();
        let mut notches = std::array::from_fn(|_| None);
        for tone in CtcssTone::supported() {
            if notch_mask & (1 << tone.table_index()) != 0 {
                let old = previous.receive_ctcss_notch[tone.table_index()].as_ref();
                notches[tone.table_index()] = Some(match old {
                    Some(old) => old.clone(),
                    _ => self.prepare_graph(
                        self.descriptions
                            .decoded_tone_notch(f64::from(tone.as_hz()), notch_width)?,
                        maximum,
                    )?,
                });
            }
        }
        let tail_notch = if notch_mask == 0 {
            None
        } else {
            Some(match &previous.receive_ctcss_tail_notch {
                Some(old) => old.clone(),
                _ => self.prepare_graph(
                    self.descriptions.decoded_tone_notch(55.0, notch_width)?,
                    maximum,
                )?,
            })
        };
        ProcessingGeneration {
            receive_deemphasis: self.replacement_graph(
                self.descriptions.receive_deemphasis(
                    plan.receive_deemphasis,
                    plan.receive_deemphasis_corner_hz,
                )?,
                self.descriptions.receive_deemphasis(
                    previous_plan.receive_deemphasis,
                    previous_plan.receive_deemphasis_corner_hz,
                )?,
                &previous.receive_deemphasis,
                0,
            )?,
            receive_filter: self.replacement_graph(
                plan.receive_filter(&self.descriptions)?,
                previous_plan.receive_filter(&self.descriptions)?,
                &previous.receive_filter,
                0,
            )?,
            receive_ctcss_notch: notches,
            receive_ctcss_tail_notch: tail_notch,
            receive_noise_reduction: if plan.rnnoise_enabled() {
                Some(match &previous.receive_noise_reduction {
                    Some(old) => old.clone(),
                    None => self.prepare_denoise(true)?,
                })
            } else {
                None
            },
            receive_dynamics: self.replacement_graph(
                plan.receive_dynamics(&self.descriptions)?,
                previous_plan.receive_dynamics(&self.descriptions)?,
                &previous.receive_dynamics,
                limiter_lookahead_frames(plan.local.as_ref()),
            )?,
            transmit_program: self.replacement_programme_graph(
                plan.transmitter(&self.descriptions)?,
                previous_plan.transmitter(&self.descriptions)?,
                &previous.transmit_program,
                transmit_lookahead_frames(&plan),
            )?,
            receive_loudness: None,
            receive_input_gain: 0.0,
            transmit_loudness: None,
            transmit_dcs_normal_filter: self.replacement_graph(
                plan.dcs_filter(&self.descriptions, false),
                previous_plan.dcs_filter(&self.descriptions, false),
                &previous.transmit_dcs_normal_filter,
                0,
            )?,
            transmit_dcs_turnoff_filter: self.replacement_graph(
                plan.dcs_filter(&self.descriptions, true),
                previous_plan.dcs_filter(&self.descriptions, true),
                &previous.transmit_dcs_turnoff_filter,
                0,
            )?,
        }
        .with_output_meters(&plan, maximum, Some(previous))
    }

    /// Prepare a fresh, flat TX programme graph while retaining all RX, RNNoise,
    /// and access-signaling state. This deliberately avoids the ordinary update's
    /// predecessor priming, crossfade, and equal-plan reuse.
    ///
    /// # Safety
    /// `previous` and this replacement must remain with the same sole RX/TX
    /// owners through the existing acknowledged update lifecycle.
    pub unsafe fn prepare_programme_retirement(
        &self,
        plan: impl Into<ExplicitNativeProcessingPlan>,
        previous: &ProcessingGeneration,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        let plan = plan.into();
        validate_plan(&plan)?;
        let mut fresh = previous.clone_for_owner();
        fresh.transmit_program = prepare_recipe(
            self.graph_provider,
            plan.transmitter(&self.descriptions)?,
            self.stream.maximum_frame_count(),
        )?;
        fresh.with_output_meters(&plan, self.stream.maximum_frame_count(), Some(previous))
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
        target_plan: impl Into<ExplicitNativeProcessingPlan>,
        target: &ProcessingGeneration,
        current: &ProcessingGeneration,
    ) -> Result<ProcessingGeneration, ProcessingRuntimeError> {
        let target_plan = target_plan.into();
        validate_plan(&target_plan)?;
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
        ProcessingGeneration {
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
            receive_dynamics: restore(
                &target.receive_dynamics,
                &current.receive_dynamics,
                limiter_lookahead_frames(target_plan.local.as_ref()),
            )?,
            transmit_program: restore(
                &target.transmit_program,
                &current.transmit_program,
                transmit_lookahead_frames(&target_plan),
            )?,
            receive_loudness: None,
            receive_input_gain: 0.0,
            transmit_loudness: None,
            transmit_dcs_normal_filter: restore(
                &target.transmit_dcs_normal_filter,
                &current.transmit_dcs_normal_filter,
                0,
            )?,
            transmit_dcs_turnoff_filter: restore(
                &target.transmit_dcs_turnoff_filter,
                &current.transmit_dcs_turnoff_filter,
                0,
            )?,
        }
        .with_output_meters(
            &target_plan,
            self.stream.maximum_frame_count(),
            Some(current),
        )
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

    fn replacement_programme_graph(
        &self,
        recipe: GraphRecipe,
        previous_recipe: GraphRecipe,
        previous: &SharedGraph,
        lookahead_frames: usize,
    ) -> Result<SharedGraph, ProcessingRuntimeError> {
        if recipe == previous_recipe {
            Ok(previous.clone())
        } else {
            let graph = prepare_warmed_recipe(
                self.graph_provider,
                recipe,
                self.stream.maximum_frame_count(),
            )?;
            self.transition_graph(SharedProcessor::new(graph), previous, lookahead_frames)
        }
    }

    fn prepare_denoise(&self, activate: bool) -> Result<SharedDenoise, ProcessingRuntimeError> {
        prepare_denoise(
            self.denoise_provider
                .ok_or(DenoiseError::IncompatibleAdapter)?,
            activate,
        )
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
            buffering: UnsafeCell::new(ProgrammeBuffering::default()),
            audible: Arc::clone(&previous.0.audible),
            handoff: Some(GraphHandoff {
                previous: GraphPredecessor {
                    current: current.clone(),
                    pending,
                },
                transition: UnsafeCell::new(GraphTransition {
                    output: vec![0.0; maximum].into_boxed_slice(),
                    prime_remaining: (lookahead_frames
                        + if lookahead_frames == 0 {
                            GRAPH_TRANSITION_FRAMES
                        } else {
                            LIMITER_PRIME_MARGIN_FRAMES
                        })
                    .max(maximum),
                    faded: 0,
                    use_pending: None,
                }),
            }),
            complete: AtomicBool::new(false),
        })))
    }

    fn prepare_graph(
        &self,
        description: String,
        maximum_frame_count: u32,
    ) -> Result<SharedGraph, ProcessingRuntimeError> {
        prepare_graph(self.graph_provider, description, maximum_frame_count)
    }

    fn prepare_warmed_graph(
        &self,
        description: String,
        maximum_frame_count: u32,
    ) -> Result<PreparedGraph, ProcessingRuntimeError> {
        prepare_warmed_graph(self.graph_provider, description, maximum_frame_count)
    }
}

fn prepare_denoise(
    provider: DenoiseProvider,
    activate: bool,
) -> Result<SharedDenoise, ProcessingRuntimeError> {
    Ok(SharedDenoise::new(
        SharedProcessor::new(provider.prepare()?),
        activate,
    ))
}

fn prepare_warmed_graph(
    provider: GraphProvider,
    description: String,
    maximum: u32,
) -> Result<PreparedGraph, ProcessingRuntimeError> {
    prepare_warmed_recipe(
        provider,
        GraphRecipe {
            description,
            buffering: ProgrammeBufferingPlan::Unknown,
        },
        maximum,
    )
}

fn prepare_warmed_recipe(
    provider: GraphProvider,
    recipe: GraphRecipe,
    maximum: u32,
) -> Result<PreparedGraph, ProcessingRuntimeError> {
    let description = CString::new(recipe.description)
        .map_err(|_| ProcessingRuntimeError::InvalidGraphDescription)?;
    let mut graph =
        provider.prepare_at_rate_with_plan(&description, 48_000, maximum, recipe.buffering)?;
    let silence = vec![0.0; maximum as usize];
    let mut output = vec![0.0; maximum as usize];
    graph.warm_up(
        &silence,
        &mut output,
        GRAPH_WARMUP_BLOCKS.max(64_usize.div_ceil(maximum as usize)),
    )?;
    graph.finish_warmup();
    Ok(graph)
}

fn prepare_graph(
    provider: GraphProvider,
    description: String,
    maximum: u32,
) -> Result<SharedGraph, ProcessingRuntimeError> {
    prepare_recipe(
        provider,
        GraphRecipe {
            description,
            buffering: ProgrammeBufferingPlan::Unknown,
        },
        maximum,
    )
}

fn prepare_recipe(
    provider: GraphProvider,
    recipe: GraphRecipe,
    maximum: u32,
) -> Result<SharedGraph, ProcessingRuntimeError> {
    let graph = prepare_warmed_recipe(provider, recipe, maximum)?;
    let buffering = UnsafeCell::new(graph.buffering_state());
    let primary = SharedProcessor::new(graph);
    let audible = Arc::new(AtomicPtr::new(primary.pointer().as_ptr()));
    Ok(SharedGraph(Arc::new(GraphStage {
        primary,
        buffering,
        handoff: None,
        audible,
        complete: AtomicBool::new(true),
    })))
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
    buffering: UnsafeCell<ProgrammeBuffering>,
    handoff: Option<GraphHandoff>,
    audible: Arc<AtomicPtr<PreparedGraph>>,
    complete: AtomicBool,
}

struct GraphHandoff {
    previous: GraphPredecessor,
    transition: UnsafeCell<GraphTransition>,
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

/// Borrowed final-programme permission check on the existing serial TX owner.
///
/// The callback runs after the complete programme graph and before independent
/// radio signaling. Only one permits this block; all other values suppress it.
/// It must be bounded, allocation-free and non-unwinding, without calling control
/// or hardware APIs. Its context and code
/// remain live until quiescent unbind/rebind or destruction.
#[derive(Clone, Copy)]
pub struct ProgrammeFence {
    /// Borrowed caller-owned state, never dereferenced by this library.
    pub context: *mut c_void,
    /// Actual physical generation and graph frame count, not source timestamps.
    pub permit: unsafe extern "C" fn(*mut c_void, u64, u32) -> u32,
}

// SAFETY: copying the borrowed address transfers no access. The unsafe binding
// contract requires one serial TX owner and context lifetime through quiescence.
unsafe impl Send for ProgrammeFence {}

#[derive(Clone, Copy)]
struct BoundProgrammeFence {
    port: ProgrammeFence,
    generation: u64,
}

struct OutputLoudnessStage {
    graph: SharedGraph,
    meter: UnsafeCell<OutputLoudnessMeter>,
    observer: OutputLoudnessObserver,
    programme_fence: UnsafeCell<Option<BoundProgrammeFence>>,
}

// SAFETY: only the original serial TX owner mutates the meter and graph.
// Control-plane callers read the independent atomic observer only.
unsafe impl Sync for OutputLoudnessStage {}

impl SharedGraph {
    fn pointer(&self) -> NonNull<GraphStage> {
        NonNull::from(self.0.as_ref())
    }

    fn lease_for(&self, pointer: *mut PreparedGraph) -> Option<&SharedProcessor<PreparedGraph>> {
        if self.0.primary.pointer().as_ptr() == pointer {
            return Some(&self.0.primary);
        }
        let previous = &self.0.handoff.as_ref()?.previous;
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
    receive_loudness: Option<Arc<OutputLoudnessStage>>,
    receive_input_gain: f32,
    transmit_loudness: Option<Arc<OutputLoudnessStage>>,
    transmit_dcs_normal_filter: SharedGraph,
    transmit_dcs_turnoff_filter: SharedGraph,
}

impl ProcessingGeneration {
    fn with_output_meters(
        mut self,
        plan: &ExplicitNativeProcessingPlan,
        maximum: u32,
        previous: Option<&Self>,
    ) -> Result<Self, ProcessingRuntimeError> {
        self.receive_input_gain = plan.receive_input_gain;
        let old_receive = previous.filter(|old| self.same_receive_path(old));
        self.receive_loudness = Some(match old_receive {
            Some(old) => old
                .receive_loudness
                .as_ref()
                .expect("prepared RX meter")
                .clone(),
            None => Self::new_output_meter(self.receive_dynamics.clone(), maximum)?,
        });
        self.transmit_loudness = Some(
            match previous
                .filter(|old| Arc::ptr_eq(&self.transmit_program.0, &old.transmit_program.0))
            {
                Some(old) => old
                    .transmit_loudness
                    .as_ref()
                    .expect("prepared TX meter")
                    .clone(),
                None => Self::new_output_meter(self.transmit_program.clone(), maximum)?,
            },
        );
        Ok(self)
    }

    fn new_output_meter(
        graph: SharedGraph,
        maximum: u32,
    ) -> Result<Arc<OutputLoudnessStage>, ProcessingRuntimeError> {
        let meter = OutputLoudnessMeter::new(48_000, maximum as usize)
            .map_err(ProcessingRuntimeError::Loudness)?;
        let observer = meter.observer();
        Ok(Arc::new(OutputLoudnessStage {
            graph,
            meter: UnsafeCell::new(meter),
            observer,
            programme_fence: UnsafeCell::new(None),
        }))
    }

    fn same_receive_path(&self, old: &Self) -> bool {
        let same_optional_graph =
            |left: &Option<SharedGraph>, right: &Option<SharedGraph>| match (left, right) {
                (Some(left), Some(right)) => Arc::ptr_eq(&left.0, &right.0),
                (None, None) => true,
                _ => false,
            };
        self.receive_input_gain == old.receive_input_gain
            && Arc::ptr_eq(&self.receive_deemphasis.0, &old.receive_deemphasis.0)
            && Arc::ptr_eq(&self.receive_filter.0, &old.receive_filter.0)
            && Arc::ptr_eq(&self.receive_dynamics.0, &old.receive_dynamics.0)
            && self
                .receive_ctcss_notch
                .iter()
                .zip(&old.receive_ctcss_notch)
                .all(|(left, right)| same_optional_graph(left, right))
            && same_optional_graph(
                &self.receive_ctcss_tail_notch,
                &old.receive_ctcss_tail_notch,
            )
            && match (&self.receive_noise_reduction, &old.receive_noise_reduction) {
                (Some(left), Some(right)) => Arc::ptr_eq(&left.0, &right.0),
                (None, None) => true,
                _ => false,
            }
    }

    /// Mandatory local output measurement after its final graph and handoff.
    pub fn receive_loudness(&self) -> Option<OutputLoudnessSnapshot> {
        Some(
            self.receive_loudness
                .as_ref()
                .expect("prepared RX meter")
                .observer
                .snapshot(),
        )
    }

    /// Final transmit programme since this graph's meter epoch began. Preparation
    /// and graph warmup are excluded; pauses retain integrated history.
    pub fn transmit_loudness(&self) -> Option<OutputLoudnessSnapshot> {
        Some(
            self.transmit_loudness
                .as_ref()
                .expect("prepared TX meter")
                .observer
                .snapshot(),
        )
    }

    /// Set this block's final-programme fence on the existing TX graph wrapper.
    ///
    /// # Safety
    /// Only the original serial TX owner may call this, between graph operations.
    /// The callback/context must remain live through all ensuing renders, and
    /// obey the bounded no-allocation/no-unwind contract of ProgrammeFence.
    pub unsafe fn set_transmit_programme_fence(
        &self,
        port: Option<ProgrammeFence>,
        generation: u64,
    ) {
        let stage = self.transmit_loudness.as_ref().expect("prepared TX meter");
        // SAFETY: the caller is the sole TX owner between graph operations.
        unsafe {
            *stage.programme_fence.get() =
                port.map(|port| BoundProgrammeFence { port, generation });
        }
    }

    /// Last completed programme render's conservative buffering bound. Access
    /// signaling and RX/RNNoise are outside this TX-only contract.
    ///
    /// # Safety
    /// Call only on the original serial TX owner, between its graph operations.
    /// Control-plane observers must use a separately published immutable record.
    pub unsafe fn transmit_programme_buffering(&self) -> ProgrammeBuffering {
        // SAFETY: the caller is the sole owner of this callback-mutated record.
        unsafe { *self.transmit_program.0.buffering.get() }
    }

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
            receive_loudness: self.receive_loudness.clone(),
            receive_input_gain: self.receive_input_gain,
            transmit_loudness: self.transmit_loudness.clone(),
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
            receive_dynamics: _,
            transmit_program: _,
            receive_loudness,
            receive_input_gain: _,
            transmit_loudness,
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
            receive_dynamics: metered_graph_port(
                receive_loudness.as_ref().expect("prepared RX meter"),
            ),
            transmit_program: metered_graph_port(
                transmit_loudness.as_ref().expect("prepared TX meter"),
            ),
            transmit_dcs_normal_filter: graph_port(transmit_dcs_normal_filter),
            transmit_dcs_turnoff_filter: graph_port(transmit_dcs_turnoff_filter),
            program_ring,
            receive_ctcss_tail_notch: tail_notch_port,
        }
    }
}

fn validate_plan(
    plan: impl Into<ExplicitNativeProcessingPlan>,
) -> Result<(), ProcessingRuntimeError> {
    let plan = plan.into();
    for (role, chain) in [
        (ChainRole::LocalReceive, plan.local.as_ref()),
        (ChainRole::VoiceTelemetry, plan.voice_telemetry.as_ref()),
    ] {
        if let Some(chain) = chain {
            if chain.role != role {
                return Err(ProcessingRuntimeError::IncorrectChainRole);
            }
            chain.validate()?;
        }
    }
    Ok(())
}

fn transmit_lookahead_frames(plan: &ExplicitNativeProcessingPlan) -> usize {
    limiter_lookahead_frames(plan.voice_telemetry.as_ref())
}

fn limiter_lookahead_frames(chain: Option<&ProcessingChain>) -> usize {
    chain
        .filter(|chain| chain.peak_limiter.enabled)
        .map_or(0, |chain| {
            (chain.peak_limiter.lookahead_ms * 48.0).ceil() as usize
        })
}

fn metered_graph_port(stage: &Arc<OutputLoudnessStage>) -> ProcessorPort<'_> {
    // SAFETY: the generation retains this stable stage for its sole RX or TX owner.
    unsafe {
        ProcessorPort::from_raw(
            NonNull::from(stage.as_ref()).cast(),
            metered_graph_process,
            None,
            None,
        )
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

unsafe extern "C" fn metered_graph_process(
    context: *mut c_void,
    input: *const f32,
    output: *mut f32,
    frame_count: u32,
) -> c_int {
    let Some(context) = NonNull::new(context.cast::<OutputLoudnessStage>()) else {
        return PORT_ERROR;
    };
    // SAFETY: the retained stage is borrowed by its sole RX or TX callback owner.
    let stage = unsafe { context.as_ref() };
    // SAFETY: the graph validates the caller's complete input/output spans.
    let result = unsafe {
        graph_process(
            stage.graph.pointer().cast().as_ptr(),
            input,
            output,
            frame_count,
        )
    };
    if result != PORT_OK {
        return result;
    }
    // SAFETY: only this serial owner reads or replaces its borrowed scalar port.
    if let Some(fence) = unsafe { *stage.programme_fence.get() } {
        // Zero is never a physical generation. Do not delegate its admission.
        let permitted = fence.generation != 0
            // SAFETY: the binding contract retains this bounded callback/context.
            && unsafe { (fence.port.permit)(fence.port.context, fence.generation, frame_count) } == 1;
        if !permitted {
            // SAFETY: the successful graph call validated this writable span.
            unsafe { slice::from_raw_parts_mut(output, frame_count as usize) }.fill(0.0);
        }
    }
    // SAFETY: successful graph processing validated this exact output span;
    // its final handoff mix is complete and the corresponding serial callback exclusively owns the meter.
    let (samples, meter) = unsafe {
        (
            slice::from_raw_parts(output, frame_count as usize),
            &mut *stage.meter.get(),
        )
    };
    match meter.process(samples) {
        Ok(()) => PORT_OK,
        Err(_) => PORT_ERROR,
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
        // SAFETY: only this TX/RX owner accesses the stage's current observation.
        unsafe { *stage.buffering.get() = ProgrammeBuffering::default() };
        return PORT_ERROR;
    }
    let mut buffering = graph.buffering_state();
    if stage.complete.load(Ordering::Relaxed) {
        // SAFETY: this serial graph owner publishes only its completed operation.
        unsafe { *stage.buffering.get() = buffering };
        return PORT_OK;
    }
    // Only transition_graph constructs an incomplete stage, always with its
    // predecessor and scratch together; flat stages start complete.
    let handoff = stage
        .handoff
        .as_ref()
        .expect("incomplete graph has a handoff");
    let previous = &handoff.previous;
    // SAFETY: control never accesses these callback-owned scratch/counters.
    let transition = unsafe { &mut *handoff.transition.get() };
    let use_pending = match transition.use_pending {
        Some(selected) => selected,
        None => {
            let audible = stage.audible.load(Ordering::Acquire);
            if stage.primary.pointer().as_ptr() == audible {
                // The inverse target may have finished its own earlier fade
                // after inverse preparation. It is already current and warm.
                stage.complete.store(true, Ordering::Relaxed);
                // SAFETY: this owner just rendered the now-sole audible primary.
                unsafe { *stage.buffering.get() = buffering };
                return PORT_OK;
            }
            // Publication can select only the current or pending flat graph.
            let selected = previous
                .pending
                .as_ref()
                .is_some_and(|graph| graph.pointer().as_ptr() == audible);
            transition.use_pending = Some(selected);
            selected
        }
    };
    let previous = if use_pending {
        previous
            .pending
            .as_ref()
            .expect("selected pending graph exists")
    } else {
        &previous.current
    };
    // SAFETY: this flat, distinct predecessor retains the same sole owner.
    let previous = unsafe { &mut *previous.pointer().as_ptr() };
    // A successful primary process already bounded this frame count by the
    // same prepared maximum used to allocate transition scratch.
    let prior_output = &mut transition.output[..output.len()];
    if previous.process_block(input, prior_output).is_err() {
        // SAFETY: a failed audible contributor invalidates this completed block.
        unsafe { *stage.buffering.get() = ProgrammeBuffering::default() };
        return PORT_ERROR;
    }
    buffering = combine_programme_buffering(buffering, previous.buffering_state());
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
    // SAFETY: the record describes every contributor audible anywhere in this
    // block, including the last predecessor samples of a completed crossfade.
    unsafe { *stage.buffering.get() = buffering };
    PORT_OK
}

fn combine_programme_buffering(
    left: ProgrammeBuffering,
    right: ProgrammeBuffering,
) -> ProgrammeBuffering {
    // Counters and bridge padding have no single identity while graphs mix.
    // Independent bounds remain usable only when every audible graph qualifies.
    ProgrammeBuffering {
        flags: left.flags
            & right.flags
            & (PROGRAMME_PREPARED_BOUND_VALID | PROGRAMME_CURRENT_BOUND_VALID),
        prepared_max_delay_frames: left
            .prepared_max_delay_frames
            .max(right.prepared_max_delay_frames),
        current_max_delay_frames: left
            .current_max_delay_frames
            .max(right.current_max_delay_frames),
        ..ProgrammeBuffering::default()
    }
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
