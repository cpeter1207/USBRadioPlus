/** @file
 * @brief Shared Asterisk channel lifecycle, radio configuration, and tuning operations.
 */

#ifndef USBRADIOPLUS_CHANNEL_COMMON_H
#define USBRADIOPLUS_CHANNEL_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "asterisk/channel.h"
#include "asterisk/config.h"

#include "usbradioplus_channel_private.h"
#include "usbradioplus_rpt_advanced.h"

/** @brief Select transport mode before starting an exclusively owned channel.
 * @param channel Private hardware state with no active audio reader.
 * @param advanced Nonzero selects hardware-clocked native PCM; zero selects app_rpt.
 */
void usbradioplus_interface_mode(struct chan_usbradio_pvt *channel, int advanced);

/** @brief Configure a newly reserved Asterisk channel for rpt_advanced.
 * @param channel Exclusively owned channel which has not been called yet.
 */
void usbradioplus_configure_advanced(struct ast_channel *channel);

/** Parallel outputs with active timed pulses. */
extern int8_t pp_pulsemask;
/** Previously applied parallel-port pulse mask. */
extern int8_t pp_lastmask;

int hidhdwconfig(struct chan_usbradio_pvt *o);

/** @brief Return the first configured radio channel in module-list order.
 * @return Head of the configured channel list, or NULL when no radios exist.
 */
struct chan_usbradio_pvt *usbradioplus_channel_first(void);

/** @brief Release a channel that failed before it was linked into the active list.
 * @param o Private state of the selected radio channel.
 */
void destroy_unlinked_channel(struct chan_usbradio_pvt *o);

/** @brief Wake the HID worker after a control-plane output request.
 * @param o Private state of the selected radio channel.
 *
 * Native audio never calls this helper. The worker also polls its atomic PTT
 * request so a missed wake cannot delay a fail-safe unkey indefinitely.
 */
void kickptt(const struct chan_usbradio_pvt *o);

/** Bits packed by the HID worker into plus_hardware_inputs. */
enum usbradioplus_hardware_input_bits {
	URP_HARDWARE_INPUT_HID_CARRIER = 1U << 0,
	URP_HARDWARE_INPUT_HID_CTCSS = 1U << 1,
	URP_HARDWARE_INPUT_PARALLEL_CARRIER = 1U << 2,
	URP_HARDWARE_INPUT_PARALLEL_CTCSS = 1U << 3,
};

/** @brief Snapshot of a control-plane radio-programming request. */
struct usbradioplus_radio_program_request {
	/** Requested receive frequency in hertz. */
	uint32_t rx_frequency;
	/** Requested transmit frequency in hertz. */
	uint32_t tx_frequency;
	/** Nonzero requests the high-power radio programming profile. */
	int high_power;
	/** Monotonic control-plane publication generation. */
	unsigned int generation;
};

/* Copy hardware-worker input and PTT-ack snapshots for the native callback.
 *
 * This operation uses atomics only. The HID worker must never modify the
 * signaling state directly.
 */
void usbradioplus_audio_load_hardware_state(struct chan_usbradio_pvt *channel);

/** @brief Publish the signaling engine's immediate PTT request from the native callback.
 * @param channel Private channel whose callback completed the signaling tick.
 * @param asserted Nonzero requests physical PTT assertion.
 */
void usbradioplus_publish_hardware_ptt(struct chan_usbradio_pvt *channel, int asserted);

/** @brief Restore the signaling engine's unmodified PTT output before its callback tick.
 * @param channel Private channel whose virtual DAC-drain PTT state is restored.
 */
void usbradioplus_tx_playout_hold_prepare(struct chan_usbradio_pvt *channel);

/** @brief Apply the post-DAC PTT hold after a signaling tick.
 * @param channel Private channel whose signaling-engine PTT result is available.
 */
void usbradioplus_tx_playout_hold_apply(struct chan_usbradio_pvt *channel);

/** @brief Publish effective PTT and select source PCM after applying the hold.
 * @param channel Private channel whose post-DAC PTT state is ready.
 */
void usbradioplus_tx_playout_hold_publish(struct chan_usbradio_pvt *channel);

/** @brief Reset a DAC-drain hold after the underlying audio device is reset.
 * @param channel Private channel whose playback queue was discarded.
 */
void usbradioplus_tx_playout_hold_reset(struct chan_usbradio_pvt *channel);

/** @brief Record one accepted native DAC block for post-playout PTT timing.
 * @param channel Private channel owning the submission.
 * @param submitted Nonzero only when one complete DAC block was accepted.
 * @param audio_bearing Nonzero only when that accepted block contains transmitted PCM.
 * @param callbacks Number of native callbacks through queued playout plus the safety block.
 */
void usbradioplus_tx_playout_hold_note_output(struct chan_usbradio_pvt *channel, int submitted,
					      int audio_bearing, unsigned int callbacks);

/** @brief Report whether PTT is currently held only to drain already queued audio.
 * @param channel Private channel whose callback state is queried.
 * @return Nonzero while DAC output must be replaced with silence.
 */
int usbradioplus_tx_playout_hold_draining(const struct chan_usbradio_pvt *channel);

/** @brief Determine whether an interleaved PCM block contains any non-silent sample.
 * @param samples PCM samples to inspect.
 * @param count Number of samples in @p samples.
 * @return Nonzero when at least one sample is nonzero.
 */
int usbradioplus_pcm_has_audio(const short *samples, size_t count);

/** @brief Publish a native-audio clipping indication for the HID worker.
 * @param channel Private channel whose clip LED should pulse.
 */
void usbradioplus_request_clip_led(struct chan_usbradio_pvt *channel);

/** @brief Publish a packed receiver-input snapshot from the HID worker.
 * @param channel Private channel whose hardware inputs were sampled.
 * @param inputs ORed usbradioplus_hardware_input_bits values.
 */
void usbradioplus_publish_hardware_inputs(struct chan_usbradio_pvt *channel, unsigned int inputs);

/** @brief Read a coherent radio-programming request without taking a lock.
 * @param channel Private channel providing the control-plane snapshot.
 * @param request Receives a stable request on success.
 * @return Nonzero when a stable request was read; zero while a writer is active.
 */
int usbradioplus_read_radio_program_request(const struct chan_usbradio_pvt *channel,
					    struct usbradioplus_radio_program_request *request);

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

/** @brief Queue source-rate app_rpt PCM for asynchronous native transmitter rendering.
 * @param o Private state of the selected radio channel.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 */
void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples, size_t count);

/** @brief Discard legacy echo audio while no echo playback is active.
 * @param o Private state of the selected radio channel.
 */
void usbradioplus_echo_clear(struct chan_usbradio_pvt *o);

/** @brief Begin legacy echo playback if captured samples are available.
 * @param o Private state of the selected radio channel.
 * @return Nonzero when playback is active; zero when no echo was captured.
 */
int usbradioplus_echo_start(struct chan_usbradio_pvt *o);

/** @brief Record one app_rpt-rate receive block without allocating or locking.
 * @param o Private state of the selected radio channel.
 * @param samples PCM samples to capture.
 * @param count Number of supplied samples.
 */
void usbradioplus_echo_record(struct chan_usbradio_pvt *o, const short *samples, size_t count);

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

/** @brief Publish configured radio frequency and power for the HID worker.
 * @param o Private state of the selected radio channel.
 *
 * The HID worker owns physical parallel-port writes. This function is safe for
 * setup and control-plane callers but never performs bus I/O itself.
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

#ifdef URP_PROCESSING_TESTING
/** @brief Exercise CTCSS-list validation through the production parser helper. */
int usbradioplus_test_ctcss_frequency_list_valid(const char *frequencies, size_t *count);
/** @brief Exercise receive-to-transmit CTCSS-map validation through the production helper. */
int usbradioplus_test_ctcss_frequency_lists_mapped(const char *receive_frequencies,
						   const char *transmit_frequencies);
/** @brief Exercise one-frequency CTCSS validation through the production helper. */
int usbradioplus_test_ctcss_frequency_valid(const char *frequency);
/** @brief Exercise DCS-code validation through the production parser helper. */
int usbradioplus_test_dcs_code_valid(const char *code);
/** @brief Exercise resolver argument validation without exposing its private candidate type. */
int usbradioplus_test_resolve_processing_signaling(const char *category, int null_result);
/** @brief Exercise the parser-held radio configuration failure path. */
int usbradioplus_test_radio_config_locked(struct chan_usbradio_pvt *channel);
/** @brief Exercise deterministic parser-access writer and reader contention paths. */
int usbradioplus_test_radio_access_contention_paths(void);
/** @brief Exercise a radio-program snapshot retry without relying on thread timing. */
int usbradioplus_test_radio_program_snapshot_retry(void);
/** @brief Find a configured CTCSS code exactly as native notch-graph preparation does. */
int usbradioplus_test_native_ctcss_code_frequency(const char *frequencies, int code,
						  double *frequency);
/** @brief Build a native graph generation without publishing it for deterministic failure tests. */
int usbradioplus_test_native_graph_set_build(const struct chan_usbradio_pvt *channel,
					     struct usbradioplus_native_graph_set **graphs);
/** @brief Exercise deterministic native-graph control-plane contention paths. */
int usbradioplus_test_native_graph_slot_contention_paths(void);
#endif

/** @brief Resolve a named channel's unified hardware and radio options.
 * @param o Private state of the selected radio channel.
 * @param category Named radio configuration section.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int apply_processing_config_overrides(struct chan_usbradio_pvt *o, const char *category);
/** @brief Resolve only clean-slate signaling controls without touching parser state.
 * @param o Channel receiving a fully validated scalar signaling selection.
 * @param category Named channel/profile whose overrides are resolved.
 * @return Zero on success; nonzero without modifying o for invalid input.
 *
 * The processing reload calls this helper after graph staging and before the
 * candidate snapshot is published.  The caller must then invoke radio_config()
 * to replace parser-owned CTCSS/DCS state under its control-plane gate.
 */
int apply_processing_signaling_overrides(struct chan_usbradio_pvt *o, const char *category);
/** @brief Determine whether a bare configuration section creates a radio channel.
 *
 * Flat processing and signaling sections are defaults, never radio-channel
 * definitions. Keep this classification shared with the configuration loader
 * so a signaling section that precedes a named radio cannot become active.
 *
 * @param section Bare Asterisk configuration-section name.
 * @return Nonzero when section names a radio channel; zero for a reserved or invalid name.
 */
int usbradioplus_is_radio_channel_section(const char *section);
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

/** @brief Build and atomically publish a channel's native FFmpeg graphs.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; nonzero while retaining each prior graph on failure.
 *
 * This is a control-plane operation. Native audio ticks only use the already
 * prepared graphs it publishes.
 */
int usbradioplus_prepare_native_processing(struct chan_usbradio_pvt *o);
/** @brief Rebuild native graphs for all configured channels after a valid reload.
 * @return Zero on success; nonzero if any channel keeps its earlier graph set.
 */
int usbradioplus_prepare_all_native_processing(void);

/** Opaque all-channel native graph transaction built by the control plane. */
struct usbradioplus_native_graph_transaction;
/** @brief Stage all native graph replacements without changing callback state.
 * @param transaction Receives a private transaction to publish or discard.
 * @return Zero on success; nonzero while retaining all prior generations.
 */
int usbradioplus_stage_all_native_processing(
	struct usbradioplus_native_graph_transaction **transaction);
/** @brief Publish a successfully staged native graph transaction.
 * @param transaction Transaction returned by native graph staging.
 */
void usbradioplus_publish_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction);
/** @brief Destroy a staged native graph transaction without publishing it.
 * @param transaction Transaction returned by native graph staging; NULL is accepted.
 */
void usbradioplus_discard_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction);

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

/** @brief Copy a decoded CTCSS transition for the channel frame source.
 *
 * The hardware-paced audio callback calls this helper, so it performs only a
 * bounded state copy and does not log, wait, or acquire a lock.
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
