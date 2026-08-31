#ifndef USBRADIOPLUS_AVFILTER_PROCESSOR_H
#define USBRADIOPLUS_AVFILTER_PROCESSOR_H

#include <stddef.h>

#include "agc_core.h"

struct AVAudioFifo;
struct AVFilterContext;
struct AVFilterGraph;

struct txagc_avfilter {
	struct AVFilterGraph *graph;
	struct AVFilterContext *source;
	struct AVFilterContext *sink;
	struct AVFilterContext *meter_sink;
	struct AVFilterContext *cleanup_pre_sink;
	struct AVFilterContext *cleanup_pre_5_8_sink;
	struct AVFilterContext *cleanup_pre_8_plus_sink;
	struct AVFilterContext *cleanup_post_5_8_sink;
	struct AVFilterContext *cleanup_post_8_plus_sink;
	struct AVAudioFifo *fifo;
	struct txagc_config config;
	unsigned int sample_rate;
	int configured;
	int failed;
	unsigned int latency_samples;
	unsigned int buffered_samples;
	int output_started;
	unsigned long long input_samples;
	unsigned long long output_samples;
	unsigned long long underrun_samples;
	unsigned long long startup_fill_samples;
	unsigned long long runtime_underrun_samples;
	double input_peak_dbfs;
	double input_max_peak_dbfs;
	double input_rms_dbfs;
	double input_max_rms_dbfs;
	double output_peak_dbfs;
	double output_max_peak_dbfs;
	double output_rms_dbfs;
	double output_max_rms_dbfs;
	double cleanup_pre_peak_dbfs;
	double cleanup_pre_max_peak_dbfs;
	double cleanup_pre_rms_dbfs;
	double cleanup_pre_max_rms_dbfs;
	double cleanup_pre_5_8_rms_dbfs;
	double cleanup_pre_5_8_max_rms_dbfs;
	double cleanup_pre_8_plus_rms_dbfs;
	double cleanup_pre_8_plus_max_rms_dbfs;
	double cleanup_post_5_8_rms_dbfs;
	double cleanup_post_5_8_max_rms_dbfs;
	double cleanup_post_8_plus_rms_dbfs;
	double cleanup_post_8_plus_max_rms_dbfs;
};

void txagc_avfilter_init(struct txagc_avfilter *state);
void txagc_avfilter_destroy(struct txagc_avfilter *state);
void txagc_avfilter_reset(struct txagc_avfilter *state);
int txagc_avfilter_process(struct txagc_avfilter *state,
	const struct txagc_config *config, double *samples, size_t count,
	unsigned int sample_rate);

#endif
