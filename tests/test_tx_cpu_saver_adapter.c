/**
 * @file test_tx_cpu_saver_adapter.c
 * @brief Focused ABI checks for Rust-owned transmitter CPU-saver state.
 */

#include <assert.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_tx_cpu_saver_input input = {0};
	struct rptadv_radio_tx_cpu_saver_state state = {0};
	struct rptadv_radio_tx_cpu_saver_state before;
	unsigned int enabled;
	unsigned int tx_ptt_in;
	unsigned int tx_ptt_out;
	unsigned int tx_idle;

	assert(urp_radio_core_initialize() == 0);
	for (enabled = 0U; enabled < 2U; ++enabled) {
		for (tx_ptt_in = 0U; tx_ptt_in < 2U; ++tx_ptt_in) {
			for (tx_ptt_out = 0U; tx_ptt_out < 2U; ++tx_ptt_out) {
				for (tx_idle = 0U; tx_idle < 2U; ++tx_idle) {
					input = (struct rptadv_radio_tx_cpu_saver_input){
						.enabled = enabled,
						.tx_ptt_in = tx_ptt_in,
						.tx_ptt_out = tx_ptt_out,
						.tx_idle = tx_idle,
					};
					state.halted = 1U;
					assert(urp_radio_core_tx_cpu_saver_advance(&input,
										   &state) == 0);
					assert(state.halted ==
					       (enabled && !tx_ptt_in && !tx_ptt_out && tx_idle));
				}
			}
		}
	}

	input.enabled = 2U;
	before = state;
	assert(urp_radio_core_tx_cpu_saver_advance(&input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	assert(urp_radio_core_tx_cpu_saver_advance(NULL, &state) != 0);
	assert(urp_radio_core_tx_cpu_saver_advance(&input, NULL) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
