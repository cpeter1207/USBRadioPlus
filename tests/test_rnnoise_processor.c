#include "../src/txagc/rnnoise_processor.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_double_processing_and_reconfiguration(void)
{
	struct txagc_rnnoise state;
	double samples[960];
	unsigned int block;
	size_t index;

	txagc_rnnoise_init(&state);
	for (block = 0; block < 20; ++block) {
		for (index = 0; index < 960; ++index)
			samples[index] = 1000.0 * sin(2.0 * 3.141592653589793 * index / 48.0);
		assert(!txagc_rnnoise_process_double(&state, samples, 960, 48000));
	}
	assert(state.active && state.rnnoise_frames > 0 && state.output_samples > 0);
	assert(!txagc_rnnoise_process_double(&state, samples, 160, 8000));
	assert(state.input_rate == 8000);
	txagc_rnnoise_bypass(&state);
	assert(!state.active && !state.primed);
	txagc_rnnoise_bypass(&state);
	txagc_rnnoise_destroy(&state);
	assert(!state.denoise && !state.upsampler && !state.downsampler);
}

static void test_integer_and_invalid_inputs(void)
{
	struct txagc_rnnoise state;
	int16_t samples[960];
	double oversized[2049] = {0};

	memset(samples, 0, sizeof(samples));
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_process(&state, NULL, 1, 48000) < 0);
	assert(txagc_rnnoise_process(&state, samples, 2049, 48000) < 0);
	assert(txagc_rnnoise_process_double(&state, oversized, 2049, 48000) < 0);
	assert(txagc_rnnoise_process_double(&state, oversized, 1, 0) < 0);
	assert(!txagc_rnnoise_process(&state, samples, 960, 48000));
	txagc_rnnoise_destroy(&state);
}

int main(void)
{
	test_double_processing_and_reconfiguration();
	test_integer_and_invalid_inputs();
	puts("RNNoise processor tests passed");
	return 0;
}
