/** @file
 * @brief Continuous-phase 48 kHz CTCSS generation.
 */

#ifndef USBRADIOPLUS_CTCSS_H
#define USBRADIOPLUS_CTCSS_H

#include <stddef.h>

/** Continuous oscillator phase retained across native CTCSS blocks. */
struct urp_ctcss_generator {
	/** Oscillator phase in radians, retained across blocks. */
	double phase;
};

/** @brief Check whether a frequency is one of the supported CTCSS tones.
 * @param frequency CTCSS frequency in Hz.
 * @return Nonzero when the frequency has an exact radio-signaling table entry.
 */
int urp_ctcss_frequency_supported(float frequency);

/** @brief Map a requested CTCSS tone to its compatibility oscillator frequency.
 * @param frequency CTCSS frequency in Hz.
 * @return Compatibility oscillator frequency in Hz.
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
/** @brief Render a phase-continuous CTCSS block at 48 kHz, including a turn-off phase shift.
 * @param generator Persistent oscillator phase state.
 * @param output Destination sample buffer owned by the caller.
 * @param count Number of elements available in the supplied block.
 * @param frequency CTCSS frequency in Hz.
 * @param peak Absolute sample peak in PCM codes.
 * @param enabled Nonzero enables the operation.
 * @param phase_shift_degrees Turn-off phase shift in degrees; zero preserves oscillator phase.
 */
void urp_ctcss_generate(struct urp_ctcss_generator *generator, double *output, size_t count,
			double frequency, double peak, int enabled, double phase_shift_degrees);
/** @brief Render an exact-frequency tail tone at the native sample rate.
 * @param generator Persistent oscillator phase state.
 * @param output Destination sample buffer owned by the caller.
 * @param count Number of elements available in the supplied block.
 * @param frequency Tail-tone frequency in Hz.
 * @param peak Absolute sample peak in PCM codes.
 * @param enabled Nonzero enables the operation.
 */
void urp_ctcss_generate_tail_tone(struct urp_ctcss_generator *generator, double *output,
				  size_t count, double frequency, double peak, int enabled);

#endif
