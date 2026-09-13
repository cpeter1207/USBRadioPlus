/** @file
 * @brief RNNoise frame buffering for fixed-rate local receiver denoising.
 */

#ifndef TXAGC_RNNOISE_PROCESSOR_H
#define TXAGC_RNNOISE_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#include <rnnoise.h>

#define TXAGC_RNNOISE_RATE 48000

#define TXAGC_RNNOISE_FRAME 480

/** Owned RNNoise state, one-frame PCM-code staging, and denoiser counters. */
struct txagc_rnnoise {
	/** Owned RNNoise denoiser instance. */
	DenoiseState *denoise;
	/** PCM-code samples waiting for a complete RNNoise inference frame. */
	float input_frame[TXAGC_RNNOISE_FRAME];
	/** Occupied samples in input_frame. */
	size_t input_count;
	/** Denoised PCM-code samples from the preceding inference frame. */
	float output_frame[TXAGC_RNNOISE_FRAME];
	/** Next unread sample in output_frame. */
	size_t output_index;
	/** Occupied unread samples in output_frame. */
	size_t output_count;
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
	/** Cumulative denoiser setup failures. */
	uint64_t errors;
	/** Nonzero while denoising is active. */
	int active;
	/** Nonzero after the control plane allocated the denoiser. */
	int prepared;
	/** Nonzero after startup buffering permits output. */
	int primed;
};

/** @brief Initialize an empty local-receiver denoiser.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_rnnoise_init(struct txagc_rnnoise *state);
/** @brief Release RNNoise resources.
 * @param state Processor or stream state owned by the caller.
 */
void txagc_rnnoise_destroy(struct txagc_rnnoise *state);
/** @brief Allocate and silently warm fixed-48 kHz RNNoise before callback processing.
 * @param state Processor state owned by the radio channel.
 * @param sample_rate Native input sample rate in Hz.
 * @return Zero when a reusable denoiser is ready, otherwise nonzero.
 *
 * The native receiver and RNNoise both use 48 kHz.  Other rates are rejected
 * rather than introducing a hidden conversion stage.  A successful call makes
 * txagc_rnnoise_process_prepared() allocation-free. Two silent library frames
 * exercise the retained denoiser without consuming live framing or meter state.
 */
int txagc_rnnoise_prepare(struct txagc_rnnoise *state, unsigned int sample_rate);
/** @brief Process an already prepared RNNoise stream without allocating or reconfiguring.
 * @param state Prepared processor state.
 * @param samples Mutable legacy PCM-code samples updated in place.
 * @param count Number of elements available in samples.
 * @return Zero on success; a nonzero status when the prepared state cannot process.
 */
int txagc_rnnoise_process_prepared(struct txagc_rnnoise *state, double *samples, size_t count);
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
/** @} */
