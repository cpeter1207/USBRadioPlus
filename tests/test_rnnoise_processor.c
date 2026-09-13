/** @file
 * @brief Executable fixed-rate RNNoise framing and amplitude regression checks.
 */

#include "../src/txagc/rnnoise_processor.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/** @brief Render a whole-frame library oracle, independent of callback assembly.
 * @param input Complete source stream in PCM-code scale.
 * @param output Expected causal output, including startup silence.
 * @param count Stream length, a multiple of the inference frame size.
 */
static void render_reference(const double *input, double *output, size_t count)
{
	DenoiseState *denoise = rnnoise_create(NULL);
	float frame[TXAGC_RNNOISE_FRAME];
	float clean[TXAGC_RNNOISE_FRAME];
	size_t offset;
	size_t index;

	assert(denoise && count % TXAGC_RNNOISE_FRAME == 0U);
	/* The production control plane warms this same instance before live PCM. */
	memset(frame, 0, sizeof(frame));
	for (unsigned int warmup = 0; warmup < 2U; ++warmup)
		rnnoise_process_frame(denoise, clean, frame);
	memset(output, 0, count * sizeof(*output));
	for (offset = 0; offset < count; offset += TXAGC_RNNOISE_FRAME) {
		for (index = 0; index < TXAGC_RNNOISE_FRAME; ++index)
			frame[index] = (float)input[offset + index];
		rnnoise_process_frame(denoise, clean, frame);
		/* Discard the model's first inference. Every later inference becomes
		 * available only after its complete input frame has arrived. */
		if (offset && offset + TXAGC_RNNOISE_FRAME < count)
			for (index = 0; index < TXAGC_RNNOISE_FRAME; ++index)
				output[offset + TXAGC_RNNOISE_FRAME + index] = clean[index];
	}
	rnnoise_destroy(denoise);
}

/** @brief Populate a deterministic PCM-code stream for direct framing parity.
 * @param samples Destination sample span.
 * @param count Number of samples in @p samples.
 * @param offset Absolute stream offset of the first sample.
 */
static void fill_signal(double *samples, size_t count, size_t offset)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		double phase = (double)(offset + index);

		samples[index] = 12000.0 * sin(2.0 * 3.141592653589793 * phase / 47.0) +
				 1500.0 * cos(2.0 * 3.141592653589793 * phase / 211.0);
	}
}

/** @brief Compare fixed/irregular callbacks with the independent library oracle. */
static void test_partition_independent_concatenated_output(void)
{
	enum { stream_samples = TXAGC_RNNOISE_FRAME * 20 };
	static const size_t partitions[] = {0U,	  1U,	479U, 137U, 343U, 960U,
					    480U, 719U, 241U, 17U,  943U};
	struct txagc_rnnoise fixed;
	struct txagc_rnnoise variable;
	static double source[stream_samples];
	static double fixed_output[stream_samples];
	static double variable_output[stream_samples];
	static double expected_output[stream_samples];
	size_t offset;
	size_t partition = 0;

	fill_signal(source, stream_samples, 0U);
	render_reference(source, expected_output, stream_samples);
	txagc_rnnoise_init(&fixed);
	txagc_rnnoise_init(&variable);
	assert(!txagc_rnnoise_prepare(&fixed, TXAGC_RNNOISE_RATE));
	assert(!txagc_rnnoise_prepare(&variable, TXAGC_RNNOISE_RATE));
	for (offset = 0; offset < stream_samples; offset += 960U) {
		memcpy(fixed_output + offset, source + offset, 960U * sizeof(*source));
		assert(!txagc_rnnoise_process_prepared(&fixed, fixed_output + offset, 960U));
	}
	for (offset = 0; offset < stream_samples;) {
		size_t count =
			partitions[partition++ % (sizeof(partitions) / sizeof(partitions[0]))];

		if (count > stream_samples - offset)
			count = stream_samples - offset;
		memcpy(variable_output + offset, source + offset, count * sizeof(*source));
		assert(!txagc_rnnoise_process_prepared(&variable, variable_output + offset, count));
		offset += count;
	}
	for (offset = 0; offset < stream_samples; ++offset) {
		assert(fixed_output[offset] == variable_output[offset]);
		assert(fixed_output[offset] == expected_output[offset]);
	}
	assert(fixed.rnnoise_frames == stream_samples / TXAGC_RNNOISE_FRAME);
	assert(fixed.startup_samples == TXAGC_RNNOISE_FRAME * 2U);
	assert(fixed.output_samples == stream_samples - fixed.startup_samples);
	assert(fixed.rnnoise_frames == variable.rnnoise_frames);
	assert(fixed.output_samples == variable.output_samples);
	assert(fixed.startup_samples == variable.startup_samples);
	txagc_rnnoise_destroy(&fixed);
	txagc_rnnoise_destroy(&variable);
}

/** @brief Verify prepared processing and fixed-rate control-plane behavior. */
static void test_prepared_processing_and_rate_contract(void)
{
	struct txagc_rnnoise state;
	double samples[960];
	const DenoiseState *denoise;
	unsigned int block;
	size_t index;

	txagc_rnnoise_init(&state);
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	denoise = state.denoise;
	assert(txagc_rnnoise_prepare(&state, 8000U) < 0);
	assert(state.prepared && state.denoise == denoise);
	for (block = 0; block < 20U; ++block) {
		for (index = 0; index < sizeof(samples) / sizeof(samples[0]); ++index)
			samples[index] =
				1000.0 * sin(2.0 * 3.141592653589793 * (double)index / 48.0);
		assert(!txagc_rnnoise_process_prepared(&state, samples,
						       sizeof(samples) / sizeof(samples[0])));
	}
	assert(state.active && state.rnnoise_frames > 0U && state.output_samples > 0U);
	txagc_rnnoise_bypass(&state);
	assert(!state.active && !state.primed && !state.input_count && !state.output_count);
	txagc_rnnoise_bypass(&state);
	txagc_rnnoise_destroy(&state);
	assert(!state.denoise && !state.prepared);
}

/** @brief Verify control-plane and prepared-callback input guards. */
static void test_prepare_and_processing_guards(void)
{
	struct txagc_rnnoise state;
	double variable_span[2049] = {0};

	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(NULL, TXAGC_RNNOISE_RATE) < 0);
	assert(txagc_rnnoise_prepare(&state, 0U) < 0);
	assert(txagc_rnnoise_prepare(&state, 47999U) < 0);
	assert(txagc_rnnoise_prepare(&state, 48001U) < 0);
	assert(txagc_rnnoise_process_prepared(NULL, variable_span, 1U) < 0);
	assert(txagc_rnnoise_process_prepared(&state, NULL, 1U) < 0);
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	assert(!txagc_rnnoise_process_prepared(&state, variable_span,
					       sizeof(variable_span) / sizeof(variable_span[0])));
	txagc_rnnoise_destroy(&state);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_partition_independent_concatenated_output();
	test_prepared_processing_and_rate_contract();
	test_prepare_and_processing_guards();
	puts("RNNoise processor tests passed");
	return 0;
}
