/**
 * @file test_ctcss_render_state_adapter.c
 * @brief Focused ABI checks for Rust-owned CTCSS render-control transitions.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	struct rptadv_radio_ctcss_render_state_config config = {
		.struct_size = sizeof(config),
		.turnoff_duration_ms = 180,
		.turnoff_phase_shift_degrees = 120.0,
		.turnoff_tail_tone_hz = 55.0,
	};
	struct rptadv_radio_ctcss_render_state_input input = {
		.elapsed_ms = 20,
	};
	struct rptadv_radio_ctcss_render_state state = {
		.option = RPTADV_RADIO_CTCSS_RENDER_OPTION_START,
		.enabled = 1U,
	};
	struct rptadv_radio_ctcss_render_state before;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_ctcss_render_state_advance(&config, &input, &state) == 0);
	assert(state.option == RPTADV_RADIO_CTCSS_RENDER_OPTION_HOLD);
	assert(state.oscillator_state == RPTADV_RADIO_CTCSS_RENDER_ACTIVE);
	assert(state.enabled == 1U);
	assert(state.turnoff_remaining_ms == 0);
	assert(state.phase_shift_degrees == 0.0);
	assert(state.tail_tone_hz == 0.0);

	state.option = RPTADV_RADIO_CTCSS_RENDER_OPTION_TURNOFF;
	assert(urp_radio_core_ctcss_render_state_advance(&config, &input, &state) == 0);
	assert(state.option == RPTADV_RADIO_CTCSS_RENDER_OPTION_HOLD);
	assert(state.oscillator_state == RPTADV_RADIO_CTCSS_RENDER_TURNOFF);
	assert(state.turnoff_remaining_ms == 160);
	assert(state.phase_shift_degrees == 120.0);
	assert(state.tail_tone_hz == 55.0);

	/* Expiry requests disable on this callback; disable takes effect next time. */
	state.turnoff_remaining_ms = 20;
	state.phase_shift_degrees = 7.0;
	assert(urp_radio_core_ctcss_render_state_advance(&config, &input, &state) == 0);
	assert(state.option == RPTADV_RADIO_CTCSS_RENDER_OPTION_DISABLE);
	assert(state.oscillator_state == RPTADV_RADIO_CTCSS_RENDER_TURNOFF);
	assert(state.enabled == 1U);
	assert(state.phase_shift_degrees == 0.0);
	assert(urp_radio_core_ctcss_render_state_advance(&config, &input, &state) == 0);
	assert(state.option == RPTADV_RADIO_CTCSS_RENDER_OPTION_HOLD);
	assert(state.oscillator_state == RPTADV_RADIO_CTCSS_RENDER_DISABLED);
	assert(state.enabled == 0U);
	assert(state.tail_tone_hz == 0.0);

	/* A rejected descriptor request leaves caller state usable by the C fallback. */
	before = state;
	config.turnoff_tail_tone_hz = NAN;
	assert(urp_radio_core_ctcss_render_state_advance(&config, &input, &state) != 0);
	assert(memcmp(&state, &before, sizeof(state)) == 0);
	return 0;
}
