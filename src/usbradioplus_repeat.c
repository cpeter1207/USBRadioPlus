/** @file
 * @brief Bridge native repeat audio through the portable Rust radio core.
 */

#include "usbradioplus_repeat.h"

#include <stddef.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

/** @brief Fill a valid compatibility output span with PCM-code silence.
 * @param output Output PCM span, or NULL when no buffer is present.
 * @param samples Number of output samples to clear.
 */
static void native_repeat_silence(double *output, size_t samples)
{
	if (output && samples)
		memset(output, 0, samples * sizeof(*output));
}

int urp_native_repeat_initialize(void)
{
	return urp_radio_core_initialize();
}

int urp_native_repeat_prepare(double *output, const double *input, size_t samples, double gain,
			      int muted, struct urp_native_repeat_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	size_t index;

	if (!output || !workspace || samples > URP_NATIVE_MAX_SAMPLES || (!muted && !input)) {
		native_repeat_silence(output, samples);
		return -1;
	}
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor) {
		native_repeat_silence(output, samples);
		return -1;
	}
	if (!muted) {
		/* PCM codes convert exactly at this power-of-two f32 boundary. */
		for (index = 0; index < samples; ++index)
			workspace->input[index] = (float)(input[index] / 32768.0);
	}
	if (descriptor->radio_repeat_f32(muted ? NULL : workspace->input, workspace->output,
					 samples, (float)gain, !!muted) != RPTADV_RADIO_OK) {
		native_repeat_silence(output, samples);
		return -1;
	}
	for (index = 0; index < samples; ++index)
		output[index] = (double)workspace->output[index] * 32768.0;
	return 0;
}
