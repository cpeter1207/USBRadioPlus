/** @file
 * @brief Executable avfilter bandpass regression and failure-path checks.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000

#define BLOCK 960

/** @brief Measure the filter response at the requested test frequency.
 * @param frequency CTCSS frequency in Hz.
 * @param receive Nonzero selects the receive band-pass; zero selects the transmitter band-pass.
 * @return Measured level or response used by the caller's numerical assertions.
 */
static double measure(double frequency, int receive)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	memset(&config, 0, sizeof(config));
	if (receive) {
		config.receive_bandpass_enabled = 1;
		config.receive_bandpass_highpass_hz = 300.0;
		config.receive_bandpass_lowpass_hz = 3000.0;
	} else {
		config.splatter_filter_enabled = 1;
		config.output_highpass_hz = 300.0;
		config.output_lowpass_hz = 3000.0;
	}
	txagc_avfilter_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (int i = 0; i < BLOCK; ++i) {
			double t = (double)(block * BLOCK + i) / RATE;
			samples[i] = 1000.0 * sin(2.0 * M_PI * frequency * t);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0) {
			txagc_avfilter_destroy(&state);
			return NAN;
		}
		if (block >= 50) {
			for (int i = 0; i < BLOCK; ++i) {
				sum += samples[i] * samples[i];
				++count;
			}
		}
	}
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	double low = measure(100.0, 0);
	double pass = measure(1000.0, 0);
	double high = measure(5000.0, 0);
	double receive_low = measure(100.0, 1);
	double receive_pass = measure(1000.0, 1);
	double receive_high = measure(5000.0, 1);
	double low_rejection = 20.0 * log10(pass / low);
	double high_rejection = 20.0 * log10(pass / high);

	printf("100Hz=%.3f 1000Hz=%.3f 5000Hz=%.3f rejection=%.1f/%.1f dB\n", low, pass, high,
	       low_rejection, high_rejection);
	if (!isfinite(pass) || pass < 650.0 || pass > 750.0)
		return 1;
	if (low_rejection < 60.0 || high_rejection < 60.0)
		return 2;
	if (fabs(receive_pass - pass) > 0.1 || fabs(receive_low - low) > 0.1 ||
	    fabs(receive_high - high) > 0.1)
		return 3;
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
