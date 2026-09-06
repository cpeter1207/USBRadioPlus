/** @file
 * @brief RNNoise frame buffering and rate adaptation for local receiver denoising.
 */

#ifndef TXAGC_RNNOISE_PROCESSOR_H
#define TXAGC_RNNOISE_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#include <rnnoise.h>
#include <samplerate.h>

#define TXAGC_RNNOISE_RATE 48000

#define TXAGC_RNNOISE_FRAME 480

#define TXAGC_RNNOISE_FIFO 16384

/** Owned RNNoise state, sample-rate converters, stream FIFOs, and denoiser counters. */
struct txagc_rnnoise {
	/** Owned RNNoise denoiser instance. */
	DenoiseState *denoise;
	/** Owned converter into RNNoise's 48 kHz stream. */
	SRC_STATE *upsampler;
	/** Owned converter back to the caller's sample rate. */
	SRC_STATE *downsampler;
	/** Caller stream sample rate in Hz. */
	unsigned int input_rate;
	/** Samples waiting for a complete 480-sample RNNoise frame. */
	float up_fifo[TXAGC_RNNOISE_FIFO];
	/** Occupied samples in up_fifo. */
	size_t up_count;
	/** Denoised samples waiting for rate conversion or output. */
	float down_fifo[TXAGC_RNNOISE_FIFO];
	/** Occupied samples in down_fifo. */
	size_t down_count;
	/** Speech probability from the most recent RNNoise frame. */
	double vad_probability;
	/** Sum of frame speech probabilities for cumulative reporting. */
	double vad_sum;
	/** Number of complete frames processed by RNNoise. */
	uint64_t rnnoise_frames;
	/** Total output samples delivered. */
	uint64_t output_samples;
	/** Samples buffered before denoiser output was available. */
	uint64_t startup_samples;
	/** Cumulative denoiser or rate-conversion failures. */
	uint64_t errors;
	/** Nonzero while denoising is active. */
	int active;
	/** Nonzero after startup buffering permits output. */
	int primed;
};

/** @brief Initialize an empty local-receiver denoiser.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_rnnoise_init(struct txagc_rnnoise *state);
/** @brief Release RNNoise and sample-rate-converter resources.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_rnnoise_destroy(struct txagc_rnnoise *state);
/** @brief Denoise signed 16-bit receiver audio through the RNNoise stream adapter.
 * @param state Processor or stream state owned by the caller.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int txagc_rnnoise_process(struct txagc_rnnoise *state, int16_t *samples, size_t count,
			  unsigned int sample_rate);
/** @brief Denoise floating-point receiver audio using 480-sample RNNoise frames.
 * @param state Processor or stream state owned by the caller.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int txagc_rnnoise_process_double(struct txagc_rnnoise *state, double *samples, size_t count,
				 unsigned int sample_rate);
/** @brief Reset denoiser history when the local chain bypasses RNNoise.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_rnnoise_bypass(struct txagc_rnnoise *state);

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def TXAGC_RNNOISE_RATE
 * @brief RNNoise processing rate in Hz.
 */
/** @def TXAGC_RNNOISE_FRAME
 * @brief Samples in one RNNoise inference frame.
 */
/** @def TXAGC_RNNOISE_FIFO
 * @brief Capacity of each denoiser stream FIFO in samples.
 */
/** @} */
