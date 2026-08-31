#ifndef USBRADIOPLUS_REPEAT_H
#define USBRADIOPLUS_REPEAT_H

#include <stddef.h>

void urp_native_repeat_prepare(double *output, const double *input,
	size_t samples, double gain, int muted);

#endif
