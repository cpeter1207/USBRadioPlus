#include "usbradioplus_ctcss.h"

#include <math.h>
#include <string.h>

#define URP_CTCSS_RATE 48000.0
#define URP_PI 3.14159265358979323846
#define URP_LEGACY_PHASE_REVERSE_STEPS 170.0
#define URP_LEGACY_SINE_STEPS 256.0

static const double frequencies[] = {
	67.0, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8,
	97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
	131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 162.2, 167.9, 173.8,
	179.9, 186.2, 192.8, 203.5, 210.7, 218.1, 225.7, 233.6, 241.8,
	250.3
};

/* Steady-state peak PCM from XPMR's generator, CTCSS LPF, and output FIR. */
static const double peak_215[] = {
	16573, 16619, 16638, 16670, 16701, 16734, 16768, 16812, 16843, 16943,
	16915, 16952, 16981, 17020, 17049, 17083, 17104, 17101, 17096, 17065,
	17015, 16960, 16824, 16653, 16467, 16173, 15855, 15456, 14954, 14429,
	13719, 12475, 11519, 10591, 9428, 8287, 7070, 5876
};

static const double peak_250[] = {
	17435, 17558, 17614, 17684, 17751, 17819, 17887, 17965, 18024, 18157,
	18148, 18204, 18258, 18318, 18365, 18417, 18451, 18465, 18476, 18470,
	18444, 18425, 18329, 18218, 18106, 17903, 17698, 17443, 17118, 16813,
	16338, 15540, 14926, 14348, 13524, 12731, 11836, 10905
};

/* Positive peak minus negative-peak magnitude after XPMR's integer FIRs. */
static const signed char bias_215[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, -1, 0, -1, 0, 0, 0, -2, 0,
	0, -1, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, -1, 0, 0, 0, 0, 0,
	0
};

static const signed char bias_250[] = {
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, -1, 0, 0, 0,
	0, 0, 0, -1, 0, 0, 0, 0, 0,
	0
};

static size_t closest_frequency(double frequency)
{
	size_t best = 0, i;
	for (i = 1; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
		if (fabs(frequencies[i] - frequency) < fabs(frequencies[best] - frequency))
			best = i;
	}
	return best;
}

double urp_ctcss_legacy_frequency(double frequency)
{
	/* XPMR quantizes its table phase increment at an 8 kHz sample rate. */
	long freq10 = (long) (frequency * 10.0);
	long step = (256L * freq10 * 128L) / 8000L / 10L;
	return (double) step * 8000.0 / (256.0 * 128.0);
}

double urp_ctcss_legacy_peak(double frequency, int filter_250)
{
	size_t index = closest_frequency(frequency);
	return filter_250 ? peak_250[index] : peak_215[index];
}

double urp_ctcss_legacy_scaled_peak(double frequency, int filter_250,
	int tone_gain_q8, int output_gain_q8)
{
	double amplitude, bias;
	urp_ctcss_legacy_scaled_levels(frequency, filter_250, tone_gain_q8,
		output_gain_q8, &amplitude, &bias);
	return amplitude + fabs(bias);
}

void urp_ctcss_legacy_scaled_levels(double frequency, int filter_250,
	int tone_gain_q8, int output_gain_q8, double *amplitude, double *bias)
{
	size_t index = closest_frequency(frequency);
	long maximum = (long) (filter_250 ? peak_250[index] : peak_215[index]);
	long difference = filter_250 ? bias_250[index] : bias_215[index];
	long positive = difference > 0 ? maximum : maximum + difference;
	long negative = difference < 0 ? maximum : maximum - difference;

	/* Preserve XPMR's fixed-point truncation at both gain stages. */
	positive = positive * tone_gain_q8 / 256;
	positive = positive * output_gain_q8 / 256;
	negative = negative * tone_gain_q8 / 256;
	negative = negative * output_gain_q8 / 256;
	*amplitude = (positive + negative) / 2.0;
	*bias = (positive - negative) / 2.0;
}

void urp_ctcss_generate(struct urp_ctcss_generator *generator, double *output,
	size_t count, double frequency, double peak, int enabled,
	int phase_reverse)
{
	size_t i;
	double step, sine, cosine, sine_step, cosine_step;

	if (!enabled || frequency <= 0.0 || peak <= 0.0) {
		memset(output, 0, count * sizeof(*output));
		return;
	}
	if (phase_reverse) {
		/* XPMR truncates 240 degrees to 170 positions in its 256-step table. */
		generator->phase += 2.0 * URP_PI * URP_LEGACY_PHASE_REVERSE_STEPS
			/ URP_LEGACY_SINE_STEPS;
		generator->phase = fmod(generator->phase, 2.0 * URP_PI);
	}
	step = 2.0 * URP_PI * urp_ctcss_legacy_frequency(frequency) / URP_CTCSS_RATE;
	sine = sin(generator->phase);
	cosine = cos(generator->phase);
	sine_step = sin(step);
	cosine_step = cos(step);
	for (i = 0; i < count; ++i) {
		double next_sine, next_cosine;
		output[i] = peak * sine;
		next_sine = sine * cosine_step + cosine * sine_step;
		next_cosine = cosine * cosine_step - sine * sine_step;
		sine = next_sine;
		cosine = next_cosine;
	}
	generator->phase = fmod(generator->phase + step * count, 2.0 * URP_PI);
}
