/** @file
 * @brief RNNoise frame buffering and rate adaptation for local receiver denoising.
 */

#include "rnnoise_processor.h"

#include <math.h>
#include <string.h>

#ifdef URP_RNNOISE_TESTING
#include "rnnoise_processor_internal.h"
#define RNNOISE_PRIVATE
#else

#define RNNOISE_PRIVATE static
#endif

/** @brief Discard RNNoise stream history and reset both rate converters.
 * @param state Processor or stream state owned by the caller.
 */
RNNOISE_PRIVATE void reset_stream(struct txagc_rnnoise *state)
{
	if (state->upsampler) {
		src_reset(state->upsampler);
	}
	if (state->downsampler) {
		src_reset(state->downsampler);
	}
	if (state->denoise) {
		rnnoise_destroy(state->denoise);
		state->denoise = rnnoise_create(NULL);
	}
	state->up_count = 0;
	state->down_count = 0;
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
	if (state->upsampler) {
		src_delete(state->upsampler);
	}
	if (state->downsampler) {
		src_delete(state->downsampler);
	}
	memset(state, 0, sizeof(*state));
}

void txagc_rnnoise_bypass(struct txagc_rnnoise *state)
{
	if (state->active) {
		reset_stream(state);
		state->active = 0;
	}
}

/** @brief Create or reconfigure the RNNoise converters for the source sample rate.
 * @param state Processor or stream state owned by the caller.
 * @param sample_rate Audio sample rate in Hz.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
RNNOISE_PRIVATE int configure_rate(struct txagc_rnnoise *state, unsigned int sample_rate)
{
	int error;

	if (state->active && state->input_rate == sample_rate && state->denoise &&
	    state->upsampler && state->downsampler) {
		return 0;
	}
	txagc_rnnoise_destroy(state);
	state->denoise = rnnoise_create(NULL);
	state->upsampler = src_new(SRC_SINC_FASTEST, 1, &error);
	if (!state->upsampler) {
		state->errors++;
		return -1;
	}
	state->downsampler = src_new(SRC_SINC_FASTEST, 1, &error);
	if (!state->denoise || !state->downsampler) {
		state->errors++;
		return -1;
	}
	state->input_rate = sample_rate;
	state->active = 1;
	return 0;
}

/** @brief Append a block to a bounded RNNoise FIFO.
 * @param fifo Bounded audio FIFO.
 * @param fifo_count Current FIFO occupancy, updated by the operation.
 * @param data Input samples to copy into the FIFO.
 * @param count Number of elements available in the supplied block.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
RNNOISE_PRIVATE int append(float *fifo, size_t *fifo_count, const float *data, size_t count)
{
	if (*fifo_count + count > TXAGC_RNNOISE_FIFO) {
		return -1;
	}
	memcpy(fifo + *fifo_count, data, count * sizeof(*data));
	*fifo_count += count;
	return 0;
}

/** @brief Remove samples from the front of an RNNoise FIFO.
 * @param fifo Bounded audio FIFO.
 * @param fifo_count Current FIFO occupancy, updated by the operation.
 * @param count Number of elements available in the supplied block.
 */
RNNOISE_PRIVATE void consume(float *fifo, size_t *fifo_count, size_t count)
{
	memmove(fifo, fifo + count, (*fifo_count - count) * sizeof(*fifo));
	*fifo_count -= count;
}

/** @brief Quantize a floating-point sample without signed PCM overflow.
 * @param sample One sample in signed PCM amplitude units.
 * @return Bounded signed 16-bit PCM sample.
 */
RNNOISE_PRIVATE int16_t pcm_from_double(double sample)
{
	if (sample > 32767.0)
		return 32767;
	if (sample < -32768.0)
		return -32768;
	return (int16_t)lround(sample);
}

int txagc_rnnoise_process_double(struct txagc_rnnoise *state, double *samples, size_t count,
				 unsigned int sample_rate)
{
	float input[2048];
	float converted[4096];
	float clean[TXAGC_RNNOISE_FRAME];
	float down[1024];
	SRC_DATA data;
	size_t i;

	if (!sample_rate || count > sizeof(input) / sizeof(input[0]) ||
	    configure_rate(state, sample_rate)) {
		return -1;
	}
	state->active = 1;
	for (i = 0; i < count; ++i) {
		input[i] = samples[i];
	}
	memset(&data, 0, sizeof(data));
	data.data_in = input;
	data.input_frames = count;
	data.data_out = converted;
	data.output_frames = sizeof(converted) / sizeof(converted[0]);
	data.src_ratio = (double)TXAGC_RNNOISE_RATE / sample_rate;
	if (src_process(state->upsampler, &data) || data.input_frames_used != (long)count ||
	    append(state->up_fifo, &state->up_count, converted, data.output_frames_gen)) {
		state->errors++;
		reset_stream(state);
		return -1;
	}

	while (state->up_count >= TXAGC_RNNOISE_FRAME) {
		long generated;
		state->vad_probability =
			rnnoise_process_frame(state->denoise, clean, state->up_fifo);
		state->vad_sum += state->vad_probability;
		state->rnnoise_frames++;
		consume(state->up_fifo, &state->up_count, TXAGC_RNNOISE_FRAME);
		if (!state->primed) {
			state->primed = 1;
			continue;
		}

		memset(&data, 0, sizeof(data));
		data.data_in = clean;
		data.input_frames = TXAGC_RNNOISE_FRAME;
		data.data_out = down;
		data.output_frames = sizeof(down) / sizeof(down[0]);
		data.src_ratio = (double)sample_rate / TXAGC_RNNOISE_RATE;
		if (src_process(state->downsampler, &data)) {
			state->errors++;
			reset_stream(state);
			return -1;
		}
		generated = data.output_frames_gen;
		if (append(state->down_fifo, &state->down_count, down, generated)) {
			state->errors++;
			reset_stream(state);
			return -1;
		}
	}

	{
		size_t available = state->down_count;
		if (available > count) {
			available = count;
		}
		for (i = 0; i < available; ++i) {
			samples[i] = state->down_fifo[i];
			state->output_samples++;
		}
		consume(state->down_fifo, &state->down_count, available);
		for (; i < count; ++i) {
			samples[i] = 0;
			state->startup_samples++;
		}
	}
	return 0;
}

int txagc_rnnoise_process(struct txagc_rnnoise *state, int16_t *samples, size_t count,
			  unsigned int sample_rate)
{
	double work[2048];
	size_t i;
	int result;

	if (!samples || count > sizeof(work) / sizeof(work[0])) {
		return -1;
	}
	for (i = 0; i < count; ++i) {
		work[i] = samples[i];
	}
	result = txagc_rnnoise_process_double(state, work, count, sample_rate);
	if (result) {
		return result;
	}
	for (i = 0; i < count; ++i)
		samples[i] = pcm_from_double(work[i]);
	return 0;
}

/** @name File-local and build-time constants
 * @{ */
/** @def RNNOISE_PRIVATE
 * @brief Expose denoiser internals only to the linked test harness.
 */
/** @} */
