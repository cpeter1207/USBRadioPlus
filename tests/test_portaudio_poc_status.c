/**
 * @file test_portaudio_poc_status.c
 * @brief Deterministic no-hardware tests for PortAudio POC status retention.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_portaudio_poc_handoff.h"
#include "usbradioplus_portaudio_poc_status.h"

enum { TEST_SLOT_COUNT = 8U, TEST_TRACE_CAPACITY = 8U };

/** @brief Kind of externally visible Asterisk-boundary delivery in a test trace. */
enum test_trace_kind {
	TEST_TRACE_KEY,
	TEST_TRACE_UNKEY,
	TEST_TRACE_CTCSS,
	TEST_TRACE_VOTER,
	TEST_TRACE_VOICE,
};

/** @brief One deterministic stand-in for an Asterisk frame delivery. */
struct test_trace_entry {
	/** Event category in legacy delivery order. */
	enum test_trace_kind kind;
	/** CTCSS text when @ref kind is @ref TEST_TRACE_CTCSS. */
	char ctcss_frequency[32];
	/** Voter value when @ref kind is @ref TEST_TRACE_VOTER. */
	int voter_rssi;
};

/** @brief Append one key/unkey or voice marker to a bounded test trace. */
static void trace_marker(struct test_trace_entry trace[TEST_TRACE_CAPACITY], unsigned int *count,
			 enum test_trace_kind kind)
{
	assert(*count < TEST_TRACE_CAPACITY);
	trace[*count].kind = kind;
	++*count;
}

/** @brief Drain callback-latched status through one block's captured sequence limit. */
static void trace_status_through(struct usbradioplus_portaudio_poc_status_handoff *handoff,
				 uint64_t *next_sequence, uint64_t sequence_limit,
				 struct test_trace_entry trace[TEST_TRACE_CAPACITY],
				 unsigned int *count)
{
	while (*next_sequence < sequence_limit) {
		struct usbradioplus_portaudio_poc_status_event event;

		assert(usbradioplus_portaudio_poc_status_consume(handoff, next_sequence, &event) ==
		       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
		assert(*count < TEST_TRACE_CAPACITY);
		if (event.type == USBRADIOPLUS_PORTAUDIO_POC_STATUS_CTCSS) {
			trace[*count].kind = TEST_TRACE_CTCSS;
			strcpy(trace[*count].ctcss_frequency, event.ctcss_frequency);
		} else {
			assert(event.type == USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER);
			trace[*count].kind = TEST_TRACE_VOTER;
			trace[*count].voter_rssi = event.voter_rssi;
		}
		++*count;
	}
}

/** @brief Verify legacy key, CTCSS, voter, then voice ordering. */
static void test_key_ctcss_voter_voice_order(void)
{
	struct usbradioplus_portaudio_poc_status_handoff handoff;
	struct test_trace_entry trace[TEST_TRACE_CAPACITY] = {{0}};
	uint64_t next_sequence = 0U;
	uint64_t sequence_limit;
	unsigned int count = 0U;

	usbradioplus_portaudio_poc_status_init(&handoff);
	assert(usbradioplus_portaudio_poc_status_publish_ctcss(&handoff, "100.0") ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	assert(usbradioplus_portaudio_poc_status_publish_voter(&handoff, 731) ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	sequence_limit = usbradioplus_portaudio_poc_status_published(&handoff);
	trace_marker(trace, &count, TEST_TRACE_KEY);
	trace_status_through(&handoff, &next_sequence, sequence_limit, trace, &count);
	trace_marker(trace, &count, TEST_TRACE_VOICE);
	assert(count == 4U);
	assert(trace[0].kind == TEST_TRACE_KEY);
	assert(trace[1].kind == TEST_TRACE_CTCSS);
	assert(!strcmp(trace[1].ctcss_frequency, "100.0"));
	assert(trace[2].kind == TEST_TRACE_VOTER && trace[2].voter_rssi == 731);
	assert(trace[3].kind == TEST_TRACE_VOICE);
}

/** @brief Verify final empty CTCSS-ready text remains distinct from no CTCSS event. */
static void test_final_empty_ctcss_is_retained(void)
{
	struct usbradioplus_portaudio_poc_status_handoff handoff;
	struct test_trace_entry trace[TEST_TRACE_CAPACITY] = {{0}};
	uint64_t next_sequence = 0U;
	unsigned int count = 0U;

	usbradioplus_portaudio_poc_status_init(&handoff);
	assert(usbradioplus_portaudio_poc_status_publish_ctcss(&handoff, "100.0") ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	assert(usbradioplus_portaudio_poc_status_publish_ctcss(&handoff, "") ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	trace_marker(trace, &count, TEST_TRACE_UNKEY);
	trace_status_through(&handoff, &next_sequence,
			     usbradioplus_portaudio_poc_status_published(&handoff), trace, &count);
	trace_marker(trace, &count, TEST_TRACE_VOICE);
	assert(count == 4U);
	assert(trace[0].kind == TEST_TRACE_UNKEY);
	assert(trace[1].kind == TEST_TRACE_CTCSS);
	assert(!strcmp(trace[1].ctcss_frequency, "100.0"));
	assert(trace[2].kind == TEST_TRACE_CTCSS);
	assert(trace[2].ctcss_frequency[0] == '\0');
	assert(trace[3].kind == TEST_TRACE_VOICE);
}

/** @brief Publish one audio label together with its already-captured status boundary. */
static void publish_audio_block(struct usbradioplus_portaudio_poc_handoff *handoff,
				unsigned int labels[TEST_SLOT_COUNT],
				uint64_t limits[TEST_SLOT_COUNT], unsigned int label,
				uint64_t status_limit)
{
	unsigned int slot = 0U;

	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(handoff, TEST_SLOT_COUNT,
								   &slot) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	labels[slot] = label;
	limits[slot] = status_limit;
	usbradioplus_portaudio_poc_handoff_producer_publish(handoff, TEST_SLOT_COUNT);
}

/**
 * @brief Verify events from PCM blocks discarded by overload are still delivered.
 *
 * This models seven pending audio blocks, then an overflow replacement.  The
 * CTCSS start belongs to the discarded oldest backlog while the final empty
 * CTCSS and voter report belong to the replacement.  The status queue carries
 * all three events to the newest coherent audio block.
 */
static void test_full_audio_handoff_preserves_pending_status(void)
{
	struct usbradioplus_portaudio_poc_handoff audio_handoff;
	struct usbradioplus_portaudio_poc_status_handoff status_handoff;
	struct test_trace_entry trace[TEST_TRACE_CAPACITY] = {{0}};
	unsigned int labels[TEST_SLOT_COUNT] = {0U};
	uint64_t limits[TEST_SLOT_COUNT] = {0U};
	unsigned int generation = 0U;
	unsigned int slot = 0U;
	uint64_t next_sequence = 0U;
	unsigned int index;
	unsigned int count = 0U;

	usbradioplus_portaudio_poc_handoff_init(&audio_handoff);
	usbradioplus_portaudio_poc_status_init(&status_handoff);
	for (index = 0U; index < TEST_SLOT_COUNT - 2U; ++index)
		publish_audio_block(&audio_handoff, labels, limits, index,
				    usbradioplus_portaudio_poc_status_published(&status_handoff));
	assert(usbradioplus_portaudio_poc_status_publish_ctcss(&status_handoff, "100.0") ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	publish_audio_block(&audio_handoff, labels, limits, 6U,
			    usbradioplus_portaudio_poc_status_published(&status_handoff));
	assert(usbradioplus_portaudio_poc_status_publish_ctcss(&status_handoff, "") ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	assert(usbradioplus_portaudio_poc_status_publish_voter(&status_handoff, 492) ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	publish_audio_block(&audio_handoff, labels, limits, 99U,
			    usbradioplus_portaudio_poc_status_published(&status_handoff));
	assert(usbradioplus_portaudio_poc_handoff_consumer_claim(&audio_handoff, TEST_SLOT_COUNT,
								 &generation, &slot, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	assert(labels[slot] == 99U && limits[slot] == 3U);
	assert(usbradioplus_portaudio_poc_handoff_claim_current(&audio_handoff, generation));
	usbradioplus_portaudio_poc_handoff_consumer_release(&audio_handoff, TEST_SLOT_COUNT, slot);
	trace_marker(trace, &count, TEST_TRACE_KEY);
	trace_status_through(&status_handoff, &next_sequence, limits[slot], trace, &count);
	trace_marker(trace, &count, TEST_TRACE_VOICE);
	assert(count == 5U);
	assert(trace[0].kind == TEST_TRACE_KEY);
	assert(trace[1].kind == TEST_TRACE_CTCSS);
	assert(!strcmp(trace[1].ctcss_frequency, "100.0"));
	assert(trace[2].kind == TEST_TRACE_CTCSS && trace[2].ctcss_frequency[0] == '\0');
	assert(trace[3].kind == TEST_TRACE_VOTER && trace[3].voter_rssi == 492);
	assert(trace[4].kind == TEST_TRACE_VOICE);
}

/** @brief Verify bounded status retention reports saturation without overwriting queued text. */
static void test_status_handoff_is_bounded(void)
{
	struct usbradioplus_portaudio_poc_status_handoff handoff;
	struct usbradioplus_portaudio_poc_status_event event;
	uint64_t next_sequence = 0U;
	unsigned int index;

	usbradioplus_portaudio_poc_status_init(&handoff);
	for (index = 0U; index < URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY; ++index)
		assert(usbradioplus_portaudio_poc_status_publish_voter(&handoff, (int)index) ==
		       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
	assert(usbradioplus_portaudio_poc_status_publish_voter(&handoff, 999) ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_FULL);
	for (index = 0U; index < URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY; ++index) {
		assert(usbradioplus_portaudio_poc_status_consume(&handoff, &next_sequence,
								 &event) ==
		       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
		assert(event.type == USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER &&
		       event.voter_rssi == (int)index);
	}
	assert(usbradioplus_portaudio_poc_status_consume(&handoff, &next_sequence, &event) ==
	       USBRADIOPLUS_PORTAUDIO_POC_STATUS_EMPTY);
	usbradioplus_portaudio_poc_status_reset(&handoff);
	assert(usbradioplus_portaudio_poc_status_published(&handoff) == 0U);
}

int main(void)
{
	test_key_ctcss_voter_voice_order();
	test_final_empty_ctcss_is_retained();
	test_full_audio_handoff_preserves_pending_status();
	test_status_handoff_is_bounded();
	return 0;
}
