/** @file
 * @brief Native 48 kHz DCS (DPL/CDCSS) encoder and Golay decoder.
 */

#ifndef USBRADIOPLUS_DCS_H
#define USBRADIOPLUS_DCS_H

#include <stddef.h>
#include <stdint.h>

/** @brief DCS symbol rate in bits per second. */
#define URP_DCS_BIT_RATE 134.4
/** @brief DCS turn-off tone frequency in Hz. */
#define URP_DCS_TURNOFF_FREQUENCY_HZ 134.4
/** @brief Minimum continuous DCS turn-off tone duration accepted by a receiver. */
#define URP_DCS_TURNOFF_MINIMUM_MS 100U
/** @brief Number of fixed receive timing hypotheses spanning one DCS symbol. */
#define URP_DCS_RX_PHASE_COUNT 16U

/** @brief State for one fixed DCS receive-symbol timing hypothesis. */
struct urp_dcs_receive_phase {
	/** Sliding received 23-bit Golay word. */
	uint32_t word;
	/** Fractional receive symbol accumulator. */
	uint32_t bit_accumulator;
	/** Remaining one-word loss hold after qualification. */
	uint32_t hold;
	/** Number of emitted symbols since the last matching codeword. */
	uint32_t symbols_since_match;
	/** Consecutive correctly spaced matching codewords, saturated at two. */
	uint8_t match_count;
	/** Nonzero after two correctly spaced matching codewords. */
	uint8_t qualified;
	/** Sum of discriminator samples within this hypothesis's symbol. */
	int64_t sample_accumulator;
};

/** DCS encoder and decoder state; it is wholly caller-owned and callback safe. */
struct urp_dcs_state {
	/** Fixed phase bank covering every possible callback-to-symbol alignment. */
	struct urp_dcs_receive_phase receive_phase[URP_DCS_RX_PHASE_COUNT];
	/** Sample rate used to initialize the receive phase bank. */
	unsigned int receive_sample_rate;
	/** Current transmitted 23-bit Golay word. */
	uint32_t transmit_word;
	/** Current transmitted symbol phase. */
	uint32_t transmit_phase;
	/** Fractional transmit symbol accumulator. */
	uint32_t transmit_bit_accumulator;
	/** Slow discriminator DC estimate in Q15 PCM-code units. */
	int64_t dc_estimate_q15;
	/** Goertzel coefficient for the native DCS turn-off detector. */
	double receive_turnoff_coefficient;
	/** Previous first-order Goertzel state for the DCS turn-off detector. */
	double receive_turnoff_one;
	/** Previous second-order Goertzel state for the DCS turn-off detector. */
	double receive_turnoff_two;
	/** Windowed input energy for DCS turn-off discrimination. */
	double receive_turnoff_energy;
	/** Selected-channel samples accumulated in the current detector window. */
	uint32_t receive_turnoff_window_samples;
	/** Fixed detector evaluation window in selected-channel samples. */
	uint32_t receive_turnoff_window_length;
	/** Consecutive coherent tail-tone samples. */
	uint32_t receive_turnoff_consecutive_samples;
	/** Required coherent tail-tone samples before receive qualification clears. */
	uint32_t receive_turnoff_minimum_samples;
	/** Nonzero while a qualified 134.4 Hz turn-off tail is present. */
	int receive_turnoff_active;
	/** Golay syndrome-to-error correction table. */
	uint32_t syndrome[2048];
	/** Configured nine-bit receive code. */
	int receive_code;
	/** Configured nine-bit transmit code. */
	int transmit_code;
	/** Nonzero selects inverted receive polarity. */
	int receive_inverted;
	/** Nonzero selects inverted transmit polarity. */
	int transmit_inverted;
	/** Nonzero enables DCS reception. */
	int enabled_receive;
	/** Nonzero enables DCS transmission. */
	int enabled_transmit;
	/** Nonzero while the configured receive code is qualified. */
	int valid;
	/** Tail-tone oscillator phase. */
	double tail_phase;
};

/** @brief Test whether a value fits the three-digit octal DCS code field.
 * @param code Nine-bit DCS code value.
 * @return Nonzero when code is representable as 000 through 777.
 */
int urp_dcs_code_supported(int code);
/** @brief Parse a three-digit octal DCS code with N or I polarity suffix.
 * @param text Code such as 023N or 023I.
 * @param code Receives the nine-bit code.
 * @param inverted Receives nonzero for inverse polarity.
 * @return Zero on success or nonzero for an invalid spelling.
 */
int urp_dcs_parse_code(const char *text, int *code, int *inverted);
/** @brief Format a DCS code in canonical three-digit octal plus polarity form.
 * @param text Destination buffer.
 * @param size Destination capacity.
 * @param code Nine-bit DCS code.
 * @param inverted Nonzero selects inverse polarity.
 */
void urp_dcs_format_code(char *text, size_t size, int code, int inverted);
/** @brief Initialize a DCS state and its fixed Golay syndrome table.
 * @param state State to initialize.
 */
void urp_dcs_init(struct urp_dcs_state *state);
/** @brief Configure DCS reception and/or transmission.
 * @param state State to configure.
 * @param receive_code Octal receive code, or a negative value to disable reception.
 * @param receive_inverted Nonzero selects inverse receive polarity.
 * @param transmit_code Octal transmit code, or a negative value to disable transmission.
 * @param transmit_inverted Nonzero selects inverse transmit polarity.
 */
void urp_dcs_configure(struct urp_dcs_state *state, int receive_code, int receive_inverted,
		       int transmit_code, int transmit_inverted);
/** @brief Process native-rate discriminator samples and update the qualified receive result.
 * @param state Decoder state.
 * @param samples First selected-channel discriminator PCM sample.
 * @param count Number of selected-channel samples.
 * @param stride Sample spacing in 16-bit PCM words; one for mono and two for stereo.
 * @param sample_rate Input sample rate in Hz.
 * @return Nonzero when the configured receive DCS is currently decoded.
 *
 * A previously qualified receiver clears after
 * @ref URP_DCS_TURNOFF_MINIMUM_MS of a coherent 134.4 Hz DCS turn-off tone.
 * The detector is energy- and coherence-gated, so ordinary DCS data, silence,
 * and broadband discriminator noise do not constitute a turn-off tail.
 */
int urp_dcs_process(struct urp_dcs_state *state, const int16_t *samples, size_t count,
		    size_t stride, unsigned int sample_rate);
/** @brief Render DCS NRZ modulation or its 134.4 Hz turn-off tone into PCM-domain doubles.
 * @param state Encoder state.
 * @param output Destination PCM-domain samples.
 * @param count Number of output samples.
 * @param sample_rate Output sample rate in Hz.
 * @param peak Requested peak amplitude in PCM codes.
 * @param enabled Nonzero enables normal DCS modulation.
 * @param turnoff Nonzero selects the turn-off tone.
 */
void urp_dcs_generate(struct urp_dcs_state *state, double *output, size_t count,
		      unsigned int sample_rate, double peak, int enabled, int turnoff);

#endif
