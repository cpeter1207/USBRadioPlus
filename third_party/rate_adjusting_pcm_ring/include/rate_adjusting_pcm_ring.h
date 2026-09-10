/* SPDX-License-Identifier: GPL-2.0-only */
/** @file
 * @brief Lock-free single-producer/single-consumer PCM ring with clock
 * recovery.
 */
#ifndef RATE_ADJUSTING_PCM_RING_H
#define RATE_ADJUSTING_PCM_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SRC_STATE_tag;

/** @brief libsamplerate quality selections supported by the ring. */
enum rpcr_quality {
  RPCR_SINC_BEST,
  RPCR_SINC_MEDIUM,
  RPCR_SINC_FASTEST,
};

/** @brief Lock-free consumer/controller measurements captured by @ref
 * rpcr_observe. */
struct rpcr_observation {
  size_t capacity_samples;  /**< Fixed raw-ring capacity. */
  size_t available_samples; /**< PCM currently readable from the producer. */
  size_t reserve_samples;   /**< Compatibility-only requested reserve. */
  size_t filtered_occupancy_samples; /**< Slowly filtered consumer source
                                        occupancy. */
  size_t target_samples;    /**< Latest drift-controller setpoint; it never
                               gates playout. */
  int ratio_correction_ppm; /**< Applied source-consumption correction in
                               parts per million. */
};

/** @brief One preallocated SPSC PCM ring and its consumer-only rate controller.
 *
 * @ref rpcr_producer_push_sample publishes one source sample with release
 * ordering.  @ref rpcr_consumer_pop_sample acquires that cursor and advances
 * @ref read one sample at a time.  The converter, rate controller, and packet
 * loss concealment state are owned only by the consumer.  Consequently every
 * post-initialization audio operation is allocation-free and lock-free.
 */
struct rpcr_ring {
  int16_t *storage; /**< Owned raw SPSC PCM storage. */
  float *input;     /**< Owned consumer converter input workspace. */
  float *output;    /**< Owned consumer converter output workspace. */
  int16_t *history; /**< Owned consumer PLC history. */
  size_t capacity;  /**< Raw storage and converter workspace capacity. */
  atomic_uint_fast64_t
      written; /**< Producer-owned monotonically increasing write cursor. */
  atomic_uint_fast64_t
      read; /**< Consumer-owned monotonically increasing read cursor. */
  atomic_uint_fast64_t discarded; /**< Producer-dropped incoming samples. */
  atomic_uint_fast64_t
      missing; /**< Consumer-observed output-shortfall samples. */
  atomic_uint_fast64_t
      consecutive_underruns; /**< Current contiguous output shortfall. */
  atomic_uint_fast64_t
      underrun_average_milli; /**< Ten-second shortfall EWMA. */
  atomic_uint_fast64_t
      reserve_samples; /**< Compatibility-only requested reserve. */
  atomic_uint_fast64_t
      target_samples; /**< Latest drift-controller occupancy setpoint. */
  atomic_uint_fast64_t
      filtered_occupancy_milli;    /**< Published filtered occupancy in
                                      millisamples. */
  atomic_int ratio_correction_ppm; /**< Published signed source-consumption
                                      correction. */
  struct SRC_STATE_tag
      *converter; /**< Consumer-owned persistent libsamplerate state. */
  uint64_t occupancy_milli; /**< Consumer-owned filtered occupancy. */
  double ratio;             /**< Consumer-owned source-consumption ratio. */
  size_t input_offset;      /**< First pending converter input sample. */
  size_t input_pending;     /**< Consumer-staged source samples. */
  size_t output_offset;     /**< First pending converted output sample. */
  size_t output_pending;    /**< Consumer-staged converted output samples. */
  size_t history_length;    /**< Valid PCM samples retained for PLC. */
  size_t history_next;      /**< Next consumer-only PLC history position. */
  size_t plc_period;        /**< Detected pitch period for an active erasure. */
  size_t plc_samples;      /**< Samples synthesized during an active erasure. */
  size_t recovery_samples; /**< Remaining real/synthetic recovery crossfade. */
  unsigned int input_rate; /**< PCM rate written by the producer. */
  unsigned int output_rate; /**< PCM rate rendered by the consumer. */
};

/** @brief Allocate a ring and persistent converter outside real-time
 * processing.
 * @param ring Zeroed destination.
 * @param capacity PCM samples retained and converted per refill at most.
 * @param quality libsamplerate quality selection.
 * @return Zero on success, minus one on invalid input or allocation/converter
 * failure.
 */
int rpcr_init(struct rpcr_ring *ring, size_t capacity,
              enum rpcr_quality quality);

/** @brief Release all preallocated storage after producer and consumer have
 * stopped.
 * @param ring Initialized or zeroed ring.
 */
void rpcr_destroy(struct rpcr_ring *ring);

/** @brief Publish one source PCM sample without waiting.
 * @param ring Initialized ring with exactly one producer.
 * @param sample Signed 16-bit mono PCM sample.
 * @return True when @p sample was published, false when the raw ring is full
 * or unavailable.  A full initialized ring increments @ref
 * rpcr_ring::discarded.
 *
 * This is the real-time producer API.  It uses only lock-free atomics and a
 * single released storage write after initialization.
 */
bool rpcr_producer_push_sample(struct rpcr_ring *ring, int16_t sample);

/** @brief Consume one raw source PCM sample without waiting.
 * @param ring Initialized ring with exactly one consumer.
 * @param sample Destination for one signed 16-bit PCM sample.
 * @return True when a sample was acquired, false when the raw ring is empty
 * or unavailable.
 *
 * This is the raw SPSC consumer API.  It advances the monotonic read cursor
 * by exactly one sample; converted playout should use
 * @ref rpcr_consumer_render_sample instead.
 */
bool rpcr_consumer_pop_sample(struct rpcr_ring *ring, int16_t *sample);

/** @brief Render one hardware-paced PCM sample with persistent conversion.
 * @param ring Initialized ring with exactly one consumer.
 * @param output Destination for one signed 16-bit PCM sample.
 * @param target Drift-controller source-occupancy setpoint in input samples.
 * @return True for a sample from the converter and false for concealed or
 * silent output.
 *
 * The consumer never waits for @p target occupancy: it immediately consumes
 * available source PCM and continuously adjusts its source-consumption ratio
 * toward that setpoint.  A source shortfall produces one smooth concealed
 * sample and increments the shortfall counters for that exact sample.  No
 * allocation, lock, file operation, startup priming, or block admission is
 * performed after initialization.
 */
bool rpcr_consumer_render_sample(struct rpcr_ring *ring, int16_t *output,
                                 size_t target);

/** @brief Publish a compatibility PCM block by looping over per-sample pushes.
 * @param ring Initialized ring with exactly one producer.
 * @param input PCM samples to publish.
 * @param samples Number of samples.
 *
 * @deprecated This non-real-time compatibility wrapper exists only while
 * callers migrate to @ref rpcr_producer_push_sample.  It performs no bulk
 * cursor publication or overwrite policy beyond repeated sample pushes.
 */
void rpcr_write(struct rpcr_ring *ring, const int16_t *input, size_t samples);

/** @brief Set one shared PCM rate for producer and consumer.
 * @param ring Initialized ring whose producer and consumer are stopped.
 * @param sample_rate PCM sample rate in Hz.
 * @return Zero on success, or minus one for an invalid rate.
 *
 * This compatibility shorthand calls @ref rpcr_set_rates with identical
 * input and output rates.  It must be set before the first call to
 * @ref rpcr_consumer_render_sample.
 */
int rpcr_set_sample_rate(struct rpcr_ring *ring, unsigned int sample_rate);

/** @brief Set distinct producer and consumer PCM rates.
 * @param ring Initialized ring whose producer and consumer are stopped.
 * @param input_rate PCM rate written by @ref rpcr_producer_push_sample in Hz.
 * @param output_rate PCM rate rendered by @ref rpcr_consumer_render_sample in
 * Hz.
 * @return Zero on success, or minus one for an invalid rate.
 *
 * The persistent libsamplerate stream supplies nominal conversion while its
 * consumer-owned ratio gently adjusts source consumption for independent
 * clocks.  Capacity, target, and @ref rpcr_available are input samples.  This
 * function must be called before rendering, or after both endpoints stop.
 */
int rpcr_set_rates(struct rpcr_ring *ring, unsigned int input_rate,
                   unsigned int output_rate);

/** @brief Render a compatibility PCM block by looping over per-sample renders.
 * @param ring Initialized ring with exactly one consumer.
 * @param output Destination PCM block.
 * @param samples Requested output samples.
 * @param reserve Ignored compatibility argument; it no longer gates playout.
 * @param target Drift-controller source-occupancy setpoint in input samples.
 * @return Number of real converter samples rendered.  Shortfalls are filled
 * by consumer-side pitch waveform concealment or silence.
 *
 * @deprecated This non-real-time compatibility wrapper exists only while
 * callers migrate to @ref rpcr_consumer_render_sample.  In particular, it
 * never waits for a startup target or protects a reserve floor.
 */
size_t rpcr_render(struct rpcr_ring *ring, int16_t *output, size_t samples,
                   size_t reserve, size_t target);

/** @brief Return raw PCM available to the consumer.
 * @param ring Initialized ring.
 * @return Readable raw producer samples, bounded by capacity.
 */
size_t rpcr_available(const struct rpcr_ring *ring);

/** @brief Copy lock-free occupancy and source-rate-controller measurements.
 * @param ring Initialized ring, or null to return an all-zero observation.
 * @param observation Destination for one diagnostic snapshot, or null to
 * discard it.
 *
 * Producer and consumer continue independently while this function runs.
 * Fields are individually current rather than transactionally coherent, so
 * live diagnostics never put a lock in an audio path.
 */
void rpcr_observe(const struct rpcr_ring *ring,
                  struct rpcr_observation *observation);

/** @brief Update observable output-shortfall statistics.
 * @param ring Initialized consumer-owned ring.
 * @param missing PCM samples unavailable to the callback.
 * @param samples Requested callback length.
 * @param rate Callback sample rate in Hz for the ten-second EWMA window.
 *
 * @ref rpcr_consumer_render_sample calls this for every output sample, both
 * real and shortfall, so contiguous-shortfall state is exact.
 */
void rpcr_record_shortfall(struct rpcr_ring *ring, size_t missing,
                           size_t samples, unsigned int rate);

#endif
