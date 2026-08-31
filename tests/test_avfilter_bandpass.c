#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000
#define BLOCK 960

static double measure(double frequency, int receive)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	memset(&config, 0, sizeof(config));
	if (receive) {
		config.receive_bandpass_enabled = 1;
		config.receive_bandpass_highpass_hz = 300.0;
		config.receive_bandpass_lowpass_hz = 3000.0;
	} else {
		config.splatter_filter_enabled = 1;
		config.output_highpass_hz = 300.0;
		config.output_lowpass_hz = 3000.0;
	}
	txagc_avfilter_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (int i = 0; i < BLOCK; ++i) {
			double t = (double) (block * BLOCK + i) / RATE;
			samples[i] = 1000.0 * sin(2.0 * M_PI * frequency * t);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0) {
			txagc_avfilter_destroy(&state);
			return NAN;
		}
		if (block >= 50) {
			for (int i = 0; i < BLOCK; ++i) {
				sum += samples[i] * samples[i];
				++count;
			}
		}
	}
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

int main(void)
{
	double low = measure(100.0, 0);
	double pass = measure(1000.0, 0);
	double high = measure(5000.0, 0);
	double receive_low = measure(100.0, 1);
	double receive_pass = measure(1000.0, 1);
	double receive_high = measure(5000.0, 1);
	double low_rejection = 20.0 * log10(pass / low);
	double high_rejection = 20.0 * log10(pass / high);

	printf("100Hz=%.3f 1000Hz=%.3f 5000Hz=%.3f rejection=%.1f/%.1f dB\n",
		low, pass, high, low_rejection, high_rejection);
	if (!isfinite(pass) || pass < 650.0 || pass > 750.0) return 1;
	if (low_rejection < 60.0 || high_rejection < 60.0) return 2;
	if (fabs(receive_pass - pass) > 0.1
		|| fabs(receive_low - low) > 0.1
		|| fabs(receive_high - high) > 0.1) return 3;
	return 0;
}
