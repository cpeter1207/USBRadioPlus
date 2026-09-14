//! Prepared native processing ownership for USBRadioPlus.
//!
//! Configuration and graph construction stay on the control plane. The
//! prepared generation then lends allocation-free processor ports to one
//! radio session.

mod processing;

pub use processing::{
    NativeProcessingFactory, NativeProcessingPlan, ProcessingGeneration, ProcessingRuntimeError,
};
