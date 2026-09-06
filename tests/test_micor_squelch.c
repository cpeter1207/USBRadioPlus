/** @file
 * @brief Executable micor squelch regression and failure-path checks.
 */

#include "../src/usbradioplus_squelch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** @brief Prime the squelch model with no-carrier noise measurements.
 * @param state Processor or stream state owned by the caller.
 */
static void establish_idle(struct urp_micor_squelch *state)
{
	int i;
	for (i = 0; i < 64; ++i)
		assert(urp_micor_squelch_update(state, 1, 10000, 7000, 500, 20));
}

/** @brief Verify immediate open and clean close. */
static void test_immediate_open_and_clean_close(void)
{
	struct urp_micor_squelch state = {0};
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 900, 7000, 500, 20));
	assert(urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

/** @brief Verify weak signal has approximately 150 ms tail. */
static void test_weak_signal_has_approximately_150_ms_tail(void)
{
	struct urp_micor_squelch state = {0};
	int i;
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 3000, 7000, 500, 20));
	for (i = 0; i < 7; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
	assert(urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

/** @brief Verify flutter cancels pending close. */
static void test_flutter_cancels_pending_close(void)
{
	struct urp_micor_squelch state = {0};
	int i;
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 3000, 7000, 500, 20));
	for (i = 0; i < 6; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
	assert(!urp_micor_squelch_update(&state, 0, 7000, 7000, 500, 20));
	for (i = 0; i < 7; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

/** @brief Verify counter and threshold saturation. */
static void test_counter_and_threshold_saturation(void)
{
	struct urp_micor_squelch state = {
		.idle_noise = 10000,
		.close_ms = UINT32_MAX - 5U,
	};
	assert(!urp_micor_squelch_update(&state, 0, UINT32_MAX, UINT32_MAX - 10U, 20U, 10U));
	state.close_ms = UINT32_MAX - 5U;
	assert(urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 10U));
	assert(state.close_ms == 0);
	memset(&state, 0, sizeof(state));
	assert(!urp_micor_squelch_update(&state, 1, 0, 1, 0, 1));
	memset(&state, 0, sizeof(state));
	assert(!urp_micor_squelch_update(&state, 0, 0, 1, 0, 1));
	state.idle_noise = 10000;
	assert(!urp_micor_squelch_update(&state, 0, 900, 7000, 500, 1));
	assert(state.strong_signal);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_immediate_open_and_clean_close();
	test_weak_signal_has_approximately_150_ms_tail();
	test_flutter_cancels_pending_close();
	test_counter_and_threshold_saturation();
	puts("MICOR-style squelch tests passed");
	return 0;
}
