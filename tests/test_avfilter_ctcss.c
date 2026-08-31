#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000
#define BLOCK 960

static double measure_window(struct txagc_config *config, double frequency,
		int warmup_blocks, int measurement_blocks)
{
	struct txagc_avfilter state;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	txagc_avfilter_init(&state);
	for (int block = 0; block < warmup_blocks + measurement_blocks; ++block) {
		for (int i = 0; i < BLOCK; ++i) {
			double t = (double) (block * BLOCK + i) / RATE;
			samples[i] = 1000.0 * sin(2.0 * M_PI * frequency * t);
		}
		if (txagc_avfilter_process(&state, config, samples, BLOCK, RATE) < 0)
			return NAN;
		if (block >= warmup_blocks) for (int i = 0; i < BLOCK; ++i) {
			sum += samples[i] * samples[i];
			++count;
		}
	}
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

static double measure(struct txagc_config *config, double frequency)
{
	return measure_window(config, frequency, 50, 50);
}

int main(void)
{
	struct txagc_config config;
	double bypass_speech, notch_tone, notch_low, notch_high, notch_speech;
	double edge_after_500ms, hp_tone, hp_speech;

	memset(&config, 0, sizeof(config));
	bypass_speech = measure(&config, 300.0);
	config.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	config.ctcss_notch_width_hz = 5.0;
	/* Runtime supplies only the tone currently selected by the decoder. */
	strcpy(config.ctcss_notch_frequencies, "254.1");
	notch_tone = measure(&config, 254.1);
	notch_low = measure(&config, 254.1 * 0.995);
	notch_high = measure(&config, 254.1 * 1.005);
	edge_after_500ms = measure_window(&config, 254.1 * 0.995, 25, 25);
	notch_speech = measure(&config, 300.0);
	config.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	config.ctcss_highpass_hz = 300.0;
	hp_tone = measure(&config, 114.8);
	hp_speech = measure(&config, 1000.0);
	printf("notch 254.1=%.4f tolerance=%.4f/%.4f after500ms=%.4f 300=%.2f\n"
		"highpass 114.8=%.4f 1000=%.2f\n",
		notch_tone, notch_low, notch_high, edge_after_500ms, notch_speech,
		hp_tone, hp_speech);
	if (20.0 * log10(notch_speech / notch_tone) < 40.0) return 1;
	if (20.0 * log10(hp_speech / hp_tone) < 60.0) return 3;
	if (20.0 * log10(notch_speech / notch_low) < 50.0) return 4;
	if (20.0 * log10(notch_speech / notch_high) < 50.0) return 5;
	if (fabs(20.0 * log10(notch_speech / bypass_speech)) > 0.25) return 7;
	if (20.0 * log10(notch_speech / edge_after_500ms) < 50.0) return 8;
	return 0;
}
