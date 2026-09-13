/**
 * @file test_portaudio_poc_handoff.c
 * @brief Deterministic no-hardware tests for PortAudio receive handoff recovery.
 */

#include <assert.h>
#include <stddef.h>
#include <stdatomic.h>

#include "usbradioplus_portaudio_poc_handoff.h"

enum { TEST_SLOT_COUNT = 8U };

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

int main(void)
{
	test_normal_delivery();
	test_overflow_resynchronizes_to_newest();
	test_overflow_preserves_one_coherent_edge();
	test_overflow_never_reuses_claimed_slot();
	return 0;
}
