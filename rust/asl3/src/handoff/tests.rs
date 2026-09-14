use super::*;
use std::sync::Barrier;

#[test]
fn invalid_slot_count_is_rejected() {
    assert!(matches!(
        LatestHandoff::<u32>::split(0),
        Err(HandoffError::InvalidSlotCount)
    ));
    assert!(matches!(
        LatestHandoff::<u32>::split(1),
        Err(HandoffError::InvalidSlotCount)
    ));
    assert!(matches!(
        LatestHandoff::<u32>::split(INDEX_MASK + 1),
        Err(HandoffError::InvalidSlotCount)
    ));
    assert!(!HandoffError::InvalidSlotCount.to_string().is_empty());
}

#[test]
fn ordinary_values_retain_fifo_order() {
    let (mut producer, mut consumer) = LatestHandoff::<u32>::split(4).unwrap();
    assert_eq!(producer.push(10), PublishOutcome::Published);
    assert_eq!(producer.push(20), PublishOutcome::Published);
    assert_eq!(producer.observe().available, 2);
    assert_eq!(consumer.pop(), Some(10));
    assert_eq!(consumer.pop(), Some(20));
    assert_eq!(consumer.pop(), None);
    assert_eq!(consumer.observe(), HandoffObservation::default());
}

#[test]
fn overflow_skips_to_the_newest_value() {
    let (mut producer, mut consumer) = LatestHandoff::<u32>::split(3).unwrap();
    assert_eq!(producer.push(1), PublishOutcome::Published);
    assert_eq!(producer.push(2), PublishOutcome::Published);
    assert_eq!(producer.push(3), PublishOutcome::ReplacedStale);
    assert_eq!(consumer.pop(), Some(3));
    assert_eq!(consumer.pop(), None);
    let observation = consumer.observe();
    assert_eq!(observation.available, 0);
    assert_eq!(observation.discarded, 2);
    assert_eq!(observation.resynchronization_generation, 2);
}

#[test]
fn producer_drops_when_full_slot_is_claimed() {
    let (mut producer, consumer) = LatestHandoff::<u32>::split(2).unwrap();
    assert_eq!(producer.push(1), PublishOutcome::Published);
    consumer
        .shared
        .consumer_state
        .store(READING, Ordering::Release);
    assert_eq!(producer.push(2), PublishOutcome::DroppedDuringRead);
    consumer.shared.consumer_state.store(0, Ordering::Release);
    assert_eq!(consumer.observe().discarded, 1);
    assert_eq!(consumer.observe().resynchronization_generation, 2);
}

#[test]
fn endpoints_transfer_across_threads() {
    let (mut producer, mut consumer) = LatestHandoff::<u32>::split(8).unwrap();
    let start = Arc::new(Barrier::new(2));
    let producer_start = Arc::clone(&start);
    let worker = std::thread::spawn(move || {
        producer_start.wait();
        for value in 0..100_000 {
            let _ = producer.push(value);
        }
        producer
    });
    start.wait();
    let mut last = None;
    while !worker.is_finished() {
        if let Some(value) = consumer.pop() {
            last = Some(value);
        }
    }
    let mut producer = worker.join().unwrap();
    while let Some(value) = consumer.pop() {
        last = Some(value);
    }
    assert!(last.is_some());
    assert_eq!(producer.push(100_000), PublishOutcome::Published);
    assert_eq!(consumer.pop(), Some(100_000));
}

#[test]
fn replacement_in_progress_is_non_blocking() {
    let (_, mut consumer) = LatestHandoff::<u32>::split(3).unwrap();
    consumer.shared.generation.store(1, Ordering::Release);
    assert_eq!(consumer.pop(), None);
}
