#include "../src/usbradioplus_squelch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void establish_idle(struct urp_micor_squelch *state)
{
	int i;
	for (i = 0; i < 64; ++i)
		assert(urp_micor_squelch_update(state, 1, 10000, 7000, 500, 20));
}

static void test_immediate_open_and_clean_close(void)
{
	struct urp_micor_squelch state = { 0 };
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 900, 7000, 500, 20));
	assert(urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

static void test_weak_signal_has_approximately_150_ms_tail(void)
{
	struct urp_micor_squelch state = { 0 };
	int i;
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 3000, 7000, 500, 20));
	for (i = 0; i < 7; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
	assert(urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

static void test_flutter_cancels_pending_close(void)
{
	struct urp_micor_squelch state = { 0 };
	int i;
	establish_idle(&state);
	assert(!urp_micor_squelch_update(&state, 1, 3000, 7000, 500, 20));
	for (i = 0; i < 6; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
	assert(!urp_micor_squelch_update(&state, 0, 7000, 7000, 500, 20));
	for (i = 0; i < 7; ++i)
		assert(!urp_micor_squelch_update(&state, 0, 10000, 7000, 500, 20));
}

int main(void)
{
	test_immediate_open_and_clean_close();
	test_weak_signal_has_approximately_150_ms_tail();
	test_flutter_cancels_pending_close();
	puts("MICOR-style squelch tests passed");
	return 0;
}
