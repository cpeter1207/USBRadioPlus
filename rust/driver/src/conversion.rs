//! Fixed app_rpt conversion composed from the released sample-rate adapter.

use usbradioplus_asl3::{AppRptConverter, ConversionError, ConversionProgress};
use usbradioplus_samplerate::{Converter, Quality, SampleRateAdapter, SampleRateError};

const APP_RPT_TO_NATIVE_RATIO: f64 = 8_000.0 / 48_000.0;

/// Prepare the persistent highest-quality native-to-app_rpt converter.
///
/// # Errors
///
/// Returns the released sample-rate adapter's setup failure.
pub fn prepare_app_rpt_converter(
    adapter: &SampleRateAdapter,
) -> Result<Box<dyn AppRptConverter>, SampleRateError> {
    Ok(Box::new(AppRptRateConverter(
        adapter.create(Quality::SincBest)?,
    )))
}

struct AppRptRateConverter(Converter);

impl AppRptConverter for AppRptRateConverter {
    fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
    ) -> Result<ConversionProgress, ConversionError> {
        self.0
            .process(input, output, APP_RPT_TO_NATIVE_RATIO)
            .map(|result| ConversionProgress {
                input_used: result.input_used,
                output_generated: result.output_generated,
            })
            .map_err(|_| ConversionError)
    }
}

#[cfg(test)]
#[path = "tests/conversion_tests.rs"]
mod tests;
