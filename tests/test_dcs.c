/** @file
 * @brief DCS configuration and portable receive-boundary tests.
 *
 * Exhaustive 23-bit Golay, symbol-phase, DC-tracking, and turn-off-tail
 * vectors live with the Rust DCS owner in
 * librptadvradio/src/tests.rs.  This C test verifies the compatibility
 * boundary retains its configuration and raw-PCM contract without a second
 * decoder implementation.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usbradioplus_dcs.h"
#include "usbradioplus_radio_core_adapter.h"

/** @brief TIA/ETSI 023N word in least-significant-bit-first wire order. */
#define DCS_023N_WIRE_WORD 0x763813U
/** @brief TIA/ETSI 431N word in least-significant-bit-first wire order. */
#define DCS_431N_WIRE_WORD 0x6c5919U
/** @brief Mask for a complete 23-bit DCS word. */
#define DCS_WORD_MASK 0x7fffffU
/** @brief Current CM119 native discriminator sample rate. */
#define DCS_NATIVE_SAMPLE_RATE 48000U
/** @brief Largest production native render span and test bridge workspace. */
#define DCS_MAX_CALLBACK_FRAMES 960U
/** @brief Enough 48 kHz PCM for two complete DCS words. */
#define DCS_NATIVE_CAPTURE_FRAMES 24000U
/** @brief Fixed DCS turn-off tone duration accepted by the decoder. */
#define DCS_TAIL_CAPTURE_FRAMES ((DCS_NATIVE_SAMPLE_RATE * URP_DCS_TURNOFF_MINIMUM_MS) / 1000U)
/** @brief Portable pi for deterministic DCS tail synthesis. */
#define DCS_TEST_PI 3.14159265358979323846

/** @brief Record one DCS wrapper callback invocation. */
struct dcs_callback_probe {
	/** Expected first selected-channel PCM address. */
	const int16_t *samples;
	/** Expected selected-channel frame count. */
	size_t count;
	/** Expected selected-channel PCM stride. */
	size_t stride;
	/** Expected input sample rate. */
	unsigned int sample_rate;
	/** Callback status to return. */
	int result;
	/** Qualification returned when @ref result is zero. */
	int valid;
	/** Number of observed invocations. */
	unsigned int calls;
};

/** @brief Preallocated test-only bridge to the real portable DCS object. */
struct dcs_rust_bridge {
	/** Fixed-rate Rust DCS owner. */
	struct rptadv_radio *radio;
	/** Exact signed-16 to canonical-F32 hardware-boundary conversion workspace. */
	float stereo[DCS_MAX_CALLBACK_FRAMES * 2U];
};

/** @brief Record raw compatibility-boundary values without decoding them. */
static int dcs_probe_callback(void *context, const int16_t *samples, size_t count, size_t stride,
			      unsigned int sample_rate, int *valid)
{
	struct dcs_callback_probe *probe = context;

	assert(probe);
	assert(samples == probe->samples);
	assert(count == probe->count);
	assert(stride == probe->stride);
	assert(sample_rate == probe->sample_rate);
	assert(valid);
	++probe->calls;
	*valid = probe->valid;
	return probe->result;
}

/** @brief Decode selected raw PCM through the actual released Rust DCS object. */
static int dcs_rust_bridge_callback(void *context, const int16_t *samples, size_t count,
				    size_t stride, unsigned int sample_rate, int *valid)
{
	struct dcs_rust_bridge *bridge = context;
	size_t index;

	if (!bridge || !bridge->radio || !samples || !valid || !stride ||
	    sample_rate != DCS_NATIVE_SAMPLE_RATE || count > DCS_MAX_CALLBACK_FRAMES)
		return -1;
	for (index = 0U; index < count; ++index) {
		bridge->stereo[index * 2U] = (float)samples[index * stride] / 32768.0F;
		bridge->stereo[index * 2U + 1U] = 0.0F;
	}
	return urp_radio_core_process_dcs_receive(bridge->radio, bridge->stereo, count, valid);
}

/** @brief Initialize and bind one test bridge at the native 48 kHz rate. */
static void dcs_rust_bridge_start(struct dcs_rust_bridge *bridge, struct urp_dcs_state *state,
				  int code, int inverted)
{
	memset(bridge, 0, sizeof(*bridge));
	assert(!urp_radio_core_create(DCS_NATIVE_SAMPLE_RATE, DCS_MAX_CALLBACK_FRAMES,
				      &bridge->radio));
	assert(!urp_radio_core_configure_dcs_receive(bridge->radio, code, inverted));
	urp_dcs_init(state);
	urp_dcs_configure(state, code, inverted, -1, 0);
	urp_dcs_set_receive_callback(state, dcs_rust_bridge_callback, bridge);
}

/** @brief Apply one receive-code change at the matching callback boundary. */
static void dcs_rust_bridge_configure(struct dcs_rust_bridge *bridge, struct urp_dcs_state *state,
				      int code, int inverted)
{
	assert(bridge && bridge->radio);
	assert(!urp_radio_core_configure_dcs_receive(bridge->radio, code, inverted));
	urp_dcs_configure(state, code, inverted, -1, 0);
}

/** @brief Retire one test-only Rust DCS binding. */
static void dcs_rust_bridge_stop(struct dcs_rust_bridge *bridge, struct urp_dcs_state *state)
{
	urp_dcs_set_receive_callback(state, NULL, NULL);
	urp_radio_core_destroy(bridge->radio);
	bridge->radio = NULL;
}

/** @brief Render one continuous DCS word into selected-channel signed-16 PCM. */
static void render_dcs_word(int16_t *samples, size_t count, uint32_t word)
{
	uint32_t accumulator = 0U;
	unsigned int phase = 0U;
	size_t index;

	for (index = 0U; index < count; ++index) {
		samples[index] = (word & (1U << phase)) ? 4000 : -4000;
		accumulator += 1344U;
		if (accumulator >= DCS_NATIVE_SAMPLE_RATE * 10U) {
			accumulator -= DCS_NATIVE_SAMPLE_RATE * 10U;
			phase = (phase + 1U) % 23U;
		}
	}
}

/** @brief Render one coherent DCS turn-off tail into selected-channel PCM. */
static void render_turnoff_tail(int16_t *samples, size_t count)
{
	double phase = 0.0;
	const double step =
		2.0 * DCS_TEST_PI * URP_DCS_TURNOFF_FREQUENCY_HZ / (double)DCS_NATIVE_SAMPLE_RATE;
	size_t index;

	for (index = 0U; index < count; ++index) {
		samples[index] = (int16_t)lround(4000.0 * sin(phase));
		phase += step;
		if (phase >= 2.0 * DCS_TEST_PI)
			phase -= 2.0 * DCS_TEST_PI;
	}
}

/** @brief Interleave selected PCM with a different right channel. */
static void interleave_stereo(int16_t *stereo, const int16_t *left, const int16_t *right,
			      size_t count)
{
	size_t index;

	for (index = 0U; index < count; ++index) {
		stereo[index * 2U] = left[index];
		stereo[index * 2U + 1U] = right ? right[index] : 0;
	}
}

/** @brief Feed a capture in bounded, deliberately uneven callback spans. */
static int process_capture(struct urp_dcs_state *state, const int16_t *samples, size_t count,
			   size_t stride, size_t pattern_offset)
{
	static const size_t fragments[] = {1U, 17U, 359U, 960U, 53U, 711U, 2U, 487U};
	size_t position = 0U;
	size_t fragment = 0U;
	int valid = 0;

	while (position < count) {
		size_t length = fragments[(pattern_offset + fragment) %
					  (sizeof(fragments) / sizeof(fragments[0]))];

		if (length > count - position)
			length = count - position;
		valid = urp_dcs_process(state, samples + position * stride, length, stride,
					DCS_NATIVE_SAMPLE_RATE);
		position += length;
		++fragment;
	}
	return valid;
}

/** @brief Verify canonical octal parsing and formatting remain in C. */
static void test_code_syntax(void)
{
	char text[5];
	int code;
	int inverted;

	assert(!urp_dcs_parse_code("023N", &code, &inverted));
	assert(code == 023 && !inverted);
	assert(!urp_dcs_parse_code("754i", &code, &inverted));
	assert(code == 0754 && inverted);
	assert(urp_dcs_parse_code("023X", &code, &inverted));
	assert(urp_dcs_parse_code("88N", &code, &inverted));
	assert(urp_dcs_parse_code(NULL, &code, &inverted));
	assert(urp_dcs_parse_code("023N", NULL, &inverted));
	urp_dcs_format_code(text, sizeof(text), 023, 1);
	assert(!strcmp(text, "023I"));
	urp_dcs_format_code(text, sizeof(text), 023, 0);
	assert(!strcmp(text, "023N"));
	urp_dcs_format_code(NULL, sizeof(text), 023, 1);
	urp_dcs_format_code(text, 0U, 023, 1);
}

/** @brief Verify every three-octal-digit code remains an accepted configuration. */
static void test_supported_codes(void)
{
	int candidate;

	for (candidate = -1; candidate <= 01000; ++candidate)
		assert(urp_dcs_code_supported(candidate) == (candidate >= 0 && candidate <= 0777));
}

/** @brief Verify C configuration preserves only signaling choices and qualification. */
static void test_configuration(void)
{
	struct urp_dcs_state state;

	urp_dcs_init(NULL);
	urp_dcs_configure(NULL, 0, 0, 0, 0);
	urp_dcs_init(&state);
	urp_dcs_configure(&state, 023, 1, 0431, 0);
	assert(state.receive_code == 023 && state.receive_inverted);
	assert(state.transmit_code == 0431 && !state.transmit_inverted);
	assert(state.enabled_receive && state.enabled_transmit && !state.valid);
	urp_dcs_configure(&state, 01000, 1, -1, 1);
	assert(!state.enabled_receive && !state.enabled_transmit && !state.valid);
}

/** @brief Verify raw C callback forwarding and fail-closed receiver errors. */
static void test_receive_callback_contract(void)
{
	struct urp_dcs_state state;
	struct dcs_callback_probe probe = {0};
	int16_t stereo[] = {100, -100, 200, -200};

	urp_dcs_init(&state);
	urp_dcs_configure(&state, 023, 0, -1, 0);
	probe.samples = stereo;
	probe.count = 2U;
	probe.stride = 2U;
	probe.sample_rate = DCS_NATIVE_SAMPLE_RATE;
	probe.valid = 1;
	urp_dcs_set_receive_callback(&state, dcs_probe_callback, &probe);
	assert(urp_dcs_process(&state, stereo, probe.count, probe.stride, probe.sample_rate));
	assert(probe.calls == 1U && state.valid);

	probe.valid = 0;
	assert(!urp_dcs_process(&state, stereo, probe.count, probe.stride, probe.sample_rate));
	assert(probe.calls == 2U && !state.valid);

	/* A rejected portable call must close rather than run a second decoder. */
	state.valid = 1;
	probe.result = -1;
	probe.valid = 1;
	assert(!urp_dcs_process(&state, stereo, probe.count, probe.stride, probe.sample_rate));
	assert(probe.calls == 3U && !state.valid);

	/* Invalid calls retain legacy no-mutation behavior. */
	state.valid = 1;
	assert(!urp_dcs_process(&state, NULL, probe.count, probe.stride, probe.sample_rate));
	assert(state.valid);

	/* A missing receiver occurs only while a renderer is closed and fails safe. */
	urp_dcs_set_receive_callback(&state, NULL, NULL);
	assert(!urp_dcs_process(&state, stereo, probe.count, probe.stride, probe.sample_rate));
	assert(!state.valid);

	urp_dcs_configure(&state, -1, 0, -1, 0);
	probe.calls = 0U;
	urp_dcs_set_receive_callback(&state, dcs_probe_callback, &probe);
	assert(!urp_dcs_process(&state, stereo, probe.count, probe.stride, probe.sample_rate));
	assert(!probe.calls && !state.valid);
}

/** @brief Verify the real native Rust owner receives left-channel 48 kHz PCM. */
static void test_native_rust_receive_bridge(void)
{
	struct dcs_rust_bridge bridge;
	struct urp_dcs_state state;
	static int16_t left[DCS_NATIVE_CAPTURE_FRAMES];
	static int16_t right[DCS_NATIVE_CAPTURE_FRAMES];
	static int16_t stereo[DCS_NATIVE_CAPTURE_FRAMES * 2U];
	static int16_t tail[DCS_TAIL_CAPTURE_FRAMES];

	render_dcs_word(left, DCS_NATIVE_CAPTURE_FRAMES, DCS_023N_WIRE_WORD);
	render_dcs_word(right, DCS_NATIVE_CAPTURE_FRAMES, DCS_431N_WIRE_WORD);
	interleave_stereo(stereo, left, right, DCS_NATIVE_CAPTURE_FRAMES);
	dcs_rust_bridge_start(&bridge, &state, 023, 0);
	/* The right channel contains a different valid code and must not qualify. */
	assert(process_capture(&state, stereo, DCS_NATIVE_CAPTURE_FRAMES, 2U, 3U));

	dcs_rust_bridge_configure(&bridge, &state, 0431, 0);
	assert(!process_capture(&state, stereo, DCS_NATIVE_CAPTURE_FRAMES, 2U, 4U));
	interleave_stereo(stereo, right, left, DCS_NATIVE_CAPTURE_FRAMES);
	assert(process_capture(&state, stereo, DCS_NATIVE_CAPTURE_FRAMES, 2U, 5U));

	render_dcs_word(left, DCS_NATIVE_CAPTURE_FRAMES, DCS_023N_WIRE_WORD ^ DCS_WORD_MASK);
	interleave_stereo(stereo, left, right, DCS_NATIVE_CAPTURE_FRAMES);
	dcs_rust_bridge_configure(&bridge, &state, 023, 1);
	assert(process_capture(&state, stereo, DCS_NATIVE_CAPTURE_FRAMES, 2U, 6U));

	render_turnoff_tail(tail, DCS_TAIL_CAPTURE_FRAMES);
	assert(!process_capture(&state, tail, DCS_TAIL_CAPTURE_FRAMES, 1U, 7U));
	assert(!state.valid);
	dcs_rust_bridge_stop(&bridge, &state);
}

int main(void)
{
	assert(!urp_radio_core_initialize());
	test_code_syntax();
	test_supported_codes();
	test_configuration();
	test_receive_callback_contract();
	test_native_rust_receive_bridge();
	return 0;
}
