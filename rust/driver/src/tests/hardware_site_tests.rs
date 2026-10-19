use super::*;

fn bank() -> SiteBankState {
    SiteBankState::new(0x02, 0x21, 0x01, 0x01)
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
