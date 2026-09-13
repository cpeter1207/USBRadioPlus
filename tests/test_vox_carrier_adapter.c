/**
 * @file test_vox_carrier_adapter.c
 * @brief Focused ABI checks for Rust-owned legacy VOX carrier-hang timing.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

static void advance(struct rptadv_radio_vox_carrier_state *state, uint32_t detector_active,
		    int32_t hang_time_ms, int32_t elapsed_ms)
{
	const struct rptadv_radio_vox_carrier_input input = {
		.detector_active = detector_active,
		.hang_time_ms = hang_time_ms,
		.elapsed_ms = elapsed_ms,
	};

	assert(urp_radio_core_vox_carrier_advance(&input, state) == 0);
}

int main(void)
{
	struct rptadv_radio_vox_carrier_state state = {
		.remaining_ms = 20,
		.carrier_detect = 0U,
	};
	struct rptadv_radio_vox_carrier_state whole = {0};
	struct rptadv_radio_vox_carrier_state split = {0};
	struct rptadv_radio_vox_carrier_state before;
	struct rptadv_radio_vox_carrier_input invalid = {
		.detector_active = 2U,
		.hang_time_ms = 40,
		.elapsed_ms = 20,
	};
	const int32_t partitions[] = {0, 0, 20};
	size_t index;

	assert(urp_radio_core_initialize() == 0);

	/* Legacy tests the timer before consume, so exact expiry remains carrier
	 * for this callback and clears only on the next one. */
	advance(&state, 0U, 40, 20);
	assert(state.remaining_ms == 0 && state.carrier_detect == 1U);
	advance(&state, 0U, 40, 20);
	assert(state.remaining_ms == 0 && state.carrier_detect == 0U);

	/* A detector hit reloads before elapsed time is consumed. */
	advance(&state, 1U, 40, 20);
	assert(state.remaining_ms == 20 && state.carrier_detect == 1U);

	/* Stable detector and silence spans produce the same result whether native
	 * callbacks arrive whole or are partitioned around millisecond boundaries. */
	advance(&whole, 1U, 40, 20);
	for (index = 0U; index < sizeof(partitions) / sizeof(partitions[0]); ++index)
		advance(&split, 1U, 40, partitions[index]);
	assert(!memcmp(&whole, &split, sizeof(whole)));
	advance(&whole, 0U, 40, 20);
	for (index = 0U; index < sizeof(partitions) / sizeof(partitions[0]); ++index)
		advance(&split, 0U, 40, partitions[index]);
	assert(!memcmp(&whole, &split, sizeof(whole)));
	advance(&whole, 0U, 40, 20);
	for (index = 0U; index < sizeof(partitions) / sizeof(partitions[0]); ++index)
		advance(&split, 0U, 40, partitions[index]);
	assert(!memcmp(&whole, &split, sizeof(whole)) && whole.carrier_detect == 0U);

	/* Existing C accepts nonpositive timing values; preserve that behavior. */
	state = (struct rptadv_radio_vox_carrier_state){.remaining_ms = 7};
	advance(&state, 0U, 40, -2);
	assert(state.remaining_ms == 7 && state.carrier_detect == 1U);
	advance(&state, 1U, -1, 20);
	assert(state.remaining_ms == 0 && state.carrier_detect == 0U);

	before = state;
	assert(urp_radio_core_vox_carrier_advance(&invalid, &state) != 0);
	assert(!memcmp(&state, &before, sizeof(state)));
	assert(urp_radio_core_vox_carrier_advance(NULL, &state) != 0);
	assert(urp_radio_core_vox_carrier_advance(&invalid, NULL) != 0);
	assert(!memcmp(&state, &before, sizeof(state)));
	return 0;
}
