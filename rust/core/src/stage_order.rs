//! Validated order for the optional FFmpeg processing stages.

use std::fmt;

/// Reorderable stage in one source processing chain.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum ProcessingStage {
    /// Downward expander.
    Expander,
    /// Automatic gain control.
    Agc,
    /// Dynamic-range compressor.
    Compressor,
    /// Shared FFmpeg multiband limiter.
    Limiter,
    /// Three-band parametric equalizer.
    Equalizer,
    /// Split-band de-esser.
    Deesser,
}

impl ProcessingStage {
    /// Parse one case-insensitive configuration token.
    pub fn parse(text: &str) -> Option<Self> {
        match text.trim().to_ascii_lowercase().as_str() {
            "expander" => Some(Self::Expander),
            "agc" => Some(Self::Agc),
            "compressor" => Some(Self::Compressor),
            "limiter" => Some(Self::Limiter),
            "equalizer" => Some(Self::Equalizer),
            "deesser" => Some(Self::Deesser),
            _ => None,
        }
    }

    /// Return the stable configuration token for this stage.
    pub const fn name(self) -> &'static str {
        match self {
            Self::Expander => "expander",
            Self::Agc => "agc",
            Self::Compressor => "compressor",
            Self::Limiter => "limiter",
            Self::Equalizer => "equalizer",
            Self::Deesser => "deesser",
        }
    }
}

/// Invalid optional-stage order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum StageOrderError {
    /// The comma-separated input exceeded the established configuration limit.
    TooLong,
    /// A token was empty, unknown, or named a fixed processing stage.
    UnknownStage(String),
    /// A reorderable stage appeared more than once.
    DuplicateStage(ProcessingStage),
    /// An enabled stage was omitted from the configured order.
    MissingEnabledStage(ProcessingStage),
}

impl fmt::Display for StageOrderError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooLong => formatter.write_str("stage order is too long"),
            Self::UnknownStage(stage) => {
                write!(formatter, "unknown, empty, or fixed stage '{stage}'")
            }
            Self::DuplicateStage(stage) => write!(formatter, "duplicate stage '{}'", stage.name()),
            Self::MissingEnabledStage(stage) => {
                write!(formatter, "enabled stage '{}' is missing", stage.name())
            }
        }
    }
}

impl std::error::Error for StageOrderError {}

/// Ordered, non-repeating optional processing stages.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StageOrder {
    stages: Vec<ProcessingStage>,
}

impl StageOrder {
    /// Return the shipped optional-stage order.
    pub fn standard() -> Self {
        Self {
            stages: vec![
                ProcessingStage::Equalizer,
                ProcessingStage::Expander,
                ProcessingStage::Agc,
                ProcessingStage::Deesser,
                ProcessingStage::Compressor,
                ProcessingStage::Limiter,
            ],
        }
    }

    /// Parse one comma-separated order and require every enabled stage.
    pub fn parse(text: &str, enabled: &[ProcessingStage]) -> Result<Self, StageOrderError> {
        if text.len() >= 128 {
            return Err(StageOrderError::TooLong);
        }
        let mut stages = Vec::with_capacity(6);
        for token in text.split(',') {
            let stage = ProcessingStage::parse(token)
                .ok_or_else(|| StageOrderError::UnknownStage(token.trim().to_owned()))?;
            if stages.contains(&stage) {
                return Err(StageOrderError::DuplicateStage(stage));
            }
            stages.push(stage);
        }
        for &stage in enabled {
            if !stages.contains(&stage) {
                return Err(StageOrderError::MissingEnabledStage(stage));
            }
        }
        Ok(Self { stages })
    }

    /// Return the ordered stages as an immutable slice.
    pub fn stages(&self) -> &[ProcessingStage] {
        &self.stages
    }

    /// Require every enabled stage to appear in this order.
    pub fn require_enabled(&self, enabled: &[ProcessingStage]) -> Result<(), StageOrderError> {
        for &stage in enabled {
            if !self.stages.contains(&stage) {
                return Err(StageOrderError::MissingEnabledStage(stage));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "tests/stage_order_tests.rs"]
mod tests;
