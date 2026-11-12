//! Prepared native processing ownership for USBRadioPlus.
//!
//! Configuration and graph construction stay on the control plane. The
//! prepared generation then lends allocation-free processor ports to one
//! radio session.

mod processing;

pub use processing::{
    ExplicitNativeProcessingPlan, NativeProcessingFactory, NativeProcessingPlan,
    ProcessingGeneration, ProcessingRuntimeError, ProgrammeFence,
};

// Reuse the SDK's thread-local test allocator guard; production has no allocator hook.
#[cfg(test)]
#[path = "../../station/src/tests/allocation.rs"]
mod allocation;
