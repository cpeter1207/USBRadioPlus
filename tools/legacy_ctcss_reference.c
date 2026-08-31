/* Characterize the legacy driver's integer CTCSS encoder for native-path equivalence tests. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef int16_t i16;
typedef int32_t i32;
typedef uint32_t u32;

#include "../src/usbradioplus_radio_coefficients.h"
#include "../src/usbradioplus_ctcss.h"

#define SAMPLES_PER_SINE 256

/* Reconstruct the legacy driver's Q15 table exactly without retaining generator data in
 * the production implementation. */
static i16 legacy_sine(unsigned index)
{
	return (i16) lrint(32767.0 * sin(2.0 * M_PI * index / SAMPLES_PER_SINE));
}

static int32_t fir_step(i16 *state, const i16 *coef, int taps, int32_t gain,
	int32_t sample, int32_t output_gain)
{
	int64_t sum = 0;
	int i;
	for (i = taps - 1; i > 0; --i) state[i] = state[i - 1];
	state[0] = (i16) sample;
	for (i = 0; i < taps; ++i) sum += (int32_t) coef[i] * state[i];
	return (int32_t) (((sum / gain) * output_gain) / 256);
}

struct level_result {
	int32_t peak;
	int32_t trough;
};

static struct level_result characterize(float frequency, int wide,
	int lsd_output_gain, int output_gain, int verbose)
{
	const i16 *lsd_coef = wide ? coef_fir_lpf_250_9_66 : coef_fir_lpf_215_9_88;
	const int lsd_taps = wide ? taps_fir_lpf_250_9_66 : taps_fir_lpf_215_9_88;
	const int32_t lsd_gain = wide ? gain_fir_lpf_250_9_66 : gain_fir_lpf_215_9_88;
	i16 lsd_state[128] = { 0 }, out_state[128] = { 0 };
	int32_t phase = 0, peak = 0, trough = 0;
	const int32_t freq10 = (int32_t) (frequency * 10.0f);
	const int32_t step = (256 * freq10 * 128) / 8000 / 10;
	int n, k;

	for (n = 0; n < 32000; ++n) {
		int32_t generated = ((int32_t) legacy_sine(phase / 128) * 128) / 256;
		int32_t filtered = fir_step(lsd_state, lsd_coef, lsd_taps, lsd_gain,
			generated, lsd_output_gain);
		phase = (phase + step) % (256 * 128);
		for (k = 0; k < 6; ++k) {
			int32_t output = fir_step(out_state, fir_txlpf[0].coefs,
				fir_txlpf[0].taps, fir_txlpf[0].gain, filtered, output_gain);
			if (n >= 16000) {
				if (output > peak) peak = output;
				if (output < trough) trough = output;
			}
		}
	}
	if (verbose) printf("%5.1f %s peak=%d trough=%d p2p=%d\n", frequency,
		wide ? "250" : "215", peak, trough, peak - trough);
	return (struct level_result) { peak, trough };
}

static int legacy_mult_calc(int value)
{
	int pot = (value / 4) * 4 + 2;
	return 256 - (256 * (3 - value % 4)) / (pot + 2);
}

int main(void)
{
	size_t i;
	int failures = 0;
	for (i = 0; i < sizeof(freq_ctcss) / sizeof(freq_ctcss[0]); ++i) {
		struct level_result narrow_result = characterize(freq_ctcss[i], 0,
			256, 256, 0);
		struct level_result wide_result = characterize(freq_ctcss[i], 1,
			256, 256, 0);
		int32_t narrow = narrow_result.peak > -narrow_result.trough
			? narrow_result.peak : -narrow_result.trough;
		int32_t wide = wide_result.peak > -wide_result.trough
			? wide_result.peak : -wide_result.trough;
		if ((int32_t) urp_ctcss_legacy_peak(freq_ctcss[i], 0) != narrow
			|| (int32_t) urp_ctcss_legacy_peak(freq_ctcss[i], 1) != wide)
			failures++;
		{
			double amplitude, bias;
			urp_ctcss_legacy_scaled_levels(freq_ctcss[i], 0, 256, 256,
				&amplitude, &bias);
			if ((int32_t) lrint(amplitude + bias) != narrow_result.peak
				|| (int32_t) lrint(-amplitude + bias) != narrow_result.trough)
				failures++;
			urp_ctcss_legacy_scaled_levels(freq_ctcss[i], 1, 256, 256,
				&amplitude, &bias);
			if ((int32_t) lrint(amplitude + bias) != wide_result.peak
				|| (int32_t) lrint(-amplitude + bias) != wide_result.trough)
				failures++;
		}
	}
	printf("76 legacy CTCSS frequency/filter peak and peak-to-peak levels matched\n");
	{
		static const int indices[] = { 0, 15, 31, 37 };
		static const int settings[] = { 0, 1, 200, 500, 999 };
		int max_error = 0;
		size_t fi, ai, mi;
		for (fi = 0; fi < sizeof(indices) / sizeof(indices[0]); ++fi) {
			float frequency = freq_ctcss[indices[fi]];
			int filter_250 = frequency > 203.5f;
			int base = (int) urp_ctcss_legacy_peak(frequency, filter_250);
			for (ai = 0; ai < sizeof(settings) / sizeof(settings[0]); ++ai) {
				int lsd_gain = settings[ai] * 256 / 999;
				for (mi = 0; mi < sizeof(settings) / sizeof(settings[0]); ++mi) {
					int output_gain = legacy_mult_calc(settings[mi] * 152 / 999);
					struct level_result result = characterize(frequency,
						filter_250, lsd_gain, output_gain, 0);
					int actual = result.peak > -result.trough
						? result.peak : -result.trough;
					int predicted = (int) urp_ctcss_legacy_scaled_peak(
						frequency, filter_250, lsd_gain, output_gain);
					int error = actual > predicted ? actual - predicted : predicted - actual;
					if (error > max_error) max_error = error;
				}
			}
		}
		printf("CTCSS quantized-gain maximum peak error: %d PCM codes\n", max_error);
		if (max_error > 2) failures++;
	}
	{
		/* Deployed alpha-node calibration: 114.8 Hz, 215 Hz filter,
		 * txctcssadj=400, and txmixbset=999. */
		const int lsd_gain = 400 * 256 / 999;
		const int output_gain = legacy_mult_calc(999 * 152 / 999);
		const double amplitude = urp_ctcss_legacy_scaled_peak(114.8, 0,
			lsd_gain, output_gain);
		struct level_result legacy = characterize(114.8f, 0, lsd_gain,
			output_gain, 1);
		int native_peak = (int) lrint(amplitude);
		int native_p2p = 2 * native_peak;
		int legacy_p2p = legacy.peak - legacy.trough;
		printf("active calibration native p2p=%d legacy p2p=%d\n",
			native_p2p, legacy_p2p);
		if (native_p2p != legacy_p2p) failures++;
	}
	if (failures) fprintf(stderr, "%d CTCSS reference levels differ\n", failures);
	return failures ? 1 : 0;
}
