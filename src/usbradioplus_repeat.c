/** @file
 * @brief Prepare native repeat audio with repeat level and DTMF muting.
 */

#include "usbradioplus_repeat.h"

#include <string.h>

void urp_native_repeat_prepare(double *output, const double *input, size_t samples, double gain,
			       int muted)
{
	size_t index;

	/* app_rpt's DTMF detector still owns digit recognition.  Its mute state
	 * gates the parallel native-rate copy before pre-emphasis and transmission. */
	if (muted) {
		memset(output, 0, samples * sizeof(*output));
		return;
	}
	for (index = 0; index < samples; ++index) {
		output[index] = input[index] * gain;
	}
}
