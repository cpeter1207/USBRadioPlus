//! Side-effect-free station planning and prepared processing ownership.

use std::fmt;

use usbradioplus_core::ChannelConfiguration;
use usbradioplus_radio::{RadioError, SessionConfig};
use usbradioplus_runtime::{
    NativeProcessingFactory, NativeProcessingPlan, ProcessingGeneration, ProcessingRuntimeError,
};

use crate::{
    ControllerTransport, HardwarePlan, ProgramRingPlan, hardware_plan, native_processing_plan,
    program_ring_plan, radio_session_config,
};

/// Failure while constructing a replacement station generation.
#[derive(Debug)]
pub enum StationPreparationError {
    /// Configuration could not be represented by the radio core.
    Radio(RadioError),
    /// A native processing graph or denoiser could not be prepared and warmed.
    Processing(ProcessingRuntimeError),
}

impl fmt::Display for StationPreparationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Radio(error) => write!(formatter, "radio preparation failed: {error}"),
            Self::Processing(error) => write!(formatter, "processing preparation failed: {error}"),
        }
    }
}

impl std::error::Error for StationPreparationError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Radio(error) => Some(error),
            Self::Processing(error) => Some(error),
        }
    }
}

impl From<RadioError> for StationPreparationError {
    fn from(error: RadioError) -> Self {
        Self::Radio(error)
    }
}

impl From<ProcessingRuntimeError> for StationPreparationError {
    fn from(error: ProcessingRuntimeError) -> Self {
        Self::Processing(error)
    }
}

/// Immutable product-composition plan for one resolved channel generation.
///
/// Construction performs no external I/O and does not alter a running
/// station. A reload can therefore retain its active generation whenever
/// planning or subsequent preparation fails.
#[derive(Clone, Debug, PartialEq)]
pub struct StationPlan {
    channel: String,
    transport: ControllerTransport,
    configuration: ChannelConfiguration,
    hardware: HardwarePlan,
    program_ring: ProgramRingPlan,
    radio: SessionConfig,
    processing: NativeProcessingPlan,
}

impl StationPlan {
    /// Translate one already-resolved channel into a complete preparation plan.
    ///
    /// # Errors
    ///
    /// Returns an error when a validated setting cannot be represented by the
    /// radio-core session contract.
    pub fn new(
        channel: impl Into<String>,
        configuration: ChannelConfiguration,
        transport: ControllerTransport,
        generation_id: u64,
        maximum_frame_count: u32,
    ) -> Result<Self, StationPreparationError> {
        if maximum_frame_count == 0 {
            return Err(RadioError::InvalidArgument.into());
        }
        Ok(Self {
            hardware: hardware_plan(&configuration),
            program_ring: program_ring_plan(transport),
            radio: radio_session_config(
                &configuration,
                transport,
                generation_id,
                maximum_frame_count,
            )?,
            processing: native_processing_plan(&configuration),
            channel: channel.into(),
            transport,
            configuration,
        })
    }

    /// Return the configured channel name.
    #[must_use]
    pub fn channel(&self) -> &str {
        &self.channel
    }

    /// Return the configured Asterisk controller transport.
    #[must_use]
    pub const fn transport(&self) -> ControllerTransport {
        self.transport
    }

    /// Return the channel's complete effective configuration.
    #[must_use]
    pub const fn configuration(&self) -> &ChannelConfiguration {
        &self.configuration
    }

    /// Return the hardware-selection and signaling request.
    #[must_use]
    pub const fn hardware(&self) -> &HardwarePlan {
        &self.hardware
    }

    /// Return the sole transmitter program-ring policy.
    #[must_use]
    pub const fn program_ring(&self) -> ProgramRingPlan {
        self.program_ring
    }

    /// Return the immutable radio-session configuration.
    #[must_use]
    pub const fn radio(&self) -> &SessionConfig {
        &self.radio
    }

    /// Return the immutable native-processing plan.
    #[must_use]
    pub const fn processing(&self) -> &NativeProcessingPlan {
        &self.processing
    }

    /// Prepare and warm the plan's selected processing outside audio callbacks.
    ///
    /// This is the only external-provider operation in station preparation.
    /// It does not publish the candidate or open a hardware device.
    ///
    /// # Errors
    ///
    /// Returns the processing provider's typed setup failure. The plan and any
    /// existing active station remain untouched.
    pub fn prepare(
        self,
        factory: &NativeProcessingFactory,
    ) -> Result<PreparedStation, StationPreparationError> {
        let processing = factory.prepare(&self.processing)?;
        Ok(PreparedStation {
            plan: self,
            processing,
        })
    }
}

/// One fully planned station with all native processors prepared and warmed.
///
/// The later lifecycle binder consumes this value, prepares the program ring,
/// and borrows both owners while creating a radio session. Keeping that phase
/// separate avoids self-referential ownership and leaves device handoff policy
/// with the station host.
pub struct PreparedStation {
    plan: StationPlan,
    processing: ProcessingGeneration,
}

impl PreparedStation {
    /// Return the immutable plan used to prepare this generation.
    #[must_use]
    pub const fn plan(&self) -> &StationPlan {
        &self.plan
    }

    /// Transfer the plan and processing owner to the runtime lifecycle binder.
    #[must_use]
    pub fn into_parts(self) -> (StationPlan, ProcessingGeneration) {
        (self.plan, self.processing)
    }
}

#[cfg(test)]
#[path = "control_tests.rs"]
pub(crate) mod tests;
