#include "../src/usbradioplus_dsp.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double rms(const int16_t *x, size_t n)
{
	double sum = 0.0;
	size_t i;
	for (i = 0; i < n; ++i) sum += (double) x[i] * x[i];
	return sqrt(sum / n);
}

static void tone(int16_t *x, size_t n, unsigned int rate, double hz, double level)
{
	size_t i;
	for (i = 0; i < n; ++i) x[i] = (int16_t) lrint(level * sin(2.0 * M_PI * hz * i / rate));
}

static void test_hpf(void)
{
	struct urp_biquad f;
	int16_t low[URP_NATIVE_SAMPLES], high[URP_NATIVE_SAMPLES];
	double in = 10000.0 / sqrt(2.0), low_out, high_out;
	tone(low, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 100.0, 10000.0);
	tone(high, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 1000.0, 10000.0);
	assert(!urp_biquad_highpass(&f, URP_RATE_NATIVE, 250.0, 1));
	urp_biquad_process(&f, low, URP_NATIVE_SAMPLES);
	urp_biquad_reset(&f);
	urp_biquad_process(&f, high, URP_NATIVE_SAMPLES);
	low_out = rms(low + 480, 480);
	high_out = rms(high + 480, 480);
	assert(20.0 * log10(low_out / in) < -14.0);
	assert(20.0 * log10(high_out / in) > -1.0);
}

static double emphasis_level(int pre, double hz)
{
	struct urp_deemphasis f;
	int16_t block[URP_NATIVE_SAMPLES];
	int frame;
	memset(&f, 0, sizeof(f));
	assert(!urp_land_mobile_emphasis_configure(&f, URP_RATE_NATIVE,
		20.0, pre, 1));
	for (frame = 0; frame < 10; ++frame) {
		tone(block, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, hz, 1000.0);
		urp_deemphasis_process(&f, block, URP_NATIVE_SAMPLES);
	}
	return 20.0 * log10(rms(block, URP_NATIVE_SAMPLES) / (1000.0 / sqrt(2.0)));
}

static void test_land_mobile_emphasis(void)
{
	double pre300 = emphasis_level(1, 300.0);
	double pre1k = emphasis_level(1, 1000.0);
	double pre3k = emphasis_level(1, 3000.0);
	double de300 = emphasis_level(0, 300.0);
	double de1k = emphasis_level(0, 1000.0);
	double de3k = emphasis_level(0, 3000.0);
	printf("emphasis dB pre[300,1k,3k]=%.3f,%.3f,%.3f de=%.3f,%.3f,%.3f\n",
		pre300, pre1k, pre3k, de300, de1k, de3k);
	assert(fabs(pre1k) < 0.1 && fabs(de1k) < 0.1);
	assert(fabs(pre300 + 10.4576) < 0.4);
	assert(fabs(pre3k - 9.5424) < 0.4);
	assert(fabs(de300 - 10.4576) < 0.4);
	assert(fabs(de3k + 9.5424) < 0.4);
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
		assert(!urp_rate_convert(up, in, URP_LINK_SAMPLES, URP_RATE_LINK,
			native, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
		assert(used == URP_LINK_SAMPLES);
		total_up += made;
		assert(!urp_rate_convert(down, native, made, URP_RATE_NATIVE,
			back, URP_LINK_SAMPLES, URP_RATE_LINK, &used, &made));
		total_down += made;
	}
	assert(total_up > 9 * URP_NATIVE_SAMPLES);
	assert(total_down > 8 * URP_LINK_SAMPLES);
	urp_src_destroy(up); urp_src_destroy(down);
}

static void test_same_rate_bypass(void)
{
	int16_t input[URP_NATIVE_SAMPLES], output[URP_NATIVE_SAMPLES];
	size_t used = 0, made = 0;
	tone(input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, 1234.0, 9000.0);
	assert(!urp_rate_convert(NULL, input, URP_NATIVE_SAMPLES, URP_RATE_NATIVE,
		output, URP_NATIVE_SAMPLES, URP_RATE_NATIVE, &used, &made));
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
	assert(urp_legacy_limiter_ceiling_dbfs(1) ==
		urp_legacy_limiter_ceiling_dbfs(5000));
	assert(urp_legacy_limiter_ceiling_dbfs(32000) ==
		urp_legacy_limiter_ceiling_dbfs(13000));
}

static void test_clock_recovery(void)
{
	struct urp_clock_recovery clock = { 0 };
	double low = 0.0, high = 0.0;
	int i;
	for (i = 0; i < 500; ++i) low = urp_clock_recovery_update(&clock, 960, 2880);
	assert(low > 0.0 && low <= URP_CLOCK_MAX_CORRECTION);
	for (i = 0; i < 1000; ++i) high = urp_clock_recovery_update(&clock, 4800, 2880);
	assert(high < 0.0 && high >= -URP_CLOCK_MAX_CORRECTION);
	assert(fabs(high - low) < 2.0 * URP_CLOCK_MAX_CORRECTION);
	urp_clock_recovery_reset(&clock);
	assert(clock.correction == 0.0);
}

static void simulate_clock_drift(double producer_ppm)
{
	struct urp_clock_recovery clock = { 0 };
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
			double correction = urp_clock_recovery_update(&clock,
				(size_t) native_fifo + app_frames * 960, 2880);
			native_fifo += 960.0 * (1.0 + correction);
			app_frames--;
		}
		if (native_fifo >= 960.0) native_fifo -= 960.0;
		else underflows++;
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
	struct urp_clock_recovery clock = { 0 };
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
		while (phase >= 1.0) { app_frames++; phase -= 1.0; }
		while (native_count < 3 * URP_NATIVE_SAMPLES && app_frames) {
			double correction = urp_clock_recovery_update(&clock,
				native_count + app_frames * URP_NATIVE_SAMPLES,
				3 * URP_NATIVE_SAMPLES);
			size_t used = 0, made = 0;
			assert(!urp_src_process(src, input, URP_LINK_SAMPLES, output,
				URP_NATIVE_SAMPLES * 2, 6.0 * (1.0 + correction),
				&used, &made));
			assert(used == URP_LINK_SAMPLES);
			native_count += made;
			app_frames--;
		}
		if (!primed && native_count >= 3 * URP_NATIVE_SAMPLES) primed = 1;
		if (primed) {
			if (native_count >= URP_NATIVE_SAMPLES) native_count -= URP_NATIVE_SAMPLES;
			else { underflows++; primed = 0; }
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
		mixed[i] = local8[i] + (int16_t) (1500.0 * sin(2 * M_PI * 1300 * i / 8000));
	assert(urp_echo_remove(&e, mixed, recovered, 0.75));
	assert(e.last_correlation > 0.9);
	assert(rms(mixed, URP_LINK_SAMPLES) < 2500.0);
	assert(rms(recovered, URP_NATIVE_SAMPLES) > 5000.0);
}

int main(void)
{
	test_hpf(); test_land_mobile_emphasis(); test_src();
	test_same_rate_bypass(); test_cutoff_parser(); test_legacy_filter_selectors();
	test_legacy_limiter_ceiling();
	test_clock_recovery();
	test_simulated_clock_drift(); test_src_clock_drift(); test_echo();
	puts("usbradioplus DSP tests passed");
	return 0;
}
