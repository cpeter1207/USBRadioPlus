/** @file
 * @brief Shared FFmpeg filtering, emphasis, equalization, dynamics, limiting, and meters.
 */

#ifndef USBRADIOPLUS_AVFILTER_PROCESSOR_H
#define USBRADIOPLUS_AVFILTER_PROCESSOR_H

#include <stddef.h>

#include "agc_core.h"

struct AVAudioFifo;
struct AVFilterContext;
struct AVFilterGraph;

/** Owned FFmpeg graph, frame FIFO, latency tracking, and per-stage measurements. */
struct txagc_avfilter {
	/** Owned FFmpeg filter graph. */
	struct AVFilterGraph *graph;
	/** Input buffer or source endpoint for this state. */
	struct AVFilterContext *source;
	/** Output buffer or sink endpoint for this state. */
	struct AVFilterContext *sink;
	/** FFmpeg sink for input-level measurements. */
	struct AVFilterContext *meter_sink;
	/** Unfiltered spectral measurement sink. */
	struct AVFilterContext *cleanup_pre_sink;
	/** Input spectral meter for the 5 to 8 kHz band. */
	struct AVFilterContext *cleanup_pre_5_8_sink;
	/** Input spectral meter above 8 kHz. */
	struct AVFilterContext *cleanup_pre_8_plus_sink;
	/** Output spectral meter for the 5 to 8 kHz band. */
	struct AVFilterContext *cleanup_post_5_8_sink;
	/** Output spectral meter above 8 kHz. */
	struct AVFilterContext *cleanup_post_8_plus_sink;
	/** Owned FIFO of filtered samples waiting for the caller's output block. */
	struct AVAudioFifo *fifo;
	/** Settings used to construct the current graph. */
	struct txagc_config config;
	/** Current stream sample rate in Hz. */
	unsigned int sample_rate;
	/** Nonzero after the processor is configured successfully. */
	int configured;
	/** Nonzero after graph configuration or runtime processing fails. */
	int failed;
	/** Filter latency expressed in source-rate samples. */
	unsigned int latency_samples;
	/** Current output FIFO occupancy in samples. */
	unsigned int buffered_samples;
	/** Nonzero once filter startup latency has been filled. */
	int output_started;
	/** Total samples submitted to the graph. */
	unsigned long long input_samples;
	/** Total output samples delivered. */
	unsigned long long output_samples;
	/** Total output samples unavailable at the requested time. */
	unsigned long long underrun_samples;
	/** Silence samples emitted while filling initial filter latency. */
	unsigned long long startup_fill_samples;
	/** Missing output samples after startup has completed. */
	unsigned long long runtime_underrun_samples;
	/** Input peak in DBFS. */
	double input_peak_dbfs;
	/** Input max peak in DBFS. */
	double input_max_peak_dbfs;
	/** Input RMS in DBFS. */
	double input_rms_dbfs;
	/** Input max RMS in DBFS. */
	double input_max_rms_dbfs;
	/** Output peak in DBFS. */
	double output_peak_dbfs;
	/** Output max peak in DBFS. */
	double output_max_peak_dbfs;
	/** Output RMS in DBFS. */
	double output_rms_dbfs;
	/** Output max RMS in DBFS. */
	double output_max_rms_dbfs;
	/** Cleanup pre peak in DBFS. */
	double cleanup_pre_peak_dbfs;
	/** Cleanup pre max peak in DBFS. */
	double cleanup_pre_max_peak_dbfs;
	/** Cleanup pre RMS in DBFS. */
	double cleanup_pre_rms_dbfs;
	/** Cleanup pre max RMS in DBFS. */
	double cleanup_pre_max_rms_dbfs;
	/** Cleanup pre 5 8 RMS in DBFS. */
	double cleanup_pre_5_8_rms_dbfs;
	/** Cleanup pre 5 8 max RMS in DBFS. */
	double cleanup_pre_5_8_max_rms_dbfs;
	/** Cleanup pre 8 plus RMS in DBFS. */
	double cleanup_pre_8_plus_rms_dbfs;
	/** Cleanup pre 8 plus max RMS in DBFS. */
	double cleanup_pre_8_plus_max_rms_dbfs;
	/** Cleanup post 5 8 RMS in DBFS. */
	double cleanup_post_5_8_rms_dbfs;
	/** Cleanup post 5 8 max RMS in DBFS. */
	double cleanup_post_5_8_max_rms_dbfs;
	/** Cleanup post 8 plus RMS in DBFS. */
	double cleanup_post_8_plus_rms_dbfs;
	/** Cleanup post 8 plus max RMS in DBFS. */
	double cleanup_post_8_plus_max_rms_dbfs;
};

/** @brief Initialize an empty FFmpeg processor and its level statistics.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_avfilter_init(struct txagc_avfilter *state);
/** @brief Release all resources owned by an FFmpeg processor.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_avfilter_destroy(struct txagc_avfilter *state);
/** @brief Discard buffered filter history while retaining a reusable processor state.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_avfilter_reset(struct txagc_avfilter *state);
/** @brief Process a mono block in place through the shared FFmpeg graph.
 * @param state Processor or stream state owned by the caller.
 * @param config Filter and dynamics settings for the shared FFmpeg graph.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 */
int txagc_avfilter_process(struct txagc_avfilter *state, const struct txagc_config *config,
			   double *samples, size_t count, unsigned int sample_rate);

#endif
