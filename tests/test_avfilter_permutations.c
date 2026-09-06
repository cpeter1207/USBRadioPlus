/** @file
 * @brief Executable avfilter permutations regression and failure-path checks.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/log.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000U

#define BLOCK 960U

#define PERMUTATIONS 720U

#define BLOCKS_PER_PERMUTATION 2U

#define PERFORMANCE_BLOCKS 200U

/** @brief Create the complete optional-stage configuration used by permutation tests.
 * @param cfg Candidate configuration; the caller retains ownership.
 */
static void configure(struct txagc_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->stage_count = TXAGC_MAX_DYNAMICS_STAGES;
	cfg->equalizer_enabled = 1;
	cfg->equalizer_low_gain_db = 2.0;
	cfg->equalizer_low_frequency_hz = 500.0;
	cfg->equalizer_low_slope = 0.7;
	cfg->equalizer_mid_gain_db = -0.5;
	cfg->equalizer_mid_frequency_hz = 1000.0;
	cfg->equalizer_mid_width_octaves = 1.0;
	cfg->equalizer_high_gain_db = -1.0;
	cfg->equalizer_high_frequency_hz = 2000.0;
	cfg->equalizer_high_slope = 0.7;
	cfg->deesser_enabled = 1;
	cfg->deesser_frequency_hz = 4000.0;
	cfg->deesser_width_octaves = 1.0;
	cfg->deesser_threshold_dbfs = -18.0;
	cfg->deesser_ratio = 3.0;
	cfg->deesser_max_reduction_db = 4.0;
	cfg->deesser_attack_ms = 2.0;
	cfg->deesser_release_ms = 60.0;
	cfg->ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	cfg->ctcss_notch_width_hz = 5.0;
	strcpy(cfg->ctcss_notch_frequencies, "254.1");
	cfg->agc_enabled = cfg->expander_enabled = 1;
	cfg->compressor_enabled = cfg->limiter_enabled = 1;
	cfg->target_dbfs = -12.0;
	cfg->max_gain_db = 6.0;
	cfg->max_attenuation_db = 6.0;
	cfg->agc_rms_averaging_ms = 200.0;
	cfg->agc_gain_increase_db_per_second = 2.0;
	cfg->agc_gain_decrease_db_per_second = 6.0;
	cfg->agc_activity_threshold_dbfs = -60.0;
	cfg->agc_activity_hysteresis_db = 3.0;
	cfg->agc_hold_ms = 500.0;
	cfg->agc_deadband_db = 1.0;
	cfg->expander_threshold_dbfs = -55.0;
	cfg->expander_ratio = 1.5;
	cfg->expander_max_attenuation_db = 9.0;
	cfg->expander_attack_ms = 10.0;
	cfg->expander_release_ms = 250.0;
	cfg->compressor_threshold_dbfs = -8.0;
	cfg->compressor_ratio = 2.0;
	cfg->compressor_attack_ms = 40.0;
	cfg->compressor_release_ms = 300.0;
	cfg->limiter_low_crossover_hz = 500.0;
	cfg->limiter_high_crossover_hz = 2000.0;
	cfg->low_limiter_threshold_dbfs = -2.0;
	cfg->low_limiter_ratio = 10.0;
	cfg->low_limiter_knee_db = 6.0;
	cfg->low_limiter_attack_ms = 50.0;
	cfg->low_limiter_release_ms = 250.0;
	cfg->mid_limiter_threshold_dbfs = -2.0;
	cfg->mid_limiter_ratio = 10.0;
	cfg->mid_limiter_knee_db = 6.0;
	cfg->mid_limiter_attack_ms = 10.0;
	cfg->mid_limiter_release_ms = 100.0;
	cfg->high_limiter_threshold_dbfs = -2.0;
	cfg->high_limiter_ratio = 20.0;
	cfg->high_limiter_knee_db = 6.0;
	cfg->high_limiter_attack_ms = 0.5;
	cfg->high_limiter_release_ms = 25.0;
	cfg->sidechain_highpass_hz = cfg->expander_sidechain_highpass_hz = 300.0;
	cfg->compressor_sidechain_highpass_hz = 300.0;
	cfg->sidechain_lowpass_hz = cfg->expander_sidechain_lowpass_hz = 3000.0;
	cfg->compressor_sidechain_lowpass_hz = 3000.0;
}

/** @brief Process audio through one ordering and record correctness and execution time.
 * @param order Stage-order text or permutation array.
 * @param number Receives or supplies the permutation sequence number.
 * @param block_count Audio blocks processed for this permutation.
 * @param total_seconds Accumulates measured processing time in seconds.
 * @return Result used by the test's assertions.
 */
static int run_permutation(const enum txagc_stage *order, unsigned int number,
			   unsigned int block_count, double *total_seconds)
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
	for (block = 0; block < block_count; ++block) {
		for (index = 0; index < BLOCK; ++index) {
			double time = (double)(block * BLOCK + index) / RATE;
			samples[index] = 6000.0 * sin(2.0 * M_PI * 900.0 * time);
		}
		if (txagc_avfilter_process(&state, &cfg, samples, BLOCK, RATE) < 0) {
			fprintf(stderr, "permutation %u failed at block %u\n", number, block);
			txagc_avfilter_destroy(&state);
			return -1;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	*total_seconds += end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) / 1000000000.0;
	txagc_avfilter_destroy(&state);
	return 0;
}

/** @brief Enumerate every stage ordering and execute its audio assertions.
 * @param order Stage-order text or permutation array.
 * @param at Current permutation recursion depth.
 * @param number Receives or supplies the permutation sequence number.
 * @param seconds Accumulates total processing time in seconds.
 * @return Result used by the test's assertions.
 */
static int permute(enum txagc_stage *order, unsigned int at, unsigned int *number, double *seconds)
{
	unsigned int index;
	if (at == TXAGC_MAX_DYNAMICS_STAGES) {
		return run_permutation(order, (*number)++, BLOCKS_PER_PERMUTATION, seconds);
	}
	for (index = at; index < TXAGC_MAX_DYNAMICS_STAGES; ++index) {
		enum txagc_stage swap = order[at];
		order[at] = order[index];
		order[index] = swap;
		if (permute(order, at + 1, number, seconds))
			return -1;
		swap = order[at];
		order[at] = order[index];
		order[index] = swap;
	}
	return 0;
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	enum txagc_stage order[TXAGC_MAX_DYNAMICS_STAGES] = {
		TXAGC_STAGE_EXPANDER, TXAGC_STAGE_AGC,	     TXAGC_STAGE_COMPRESSOR,
		TXAGC_STAGE_LIMITER,  TXAGC_STAGE_EQUALIZER, TXAGC_STAGE_DEESSER,
	};
	unsigned int number = 0;
	double seconds = 0.0;
	double milliseconds;
	const char *performance_limit_text;
	double performance_limit = 20.0;
	char *performance_limit_end;
	double configured_limit;

	performance_limit_text = getenv("USBRADIOPLUS_TEST_BLOCK_LIMIT_MS");
	if (performance_limit_text) {
		configured_limit = strtod(performance_limit_text, &performance_limit_end);
		if (*performance_limit_end || configured_limit <= 0.0)
			return 1;
		performance_limit = configured_limit;
	}
	av_log_set_level(AV_LOG_ERROR);
	if (permute(order, 0, &number, &seconds))
		return 1;
	if (number != PERMUTATIONS)
		return 1;
	seconds = 0.0;
	if (run_permutation(order, number, PERFORMANCE_BLOCKS, &seconds))
		return 1;
	milliseconds = seconds * 1000.0 / PERFORMANCE_BLOCKS;
	printf("%u permutations, representative average %.3f ms per 20 ms block\n", number,
	       milliseconds);
	if (milliseconds >= performance_limit) {
		fprintf(stderr, "processing did not maintain real-time average\n");
		return 1;
	}
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
/** @def PERMUTATIONS
 * @brief PERMUTATIONS selection for this isolated test harness.
 */
/** @def BLOCKS_PER_PERMUTATION
 * @brief BLOCKS PER PERMUTATION selection for this isolated test harness.
 */
/** @def PERFORMANCE_BLOCKS
 * @brief PERFORMANCE BLOCKS selection for this isolated test harness.
 */
