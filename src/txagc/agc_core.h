#ifndef TXAGC_CORE_H
#define TXAGC_CORE_H

#include <stddef.h>
#define TXAGC_CTCSS_FREQUENCIES_SIZE 512
#define TXAGC_MAX_DYNAMICS_STAGES 6

enum txagc_stage {
	TXAGC_STAGE_EXPANDER,
	TXAGC_STAGE_AGC,
	TXAGC_STAGE_COMPRESSOR,
	TXAGC_STAGE_LIMITER,
	TXAGC_STAGE_EQUALIZER,
	TXAGC_STAGE_DEESSER,
};

enum txagc_ctcss_filter_mode {
	TXAGC_CTCSS_FILTER_DISABLED,
	TXAGC_CTCSS_FILTER_NOTCH,
	TXAGC_CTCSS_FILTER_HIGHPASS,
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
	int receive_bandpass_enabled;
	double receive_bandpass_highpass_hz;
	double receive_bandpass_lowpass_hz;
	int ctcss_filter_mode;
	double ctcss_notch_width_hz;
	double ctcss_highpass_hz;
	char ctcss_notch_frequencies[TXAGC_CTCSS_FREQUENCIES_SIZE];
	double input_gain_db;
	int equalizer_enabled;
	double equalizer_low_gain_db;
	double equalizer_low_frequency_hz;
	double equalizer_low_slope;
	double equalizer_mid_gain_db;
	double equalizer_mid_frequency_hz;
	double equalizer_mid_width_octaves;
	double equalizer_high_gain_db;
	double equalizer_high_frequency_hz;
	double equalizer_high_slope;
	int deesser_enabled;
	double deesser_frequency_hz;
	double deesser_width_octaves;
	double deesser_threshold_dbfs;
	double deesser_ratio;
	double deesser_max_reduction_db;
	double deesser_attack_ms;
	double deesser_release_ms;
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
	double limiter_low_crossover_hz;
	double limiter_high_crossover_hz;
	double low_limiter_threshold_dbfs;
	double low_limiter_ratio;
	double low_limiter_knee_db;
	double low_limiter_attack_ms;
	double low_limiter_release_ms;
	double mid_limiter_threshold_dbfs;
	double mid_limiter_ratio;
	double mid_limiter_knee_db;
	double mid_limiter_attack_ms;
	double mid_limiter_release_ms;
	double high_limiter_threshold_dbfs;
	double high_limiter_ratio;
	double high_limiter_knee_db;
	double high_limiter_attack_ms;
	double high_limiter_release_ms;
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

int txagc_parse_stage_order(const char *text, struct txagc_config *config, char *error,
			    size_t error_size);

#endif
