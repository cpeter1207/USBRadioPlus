#include <assert.h>
#include <stdio.h>

#include "../src/usbradioplus_repeat.h"

int main(void)
{
	const double input[] = {1200.0, -600.0, 300.0, -150.0};
	double output[4];
	size_t index;

	urp_native_repeat_prepare(output, input, 4, 0.5, 0);
	assert(output[0] == 600.0);
	assert(output[1] == -300.0);
	assert(output[2] == 150.0);
	assert(output[3] == -75.0);

	urp_native_repeat_prepare(output, input, 4, 1.0, 1);
	for (index = 0; index < 4; ++index)
		assert(output[index] == 0.0);

	puts("native repeat gain and DTMF mute tests passed");
	return 0;
}
