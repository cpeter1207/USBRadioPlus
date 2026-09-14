//! Single ASL3 compatibility boundary for USBRadioPlus.
//!
//! This crate owns Asterisk-rate PCM representation, controller compatibility
//! state, and the lock-free handoff from native audio callbacks to a
//! non-real-time Asterisk delivery owner. It contains no radio DSP, hardware
//! I/O, Asterisk types, logging, or audio-thread allocation.

mod adapter;
mod control;
mod handoff;
mod link;
mod pcm;

pub use adapter::{
    AdapterError, AdapterSetupError, AppRptConverter, ControllerReloadState, ControllerState,
    ConversionError, ConversionProgress, CtcssToneIndex, DeliveryAction, DtmfAction, DtmfEvent,
    DtmfEventKind, EchoConfiguration, ProgramProducer, ProgramProducerError, ProgramSource,
    ProgramWrite, PublishReport, ReceiveMetadata, ReceivePublisher, ReceiveQualification,
    ReceiveStatus, StatusObservation,
};
pub use control::{
    ControlAction, ControlMessage, ControlParseError, ControlSnapshot, CtcssTone, GpioPin,
    OutputRequest, ParallelPin, RemoteRadio, parse_controller_text,
};
pub use handoff::{
    Consumer, HandoffError, HandoffObservation, LatestHandoff, Producer, PublishOutcome,
};
pub use link::{
    ExactRateGraph, LinkDirection, LinkError, LinkObservation, LinkProcessOutcome, PreparedLink,
};
pub use pcm::{
    ADVANCED_FRAME_SAMPLES, APP_RPT_FRAME_SAMPLES, AsteriskPcmMode, ControllerPcmFrame,
    FrameAssembler, PcmBoundaryError, f32_to_s16, s16_to_f32,
};
