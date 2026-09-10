/** @file
 * @brief Select the adapter state shared with linked channel implementation objects.
 */

#ifndef USBRADIOPLUS_CHANNEL_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_PRIVATE_H

#include <stdatomic.h>

#include "txagc/avfilter_processor.h"
#include "usbradioplus_radio.h"

#define DUPLEX3_LEVEL_MAX 999

#define DEFAULT_ECHO_MAX 1000

#define URP_LEGACY_TEST_TONE_PEAK 7518.0

#define RX_ON_DELAY_MAX 60000

#define TX_OFF_DELAY_MAX 60000

#define MS_PER_FRAME 20

#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME)

#define READERR_THRESHOLD 50

#define QUEUE_SIZE 20 /* 400 milliseconds of sound-card output buffering. */

#define plus_mix_has_program(mix) urp_tx_output_has_program((enum urp_tx_output_mode)(mix))

#define PP_PORT "/dev/parport0"

#define PP_IOPORT 0x378

#define RPT_TO_STRING(x) #x

#define N_FMT(duf) "%30" #duf

#define CONFIG "usbradioplus.conf"

/** Names of supported carrier-detection assignments. */
extern const char *const cd_signal_type[];
/** Names of supported subaudible signaling-source assignments. */
extern const char *const sd_signal_type[];
/** Template defaults and head of the configured radio-channel list. */
extern struct chan_usbradio_pvt usbradio_default;
/** Asterisk jitter-buffer settings applied to newly created channels. */
extern struct ast_jb_conf global_jbconf;
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

/** Complete native-rate graph generation published as one callback snapshot. */
struct usbradioplus_native_graph_set {
	/** App-facing sample rate selected when this generation was prepared. */
	unsigned int app_rpt_rate;
	/** App-facing samples per native hardware tick. */
	unsigned int app_rpt_samples;
	/** Program-ring clock-recovery target in source-rate samples. */
	size_t program_target_samples;
	/** Nonzero when this generation serves the legacy app_rpt-rate interface. */
	int legacy_interface;
	/** Nonzero allows optional dynamics to idle while receiver qualification is absent. */
	int receive_cpu_saver;
	/** Nonzero applies the native DSP noise-squelch sample gate. */
	int noise_squelch_gate;
	/** Receiver delay expressed in immutable native-rate samples. */
	size_t receive_squelch_delay_samples;
	/** Echo-mode state selected by the control plane. */
	int echo_mode;
	/** Nonzero mixes local receive into transmit program PCM in this generation. */
	int software_repeat_enabled;
	/** Local-repeat gain resolved from the configured 0--999 control. */
	double software_repeat_gain;
	/** Whether the optional local receiver chain is enabled for this generation. */
	int local_chain_enabled;
	/** Whether this generation runs its prepared RNNoise stage before dynamics. */
	int local_rnnoise_enabled;
	/** Linear gain immediately after deemphasis, resolved off the audio thread. */
	double local_input_gain_linear;
	/** Fixed deemphasis stage immediately after ADC conversion. */
	struct txagc_avfilter receive_deemphasis;
	/** Fixed receiver band-pass and selected PL mode. */
	struct txagc_avfilter receive_filter;
	/** Optional local dynamics stages following receive filtering. */
	struct txagc_avfilter local_dynamics;
	/** Fixed transmitter filter/limiter/filter tail. */
	struct txagc_avfilter final;
	/** DCS spectrum-shaping stage. */
	struct txagc_avfilter dcs;
	/** DCS 134.4-Hz turn-off-tone spectrum-shaping stage. */
	struct txagc_avfilter dcs_turnoff;
	/** One prepared CTCSS notch graph per selectable decode code. */
	struct txagc_avfilter ctcss_notch[CTCSS_NUM_CODES];
	/** Older generation retained until a quiescent control-plane reclaim. */
	struct usbradioplus_native_graph_set *next_retired;
};

/** Atomically published native graph generation owned by a radio channel. */
struct usbradioplus_native_graph_slot {
	/** Current complete graph generation, or NULL before setup succeeds. */
	_Atomic(struct usbradioplus_native_graph_set *) active;
	/** Number of ticks or readers holding a generation reference. */
	_Atomic unsigned int readers;
	/** Serializes setup/reload/teardown, never taken by the audio callback. */
	atomic_flag writer;
	/** Current generation and any retired generations pending safe reclaim. */
	struct usbradioplus_native_graph_set *owned;
};

/** Lock-free reader/writer gate for mutable radio-signaling state.
 *
 * The legacy-compatible CTCSS parser replaces decoder-owned arrays.  Native
 * audio workers therefore announce the short span in which they touch the
 * signaling engine, while a control-plane reload temporarily excludes new
 * readers before reparsing.  The reader side uses atomics only and never
 * waits, allocates, or takes an adapter lock.
 */
struct usbradioplus_radio_access_slot {
	/** Active native audio spans reading the signaling engine. */
	_Atomic unsigned int readers;
	/** Nonzero while the control plane is replacing parser-owned state. */
	_Atomic int reconfiguring;
	/** Serializes rare control-plane reconfiguration requests. */
	atomic_flag writer;
};

/** Opaque per-channel worker that owns non-real-time native audio processing. */
struct usbradioplus_native_worker;
struct audiostatistics;

/** Meter values copied from one FFmpeg graph by its owning worker. */
struct usbradioplus_native_filter_statistics {
	/** Graph-reported latency in samples. */
	unsigned int latency_samples;
	/** Graph-reported queued samples. */
	unsigned int buffered_samples;
	/** Cumulative graph input samples. */
	unsigned long long input_samples;
	/** Cumulative graph output samples. */
	unsigned long long output_samples;
	/** Samples withheld during graph startup. */
	unsigned long long startup_fill_samples;
	/** Samples affected by graph runtime underrun. */
	unsigned long long runtime_underrun_samples;
	/** Most recent input peak in dBFS. */
	double input_peak_dbfs;
	/** Largest input peak in dBFS. */
	double input_max_peak_dbfs;
	/** Most recent input RMS in dBFS. */
	double input_rms_dbfs;
	/** Largest input RMS in dBFS. */
	double input_max_rms_dbfs;
	/** Most recent output peak in dBFS. */
	double output_peak_dbfs;
	/** Largest output peak in dBFS. */
	double output_max_peak_dbfs;
	/** Most recent output RMS in dBFS. */
	double output_rms_dbfs;
	/** Largest output RMS in dBFS. */
	double output_max_rms_dbfs;
	/** Peak before cleanup filtering in dBFS. */
	double cleanup_pre_peak_dbfs;
	/** Largest pre-cleanup peak in dBFS. */
	double cleanup_pre_max_peak_dbfs;
	/** RMS before cleanup filtering in dBFS. */
	double cleanup_pre_rms_dbfs;
	/** Largest pre-cleanup RMS in dBFS. */
	double cleanup_pre_max_rms_dbfs;
	/** RMS in the 5--8 kHz band before cleanup. */
	double cleanup_pre_5_8_rms_dbfs;
	/** Largest 5--8 kHz pre-cleanup RMS. */
	double cleanup_pre_5_8_max_rms_dbfs;
	/** RMS in the 5--8 kHz band after cleanup. */
	double cleanup_post_5_8_rms_dbfs;
	/** Largest 5--8 kHz post-cleanup RMS. */
	double cleanup_post_5_8_max_rms_dbfs;
	/** RMS above 8 kHz before cleanup. */
	double cleanup_pre_8_plus_rms_dbfs;
	/** Largest above-8-kHz pre-cleanup RMS. */
	double cleanup_pre_8_plus_max_rms_dbfs;
	/** RMS above 8 kHz after cleanup. */
	double cleanup_post_8_plus_rms_dbfs;
	/** Largest above-8-kHz post-cleanup RMS. */
	double cleanup_post_8_plus_max_rms_dbfs;
};

/** Lock-free snapshot of measurements owned by a native audio worker.
 *
 * The worker is the sole writer of these values.  Control-plane users obtain
 * a coherent copy through usbradioplus_native_worker_stats_read() rather than
 * reading live FFmpeg, RNNoise, SRC, or parrot state directly.
 */
struct usbradioplus_native_worker_stats {
	/** Completed native render blocks. */
	uint64_t native_frames;
	/** Failed native-rate/sample-rate conversions. */
	uint64_t src_errors;
	/** Program-source shortfalls observed while transmit was requested. */
	uint64_t link_queue_underflows;
	/** Complete ADC frames rejected because the worker input queue was full. */
	uint64_t worker_input_overflows;
	/** DAC/app frames replaced with silence because worker output was unavailable. */
	uint64_t worker_output_underflows;
	/** Published output records discarded because their paired PCM was malformed. */
	uint64_t worker_output_malformed;
	/** Native echo playback blocks emitted. */
	uint64_t parrot_playback_frames;
	/** Current native echo recording length in samples. */
	size_t parrot_samples;
	/** Nonzero while the native echo recording is playing. */
	int parrot_playing;
	/** Nonzero if the native echo recording reached its configured limit. */
	int parrot_truncated;
	/** Receiver ADC peak for the most recent rendered block. */
	double adc_peak_dbfs;
	/** Largest receiver ADC peak since the last reset. */
	double adc_max_peak_dbfs;
	/** Cumulative receiver ADC PCM-rail samples. */
	uint64_t adc_rail_samples;
	/** Local-repeat input peak for the most recent block. */
	double preemphasis_input_peak_dbfs;
	/** Largest local-repeat input peak since the last reset. */
	double preemphasis_input_max_peak_dbfs;
	/** Local-repeat program peak for the most recent block. */
	double local_tx_peak_dbfs;
	/** Largest local-repeat program peak since the last reset. */
	double local_tx_max_peak_dbfs;
	/** Final transmitter program peak for the most recent block. */
	double tx_program_peak_dbfs;
	/** Largest final transmitter program peak since the last reset. */
	double tx_program_max_peak_dbfs;
	/** Cumulative final transmitter PCM-rail samples. */
	uint64_t tx_program_rail_samples;
	/** RNNoise frames completed by the worker. */
	uint64_t rnnoise_frames;
	/** RNNoise output samples delivered by the worker. */
	uint64_t rnnoise_output_samples;
	/** RNNoise startup samples withheld before output became available. */
	uint64_t rnnoise_startup_samples;
	/** RNNoise or its converters' cumulative failures. */
	uint64_t rnnoise_errors;
	/** Most recent RNNoise voice-activity probability. */
	double rnnoise_vad_probability;
	/** Measurements from the local receive dynamics graph. */
	struct usbradioplus_native_filter_statistics local_filter;
	/** Measurements from the fixed receive deemphasis graph. */
	struct usbradioplus_native_filter_statistics receive_deemphasis_filter;
	/** Measurements from the fixed transmitter final graph. */
	struct usbradioplus_native_filter_statistics final_filter;
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
/** @brief Read the resolved output-A program/transmit-signaling routing assignment.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *channel);
/** @brief Read the resolved output-B program/transmit-signaling routing assignment.
 * @param channel Private state of the selected radio channel.
 * @return Resolved gain, mixer level, or routing value in the units described above.
 */
enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *channel);
/** @brief Apply changed hardware gains, assignments, and CTCSS maps from the control plane.
 * @param channel Private state of the selected radio channel.
 *
 * This operation writes mixer controls and may take adapter locks. It must run
 * during setup, configuration reload, or tuning--never from a native audio callback.
 */
void refresh_processing_hardware(struct chan_usbradio_pvt *channel);
/** @brief Apply resolved hardware settings to every live radio from the control plane.
 * @return Zero after visiting all configured channels.
 */
int usbradioplus_refresh_all_processing_hardware(void);
/** @brief Apply validated signaling selections to live radio states.
 * @return Zero when every initialized radio accepted the staged selection.
 */
int usbradioplus_refresh_all_processing_signaling(void);
/** @brief Begin a lock-free native-audio read span over radio signaling state.
 * @param channel Private channel whose signaling state is read.
 * @return Nonzero when the caller may use channel->radio; zero during reload.
 */
int usbradioplus_radio_access_acquire(struct chan_usbradio_pvt *channel);
/** @brief End a radio signaling state read span started by the hardware-paced audio path.
 * @param channel Private channel whose read span ends.
 */
void usbradioplus_radio_access_release(struct chan_usbradio_pvt *channel);
/** @brief Acquire the native graph generation published for one channel.
 * @param channel Native radio channel.
 * @return Stable graph generation until usbradioplus_native_graphs_release().
 */
struct usbradioplus_native_graph_set *
usbradioplus_native_graphs_acquire(struct chan_usbradio_pvt *channel);
/** @brief Release a native graph generation acquired for one channel.
 * @param channel Native radio channel.
 */
void usbradioplus_native_graphs_release(struct chan_usbradio_pvt *channel);
/** @brief Process a native receiver block and render the corresponding transmitter block.
 * @param channel Private state of the selected radio channel.
 */
void usbradioplus_native_tick(struct chan_usbradio_pvt *channel);
/** @brief Acknowledge that the current native DAC frame was completely accepted.
 * @param channel Private state of the selected radio channel.
 *
 * The hardware callback calls this only after a complete sound-device write.
 * It retires only the native frame copied by that callback; a write that
 * deliberately substituted idle silence discards that exact staged frame.
 * Until then, the native TX queue retains the same frame for retry while
 * receive PCM continues toward app_rpt at its normal cadence.
 */
void usbradioplus_native_tx_output_ack(struct chan_usbradio_pvt *channel);
/** @brief Start the per-channel non-real-time native audio worker.
 * @param channel Private state of the selected radio channel.
 * @return Zero on success or nonzero when the worker could not start.
 *
 * The worker owns FFmpeg, RNNoise, and sample-rate conversion. The hardware
 * callback exchanges only bounded PCM frames and snapshot metadata with it
 * through SPSC queues.
 */
int usbradioplus_native_worker_start(struct chan_usbradio_pvt *channel);
/** @brief Stop and destroy a channel's native audio worker.
 * @param channel Private state of the selected radio channel.
 */
void usbradioplus_native_worker_stop(struct chan_usbradio_pvt *channel);
/** @brief Copy the most recently published coherent native-worker diagnostics snapshot.
 * @param channel Private state of the selected radio channel.
 * @param statistics Receives the current worker-owned measurements.
 * @return Zero on success, or nonzero when native processing is unavailable or a
 * bounded retry cannot pin a snapshot without waiting.
 */
int usbradioplus_native_worker_stats_read(struct chan_usbradio_pvt *channel,
					  struct usbradioplus_native_worker_stats *statistics);
/** @brief Request a worker-side reset of native diagnostic measurements.
 * @param channel Private state of the selected radio channel.
 *
 * The request is consumed between complete worker frames, so it never races
 * mutable FFmpeg or RNNoise measurement state.
 */
void usbradioplus_native_worker_stats_reset(struct chan_usbradio_pvt *channel);
/** @brief Request that the worker discard native echo recording and playback.
 * @param channel Private state of the selected radio channel.
 *
 * The request clears native echo playback and its admission gate at the next
 * complete render-frame boundary.
 */
void usbradioplus_native_worker_clear_parrot(struct chan_usbradio_pvt *channel);
/** @brief Request that the worker discard queued legacy echo PCM.
 * @param channel Private state of the selected radio channel.
 *
 * The request is consumed between complete worker frames by advancing the
 * legacy echo queue's consumer cursor.  It never resets the producer cursor
 * while the audio adapter can still record echo audio.
 */
void usbradioplus_native_worker_clear_legacy_echo(struct chan_usbradio_pvt *channel);
/** @brief Copy transmitter audio measurements owned by the native worker.
 * @param channel Private state of the selected radio channel.
 * @param statistics Receives the Asterisk-compatible transmitter meter state.
 * @return Zero on success, or nonzero when native processing is unavailable or a
 * bounded retry cannot pin a snapshot without waiting.
 */
int usbradioplus_native_worker_tx_audio_stats_read(struct chan_usbradio_pvt *channel,
						   struct audiostatistics *statistics);
#ifdef URP_PROCESSING_TESTING
/** @brief Advance queued native graph work explicitly in the deterministic harness.
 * @param channel Channel with an already started native worker.
 *
 * This test-only operation runs outside @ref usbradioplus_native_tick.  Tests
 * call it after submitting a callback frame, then use a later callback frame
 * (or the output-consumption hook below) to consume the rendered result.
 */
void usbradioplus_native_worker_test_process_all(struct chan_usbradio_pvt *channel);
/** @brief Queue one synthetic native-worker output record for a deterministic regression.
 * @param channel Channel with an already started native worker.
 * @param descriptor_app_samples App-sample count written into the output descriptor.
 * @param paired_app_samples Actual app PCM words placed before the fixed DAC span.
 * @param fill Value used for every injected PCM word.
 * @return Zero on success, or nonzero if the bounded test queue cannot accept the record.
 *
 * This test-only hook deliberately permits a descriptor count different from its
 * paired app span. Production code never constructs such a record; the hook
 * verifies that corruption cannot wedge the consumer or affect signaling PTT.
 */
int usbradioplus_native_worker_test_inject_output(struct chan_usbradio_pvt *channel,
						  unsigned int descriptor_app_samples,
						  unsigned int paired_app_samples, short fill);
/** @brief Consume one synthetic native-worker result using normal callback behavior.
 * @param channel Channel with an already started native worker.
 *
 * The helper runs the ordinary output-consumption path and independently
 * republishes the signaling engine's desired physical PTT state. It exists only
 * for deterministic queue-corruption regression tests.
 */
void usbradioplus_native_worker_test_consume(struct chan_usbradio_pvt *channel);
/** @brief Consume a synthetic result while deliberately retaining its DAC frame.
 * @param channel Channel with an already started native worker.
 *
 * This test-only hook models a temporarily full sound-device queue.  A later
 * ordinary test consume acknowledges the retained frame.
 */
void usbradioplus_native_worker_test_consume_unacked(struct chan_usbradio_pvt *channel);
#endif
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
