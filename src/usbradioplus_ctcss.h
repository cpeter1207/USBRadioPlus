/** @file
 * @brief Continuous-phase 48 kHz CTCSS generation and reference-level calibration.
 */

#ifndef USBRADIOPLUS_CTCSS_H
#define USBRADIOPLUS_CTCSS_H

#include <stddef.h>

/** Continuous oscillator phase retained across native CTCSS blocks. */
struct urp_ctcss_generator {
	/** Oscillator phase in radians, retained across blocks. */
	double phase;
};

/** @brief Map a requested CTCSS tone to its calibrated reference oscillator frequency.
 * @param frequency CTCSS frequency in Hz.
 * @return Calibrated oscillator frequency in Hz.
 */
double urp_ctcss_legacy_frequency(double frequency);
/** @brief Read the reference CTCSS peak for the selected detector-filter calibration.
 * @param frequency CTCSS frequency in Hz.
 * @param filter_250 Nonzero selects the 250 Hz reference; zero selects the 215 Hz reference.
 * @return Reference peak amplitude in signed PCM codes.
 */
double urp_ctcss_legacy_peak(double frequency, int filter_250);
/** @brief Calculate CTCSS peak after the reference Q8 tone and output gains.
 * @param frequency CTCSS frequency in Hz.
 * @param filter_250 Nonzero selects the 250 Hz reference; zero selects the 215 Hz reference.
 * @param tone_gain_q8 Tone amplitude multiplier with eight fractional bits.
 * @param output_gain_q8 Output multiplier with eight fractional bits.
 * @return Maximum absolute reference sample level in PCM codes.
 */
double urp_ctcss_legacy_scaled_peak(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8);
/** @brief Calculate reference CTCSS amplitude and DC bias after Q8 gain scaling.
 * @param frequency CTCSS frequency in Hz.
 * @param filter_250 Nonzero selects the 250 Hz reference; zero selects the 215 Hz reference.
 * @param tone_gain_q8 Tone amplitude multiplier with eight fractional bits.
 * @param output_gain_q8 Output multiplier with eight fractional bits.
 * @param amplitude Receives the oscillator amplitude in PCM codes.
 * @param bias Receives the reference DC bias in PCM codes.
 */
void urp_ctcss_legacy_scaled_levels(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8, double *amplitude, double *bias);
/** @brief Render a phase-continuous CTCSS block at 48 kHz, including optional reverse burst.
 * @param generator Persistent oscillator phase state.
 * @param output Destination sample buffer owned by the caller.
 * @param count Number of elements available in the supplied block.
 * @param frequency CTCSS frequency in Hz.
 * @param peak Absolute sample peak in PCM codes.
 * @param enabled Nonzero enables the operation.
 * @param phase_reverse Nonzero applies the configured reverse-burst phase shift.
 */
void urp_ctcss_generate(struct urp_ctcss_generator *generator, double *output, size_t count,
			double frequency, double peak, int enabled, int phase_reverse);

#endif
