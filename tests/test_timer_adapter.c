/**
 * @file test_timer_adapter.c
 * @brief Focused ABI checks for sample-clocked portable timer arithmetic.
 */

#include <assert.h>
#include <stdint.h>

#include "usbradioplus_radio_core_adapter.h"

int main(void)
{
	uint32_t remainder = 47U;
	int32_t elapsed = -1;
	int32_t timer = 2;
	int32_t remaining = -1;

	assert(urp_radio_core_initialize() == 0);
	assert(urp_radio_core_elapsed_ms(&remainder, 49U, &elapsed) == 0);
	assert(elapsed == 2);
	assert(remainder == 0U);
	assert(urp_radio_core_timer_consume(&timer, elapsed, &remaining) == 0);
	assert(timer == 0);
	assert(remaining == 0);

	remainder = 47U;
	elapsed = -1;
	assert(urp_radio_core_elapsed_ms(&remainder, 1U, &elapsed) == 0);
	assert(elapsed == 1);
	assert(remainder == 0U);
	assert(urp_radio_core_elapsed_ms(NULL, 48U, &elapsed) != 0);
	assert(elapsed == 0);

	timer = 5;
	remaining = -1;
	assert(urp_radio_core_timer_consume(&timer, 7, &remaining) == 0);
	assert(timer == 0);
	assert(remaining == 2);
	assert(urp_radio_core_timer_consume(NULL, 7, &remaining) != 0);
	assert(remaining == 0);
	return 0;
}
