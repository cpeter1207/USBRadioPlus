/** @file
 * @brief Select the adapter state shared with linked channel implementation objects.
 */

#ifndef USBRADIOPLUS_CHANNEL_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_PRIVATE_H

#define PLUS_LINK_NATIVE_TARGET_SAMPLES (URP_NATIVE_SAMPLES * 3)

#define DUPLEX3_LEVEL_MAX 999

#define DEFAULT_ECHO_MAX 1000

#define URP_LEGACY_TEST_TONE_PEAK 7518.0

#define RX_ON_DELAY_MAX 60000

#define TX_OFF_DELAY_MAX 60000

#define MS_PER_FRAME 20

#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME)

#define DEFAULT_TX_SOFT_LIMITER_SETPOINT 12000

#define READERR_THRESHOLD 50

#define QUEUE_SIZE 20 /* 400 milliseconds of sound-card output buffering. */

#define plus_mix_has_program(mix) urp_tx_output_has_program((enum urp_tx_output_mode)(mix))

#define PP_PORT "/dev/parport0"

#define PP_IOPORT 0x378

#define RPT_TO_STRING(x) #x

#define N_FMT(duf) "%30" #duf

#define CONFIG "usbradioplus.conf"

#define RX_CAP_RAW_FILE "/tmp/rx_cap_in.pcm"

#define RX_CAP_TRACE_FILE "/tmp/rx_trace.pcm"

#define TX_CAP_RAW_FILE "/tmp/tx_cap_in.pcm"

#define TX_CAP_TRACE_FILE "/tmp/tx_trace.pcm"

/** Names of supported carrier-detection assignments. */
extern const char *const cd_signal_type[];
/** Names of supported subaudible signaling-source assignments. */
extern const char *const sd_signal_type[];
/** Template defaults and head of the configured radio-channel list. */
extern struct chan_usbradio_pvt usbradio_default;
/** Asterisk jitter-buffer settings applied to newly created channels. */
extern struct ast_jb_conf global_jbconf;
/** Receiver raw input capture stream. */
extern FILE *frxcapraw;
/** Receiver trace capture stream. */
extern FILE *frxcaptrace;
/** Receiver output capture stream. */
extern FILE *frxoutraw;
/** Transmitter raw input capture stream. */
extern FILE *ftxcapraw;
/** Transmitter trace capture stream. */
extern FILE *ftxcaptrace;
/** Transmitter output capture stream. */
extern FILE *ftxoutraw;
/** Mutex protecting shared parallel-port output state. */
extern ast_mutex_t pp_lock;
/** Cached parallel-port output byte. */
extern int8_t pp_val;
/** Parallel outputs with active timed pulses. */
extern int8_t pp_pulsemask;
/** Previously applied parallel-port pulse mask. */
extern int8_t pp_lastmask;
/** Remaining pulse duration for each parallel output. */
extern int pp_pulsetimer[32];
/** Nonzero when parallel-port hardware is available. */
extern int haspp;
/** Open parallel-port device descriptor. */
extern int ppfd;
/** Parallel-port device path. */
extern char pport[50];
/** Parallel-port I/O base address. */
extern int pbase;
/** Stop request observed by the parallel-port pulse worker. */
extern char stoppulser;
/** Name of the radio selected for interactive tuning. */
extern char *usbradio_active;

/** Local receiver-to-transmitter repeat implementation. */
enum duplex3_mode {
	DUPLEX3_MODE_HARDWARE = 0 /**< DUPLEX3 MODE HARDWARE. */,
	DUPLEX3_MODE_SOFTWARE /**< DUPLEX3 MODE SOFTWARE. */
};

#ifdef URP_CHANNEL_MODERN
#include "usbradioplus_channel_modern_private.h"
#else
#include "usbradioplus_channel_legacy_private.h"
#endif

#define plus_parrot plus_parrot_state.audio

#define plus_parrot_capacity plus_parrot_state.capacity

#define plus_parrot_count plus_parrot_state.count

#define plus_parrot_play plus_parrot_state.play

#define plus_parrot_playing plus_parrot_state.playing

#define plus_parrot_truncated plus_parrot_state.truncated

#define usbradioplus_native_echo(channel)                                                          \
	urp_native_echo_enabled((channel)->duplex3, (channel)->duplex3mode == DUPLEX3_MODE_SOFTWARE)

/** @brief Read the resolved local-receiver gain immediately after deemphasis.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
double effective_rx_input_gain_db(const struct chan_usbradio_pvt *channel);
/** @brief Read the resolved output-A program/CTCSS routing assignment.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *channel);
/** @brief Read the resolved output-B program/CTCSS routing assignment.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *channel);
/** @brief Apply changed hardware gains, assignments, and CTCSS maps to a live radio.
 * @param channel Private state of the selected radio channel.
 */
void refresh_processing_hardware(struct chan_usbradio_pvt *channel);
/** @brief Append resampled app_rpt audio to the elastic native FIFO.
 * @param channel Private state of the selected radio channel.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 */
void plus_link_native_push(struct chan_usbradio_pvt *channel, const short *samples, size_t count);
/** @brief Take one complete native-rate transmitter block from the elastic FIFO.
 * @param channel Private state of the selected radio channel.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int plus_link_native_pop(struct chan_usbradio_pvt *channel, short *samples);
/** @brief Update transmitter peak, RMS, and clipping measurements.
 * @param channel Private state of the selected radio channel.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 */
void usbradioplus_check_tx_audio(struct chan_usbradio_pvt *channel, short *samples, size_t count);
/** @brief Process a native receiver block and render the corresponding transmitter block.
 * @param channel Private state of the selected radio channel.
 */
void usbradioplus_native_tick(struct chan_usbradio_pvt *channel);
/** @brief Read capture, playback, and sidetone mixer limits for calibration.
 * @param channel Private state of the selected radio channel.
 * @param microphone_max Receives the maximum capture mixer step.
 * @param speaker_max Receives the maximum playback mixer step.
 * @param microphone_playback_max Receives the maximum hardware repeat mixer step.
 */
void usbradioplus_tune_mixer_limits(struct chan_usbradio_pvt *channel, int *microphone_max,
				    int *speaker_max, int *microphone_playback_max);
/** @brief Dispatch the calibration utility's live command protocol.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param command CLI initialization, completion, or execution selector.
 */
void tune_menusupport(int fd, struct chan_usbradio_pvt *channel, const char *command);
/** @brief Apply hardware capture, playback, and local-repeat mixer settings.
 * @param o Private state of the selected radio channel.
 */
void mixer_write(struct chan_usbradio_pvt *o);
/** @brief Save current tuning to configuration and request EEPROM storage when enabled.
 * @param o Private state of the selected radio channel.
 */
void tune_write(struct chan_usbradio_pvt *o);
/** @brief Calibrate the hardware capture mixer from discriminator noise and optionally set squelch.
 * @param fd Asterisk CLI output descriptor.
 * @param o Private state of the selected radio channel.
 * @param setsql Nonzero also calibrates the DSP squelch threshold.
 * @param intflag Nonzero permits interactive cancellation.
 */
void tune_rxinput(int fd, struct chan_usbradio_pvt *o, int setsql, int intflag);
/** @brief Display or adjust the post-deemphasis receiver voice gain.
 * @param fd Asterisk CLI output descriptor.
 * @param o Private state of the selected radio channel.
 * @param str Text supplied by the tuning command.
 */
void _menu_rxvoice(int fd, struct chan_usbradio_pvt *o, const char *str);
/** @brief Report active radio tuning and device assignments.
 * @param fd Asterisk CLI output descriptor.
 * @param o Private state of the selected radio channel.
 */
void _menu_print(int fd, struct chan_usbradio_pvt *o);
/** @brief Transmit three calibration bursts while honoring interactive cancellation.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param interactive Nonzero permits interactive cancellation.
 */
void tune_flash(int fd, struct chan_usbradio_pvt *channel, int interactive);
/** @brief Check the calibrated transmitter soft-limiter setpoint before applying it.
 * @param channel Private state of the selected radio channel.
 * @param setpoint Normalized calibration setpoint.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int validate_tx_soft_limiter_setpoint(struct chan_usbradio_pvt *channel, int setpoint);
/** @brief Read the resolved CTCSS decoder-input gain.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
float effective_rx_decoder_gain(const struct chan_usbradio_pvt *channel);
/** @brief Convert resolved hardware input gain to the normalized CM119 mixer scale.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
int effective_rxmixerset(const struct chan_usbradio_pvt *channel);
/** @brief Stream the receive voice calibration level until input cancels the display.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 */
void tune_rxdisplay(int fd, struct chan_usbradio_pvt *channel);
/** @brief Stream COS, CTCSS, PTT, receive, and transmit measurements on one screen.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 */
void tune_rxtx_status(int fd, struct chan_usbradio_pvt *channel);
/** @brief Display or adjust the live DSP squelch threshold.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param value Optional textual tuning command; an empty string requests the current value.
 */
void _menu_rxsquelch(int fd, struct chan_usbradio_pvt *channel, const char *value);
/** @brief Display or adjust the voice output and optional calibration tone.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param value Optional textual tuning command; an empty string requests the current value.
 */
void _menu_txvoice(int fd, struct chan_usbradio_pvt *channel, const char *value);
/** @brief Display or adjust the auxiliary voice output level.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param value Optional textual tuning command; an empty string requests the current value.
 */
void _menu_auxvoice(int fd, struct chan_usbradio_pvt *channel, const char *value);
/** @brief Display or adjust transmit CTCSS level and optional keyed tone.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param value Optional textual tuning command; an empty string requests the current value.
 */
void _menu_txtone(int fd, struct chan_usbradio_pvt *channel, const char *value);
/** @brief Calibrate receiver voice gain from a 1 kHz reference signal.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param interactive Nonzero permits interactive cancellation.
 */
void tune_rxvoice(int fd, struct chan_usbradio_pvt *channel, int interactive);
/** @brief Calibrate the CTCSS decoder's input level.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Private state of the selected radio channel.
 * @param interactive Nonzero permits interactive cancellation.
 */
void tune_rxctcss(int fd, struct chan_usbradio_pvt *channel, int interactive);
/** @brief Reserve native echo storage for the configured maximum duration.
 * @param channel Private state of the selected radio channel.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_ensure_parrot_capacity(struct chan_usbradio_pvt *channel);
/** @brief Return the head of the configured radio-channel list.
 * @return Borrowed head of the configured channel list.
 */
struct chan_usbradio_pvt *usbradioplus_channel_first(void);
/** @brief Find a configured radio by channel name or USB-device identifier.
 * @param device Channel name or USB device identifier.
 * @return Borrowed matching channel, or NULL if none matches.
 */
struct chan_usbradio_pvt *find_desc(const char *device);

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def PLUS_LINK_NATIVE_TARGET_SAMPLES
 * @brief Target occupancy of the native transmitter FIFO in samples.
 */
/** @def DUPLEX3_LEVEL_MAX
 * @brief Maximum normalized local-repeat level.
 */
/** @def DEFAULT_ECHO_MAX
 * @brief Default app_rpt-rate echo capacity in frames.
 */
/** @def URP_LEGACY_TEST_TONE_PEAK
 * @brief PCM peak of the calibrated 1 kHz transmitter test tone.
 */
/** @def RX_ON_DELAY_MAX
 * @brief Maximum receiver-on delay in app_rpt frames.
 */
/** @def TX_OFF_DELAY_MAX
 * @brief Maximum post-transmit receiver delay in app_rpt frames.
 */
/** @def MS_PER_FRAME
 * @brief Duration of one app_rpt processing frame in milliseconds.
 */
/** @def MS_TO_FRAMES
 * @brief Convert a millisecond interval to app_rpt frame count.
 */
/** @def DEFAULT_TX_SOFT_LIMITER_SETPOINT
 * @brief Default normalized final-limiter calibration setpoint.
 */
/** @def READERR_THRESHOLD
 * @brief Consecutive audio-read failures that trigger device recovery.
 */
/** @def QUEUE_SIZE
 * @brief 400 milliseconds of sound-card output buffering.
 */
/** @def plus_mix_has_program
 * @brief Test whether a hardware output assignment carries program audio.
 */
/** @def PP_PORT
 * @brief Default parallel-port device index.
 */
/** @def PP_IOPORT
 * @brief Default parallel-port I/O base address.
 */
/** @def RPT_TO_STRING
 * @brief Stringify a macro value after expansion.
 */
/** @def N_FMT
 * @brief Generate a numeric-setting format fragment.
 */
/** @def CONFIG
 * @brief Unified channel-driver configuration filename.
 */
/** @def RX_CAP_RAW_FILE
 * @brief Receiver cap raw file path.
 */
/** @def RX_CAP_TRACE_FILE
 * @brief Receiver cap trace file path.
 */
/** @def TX_CAP_RAW_FILE
 * @brief Transmitter cap raw file path.
 */
/** @def TX_CAP_TRACE_FILE
 * @brief Transmitter cap trace file path.
 */
/** @def plus_parrot
 * @brief Alias for the native echo sample buffer.
 */
/** @def plus_parrot_capacity
 * @brief Alias for native echo allocation capacity.
 */
/** @def plus_parrot_count
 * @brief Alias for the native echo recording length.
 */
/** @def plus_parrot_play
 * @brief Alias for the native echo playback cursor.
 */
/** @def plus_parrot_playing
 * @brief Alias for native echo playback state.
 */
/** @def plus_parrot_truncated
 * @brief Alias for native echo truncation state.
 */
/** @def usbradioplus_native_echo
 * @brief Select native echo when software local repeat is active.
 */
/** @} */
