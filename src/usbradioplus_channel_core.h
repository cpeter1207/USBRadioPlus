#ifndef USBRADIOPLUS_CHANNEL_CORE_H
#define USBRADIOPLUS_CHANNEL_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "usbradioplus_dsp.h"

#define URP_PROGRAM_QUEUE_FRAMES 8U
#define URP_NATIVE_FIFO_SAMPLES (URP_NATIVE_SAMPLES * 8U)

/** Transport-independent receive audio source values. */
enum urp_rx_audio_mode {
	URP_RX_AUDIO_DISABLED,
	URP_RX_AUDIO_SPEAKER,
	URP_RX_AUDIO_FLAT,
};

/** Transport-independent transmit output routing values. */
enum urp_tx_output_mode {
	URP_TX_OUTPUT_DISABLED,
	URP_TX_OUTPUT_VOICE,
	URP_TX_OUTPUT_TONE,
	URP_TX_OUTPUT_COMPOSITE,
	URP_TX_OUTPUT_AUX_VOICE,
};

/** Transport-independent carrier-detection source values. */
enum urp_carrier_source {
	URP_CARRIER_DISABLED,
	URP_CARRIER_DSP,
	URP_CARRIER_VOX,
	URP_CARRIER_USB,
	URP_CARRIER_USB_INVERTED,
	URP_CARRIER_PARALLEL,
	URP_CARRIER_PARALLEL_INVERTED,
};

/** Transport-independent CTCSS-detection source values. */
enum urp_ctcss_source {
	URP_CTCSS_DISABLED,
	URP_CTCSS_USB,
	URP_CTCSS_USB_INVERTED,
	URP_CTCSS_DSP,
	URP_CTCSS_PARALLEL,
	URP_CTCSS_PARALLEL_INVERTED,
};

/** Transport-independent transmitter CTCSS turn-off behavior. */
enum urp_tone_off_mode {
	URP_TONE_OFF_NONE,
	URP_TONE_OFF_PHASE_REVERSE,
	URP_TONE_OFF_REMOVE,
};

/** Transport-neutral app_rpt frame queue shared by both channel adapters. */
struct urp_program_queue {
	short frames[URP_PROGRAM_QUEUE_FRAMES][URP_NATIVE_SAMPLES];
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	unsigned int high_water;
};

/** Native-rate elastic FIFO shared by the OSS and PortAudio adapters. */
struct urp_native_fifo {
	short samples[URP_NATIVE_FIFO_SAMPLES];
	unsigned int head;
	unsigned int count;
	unsigned int primed : 1;
};

/** Buffer and playback cursor for native-rate echo audio. */
struct urp_parrot_state {
	double *audio;
	size_t capacity;
	size_t count;
	size_t play;
	unsigned int playing : 1;
	unsigned int truncated : 1;
};

/** Measurements collected while preparing one native receiver block. */
struct urp_receive_block_stats {
	unsigned int peak;
	unsigned long rail_samples;
};

/**
 * Extract the left CM119 channel, collect ADC measurements, apply the optional
 * squelch-tail delay, and create the floating-point receiver working block.
 */
void urp_prepare_receive_block(const short *stereo, short *pcm, double *working, size_t count,
			       short *delay, size_t delay_samples, unsigned int *delay_index,
			       struct urp_receive_block_stats *stats);

/**
 * Quantize and route one native transmitter block to the CM119 channels.
 * The optional meter buffer receives program audio before hardware routing.
 *
 * \return Number of program samples that exceeded signed 16-bit PCM.
 */
unsigned long urp_render_transmit_block(const double *program, const double *ctcss, size_t count,
					enum urp_tx_output_mode output_a,
					enum urp_tx_output_mode output_b, double ctcss_peak_a,
					double ctcss_bias_a, double ctcss_peak_b,
					double ctcss_bias_b, short *stereo, short *meter_stereo);

/** Queue one app_rpt frame, optionally prefixing silence for clock-recovery startup. */
int urp_program_queue_push(struct urp_program_queue *queue, const short *samples, size_t count,
			   size_t frame_samples, unsigned int seed_frames);

/** Remove one queued frame. Returns zero when the queue is empty. */
int urp_program_queue_pop(struct urp_program_queue *queue, short *samples);

/** Return nonzero when at least one app_rpt frame is queued. */
int urp_program_queue_pending(const struct urp_program_queue *queue);

/** Append samples, retaining the newest audio if the FIFO is full. */
size_t urp_native_fifo_push(struct urp_native_fifo *fifo, const short *samples, size_t count);

/** Remove one 10-ms native-rate frame. Returns zero until a full frame exists. */
int urp_native_fifo_pop(struct urp_native_fifo *fifo, short *samples);

/** Empty and de-prime the FIFO after clock-recovery loss. */
void urp_native_fifo_reset(struct urp_native_fifo *fifo);

/** Convert a dB hardware gain around the 500 midpoint to the 0--999 mixer scale. */
int urp_gain_db_to_mixer(double gain_db);

/** Convert a 0--999 hardware mixer setting to dB relative to its 500 midpoint. */
double urp_mixer_to_gain_db(int setting);

/** Calculate the CM119 fixed-point fine-gain multiplier. */
int urp_hardware_level_multiplier(int value);

/** Add two PCM samples without signed overflow. */
short urp_saturating_add(short left, short right);

/** Apply linear gain and saturate to the signed 16-bit PCM range. */
short urp_apply_gain(short sample, double linear);

/** Return the absolute peak of signed 16-bit PCM, including 32768 for INT16_MIN. */
unsigned int urp_pcm_peak(const short *samples, size_t count);

/** Convert an absolute PCM peak to dBFS; zero becomes negative infinity. */
double urp_pcm_peak_dbfs(unsigned int peak);

/** Return the largest absolute value in a floating-point audio buffer. */
double urp_double_peak(const double *samples, size_t count);

/** Return nonzero when a transmitter assignment contains program audio. */
int urp_tx_output_has_program(enum urp_tx_output_mode mode);

/** Return nonzero when an output assignment carries transmitter voice audio. */
int urp_tx_output_has_voice(enum urp_tx_output_mode mode);

/** Return nonzero when an output assignment carries transmitter CTCSS. */
int urp_tx_output_has_tone(enum urp_tx_output_mode mode);

/** Return nonzero when either output assignment carries transmitter voice audio. */
int urp_tx_pair_has_voice(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b);

/** Return nonzero when either output assignment carries transmitter CTCSS. */
int urp_tx_pair_has_tone(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b);

/** Return nonzero when configured transmitter CTCSS has no assigned output. */
int urp_tx_tone_route_missing(const char *frequency, enum urp_tx_output_mode output_a,
			      enum urp_tx_output_mode output_b);

/** Return nonzero when configured parallel outputs require the pulse worker. */
int urp_parallel_pulser_needed(int parallel_port_enabled, int output_configured);

/** Return nonzero when duplex-3 uses the native software repeat path. */
int urp_native_echo_enabled(int duplex3_level, int software_mode);

/** Apply logical PTT and polarity to the USB and optional parallel outputs. */
void urp_apply_ptt_outputs(int asserted, int inverted, int parallel_mask, int usb_mask,
			   int32_t *usb_value, int8_t *parallel_value);

/** Update echo state across an RX carrier transition; returns one when playback starts. */
int urp_parrot_rx_transition(struct urp_parrot_state *state, int was_keyed, int is_keyed);

/** Copy the next playback block and return its sample count. */
size_t urp_parrot_play(struct urp_parrot_state *state, double *output, size_t count);

/** Append a recording block within a configured sample limit. */
size_t urp_parrot_record(struct urp_parrot_state *state, const double *input, size_t count,
			 size_t limit);

/** Parse a receive audio source. Returns zero on success. */
int urp_parse_rx_audio_mode(const char *text, enum urp_rx_audio_mode *mode);

/** Parse a transmitter output assignment. Returns zero on success. */
int urp_parse_tx_output_mode(const char *text, enum urp_tx_output_mode *mode);

/** Parse a carrier-detection source. Returns zero on success. */
int urp_parse_carrier_source(const char *text, enum urp_carrier_source *source);

/** Parse a CTCSS-detection source, including accepted symbolic aliases. */
int urp_parse_ctcss_source(const char *text, enum urp_ctcss_source *source);

/** Parse a transmitter CTCSS turn-off mode, including symbolic aliases. */
int urp_parse_tone_off_mode(const char *text, enum urp_tone_off_mode *mode);

#endif
