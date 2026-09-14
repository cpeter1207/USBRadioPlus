//! Complete composition of one resolved station generation.

use std::fmt;

use usbradioplus_asl3::EchoConfiguration;
use usbradioplus_core::ResolvedChannelConfiguration;
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_radio::RadioProvider;
use usbradioplus_ring::RingProvider;
use usbradioplus_rnnoise::DenoiseProvider;
use usbradioplus_runtime::NativeProcessingFactory;
use usbradioplus_samplerate::{SampleRateAdapter, SampleRateError};
use usbradioplus_station::{
    ControllerSetup, ControllerTransport, StationMedia, StationMediaError, StationPlan,
    StationPreparationError,
};

use crate::prepare_app_rpt_converter;

/// Transport-specific Asterisk handoff configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControllerConfiguration {
    /// Fixed 8 kHz app_rpt compatibility transport.
    AppRpt {
        /// Bounded callback-to-Asterisk handoff slots.
        handoff_slots: usize,
        /// Retained app_rpt echo policy.
        echo: EchoConfiguration,
    },
    /// Native 48 kHz rpt_advanced transport.
    RptAdvanced {
        /// Bounded callback-to-Asterisk handoff slots.
        handoff_slots: usize,
    },
}

impl ControllerConfiguration {
    const fn transport(self) -> ControllerTransport {
        match self {
            Self::AppRpt { .. } => ControllerTransport::AppRpt,
            Self::RptAdvanced { .. } => ControllerTransport::RptAdvanced,
        }
    }
}

/// Failure to construct the factory or compose one station generation.
#[derive(Debug)]
pub enum StationFactoryError {
    /// Station planning or native processing preparation failed.
    Station(StationPreparationError),
    /// The app_rpt sample-rate converter could not be prepared.
    AppRptConverter(SampleRateError),
    /// Prepared station resources could not be bound to released providers.
    Media(StationMediaError),
}

impl fmt::Display for StationFactoryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Station(error) => write!(formatter, "station preparation failed: {error}"),
            Self::AppRptConverter(error) => {
                write!(formatter, "app_rpt converter preparation failed: {error}")
            }
            Self::Media(error) => write!(formatter, "station media binding failed: {error}"),
        }
    }
}

impl std::error::Error for StationFactoryError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Station(error) => Some(error),
            Self::AppRptConverter(error) => Some(error),
            Self::Media(error) => Some(error),
        }
    }
}

impl From<StationPreparationError> for StationFactoryError {
    fn from(error: StationPreparationError) -> Self {
        Self::Station(error)
    }
}

impl From<SampleRateError> for StationFactoryError {
    fn from(error: SampleRateError) -> Self {
        Self::AppRptConverter(error)
    }
}

impl From<StationMediaError> for StationFactoryError {
    fn from(error: StationMediaError) -> Self {
        Self::Media(error)
    }
}

/// Reusable owner of validated released providers and native DSP setup.
#[derive(Clone)]
pub struct StationFactory {
    processing: NativeProcessingFactory,
    providers: StationProviders,
    maximum_frame_count: u32,
}

/// Validated released capabilities required by every station generation.
#[derive(Clone, Copy)]
pub struct StationProviders {
    /// FFmpeg processing-graph capability.
    pub graph: GraphProvider,
    /// RNNoise denoising capability.
    pub denoise: DenoiseProvider,
    /// Rate-adjusting program-ring capability.
    pub ring: RingProvider,
    /// Native radio-session capability.
    pub radio: RadioProvider,
    /// Sample-rate conversion capability used by app_rpt.
    pub sample_rate: SampleRateAdapter,
}

impl StationFactory {
    /// Validate the fixed 48 kHz native stream and construct its DSP factory.
    ///
    /// The providers must already have passed their released ABI validation.
    ///
    /// # Errors
    ///
    /// Returns a typed processing error for an invalid frame bound or graph
    /// description configuration.
    pub fn new(
        providers: StationProviders,
        agc_plugin_path: impl Into<String>,
        maximum_frame_count: u32,
    ) -> Result<Self, StationFactoryError> {
        let processing = NativeProcessingFactory::new(
            providers.graph,
            providers.denoise,
            agc_plugin_path,
            maximum_frame_count,
        )
        .map_err(StationPreparationError::from)?;
        Ok(Self {
            processing,
            providers,
            maximum_frame_count,
        })
    }

    /// Plan, prepare, warm, and bind one resolved station generation.
    ///
    /// No device is opened and no callback is published by this operation.
    ///
    /// # Errors
    ///
    /// Returns the failing station-planning, processing, converter, program
    /// ring, controller-handoff, or radio-provider operation with its source.
    pub fn prepare(
        &self,
        channel: ResolvedChannelConfiguration,
        generation_id: u64,
        controller: ControllerConfiguration,
    ) -> Result<StationMedia, StationFactoryError> {
        let transport = controller.transport();
        let channel_name = channel.channel().to_owned();
        let plan = StationPlan::new(
            channel_name,
            channel.into_config(),
            transport,
            generation_id,
            self.maximum_frame_count,
        )?;
        let prepared = plan.prepare(&self.processing)?;
        let controller = match controller {
            ControllerConfiguration::AppRpt {
                handoff_slots,
                echo,
            } => ControllerSetup::AppRpt {
                converter: prepare_app_rpt_converter(&self.providers.sample_rate)?,
                handoff_slots,
                echo,
            },
            ControllerConfiguration::RptAdvanced { handoff_slots } => {
                ControllerSetup::RptAdvanced { handoff_slots }
            }
        };
        prepared
            .bind_media(self.providers.ring, self.providers.radio, controller)
            .map_err(StationFactoryError::from)
    }
}

#[cfg(test)]
#[path = "tests/factory_tests.rs"]
mod tests;
