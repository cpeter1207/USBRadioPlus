#ifndef USBRADIOPLUS_CTCSS_H
#define USBRADIOPLUS_CTCSS_H

#include <stddef.h>

struct urp_ctcss_generator {
	double phase;
};

double urp_ctcss_legacy_frequency(double frequency);
double urp_ctcss_legacy_peak(double frequency, int filter_250);
double urp_ctcss_legacy_scaled_peak(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8);
void urp_ctcss_legacy_scaled_levels(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8, double *amplitude, double *bias);
void urp_ctcss_generate(struct urp_ctcss_generator *generator, double *output, size_t count,
			double frequency, double peak, int enabled, int phase_reverse);

#endif
