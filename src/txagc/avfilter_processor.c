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
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#define GRAPH_SIZE 16384
#define NAME_SIZE 32
#define CTCSS_NOTCH_SECTIONS 8

static double db_to_linear(double db)
{
	return pow(10.0, db / 20.0);
}

static double clamp(double value, double low, double high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static int graph_append(char *graph, size_t size, const char *format, ...)
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
	if (result < 0 || (size_t) result >= size - used) {
		return AVERROR(ENOSPC);
	}
	return 0;
}

static int astats_value(const AVFrame *frame, const char *name, double *value)
{
	AVDictionaryEntry *entry = av_dict_get(frame->metadata, name, NULL, 0);
	char *end;
	if (!entry || !entry->value) {
		return -1;
	}
	*value = strtod(entry->value, &end);
	return end != entry->value ? 0 : -1;
}

static void update_astats(struct txagc_avfilter *state, const AVFrame *frame,
	int input)
{
	double peak;
	double rms;
	if (astats_value(frame, "lavfi.astats.Overall.Peak_level", &peak)
			|| astats_value(frame, "lavfi.astats.Overall.RMS_level", &rms)
			|| rms <= -90.0) {
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

enum cleanup_meter {
	CLEANUP_PRE_FULL,
	CLEANUP_PRE_5_8,
	CLEANUP_PRE_8_PLUS,
	CLEANUP_POST_5_8,
	CLEANUP_POST_8_PLUS,
};

static void update_cleanup_astats(struct txagc_avfilter *state,
	const AVFrame *frame, enum cleanup_meter meter)
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

static int add_sidechain_stage(char *graph, size_t size, const char *input,
	const char *output, const char *prefix, const char *filter,
	double highpass, double lowpass, const char *options)
{
	return graph_append(graph, size,
		"[%s]asplit=2[%smain][%ssc];"
		"[%ssc]highpass=f=%.9g:p=2,lowpass=f=%.9g:p=2[%sdet];"
		"[%smain][%sdet]%s=%s[%s];",
		input, prefix, prefix, prefix, highpass, lowpass, prefix,
		prefix, prefix, filter, options, output);
}

static int add_brickwall_bandpass(char *graph, size_t size, const char *input,
	const char *output, const char *prefix, double highpass, double lowpass)
{
	return graph_append(graph, size,
		"[%s]acrossover=split=%.9g:order=20th[%slo][%spass];"
		"[%slo]anullsink;"
		"[%spass]acrossover=split=%.9g:order=20th[%s][%shi];"
		"[%shi]anullsink;",
		input, highpass, prefix, prefix, prefix,
		prefix, lowpass, output, prefix, prefix);
}

static int add_emphasis(char *graph, size_t size, const char *input,
	const char *output, int production, double corner_hz,
	double reference_hz, unsigned int sample_rate)
{
	double pole = exp(-2.0 * M_PI * corner_hz / sample_rate);
	double omega = 2.0 * M_PI * reference_hz / sample_rate;
	double inverse_at_reference = sqrt(1.0 + pole * pole
		- 2.0 * pole * cos(omega)) / (1.0 - pole);
	if (production) {
		double scale = 1.0 / inverse_at_reference;
		return graph_append(graph, size,
			"[%s]biquad=b0=%.17g:b1=%.17g:b2=0:a0=1:a1=0:a2=0:"
			"precision=f64[%s];",
			input, scale / (1.0 - pole),
			-pole * scale / (1.0 - pole), output);
	}
	return graph_append(graph, size,
		"[%s]biquad=b0=%.17g:b1=0:b2=0:a0=1:a1=%.17g:a2=0:"
		"precision=f64[%s];",
		input, inverse_at_reference * (1.0 - pole), -pole, output);
}

static int add_dynamic_stage(char *graph, size_t size,
	const struct txagc_config *cfg, enum txagc_stage kind,
	const char *current, const char *next, unsigned int serial)
{
	char options[1024];
	char prefix[32];
	double target;
	double max_gain;

	snprintf(prefix, sizeof(prefix), "d%u", serial);
	switch (kind) {
	case TXAGC_STAGE_DEESSER: {
		double octave_ratio;
		double q;
		if (!cfg->deesser_enabled) return 0;
		octave_ratio = pow(2.0, cfg->deesser_width_octaves);
		q = sqrt(octave_ratio) / (octave_ratio - 1.0);
#if LIBAVFILTER_VERSION_MAJOR >= 9
		return graph_append(graph, size,
			"[%s]adynamicequalizer=threshold=%.12g:dfrequency=%.9g:"
			"dqfactor=%.12g:tfrequency=%.9g:tqfactor=%.12g:"
			"attack=%.9g:release=%.9g:ratio=%.9g:range=%.12g:"
			"mode=cutabove:dftype=bandpass:tftype=bell:precision=double[%s];",
			current, db_to_linear(cfg->deesser_threshold_dbfs),
			cfg->deesser_frequency_hz, q, cfg->deesser_frequency_hz, q,
			cfg->deesser_attack_ms, cfg->deesser_release_ms,
			cfg->deesser_ratio, db_to_linear(cfg->deesser_max_reduction_db), next);
#else
		/* FFmpeg 5.1 uses fixed band-pass detection and names cut-above "cut". */
		return graph_append(graph, size,
			"[%s]adynamicequalizer=threshold=%.12g:dfrequency=%.9g:"
			"dqfactor=%.12g:tfrequency=%.9g:tqfactor=%.12g:"
			"attack=%.9g:release=%.9g:ratio=%.9g:range=%.12g:"
			"mode=cut:tftype=bell[%s];",
			current, db_to_linear(cfg->deesser_threshold_dbfs),
			cfg->deesser_frequency_hz, q, cfg->deesser_frequency_hz, q,
			cfg->deesser_attack_ms, cfg->deesser_release_ms,
			cfg->deesser_ratio, db_to_linear(cfg->deesser_max_reduction_db), next);
#endif
	}
	case TXAGC_STAGE_EQUALIZER:
		if (!cfg->equalizer_enabled) return 0;
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
		if (!cfg->agc_enabled) return 0;
		target = clamp(db_to_linear(cfg->target_dbfs), 0.000001, 1.0);
		max_gain = clamp(db_to_linear(cfg->max_gain_db), 1.0, 100.0);
		return graph_append(graph, size,
			"[%s]dynaudnorm=framelen=10:gausssize=3:peak=1:"
			"maxgain=%.12g:targetrms=%.12g:threshold=%.12g:"
			"overlap=0.5:correctdc=0[%s];",
			current, max_gain, target,
			clamp(db_to_linear(cfg->agc_floor_dbfs), 0.0, 1.0), next);
	case TXAGC_STAGE_EXPANDER:
		if (!cfg->expander_enabled) return 0;
		snprintf(options, sizeof(options),
			"threshold=%.12g:ratio=%.9g:range=%.12g:attack=%.9g:"
			"release=%.9g:knee=2.828427:detection=rms",
			db_to_linear(cfg->expander_threshold_dbfs),
			clamp(cfg->expander_ratio, 1.0, 20.0),
			db_to_linear(-fabs(cfg->expander_max_attenuation_db)),
			cfg->expander_attack_ms, cfg->expander_release_ms);
		return add_sidechain_stage(graph, size, current, next, prefix,
			"sidechaingate", cfg->expander_sidechain_highpass_hz,
			cfg->expander_sidechain_lowpass_hz, options);
	case TXAGC_STAGE_COMPRESSOR:
		if (!cfg->compressor_enabled) return 0;
		snprintf(options, sizeof(options),
			"mode=downward:threshold=%.12g:ratio=%.9g:attack=%.9g:"
			"release=%.9g:makeup=%.12g:knee=2.828427:detection=rms",
			db_to_linear(cfg->compressor_threshold_dbfs),
			clamp(cfg->compressor_ratio, 1.0, 20.0),
			cfg->compressor_attack_ms, cfg->compressor_release_ms,
			db_to_linear(cfg->compressor_makeup_gain_db));
		return add_sidechain_stage(graph, size, current, next, prefix,
			"sidechaincompress", cfg->compressor_sidechain_highpass_hz,
			cfg->compressor_sidechain_lowpass_hz, options);
	case TXAGC_STAGE_LIMITER:
		if (!cfg->limiter_enabled) return 0;
		return graph_append(graph, size,
			"[%s]acrossover=split=%.9g:order=4th[%slo][%shi];"
			"[%slo]acompressor=threshold=%.12g:ratio=%.9g:attack=%.9g:"
			"release=%.9g:knee=%.12g:detection=peak[%sloc];"
			"[%shi]acompressor=threshold=%.12g:ratio=%.9g:attack=%.9g:"
			"release=%.9g:knee=%.12g:detection=peak[%shic];"
			"[%sloc][%shic]amix=inputs=2:normalize=0[%s];",
			current, cfg->limiter_crossover_hz, prefix, prefix,
			prefix, db_to_linear(cfg->low_limiter_threshold_dbfs),
			clamp(cfg->low_limiter_ratio, 1.0, 20.0),
			cfg->low_limiter_attack_ms, cfg->low_limiter_release_ms,
			db_to_linear(cfg->low_limiter_knee_db), prefix,
			prefix, db_to_linear(cfg->high_clip_dbfs),
			clamp(cfg->high_limiter_ratio, 1.0, 20.0),
			cfg->high_limiter_attack_ms, cfg->high_limiter_release_ms,
			db_to_linear(cfg->high_limiter_knee_db), prefix,
			prefix, prefix, next);
	}
	return AVERROR(EINVAL);
}

static int build_description(char *graph, size_t size,
	const struct txagc_config *cfg, unsigned int sample_rate)
{
	char current[NAME_SIZE] = "s0";
	const char *graph_input = "in";
	char next[NAME_SIZE];
	unsigned int stage = 1;
	unsigned int order_index;

	graph[0] = '\0';
	if (cfg->deemphasis_enabled) {
		if (add_emphasis(graph, size, "in", "deemphasized", 0,
				cfg->emphasis_corner_hz, cfg->emphasis_reference_hz,
				sample_rate) < 0) {
			return AVERROR(ENOSPC);
		}
		graph_input = "deemphasized";
	}
	if (cfg->receive_bandpass_enabled) {
		if (add_brickwall_bandpass(graph, size, graph_input, "rxbandpass",
				"rxbp", cfg->receive_bandpass_highpass_hz,
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
			if (frequency < 50.0 || frequency > 300.0) continue;
			/* Higher order supplies 50 dB rejection at the TIA-603
			 * frequency-tolerance edges without widening into speech. */
			for (section = 0; section < CTCSS_NOTCH_SECTIONS; ++section) {
				snprintf(output_name, sizeof(output_name), "ctn%u_%u", notch, section);
				if (graph_append(graph, size,
						"[%s]bandreject=f=%.9g:t=h:w=%.9g:r=f64[%s];",
						input_name, frequency, cfg->ctcss_notch_width_hz,
						output_name) < 0) return AVERROR(ENOSPC);
				strcpy(input_name, output_name);
			}
			++notch;
		}
		if (notch) graph_input = strcpy(next, input_name);
	} else if (cfg->ctcss_filter_mode == TXAGC_CTCSS_FILTER_HIGHPASS
			&& cfg->ctcss_highpass_hz > 0.0) {
		if (graph_append(graph, size,
				"[%s]acrossover=split=%.9g:order=20th[ctlow][cthigh];"
				"[ctlow]anullsink;",
				graph_input, cfg->ctcss_highpass_hz) < 0) return AVERROR(ENOSPC);
		graph_input = "cthigh";
	}
	if (graph_append(graph, size,
			"[%s]asplit=2[programin][metertap];"
			"[metertap]astats=metadata=1:reset=1:measure_perchannel=none:measure_overall=Peak_level+RMS_level[in_meter];"
			"[programin]volume=%.12g[%s];",
			graph_input, db_to_linear(cfg->input_gain_db), current) < 0) {
		return AVERROR(ENOSPC);
	}

	for (order_index = 0; order_index < cfg->stage_count; ++order_index) {
		int enabled = (cfg->stage_order[order_index] == TXAGC_STAGE_AGC && cfg->agc_enabled)
			|| (cfg->stage_order[order_index] == TXAGC_STAGE_EXPANDER && cfg->expander_enabled)
			|| (cfg->stage_order[order_index] == TXAGC_STAGE_COMPRESSOR && cfg->compressor_enabled)
			|| (cfg->stage_order[order_index] == TXAGC_STAGE_LIMITER && cfg->limiter_enabled)
			|| (cfg->stage_order[order_index] == TXAGC_STAGE_EQUALIZER
				&& cfg->equalizer_enabled)
			|| (cfg->stage_order[order_index] == TXAGC_STAGE_DEESSER
				&& cfg->deesser_enabled);
		if (!enabled) continue;
		snprintf(next, sizeof(next), "s%u", stage++);
		if (add_dynamic_stage(graph, size, cfg, cfg->stage_order[order_index],
				current, next, order_index) < 0) return AVERROR(ENOSPC);
		strcpy(current, next);
	}

	if (cfg->preemphasis_enabled) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (add_emphasis(graph, size, current, next, 1,
				cfg->emphasis_corner_hz, cfg->emphasis_reference_hz,
				sample_rate) < 0) {
			return AVERROR(ENOSPC);
		}
		strcpy(current, next);
	}

	if (cfg->splatter_filter_enabled && cfg->output_highpass_hz > 0.0) {
		char rejected[NAME_SIZE];
		snprintf(next, sizeof(next), "s%u", stage++);
		snprintf(rejected, sizeof(rejected), "hp%urej", stage);
		if (graph_append(graph, size,
				"[%s]acrossover=split=%.9g:order=20th[%s][%s];[%s]anullsink;",
				current, cfg->output_highpass_hz, rejected, next, rejected) < 0)
			return AVERROR(ENOSPC);
		strcpy(current, next);
	}
	if (cfg->splatter_filter_enabled && cfg->output_lowpass_hz > 0.0) {
		char rejected[NAME_SIZE];
		snprintf(next, sizeof(next), "s%u", stage++);
		snprintf(rejected, sizeof(rejected), "lp%urej", stage);
		if (graph_append(graph, size,
				"[%s]acrossover=split=%.9g:order=20th[%s][%s];[%s]anullsink;",
				current, cfg->output_lowpass_hz, next, rejected, rejected) < 0)
			return AVERROR(ENOSPC);
		strcpy(current, next);
	}

	/* Output gain is part of the signal presented to the final lookahead
	 * limiter.  Applying it afterward would defeat the configured ceiling and
	 * force the integer-boundary safety clamp to act as a clipper. */
	if (cfg->output_gain_db != 0.0) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (graph_append(graph, size, "[%s]volume=%.12g[%s];",
				current, db_to_linear(cfg->output_gain_db), next) < 0) {
			return AVERROR(ENOSPC);
		}
		strcpy(current, next);
	}

	if (cfg->lookahead_limiter_enabled) {
		snprintf(next, sizeof(next), "s%u", stage++);
		if (graph_append(graph, size,
				"[%s]alimiter=limit=%.12g:attack=%.9g:release=%.9g:"
				"level=0:latency=0[%s];",
				current, db_to_linear(cfg->lookahead_limit_dbfs),
				clamp(cfg->lookahead_ms, 0.1, 80.0),
				cfg->lookahead_release_ms, next) < 0) {
			return AVERROR(ENOSPC);
		}
		strcpy(current, next);
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
		strcpy(current, next);
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
		"[%s]astats=metadata=1:reset=1:measure_perchannel=none:measure_overall=Peak_level+RMS_level[out]",
		current);
}

static void free_graph(struct txagc_avfilter *state)
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

static int configure(struct txagc_avfilter *state,
	const struct txagc_config *config, unsigned int sample_rate)
{
	const AVFilter *source_filter;
	const AVFilter *sink_filter;
	AVFilterInOut *inputs = NULL;
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *meter_input = NULL;
	AVFilterInOut *meter_tail = NULL;
	AVFilterInOut *cleanup_input;
	AVFilterContext **cleanup_sinks[] = {
		&state->cleanup_pre_sink,
		&state->cleanup_pre_5_8_sink,
		&state->cleanup_pre_8_plus_sink,
		&state->cleanup_post_5_8_sink,
		&state->cleanup_post_8_plus_sink,
	};
	const char *cleanup_filter_names[] = {
		"cleanup_pre_sink", "cleanup_pre_5_8_sink",
		"cleanup_pre_8_plus_sink", "cleanup_post_5_8_sink",
		"cleanup_post_8_plus_sink",
	};
	const char *cleanup_pad_names[] = {
		"cleanup_pre_meter", "cleanup_pre_5_8_meter",
		"cleanup_pre_8_plus_meter", "cleanup_post_5_8_meter",
		"cleanup_post_8_plus_meter",
	};
	char args[256];
	char description[GRAPH_SIZE];
	const int sample_formats[] = { AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_NONE };
	int result;

	free_graph(state);
	result = build_description(description, sizeof(description), config,
		sample_rate);
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
		"time_base=1/%u:sample_rate=%u:sample_fmt=dbl:channel_layout=mono",
		sample_rate, sample_rate);
	result = avfilter_graph_create_filter(&state->source, source_filter, "source",
		args, NULL, state->graph);
	if (result < 0) {
		goto fail;
	}
	if (config->post_limiter_lowpass_enabled) {
		for (size_t index = 0; index < 5; ++index) {
			result = avfilter_graph_create_filter(cleanup_sinks[index],
				sink_filter, cleanup_filter_names[index], NULL, NULL,
				state->graph);
			if (result < 0) {
				goto fail;
			}
			av_opt_set_int_list(*cleanup_sinks[index], "sample_fmts",
				sample_formats, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
		}
	}
	result = avfilter_graph_create_filter(&state->sink, sink_filter, "sink",
		NULL, NULL, state->graph);
	if (result < 0) {
		goto fail;
	}
	result = avfilter_graph_create_filter(&state->meter_sink, sink_filter,
		"meter_sink", NULL, NULL, state->graph);
	if (result < 0) {
		goto fail;
	}
	av_opt_set_int_list(state->sink, "sample_fmts", sample_formats,
		AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int_list(state->meter_sink, "sample_fmts", sample_formats,
		AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

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
			cleanup_input->filter_ctx = *cleanup_sinks[index];
			cleanup_input->pad_idx = 0;
		}
	}
	result = avfilter_graph_parse_ptr(state->graph, description, &inputs, &outputs,
		NULL);
	if (result < 0) {
		goto fail;
	}
	result = avfilter_graph_config(state->graph, NULL);
	if (result < 0) {
		goto fail;
	}
	state->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_DBL, 1, sample_rate / 2);
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
	state->cleanup_pre_5_8_rms_dbfs =
		state->cleanup_pre_5_8_max_rms_dbfs = -INFINITY;
	state->cleanup_pre_8_plus_rms_dbfs =
		state->cleanup_pre_8_plus_max_rms_dbfs = -INFINITY;
	state->cleanup_post_5_8_rms_dbfs =
		state->cleanup_post_5_8_max_rms_dbfs = -INFINITY;
	state->cleanup_post_8_plus_rms_dbfs =
		state->cleanup_post_8_plus_max_rms_dbfs = -INFINITY;
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

static int drain_cleanup_meter(struct txagc_avfilter *state,
	struct AVFilterContext *sink, AVFrame *frame, enum cleanup_meter meter)
{
	int result;

	while ((result = av_buffersink_get_frame(sink, frame)) >= 0) {
		update_cleanup_astats(state, frame, meter);
		av_frame_unref(frame);
	}
	return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

int txagc_avfilter_process(struct txagc_avfilter *state,
	const struct txagc_config *config, double *samples, size_t count,
	unsigned int sample_rate)
{
	AVFrame *input = NULL;
	AVFrame *output = NULL;
	int available;
	int result;
	int written;
	double *output_pointer;
	size_t copied = 0;

	if (!state->configured || state->sample_rate != sample_rate ||
			memcmp(&state->config, config, sizeof(*config))) {
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
	input->sample_rate = sample_rate;
	input->nb_samples = (int) count;
	av_channel_layout_default(&input->ch_layout, 1);
	result = av_frame_get_buffer(input, 0);
	if (result < 0) {
		goto done;
	}
	/* Asterisk/usbradio uses floating point with 16-bit PCM units while
	 * libavfilter's DBL format uses the normalized -1.0 .. +1.0 convention. */
	for (size_t index = 0; index < count; ++index) {
		((double *) input->data[0])[index] = samples[index] / 32768.0;
	}
	result = av_buffersrc_add_frame_flags(state->source, input,
		AV_BUFFERSRC_FLAG_KEEP_REF);
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
		if (result < 0) goto done;
		result = drain_cleanup_meter(state, state->cleanup_pre_5_8_sink, output,
			CLEANUP_PRE_5_8);
		if (result < 0) goto done;
		result = drain_cleanup_meter(state, state->cleanup_pre_8_plus_sink, output,
			CLEANUP_PRE_8_PLUS);
		if (result < 0) goto done;
		result = drain_cleanup_meter(state, state->cleanup_post_5_8_sink, output,
			CLEANUP_POST_5_8);
		if (result < 0) goto done;
		result = drain_cleanup_meter(state, state->cleanup_post_8_plus_sink, output,
			CLEANUP_POST_8_PLUS);
		if (result < 0) goto done;
	}

	while ((result = av_buffersink_get_frame(state->sink, output)) >= 0) {
		void *fifo_data[1];
		update_astats(state, output, 0);
		if (av_audio_fifo_realloc(state->fifo,
				av_audio_fifo_size(state->fifo) + output->nb_samples) < 0) {
			result = AVERROR(ENOMEM);
			goto done;
		}
		output_pointer = (double *) output->data[0];
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
	copied = available < (int) count ? (size_t) available : count;
	if (!state->output_started) {
		size_t fill = count - copied;
		memset(samples, 0, fill * sizeof(*samples));
		state->startup_fill_samples += fill;
		state->underrun_samples += fill;
		if (copied) {
			output_pointer = samples + fill;
			av_audio_fifo_read(state->fifo, (void **) &output_pointer,
				(int) copied);
			for (size_t index = fill; index < count; ++index) {
				samples[index] *= 32768.0;
			}
			state->latency_samples = (unsigned int) state->startup_fill_samples;
			state->output_started = 1;
		}
	} else {
		if (copied) {
			output_pointer = samples;
			av_audio_fifo_read(state->fifo, (void **) &output_pointer,
				(int) copied);
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
	state->buffered_samples = (unsigned int) av_audio_fifo_size(state->fifo);
	state->output_samples += copied;
	result = 0;

done:
	av_frame_free(&input);
	av_frame_free(&output);
	return result;
}
