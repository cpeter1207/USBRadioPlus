#ifndef USBRADIOPLUS_SQUELCH_H
#define USBRADIOPLUS_SQUELCH_H

#include <stdint.h>

#define URP_MICOR_WEAK_CLOSE_MS 150U

struct urp_micor_squelch {
	uint32_t idle_noise;
	unsigned int close_ms;
	int strong_signal;
};

/* Model the MICOR bi-level squelch: open immediately, close a clean signal
 * immediately, and hold a noisy or fluttering signal for about 150 ms. */
static inline int urp_micor_squelch_update(struct urp_micor_squelch *state,
	int squelched, uint32_t noise, uint32_t open_level,
	uint32_t hysteresis, unsigned int elapsed_ms)
{
	uint32_t close_level;

	if (UINT32_MAX - open_level < hysteresis)
		close_level = UINT32_MAX;
	else
		close_level = open_level + hysteresis;

	if (squelched) {
		/* Follow the unsquelched discriminator-noise reference slowly enough
		 * that one quiet carrier block cannot redefine 20 dB quieting. */
		if (!state->idle_noise)
			state->idle_noise = noise;
		else
			state->idle_noise = (state->idle_noise * 31U + noise) / 32U;
		state->close_ms = 0;
		state->strong_signal = 0;
		if (noise < open_level) {
			state->strong_signal = state->idle_noise
				&& noise <= state->idle_noise / 10U;
			return 0;
		}
		return 1;
	}

	if (noise <= close_level) {
		state->close_ms = 0;
		state->strong_signal = state->idle_noise
			&& noise <= state->idle_noise / 10U;
		return 0;
	}

	if (state->strong_signal) {
		state->close_ms = 0;
		state->strong_signal = 0;
		return 1;
	}

	if (UINT32_MAX - state->close_ms < elapsed_ms)
		state->close_ms = UINT32_MAX;
	else
		state->close_ms += elapsed_ms;
	if (state->close_ms >= URP_MICOR_WEAK_CLOSE_MS) {
		state->close_ms = 0;
		return 1;
	}
	return 0;
}

#endif
