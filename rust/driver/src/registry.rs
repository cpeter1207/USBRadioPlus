//! Atomic module-configuration replacement outside real-time paths.

use std::sync::{Arc, RwLock};

use crate::{DriverConfiguration, DriverConfigurationError};

/// Active immutable driver configuration shared by channel-control owners.
///
/// Parsing and validation complete before the short publication lock is
/// acquired. Existing users retain their `Arc`, while later users receive the
/// complete replacement. Native audio callbacks never access this registry.
pub struct ConfigurationRegistry {
    active: RwLock<Arc<DriverConfiguration>>,
}

impl ConfigurationRegistry {
    /// Start a registry with one fully resolved configuration generation.
    #[must_use]
    pub fn new(initial: DriverConfiguration) -> Self {
        Self {
            active: RwLock::new(Arc::new(initial)),
        }
    }

    /// Retain the complete active generation for a control-plane operation.
    #[must_use]
    pub fn snapshot(&self) -> Arc<DriverConfiguration> {
        Arc::clone(
            &self
                .active
                .read()
                .unwrap_or_else(std::sync::PoisonError::into_inner),
        )
    }

    /// Resolve a candidate completely without publishing it.
    ///
    /// # Errors
    ///
    /// Returns the candidate's configuration error without changing the
    /// active generation.
    pub fn prepare(
        source: impl Into<String>,
        text: impl Into<String>,
    ) -> Result<Arc<DriverConfiguration>, DriverConfigurationError> {
        Ok(Arc::new(DriverConfiguration::parse(source, text)?))
    }

    /// Publish one previously resolved candidate in a single replacement.
    pub fn publish(&self, candidate: Arc<DriverConfiguration>) {
        *self
            .active
            .write()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = candidate;
    }
}

#[cfg(test)]
#[path = "tests/registry_tests.rs"]
mod tests;
