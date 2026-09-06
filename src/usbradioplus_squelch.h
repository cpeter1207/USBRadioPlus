/** @file
 * @brief Sample-clocked equivalent of the MICOR dual-time-constant squelch.
 */

#ifndef USBRADIOPLUS_SQUELCH_H
#define USBRADIOPLUS_SQUELCH_H

#include <stdint.h>

/** Detector settling interval at the fixed 48 kHz discriminator sample rate. */
#define URP_MICOR_SETTLE_SAMPLES 480U

/** Noise detector and long-tail capacitor state, independent of audio blocks. */
struct urp_micor_squelch {
	/** Smoothed discriminator-noise power, in squared calibration units. */
	double noise_power;
	/** Slowly tracked no-carrier reference for the 20 dB quieting comparator. */
	double idle_power;
	/** Long-tail capacitor charge, normalized to its fixed full-charge voltage. */
	double hold_charge;
	/** Samples accumulated during initial noise-detector settling. */
	unsigned int settling_samples;
};

/** @brief Advance the noise detector, three comparators and storage capacitor by one sample.
 * @param state Persistent detector state; zero-initialize at stream creation.
 * @param squelched Previous sample's output: nonzero closed, zero open.
 * @param sample_power Squared noise-filter sample scaled by 960/256 for compatibility.
 * @param open_level Calibrated noise threshold at which weak-signal charging begins.
 * @param hysteresis Additional noise margin while the squelch is open.
 * @return New closed state for this 48 kHz sample, not the containing audio frame.
 */
static inline int urp_micor_squelch_update(struct urp_micor_squelch *state, int squelched,
					   double sample_power, uint32_t open_level,
					   uint32_t hysteresis)
{
	/* Motorola US3628058: separate fast detector, finite charging path,
	 * fixed long-tail charge, and strong-signal defeat. A sudden loss must
	 * cross the charging region before the capacitor can reopen the gate.
	 * Coefficients are 1-exp(-1/(48000*tau)); no per-sample transcendental
	 * functions or frame-boundary decisions are needed.
	 *
	 * Detector tau=5 ms, defeat tau=1 ms, recharge tau=15 ms. The detector
	 * smooths random noise troughs that otherwise repeatedly recharge the
	 * long hold even with no carrier. Long decay is calibrated
	 * to 150 ms from full charge to the 3.8/5.5 comparator fraction. These
	 * are behavioral constants, not inferred IC resistor measurements. */
	const double detector_alpha = 0.00415799815489004;
	const double defeat_alpha = 0.02061781866875989;
	const double charge_alpha = 0.00138792482909171;
	const double release_alpha = 0.00005135243496113;
	const double idle_alpha = 0.00003255155352044;
	const double hold_threshold = 3.8 / 5.5;
	double open_power = (double)open_level * open_level;
	double limit = (double)open_level + (squelched ? 0.0 : hysteresis);
	double limit_power = limit * limit;
	double strong_power, direct_power;

	/* Settle once, without treating a zero-filled FIR history as a carrier.
	 * Keep this running average independent of caller buffer partitioning. */
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

	/* Preserve the calibrated opening threshold. Interpolate the direct
	 * comparator between the manual's 2.8 V charging and 5.0 V defeat
	 * points; its 3.8 V position is 5/11 of that interval. This maps the
	 * circuit topology onto our noise-power units, not literal voltages. */
	direct_power = strong_power < limit_power
			       ? limit_power + (strong_power - limit_power) * (5.0 / 11.0)
			       : limit_power;
	if (state->noise_power <= strong_power) {
		state->hold_charge -= defeat_alpha * state->hold_charge;
	} else if (state->noise_power < limit_power) {
		state->hold_charge += charge_alpha * (1.0 - state->hold_charge);
	} else {
		state->hold_charge -= release_alpha * state->hold_charge;
	}
	/* Inaudible residual state must not enter slow subnormal arithmetic
	 * during hours of idle or a fully quiet discriminator. */
	if (state->hold_charge < 1.0e-12)
		state->hold_charge = 0.0;
	if (state->noise_power < 1.0e-12)
		state->noise_power = 0.0;
	return !(state->noise_power < direct_power || state->hold_charge > hold_threshold);
}

#endif
