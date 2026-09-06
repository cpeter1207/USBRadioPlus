/** @file
 * @brief Shared Asterisk channel lifecycle, radio configuration, and tuning operations.
 */

#ifndef USBRADIOPLUS_CHANNEL_COMMON_H
#define USBRADIOPLUS_CHANNEL_COMMON_H

#include "asterisk/channel.h"
#include "asterisk/config.h"

#include "usbradioplus_channel_private.h"

/** Parallel outputs with active timed pulses. */
extern int8_t pp_pulsemask;
/** Previously applied parallel-port pulse mask. */
extern int8_t pp_lastmask;

extern int8_t pp_pulsemask;
extern int8_t pp_lastmask;

int hidhdwconfig(struct chan_usbradio_pvt *o);

/** @brief Release a channel that failed before it was linked into the active list.
 * @param o Private state of the selected radio channel.
 */
void destroy_unlinked_channel(struct chan_usbradio_pvt *o);

/** @brief Wake the HID worker so a changed PTT request is applied promptly.
 * @param o Private state of the selected radio channel.
 */
void kickptt(const struct chan_usbradio_pvt *o);

/** @brief Accept the Asterisk start-of-DTMF notification.
 * @param c Asterisk channel associated with the radio or link.
 * @param digit DTMF digit supplied by Asterisk.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_digit_begin(struct ast_channel *c, char digit);

/** @brief Accept the Asterisk end-of-DTMF notification.
 * @param c Asterisk channel associated with the radio or link.
 * @param digit DTMF digit supplied by Asterisk.
 * @param duration DTMF duration in milliseconds.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration);

/** @brief Mark the radio channel answered.
 * @param c Asterisk channel associated with the radio or link.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_answer(struct ast_channel *c);

/** @brief Queue app_rpt PCM for asynchronous native-rate transmitter rendering.
 * @param o Private state of the selected radio channel.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 */
void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples, size_t count);

/** @brief Transfer private radio ownership when Asterisk replaces a channel.
 * @param oldchan Asterisk channel associated with the radio or link.
 * @param newchan Asterisk channel associated with the radio or link.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);

/** @brief Apply Asterisk radio key, unkey, and signaling indications.
 * @param c Asterisk channel associated with the radio or link.
 * @param cond_in Asterisk control indication.
 * @param data Optional indication payload supplied by Asterisk.
 * @param datalen Payload length in bytes.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_indicate(struct ast_channel *c, int cond_in, const void *data, size_t datalen);

/** @brief Apply supported Asterisk channel options, including tone verification.
 * @param chan Asterisk channel associated with the radio or link.
 * @param option Asterisk channel option.
 * @param data Option-specific payload supplied by Asterisk.
 * @param datalen Payload length in bytes.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen);

/** @brief Assert the selected radio's calibration PTT request.
 * @param fd Asterisk CLI output descriptor.
 * @param argc Number of CLI arguments.
 * @param argv CLI argument vector.
 * @return Asterisk tuning-command result code.
 */
int console_key(int fd, int argc, const char *const *argv);

/** @brief Release the selected radio's calibration PTT request.
 * @param fd Asterisk CLI output descriptor.
 * @param argc Number of CLI arguments.
 * @param argv CLI argument vector.
 * @return Asterisk tuning-command result code.
 */
int console_unkey(int fd, int argc, const char *const *argv);

/** @brief Dispatch tuning commands for the selected radio channel.
 * @param fd Asterisk CLI output descriptor.
 * @param argc Number of CLI arguments.
 * @param argv CLI argument vector.
 * @return Asterisk tuning-command result code.
 */
int radio_tune(int fd, int argc, const char *const *argv);

/** @brief Apply transmit CTCSS deviation to the signaling generator.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int set_txctcss_level(struct chan_usbradio_pvt *o);

/** @brief Parse a normalized hardware tuning level in the range 0 through 999.
 * @param text Decimal hardware-level text.
 * @param level Receives the normalized 0–999 level on success.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int parse_tune_level(const char *text, int *level);

/** @brief Set the selected radio's DSP trace verbosity.
 * @param fd Asterisk CLI output descriptor.
 * @param argc Number of CLI arguments.
 * @param argv CLI argument vector.
 * @return Asterisk tuning-command result code.
 */
int radio_set_dsp_debug(int fd, int argc, const char *const *argv);

/** @brief Parse the configured receiver audio-source assignment.
 * @param o Private state of the selected radio channel.
 * @param s Receiver audio-source name: disabled, speaker, or flat.
 */
void store_rxdemod(struct chan_usbradio_pvt *o, const char *s);

/** @brief Parse the configured CTCSS indication source.
 * @param o Private state of the selected radio channel.
 * @param s Receive CTCSS indication-source name.
 */
void store_rxsdtype(struct chan_usbradio_pvt *o, const char *s);

/** @brief Convert resolved output-A hardware gain to the normalized CM119 mixer scale.
 * @param o Private state of the selected radio channel.
 * @return Normalized output-A mixer setting from 0 through 999.
 */
int effective_txmixaset(const struct chan_usbradio_pvt *o);

/** @brief Convert resolved output-B hardware gain to the normalized CM119 mixer scale.
 * @param o Private state of the selected radio channel.
 * @return Normalized output-B mixer setting from 0 through 999.
 */
int effective_txmixbset(const struct chan_usbradio_pvt *o);

/** @brief Read the resolved hardware carrier-detection source.
 * @param o Private state of the selected radio channel.
 * @return Selected carrier-detector assignment.
 */
enum radio_carrier_detect effective_rxcdtype(const struct chan_usbradio_pvt *o);

/** @brief Parse the transmitter CTCSS turn-off assignment.
 * @param o Private state of the selected radio channel.
 * @param s Transmit CTCSS turn-off mode name.
 */
void store_txtoctype(struct chan_usbradio_pvt *o, const char *s);

/** @brief Key the calibrated transmitter test tone for up to five seconds.
 * @param o Private state of the selected radio channel.
 * @param value Unused compatibility argument; output gain is already applied by tuning.
 * @param fd Asterisk CLI output descriptor.
 * @param intflag Nonzero permits interactive cancellation.
 */
void tune_txoutput(struct chan_usbradio_pvt *o, int value, int fd, int intflag);

/** @brief Refresh fixed-point radio calibration multipliers after tuning.
 * @param o Private state of the selected radio channel.
 */
void mult_set(struct chan_usbradio_pvt *o);

/** @brief Program configured radio frequency and power through the hardware bus.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_program_radio(struct chan_usbradio_pvt *o);

/** @brief Write a synthesizer-programming byte under the parallel-port mutex.
 * @param opaque Caller-owned hardware callback context.
 * @param value Parallel-port output byte.
 */
void usbradioplus_parallel_program_write(void *opaque, uint8_t value);

/** @brief Select a binary channel on the configured parallel-port interface.
 * @param channel Binary radio channel-select code.
 */
void usbradioplus_set_channel(uint8_t channel);

int radio_config(struct chan_usbradio_pvt *o);

/** @brief Resolve a named channel's unified hardware and radio options.
 * @param o Private state of the selected radio channel.
 * @param category Named radio configuration section.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int apply_processing_config_overrides(struct chan_usbradio_pvt *o, const char *category);
/** @brief Write current radio calibration values to the unified configuration.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int save_tuning_config(struct chan_usbradio_pvt *o);

/** @brief Allocate native receive/transmit processing and sample-rate-conversion state.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_dsp_init(struct chan_usbradio_pvt *o);

/** @brief Release native processing, rate-conversion, and echo buffers.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o);

/** @brief Prepare the undelayed radio-detector input from the native ADC block.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o);

/** @brief Read the selected DSP, VOX, USB, or parallel-port carrier indication.
 * @param o Private state of the selected radio channel.
 * @param source Carrier-detector assignment to evaluate.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int usbradioplus_carrier_detected(const struct chan_usbradio_pvt *o,
				  enum radio_carrier_detect source);

/** @brief Read the selected CTCSS indication, applying override and polarity.
 * @param o Private state of the selected radio channel.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int usbradioplus_ctcss_detected(const struct chan_usbradio_pvt *o);

/** @brief Publish the current decoded tone before the native PL-filter stage.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_refresh_ctcss_decode(struct chan_usbradio_pvt *o);

/** @brief Wait until the HID worker completes a pending EEPROM operation.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_wait_for_eeprom_idle(struct chan_usbradio_pvt *o);

/** @brief Start or complete echo recording when receiver key state changes.
 * @param o Private state of the selected radio channel.
 * @param was_keyed Receiver keyed state before this block.
 */
void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed);

/** @brief Load configured radio channels and their resolved processing profiles.
 * @param reload Nonzero selects reload handling for existing channels.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int load_config(int reload);

/** @brief Reload the module's channel and processing configuration.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int reload_module(void);

/** @brief Drive timed pulses on configured parallel-port outputs.
 * @param arg Private radio state passed to the worker.
 * @return NULL when the pulse worker exits.
 */
void *pulserthread(void *arg);

/** @brief Allocate and populate one named radio channel from configuration.
 * @param ctg Named radio configuration category.
 * @return Initialized private radio state, or NULL if setup fails.
 */
struct chan_usbradio_pvt *store_config(const char *ctg);
/** @brief Report active radio settings, hardware assignment, and detector state.
 * @param o Private state of the selected radio channel.
 * @param fd Asterisk CLI output descriptor.
 */
void radio_dump(struct chan_usbradio_pvt *o, int fd);
/** @brief Swap USB assignments between two configured radio channels.
 * @param fd Asterisk CLI output descriptor.
 * @param other Other configured channel whose USB assignment is exchanged.
 * @return Asterisk tuning-command result code.
 */
int usb_device_swap(int fd, const char *other);

#endif
