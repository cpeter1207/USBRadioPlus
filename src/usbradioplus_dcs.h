/** @file
 * @brief Native-rate DCS (DPL/CDCSS) receive decoder and configuration state.
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
/** @brief Native DCS receiver supplied by the portable radio core.
 * @param context Callback-local renderer context supplied at registration.
 * @param samples First selected-channel signed-16 PCM sample.
 * @param count Number of selected-channel frames.
 * @param stride PCM-word distance between selected-channel samples.
 * @param sample_rate Native input rate in hertz.
 * @param valid Receives nonzero while the configured code qualifies.
 * @return Zero when @p valid is authoritative; nonzero rejects this span.
 *
 * The compatibility radio state machine owns ordering and invokes this hook
 * after its receive-switch blanking.  It therefore lets the Rust core replace
 * decoding without moving or duplicating the established signaling sequence.
 * The C compatibility state deliberately contains no decoder fallback.
 */
typedef int (*urp_dcs_receive_callback)(void *context, const int16_t *samples, size_t count,
					size_t stride, unsigned int sample_rate, int *valid);

/** DCS configuration and portable-receiver binding; it is caller-owned and callback safe. */
struct urp_dcs_state {
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
	/** Optional portable-core receiver invoked at the legacy decode point. */
	urp_dcs_receive_callback receive_callback;
	/** Opaque caller-owned context for @ref receive_callback. */
	void *receive_callback_context;
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
/** @brief Initialize a DCS configuration and portable-receiver binding.
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
/** @brief Install or remove the portable native DCS receive implementation.
 * @param state DCS state that owns the decoder selection.
 * @param callback Optional callback; NULL closes receive qualification.
 * @param context Stable caller-owned context supplied to @p callback.
 *
 * This is a setup/teardown operation. It changes no DCS configuration. A
 * missing or failed portable receiver is fail-closed rather than reviving a
 * second decoder implementation in the C compatibility layer.
 */
void urp_dcs_set_receive_callback(struct urp_dcs_state *state, urp_dcs_receive_callback callback,
				  void *context);
/** @brief Process native-rate discriminator samples and update the qualified receive result.
 * @param state Decoder state.
 * @param samples First selected-channel discriminator PCM sample.
 * @param count Number of selected-channel samples.
 * @param stride Sample spacing in 16-bit PCM words; one for mono and two for stereo.
 * @param sample_rate Input sample rate in Hz.
 * @return Nonzero when the configured receive DCS is currently decoded.
 *
 * The bound portable object preserves the established 23-bit Golay correction,
 * phase-bank qualification, DC removal, and DCS turn-off-tail behavior. A
 * missing or rejected receiver clears @ref urp_dcs_state::valid safely.
 */
int urp_dcs_process(struct urp_dcs_state *state, const int16_t *samples, size_t count,
		    size_t stride, unsigned int sample_rate);
#endif
