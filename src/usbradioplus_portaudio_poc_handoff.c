/**
 * @file
 * @brief USBRadioPlus PortAudio POC handoff.
 *
 * Lock-free newest-block recovery for the optional PortAudio handoff.
 */

#include "usbradioplus_portaudio_poc_handoff.h"

#include <limits.h>

/** @brief High consumer-state bit marking a slot that is being copied. */
#define URP_PORTAUDIO_POC_HANDOFF_READING (UINT_MAX - (UINT_MAX >> 1U))
/** @brief Consumer-state bits holding the ring index. */
#define URP_PORTAUDIO_POC_HANDOFF_INDEX_MASK (UINT_MAX >> 1U)

#ifdef URP_PORTAUDIO_POC_HANDOFF_TESTING
/** @brief Inject one deterministic competing-actor step before an atomic boundary. */
extern void usbradioplus_portaudio_poc_handoff_test_interleave(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int boundary);
/** @brief Test-only scheduling point, absent from the shipped callback. */
#define URP_HANDOFF_INTERLEAVE(boundary)                                                           \
	usbradioplus_portaudio_poc_handoff_test_interleave(handoff, boundary)
#else
/** @brief Production handoff scheduling remains entirely owned by its two actors. */
#define URP_HANDOFF_INTERLEAVE(boundary) ((void)0)
#endif

/**
 * @brief Return whether a ring count can coexist with the consumer claim bit.
 * @param slot_count Requested bounded slot count.
 * @return Nonzero when the count leaves one state bit available for a claim.
 */
static int portaudio_poc_handoff_slot_count_valid(unsigned int slot_count)
{
	return slot_count > 1U && slot_count <= URP_PORTAUDIO_POC_HANDOFF_INDEX_MASK;
}

/**
 * @brief Advance one bounded ring index.
 * @param index Existing ring index.
 * @param slot_count Number of bounded slots.
 * @return The next ring index.
 */
static unsigned int portaudio_poc_handoff_next(unsigned int index, unsigned int slot_count)
{
	return (index + 1U) % slot_count;
}

/**
 * @brief Return the preceding bounded ring index.
 * @param index Existing ring index.
 * @param slot_count Number of bounded slots.
 * @return The preceding ring index.
 */
static unsigned int portaudio_poc_handoff_previous(unsigned int index, unsigned int slot_count)
{
	return index ? index - 1U : slot_count - 1U;
}

/**
 * @brief Return pending slots skipped when moving a consumer cursor forward.
 * @param first Existing consumer index.
 * @param second Replacement consumer index.
 * @param slot_count Number of bounded slots.
 * @return Number of slots discarded before the replacement index.
 */
static unsigned int portaudio_poc_handoff_distance(unsigned int first, unsigned int second,
						   unsigned int slot_count)
{
	return (second + slot_count - first) % slot_count;
}

void usbradioplus_portaudio_poc_handoff_init(struct usbradioplus_portaudio_poc_handoff *handoff)
{
	if (!handoff)
		return;
	atomic_init(&handoff->producer, 0U);
	atomic_init(&handoff->consumer_state, 0U);
	atomic_init(&handoff->resync_generation, 0U);
	handoff->producer_resync_pending = 0U;
	atomic_init(&handoff->discarded, 0U);
}

void usbradioplus_portaudio_poc_handoff_reset(struct usbradioplus_portaudio_poc_handoff *handoff)
{
	if (!handoff)
		return;
	atomic_store_explicit(&handoff->producer, 0U, memory_order_relaxed);
	atomic_store_explicit(&handoff->consumer_state, 0U, memory_order_relaxed);
	atomic_store_explicit(&handoff->resync_generation, 0U, memory_order_relaxed);
	handoff->producer_resync_pending = 0U;
	atomic_store_explicit(&handoff->discarded, 0U, memory_order_relaxed);
}

enum usbradioplus_portaudio_poc_handoff_result usbradioplus_portaudio_poc_handoff_producer_reserve(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int *slot)
{
	unsigned int producer;
	int resync_notified = 0;

	if (!handoff || !slot || !portaudio_poc_handoff_slot_count_valid(slot_count))
		return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID;
	producer = atomic_load_explicit(&handoff->producer, memory_order_relaxed);
	for (;;) {
		unsigned int consumer_state =
			atomic_load_explicit(&handoff->consumer_state, memory_order_acquire);
		unsigned int consumer = consumer_state & URP_PORTAUDIO_POC_HANDOFF_INDEX_MASK;
		unsigned int next = portaudio_poc_handoff_next(producer, slot_count);

		if (next != consumer) {
			if (resync_notified)
				handoff->producer_resync_pending = 1U;
			*slot = producer;
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY;
		}
		/* Mark replacement in progress before cursor movement. A consumer that
		 * observes the odd generation waits until this callback has published its
		 * newest block, then skips stale latency in one coherent operation. */
		if (!resync_notified) {
			atomic_fetch_add_explicit(&handoff->resync_generation, 1U,
						  memory_order_release);
			resync_notified = 1;
		}
		if (consumer_state & URP_PORTAUDIO_POC_HANDOFF_READING) {
			/* A producer must never reuse a slot while the worker copies its PCM. */
			atomic_fetch_add_explicit(&handoff->discarded, 1U, memory_order_relaxed);
			atomic_fetch_add_explicit(&handoff->resync_generation, 1U,
						  memory_order_release);
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_DROPPED;
		}
		URP_HANDOFF_INTERLEAVE(1U);
		if (atomic_compare_exchange_weak_explicit(
			    &handoff->consumer_state, &consumer_state,
			    portaudio_poc_handoff_next(consumer, slot_count), memory_order_acq_rel,
			    memory_order_acquire)) {
			atomic_fetch_add_explicit(&handoff->discarded, 1U, memory_order_relaxed);
			handoff->producer_resync_pending = 1U;
			*slot = producer;
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY;
		}
	}
}

void usbradioplus_portaudio_poc_handoff_producer_publish(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count)
{
	unsigned int producer;

	if (!handoff || !portaudio_poc_handoff_slot_count_valid(slot_count))
		return;
	producer = atomic_load_explicit(&handoff->producer, memory_order_relaxed);
	atomic_store_explicit(&handoff->producer, portaudio_poc_handoff_next(producer, slot_count),
			      memory_order_release);
	if (handoff->producer_resync_pending) {
		handoff->producer_resync_pending = 0U;
		atomic_fetch_add_explicit(&handoff->resync_generation, 1U, memory_order_release);
	}
}

/**
 * @brief Move an idle consumer cursor to the latest currently published slot.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots.
 * @return Nonzero when the cursor skipped at least one stale slot.
 */
static int
portaudio_poc_handoff_consumer_resynchronize(struct usbradioplus_portaudio_poc_handoff *handoff,
					     unsigned int slot_count)
{
	for (;;) {
		unsigned int consumer_state =
			atomic_load_explicit(&handoff->consumer_state, memory_order_acquire);
		unsigned int consumer;
		unsigned int producer;
		unsigned int newest;
		unsigned int skipped;

		if (consumer_state & URP_PORTAUDIO_POC_HANDOFF_READING)
			return 0;
		consumer = consumer_state & URP_PORTAUDIO_POC_HANDOFF_INDEX_MASK;
		producer = atomic_load_explicit(&handoff->producer, memory_order_acquire);
		if (consumer == producer)
			return 0;
		newest = portaudio_poc_handoff_previous(producer, slot_count);
		if (consumer == newest)
			return 0;
		skipped = portaudio_poc_handoff_distance(consumer, newest, slot_count);
		URP_HANDOFF_INTERLEAVE(2U);
		if (atomic_compare_exchange_weak_explicit(&handoff->consumer_state, &consumer_state,
							  newest, memory_order_acq_rel,
							  memory_order_acquire)) {
			atomic_fetch_add_explicit(&handoff->discarded, skipped,
						  memory_order_relaxed);
			return 1;
		}
	}
}

enum usbradioplus_portaudio_poc_handoff_result usbradioplus_portaudio_poc_handoff_consumer_claim(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int *seen_generation, unsigned int *slot, int *resynchronized)
{
	unsigned int consumer_state;
	unsigned int consumer;
	unsigned int producer;

	if (resynchronized)
		*resynchronized = 0;
	if (!handoff || !seen_generation || !slot ||
	    !portaudio_poc_handoff_slot_count_valid(slot_count))
		return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID;
	for (;;) {
		unsigned int generation =
			atomic_load_explicit(&handoff->resync_generation, memory_order_acquire);

		if (generation & 1U)
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY;
		if (generation != *seen_generation) {
			if (portaudio_poc_handoff_consumer_resynchronize(handoff, slot_count) &&
			    resynchronized)
				*resynchronized = 1;
			*seen_generation = generation;
			continue;
		}
		consumer_state =
			atomic_load_explicit(&handoff->consumer_state, memory_order_acquire);
		if (consumer_state & URP_PORTAUDIO_POC_HANDOFF_READING)
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID;
		consumer = consumer_state & URP_PORTAUDIO_POC_HANDOFF_INDEX_MASK;
		producer = atomic_load_explicit(&handoff->producer, memory_order_acquire);
		if (consumer == producer)
			return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY;
		URP_HANDOFF_INTERLEAVE(3U);
		if (!atomic_compare_exchange_weak_explicit(
			    &handoff->consumer_state, &consumer_state,
			    consumer | URP_PORTAUDIO_POC_HANDOFF_READING, memory_order_acq_rel,
			    memory_order_acquire))
			continue;
		/* If a full-ring marker arrived after the claim, discard this newly stale
		 * block before copying it and let the next pass select the latest block. */
		URP_HANDOFF_INTERLEAVE(4U);
		if (atomic_load_explicit(&handoff->resync_generation, memory_order_acquire) !=
		    *seen_generation) {
			usbradioplus_portaudio_poc_handoff_consumer_discard(handoff, slot_count,
									    consumer);
			if (resynchronized)
				*resynchronized = 1;
			continue;
		}
		*slot = consumer;
		return USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY;
	}
}

int usbradioplus_portaudio_poc_handoff_claim_current(
	const struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int claim_generation)
{
	unsigned int generation;

	if (!handoff)
		return 0;
	generation = atomic_load_explicit(&handoff->resync_generation, memory_order_acquire);
	return !(generation & 1U) && generation == claim_generation;
}

void usbradioplus_portaudio_poc_handoff_consumer_release(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int slot)
{
	if (!handoff || !portaudio_poc_handoff_slot_count_valid(slot_count) || slot >= slot_count)
		return;
	atomic_store_explicit(&handoff->consumer_state,
			      portaudio_poc_handoff_next(slot, slot_count), memory_order_release);
}

void usbradioplus_portaudio_poc_handoff_consumer_discard(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int slot)
{
	if (!handoff || !portaudio_poc_handoff_slot_count_valid(slot_count) || slot >= slot_count)
		return;
	atomic_fetch_add_explicit(&handoff->discarded, 1U, memory_order_relaxed);
	usbradioplus_portaudio_poc_handoff_consumer_release(handoff, slot_count, slot);
}
