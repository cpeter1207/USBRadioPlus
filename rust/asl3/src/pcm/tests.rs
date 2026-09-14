use super::*;

#[test]
fn pcm_conversion_preserves_rails_rounding_and_silence() {
    assert_eq!(s16_to_f32(i16::MIN), -1.0);
    assert_eq!(s16_to_f32(0), 0.0);
    assert_eq!(f32_to_s16(-1.0), i16::MIN);
    assert_eq!(f32_to_s16(1.0), i16::MAX);
    assert_eq!(f32_to_s16(2.0), i16::MAX);
    assert_eq!(f32_to_s16(-2.0), i16::MIN);
    assert_eq!(f32_to_s16(f32::NAN), 0);
    assert_eq!(f32_to_s16(f32::INFINITY), 0);
    assert_eq!(f32_to_s16(2.5 / 32_768.0), 2);
    assert_eq!(f32_to_s16(3.5 / 32_768.0), 4);
}

#[test]
fn both_transport_modes_report_fixed_boundaries() {
    assert_eq!(AsteriskPcmMode::AppRpt.sample_rate_hz(), 8_000);
    assert_eq!(AsteriskPcmMode::AppRpt.frame_samples(), 160);
    assert_eq!(AsteriskPcmMode::Advanced.sample_rate_hz(), 48_000);
    assert_eq!(AsteriskPcmMode::Advanced.frame_samples(), 960);
}

#[test]
fn assembler_retains_partial_and_multiple_frames() {
    let mut assembler = FrameAssembler::new(AsteriskPcmMode::AppRpt);
    assembler.append(&[1; 100]).unwrap();
    let mut frame = [0; APP_RPT_FRAME_SAMPLES];
    assert!(!assembler.take(&mut frame).unwrap());
    assembler.append(&[2; 220]).unwrap();
    assert!(assembler.take(&mut frame).unwrap());
    assert_eq!(&frame[..100], &[1; 100]);
    assert_eq!(&frame[100..], &[2; 60]);
    assert_eq!(assembler.pending_samples(), 160);
    assert!(assembler.take(&mut frame).unwrap());
    assert_eq!(frame, [2; APP_RPT_FRAME_SAMPLES]);
    assert_eq!(assembler.pending_samples(), 0);
    assembler.append(&[3; 10]).unwrap();
    assembler.clear();
    assert_eq!(assembler.pending_samples(), 0);
}

#[test]
fn assembler_rejects_invalid_bounds() {
    let mut assembler = FrameAssembler::new(AsteriskPcmMode::Advanced);
    assert_eq!(
        assembler.append(&[1; ASSEMBLY_CAPACITY + 1]),
        Err(PcmBoundaryError::AssemblyOverflow)
    );
    assert_eq!(
        assembler.take(&mut [0; APP_RPT_FRAME_SAMPLES]),
        Err(PcmBoundaryError::InvalidFrameSize)
    );
    assert_eq!(
        assembler.take_padded(&mut [0; APP_RPT_FRAME_SAMPLES]),
        Err(PcmBoundaryError::InvalidFrameSize)
    );
    assert!(!PcmBoundaryError::AssemblyOverflow.to_string().is_empty());
    assert!(!PcmBoundaryError::InvalidFrameSize.to_string().is_empty());
}
