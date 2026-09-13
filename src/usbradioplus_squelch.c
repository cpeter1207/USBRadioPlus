/**
 * @file usbradioplus_squelch.c
 * @brief Portable-core bridge and compatibility fallback for MICOR squelch.
 */

#include "usbradioplus_squelch.h"

#include "usbradioplus_radio_core_adapter.h"

/** @brief Advance the retained C implementation when the optional Rust ABI is unavailable.
 * @param state Persistent detector state.
 * @param squelched Previous squelch-closed indication.
 * @param sample_power Current noise sample power.
 * @param open_level Configured squelch opening threshold.
 * @param hysteresis Additional closing-threshold margin.
 * @return Nonzero when squelch remains closed.
 *
 * This is a recovery fallback for an older installed @c librptadvradio or a
 * failed shared-object call.  Its arithmetic deliberately remains identical
 * to the previous header implementation; the normal path calls the portable
 * Rust primitive through @ref urp_radio_core_micor_squelch_update.
 */
static int micor_squelch_update_legacy(struct rptadv_radio_micor_squelch_state *state,
				       int squelched, double sample_power, uint32_t open_level,
				       uint32_t hysteresis)
{
	const double detector_alpha = 0.00415799815489004;
	const double defeat_alpha = 0.02061781866875989;
	const double charge_alpha = 0.00138792482909171;
	const double release_alpha = 0.00005135243496113;
	const double idle_alpha = 0.00003255155352044;
	const double hold_threshold = 3.8 / 5.5;
	double open_power = (double)open_level * open_level;
	double limit = (double)open_level + (squelched ? 0.0 : hysteresis);
	double limit_power = limit * limit;
	double strong_power;
	double direct_power;

	if (state->settling_samples < URP_MICOR_SETTLE_SAMPLES) {
		state->settling_samples++;
		state->noise_power += (sample_power - state->noise_power) / state->settling_samples;
		state->idle_power = state->noise_power;
		return 1;
	}
	state->noise_power += detector_alpha * (sample_power - state->noise_power);
	if (squelched && state->noise_power >= open_power)
		state->idle_power += idle_alpha * (state->noise_power - state->idle_power);
	strong_power = state->idle_power * 0.01;
	/* The comparator interpolation retains the established MICOR calibration. */
	direct_power = strong_power < limit_power
			       ? limit_power + (strong_power - limit_power) * (5.0 / 11.0)
			       : limit_power;
	if (state->noise_power <= strong_power)
		state->hold_charge -= defeat_alpha * state->hold_charge;
	else if (state->noise_power < limit_power)
		state->hold_charge += charge_alpha * (1.0 - state->hold_charge);
	else
		state->hold_charge -= release_alpha * state->hold_charge;
	if (state->hold_charge < 1.0e-12)
		state->hold_charge = 0.0;
	if (state->noise_power < 1.0e-12)
		state->noise_power = 0.0;
	return !(state->noise_power < direct_power || state->hold_charge > hold_threshold);
}

int urp_micor_squelch_update(struct rptadv_radio_micor_squelch_state *state, int squelched,
			     double sample_power, uint32_t open_level, uint32_t hysteresis)
{
	int closed = 1;

	if (!state)
		return 1;
	if (!urp_radio_core_micor_squelch_update(state, squelched, sample_power, open_level,
						 hysteresis, &closed))
		return closed;
	return micor_squelch_update_legacy(state, squelched, sample_power, open_level, hysteresis);
}
