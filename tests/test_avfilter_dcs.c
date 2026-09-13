/** @file
 * @brief Spectral regression test for native DCS output shaping.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/usbradioplus_ffmpeg_adapter.h"
#include "../src/usbradioplus_dcs.h"
#include "../src/usbradioplus_radio_core_adapter.h"

/** Native CM119 sample rate. */
#define DCS_RATE 48000U
/** One native audio period. */
#define DCS_BLOCK 960U
/** Radix-two FFT length used after the FFmpeg graph reaches steady state. */
#define DCS_FFT_POINTS 32768U
/** Periods discarded while the graph's crossover history settles. */
#define DCS_SETTLE_BLOCKS 50U
/** Required maximum fraction of shaped DCS energy above 300 Hz. */
#define DCS_MAX_HIGH_FRACTION 0.01
/** Requested peak source amplitude in PCM codes. */
#define DCS_REQUESTED_PEAK 2000.0
/** Maximum permitted post-shaper DCS peak error. */
#define DCS_PEAK_TOLERANCE_DB 0.25

/* Static storage keeps this numerical test independent of the process heap. */
static double fft_real[DCS_FFT_POINTS];
static double fft_imaginary[DCS_FFT_POINTS];
static float dcs_source[DCS_BLOCK];

/** @brief Reverse the low-order FFT index bits.
 * @param value Index to reverse.
 * @param width Number of index bits.
 * @return Bit-reversed index.
 */
static unsigned int reverse_bits(unsigned int value, unsigned int width)
{
	unsigned int reversed = 0;

	while (width--) {
		reversed = (reversed << 1U) | (value & 1U);
		value >>= 1U;
	}
	return reversed;
}

/** @brief Transform a real, Blackman-Harris-windowed block in place.
 *
 * The window makes the out-of-band sum conservative for a waveform whose
 * 134.4-b/s symbol clock is not coherent with the FFT interval.
 */
static void fft(void)
{
	unsigned int index;
	unsigned int width = 0;

	for (index = DCS_FFT_POINTS; index > 1U; index >>= 1U)
		++width;
	for (index = 0; index < DCS_FFT_POINTS; ++index) {
		unsigned int reversed = reverse_bits(index, width);

		if (reversed > index) {
			double temporary = fft_real[index];

			fft_real[index] = fft_real[reversed];
			fft_real[reversed] = temporary;
		}
	}
	for (unsigned int length = 2U; length <= DCS_FFT_POINTS; length <<= 1U) {
		double step_angle = -2.0 * M_PI / (double)length;

		for (unsigned int start = 0; start < DCS_FFT_POINTS; start += length) {
			for (unsigned int offset = 0; offset < length / 2U; ++offset) {
				double angle = step_angle * (double)offset;
				double cosine = cos(angle);
				double sine = sin(angle);
				unsigned int even = start + offset;
				unsigned int odd = even + length / 2U;
				double transformed_real =
					cosine * fft_real[odd] - sine * fft_imaginary[odd];
				double transformed_imaginary =
					cosine * fft_imaginary[odd] + sine * fft_real[odd];
				double even_real = fft_real[even];
				double even_imaginary = fft_imaginary[even];

				fft_real[even] = even_real + transformed_real;
				fft_imaginary[even] = even_imaginary + transformed_imaginary;
				fft_real[odd] = even_real - transformed_real;
				fft_imaginary[odd] = even_imaginary - transformed_imaginary;
			}
		}
	}
}

/** @brief Feed one portable DCS period through the prepared shared graph.
 * @param encoder Portable transmitter state.
 * @param graph Prepared spectral-shaping graph.
 * @param output Receives shaped PCM samples.
 * @param turnoff Nonzero renders the 134.4 Hz DCS turn-off code.
 */
static void render_block(struct rptadv_radio *encoder, struct usbradioplus_ffmpeg_adapter *graph,
			 float *output, int turnoff)
{
	assert(!urp_radio_core_generate_dcs(encoder, dcs_source, DCS_BLOCK,
					    DCS_REQUESTED_PEAK / 32767.0, 1, turnoff));
	assert(!usbradioplus_ffmpeg_adapter_process_block(graph, dcs_source, DCS_BLOCK, output));
}

/** @brief Assert the fixed DCS graph preserves the configured source peak. */
static void assert_shaped_peak(double peak)
{
	double minimum = DCS_REQUESTED_PEAK * pow(10.0, -DCS_PEAK_TOLERANCE_DB / 20.0);
	double maximum = DCS_REQUESTED_PEAK * pow(10.0, DCS_PEAK_TOLERANCE_DB / 20.0);

	fprintf(stderr, "DCS post-shaper peak: %.3f PCM\n", peak);
	assert(peak >= minimum);
	assert(peak <= maximum);
}

/** @brief Assert DCS retains its intended baseband signal and meets its spectral limit. */
static void test_dcs_spectral_shaping(void)
{
	struct usbradioplus_ffmpeg_adapter graph = {0};
	struct rptadv_radio *encoder = NULL;
	float block[DCS_BLOCK];
	double in_band_power = 0.0;
	double total_power = 0.0;
	double high_power = 0.0;
	double peak = 0.0;
	unsigned int captured = 0;

	assert(!urp_radio_core_create(DCS_RATE, DCS_BLOCK, &encoder));
	assert(!urp_radio_core_configure_dcs(encoder, 023, 0));
	assert(!usbradioplus_ffmpeg_adapter_prepare_dcs(&graph, 0, DCS_RATE, DCS_BLOCK));
	for (unsigned int period = 0; period < DCS_SETTLE_BLOCKS; ++period)
		render_block(encoder, &graph, block, 0);
	while (captured < DCS_FFT_POINTS) {
		render_block(encoder, &graph, block, 0);
		for (unsigned int sample = 0; sample < DCS_BLOCK && captured < DCS_FFT_POINTS;
		     ++sample, ++captured) {
			double phase = 2.0 * M_PI * (double)captured / (DCS_FFT_POINTS - 1U);
			double window = 0.35875 - 0.48829 * cos(phase) +
					0.14128 * cos(2.0 * phase) - 0.01168 * cos(3.0 * phase);
			double magnitude = fabs(block[sample]) * 32767.0;

			if (magnitude > peak)
				peak = magnitude;
			fft_real[captured] = block[sample] * window;
			fft_imaginary[captured] = 0.0;
		}
	}
	fft();
	for (unsigned int bin = 1; bin < DCS_FFT_POINTS / 2U; ++bin) {
		double frequency = (double)bin * DCS_RATE / DCS_FFT_POINTS;
		double power =
			fft_real[bin] * fft_real[bin] + fft_imaginary[bin] * fft_imaginary[bin];

		total_power += power;
		if (frequency >= 100.0 && frequency <= 175.0)
			in_band_power += power;
		if (frequency > 300.0)
			high_power += power;
	}
	printf("DCS spectral power: 100-175 Hz=%.3f%%, above 300 Hz=%.5f%%\n",
	       100.0 * in_band_power / total_power, 100.0 * high_power / total_power);
	assert(total_power > 0.0);
	/* A Golay NRZ word spreads power across its data sidebands, so the 134.4 Hz
	 * neighborhood is not expected to contain most total baseband energy. It
	 * must nevertheless retain a clear, measurable carrier neighborhood. */
	assert(in_band_power / total_power > 0.01);
	assert(high_power / total_power < DCS_MAX_HIGH_FRACTION);
	assert_shaped_peak(peak);
	usbradioplus_ffmpeg_adapter_close(&graph);
	urp_radio_core_destroy(encoder);
}

/** @brief Verify the shaped 134.4 Hz DCS turn-off code preserves level and spectrum. */
static void test_dcs_turnoff_spectral_shaping(void)
{
	struct usbradioplus_ffmpeg_adapter graph = {0};
	struct rptadv_radio *encoder = NULL;
	float block[DCS_BLOCK];
	double total_power = 0.0;
	double high_power = 0.0;
	double dominant_power = 0.0;
	double dominant_frequency = 0.0;
	double peak = 0.0;
	unsigned int captured = 0;

	assert(!urp_radio_core_create(DCS_RATE, DCS_BLOCK, &encoder));
	assert(!urp_radio_core_configure_dcs(encoder, 023, 0));
	assert(!usbradioplus_ffmpeg_adapter_prepare_dcs(&graph, 1, DCS_RATE, DCS_BLOCK));
	for (unsigned int period = 0; period < DCS_SETTLE_BLOCKS; ++period)
		render_block(encoder, &graph, block, 1);
	while (captured < DCS_FFT_POINTS) {
		render_block(encoder, &graph, block, 1);
		for (unsigned int sample = 0; sample < DCS_BLOCK && captured < DCS_FFT_POINTS;
		     ++sample, ++captured) {
			double phase = 2.0 * M_PI * (double)captured / (DCS_FFT_POINTS - 1U);
			double window = 0.35875 - 0.48829 * cos(phase) +
					0.14128 * cos(2.0 * phase) - 0.01168 * cos(3.0 * phase);
			double magnitude = fabs(block[sample]) * 32767.0;

			if (magnitude > peak)
				peak = magnitude;
			fft_real[captured] = block[sample] * window;
			fft_imaginary[captured] = 0.0;
		}
	}
	fft();
	for (unsigned int bin = 1; bin < DCS_FFT_POINTS / 2U; ++bin) {
		double frequency = (double)bin * DCS_RATE / DCS_FFT_POINTS;
		double power =
			fft_real[bin] * fft_real[bin] + fft_imaginary[bin] * fft_imaginary[bin];

		total_power += power;
		if (frequency > 300.0)
			high_power += power;
		if (frequency >= 100.0 && frequency <= 175.0 && power > dominant_power) {
			dominant_power = power;
			dominant_frequency = frequency;
		}
	}
	printf("DCS turn-off spectral power: dominant=%.3f Hz, above 300 Hz=%.5f%%\n",
	       dominant_frequency, 100.0 * high_power / total_power);
	assert(total_power > 0.0);
	assert(dominant_power > 0.0);
	assert(fabs(dominant_frequency - URP_DCS_TURNOFF_FREQUENCY_HZ) <
	       1.5 * DCS_RATE / DCS_FFT_POINTS);
	assert(high_power / total_power < DCS_MAX_HIGH_FRACTION);
	assert_shaped_peak(peak);
	usbradioplus_ffmpeg_adapter_close(&graph);
	urp_radio_core_destroy(encoder);
}

/** @brief Callback partitioning must not change either shaper's PCM history. */
static void test_partitioned_shaping(void)
{
	for (int turnoff = 0; turnoff <= 1; ++turnoff) {
		struct usbradioplus_ffmpeg_adapter whole = {0};
		struct usbradioplus_ffmpeg_adapter split = {0};
		struct rptadv_radio *encoder = NULL;
		float expected[DCS_BLOCK];
		float actual[DCS_BLOCK];

		assert(!urp_radio_core_create(DCS_RATE, DCS_BLOCK, &encoder));
		assert(!urp_radio_core_configure_dcs(encoder, 023, 0));
		assert(!usbradioplus_ffmpeg_adapter_prepare_dcs(&whole, turnoff, DCS_RATE,
								DCS_BLOCK));
		assert(!usbradioplus_ffmpeg_adapter_prepare_dcs(&split, turnoff, DCS_RATE,
								DCS_BLOCK));
		for (unsigned block = 0; block < DCS_SETTLE_BLOCKS; ++block) {
			assert(!urp_radio_core_generate_dcs(encoder, dcs_source, DCS_BLOCK,
							    DCS_REQUESTED_PEAK / 32767.0, 1,
							    turnoff));
			assert(!usbradioplus_ffmpeg_adapter_process_block(&whole, dcs_source,
									  DCS_BLOCK, expected));
			assert(!usbradioplus_ffmpeg_adapter_process_block(&split, dcs_source, 317,
									  actual));
			assert(!usbradioplus_ffmpeg_adapter_process_block(
				&split, dcs_source + 317, DCS_BLOCK - 317, actual + 317));
			for (unsigned sample = 0; sample < DCS_BLOCK; ++sample)
				assert(fabsf(actual[sample] - expected[sample]) < 1e-7F);
		}
		usbradioplus_ffmpeg_adapter_close(&whole);
		usbradioplus_ffmpeg_adapter_close(&split);
		urp_radio_core_destroy(encoder);
	}
}

/** @brief Execute the DCS shared-FFmpeg spectral regression test.
 * @return Zero when all assertions pass.
 */
int main(void)
{
	assert(!urp_radio_core_initialize());
	test_dcs_spectral_shaping();
	test_dcs_turnoff_spectral_shaping();
	test_partitioned_shaping();
	return 0;
}

/** @def DCS_RATE
 * @brief Native DCS synthesis sample rate.
 */
/** @def DCS_BLOCK
 * @brief Native DCS callback period in samples.
 */
/** @def DCS_FFT_POINTS
 * @brief FFT length used to sum shaped DCS spectral power.
 */
/** @def DCS_SETTLE_BLOCKS
 * @brief Number of graph-history periods discarded before measurement.
 */
/** @def DCS_MAX_HIGH_FRACTION
 * @brief Maximum allowed shaped DCS power above 300 Hz.
 */
/** @def DCS_REQUESTED_PEAK
 * @brief Requested DCS source peak in PCM codes.
 */
/** @def DCS_PEAK_TOLERANCE_DB
 * @brief Maximum permitted post-shaper DCS peak error.
 */
