#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000
#define BLOCK 960

static double measure(struct txagc_config *config, double frequency)
{
	struct txagc_avfilter state;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	txagc_avfilter_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (int i = 0; i < BLOCK; ++i) {
			double t = (double) (block * BLOCK + i) / RATE;
			samples[i] = 1000.0 * sin(2.0 * M_PI * frequency * t);
		}
		if (txagc_avfilter_process(&state, config, samples, BLOCK, RATE) < 0)
			return NAN;
		if (block >= 50) for (int i = 0; i < BLOCK; ++i) {
			sum += samples[i] * samples[i];
			++count;
		}
	}
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

int main(void)
{
	struct txagc_config config;
	double notch_tone, notch_speech, hp_tone, hp_speech;

	memset(&config, 0, sizeof(config));
	config.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	config.ctcss_notch_width_hz = 2.0;
	strcpy(config.ctcss_notch_frequencies, "100.0,114.8,123.0");
	notch_tone = measure(&config, 114.8);
	notch_speech = measure(&config, 300.0);
	config.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	config.ctcss_highpass_hz = 300.0;
	hp_tone = measure(&config, 114.8);
	hp_speech = measure(&config, 1000.0);
	printf("notch 114.8=%.4f 300=%.2f; highpass 114.8=%.4f 1000=%.2f\n",
		notch_tone, notch_speech, hp_tone, hp_speech);
	if (20.0 * log10(notch_speech / notch_tone) < 40.0) return 1;
	if (20.0 * log10(hp_speech / hp_tone) < 60.0) return 2;
	return 0;
}
