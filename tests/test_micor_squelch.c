/** @file
 * @brief Sample-clocked MICOR squelch timing, flutter and frame-alignment tests.
 */

#include "../src/usbradioplus_squelch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/** Native discriminator samples per millisecond. */
#define SAMPLES_PER_MS 48U

/** Detector fixture preserving the previous sample's comparator output. */
struct detector {
	/** Continuous capacitor and detector state. */
	struct urp_micor_squelch state;
	/** Most recent output: one closed, zero open. */
	int closed;
};

/** @brief Feed a constant noise level for a specified number of samples.
 * @param d Detector under test.
 * @param level RMS-equivalent noise in existing calibration units.
 * @param samples Number of native samples.
 */
static void feed(struct detector *d, double level, unsigned int samples)
{
	for (unsigned int i = 0; i < samples; ++i)
		d->closed =
			urp_micor_squelch_update(&d->state, d->closed, level * level, 7000, 500);
}

/** @brief Establish idle noise and then a steady carrier.
 * @param level Received-carrier discriminator noise.
 * @return Settled open detector.
 */
static struct detector carrier(double level)
{
	struct detector d = {.closed = 1};
	feed(&d, 10000.0, 1000 * SAMPLES_PER_MS);
	assert(d.closed);
	feed(&d, level, 500 * SAMPLES_PER_MS);
	assert(!d.closed);
	return d;
}

/** @brief Measure a closing transition without rounding it to a frame.
 * @param d Open detector.
 * @return Samples from carrier loss to the closed comparator.
 */
static unsigned int close_samples(struct detector *d)
{
	unsigned int samples = 0;
	while (!d->closed && samples < 250 * SAMPLES_PER_MS) {
		feed(d, 10000.0, 1);
		samples++;
	}
	assert(d->closed);
	return samples;
}

/** @brief Check closing time at every possible offset in a 20 ms audio frame. */
static void test_every_carrier_loss_alignment(void)
{
	const struct detector steady = carrier(900.0);
	unsigned int min_close = 250 * SAMPLES_PER_MS, max_close = 0;
	for (unsigned int offset = 0; offset < 960; ++offset) {
		struct detector d = steady;
		feed(&d, 900.0, offset);
		unsigned int elapsed = close_samples(&d);
		if (elapsed < min_close)
			min_close = elapsed;
		if (elapsed > max_close)
			max_close = elapsed;
		assert(elapsed < 10 * SAMPLES_PER_MS);
	}
	assert(min_close == max_close);
	printf("Strong-signal loss: %.3f ms at all 960 frame offsets\n",
	       max_close / (double)SAMPLES_PER_MS);
}

/** @brief A partial carrier-loss frame cannot spuriously charge a full long tail. */
static void test_fast_ramps_and_weak_hold(void)
{
	const struct detector strong = carrier(900.0);
	for (unsigned int ramp_ms = 0; ramp_ms <= 5; ++ramp_ms) {
		struct detector d = strong;
		unsigned int count = ramp_ms * SAMPLES_PER_MS;
		for (unsigned int i = 0; i < count; ++i)
			feed(&d, 900.0 + 9100.0 * (i + 1) / count, 1);
		assert(close_samples(&d) + count < 10 * SAMPLES_PER_MS);
	}
	for (unsigned int weak = 3000; weak <= 6900; weak += 1300) {
		struct detector d = carrier(weak);
		unsigned int elapsed = close_samples(&d);
		assert(elapsed >= 149 * SAMPLES_PER_MS);
		assert(elapsed <= 155 * SAMPLES_PER_MS);
		printf("Weak noise %u: %.3f ms closing\n", weak, elapsed / (double)SAMPLES_PER_MS);
	}
}

/** @brief Gradual fading restores flutter protection; a strong signal defeats it. */
static void test_fade_flutter_and_defeat(void)
{
	struct detector d = carrier(900.0);
	for (unsigned int i = 0; i < 100 * SAMPLES_PER_MS; ++i) {
		feed(&d, 900.0 + 9100.0 * i / (100 * SAMPLES_PER_MS), 1);
		assert(!d.closed);
	}
	assert(close_samples(&d) > 90 * SAMPLES_PER_MS);

	d = carrier(6500.0);
	for (unsigned int fade = 0; fade < 8; ++fade) {
		feed(&d, 10000.0, 100 * SAMPLES_PER_MS);
		assert(!d.closed);
		feed(&d, 6500.0, 25 * SAMPLES_PER_MS);
		assert(!d.closed);
	}
	/* Brief recoveries recharge only part of the capacitor, not a timer
	 * reset. They must still protect the next 100 ms flutter interval. */
	assert(close_samples(&d) >= 100 * SAMPLES_PER_MS);

	d = carrier(6500.0);
	feed(&d, 900.0, 30 * SAMPLES_PER_MS);
	assert(!d.closed);
	assert(close_samples(&d) < 10 * SAMPLES_PER_MS);
}

/** @brief The weak-opening comparator and configurable hysteresis retain their units. */
static void test_thresholds_startup_and_boundaries(void)
{
	struct detector d = {.closed = 1};
	for (unsigned int i = 0; i < URP_MICOR_SETTLE_SAMPLES; ++i) {
		feed(&d, 10000.0, 1);
		assert(d.closed);
	}
	feed(&d, 7100.0, 500 * SAMPLES_PER_MS);
	assert(d.closed);
	feed(&d, 6900.0, 40 * SAMPLES_PER_MS);
	assert(!d.closed);
	feed(&d, 7400.0, 500 * SAMPLES_PER_MS);
	assert(!d.closed);
	feed(&d, 7600.0, 200 * SAMPLES_PER_MS);
	assert(d.closed);

	/* Conversion to double before threshold addition/squaring avoids the
	 * old uint32_t idle-average and threshold overflow paths. */
	memset(&d, 0, sizeof(d));
	for (unsigned int i = 0; i < 1000; ++i)
		d.closed = urp_micor_squelch_update(&d.state, d.closed,
						    (double)UINT32_MAX * UINT32_MAX, UINT32_MAX,
						    UINT32_MAX);
	assert(d.state.noise_power > 0.0);
	feed(&d, 0.0, 2000 * SAMPLES_PER_MS);
	assert(!d.closed);
	assert(d.state.noise_power == 0.0);
	assert(d.state.hold_charge == 0.0);

	memset(&d, 0, sizeof(d));
	for (unsigned int i = 0; i < 1000; ++i)
		d.closed = urp_micor_squelch_update(&d.state, d.closed, 0.0, 0, 0);
	assert(d.closed);
}

/** @brief Exercise broadband noise without dependence on platform rand implementations. */
static void test_noise_ripple_and_scale(void)
{
	for (unsigned int scale = 1; scale <= 4; ++scale) {
		struct detector d = {.closed = 1};
		uint32_t random = 1;
		for (unsigned int i = 0; i < 2000 * SAMPLES_PER_MS; ++i) {
			double level = i < 1000 * SAMPLES_PER_MS ? 10000.0 : 800.0;
			double noise;
			random = random * 1664525U + 1013904223U;
			noise = 2.0 * (random >> 8) / 16777215.0 - 1.0;
			d.closed = urp_micor_squelch_update(&d.state, d.closed,
							    3.0 * noise * noise * level * level *
								    scale * scale,
							    7000 * scale, 500 * scale);
			if (i > 1020 * SAMPLES_PER_MS)
				assert(!d.closed);
		}
		assert(d.state.hold_charge < 0.1);
		unsigned int samples = 0;
		while (!d.closed && samples < 10 * SAMPLES_PER_MS) {
			random = random * 1664525U + 1013904223U;
			double noise = 2.0 * (random >> 8) / 16777215.0 - 1.0;
			d.closed = urp_micor_squelch_update(&d.state, d.closed,
							    3.0 * noise * noise * 10000.0 *
								    10000.0 * scale * scale,
							    7000 * scale, 500 * scale);
			samples++;
		}
		assert(d.closed);
	}
}

/** @brief Verify buffer partitioning never changes the capacitor or output sequence. */
static void test_partition_invariance(void)
{
	struct detector reference = carrier(900.0);
	struct detector chunked = reference;
	unsigned char output[24000];
	for (unsigned int i = 0; i < sizeof(output); ++i) {
		double level = i < 1537 ? 900.0 : i < 11017 ? 6000.0 : 10000.0;
		feed(&reference, level, 1);
		output[i] = (unsigned char)reference.closed;
	}
	unsigned int done = 0;
	while (done < sizeof(output)) {
		unsigned int count = 1U + (done * 37U) % 960U;
		for (unsigned int j = 0; j < count && done < sizeof(output); ++j, ++done) {
			double level = done < 1537 ? 900.0 : done < 11017 ? 6000.0 : 10000.0;
			feed(&chunked, level, 1);
			assert(chunked.closed == output[done]);
		}
	}
	assert(chunked.state.noise_power == reference.state.noise_power);
	assert(chunked.state.hold_charge == reference.state.hold_charge);
}

/** @brief Report detector CPU cost without a platform-sensitive timing assertion. */
static void benchmark_detector(void)
{
	struct detector d = carrier(3000.0);
	clock_t start = clock();
	feed(&d, 3000.0, 1000 * 48000U);
	double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
	printf("1000 audio seconds: %.3f CPU seconds (%.4f%% of one core); state=%d\n", seconds,
	       seconds / 10.0, d.closed);
	assert(!d.closed);
}

/** @brief Execute continuous squelch regression tests.
 * @return Zero when all assertions pass.
 */
int main(void)
{
	test_every_carrier_loss_alignment();
	test_fast_ramps_and_weak_hold();
	test_fade_flutter_and_defeat();
	test_thresholds_startup_and_boundaries();
	test_noise_ripple_and_scale();
	test_partition_invariance();
	benchmark_detector();
	puts("Sample-clocked MICOR squelch tests passed");
	return 0;
}
