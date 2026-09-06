/** @file
 * @brief Exercise the gain rider through the same FFmpeg graph used by the channel.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/log.h>

#include "../src/txagc/avfilter_processor.h"

/** @brief Construct valid standalone AGC settings with all other stages bypassed.
 * @return Configuration matching the shipped gain-rider controls.
 */
static struct txagc_config configuration(void)
{
	struct txagc_config cfg = {0};
	cfg.stage_count = 1;
	cfg.stage_order[0] = TXAGC_STAGE_AGC;
	cfg.agc_enabled = 1;
	cfg.target_dbfs = -24.0;
	cfg.max_gain_db = 6.0;
	cfg.max_attenuation_db = 6.0;
	cfg.agc_rms_averaging_ms = 200.0;
	cfg.agc_gain_increase_db_per_second = 2.0;
	cfg.agc_gain_decrease_db_per_second = 6.0;
	cfg.agc_activity_threshold_dbfs = -50.0;
	cfg.agc_activity_hysteresis_db = 3.0;
	cfg.agc_hold_ms = 500.0;
	cfg.agc_deadband_db = 1.0;
	cfg.sidechain_highpass_hz = 800.0;
	cfg.sidechain_lowpass_hz = 1500.0;
	return cfg;
}

/** @brief Pass a stream through a persistent graph in host-selected partitions.
 * @param cfg Graph configuration.
 * @param samples PCM-code samples, processed in place.
 * @param count Total sample count.
 * @param rate Sample rate in Hz.
 * @param block Maximum samples per host callback.
 */
static void process(const struct txagc_config *cfg, double *samples, size_t count,
		    unsigned int rate, size_t block)
{
	struct txagc_avfilter graph;
	txagc_avfilter_init(&graph);
	for (size_t offset = 0; offset < count; offset += block) {
		size_t size = count - offset < block ? count - offset : block;
		assert(txagc_avfilter_process(&graph, cfg, samples + offset, size, rate) == 0);
	}
	/* An AGC-only graph must return every sample immediately, including startup. */
	assert(graph.underrun_samples == 0);
	txagc_avfilter_destroy(&graph);
}

/** @brief Build a sine with an explicitly specified RMS level.
 * @param samples Destination samples.
 * @param count Number of samples.
 * @param rate Sample rate in Hz.
 * @param frequency Sine frequency in Hz.
 * @param rms_dbfs RMS relative to a full-scale constant, not full-scale sine.
 */
static void tone(double *samples, size_t count, unsigned int rate, double frequency,
		 double rms_dbfs)
{
	double amplitude = 32768.0 * sqrt(2.0) * pow(10.0, rms_dbfs / 20.0);
	for (size_t index = 0; index < count; ++index)
		samples[index] = amplitude * sin(2.0 * M_PI * frequency * (double)index / rate);
}

/** @brief Calculate normalized RMS level from a non-silent sample span.
 * @param samples Audio samples in PCM codes.
 * @param count Number of samples.
 * @return RMS in dBFS.
 */
static double rms(const double *samples, size_t count)
{
	double sum = 0.0;
	for (size_t index = 0; index < count; ++index)
		sum += samples[index] * samples[index];
	return 10.0 * log10(sum / (double)count / (32768.0 * 32768.0));
}

/** @brief Verify rate-independent leveling, immediate output and boost/cut limits.
 */
static void test_rates_and_limits(void)
{
	static const unsigned int rates[] = {8000, 16000, 48000};
	for (size_t index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index) {
		unsigned int rate = rates[index];
		size_t count = rate * 8U;
		double *samples = malloc(count * sizeof(*samples));
		struct txagc_config cfg = configuration();
		assert(samples);
		cfg.sidechain_highpass_hz = cfg.sidechain_lowpass_hz = 0.0;
		for (int loud = 0; loud < 2; ++loud) {
			double input = loud ? -12.0 : -36.0;
			double expected = loud ? -18.0 : -30.0;
			tone(samples, count, rate, 1000.0, input);
			process(&cfg, samples, count, rate, rate / 50U);
			double measured = rms(samples + count - rate, rate);
			printf("AGC %u Hz bounded %+.3f -> %+.3f dBFS RMS\n", rate, input,
			       measured);
			assert(fabs(measured - expected) < 0.02);
		}
		/* A reachable target settles inside its deadband, not at a peak ceiling. */
		tone(samples, count, rate, 1000.0, -21.0);
		process(&cfg, samples, count, rate, 37);
		assert(fabs(rms(samples + count - rate, rate) - cfg.target_dbfs) <= 1.05);
		free(samples);
	}
}

/** @brief Detector filters must neither filter program audio nor introduce a limiter.
 */
static void test_detector_isolation(void)
{
	struct txagc_config cfg = configuration();
	double samples[1600];
	double original[1600];
	cfg.max_gain_db = cfg.max_attenuation_db = 0.0;
	for (int highpass = 0; highpass < 2; ++highpass) {
		for (int lowpass = 0; lowpass < 2; ++lowpass) {
			cfg.sidechain_highpass_hz = highpass ? 800.0 : 0.0;
			cfg.sidechain_lowpass_hz = lowpass ? 1500.0 : 0.0;
			for (size_t index = 0; index < 1600; ++index) {
				double time = (double)index / 8000.0;
				original[index] = 40000.0 +
						  5000.0 * sin(2.0 * M_PI * 200.0 * time) +
						  3000.0 * sin(2.0 * M_PI * 3000.0 * time);
			}
			memcpy(samples, original, sizeof(samples));
			process(&cfg, samples, 1600, 8000, 160);
			for (size_t index = 0; index < 1600; ++index)
				assert(fabs(samples[index] - original[index]) < 0.01);
		}
	}
}

/** @brief Each sidechain edge must affect measurement while both zero means bypass.
 */
static void test_detector_response(void)
{
	struct txagc_config cfg = configuration();
	size_t count = 64000;
	double *samples = malloc(count * sizeof(*samples));
	assert(samples);
	for (int highpass = 0; highpass < 2; ++highpass) {
		double frequency = highpass ? 200.0 : 3000.0;
		cfg.sidechain_highpass_hz = cfg.sidechain_lowpass_hz = 0.0;
		tone(samples, count, 8000, frequency, -30.0);
		process(&cfg, samples, count, 8000, 160);
		double bypass = rms(samples + count - 8000, 8000);
		cfg.sidechain_highpass_hz = highpass ? 800.0 : 0.0;
		cfg.sidechain_lowpass_hz = highpass ? 0.0 : 1500.0;
		tone(samples, count, 8000, frequency, -30.0);
		process(&cfg, samples, count, 8000, 160);
		double filtered = rms(samples + count - 8000, 8000);
		/* Attenuated detector falls below activity threshold: gain holds at unity. */
		assert(filtered < bypass - 3.0);
	}
	free(samples);
}

/** @brief Host frame boundaries cannot reset smoothing, hold, or the gain ramp.
 */
static void test_partitions(void)
{
	struct txagc_config cfg = configuration();
	const size_t count = 24000;
	double *short_blocks = malloc(count * sizeof(*short_blocks));
	double *long_blocks = malloc(count * sizeof(*long_blocks));
	assert(short_blocks && long_blocks);
	for (size_t index = 0; index < count; ++index) {
		double amplitude = index < 8000 ? 500.0 : index < 16000 ? 15000.0 : 0.1;
		short_blocks[index] = amplitude * sin(2.0 * M_PI * 1000.0 * (double)index / 8000.0);
	}
	memcpy(long_blocks, short_blocks, count * sizeof(*long_blocks));
	process(&cfg, short_blocks, count, 8000, 7);
	process(&cfg, long_blocks, count, 8000, 160);
	for (size_t index = 0; index < count; ++index)
		assert(fabs(short_blocks[index] - long_blocks[index]) < 0.01);
	free(short_blocks);
	free(long_blocks);
}

/** @brief Report whole-graph CPU cost without a machine-dependent pass threshold.
 */
static void benchmark(void)
{
	const size_t count = 48000U * 10U;
	double *samples = malloc(count * sizeof(*samples));
	struct txagc_config cfg = configuration();
	assert(samples);
	tone(samples, count, 48000, 1000.0, -27.0);
	clock_t start = clock();
	process(&cfg, samples, count, 48000, 960);
	double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
	printf("AGC graph 48 kHz: %.4f CPU seconds / 10 seconds audio\n", elapsed);
	free(samples);
}

/** @brief Execute graph-level regressions and a non-gating performance sample.
 * @return Zero after all assertions pass.
 */
int main(void)
{
	av_log_set_level(AV_LOG_ERROR);
	test_rates_and_limits();
	test_detector_isolation();
	test_detector_response();
	test_partitions();
	benchmark();
	puts("FFmpeg gated RMS gain rider tests passed");
	return 0;
}
