/**
 * @file test_rx_blanking_adapter.c
 * @brief Focused ABI checks for Rust-owned post-transmit receive blanking.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_rx_blanking_input input = {
		.elapsed_ms = 0,
		.native_frame_count = 36U,
		.sample_remainder_before = 0U,
	};
	struct rptadv_radio_rx_blanking_state state = {
		.remaining_ms = 1,
		.blanked_frame_count = 99U,
	};
	struct rptadv_radio_rx_blanking_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_rx_blanking_advance(&input, &state) == 0);
	assert(state.blanked_frame_count == 36U && state.remaining_ms == 1);

	input.elapsed_ms = 1;
	input.sample_remainder_before = 36U;
	assert(urp_radio_core_rx_blanking_advance(&input, &state) == 0);
	assert(state.blanked_frame_count == 12U && state.remaining_ms == 0);

	state = (struct rptadv_radio_rx_blanking_state){
		.remaining_ms = 1,
		.blanked_frame_count = 99U,
	};
	input.native_frame_count = 48U;
	input.sample_remainder_before = UINT32_MAX;
	assert(urp_radio_core_rx_blanking_advance(&input, &state) == 0);
	assert(state.blanked_frame_count == 0U && state.remaining_ms == 0);

	before = state;
	input.elapsed_ms = -1;
	assert(urp_radio_core_rx_blanking_advance(&input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
