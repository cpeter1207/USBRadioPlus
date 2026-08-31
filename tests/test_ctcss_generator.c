#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/usbradioplus_ctcss.h"

#define TEST_PI 3.14159265358979323846

int main(void)
{
	struct urp_ctcss_generator normal = { 0 }, reversed = { 0 };
	double output[48000], one[1], max = 0.0, min = 0.0, difference;
	double amplitude, bias;
	size_t i;

	assert(fabs(urp_ctcss_legacy_frequency(114.8) - 114.74609375) < 1e-9);
	assert(urp_ctcss_legacy_peak(114.8, 0) == 17083.0);
	assert(urp_ctcss_legacy_peak(114.8, 1) == 18417.0);
	assert(urp_ctcss_legacy_scaled_peak(114.8, 0, 102, 252) == 6699.0);
	urp_ctcss_legacy_scaled_levels(85.4, 1, 256, 256,
		&amplitude, &bias);
	assert(amplitude == 17886.0 && bias == 1.0);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256,
		&amplitude, &bias);
	assert(amplitude == 16951.5 && bias == -0.5);

	urp_ctcss_generate(&normal, output, 48000, 114.8, 17083.0, 1, 0);
	for (i = 0; i < 48000; ++i) {
		if (output[i] > max) max = output[i];
		if (output[i] < min) min = output[i];
	}
	assert(max > 17082.99 && min < -17082.99);

	memset(output, 1, sizeof(output));
	urp_ctcss_generate(&normal, output, 48000, 114.8, 17083.0, 0, 0);
	for (i = 0; i < 48000; ++i) assert(output[i] == 0.0);

	normal.phase = reversed.phase = 0.75;
	urp_ctcss_generate(&normal, one, 1, 114.8, 1.0, 1, 0);
	urp_ctcss_generate(&reversed, one, 1, 114.8, 1.0, 1, 1);
	difference = fmod(reversed.phase - normal.phase + 2.0 * TEST_PI,
		2.0 * TEST_PI);
	/* XPMR's integer table advances 170 of 256 positions, or 239.0625 degrees. */
	assert(fabs(difference - 2.0 * TEST_PI * 170.0 / 256.0) < 1e-12);

	puts("native CTCSS frequency, level, mute, and phase-reversal tests passed");
	return 0;
}
