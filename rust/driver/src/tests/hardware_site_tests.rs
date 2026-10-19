use super::*;

fn bank() -> SiteBankState {
    SiteBankState::new(0x02, 0x21, 0x01, 0x01)
}

#[test]
fn site_binding_rejects_levels_outside_owned_pins_and_retired_generations() {
    let mut bank = bank();
    let mut output = 1;
    for (initial, safe) in [(0x40, 0), (0, 0x40)] {
        assert_eq!(
            bank.bind(
                SiteDigitalBinding {
                    generation: bank.snapshot().generation,
                    output_mask: 0x20,
                    initial_high_mask: initial,
                    safe_high_mask: safe,
                },
                &mut output
            ),
            Err(SiteError::InvalidMask)
        );
        assert_eq!(output, 1);
        assert_eq!(bank.snapshot().claimed_mask, 0);
    }
    bank.retire(&mut output);
    assert_eq!(
        bank.bind(
            SiteDigitalBinding {
                generation: 0,
                output_mask: 0,
                initial_high_mask: 0,
                safe_high_mask: 0,
            },
            &mut output
        ),
        Err(SiteError::StaleGeneration)
    );
    assert_eq!(output, 1);
}

#[test]
fn site_cache_preserves_each_rejection_and_acquisition_quality() {
    let cache = SiteCache::default();
    let mut bank = bank();
    for error in [
        SiteError::Unavailable,
        SiteError::StaleGeneration,
        SiteError::InvalidMask,
        SiteError::Busy,
        SiteError::Fault,
    ] {
        bank.reject(error);
        for quality in [
            SiteReadQuality::Unavailable,
            SiteReadQuality::Valid,
            SiteReadQuality::Fault,
        ] {
            let mut snapshot = bank.snapshot();
            snapshot.input_quality = quality;
            snapshot.snapshot_at_ms = 30;
            cache.publish(snapshot, 30);
            assert_eq!(cache.read(), Ok(snapshot));
        }
    }
    assert_eq!(cache.read().unwrap().rejected_commands, 5);
}

#[test]
fn site_cache_concurrent_publication_never_returns_a_torn_observation() {
    use std::sync::{Barrier, atomic::AtomicBool};
    let cache = SiteCache::default();
    let ready = Barrier::new(2);
    let done = AtomicBool::new(false);
    std::thread::scope(|scope| {
        scope.spawn(|| {
            ready.wait();
            let mut generation = 1;
            while !done.load(Ordering::Acquire) {
                cache.publish(
                    SiteDigitalSnapshot {
                        generation,
                        observed_at_ms: Some(generation),
                        submitted_generation: generation,
                        serviced_generation: generation,
                        ..SiteDigitalSnapshot::default()
                    },
                    generation,
                );
                generation += 1;
            }
        });
        ready.wait();
        let coherent = (0..100_000).all(|_| match cache.read() {
            Ok(snapshot) => {
                snapshot.generation == snapshot.submitted_generation
                    && snapshot.generation == snapshot.serviced_generation
                    && snapshot.generation == snapshot.snapshot_at_ms
                    && snapshot.observed_at_ms.unwrap_or(0) == snapshot.generation
            }
            Err(error) => error == SiteError::Busy,
        });
        done.store(true, Ordering::Release);
        assert!(
            coherent,
            "a cached read must retry rather than expose a torn snapshot"
        );
    });
    assert!(
        cache.read().is_ok(),
        "a joined writer must leave a readable cache"
    );
}

#[test]
fn radio_reservation_supersedes_a_released_sites_safe_high_level() {
    let mut bank = bank();
    let mut output = 1;
    bank.bind(
        SiteDigitalBinding {
            generation: bank.snapshot().generation,
            output_mask: 0x20,
            initial_high_mask: 0,
            safe_high_mask: 0x20,
        },
        &mut output,
    )
    .unwrap();
    bank.bind(
        SiteDigitalBinding {
            generation: bank.snapshot().generation,
            output_mask: 0,
            initial_high_mask: 0,
            safe_high_mask: 0,
        },
        &mut output,
    )
    .unwrap();
    assert_eq!(output, 0x21);
    assert!(bank.reserve(0x20));
    output = 1; // The new radio owner deasserts its signaling output.
    bank.retire(&mut output);
    assert_eq!(
        output, 1,
        "retired site policy must not reassert radio signaling"
    );
}

#[test]
fn site_owner_masks_and_generation_reject_before_mutation() {
    let mut bank = bank();
    let original = bank.snapshot().generation;
    let mut output = 0x01;
    for mask in [0x01, 0x04, 0x40] {
        assert!(
            bank.bind(
                SiteDigitalBinding {
                    generation: original,
                    output_mask: mask,
                    initial_high_mask: mask,
                    safe_high_mask: 0,
                },
                &mut output
            )
            .is_err()
        );
        assert_eq!(output, 0x01);
    }
    bank.bind(
        SiteDigitalBinding {
            generation: original,
            output_mask: 0x20,
            initial_high_mask: 0x20,
            safe_high_mask: 0,
        },
        &mut output,
    )
    .unwrap();
    assert_eq!(output, 0x21);
    let current = bank.snapshot().generation;
    assert_ne!(current, original);
    for (generation, output_mask, high_mask) in [
        (original, 0x20, 0),
        (current, 0x21, 0),
        (current, 0x20, 0x40),
    ] {
        assert!(
            bank.apply(
                SiteDigitalRequest {
                    generation,
                    output_mask,
                    high_mask
                },
                &mut output
            )
            .is_err()
        );
        assert_eq!(output, 0x21);
    }
    bank.apply(
        SiteDigitalRequest {
            generation: current,
            output_mask: 0x20,
            high_mask: 0,
        },
        &mut output,
    )
    .unwrap();
    assert_eq!(output, 0x01);
    assert!(!bank.permits_ordinary(0x20));
    assert!(
        bank.permits_ordinary(0x01),
        "existing indicator control remains available"
    );
}

#[test]
fn site_owner_reload_preserves_outputs_and_releases_to_prior_safe_state() {
    let mut bank = bank();
    let mut output = 0x01;
    let binding = SiteDigitalBinding {
        generation: bank.snapshot().generation,
        output_mask: 0x20,
        initial_high_mask: 0x20,
        safe_high_mask: 0,
    };
    bank.bind(binding, &mut output).unwrap();
    let previous = bank.snapshot().generation;
    bank.bind(
        SiteDigitalBinding {
            generation: previous,
            initial_high_mask: 0,
            ..binding
        },
        &mut output,
    )
    .unwrap();
    assert_eq!(
        output, 0x21,
        "unchanged ownership must not reapply startup state"
    );
    bank.bind(
        SiteDigitalBinding {
            generation: bank.snapshot().generation,
            output_mask: 0,
            initial_high_mask: 0,
            safe_high_mask: 0,
        },
        &mut output,
    )
    .unwrap();
    assert_eq!(output, 0x01, "removed bits return to their old safe state");
    assert!(bank.permits_ordinary(0x20));
    assert!(
        bank.apply(
            SiteDigitalRequest {
                generation: previous,
                output_mask: 0x20,
                high_mask: 0x20
            },
            &mut output
        )
        .is_err()
    );
    assert_eq!(output, 0x01);
}

#[test]
fn site_owner_read_failure_is_not_fresh_and_no_fabricated_readback() {
    let mut bank = bank();
    assert_eq!(bank.snapshot().input_quality, SiteReadQuality::Unavailable);
    bank.observe(Some(0xff), 10);
    assert_eq!(bank.snapshot().input_high_mask, 0x02);
    bank.observe(None, 20);
    let failed = bank.snapshot();
    assert_eq!(failed.observed_at_ms, Some(10));
    assert_eq!(failed.input_high_mask, 0x02);
    assert_eq!(failed.input_quality, SiteReadQuality::Fault);
    bank.submitted(0x21);
    let submitted = bank.snapshot();
    assert_eq!(submitted.submitted_high_mask, 0x21);
    assert_eq!(submitted.readback_valid_mask, 0);
    bank.observe(Some(0), 30);
    assert_eq!(bank.snapshot().observed_at_ms, Some(30));
    assert_eq!(bank.snapshot().input_quality, SiteReadQuality::Valid);
}

#[test]
fn site_owner_replacement_rejects_prior_owner_generation() {
    let old = bank();
    let mut replacement = bank();
    let mut output = 1;
    assert!(
        replacement
            .bind(
                SiteDigitalBinding {
                    generation: old.snapshot().generation,
                    output_mask: 0x20,
                    initial_high_mask: 0x20,
                    safe_high_mask: 0,
                },
                &mut output
            )
            .is_err()
    );
    assert_eq!(output, 1);
}

#[test]
fn site_owner_cached_snapshot_retains_acquisition_time_and_request_failure() {
    let cache = SiteCache::default();
    let mut bank = bank();
    bank.observe(Some(2), 10);
    bank.reject(SiteError::StaleGeneration);
    cache.publish(bank.snapshot(), 25);
    let snapshot = cache.read().unwrap();
    assert_eq!(snapshot.observed_at_ms, Some(10));
    assert_eq!(snapshot.snapshot_at_ms, 25);
    assert_eq!(snapshot.rejected_commands, 1);
    assert_eq!(
        snapshot.last_command_error,
        Some(SiteError::StaleGeneration)
    );
    bank.observe(None, 30);
    cache.publish(bank.snapshot(), 40);
    let failed = cache.read().unwrap();
    assert_eq!(failed.observed_at_ms, Some(10));
    assert_eq!(failed.snapshot_at_ms, 40);
    assert_eq!(failed.input_quality, SiteReadQuality::Fault);
}
