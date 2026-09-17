//! USBRadioPlus channel-driver ownership and lifecycle.
//!
//! This crate owns product configuration and station lifetimes. The Rust
//! Asterisk host translates only Asterisk objects and operations at the edge.

mod configuration;
mod conversion;
mod factory;
mod hardware_host;
mod link;
mod registry;

pub use configuration::{DriverConfiguration, DriverConfigurationError};
pub use conversion::prepare_app_rpt_converter;
pub use factory::{ControllerConfiguration, StationFactory, StationFactoryError, StationProviders};
pub use hardware_host::{
    EepromTuning, HardwareInput, HardwareInputEvent, HardwareMixer, HardwareOperation,
    HardwarePreflight, HardwareServiceStatistics, HardwareStation, HardwareStationDiagnostics,
    HardwareStationError, HardwareTransientState,
};
pub use link::{LinkPreparationError, LinkProcessingFactory};
pub use registry::ConfigurationRegistry;

#[cfg(test)]
#[path = "tests/support.rs"]
mod test_support;
