#ifndef TXAGC_CORE_H
#define TXAGC_CORE_H

#include <stddef.h>
#include <stdint.h>

#define TXAGC_MAX_LOOKAHEAD_SAMPLES 4096
#define TXAGC_CTCSS_FREQUENCIES_SIZE 512
#define TXAGC_MAX_DYNAMICS_STAGES 4

enum txagc_stage {
	TXAGC_STAGE_EXPANDER,
	TXAGC_STAGE_AGC,
	TXAGC_STAGE_COMPRESSOR,
	TXAGC_STAGE_LIMITER,
};

enum txagc_ctcss_filter_mode {
	TXAGC_CTCSS_FILTER_DISABLED,
	TXAGC_CTCSS_FILTER_NOTCH,
	TXAGC_CTCSS_FILTER_COMB,
	TXAGC_CTCSS_FILTER_HIGHPASS,
};

struct txagc_lookahead_sample {
	double program;
	double predicted;
};

struct txagc_config {
	/* Order applies only to the optional dynamics stages.  The receive
	 * conditioning and final transmitter tail are intentionally outside it. */
	unsigned int stage_count;
	enum txagc_stage stage_order[TXAGC_MAX_DYNAMICS_STAGES];
	int deemphasis_enabled;
	int preemphasis_enabled;
	double emphasis_corner_hz;
	double emphasis_reference_hz;
	int ctcss_filter_mode;
	double ctcss_notch_width_hz;
	double ctcss_highpass_hz;
	char ctcss_notch_frequencies[TXAGC_CTCSS_FREQUENCIES_SIZE];
	double input_gain_db;
	int agc_enabled;
	double target_dbfs;
	double max_gain_db;
	double max_attenuation_db;
	double agc_floor_dbfs;
	double attack_ms;
	double release_ms;
	double reset_after_ms;
	double sidechain_highpass_hz;
	double sidechain_lowpass_hz;
	int expander_enabled;
	double expander_threshold_dbfs;
	double expander_ratio;
	double expander_max_attenuation_db;
	double expander_attack_ms;
	double expander_release_ms;
	double expander_sidechain_highpass_hz;
	double expander_sidechain_lowpass_hz;
	int compressor_enabled;
	double compressor_threshold_dbfs;
	double compressor_ratio;
	double compressor_makeup_gain_db;
	double compressor_attack_ms;
	double compressor_release_ms;
	double compressor_sidechain_highpass_hz;
	double compressor_sidechain_lowpass_hz;
	int limiter_enabled;
	int splatter_filter_enabled;
	double limiter_crossover_hz;
	double low_limiter_threshold_dbfs;
	double low_limiter_ratio;
	double low_limiter_knee_db;
	double low_limiter_attack_ms;
	double low_limiter_release_ms;
	double high_clip_dbfs;
	double high_limiter_ratio;
	double high_limiter_knee_db;
	double high_limiter_attack_ms;
	double high_limiter_release_ms;
	int final_clipper_enabled;
	double final_clip_dbfs;
	int lookahead_limiter_enabled;
	double lookahead_limit_dbfs;
	double lookahead_ms;
	double lookahead_attack_ms;
	double lookahead_release_ms;
	int post_limiter_lowpass_enabled;
	double post_limiter_lowpass_hz;
	double output_highpass_hz;
	double output_lowpass_hz;
	double output_gain_db;
};

struct txagc_core {
	double gain;
	double input_dbfs;
	double sidechain_dbfs;
	double expander_sidechain_dbfs;
	double expander_gain;
	double expander_reduction_db;
	double compressor_sidechain_dbfs;
	double compressor_gain;
	double compressor_reduction_db;
	double output_dbfs;
	double output_peak_dbfs;
	double max_output_peak_dbfs;
	double below_floor_ms;
	double low_limiter_envelope;
	double low_limiter_reduction_db;
	double high_limiter_gain;
	double high_limiter_envelope;
	double high_limiter_reduction_db;
	double lookahead_gain;
	double lookahead_reduction_db;
	/* One raw-program delay line shared by every dynamics stage. Sidechains
	 * inspect the undelayed sample and their gains are applied to the sample
	 * emerging from this buffer lookahead_ms later. */
	struct txagc_lookahead_sample lookahead_buffer[TXAGC_MAX_LOOKAHEAD_SAMPLES];
	size_t lookahead_write;
	size_t lookahead_count;
	size_t lookahead_delay_samples;
	unsigned int lookahead_sample_rate;
	double crossover_low;
	double detector_crossover_low;
	double output_lowpass_z1;
	double output_lowpass_z2;
	double sidechain_z1;
	double sidechain_z2;
	double sidechain_highpass_z1;
	double sidechain_highpass_z2;
	unsigned int sidechain_sample_rate;
	double expander_sidechain_z1;
	double expander_sidechain_z2;
	double expander_sidechain_highpass_z1;
	double expander_sidechain_highpass_z2;
	unsigned int expander_sidechain_sample_rate;
	double compressor_sidechain_z1;
	double compressor_sidechain_z2;
	double compressor_sidechain_highpass_z1;
	double compressor_sidechain_highpass_z2;
	unsigned int compressor_sidechain_sample_rate;
	uint64_t frames;
	uint64_t samples;
	uint64_t below_floor_frames;
	uint64_t expanded_frames;
	uint64_t compressed_frames;
	uint64_t clipped_samples;
	uint64_t low_limited_samples;
	uint64_t high_clipped_samples;
	uint64_t final_clipped_samples;
	uint64_t lookahead_limited_samples;
};

int txagc_parse_stage_order(const char *text, struct txagc_config *config,
	char *error, size_t error_size);

void txagc_core_init(struct txagc_core *state);
void txagc_core_stream_reset(struct txagc_core *state);
void txagc_core_process(struct txagc_core *state, const struct txagc_config *cfg,
	int16_t *samples, size_t count, unsigned int sample_rate);
void txagc_core_process_double(struct txagc_core *state,
	const struct txagc_config *cfg, double *samples, size_t count,
	unsigned int sample_rate);

#endif
