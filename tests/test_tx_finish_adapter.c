/**
 * @file test_tx_finish_adapter.c
 * @brief Focused ABI checks for the Rust-owned normal transmitter drain.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_tx_finish_input input = {
		.elapsed_ms = 20,
	};
	struct rptadv_radio_tx_finish_state state = {
		.buffer_clear_frames = 99,
		.finish_remaining_ms = 9,
		.tx_state = 1,
	};
	struct rptadv_radio_tx_finish_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_tx_finish_advance(&input, &state) == 0);
	assert(state.buffer_clear_frames == 3);
	assert(state.finish_remaining_ms == 60);
	assert(state.tx_state == RPTADV_RADIO_TX_STATE_FINISHING);

	input.elapsed_ms = 80;
	assert(urp_radio_core_tx_finish_advance(&input, &state) == 0);
	assert(state.buffer_clear_frames == 0);
	assert(state.finish_remaining_ms == 0);
	assert(state.tx_state == RPTADV_RADIO_TX_STATE_COMPLETE);

	state = (struct rptadv_radio_tx_finish_state){
		.buffer_clear_frames = 3,
		.tx_state = RPTADV_RADIO_TX_STATE_FINISHING,
	};
	input.elapsed_ms = 20;
	assert(urp_radio_core_tx_finish_continue(&input, &state) == 0);
	assert(state.finish_remaining_ms == 40);
	assert(state.buffer_clear_frames == 3);
	assert(state.tx_state == RPTADV_RADIO_TX_STATE_FINISHING);
	input.elapsed_ms = 40;
	assert(urp_radio_core_tx_finish_continue(&input, &state) == 0);
	assert(state.finish_remaining_ms == 0);
	assert(state.buffer_clear_frames == 0);
	assert(state.tx_state == RPTADV_RADIO_TX_STATE_COMPLETE);

	before = state;
	input.elapsed_ms = -1;
	assert(urp_radio_core_tx_finish_advance(&input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
