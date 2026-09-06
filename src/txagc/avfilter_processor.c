/** @file
 * @brief Shared FFmpeg filtering, emphasis, equalization, dynamics, limiting, and meters.
 */

#include "avfilter_processor.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/version.h>

/* Package builds select a private absolute path. Standalone graph tests use
 * LADSPA_PATH so they exercise the freshly built effect, never a node install. */
#ifndef URP_AGC_PLUGIN_PATH
#define URP_AGC_PLUGIN_PATH "usbradioplus_agc"
#endif
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#ifdef URP_AVFILTER_TESTING
#include "avfilter_processor_internal.h"
#endif

#ifdef URP_AVFILTER_TESTING
#define AVFILTER_PRIVATE
#else

#define AVFILTER_PRIVATE static
#endif

#define GRAPH_SIZE 16384

#define NAME_SIZE 32

#define CTCSS_NOTCH_SECTIONS 8

/** @brief Convert a decibel amplitude gain to a linear multiplier.
 * @param db Amplitude gain in decibels.
 * @return Linear amplitude multiplier.
 */
AVFILTER_PRIVATE double db_to_linear(double db)
{
	return pow(10.0, db / 20.0);
}

/** @brief Bound a floating-point value to the inclusive interval.
 * @param value Floating-point value to bound.
 * @param low Inclusive lower bound.
 * @param high Inclusive upper bound.
 * @return The value bounded to [low, high].
 */
AVFILTER_PRIVATE double clamp(double value, double low, double high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

/** @brief Append formatted filter syntax without writing beyond the graph buffer.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param format printf-style message format.
 * @param ... Values for the preceding format string.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int graph_append(char *graph, size_t size, const char *format, ...)
{
	va_list ap;
	size_t used = strlen(graph);
	int result;

	if (used >= size) {
		return AVERROR(ENOSPC);
	}
	va_start(ap, format);
	result = vsnprintf(graph + used, size - used, format, ap);
	va_end(ap);
	if (result < 0 || (size_t)result >= size - used) {
		return AVERROR(ENOSPC);
	}
	return 0;
}

/** @brief Read one numeric FFmpeg astats metadata value.
 * @param frame FFmpeg frame containing astats metadata.
 * @param name astats metadata key.
 * @param value Receives the parsed numeric metadata value.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
AVFILTER_PRIVATE int astats_value(const AVFrame *frame, const char *name, double *value)
{
	const AVDictionaryEntry *entry = av_dict_get(frame->metadata, name, NULL, 0);
	char *end;
	if (!entry) {
		return -1;
	}
	*value = strtod(entry->value, &end);
	return end != entry->value ? 0 : -1;
}

/** @brief Accumulate input or output peak and RMS readings from an FFmpeg frame.
 * @param state Processor or stream state owned by the caller.
 * @param frame FFmpeg frame containing astats metadata.
 * @param input Nonzero updates input meters; zero updates output meters.
 */
AVFILTER_PRIVATE void update_astats(struct txagc_avfilter *state, const AVFrame *frame, int input)
{
	double peak;
	double rms;
	if (astats_value(frame, "lavfi.astats.Overall.Peak_level", &peak) ||
	    astats_value(frame, "lavfi.astats.Overall.RMS_level", &rms) || rms <= -90.0) {
		return;
	}
	if (input) {
		state->input_peak_dbfs = peak;
		state->input_rms_dbfs = rms;
		if (peak > state->input_max_peak_dbfs) {
			state->input_max_peak_dbfs = peak;
		}
		if (rms > state->input_max_rms_dbfs) {
			state->input_max_rms_dbfs = rms;
		}
	} else {
		state->output_peak_dbfs = peak;
		state->output_rms_dbfs = rms;
		if (peak > state->output_max_peak_dbfs) {
			state->output_max_peak_dbfs = peak;
		}
		if (rms > state->output_max_rms_dbfs) {
			state->output_max_rms_dbfs = rms;
		}
	}
}

#ifndef URP_AVFILTER_TESTING
/** Spectral measurement taps around the fixed band-pass. */
enum cleanup_meter {
	CLEANUP_PRE_FULL /**< Full-band input before the fixed band-pass. */,
	CLEANUP_PRE_5_8 /**< 5–8 kHz input energy before filtering. */,
	CLEANUP_PRE_8_PLUS /**< Input energy above 8 kHz. */,
	CLEANUP_POST_5_8 /**< 5–8 kHz output energy after filtering. */,
	CLEANUP_POST_8_PLUS /**< Output energy above 8 kHz after filtering. */
};
#endif

/** @brief Update a pre- or post-filter spectral meter from astats metadata.
 * @param state Processor or stream state owned by the caller.
 * @param frame FFmpeg frame containing spectral astats metadata.
 * @param meter Spectral meter selected for this update.
 */
AVFILTER_PRIVATE void update_cleanup_astats(struct txagc_avfilter *state, const AVFrame *frame,
					    enum cleanup_meter meter)
{
	double peak;
	double rms;
	double *current;
	double *maximum;

	if (astats_value(frame, "lavfi.astats.Overall.RMS_level", &rms)) {
		return;
	}
	if (meter == CLEANUP_PRE_FULL) {
		if (astats_value(frame, "lavfi.astats.Overall.Peak_level", &peak)) {
			return;
		}
		state->cleanup_pre_peak_dbfs = peak;
		state->cleanup_pre_rms_dbfs = rms;
		if (peak > state->cleanup_pre_max_peak_dbfs) {
			state->cleanup_pre_max_peak_dbfs = peak;
		}
		if (rms > state->cleanup_pre_max_rms_dbfs) {
			state->cleanup_pre_max_rms_dbfs = rms;
		}
		return;
	}
	switch (meter) {
	case CLEANUP_PRE_5_8:
		current = &state->cleanup_pre_5_8_rms_dbfs;
		maximum = &state->cleanup_pre_5_8_max_rms_dbfs;
		break;
	case CLEANUP_PRE_8_PLUS:
		current = &state->cleanup_pre_8_plus_rms_dbfs;
		maximum = &state->cleanup_pre_8_plus_max_rms_dbfs;
		break;
	case CLEANUP_POST_5_8:
		current = &state->cleanup_post_5_8_rms_dbfs;
		maximum = &state->cleanup_post_5_8_max_rms_dbfs;
		break;
	default:
		current = &state->cleanup_post_8_plus_rms_dbfs;
		maximum = &state->cleanup_post_8_plus_max_rms_dbfs;
		break;
	}
	*current = rms;
	if (rms > *maximum) {
		*maximum = rms;
	}
}

/** @brief Append a dynamics stage with a separately filtered detector input.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param input Input FFmpeg graph label.
 * @param output Output FFmpeg graph label.
 * @param prefix Unique label prefix for the appended graph fragment.
 * @param filter FFmpeg dynamics filter name.
 * @param highpass Lower passband edge in Hz; zero disables the lower edge.
 * @param lowpass Upper passband edge in Hz; zero disables the upper edge.
 * @param options FFmpeg dynamics-filter option string.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int add_sidechain_stage(char *graph, size_t size, const char *input,
					 const char *output, const char *prefix, const char *filter,
					 double highpass, double lowpass, const char *options)
{
	char detector_highpass[64] = "anull";
	char detector_lowpass[64] = "anull";
	/* Omit disabled filters rather than asking FFmpeg to accept a zero cutoff. */
	if (highpass > 0.0)
		snprintf(detector_highpass, sizeof(detector_highpass), "highpass=f=%.9g:p=2",
			 highpass);
	if (lowpass > 0.0)
		snprintf(detector_lowpass, sizeof(detector_lowpass), "lowpass=f=%.9g:p=2", lowpass);
	return graph_append(graph, size,
			    "[%s]asplit=2[%smain][%ssc];"
			    "[%ssc]%s,%s[%sdet];"
			    "[%smain][%sdet]%s=%s[%s];",
			    input, prefix, prefix, prefix, detector_highpass, detector_lowpass,
			    prefix, prefix, prefix, filter, options, output);
}

/** @brief Append the shared FFT band-pass filter between two graph labels.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param input Input FFmpeg graph label.
 * @param output Output FFmpeg graph label.
 * @param prefix Unique label prefix for the appended graph fragment.
 * @param highpass Lower passband edge in Hz; zero disables the lower edge.
 * @param lowpass Upper passband edge in Hz; zero disables the upper edge.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int add_brickwall_bandpass(char *graph, size_t size, const char *input,
					    const char *output, const char *prefix, double highpass,
					    double lowpass)
{
	return graph_append(graph, size,
			    "[%s]acrossover=split=%.9g:order=20th[%slo][%spass];"
			    "[%slo]anullsink;"
			    "[%spass]acrossover=split=%.9g:order=20th[%s][%shi];"
			    "[%shi]anullsink;",
			    input, highpass, prefix, prefix, prefix, prefix, lowpass, output,
			    prefix, prefix);
}

/** @brief Append FFmpeg emphasis normalized at the configured reference frequency.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param input Input FFmpeg graph label.
 * @param output Output FFmpeg graph label.
 * @param production Nonzero selects preemphasis; zero selects deemphasis.
 * @param corner_hz Emphasis transition frequency in Hz.
 * @param reference_hz Frequency in Hz at which the emphasis response is normalized.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int add_emphasis(char *graph, size_t size, const char *input, const char *output,
				  int production, double corner_hz, double reference_hz,
				  unsigned int sample_rate)
{
	double pole = exp(-2.0 * M_PI * corner_hz / sample_rate);
	double omega = 2.0 * M_PI * reference_hz / sample_rate;
	double inverse_at_reference =
		sqrt(1.0 + pole * pole - 2.0 * pole * cos(omega)) / (1.0 - pole);
	if (production) {
		double scale = 1.0 / inverse_at_reference;
		return graph_append(graph, size,
				    "[%s]biquad=b0=%.17g:b1=%.17g:b2=0:a0=1:a1=0:a2=0:"
				    "precision=f64[%s];",
				    input, scale / (1.0 - pole), -pole * scale / (1.0 - pole),
				    output);
	}
	return graph_append(graph, size,
			    "[%s]biquad=b0=%.17g:b1=0:b2=0:a0=1:a1=%.17g:a2=0:"
			    "precision=f64[%s];",
			    input, inverse_at_reference * (1.0 - pole), -pole, output);
}

/** Settings for one independently detected dynamics band. */
struct dynamics_band {
	double threshold_dbfs; /**< Detector threshold in dBFS. */
	double ratio;	       /**< Downward compression ratio. */
	double makeup_gain_db; /**< Gain applied after compression in dB. */
	double knee_db;	       /**< Soft-knee width in dB; zero selects a hard knee. */
	double attack_ms;      /**< Detector attack in milliseconds. */
	double release_ms;     /**< Detector release in milliseconds. */
};

/** @brief Split, independently compress, and recombine three complementary bands.
 * @param graph Destination FFmpeg graph description.
 * @param size Destination capacity including its terminator.
 * @param input Input graph label.
 * @param output Output graph label.
 * @param prefix Unique label prefix for this stage instance.
 * @param low_crossover Lower crossover frequency in Hz.
 * @param high_crossover Upper crossover frequency in Hz.
 * @param bands Low-, middle-, and high-band controls.
 * @param detection FFmpeg detector type: rms for compression, peak for limiting.
 * @return Zero on success, or a negative FFmpeg error on insufficient capacity.
 */
static int add_three_band_dynamics(char *graph, size_t size, const char *input, const char *output,
				   const char *prefix, double low_crossover, double high_crossover,
				   const struct dynamics_band bands[3], const char *detection)
{
	static const char *const names[] = {"lo", "mid", "hi"};
	int result;

	/* Complementary crossover outputs retain unity summed response. Each band's
	 * detector follows its own program audio; no speech sidechain trims a band. */
	result = graph_append(graph, size,
			      "[%s]acrossover=split=%.9g %.9g:order=4th[%slo][%smid][%shi];", input,
			      low_crossover, high_crossover, prefix, prefix, prefix);
	if (result < 0)
		return result;
	for (unsigned int index = 0; index < 3; ++index) {
		const struct dynamics_band *band = &bands[index];
		char makeup[80] = "";
		/* FFmpeg's compressor makeup cannot attenuate. A post-compression
		 * gain supports the configured negative gains without changing detection. */
		if (band->makeup_gain_db != 0.0)
			snprintf(makeup, sizeof(makeup), ",volume=%.12g:precision=double",
				 db_to_linear(band->makeup_gain_db));
		result = graph_append(graph, size,
				      "[%s%s]acompressor=mode=downward:threshold=%.12g:ratio=%.9g:"
				      "knee=%.12g:attack=%.9g:release=%.9g:detection=%s%s[%s%sc];",
				      prefix, names[index], db_to_linear(band->threshold_dbfs),
				      clamp(band->ratio, 1.0, 20.0), db_to_linear(band->knee_db),
				      band->attack_ms, band->release_ms, detection, makeup, prefix,
				      names[index]);
		if (result < 0)
			return result;
	}
	return graph_append(graph, size, "[%sloc][%smidc][%shic]amix=inputs=3:normalize=0[%s];",
			    prefix, prefix, prefix, output);
}

/** @brief Append one selected equalizer, expander, AGC, de-esser, compressor, or limiter.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param cfg Filter and dynamics settings for the shared FFmpeg graph.
 * @param kind Stage or configuration-section kind.
 * @param current Input FFmpeg graph label.
 * @param next Output FFmpeg graph label.
 * @param serial Sequence number used to keep filter labels unique.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int add_dynamic_stage(char *graph, size_t size, const struct txagc_config *cfg,
				       enum txagc_stage kind, const char *current, const char *next,
				       unsigned int serial)
{
	char options[1024];
	char prefix[32];

	snprintf(prefix, sizeof(prefix), "d%u", serial);
	switch (kind) {
	case TXAGC_STAGE_DEESSER: {
		double octave_ratio;
		double q;
		if (!cfg->deesser_enabled)
			return 0;
		octave_ratio = pow(2.0, cfg->deesser_width_octaves);
		q = sqrt(octave_ratio) / (octave_ratio - 1.0);
#if LIBAVFILTER_VERSION_MAJOR >= 9
		return graph_append(
			graph, size,
			"[%s]adynamicequalizer=threshold=%.12g:dfrequency=%.9g:"
			"dqfactor=%.12g:tfrequency=%.9g:tqfactor=%.12g:"
			"attack=%.9g:release=%.9g:ratio=%.9g:range=%.12g:"
			"mode=cutabove:dftype=bandpass:tftype=bell:precision=double[%s];",
			current, db_to_linear(cfg->deesser_threshold_dbfs),
			cfg->deesser_frequency_hz, q, cfg->deesser_frequency_hz, q,
			cfg->deesser_attack_ms, cfg->deesser_release_ms, cfg->deesser_ratio,
			cfg->deesser_max_reduction_db / 2.0, next);
#else
		double ratio = cfg->deesser_ratio;
		double span = -cfg->deesser_threshold_dbfs;
		double detector_reduction = cfg->deesser_max_reduction_db / 2.0;
		/* FFmpeg 5.1 does not apply range to cuts. Cap its ratio so a
		 * full-scale detector input cannot exceed the requested reduction.
		 * Its bell response applies the detector gain twice at band center. */
		if (span > detector_reduction) {
			double limited_ratio = span / (span - detector_reduction);
			if (ratio > limited_ratio)
				ratio = limited_ratio;
		}
		return graph_append(graph, size,
				    "[%s]adynamicequalizer=threshold=%.12g:dfrequency=%.9g:"
				    "dqfactor=%.12g:tfrequency=%.9g:tqfactor=%.12g:"
				    "attack=%.9g:release=%.9g:ratio=%.9g:range=%.12g:"
				    "mode=cut:tftype=bell[%s];",
				    current, db_to_linear(cfg->deesser_threshold_dbfs),
				    cfg->deesser_frequency_hz, q, cfg->deesser_frequency_hz, q,
				    cfg->deesser_attack_ms, cfg->deesser_release_ms, ratio, 0.0,
				    next);
#endif
	}
	case TXAGC_STAGE_EQUALIZER:
		if (!cfg->equalizer_enabled)
			return 0;
		return graph_append(graph, size,
				    "[%s]bass=g=%.9g:f=%.9g:t=s:w=%.9g:r=f64,"
				    "equalizer=f=%.9g:t=o:w=%.9g:g=%.9g:r=f64,"
				    "treble=g=%.9g:f=%.9g:t=s:w=%.9g:r=f64[%s];",
				    current, cfg->equalizer_low_gain_db,
				    cfg->equalizer_low_frequency_hz, cfg->equalizer_low_slope,
				    cfg->equalizer_mid_frequency_hz,
				    cfg->equalizer_mid_width_octaves, cfg->equalizer_mid_gain_db,
				    cfg->equalizer_high_gain_db, cfg->equalizer_high_frequency_hz,
				    cfg->equalizer_high_slope, next);
	case TXAGC_STAGE_AGC:
		if (!cfg->agc_enabled)
			return 0;
		/* Pack program and detector into explicit channels for the LADSPA
		 * two-input/one-output effect. Only its detector sees the filters.
		 * FFmpeg 5 needs the effect's single output labeled explicitly as mono. */
		snprintf(options, sizeof(options),
			 "inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,"
			 "ladspa=file='%s':plugin=usbradioplus_agc:controls="
			 "c0=%.12g|c1=%.12g|c2=%.12g|c3=%.12g|c4=%.12g|c5=%.12g|"
			 "c6=%.12g|c7=%.12g|c8=%.12g|c9=%.12g,"
			 "aformat=channel_layouts=mono",
			 URP_AGC_PLUGIN_PATH, cfg->target_dbfs, cfg->agc_rms_averaging_ms,
			 cfg->agc_gain_increase_db_per_second, cfg->agc_gain_decrease_db_per_second,
			 cfg->max_gain_db, cfg->max_attenuation_db,
			 cfg->agc_activity_threshold_dbfs, cfg->agc_activity_hysteresis_db,
			 cfg->agc_hold_ms, cfg->agc_deadband_db);
		return add_sidechain_stage(graph, size, current, next, prefix, "join",
					   cfg->sidechain_highpass_hz, cfg->sidechain_lowpass_hz,
					   options);
	case TXAGC_STAGE_EXPANDER:
		if (!cfg->expander_enabled)
			return 0;
		snprintf(options, sizeof(options),
			 "threshold=%.12g:ratio=%.9g:range=%.12g:attack=%.9g:"
			 "release=%.9g:knee=2.828427:detection=rms",
			 db_to_linear(cfg->expander_threshold_dbfs),
			 clamp(cfg->expander_ratio, 1.0, 20.0),
			 db_to_linear(-fabs(cfg->expander_max_attenuation_db)),
			 cfg->expander_attack_ms, cfg->expander_release_ms);
		return add_sidechain_stage(graph, size, current, next, prefix, "sidechaingate",
					   cfg->expander_sidechain_highpass_hz,
					   cfg->expander_sidechain_lowpass_hz, options);
	case TXAGC_STAGE_COMPRESSOR: {
		const struct dynamics_band bands[] = {
			{cfg->compressor_low_threshold_dbfs, cfg->compressor_low_ratio,
			 cfg->compressor_low_makeup_gain_db, cfg->compressor_low_knee_db,
			 cfg->compressor_low_attack_ms, cfg->compressor_low_release_ms},
			{cfg->compressor_mid_threshold_dbfs, cfg->compressor_mid_ratio,
			 cfg->compressor_mid_makeup_gain_db, cfg->compressor_mid_knee_db,
			 cfg->compressor_mid_attack_ms, cfg->compressor_mid_release_ms},
			{cfg->compressor_high_threshold_dbfs, cfg->compressor_high_ratio,
			 cfg->compressor_high_makeup_gain_db, cfg->compressor_high_knee_db,
			 cfg->compressor_high_attack_ms, cfg->compressor_high_release_ms},
		};
		if (!cfg->compressor_enabled)
			return 0;
		if (cfg->compressor_bands == 3)
			return add_three_band_dynamics(graph, size, current, next, prefix,
						       cfg->compressor_low_crossover_hz,
						       cfg->compressor_high_crossover_hz, bands,
						       "rms");
		snprintf(options, sizeof(options),
			 "mode=downward:threshold=%.12g:ratio=%.9g:attack=%.9g:"
			 "release=%.9g:knee=2.828427:detection=rms,volume=%.12g:precision=double",
			 db_to_linear(cfg->compressor_threshold_dbfs),
			 clamp(cfg->compressor_ratio, 1.0, 20.0), cfg->compressor_attack_ms,
			 cfg->compressor_release_ms, db_to_linear(cfg->compressor_makeup_gain_db));
		return add_sidechain_stage(graph, size, current, next, prefix, "sidechaincompress",
					   cfg->compressor_sidechain_highpass_hz,
					   cfg->compressor_sidechain_lowpass_hz, options);
	}
	case TXAGC_STAGE_LIMITER: {
		const struct dynamics_band bands[] = {
			{cfg->low_limiter_threshold_dbfs, cfg->low_limiter_ratio, 0.0,
			 cfg->low_limiter_knee_db, cfg->low_limiter_attack_ms,
			 cfg->low_limiter_release_ms},
			{cfg->mid_limiter_threshold_dbfs, cfg->mid_limiter_ratio, 0.0,
			 cfg->mid_limiter_knee_db, cfg->mid_limiter_attack_ms,
			 cfg->mid_limiter_release_ms},
			{cfg->high_limiter_threshold_dbfs, cfg->high_limiter_ratio, 0.0,
			 cfg->high_limiter_knee_db, cfg->high_limiter_attack_ms,
			 cfg->high_limiter_release_ms},
		};
		if (!cfg->limiter_enabled)
			return 0;
		if (cfg->limiter_bands == 3)
			return add_three_band_dynamics(
				graph, size, current, next, prefix, cfg->limiter_low_crossover_hz,
				cfg->limiter_high_crossover_hz, bands, "peak");
		return graph_append(graph, size,
				    "[%s]acompressor=mode=downward:threshold=%.12g:ratio=%.9g:"
				    "knee=%.12g:attack=%.9g:release=%.9g:detection=peak[%s];",
				    current, db_to_linear(cfg->limiter_threshold_dbfs),
				    clamp(cfg->limiter_ratio, 1.0, 20.0),
				    db_to_linear(cfg->limiter_knee_db), cfg->limiter_attack_ms,
				    cfg->limiter_release_ms, next);
	}
	}
	return AVERROR(EINVAL);
}

/** @brief Build the ordered optional graph between the fixed receive and transmitter stages.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param cfg Filter and dynamics settings for the shared FFmpeg graph.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int build_description(char *graph, size_t size, const struct txagc_config *cfg,
				       unsigned int sample_rate)
{
	char current[NAME_SIZE] = "s0";
	const char *graph_input = "in";
	char next[NAME_SIZE];
	unsigned int stage = 1;
	unsigned int order_index;

	graph[0] = '\0';
	/* A link can run at a lower sample rate than the native radio chain. Reject
	 * impossible band definitions instead of silently moving their crossovers. */
	if (cfg->compressor_enabled && cfg->compressor_bands == 3 &&
	    cfg->compressor_high_crossover_hz >= sample_rate * 0.5) {
		av_log(NULL, AV_LOG_ERROR,
		       "USBRadioPlus: compressor crossovers %.9g/%.9g Hz must be below "
		       "Nyquist %.9g Hz at %u Hz sample rate\n",
		       cfg->compressor_low_crossover_hz, cfg->compressor_high_crossover_hz,
		       sample_rate * 0.5, sample_rate);
		return AVERROR(EINVAL);
	}
	if (cfg->limiter_enabled && cfg->limiter_bands == 3 &&
	    cfg->limiter_high_crossover_hz >= sample_rate * 0.5) {
		av_log(NULL, AV_LOG_ERROR,
		       "USBRadioPlus: limiter crossovers %.9g/%.9g Hz must be below "
		       "Nyquist %.9g Hz at %u Hz sample rate\n",
		       cfg->limiter_low_crossover_hz, cfg->limiter_high_crossover_hz,
		       sample_rate * 0.5, sample_rate);
		return AVERROR(EINVAL);
	}
	if (cfg->deemphasis_enabled) {
		if (add_emphasis(graph, size, "in", "deemphasized", 0, cfg->emphasis_corner_hz,
				 cfg->emphasis_reference_hz, sample_rate) < 0) {
			return AVERROR(ENOSPC);
		}
		graph_input = "deemphasized";
	}
	if (cfg->receive_bandpass_enabled) {
		if (add_brickwall_bandpass(graph, size, graph_input, "rxbandpass", "rxbp",
					   cfg->receive_bandpass_highpass_hz,
					   cfg->receive_bandpass_lowpass_hz) < 0) {
			return AVERROR(ENOSPC);
		}
		graph_input = "rxbandpass";
	}
	if (cfg->ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH) {
		const char *cursor = cfg->ctcss_notch_frequencies;
		char input_name[NAME_SIZE];
		char output_name[NAME_SIZE];
		unsigned int notch = 0;

		snprintf(input_name, sizeof(input_name), "%s", graph_input);
		while (*cursor) {
			char *end;
			double frequency = strtod(cursor, &end);
			unsigned int section;
			if (end == cursor) {
				++cursor;
				continue;
			}
			cursor = end;
			if (frequency < 50.0 || frequency > 300.0)
				continue;
			/* Higher order supplies 50 dB rejection at the TIA-603
			 * frequency-tolerance edges without widening into speech. */
			for (section = 0; section < CTCSS_NOTCH_SECTIONS; ++section) {
				snprintf(output_name, sizeof(output_name), "ctn%u_%u", notch,
					 section);
				if (graph_append(graph, size,
						 "[%s]bandreject=f=%.9g:t=h:w=%.9g:r=f64[%s];",
						 input_name, frequency, cfg->ctcss_notch_width_hz,
						 output_name) < 0)
					return AVERROR(ENOSPC);
				snprintf(input_name, sizeof(input_name), "%s", output_name);
			}
			++notch;
		}
		if (notch) {
			snprintf(next, sizeof(next), "%s", input_name);
			graph_input = next;
		}
	} else if (cfg->ctcss_filter_mode == TXAGC_CTCSS_FILTER_HIGHPASS &&
		   cfg->ctcss_highpass_hz > 0.0) {
		if (graph_append(graph, size,
				 "[%s]acrossover=split=%.9g:order=20th[ctlow][cthigh];"
				 "[ctlow]anullsink;",
				 graph_input, cfg->ctcss_highpass_hz) < 0)
			return AVERROR(ENOSPC);
		graph_input = "cthigh";
	}
	if (graph_append(graph, size,
			 "[%s]asplit=2[programin][metertap];"
			 "[metertap]astats=metadata=1:reset=1:measure_perchannel=none:measure_"
			 "overall=Peak_level+RMS_level[in_meter];"
			 "[programin]volume=%.12g[%s];",
			 graph_input, db_to_linear(cfg->input_gain_db), current) < 0) {
		return AVERROR(ENOSPC);
	}

	for (order_index = 0; order_index < cfg->stage_count; ++order_index) {
		int enabled =
			(cfg->stage_order[order_index] == TXAGC_STAGE_AGC && cfg->agc_enabled) ||
			(cfg->stage_order[order_index] == TXAGC_STAGE_EXPANDER &&
			 cfg->expander_enabled) ||
			(cfg->stage_order[order_index] == TXAGC_STAGE_COMPRESSOR &&
			 cfg->compressor_enabled) ||
			(cfg->stage_order[order_index] == TXAGC_STAGE_LIMITER &&
			 cfg->limiter_enabled) ||
			(cfg->stage_order[order_index] == TXAGC_STAGE_EQUALIZER &&
			 cfg->equalizer_enabled) ||
			(cfg->stage_order[order_index] == TXAGC_STAGE_DEESSER &&
			 cfg->deesser_enabled);
		if (!enabled)
			continue;
		snprintf(next, sizeof(next), "s%u", stage++);
		if (add_dynamic_stage(graph, size, cfg, cfg->stage_order[order_index], current,
				      next, order_index) < 0)
			return AVERROR(ENOSPC);
		snprintf(current, sizeof(current), "%s", next);
	}

	if (cfg->preemphasis_enabled) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (add_emphasis(graph, size, current, next, 1, cfg->emphasis_corner_hz,
				 cfg->emphasis_reference_hz, sample_rate) < 0) {
			return AVERROR(ENOSPC);
		}
		snprintf(current, sizeof(current), "%s", next);
	}

	if (cfg->splatter_filter_enabled && cfg->output_highpass_hz > 0.0) {
		char rejected[NAME_SIZE];
		snprintf(next, sizeof(next), "s%u", stage++);
		snprintf(rejected, sizeof(rejected), "hp%urej", stage);
		if (graph_append(graph, size,
				 "[%s]acrossover=split=%.9g:order=20th[%s][%s];[%s]anullsink;",
				 current, cfg->output_highpass_hz, rejected, next, rejected) < 0)
			return AVERROR(ENOSPC);
		snprintf(current, sizeof(current), "%s", next);
	}
	if (cfg->splatter_filter_enabled && cfg->output_lowpass_hz > 0.0) {
		char rejected[NAME_SIZE];
		snprintf(next, sizeof(next), "s%u", stage++);
		snprintf(rejected, sizeof(rejected), "lp%urej", stage);
		if (graph_append(graph, size,
				 "[%s]acrossover=split=%.9g:order=20th[%s][%s];[%s]anullsink;",
				 current, cfg->output_lowpass_hz, next, rejected, rejected) < 0)
			return AVERROR(ENOSPC);
		snprintf(current, sizeof(current), "%s", next);
	}

	/* Output gain precedes the final limiter so it cannot defeat the configured
	 * ceiling. PCM conversion clamps only values that cannot be represented. */
	if (cfg->output_gain_db != 0.0) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (graph_append(graph, size, "[%s]volume=%.12g[%s];", current,
				 db_to_linear(cfg->output_gain_db), next) < 0) {
			return AVERROR(ENOSPC);
		}
		snprintf(current, sizeof(current), "%s", next);
	}

	if (cfg->lookahead_limiter_enabled) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (graph_append(graph, size,
				 "[%s]alimiter=limit=%.12g:attack=%.9g:release=%.9g:"
				 "level=0:latency=0[%s];",
				 current, db_to_linear(cfg->lookahead_limit_dbfs),
				 clamp(cfg->lookahead_ms, 0.1, 80.0), cfg->lookahead_release_ms,
				 next) < 0) {
			return AVERROR(ENOSPC);
		}
		snprintf(current, sizeof(current), "%s", next);
	}

	if (cfg->post_limiter_lowpass_enabled) {
		char rejected[NAME_SIZE];
		snprintf(next, sizeof(next), "s%u", stage++);
		snprintf(rejected, sizeof(rejected), "cln%urej", stage);
		if (graph_append(graph, size,
				 "[%s]asplit=3[cleanupmain][cleanuppre][cleanupspec];"
				 "[cleanuppre]astats=metadata=1:reset=1:measure_perchannel=none:"
				 "measure_overall=Peak_level+RMS_level[cleanup_pre_meter];"
				 "[cleanupspec]acrossover=split=5000 8000:order=20th"
				 "[prebelow5][pre5to8][preabove8];"
				 "[prebelow5]anullsink;"
				 "[pre5to8]astats=metadata=1:reset=1:measure_perchannel=none:"
				 "measure_overall=RMS_level[cleanup_pre_5_8_meter];"
				 "[preabove8]astats=metadata=1:reset=1:measure_perchannel=none:"
				 "measure_overall=RMS_level[cleanup_pre_8_plus_meter];"
				 "[cleanupmain]acrossover=split=%.9g:order=20th[%s][%s];"
				 "[%s]anullsink;",
				 current, cfg->post_limiter_lowpass_hz, next, rejected,
				 rejected) < 0) {
			return AVERROR(ENOSPC);
		}
		snprintf(current, sizeof(current), "%s", next);
	}

	if (cfg->post_limiter_lowpass_enabled) {
		return graph_append(graph, size,
				    "[%s]asplit=2[postmain][postspec];"
				    "[postspec]acrossover=split=5000 8000:order=20th"
				    "[postbelow5][post5to8][postabove8];"
				    "[postbelow5]anullsink;"
				    "[post5to8]astats=metadata=1:reset=1:measure_perchannel=none:"
				    "measure_overall=RMS_level[cleanup_post_5_8_meter];"
				    "[postabove8]astats=metadata=1:reset=1:measure_perchannel=none:"
				    "measure_overall=RMS_level[cleanup_post_8_plus_meter];"
				    "[postmain]astats=metadata=1:reset=1:measure_perchannel=none:"
				    "measure_overall=Peak_level+RMS_level[out]",
				    current);
	}
	return graph_append(graph, size,
			    "[%s]astats=metadata=1:reset=1:measure_perchannel=none:measure_overall="
			    "Peak_level+RMS_level[out]",
			    current);
}

/** @brief Release the FFmpeg graph and its output FIFO.
 * @param state Processor or stream state owned by the caller.
 */
AVFILTER_PRIVATE void free_graph(struct txagc_avfilter *state)
{
	avfilter_graph_free(&state->graph);
	state->source = NULL;
	state->sink = NULL;
	state->meter_sink = NULL;
	state->cleanup_pre_sink = NULL;
	state->cleanup_pre_5_8_sink = NULL;
	state->cleanup_pre_8_plus_sink = NULL;
	state->cleanup_post_5_8_sink = NULL;
	state->cleanup_post_8_plus_sink = NULL;
	if (state->fifo) {
		av_audio_fifo_free(state->fifo);
		state->fifo = NULL;
	}
	state->configured = 0;
}

/** @brief Constrain the graph sink to double-precision mono audio.
 * @param sink FFmpeg filter sink.
 */
AVFILTER_PRIVATE void set_double_sample_format(AVFilterContext *sink)
{
	const int formats[] = {AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_NONE};
	av_opt_set_bin(sink, "sample_fmts", (const uint8_t *)formats, sizeof(formats[0]),
		       AV_OPT_SEARCH_CHILDREN);
}

/** @brief Rebuild the FFmpeg graph when its configuration or sample rate changes.
 * @param state Processor or stream state owned by the caller.
 * @param config Filter and dynamics settings for the shared FFmpeg graph.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int configure(struct txagc_avfilter *state, const struct txagc_config *config,
			       unsigned int sample_rate)
{
	const AVFilter *source_filter;
	const AVFilter *sink_filter;
	AVFilterInOut *inputs = NULL;
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *meter_input = NULL;
	AVFilterInOut *meter_tail = NULL;
	AVFilterInOut *cleanup_input;
	AVFilterContext **cleanup_sink_slots[] = {
		&state->cleanup_pre_sink,	  &state->cleanup_pre_5_8_sink,
		&state->cleanup_pre_8_plus_sink,  &state->cleanup_post_5_8_sink,
		&state->cleanup_post_8_plus_sink,
	};
	const char *cleanup_filter_names[] = {
		"cleanup_pre_sink",	 "cleanup_pre_5_8_sink",     "cleanup_pre_8_plus_sink",
		"cleanup_post_5_8_sink", "cleanup_post_8_plus_sink",
	};
	const char *cleanup_pad_names[] = {
		"cleanup_pre_meter",	  "cleanup_pre_5_8_meter",     "cleanup_pre_8_plus_meter",
		"cleanup_post_5_8_meter", "cleanup_post_8_plus_meter",
	};
	char args[256];
	char description[GRAPH_SIZE];
	int result;

	free_graph(state);
	result = build_description(description, sizeof(description), config, sample_rate);
	if (result < 0) {
		return result;
	}
	state->graph = avfilter_graph_alloc();
	if (!state->graph) {
		return AVERROR(ENOMEM);
	}
	source_filter = avfilter_get_by_name("abuffer");
	sink_filter = avfilter_get_by_name("abuffersink");
	snprintf(args, sizeof(args),
		 "time_base=1/%u:sample_rate=%u:sample_fmt=dbl:channel_layout=mono", sample_rate,
		 sample_rate);
	result = avfilter_graph_create_filter(&state->source, source_filter, "source", args, NULL,
					      state->graph);
	if (result < 0) {
		goto fail;
	}
	if (config->post_limiter_lowpass_enabled) {
		for (size_t index = 0; index < 5; ++index) {
			AVFilterContext **sink_slot = cleanup_sink_slots[index];
			result = avfilter_graph_create_filter(sink_slot, sink_filter,
							      cleanup_filter_names[index], NULL,
							      NULL, state->graph);
			if (result < 0) {
				goto fail;
			}
			set_double_sample_format(*sink_slot);
		}
	}
	result = avfilter_graph_create_filter(&state->sink, sink_filter, "sink", NULL, NULL,
					      state->graph);
	if (result < 0) {
		goto fail;
	}
	result = avfilter_graph_create_filter(&state->meter_sink, sink_filter, "meter_sink", NULL,
					      NULL, state->graph);
	if (result < 0) {
		goto fail;
	}
	set_double_sample_format(state->sink);
	set_double_sample_format(state->meter_sink);

	outputs = avfilter_inout_alloc();
	inputs = avfilter_inout_alloc();
	meter_input = avfilter_inout_alloc();
	if (!outputs || !inputs || !meter_input) {
		result = AVERROR(ENOMEM);
		goto fail;
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = state->source;
	outputs->pad_idx = 0;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = state->sink;
	inputs->pad_idx = 0;
	inputs->next = meter_input;
	meter_tail = meter_input;
	meter_input->name = av_strdup("in_meter");
	meter_input->filter_ctx = state->meter_sink;
	meter_input->pad_idx = 0;
	if (config->post_limiter_lowpass_enabled) {
		for (size_t index = 0; index < 5; ++index) {
			cleanup_input = avfilter_inout_alloc();
			if (!cleanup_input) {
				result = AVERROR(ENOMEM);
				goto fail;
			}
			meter_tail->next = cleanup_input;
			meter_tail = cleanup_input;
			cleanup_input->name = av_strdup(cleanup_pad_names[index]);
			cleanup_input->filter_ctx = *cleanup_sink_slots[index];
			cleanup_input->pad_idx = 0;
		}
	}
	result = avfilter_graph_parse_ptr(state->graph, description, &inputs, &outputs, NULL);
	if (result < 0) {
		goto fail;
	}
	result = avfilter_graph_config(state->graph, NULL);
	if (result < 0) {
		goto fail;
	}
	state->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_DBL, 1, (int)(sample_rate / 2));
	if (!state->fifo) {
		result = AVERROR(ENOMEM);
		goto fail;
	}
	memcpy(&state->config, config, sizeof(*config));
	state->sample_rate = sample_rate;
	state->configured = 1;
	state->failed = 0;
	state->latency_samples = 0;
	state->buffered_samples = 0;
	state->output_started = 0;
	state->startup_fill_samples = 0;
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return 0;

fail:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	free_graph(state);
	state->failed = result;
	return result;
}

void txagc_avfilter_init(struct txagc_avfilter *state)
{
	memset(state, 0, sizeof(*state));
	state->input_peak_dbfs = state->input_max_peak_dbfs = -INFINITY;
	state->input_rms_dbfs = state->input_max_rms_dbfs = -INFINITY;
	state->output_peak_dbfs = state->output_max_peak_dbfs = -INFINITY;
	state->output_rms_dbfs = state->output_max_rms_dbfs = -INFINITY;
	state->cleanup_pre_peak_dbfs = state->cleanup_pre_max_peak_dbfs = -INFINITY;
	state->cleanup_pre_rms_dbfs = state->cleanup_pre_max_rms_dbfs = -INFINITY;
	state->cleanup_pre_5_8_rms_dbfs = state->cleanup_pre_5_8_max_rms_dbfs = -INFINITY;
	state->cleanup_pre_8_plus_rms_dbfs = state->cleanup_pre_8_plus_max_rms_dbfs = -INFINITY;
	state->cleanup_post_5_8_rms_dbfs = state->cleanup_post_5_8_max_rms_dbfs = -INFINITY;
	state->cleanup_post_8_plus_rms_dbfs = state->cleanup_post_8_plus_max_rms_dbfs = -INFINITY;
}

void txagc_avfilter_destroy(struct txagc_avfilter *state)
{
	free_graph(state);
	memset(state, 0, sizeof(*state));
}

void txagc_avfilter_reset(struct txagc_avfilter *state)
{
	free_graph(state);
}

/** @brief Drain available frames from one spectral measurement sink.
 * @param state Processor or stream state owned by the caller.
 * @param sink FFmpeg filter sink.
 * @param frame FFmpeg audio frame.
 * @param meter Spectral meter selected for this update.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
AVFILTER_PRIVATE int drain_cleanup_meter(struct txagc_avfilter *state, struct AVFilterContext *sink,
					 AVFrame *frame, enum cleanup_meter meter)
{
	int result;

	while ((result = av_buffersink_get_frame(sink, frame)) >= 0) {
		update_cleanup_astats(state, frame, meter);
		av_frame_unref(frame);
	}
	return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

int txagc_avfilter_process(struct txagc_avfilter *state, const struct txagc_config *config,
			   double *samples, size_t count, unsigned int sample_rate)
{
	AVFrame *input = NULL;
	AVFrame *output = NULL;
	int available;
	int result;
	int written;
	double *output_pointer;
	size_t copied = 0;

	/* Configurations are zero-initialized before fields are assigned, making
	 * their padding deterministic and this real-time comparison well-defined. */
	if (!state->configured || state->sample_rate != sample_rate ||
	    memcmp(&state->config, config, sizeof(*config)) != 0) { /* NOLINT */
		result = configure(state, config, sample_rate);
		if (result < 0) {
			return result;
		}
	}
	input = av_frame_alloc();
	output = av_frame_alloc();
	if (!input || !output) {
		result = AVERROR(ENOMEM);
		goto done;
	}
	input->format = AV_SAMPLE_FMT_DBL;
	input->sample_rate = (int)sample_rate;
	input->nb_samples = (int)count;
	av_channel_layout_default(&input->ch_layout, 1);
	result = av_frame_get_buffer(input, 0);
	if (result < 0) {
		goto done;
	}
	/* Asterisk/usbradio uses floating point with 16-bit PCM units while
	 * libavfilter's DBL format uses the normalized -1.0 .. +1.0 convention. */
	for (size_t index = 0; index < count; ++index) {
		((double *)input->data[0])[index] = samples[index] / 32768.0;
	}
	result = av_buffersrc_add_frame_flags(state->source, input, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (result < 0) {
		goto done;
	}
	state->input_samples += count;
	while ((result = av_buffersink_get_frame(state->meter_sink, output)) >= 0) {
		update_astats(state, output, 1);
		av_frame_unref(output);
	}
	if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
		goto done;
	}
	if (config->post_limiter_lowpass_enabled) {
		result = drain_cleanup_meter(state, state->cleanup_pre_sink, output,
					     CLEANUP_PRE_FULL);
		if (result < 0)
			goto done;
		result = drain_cleanup_meter(state, state->cleanup_pre_5_8_sink, output,
					     CLEANUP_PRE_5_8);
		if (result < 0)
			goto done;
		result = drain_cleanup_meter(state, state->cleanup_pre_8_plus_sink, output,
					     CLEANUP_PRE_8_PLUS);
		if (result < 0)
			goto done;
		result = drain_cleanup_meter(state, state->cleanup_post_5_8_sink, output,
					     CLEANUP_POST_5_8);
		if (result < 0)
			goto done;
		result = drain_cleanup_meter(state, state->cleanup_post_8_plus_sink, output,
					     CLEANUP_POST_8_PLUS);
		if (result < 0)
			goto done;
	}

	while ((result = av_buffersink_get_frame(state->sink, output)) >= 0) {
		void *fifo_data[1];
		update_astats(state, output, 0);
		if (av_audio_fifo_realloc(state->fifo, av_audio_fifo_size(state->fifo) +
							       output->nb_samples) < 0) {
			result = AVERROR(ENOMEM);
			goto done;
		}
		output_pointer = (double *)output->data[0];
		fifo_data[0] = output_pointer;
		written = av_audio_fifo_write(state->fifo, fifo_data, output->nb_samples);
		if (written != output->nb_samples) {
			result = AVERROR(EIO);
			goto done;
		}
		av_frame_unref(output);
	}
	if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
		goto done;
	}
	available = av_audio_fifo_size(state->fifo);
	copied = available < (int)count ? (size_t)available : count;
	if (!state->output_started) {
		size_t fill = count - copied;
		memset(samples, 0, fill * sizeof(*samples));
		state->startup_fill_samples += fill;
		state->underrun_samples += fill;
		if (copied) {
			output_pointer = samples + fill;
			av_audio_fifo_read(state->fifo, (void **)&output_pointer, (int)copied);
			for (size_t index = fill; index < count; ++index) {
				samples[index] *= 32768.0;
			}
			state->latency_samples = (unsigned int)state->startup_fill_samples;
			state->output_started = 1;
		}
	} else {
		if (copied) {
			output_pointer = samples;
			av_audio_fifo_read(state->fifo, (void **)&output_pointer, (int)copied);
			for (size_t index = 0; index < copied; ++index) {
				samples[index] *= 32768.0;
			}
		}
		if (copied < count) {
			memset(samples + copied, 0, (count - copied) * sizeof(*samples));
			state->underrun_samples += count - copied;
			state->runtime_underrun_samples += count - copied;
		}
	}
	state->buffered_samples = (unsigned int)av_audio_fifo_size(state->fifo);
	state->output_samples += copied;
	result = 0;

done:
	av_frame_free(&input);
	av_frame_free(&output);
	return result;
}

/** @name File-local and build-time constants
 * @{ */
/** @def AVFILTER_PRIVATE
 * @brief Expose FFmpeg internals only to the linked test harness.
 */
/** @def GRAPH_SIZE
 * @brief Maximum generated FFmpeg graph text in bytes.
 */
/** @def NAME_SIZE
 * @brief Capacity of an internal graph label.
 */
/** @def CTCSS_NOTCH_SECTIONS
 * @brief Cascaded FFmpeg notch sections used to reach the rejection target.
 */
/** @def URP_AGC_PLUGIN_PATH
 * @brief Private installed LADSPA effect path, or test-only search-path name.
 */
/** @} */
