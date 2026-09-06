/** @file
 * @brief Executable avfilter processor regression and failure-path checks.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000

#define BLOCK 48

/** @brief Calculate the maximum absolute sample value for an audio assertion.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @return Measured level or response used by the caller's numerical assertions.
 */
static double peak(const double *samples, size_t count)
{
	double value = 0.0;
	for (size_t index = 0; index < count; ++index) {
		double sample = fabs(samples[index]);
		if (sample > value) {
			value = sample;
		}
	}
	return value;
}

/** @brief Measure a steady input through the same live graph at the IAX audio rate.
 * @param state Persistent graph, retained across stage-enable changes.
 * @param config Current optional-stage configuration.
 * @param frequency Test-tone frequency in Hz.
 * @return Output RMS in PCM codes, or NaN when graph processing fails.
 */
static double measure_link_graph(struct txagc_avfilter *state, const struct txagc_config *config,
				 double frequency)
{
	double samples[160];
	double sum = 0.0;

	for (unsigned int block = 0; block < 400; ++block) {
		for (unsigned int index = 0; index < 160; ++index) {
			double time = (double)(block * 160 + index) / 8000;
			samples[index] = 12000.0 * sin(2.0 * M_PI * frequency * time);
		}
		if (txagc_avfilter_process(state, config, samples, 160, 8000) < 0)
			return NAN;
		if (block >= 300)
			for (unsigned int index = 0; index < 160; ++index)
				sum += samples[index] * samples[index];
	}
	return sqrt(sum / 16000);
}

/** @brief Toggle every link stage on an existing 8 kHz graph and verify actual audio changes.
 * @param defaults Valid parameters used by the main dynamics regression.
 * @return Zero if each stage changes the audio and disabling it restores bypass.
 */
static int test_link_stage_toggles(const struct txagc_config *defaults)
{
	static const size_t flag_offsets[] = {
		offsetof(struct txagc_config, expander_enabled),
		offsetof(struct txagc_config, agc_enabled),
		offsetof(struct txagc_config, compressor_enabled),
		offsetof(struct txagc_config, limiter_enabled),
		offsetof(struct txagc_config, equalizer_enabled),
		offsetof(struct txagc_config, deesser_enabled),
	};
	struct txagc_config config = *defaults;
	struct txagc_avfilter state;

	config.lookahead_limiter_enabled = 0;
	config.stage_count = 1;
	for (size_t index = 0; index < sizeof(flag_offsets) / sizeof(flag_offsets[0]); ++index)
		*(int *)((char *)&config + flag_offsets[index]) = 0;
	/* Put each detector well beyond its threshold so an enabled stage must be audible. */
	config.target_dbfs = -24.0;
	config.max_attenuation_db = 18.0;
	config.expander_threshold_dbfs = -6.0;
	config.expander_ratio = 4.0;
	config.compressor_threshold_dbfs = -24.0;
	config.compressor_ratio = 4.0;
	config.low_limiter_threshold_dbfs = -24.0;
	config.mid_limiter_threshold_dbfs = -24.0;
	config.high_limiter_threshold_dbfs = -24.0;
	config.equalizer_low_gain_db = -6.0;
	config.equalizer_low_frequency_hz = 300.0;
	config.equalizer_low_slope = 0.7;
	config.equalizer_mid_gain_db = -6.0;
	config.equalizer_mid_frequency_hz = 1000.0;
	config.equalizer_mid_width_octaves = 1.0;
	config.equalizer_high_gain_db = -6.0;
	config.equalizer_high_frequency_hz = 2500.0;
	config.equalizer_high_slope = 0.7;
	config.deesser_frequency_hz = 3000.0;
	config.deesser_width_octaves = 1.0;
	config.deesser_threshold_dbfs = -30.0;
	config.deesser_ratio = 4.0;
	config.deesser_max_reduction_db = 6.0;
	config.deesser_attack_ms = 2.0;
	config.deesser_release_ms = 60.0;
	txagc_avfilter_init(&state);
	for (size_t index = 0; index < sizeof(flag_offsets) / sizeof(flag_offsets[0]); ++index) {
		int *enabled = (int *)((char *)&config + flag_offsets[index]);
		double frequency = index == TXAGC_STAGE_DEESSER ? 3000.0 : 1000.0;
		double bypass;
		double processed;
		double restored;
		config.stage_order[0] = (enum txagc_stage)index;
		bypass = measure_link_graph(&state, &config, frequency);
		*enabled = 1;
		processed = measure_link_graph(&state, &config, frequency);
		*enabled = 0;
		restored = measure_link_graph(&state, &config, frequency);
		printf("8 kHz link stage %zu: change=%+.2f dB bypass_error=%.6f\n", index,
		       20.0 * log10(processed / bypass), fabs(restored - bypass));
		if (!isfinite(bypass) || !isfinite(processed) || !isfinite(restored) ||
		    fabs(restored - bypass) > 0.01 || processed >= bypass * 0.89) {
			txagc_avfilter_destroy(&state);
			return -1;
		}
	}
	txagc_avfilter_destroy(&state);
	return 0;
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double maximum = 0.0;
	double quiet_sum = 0.0;
	double loud_sum = 0.0;

	memset(&config, 0, sizeof(config));
	config.stage_count = 4;
	config.stage_order[0] = TXAGC_STAGE_EXPANDER;
	config.stage_order[1] = TXAGC_STAGE_AGC;
	config.stage_order[2] = TXAGC_STAGE_COMPRESSOR;
	config.stage_order[3] = TXAGC_STAGE_LIMITER;
	config.agc_enabled = 1;
	config.target_dbfs = -16.0;
	config.max_gain_db = 12.0;
	config.max_attenuation_db = 6.0;
	config.agc_rms_averaging_ms = 200.0;
	config.agc_gain_increase_db_per_second = 2.0;
	config.agc_gain_decrease_db_per_second = 6.0;
	config.agc_activity_threshold_dbfs = -55.0;
	config.agc_activity_hysteresis_db = 3.0;
	config.agc_hold_ms = 500.0;
	config.agc_deadband_db = 1.0;
	config.expander_enabled = 1;
	config.expander_threshold_dbfs = -55.0;
	config.expander_ratio = 1.5;
	config.expander_max_attenuation_db = 9.0;
	config.expander_attack_ms = 10.0;
	config.expander_release_ms = 250.0;
	config.expander_sidechain_highpass_hz = 800.0;
	config.expander_sidechain_lowpass_hz = 1500.0;
	config.compressor_enabled = 1;
	config.compressor_threshold_dbfs = -8.0;
	config.compressor_ratio = 2.0;
	config.compressor_attack_ms = 40.0;
	config.compressor_release_ms = 300.0;
	config.compressor_sidechain_highpass_hz = 800.0;
	config.compressor_sidechain_lowpass_hz = 1500.0;
	config.limiter_enabled = 1;
	config.limiter_low_crossover_hz = 500.0;
	config.limiter_high_crossover_hz = 2000.0;
	config.low_limiter_threshold_dbfs = -1.5;
	config.low_limiter_ratio = 10.0;
	config.low_limiter_knee_db = 6.0;
	config.low_limiter_attack_ms = 50.0;
	config.low_limiter_release_ms = 250.0;
	config.mid_limiter_threshold_dbfs = -1.5;
	config.mid_limiter_ratio = 10.0;
	config.mid_limiter_knee_db = 6.0;
	config.mid_limiter_attack_ms = 10.0;
	config.mid_limiter_release_ms = 100.0;
	config.high_limiter_threshold_dbfs = -1.5;
	config.high_limiter_ratio = 20.0;
	config.high_limiter_knee_db = 6.0;
	config.high_limiter_attack_ms = 0.5;
	config.high_limiter_release_ms = 25.0;
	config.lookahead_limiter_enabled = 1;
	config.lookahead_limit_dbfs = -3.0;
	config.lookahead_ms = 5.0;
	config.lookahead_release_ms = 100.0;
	txagc_avfilter_init(&state);

	for (int block = 0; block < 3000; ++block) {
		double amplitude = (block < 1500 ? 0.03 : 0.9) * 32768.0;
		for (int index = 0; index < BLOCK; ++index) {
			double t = (double)(block * BLOCK + index) / RATE;
			samples[index] = amplitude * sin(2.0 * M_PI * 1000.0 * t);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0) {
			fprintf(stderr, "processing failed\n");
			return 1;
		}
		double value = peak(samples, BLOCK);
		if (value > maximum) {
			maximum = value;
		}
		if (block >= 1000 && block < 1400) {
			quiet_sum += value;
		}
		if (block >= 2500) {
			loud_sum += value;
		}
	}
	printf("quiet_peak=%.6f loud_peak=%.6f max_peak=%.6f underrun=%llu\n", quiet_sum / 400.0,
	       loud_sum / 500.0, maximum, state.underrun_samples);
	if (maximum > 32768.0 * pow(10.0, -3.0 / 20.0) * 1.001) {
		fprintf(stderr, "limiter ceiling exceeded\n");
		return 2;
	}
	if (quiet_sum / 400.0 <= 0.03 * 32768.0) {
		fprintf(stderr, "AGC did not raise quiet audio\n");
		return 3;
	}
	txagc_avfilter_destroy(&state);
	if (test_link_stage_toggles(&config))
		return 4;
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
