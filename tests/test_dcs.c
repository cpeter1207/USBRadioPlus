/** @file
 * @brief Unit tests for native DCS encoding, correction, polarity, and turn-off output.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_dcs.h"

/** @brief TIA/ETSI 023N codeword, written in on-air LSB-first bit order. */
#define DCS_023N_WIRE_WORD 0x763813U
/** @brief TIA/ETSI 431N codeword, used as a nonmatching external vector. */
#define DCS_431N_WIRE_WORD 0x6c5919U
/** @brief Mask for a complete 23-bit DCS word. */
#define DCS_TEST_WORD_MASK 0x7fffffU
/** @brief Native CM119 discriminator sample rate used by this harness. */
#define DCS_TEST_SAMPLE_RATE 48000U
/** @brief Narrowband app_rpt rate supported by the generic DCS decoder API. */
#define DCS_TEST_LINK_SAMPLE_RATE 8000U
/** @brief Longest native-rate DCS symbol at 48 kHz. */
#define DCS_TEST_MAX_SYMBOL_SAMPLES 358U
/** @brief Samples supplied after each arbitrary capture start offset. */
#define DCS_TEST_PHASE_CAPTURE_SAMPLES 20000U
/** @brief Source length needed for a full phase-offset capture. */
#define DCS_TEST_PHASE_SOURCE_SAMPLES (DCS_TEST_PHASE_CAPTURE_SAMPLES + DCS_TEST_MAX_SYMBOL_SAMPLES)
/** @brief Longest 8 kHz DCS symbol. */
#define DCS_TEST_LINK_MAX_SYMBOL_SAMPLES 60U
/** @brief Samples supplied after each 8 kHz phase offset. */
#define DCS_TEST_LINK_CAPTURE_SAMPLES 4000U
/** @brief Source length needed for a full 8 kHz phase-offset capture. */
#define DCS_TEST_LINK_SOURCE_SAMPLES                                                               \
	(DCS_TEST_LINK_CAPTURE_SAMPLES + DCS_TEST_LINK_MAX_SYMBOL_SAMPLES)
/** @brief Portable pi used to assess the turn-off tone phase. */
#define DCS_TEST_PI 3.14159265358979323846
/** @brief Tail shorter than the receive detector's accepted duration. */
#define DCS_TEST_TURNOFF_SHORT_MS 80U
/** @brief Exact minimum valid 134.4 Hz tail duration for prompt receive clearing. */
#define DCS_TEST_TURNOFF_VALID_MS URP_DCS_TURNOFF_MINIMUM_MS
/** @brief Normal DCS interval used to qualify and rekey a receiver. */
#define DCS_TEST_NORMAL_MS 500U
/** @brief Normal DCS interval that resets a partial tail detector window. */
#define DCS_TEST_TURNOFF_RESET_MS 40U
/** @brief Largest selected-channel tail capture used by DCS tail tests. */
#define DCS_TEST_TURNOFF_MAX_SAMPLES ((DCS_TEST_SAMPLE_RATE * DCS_TEST_TURNOFF_VALID_MS) / 1000U)
/** @brief Weak but usable discriminator DCS symbol amplitude in PCM codes. */
#define DCS_TEST_DC_BIASED_SIGNAL_PCM 500
/** @brief Realistic fixed discriminator bias that exceeds the weak symbol amplitude. */
#define DCS_TEST_DC_BIAS_PCM 800
/** @brief Capture span that allows the slow DC tracker to settle while decoding. */
#define DCS_TEST_DC_BIASED_CAPTURE_SAMPLES (DCS_TEST_SAMPLE_RATE * 2U)

static int16_t turnoff_normal[DCS_TEST_SAMPLE_RATE * 2U];
static int16_t turnoff_tail[DCS_TEST_TURNOFF_MAX_SAMPLES * 2U];
static int16_t turnoff_noise[DCS_TEST_TURNOFF_MAX_SAMPLES * 2U];
static int16_t turnoff_capture[DCS_TEST_SAMPLE_RATE * 2U];
static double generated_turnoff[DCS_TEST_TURNOFF_MAX_SAMPLES];
static int16_t dc_biased_capture[DCS_TEST_DC_BIASED_CAPTURE_SAMPLES];

/** @brief Return an exact whole-millisecond selected-channel sample count. */
static size_t samples_for_ms(unsigned int sample_rate, unsigned int milliseconds)
{
	return ((size_t)sample_rate * milliseconds) / 1000U;
}

/** @brief Copy mono selected-channel PCM into a mono or stereo capture buffer. */
static void interleave_selected_channel(int16_t *destination, const int16_t *source, size_t count,
					size_t stride)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		destination[index * stride] = source[index];
		if (stride == 2U)
			destination[index * stride + 1U] = (int16_t)-source[index];
	}
}

/** @brief Render the fixed-frequency DCS turn-off sine at one sample rate. */
static void render_turnoff_tone(int16_t *samples, size_t count, unsigned int sample_rate)
{
	double phase = 0.0;
	double step = 2.0 * DCS_TEST_PI * URP_DCS_TURNOFF_FREQUENCY_HZ / sample_rate;
	size_t index;

	for (index = 0; index < count; ++index) {
		samples[index] = (int16_t)lround(4000.0 * sin(phase));
		phase += step;
		if (phase >= 2.0 * DCS_TEST_PI)
			phase -= 2.0 * DCS_TEST_PI;
	}
}

/** @brief Render deterministic wideband discriminator noise. */
static void render_noise(int16_t *samples, size_t count)
{
	uint32_t state = 0x71f2a3c5U;
	size_t index;

	for (index = 0; index < count; ++index) {
		state = state * 1664525U + 1013904223U;
		samples[index] = (int16_t)((int)((state >> 16) & 0x1fffU) - 4096);
	}
}

/** @brief Render an externally specified TIA/ETSI DCS wire word.
 * @param samples Destination mono PCM samples.
 * @param count Number of destination samples.
 * @param word TIA/ETSI codeword whose bit zero is sent first.
 */
static void render_wire_word_at_rate(int16_t *samples, size_t count, uint32_t word,
				     unsigned int sample_rate)
{
	uint32_t accumulator = 0;
	unsigned int phase = 0;
	size_t index;

	for (index = 0; index < count; ++index) {
		samples[index] = (word & (1U << phase)) ? 4000 : -4000;
		accumulator += (uint32_t)(URP_DCS_BIT_RATE * 10.0);
		if (accumulator >= sample_rate * 10U) {
			accumulator -= sample_rate * 10U;
			phase = (phase + 1U) % 23U;
		}
	}
}

/** @brief Render an external DCS wire word at the native test rate.
 * @param samples Destination mono PCM samples.
 * @param count Number of destination samples.
 * @param word TIA/ETSI codeword whose bit zero is sent first.
 */
static void render_wire_word(int16_t *samples, size_t count, uint32_t word)
{
	render_wire_word_at_rate(samples, count, word, DCS_TEST_SAMPLE_RATE);
}

/** @brief Render one external word followed by a different repeated word.
 * @param samples Destination mono PCM samples.
 * @param count Number of destination samples.
 * @param first First on-air, LSB-first codeword.
 * @param following Repeated on-air, LSB-first codeword after the first word.
 */
static void render_wire_word_then_word(int16_t *samples, size_t count, uint32_t first,
				       uint32_t following)
{
	uint32_t accumulator = 0;
	unsigned int phase = 0;
	int first_word = 1;
	size_t index;

	for (index = 0; index < count; ++index) {
		uint32_t word = first_word ? first : following;

		samples[index] = (word & (1U << phase)) ? 4000 : -4000;
		accumulator += (uint32_t)(URP_DCS_BIT_RATE * 10.0);
		if (accumulator >= DCS_TEST_SAMPLE_RATE * 10U) {
			accumulator -= DCS_TEST_SAMPLE_RATE * 10U;
			if (++phase == 23U) {
				phase = 0;
				first_word = 0;
			}
		}
	}
}

/** @brief Render a DCS word with repeatable additive noise and selected symbol inversions.
 *
 * The inversions are repeated on each codeword, which directly exercises the
 * three-symbol Golay-correction limit rather than relying on a test-only
 * decoder mutation.
 *
 * @param samples Destination mono PCM samples.
 * @param count Number of destination samples.
 * @param word TIA/ETSI codeword whose bit zero is sent first.
 * @param inverted_symbols Bit mask of symbol positions to invert.
 * @param dc_offset Constant discriminator offset in PCM codes.
 * @param noise_peak Peak additive pseudo-random noise in PCM codes.
 */
static void render_impaired_wire_word(int16_t *samples, size_t count, uint32_t word,
				      uint32_t inverted_symbols, int dc_offset, int noise_peak)
{
	uint32_t accumulator = 0;
	uint32_t noise_state = 0x13579bdfU;
	unsigned int phase = 0;
	size_t index;

	for (index = 0; index < count; ++index) {
		int positive = !!(word & (1U << phase));
		int noise;

		if (inverted_symbols & (1U << phase))
			positive = !positive;
		noise_state = noise_state * 1664525U + 1013904223U;
		noise = (int)((noise_state >> 16) & 0x7ffU) - 1024;
		noise = (noise * noise_peak) / 1024;
		samples[index] = (int16_t)((positive ? 4000 : -4000) + dc_offset + noise);
		accumulator += (uint32_t)(URP_DCS_BIT_RATE * 10.0);
		if (accumulator >= DCS_TEST_SAMPLE_RATE * 10U) {
			accumulator -= DCS_TEST_SAMPLE_RATE * 10U;
			phase = (phase + 1U) % 23U;
		}
	}
}

/** @brief Feed one capture in deliberately uneven callback-sized fragments.
 * @param state Decoder state.
 * @param samples First selected-channel sample.
 * @param count Number of selected-channel samples.
 * @param stride PCM-word stride between selected-channel samples.
 * @param pattern_offset Offset into the callback-size pattern.
 * @return Final decoder qualification state.
 */
static int process_fragmented_capture(struct urp_dcs_state *state, const int16_t *samples,
				      size_t count, size_t stride, unsigned int sample_rate,
				      size_t pattern_offset)
{
	static const size_t fragment_sizes[] = {1U, 17U, 359U, 960U, 53U, 711U, 2U, 487U};
	size_t position = 0;
	size_t fragment = 0;
	int valid = 0;

	while (position < count) {
		size_t length =
			fragment_sizes[(pattern_offset + fragment) %
				       (sizeof(fragment_sizes) / sizeof(fragment_sizes[0]))];

		if (length > count - position)
			length = count - position;
		valid = urp_dcs_process(state, samples + position * stride, length, stride,
					sample_rate);
		position += length;
		++fragment;
	}
	return valid;
}

/** @brief Verify the first transmitted DCS word against an external wire vector.
 * @param samples Generated PCM samples.
 * @param count Number of generated PCM samples.
 * @param word Expected TIA/ETSI word with bit zero transmitted first.
 */
static void assert_generated_word(const double *samples, size_t count, uint32_t word)
{
	uint32_t accumulator = 0;
	size_t index = 0;
	unsigned int phase;

	for (phase = 0; phase < 23U; ++phase) {
		assert(index < count);
		assert((samples[index] >= 0.0) == !!(word & (1U << phase)));
		do {
			accumulator += (uint32_t)(URP_DCS_BIT_RATE * 10.0);
			++index;
		} while (accumulator < DCS_TEST_SAMPLE_RATE * 10U);
		accumulator -= DCS_TEST_SAMPLE_RATE * 10U;
	}
}

static void test_code_syntax(void)
{
	char code[5];
	int value, inverted;
	assert(!urp_dcs_parse_code("023N", &value, &inverted));
	assert(value == 023 && !inverted);
	assert(!urp_dcs_parse_code("754i", &value, &inverted));
	assert(value == 0754 && inverted);
	assert(!urp_dcs_parse_code("000N", &value, &inverted));
	assert(value == 000 && !inverted);
	assert(!urp_dcs_parse_code("001I", &value, &inverted));
	assert(value == 001 && inverted);
	assert(urp_dcs_parse_code("23N", &value, &inverted));
	assert(urp_dcs_parse_code("888N", &value, &inverted));
	assert(urp_dcs_parse_code("088N", &value, &inverted));
	assert(urp_dcs_parse_code("008N", &value, &inverted));
	assert(urp_dcs_parse_code("/23N", &value, &inverted));
	assert(urp_dcs_parse_code("0/3N", &value, &inverted));
	assert(urp_dcs_parse_code("02/N", &value, &inverted));
	assert(urp_dcs_parse_code("023X", &value, &inverted));
	assert(urp_dcs_parse_code(NULL, &value, &inverted));
	assert(urp_dcs_parse_code("023N", NULL, &inverted));
	assert(urp_dcs_parse_code("023N", &value, NULL));
	urp_dcs_format_code(code, sizeof(code), 023, 1);
	assert(!strcmp(code, "023I"));
	urp_dcs_format_code(code, sizeof(code), 023, 0);
	assert(!strcmp(code, "023N"));
	urp_dcs_format_code(NULL, sizeof(code), 023, 1);
	urp_dcs_format_code(code, 0, 023, 1);
}

/** @brief Verify every three-digit octal code and polarity can round-trip. */
static void test_supported_codes(void)
{
	int candidate;

	for (candidate = -1; candidate <= 01000; ++candidate) {
		int expected = candidate >= 0 && candidate <= 0777;

		assert(urp_dcs_code_supported(candidate) == expected);
	}
	for (candidate = 0; candidate <= 0777; ++candidate) {
		char normal[5];
		char inverse[5];
		int code;
		int inverted;

		assert(snprintf(normal, sizeof(normal), "%03oN", candidate) == 4);
		assert(snprintf(inverse, sizeof(inverse), "%03oI", candidate) == 4);
		assert(!urp_dcs_parse_code(normal, &code, &inverted));
		assert(code == candidate && !inverted);
		assert(!urp_dcs_parse_code(inverse, &code, &inverted));
		assert(code == candidate && inverted);
	}
}

static void test_disabled_interfaces(void)
{
	struct urp_dcs_state state;
	double output[2] = {1.0, 1.0};
	int16_t input[2] = {0, 0};

	urp_dcs_init(NULL);
	urp_dcs_init(&state);
	urp_dcs_configure(NULL, 0, 0, 0, 0);
	urp_dcs_configure(&state, -1, 0, -1, 0);
	urp_dcs_configure(&state, 000, 0, 001, 1);
	assert(state.enabled_receive && state.enabled_transmit);
	urp_dcs_configure(&state, 01000, 0, 01000, 1);
	assert(!state.enabled_receive && !state.enabled_transmit);
	urp_dcs_configure(&state, -1, 0, -1, 0);
	assert(!urp_dcs_process(NULL, input, 2, 1, DCS_TEST_SAMPLE_RATE));
	assert(!urp_dcs_process(&state, NULL, 2, 1, DCS_TEST_SAMPLE_RATE));
	assert(!urp_dcs_process(&state, input, 2, 0, DCS_TEST_SAMPLE_RATE));
	assert(!urp_dcs_process(&state, input, 2, 1, 0));
	assert(!urp_dcs_process(&state, input, 2, 1, DCS_TEST_SAMPLE_RATE));
	urp_dcs_generate(NULL, output, 2, 48000, 1.0, 1, 0);
	assert(output[0] == 0.0 && output[1] == 0.0);
	urp_dcs_generate(&state, output, 2, 48000, 0.0, 1, 0);
	assert(output[0] == 0.0 && output[1] == 0.0);
	urp_dcs_generate(&state, output, 2, 0, 1.0, 1, 0);
	assert(output[0] == 0.0 && output[1] == 0.0);
	urp_dcs_generate(&state, output, 2, 48000, 1.0, 0, 0);
	assert(output[0] == 0.0 && output[1] == 0.0);
	urp_dcs_generate(&state, NULL, 2, 48000, 1.0, 1, 0);
	urp_dcs_generate(&state, output, 0, 48000, 1.0, 1, 0);
}

static void test_round_trip(void)
{
	struct urp_dcs_state encoder, decoder;
	double rendered[48000];
	int16_t samples[48000];
	size_t index;
	urp_dcs_init(&encoder);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&encoder, -1, 0, 023, 0);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	urp_dcs_generate(&encoder, rendered, 48000, DCS_TEST_SAMPLE_RATE, 4000.0, 1, 0);
	for (index = 0; index < 48000; ++index)
		samples[index] = (int16_t)rendered[index];
	assert(urp_dcs_process(&decoder, samples, 48000, 1, DCS_TEST_SAMPLE_RATE));
}

/** @brief Verify all 512 octal code values round-trip at the generic decoder rate. */
static void test_all_code_round_trips(void)
{
	double rendered[DCS_TEST_LINK_CAPTURE_SAMPLES];
	int16_t samples[DCS_TEST_LINK_CAPTURE_SAMPLES];
	int code;
	int inverted;
	size_t index;

	for (code = 0; code <= 0777; ++code) {
		for (inverted = 0; inverted <= 1; ++inverted) {
			struct urp_dcs_state encoder;
			struct urp_dcs_state decoder;

			urp_dcs_init(&encoder);
			urp_dcs_init(&decoder);
			urp_dcs_configure(&encoder, -1, 0, code, inverted);
			urp_dcs_configure(&decoder, code, inverted, -1, 0);
			urp_dcs_generate(&encoder, rendered, DCS_TEST_LINK_CAPTURE_SAMPLES,
					 DCS_TEST_LINK_SAMPLE_RATE, 4000.0, 1, 0);
			for (index = 0; index < DCS_TEST_LINK_CAPTURE_SAMPLES; ++index)
				samples[index] = (int16_t)rendered[index];
			assert(urp_dcs_process(&decoder, samples, DCS_TEST_LINK_CAPTURE_SAMPLES, 1,
					       DCS_TEST_LINK_SAMPLE_RATE));
			/* Repeated normal DCS words cannot satisfy the coherent tail detector. */
			assert(!decoder.receive_turnoff_active);
		}
	}
}

/** @brief Verify encoder and decoder against the published 023 DCS wire vector. */
static void test_tia_etsi_wire_vector(void)
{
	struct urp_dcs_state encoder, decoder;
	double generated[9000];
	int16_t mono[24000];
	int16_t stereo[24000 * 2];
	size_t index;

	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 023, 0);
	urp_dcs_generate(&encoder, generated, 9000, DCS_TEST_SAMPLE_RATE, 4000.0, 1, 0);
	assert_generated_word(generated, 9000, DCS_023N_WIRE_WORD);

	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 023, 1);
	urp_dcs_generate(&encoder, generated, 9000, DCS_TEST_SAMPLE_RATE, 4000.0, 1, 0);
	assert_generated_word(generated, 9000, DCS_023N_WIRE_WORD ^ DCS_TEST_WORD_MASK);
	/* 431N is a second independent Table-2 vector. */
	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 0431, 0);
	urp_dcs_generate(&encoder, generated, 9000, DCS_TEST_SAMPLE_RATE, 4000.0, 1, 0);
	assert_generated_word(generated, 9000, DCS_431N_WIRE_WORD);
	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 0431, 1);
	urp_dcs_generate(&encoder, generated, 9000, DCS_TEST_SAMPLE_RATE, 4000.0, 1, 0);
	assert_generated_word(generated, 9000, DCS_431N_WIRE_WORD ^ DCS_TEST_WORD_MASK);

	render_wire_word(mono, 24000, DCS_023N_WIRE_WORD);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(urp_dcs_process(&decoder, mono, 24000, 1, DCS_TEST_SAMPLE_RATE));

	render_wire_word(mono, 24000, DCS_023N_WIRE_WORD ^ DCS_TEST_WORD_MASK);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 1, -1, 0);
	assert(urp_dcs_process(&decoder, mono, 24000, 1, DCS_TEST_SAMPLE_RATE));

	render_wire_word(mono, 24000, DCS_023N_WIRE_WORD);
	for (index = 0; index < 24000; ++index) {
		stereo[index * 2] = mono[index];
		stereo[index * 2 + 1] = 0;
	}
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(urp_dcs_process(&decoder, stereo, 24000, 2, DCS_TEST_SAMPLE_RATE));
}

/** @brief A single matching word cannot open DCS squelch. */
static void test_receive_requires_two_spaced_words(void)
{
	int16_t samples[24000];
	struct urp_dcs_state decoder;

	render_wire_word_then_word(samples, sizeof(samples) / sizeof(samples[0]),
				   DCS_023N_WIRE_WORD, DCS_431N_WIRE_WORD);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(!process_fragmented_capture(&decoder, samples, sizeof(samples) / sizeof(samples[0]),
					   1, DCS_TEST_SAMPLE_RATE, 0));
}

/** @brief A qualified result survives one missing word but then clears. */
static void test_receive_qualified_loss_hold(void)
{
	int16_t matching[24000];
	int16_t nonmatching[24000];
	struct urp_dcs_state decoder;

	render_wire_word(matching, sizeof(matching) / sizeof(matching[0]), DCS_023N_WIRE_WORD);
	render_wire_word(nonmatching, sizeof(nonmatching) / sizeof(nonmatching[0]),
			 DCS_431N_WIRE_WORD);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(process_fragmented_capture(&decoder, matching,
					  sizeof(matching) / sizeof(matching[0]), 1,
					  DCS_TEST_SAMPLE_RATE, 0));
	/* 4,000 native samples are fewer than one 23-symbol word. */
	assert(process_fragmented_capture(&decoder, nonmatching, 4000, 1, DCS_TEST_SAMPLE_RATE, 1));
	assert(!process_fragmented_capture(&decoder, nonmatching + 4000,
					   sizeof(nonmatching) / sizeof(nonmatching[0]) - 4000, 1,
					   DCS_TEST_SAMPLE_RATE, 2));
}

/** @brief Verify every native callback-to-symbol offset is covered by the phase bank. */
static void assert_receive_phase_bank_offsets(const int16_t *samples, size_t stride,
					      unsigned int sample_rate, size_t max_symbol_samples,
					      size_t capture_samples, int inverted)
{
	size_t offset;

	/* Exhaust every possible callback-to-symbol offset and selected PCM stride. */
	for (offset = 0; offset < max_symbol_samples; ++offset) {
		struct urp_dcs_state decoder;

		urp_dcs_init(&decoder);
		urp_dcs_configure(&decoder, 023, inverted, -1, 0);
		assert(process_fragmented_capture(&decoder, samples + offset * stride,
						  capture_samples, stride, sample_rate, offset));
	}
}

/** @brief Verify every supported callback-to-symbol phase offset is decodable. */
static void test_receive_phase_bank_offsets(void)
{
	static int16_t normal[DCS_TEST_PHASE_SOURCE_SAMPLES];
	static int16_t inverse[DCS_TEST_PHASE_SOURCE_SAMPLES];
	static int16_t normal_stereo[DCS_TEST_PHASE_SOURCE_SAMPLES * 2U];
	static int16_t inverse_stereo[DCS_TEST_PHASE_SOURCE_SAMPLES * 2U];
	static int16_t normal_link[DCS_TEST_LINK_SOURCE_SAMPLES];
	static int16_t inverse_link[DCS_TEST_LINK_SOURCE_SAMPLES];
	int16_t right[DCS_TEST_PHASE_SOURCE_SAMPLES];
	size_t index;

	render_wire_word(normal, sizeof(normal) / sizeof(normal[0]), DCS_023N_WIRE_WORD);
	render_wire_word(inverse, sizeof(inverse) / sizeof(inverse[0]),
			 DCS_023N_WIRE_WORD ^ DCS_TEST_WORD_MASK);
	render_wire_word(right, sizeof(right) / sizeof(right[0]), DCS_431N_WIRE_WORD);
	render_wire_word_at_rate(normal_link, sizeof(normal_link) / sizeof(normal_link[0]),
				 DCS_023N_WIRE_WORD, DCS_TEST_LINK_SAMPLE_RATE);
	render_wire_word_at_rate(inverse_link, sizeof(inverse_link) / sizeof(inverse_link[0]),
				 DCS_023N_WIRE_WORD ^ DCS_TEST_WORD_MASK,
				 DCS_TEST_LINK_SAMPLE_RATE);
	for (index = 0; index < sizeof(normal) / sizeof(normal[0]); ++index) {
		normal_stereo[index * 2U] = normal[index];
		normal_stereo[index * 2U + 1U] = right[index];
		inverse_stereo[index * 2U] = inverse[index];
		inverse_stereo[index * 2U + 1U] = -right[index];
	}
	assert_receive_phase_bank_offsets(normal, 1U, DCS_TEST_SAMPLE_RATE,
					  DCS_TEST_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_PHASE_CAPTURE_SAMPLES, 0);
	assert_receive_phase_bank_offsets(inverse, 1U, DCS_TEST_SAMPLE_RATE,
					  DCS_TEST_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_PHASE_CAPTURE_SAMPLES, 1);
	assert_receive_phase_bank_offsets(normal_stereo, 2U, DCS_TEST_SAMPLE_RATE,
					  DCS_TEST_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_PHASE_CAPTURE_SAMPLES, 0);
	assert_receive_phase_bank_offsets(inverse_stereo, 2U, DCS_TEST_SAMPLE_RATE,
					  DCS_TEST_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_PHASE_CAPTURE_SAMPLES, 1);
	assert_receive_phase_bank_offsets(normal_link, 1U, DCS_TEST_LINK_SAMPLE_RATE,
					  DCS_TEST_LINK_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_LINK_CAPTURE_SAMPLES, 0);
	assert_receive_phase_bank_offsets(inverse_link, 1U, DCS_TEST_LINK_SAMPLE_RATE,
					  DCS_TEST_LINK_MAX_SYMBOL_SAMPLES,
					  DCS_TEST_LINK_CAPTURE_SAMPLES, 1);
}

/** @brief Verify phase recovery tolerates three corrected symbols plus discriminator noise. */
static void test_receive_phase_bank_correction_and_noise(void)
{
	int16_t impaired[DCS_TEST_PHASE_SOURCE_SAMPLES];
	struct urp_dcs_state decoder;

	render_impaired_wire_word(impaired, sizeof(impaired) / sizeof(impaired[0]),
				  DCS_023N_WIRE_WORD, (1U << 2) | (1U << 9) | (1U << 17), 700, 900);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(process_fragmented_capture(&decoder, impaired + DCS_TEST_MAX_SYMBOL_SAMPLES / 2U,
					  DCS_TEST_PHASE_CAPTURE_SAMPLES, 1, DCS_TEST_SAMPLE_RATE,
					  DCS_TEST_MAX_SYMBOL_SAMPLES / 2U));
}

/** @brief Verify a weak DCS discriminator signal survives a normal DC bias.
 *
 * Both biased symbols are below the old integer tracker's 32,768-code
 * update quantum.  The former implementation therefore kept a zero estimate
 * forever and made every bit positive.  The fractional tracker must settle
 * while the live signal remains present and then qualify the configured code.
 */
static void test_receive_dc_offset_tracking(void)
{
	struct urp_dcs_state decoder;
	uint32_t accumulator = 0;
	unsigned int phase = 0;
	size_t index;

	for (index = 0; index < sizeof(dc_biased_capture) / sizeof(dc_biased_capture[0]); ++index) {
		dc_biased_capture[index] = (int16_t)(DCS_TEST_DC_BIAS_PCM +
						     ((DCS_023N_WIRE_WORD & (1U << phase))
							      ? DCS_TEST_DC_BIASED_SIGNAL_PCM
							      : -DCS_TEST_DC_BIASED_SIGNAL_PCM));
		accumulator += (uint32_t)(URP_DCS_BIT_RATE * 10.0);
		if (accumulator >= DCS_TEST_SAMPLE_RATE * 10U) {
			accumulator -= DCS_TEST_SAMPLE_RATE * 10U;
			phase = (phase + 1U) % 23U;
		}
	}
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(process_fragmented_capture(&decoder, dc_biased_capture,
					  sizeof(dc_biased_capture) / sizeof(dc_biased_capture[0]),
					  1U, DCS_TEST_SAMPLE_RATE, 6U));
}

static void test_polarity_and_turnoff(void)
{
	struct urp_dcs_state encoder, decoder;
	double normal[960], tail[960];
	int16_t samples[960];
	size_t index;
	urp_dcs_init(&encoder);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&encoder, -1, 0, 0431, 1);
	urp_dcs_configure(&decoder, 0431, 1, -1, 0);
	for (index = 0; index < 80; ++index) {
		size_t sample;
		urp_dcs_generate(&encoder, normal, 960, DCS_TEST_SAMPLE_RATE, 2000.0, 1, 0);
		for (sample = 0; sample < 960; ++sample)
			samples[sample] = (int16_t)normal[sample];
		(void)urp_dcs_process(&decoder, samples, 960, 1, DCS_TEST_SAMPLE_RATE);
	}
	assert(decoder.valid);
	urp_dcs_generate(&encoder, tail, 960, DCS_TEST_SAMPLE_RATE, 2000.0, 1, 1);
	assert(fabs(tail[0]) < 0.01);
	assert(fabs(tail[100]) > 10.0);
}

/** @brief Verify one qualified receiver clears only for a complete DCS tail.
 *
 * The same test is run at native and app_rpt rates, with both receive
 * polarities and selected-channel strides.  Fragmenting every capture proves
 * detector timing is independent of callback boundaries.
 */
static void assert_receive_turnoff_at_rate(unsigned int sample_rate, size_t stride, int inverted,
					   size_t fragment_offset)
{
	struct urp_dcs_state encoder;
	struct urp_dcs_state decoder;
	size_t normal_count = samples_for_ms(sample_rate, DCS_TEST_NORMAL_MS);
	size_t reset_count = samples_for_ms(sample_rate, DCS_TEST_TURNOFF_RESET_MS);
	size_t short_count = samples_for_ms(sample_rate, DCS_TEST_TURNOFF_SHORT_MS);
	size_t tail_count = samples_for_ms(sample_rate, DCS_TEST_TURNOFF_VALID_MS);
	uint32_t word = DCS_023N_WIRE_WORD;

	if (inverted)
		word ^= DCS_TEST_WORD_MASK;
	render_wire_word_at_rate(turnoff_normal, normal_count, word, sample_rate);
	interleave_selected_channel(turnoff_capture, turnoff_normal, normal_count, stride);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, inverted, -1, 0);
	assert(process_fragmented_capture(&decoder, turnoff_capture, normal_count, stride,
					  sample_rate, fragment_offset));
	assert(decoder.valid && !decoder.receive_turnoff_active);
	/* Ordinary DCS remains qualified and cannot be mistaken for its tail. */
	assert(process_fragmented_capture(&decoder, turnoff_capture, reset_count, stride,
					  sample_rate, fragment_offset + 1U));
	assert(decoder.valid && !decoder.receive_turnoff_active);

	render_turnoff_tone(turnoff_tail, short_count, sample_rate);
	interleave_selected_channel(turnoff_capture, turnoff_tail, short_count, stride);
	assert(process_fragmented_capture(&decoder, turnoff_capture, short_count, stride,
					  sample_rate, fragment_offset + 2U));
	assert(decoder.valid && !decoder.receive_turnoff_active);
	/* A normal interval breaks the rejected short tail before the valid tail. */
	interleave_selected_channel(turnoff_capture, turnoff_normal, reset_count, stride);
	assert(process_fragmented_capture(&decoder, turnoff_capture, reset_count, stride,
					  sample_rate, fragment_offset + 3U));
	assert(decoder.valid && !decoder.receive_turnoff_active);

	/* Feed the transmitter's actual default-level tail into the receiver. */
	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 023, 0);
	urp_dcs_generate(&encoder, generated_turnoff, tail_count, sample_rate, 1000.0, 1, 1);
	for (size_t index = 0; index < tail_count; ++index)
		turnoff_tail[index] = (int16_t)lround(generated_turnoff[index]);
	interleave_selected_channel(turnoff_capture, turnoff_tail, tail_count, stride);
	assert(!process_fragmented_capture(&decoder, turnoff_capture, tail_count, stride,
					   sample_rate, fragment_offset + 4U));
	assert(!decoder.valid && decoder.receive_turnoff_active);
	/* A normal rekey clears tail suppression and reacquires after two DCS words. */
	interleave_selected_channel(turnoff_capture, turnoff_normal, normal_count, stride);
	assert(process_fragmented_capture(&decoder, turnoff_capture, normal_count, stride,
					  sample_rate, fragment_offset + 5U));
	assert(decoder.valid && !decoder.receive_turnoff_active);
}

/** @brief Verify tail detection is rate-, stride-, and polarity-independent. */
static void test_receive_turnoff_detection(void)
{
	assert_receive_turnoff_at_rate(DCS_TEST_SAMPLE_RATE, 1U, 0, 0U);
	assert_receive_turnoff_at_rate(DCS_TEST_SAMPLE_RATE, 2U, 1, 1U);
	assert_receive_turnoff_at_rate(DCS_TEST_LINK_SAMPLE_RATE, 1U, 1, 2U);
	assert_receive_turnoff_at_rate(DCS_TEST_LINK_SAMPLE_RATE, 2U, 0, 3U);
}

/** @brief Verify broadband noise never asserts the coherent turn-off detector. */
static void test_receive_turnoff_rejects_noise(void)
{
	struct urp_dcs_state decoder;
	size_t normal_count = samples_for_ms(DCS_TEST_SAMPLE_RATE, DCS_TEST_NORMAL_MS);
	size_t noise_count = samples_for_ms(DCS_TEST_SAMPLE_RATE, DCS_TEST_TURNOFF_VALID_MS);

	render_wire_word_at_rate(turnoff_normal, normal_count, DCS_023N_WIRE_WORD,
				 DCS_TEST_SAMPLE_RATE);
	interleave_selected_channel(turnoff_capture, turnoff_normal, normal_count, 1U);
	urp_dcs_init(&decoder);
	urp_dcs_configure(&decoder, 023, 0, -1, 0);
	assert(process_fragmented_capture(&decoder, turnoff_capture, normal_count, 1U,
					  DCS_TEST_SAMPLE_RATE, 4U));
	render_noise(turnoff_noise, noise_count);
	(void)process_fragmented_capture(&decoder, turnoff_noise, noise_count, 1U,
					 DCS_TEST_SAMPLE_RATE, 5U);
	assert(!decoder.receive_turnoff_active);
}

/** @brief Verify the standard 134.4 Hz DCS audio turn-off tone at its PCM source. */
static void test_turnoff_tone_frequency_and_peak(void)
{
	struct urp_dcs_state encoder;
	double tail[DCS_TEST_SAMPLE_RATE];
	double positive_peak = 0.0;
	double negative_peak = 0.0;
	double expected_phase = 0.8 * DCS_TEST_PI;
	unsigned int rising_crossings = 0;
	size_t index;

	urp_dcs_init(&encoder);
	urp_dcs_configure(&encoder, -1, 0, 023, 0);
	urp_dcs_generate(&encoder, tail, DCS_TEST_SAMPLE_RATE, DCS_TEST_SAMPLE_RATE, 2000.0, 1, 1);
	for (index = 0; index < DCS_TEST_SAMPLE_RATE; ++index) {
		if (tail[index] > positive_peak)
			positive_peak = tail[index];
		if (-tail[index] > negative_peak)
			negative_peak = -tail[index];
		if (index && tail[index - 1U] <= 0.0 && tail[index] > 0.0)
			++rising_crossings;
	}
	assert(fabs(tail[0]) < 1.0e-12);
	assert(fabs(positive_peak - 2000.0) < 1.0);
	assert(fabs(negative_peak - 2000.0) < 1.0);
	/* One second contains 134.4 cycles. The sampled phase advances to 0.4 cycle. */
	assert(rising_crossings == 134U || rising_crossings == 135U);
	assert(fabs(encoder.tail_phase - expected_phase) < 1.0e-9);
}

/** @brief Verify simultaneous DCS receive and transmit retain independent clocks. */
static void test_duplex_symbol_clocks(void)
{
	struct urp_dcs_state source, duplex;
	double received[960], transmitted[960];
	int16_t samples[960];
	size_t block, sample;

	urp_dcs_init(&source);
	urp_dcs_init(&duplex);
	urp_dcs_configure(&source, -1, 0, 023, 0);
	urp_dcs_configure(&duplex, 023, 0, 0431, 0);
	for (block = 0; block < 80; ++block) {
		urp_dcs_generate(&source, received, 960, DCS_TEST_SAMPLE_RATE, 2000.0, 1, 0);
		for (sample = 0; sample < 960; ++sample)
			samples[sample] = (int16_t)received[sample];
		/* A live transmitter may advance at a different callback cadence. */
		urp_dcs_generate(&duplex, transmitted, 479, DCS_TEST_SAMPLE_RATE, 2000.0, 1, 0);
		(void)urp_dcs_process(&duplex, samples, 960, 1, DCS_TEST_SAMPLE_RATE);
	}
	assert(duplex.valid);
	assert(duplex.receive_phase[0].bit_accumulator != duplex.transmit_bit_accumulator);
}

/** @brief Verify a one-Hz input still gives the tail detector nonzero windows.
 *
 * The production rates are much higher, but this boundary case protects the
 * rounded millisecond counts used when a decoder is initialized.
 */
static void test_turnoff_detector_minimum_rate(void)
{
	struct urp_dcs_state state;
	int16_t sample = 0;

	urp_dcs_init(&state);
	urp_dcs_configure(&state, 023, 0, -1, 0);
	assert(!urp_dcs_process(&state, &sample, 1, 1, 1));
	assert(state.receive_sample_rate == 1);
	assert(state.receive_turnoff_window_length == 1);
	assert(state.receive_turnoff_minimum_samples == 1);
}

int main(void)
{
	test_code_syntax();
	test_supported_codes();
	test_disabled_interfaces();
	test_round_trip();
	test_all_code_round_trips();
	test_tia_etsi_wire_vector();
	test_receive_requires_two_spaced_words();
	test_receive_qualified_loss_hold();
	test_receive_phase_bank_offsets();
	test_receive_phase_bank_correction_and_noise();
	test_receive_dc_offset_tracking();
	test_polarity_and_turnoff();
	test_receive_turnoff_detection();
	test_receive_turnoff_rejects_noise();
	test_turnoff_tone_frequency_and_peak();
	test_duplex_symbol_clocks();
	test_turnoff_detector_minimum_rate();
	return 0;
}
