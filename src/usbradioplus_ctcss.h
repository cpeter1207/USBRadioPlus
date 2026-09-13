/** @file
 * @brief CTCSS configuration validation and legacy calibration helpers.
 *
 * The declarations preserve the USBRadioPlus compatibility surface while the
 * implementation forwards to the released portable Rust radio core.
 */

#ifndef USBRADIOPLUS_CTCSS_H
#define USBRADIOPLUS_CTCSS_H

/** @brief CTCSS phase mirror published by the portable radio core. */
struct urp_ctcss_phase_state {
	/** Current portable-core oscillator phase in radians. */
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

#endif
