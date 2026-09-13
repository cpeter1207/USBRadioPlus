/**
 * @file test_signal_mode_adapter.c
 * @brief Focused ABI checks for Rust-owned legacy signaling-mode selection.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

static struct rptadv_radio_signal_mode_config signal_config(void)
{
	struct rptadv_radio_signal_mode_config config = {
		.struct_size = sizeof(config),
		.hold_ms = 20,
		.ctcss_tx_enabled = 1U,
		.default_tx_ctcss_frequency_tenths_hz = 670,
	};

	config.mapped_tx_ctcss_frequency_tenths_hz[4] = 1000;
	return config;
}

int main(void)
{
	struct rptadv_radio_signal_mode_config config = signal_config();
	struct rptadv_radio_signal_mode_input input = {
		.decoded_ctcss = 4,
	};
	struct rptadv_radio_signal_mode_state state = {
		.last_rx_ctcss = -1,
	};
	struct rptadv_radio_signal_mode_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_signal_mode_advance(&config, &input, &state) == 0);
	assert(state.smode == RPTADV_RADIO_SIGNAL_MODE_CTCSS);
	assert(state.smode_was == RPTADV_RADIO_SIGNAL_MODE_CTCSS);
	assert(state.smode_timer_ms == 20);
	assert(state.last_rx_ctcss == 4);
	assert(state.tx_ctcss_frequency_tenths_hz == 1000);
	assert(state.tx_ctcss_option == 1U);

	/* The timer is frozen while transmitter PTT input remains asserted. */
	state.tx_ctcss_option = 0U;
	input.decoded_ctcss = -1;
	input.elapsed_ms = 100;
	input.tx_ptt_in = 1U;
	assert(urp_radio_core_signal_mode_advance(&config, &input, &state) == 0);
	assert(state.smode_timer_ms == 20);
	assert(state.tx_ctcss_option == 1U);
	assert(state.tx_ctcss_frequency_tenths_hz == 670);

	/* An expired hold latches the historical turn-off indication. */
	state.tx_ctcss_option = 0U;
	input.tx_ptt_in = 0U;
	input.elapsed_ms = 20;
	assert(urp_radio_core_signal_mode_advance(&config, &input, &state) == 0);
	assert(state.smode == RPTADV_RADIO_SIGNAL_MODE_NONE);
	assert(state.smode_was == RPTADV_RADIO_SIGNAL_MODE_CTCSS);
	assert(state.smode_timer_ms == 0);
	assert(state.last_rx_ctcss == -1);
	assert(state.smode_turnoff == 1U);

	/* Receive-only mappings must not replace a selected transmitter tone. */
	config.mapped_tx_ctcss_frequency_tenths_hz[4] = 0;
	state = (struct rptadv_radio_signal_mode_state){
		.last_rx_ctcss = -1,
		.tx_ctcss_frequency_tenths_hz = 777,
		.tx_ctcss_option = 2U,
	};
	input = (struct rptadv_radio_signal_mode_input){
		.decoded_ctcss = 4,
	};
	assert(urp_radio_core_signal_mode_advance(&config, &input, &state) == 0);
	assert(state.last_rx_ctcss == 4);
	assert(state.tx_ctcss_frequency_tenths_hz == 777);
	assert(state.tx_ctcss_option == 2U);

	/* A rejected request leaves compatibility state untouched for C fallback. */
	before = state;
	config.ctcss_tx_enabled = 2U;
	assert(urp_radio_core_signal_mode_advance(&config, &input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
