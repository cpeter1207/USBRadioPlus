/** @file
 * @brief Executable avfilter processor regression and failure-path checks.
 */

#include <assert.h>
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

/** @brief Build unity-gain dynamics with independent single- and three-band controls.
 * @param stage Compressor or limiter selected for the test.
 * @param bands One full band or three crossover bands.
 * @return Initialized graph configuration without other processing.
 */
static struct txagc_config dynamics_config(enum txagc_stage stage, int bands)
{
	struct txagc_config cfg = {0};
	cfg.stage_count = 1;
	cfg.stage_order[0] = stage;
	cfg.compressor_enabled = stage == TXAGC_STAGE_COMPRESSOR;
	cfg.limiter_enabled = stage == TXAGC_STAGE_LIMITER;
	cfg.compressor_bands = cfg.limiter_bands = bands;
	cfg.compressor_low_crossover_hz = cfg.limiter_low_crossover_hz = 500.0;
	cfg.compressor_high_crossover_hz = cfg.limiter_high_crossover_hz = 2000.0;
	cfg.compressor_threshold_dbfs = cfg.limiter_threshold_dbfs = -12.0;
	cfg.compressor_ratio = cfg.limiter_ratio = 1.0;
	cfg.compressor_attack_ms = cfg.limiter_attack_ms = 1.0;
	cfg.compressor_release_ms = cfg.limiter_release_ms = 10.0;
	cfg.compressor_low_ratio = cfg.compressor_mid_ratio = cfg.compressor_high_ratio = 1.0;
	cfg.low_limiter_ratio = cfg.mid_limiter_ratio = cfg.high_limiter_ratio = 1.0;
	cfg.compressor_low_attack_ms = cfg.compressor_mid_attack_ms =
		cfg.compressor_high_attack_ms = 1.0;
	cfg.low_limiter_attack_ms = cfg.mid_limiter_attack_ms = cfg.high_limiter_attack_ms = 1.0;
	cfg.compressor_low_release_ms = cfg.compressor_mid_release_ms =
		cfg.compressor_high_release_ms = 10.0;
	cfg.low_limiter_release_ms = cfg.mid_limiter_release_ms = cfg.high_limiter_release_ms =
		10.0;
	return cfg;
}

/** @brief Measure steady sine-wave amplitude through one unbuffered dynamics graph.
 * @param cfg Graph configuration to exercise.
 * @param rate Sample rate in Hz.
 * @param frequency Input sine-wave frequency in Hz.
 * @return Output/input RMS gain after crossover and detector settling.
 */
static double dynamics_response(const struct txagc_config *cfg, unsigned int rate, double frequency)
{
	struct txagc_avfilter state;
	double samples[960];
	double sum = 0.0;
	unsigned int count = rate / 50;

	txagc_avfilter_init(&state);
	for (unsigned int block = 0; block < 50; ++block) {
		for (unsigned int index = 0; index < count; ++index) {
			double time = (double)(block * count + index) / rate;
			samples[index] = 3276.8 * sin(2.0 * M_PI * frequency * time);
		}
		assert(!txagc_avfilter_process(&state, cfg, samples, count, rate));
		if (block >= 25)
			for (unsigned int index = 0; index < count; ++index)
				sum += samples[index] * samples[index];
	}
	assert(state.underrun_samples == 0);
	txagc_avfilter_destroy(&state);
	return sqrt(sum / (25 * count)) / (3276.8 / sqrt(2.0));
}

/** @brief Verify unity crossover recombination, arbitrary splits, and independent bands. */
static void test_band_dynamics_audio(void)
{
	static const unsigned int rates[] = {8000, 16000, 48000};
	static const double frequencies[] = {100.0, 500.0, 1000.0, 2000.0, 3500.0};
	static const double band_centers[] = {100.0, 1000.0, 3500.0};
	const enum txagc_stage stages[] = {TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER};

	for (unsigned int rate = 0; rate < 3; ++rate) {
		for (unsigned int stage = 0; stage < 2; ++stage) {
			for (int bands = 1; bands <= 3; bands += 2) {
				struct txagc_config cfg = dynamics_config(stages[stage], bands);
				for (unsigned int tone = 0; tone < 5; ++tone)
					assert(fabs(dynamics_response(&cfg, rates[rate],
								      frequencies[tone]) -
						    1.0) < 0.003);
				/* Crossovers are inactive in single-band mode. In three-band
				 * mode their phase changes must not change the summed level. */
				cfg.compressor_low_crossover_hz = cfg.limiter_low_crossover_hz =
					750.5;
				cfg.compressor_high_crossover_hz = cfg.limiter_high_crossover_hz =
					2500.25;
				assert(fabs(dynamics_response(&cfg, rates[rate], 1000.0) - 1.0) <
				       0.003);
			}
			for (unsigned int band = 0; band < 3; ++band) {
				struct txagc_config cfg = dynamics_config(stages[stage], 3);
				double *thresholds[] = {
					&cfg.compressor_low_threshold_dbfs,
					&cfg.compressor_mid_threshold_dbfs,
					&cfg.compressor_high_threshold_dbfs,
					&cfg.low_limiter_threshold_dbfs,
					&cfg.mid_limiter_threshold_dbfs,
					&cfg.high_limiter_threshold_dbfs,
				};
				cfg.compressor_low_ratio = cfg.compressor_mid_ratio =
					cfg.compressor_high_ratio = 20.0;
				cfg.low_limiter_ratio = cfg.mid_limiter_ratio =
					cfg.high_limiter_ratio = 20.0;
				*thresholds[stage * 3 + band] = -40.0;
				for (unsigned int tone = 0; tone < 3; ++tone) {
					double gain = dynamics_response(&cfg, rates[rate],
									band_centers[tone]);
					if (tone == band)
						assert(gain < 0.5);
					else
						assert(gain > 0.78);
				}
			}
		}
		for (unsigned int band = 0; band < 3; ++band) {
			struct txagc_config cfg = dynamics_config(TXAGC_STAGE_COMPRESSOR, 3);
			double *makeup[] = {&cfg.compressor_low_makeup_gain_db,
					    &cfg.compressor_mid_makeup_gain_db,
					    &cfg.compressor_high_makeup_gain_db};
			*makeup[band] = 6.0;
			assert(dynamics_response(&cfg, rates[rate], band_centers[band]) > 1.7);
			*makeup[band] = -30.0;
			assert(dynamics_response(&cfg, rates[rate], band_centers[band]) < 0.16);
		}
		{
			struct txagc_config cfg = dynamics_config(TXAGC_STAGE_COMPRESSOR, 1);
			cfg.compressor_makeup_gain_db = -30.0;
			assert(fabs(dynamics_response(&cfg, rates[rate], 1000.0) -
				    pow(10.0, -30.0 / 20.0)) < 0.0001);
		}
	}
}

/** @brief Measure attack or release after a large full-band input-level step.
 * @param stage Compressor or limiter to exercise.
 * @param rate Audio sample rate in Hz.
 * @param bands One full band or three crossover bands.
 * @param slow Nonzero selects a deliberately slow detector response.
 * @param release Nonzero measures recovery after the input becomes quiet.
 * @return RMS from the first 20 ms of attack or the 80–100 ms release interval.
 */
static double dynamics_step(enum txagc_stage stage, unsigned int rate, int bands, int slow,
			    int release)
{
	struct txagc_config cfg = dynamics_config(stage, bands);
	struct txagc_avfilter state;
	double samples[960];
	double sum = 0.0;
	unsigned int count = rate / 50;
	unsigned int blocks = release ? 105 : 1;

	cfg.compressor_threshold_dbfs = cfg.limiter_threshold_dbfs = -24.0;
	cfg.compressor_ratio = cfg.limiter_ratio = 20.0;
	cfg.compressor_attack_ms = cfg.limiter_attack_ms = slow && !release ? 1000.0 : 0.1;
	cfg.compressor_release_ms = cfg.limiter_release_ms = slow ? 1000.0 : 10.0;
	cfg.compressor_low_threshold_dbfs = cfg.compressor_mid_threshold_dbfs =
		cfg.compressor_high_threshold_dbfs = -24.0;
	cfg.low_limiter_threshold_dbfs = cfg.mid_limiter_threshold_dbfs =
		cfg.high_limiter_threshold_dbfs = -24.0;
	cfg.compressor_low_ratio = cfg.compressor_mid_ratio = cfg.compressor_high_ratio = 20.0;
	cfg.low_limiter_ratio = cfg.mid_limiter_ratio = cfg.high_limiter_ratio = 20.0;
	cfg.compressor_low_attack_ms = cfg.compressor_mid_attack_ms =
		cfg.compressor_high_attack_ms = cfg.compressor_attack_ms;
	cfg.low_limiter_attack_ms = cfg.mid_limiter_attack_ms = cfg.high_limiter_attack_ms =
		cfg.limiter_attack_ms;
	cfg.compressor_low_release_ms = cfg.compressor_mid_release_ms =
		cfg.compressor_high_release_ms = cfg.compressor_release_ms;
	cfg.low_limiter_release_ms = cfg.mid_limiter_release_ms = cfg.high_limiter_release_ms =
		cfg.limiter_release_ms;
	txagc_avfilter_init(&state);
	for (unsigned int block = 0; block < blocks; ++block) {
		double amplitude = block >= 100 ? 1000.0 : 16000.0;
		for (unsigned int index = 0; index < count; ++index) {
			double time = (double)(block * count + index) / rate;
			samples[index] = amplitude * sin(2.0 * M_PI * 1000.0 * time);
		}
		assert(!txagc_avfilter_process(&state, &cfg, samples, count, rate));
	}
	for (unsigned int index = 0; index < count; ++index)
		sum += samples[index] * samples[index];
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

/** @brief Both band modes honor independent attack and release at each sample rate. */
static void test_single_band_dynamics(void)
{
	const enum txagc_stage stages[] = {TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER};
	const unsigned int rates[] = {8000, 16000, 48000};
	for (unsigned int stage = 0; stage < 2; ++stage)
		for (unsigned int rate = 0; rate < 3; ++rate)
			for (int bands = 1; bands <= 3; bands += 2) {
				assert(dynamics_step(stages[stage], rates[rate], bands, 1, 0) >
				       dynamics_step(stages[stage], rates[rate], bands, 0, 0) *
					       1.5);
				assert(dynamics_step(stages[stage], rates[rate], bands, 0, 1) >
				       dynamics_step(stages[stage], rates[rate], bands, 1, 1) *
					       1.5);
			}
}

/** @brief Both three-band stages retain sample-exact state across arbitrary frame boundaries. */
static void test_multiband_frame_boundaries(void)
{
	const unsigned int rates[] = {8000, 16000, 48000};
	for (unsigned int rate = 0; rate < 3; ++rate) {
		struct txagc_config cfg = dynamics_config(TXAGC_STAGE_COMPRESSOR, 3);
		struct txagc_avfilter reference;
		struct txagc_avfilter fragmented;
		double expected[960];
		double actual[960];
		unsigned int count = rates[rate] / 50;
		cfg.stage_count = 2;
		cfg.stage_order[1] = TXAGC_STAGE_LIMITER;
		cfg.limiter_enabled = 1;
		cfg.compressor_low_threshold_dbfs = cfg.compressor_mid_threshold_dbfs =
			cfg.compressor_high_threshold_dbfs = -30.0;
		cfg.compressor_low_ratio = cfg.compressor_mid_ratio = cfg.compressor_high_ratio =
			2.0;
		cfg.low_limiter_threshold_dbfs = cfg.mid_limiter_threshold_dbfs =
			cfg.high_limiter_threshold_dbfs = -18.0;
		cfg.low_limiter_ratio = cfg.mid_limiter_ratio = cfg.high_limiter_ratio = 10.0;
		txagc_avfilter_init(&reference);
		txagc_avfilter_init(&fragmented);
		for (unsigned int block = 0; block < 32; ++block) {
			for (unsigned int index = 0; index < count; ++index) {
				double time = (double)(block * count + index) / rates[rate];
				expected[index] = 10000.0 * sin(2.0 * M_PI * 100.0 * time) +
						  6000.0 * sin(2.0 * M_PI * 1000.0 * time) +
						  3000.0 * sin(2.0 * M_PI * 3500.0 * time);
				actual[index] = expected[index];
			}
			assert(!txagc_avfilter_process(&reference, &cfg, expected, count,
						       rates[rate]));
			for (unsigned int offset = 0; offset < count;) {
				unsigned int chunk = 1 + (offset * 17 + block) % 53;
				if (chunk > count - offset)
					chunk = count - offset;
				assert(!txagc_avfilter_process(&fragmented, &cfg, actual + offset,
							       chunk, rates[rate]));
				offset += chunk;
			}
			for (unsigned int index = 0; index < count; ++index)
				assert(fabs(expected[index] - actual[index]) < 1e-6);
		}
		assert(reference.underrun_samples == 0 && fragmented.underrun_samples == 0);
		txagc_avfilter_destroy(&reference);
		txagc_avfilter_destroy(&fragmented);
	}
}

/** @brief Live mode and sample-rate changes retain configured crossover frequencies. */
static void test_dynamics_rate_changes(void)
{
	const enum txagc_stage stages[] = {TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER};
	for (unsigned int stage = 0; stage < 2; ++stage) {
		struct txagc_config cfg = dynamics_config(stages[stage], 1);
		struct txagc_avfilter state;
		double samples[960] = {0};
		cfg.compressor_high_crossover_hz = cfg.limiter_high_crossover_hz = 4500.0;
		txagc_avfilter_init(&state);
		assert(!txagc_avfilter_process(&state, &cfg, samples, 160, 8000));
		cfg.compressor_bands = cfg.limiter_bands = 3;
		assert(txagc_avfilter_process(&state, &cfg, samples, 160, 8000) < 0);
		assert(!txagc_avfilter_process(&state, &cfg, samples, 320, 16000));
		assert(!txagc_avfilter_process(&state, &cfg, samples, 960, 48000));
		assert(cfg.compressor_high_crossover_hz == 4500.0 &&
		       cfg.limiter_high_crossover_hz == 4500.0);
		txagc_avfilter_destroy(&state);
	}
}

/** @brief FFmpeg accepts the complete permitted control ranges, including makeup attenuation. */
static void test_dynamics_control_boundaries(void)
{
	const enum txagc_stage stages[] = {TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER};
	for (unsigned int stage = 0; stage < 2; ++stage)
		for (int bands = 1; bands <= 3; bands += 2)
			for (int upper = 0; upper <= 1; ++upper) {
				struct txagc_config cfg = dynamics_config(stages[stage], bands);
				cfg.compressor_threshold_dbfs = cfg.compressor_low_threshold_dbfs =
					cfg.compressor_mid_threshold_dbfs =
						cfg.compressor_high_threshold_dbfs =
							upper ? 0.0 : -60.0;
				cfg.limiter_threshold_dbfs = cfg.low_limiter_threshold_dbfs =
					cfg.mid_limiter_threshold_dbfs =
						cfg.high_limiter_threshold_dbfs =
							upper ? -1.0 : -40.0;
				cfg.compressor_ratio = cfg.compressor_low_ratio =
					cfg.compressor_mid_ratio = cfg.compressor_high_ratio =
						cfg.limiter_ratio = cfg.low_limiter_ratio =
							cfg.mid_limiter_ratio =
								cfg.high_limiter_ratio =
									upper ? 20.0 : 1.0;
				cfg.compressor_makeup_gain_db = cfg.compressor_low_makeup_gain_db =
					cfg.compressor_mid_makeup_gain_db =
						cfg.compressor_high_makeup_gain_db =
							upper ? 30.0 : -30.0;
				cfg.compressor_low_knee_db = cfg.compressor_mid_knee_db =
					cfg.compressor_high_knee_db = cfg.limiter_knee_db =
						cfg.low_limiter_knee_db = cfg.mid_limiter_knee_db =
							cfg.high_limiter_knee_db =
								upper ? 18.0 : 0.0;
				cfg.compressor_attack_ms = cfg.compressor_low_attack_ms =
					cfg.compressor_mid_attack_ms = cfg.compressor_high_attack_ms =
						cfg.limiter_attack_ms = cfg.low_limiter_attack_ms =
							cfg.mid_limiter_attack_ms =
								cfg.high_limiter_attack_ms =
									upper ? 1000.0 : 0.1;
				cfg.compressor_release_ms = cfg.compressor_low_release_ms =
					cfg.compressor_mid_release_ms = cfg.compressor_high_release_ms =
						cfg.limiter_release_ms = cfg.low_limiter_release_ms =
							cfg.mid_limiter_release_ms =
								cfg.high_limiter_release_ms =
									upper ? 9000.0 : 1.0;
				cfg.compressor_low_crossover_hz = cfg.limiter_low_crossover_hz =
					upper ? 2000.0 : 100.0;
				cfg.compressor_high_crossover_hz = cfg.limiter_high_crossover_hz =
					upper ? 5000.0 : 101.0;
				assert(isfinite(dynamics_response(&cfg, 16000, 1000.0)));
				assert(isfinite(dynamics_response(&cfg, 48000, 1000.0)));
			}
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
	config.compressor_bands = 1;
	config.compressor_threshold_dbfs = -8.0;
	config.compressor_ratio = 2.0;
	config.compressor_attack_ms = 40.0;
	config.compressor_release_ms = 300.0;
	config.compressor_sidechain_highpass_hz = 800.0;
	config.compressor_sidechain_lowpass_hz = 1500.0;
	config.limiter_enabled = 1;
	config.limiter_bands = 3;
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
	test_band_dynamics_audio();
	test_single_band_dynamics();
	test_multiband_frame_boundaries();
	test_dynamics_rate_changes();
	test_dynamics_control_boundaries();
	return 0;
}

/** @def RATE
 * @brief Sample rate in Hz used by this audio test.
 */
/** @def BLOCK
 * @brief Samples processed per audio block.
 */
