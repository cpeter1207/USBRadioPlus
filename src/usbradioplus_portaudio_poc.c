/**
 * @file
 * @brief USBRadioPlus direct PortAudio POC.
 *
 * Direct PortAudio proof-of-concept bridge for ASL adapters.
 *
 * PortAudio owns the real-time callback. The callback submits canonical F32
 * PCM to the native-tick boundary, advances one exact native sample span, and
 * publishes the matching receive span to a fixed single-producer/single-
 * consumer ring. A separate non-real-time worker performs the required
 * Asterisk frame work.
 */

#ifdef URP_HAVE_PORTAUDIO_POC

#include "asterisk.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#include "asterisk/channel.h"
#include "asterisk/format_cache.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/dsp.h"
#include "asterisk/options.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"

#include "usbradioplus_ctcss.h"
#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"
#include "usbradioplus_host_util.h"
#include "usbradioplus_portaudio_poc.h"
#include "usbradioplus_portaudio_poc_handoff.h"
#include "usbradioplus_portaudio_poc_selection.h"
#include "usbradioplus_portaudio_poc_status.h"
#include "usbradioplus_portaudio_poc_timing.h"
#ifdef URP_HAVE_GPIO_POC
#include "usbradioplus_hardware_adapter.h"
#include "usbradioplus_portaudio_poc_identity.h"
#endif

/** @brief End byte needed to safely read one non-optional stream descriptor member. */
#define URP_PORTAUDIO_POC_DESCRIPTOR_MEMBER_END(member)                                            \
	(offsetof(struct rptadv_audio_adapter_descriptor, member) +                                \
	 sizeof(((struct rptadv_audio_adapter_descriptor *)0)->member))

/** @brief Descriptor prefix required by every direct PortAudio POC stream. */
#define URP_PORTAUDIO_POC_STREAM_DESCRIPTOR_SIZE                                                   \
	URP_PORTAUDIO_POC_DESCRIPTOR_MEMBER_END(stream_destroy)

#ifdef URP_PROCESSING_TESTING
/** @brief One-shot competing producer publication injected after a consumer copy. */
static atomic_int portaudio_poc_test_invalidate_claim;

void usbradioplus_portaudio_poc_test_invalidate_next_claim(void)
{
	atomic_store(&portaudio_poc_test_invalidate_claim, 1);
}
#endif

/** @brief Return whether an adapter supplies the stream ABI used by this POC. */
static int portaudio_poc_stream_adapter_valid(const struct rptadv_audio_adapter_descriptor *adapter)
{
	return adapter && adapter->struct_size >= URP_PORTAUDIO_POC_STREAM_DESCRIPTOR_SIZE &&
	       adapter->abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION && adapter->stream_create &&
	       adapter->stream_start && adapter->stream_stop && adapter->stream_destroy;
}

/** @brief Report immutable PortAudio timing after successful POC stream startup. */
static void portaudio_poc_log_stream_timing(struct chan_usbradio_pvt *channel,
					    const struct rptadv_audio_adapter_descriptor *adapter)
{
	struct rptadv_audio_stream_timing timing = {
		.struct_size = sizeof(timing),
	};

#ifdef URP_HAVE_GPIO_POC
	int timing_result;

	/* Startup has already required the prepared combined hardware facade. */
	(void)adapter;
	timing_result = usbradioplus_hardware_adapter_stream_get_timing(
				&channel->plus_hardware_adapter, channel->plus_portaudio_stream,
				&timing) == USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	if (!timing_result)
		return;
#else
	if (usbradioplus_portaudio_poc_get_stream_timing(adapter, channel->plus_portaudio_stream,
							 &timing) != RPTADV_AUDIO_OK)
		return;
#endif
	ast_log(LOG_NOTICE,
		"RadioPlus/%s: direct PortAudio proof-of-concept timing: %.3f Hz, input %.3f ms, "
		"output %.3f ms\n",
		channel->name, timing.sample_rate_hz, timing.input_latency_seconds * 1000.0,
		timing.output_latency_seconds * 1000.0);
}

/**
 * @brief Latch legacy CTCSS-ready and voter text before the matching PCM block.
 * @param channel Callback-owned legacy state protected by successful radio access.
 *
 * Status is retained in its own preallocated SPSC queue, rather than inside
 * the bounded PCM queue.  Therefore an audio-overload resynchronization can
 * discard stale voice blocks without losing the required final `cstx=` event
 * or the next voter report.
 */
static void portaudio_poc_capture_status(struct chan_usbradio_pvt *channel, size_t frame_count)
{
	int voter_due = 0;

	if (channel->plus_portaudio_callback_keyed != channel->rxkeyed) {
		channel->plus_portaudio_callback_keyed = channel->rxkeyed;
		/* The legacy worker reports once at the first complete 20 ms keyed
		 * block, then every ten blocks. Report at this first callback boundary
		 * and retain its 200 ms cadence in native sample time thereafter. */
		channel->plus_portaudio_voter_remaining_frames = 0U;
		voter_due = channel->rxkeyed;
	}
	if (channel->radio->b.txCtcssReady) {
		if (usbradioplus_portaudio_poc_status_publish_ctcss(
			    &channel->plus_portaudio_status_handoff, channel->radio->txctcssfreq) ==
		    USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY) {
			channel->radio->b.txCtcssReady = 0;
		}
	}
	if (channel->sendvoter && channel->rxkeyed && !voter_due) {
		if (channel->plus_portaudio_voter_remaining_frames > frame_count) {
			channel->plus_portaudio_voter_remaining_frames -= frame_count;
		} else {
			channel->plus_portaudio_voter_remaining_frames = 0U;
			voter_due = 1;
		}
	}
	if (channel->sendvoter && channel->rxkeyed && voter_due) {
		const int rssi = ((32767 - channel->radio->rxRssi) * 1000) / 32767;

		if (usbradioplus_portaudio_poc_status_publish_voter(
			    &channel->plus_portaudio_status_handoff, rssi) ==
		    USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY) {
			channel->plus_portaudio_voter_remaining_frames = 10U * URP_NATIVE_SAMPLES;
		} else {
			/* Retry at the next callback when a bounded status queue is full. */
			channel->plus_portaudio_voter_remaining_frames = 0U;
		}
	}
}

#ifdef URP_PROCESSING_TESTING
void usbradioplus_portaudio_poc_test_capture_status(struct chan_usbradio_pvt *channel,
						    size_t frame_count)
{
	portaudio_poc_capture_status(channel, frame_count);
}
#endif

/** @brief Publish a caller-validated nonempty bounded receive span without waiting. */
static void portaudio_poc_publish_receive(struct chan_usbradio_pvt *channel, const short *pcm,
					  unsigned int frame_count)
{
	struct usbradioplus_portaudio_poc_rx_block *block;
	unsigned int slot;

	if (usbradioplus_portaudio_poc_handoff_producer_reserve(
		    &channel->plus_portaudio_rx_handoff, URP_PORTAUDIO_POC_RX_BLOCK_COUNT, &slot) !=
	    USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY)
		return;
	block = &channel->plus_portaudio_rx_blocks[slot];
	block->frame_count = frame_count;
	block->keyed = channel->rxkeyed;
	if (block->keyed) {
		memcpy(block->pcm, pcm, frame_count * sizeof(*block->pcm));
	} else {
		memset(block->pcm, 0, frame_count * sizeof(*block->pcm));
	}
	memcpy(block->ctcss_frequency, channel->rxctcssfreq, sizeof(block->ctcss_frequency));
	block->ctcss_frequency[sizeof(block->ctcss_frequency) - 1U] = '\0';
	block->status_event_limit = usbradioplus_portaudio_poc_status_published(
		&channel->plus_portaudio_status_handoff);
	usbradioplus_portaudio_poc_handoff_producer_publish(&channel->plus_portaudio_rx_handoff,
							    URP_PORTAUDIO_POC_RX_BLOCK_COUNT);
}

/**
 * @brief Release one ordinary app_rpt frame at a native 20 ms boundary.
 *
 * The renderer supplies only actual streaming-SRC output.  Retaining it here
 * prevents callback partitioning from adding padded samples; only a true SRC
 * startup deficit at the established 20 ms boundary is zero-filled.
 */
static void portaudio_poc_publish_legacy_receive(struct chan_usbradio_pvt *channel,
						 const short *pcm, size_t app_sample_count,
						 size_t receive_remainder_before,
						 size_t native_frame_count)
{
	struct usbradioplus_portaudio_poc_receive_assembler *assembler;
	short *handoff_pcm;
	size_t copied;

	/* The callback supplies its owned channel buffer and bounded SRC result. */
	assembler = &channel->plus_portaudio_legacy_rx_assembler;
	if (usbradioplus_portaudio_poc_receive_assembler_append(assembler, pcm, app_sample_count)) {
		/* This cannot occur for a legal bounded callback.  Recover to silence
		 * rather than retain stale PCM if a future producer violates that bound. */
		usbradioplus_portaudio_poc_receive_assembler_reset(assembler);
	}
	if (receive_remainder_before + native_frame_count < URP_NATIVE_SAMPLES)
		return;
	handoff_pcm = channel->plus_portaudio_legacy_rx_frame;
	copied = usbradioplus_portaudio_poc_receive_assembler_take(
		assembler, handoff_pcm, URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES);
	if (copied < URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES)
		memset(handoff_pcm + copied, 0,
		       (URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES - copied) * sizeof(*handoff_pcm));
	portaudio_poc_publish_receive(channel, handoff_pcm,
				      URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES);
}

/**
 * @brief Run one direct hardware callback without entering Asterisk.
 * @param context Adapter channel state supplied at stream creation.
 * @param input Canonical stereo float input provided by the shared adapter.
 * @param output Canonical stereo float output populated for the shared adapter.
 * @param frame_count Number of native-rate PCM frames in this callback.
 * @return Zero after producing exactly @p frame_count output frames.
 */
static int32_t portaudio_poc_native_tick(void *context, const float *input, float *output,
					 uint32_t frame_count)
{
	struct chan_usbradio_pvt *channel = context;
	const short *app_pcm;
	int output_has_audio;

	if (!channel || !output || !frame_count || frame_count > channel->plus_native_max_frames ||
	    frame_count > URP_NATIVE_MAX_SAMPLES) {
		if (output && frame_count)
			memset(output, 0,
			       (size_t)frame_count * RPTADV_AUDIO_CANONICAL_CHANNELS *
				       sizeof(*output));
		return 0;
	}
	if (!usbradioplus_radio_access_acquire(channel)) {
		memset(output, 0,
		       (size_t)frame_count * RPTADV_AUDIO_CANONICAL_CHANNELS * sizeof(*output));
		return 0;
	}
	/* Direct callbacks submit exactly the span they consume. Advancing the
	 * playout hold in sample time preserves the normal PTT timing contract
	 * without a callback-size-dependent assembly delay. */
	usbradioplus_tx_playout_hold_advance(channel, frame_count);
	if (!channel->plus_advanced) {
		int any_audio = 0;
		size_t callback_offset = 0U;
		size_t remaining = frame_count;

		/* Preserve each legacy app_rpt boundary even when one PortAudio callback
		 * straddles it. The F32 native boundary converts only the current span,
		 * so no callback-side S16 assembly or movement can overwrite a future
		 * input span. */
		while (remaining) {
			size_t receive_remainder_before =
				channel->plus_receive_state_native_remainder;
			size_t span = URP_NATIVE_SAMPLES - receive_remainder_before;
			size_t app_rx_count;

			if (span > remaining)
				span = remaining;
			app_rx_count = usbradioplus_native_tick_f32(
				channel,
				input ? input + callback_offset * RPTADV_AUDIO_CANONICAL_CHANNELS
				      : NULL,
				output + callback_offset * RPTADV_AUDIO_CANONICAL_CHANNELS, span,
				NULL, &output_has_audio);
			(void)usbradioplus_update_receive_state_timed(channel, span);
			portaudio_poc_capture_status(channel, span);
			any_audio |= output_has_audio;
			app_pcm = (const short *)(channel->usbradio_read_buf_8k +
						  AST_FRIENDLY_OFFSET);
			portaudio_poc_publish_legacy_receive(channel, app_pcm, app_rx_count,
							     receive_remainder_before, span);
			callback_offset += span;
			remaining -= span;
		}
		usbradioplus_tx_playout_hold_note_output(channel, 1, any_audio, frame_count,
							 frame_count);
		usbradioplus_tx_playout_hold_prepare(channel);
		usbradioplus_tx_playout_hold_apply(channel);
		usbradioplus_tx_playout_hold_publish(channel);
		usbradioplus_radio_access_release(channel);
		return 0;
	}
	(void)usbradioplus_native_tick_f32(channel, input, output, frame_count, NULL,
					   &output_has_audio);
	(void)usbradioplus_update_receive_state_timed(channel, frame_count);
	portaudio_poc_capture_status(channel, frame_count);
	/* The renderer can retain filter history after logical PTT releases. Match
	 * the established adapter contract by sending silence in that state while
	 * retaining physical PTT only when this exact callback span contains audio. */
	usbradioplus_tx_playout_hold_note_output(channel, 1, output_has_audio, frame_count,
						 frame_count);
	usbradioplus_tx_playout_hold_prepare(channel);
	usbradioplus_tx_playout_hold_apply(channel);
	usbradioplus_tx_playout_hold_publish(channel);
	app_pcm = (const short *)(channel->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET);
	portaudio_poc_publish_receive(channel, app_pcm, frame_count);
	usbradioplus_radio_access_release(channel);
	return 0;
}

#ifdef URP_PROCESSING_TESTING
/**
 * @brief Exercise one hardware-free callback through the direct PortAudio proof.
 * @param channel Prepared adapter channel state.
 * @param input Canonical stereo float capture PCM, or NULL for silence.
 * @param output Canonical stereo float DAC PCM.
 * @param frame_count Native callback size.
 * @return The callback result.
 */
int32_t usbradioplus_portaudio_poc_test_callback(struct chan_usbradio_pvt *channel,
						 const float *input, float *output,
						 uint32_t frame_count)
{
	return portaudio_poc_native_tick(channel, input, output, frame_count);
}
#endif

/** @brief Deliver one control frame from the non-real-time receive bridge. */
static void portaudio_poc_deliver_key(struct chan_usbradio_pvt *channel,
				      const struct usbradioplus_portaudio_poc_rx_block *block)
{
	struct ast_frame frame = {
		.frametype = AST_FRAME_CONTROL,
		.subclass.integer = block->keyed ? AST_CONTROL_RADIO_KEY : AST_CONTROL_RADIO_UNKEY,
		.src = __PRETTY_FUNCTION__,
	};

	if (block->keyed && block->ctcss_frequency[0]) {
		frame.data.ptr = (void *)block->ctcss_frequency;
		frame.datalen = strlen(block->ctcss_frequency) + 1U;
	}
	channel->lastrx = block->keyed;
	(void)ast_queue_frame(channel->owner, &frame);
}

/**
 * @brief Deliver one callback-latched legacy text event at the Asterisk boundary.
 * @param channel Legacy channel with an owner checked by the delivery boundary.
 * @param event Non-NULL complete event copied from the lock-free status queue.
 */
static void
portaudio_poc_deliver_status(struct chan_usbradio_pvt *channel,
			     const struct usbradioplus_portaudio_poc_status_event *event)
{
	struct ast_frame frame = {
		.frametype = AST_FRAME_TEXT,
		.src = __PRETTY_FUNCTION__,
	};
	char message[32];

	switch (event->type) {
	case USBRADIOPLUS_PORTAUDIO_POC_STATUS_CTCSS:
		snprintf(message, sizeof(message), "cstx=%.26s", event->ctcss_frequency);
		break;
	case USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER:
		snprintf(message, sizeof(message), "R %i", event->voter_rssi);
		break;
	default:
		return;
	}
	frame.data.ptr = message;
	frame.datalen = strlen(message) + 1U;
	(void)ast_queue_frame(channel->owner, &frame);
}

/**
 * @brief Deliver all status events captured no later than one receive block.
 * @param channel Legacy channel receiving the text frames.
 * @param sequence_limit Event sequence recorded with the receive block.
 * @param next_sequence Worker-local next event sequence.
 * @return Zero on complete ordered delivery, otherwise -1.
 */
static int portaudio_poc_deliver_status_through(struct chan_usbradio_pvt *channel,
						uint64_t sequence_limit, uint64_t *next_sequence)
{
	while (*next_sequence < sequence_limit) {
		struct usbradioplus_portaudio_poc_status_event event;

		if (usbradioplus_portaudio_poc_status_consume(
			    &channel->plus_portaudio_status_handoff, next_sequence, &event) !=
		    USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY)
			return -1;
		portaudio_poc_deliver_status(channel, &event);
	}
	return 0;
}

/** @brief Deliver one complete voice frame from the non-real-time receive bridge. */
static void portaudio_poc_deliver_voice(struct chan_usbradio_pvt *channel,
					const struct usbradioplus_portaudio_poc_rx_block *block)
{
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = ast_format_cache_get_slin_by_rate(channel->plus_app_rpt_rate),
		.samples = block->frame_count,
		.datalen = block->frame_count * sizeof(*block->pcm),
		.data.ptr = (void *)block->pcm,
		.src = __PRETTY_FUNCTION__,
	};
	struct ast_frame *delivered = &frame;

	/* The handoff consumer owns app-rate echo recording; the native callback
	 * remains its sole playback consumer. */
	if (channel->plus_advanced || !channel->echomode) {
		usbradioplus_echo_clear(channel);
	} else if (!usbradioplus_native_echo(channel)) {
		if (block->keyed)
			usbradioplus_echo_record(channel, block->pcm, block->frame_count);
		else
			(void)usbradioplus_echo_start(channel);
	}
	if (ast_channel_state(channel->owner) != AST_STATE_UP)
		return;
	/* app_rpt retains its 8 kHz DTMF boundary. Advanced controllers receive
	 * every native voice block and own their own digit policy. */
	if (!channel->plus_advanced && channel->usedtmf && channel->dsp) {
		struct ast_frame *detected = ast_dsp_process(channel->owner, channel->dsp, &frame);

		if (detected->frametype == AST_FRAME_DTMF_END ||
		    detected->frametype == AST_FRAME_DTMF_BEGIN) {
			if (detected->subclass.integer == 'm' ||
			    detected->subclass.integer == 'u') {
				detected->frametype = AST_FRAME_NULL;
				detected->subclass.integer = 0;
			} else if (detected->frametype == AST_FRAME_DTMF_END) {
				detected->len =
					ast_tvdiff_ms(usbradioplus_host_tvnow(), channel->tonetime);
				if (option_verbose)
					ast_log(LOG_NOTICE,
						"Channel %s: Got DTMF char %c duration %ld ms\n",
						channel->name, detected->subclass.integer,
						detected->len);
				channel->toneflag = 0;
			} else if (channel->toneflag) {
				ast_frfree(detected);
				detected = NULL;
			} else {
				channel->tonetime = usbradioplus_host_tvnow();
				channel->toneflag = 1;
			}
			if (detected)
				delivered = detected;
		}
	}

	(void)ast_queue_frame(channel->owner, delivered);
	if (delivered != &frame)
		ast_frfree(delivered);
}

/**
 * @brief Deliver at most one callback handoff outside the real-time thread.
 * @param channel Channel that owns the handoff.
 * @param delivered_keyed Worker-owned last delivered key state.
 * @param seen_generation Worker-owned receive queue generation.
 * @param next_status_sequence Worker-owned next status event sequence.
 * @return One when a handoff was consumed, zero when empty, or negative on failure.
 */
static int portaudio_poc_deliver_pending(struct chan_usbradio_pvt *channel, int *delivered_keyed,
					 unsigned int *seen_generation,
					 uint64_t *next_status_sequence)
{
	struct usbradioplus_portaudio_poc_rx_block block;
	unsigned int slot;
	enum usbradioplus_portaudio_poc_handoff_result handoff_result;

	handoff_result = usbradioplus_portaudio_poc_handoff_consumer_claim(
		&channel->plus_portaudio_rx_handoff, URP_PORTAUDIO_POC_RX_BLOCK_COUNT,
		seen_generation, &slot, NULL);
	if (handoff_result == USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY)
		return 0;
	if (handoff_result != USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY)
		return -1;
	memcpy(&block, &channel->plus_portaudio_rx_blocks[slot], sizeof(block));
#ifdef URP_PROCESSING_TESTING
	if (atomic_exchange(&portaudio_poc_test_invalidate_claim, 0))
		atomic_fetch_add(&channel->plus_portaudio_rx_handoff.resync_generation, 2U);
#endif
	if (!usbradioplus_portaudio_poc_handoff_claim_current(&channel->plus_portaudio_rx_handoff,
							      *seen_generation)) {
		usbradioplus_portaudio_poc_handoff_consumer_discard(
			&channel->plus_portaudio_rx_handoff, URP_PORTAUDIO_POC_RX_BLOCK_COUNT,
			slot);
		return 1;
	}
	usbradioplus_portaudio_poc_handoff_consumer_release(&channel->plus_portaudio_rx_handoff,
							    URP_PORTAUDIO_POC_RX_BLOCK_COUNT, slot);
	if (!channel->owner)
		return 1;
	/* Resynchronization retains only the newest state edge. */
	if (*delivered_keyed != block.keyed) {
		portaudio_poc_deliver_key(channel, &block);
		*delivered_keyed = block.keyed;
		atomic_store_explicit(&channel->plus_portaudio_delivered_keyed, *delivered_keyed,
				      memory_order_release);
	}
	/* Preserve receiver state, status text, then associated voice ordering. */
	if (!portaudio_poc_deliver_status_through(channel, block.status_event_limit,
						  next_status_sequence))
		portaudio_poc_deliver_voice(channel, &block);
	return 1;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_portaudio_poc_test_deliver(struct chan_usbradio_pvt *channel, int *delivered_keyed,
					    unsigned int *seen_generation,
					    uint64_t *next_status_sequence)
{
	if (!channel || !delivered_keyed || !seen_generation || !next_status_sequence)
		return -1;
	return portaudio_poc_deliver_pending(channel, delivered_keyed, seen_generation,
					     next_status_sequence);
}
#endif

/** @brief Drain the fixed callback handoff outside the PortAudio real-time thread. */
static void *portaudio_poc_delivery_worker(void *opaque)
{
	struct chan_usbradio_pvt *channel = opaque;
	int delivered_keyed = 0;
	unsigned int seen_generation = 0U;
	uint64_t next_status_sequence = 0U;

	while (!atomic_load_explicit(&channel->plus_portaudio_delivery_stop,
				     memory_order_acquire)) {
		int result = portaudio_poc_deliver_pending(channel, &delivered_keyed,
							   &seen_generation, &next_status_sequence);

		if (result < 0)
			break;
		if (!result)
			usleep(1000U);
	}
	return NULL;
}

int usbradioplus_portaudio_poc_start(struct chan_usbradio_pvt *channel)
{
	const struct rptadv_audio_adapter_descriptor *adapter = NULL;
	struct rptadv_audio_stream_config config = {
		.struct_size = sizeof(struct rptadv_audio_stream_config),
		.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
		.native_sample_rate_hz = URP_RATE_NATIVE,
		.maximum_frame_count = URP_NATIVE_SAMPLES,
		/* CM119 capture is the mono microphone ADC. The adapter duplicates it
		 * into its canonical stereo callback representation; DAC A/B remain
		 * independent physical output channels. */
		.input_device_channels = 1U,
		.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS,
		.native_tick = portaudio_poc_native_tick,
		.native_tick_context = channel,
	};
	enum rptadv_audio_result result;
	int status = -1;

	if (!channel || !channel->plus_portaudio_poc || !channel->plus_cm119_gpio_poc)
		return -1;
	if (!channel->plus_advanced &&
	    (channel->plus_app_rpt_rate != URP_APP_RPT_RATE_DEFAULT ||
	     channel->plus_app_rpt_samples != URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES)) {
		ast_log(LOG_ERROR,
			"RadioPlus/%s: direct PortAudio proof supports only the ordinary 8 kHz "
			"legacy app boundary\n",
			channel->name);
		return -1;
	}
	/* PCM and GPIO use the facade's single resolved physical device. */
	if (usbradioplus_portaudio_poc_combined_facade_validate(
		    &channel->plus_hardware_adapter, channel->plus_hardware_adapter_prepared,
		    channel->plus_cm119_gpio_usb_port_path) !=
	    USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK) {
		ast_log(LOG_ERROR, "RadioPlus/%s: hardware adapter identity is not prepared\n",
			channel->name);
		return -1;
	}
	adapter = channel->plus_hardware_adapter.audio;
	if (!portaudio_poc_stream_adapter_valid(adapter))
		return -1;
	/* Lifecycle operations are control-plane work. Serialize them with the HID
	 * owner so hangup/retry cannot detach the Asterisk channel while startup has
	 * a worker but has not yet published its stream handle. The callback never
	 * takes this mutex. */
	ast_mutex_lock(&channel->usblock);
	if (atomic_load_explicit(&channel->plus_hardware_stop_request, memory_order_acquire))
		goto done;
	if (channel->plus_portaudio_stream) {
		status = 0;
		goto done;
	}
	usbradioplus_portaudio_poc_handoff_reset(&channel->plus_portaudio_rx_handoff);
	usbradioplus_portaudio_poc_status_reset(&channel->plus_portaudio_status_handoff);
	atomic_store_explicit(&channel->plus_portaudio_delivery_stop, 0, memory_order_release);
	atomic_store_explicit(&channel->plus_portaudio_delivery_running, 1, memory_order_release);
	atomic_store_explicit(&channel->plus_portaudio_delivered_keyed, 0, memory_order_release);
	channel->plus_portaudio_callback_keyed = 0;
	channel->plus_portaudio_voter_remaining_frames = 0U;
	usbradioplus_portaudio_poc_receive_assembler_reset(
		&channel->plus_portaudio_legacy_rx_assembler);
	if (ast_pthread_create_background(&channel->plus_portaudio_delivery_thread, NULL,
					  portaudio_poc_delivery_worker, channel)) {
		atomic_store_explicit(&channel->plus_portaudio_delivery_running, 0,
				      memory_order_release);
		goto done;
	}
	if (usbradioplus_hardware_adapter_stream_create(&channel->plus_hardware_adapter, &config,
							&channel->plus_portaudio_stream) !=
	    USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		goto failed;
	result = adapter->stream_start(channel->plus_portaudio_stream);
	if (result == RPTADV_AUDIO_OK) {
		portaudio_poc_log_stream_timing(channel, adapter);

		ast_log(LOG_NOTICE,
			"RadioPlus/%s: started direct PortAudio proof-of-concept stream\n",
			channel->name);
		status = 0;
		goto done;
	}
	adapter->stream_destroy(channel->plus_portaudio_stream);
	channel->plus_portaudio_stream = NULL;
failed:
	/* stream_create() is allowed to return a handle only with success, but do
	 * not leak a partially created adapter stream if that contract is violated. */
	if (channel->plus_portaudio_stream) {
		adapter->stream_destroy(channel->plus_portaudio_stream);
		channel->plus_portaudio_stream = NULL;
	}
	atomic_store_explicit(&channel->plus_portaudio_delivery_stop, 1, memory_order_release);
	pthread_join(channel->plus_portaudio_delivery_thread, NULL);
	atomic_store_explicit(&channel->plus_portaudio_delivery_running, 0, memory_order_release);
done:
	ast_mutex_unlock(&channel->usblock);
	return status;
}

void usbradioplus_portaudio_poc_stop(struct chan_usbradio_pvt *channel)
{
	const struct rptadv_audio_adapter_descriptor *adapter;

	if (!channel)
		return;
	/* See start(): these are control-plane operations. Holding this mutex makes
	 * the non-atomic adapter handle single-owner without burdening audio. */
	ast_mutex_lock(&channel->usblock);
	adapter = channel->plus_hardware_adapter.audio;
	if (channel->plus_portaudio_stream && portaudio_poc_stream_adapter_valid(adapter)) {
		(void)adapter->stream_stop(channel->plus_portaudio_stream);
		adapter->stream_destroy(channel->plus_portaudio_stream);
		channel->plus_portaudio_stream = NULL;
	}
	if (atomic_exchange_explicit(&channel->plus_portaudio_delivery_running, 0,
				     memory_order_acq_rel)) {
		atomic_store_explicit(&channel->plus_portaudio_delivery_stop, 1,
				      memory_order_release);
		pthread_join(channel->plus_portaudio_delivery_thread, NULL);
	}
	if (atomic_exchange_explicit(&channel->plus_portaudio_delivered_keyed, 0,
				     memory_order_acq_rel) &&
	    channel->owner) {
		const struct usbradioplus_portaudio_poc_rx_block unkey = {0};

		/* A device retry can stop at the last keyed block. Publish the missing
		 * edge before this compatibility channel is detached. */
		portaudio_poc_deliver_key(channel, &unkey);
	}
	channel->plus_portaudio_voter_remaining_frames = 0U;
	usbradioplus_portaudio_poc_receive_assembler_reset(
		&channel->plus_portaudio_legacy_rx_assembler);
	usbradioplus_native_output_stage_fail_safe_reset(channel);
	ast_mutex_unlock(&channel->usblock);
}

#endif
