/**
 * @file test_dcs_turnoff_adapter.c
 * @brief Focused ABI checks for the Rust-owned DCS turn-off transition.
 */

#include <assert.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_dcs_turnoff_config config = {
		.turnoff_duration_ms = 180,
	};
	struct rptadv_radio_dcs_turnoff_input input = {
		.elapsed_ms = 20,
		.begin_turnoff = 1U,
	};
	struct rptadv_radio_dcs_turnoff_state state = {
		.tx_state = RPTADV_RADIO_DCS_TURNOFF_STATE_ACTIVE,
		.dcs_turnoff_remaining_ms = 9,
		.tx_hang_remaining_ms = 42,
		.finish_requested = 99U,
		.finish_elapsed_ms = 77,
	};
	struct rptadv_radio_dcs_turnoff_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_dcs_turnoff_advance(&config, &input, &state) == 0);
	assert(state.tx_state == RPTADV_RADIO_DCS_TURNOFF_STATE_TOC);
	assert(state.dcs_turnoff_remaining_ms == 160);
	assert(state.tx_hang_remaining_ms == 0);
	assert(state.finish_requested == 0U && state.finish_elapsed_ms == 0);

	/* A rekey cancels the tail without allowing its stale timer to finish. */
	input = (struct rptadv_radio_dcs_turnoff_input){
		.elapsed_ms = 20,
		.tx_ptt_in = 1U,
	};
	state.tx_hang_remaining_ms = 13;
	assert(urp_radio_core_dcs_turnoff_advance(&config, &input, &state) == 0);
	assert(state.tx_state == RPTADV_RADIO_DCS_TURNOFF_STATE_ACTIVE);
	assert(state.dcs_turnoff_remaining_ms == 0);
	assert(state.tx_hang_remaining_ms == 13);
	assert(state.finish_requested == 0U);

	/* Expiry carries only residual time to the retained C finishing helper. */
	input = (struct rptadv_radio_dcs_turnoff_input){
		.elapsed_ms = 20,
	};
	state = (struct rptadv_radio_dcs_turnoff_state){
		.tx_state = RPTADV_RADIO_DCS_TURNOFF_STATE_TOC,
		.dcs_turnoff_remaining_ms = 15,
	};
	assert(urp_radio_core_dcs_turnoff_advance(&config, &input, &state) == 0);
	assert(state.tx_state == RPTADV_RADIO_DCS_TURNOFF_STATE_TOC);
	assert(state.dcs_turnoff_remaining_ms == 0);
	assert(state.finish_requested == 1U && state.finish_elapsed_ms == 5);

	/* Invalid descriptors leave compatibility state untouched for C fallback. */
	before = state;
	input.tx_ptt_in = 2U;
	assert(urp_radio_core_dcs_turnoff_advance(&config, &input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	assert(urp_radio_core_dcs_turnoff_advance(NULL, &input, &state) != 0);
	assert(urp_radio_core_dcs_turnoff_advance(&config, NULL, &state) != 0);
	assert(urp_radio_core_dcs_turnoff_advance(&config, &input, NULL) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
