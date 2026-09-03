#include "../src/usbradioplus_dsp.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int allocation_to_fail;
static int allocation_count;

void *urp_test_calloc(size_t count, size_t size)
{
	allocation_count++;
	return allocation_count == allocation_to_fail ? NULL : calloc(count, size);
}

void *urp_test_realloc(void *pointer, size_t size)
{
	allocation_count++;
	return allocation_count == allocation_to_fail ? NULL : realloc(pointer, size);
}

static double rms(const int16_t *x, size_t n)
{
	double sum = 0.0;
	size_t i;
	for (i = 0; i < n; ++i)
		sum += (double)x[i] * x[i];
	return sqrt(sum / n);
}

static void tone(int16_t *x, size_t n, unsigned int rate, double hz, double level)
{
	size_t i;
	for (i = 0; i < n; ++i)
		x[i] = (int16_t)lrint(level * sin(2.0 * M_PI * hz * i / rate));
}

static void test_src(void)
{
	struct urp_src *up = urp_src_create(0, 1);
	struct urp_src *down = urp_src_create(0, 1);
	int16_t in[URP_LINK_SAMPLES], native[URP_NATIVE_SAMPLES], back[URP_LINK_SAMPLES];
	size_t used, made, total_up = 0, total_down = 0;
	int frame;
	assert(up && down);
	tone(in, URP_LINK_SAMPLES, URP_RATE_LINK, 1000.0, 12000.0);
	/* Sinc converters intentionally have startup latency. Verify steady-state
	 * frame accounting rather than demanding a full first block. */
	for (frame = 0; frame < 12; ++frame) {
		assert(!urp_rate_convert(up, in, URP_LINK_SAMPLES, URP_RATE_LINK, native,
					 URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
		assert(used == URP_LINK_SAMPLES);
		total_up += made;
		assert(!urp_rate_convert(down, native, made, URP_RATE_NATIVE, back,
					 URP_LINK_SAMPLES, URP_RATE_LINK, &used, &made));
		total_down += made;
	}
	assert(total_up > 9 * URP_NATIVE_SAMPLES);
	assert(total_down > 8 * URP_LINK_SAMPLES);
	urp_src_destroy(up);
	urp_src_destroy(down);
}

static void test_same_rate_bypass(void)
{
	int16_t input[URP_NATIVE_SAMPLES], output[URP_NATIVE_SAMPLES];
	size_t used = 0, made = 0;
	tone(input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 1234.0, 9000.0);
	assert(!urp_rate_convert(NULL, input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, output,
				 URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
	assert(used == URP_NATIVE_SAMPLES && made == URP_NATIVE_SAMPLES);
	assert(!memcmp(input, output, sizeof(input)));
}

static void test_cutoff_parser(void)
{
	struct urp_cutoff_setting value;
	assert(!urp_parse_cutoff("0", 300.0, 24000.0, &value));
	assert(value.enabled && !value.exact && value.selector == 0);
	assert(!urp_parse_cutoff("2", 300.0, 24000.0, &value));
	assert(value.enabled && !value.exact && value.selector == 2);
	assert(!urp_parse_cutoff("no", 300.0, 24000.0, &value));
	assert(!value.enabled && value.exact);
	assert(!urp_parse_cutoff("yes", 300.0, 24000.0, &value));
	assert(value.enabled && value.exact && value.frequency_hz == 300.0);
	assert(!urp_parse_cutoff("3000.0", 300.0, 24000.0, &value));
	assert(value.enabled && value.exact && value.frequency_hz == 3000.0);
	assert(!urp_parse_cutoff("3e3", 300.0, 24000.0, &value));
	assert(value.frequency_hz == 3000.0);
	assert(urp_parse_cutoff("-1", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("nan", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("inf", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("0.0", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("24000.0", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("junk", 300.0, 24000.0, &value));
	assert(urp_parse_cutoff("999999999999999999999", 300.0, 24000.0, &value));
}

static void test_legacy_filter_selectors(void)
{
	assert(urp_legacy_cutoff(URP_FILTER_RX_LOWPASS, 0) == 3000.0);
	assert(urp_legacy_cutoff(URP_FILTER_RX_LOWPASS, 1) == 3300.0);
	assert(urp_legacy_cutoff(URP_FILTER_RX_LOWPASS, 2) == 3700.0);
	assert(urp_legacy_cutoff(URP_FILTER_RX_HIGHPASS, 0) == 300.0);
	assert(urp_legacy_cutoff(URP_FILTER_RX_HIGHPASS, 1) == 250.0);
	assert(urp_legacy_cutoff(URP_FILTER_TX_LOWPASS, 1) == 3300.0);
	assert(urp_legacy_cutoff(URP_FILTER_TX_HIGHPASS, 2) == 120.0);
	assert(urp_legacy_cutoff(URP_FILTER_TX_HIGHPASS, 99) == 300.0);
	assert(urp_legacy_cutoff(URP_FILTER_TX_HIGHPASS, -1) == 300.0);
}

static void test_legacy_limiter_ceiling(void)
{
	assert(fabs(urp_legacy_limiter_ceiling_dbfs(12000) - (-2.704774)) < 0.0001);
	assert(fabs(urp_legacy_limiter_ceiling_dbfs(5000) - (-10.308999)) < 0.0001);
	assert(fabs(urp_legacy_limiter_ceiling_dbfs(13000) - (-2.009532)) < 0.0001);
	assert(urp_legacy_limiter_ceiling_dbfs(1) == urp_legacy_limiter_ceiling_dbfs(5000));
	assert(urp_legacy_limiter_ceiling_dbfs(32000) == urp_legacy_limiter_ceiling_dbfs(13000));
}

static void test_clock_recovery(void)
{
	struct urp_clock_recovery clock = {0};
	double low = 0.0, high = 0.0;
	int i;
	for (i = 0; i < 500; ++i)
		low = urp_clock_recovery_update(&clock, 960, 2880);
	assert(low > 0.0 && low <= URP_CLOCK_MAX_CORRECTION);
	for (i = 0; i < 1000; ++i)
		high = urp_clock_recovery_update(&clock, 4800, 2880);
	assert(high < 0.0 && high >= -URP_CLOCK_MAX_CORRECTION);
	assert(fabs(high - low) < 2.0 * URP_CLOCK_MAX_CORRECTION);
	urp_clock_recovery_reset(&clock);
	assert(clock.correction == 0.0);
}

static void simulate_clock_drift(double producer_ppm)
{
	struct urp_clock_recovery clock = {0};
	double app_phase = 3.0, native_fifo = 0.0;
	unsigned int app_frames = 0, underflows = 0;
	int tick;
	for (tick = 0; tick < 60000; ++tick) { /* Twenty minutes at 20 ms. */
		app_phase += 1.0 + producer_ppm / 1000000.0;
		while (app_phase >= 1.0) {
			app_frames++;
			app_phase -= 1.0;
		}
		while (native_fifo < 2880.0 && app_frames) {
			double correction = urp_clock_recovery_update(
				&clock, (size_t)native_fifo + app_frames * 960, 2880);
			native_fifo += 960.0 * (1.0 + correction);
			app_frames--;
		}
		if (native_fifo >= 960.0)
			native_fifo -= 960.0;
		else
			underflows++;
	}
	printf("clock drift %+.0f ppm: underruns %u, app frames %u, native %.1f, correction %.6f\n",
	       producer_ppm, underflows, app_frames, native_fifo, clock.correction);
	assert(underflows == 0);
	assert(app_frames < 8);
	assert(native_fifo < 8.0 * 960.0);
}

static void test_simulated_clock_drift(void)
{
	simulate_clock_drift(-1000.0);
	simulate_clock_drift(1000.0);
}

static void simulate_src_clock_drift(double producer_ppm)
{
	struct urp_clock_recovery clock = {0};
	struct urp_src *src = urp_src_create(0, 1);
	int16_t input[URP_LINK_SAMPLES], output[URP_NATIVE_SAMPLES * 2];
	double phase = 3.0;
	size_t native_count = 0;
	unsigned int app_frames = 0, underflows = 0;
	int primed = 0, tick;
	assert(src);
	tone(input, URP_LINK_SAMPLES, URP_RATE_LINK, 1000.0, 5000.0);
	for (tick = 0; tick < 6000; ++tick) {
		phase += 1.0 + producer_ppm / 1000000.0;
		while (phase >= 1.0) {
			app_frames++;
			phase -= 1.0;
		}
		while (native_count < 3 * URP_NATIVE_SAMPLES && app_frames) {
			double correction = urp_clock_recovery_update(
				&clock, native_count + app_frames * URP_NATIVE_SAMPLES,
				3 * URP_NATIVE_SAMPLES);
			size_t used = 0, made = 0;
			assert(!urp_src_process(src, input, URP_LINK_SAMPLES, output,
						URP_NATIVE_SAMPLES * 2, 6.0 * (1.0 + correction),
						&used, &made));
			assert(used == URP_LINK_SAMPLES);
			native_count += made;
			app_frames--;
		}
		if (!primed && native_count >= 3 * URP_NATIVE_SAMPLES)
			primed = 1;
		if (primed) {
			if (native_count >= URP_NATIVE_SAMPLES)
				native_count -= URP_NATIVE_SAMPLES;
			else {
				underflows++;
				primed = 0;
			}
		}
	}
	assert(underflows == 0);
	assert(primed);
	urp_src_destroy(src);
}

static void test_src_clock_drift(void)
{
	simulate_src_clock_drift(-1000.0);
	simulate_src_clock_drift(1000.0);
}

static void test_elastic_fifo_short_burst_and_recovery(void)
{
	size_t native_count = 0;
	unsigned int emitted = 0, program_frames = 0, underflows = 0;
	int primed = 0;

	/* Two leading safety frames plus one telemetry frame reach the target. */
	native_count += 3 * URP_NATIVE_SAMPLES;
	program_frames = 1;
	if (!primed && native_count >= 3 * URP_NATIVE_SAMPLES)
		primed = 1;
	while (primed && native_count >= URP_NATIVE_SAMPLES) {
		native_count -= URP_NATIVE_SAMPLES;
		emitted++;
	}
	assert(emitted == 3 && program_frames == 1);

	/* An empty tick records the underrun and resets recovery state. */
	if (primed && native_count < URP_NATIVE_SAMPLES) {
		underflows++;
		primed = 0;
		native_count = 0;
	}
	assert(underflows == 1);

	/* Recovery seeds the same margin and preserves its first program frame. */
	native_count += 3 * URP_NATIVE_SAMPLES;
	program_frames++;
	if (!primed && native_count >= 3 * URP_NATIVE_SAMPLES)
		primed = 1;
	while (primed && native_count >= URP_NATIVE_SAMPLES) {
		native_count -= URP_NATIVE_SAMPLES;
		emitted++;
	}
	assert(emitted == 6 && program_frames == 2);
}

static void test_echo(void)
{
	struct urp_echo_replacer e;
	int16_t local8[URP_LINK_SAMPLES], local48[URP_NATIVE_SAMPLES];
	int16_t mixed[URP_LINK_SAMPLES], recovered[URP_NATIVE_SAMPLES];
	size_t i;
	urp_echo_init(&e);
	tone(local8, URP_LINK_SAMPLES, URP_RATE_LINK, 713.0, 8000.0);
	tone(local48, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 713.0, 8000.0);
	urp_echo_push(&e, local8, local48);
	for (i = 0; i < URP_LINK_SAMPLES; ++i)
		mixed[i] = local8[i] + (int16_t)(1500.0 * sin(2 * M_PI * 1300 * i / 8000));
	assert(urp_echo_remove(&e, mixed, recovered, 0.75));
	assert(e.last_correlation > 0.9);
	assert(rms(mixed, URP_LINK_SAMPLES) < 2500.0);
	assert(rms(recovered, URP_NATIVE_SAMPLES) > 5000.0);
}

static void test_defensive_and_boundary_paths(void)
{
	struct urp_clock_recovery clock = {.correction = 0.25};
	struct urp_cutoff_setting cutoff;
	struct urp_echo_replacer echo;
	struct urp_src *src;
	int16_t mono[] = {20000, -20000, 100};
	int16_t stereo[6], extracted[3], short_output[2];
	int16_t silent_link[URP_LINK_SAMPLES] = {0};
	int16_t silent_native[URP_NATIVE_SAMPLES] = {0};
	size_t used = 99, made = 99;

	urp_clock_recovery_reset(NULL);
	assert(urp_clock_recovery_update(NULL, 1, 1) == 0.0);
	assert(urp_clock_recovery_update(&clock, 1, 0) == 0.0);
	clock.correction = 0.0;
	assert(urp_clock_recovery_update(&clock, 10000, 1) < 0.0);
	assert(urp_parse_cutoff(NULL, 300.0, 24000.0, &cutoff) < 0);
	assert(urp_parse_cutoff("", 300.0, 24000.0, &cutoff) < 0);
	assert(urp_parse_cutoff("yes", NAN, 24000.0, &cutoff) < 0);
	assert(urp_parse_cutoff("yes", 0.0, 24000.0, &cutoff) < 0);
	assert(urp_parse_cutoff("yes", 24000.0, 24000.0, &cutoff) < 0);
	assert(urp_parse_cutoff("yes", 300.0, 24000.0, NULL) < 0);
	assert(urp_legacy_cutoff((enum urp_legacy_filter) - 1, 0) == 0.0);
	assert(urp_legacy_cutoff((enum urp_legacy_filter)99, 0) == 0.0);
	assert(!urp_src_create(0, 0));
	assert(!urp_src_create(999999, 1));
	urp_src_destroy(NULL);
	urp_src_reset(NULL);

	src = urp_src_create(0, 1);
	assert(src);
	assert(urp_src_process(NULL, mono, 3, extracted, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process(src, NULL, 3, extracted, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process(src, mono, 3, NULL, 3, 1.0, &used, &made) < 0);
	assert(urp_src_process(src, mono, 3, extracted, 3, 0.0, &used, &made) < 0);
	urp_src_reset(src);
	assert(!urp_src_process(src, mono, 3, extracted, 3, 1.0, NULL, NULL));
	urp_src_destroy(src);

	assert(urp_rate_convert(NULL, NULL, 3, 48000, extracted, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert(NULL, mono, 3, 0, extracted, 3, 48000, &used, &made) < 0);
	assert(urp_rate_convert(NULL, mono, 3, 48000, extracted, 3, 0, &used, &made) < 0);
	assert(urp_rate_convert(NULL, mono, 3, 48000, short_output, 2, 48000, NULL, NULL) < 0);
	assert(short_output[0] == mono[0] && short_output[1] == mono[1]);
	assert(!urp_rate_convert(NULL, mono, 2, 48000, extracted, 3, 48000, &used, &made));
	assert(used == 2 && made == 2 && extracted[2] == 0);

	urp_duplicate_mono(mono, stereo, 3, 2.0, 2.0);
	assert(stereo[0] == 32767 && stereo[1] == 32767);
	assert(stereo[2] == -32768 && stereo[3] == -32768);
	urp_extract_mono(stereo, extracted, 3, 0);
	assert(extracted[0] == stereo[0]);
	urp_extract_mono(stereo, extracted, 3, 9);
	assert(extracted[0] == stereo[1]);

	urp_echo_init(NULL);
	urp_echo_init(&echo);
	urp_echo_push(NULL, silent_link, silent_native);
	urp_echo_push(&echo, NULL, silent_native);
	urp_echo_push(&echo, silent_link, NULL);
	assert(!urp_echo_remove(NULL, silent_link, silent_native, 0.5));
	assert(!urp_echo_remove(&echo, NULL, silent_native, 0.5));
	assert(!urp_echo_remove(&echo, silent_link, NULL, 0.5));
	assert(!urp_echo_remove(&echo, silent_link, silent_native, 0.5));
	assert(echo.misses == 1 && echo.last_correlation == 0.0);
	urp_echo_push(&echo, silent_link, silent_native);
	assert(!urp_echo_remove(&echo, silent_link, silent_native, 0.5));
}

static void test_allocation_and_converter_failures(void)
{
	struct urp_src *src;
	int16_t input[3] = {1, 2, 3};
	int16_t output[3];

	allocation_count = 0;
	allocation_to_fail = 1;
	assert(!urp_src_create(0, 1));
	allocation_to_fail = 0;
	src = urp_src_create(0, 1);
	assert(src);
	allocation_count = 0;
	allocation_to_fail = 1;
	assert(urp_src_process(src, input, 3, output, 3, 1.0, NULL, NULL) < 0);
	allocation_count = 0;
	allocation_to_fail = 2;
	assert(urp_src_process(src, input, 3, output, 3, 1.0, NULL, NULL) < 0);
	allocation_to_fail = 0;
	assert(urp_src_process(src, input, 3, output, 3, 1000.0, NULL, NULL) != 0);
	urp_src_destroy(src);
}

int main(void)
{
	test_src();
	test_same_rate_bypass();
	test_cutoff_parser();
	test_legacy_filter_selectors();
	test_legacy_limiter_ceiling();
	test_clock_recovery();
	test_simulated_clock_drift();
	test_src_clock_drift();
	test_elastic_fifo_short_burst_and_recovery();
	test_echo();
	test_defensive_and_boundary_paths();
	test_allocation_and_converter_failures();
	puts("usbradioplus DSP tests passed");
	return 0;
}
