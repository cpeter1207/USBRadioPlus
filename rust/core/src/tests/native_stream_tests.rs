use super::*;

#[test]
fn stream_rate_and_callback_bound_are_fixed_at_construction() {
    assert_eq!(
        NativeStreamSpec::new(8_000, 960),
        Err(StreamSpecError::UnsupportedSampleRate)
    );
    assert_eq!(
        NativeStreamSpec::new(NATIVE_SAMPLE_RATE_HZ, 0),
        Err(StreamSpecError::EmptyMaximumFrameCount)
    );
    assert_eq!(
        NativeStreamSpec::new(NATIVE_SAMPLE_RATE_HZ, 960)
            .unwrap()
            .maximum_frame_count(),
        960
    );
}
