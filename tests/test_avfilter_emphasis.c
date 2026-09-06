/** @file
 * @brief Executable avfilter emphasis regression and failure-path checks.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000

#define BLOCK 960

/** @brief Calculate sample RMS for an audio-result assertion.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @return Measured level or response used by the caller's numerical assertions.
 */
static double rms(const double *samples, size_t count)
{
	double sum = 0.0;
	for (size_t index = 0; index < count; ++index) {
		sum += samples[index] * samples[index];
	}
	return sqrt(sum / count);
}

/** @brief Measure normalized emphasis response for the selected frequency.
 * @param production Nonzero selects preemphasis; zero selects deemphasis.
 * @param frequency CTCSS frequency in Hz.
 * @return Measured level or response used by the caller's numerical assertions.
 */
static double response(int production, double frequency)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double output_rms = 0.0;
	memset(&config, 0, sizeof(config));
	config.preemphasis_enabled = production;
	config.deemphasis_enabled = !production;
	config.emphasis_corner_hz = 20.0;
	config.emphasis_reference_hz = 1000.0;
	txagc_avfilter_init(&state);
	for (int block = 0; block < 20; ++block) {
		for (int index = 0; index < BLOCK; ++index) {
			double t = (double)(block * BLOCK + index) / RATE;
			samples[index] = 1000.0 * sin(2.0 * M_PI * frequency * t);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0) {
			return NAN;
		}
		if (block == 19) {
			output_rms = rms(samples, BLOCK);
		}
	}
	txagc_avfilter_destroy(&state);
	return 20.0 * log10(output_rms / (1000.0 / sqrt(2.0)));
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	const double frequencies[] = {300.0, 600.0, 1000.0, 1200.0, 2400.0, 3000.0};
	double previous_pre = NAN;
	double previous_de = NAN;
	for (size_t index = 0; index < sizeof(frequencies) / sizeof(frequencies[0]); ++index) {
		double pre = response(1, frequencies[index]);
		double de = response(0, frequencies[index]);
		printf("%.0f Hz pre=%+.3f dB de=%+.3f dB sum=%+.3f dB\n", frequencies[index], pre,
		       de, pre + de);
		if (!isfinite(pre) || !isfinite(de) || fabs(pre + de) > 0.05) {
			return 1;
		}
		if (frequencies[index] == 1000.0 && (fabs(pre) > 0.05 || fabs(de) > 0.05)) {
			return 2;
		}
		if (isfinite(previous_pre) && frequencies[index] == 1200.0 &&
		    (fabs((pre - previous_pre) - 6.0) > 0.5 ||
		     fabs((de - previous_de) + 6.0) > 0.5)) {
			return 3;
		}
		if (frequencies[index] == 600.0) {
			previous_pre = pre;
			previous_de = de;
		}
	}
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
