/** @file
 * @brief Native 48 kHz DCS (DPL/CDCSS) encoder and Golay decoder.
 */

#include "usbradioplus_dcs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/** @brief Mask for one 23-bit Golay codeword. */
#define DCS_WORD_MASK 0x7fffffU
/** @brief Generator polynomial for the shortened Golay code. */
#define DCS_GENERATOR 0xc75U
/** @brief TIA/ETSI wire-layout parity polynomial. */
#define DCS_WIRE_GENERATOR 0x08eaU
/** @brief Portable double-precision pi for the turn-off oscillator. */
#define DCS_PI 3.14159265358979323846
/** @brief Integer scaling used to represent the 134.4-bit/s symbol clock exactly. */
#define DCS_CLOCK_SCALE 10U
/** @brief DCS symbol-clock increment in DCS_CLOCK_SCALE units per input sample. */
#define DCS_CLOCK_INCREMENT 1344U
/** @brief DCS turn-off detector evaluation span in milliseconds. */
#define DCS_TURNOFF_WINDOW_MS 20U
/** @brief Minimum RMS discriminator level accepted as a DCS turn-off tone. */
#define DCS_TURNOFF_MINIMUM_RMS 200.0
/** @brief Minimum coherent single-frequency energy fraction for a tail tone. */
#define DCS_TURNOFF_MINIMUM_COHERENCE 0.25
/** @brief Fractional scale for the slow discriminator DC tracker. */
#define DCS_DC_TRACKER_SCALE 32768LL

int urp_dcs_code_supported(int code)
{
	/* DCS code assignments beyond published tables are common in deployed radios. */
	return code >= 0 && code <= 0777;
}

/** @brief Return the cyclic Golay remainder of a 23-bit word.
 * @param word Candidate codeword.
 * @return Eleven-bit cyclic remainder.
 */
static uint32_t golay_remainder(uint32_t word)
{
	int bit;
	for (bit = 22; bit >= 11; --bit)
		if (word & (1U << bit))
			word ^= DCS_GENERATOR << (bit - 11);
	return word & 0x7ffU;
}

/** @brief Encode a DCS word in its TIA/ETSI least-significant-bit-first layout.
 * @param code Nine-bit DCS code.
 * @return Systematic 23-bit Golay word.
 */
static uint32_t dcs_word(int code)
{
	uint32_t data = ((uint32_t)code & 0x1ffU) | 0x800U;
	uint32_t remainder = data;
	int bit;

	for (bit = 0; bit < 12; ++bit) {
		remainder <<= 1;
		if (remainder & 0x1000U)
			remainder ^= DCS_WIRE_GENERATOR;
	}
	return data | ((remainder & 0x0ffeU) << 11);
}

/** @brief Add one correctable error pattern to a syndrome table.
 * @param state State owning the syndrome table.
 * @param error Correctable 23-bit error pattern.
 */
static void install_error(struct urp_dcs_state *state, uint32_t error)
{
	state->syndrome[golay_remainder(error)] = error;
}

/** @brief Reset the fixed-frequency DCS turn-off detector for one input rate.
 *
 * The detector evaluates short Goertzel windows rather than relying on the
 * DCS word decoder to lose synchronization.  Its state is wholly owned by
 * the receive callback and contains no dynamic storage.
 *
 * @param state Decoder state.
 * @param sample_rate Selected-channel discriminator sample rate.
 */
static void dcs_reset_turnoff_detector(struct urp_dcs_state *state, unsigned int sample_rate)
{
	uint32_t window = (sample_rate * DCS_TURNOFF_WINDOW_MS) / 1000U;
	uint32_t minimum = (sample_rate * URP_DCS_TURNOFF_MINIMUM_MS) / 1000U;
	double phase = 2.0 * DCS_PI * URP_DCS_TURNOFF_FREQUENCY_HZ / sample_rate;

	if (!window)
		window = 1;
	if (!minimum)
		minimum = 1;
	state->receive_turnoff_coefficient = 2.0 * cos(phase);
	state->receive_turnoff_one = 0.0;
	state->receive_turnoff_two = 0.0;
	state->receive_turnoff_energy = 0.0;
	state->receive_turnoff_window_samples = 0;
	state->receive_turnoff_window_length = window;
	state->receive_turnoff_consecutive_samples = 0;
	state->receive_turnoff_minimum_samples = minimum;
	state->receive_turnoff_active = 0;
}

/** @brief Accumulate one discriminator sample into the DCS turn-off detector.
 *
 * A coherent sine near 134.4 Hz concentrates at least one quarter of its
 * windowed energy in the target bin.  The separate RMS threshold rejects
 * near-silence, while ordinary DCS NRZ data and broadband noise fail the
 * coherence test.  Requiring a continuous 100 ms interval avoids mistaking a
 * short transition for a valid end-of-transmission tail.
 *
 * @param state Decoder state.
 * @param sample DC-removed discriminator sample.
 * @return Nonzero only after a valid DCS turn-off tail is present.
 */
static int dcs_process_turnoff_sample(struct urp_dcs_state *state, int32_t sample)
{
	double current = (double)sample +
			 state->receive_turnoff_coefficient * state->receive_turnoff_one -
			 state->receive_turnoff_two;

	state->receive_turnoff_two = state->receive_turnoff_one;
	state->receive_turnoff_one = current;
	state->receive_turnoff_energy += (double)sample * sample;
	++state->receive_turnoff_window_samples;
	if (state->receive_turnoff_window_samples < state->receive_turnoff_window_length)
		return state->receive_turnoff_active;
	{
		double samples = state->receive_turnoff_window_samples;
		double power = state->receive_turnoff_one * state->receive_turnoff_one +
			       state->receive_turnoff_two * state->receive_turnoff_two -
			       state->receive_turnoff_coefficient * state->receive_turnoff_one *
				       state->receive_turnoff_two;
		double minimum_energy = samples * DCS_TURNOFF_MINIMUM_RMS * DCS_TURNOFF_MINIMUM_RMS;
		int tone = state->receive_turnoff_energy >= minimum_energy &&
			   power >= state->receive_turnoff_energy * samples *
					    DCS_TURNOFF_MINIMUM_COHERENCE;

		if (tone) {
			state->receive_turnoff_consecutive_samples +=
				state->receive_turnoff_window_samples;
			if (state->receive_turnoff_consecutive_samples >=
			    state->receive_turnoff_minimum_samples)
				state->receive_turnoff_active = 1;
		} else {
			state->receive_turnoff_consecutive_samples = 0;
			state->receive_turnoff_active = 0;
		}
	}
	state->receive_turnoff_one = 0.0;
	state->receive_turnoff_two = 0.0;
	state->receive_turnoff_energy = 0.0;
	state->receive_turnoff_window_samples = 0;
	return state->receive_turnoff_active;
}

/** @brief Reset the bounded receive phase bank for an input sample rate.
 *
 * Each bank is offset by one sixteenth of a symbol.  A live capture callback
 * can begin at any point in an over-air DCS symbol; one bank therefore keeps
 * its integration boundary close to the actual modulation boundary without a
 * timing loop, allocation, or callback-to-callback reset.
 *
 * @param state Decoder state to reset.
 * @param sample_rate Selected-channel sample rate in Hz.
 */
static void dcs_reset_receive_phase_bank(struct urp_dcs_state *state, unsigned int sample_rate)
{
	uint64_t clock_limit = (uint64_t)sample_rate * DCS_CLOCK_SCALE;
	unsigned int phase;

	memset(state->receive_phase, 0, sizeof(state->receive_phase));
	for (phase = 0; phase < URP_DCS_RX_PHASE_COUNT; ++phase)
		state->receive_phase[phase].bit_accumulator =
			(uint32_t)((clock_limit * phase) / URP_DCS_RX_PHASE_COUNT);
	state->receive_sample_rate = sample_rate;
	state->dc_estimate_q15 = 0;
	dcs_reset_turnoff_detector(state, sample_rate);
	state->valid = 0;
}

int urp_dcs_parse_code(const char *text, int *code, int *inverted)
{
	int value;
	if (!text || strlen(text) != 4 || !code || !inverted)
		return -1;
	if (text[0] < '0' || text[0] > '7' || text[1] < '0' || text[1] > '7' || text[2] < '0' ||
	    text[2] > '7')
		return -1;
	/* Three validated octal digits always represent a supported 000..777 code. */
	value = ((text[0] - '0') << 6) | ((text[1] - '0') << 3) | (text[2] - '0');
	if (toupper((unsigned char)text[3]) == 'N')
		*inverted = 0;
	else if (toupper((unsigned char)text[3]) == 'I')
		*inverted = 1;
	else
		return -1;
	*code = (int)value;
	return 0;
}

void urp_dcs_format_code(char *text, size_t size, int code, int inverted)
{
	if (!text || !size)
		return;
	(void)snprintf(text, size, "%03o%c", code & 0777, inverted ? 'I' : 'N');
}

void urp_dcs_init(struct urp_dcs_state *state)
{
	int first, second, third;
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	for (first = 0; first < 23; ++first) {
		install_error(state, 1U << first);
		for (second = first + 1; second < 23; ++second) {
			install_error(state, (1U << first) | (1U << second));
			for (third = second + 1; third < 23; ++third)
				install_error(state,
					      (1U << first) | (1U << second) | (1U << third));
		}
	}
	install_error(state, 0);
}

void urp_dcs_configure(struct urp_dcs_state *state, int receive_code, int receive_inverted,
		       int transmit_code, int transmit_inverted)
{
	if (!state)
		return;
	state->receive_code = urp_dcs_code_supported(receive_code) ? receive_code : -1;
	state->receive_inverted = !!receive_inverted;
	state->transmit_code = urp_dcs_code_supported(transmit_code) ? transmit_code : -1;
	state->transmit_inverted = !!transmit_inverted;
	state->enabled_receive = state->receive_code >= 0;
	state->enabled_transmit = state->transmit_code >= 0;
	memset(state->receive_phase, 0, sizeof(state->receive_phase));
	state->receive_sample_rate = 0;
	state->transmit_bit_accumulator = 0;
	state->dc_estimate_q15 = 0;
	state->receive_turnoff_coefficient = 0.0;
	state->receive_turnoff_one = 0.0;
	state->receive_turnoff_two = 0.0;
	state->receive_turnoff_energy = 0.0;
	state->receive_turnoff_window_samples = 0;
	state->receive_turnoff_window_length = 0;
	state->receive_turnoff_consecutive_samples = 0;
	state->receive_turnoff_minimum_samples = 0;
	state->receive_turnoff_active = 0;
	state->valid = 0;
}

/** @brief Decode a sliding least-significant-bit-first received codeword.
 * @param state Decoder state.
 * @param received Candidate received 23-bit word.
 * @return Nonzero when the corrected word matches the configured receive code.
 */
static int decode_word(struct urp_dcs_state *state, uint32_t received)
{
	uint32_t error = state->syndrome[golay_remainder(received)];
	uint32_t corrected = received ^ error;
	uint32_t data = corrected & 0xfffU;
	if ((data & 0xe00U) != 0x800U)
		return 0;
	return (int)(data & 0x1ffU) == state->receive_code;
}

/** @brief Return whether any timing hypothesis currently qualifies the configured code.
 * @param state Decoder state.
 * @return Nonzero when a phase bank remains qualified.
 */
static int dcs_phase_bank_valid(const struct urp_dcs_state *state)
{
	unsigned int phase;

	for (phase = 0; phase < URP_DCS_RX_PHASE_COUNT; ++phase)
		if (state->receive_phase[phase].qualified)
			return 1;
	return 0;
}

/** @brief Clear every receive qualification after a valid DCS turn-off tail.
 *
 * Preserve symbol timing and the sliding words so a subsequent normal DCS
 * transmission can reacquire without a callback-boundary reset.  Matching
 * state is cleared so a tail cannot accidentally complete a stale codeword.
 *
 * @param state Decoder state.
 */
static void dcs_clear_receive_qualification(struct urp_dcs_state *state)
{
	unsigned int phase;

	for (phase = 0; phase < URP_DCS_RX_PHASE_COUNT; ++phase) {
		struct urp_dcs_receive_phase *receiver = &state->receive_phase[phase];

		receiver->hold = 0;
		receiver->symbols_since_match = 0;
		receiver->match_count = 0;
		receiver->qualified = 0;
	}
	state->valid = 0;
}

/** @brief Qualify one symbol decision in one timing hypothesis.
 *
 * A Golay(23,12) code is perfect: every received word maps to a codeword.
 * Require the same configured word at the next 23-symbol boundary before
 * opening squelch, then retain an already-qualified result for one lost word.
 *
 * @param state Decoder state.
 * @param receiver One timing hypothesis to update.
 * @param bit Decided DCS symbol.
 */
static void dcs_process_received_symbol(struct urp_dcs_state *state,
					struct urp_dcs_receive_phase *receiver, int bit)
{
	int matched;

	++receiver->symbols_since_match;
	receiver->word = ((receiver->word >> 1) | ((uint32_t)bit << 22)) & DCS_WORD_MASK;
	matched = decode_word(state, receiver->word);
	if (matched) {
		if (receiver->match_count && receiver->symbols_since_match == 23U) {
			if (receiver->match_count < 2U)
				++receiver->match_count;
		} else {
			receiver->match_count = 1;
		}
		receiver->symbols_since_match = 0;
		if (receiver->match_count >= 2U) {
			receiver->qualified = 1;
			receiver->hold = 23U;
		}
		return;
	}
	if (receiver->qualified) {
		/* Qualification always starts a nonzero one-word hold interval. */
		--receiver->hold;
		if (!receiver->hold) {
			receiver->qualified = 0;
			receiver->match_count = 0;
		}
	}
}

/** @brief Remove slow discriminator DC bias without losing fractional updates.
 *
 * The original integer estimate could not move at all for ordinary offsets
 * below the 32,768-code time constant.  Retaining the same slow IIR response
 * in Q15 PCM-code units lets every nonzero sample error contribute while
 * keeping this callback-owned operation allocation- and lock-free.
 *
 * @param state Decoder state owning the fractional estimate.
 * @param sample One selected-channel discriminator PCM sample.
 * @return The DC-removed sample in PCM codes.
 */
static int32_t dcs_remove_dc(struct urp_dcs_state *state, int32_t sample)
{
	int64_t target = (int64_t)sample * DCS_DC_TRACKER_SCALE;

	state->dc_estimate_q15 += (target - state->dc_estimate_q15) / DCS_DC_TRACKER_SCALE;
	return sample - (int32_t)(state->dc_estimate_q15 / DCS_DC_TRACKER_SCALE);
}

int urp_dcs_process(struct urp_dcs_state *state, const int16_t *samples, size_t count,
		    size_t stride, unsigned int sample_rate)
{
	size_t index;
	uint32_t clock_limit;
	if (!state || !samples || !stride || !sample_rate || !state->enabled_receive)
		return 0;
	if (state->receive_sample_rate != sample_rate)
		dcs_reset_receive_phase_bank(state, sample_rate);
	clock_limit = sample_rate * DCS_CLOCK_SCALE;
	for (index = 0; index < count; ++index) {
		int32_t sample = samples[index * stride];
		int32_t centered;
		int tail_was_active;
		int tail_active;
		unsigned int phase;

		centered = dcs_remove_dc(state, sample);
		tail_was_active = state->receive_turnoff_active;
		tail_active = dcs_process_turnoff_sample(state, centered);
		if (tail_active && !tail_was_active)
			dcs_clear_receive_qualification(state);
		for (phase = 0; phase < URP_DCS_RX_PHASE_COUNT; ++phase) {
			struct urp_dcs_receive_phase *receiver = &state->receive_phase[phase];
			int bit;

			receiver->sample_accumulator += centered;
			receiver->bit_accumulator += DCS_CLOCK_INCREMENT;
			if (receiver->bit_accumulator < clock_limit)
				continue;
			receiver->bit_accumulator -= clock_limit;
			bit = receiver->sample_accumulator >= 0;
			if (state->receive_inverted)
				bit = !bit;
			receiver->sample_accumulator = 0;
			/* A qualified 134.4 Hz tail replaces data; ignore its symbol
			 * decisions until a non-tail window arrives, rather than allowing
			 * a stale or accidental Golay word to re-open the receiver. */
			if (!tail_active)
				dcs_process_received_symbol(state, receiver, bit);
		}
	}
	state->valid = dcs_phase_bank_valid(state);
	return state->valid;
}

void urp_dcs_generate(struct urp_dcs_state *state, double *output, size_t count,
		      unsigned int sample_rate, double peak, int enabled, int turnoff)
{
	size_t index;
	if (!output || !count)
		return;
	if (!state || !enabled || peak <= 0.0 || !sample_rate) {
		memset(output, 0, count * sizeof(*output));
		return;
	}
	if (turnoff) {
		double step = 2.0 * DCS_PI * URP_DCS_TURNOFF_FREQUENCY_HZ / sample_rate;
		for (index = 0; index < count; ++index) {
			output[index] = peak * sin(state->tail_phase);
			state->tail_phase += step;
			if (state->tail_phase >= 2.0 * DCS_PI)
				state->tail_phase -= 2.0 * DCS_PI;
		}
		return;
	}
	state->transmit_word = dcs_word(state->transmit_code);
	for (index = 0; index < count; ++index) {
		int bit = (state->transmit_word >> state->transmit_phase) & 1U;
		if (state->transmit_inverted)
			bit = !bit;
		output[index] = bit ? peak : -peak;
		state->transmit_bit_accumulator += DCS_CLOCK_INCREMENT;
		if (state->transmit_bit_accumulator >= sample_rate * DCS_CLOCK_SCALE) {
			state->transmit_bit_accumulator -= sample_rate * DCS_CLOCK_SCALE;
			state->transmit_phase = (state->transmit_phase + 1) % 23U;
		}
	}
}
