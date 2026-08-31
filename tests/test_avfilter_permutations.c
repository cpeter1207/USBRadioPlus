#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000U
#define BLOCK 960U
#define PERMUTATIONS 24U
#define BLOCKS_PER_PERMUTATION 80U

static void configure(struct txagc_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->stage_count = TXAGC_MAX_DYNAMICS_STAGES;
	cfg->ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	cfg->ctcss_notch_width_hz = 5.0;
	strcpy(cfg->ctcss_notch_frequencies, "254.1");
	cfg->agc_enabled = cfg->expander_enabled = 1;
	cfg->compressor_enabled = cfg->limiter_enabled = 1;
	cfg->target_dbfs = -12.0;
	cfg->max_gain_db = 6.0;
	cfg->agc_floor_dbfs = -60.0;
	cfg->expander_threshold_dbfs = -55.0;
	cfg->expander_ratio = 1.5;
	cfg->expander_max_attenuation_db = 9.0;
	cfg->expander_attack_ms = 10.0;
	cfg->expander_release_ms = 250.0;
	cfg->compressor_threshold_dbfs = -8.0;
	cfg->compressor_ratio = 2.0;
	cfg->compressor_attack_ms = 40.0;
	cfg->compressor_release_ms = 300.0;
	cfg->limiter_crossover_hz = 1000.0;
	cfg->low_limiter_threshold_dbfs = -2.0;
	cfg->low_limiter_ratio = 10.0;
	cfg->low_limiter_knee_db = 6.0;
	cfg->low_limiter_attack_ms = 50.0;
	cfg->low_limiter_release_ms = 250.0;
	cfg->high_clip_dbfs = -2.0;
	cfg->high_limiter_ratio = 20.0;
	cfg->high_limiter_knee_db = 6.0;
	cfg->high_limiter_attack_ms = 0.5;
	cfg->high_limiter_release_ms = 25.0;
	cfg->sidechain_highpass_hz = cfg->expander_sidechain_highpass_hz = 300.0;
	cfg->compressor_sidechain_highpass_hz = 300.0;
	cfg->sidechain_lowpass_hz = cfg->expander_sidechain_lowpass_hz = 3000.0;
	cfg->compressor_sidechain_lowpass_hz = 3000.0;
}

static int run_permutation(const enum txagc_stage *order, unsigned int number,
	double *total_seconds)
{
	struct txagc_avfilter state;
	struct txagc_config cfg;
	double samples[BLOCK];
	struct timespec begin, end;
	unsigned int block, index;

	configure(&cfg);
	memcpy(cfg.stage_order, order, sizeof(cfg.stage_order));
	txagc_avfilter_init(&state);
	clock_gettime(CLOCK_MONOTONIC, &begin);
	for (block = 0; block < BLOCKS_PER_PERMUTATION; ++block) {
		for (index = 0; index < BLOCK; ++index) {
			double time = (double) (block * BLOCK + index) / RATE;
			samples[index] = 6000.0 * sin(2.0 * M_PI * 900.0 * time);
		}
		if (txagc_avfilter_process(&state, &cfg, samples, BLOCK, RATE) < 0) {
			fprintf(stderr, "permutation %u failed at block %u\n", number, block);
			txagc_avfilter_destroy(&state);
			return -1;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	*total_seconds += end.tv_sec - begin.tv_sec
		+ (end.tv_nsec - begin.tv_nsec) / 1000000000.0;
	txagc_avfilter_destroy(&state);
	return 0;
}

static int permute(enum txagc_stage *order, unsigned int at,
	unsigned int *number, double *seconds)
{
	unsigned int index;
	if (at == TXAGC_MAX_DYNAMICS_STAGES) {
		return run_permutation(order, (*number)++, seconds);
	}
	for (index = at; index < TXAGC_MAX_DYNAMICS_STAGES; ++index) {
		enum txagc_stage swap = order[at];
		order[at] = order[index];
		order[index] = swap;
		if (permute(order, at + 1, number, seconds)) return -1;
		swap = order[at];
		order[at] = order[index];
		order[index] = swap;
	}
	return 0;
}

int main(void)
{
	enum txagc_stage order[TXAGC_MAX_DYNAMICS_STAGES] = {
		TXAGC_STAGE_EXPANDER, TXAGC_STAGE_AGC,
		TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER,
	};
	unsigned int number = 0;
	double seconds = 0.0;
	double milliseconds;
	if (permute(order, 0, &number, &seconds)) return 1;
	milliseconds = seconds * 1000.0 / (number * BLOCKS_PER_PERMUTATION);
	printf("%u permutations, average %.3f ms per 20 ms block\n",
		number, milliseconds);
	if (number != PERMUTATIONS || milliseconds >= 20.0) {
		fprintf(stderr, "processing did not maintain real-time average\n");
		return 1;
	}
	return 0;
}
