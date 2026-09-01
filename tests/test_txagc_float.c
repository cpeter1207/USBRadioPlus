#include "../src/txagc/agc_core.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct txagc_core state;
	struct txagc_config cfg;
	double samples[960];
	double expected;
	size_t i;

	memset(&cfg, 0, sizeof(cfg));
	{
		char error[128];
		assert(!txagc_parse_stage_order("limiter,expander,agc,deesser,compressor,equalizer",
			&cfg, error, sizeof(error)));
		assert(cfg.stage_count == 6 && cfg.stage_order[3] == TXAGC_STAGE_DEESSER);
		assert(txagc_parse_stage_order("agc,agc", &cfg, error, sizeof(error)));
		assert(strstr(error, "duplicate stage"));
		assert(txagc_parse_stage_order("rnnoise,agc", &cfg, error, sizeof(error)));
		assert(strstr(error, "unknown, empty, or fixed"));
		assert(txagc_parse_stage_order("agc,,limiter", &cfg, error, sizeof(error)));
		cfg.compressor_enabled = 1;
		assert(txagc_parse_stage_order("agc,limiter", &cfg, error, sizeof(error)));
		assert(strstr(error, "enabled stage 'compressor' is missing"));
		cfg.compressor_enabled = 0;
		cfg.stage_count = 0;
	}
	cfg.sidechain_highpass_hz = 50.0;
	cfg.sidechain_lowpass_hz = 3000.0;
	cfg.expander_sidechain_highpass_hz = 50.0;
	cfg.expander_sidechain_lowpass_hz = 3000.0;
	cfg.compressor_sidechain_highpass_hz = 50.0;
	cfg.compressor_sidechain_lowpass_hz = 3000.0;
	cfg.attack_ms = cfg.release_ms = cfg.reset_after_ms = 100.0;
	cfg.expander_attack_ms = cfg.expander_release_ms = 100.0;
	cfg.compressor_attack_ms = cfg.compressor_release_ms = 100.0;
	cfg.low_limiter_attack_ms = cfg.low_limiter_release_ms = 10.0;
	cfg.high_limiter_attack_ms = 0.5;
	cfg.high_limiter_release_ms = 25.0;
	cfg.high_limiter_ratio = 20.0;
	cfg.high_limiter_knee_db = 6.0;
	cfg.lookahead_attack_ms = cfg.lookahead_release_ms = 10.0;
	cfg.limiter_crossover_hz = 1000.0;
	cfg.output_lowpass_hz = 3000.0;
	cfg.lookahead_ms = 5.0;
	cfg.expander_ratio = cfg.compressor_ratio = cfg.low_limiter_ratio = 1.0;

	/* With every stage disabled, samples beyond the PCM domain must survive
	 * unchanged. This is the contract for unlimited internal float headroom. */
	for (i = 0; i < 960; ++i) samples[i] = (i & 1) ? 65536.25 : -98304.5;
	expected = samples[0];
	txagc_core_init(&state);
	txagc_core_process_double(&state, &cfg, samples, 960, 48000);
	assert(samples[0] == expected);
	assert(samples[1] == 65536.25);
	assert(state.clipped_samples == 0);
	assert(state.output_peak_dbfs > 9.5);

	/* The detector sees voice-band energy but strongly rejects CTCSS.  Program
	 * samples remain unchanged because this filter is sidechain-only. */
	cfg.sidechain_highpass_hz = 800.0;
	cfg.sidechain_lowpass_hz = 1500.0;
	txagc_core_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (i = 0; i < 960; ++i) {
			samples[i] = 12000.0 * sin(2.0 * M_PI * 114.8
				* (block * 960 + i) / 48000.0);
		}
		txagc_core_process_double(&state, &cfg, samples, 960, 48000);
	}
	double ctcss_dbfs = state.sidechain_dbfs;
	txagc_core_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (i = 0; i < 960; ++i) {
			samples[i] = 12000.0 * sin(2.0 * M_PI * 1000.0
				* (block * 960 + i) / 48000.0);
		}
		txagc_core_process_double(&state, &cfg, samples, 960, 48000);
	}
	assert(state.sidechain_dbfs > ctcss_dbfs + 25.0);

	/* The compressor detector must include the gain actually applied by AGC. */
	cfg.agc_enabled = 1;
	cfg.target_dbfs = -16.0;
	cfg.max_gain_db = 12.0;
	cfg.max_attenuation_db = 12.0;
	cfg.agc_floor_dbfs = -100.0;
	cfg.lookahead_ms = 5.0;
	txagc_core_init(&state);
	state.gain = pow(10.0, 12.0 / 20.0);
	for (int block = 0; block < 100; ++block) {
		for (i = 0; i < 960; ++i) {
			samples[i] = 1200.0 * sin(2.0 * M_PI * 1000.0
				* (block * 960 + i) / 48000.0);
		}
		txagc_core_process_double(&state, &cfg, samples, 960, 48000);
	}
	assert(state.compressor_sidechain_dbfs > state.sidechain_dbfs + 11.5);
	{
		uint64_t frames = state.frames;
		state.lookahead_count = 17;
		state.high_limiter_gain = 0.25;
		state.crossover_low = 1234.0;
		txagc_core_stream_reset(&state);
		assert(state.frames == frames);
		assert(state.lookahead_count == 0);
		assert(state.high_limiter_gain == 1.0);
		assert(state.crossover_low == 0.0);
	}

	printf("txagc float, band-pass, detector, and stream-reset tests passed\n");
	return 0;
}
