/** @file
 * @brief Private FFmpeg graph construction and meter-draining test interfaces.
 */

#ifndef TXAGC_AVFILTER_PROCESSOR_INTERNAL_H
#define TXAGC_AVFILTER_PROCESSOR_INTERNAL_H

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>

#include "avfilter_processor.h"

#ifdef URP_AVFILTER_TESTING
#define GRAPH_SIZE 16384

/** Spectral measurement taps around the fixed band-pass. */
enum cleanup_meter {
	CLEANUP_PRE_FULL /**< Full-band input before the fixed band-pass. */,
	CLEANUP_PRE_5_8 /**< 5–8 kHz input energy before filtering. */,
	CLEANUP_PRE_8_PLUS /**< Input energy above 8 kHz. */,
	CLEANUP_POST_5_8 /**< 5–8 kHz output energy after filtering. */,
	CLEANUP_POST_8_PLUS /**< Output energy above 8 kHz after filtering. */
};

double db_to_linear(double db);
double clamp(double value, double low, double high);
int graph_append(char *graph, size_t size, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
int astats_value(const AVFrame *frame, const char *name, double *value);
void update_astats(struct txagc_avfilter *state, const AVFrame *frame, int input);
void update_cleanup_astats(struct txagc_avfilter *state, const AVFrame *frame,
			   enum cleanup_meter meter);
int add_sidechain_stage(char *graph, size_t size, const char *input, const char *output,
			const char *prefix, const char *filter, double highpass, double lowpass,
			const char *options);
int add_brickwall_bandpass(char *graph, size_t size, const char *input, const char *output,
			   const char *prefix, double highpass, double lowpass);
int add_emphasis(char *graph, size_t size, const char *input, const char *output, int production,
		 double corner_hz, double reference_hz, unsigned int sample_rate);
int add_dynamic_stage(char *graph, size_t size, const struct txagc_config *cfg,
		      enum txagc_stage stage, const char *input, const char *output,
		      unsigned int sample_rate);
int build_description(char *graph, size_t size, const struct txagc_config *cfg,
		      unsigned int sample_rate);
void free_graph(struct txagc_avfilter *state);
void set_double_sample_format(AVFilterContext *sink);
int configure(struct txagc_avfilter *state, const struct txagc_config *config,
	      unsigned int sample_rate);
int drain_cleanup_meter(struct txagc_avfilter *state, AVFilterContext *sink, AVFrame *frame,
			enum cleanup_meter meter);
#endif

#endif
