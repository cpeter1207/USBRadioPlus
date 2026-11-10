use super::*;

fn counters(mut observation: LinkObservation) -> LinkObservation {
    observation.loudness = Default::default();
    observation
}

struct Gain {
    factor: f32,
    succeeds: bool,
}

impl ExactRateGraph for Gain {
    fn process_exact(&mut self, input: &[f32], output: &mut [f32]) -> bool {
        for (output, input) in output.iter_mut().zip(input) {
            *output = *input * self.factor;
        }
        self.succeeds
    }
}

fn graph(factor: f32, succeeds: bool) -> Box<dyn ExactRateGraph> {
    Box::new(Gain { factor, succeeds })
}

#[test]
fn setup_requires_positive_exact_bounds() {
    assert!(matches!(
        PreparedLink::new(0, 160, graph(1.0, true)),
        Err(LinkError::InvalidConfiguration)
    ));
    assert!(matches!(
        PreparedLink::new(8_000, 0, graph(1.0, true)),
        Err(LinkError::InvalidConfiguration)
    ));
    assert!(!LinkError::InvalidConfiguration.to_string().is_empty());

    let link = PreparedLink::new(8_000, 160, graph(1.0, true)).unwrap();
    assert_eq!(counters(link.observe()), LinkObservation::default());
    assert_eq!(link.buffering_state(), ProgrammeBuffering::default());
}

#[test]
fn exact_read_frames_are_converted_processed_and_saturated() {
    let mut link = PreparedLink::new(8_000, 4, graph(2.0, true)).unwrap();
    let mut pcm = [i16::MIN, -1, 1, i16::MAX];
    assert_eq!(
        link.process_s16(LinkDirection::Read, 8_000, &mut pcm),
        LinkProcessOutcome::Processed
    );
    assert_eq!(pcm, [i16::MIN, -2, 2, i16::MAX]);
    assert_eq!(
        counters(link.observe()),
        LinkObservation {
            processed_blocks: 1,
            bypassed_blocks: 0,
            failed_blocks: 0,
            ..LinkObservation::default()
        }
    );
}

#[test]
fn nonmatching_frames_bypass_without_changing_pcm() {
    let mut link = PreparedLink::new(8_000, 2, graph(2.0, true)).unwrap();
    let mut wrong_direction = [1];
    assert_eq!(
        link.process_s16(LinkDirection::Write, 8_000, &mut wrong_direction),
        LinkProcessOutcome::Bypassed
    );
    let mut wrong_rate = [2];
    assert_eq!(
        link.process_s16(LinkDirection::Read, 48_000, &mut wrong_rate),
        LinkProcessOutcome::Bypassed
    );
    assert_eq!(
        link.process_s16(LinkDirection::Read, 8_000, &mut []),
        LinkProcessOutcome::Bypassed
    );
    let mut oversized = [3; 3];
    assert_eq!(
        link.process_s16(LinkDirection::Read, 8_000, &mut oversized),
        LinkProcessOutcome::Bypassed
    );
    assert_eq!(wrong_direction, [1]);
    assert_eq!(wrong_rate, [2]);
    assert_eq!(oversized, [3; 3]);
    assert_eq!(link.observe().bypassed_blocks, 4);
}

#[test]
fn graph_failure_retains_original_pcm() {
    let mut link = PreparedLink::new(48_000, 2, graph(0.0, false)).unwrap();
    let mut pcm = [123, -456];
    assert_eq!(
        link.process_s16(LinkDirection::Read, 48_000, &mut pcm),
        LinkProcessOutcome::GraphFailed
    );
    assert_eq!(pcm, [123, -456]);
    assert_eq!(link.observe().failed_blocks, 1);
}

#[test]
fn native_float_frames_preserve_precision_without_integer_conversion() {
    let mut link = PreparedLink::new(48_000, 4, graph(2.0, true)).unwrap();
    let mut pcm = [-0.5, -0.123_456_79, 0.123_456_79, 0.5];
    assert_eq!(
        link.process_f32(LinkDirection::Read, 48_000, &mut pcm),
        LinkProcessOutcome::Processed
    );
    assert_eq!(pcm, [-1.0, -0.246_913_58, 0.246_913_58, 1.0]);
    assert_eq!(
        counters(link.observe()),
        LinkObservation {
            processed_blocks: 1,
            bypassed_blocks: 0,
            failed_blocks: 0,
            ..LinkObservation::default()
        }
    );
}

#[test]
fn native_float_rejections_and_graph_failure_retain_original_pcm() {
    let mut link = PreparedLink::new(48_000, 2, graph(0.0, false)).unwrap();
    for (direction, rate, mut pcm) in [
        (LinkDirection::Write, 48_000, vec![0.25]),
        (LinkDirection::Read, 8_000, vec![0.5]),
        (LinkDirection::Read, 48_000, vec![]),
        (LinkDirection::Read, 48_000, vec![0.75; 3]),
    ] {
        let original = pcm.clone();
        assert_eq!(
            link.process_f32(direction, rate, &mut pcm),
            LinkProcessOutcome::Bypassed
        );
        assert_eq!(pcm, original);
    }
    let mut pcm = [-0.123_456_79, 0.987_654_3];
    let original = pcm;
    assert_eq!(
        link.process_f32(LinkDirection::Read, 48_000, &mut pcm),
        LinkProcessOutcome::GraphFailed
    );
    assert_eq!(pcm, original);
    assert_eq!(
        counters(link.observe()),
        LinkObservation {
            processed_blocks: 0,
            bypassed_blocks: 4,
            failed_blocks: 1,
            ..LinkObservation::default()
        }
    );
}

#[test]
fn output_meter_tracks_returned_audio_idle_and_failure_without_resetting_history() {
    use usbradioplus_core::OutputLoudnessMeter;
    for rate in [8_000, 48_000] {
        let frames = (rate / 50) as usize;
        let mut link = PreparedLink::new(rate, frames, graph(0.5, true)).unwrap();
        let observer = link.observer();
        let epoch = observer.observe().loudness.epoch;
        assert_ne!(epoch, 0);
        assert_eq!(observer.observe().loudness.sample_count, 0);
        let mut reference = OutputLoudnessMeter::new(rate, frames).unwrap();
        for _ in 0..50 {
            let mut pcm: Vec<i16> = (0..frames)
                .map(|i| [0, 5657, 8000, 5657, 0, -5657, -8000, -5657][i % 8])
                .collect();
            assert_eq!(
                link.process_s16(LinkDirection::Read, rate, &mut pcm),
                LinkProcessOutcome::Processed
            );
            let output: Vec<f32> = pcm.into_iter().map(s16_to_f32).collect();
            reference.process(&output).unwrap();
        }
        for _ in 0..150 {
            assert_eq!(link.advance_silence(frames), LinkProcessOutcome::Processed);
            assert!(
                link.input
                    .iter()
                    .chain(link.output.iter())
                    .all(|v| *v == 0.0)
            );
            reference.process(&vec![0.0; frames]).unwrap();
        }
        let observed = observer.observe().loudness;
        let expected = reference.observer().snapshot();
        assert_eq!(observed.epoch, epoch);
        assert_eq!(observed.sample_count, u64::from(rate) * 4);
        assert_eq!(observed.integrated_lufs, expected.integrated_lufs);
        assert_eq!(observed.true_peak_dbtp, expected.true_peak_dbtp);
        for count in [0, frames + 1] {
            assert_eq!(link.advance_silence(count), LinkProcessOutcome::Bypassed);
            assert_eq!(observer.observe().loudness, observed);
        }
        let mut failed = PreparedLink::new(rate, frames, graph(2.0, false)).unwrap();
        let mut original = vec![1234; frames];
        assert_eq!(
            failed.process_s16(LinkDirection::Read, rate, &mut original),
            LinkProcessOutcome::GraphFailed
        );
        assert_eq!(original, vec![1234; frames]);
        assert_eq!(
            failed.advance_silence(frames),
            LinkProcessOutcome::GraphFailed
        );
        assert_eq!(failed.observe().loudness.sample_count, 2 * frames as u64);
        assert!(
            failed
                .input
                .iter()
                .chain(failed.output.iter())
                .all(|v| *v == 0.0)
        );
    }
}
