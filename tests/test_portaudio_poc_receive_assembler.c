/**
 * @file
 * @brief Deterministic no-hardware tests for ordinary direct-PortAudio RX assembly.
 */

#include <assert.h>
#include <stddef.h>

#include "usbradioplus_portaudio_poc.h"

/** Fill an ordered signed-PCM sequence suitable for order assertions. */
static void fill_sequence(short *samples, size_t count, short first)
{
	size_t index;

	for (index = 0U; index < count; ++index)
		samples[index] = (short)(first + (short)index);
}

/** Verify one assembled block contains the requested ordered sequence. */
static void assert_block(const short *actual, short first)
{
	size_t index;

	for (index = 0U; index < URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES; ++index)
		assert(actual[index] == (short)(first + (short)index));
}

/** Append one source span and verify the bounded FIFO result separately from assert(). */
static void assert_append_result(struct usbradioplus_portaudio_poc_receive_assembler *assembler,
				 const short *samples, size_t sample_count, int expected)
{
	int result = usbradioplus_portaudio_poc_receive_assembler_append(assembler, samples,
									 sample_count);

	assert(result == expected);
}

/** Verify a full 20 ms source span passes through unchanged. */
static void test_exact_block(void)
{
	struct usbradioplus_portaudio_poc_receive_assembler assembler = {0};
	short input[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	short output[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];

	fill_sequence(input, sizeof(input) / sizeof(*input), 100);
	assert_append_result(&assembler, input, sizeof(input) / sizeof(*input), 0);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, 100);
	assert(!assembler.sample_count);
}

/** Verify callback-sized fragments do not alter the assembled sample order. */
static void test_irregular_partitions(void)
{
	struct usbradioplus_portaudio_poc_receive_assembler assembler = {0};
	static const size_t partitions[] = {1U, 5U, 1U, 7U, 5U, 19U, 3U, 41U, 78U};
	short input[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	short output[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	size_t offset = 0U;
	size_t index;

	fill_sequence(input, sizeof(input) / sizeof(*input), -80);
	for (index = 0U; index < sizeof(partitions) / sizeof(*partitions); ++index) {
		assert_append_result(&assembler, input + offset, partitions[index], 0);
		offset += partitions[index];
		if (offset < URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES)
			assert(!usbradioplus_portaudio_poc_receive_assembler_pop(&assembler,
										 output));
	}
	assert(offset == URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, -80);
}

/** Verify an SRC carried sample begins the following full legacy handoff. */
static void test_carried_sample(void)
{
	struct usbradioplus_portaudio_poc_receive_assembler assembler = {0};
	short input[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES + 1U];
	short tail[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES - 1U];
	short output[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];

	fill_sequence(input, sizeof(input) / sizeof(*input), 500);
	fill_sequence(tail, sizeof(tail) / sizeof(*tail),
		      (short)(500 + URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES + 1U));
	assert_append_result(&assembler, input, sizeof(input) / sizeof(*input), 0);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, 500);
	assert(assembler.sample_count == 1U);
	assert_append_result(&assembler, tail, sizeof(tail) / sizeof(*tail), 0);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, (short)(500 + URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES));
}

/** Verify startup padding drains its interval instead of shifting the next one. */
static void test_startup_deficit_preserves_next_boundary(void)
{
	struct usbradioplus_portaudio_poc_receive_assembler assembler = {0};
	short input[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES - 1U];
	short next[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	short output[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	size_t taken;

	fill_sequence(input, sizeof(input) / sizeof(*input), -300);
	assert_append_result(&assembler, NULL, 0U, 0);
	assert_append_result(&assembler, input, sizeof(input) / sizeof(*input), 0);
	assert(!usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	taken = usbradioplus_portaudio_poc_receive_assembler_take(
		&assembler, output, URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES);
	assert(taken == URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES - 1U);
	for (size_t index = 0U; index < URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES - 1U; ++index)
		assert(output[index] == (short)(-300 + (short)index));
	assert(!assembler.sample_count);
	fill_sequence(next, sizeof(next) / sizeof(*next), 77);
	assert_append_result(&assembler, next, sizeof(next) / sizeof(*next), 0);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, 77);
}

/** Verify wrap, bounded overflow rejection, and reset retain no stale PCM. */
static void test_multiple_blocks_and_reset(void)
{
	struct usbradioplus_portaudio_poc_receive_assembler assembler = {0};
	short input[URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES];
	short output[URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES];
	short extra = 1;

	fill_sequence(input, sizeof(input) / sizeof(*input), 1000);
	assert_append_result(&assembler, input, sizeof(input) / sizeof(*input), 0);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, 1000);
	assert(usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
	assert_block(output, (short)(1000 + URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES));
	assert_append_result(&assembler, &extra, 1U, 0);
	assert_append_result(&assembler, input, sizeof(input) / sizeof(*input), -1);
	usbradioplus_portaudio_poc_receive_assembler_reset(&assembler);
	assert(!assembler.sample_count && !assembler.read_index);
	assert(!usbradioplus_portaudio_poc_receive_assembler_pop(&assembler, output));
}

int main(void)
{
	test_exact_block();
	test_irregular_partitions();
	test_carried_sample();
	test_startup_deficit_preserves_next_boundary();
	test_multiple_blocks_and_reset();
	return 0;
}
