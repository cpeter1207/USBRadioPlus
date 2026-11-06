use super::*;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static AUDIO_ACTIVE: Cell<bool> = const { Cell::new(false) };
    static AUDIO_ALLOCS: Cell<usize> = const { Cell::new(0) };
}

struct CheckedAllocator;

// SAFETY: This delegates each allocator contract unchanged to System; thread-local
// counters observe allocations without allocating or accessing another thread's state.
unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        AUDIO_ACTIVE.with(|active| {
            if active.get() {
                AUDIO_ALLOCS.with(|count| count.set(count.get() + 1));
            }
        });
        // SAFETY: Forward the allocator's original valid layout.
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: Forward the matching pointer and layout to the backing allocator.
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

fn generate_sine_wave(frequency: f64, amplitude: f32, count: usize) -> Vec<f32> {
    (0..count)
        .map(|index| {
            (f64::from(amplitude)
                * (std::f64::consts::TAU * frequency * index as f64 / 48000.0).sin())
                as f32
        })
        .collect()
}
#[test]
fn integrated_loudness_survives_gated_quiet_and_silence() {
    let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    let sine_wave = generate_sine_wave(1000.0, 0.1, 960);
    let mut blocks = vec![sine_wave; 200];

    for block in blocks.iter_mut() {
        meter.process(block).unwrap();
    }

    let first_snapshot = meter.observer().snapshot();
    assert!(first_snapshot.integrated_lufs.is_some());
    assert!((first_snapshot.integrated_lufs.unwrap() + 23.0).abs() < 0.2);

    for amplitude in [0.001f32, 0.0].iter() {
        let quiet_wave = generate_sine_wave(1000.0, *amplitude, 960);
        blocks = vec![quiet_wave; 200];

        for block in blocks.iter_mut() {
            meter.process(block).unwrap();
        }

        let snapshot = meter.observer().snapshot();
        assert!(snapshot.integrated_lufs.is_some());
        assert!(
            (snapshot.integrated_lufs.unwrap() - first_snapshot.integrated_lufs.unwrap()).abs()
                < 0.3
        );
    }

    let final_snapshot = meter.observer().snapshot();
    assert!(final_snapshot.sample_count == 576000);
}
#[test]
fn validates_setup_and_rejects_oversize_without_changing_epoch() {
    for rate in [0, 7999, 384001] {
        let result = OutputLoudnessMeter::new(rate, 960);
        assert!(result.is_err());
        assert!(!result.err().unwrap().to_string().is_empty());
    }
    for maximum in [0, 4097] {
        let result = OutputLoudnessMeter::new(48000, maximum);
        assert!(result.is_err());
        assert!(!result.err().unwrap().to_string().is_empty());
    }
    for rate in [8000, 48000, 384000] {
        assert!(OutputLoudnessMeter::new(rate, 1).is_ok());
    }
    let mut meter = OutputLoudnessMeter::new(48000, 8).unwrap();
    let observer = meter.observer();
    let before = observer.snapshot();
    meter.process(&[]).unwrap();
    assert_eq!(observer.snapshot(), before);
    let error = meter.process(&[0.0; 9]).unwrap_err();
    assert!(!error.to_string().is_empty());
    assert_eq!(observer.snapshot(), before);
}

#[test]
fn requires_four_hundred_ms_and_preserves_state_on_silence() {
    let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    let observer = meter.observer();
    let initial = observer.snapshot();
    assert!(initial.epoch > 0);
    assert_eq!(initial.sample_count, 0);
    assert_eq!(initial.integrated_lufs, None);
    assert_eq!(initial.true_peak_dbtp, None);
    let tone = generate_sine_wave(1000.0, 0.1, 960);
    for _ in 0..19 {
        meter.process(&tone).unwrap();
    }
    assert_eq!(observer.snapshot().integrated_lufs, None);
    assert_eq!(observer.snapshot().true_peak_dbtp, None);
    meter.process(&tone).unwrap();
    let measured = observer.snapshot();
    assert_eq!(measured.sample_count, 19200);
    assert!((measured.integrated_lufs.unwrap() + 23.0).abs() < 0.2);
    assert!((measured.true_peak_dbtp.unwrap() + 20.0).abs() < 0.2);
}

// EBU Tech 3341 v4 (2023), Table 1 cases 15-19: https://tech.ebu.ch/docs/tech/tech3341.pdf
#[test]
fn true_peak_matches_ebu_tech_3341_table_one_phase_vectors() {
    for (frequency, phase, amplitude, expected) in [
        (12000.0, 0.0, 0.5, -6.0),
        (12000.0, std::f64::consts::FRAC_PI_4, 0.5, -6.0),
        (8000.0, std::f64::consts::PI / 3.0, 0.5, -6.0),
        (6000.0, 3.0 * std::f64::consts::PI / 8.0, 0.5, -6.0),
        (12000.0, std::f64::consts::FRAC_PI_4, 1.41, 3.0),
    ] {
        let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
        let signal: Vec<f32> = (0..48000)
            .map(|index| {
                let fade = index.min(47999 - index).min(480) as f64 / 480.0;
                (amplitude
                    * fade
                    * (std::f64::consts::TAU * frequency * index as f64 / 48000.0 + phase).sin())
                    as f32
            })
            .collect();
        for block in signal.chunks(960) {
            meter.process(block).unwrap();
        }
        let peak = meter.observer().snapshot().true_peak_dbtp.unwrap();
        assert!(
            (expected - 0.4..=expected + 0.2).contains(&peak),
            "{frequency} Hz: {peak} dBTP"
        );
    }
}

#[test]
fn histogram_and_peak_do_not_depend_on_input_block_boundaries() {
    let signal = generate_sine_wave(4000.0, 0.1, 192000);
    let mut reference: Option<(f64, f64)> = None;
    for count in [1, 7, 113, 960, 4096] {
        let mut meter = OutputLoudnessMeter::new(48000, count).unwrap();
        for block in signal.chunks(count) {
            meter.process(block).unwrap();
        }
        let snapshot = meter.observer().snapshot();
        assert_eq!(snapshot.sample_count, 192000);
        assert!((snapshot.integrated_lufs.unwrap() + 19.7).abs() < 0.2);
        if let Some((level, peak)) = reference {
            assert!((snapshot.integrated_lufs.unwrap() - level).abs() < 1e-10);
            assert!((snapshot.true_peak_dbtp.unwrap() - peak).abs() < 1e-6);
        } else {
            reference = Some((
                snapshot.integrated_lufs.unwrap(),
                snapshot.true_peak_dbtp.unwrap(),
            ));
        }
    }
}

#[test]
fn callback_observation_and_reset_allocate_nothing() {
    let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    let observer = meter.observer();
    let tone = generate_sine_wave(1000.0, 0.1, 960);
    AUDIO_ALLOCS.with(|count| count.set(0));
    AUDIO_ACTIVE.with(|active| active.set(true));
    for _ in 0..200 {
        meter.process(&tone).unwrap();
    }
    let before = observer.snapshot();
    meter.reset();
    let reset = observer.snapshot();
    for _ in 0..20 {
        meter.process(&[0.0; 960]).unwrap();
    }
    let silence = observer.snapshot();
    AUDIO_ACTIVE.with(|active| active.set(false));
    AUDIO_ALLOCS.with(|count| assert_eq!(count.get(), 0));
    assert!(before.integrated_lufs.is_some());
    assert_ne!(reset.epoch, before.epoch);
    assert_eq!(reset.sample_count, 0);
    assert_eq!(reset.integrated_lufs, None);
    assert_eq!(reset.true_peak_dbtp, None);
    assert_eq!(silence.epoch, reset.epoch);
    assert_eq!(silence.sample_count, 19200);
    assert_eq!(silence.integrated_lufs, None);
    assert_eq!(silence.true_peak_dbtp, None);
    drop(meter);
    assert_eq!(observer.snapshot(), silence);
}

#[test]
fn nonfinite_input_is_silence_and_cannot_poison_later_program() {
    let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    let mut invalid = [0.0; 960];
    for frame in invalid.chunks_mut(4) {
        frame.copy_from_slice(&[f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 0.0]);
    }
    for _ in 0..200 {
        meter.process(&invalid).unwrap();
    }
    assert_eq!(meter.observer().snapshot().integrated_lufs, None);
    let tone = generate_sine_wave(1000.0, 0.1, 960);
    for _ in 0..200 {
        meter.process(&tone).unwrap();
    }
    assert!((meter.observer().snapshot().integrated_lufs.unwrap() + 23.0).abs() < 0.3);
}
#[test]
fn observer_is_bounded_when_busy_and_coherent_during_publication() {
    let meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    let observer = meter.observer();
    meter.published.publish(OutputLoudnessSnapshot {
        epoch: 19200,
        sample_count: 19200,
        integrated_lufs: Some(-23.0),
        true_peak_dbtp: Some(-20.0),
    });
    meter.published.sequence.fetch_add(1, Ordering::SeqCst);
    let busy = observer.snapshot();
    assert_eq!(busy.sample_count, 0);
    assert_eq!(busy.integrated_lufs, None);
    assert_eq!(busy.true_peak_dbtp, None);
    meter.published.sequence.fetch_add(1, Ordering::SeqCst);
    let publication = Arc::clone(&meter.published);
    let writer = std::thread::spawn(move || {
        for count in 1..=50000 {
            publication.publish(OutputLoudnessSnapshot {
                epoch: count,
                sample_count: count,
                integrated_lufs: Some(-23.0),
                true_peak_dbtp: Some(-20.0),
            });
        }
    });
    for _ in 0..100000 {
        let snapshot = observer.snapshot();
        if snapshot.sample_count != 0 {
            assert_eq!(snapshot.epoch, snapshot.sample_count);
            assert_eq!(snapshot.integrated_lufs, Some(-23.0));
            assert_eq!(snapshot.true_peak_dbtp, Some(-20.0));
        }
    }
    writer.join().unwrap();
    assert_eq!(observer.snapshot().sample_count, 50000);
}

#[test]
fn extreme_finite_input_never_publishes_nonfinite_metric_values() {
    let mut meter = OutputLoudnessMeter::new(48000, 960).unwrap();
    for _ in 0..20 {
        meter.process(&[f32::MAX; 960]).unwrap();
    }
    let snapshot = meter.observer().snapshot();
    assert_eq!(snapshot.sample_count, 19200);
    assert!(snapshot.integrated_lufs.is_none_or(f64::is_finite));
    assert!(snapshot.true_peak_dbtp.is_none_or(f64::is_finite));
}
