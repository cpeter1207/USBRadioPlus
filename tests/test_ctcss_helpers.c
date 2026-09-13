/** @file
 * @brief CTCSS configuration and calibration compatibility checks.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../src/usbradioplus_ctcss.h"
#include "../src/usbradioplus_radio_core_adapter.h"

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	double amplitude, bias;

	assert(urp_radio_core_initialize() == 0);
	assert(fabs(urp_ctcss_legacy_frequency(114.8) - 114.74609375) < 1e-9);
	assert(urp_ctcss_frequency_supported(67.0F));
	assert(urp_ctcss_frequency_supported(250.3F));
	assert(!urp_ctcss_frequency_supported(49.0F));
	assert(!urp_ctcss_frequency_supported(100.0001F));
	assert(urp_ctcss_legacy_peak(114.8, 0) == 17083.0);
	assert(urp_ctcss_legacy_peak(114.8, 1) == 18417.0);
	assert(urp_ctcss_legacy_scaled_peak(114.8, 0, 102, 252) == 6699.0);
	urp_ctcss_legacy_scaled_levels(85.4, 1, 256, 256, &amplitude, &bias);
	assert(amplitude == 17886.0 && bias == 1.0);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256, &amplitude, &bias);
	assert(amplitude == 16951.5 && bias == -0.5);

	puts("CTCSS frequency validation and legacy calibration helpers passed");
	return 0;
}
