/**
 * @file
 * @brief USBRadioPlus direct PortAudio callback and Asterisk delivery API.
 *
 * Both Asterisk interfaces share one PortAudio/ALSA and CM119 GPIO composition.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_H
#define USBRADIOPLUS_PORTAUDIO_POC_H

#include <stddef.h>
#include <stdint.h>

#include "usbradioplus_channel_core.h"

struct chan_usbradio_pvt;

/** Opaque direct PortAudio stream supplied by the released audio adapter. */
struct rptadv_audio_stream;

/** Bounded callback-to-Asterisk handoff depth. */
enum { URP_PORTAUDIO_POC_RX_BLOCK_COUNT = 8U };

/** One complete app-facing receive block published by the PortAudio callback. */
struct usbradioplus_portaudio_poc_rx_block {
	/** Mono signed PCM delivered to the Asterisk compatibility boundary. */
	short pcm[URP_NATIVE_MAX_SAMPLES];
	/** Number of valid mono PCM frames in \c pcm. */
	unsigned int frame_count;
	/** Receiver qualification state for this block. */
	int keyed;
	/** Decoded CTCSS value associated with a key transition. */
	char ctcss_frequency[32];
	/** Completed callback status events that must precede this block's voice frame. */
	uint64_t status_event_limit;
};

/** Number of 8 kHz samples in one ordinary legacy app_rpt handoff. */
enum { URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES = 160U };

/**
 * @brief Bounded callback-private assembler for ordinary 8 kHz receive PCM.
 *
 * A direct native callback can end between 8 kHz sample boundaries.  The
 * renderer reports only samples actually produced by its streaming converter;
 * this FIFO retains those samples until the established 20 ms legacy handoff
 * can be emitted.  Two whole blocks cover the largest legal carried-phase
 * conversion result without allocation or waiting.
 */
enum { URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES = 2U * URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES };

/** Callback-private preallocated ordinary legacy receive assembler state. */
struct usbradioplus_portaudio_poc_receive_assembler {
	/** Circular FIFO containing actual 8 kHz converter output. */
	short fifo[URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES];
	/** Index of the oldest pending sample. */
	size_t read_index;
	/** Number of pending samples in \c fifo. */
	size_t sample_count;
};

/** @brief Reset a quiesced or newly created ordinary receive assembler.
 * @param assembler Caller-owned assembler, or NULL for no operation.
 */
static inline void usbradioplus_portaudio_poc_receive_assembler_reset(
	struct usbradioplus_portaudio_poc_receive_assembler *assembler)
{
	if (!assembler)
		return;
	assembler->read_index = 0U;
	assembler->sample_count = 0U;
}

/**
 * @brief Append actual converted receive samples without padding.
 * @param assembler Callback-owned FIFO receiving the samples.
 * @param samples Converted mono PCM; may be NULL only for an empty span.
 * @param sample_count Number of converted samples to append.
 * @return Zero on success or nonzero when a caller violated the bounded FIFO contract.
 */
static inline int usbradioplus_portaudio_poc_receive_assembler_append(
	struct usbradioplus_portaudio_poc_receive_assembler *assembler, const short *samples,
	size_t sample_count)
{
	size_t index;
	size_t write_index;

	if (!assembler || assembler->sample_count > URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES ||
	    (!samples && sample_count) ||
	    sample_count > URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES - assembler->sample_count)
		return -1;
	write_index = (assembler->read_index + assembler->sample_count) %
		      URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES;
	for (index = 0U; index < sample_count; ++index) {
		assembler->fifo[write_index] = samples[index];
		write_index = (write_index + 1U) % URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES;
	}
	assembler->sample_count += sample_count;
	return 0;
}

/**
 * @brief Remove up to a caller-selected number of ordered pending samples.
 * @param assembler Callback-owned FIFO supplying the samples.
 * @param output Destination with room for at least maximum_count samples.
 * @param maximum_count Maximum number of samples to remove.
 * @return Number of samples copied to @p output.
 *
 * A 20 ms boundary may arrive before the SRC has emitted every matching 8 kHz
 * sample during startup.  Draining that partial interval before zero-padding
 * its tail prevents those samples from shifting the next complete interval.
 */
static inline size_t usbradioplus_portaudio_poc_receive_assembler_take(
	struct usbradioplus_portaudio_poc_receive_assembler *assembler, short *output,
	size_t maximum_count)
{
	size_t count;
	size_t index;

	if (!assembler || !output ||
	    assembler->sample_count > URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES)
		return 0U;
	count = assembler->sample_count < maximum_count ? assembler->sample_count : maximum_count;
	for (index = 0U; index < count; ++index) {
		output[index] = assembler->fifo[assembler->read_index];
		assembler->read_index =
			(assembler->read_index + 1U) % URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES;
	}
	assembler->sample_count -= count;
	return count;
}

/**
 * @brief Pop one complete ordinary 20 ms receive handoff.
 * @param assembler Callback-owned FIFO supplying the samples.
 * @param output Destination with room for one 160-sample handoff.
 * @return Nonzero when @p output received exactly 160 ordered samples.
 */
static inline int usbradioplus_portaudio_poc_receive_assembler_pop(
	struct usbradioplus_portaudio_poc_receive_assembler *assembler, short *output)
{
	if (!assembler || !output ||
	    assembler->sample_count < URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES)
		return 0;
	return usbradioplus_portaudio_poc_receive_assembler_take(
		       assembler, output, URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES) ==
	       URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES;
}

/**
 * @brief Start the direct PortAudio stream and its non-real-time Asterisk bridge.
 * @param channel Channel whose radio and HID state are already initialized.
 * @return Zero on success; nonzero when the stream or bridge cannot start.
 *
 * Requires the prepared hardware facade to prove one common audio, HID, and
 * mixer identity before either controller-facing interface opens its stream.
 */
int usbradioplus_portaudio_poc_start(struct chan_usbradio_pvt *channel);

/**
 * @brief Stop the direct callback before its radio or channel state is retired.
 * @param channel Channel owning the direct stream.
 */
void usbradioplus_portaudio_poc_stop(struct chan_usbradio_pvt *channel);

#ifdef URP_PROCESSING_TESTING
/** @brief Inject one competing producer generation change after the next consumer copy. */
void usbradioplus_portaudio_poc_test_invalidate_next_claim(void);
/** @brief Capture status from valid callback-owned channel/radio state without processing PCM.
 * @param channel Valid callback-owned channel with an initialized radio.
 * @param frame_count Native sample span elapsed since the last status capture.
 */
void usbradioplus_portaudio_poc_test_capture_status(struct chan_usbradio_pvt *channel,
						    size_t frame_count);
/**
 * @brief Run one hardware-free direct PortAudio callback for deterministic tests.
 * @param channel Prepared adapter channel state.
 * @param input Canonical stereo float capture PCM, or NULL for silence.
 * @param output Canonical stereo float DAC PCM destination.
 * @param frame_count Native callback size.
 * @return Zero after the complete callback span is processed.
 */
int32_t usbradioplus_portaudio_poc_test_callback(struct chan_usbradio_pvt *channel,
						 const float *input, float *output,
						 uint32_t frame_count);
/**
 * @brief Deliver at most one pending handoff through the real Asterisk bridge.
 * @param channel Channel that owns the handoff.
 * @param delivered_keyed Caller-owned last delivered key state.
 * @param seen_generation Caller-owned receive queue generation.
 * @param next_status_sequence Caller-owned next status event sequence.
 * @return One when consumed, zero when empty, or negative on invalid input.
 */
int usbradioplus_portaudio_poc_test_deliver(struct chan_usbradio_pvt *channel, int *delivered_keyed,
					    unsigned int *seen_generation,
					    uint64_t *next_status_sequence);
#endif

#endif
