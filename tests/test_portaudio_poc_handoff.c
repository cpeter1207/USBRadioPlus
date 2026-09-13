/**
 * @file test_portaudio_poc_handoff.c
 * @brief Deterministic no-hardware tests for PortAudio receive handoff recovery.
 */

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>

#include "usbradioplus_portaudio_poc_handoff.h"

enum { TEST_SLOT_COUNT = 8U };

/** @brief One-shot boundary and competing-actor state selected by the test. */
static unsigned int interleave_boundary, interleave_consumer, interleave_generation;

/** @brief Perform one competing actor's publication deterministically, without a thread race. */
void usbradioplus_portaudio_poc_handoff_test_interleave(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int boundary)
{
	if (boundary != interleave_boundary)
		return;
	interleave_boundary = 0U;
	if (boundary == 4U)
		atomic_store(&handoff->resync_generation, interleave_generation);
	else
		atomic_store(&handoff->consumer_state, interleave_consumer);
}

/** @brief Reserve, label, and publish one producer slot. */
static void publish_block(struct usbradioplus_portaudio_poc_handoff *handoff,
			  unsigned int labels[TEST_SLOT_COUNT], unsigned int label)
{
	unsigned int slot = 0U;

	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(handoff, TEST_SLOT_COUNT,
								   &slot) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	labels[slot] = label;
	usbradioplus_portaudio_poc_handoff_producer_publish(handoff, TEST_SLOT_COUNT);
}

/** @brief Claim one delivered block and release it after copying its label. */
static unsigned int claim_block(struct usbradioplus_portaudio_poc_handoff *handoff,
				const unsigned int labels[TEST_SLOT_COUNT],
				unsigned int *generation, int *resynchronized)
{
	unsigned int slot = 0U;

	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(
		       handoff, TEST_SLOT_COUNT, generation, &slot, resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(usbradioplus_portaudio_poc_handoff_claim_current(handoff, *generation));
	{
		unsigned int label = labels[slot];

		usbradioplus_portaudio_poc_handoff_consumer_release(handoff, TEST_SLOT_COUNT, slot);
		return label;
	}
}

/** @brief Verify ordinary producer/consumer delivery retains FIFO order. */
static void test_normal_delivery(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int labels[TEST_SLOT_COUNT] = {0U};
	unsigned int generation = 0U;
	int resynchronized = -1;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	publish_block(&handoff, labels, 10U);
	publish_block(&handoff, labels, 11U);
	assert(claim_block(&handoff, labels, &generation, &resynchronized) == 10U);
	assert(!resynchronized);
	assert(claim_block(&handoff, labels, &generation, &resynchronized) == 11U);
	assert(!resynchronized);
	assert(atomic_load_explicit(&handoff.discarded, memory_order_relaxed) == 0U);
}

/** @brief Verify overflow drops stale blocks and resumes at the newest coherent block. */
static void test_overflow_resynchronizes_to_newest(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int labels[TEST_SLOT_COUNT] = {0U};
	unsigned int generation = 0U;
	int resynchronized = 0;
	unsigned int label;
	unsigned int index;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	for (index = 0U; index < TEST_SLOT_COUNT - 1U; ++index)
		publish_block(&handoff, labels, index);
	/* This newest block replaces the oldest queued block rather than being lost. */
	publish_block(&handoff, labels, 99U);
	label = claim_block(&handoff, labels, &generation, &resynchronized);
	assert(resynchronized);
	assert(label == 99U);
	assert(atomic_load_explicit(&handoff.discarded, memory_order_relaxed) ==
	       TEST_SLOT_COUNT - 1U);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, TEST_SLOT_COUNT,
								 &generation, &index, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY);
	usbradioplus_portaudio_poc_handoff_reset(&handoff);
	assert(atomic_load_explicit(&handoff.producer, memory_order_relaxed) == 0U);
	assert(atomic_load_explicit(&handoff.consumer_state, memory_order_relaxed) == 0U);
	assert(atomic_load_explicit(&handoff.resync_generation, memory_order_relaxed) == 0U);
	assert(!handoff.producer_resync_pending);
	assert(atomic_load_explicit(&handoff.discarded, memory_order_relaxed) == 0U);
}

/** @brief Record a state edge exactly as the delivery worker does. */
static void deliver_edge_if_changed(int keyed, int *delivered_keyed, unsigned int *edge_count,
				    int *last_edge)
{
	if (*delivered_keyed != keyed) {
		*delivered_keyed = keyed;
		*last_edge = keyed;
		(*edge_count)++;
	}
}

/** @brief Verify stale transitions never produce intermediate receive-state edges. */
static void test_overflow_preserves_one_coherent_edge(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int labels[TEST_SLOT_COUNT] = {0U};
	unsigned int generation = 0U;
	unsigned int edge_count = 0U;
	unsigned int slot = 0U;
	unsigned int index;
	int resynchronized = 0;
	int delivered_keyed = 0;
	int last_edge = -1;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	/* Deliver an initial key edge before the worker falls behind. */
	publish_block(&handoff, labels, 1U);
	assert(claim_block(&handoff, labels, &generation, &resynchronized) == 1U);
	deliver_edge_if_changed(1, &delivered_keyed, &edge_count, &last_edge);
	assert(edge_count == 1U && last_edge == 1);
	/* The stale queue deliberately contains alternating state, but the final
	 * newest block is unkeyed. Recovery must emit only that one unkey edge. */
	for (index = 0U; index < TEST_SLOT_COUNT - 1U; ++index)
		publish_block(&handoff, labels, (index & 1U) ? 1U : 0U);
	publish_block(&handoff, labels, 0U);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(
		       &handoff, TEST_SLOT_COUNT, &generation, &slot, &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(resynchronized);
	assert(labels[slot] == 0U);
	assert(usbradioplus_portaudio_poc_handoff_claim_current(&handoff, generation));
	usbradioplus_portaudio_poc_handoff_consumer_release(&handoff, TEST_SLOT_COUNT, slot);
	deliver_edge_if_changed(0, &delivered_keyed, &edge_count, &last_edge);
	assert(edge_count == 2U && last_edge == 0);
}

/** @brief Verify an active consumer claim is never overwritten by an overflow producer. */
static void test_overflow_never_reuses_claimed_slot(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int labels[TEST_SLOT_COUNT] = {0U};
	unsigned int generation = 0U;
	unsigned int slot = 0U;
	unsigned int index;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	for (index = 0U; index < TEST_SLOT_COUNT - 1U; ++index)
		publish_block(&handoff, labels, index);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, TEST_SLOT_COUNT,
								 &generation, &slot, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(slot == 0U && labels[slot] == 0U);
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(&handoff, TEST_SLOT_COUNT,
								   &index) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_DROPPED);
	assert(labels[slot] == 0U);
	assert(!usbradioplus_portaudio_poc_handoff_claim_current(&handoff, generation));
	usbradioplus_portaudio_poc_handoff_consumer_discard(&handoff, TEST_SLOT_COUNT, slot);
}

/** @brief Reject incomplete public calls without modifying valid handoff state. */
static void test_handoff_boundaries(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int generation = 0U;
	unsigned int slot = 0U;
	int resynchronized = 0;
	unsigned int invalid_counts[] = {0U, 1U, UINT_MAX};
	size_t index;

	usbradioplus_portaudio_poc_handoff_init(NULL);
	usbradioplus_portaudio_poc_handoff_reset(NULL);
	usbradioplus_portaudio_poc_handoff_init(&handoff);
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(NULL, 8U, &slot) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(&handoff, 8U, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(NULL, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, NULL, &slot, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, NULL,
								 NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	usbradioplus_portaudio_poc_handoff_producer_publish(NULL, 8U);
	usbradioplus_portaudio_poc_handoff_consumer_release(NULL, 8U, 0U);
	usbradioplus_portaudio_poc_handoff_consumer_discard(NULL, 8U, 0U);
	usbradioplus_portaudio_poc_handoff_consumer_release(&handoff, 8U, 8U);
	usbradioplus_portaudio_poc_handoff_consumer_discard(&handoff, 8U, 8U);
	assert(!usbradioplus_portaudio_poc_handoff_claim_current(NULL, 0U));
	for (index = 0U; index < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++index) {
		unsigned int count = invalid_counts[index];
		assert(usbradioplus_portaudio_poc_handoff_producer_reserve(&handoff, count,
									   &slot) ==
		       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
		assert(usbradioplus_portaudio_poc_handoff_consumer_claim(
			       &handoff, count, &generation, &slot, NULL) ==
		       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
		usbradioplus_portaudio_poc_handoff_producer_publish(&handoff, count);
		usbradioplus_portaudio_poc_handoff_consumer_release(&handoff, count, 0U);
		usbradioplus_portaudio_poc_handoff_consumer_discard(&handoff, count, 0U);
	}
	assert(atomic_load(&handoff.producer) == 0U);
	assert(atomic_load(&handoff.consumer_state) == 0U);
	assert(atomic_load(&handoff.discarded) == 0U);
}

/** @brief Exercise coherent empty, in-flight, and already-current resynchronization states. */
static void test_handoff_resynchronization_boundaries(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int generation = 0U;
	unsigned int slot = 0U;
	int resynchronized = -1;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	atomic_store(&handoff.resync_generation, 1U);
	assert(!usbradioplus_portaudio_poc_handoff_claim_current(&handoff, 1U));
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY);
	assert(!resynchronized && generation == 0U);
	atomic_store(&handoff.resync_generation, 2U);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY);
	assert(!resynchronized && generation == 2U);
	atomic_store(&handoff.producer, 1U);
	atomic_store(&handoff.resync_generation, 4U);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(!resynchronized && generation == 4U && slot == 0U);
	atomic_store(&handoff.resync_generation, 6U);
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID);
	assert(!resynchronized && generation == 6U);
	usbradioplus_portaudio_poc_handoff_consumer_release(&handoff, 8U, 0U);
}

/** @brief Replay atomic conflicts and generation changes at each retry boundary. */
static void test_handoff_atomic_interleavings(void)
{
	struct usbradioplus_portaudio_poc_handoff handoff;
	unsigned int generation, slot;
	int resynchronized;

	usbradioplus_portaudio_poc_handoff_init(&handoff);
	atomic_store(&handoff.producer, 7U);
	interleave_boundary = 1U;
	interleave_consumer = 1U;
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(&handoff, 8U, &slot) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(slot == 7U && handoff.producer_resync_pending == 1U && !interleave_boundary);
	usbradioplus_portaudio_poc_handoff_producer_publish(&handoff, 8U);
	assert(atomic_load(&handoff.resync_generation) == 2U);

	usbradioplus_portaudio_poc_handoff_reset(&handoff);
	atomic_store(&handoff.producer, 7U);
	interleave_boundary = 1U;
	interleave_consumer = UINT_MAX - (UINT_MAX >> 1U);
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(&handoff, 8U, &slot) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_DROPPED);
	assert(atomic_load(&handoff.discarded) == 1U && !interleave_boundary);

	usbradioplus_portaudio_poc_handoff_reset(&handoff);
	atomic_store(&handoff.producer, 2U);
	atomic_store(&handoff.resync_generation, 2U);
	generation = 0U;
	interleave_boundary = 2U;
	interleave_consumer = 1U;
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(slot == 1U && !interleave_boundary);

	usbradioplus_portaudio_poc_handoff_reset(&handoff);
	atomic_store(&handoff.producer, 2U);
	generation = 0U;
	interleave_boundary = 3U;
	interleave_consumer = 1U;
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&handoff, 8U, &generation, &slot,
								 &resynchronized) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(slot == 1U && !interleave_boundary);

	for (unsigned int report = 0U; report < 2U; ++report) {
		usbradioplus_portaudio_poc_handoff_reset(&handoff);
		atomic_store(&handoff.producer, 2U);
		generation = 0U;
		interleave_boundary = 4U;
		interleave_generation = 2U;
		assert(usbradioplus_portaudio_poc_handoff_consumer_claim(
			       &handoff, 8U, &generation, &slot, report ? &resynchronized : NULL) ==
		       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
		assert(slot == 1U && generation == 2U && !interleave_boundary);
		assert(atomic_load(&handoff.discarded) == 1U);
		if (report)
			assert(resynchronized);
	}
}

int main(void)
{
	test_normal_delivery();
	test_overflow_resynchronizes_to_newest();
	test_overflow_preserves_one_coherent_edge();
	test_overflow_never_reuses_claimed_slot();
	test_handoff_boundaries();
	test_handoff_resynchronization_boundaries();
	test_handoff_atomic_interleavings();
	return 0;
}
