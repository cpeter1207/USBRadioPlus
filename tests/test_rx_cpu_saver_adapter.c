/**
 * @file test_rx_cpu_saver_adapter.c
 * @brief Focused ABI parity checks for Rust-owned receiver CPU-saver state.
 */

#include <assert.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_rx_cpu_saver_input input = {0};
	struct rptadv_radio_rx_cpu_saver_state state = {0};
	struct rptadv_radio_rx_cpu_saver_state before;
	unsigned int enabled;
	unsigned int carrier_detect;
	unsigned int signal_mode_null;
	unsigned int tx_ptt_in;
	unsigned int tx_ptt_out;
	unsigned int prior_halted;

	assert(urp_radio_core_initialize() == 0);
	for (enabled = 0U; enabled < 2U; ++enabled) {
		for (carrier_detect = 0U; carrier_detect < 2U; ++carrier_detect) {
			for (signal_mode_null = 0U; signal_mode_null < 2U; ++signal_mode_null) {
				for (tx_ptt_in = 0U; tx_ptt_in < 2U; ++tx_ptt_in) {
					for (tx_ptt_out = 0U; tx_ptt_out < 2U; ++tx_ptt_out) {
						for (prior_halted = 0U; prior_halted < 2U;
						     ++prior_halted) {
							unsigned int expected_halted;
							unsigned int expected_action;

							input = (struct
								 rptadv_radio_rx_cpu_saver_input){
								.enabled = enabled,
								.carrier_detect = carrier_detect,
								.signal_mode_null =
									signal_mode_null,
								.tx_ptt_in = tx_ptt_in,
								.tx_ptt_out = tx_ptt_out,
							};
							expected_halted = enabled &&
									  !carrier_detect &&
									  signal_mode_null &&
									  !tx_ptt_in && !tx_ptt_out;
							expected_action =
								expected_halted == prior_halted
									? RPTADV_RADIO_RX_CPU_SAVER_ACTION_NONE
								: expected_halted
									? RPTADV_RADIO_RX_CPU_SAVER_ACTION_ENTER
									: RPTADV_RADIO_RX_CPU_SAVER_ACTION_LEAVE;
							state = (struct
								 rptadv_radio_rx_cpu_saver_state){
								.halted = prior_halted,
								.action = UINT32_MAX,
							};
							assert(urp_radio_core_rx_cpu_saver_advance(
								       &input, &state) == 0);
							assert(state.halted == expected_halted);
							assert(state.action == expected_action);
						}
					}
				}
			}
		}
	}

	input.enabled = 2U;
	before = state;
	assert(urp_radio_core_rx_cpu_saver_advance(&input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	assert(urp_radio_core_rx_cpu_saver_advance(NULL, &state) != 0);
	assert(urp_radio_core_rx_cpu_saver_advance(&input, NULL) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
