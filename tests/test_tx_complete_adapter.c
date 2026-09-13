/**
 * @file test_tx_complete_adapter.c
 * @brief Focused ABI checks for Rust-owned transmitter completion cleanup.
 */

#include <assert.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_tx_complete_config config = {
		.struct_size = sizeof(config),
		.txrx_blanking_time_ms = 125,
	};
	struct rptadv_radio_tx_complete_state state = {
		.tx_state = RPTADV_RADIO_TX_STATE_COMPLETE,
		.tx_ptt_out = 1U,
		.tx_ctcss_option = RPTADV_RADIO_CTCSS_RENDER_OPTION_START,
		.txrx_blanking_timer_ms = 9,
		.txrx_blanking_sample_remainder = 17U,
		.tx_ctcss_ready = 0U,
	};
	struct rptadv_radio_tx_complete_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_tx_complete(&config, &state) == 0);
	assert(state.tx_state == RPTADV_RADIO_TX_STATE_IDLE);
	assert(state.tx_ptt_out == 0U);
	assert(state.tx_ctcss_option == RPTADV_RADIO_CTCSS_RENDER_OPTION_DISABLE);
	assert(state.txrx_blanking_timer_ms == 125);
	assert(state.txrx_blanking_sample_remainder == 0U);
	assert(state.tx_ctcss_ready == 1U);

	config.txrx_blanking_time_ms = -1;
	assert(urp_radio_core_tx_complete(&config, &state) == 0);
	assert(state.txrx_blanking_timer_ms == -1);

	before = state;
	config.struct_size = 0U;
	assert(urp_radio_core_tx_complete(&config, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
