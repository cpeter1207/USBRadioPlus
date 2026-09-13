/** @file
 * @brief Executable usbradioplus dsp regression and failure-path checks.
 */

#include "../src/usbradioplus_channel_core.h"

#include <assert.h>
#include <math.h>
#include <samplerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI

#define M_PI 3.14159265358979323846
#endif

/** Controls injected allocation to fail failure for this test. */
static int allocation_to_fail;
/** Recorded allocation count for assertions. */
static int allocation_count;

/** @brief Allocate DSP test memory or return the scheduled allocation failure.
 * @param count Number of elements available in the supplied block.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @return Result used by the test's assertions.
 */
void *urp_test_calloc(size_t count, size_t size)
{
	allocation_count++;
	return allocation_count == allocation_to_fail ? NULL : calloc(count, size);
}

/** @brief Resize DSP test memory or return the scheduled allocation failure.
 * @param pointer Allocated buffer passed through the failure-injection shim.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @return Result used by the test's assertions.
 */
void *urp_test_realloc(void *pointer, size_t size)
{
	allocation_count++;
	return allocation_count == allocation_to_fail ? NULL : realloc(pointer, size);
}

/** @brief Generate deterministic PCM tone samples for rate-conversion tests.
 * @param x Sample buffer used by the test.
 * @param n Number of samples.
 * @param rate Sample rate in Hz.
 * @param hz Generated test tone frequency in Hz.
 * @param level Requested level or normalized tuning level, as declared.
 */
static void tone(int16_t *x, size_t n, unsigned int rate, double hz, double level)
{
	size_t i;
	for (i = 0; i < n; ++i)
		x[i] = (int16_t)lrint(level * sin(2.0 * M_PI * hz * i / rate));
}

/** @brief Verify src. */
static void test_src(void)
{
	struct urp_src *up = urp_src_create(0, 1);
	struct urp_src *down = urp_src_create(0, 1);
	int16_t in[URP_LINK_SAMPLES], native[URP_NATIVE_SAMPLES], back[URP_LINK_SAMPLES];
	size_t used, made, total_up = 0, total_down = 0;
	int frame;
	assert(up && down);
	assert(!urp_src_reserve(up, URP_LINK_SAMPLES, URP_NATIVE_SAMPLES));
	assert(!urp_src_reserve(down, URP_NATIVE_SAMPLES, URP_LINK_SAMPLES));
	tone(in, URP_LINK_SAMPLES, URP_RATE_LINK, 1000.0, 12000.0);
	/* Sinc converters intentionally have startup latency. Verify steady-state
	 * frame accounting rather than demanding a full first block. */
	for (frame = 0; frame < 12; ++frame) {
		assert(!urp_rate_convert_prepared(up, in, URP_LINK_SAMPLES, URP_RATE_LINK, native,
						  URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used,
						  &made));
		assert(used == URP_LINK_SAMPLES);
		total_up += made;
		assert(!urp_rate_convert_prepared(down, native, made, URP_RATE_NATIVE, back,
						  URP_LINK_SAMPLES, URP_RATE_LINK, &used, &made));
		total_down += made;
	}
	assert(total_up > 9 * URP_NATIVE_SAMPLES);
	assert(total_down > 8 * URP_LINK_SAMPLES);
	urp_src_destroy(up);
	urp_src_destroy(down);
}

/** @brief Verify the released F32 adapter preserves legacy mono sinc output exactly. */
static void test_adapter_matches_legacy_sinc(void)
{
	struct urp_src *adapter = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	SRC_DATA data;
	SRC_STATE *legacy;
	float legacy_input[URP_LINK_SAMPLES];
	float legacy_output[URP_NATIVE_SAMPLES];
	int16_t input[URP_LINK_SAMPLES], adapter_output[URP_NATIVE_SAMPLES];
	int16_t legacy_pcm[URP_NATIVE_SAMPLES];
	size_t used, made;
	int error = 0;
	int frame;

	assert(adapter);
	assert(!urp_src_reserve(adapter, URP_LINK_SAMPLES, URP_NATIVE_SAMPLES));
	legacy = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
	assert(legacy && !error);
	tone(input, URP_LINK_SAMPLES, URP_RATE_LINK, 997.0, 30000.0);
	for (frame = 0; frame < 12; ++frame) {
		src_short_to_float_array(input, legacy_input, URP_LINK_SAMPLES);
		memset(&data, 0, sizeof(data));
		data.data_in = legacy_input;
		data.data_out = legacy_output;
		data.input_frames = URP_LINK_SAMPLES;
		data.output_frames = URP_NATIVE_SAMPLES;
		data.src_ratio = (double)URP_RATE_NATIVE / URP_RATE_LINK;
		assert(!src_process(legacy, &data));
		src_float_to_short_array(legacy_output, legacy_pcm, (int)data.output_frames_gen);
		memset(legacy_pcm + data.output_frames_gen, 0,
		       (URP_NATIVE_SAMPLES - (size_t)data.output_frames_gen) * sizeof(*legacy_pcm));
		assert(!urp_rate_convert_prepared(adapter, input, URP_LINK_SAMPLES, URP_RATE_LINK,
						  adapter_output, URP_NATIVE_SAMPLES,
						  URP_RATE_NATIVE, &used, &made));
		assert(used == (size_t)data.input_frames_used);
		assert(made == (size_t)data.output_frames_gen);
		assert(!memcmp(adapter_output, legacy_pcm, sizeof(adapter_output)));
	}
	src_delete(legacy);
	urp_src_destroy(adapter);
}

/** @brief Verify same rate bypass. */
static void test_same_rate_bypass(void)
{
	int16_t input[URP_NATIVE_SAMPLES], output[URP_NATIVE_SAMPLES];
	size_t used = 0, made = 0;
	tone(input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 1234.0, 9000.0);
	assert(!urp_rate_convert_prepared(NULL, input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, output,
					  URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
	assert(used == URP_NATIVE_SAMPLES && made == URP_NATIVE_SAMPLES);
	assert(!memcmp(input, output, sizeof(input)));
}

/** @brief Verify the native prepared SRC path cannot grow callback storage. */
static void test_prepared_src_no_allocation(void)
{
	struct urp_src *src;
	int16_t input[URP_LINK_SAMPLES] = {0};
	int16_t output[URP_NATIVE_SAMPLES];
	size_t used = 0;
	size_t made = 0;
	int prepared_allocations;

	src = urp_src_create(0, 1);
	assert(src);
	allocation_count = 0;
	allocation_to_fail = 0;
	assert(!urp_src_reserve(src, URP_LINK_SAMPLES, URP_NATIVE_SAMPLES));
	prepared_allocations = allocation_count;
	/* A hidden resize would both increment the counter and fail this call. */
	allocation_to_fail = prepared_allocations + 1;
	assert(!urp_rate_convert_prepared(src, input, URP_LINK_SAMPLES, URP_RATE_LINK, output,
					  URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
	assert(allocation_count == prepared_allocations);
	assert(urp_rate_convert_prepared(src, input, URP_LINK_SAMPLES + 1U, URP_RATE_LINK, output,
					 URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made) < 0);
	assert(allocation_count == prepared_allocations);
	/* Same-rate conversion intentionally needs neither a converter nor workspace. */
	assert(!urp_rate_convert_prepared(NULL, input, URP_LINK_SAMPLES, URP_RATE_LINK, output,
					  URP_LINK_SAMPLES, URP_RATE_LINK, &used, &made));
	assert(allocation_count == prepared_allocations);
	allocation_to_fail = 0;
	urp_src_destroy(src);
}

/** @brief Verify defensive and boundary paths. */
static void test_defensive_and_boundary_paths(void)
{
	struct urp_src *src;
	int16_t mono[] = {20000, -20000, 100};
	int16_t oversized[] = {20000, -20000, 100, -100};
	int16_t extracted[3], short_output[2];
	size_t used = 99, made = 99;

	assert(!urp_src_create(0, 0));
	assert(!urp_src_create(999999, 1));
	urp_src_destroy(NULL);
	urp_src_reset(NULL);
	assert(urp_src_reserve(NULL, 1, 1) < 0);

	src = urp_src_create(0, 1);
	assert(src);
	assert(urp_src_reserve(src, 0, 1) < 0);
	assert(urp_src_reserve(src, 1, 0) < 0);
	assert(!urp_src_reserve(src, 3, 3));
	assert(urp_src_process_prepared(NULL, mono, 3, extracted, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process_prepared(src, NULL, 3, extracted, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process_prepared(src, mono, 3, NULL, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process_prepared(src, mono, 3, extracted, 3, 0.0, &used, &made) < 0);
	/* The callback-owned workspaces are fixed after preparation.  Oversized
	 * source and destination requests must fail rather than allocate there. */
	assert(urp_src_process_prepared(src, oversized, 4, extracted, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process_prepared(src, mono, 3, oversized, 4, 1.0, &used, &made) < 0);
	urp_src_reset(src);
	assert(!urp_src_process_prepared(src, mono, 3, extracted, 3, 1.0, NULL, NULL));
	urp_src_destroy(src);

	assert(urp_rate_convert_prepared(NULL, NULL, 3, 48000, extracted, 3, 48000, &used, &made) <
	       0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 0, extracted, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 48000, NULL, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 48000, extracted, 3, 0, &used, &made) < 0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 48000, short_output, 2, 48000, NULL, NULL) <
	       0);
	assert(short_output[0] == mono[0] && short_output[1] == mono[1]);
	assert(!urp_rate_convert_prepared(NULL, mono, 2, 48000, extracted, 3, 48000, &used, &made));
	assert(used == 2 && made == 2 && extracted[2] == 0);
	assert(urp_rate_convert_prepared(NULL, NULL, 3, 48000, extracted, 3, 48000, &used, &made) <
	       0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 0, extracted, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 48000, NULL, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert_prepared(NULL, mono, 3, 48000, extracted, 3, 0, &used, &made) < 0);
}

/** @brief Verify allocation and converter failures. */
static void test_allocation_and_converter_failures(void)
{
	struct urp_src *src;
	int16_t input[3] = {1, 2, 3};
	int16_t output[3];

	allocation_count = 0;
	allocation_to_fail = 1;
	assert(!urp_src_create(0, 1));
	allocation_to_fail = 0;
	src = urp_src_create(0, 1);
	assert(src);
	allocation_count = 0;
	allocation_to_fail = 1;
	assert(urp_src_reserve(src, 3, 3) < 0);
	allocation_count = 0;
	allocation_to_fail = 2;
	assert(urp_src_reserve(src, 3, 3) < 0);
	allocation_to_fail = 0;
	assert(!urp_src_reserve(src, 3, 3));
	assert(urp_src_process_prepared(src, input, 3, output, 3, 1000.0, NULL, NULL) != 0);
	urp_src_destroy(src);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_src();
	test_adapter_matches_legacy_sinc();
	test_same_rate_bypass();
	test_prepared_src_no_allocation();
	test_defensive_and_boundary_paths();
	test_allocation_and_converter_failures();
	puts("usbradioplus DSP tests passed");
	return 0;
}

/** @def M_PI
 * @brief Pi for platforms whose math headers omit it.
 */
