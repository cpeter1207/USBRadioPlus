#include <assert.h>
#include <stdio.h>

#include "../src/usbradioplus_hardware.h"

static uint8_t writes[256];
static unsigned write_count;

static void capture_write(void *opaque, uint8_t value)
{
	(void)opaque;
	assert(write_count < sizeof(writes));
	writes[write_count++] = value;
}

int main(void)
{
	struct urp_rtx_words words;
	struct urp_parallel_bus bus = {0x0fU, capture_write, NULL};

	words = urp_hardware_rtx_words(146940000U, 146340000U, 0);
	assert(words.reference == 6401U);
	assert(words.synthesizer == 108896U);
	words = urp_hardware_rtx_words(146940000U, 146340000U, 1);
	assert(words.synthesizer == 117032U);

	words = urp_hardware_rtx_words(444500000U, 449500000U, 0);
	assert(words.reference == 2563U);
	assert(words.synthesizer == 135280U);

	urp_hardware_set_channel(&bus, 5U);
	assert(write_count == 2U);
	assert(writes[0] == 0xffU);
	assert(writes[1] == 0xafU);
	urp_hardware_set_channel(NULL, 5U);
	{
		struct urp_parallel_bus no_writer = {0};
		urp_hardware_set_channel(&no_writer, 5U);
		urp_hardware_program_radio(&no_writer, 146940000U, 146340000U, 0, 0);
	}
	urp_hardware_program_radio(NULL, 146940000U, 146340000U, 0, 0);
	urp_hardware_program_radio(&bus, 0, 146340000U, 0, 0);

	write_count = 0;
	bus.value = 0xe0U;
	urp_hardware_program_radio(&bus, 146940000U, 146340000U, 1, 0);
	assert(write_count == 129U);
	assert(bus.value == 0xe8U);

	write_count = 0;
	bus.value = 0xffU;
	urp_hardware_program_radio(&bus, 444500000U, 449500000U, 0, 1);
	assert(write_count == 129U);
	assert((bus.value & 0x18U) == 0);

	puts("legacy binary and RTX parallel-port protocol tests passed");
	return 0;
}
