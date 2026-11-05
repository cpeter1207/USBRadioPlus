//! USBRadioPlus channel-driver ownership and lifecycle.
//!
//! This crate owns product configuration and station lifetimes. The Rust
//! Asterisk host translates only Asterisk objects and operations at the edge.

mod configuration;
mod conversion;
mod factory;
mod hardware_host;
mod hardware_site;
mod link;
mod registry;
mod remote_base;
mod remote_hardware;
mod remote_owner;
mod remote_setup;
pub use remote_owner::{RemoteDevice, RemoteOwner};
pub use remote_setup::{RemoteSetup, RemoteSetupError};
mod remote_request;
pub use remote_base::{
    RemoteQueue, RemoteReply, RemoteRequestError, RemoteSource, RemoteSourceLease, RemoteTicket,
    RemoteWork,
};
pub use remote_hardware::{RemotePhysicalWiring, RemotePttSource};
pub use remote_request::{PreparedRemoteOperation, prepare_remote_operation};

pub use configuration::{DriverConfiguration, DriverConfigurationError};
pub use conversion::prepare_app_rpt_converter;
pub use factory::{ControllerConfiguration, StationFactory, StationFactoryError, StationProviders};
pub use hardware_host::{
    EepromTuning, HardwareInput, HardwareInputEvent, HardwareMaintenance, HardwareMixer,
    HardwareOperation, HardwarePreflight, HardwareService, HardwareServiceStatistics,
    HardwareStation, HardwareStationDiagnostics, HardwareStationError, HardwareTransientState,
    select_native_hardware,
};
pub use hardware_site::{
    SiteBank, SiteDigitalBinding, SiteDigitalRequest, SiteDigitalSnapshot, SiteError,
    SiteReadQuality,
};
pub use link::{LinkPreparationError, LinkProcessingFactory};
pub use registry::ConfigurationRegistry;

#[cfg(test)]
#[path = "tests/support.rs"]
mod test_support;
