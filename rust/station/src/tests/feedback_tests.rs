use super::*;

#[test]
fn evidence_preserves_validity_age_and_stale_request_identity() {
    let state = FeedbackState::default();
    assert_eq!(state.request(), None);
    let request = RadioFeedbackRequest {
        generation: 7,
        sample_index: 960,
        request_id: 2,
        keyed: false,
    };
    state.publish_request(request);
    assert_eq!(state.request(), Some(request));
    for (serviced, observed, expected) in [
        (None, None, 0),
        (Some(false), None, FEEDBACK_SERVICE_VALID),
        (
            Some(true),
            Some(false),
            FEEDBACK_SERVICE_VALID | FEEDBACK_READBACK_VALID,
        ),
        (None, Some(true), FEEDBACK_READBACK_VALID),
    ] {
        state.physical(request, serviced, observed, true);
        let mut record = RadioFeedback {
            generation_id: 7,
            sample_index: 1920,
            request_id: 3,
            flags: FEEDBACK_REQUEST_VALID,
            ..Default::default()
        };
        state.merge(&mut record);
        assert_eq!(
            record.flags & (FEEDBACK_SERVICE_VALID | FEEDBACK_READBACK_VALID),
            expected
        );
        if expected != 0 {
            assert_eq!(record.acquisition_age_frames, 960);
            assert_eq!(record.acquired_sample_index, 960);
            assert_eq!(
                record.flags & FEEDBACK_ACQUISITION_VALID,
                FEEDBACK_ACQUISITION_VALID
            );
        }
        if serviced.is_some() {
            assert_eq!(
                (record.service_generation, record.service_request_id),
                (7, 2)
            );
            assert_ne!(record.service_request_id, record.request_id);
            assert_eq!(record.serviced_ptt, u32::from(serviced == Some(true)));
        }
        if observed.is_some() {
            assert_eq!(
                (record.readback_generation, record.readback_request_id),
                (7, 2)
            );
            assert_eq!(record.observed_ptt, u32::from(observed == Some(true)));
        }
    }
}

#[test]
fn replacement_future_and_in_progress_publications_remain_unknown() {
    let state = FeedbackState::default();
    state.physical(
        RadioFeedbackRequest {
            generation: 7,
            sample_index: 1000,
            request_id: 4,
            keyed: false,
        },
        Some(false),
        Some(false),
        true,
    );
    for (generation_id, sample_index, flags) in [
        (8, 2000, FEEDBACK_REQUEST_VALID),
        (7, 999, FEEDBACK_REQUEST_VALID),
        (7, 2000, 0),
    ] {
        let mut record = RadioFeedback {
            generation_id,
            sample_index,
            flags,
            ..Default::default()
        };
        let before = record;
        state.merge(&mut record);
        assert_eq!(record, before);
    }
    state.physical.sequence.store(1, Ordering::Release);
    let mut record = RadioFeedback {
        generation_id: 7,
        sample_index: 2000,
        flags: FEEDBACK_REQUEST_VALID,
        ..Default::default()
    };
    let before = record;
    state.merge(&mut record);
    assert_eq!(record, before);
    state.request.sequence.store(1, Ordering::Release);
    assert_eq!(state.request(), None);
}

#[test]
fn cached_service_has_unknown_age_and_long_lived_positions_do_not_wrap() {
    let state = FeedbackState::default();
    let request = RadioFeedbackRequest {
        generation: 9,
        sample_index: u64::MAX - 10,
        request_id: 0,
        keyed: false,
    };
    for acquired in [false, true] {
        state.physical(request, Some(false), None, acquired);
        let mut record = RadioFeedback {
            generation_id: 9,
            sample_index: u64::MAX - 1,
            request_id: 4,
            flags: FEEDBACK_REQUEST_VALID,
            ..Default::default()
        };
        state.merge(&mut record);
        assert_ne!(record.flags & FEEDBACK_SERVICE_VALID, 0);
        assert_eq!(record.service_request_id, 0);
        assert_eq!(record.flags & FEEDBACK_ACQUISITION_VALID != 0, acquired);
        assert_eq!(record.acquisition_age_frames, if acquired { 9 } else { 0 });
        assert_eq!(record.observed_ptt, 0);
        assert_eq!(record.flags & FEEDBACK_READBACK_VALID, 0);
    }
}
