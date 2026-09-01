#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/txagc/avfilter_processor.h"

#define RATE 48000
#define BLOCK 960

static double measure(double frequency, double amplitude, int enabled)
{
	struct txagc_avfilter state;
	struct txagc_config config;
	double samples[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	memset(&config, 0, sizeof(config));
	config.stage_count = 1;
	config.stage_order[0] = TXAGC_STAGE_DEESSER;
	config.deesser_enabled = enabled;
	config.deesser_frequency_hz = 4000.0;
	config.deesser_width_octaves = 1.0;
	config.deesser_threshold_dbfs = -18.0;
	config.deesser_ratio = 3.0;
	config.deesser_max_reduction_db = 4.0;
	config.deesser_attack_ms = 2.0;
	config.deesser_release_ms = 60.0;
	txagc_avfilter_init(&state);
	for (int block = 0; block < 100; ++block) {
		for (int index = 0; index < BLOCK; ++index) {
			double time = (double) (block * BLOCK + index) / RATE;
			samples[index] = amplitude * sin(2.0 * M_PI * frequency * time);
		}
		if (txagc_avfilter_process(&state, &config, samples, BLOCK, RATE) < 0)
			return NAN;
		if (block >= 50) for (int index = 0; index < BLOCK; ++index) {
			sum += samples[index] * samples[index];
			++count;
		}
	}
	txagc_avfilter_destroy(&state);
	return sqrt(sum / count);
}

static double change_db(double frequency, double amplitude)
{
	return 20.0 * log10(measure(frequency, amplitude, 1)
		/ measure(frequency, amplitude, 0));
}

int main(void)
{
	double speech = change_db(1000.0, 12000.0);
	double quiet_sibilance = change_db(4000.0, 2000.0);
	double loud_sibilance = change_db(4000.0, 12000.0);

	printf("de-esser 1kHz=%+.2f quiet-4kHz=%+.2f loud-4kHz=%+.2f dB\n",
		speech, quiet_sibilance, loud_sibilance);
	if (!isfinite(speech) || !isfinite(quiet_sibilance)
		|| !isfinite(loud_sibilance)) return 1;
	if (fabs(speech) > 0.5 || fabs(quiet_sibilance) > 0.5) return 2;
	if (loud_sibilance > -1.0 || loud_sibilance < -4.5) return 3;
	return 0;
}
