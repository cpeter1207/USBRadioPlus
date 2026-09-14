use super::*;

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
    assert_eq!(link.observe(), LinkObservation::default());
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
        link.observe(),
        LinkObservation {
            processed_blocks: 1,
            bypassed_blocks: 0,
            failed_blocks: 0,
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
