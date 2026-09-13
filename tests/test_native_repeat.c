/** @file
 * @brief Executable native repeat regression and failure-path checks.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/usbradioplus_repeat.h"
#include "../src/usbradioplus_radio_core_adapter.h"

/** The selected descriptor is controlled only in this executable. */
static const struct rptadv_radio_descriptor *selected_descriptor;
/** @brief Provide a complete descriptor or simulate a missing provider. */
const struct rptadv_radio_descriptor *urp_radio_core_adapter_test_descriptor(void)
{
	return selected_descriptor;
}
/** @brief Simulate failure without touching the caller's output span. */
static enum rptadv_radio_result fail_repeat(const float *input, float *output, uint32_t count,
					    float gain, uint32_t muted)
{
	(void)input;
	(void)output;
	(void)count;
	(void)gain;
	(void)muted;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	const double input[] = {1200.0, -600.0, 300.0, -150.0};
	double output[4];
	struct urp_native_repeat_workspace workspace = {0};
	size_t index;
	struct rptadv_radio_descriptor failed;
	double oversized[URP_NATIVE_MAX_SAMPLES + 1] = {0};
	assert(urp_native_repeat_prepare(output, input, 4, 1.0, 0, &workspace) == -1);
	assert(urp_native_repeat_prepare(NULL, input, 4, 1.0, 0, &workspace) == -1);
	assert(urp_native_repeat_prepare(output, input, 0, 1.0, 0, NULL) == -1);
	assert(urp_native_repeat_prepare(output, NULL, 4, 1.0, 0, &workspace) == -1);
	assert(urp_native_repeat_prepare(oversized, NULL, URP_NATIVE_MAX_SAMPLES + 1, 1.0, 1,
					 &workspace) == -1);
	selected_descriptor = rptadv_radio_descriptor();

	assert(urp_native_repeat_initialize() == 0);
	assert(urp_native_repeat_prepare(output, input, 4, 0.5, 0, &workspace) == 0);
	assert(output[0] == 600.0);
	assert(output[1] == -300.0);
	assert(output[2] == 150.0);
	assert(output[3] == -75.0);

	assert(urp_native_repeat_prepare(output, input, 4, 1.0, 1, &workspace) == 0);
	for (index = 0; index < 4; ++index)
		assert(output[index] == 0.0);
	memcpy(&failed, selected_descriptor, sizeof(failed));
	failed.radio_repeat_f32 = fail_repeat;
	selected_descriptor = &failed;
	assert(urp_native_repeat_initialize() == 0);
	output[0] = 1.0;
	assert(urp_native_repeat_prepare(output, input, 4, 1.0, 0, &workspace) == -1);
	assert(output[0] == 0.0);

	puts("native repeat gain and DTMF mute tests passed");
	return 0;
}
