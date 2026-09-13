/** @file
 * @brief RNNoise frame buffering for fixed-rate local receiver denoising.
 */

#include "rnnoise_processor.h"

#include <string.h>

/** @brief Discard bounded stream history without allocating or replacing the denoiser.
 * @param state Processor or stream state owned by the caller.
 *
 * This function is used by the native render callback after a processing error
 * or bypass transition.  Keeping the prepared denoiser object intact is what
 * prevents a receiver-signal transition from allocating in real time.
 */
static void reset_stream(struct txagc_rnnoise *state)
{
	state->input_count = 0;
	state->output_index = 0;
	state->output_count = 0;
	state->active = 0;
	state->primed = 0;
}

void txagc_rnnoise_init(struct txagc_rnnoise *state)
{
	memset(state, 0, sizeof(*state));
}

void txagc_rnnoise_destroy(struct txagc_rnnoise *state)
{
	if (state->denoise) {
		rnnoise_destroy(state->denoise);
	}
	memset(state, 0, sizeof(*state));
}

void txagc_rnnoise_bypass(struct txagc_rnnoise *state)
{
	if (state->active) {
		reset_stream(state);
	}
}

int txagc_rnnoise_prepare(struct txagc_rnnoise *state, unsigned int sample_rate)
{
	uint64_t errors;

	if (!state || sample_rate != TXAGC_RNNOISE_RATE)
		return -1;
	if (state->prepared && state->denoise)
		return 0;
	errors = state->errors;
	txagc_rnnoise_destroy(state);
	state->errors = errors;
	state->denoise = rnnoise_create(NULL);
	if (!state->denoise) {
		txagc_rnnoise_destroy(state);
		state->errors = errors + 1;
		return -1;
	}
	/* Exercise the actual denoiser's two-frame window/FFT path on the control
	 * plane. Its preallocated staging is still empty for live audio, preserving
	 * the existing framing delay and counters without recreating warmed state. */
	for (unsigned int frame = 0; frame < 2U; ++frame)
		(void)rnnoise_process_frame(state->denoise, state->output_frame,
					    state->input_frame);
	memset(state->output_frame, 0, sizeof(state->output_frame));
	state->prepared = 1;
	return 0;
}

int txagc_rnnoise_process_prepared(struct txagc_rnnoise *state, double *samples, size_t count)
{
	float clean[TXAGC_RNNOISE_FRAME];
	size_t offset = 0;

	if (!state || !samples || !state->prepared || !state->denoise) {
		return -1;
	}
	state->active = 1;
	while (offset < count) {
		size_t span = TXAGC_RNNOISE_FRAME - state->input_count;
		size_t index;

		if (span > count - offset)
			span = count - offset;
		/* Capture input before replacing this same caller-owned span with the
		 * preceding frame's output.  RNNoise and the callback share PCM-code
		 * scale, so this has no unity SRC latency or gain conversion. */
		for (index = 0; index < span; ++index)
			state->input_frame[state->input_count + index] =
				(float)samples[offset + index];
		if (state->output_count) {
			for (index = 0; index < span; ++index)
				samples[offset + index] =
					state->output_frame[state->output_index + index];
			state->output_index += span;
			state->output_count -= span;
			state->output_samples += span;
		} else {
			for (index = 0; index < span; ++index)
				samples[offset + index] = 0.0;
			state->startup_samples += span;
		}
		state->input_count += span;
		offset += span;
		if (state->input_count != TXAGC_RNNOISE_FRAME)
			continue;
		state->vad_probability =
			rnnoise_process_frame(state->denoise, clean, state->input_frame);
		state->vad_sum += state->vad_probability;
		state->rnnoise_frames++;
		state->input_count = 0;
		if (!state->primed) {
			state->primed = 1;
			continue;
		}
		memcpy(state->output_frame, clean, sizeof(clean));
		state->output_index = 0;
		state->output_count = TXAGC_RNNOISE_FRAME;
	}
	return 0;
}
