//! Real shared-FFmpeg checks for the native RX and TX band-pass descriptions.

#![cfg(target_os = "linux")]

use std::ffi::{CString, c_void};
use std::time::Instant;

use usbradioplus_core::{ChainRole, GraphDescriptionFactory, PlFilter, ProcessingChain};
use usbradioplus_ffmpeg::{GraphProvider, PreparedGraph};

const RATE: f64 = 48_000.0;
const MAXIMUM: usize = 960;
const RESPONSE_FRAMES: usize = 65_536;
type ResponseCase<'a> = (f64, f64, bool, usize, &'a [f64], &'a [f64]);

#[link(name = "rptadv_ffmpeg_adapter")]
unsafe extern "C" {
    fn rptadv_ffmpeg_adapter_descriptor() -> *const c_void;
}

fn description(role: ChainRole, highpass: f64, lowpass: f64, enabled: bool) -> String {
    let factory = GraphDescriptionFactory::new("unused-in-this-test").unwrap();
    let mut chain = ProcessingChain::shipped(role);
    chain.enabled = false;
    chain.input_gain_db = 0.0;
    chain.output_gain_db = 0.0;
    if role == ChainRole::LocalReceive {
        chain.receive.bandpass_enabled = enabled;
        chain.receive.bandpass_highpass_hz = highpass;
        chain.receive.bandpass_lowpass_hz = lowpass;
        chain.receive.pl_filter = PlFilter::Disabled;
        factory.receive_filter(&chain).unwrap()
    } else {
        chain.transmit_tail.limiter_enabled = false;
        chain.transmit_tail.bandpass_enabled = enabled;
        chain.transmit_tail.bandpass_highpass_hz = highpass;
        chain.transmit_tail.bandpass_lowpass_hz = lowpass;
        factory.transmitter(&chain, false, 300.0).unwrap()
    }
}

fn prepared(description: &str) -> PreparedGraph {
    // SAFETY: the dynamically linked adapter exports an immutable descriptor
    // whose storage and function pointers remain loaded for this process.
    let provider =
        unsafe { GraphProvider::from_raw_descriptor(rptadv_ffmpeg_adapter_descriptor()).unwrap() };
    let mut graph = provider
        .prepare(&CString::new(description).unwrap(), MAXIMUM as u32)
        .unwrap_or_else(|error| panic!("prepare {description}: {error}"));
    // Match production preparation, including its retained silent history.
    graph
        .warm_up(&[0.0; MAXIMUM], &mut [0.0; MAXIMUM], 8)
        .unwrap();
    graph
}

fn render(description: &str, input: &[f32], counts: &[usize]) -> Vec<f32> {
    let mut graph = prepared(description);
    let mut output = vec![0.0; input.len()];
    let started = Instant::now();
    let mut offset = 0;
    for count in counts.iter().copied().cycle() {
        if offset == input.len() {
            break;
        }
        let end = (offset + count).min(input.len());
        graph
            .process_block(&input[offset..end], &mut output[offset..end])
            .unwrap_or_else(|error| panic!("frames={}, offset={offset}: {error}", end - offset));
        offset = end;
    }
    assert!(output.iter().all(|sample| sample.is_finite()));
    eprintln!(
        "FFmpeg band-pass: {} frames in {:?} ({:.3}s audio), counts={counts:?}",
        input.len(),
        started.elapsed(),
        input.len() as f64 / RATE,
    );
    output
}

fn magnitude(response: &[f32], frequency: f64) -> f64 {
    let step = std::f64::consts::TAU * frequency / RATE;
    let (real, imaginary) =
        response
            .iter()
            .enumerate()
            .fold((0.0, 0.0), |(real, imaginary), (index, sample)| {
                let (sine, cosine) = (step * index as f64).sin_cos();
                (
                    real + f64::from(*sample) * cosine,
                    imaginary - f64::from(*sample) * sine,
                )
            });
    real.hypot(imaginary)
}

#[test]
fn native_lowpasses_have_positive_linear_phase_and_sharp_edges() {
    // Literal centers include the graph's seven-sample partition guard plus
    // (length - 1) / 2 for the specified odd FIR lengths: 155 and 39.
    // They are independent of the production graph-description implementation.
    let cases: [ResponseCase<'_>; 4] = [
        (0.0, 0.0, true, 0, &[10.0, 1_000.0, 6_000.0], &[]),
        (20.0, 5_000.0, false, 0, &[10.0, 1_000.0, 6_000.0], &[]),
        (
            0.0,
            5_000.0,
            true,
            84,
            &[40.0, 1_000.0, 4_000.0],
            &[7_000.0, 9_000.0],
        ),
        (0.0, 20_000.0, true, 26, &[350.0, 15_000.0], &[23_000.0]),
    ];
    for role in [ChainRole::LocalReceive, ChainRole::VoiceTelemetry] {
        for (highpass, lowpass, enabled, center, passband, stopband) in cases {
            let graph = description(role, highpass, lowpass, enabled);
            let mut impulse = vec![0.0; RESPONSE_FRAMES];
            impulse[0] = 1.0;
            let response = render(&graph, &impulse, &[MAXIMUM]);
            let peak = response
                .iter()
                .enumerate()
                .max_by(|(_, left), (_, right)| left.abs().total_cmp(&right.abs()))
                .unwrap()
                .0;
            assert_eq!(peak, center, "wrong group delay: {graph}");
            assert!(response[center] > 0.0, "inverted impulse: {graph}");
            for offset in 0..=center {
                assert!(
                    (response[center - offset] - response[center + offset]).abs() < 2.0e-5,
                    "asymmetric impulse at {offset}: {graph}"
                );
            }
            assert!(
                response[2 * center + 1..]
                    .iter()
                    .all(|sample| sample.abs() < 2.0e-5),
                "unexpected impulse tail: {graph}"
            );
            for frequency in passband {
                let gain = magnitude(&response, *frequency);
                assert!(
                    (gain - 1.0).abs() < 0.002,
                    "passband {frequency}Hz gain={gain}: {graph}"
                );
            }
            for frequency in stopband {
                let gain = magnitude(&response, *frequency);
                assert!(
                    gain <= 10.0_f64.powf(-50.0 / 20.0),
                    "stopband {frequency}Hz gain={gain}: {graph}"
                );
            }
        }
    }
}

#[test]
fn native_highpasses_retain_twentieth_order_iir_before_fir_lowpass() {
    let mut impulse = vec![0.0; RESPONSE_FRAMES];
    impulse[0] = 1.0;
    for highpass in [20.0, 300.0] {
        // Independent reference: retain the original 20th-order crossover,
        // including its polarity and nonlinear phase, without a new HP design.
        let reference_graph =
            format!("[in]acrossover=split={highpass}:order=20th[lo][out];[lo]anullsink;");
        let highpass_only = render(&reference_graph, &impulse, &[MAXIMUM]);
        for role in [ChainRole::LocalReceive, ChainRole::VoiceTelemetry] {
            for lowpass in [0.0, 5_000.0] {
                let graph = description(role, highpass, lowpass, true);
                let actual = render(&graph, &impulse, &[MAXIMUM]);
                let expected = if lowpass == 0.0 {
                    highpass_only.clone()
                } else {
                    // The low-pass is independently checked above. Cascading it
                    // with the original HP reference detects a changed HP stage.
                    render(
                        &description(role, 0.0, lowpass, true),
                        &highpass_only,
                        &[MAXIMUM],
                    )
                };
                for (index, (expected, actual)) in expected.iter().zip(&actual).enumerate() {
                    assert!(
                        (expected - actual).abs() < 2.0e-5,
                        "original HP cascade mismatch at {index}: {expected} != {actual}; {graph}"
                    );
                }
                let peak = actual
                    .iter()
                    .enumerate()
                    .max_by(|(_, left), (_, right)| left.abs().total_cmp(&right.abs()))
                    .unwrap()
                    .0;
                if lowpass == 0.0 {
                    assert_eq!(peak, 0, "HP-only must not acquire FIR delay: {graph}");
                } else if highpass == 20.0 {
                    assert!(peak <= 650, "20-5000Hz impulse delayed to {peak}: {graph}");
                    assert!(actual[peak] > 0.0, "inverted band-pass impulse: {graph}");
                    for frequency in [10.0, 16.0, 20.0, 24.0, 4_800.0, 5_000.0, 5_200.0] {
                        let gain = magnitude(&actual, frequency);
                        eprintln!(
                            "{role:?} 20-5000Hz: {frequency}Hz gain={gain:.9} ({:.3}dB), peak={peak}",
                            20.0 * gain.log10()
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn native_bandpasses_preserve_audio_across_variable_callback_sizes() {
    let input: Vec<f32> = (0..RESPONSE_FRAMES)
        .map(|index| {
            let phase = std::f64::consts::TAU * index as f64 / RATE;
            (0.3 * (997.0 * phase).sin() + 0.1 * (41.0 * phase).sin()) as f32
        })
        .collect();
    for role in [ChainRole::LocalReceive, ChainRole::VoiceTelemetry] {
        let graph = description(role, 20.0, 5_000.0, true);
        let fixed = render(&graph, &input, &[MAXIMUM]);
        // Consecutive one-frame callbacks also exercise the adapter's eight-frame
        // source pool; merely interspersing tiny frames with 960 could hide stalls.
        let variable = render(
            &graph,
            &input,
            &[1, 1, 1, 1, 1, 1, 1, 1, 1, 17, 31, 64, 960],
        );
        for (index, (expected, actual)) in fixed.iter().zip(variable).enumerate() {
            assert!(
                (expected - actual).abs() < 2.0e-5,
                "partition mismatch at {index}: {expected} != {actual}"
            );
        }
    }
}
