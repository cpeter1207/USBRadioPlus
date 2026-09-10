/** @file
 * @brief Shared FFmpeg filtering, emphasis, equalization, dynamics, limiting, and meters.
 */

#ifndef USBRADIOPLUS_AVFILTER_PROCESSOR_H
#define USBRADIOPLUS_AVFILTER_PROCESSOR_H

#include <stddef.h>
#include <stdatomic.h>

#include "agc_core.h"

struct AVAudioFifo;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct txagc_avfilter_slot_node;

/** Number of preallocated source frames available to the real-time graph feeder. */
#define TXAGC_AVFILTER_INPUT_FRAME_COUNT 8

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
	/** Preallocated source-frame pool; the graph may retain a few input buffers. */
	struct AVFrame *input_frames[TXAGC_AVFILTER_INPUT_FRAME_COUNT];
	/** Reusable destination frame used while draining every FFmpeg sink. */
	struct AVFrame *output_frame;
	/** Capacity in samples of every preallocated source frame. */
	unsigned int input_capacity;
	/** Fixed capacity in samples of the output FIFO. */
	unsigned int fifo_capacity;
	/** Next source-frame-pool slot considered for a writable input block. */
	unsigned int input_frame_index;
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

/**
 * @brief Atomically published, control-plane-owned FFmpeg graph.
 *
 * Reconfiguration builds a complete replacement graph before publishing it.
 * The control plane and readers use sequentially consistent active-pointer and
 * reader-count operations.  A reader increments its count before loading the
 * active graph; replacement publishes a new pointer before observing that
 * count.  The single order therefore either pins the old graph or makes the
 * reader see the replacement, without a callback lock.
 */
struct txagc_avfilter_slot {
	/** Current prepared graph, or NULL before the first successful preparation. */
	_Atomic(struct txagc_avfilter *) active;
	/** Number of real-time calls which may still hold the active graph pointer. */
	_Atomic unsigned int readers;
	/** Serializes control-plane replacement and teardown operations. */
	atomic_flag writer;
	/** Control-plane owner of the graph currently published through active. */
	struct txagc_avfilter_slot_node *owned;
};

/** A complete graph candidate held off-slot until a multi-hook reload can commit. */
struct txagc_avfilter_slot_candidate {
	/** Private heap node containing the prepared FFmpeg graph. */
	struct txagc_avfilter_slot_node *node;
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

/** @brief Initialize an empty atomically published graph slot.
 * @param slot Slot owned by a channel or audiohook.
 */
void txagc_avfilter_slot_init(struct txagc_avfilter_slot *slot);
/** @brief Destroy every graph retained by a slot after its audio reader stops.
 * @param slot Slot whose reader has already been detached or stopped.
 */
void txagc_avfilter_slot_destroy(struct txagc_avfilter_slot *slot);
/** @brief Build an unpublished FFmpeg graph for an all-or-nothing reload.
 * @param candidate Receives the prepared candidate, initially zeroed by the caller.
 * @param config Filter and dynamics settings for the candidate.
 * @param sample_rate Source sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code with candidate empty on failure.
 *
 * This control-plane operation deliberately does not modify any active slot.
 * Pair it with txagc_avfilter_slot_publish_candidate() after every related
 * candidate has built successfully, or txagc_avfilter_slot_candidate_destroy().
 */
int txagc_avfilter_slot_candidate_prepare(struct txagc_avfilter_slot_candidate *candidate,
					  const struct txagc_config *config,
					  unsigned int sample_rate);
/** @brief Discard an unpublished candidate graph.
 * @param candidate Candidate prepared by txagc_avfilter_slot_candidate_prepare().
 */
void txagc_avfilter_slot_candidate_destroy(struct txagc_avfilter_slot_candidate *candidate);
/** @brief Atomically publish an already prepared candidate graph.
 * @param slot Destination graph slot.
 * @param candidate Candidate consumed on success or no-op replacement.
 * @return Zero on success; a negative error when either argument is invalid.
 *
 * This control-plane operation waits only for pre-existing readers while it
 * retires the previous graph; it never allocates and the callback never waits.
 */
int txagc_avfilter_slot_publish_candidate(struct txagc_avfilter_slot *slot,
					  struct txagc_avfilter_slot_candidate *candidate);
/** @brief Build and atomically publish a replacement graph from the control plane.
 * @param slot Slot to update.
 * @param config Filter and dynamics settings for the replacement graph.
 * @param sample_rate Source sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code without disturbing the active graph.
 *
 * This operation may allocate. Call it only while attaching/reloading a hook or
 * otherwise outside a real-time audio callback. A failed replacement leaves the
 * previous prepared graph active.
 */
int txagc_avfilter_slot_prepare(struct txagc_avfilter_slot *slot, const struct txagc_config *config,
				unsigned int sample_rate);
/** @brief Run the currently prepared slot graph without allocation or locking.
 * @param slot Slot prepared by txagc_avfilter_slot_prepare().
 * @param samples Mutable mono PCM buffer in 16-bit-code floating-point units.
 * @param count Number of samples in samples.
 * @return Zero on success; a negative FFmpeg error code when no prepared graph is available.
 */
int txagc_avfilter_slot_process_prepared(struct txagc_avfilter_slot *slot, double *samples,
					 size_t count);
/** @brief Obtain the graph currently published by a slot for non-real-time inspection.
 * @param slot Slot to inspect.
 * @return Borrowed graph pointer, or NULL when the slot has no prepared graph.
 *
 * This convenience accessor is appropriate only when the caller otherwise
 * prevents concurrent replacement. Use txagc_avfilter_slot_acquire() and
 * txagc_avfilter_slot_release() for an asynchronous statistics reader.
 */
struct txagc_avfilter *txagc_avfilter_slot_active(const struct txagc_avfilter_slot *slot);
/** @brief Retain the graph current at the instant of acquisition for inspection.
 * @param slot Slot to inspect.
 * @return Graph pointer that remains valid until txagc_avfilter_slot_release().
 */
struct txagc_avfilter *txagc_avfilter_slot_acquire(struct txagc_avfilter_slot *slot);
/** @brief Release a graph reference obtained by txagc_avfilter_slot_acquire().
 * @param slot Slot whose read-side reference is being released.
 */
void txagc_avfilter_slot_release(struct txagc_avfilter_slot *slot);
/** @brief Allocate or rebuild a shared FFmpeg graph outside the audio callback.
 * @param state Processor or stream state owned by the caller.
 * @param config Filter and dynamics settings for the shared FFmpeg graph.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 *
 * This control-plane operation creates the graph, its fixed FIFO, and a pool
 * of reusable frames.  Call it before using txagc_avfilter_process_prepared()
 * from a real-time audio callback and after each settings or rate change.
 */
int txagc_avfilter_prepare(struct txagc_avfilter *state, const struct txagc_config *config,
			   unsigned int sample_rate);
/** @brief Process one mono block through an already prepared FFmpeg graph.
 * @param state Processor prepared with txagc_avfilter_prepare().
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @return Zero on success; a negative FFmpeg error code on failure.
 *
 * This real-time path uses only preallocated processor-owned frames and FIFO
 * capacity.  It neither rebuilds the graph nor grows a buffer.  Blocks longer
 * than the prepared frame capacity fail instead of allocating in the callback.
 */
int txagc_avfilter_process_prepared(struct txagc_avfilter *state, double *samples, size_t count);
/** @brief Process a mono block in place through the shared FFmpeg graph.
 * @param state Processor or stream state owned by the caller.
 * @param config Filter and dynamics settings for the shared FFmpeg graph.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a negative FFmpeg error code on failure.
 *
 * This compatibility helper prepares the graph when its configuration or rate
 * changed, then calls txagc_avfilter_process_prepared().  Use the explicit
 * prepare/process-prepared pair in real-time callbacks so graph rebuilding and
 * allocation remain on the control plane.
 */
int txagc_avfilter_process(struct txagc_avfilter *state, const struct txagc_config *config,
			   double *samples, size_t count, unsigned int sample_rate);

#endif
