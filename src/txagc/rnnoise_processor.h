#ifndef TXAGC_RNNOISE_PROCESSOR_H
#define TXAGC_RNNOISE_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#include <rnnoise.h>
#include <samplerate.h>

#define TXAGC_RNNOISE_RATE 48000
#define TXAGC_RNNOISE_FRAME 480
#define TXAGC_RNNOISE_FIFO 16384

struct txagc_rnnoise {
	DenoiseState *denoise;
	SRC_STATE *upsampler;
	SRC_STATE *downsampler;
	unsigned int input_rate;
	float up_fifo[TXAGC_RNNOISE_FIFO];
	size_t up_count;
	float down_fifo[TXAGC_RNNOISE_FIFO];
	size_t down_count;
	double vad_probability;
	double vad_sum;
	uint64_t rnnoise_frames;
	uint64_t output_samples;
	uint64_t startup_samples;
	uint64_t errors;
	int active;
	int primed;
};

void txagc_rnnoise_init(struct txagc_rnnoise *state);
void txagc_rnnoise_destroy(struct txagc_rnnoise *state);
int txagc_rnnoise_process(struct txagc_rnnoise *state, int16_t *samples, size_t count,
			  unsigned int sample_rate);
int txagc_rnnoise_process_double(struct txagc_rnnoise *state, double *samples, size_t count,
				 unsigned int sample_rate);
void txagc_rnnoise_bypass(struct txagc_rnnoise *state);

#endif
