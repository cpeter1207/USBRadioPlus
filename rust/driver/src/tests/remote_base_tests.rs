use super::*;
use usbradioplus_core::remote_operation::{RemoteOperation, RemoteOperationKind as Kind};

#[test]
fn remote_activation_requires_initial_commit_and_cannot_revive_retired_owners() {
    let queue = RemoteQueue::new(1);
    assert!(!queue.ready());
    queue.mark_ready();
    assert!(queue.ready());
    queue.retire();
    queue.mark_ready();
    assert!(!queue.ready());
}

#[test]
fn legacy_parallel_work_keeps_generation_ownership_without_a_caller_ticket() {
    let queue = RemoteQueue::new(1);
    let request = ParallelRadioRequest::SelectChannel(7);
    queue.submit_parallel(request).unwrap();
    let work = queue.next_ordinary().unwrap();
    assert_eq!(work.parallel_request(), Some(request));
    assert!(work.operation().is_none());
    assert!(work.source().valid && !work.source().active);
    work.complete(Err(RemoteRequestError::Rejected(-123)));
    assert_eq!(queue.parallel_error(), -123);
    queue.submit_parallel(request).unwrap();
    queue
        .next_ordinary()
        .unwrap()
        .complete(Err(RemoteRequestError::Busy));
    assert_eq!(queue.parallel_error(), -123);
    queue.submit_parallel(request).unwrap();
    queue.retire();
    let work = queue.next_ordinary().unwrap();
    assert!(!work.source().valid);
    drop(work);
    assert_eq!(queue.parallel_error(), -123);
}

#[test]
fn remote_queue_has_explicit_backpressure_and_reserved_stop() {
    let queue = RemoteQueue::new(2);
    let mut invalid = RemoteOperation::new(Kind::Tune);
    invalid.operation = u32::MAX;
    assert!(matches!(
        queue.submit(invalid, true, false),
        Err(RemoteRequestError::Rejected(-1003))
    ));
    let request = RemoteOperation::new(Kind::Tune);
    let first = queue.submit(request, true, false).unwrap();
    let second = queue.submit(request, false, true).unwrap();
    assert!(matches!(
        queue.submit(request, true, false),
        Err(RemoteRequestError::Busy)
    ));
    let stop = queue
        .submit(RemoteOperation::new(Kind::StopScan), true, false)
        .unwrap();
    assert!(matches!(
        queue.submit(RemoteOperation::new(Kind::StopScan), true, false),
        Err(RemoteRequestError::Busy)
    ));
    let work = queue.next().unwrap();
    assert_eq!(work.operation().unwrap().kind(), Some(Kind::StopScan));
    work.complete(Ok(RemoteReply::Applied));
    assert!(matches!(
        stop.take_completion(),
        Some(Ok(RemoteReply::Applied))
    ));
    let work = queue.next().unwrap();
    assert!(work.source().local);
    work.complete(Ok(RemoteReply::Applied));
    assert!(matches!(
        first.take_completion(),
        Some(Ok(RemoteReply::Applied))
    ));
    let second_work = queue.next().unwrap();
    assert!(!second_work.source().local);
    assert!(second.take_completion().is_none());
    assert!(queue.next().is_none());
}

#[test]
fn remote_queue_source_revocation_and_retirement_are_terminal() {
    let queue = RemoteQueue::new(2);
    let request = RemoteOperation::new(Kind::Tune);
    let ticket = queue.submit(request, false, true).unwrap();
    ticket.source_state(false, true);
    assert!(!queue.next().unwrap().source().active);
    let stale = queue.submit(request, false, false).unwrap();
    stale.source_state(false, false);
    stale.source_state(true, true);
    let work = queue.next().unwrap();
    assert!(!work.source().valid);
    work.complete(Err(RemoteRequestError::StaleSource));
    assert!(matches!(
        stale.take_completion(),
        Some(Err(RemoteRequestError::StaleSource))
    ));
    let retired = queue.submit(request, true, false).unwrap();
    queue.retire();
    assert!(matches!(
        retired.take_completion(),
        Some(Err(RemoteRequestError::Stopped))
    ));
    assert!(retired.take_completion().is_none());
    assert!(matches!(
        queue.submit(request, true, false),
        Err(RemoteRequestError::Stopped)
    ));
    assert!(!queue.next().unwrap().source().valid);
}

#[test]
fn remote_queue_caller_drop_cancels_before_dequeue_and_other_rigs_stay_independent() {
    let blocked = RemoteQueue::new(1);
    let other = RemoteQueue::new(1);
    let request = RemoteOperation::new(Kind::Tune);
    let cancelled = blocked.submit(request, true, false).unwrap();
    drop(cancelled);
    assert!(!blocked.next().unwrap().source().valid);
    let inflight = blocked.submit(request, true, false).unwrap();
    let work = blocked.next().unwrap();
    let live = other.submit(request, true, false).unwrap();
    other.next().unwrap().complete(Ok(RemoteReply::Applied));
    assert!(matches!(
        live.take_completion(),
        Some(Ok(RemoteReply::Applied))
    ));
    assert!(inflight.take_completion().is_none());
    blocked.retire();
    work.complete(Ok(RemoteReply::Applied));
    assert!(matches!(
        inflight.take_completion(),
        Some(Err(RemoteRequestError::Stopped))
    ));
}

#[test]
fn remote_queue_revocation_during_io_cannot_report_success() {
    let queue = RemoteQueue::new(1);
    let ticket = queue
        .submit(RemoteOperation::new(Kind::Tune), false, false)
        .unwrap();
    let work = queue.next().unwrap();
    let lease = work.source_lease();
    ticket.source_state(false, false);
    assert!(!lease.source().valid);
    work.complete(Ok(RemoteReply::Applied));
    assert!(matches!(
        ticket.take_completion(),
        Some(Err(RemoteRequestError::StaleSource))
    ));
}
