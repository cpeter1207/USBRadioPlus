#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000
#define BLOCK 48

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
	config.agc_floor_dbfs = -55.0;
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
	config.limiter_crossover_hz = 1000.0;
	config.low_limiter_threshold_dbfs = -1.5;
	config.low_limiter_ratio = 10.0;
	config.low_limiter_knee_db = 6.0;
	config.low_limiter_attack_ms = 50.0;
	config.low_limiter_release_ms = 250.0;
	config.high_clip_dbfs = -1.5;
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
			double t = (double) (block * BLOCK + index) / RATE;
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
	printf("quiet_peak=%.6f loud_peak=%.6f max_peak=%.6f underrun=%llu\n",
		quiet_sum / 400.0, loud_sum / 500.0, maximum,
		state.underrun_samples);
	if (maximum > 32768.0 * pow(10.0, -3.0 / 20.0) * 1.001) {
		fprintf(stderr, "limiter ceiling exceeded\n");
		return 2;
	}
	if (quiet_sum / 400.0 <= 0.03 * 32768.0) {
		fprintf(stderr, "AGC did not raise quiet audio\n");
		return 3;
	}
	txagc_avfilter_destroy(&state);
	return 0;
}
