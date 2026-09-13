/**
 * @file test_audio_meter_adapter.c
 * @brief Verify the signed-16 compatibility bridge to the Rust raw PCM meter.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "usbradioplus_radio_core_adapter.h"

/** @brief Assert two public meter states have equal fields. */
static void assert_statistics_equal(const struct rptadv_radio_audio_statistics *left,
				    const struct rptadv_radio_audio_statistics *right)
{
	assert(!memcmp(left->maxbuf, right->maxbuf, sizeof(left->maxbuf)));
	assert(!memcmp(left->clipbuf, right->clipbuf, sizeof(left->clipbuf)));
	assert(!memcmp(left->pwrbuf, right->pwrbuf, sizeof(left->pwrbuf)));
	assert(left->index == right->index);
}

/** @brief Check that the bridge retains ASL3's raw stereo meter semantics. */
static void test_stereo_parity(void)
{
	int16_t samples[12U * 160U] = {0};
	float workspace[12U * 160U];
	struct rptadv_radio_audio_statistics statistics = {0};
	uint32_t expected_power;
	int clipping = 0;
	unsigned int offset;

	for (offset = 0U; offset < 4U; ++offset)
		samples[10U + offset * 12U] = INT16_MAX;
	samples[10U + 4U * 12U] = INT16_MIN;
	/* A high non-selected 48 kHz phase must remain invisible to the meter. */
	samples[0] = INT16_MIN;
	expected_power = (uint32_t)((4ULL * INT16_MAX * INT16_MAX + 1ULL * 32768U * 32768U) / 160U);
	assert(!urp_radio_core_measure_audio_s16(samples, 12U * 160U, 2U, &statistics, workspace,
						 12U * 160U, &clipping));
	assert(clipping == 1);
	assert(statistics.index == 1);
	assert(statistics.maxbuf[0] == 32768U);
	assert(statistics.clipbuf[0] == 4U);
	assert(statistics.pwrbuf[0] == expected_power);
	assert(samples[0] == INT16_MIN);
}

/** @brief Check invalid arguments preserve meter state and clear status. */
static void test_invalid_and_empty_spans(void)
{
	struct rptadv_radio_audio_statistics statistics = {
		.maxbuf = {7}, .clipbuf = {8}, .pwrbuf = {9}, .index = 3};
	struct rptadv_radio_audio_statistics expected = statistics;
	float workspace[6] = {0};
	int16_t samples[6] = {0};
	int clipping = 99;

	assert(urp_radio_core_measure_audio_s16(samples, 6U, 3U, &statistics, workspace, 6U,
						&clipping) == -1);
	assert(clipping == 0);
	assert_statistics_equal(&statistics, &expected);
	clipping = 99;
	assert(urp_radio_core_measure_audio_s16(samples, 6U, 1U, &statistics, workspace, 5U,
						&clipping) == -1);
	assert(clipping == 0);
	assert_statistics_equal(&statistics, &expected);
	assert(!urp_radio_core_measure_audio_s16(NULL, 0U, 1U, &statistics, NULL, 0U, &clipping));
	assert(statistics.index == 4);
}

/** @brief Run bridge compatibility regressions. */
int main(void)
{
	assert(!urp_radio_core_initialize());
	test_stereo_parity();
	test_invalid_and_empty_spans();
	return 0;
}
