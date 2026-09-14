//! Bounded newest-audio handoff from a real-time producer to Asterisk.

use std::cell::UnsafeCell;
use std::fmt;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

const READING: usize = 1_usize << (usize::BITS - 1);
const INDEX_MASK: usize = !READING;

/// Invalid static handoff construction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HandoffError {
    /// At least two slots are required and indexes must leave one claim bit.
    InvalidSlotCount,
}

impl fmt::Display for HandoffError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("invalid callback handoff slot count")
    }
}

impl std::error::Error for HandoffError {}

/// Result of one non-waiting real-time publication.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PublishOutcome {
    /// The new value was retained without evicting an older value.
    Published,
    /// Stale idle values were evicted and the newest value was retained.
    ReplacedStale,
    /// The consumer changed the only reusable slot, so the new value was dropped.
    DroppedDuringRead,
}

/// Individually current handoff statistics.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct HandoffObservation {
    /// Values presently awaiting delivery, including a claimed value.
    pub available: usize,
    /// Values discarded to bound latency.
    pub discarded: u64,
    /// Even stable or odd replacement-in-progress generation.
    pub resynchronization_generation: usize,
}

struct Shared<T> {
    slots: Box<[UnsafeCell<MaybeUninit<T>>]>,
    producer: AtomicUsize,
    consumer_state: AtomicUsize,
    generation: AtomicUsize,
    discarded: AtomicU64,
}

// SAFETY: a split creates exactly one producer and one consumer. Atomic cursor
// ownership prevents concurrent access to one slot, and T is copied only after
// acquire publication.
unsafe impl<T: Copy + Send> Sync for Shared<T> {}

/// Constructor for one bounded single-producer/single-consumer handoff.
pub struct LatestHandoff<T>(PhantomData<T>);

impl<T: Copy + Send> LatestHandoff<T> {
    /// Allocate fixed slots on the control plane and split their endpoint ownership.
    pub fn split(slot_count: usize) -> Result<(Producer<T>, Consumer<T>), HandoffError> {
        if !(2..=INDEX_MASK).contains(&slot_count) {
            return Err(HandoffError::InvalidSlotCount);
        }
        let shared = Arc::new(Shared {
            slots: (0..slot_count)
                .map(|_| UnsafeCell::new(MaybeUninit::uninit()))
                .collect(),
            producer: AtomicUsize::new(0),
            consumer_state: AtomicUsize::new(0),
            generation: AtomicUsize::new(0),
            discarded: AtomicU64::new(0),
        });
        Ok((
            Producer {
                shared: Arc::clone(&shared),
            },
            Consumer { shared },
        ))
    }
}

/// Sole real-time producer endpoint.
pub struct Producer<T> {
    shared: Arc<Shared<T>>,
}

impl<T: Copy + Send> Producer<T> {
    /// Publish one fully initialized value without waiting.
    ///
    /// When full, stale idle backlog is skipped so the newest audio wins. A
    /// value is dropped only when the consumer concurrently changes the sole
    /// slot that could be reused.
    pub fn push(&mut self, value: T) -> PublishOutcome {
        let slot_count = self.shared.slots.len();
        let producer = self.shared.producer.load(Ordering::Relaxed);
        let next = next_index(producer, slot_count);
        let consumer_state = self.shared.consumer_state.load(Ordering::Acquire);
        if next != consumer_state & INDEX_MASK {
            self.publish(producer, next, value);
            return PublishOutcome::Published;
        }

        self.shared.generation.fetch_add(1, Ordering::Release);
        let consumer_state = self.shared.consumer_state.load(Ordering::Acquire);
        let consumer = consumer_state & INDEX_MASK;
        if self
            .shared
            .consumer_state
            .compare_exchange(consumer, producer, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return self.finish_drop();
        }
        self.shared.discarded.fetch_add(
            distance(consumer, producer, slot_count) as u64,
            Ordering::Relaxed,
        );
        self.publish(producer, next, value);
        self.shared.generation.fetch_add(1, Ordering::Release);
        PublishOutcome::ReplacedStale
    }

    /// Return a best-effort current observation.
    pub fn observe(&self) -> HandoffObservation {
        observe(&self.shared)
    }

    fn publish(&self, slot: usize, next: usize, value: T) {
        // SAFETY: only the producer writes its unpublished cursor slot. It is
        // not made visible until the release store below.
        unsafe { (*self.shared.slots[slot].get()).write(value) };
        self.shared.producer.store(next, Ordering::Release);
    }

    fn finish_drop(&self) -> PublishOutcome {
        self.shared.discarded.fetch_add(1, Ordering::Relaxed);
        self.shared.generation.fetch_add(1, Ordering::Release);
        PublishOutcome::DroppedDuringRead
    }
}

/// Sole non-real-time Asterisk delivery endpoint.
pub struct Consumer<T> {
    shared: Arc<Shared<T>>,
}

impl<T: Copy + Send> Consumer<T> {
    /// Copy and release the next coherent value, skipping stale overflow backlog.
    pub fn pop(&mut self) -> Option<T> {
        loop {
            let generation = self.shared.generation.load(Ordering::Acquire);
            if generation & 1 != 0 {
                return None;
            }
            let consumer_state = self.shared.consumer_state.load(Ordering::Acquire);
            let consumer = consumer_state & INDEX_MASK;
            let producer = self.shared.producer.load(Ordering::Acquire);
            if consumer == producer {
                return None;
            }
            let claimed = self
                .shared
                .consumer_state
                .fetch_or(READING, Ordering::AcqRel)
                & INDEX_MASK;
            if self.shared.generation.load(Ordering::Acquire) != generation {
                self.shared.consumer_state.store(claimed, Ordering::Release);
                continue;
            }
            // SAFETY: the acquire publication proved this slot initialized and
            // the claim prevents the producer from overwriting it while copied.
            let value = unsafe { (*self.shared.slots[claimed].get()).assume_init_read() };
            self.release_claim(claimed);
            return Some(value);
        }
    }

    /// Return a best-effort current observation.
    pub fn observe(&self) -> HandoffObservation {
        observe(&self.shared)
    }

    fn release_claim(&self, slot: usize) {
        self.shared
            .consumer_state
            .store(next_index(slot, self.shared.slots.len()), Ordering::Release);
    }
}

fn observe<T>(shared: &Shared<T>) -> HandoffObservation {
    let producer = shared.producer.load(Ordering::Acquire);
    let consumer = shared.consumer_state.load(Ordering::Acquire) & INDEX_MASK;
    HandoffObservation {
        available: distance(consumer, producer, shared.slots.len()),
        discarded: shared.discarded.load(Ordering::Relaxed),
        resynchronization_generation: shared.generation.load(Ordering::Acquire),
    }
}

const fn next_index(index: usize, count: usize) -> usize {
    (index + 1) % count
}

const fn distance(first: usize, second: usize, count: usize) -> usize {
    (second + count - first) % count
}

#[cfg(test)]
mod tests;
