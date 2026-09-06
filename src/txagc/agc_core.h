/** @file
 * @brief Processing-stage identifiers, graph parameters, and strict stage-order parsing.
 */

#ifndef TXAGC_CORE_H
#define TXAGC_CORE_H

#include <stddef.h>

#define TXAGC_CTCSS_FREQUENCIES_SIZE 512

#define TXAGC_MAX_DYNAMICS_STAGES 6

/** Reorderable optional stages in the shared FFmpeg graph. */
enum txagc_stage {
	TXAGC_STAGE_EXPANDER /**< Downward expander. */,
	TXAGC_STAGE_AGC /**< Automatic level control. */,
	TXAGC_STAGE_COMPRESSOR /**< Dynamic-range compressor. */,
	TXAGC_STAGE_LIMITER /**< Shared FFmpeg multiband limiter. */,
	TXAGC_STAGE_EQUALIZER /**< Three-band equalizer. */,
	TXAGC_STAGE_DEESSER /**< Split-band de-esser. */
};

/** Fixed receive PL-filter selection. */
enum txagc_ctcss_filter_mode {
	TXAGC_CTCSS_FILTER_DISABLED /**< Bypass PL filtering. */,
	TXAGC_CTCSS_FILTER_NOTCH /**< Reject the currently decoded CTCSS tone. */,
	TXAGC_CTCSS_FILTER_HIGHPASS /**< Reject frequencies below the configured PL cutoff. */
};

/** Parameters for the shared FFmpeg graph; levels use dBFS and timings use milliseconds. */
struct txagc_config {
	/* Order applies only to the optional dynamics stages.  The receive
	 * conditioning and final transmitter tail are intentionally outside it. */
	/** Number of entries in stage_order. */
	unsigned int stage_count;
	/** Optional stages in execution order; fixed receive and transmitter stages are outside
	 * this list. */
	enum txagc_stage stage_order[TXAGC_MAX_DYNAMICS_STAGES];
	/** Nonzero enables deemphasis. */
	int deemphasis_enabled;
	/** Nonzero enables preemphasis. */
	int preemphasis_enabled;
	/** Emphasis corner in Hz. */
	double emphasis_corner_hz;
	/** Reference frequency in Hz used to normalize the emphasis response. */
	double emphasis_reference_hz;
	/** Nonzero enables receive bandpass. */
	int receive_bandpass_enabled;
	/** Receive bandpass highpass in Hz. */
	double receive_bandpass_highpass_hz;
	/** Receive bandpass lowpass in Hz. */
	double receive_bandpass_lowpass_hz;
	/** PL-filter selection: disabled, decoded-tone notch, or high-pass. */
	int ctcss_filter_mode;
	/** CTCSS notch width in Hz. */
	double ctcss_notch_width_hz;
	/** CTCSS highpass in Hz. */
	double ctcss_highpass_hz;
	/** Comma-separated notch frequencies in Hz. */
	char ctcss_notch_frequencies[TXAGC_CTCSS_FREQUENCIES_SIZE];
	/** Input gain in DB. */
	double input_gain_db;
	/** Nonzero enables equalizer. */
	int equalizer_enabled;
	/** Equalizer low gain in DB. */
	double equalizer_low_gain_db;
	/** Equalizer low frequency in Hz. */
	double equalizer_low_frequency_hz;
	/** Low shelving-filter slope. */
	double equalizer_low_slope;
	/** Equalizer mid gain in DB. */
	double equalizer_mid_gain_db;
	/** Equalizer mid frequency in Hz. */
	double equalizer_mid_frequency_hz;
	/** Midrange equalizer bandwidth in octaves. */
	double equalizer_mid_width_octaves;
	/** Equalizer high gain in DB. */
	double equalizer_high_gain_db;
	/** Equalizer high frequency in Hz. */
	double equalizer_high_frequency_hz;
	/** High shelving-filter slope. */
	double equalizer_high_slope;
	/** Nonzero enables deesser. */
	int deesser_enabled;
	/** Deesser frequency in Hz. */
	double deesser_frequency_hz;
	/** De-esser detector bandwidth in octaves. */
	double deesser_width_octaves;
	/** Deesser threshold in DBFS. */
	double deesser_threshold_dbfs;
	/** Selected-band compression ratio above the de-esser threshold. */
	double deesser_ratio;
	/** Deesser max reduction in DB. */
	double deesser_max_reduction_db;
	/** Deesser attack in Milliseconds. */
	double deesser_attack_ms;
	/** Deesser release in Milliseconds. */
	double deesser_release_ms;
	/** Nonzero enables agc. */
	int agc_enabled;
	/** AGC target RMS level in dBFS. */
	double target_dbfs;
	/** Maximum AGC gain increase in dB. */
	double max_gain_db;
	/** Maximum AGC gain reduction in dB. */
	double max_attenuation_db;
	/** AGC floor in DBFS. */
	double agc_floor_dbfs;
	/** AGC attack time in milliseconds. */
	double attack_ms;
	/** AGC release time in milliseconds. */
	double release_ms;
	/** Idle interval in milliseconds before AGC history resets. */
	double reset_after_ms;
	/** AGC detector's lower passband edge in Hz. */
	double sidechain_highpass_hz;
	/** AGC detector's upper passband edge in Hz. */
	double sidechain_lowpass_hz;
	/** Nonzero enables expander. */
	int expander_enabled;
	/** Expander threshold in DBFS. */
	double expander_threshold_dbfs;
	/** Expansion ratio below the expander threshold. */
	double expander_ratio;
	/** Expander max attenuation in DB. */
	double expander_max_attenuation_db;
	/** Expander attack in Milliseconds. */
	double expander_attack_ms;
	/** Expander release in Milliseconds. */
	double expander_release_ms;
	/** Expander sidechain highpass in Hz. */
	double expander_sidechain_highpass_hz;
	/** Expander sidechain lowpass in Hz. */
	double expander_sidechain_lowpass_hz;
	/** Nonzero enables compressor. */
	int compressor_enabled;
	/** Compressor threshold in DBFS. */
	double compressor_threshold_dbfs;
	/** Compression ratio above the compressor threshold. */
	double compressor_ratio;
	/** Compressor makeup gain in DB. */
	double compressor_makeup_gain_db;
	/** Compressor attack in Milliseconds. */
	double compressor_attack_ms;
	/** Compressor release in Milliseconds. */
	double compressor_release_ms;
	/** Compressor sidechain highpass in Hz. */
	double compressor_sidechain_highpass_hz;
	/** Compressor sidechain lowpass in Hz. */
	double compressor_sidechain_lowpass_hz;
	/** Nonzero enables the shared FFmpeg multiband limiter. */
	int limiter_enabled;
	/** Nonzero enables the fixed transmitter FFT band-pass. */
	int splatter_filter_enabled;
	/** Limiter low crossover in Hz. */
	double limiter_low_crossover_hz;
	/** Limiter high crossover in Hz. */
	double limiter_high_crossover_hz;
	/** Low limiter threshold in DBFS. */
	double low_limiter_threshold_dbfs;
	/** Low-band limiting ratio. */
	double low_limiter_ratio;
	/** Low limiter knee in DB. */
	double low_limiter_knee_db;
	/** Low limiter attack in Milliseconds. */
	double low_limiter_attack_ms;
	/** Low limiter release in Milliseconds. */
	double low_limiter_release_ms;
	/** Mid limiter threshold in DBFS. */
	double mid_limiter_threshold_dbfs;
	/** Middle-band limiting ratio. */
	double mid_limiter_ratio;
	/** Mid limiter knee in DB. */
	double mid_limiter_knee_db;
	/** Mid limiter attack in Milliseconds. */
	double mid_limiter_attack_ms;
	/** Mid limiter release in Milliseconds. */
	double mid_limiter_release_ms;
	/** High limiter threshold in DBFS. */
	double high_limiter_threshold_dbfs;
	/** High-band limiting ratio. */
	double high_limiter_ratio;
	/** High limiter knee in DB. */
	double high_limiter_knee_db;
	/** High limiter attack in Milliseconds. */
	double high_limiter_attack_ms;
	/** High limiter release in Milliseconds. */
	double high_limiter_release_ms;
	/** Nonzero enables the fixed final limiter. */
	int lookahead_limiter_enabled;
	/** Final-limiter peak threshold in dBFS. */
	double lookahead_limit_dbfs;
	/** Final-limiter lookahead in milliseconds. */
	double lookahead_ms;
	/** Final-limiter attack in milliseconds. */
	double lookahead_attack_ms;
	/** Final-limiter release in milliseconds. */
	double lookahead_release_ms;
	/** Nonzero enables post limiter lowpass. */
	int post_limiter_lowpass_enabled;
	/** Post limiter lowpass in Hz. */
	double post_limiter_lowpass_hz;
	/** Transmitter band-pass lower cutoff in Hz. */
	double output_highpass_hz;
	/** Transmitter band-pass upper cutoff in Hz. */
	double output_lowpass_hz;
	/** Output gain in DB. */
	double output_gain_db;
};

/** @brief Parse the comma-separated optional-stage order; reject unknown and repeated stages.
 * @param text Text to parse; mutable storage may be edited in place.
 * @param config Filter and dynamics settings for the shared FFmpeg graph.
 * @param error Receives a diagnostic for invalid input.
 * @param error_size Diagnostic buffer capacity in bytes.
 * @return Zero on success; -1 with a diagnostic for invalid order.
 */
int txagc_parse_stage_order(const char *text, struct txagc_config *config, char *error,
			    size_t error_size);

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def TXAGC_CTCSS_FREQUENCIES_SIZE
 * @brief Capacity of the comma-separated notch-frequency string.
 */
/** @def TXAGC_MAX_DYNAMICS_STAGES
 * @brief Maximum reorderable stages in a source chain.
 */
/** @} */
