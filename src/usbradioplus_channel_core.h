/** @file
 * @brief Transport-independent PCM conversion, routing, elastic queues, and echo state.
 */

#ifndef USBRADIOPLUS_CHANNEL_CORE_H
#define USBRADIOPLUS_CHANNEL_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "usbradioplus_dsp.h"

/* A non-lock-free atomic implementation would violate the audio-thread contract. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "USBRadioPlus requires lock-free atomic cursors");

/** Maximum retained program-audio history: 200 ms at the native rate. */
#define URP_PROGRAM_RING_FRAMES 10U
/** Number of PCM samples allocated for the app_rpt-to-native program ring. */
#define URP_PROGRAM_RING_SAMPLES (URP_PROGRAM_RING_FRAMES * URP_NATIVE_SAMPLES)
/** Source-audio occupancy used by the program-ring drift controller.
 *
 * This is a controller setpoint, not an admission threshold: playout starts
 * immediately and a temporary shortfall is concealed by the consumer.  The
 * 110 ms target leaves room for independent Asterisk and CM119 clocks without
 * adding a keyed-transmit startup delay.
 */
#define URP_PROGRAM_RING_TARGET_MS 110U
/** Maximum duration retained by the legacy 8 kHz echo path: 20 seconds. */
#define URP_ECHO_QUEUE_SAMPLES (URP_APP_RPT_RATE_DEFAULT * 20U)

/** Transport-independent receive audio source values. */
/** Receiver audio-source assignments. */
enum urp_rx_audio_mode {
	URP_RX_AUDIO_DISABLED /**< Do not admit receiver audio. */,
	URP_RX_AUDIO_SPEAKER /**< Speaker audio with radio-supplied deemphasis. */,
	URP_RX_AUDIO_FLAT /**< Flat discriminator audio requiring deemphasis. */
};

/** Transport-independent transmit output routing values. */
/** Hardware voice/transmit-signaling output assignments.
 *
 * The \c ctcss route names carry either selected transmit signaling method:
 * CTCSS or DCS.
 */
enum urp_tx_output_mode {
	URP_TX_OUTPUT_DISABLED /**< Silence on this DAC output. */,
	URP_TX_OUTPUT_VOICE /**< Processed voice without transmit signaling. */,
	URP_TX_OUTPUT_TONE /**< Transmit signaling without voice. */,
	URP_TX_OUTPUT_COMPOSITE /**< Processed voice mixed with transmit signaling. */,
	URP_TX_OUTPUT_AUX_VOICE /**< Auxiliary voice routing. */
};

/** Transport-independent carrier-detection source values. */
/** Carrier detector source and input polarity. */
enum urp_carrier_source {
	URP_CARRIER_DISABLED /**< No carrier indication. */,
	URP_CARRIER_DSP /**< Native discriminator-noise squelch. */,
	URP_CARRIER_VOX /**< Audio-level carrier detection. */,
	URP_CARRIER_USB /**< USB GPIO carrier indication. */,
	URP_CARRIER_USB_INVERTED /**< Inverted USB GPIO carrier indication. */,
	URP_CARRIER_PARALLEL /**< Parallel-port carrier indication. */,
	URP_CARRIER_PARALLEL_INVERTED /**< Inverted parallel-port carrier indication. */
};

/** Transport-independent CTCSS-detection source values. */
/** CTCSS indication source and input polarity. */
enum urp_ctcss_source {
	URP_CTCSS_DISABLED /**< Do not require a CTCSS indication. */,
	URP_CTCSS_USB /**< USB GPIO CTCSS-decode indication. */,
	URP_CTCSS_USB_INVERTED /**< Inverted USB GPIO CTCSS-decode indication. */,
	URP_CTCSS_DSP /**< Native CTCSS decoder. */,
	URP_CTCSS_PARALLEL /**< Parallel-port CTCSS-decode indication. */,
	URP_CTCSS_PARALLEL_INVERTED /**< Inverted parallel-port CTCSS-decode indication. */
};

/** Transport-independent transmitter CTCSS turn-off behavior. */
/** Transmitter CTCSS turn-off sequences. */
enum urp_tone_off_mode {
	URP_TONE_OFF_NONE /**< End CTCSS with PTT release. */,
	URP_TONE_OFF_PHASE_SHIFT /**< Shift CTCSS phase before PTT release. */,
	URP_TONE_OFF_TONE_REMOVE /**< Remove CTCSS before PTT release. */,
	URP_TONE_OFF_TAIL_TONE /**< Substitute a low-frequency tail tone before PTT release. */
};

/**
 * Lock-free single-producer/single-consumer PCM queue for legacy echo audio.
 *
 * Its producer and consumer are fixed for each use. App_rpt program audio uses
 * the shared rate-adjusting PCM ring; this fixed-storage queue is retained for
 * the hardware worker's legacy echo source. The cursors
 * deliberately are not reduced modulo the ring capacity; modulo arithmetic is
 * used only for indexing.  That lets each side determine occupancy without a
 * mutex and publish samples with acquire/release ordering.
 */
struct urp_sample_queue {
	/** Caller-owned PCM storage. */
	short *samples;
	/** Number of samples available in the caller-owned storage. */
	unsigned int capacity;
	/** Next sample cursor owned by the consumer. */
	atomic_uint read;
	/** Next sample cursor owned by the producer. */
	atomic_uint write;
	/** Largest observed occupancy, in samples. */
	atomic_uint high_water;
};

/** Buffer and playback cursor for native-rate echo audio. */
struct urp_parrot_state {
	/** Owned floating-point native echo recording. */
	double *audio;
	/** Allocated recording capacity in samples. */
	size_t capacity;
	/** Number of occupied elements. */
	size_t count;
	/** Next playback sample offset. */
	size_t play;
	/** Nonzero while recorded audio is being transmitted. */
	unsigned int playing : 1;
	/** Nonzero when the recording exceeded its configured duration. */
	unsigned int truncated : 1;
};

/** Measurements collected while preparing one native receiver block. */
struct urp_receive_block_stats {
	/** Largest observed absolute sample magnitude. */
	unsigned int peak;
	/** Count of samples at a signed 16-bit PCM rail. */
	unsigned long rail_samples;
};

/** @brief Extract the left CM119 channel, collect ADC measurements, apply the optional squelch-tail
 * delay, and create the floating-point receiver working block.
 * @param stereo Interleaved signed 16-bit stereo samples.
 * @param pcm Receives the delayed signed 16-bit receiver block.
 * @param working Receives the floating-point receiver working block.
 * @param count Number of elements available in the supplied block.
 * @param delay Receiver squelch-tail delay ring.
 * @param delay_samples Delay-ring length in samples.
 * @param delay_index Delay-ring cursor, updated in place.
 * @param stats Receives raw ADC peak and rail counts.
 */
void urp_prepare_receive_block(const short *stereo, short *pcm, double *working, size_t count,
			       short *delay, size_t delay_samples, unsigned int *delay_index,
			       struct urp_receive_block_stats *stats);

/** @brief Quantize and route one native transmitter block to the CM119 channels.
 * @param program Processed transmitter program audio.
 * @param ctcss Unit-amplitude native CTCSS samples.
 * @param dcs Native DCS samples already scaled in PCM codes.
 * @param count Number of elements available in the supplied block.
 * @param output_a Output-A routing assignment.
 * @param output_b Output-B routing assignment.
 * @param ctcss_peak_a CTCSS amplitude in PCM codes for output A.
 * @param ctcss_bias_a CTCSS calibration bias in PCM codes for output A.
 * @param ctcss_peak_b CTCSS amplitude in PCM codes for output B.
 * @param ctcss_bias_b CTCSS calibration bias in PCM codes for output B.
 * @param stereo Interleaved signed 16-bit stereo samples.
 * @param meter_stereo Optional unrouted program-audio buffer for transmitter metering.
 * @return Number of program samples outside signed 16-bit PCM range.
 */
unsigned long urp_render_transmit_block(const double *program, const double *ctcss,
					const double *dcs, size_t count,
					enum urp_tx_output_mode output_a,
					enum urp_tx_output_mode output_b, double ctcss_peak_a,
					double ctcss_bias_a, double ctcss_peak_b,
					double ctcss_bias_b, short *stereo, short *meter_stereo);

/** @brief Initialize an SPSC sample ring before either endpoint uses it.
 * @param queue Ring to initialize.
 * @param samples Caller-owned PCM storage.
 * @param capacity Number of PCM samples in the supplied storage.
 */
void urp_sample_queue_init(struct urp_sample_queue *queue, short *samples, unsigned int capacity);

/** @brief Empty an inactive sample queue without changing its storage.
 * @param queue Queue to empty.
 */
void urp_sample_queue_reset(struct urp_sample_queue *queue);

/** @brief Discard samples already published to an active SPSC queue.
 * @param queue Queue whose consumer owns this operation.
 *
 * This advances only the consumer cursor to an acquire-loaded producer cursor.
 * The producer cursor and its high-water measurement remain monotonic, so a
 * producer that publishes concurrently either contributes to the discarded
 * prefix or remains available after the new read cursor.  Unlike
 * @ref urp_sample_queue_reset, this operation is safe while the producer is
 * active.
 */
void urp_sample_queue_discard(struct urp_sample_queue *queue);

/** @brief Add one PCM sample to an SPSC queue.
 * @param queue Queue whose producer owns this operation.
 * @param sample PCM value to append.
 * @return Nonzero on success; zero when the queue is full.
 */
int urp_sample_queue_push_sample(struct urp_sample_queue *queue, short sample);

/** @brief Remove one PCM sample from an SPSC queue.
 * @param queue Queue whose consumer owns this operation.
 * @param sample Receives the removed PCM value on success.
 * @return Nonzero on success; zero when the queue is empty.
 */
int urp_sample_queue_pop_sample(struct urp_sample_queue *queue, short *sample);

/** @brief Return the number of queued PCM samples.
 * @param queue Queue to inspect.
 * @return Current sample occupancy.
 */
unsigned int urp_sample_queue_samples(const struct urp_sample_queue *queue);

/** @brief Return the largest observed sample occupancy.
 * @param queue Queue to inspect.
 * @return Peak occupancy since initialization or last reset.
 */
unsigned int urp_sample_queue_high_water(const struct urp_sample_queue *queue);

/** @brief Reset the sample occupancy high-water mark.
 * @param queue Queue whose peak measurement is reset.
 */
void urp_sample_queue_reset_high_water(struct urp_sample_queue *queue);

/** @brief Convert a dB hardware gain around the 500 midpoint to the 0 through 999 mixer scale.
 * @param gain_db Gain in dB.
 * @return Mixer setting clamped to 0 through 999.
 */
int urp_gain_db_to_mixer(double gain_db);

/** @brief Convert a 0 through 999 hardware mixer setting to dB relative to its 500 midpoint.
 * @param setting Normalized CM119 mixer setting from 0 through 999.
 * @return Gain in dB relative to mixer setting 500.
 */
double urp_mixer_to_gain_db(int setting);

/** @brief Calculate the CM119 fixed-point fine-gain multiplier.
 * @param value Normalized hardware mixer level from 0 through 999.
 * @return Fine-gain multiplier with eight fractional bits.
 */
int urp_hardware_level_multiplier(int value);

/** @brief Add two PCM samples without signed overflow.
 * @param left First signed 16-bit PCM sample.
 * @param right Second signed 16-bit PCM sample.
 * @return Sum clamped to signed 16-bit PCM.
 */
short urp_saturating_add(short left, short right);

/** @brief Apply linear gain and saturate to the signed 16-bit PCM range.
 * @param sample One sample in signed PCM amplitude units.
 * @param linear Linear amplitude multiplier.
 * @return Scaled sample clamped to signed 16-bit PCM.
 */
short urp_apply_gain(short sample, double linear);

/** @brief Return the absolute peak of signed 16-bit PCM, including 32768 for INT16_MIN.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @return Absolute PCM peak, including 32768 for INT16_MIN.
 */
unsigned int urp_pcm_peak(const short *samples, size_t count);

/** @brief Convert an absolute PCM peak to dBFS; zero becomes negative infinity.
 * @param peak Absolute sample peak in PCM codes.
 * @return Peak level in dBFS, or negative infinity for silence.
 */
double urp_pcm_peak_dbfs(unsigned int peak);

/** @brief Return the largest absolute value in a floating-point audio buffer.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param count Number of elements available in the supplied block.
 * @return Maximum absolute sample magnitude.
 */
double urp_double_peak(const double *samples, size_t count);

/** @brief Return nonzero when a transmitter assignment contains program audio.
 * @param mode Configured routing, detection, or hardware-open mode.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_output_has_program(enum urp_tx_output_mode mode);

/** @brief Return nonzero when an output assignment carries transmitter voice audio.
 * @param mode Configured routing, detection, or hardware-open mode.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_output_has_voice(enum urp_tx_output_mode mode);

/** @brief Return nonzero when an output assignment carries transmitter signaling.
 * @param mode Configured routing, detection, or hardware-open mode.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_output_has_tone(enum urp_tx_output_mode mode);

/** @brief Return nonzero when either output assignment carries transmitter voice audio.
 * @param output_a Output-A routing assignment.
 * @param output_b Output-B routing assignment.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_pair_has_voice(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b);

/** @brief Return nonzero when either output assignment carries transmitter signaling.
 * @param output_a Output-A routing assignment.
 * @param output_b Output-B routing assignment.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_pair_has_tone(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b);

/** @brief Return nonzero when configured transmitter signaling has no assigned output.
 * @param signaling_enabled Nonzero when CTCSS or DCS transmit signaling is selected.
 * @param output_a Output-A routing assignment.
 * @param output_b Output-B routing assignment.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_tx_signaling_route_missing(int signaling_enabled, enum urp_tx_output_mode output_a,
				   enum urp_tx_output_mode output_b);

/** @brief Return nonzero when configured parallel outputs require the pulse worker.
 * @param parallel_port_enabled Nonzero when parallel-port hardware is available.
 * @param output_configured Nonzero when a parallel-port output is assigned.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_parallel_pulser_needed(int parallel_port_enabled, int output_configured);

/** @brief Return nonzero when duplex-3 uses the native software repeat path.
 * @param duplex3_level Local-repeat level from 0 through 999.
 * @param software_mode Nonzero selects the native software repeat route.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int urp_native_echo_enabled(int duplex3_level, int software_mode);

/** @brief Apply logical PTT and polarity to the USB and optional parallel outputs.
 * @param asserted Logical PTT request before polarity inversion.
 * @param inverted Nonzero inverts the physical PTT polarity.
 * @param parallel_mask Parallel-port PTT bit mask.
 * @param usb_mask USB GPIO PTT bit mask.
 * @param usb_value USB GPIO byte updated in place.
 * @param parallel_value Parallel-port byte updated in place.
 */
void urp_apply_ptt_outputs(int asserted, int inverted, int parallel_mask, int usb_mask,
			   int32_t *usb_value, int8_t *parallel_value);

/** @brief Update echo state across an RX carrier transition; returns one when playback starts.
 * @param state Processor or stream state owned by the caller.
 * @param was_keyed Receiver keyed state before this block.
 * @param is_keyed Receiver keyed state for the current block.
 * @return One when playback starts; zero otherwise.
 */
int urp_parrot_rx_transition(struct urp_parrot_state *state, int was_keyed, int is_keyed);

/** @brief Copy the next playback block and return its sample count.
 * @param state Processor or stream state owned by the caller.
 * @param output Destination sample buffer owned by the caller.
 * @param count Number of elements available in the supplied block.
 * @return Number of playback samples copied.
 */
size_t urp_parrot_play(struct urp_parrot_state *state, double *output, size_t count);

/** @brief Append a recording block within a configured sample limit.
 * @param state Processor or stream state owned by the caller.
 * @param input Input samples; the caller retains ownership.
 * @param count Number of elements available in the supplied block.
 * @param limit Maximum recording length in samples.
 * @return Number of samples appended to the recording.
 */
size_t urp_parrot_record(struct urp_parrot_state *state, const double *input, size_t count,
			 size_t limit);

/** @brief Parse a receive audio source.
 * @param text Text to parse; mutable storage may be edited in place.
 * @param mode Receives the parsed assignment.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_parse_rx_audio_mode(const char *text, enum urp_rx_audio_mode *mode);

/** @brief Parse a carrier-detection source.
 * @param text Text to parse; mutable storage may be edited in place.
 * @param source Receives the parsed detector-source assignment.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_parse_carrier_source(const char *text, enum urp_carrier_source *source);

/** @brief Parse a CTCSS-detection source, including accepted symbolic aliases.
 * @param text Text to parse; mutable storage may be edited in place.
 * @param source Receives the parsed detector-source assignment.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_parse_ctcss_source(const char *text, enum urp_ctcss_source *source);

/** @brief Parse a transmitter CTCSS turn-off mode, including symbolic aliases.
 * @param text Text to parse; mutable storage may be edited in place.
 * @param mode Receives the parsed assignment.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int urp_parse_tone_off_mode(const char *text, enum urp_tone_off_mode *mode);

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def URP_PROGRAM_RING_FRAMES
 * @brief Maximum retained app_rpt program-audio history at the native rate.
 */
/** @} */
