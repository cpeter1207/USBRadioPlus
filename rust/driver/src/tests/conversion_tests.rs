use super::*;

use crate::test_support::sample_rate_adapter;

#[test]
fn converter_uses_best_quality_fixed_ratio_and_maps_results() {
    let mut converter = prepare_app_rpt_converter(&sample_rate_adapter()).unwrap();
    let input = [1.0_f32; 12];
    let mut output = [0.0; 2];
    assert_eq!(
        converter.process(&input, &mut output).unwrap(),
        ConversionProgress {
            input_used: 12,
            output_generated: 2,
        }
    );
    assert_eq!(output, [1.0, 1.0]);
    assert_eq!(converter.process(&input, &mut output), Err(ConversionError));
}
