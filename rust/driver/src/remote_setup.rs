//! Selected provider admission before physical acquisition; contexts open on the worker.

use crate::{RemoteOwner, RemoteQueue};
use std::{fmt, sync::Arc};
use usbradioplus_core::{CarrierSource, RemoteBackend, ResolvedRemoteConfiguration};
use usbradioplus_radio::SubaudibleSource;
use usbradioplus_remote::{
    CallError, HamlibOperating, HamlibProvider, HamlibRig, ProviderError, RemoteProvider,
    SelectedCapabilities, Setting,
};
use usbradioplus_station::{HardwarePlan, RemoteDspControl, RemoteDspPort};

/// Provider admission failure, without secret-bearing transport options.
#[derive(Debug)]
pub enum RemoteSetupError {
    /// Missing/incompatible selected shared library.
    Provider(ProviderError),
    /// The selected model or copied operating contract was rejected.
    Call(CallError),
}
impl fmt::Display for RemoteSetupError {
    fn fmt(&self, output: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Provider(error) => error.fmt(output),
            Self::Call(error) => error.fmt(output),
        }
    }
}
impl std::error::Error for RemoteSetupError {}

/// Immutable, Send setup. It contains no rig/session, station pointer or live
/// control observer. Create actual contexts only on the existing hardware worker.
pub struct RemoteSetup {
    configuration: ResolvedRemoteConfiguration,
    engine: Arc<RemoteProvider>,
    hamlib: Option<Arc<HamlibProvider>>,
    plan: HardwarePlan,
    carrier: CarrierSource,
    external_decode: SubaudibleSource,
    physical_duplex: bool,
    dsp: Arc<RemoteDspPort>,
}
impl RemoteSetup {
    /// Admit selected DSOs before preflight acquires hardware. Return the
    /// original-control DSP pump separately; move only setup to the worker.
    pub fn prepare(
        configuration: ResolvedRemoteConfiguration,
        mut plan: HardwarePlan,
        carrier: CarrierSource,
        qualification: usbradioplus_radio::QualificationConfig,
    ) -> Result<(Self, RemoteDspControl), RemoteSetupError> {
        if configuration.audio_source == usbradioplus_core::RemoteAudioSource::Alsa {
            plan.cm119 = None;
        }
        crate::remote_hardware::external_encoder(&configuration, &plan)
            .map_err(|_| RemoteSetupError::Call(CallError(-6)))?;
        let engine = Arc::new(RemoteProvider::load().map_err(RemoteSetupError::Provider)?);
        usbradioplus_remote::RemoteSession::validate_policy(Arc::clone(&engine), &configuration)
            .map_err(RemoteSetupError::Call)?;
        let hamlib = if configuration.backend == RemoteBackend::Hamlib {
            let provider = Arc::new(HamlibProvider::load().map_err(RemoteSetupError::Provider)?);
            if !provider
                .has_model(&configuration.hamlib_model)
                .map_err(RemoteSetupError::Call)?
            {
                return Err(RemoteSetupError::Call(CallError(-1001)));
            }
            Some(provider)
        } else {
            None
        };
        let control = RemoteDspControl::new();
        Ok((
            Self {
                configuration,
                engine,
                hamlib,
                plan,
                carrier,
                external_decode: qualification.subaudible_source,
                physical_duplex: qualification.radio_duplex,
                dsp: control.port(),
            },
            control,
        ))
    }
    /// Immutable source policy and definitions, copied before device access.
    pub fn configuration(&self) -> &ResolvedRemoteConfiguration {
        &self.configuration
    }
    /// Prepared physical assignments, without assumed CM119 capabilities.
    pub fn hardware(&self) -> &HardwarePlan {
        &self.plan
    }
    /// Inherited carrier polarity/source used in actual capability selection.
    pub fn inherited_carrier(&self) -> CarrierSource {
        self.carrier
    }
    /// Inherited external qualifier, independent of Hamlib DCD.
    pub fn external_decode(&self) -> SubaudibleSource {
        self.external_decode
    }
    /// Physical radio duplex, independent of the controller's software routing.
    pub fn physical_duplex(&self) -> bool {
        self.physical_duplex
    }
    /// Worker port for graph preparation and existing callback adoption ACKs.
    pub fn dsp(&self) -> &RemoteDspPort {
        &self.dsp
    }
    /// Open only on the physical worker. Parallel-only compositions open no rig.
    pub fn open_hamlib_on_owner(&self) -> Result<Option<HamlibOperating>, CallError> {
        self.hamlib
            .as_ref()
            .map(|provider| {
                HamlibRig::configured(Arc::clone(provider), &self.configuration)
                    .and_then(HamlibOperating::new)
            })
            .transpose()
    }
    /// Create the dynamic operating context with actual selected capabilities,
    /// on the same worker which owns its rig/GPIO operations.
    pub fn open_on_owner(
        &self,
        caps: &SelectedCapabilities,
        settings: Vec<Setting>,
        queue: Arc<RemoteQueue>,
    ) -> Result<RemoteOwner, CallError> {
        RemoteOwner::new(
            self.configuration.clone(),
            caps,
            settings,
            queue,
            Arc::clone(&self.engine),
        )
    }
}

#[cfg(test)]
#[path = "tests/remote_setup_tests.rs"]
mod tests;
