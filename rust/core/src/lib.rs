//! Adapter-neutral USBRadioPlus runtime components.
//!
//! This private Rust library owns USBRadioPlus policy and state while keeping
//! Asterisk and external C-library types outside the core. Complete objects
//! move here as their former C owners are removed.

mod channel_configuration;
mod config_document;
mod ffmpeg_graph;
mod native_stream;
mod processing_profile;
mod profile_resolution;
mod stage_order;
mod station_config;

pub use channel_configuration::{
    ChannelConfiguration, ChannelConfigurationError, ResolvedChannelConfiguration,
};
pub use config_document::{ConfigDocument, ConfigError};
pub use ffmpeg_graph::{GraphDescriptionError, GraphDescriptionFactory};
pub use native_stream::{NATIVE_SAMPLE_RATE_HZ, NativeStreamSpec, StreamSpecError};
pub use processing_profile::{
    Agc, BandLayout, ChainRole, Compressor, Deesser, DynamicsBand, Equalizer, Expander, Limiter,
    PlFilter, ProcessingChain, ProcessingConfigError, ReceiveConditioning, TransmitTail,
};
pub use profile_resolution::{
    ProfileResolutionError, RawOverlay, ResolutionWarning, ResolutionWarningKind, ResolvedProfile,
};
pub use stage_order::{ProcessingStage, StageOrder, StageOrderError};
pub use station_config::{
    AsteriskConfig, CarrierSource, CtcssConfig, CtcssSource, CtcssTone, CtcssToneParseError,
    CtcssTurnoffMode, DcsCode, DcsCodeParseError, DcsConfig, DcsPolarity, DiagnosticsConfig,
    DuplexConfig, GpioMode, HardwareConfig, HardwareInterfaceType, HardwareOutputAssignment,
    JitterBufferImplementation, ParallelInputAssignment, ParallelOutputAssignment,
    ParallelPortConfig, RadioDuplexMode, ReceiveAudioSource, ReceiveConfig, ResolvedStationConfig,
    SignalingMethod, StationConfig, StationConfigError, StationValidationError, TransmitConfig,
};
