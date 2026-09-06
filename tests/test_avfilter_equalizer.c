/** @file
 * @brief Executable avfilter equalizer regression and failure-path checks.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000

#define BLOCK 960

/** @brief Measure the filter response at the requested test frequency.
 * @param frequency CTCSS frequency in Hz.
 * @param enabled Nonzero enables the operation.
 * @return Measured level or response used by the caller's numerical assertions.
 */
static double measure(double frequency, int enabled)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	memset(&config, 0, sizeof(config));
	config.stage_count = 1;
	config.stage_order[0] = TXAGC_STAGE_EQUALIZER;
	config.equalizer_enabled = enabled;
	config.equalizer_low_gain_db = 2.0;
	config.equalizer_low_frequency_hz = 300.0;
	config.equalizer_low_slope = 0.7;
	config.equalizer_mid_gain_db = -0.5;
	config.equalizer_mid_frequency_hz = 750.0;
	config.equalizer_mid_width_octaves = 1.0;
	config.equalizer_high_gain_db = -1.0;
	config.equalizer_high_frequency_hz = 2500.0;
	config.equalizer_high_slope = 0.7;
	txagc_avfilter_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (int index = 0; index < BLOCK; ++index) {
			double time = (double)(block * BLOCK + index) / RATE;
			samples[index] = 1000.0 * sin(2.0 * M_PI * frequency * time);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0)
			return NAN;
		if (block >= 50)
			for (int index = 0; index < BLOCK; ++index) {
				sum += samples[index] * samples[index];
				++count;
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
	double bypass = measure(1000.0, 0);
	double low = measure(150.0, 1);
	double mid = measure(750.0, 1);
	double high = measure(4000.0, 1);
	double low_db = 20.0 * log10(low / bypass);
	double mid_db = 20.0 * log10(mid / bypass);
	double high_db = 20.0 * log10(high / bypass);

	printf("equalizer 150Hz=%+.2f 750Hz=%+.2f 4000Hz=%+.2f dB\n", low_db, mid_db, high_db);
	if (!isfinite(low_db) || !isfinite(mid_db) || !isfinite(high_db))
		return 1;
	if (low_db < 1.0 || mid_db > 0.25 || high_db > -0.5)
		return 2;
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
