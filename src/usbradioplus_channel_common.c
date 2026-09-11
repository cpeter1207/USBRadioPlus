/** @file
 * @brief Shared Asterisk channel lifecycle, radio configuration, and tuning operations.
 */

#include "asterisk.h"
#include "asterisk/res_usbradio.h"

#include <errno.h>
#include <math.h>
#include <sched.h>
#include <search.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_SYS_IO
#include <sys/io.h>
#endif

#include "asterisk/abstract_jb.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/dsp.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/lock.h"
#include "asterisk/module.h"
#include "asterisk/musiconhold.h"
#include "asterisk/options.h"
#include "asterisk/utils.h"

#include "txagc/avfilter_processor.h"
#include "txagc/rnnoise_processor.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_dsp.h"
#include "usbradioplus_hardware.h"
#include "usbradioplus_processing.h"
#include "usbradioplus_radio.h"
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_common.h"

/*! \brief Drive timed pulses on configured parallel-port outputs. */
void *pulserthread(void *arg)
{
	(void)arg;
	struct timeval now, then;
	register int i, j, k;

#ifdef HAVE_SYS_IO
	if (haspp == 2) {
		ioperm(pbase, 2, 1);
	}
#endif
	stoppulser = 0;
	pp_lastmask = 0;
	ast_mutex_lock(&pp_lock);
	ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
	ast_mutex_unlock(&pp_lock);
	then = ast_radio_tvnow();

	while (!stoppulser) {
		usleep(50000);
		ast_mutex_lock(&pp_lock);
		now = ast_radio_tvnow();
		j = ast_tvdiff_ms(now, then);
		then = now;
		pp_lastmask = pp_pulsemask;
		pp_pulsemask = 0;
		for (i = 2; i <= 9; i++) {
			k = pp_pulsetimer[i];
			if (k) {
				k -= j;
				if (k < 0) {
					k = 0;
				}
				pp_pulsetimer[i] = k;
			}
			if (k) {
				pp_pulsemask |= 1 << (i - 2);
			}
		}
		if (pp_pulsemask != pp_lastmask) {
			pp_val ^= pp_lastmask ^ pp_pulsemask;
			ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
		}
		ast_mutex_unlock(&pp_lock);
	}
	return NULL;
}

/*! \brief Release a channel that failed before it was linked into the active list. */
void destroy_unlinked_channel(struct chan_usbradio_pvt *o)
{
	if (!o || o == &usbradio_default) {
		return;
	}
	if (o->dsp) {
		ast_dsp_free(o->dsp);
	}
	usbradioplus_dsp_destroy(o);
	if (o->radio) {
		urp_radio_destroy(o->radio);
	}
	ast_free(o->name);
	ast_free(o);
}

int hidhdwconfig(struct chan_usbradio_pvt *o)
{
	int i;

	/* NOTE: on the CM-108AH, GPIO2 is *not* a REAL GPIO.. it was re-purposed
	 *  as a signal called "HOOK" which can only be read from the HID.
	 *  Apparently, in a REAL CM-108, GPIO really works as a GPIO
	 */

	if (o->hdwtype == 1) {
		/* sphusb */
		o->hid_gpio_ctl = 0x08;	 /* set GPIO4 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 4;	 /* GPIO3 is COR */
		o->hid_io_cor_loc = 1;	 /* GPIO3 is COR */
		o->hid_io_ctcss = 2;	 /* GPIO 2 is External CTCSS */
		o->hid_io_ctcss_loc = 1; /* is GPIO 2 */
		o->hid_io_ptt = 8;	 /* GPIO 4 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 1;	 /* for GPIO 1 */
	} else if (o->hdwtype == 0) {
		/* dudeusb */
		o->hid_gpio_ctl = 0x04;	 /* set GPIO 3 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;	 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 1;	 /* VOL UP External CTCSS */
		o->hid_io_ctcss_loc = 0; /* VOL UP External CTCSS */
		o->hid_io_ptt = 4;	 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 0xfb;	 /* for GPIO 1,2,4,5,6,7,8 (5,6,7,8 for CM-119 only) */
	} else if (o->hdwtype == 2) {
		/* NHRC (N1KDO) (dudeusb w/o user GPIO) */
		o->hid_gpio_ctl = 0x04;	 /* set GPIO 3 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;	 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 1;	 /* VOL UP is External CTCSS */
		o->hid_io_ctcss_loc = 0; /* VOL UP CTCSS */
		o->hid_io_ptt = 4;	 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 0;	 /* for GPIO 1,2,4 */
	} else if (o->hdwtype == 3) {
		/* custom version */
		o->hid_gpio_ctl = 0x0c;	 /* set GPIO 3 & 4 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;	 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 2;	 /* GPIO 2 is External CTCSS */
		o->hid_io_ctcss_loc = 1; /* is GPIO 2 */
		o->hid_io_ptt = 4;	 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 1;	 /* for GPIO 1 */
	}
	/* validate clipledgpio setting (Clip LED GPIO#) */
	if (o->clipledgpio) {
		if (o->clipledgpio >= GPIO_PINCOUNT ||
		    !(o->valid_gpios & (1 << (o->clipledgpio - 1)))) {
			ast_log(LOG_ERROR, "Channel %s: clipledgpio = GPIO%d not supported\n",
				o->name, o->clipledgpio);
			o->clipledgpio = 0;
		} else {
			o->hid_gpio_ctl |= 1 << (o->clipledgpio -
						 1); /* confirm Clip LED GPIO set to output mode */
		}
	}
	o->hid_gpio_val = 0;
	for (i = 0; i < GPIO_PINCOUNT; i++) {
		/* skip if this one not specified */
		if (!o->gpios[i]) {
			continue;
		}
		/* skip if not out */
		if (strncasecmp(o->gpios[i], "out", 3)) {
			continue;
		}
		/* skip if PTT */
		if ((1 << i) & o->hid_io_ptt) {
			ast_log(LOG_ERROR,
				"Channel %s: You can't specify gpio%d, since its the PTT.\n",
				o->name, i + 1);
			continue;
		}
		/* skip if not a valid GPIO */
		if (!(o->valid_gpios & (1 << i))) {
			ast_log(LOG_ERROR,
				"Channel %s: You can't specify gpio%d, it is not valid in this "
				"configuration.\n",
				o->name, i + 1);
			continue;
		}
		o->hid_gpio_ctl |= (1 << i); /* set this one to output, also */
		/* if default value is 1, set it */
		if (!strcasecmp(o->gpios[i], "out1")) {
			o->hid_gpio_val |= (1 << i);
		}
	}
	if (o->invertptt) {
		o->hid_gpio_val |= o->hid_io_ptt;
	}
	return 0;
}

void kickptt(const struct chan_usbradio_pvt *o)
{
	char c = 0;
	int res;

	if (!o) {
		return;
	}
	if (o->pttkick[1] == -1) {
		return;
	}
	res = write(o->pttkick[1], &c, 1);
	if (res <= 0) {
		/* A wake is advisory; the hardware worker observes the atomic request on
		 * its regular poll.  A full nonblocking pipe must not create log noise. */
		if (errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
		    || errno == EWOULDBLOCK
#endif
		)
			return;
		ast_log(LOG_ERROR, "Channel %s: Write failed: %s\n", o->name, strerror(errno));
	}
}

/** @brief Copy lock-free HID-worker state into fields owned by the audio callback.
 * @param o Private channel whose callback imports immutable hardware state.
 *
 * The radio signaling engine has a single writer: the native audio callback.
 * HID sampling and physical PTT completion cross that boundary only through
 * atomics, which avoids both a mutex in the callback and a C data race.
 */
void usbradioplus_audio_load_hardware_state(struct chan_usbradio_pvt *o)
{
	unsigned int inputs;

	if (!o)
		return;
	inputs = atomic_load_explicit(&o->plus_hardware_inputs, memory_order_acquire);
	o->rxhidsq = !!(inputs & URP_HARDWARE_INPUT_HID_CARRIER);
	o->rxhidctcss = !!(inputs & URP_HARDWARE_INPUT_HID_CTCSS);
	o->rxppsq = !!(inputs & URP_HARDWARE_INPUT_PARALLEL_CARRIER);
	o->rxppctcss = !!(inputs & URP_HARDWARE_INPUT_PARALLEL_CTCSS);
	if (o->radio) {
		o->radio->txPttHid =
			atomic_load_explicit(&o->plus_hardware_ptt_applied, memory_order_acquire);
	}
}

void usbradioplus_publish_hardware_ptt(struct chan_usbradio_pvt *o, int asserted)
{
	if (o)
		atomic_store_explicit(&o->plus_hardware_ptt_request, !!asserted,
				      memory_order_release);
}

void usbradioplus_tx_playout_hold_prepare(struct chan_usbradio_pvt *channel)
{
	const struct usbradioplus_tx_playout_hold *hold;

	if (!channel || !channel->radio)
		return;
	hold = &channel->plus_tx_playout_hold;
	/* The previous callback may have made txPttOut high solely to drain the
	 * audio device. Restore the engine's unmodified output before it advances. */
	if (hold->draining)
		channel->radio->txPttOut = hold->engine_ptt_out;
}

void usbradioplus_tx_playout_hold_apply(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_tx_playout_hold *hold;
	int input_keyed;
	int was_draining;

	if (!channel || !channel->radio)
		return;
	hold = &channel->plus_tx_playout_hold;
	was_draining = hold->draining;
	input_keyed = !!channel->radio->txPttIn;
	/* A new external key request supersedes queued audio from the previous
	 * transmission. It must never inherit a stale post-playout release timer. */
	if (input_keyed && !hold->input_keyed) {
		hold->callbacks_remaining = 0U;
		hold->draining = 0;
	}
	hold->input_keyed = input_keyed;
	/* Capture the real signaling decision before applying a virtual drain PTT.
	 * This preserves existing CTCSS/DCS tail handling and rekey semantics. */
	hold->engine_ptt_out = !!channel->radio->txPttOut;
	if (hold->engine_ptt_out) {
		hold->draining = 0;
		return;
	}
	if (hold->callbacks_remaining) {
		channel->radio->txPttOut = 1;
		hold->draining = 1;
	} else {
		channel->radio->txPttOut = 0;
		hold->draining = 0;
		/* The signaling engine started its RX blanking interval when it first
		 * requested unkey. Start it again at the actual physical-release point. */
		if (was_draining)
			channel->radio->txrxblankingtimer = channel->radio->txrxblankingtime;
	}
}

void usbradioplus_tx_playout_hold_publish(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_tx_playout_hold *hold;

	if (!channel || !channel->radio)
		return;
	hold = &channel->plus_tx_playout_hold;
	/* The virtual PTT keeps the transmitter keyed only while already accepted
	 * PCM drains.  That interval must submit silence, not later program audio. */
	atomic_store_explicit(&channel->plus_radio_tx_active,
			      !!channel->radio->txPttIn || hold->engine_ptt_out,
			      memory_order_release);
	usbradioplus_publish_hardware_ptt(channel, channel->radio->txPttOut);
}

void usbradioplus_tx_playout_hold_reset(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_tx_playout_hold *hold;

	if (!channel)
		return;
	hold = &channel->plus_tx_playout_hold;
	if (channel->radio && hold->draining)
		channel->radio->txPttOut = hold->engine_ptt_out;
	hold->callbacks_remaining = 0U;
	hold->engine_ptt_out = 0;
	hold->input_keyed = 0;
	hold->draining = 0;
}

void usbradioplus_tx_playout_hold_note_output(struct chan_usbradio_pvt *channel, int submitted,
					      int audio_bearing, unsigned int callbacks)
{
	struct usbradioplus_tx_playout_hold *hold;

	if (!channel || !channel->radio || !submitted)
		return;
	hold = &channel->plus_tx_playout_hold;
	/* The adapter already includes the requested 20 ms safety block in this
	 * rounded native-callback count. A zero estimate still keeps one block. */
	if (audio_bearing) {
		hold->callbacks_remaining = callbacks ? callbacks : 1U;
	} else if (hold->callbacks_remaining) {
		/* Only a complete DAC submission advances the drain clock. A full
		 * device queue therefore extends PTT instead of cutting queued PCM off. */
		--hold->callbacks_remaining;
	}
}

int usbradioplus_tx_playout_hold_draining(const struct chan_usbradio_pvt *channel)
{
	return channel && channel->plus_tx_playout_hold.draining;
}

int usbradioplus_pcm_has_audio(const short *samples, size_t count)
{
	size_t index;

	if (!samples)
		return 0;
	for (index = 0U; index < count; ++index)
		if (samples[index])
			return 1;
	return 0;
}

void usbradioplus_request_clip_led(struct chan_usbradio_pvt *o)
{
	if (o && o->clipledgpio)
		atomic_store_explicit(&o->plus_clip_led_request, 1, memory_order_release);
}

void usbradioplus_publish_hardware_inputs(struct chan_usbradio_pvt *o, unsigned int inputs)
{
	if (o)
		atomic_store_explicit(&o->plus_hardware_inputs, inputs, memory_order_release);
}

#ifdef URP_PROCESSING_TESTING
static atomic_uint test_radio_program_snapshot_invalidations;
#endif

int usbradioplus_read_radio_program_request(const struct chan_usbradio_pvt *o,
					    struct usbradioplus_radio_program_request *request)
{
	if (!o || !request)
		return 0;
	for (unsigned int attempt = 0; attempt < 2U; ++attempt) {
		unsigned int before;
		unsigned int after;

		before = atomic_load_explicit(&o->plus_radio_program_generation,
					      memory_order_acquire);
		if (before & 1U)
			continue;
		request->rx_frequency = atomic_load_explicit(&o->plus_radio_program_rx_frequency,
							     memory_order_relaxed);
		request->tx_frequency = atomic_load_explicit(&o->plus_radio_program_tx_frequency,
							     memory_order_relaxed);
		request->high_power = atomic_load_explicit(&o->plus_radio_program_high_power,
							   memory_order_relaxed);
#ifdef URP_PROCESSING_TESTING
		if (atomic_load_explicit(&test_radio_program_snapshot_invalidations,
					 memory_order_relaxed) != 0U)
			atomic_fetch_sub_explicit(&test_radio_program_snapshot_invalidations, 1U,
						  memory_order_relaxed);
#endif
		after = atomic_load_explicit(&o->plus_radio_program_generation,
					     memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		if (atomic_load_explicit(&test_radio_program_snapshot_invalidations,
					 memory_order_relaxed) != 0U)
			++after;
#endif
		/* A matching generation is necessarily even: odd values were rejected
		 * before reading the payload. */
		if (before == after) {
			request->generation = after;
			return 1;
		}
	}
	return 0;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_radio_program_snapshot_retry(void)
{
	struct chan_usbradio_pvt channel = {0};
	struct usbradioplus_radio_program_request request;

	atomic_init(&channel.plus_radio_program_generation, 0U);
	atomic_init(&channel.plus_radio_program_rx_frequency, 146520000U);
	atomic_init(&channel.plus_radio_program_tx_frequency, 146940000U);
	atomic_init(&channel.plus_radio_program_high_power, 1);
	atomic_store_explicit(&test_radio_program_snapshot_invalidations, 2U, memory_order_relaxed);
	return usbradioplus_read_radio_program_request(&channel, &request);
}
#endif

int usbradio_digit_begin(struct ast_channel *c, char digit)
{
	(void)c;
	(void)digit;
	return 0;
}

int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration)
{
	(void)c;
	/* no better use for received digits than print them */
	ast_verbose(" << Console Received digit %c of duration %u ms >> \n", digit, duration);
	return 0;
}

int usbradio_answer(struct ast_channel *c)
{
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

void usbradioplus_interface_mode(struct chan_usbradio_pvt *channel, int advanced)
{
	channel->plus_advanced = advanced;
	channel->plus_app_rpt_rate = advanced ? URP_RATE_NATIVE : URP_APP_RPT_RATE_DEFAULT;
	channel->plus_app_rpt_samples = channel->plus_app_rpt_rate / 50;
	/* The shared ring owns the one persistent app_rpt-to-native conversion and
	 * clock correction stream.  Its source-rate target steers clock recovery;
	 * it never delays startup or reserves a block from playout. */
	channel->plus_program_target_samples =
		(channel->plus_app_rpt_rate * URP_PROGRAM_RING_TARGET_MS + 999U) / 1000U;
	if (rpcr_set_rates(&channel->plus_program_ring, channel->plus_app_rpt_rate,
			   URP_RATE_NATIVE))
		ast_log(LOG_ERROR, "RadioPlus/%s: unable to configure program sample rates\n",
			channel->name);
	/* The native renderer owns SRC history and resets it after it observes the
	 * graph generation carrying this rate. No control-plane thread touches a
	 * live converter. */
	if (channel->plus_dsp_initialized) {
		if (usbradioplus_prepare_native_processing(channel))
			ast_log(LOG_ERROR, "RadioPlus/%s: unable to apply native interface mode\n",
				channel->name);
	}
}

void usbradioplus_configure_advanced(struct ast_channel *channel)
{
	struct chan_usbradio_pvt *radio = ast_channel_tech_pvt(channel);
	usbradioplus_interface_mode(radio, 1);
	if (radio->radio)
		radio->radio->radioDuplex = 1;
	/* The controller, not a hardware sidetone path, owns local repeat. */
	if (radio->hasusb)
		mixer_write(radio);
}

void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples, size_t count)
{
	uint64_t discarded =
		atomic_load_explicit(&o->plus_program_ring.discarded, memory_order_relaxed);
	size_t index;

	if (!samples || !count)
		return;
	/* This is the sole app_rpt-to-hardware boundary.  Publish one sample at a
	 * time so the producer and native consumer advance independent SPSC cursors
	 * without a frame admission lock or a startup batch. */
	for (index = 0; index < count; ++index)
		(void)rpcr_producer_push_sample(&o->plus_program_ring, samples[index]);
	if (atomic_load_explicit(&o->plus_program_ring.discarded, memory_order_relaxed) !=
	    discarded)
		o->plus_link_queue_overflows++;
}

void usbradioplus_echo_clear(struct chan_usbradio_pvt *o)
{
	/* Stop admission immediately, then let the callback renderer discard with
	 * its owned consumer cursor at the next complete render boundary. Resetting
	 * both cursors here would race active legacy-echo recording. */
	atomic_store_explicit(&o->echoing, 0, memory_order_release);
	usbradioplus_native_renderer_clear_legacy_echo(o);
}

int usbradioplus_echo_start(struct chan_usbradio_pvt *o)
{
	int active = urp_sample_queue_samples(&o->echo_queue) != 0U;

	atomic_store_explicit(&o->echoing, active, memory_order_release);
	return active;
}

void usbradioplus_echo_record(struct chan_usbradio_pvt *o, const short *samples, size_t count)
{
	unsigned int app_samples =
		o->plus_app_rpt_samples ? o->plus_app_rpt_samples : URP_APP_RPT_RATE_DEFAULT / 50U;
	unsigned int capacity;

	if (o->echomax <= 0 || atomic_load_explicit(&o->echoing, memory_order_acquire))
		return;
	if ((unsigned int)o->echomax > URP_ECHO_QUEUE_SAMPLES / app_samples)
		capacity = URP_ECHO_QUEUE_SAMPLES;
	else
		capacity = (unsigned int)o->echomax * app_samples;
	while (count-- && urp_sample_queue_samples(&o->echo_queue) < capacity) {
		if (!urp_sample_queue_push_sample(&o->echo_queue, *samples))
			break;
		++samples;
	}
}

int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	(void)oldchan;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(newchan);
	ast_log(LOG_WARNING, "Channel %s: Fixup received.\n", o->name);
	o->owner = newchan;
	return 0;
}

int usbradio_indicate(struct ast_channel *c, int cond_in, const void *data, size_t datalen)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	enum ast_control_frame_type cond = cond_in;

	switch (cond) {
	case AST_CONTROL_BUSY:
	case AST_CONTROL_CONGESTION:
	case AST_CONTROL_RINGING:
		break;
	case AST_CONTROL_VIDUPDATE:
		break;
	case AST_CONTROL_HOLD:
		ast_verbose("Channel %s: Console has been placed on hold.\n", o->name);
		ast_moh_start(c, data, "default");
		break;
	case AST_CONTROL_UNHOLD:
		ast_verbose("Channel %s: Console has been retrieved from hold.\n", o->name);
		ast_moh_stop(c);
		break;
	case AST_CONTROL_PROCEEDING:
		ast_verbose("Channel %s: Call Proceeding.\n", o->name);
		ast_moh_stop(c);
		break;
	case AST_CONTROL_PROGRESS:
		ast_verbose("Channel %s: Call Progress.\n", o->name);
		ast_moh_stop(c);
		break;
	case AST_CONTROL_RADIO_KEY:
		ast_debug(5, "URP_TXTRACE channel=%s event=request key=1\n", o->name);
		atomic_store_explicit(&o->txkeyed, 1, memory_order_release);
		kickptt(o);
		ast_debug(1, "Channel %s: ACRK code=%s TX ON.\n", o->name, (char *)data);
		if (datalen && ((char *)(data))[0] != '0') {
			o->forcetxcode = 1;
			memset(o->set_txctcssfreq, 0,
			       sizeof(o->set_txctcssfreq)); /* Possibly unnecessary, if this is used
							       as a string? */
			ast_copy_string(o->set_txctcssfreq, data, sizeof(o->set_txctcssfreq));
			radio_config(o);
		}
		break;
	case AST_CONTROL_RADIO_UNKEY:
		ast_debug(5, "URP_TXTRACE channel=%s event=request key=0\n", o->name);
		atomic_store_explicit(&o->txkeyed, 0, memory_order_release);
		kickptt(o);
		ast_debug(1, "Channel %s: ACRUK TX OFF.\n", o->name);
		if (o->forcetxcode) {
			o->forcetxcode = 0;
			o->radio->pTxCodeDefault = o->txctcssdefault;
			ast_debug(1, "Channel %s: Forced Tx Squelch Code cleared.\n", o->name);
		}
		break;
	default:
		ast_log(LOG_WARNING, "Channel %s: Don't know how to display condition %d.\n",
			o->name, cond);
		return -1;
	}

	return 0;
}

int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen)
{
	const char *cp;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(chan);

	/* all supported options require data */
	if (!data || (datalen < 1)) {
		errno = EINVAL;
		return -1;
	}

	switch (option) {
	case AST_OPTION_TONE_VERIFY:
		cp = data;
		switch (*cp) {
		case 1:
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: OFF(0).\n",
				o->name);
			o->usedtmf = 1;
			break;
		case 2:
			ast_log(LOG_NOTICE,
				"Channel %s: Set option TONE VERIFY, mode: MUTECONF/MAX(2).\n",
				o->name);
			o->usedtmf = 1;
			break;
		case 3:
			ast_log(LOG_NOTICE,
				"Channel %s: Set option TONE VERIFY, mode: DISABLE DETECT(3).\n",
				o->name);
			o->usedtmf = 0;
			break;
		default:
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: OFF(0).\n",
				o->name);
			o->usedtmf = 1;
			break;
		}
		break;
	default:
		break;
	}
	errno = 0;
	return 0;
}

int console_key(int fd, int argc, const char *const *argv)
{
	(void)fd;
	(void)argv;
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);

	if (argc != 2) {
		return RESULT_SHOWUSAGE;
	}
	atomic_store_explicit(&o->txtestkey, 1, memory_order_release);
	kickptt(o);
	return RESULT_SUCCESS;
}

int console_unkey(int fd, int argc, const char *const *argv)
{
	(void)fd;
	(void)argv;
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);

	if (argc != 2) {
		return RESULT_SHOWUSAGE;
	}
	atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
	kickptt(o);
	return RESULT_SUCCESS;
}

void tune_flash(int fd, struct chan_usbradio_pvt *o, int intflag)
{

#define NFLASH 3

	int i;

	if (fd > 0) {
		ast_cli(fd, "Channel %s: USB Device Flash starting.\n", o->name);
	}
	for (i = 0; i < NFLASH; i++) {
		atomic_store_explicit(&o->txtestkey, 1, memory_order_release);
		atomic_store_explicit(&o->plus_test_tone_enabled, 1, memory_order_release);
		if ((fd > 0) && intflag) {
			if (ast_radio_wait_or_poll(fd, 1000, intflag)) {
				atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
				atomic_store_explicit(&o->plus_test_tone_enabled, 0,
						      memory_order_release);
				break;
			}
		} else {
			usleep(1000000);
		}
		atomic_store_explicit(&o->plus_test_tone_enabled, 0, memory_order_release);
		atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
		if (i < (NFLASH - 1) && (fd > 0) && intflag) {
			if (ast_radio_wait_or_poll(fd, 1500, intflag)) {
				atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
				break;
			}
		} else if (i < (NFLASH - 1)) {
			usleep(1500000);
		}
	}
	if (fd > 0) {
		ast_cli(fd, "Channel %s: USB Device Flash completed.\n", o->name);
	}
	atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
	atomic_store_explicit(&o->plus_test_tone_enabled, 0, memory_order_release);
}

int radio_tune(int fd, int argc, const char *const *argv)
{
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);
	int i;

	if ((argc < 3) || (argc > 4)) {
		return RESULT_SHOWUSAGE;
	}

	o->radio->b.tuning = 1;

	if (!strcasecmp(argv[2], "dump")) {
		radio_dump(o, fd);
	} else if (!strcasecmp(argv[2], "swap")) {
		if (argc > 3) {
			usb_device_swap(fd, argv[3]);
			return RESULT_SUCCESS;
		}
		return RESULT_SHOWUSAGE;
	} else if (!strcasecmp(argv[2], "menu-support")) {
		if (argc > 3) {
			tune_menusupport(fd, o, argv[3]);
		}
		return RESULT_SUCCESS;
	}

	if (!o->hasusb) {
		ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
		return RESULT_SUCCESS;
	}

	if (!strcasecmp(argv[2], "rxnoise")) {
		tune_rxinput(fd, o, 0, 0);
	} else if (!strcasecmp(argv[2], "rxvoice")) {
		tune_rxvoice(fd, o, 0);
	} else if (!strcasecmp(argv[2], "rxtone")) {
		tune_rxctcss(fd, o, 0);
	} else if (!strcasecmp(argv[2], "flash")) {
		tune_flash(fd, o, 0);
	} else if (!strcasecmp(argv[2], "rxsquelch")) {
		if (argc == 3) {
			ast_cli(fd, "Current Signal Strength is %d\n",
				((32767 - o->radio->rxRssi) * 1000 / 32767));
			ast_cli(fd, "Current Squelch setting is %d\n", o->rxsquelchadj);
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}
			ast_cli(fd, "Changed Squelch setting to %d\n", i);
			o->rxsquelchadj = i;
			*(o->radio->prxSquelchAdjust) = ((999 - i) * 32767) / AUDIO_ADJUSTMENT;
		}
	} else if (!strcasecmp(argv[2], "txvoice")) {
		i = 0;

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) &&
		    (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		} else if (argc == 3) {
			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				ast_cli(fd, "Current txvoice setting on Channel A is %d\n",
					o->txmixaset);
			} else {
				ast_cli(fd, "Current txvoice setting on Channel B is %d\n",
					o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}

			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				o->txmixaset = i;
				ast_cli(fd, "Changed txvoice setting on Channel A to %d\n",
					o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed txvoice setting on Channel B to %d\n",
					o->txmixbset);
			}
			mixer_write(o);
			mult_set(o);
			ast_cli(fd, "Changed Tx Voice Output setting to %d\n", i);
		}
		o->radio->b.txCtcssInhibit = 1;
		tune_txoutput(o, i, fd, 0);
		o->radio->b.txCtcssInhibit = 0;
	} else if (!strcasecmp(argv[2], "txall")) {
		i = 0;

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) &&
		    (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		} else if (argc == 3) {
			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				ast_cli(fd, "Current txvoice setting on Channel A is %d\n",
					o->txmixaset);
			} else {
				ast_cli(fd, "Current txvoice setting on Channel B is %d\n",
					o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}

			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				o->txmixaset = i;
				ast_cli(fd, "Changed txvoice setting on Channel A to %d\n",
					o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed txvoice setting on Channel B to %d\n",
					o->txmixbset);
			}
			mixer_write(o);
			mult_set(o);
			ast_cli(fd, "Changed Tx Voice Output setting to %d\n", i);
		}
		tune_txoutput(o, i, fd, 0);
	} else if (!strcasecmp(argv[2], "auxvoice")) {
		if ((o->txmixa != TX_OUT_AUX) && (o->txmixb != TX_OUT_AUX)) {
			ast_log(LOG_WARNING, "No auxvoice output configured.\n");
		} else if (argc == 3) {
			if (o->txmixa == TX_OUT_AUX) {
				ast_cli(fd, "Current auxvoice setting on Channel A is %d\n",
					o->txmixaset);
			} else {
				ast_cli(fd, "Current auxvoice setting on Channel B is %d\n",
					o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}
			if (o->txmixa == TX_OUT_AUX) {
				o->txmixaset = i;
				ast_cli(fd, "Changed auxvoice setting on Channel A to %d\n",
					o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed auxvoice setting on Channel B to %d\n",
					o->txmixbset);
			}
			mixer_write(o);
			mult_set(o);
		}
	} else if (!strcasecmp(argv[2], "txtone")) {
		if (argc == 3) {
			ast_cli(fd, "Current Tx CTCSS modulation setting = %d\n", o->txctcssadj);
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}
			o->txctcssadj = i;
			o->ctcss_level = 32767.0 * (double)i / 999.0;
			set_txctcss_level(o);
			ast_cli(fd, "Changed Tx CTCSS modulation setting to %i\n", i);
		}
		atomic_store_explicit(&o->txtestkey, 1, memory_order_release);
		usleep(5000000);
		atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
	} else if (!strcasecmp(argv[2], "save")) {
		tune_write(o);
		ast_cli(fd, "Saved radio tuning settings.\n");
	} else if (!strcasecmp(argv[2], "load")) {
		ast_mutex_lock(&o->eepromlock);
		while (o->eepromctl) {
			ast_mutex_unlock(&o->eepromlock);
			usleep(10000);
			ast_mutex_lock(&o->eepromlock);
		}
		o->eepromctl = 1; /* request a load */
		ast_mutex_unlock(&o->eepromlock);

		ast_cli(fd, "Requesting loading of tuning settings from EEPROM for channel %s\n",
			o->name);
	} else {
		o->radio->b.tuning = 0;
		return RESULT_SHOWUSAGE;
	}
	o->radio->b.tuning = 0;
	return RESULT_SUCCESS;
}

int set_txctcss_level(struct chan_usbradio_pvt *o)
{
	if (o && o->radio)
		o->radio->txCtcssPeak = o->ctcss_level;
	return 0;
}

int radio_set_dsp_debug(int fd, int argc, const char *const *argv)
{
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);

	if (argc == 4) {
		int i;
		i = atoi(argv[3]);
		if ((i >= 0) && (i <= 100)) {
			o->radio->tracelevel = i;
		}
	}
	/* add ability to set it for a number of frames after which it reverts */
	ast_cli(fd, "Channel %s: xdebug on tracelevel %i\n", o->name, o->radio->tracelevel);

	return RESULT_SUCCESS;
}

void store_rxdemod(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_rx_audio_mode mode;
	if (urp_parse_rx_audio_mode(s, &mode))
		ast_log(LOG_WARNING, "Unrecognized rxdemod parameter: %s\n", s);
	else
		o->rxdemod = (enum radio_rx_audio)mode;
}

void store_rxsdtype(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_ctcss_source source;
	if (urp_parse_ctcss_source(s, &source))
		ast_log(LOG_WARNING, "Unrecognized rxsdtype parameter: %s\n", s);
	else
		o->rxsdtype = (enum radio_squelch_detect)source;
}

double effective_rx_input_gain_db(const struct chan_usbradio_pvt *o)
{
	struct txagc_chain chain;
	usbradioplus_processing_get_local_rt(o->name, &chain);
	return chain.agc.input_gain_db;
}

float effective_rx_decoder_gain(const struct chan_usbradio_pvt *o)
{
	return (float)(pow(10.0, effective_rx_input_gain_db(o) / 20.0) / 2.0);
}

int effective_rxmixerset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	return urp_gain_db_to_mixer(hardware.input_gain_db);
}

int effective_txmixaset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	return urp_gain_db_to_mixer(hardware.output_a_gain_db);
}

int effective_txmixbset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	return urp_gain_db_to_mixer(hardware.output_b_gain_db);
}

enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	return (enum radio_tx_mix)hardware.output_a_assignment;
}

enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	return (enum radio_tx_mix)hardware.output_b_assignment;
}

/** @brief Translate a receive COS assignment to the radio signaling source.
 * @param value Configured carrier assignment.
 * @return Radio carrier-detector source selected by the assignment.
 */
static enum radio_carrier_detect carrier_detect_from_assignment(const char *value)
{
	if (!strcasecmp(value, "usb"))
		return CD_HID;
	if (!strcasecmp(value, "usbinvert"))
		return CD_HID_INVERT;
	if (!strcasecmp(value, "dsp"))
		return CD_XPMR_NOISE;
	if (!strcasecmp(value, "vox"))
		return CD_XPMR_VOX;
	if (!strcasecmp(value, "pp"))
		return CD_PP;
	if (!strcasecmp(value, "ppinvert"))
		return CD_PP_INVERT;
	return CD_IGNORE;
}

enum radio_carrier_detect effective_rxcdtype(const struct chan_usbradio_pvt *o)
{
	return o->rxcdtype;
}

void refresh_processing_hardware(struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	int rx, a, b, route_a, route_b;
	int output_gain_a, output_gain_b;

	if (!o || !o->radio)
		return;
	/* Take one immutable settings copy so a reload cannot mix old and new
	 * hardware fields within one mixer update. This control-plane operation may
	 * call mixer_write(), which takes an adapter lock on modern devices. */
	usbradioplus_processing_get_hardware_rt(o->name, &hardware);
	rx = urp_gain_db_to_mixer(hardware.input_gain_db);
	a = urp_gain_db_to_mixer(hardware.output_a_gain_db);
	b = urp_gain_db_to_mixer(hardware.output_b_gain_db);
	route_a = hardware.output_a_assignment;
	route_b = hardware.output_b_assignment;
	/* A changed code map must go through radio_config().  Besides selecting the
	 * direction-specific sources, that routine owns the parser's control-plane
	 * quiesce protocol.  Calling urp_radio_parse_codes() here used to free a
	 * decoder filter while the hardware callback could still be using it. */
	if (!o->remoted && (strcmp(o->rxctcssfreqs, o->plus_applied_rxctcssfreqs) ||
			    strcmp(o->txctcssfreqs, o->plus_applied_txctcssfreqs)))
		(void)radio_config(o);
	if (o->plus_hardware_applied && rx == o->plus_applied_rxmixer &&
	    a == o->plus_applied_txmixaset && b == o->plus_applied_txmixbset &&
	    route_a == atomic_load_explicit(&o->plus_applied_txmixa, memory_order_acquire) &&
	    route_b == atomic_load_explicit(&o->plus_applied_txmixb, memory_order_acquire))
		return;
	o->plus_applied_rxmixer = rx;
	o->plus_applied_txmixaset = a;
	o->plus_applied_txmixbset = b;
	mixer_write(o);
	output_gain_a = urp_hardware_level_multiplier((a * 152) / AUDIO_ADJUSTMENT);
	output_gain_b = route_a == route_b
				? output_gain_a
				: urp_hardware_level_multiplier((b * 152) / AUDIO_ADJUSTMENT);
	o->radio->txOutputGainA = output_gain_a;
	o->radio->txOutputGainB = output_gain_b;
	/* Publish a coherent snapshot only after the device has accepted the mixer
	 * state. The native tick retries an odd generation instead of taking either
	 * device_lock or settings_lock in the audio callback. */
	atomic_fetch_add_explicit(&o->plus_hardware_generation, 1U, memory_order_release);
	atomic_store_explicit(&o->plus_applied_tx_output_gain_a, output_gain_a,
			      memory_order_relaxed);
	atomic_store_explicit(&o->plus_applied_tx_output_gain_b, output_gain_b,
			      memory_order_relaxed);
	atomic_store_explicit(&o->plus_applied_txmixa, route_a, memory_order_relaxed);
	atomic_store_explicit(&o->plus_applied_txmixb, route_b, memory_order_relaxed);
	atomic_fetch_add_explicit(&o->plus_hardware_generation, 1U, memory_order_release);
	o->plus_hardware_applied = 1;
}

int usbradioplus_refresh_all_processing_hardware(void)
{
	struct chan_usbradio_pvt *channel;

	for (channel = usbradioplus_channel_first(); channel; channel = channel->next)
		refresh_processing_hardware(channel);
	return 0;
}

void store_txtoctype(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_tone_off_mode mode;
	if (urp_parse_tone_off_mode(s, &mode))
		ast_log(LOG_WARNING, "Unrecognized txtoctype parameter: %s\n", s);
	else
		o->txtoctype = (enum usbradio_carrier_type)mode;
}

void tune_txoutput(struct chan_usbradio_pvt *o, int value, int fd, int intflag)
{
	(void)value;
	atomic_store_explicit(&o->txtestkey, 1, memory_order_release);
	atomic_store_explicit(&o->plus_test_tone_enabled, 1, memory_order_release);
	if (fd > 0) {
		ast_cli(fd, "Tone output starting on channel %s...\n", o->name);
		if (ast_radio_wait_or_poll(fd, 5000, intflag)) {
			atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
			atomic_store_explicit(&o->plus_test_tone_enabled, 0, memory_order_release);
		}
	} else
		usleep(5000000);
	atomic_store_explicit(&o->plus_test_tone_enabled, 0, memory_order_release);
	if (fd > 0) {
		ast_cli(fd, "Tone output ending on channel %s...\n", o->name);
	}
	atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
	o->plus_test_tone_phase = 0.0;
}

void tune_rxdisplay(int fd, struct chan_usbradio_pvt *o)
{
	int j, waskeyed, meas, ncols = 75;
	char str[256];

	ast_cli(fd, "RX VOICE DISPLAY:\n");
	ast_cli(fd, "                                 v -- 3KHz        v -- 5KHz\n");

	if (!o->radio->spsMeasure) {
		ast_cli(fd, "ERROR: NO MEASURE BLOCK.\n");
		return;
	}

	if (!o->radio->spsMeasure->source || !o->radio->prxVoiceAdjust) {
		ast_cli(fd, "ERROR: NO SOURCE OR MEASURE SETTING.\n");
		return;
	}

	o->radio->spsMeasure->source = o->radio->spsRxOut->sink;

	o->radio->spsMeasure->enabled = 1;
	o->radio->spsMeasure->discfactor = 1000;

	waskeyed = !o->rxkeyed;
	for (;;) {
		o->radio->spsMeasure->amax = o->radio->spsMeasure->amin = 0;
		if (ast_radio_poll_input(fd, 100)) {
			break;
		}
		if (o->rxkeyed != waskeyed) {
			for (j = 0; j < ncols; j++) {
				str[j] = ' ';
			}
			str[j] = 0;
			ast_cli(fd, " %s \r", str);
		}
		waskeyed = o->rxkeyed;
		if (!o->rxkeyed) {
			ast_cli(fd, "\r");
			continue;
		}
		meas = o->radio->spsMeasure->apeak;
		for (j = 0; j < ncols; j++) {
			int thresh = (meas * ncols) / 16384;
			if (j < thresh) {
				str[j] = '=';
			} else if (j == thresh) {
				str[j] = '>';
			} else {
				str[j] = ' ';
			}
		}
		str[j] = 0;
		ast_cli(fd, "|%s|\r", str);
	}
	o->radio->spsMeasure->enabled = 0;
}

void tune_rxtx_status(int fd, struct chan_usbradio_pvt *o)
{
	int wasverbose;

	ast_cli(fd, "Receiver/Transmitter Status Display:\n");
	ast_cli(fd, "  COS   | CTCSS  | COS   | PTT\n");
	ast_cli(fd, " Input  | Input  | Out   | Out\n");

	wasverbose = option_verbose;
	option_verbose = 0;

	for (;;) {
		/* If they press any key, exit live display */
		if (ast_radio_poll_input(fd, 200)) {
			break;
		}
		ast_cli(fd, " %s  | %s  | %s | %s\r",
			o->rxcdtype ? (o->rx_cos_active ? "Keyed" : "Clear") : "Off  ",
			o->rxsdtype ? (o->rx_ctcss_active ? "Keyed" : "Clear") : "Off  ",
			o->rxkeyed ? "Keyed" : "Clear",
			(atomic_load_explicit(&o->txkeyed, memory_order_acquire) ||
			 atomic_load_explicit(&o->txtestkey, memory_order_acquire))
				? "Keyed"
				: "Clear");
	}

	option_verbose = wasverbose;
}

int parse_tune_level(const char *text, int *level)
{
	char *end;
	unsigned long value;

	if (!text || !*text || !level)
		return -1;
	value = strtoul(text, &end, 10);
	if (*end || value > 999)
		return -1;
	*level = (int)value;
	return 0;
}

void _menu_rxsquelch(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i;

	if (!str[0]) {
		ast_cli(fd, "Current Signal Strength is %d\n",
			((32767 - o->radio->rxRssi) * 1000 / 32767));
		ast_cli(fd, "Current Squelch setting is %d\n", o->rxsquelchadj);
		return;
	}
	if (parse_tune_level(str, &i)) {
		ast_cli(fd, "Entry Error, Rx Squelch Level setting not changed\n");
		return;
	}
	ast_cli(fd, "Changed Rx Squelch Level setting to %d\n", i);
	o->rxsquelchadj = i;
	/* adjust settings based on the device */
	*(o->radio->prxSquelchAdjust) = ((999 - i) * 32767) / AUDIO_ADJUSTMENT;
}

void _menu_txvoice(int fd, struct chan_usbradio_pvt *o, const char *cstr)
{
	const char *str = cstr;
	int i, j, dokey, withctcss;

	if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) &&
	    (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
		ast_cli(fd, "Error, No txvoice output configured.\n");
		return;
	}
	if (!str[0]) {
		if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
			ast_cli(fd, "Current Tx Voice Level setting on Channel A is %d\n",
				o->txmixaset);
		} else {
			ast_cli(fd, "Current Tx Voice Level setting on Channel B is %d\n",
				o->txmixbset);
		}
		return;
	}
	if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
		j = o->txmixaset;
	} else {
		j = o->txmixbset;
	}
	dokey = 0;
	if (str[0] == 'K') {
		dokey = 1;
		str++;
	}
	withctcss = 0;
	if (str[0] == 'C') {
		withctcss = 1;
		str++;
	}
	if (!str[0]) {
		ast_cli(fd, "Keying Transmitter and sending 1000 Hz tone for 5 seconds...\n");
		if (withctcss) {
			o->radio->b.txCtcssInhibit = 1;
		}
		tune_txoutput(o, j, fd, 1);
		o->radio->b.txCtcssInhibit = 0;
		ast_cli(fd, "DONE.\n");
		return;
	}
	if (parse_tune_level(str, &i)) {
		ast_cli(fd, "Entry Error, Tx Voice Level setting not changed\n");
		return;
	}
	if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
		o->txmixaset = i;
		ast_cli(fd, "Changed Tx Voice Level setting on Channel A to %d\n", o->txmixaset);
	} else {
		o->txmixbset = i;
		ast_cli(fd, "Changed Tx Voice Level setting on Channel B to %d\n", o->txmixbset);
	}
	mixer_write(o);
	mult_set(o);
	if (dokey) {
		ast_cli(fd, "Keying Transmitter and sending 1000 Hz tone for 5 seconds...\n");
		if (!withctcss) {
			o->radio->b.txCtcssInhibit = 1;
		}
		tune_txoutput(o, i, fd, 1);
		o->radio->b.txCtcssInhibit = 0;
		ast_cli(fd, "DONE.\n");
	}
}

void _menu_auxvoice(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i;

	if ((o->txmixa != TX_OUT_AUX) && (o->txmixb != TX_OUT_AUX)) {
		ast_cli(fd, "Error, No Auxvoice output configured.\n");
		return;
	}
	if (!str[0]) {
		if (o->txmixa == TX_OUT_AUX) {
			ast_cli(fd, "Current Aux Voice Level setting on Channel A is %d\n",
				o->txmixaset);
		} else {
			ast_cli(fd, "Current Aux Voice Level setting on Channel B is %d\n",
				o->txmixbset);
		}
		return;
	}
	if (parse_tune_level(str, &i)) {
		ast_cli(fd, "Entry Error, Aux Voice Level setting not changed\n");
		return;
	}
	if (o->txmixa == TX_OUT_AUX) {
		o->txmixaset = i;
		ast_cli(fd, "Changed Aux Voice setting on Channel A to %d\n", o->txmixaset);
	} else {
		o->txmixbset = i;
		ast_cli(fd, "Changed Aux Voice setting on Channel B to %d\n", o->txmixbset);
	}
	mixer_write(o);
	mult_set(o);
}

void _menu_txtone(int fd, struct chan_usbradio_pvt *o, const char *cstr)
{
	const char *str = cstr;
	int i, dokey;

	if (!str[0]) {
		ast_cli(fd, "Current Tx CTCSS Modulation Level setting = %d\n", o->txctcssadj);
		return;
	}
	dokey = 0;
	if (str[0] == 'K') {
		dokey = 1;
		str++;
	}
	if (str[0]) {
		if (parse_tune_level(str, &i)) {
			ast_cli(fd, "Entry Error, Tx CTCSS Modulation Level setting not changed\n");
			return;
		}
		o->txctcssadj = i;
		o->ctcss_level = 32767.0 * (double)i / 999.0;
		set_txctcss_level(o);
		ast_cli(fd, "Changed Tx CTCSS Modulation Level setting to %i\n", i);
	}
	if (dokey) {
		ast_cli(fd, "Keying Radio and sending CTCSS tone for 5 seconds...\n");
		atomic_store_explicit(&o->txtestkey, 1, memory_order_release);
		ast_radio_wait_or_poll(fd, 5000, 1);
		atomic_store_explicit(&o->txtestkey, 0, memory_order_release);
		ast_cli(fd, "DONE.\n");
	}
}

void tune_rxvoice(int fd, struct chan_usbradio_pvt *o, int intflag)
{
	const int target = 7200;   /* peak */
	const int tolerance = 360; /* peak to peak */
	const float settingmin = 0.1;
	const float settingmax = 5;
	const float settingstart = 1;
	const int maxtries = 12;

	float setting;

	int tries = 0, meas;

	ast_cli(fd, "INFO: RX VOICE ADJUST START.\n");
	ast_cli(fd, "target=%i tolerance=%i \n", target, tolerance);

	o->radio->b.tuning = 1;
	if (!o->radio->spsMeasure) {
		ast_cli(fd, "ERROR: NO MEASURE BLOCK.\n");
		o->radio->b.tuning = 0;
		return;
	}

	if (!o->radio->spsMeasure->source || !o->radio->prxVoiceAdjust) {
		ast_cli(fd, "ERROR: NO SOURCE OR MEASURE SETTING.\n");
		o->radio->b.tuning = 0;
		return;
	}

	o->radio->spsMeasure->source = o->radio->spsRxOut->sink;
	o->radio->spsMeasure->enabled = 1;
	o->radio->spsMeasure->discfactor = 1000;

	setting = settingstart;

	while (tries < maxtries) {
		*(o->radio->prxVoiceAdjust) = setting * M_Q8;
		if (ast_radio_wait_or_poll(fd, 10, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		o->radio->spsMeasure->amax = o->radio->spsMeasure->amin = 0;
		if (ast_radio_wait_or_poll(fd, 1000, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		meas = o->radio->spsMeasure->apeak;
		ast_cli(fd, "tries=%i, setting=%f, meas=%i\n", tries, setting, meas);

		if (meas < (target - tolerance) || meas > (target + tolerance) || tries < 3) {
			setting = setting * target / meas;
		} else if (tries > 4 && meas > (target - tolerance) &&
			   meas < (target + tolerance)) {
			break;
		}
		if (setting < settingmin) {
			setting = settingmin;
		} else if (setting > settingmax) {
			setting = settingmax;
		}

		tries++;
	}

	o->radio->spsMeasure->enabled = 0;

	ast_cli(fd, "DONE tries=%i, setting=%f, meas=%f\n", tries, setting, (float)meas);
	if (meas < (target - tolerance) || meas > (target + tolerance)) {
		ast_cli(fd, "ERROR: RX VOICE GAIN ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX VOICE GAIN ADJUST SUCCESS.\n");
		usbradioplus_processing_set_local_input_gain(
			o->name, 20.0 * log10(fmax(0.000001, 2.0 * setting)));
	}
	o->radio->b.tuning = 0;
}

void tune_rxctcss(int fd, struct chan_usbradio_pvt *o, int intflag)
{
	const int target = 2400; /* was 4096 pre 20080205 */
	const int tolerance = 100;
	const float settingmin = 0.1;
	const float settingmax = 8;
	const float settingstart = 1;
	const int maxtries = 12;

	float setting;
	int tries = 0, meas;

	ast_cli(fd, "INFO: RX CTCSS ADJUST START.\n");
	ast_cli(fd, "target=%i tolerance=%i \n", target, tolerance);

	o->radio->b.tuning = 1;
	o->radio->spsMeasure->source = o->radio->prxCtcssMeasure;
	o->radio->spsMeasure->discfactor = 400;
	o->radio->spsMeasure->enabled = 1;

	setting = settingstart;

	while (tries < maxtries) {
		*(o->radio->prxCtcssAdjust) = setting * M_Q8;
		if (ast_radio_wait_or_poll(fd, 10, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		o->radio->spsMeasure->amax = o->radio->spsMeasure->amin = 0;
		if (ast_radio_wait_or_poll(fd, 500, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		meas = o->radio->spsMeasure->apeak;
		ast_cli(fd, "tries=%i, setting=%f, meas=%i\n", tries, setting, meas);

		if (meas < (target - tolerance) || meas > (target + tolerance) || tries < 3) {
			setting = setting * target / meas;
		} else if (tries > 4 && meas > (target - tolerance) &&
			   meas < (target + tolerance)) {
			break;
		}
		if (setting < settingmin) {
			setting = settingmin;
		} else if (setting > settingmax) {
			setting = settingmax;
		}

		tries++;
	}
	o->radio->spsMeasure->enabled = 0;
	ast_cli(fd, "DONE tries=%i, setting=%f, meas=%.2f\n", tries, setting, (float)meas);
	if (meas < (target - tolerance) || meas > (target + tolerance)) {
		ast_cli(fd, "ERROR: RX CTCSS GAIN ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX CTCSS GAIN ADJUST SUCCESS.\n");
		o->rxctcssadj = setting;
	}

	if (o->rxcdtype == CD_XPMR_NOISE) {
		int normRssi;

		if (ast_radio_wait_or_poll(fd, 200, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}

		normRssi = ((32767 - o->radio->rxRssi) * AUDIO_ADJUSTMENT / 32767);

		if (o->rxsquelchadj > normRssi) {
			ast_cli(fd,
				"WARNING: RSSI=%i SQUELCH=%i and is too tight. Use 'radio tune "
				"rxsquelch'.\n",
				normRssi, o->rxsquelchadj);
		} else {
			ast_cli(fd, "INFO: RX RSSI=%i\n", normRssi);
		}
	}
	o->radio->b.tuning = 0;
}

void mult_set(struct chan_usbradio_pvt *o)
{
	o->radio->txOutputGainA =
		urp_hardware_level_multiplier((effective_txmixaset(o) * 152) / AUDIO_ADJUSTMENT);
	/* Matching output routes share channel A's gain to keep both DAC legs equal. */
	o->radio->txOutputGainB =
		effective_txmixa(o) == effective_txmixb(o)
			? o->radio->txOutputGainA
			: urp_hardware_level_multiplier((effective_txmixbset(o) * 152) /
							AUDIO_ADJUSTMENT);
}

void usbradioplus_program_radio(struct chan_usbradio_pvt *o)
{
	uint32_t rx_freq;
	uint32_t tx_freq;
	int high_power;

	if (!o)
		return;
	rx_freq = o->remoted ? o->set_rxfreq : o->rxfreq;
	tx_freq = o->remoted ? o->set_txfreq : o->txfreq;
	high_power = o->remoted ? o->set_txpower : 0;
	/* The physical bus belongs to the HID worker. The odd generation brackets
	 * a coherent control-plane request without making an audio callback wait. */
	atomic_fetch_add_explicit(&o->plus_radio_program_generation, 1U, memory_order_release);
	atomic_store_explicit(&o->plus_radio_program_rx_frequency, rx_freq, memory_order_relaxed);
	atomic_store_explicit(&o->plus_radio_program_tx_frequency, tx_freq, memory_order_relaxed);
	atomic_store_explicit(&o->plus_radio_program_high_power, high_power, memory_order_relaxed);
	atomic_fetch_add_explicit(&o->plus_radio_program_generation, 1U, memory_order_release);
}

void usbradioplus_parallel_program_write(void *opaque, uint8_t value)
{
	(void)opaque;
	pp_val = (int8_t)value;
	ast_radio_ppwrite(haspp, ppfd, pbase, pport, value);
}

void usbradioplus_set_channel(uint8_t channel)
{
	struct urp_parallel_bus bus;
	if (!haspp)
		return;
	ast_mutex_lock(&pp_lock);
	bus.value = (uint8_t)pp_val;
	bus.write = usbradioplus_parallel_program_write;
	bus.opaque = NULL;
	urp_hardware_set_channel(&bus, channel);
	pp_val = (int8_t)bus.value;
	ast_mutex_unlock(&pp_lock);
}

/* Unit-only control-plane hooks make race outcomes deterministic without being
 * present in the installed module. */
#ifdef URP_PROCESSING_TESTING
static atomic_int test_radio_access_force_reader_retry;
#endif

/** @brief Enter the control-plane exclusion window for parser-owned radio state.
 * @param channel Radio whose CTCSS/DCS parser state will be replaced.
 *
 * The native callback never waits for this writer. It observes reconfiguring,
 * emits its ordinary silent frame for that tick, and resumes on the next
 * block. The sequentially consistent writer flag/store and reader-count load
 * pair with acquire's flag load, count increment, and recheck: a reader that
 * starts after the writer's zero-count observation must see the asserted flag
 * on its recheck, while a reader that increments first keeps the writer
 * waiting. This prevents urp_radio_parse_codes() from freeing decoder memory
 * while the callback is using it without a callback lock.
 */
static void radio_access_begin_reconfigure(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_radio_access_slot *slot = &channel->plus_radio_access;

	if (!channel->plus_dsp_initialized)
		return;
	while (atomic_flag_test_and_set_explicit(&slot->writer, memory_order_acquire)) {
		sched_yield();
#ifdef URP_PROCESSING_TESTING
		atomic_flag_clear_explicit(&slot->writer, memory_order_release);
#endif
	}
	atomic_store_explicit(&slot->reconfiguring, 1, memory_order_seq_cst);
	while (atomic_load_explicit(&slot->readers, memory_order_seq_cst) != 0U)
		sched_yield();
}

/** @brief Leave the parser-owned radio-state exclusion window.
 * @param channel Radio whose control-plane reconfiguration is complete.
 */
static void radio_access_end_reconfigure(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_radio_access_slot *slot = &channel->plus_radio_access;

	if (!channel->plus_dsp_initialized)
		return;
	atomic_store_explicit(&slot->reconfiguring, 0, memory_order_seq_cst);
	atomic_flag_clear_explicit(&slot->writer, memory_order_release);
}

int usbradioplus_radio_access_acquire(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_radio_access_slot *slot;

	if (!channel || !channel->radio)
		return 0;
	/* Unit construction and teardown have no active hardware callback. */
	if (!channel->plus_dsp_initialized)
		return 1;
	slot = &channel->plus_radio_access;
	if (atomic_load_explicit(&slot->reconfiguring, memory_order_seq_cst))
		return 0;
	atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst);
#ifdef URP_PROCESSING_TESTING
	if (atomic_exchange_explicit(&test_radio_access_force_reader_retry, 0,
				     memory_order_relaxed))
		atomic_store_explicit(&slot->reconfiguring, 1, memory_order_seq_cst);
#endif
	if (!atomic_load_explicit(&slot->reconfiguring, memory_order_seq_cst))
		return 1;
	atomic_fetch_sub_explicit(&slot->readers, 1U, memory_order_seq_cst);
	return 0;
}

void usbradioplus_radio_access_release(struct chan_usbradio_pvt *channel)
{
	if (!channel || !channel->radio || !channel->plus_dsp_initialized)
		return;
	atomic_fetch_sub_explicit(&channel->plus_radio_access.readers, 1U, memory_order_seq_cst);
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_radio_access_contention_paths(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio = {0};

	channel.radio = &radio;
	channel.plus_dsp_initialized = 1;
	atomic_init(&channel.plus_radio_access.readers, 0U);
	atomic_init(&channel.plus_radio_access.reconfiguring, 0);
	atomic_flag_clear_explicit(&channel.plus_radio_access.writer, memory_order_relaxed);
	atomic_flag_test_and_set_explicit(&channel.plus_radio_access.writer, memory_order_relaxed);
	radio_access_begin_reconfigure(&channel);
	radio_access_end_reconfigure(&channel);
	atomic_store_explicit(&test_radio_access_force_reader_retry, 1, memory_order_relaxed);
	(void)usbradioplus_radio_access_acquire(&channel);
	return (int)atomic_load_explicit(&channel.plus_radio_access.readers, memory_order_seq_cst);
}
#endif

/** @brief Reconfigure DCS only for directions that selected DCS signaling.
 * @param channel Channel holding raw clean-slate DCS controls.
 * @param receive_dcs Nonzero when receive signaling is DCS.
 * @param transmit_dcs Nonzero when transmit signaling is DCS.
 * @return Zero on success; nonzero for an invalid selected DCS code.
 */
static int radio_configure_dcs(struct chan_usbradio_pvt *channel, int receive_dcs, int transmit_dcs)
{
	/* A disabled direction stores five NUL bytes, matching the canonical code field. */
	static const char disabled_dcs_code[5] = "";
	int receive_code = -1;
	int receive_inverted = 0;
	int transmit_code = -1;
	int transmit_inverted = 0;
	urp_radio_state *radio = channel->radio;

	if (receive_dcs &&
	    urp_dcs_parse_code(channel->dcs_receive_code, &receive_code, &receive_inverted))
		return -1;
	if (transmit_dcs &&
	    urp_dcs_parse_code(channel->dcs_transmit_code, &transmit_code, &transmit_inverted))
		return -1;
	urp_dcs_configure(&radio->dcs, receive_code, receive_inverted, transmit_code,
			  transmit_inverted);
	/* Parsed DCS spellings are exactly four characters plus their NUL terminator. */
	memcpy(radio->dcsRxCode, receive_dcs ? channel->dcs_receive_code : disabled_dcs_code,
	       sizeof(radio->dcsRxCode));
	memcpy(radio->dcsTxCode, transmit_dcs ? channel->dcs_transmit_code : disabled_dcs_code,
	       sizeof(radio->dcsTxCode));
	radio->dcsTurnoffEnabled = transmit_dcs && channel->dcs_turnoff_enabled;
	radio->dcsTurnoffDuration = transmit_dcs ? channel->dcs_turnoff_duration_ms : 0;
	radio->dcsTurnoffTimer = 0;
	radio->dcsPeak = transmit_dcs ? channel->dcs_level : 0.0;
	if (!receive_dcs && radio->smode == SMODE_DCS) {
		radio->smode = SMODE_NULL;
		radio->smodewas = SMODE_NULL;
		radio->smodetimer = 0;
	}
	return 0;
}

/** @brief Apply parser and runtime signaling state while the writer gate is held.
 * @param o Private channel whose signaling state is reconfigured.
 * @return Zero on success; nonzero if parser state cannot be rebuilt.
 */
static int radio_config_locked(struct chan_usbradio_pvt *o)
{
	static const char disabled_code[] = "0";
	const int receive_ctcss = ast_strlen_zero(o->receive_signaling_method) ||
				  !strcasecmp(o->receive_signaling_method, "ctcss");
	const int transmit_ctcss = ast_strlen_zero(o->transmit_signaling_method) ||
				   !strcasecmp(o->transmit_signaling_method, "ctcss");
	const int receive_dcs = !strcasecmp(o->receive_signaling_method, "dcs");
	const int transmit_dcs = !strcasecmp(o->transmit_signaling_method, "dcs");
	int parse_result;

	if (o->radio == NULL) {
		ast_log(LOG_ERROR, "native radio state is unavailable\n");
		return 1;
	}

	/* Scalar detector and transmitter controls are live state, unlike the
	 * parser-owned CTCSS lists below.  Update them in the same excluded window
	 * so a native block cannot combine old thresholds with a new decoder map. */
	o->radio->rxDemod = o->rxdemod;
	o->radio->rxCdType = effective_rxcdtype(o);
	o->radio->voxHangTime = o->voxhangtime;
	o->radio->rxSqVoxAdj = o->rxsqvoxadj;
	o->radio->rxCarrierHyst = o->rxsqhyst;
	o->radio->rxNoiseFilType = o->rxnoisefiltype;
	o->radio->rxSquelchDelay = o->rxsquelchdelay;
	o->radio->rxNoiseSquelchEnable = o->radio->rxCdType == CD_XPMR_NOISE;
	o->radio->b.rxpolarity = o->rxpolarity;
	o->radio->b.txpolarity = o->txpolarity;
	o->radio->b.lsdrxpolarity = o->lsdrxpolarity;
	o->radio->b.lsdtxpolarity = o->lsdtxpolarity;
	o->radio->txCpuSaver = o->txcpusaver;
	o->radio->txsettletime = o->txsettletime;
	o->radio->txrxblankingtime = o->txrxblankingtime;
	o->radio->rxCtcss->relax = o->rxctcssrelax;
	o->radio->txTocType = o->txtoctype;
	o->radio->txCtcssTocShift = o->ctcss_phase_shift_degrees;
	o->radio->txCtcssTocTime = o->ctcss_tail_duration_ms;
	o->radio->txCtcssTocToneHz = o->ctcss_tail_frequency_hz;
	/* CTCSS peak is defined at the native PCM output.  Hardware mixer gain
	 * remains an analog-output control and must not alter this configured level. */
	o->radio->txCtcssPeak = transmit_ctcss ? o->ctcss_level : 0.0;
	if (o->remoted) {
		o->radio->pTxCodeDefault = o->set_txctcssdefault;
		o->radio->pRxCodeSrc = o->set_rxctcssfreqs;
		o->radio->pTxCodeSrc = o->set_txctcssfreqs;

	} else {
		/* Keep the configured RX and TX CTCSS data independent.  The parser
		 * receives a disabled companion list for a receive-only CTCSS mode,
		 * which deliberately builds CTCSS_RXONLY entries instead of requiring
		 * an artificial transmitter map. */
		const char *rx_source = receive_ctcss ? o->rxctcssfreqs : disabled_code;
		const char *tx_source =
			receive_ctcss && transmit_ctcss ? o->txctcssfreqs : disabled_code;
		const char *default_source = transmit_ctcss ? o->txctcssdefault : disabled_code;
		if (ast_strlen_zero(rx_source))
			rx_source = disabled_code;
		if (ast_strlen_zero(tx_source))
			tx_source = disabled_code;
		if (ast_strlen_zero(default_source))
			default_source = disabled_code;
		o->radio->pTxCodeDefault = (char *)default_source;
		ast_copy_string(o->plus_applied_rxctcssfreqs, rx_source,
				sizeof(o->plus_applied_rxctcssfreqs));
		o->radio->pRxCodeSrc = o->plus_applied_rxctcssfreqs;
		ast_copy_string(o->plus_applied_txctcssfreqs, tx_source,
				sizeof(o->plus_applied_txctcssfreqs));
		o->radio->pTxCodeSrc = o->plus_applied_txctcssfreqs;
	}

	if (o->forcetxcode) {
		o->radio->pTxCodeDefault = o->set_txctcssfreq;
		ast_debug(3, "Channel %s: Forced Tx Squelch Code code=%s.\n", o->name,
			  o->radio->pTxCodeDefault);
	}

	parse_result = urp_radio_parse_codes(o->radio);
	if (parse_result)
		return 1;
	/* Parsing resets the CTCSS filter gain.  Restore the selected decoder and
	 * transmitter calibration before audio readers can observe this generation. */
	if (o->radio->prxCtcssAdjust)
		*o->radio->prxCtcssAdjust = (i32)lround(o->rxctcssadj * M_Q8);
	if (o->radio->prxSquelchAdjust)
		*o->radio->prxSquelchAdjust = ((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
	if (!receive_ctcss && o->radio->smode == SMODE_CTCSS) {
		o->radio->smode = SMODE_NULL;
		o->radio->smodewas = SMODE_NULL;
		o->radio->smodetimer = 0;
	}
	if (!transmit_ctcss) {
		o->radio->txCtcssEnabled = 0;
		o->radio->txCtcssOption = 3;
		o->radio->txCtcssState = 0;
		o->radio->txCtcssTurnoffTimer = 0;
		o->radio->txCtcssTailToneHz = 0.0;
	}
	if (radio_configure_dcs(o, receive_dcs, transmit_dcs))
		return 1;
	usbradioplus_program_radio(o);

	return 0;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_radio_config_locked(struct chan_usbradio_pvt *channel)
{
	return radio_config_locked(channel);
}
#endif

int radio_config(struct chan_usbradio_pvt *o)
{
	int result;

	if (!o || !o->radio) {
		ast_log(LOG_ERROR, "native radio state is unavailable\n");
		return 1;
	}
	radio_access_begin_reconfigure(o);
	result = radio_config_locked(o);
	radio_access_end_reconfigure(o);
	return result;
}

/** @brief Validate a comma-separated CTCSS list against the radio tone table.
 * @param frequencies Candidate comma-separated CTCSS frequencies in hertz.
 * @param count Receives the number of valid entries when non-NULL.
 * @return Nonzero when every entry is a supported CTCSS tone and the list is nonempty.
 *
 * The processing-file parser intentionally owns syntax validation.  This
 * channel-side check adds the radio-specific table lookup before the signaling
 * engine is created, preventing an accepted numeric value from being ignored
 * later by the legacy-compatible code-map parser.
 */
static int ctcss_frequency_list_valid(const char *frequencies, size_t *count)
{
	const char *cursor = frequencies;
	size_t entries = 0;

	if (ast_strlen_zero(cursor))
		return 0;
	for (;;) {
		char *end;
		double frequency;

		errno = 0;
		frequency = strtod(cursor, &end);
		if (end == cursor || errno == ERANGE || !isfinite(frequency) ||
		    !urp_ctcss_frequency_supported((float)frequency))
			return 0;
		++entries;
		cursor = end;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (*cursor == '\0')
			break;
		if (*cursor != ',')
			return 0;
		++cursor;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (*cursor == '\0')
			return 0;
	}
	if (count)
		*count = entries;
	return 1;
}

/** @brief Require an equal-length receive-to-transmit CTCSS translation map.
 * @param receive_frequencies Receive CTCSS frequencies in hertz.
 * @param transmit_frequencies Corresponding transmit CTCSS frequencies in hertz.
 * @return Nonzero when both supported-tone lists have the same number of entries.
 */
static int ctcss_frequency_lists_mapped(const char *receive_frequencies,
					const char *transmit_frequencies)
{
	size_t receive_count;
	size_t transmit_count;

	return ctcss_frequency_list_valid(receive_frequencies, &receive_count) &&
	       ctcss_frequency_list_valid(transmit_frequencies, &transmit_count) &&
	       receive_count == transmit_count;
}

/** @brief Validate one configured CTCSS frequency.
 * @param frequency Candidate single CTCSS frequency in hertz.
 * @return Nonzero when the value names exactly one supported tone.
 */
static int ctcss_frequency_valid(const char *frequency)
{
	size_t count;

	return ctcss_frequency_list_valid(frequency, &count) && count == 1;
}

/** @brief Check the configured spelling of one DCS code.
 * @param code Candidate three octal digits followed by N or I.
 * @return Nonzero for a valid syntax and polarity suffix.
 */
static int dcs_code_valid(const char *code)
{
	return code && strlen(code) == 4 && code[0] >= '0' && code[0] <= '7' && code[1] >= '0' &&
	       code[1] <= '7' && code[2] >= '0' && code[2] <= '7' &&
	       (code[3] == 'N' || code[3] == 'n' || code[3] == 'I' || code[3] == 'i');
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_ctcss_frequency_list_valid(const char *frequencies, size_t *count)
{
	return ctcss_frequency_list_valid(frequencies, count);
}

int usbradioplus_test_ctcss_frequency_lists_mapped(const char *receive_frequencies,
						   const char *transmit_frequencies)
{
	return ctcss_frequency_lists_mapped(receive_frequencies, transmit_frequencies);
}

int usbradioplus_test_ctcss_frequency_valid(const char *frequency)
{
	return ctcss_frequency_valid(frequency);
}

int usbradioplus_test_dcs_code_valid(const char *code)
{
	return dcs_code_valid(code);
}
#endif

/** @brief Report a complete signaling selection that lacks required inputs.
 * @param category Configured radio channel/profile name.
 * @param message Operator-actionable explanation of the missing or invalid setting.
 */
static void signaling_configuration_error(const char *category, const char *message)
{
	ast_log(LOG_ERROR, "RadioPlus/%s: %s\n", category, message);
}

/** Candidate clean-slate signaling values resolved before touching a live radio. */
struct processing_signaling_values {
	/** Selected receive signaling method. */
	char receive_method[8];
	/** Selected transmit signaling method. */
	char transmit_method[8];
	/** Receive CTCSS frequency list. */
	char receive_ctcss[512];
	/** Receive-to-transmit CTCSS frequency map. */
	char transmit_ctcss[512];
	/** Default transmit CTCSS frequency. */
	char transmit_ctcss_default[16];
	/** Receive DCS code and polarity suffix. */
	char dcs_receive_code[5];
	/** Transmit DCS code and polarity suffix. */
	char dcs_transmit_code[5];
	/** Resolved receive audio-source mode. */
	enum radio_rx_audio rxdemod;
	/** Resolved carrier-detection source. */
	enum radio_carrier_detect rxcdtype;
	/** Resolved CTCSS indication source. */
	enum radio_squelch_detect rxsdtype;
	/** Resolved CTCSS turn-off mode. */
	enum usbradio_carrier_type txtoctype;
	/** Receive CPU-saver selection. */
	int rxcpusaver;
	/** Transmit CPU-saver selection. */
	int txcpusaver;
	/** VOX release hold in milliseconds. */
	int voxhangtime;
	/** VOX threshold. */
	int rxsqvoxadj;
	/** Noise-squelch hysteresis. */
	int rxsqhyst;
	/** Noise-detector filter selection. */
	int rxnoisefiltype;
	/** Receive squelch delay in milliseconds. */
	int rxsquelchdelay;
	/** Carrier-on delay in 20 ms frames. */
	int rxondelay;
	/** Nonzero inverts receive signaling. */
	int rxpolarity;
	/** Receive noise-squelch threshold. */
	int rxsquelchadj;
	/** Configured receive frequency in hertz. */
	int rxfreq;
	/** Nonzero inverts receive low-speed data. */
	int lsdrxpolarity;
	/** Nonzero enables transmitter pre-emphasis. */
	int txpreemphasis;
	/** Transmitter settling time in milliseconds. */
	int txsettletime;
	/** Transmit/receive blanking time in milliseconds. */
	int txrxblankingtime;
	/** Post-transmit receiver delay in frames. */
	int txoffdelay;
	/** Nonzero inverts transmit signaling. */
	int txpolarity;
	/** Configured transmit frequency in hertz. */
	int txfreq;
	/** Nonzero inverts transmit low-speed data. */
	int lsdtxpolarity;
	/** Linear receive CTCSS decoder gain. */
	float rxctcssadj;
	/** Nonzero bypasses CTCSS qualification. */
	int rxctcssoverride;
	/** CTCSS decoder talk-off tolerance. */
	int rxctcssrelax;
	/** Transmit CTCSS peak level in PCM codes. */
	double ctcss_level;
	/** CTCSS phase-shift tail angle in degrees. */
	double ctcss_phase_shift_degrees;
	/** CTCSS tail duration in milliseconds. */
	int ctcss_tail_duration_ms;
	/** CTCSS replacement-tail frequency in hertz. */
	double ctcss_tail_frequency_hz;
	/** Nonzero enables the DCS end-of-transmission tone. */
	int dcs_turnoff_enabled;
	/** DCS end-of-transmission tone duration in milliseconds. */
	int dcs_turnoff_duration_ms;
	/** DCS peak level in PCM codes. */
	double dcs_level;
};

/** @brief Read one validated boolean from a candidate signaling profile.
 * @param category Named channel/profile being resolved.
 * @param section Configuration section containing the option.
 * @param name Option name.
 * @param result Receives the resolved boolean.
 * @return Zero on success; nonzero for an absent or invalid value.
 */
static int processing_signaling_bool(const char *category, const char *section, const char *name,
				     int *result)
{
	char value[64];

	if (usbradioplus_processing_get_option(category, section, name, value, sizeof(value)) ||
	    (!ast_true(value) && !ast_false(value)))
		return -1;
	*result = ast_true(value);
	return 0;
}

/** @brief Read one validated integer from a candidate signaling profile.
 * @param category Named channel/profile being resolved.
 * @param section Configuration section containing the option.
 * @param name Option name.
 * @param result Receives the resolved integer.
 * @return Zero on success; nonzero for an absent or invalid value.
 */
static int processing_signaling_int(const char *category, const char *section, const char *name,
				    int *result)
{
	char value[64];
	char *end;
	long number;

	if (usbradioplus_processing_get_option(category, section, name, value, sizeof(value)))
		return -1;
	number = strtol(value, &end, 0);
	if (end == value || *end)
		return -1;
	*result = (int)number;
	return 0;
}

/** @brief Read one validated floating-point value from a candidate profile.
 * @param category Named channel/profile being resolved.
 * @param section Configuration section containing the option.
 * @param name Option name.
 * @param result Receives the resolved finite value.
 * @return Zero on success; nonzero for an absent or invalid value.
 */
static int processing_signaling_double(const char *category, const char *section, const char *name,
				       double *result)
{
	char value[64];
	char *end;

	if (usbradioplus_processing_get_option(category, section, name, value, sizeof(value)))
		return -1;
	*result = strtod(value, &end);
	return end == value || *end || !isfinite(*result) ? -1 : 0;
}

/** @def SIGNALING_STRING(section, name, field)
 * @brief Copy one resolved string option into a signaling candidate.
 */
/** @def SIGNALING_BOOL(section, name, field)
 * @brief Read one resolved boolean option into a signaling candidate.
 */
/** @def SIGNALING_INT(section, name, field)
 * @brief Read one resolved integer option into a signaling candidate.
 */

/** @brief Resolve and validate live-safe clean-slate signaling controls.
 * @param category Named channel/profile being resolved.
 * @param resolved Receives the candidate values without changing live state.
 * @return Zero on success; nonzero when any value or cross-field rule is invalid.
 */
static int resolve_processing_signaling(const char *category,
					struct processing_signaling_values *resolved)
{
	struct usbradioplus_hardware_settings hardware;
	char value[512];
	enum urp_carrier_source carrier;
	enum urp_ctcss_source ctcss_source;
	enum urp_rx_audio_mode audio;
	enum urp_tone_off_mode tone_off;
	double db;

	if (!category || !resolved)
		return -1;
	memset(resolved, 0, sizeof(*resolved));
/* Copy one resolved string option into a signaling candidate. */
#define SIGNALING_STRING(section, name, field)                                                     \
	do {                                                                                       \
		if (usbradioplus_processing_get_option(category, (section), (name), value,         \
						       sizeof(value)))                             \
			return -1;                                                                 \
		ast_copy_string(resolved->field, value, sizeof(resolved->field));                  \
	} while (0)
/* Read one resolved boolean option into a signaling candidate. */
#define SIGNALING_BOOL(section, name, field)                                                       \
	if (processing_signaling_bool(category, (section), (name), &resolved->field))              \
	return -1
/* Read one resolved integer option into a signaling candidate. */
#define SIGNALING_INT(section, name, field)                                                        \
	if (processing_signaling_int(category, (section), (name), &resolved->field))               \
	return -1
	SIGNALING_STRING("receive", "signaling_method", receive_method);
	SIGNALING_STRING("transmit", "signaling_method", transmit_method);
	if (strcasecmp(resolved->receive_method, "carrier") &&
	    strcasecmp(resolved->receive_method, "ctcss") &&
	    strcasecmp(resolved->receive_method, "dcs"))
		return -1;
	if (strcasecmp(resolved->transmit_method, "carrier") &&
	    strcasecmp(resolved->transmit_method, "ctcss") &&
	    strcasecmp(resolved->transmit_method, "dcs"))
		return -1;
	/* Resolve the candidate's output assignments with its signaling controls.
	 * A live reload must not accept CTCSS or DCS transmit selection that has no
	 * configured path to the CM119 DAC. */
	if (usbradioplus_processing_get_hardware(category, &hardware) ||
	    urp_tx_signaling_route_missing(!strcasecmp(resolved->transmit_method, "ctcss") ||
						   !strcasecmp(resolved->transmit_method, "dcs"),
					   (enum urp_tx_output_mode)hardware.output_a_assignment,
					   (enum urp_tx_output_mode)hardware.output_b_assignment))
		return -1;
	SIGNALING_BOOL("receive", "cpu_saver_enabled", rxcpusaver);
	SIGNALING_BOOL("transmit", "cpu_saver_enabled", txcpusaver);
	if (usbradioplus_processing_get_option(category, "receive", "audio_source", value,
					       sizeof(value)) ||
	    urp_parse_rx_audio_mode(value, &audio))
		return -1;
	resolved->rxdemod = (enum radio_rx_audio)audio;
	if (usbradioplus_processing_get_option(category, "receive", "cos_assignment", value,
					       sizeof(value)) ||
	    urp_parse_carrier_source(value, &carrier))
		return -1;
	resolved->rxcdtype = (enum radio_carrier_detect)carrier;
	if (usbradioplus_processing_get_option(category, "ctcss", "receive_source", value,
					       sizeof(value)) ||
	    urp_parse_ctcss_source(value, &ctcss_source))
		return -1;
	resolved->rxsdtype = (enum radio_squelch_detect)ctcss_source;
	SIGNALING_INT("receive", "vox_hang_ms", voxhangtime);
	SIGNALING_INT("receive", "vox_threshold", rxsqvoxadj);
	SIGNALING_INT("receive", "noise_squelch_hysteresis", rxsqhyst);
	SIGNALING_INT("receive", "noise_filter_type", rxnoisefiltype);
	SIGNALING_INT("receive", "squelch_delay_ms", rxsquelchdelay);
	SIGNALING_INT("receive", "on_delay_frames", rxondelay);
	SIGNALING_BOOL("receive", "polarity_inverted", rxpolarity);
	SIGNALING_INT("receive", "squelch_level", rxsquelchadj);
	SIGNALING_INT("receive", "frequency_hz", rxfreq);
	SIGNALING_BOOL("receive", "lsd_polarity_inverted", lsdrxpolarity);
	SIGNALING_BOOL("transmit", "preemphasis_enabled", txpreemphasis);
	SIGNALING_INT("transmit", "settle_ms", txsettletime);
	SIGNALING_INT("transmit", "rx_blanking_ms", txrxblankingtime);
	SIGNALING_INT("transmit", "off_delay_frames", txoffdelay);
	SIGNALING_BOOL("transmit", "polarity_inverted", txpolarity);
	SIGNALING_INT("transmit", "frequency_hz", txfreq);
	SIGNALING_BOOL("transmit", "lsd_polarity_inverted", lsdtxpolarity);
	SIGNALING_STRING("ctcss", "receive_frequencies", receive_ctcss);
	SIGNALING_STRING("ctcss", "transmit_frequencies", transmit_ctcss);
	SIGNALING_STRING("ctcss", "transmit_default_hz", transmit_ctcss_default);
	if ((!ast_strlen_zero(resolved->receive_ctcss) &&
	     !ctcss_frequency_list_valid(resolved->receive_ctcss, NULL)) ||
	    (!ast_strlen_zero(resolved->transmit_ctcss) &&
	     !ctcss_frequency_list_valid(resolved->transmit_ctcss, NULL)) ||
	    (!ast_strlen_zero(resolved->transmit_ctcss_default) &&
	     !ctcss_frequency_valid(resolved->transmit_ctcss_default)))
		return -1;
	if (processing_signaling_double(category, "ctcss", "receive_decoder_gain_db", &db))
		return -1;
	resolved->rxctcssadj = (float)pow(10.0, db / 20.0);
	SIGNALING_BOOL("ctcss", "receive_override_enabled", rxctcssoverride);
	/* A CTCSS override has no meaning for carrier or DCS receive.  Treat an
	 * inactive setting as inert rather than letting it bypass their decoder. */
	if (strcasecmp(resolved->receive_method, "ctcss"))
		resolved->rxctcssoverride = 0;
	SIGNALING_INT("ctcss", "receive_relax", rxctcssrelax);
	if (processing_signaling_double(category, "ctcss", "transmit_peak_dbfs", &db))
		return -1;
	resolved->ctcss_level = 32767.0 * pow(10.0, db / 20.0);
	if (usbradioplus_processing_get_option(category, "ctcss", "turnoff_mode", value,
					       sizeof(value)) ||
	    urp_parse_tone_off_mode(value, &tone_off))
		return -1;
	resolved->txtoctype = (enum usbradio_carrier_type)tone_off;
	if (processing_signaling_double(category, "ctcss", "phase_shift_degrees",
					&resolved->ctcss_phase_shift_degrees) ||
	    processing_signaling_int(category, "ctcss", "tail_duration_ms",
				     &resolved->ctcss_tail_duration_ms) ||
	    processing_signaling_double(category, "ctcss", "tail_frequency_hz",
					&resolved->ctcss_tail_frequency_hz))
		return -1;
	SIGNALING_STRING("dcs", "receive_code", dcs_receive_code);
	SIGNALING_STRING("dcs", "transmit_code", dcs_transmit_code);
	/* DCS controls are retained across signaling-mode changes, so validate both
	 * directions before selecting either one. This also makes a later DCS mode
	 * switch unable to activate a stale invalid code. */
	if (!dcs_code_valid(resolved->dcs_receive_code) ||
	    !dcs_code_valid(resolved->dcs_transmit_code))
		return -1;
	SIGNALING_BOOL("dcs", "turnoff_code_enabled", dcs_turnoff_enabled);
	SIGNALING_INT("dcs", "turnoff_duration_ms", dcs_turnoff_duration_ms);
	if (processing_signaling_double(category, "dcs", "peak_dbfs", &db))
		return -1;
	resolved->dcs_level = 32767.0 * pow(10.0, db / 20.0);
	if (!strcasecmp(resolved->receive_method, "ctcss") &&
	    (resolved->rxsdtype == SD_IGNORE ||
	     !ctcss_frequency_list_valid(resolved->receive_ctcss, NULL)))
		return -1;
	if (!strcasecmp(resolved->transmit_method, "ctcss") &&
	    !ctcss_frequency_valid(resolved->transmit_ctcss_default))
		return -1;
	if (!strcasecmp(resolved->receive_method, "ctcss") &&
	    !strcasecmp(resolved->transmit_method, "ctcss") &&
	    !ctcss_frequency_lists_mapped(resolved->receive_ctcss, resolved->transmit_ctcss))
		return -1;
#undef SIGNALING_INT
#undef SIGNALING_BOOL
#undef SIGNALING_STRING
	return 0;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_resolve_processing_signaling(const char *category, int null_result)
{
	struct processing_signaling_values resolved;

	return resolve_processing_signaling(category, null_result ? NULL : &resolved);
}
#endif

/** @brief Commit already-validated signaling values while the caller owns the channel.
 * @param o Private channel receiving the validated signaling state.
 * @param resolved Fully validated candidate signaling values.
 */
static void commit_processing_signaling(struct chan_usbradio_pvt *o,
					const struct processing_signaling_values *resolved)
{
	ast_copy_string(o->receive_signaling_method, resolved->receive_method,
			sizeof(o->receive_signaling_method));
	ast_copy_string(o->transmit_signaling_method, resolved->transmit_method,
			sizeof(o->transmit_signaling_method));
	ast_copy_string(o->rxctcssfreqs, resolved->receive_ctcss, sizeof(o->rxctcssfreqs));
	ast_copy_string(o->txctcssfreqs, resolved->transmit_ctcss, sizeof(o->txctcssfreqs));
	ast_copy_string(o->txctcssdefault, resolved->transmit_ctcss_default,
			sizeof(o->txctcssdefault));
	ast_copy_string(o->dcs_receive_code, resolved->dcs_receive_code,
			sizeof(o->dcs_receive_code));
	ast_copy_string(o->dcs_transmit_code, resolved->dcs_transmit_code,
			sizeof(o->dcs_transmit_code));
	o->rxdemod = resolved->rxdemod;
	o->rxcdtype = resolved->rxcdtype;
	o->rxsdtype =
		!strcasecmp(resolved->receive_method, "ctcss") ? resolved->rxsdtype : SD_IGNORE;
	o->rxcpusaver = resolved->rxcpusaver;
	o->txcpusaver = resolved->txcpusaver;
	o->voxhangtime = resolved->voxhangtime;
	o->rxsqvoxadj = resolved->rxsqvoxadj;
	o->rxsqhyst = resolved->rxsqhyst;
	o->rxnoisefiltype = resolved->rxnoisefiltype;
	o->rxsquelchdelay = resolved->rxsquelchdelay;
	o->rxondelay = resolved->rxondelay;
	o->rxpolarity = resolved->rxpolarity;
	o->rxsquelchadj = resolved->rxsquelchadj;
	o->rxfreq = resolved->rxfreq;
	o->lsdrxpolarity = resolved->lsdrxpolarity;
	o->txpreemphasis = resolved->txpreemphasis;
	o->txsettletime = resolved->txsettletime;
	o->txrxblankingtime = resolved->txrxblankingtime;
	o->txoffdelay = resolved->txoffdelay;
	o->txpolarity = resolved->txpolarity;
	o->txfreq = resolved->txfreq;
	o->lsdtxpolarity = resolved->lsdtxpolarity;
	o->rxctcssadj = resolved->rxctcssadj;
	o->rxctcssoverride = resolved->rxctcssoverride;
	o->rxctcssrelax = resolved->rxctcssrelax;
	o->ctcss_level = resolved->ctcss_level;
	o->txtoctype = resolved->txtoctype;
	o->ctcss_phase_shift_degrees = resolved->ctcss_phase_shift_degrees;
	o->ctcss_tail_duration_ms = resolved->ctcss_tail_duration_ms;
	o->ctcss_tail_frequency_hz = resolved->ctcss_tail_frequency_hz;
	o->dcs_turnoff_enabled = resolved->dcs_turnoff_enabled;
	o->dcs_turnoff_duration_ms = resolved->dcs_turnoff_duration_ms;
	o->dcs_level = resolved->dcs_level;
}

int apply_processing_signaling_overrides(struct chan_usbradio_pvt *o, const char *category)
{
	struct processing_signaling_values resolved;

	if (!o || resolve_processing_signaling(category, &resolved))
		return -1;
	/* Commit only after every value has been parsed and cross-validated.  This
	 * keeps a rejected reload from partially changing live receiver signaling. */
	commit_processing_signaling(o, &resolved);
	return 0;
}

int usbradioplus_refresh_all_processing_signaling(void)
{
	struct chan_usbradio_pvt *channel;

	/* Resolve every profile before touching any live radio.  The candidate
	 * settings snapshot is immutable during reload, so a second pass cannot
	 * introduce a semantic failure after an earlier channel has committed. */
	for (channel = usbradioplus_channel_first(); channel; channel = channel->next) {
		struct processing_signaling_values resolved;

		if (channel->radio && resolve_processing_signaling(channel->name, &resolved))
			return -1;
	}
	for (channel = usbradioplus_channel_first(); channel; channel = channel->next) {
		struct processing_signaling_values resolved;
		int result;

		/* Configured-but-uninitialized entries have no audio reader or parser
		 * state yet; store_config() resolves them before creating the engine. */
		if (!channel->radio)
			continue;
		/* The complete candidate was preflighted above.  Keep this defensive
		 * check in case a caller replaces the settings source concurrently. */
		if (resolve_processing_signaling(channel->name, &resolved))
			return -1;
		radio_access_begin_reconfigure(channel);
		commit_processing_signaling(channel, &resolved);
		result = radio_config_locked(channel);
		radio_access_end_reconfigure(channel);
		if (result)
			return -1;
	}
	return 0;
}

int apply_processing_config_overrides(struct chan_usbradio_pvt *o, const char *category)
{
	struct usbradioplus_hardware_settings hardware;
	char value[512];
	char option[64];
	char *end;
	int i;
	long number;
	size_t option_index;
	size_t tone_count;
	static const int parallel_pins[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15};
	static const char *const asterisk_jitter_options[] = {
		"jbenable", "jbmaxsize", "jbresyncthreshold", "jbimpl",
		"jblog",    "jbforce",	 "jbtargetextra",     "jbsyncvideo",
	};
	static const char *const modern_jitter_options[] = {
		"asterisk_jitter_buffer_enabled",
		"asterisk_jitter_buffer_max_size_ms",
		"asterisk_jitter_buffer_resync_threshold_ms",
		"asterisk_jitter_buffer_implementation",
		"asterisk_jitter_buffer_logging_enabled",
		"asterisk_jitter_buffer_force_enabled",
		"asterisk_jitter_buffer_target_extra_ms",
		"asterisk_jitter_buffer_video_sync_enabled",
	};

#define GET(section, name)                                                                         \
	(!usbradioplus_processing_get_option(category, (section), (name), value, sizeof(value)))

#define INTEGER(section, name, field)                                                              \
	do {                                                                                       \
		if (GET((section), (name))) {                                                      \
			number = strtol(value, &end, 0);                                           \
			if (end == value || *end)                                                  \
				goto invalid;                                                      \
			o->field = number;                                                         \
		}                                                                                  \
	} while (0)

#define BOOLEAN(section, name, field)                                                              \
	do {                                                                                       \
		if (GET((section), (name))) {                                                      \
			if (!ast_true(value) && !ast_false(value))                                 \
				goto invalid;                                                      \
			o->field = ast_true(value);                                                \
		}                                                                                  \
	} while (0)

#define STRING(section, name, field)                                                               \
	do {                                                                                       \
		if (GET((section), (name)))                                                        \
			ast_copy_string(o->field, value, sizeof(o->field));                        \
	} while (0)

	/* Typed hardware settings include inherited flat-section defaults.  Mirror
	 * values needed by channel initialization before creating the signaling
	 * engine; live mixer levels continue to use the typed accessors directly. */
	if (usbradioplus_processing_get_hardware(category, &hardware))
		goto invalid;
	o->txmixa = (enum radio_tx_mix)hardware.output_a_assignment;
	o->txmixb = (enum radio_tx_mix)hardware.output_b_assignment;
	o->rxcdtype = effective_rxcdtype(o);
	STRING("ctcss", "receive_frequencies", rxctcssfreqs);
	STRING("ctcss", "transmit_frequencies", txctcssfreqs);
	/* The processing parser rejects every malformed CTCSS value, including
	 * inactive-direction controls. Keep the direct override path equally strict
	 * so live reload and unit callers cannot preserve an invalid latent value. */
	if ((!ast_strlen_zero(o->rxctcssfreqs) &&
	     !ctcss_frequency_list_valid(o->rxctcssfreqs, &tone_count)) ||
	    (!ast_strlen_zero(o->txctcssfreqs) &&
	     !ctcss_frequency_list_valid(o->txctcssfreqs, &tone_count)))
		goto invalid;

	STRING("hardware", "hardware_device_identifier", devstr);
	STRING("hardware", "hardware_serial", serial);
	INTEGER("hardware", "hardware_interface_type", hdwtype);
	BOOLEAN("hardware", "hardware_eeprom_enabled", wanteeprom);
	INTEGER("hardware", "hardware_audio_fragment_count", frags);
	INTEGER("hardware", "hardware_audio_queue_size", queuesize);
	BOOLEAN("receive", "cpu_saver_enabled", rxcpusaver);
	BOOLEAN("transmit", "cpu_saver_enabled", txcpusaver);
	STRING("receive", "signaling_method", receive_signaling_method);
	STRING("transmit", "signaling_method", transmit_signaling_method);
	if (GET("receive", "cos_assignment"))
		o->rxcdtype = carrier_detect_from_assignment(value);
	if (GET("receive", "audio_source"))
		store_rxdemod(o, value);
	if (GET("ctcss", "receive_source"))
		store_rxsdtype(o, value);
	INTEGER("receive", "vox_hang_ms", voxhangtime);
	INTEGER("receive", "vox_threshold", rxsqvoxadj);
	INTEGER("receive", "noise_squelch_hysteresis", rxsqhyst);
	INTEGER("receive", "noise_filter_type", rxnoisefiltype);
	INTEGER("receive", "squelch_delay_ms", rxsquelchdelay);
	INTEGER("receive", "on_delay_frames", rxondelay);
	if (o->rxondelay > MS_TO_FRAMES(RX_ON_DELAY_MAX))
		o->rxondelay = MS_TO_FRAMES(RX_ON_DELAY_MAX);
	BOOLEAN("receive", "polarity_inverted", rxpolarity);
	INTEGER("receive", "squelch_level", rxsquelchadj);
	if (GET("ctcss", "receive_decoder_gain_db")) {
		double adjustment = strtod(value, &end);
		if (end == value || *end || !isfinite(adjustment))
			goto invalid;
		o->rxctcssadj = pow(10.0, adjustment / 20.0);
	}
	BOOLEAN("ctcss", "receive_override_enabled", rxctcssoverride);
	INTEGER("ctcss", "receive_relax", rxctcssrelax);
	STRING("ctcss", "transmit_default_hz", txctcssdefault);
	if (!ast_strlen_zero(o->txctcssdefault) && !ctcss_frequency_valid(o->txctcssdefault))
		goto invalid;
	if (GET("ctcss", "transmit_peak_dbfs")) {
		double peak = strtod(value, &end);
		if (end == value || *end || !isfinite(peak) || peak < -90.0 || peak > 0.0)
			goto invalid;
		o->ctcss_level = 32767.0 * pow(10.0, peak / 20.0);
	}
	if (GET("ctcss", "turnoff_mode"))
		store_txtoctype(o, value);
	if (GET("ctcss", "phase_shift_degrees")) {
		o->ctcss_phase_shift_degrees = strtod(value, &end);
		if (end == value || *end || !isfinite(o->ctcss_phase_shift_degrees) ||
		    o->ctcss_phase_shift_degrees <= 0.0)
			goto invalid;
	}
	if (GET("ctcss", "tail_duration_ms")) {
		number = strtol(value, &end, 0);
		/* Match the two-frame tail setup and signed 16-bit signaling timer. */
		if (end == value || *end || number < 2L * MS_PER_FRAME || number > INT16_MAX)
			goto invalid;
		o->ctcss_tail_duration_ms = number;
	}
	if (GET("ctcss", "tail_frequency_hz")) {
		o->ctcss_tail_frequency_hz = strtod(value, &end);
		if (end == value || *end || !isfinite(o->ctcss_tail_frequency_hz) ||
		    o->ctcss_tail_frequency_hz <= 0.0)
			goto invalid;
	}
	STRING("dcs", "receive_code", dcs_receive_code);
	STRING("dcs", "transmit_code", dcs_transmit_code);
	/* Preserve valid inactive DCS settings for a later mode change, but reject
	 * malformed syntax now instead of silently treating it as carrier mode. */
	if ((!ast_strlen_zero(o->dcs_receive_code) && !dcs_code_valid(o->dcs_receive_code)) ||
	    (!ast_strlen_zero(o->dcs_transmit_code) && !dcs_code_valid(o->dcs_transmit_code)))
		goto invalid;
	BOOLEAN("dcs", "turnoff_code_enabled", dcs_turnoff_enabled);
	if (GET("dcs", "turnoff_duration_ms")) {
		number = strtol(value, &end, 0);
		if (end == value || *end || number < 150 || number > 200)
			goto invalid;
		o->dcs_turnoff_duration_ms = number;
	}
	if (GET("dcs", "peak_dbfs")) {
		o->dcs_level = strtod(value, &end);
		if (end == value || *end || !isfinite(o->dcs_level) || o->dcs_level < -90.0 ||
		    o->dcs_level > 0.0)
			goto invalid;
		o->dcs_level = 32767.0 * pow(10.0, o->dcs_level / 20.0);
	}
	/* A direction has one active signaling system. Keep its configured CTCSS and
	 * DCS data intact, however: receive and transmit are independent directions,
	 * and radio_config() selects the active sources without destroying a valid
	 * inactive-direction setting. */
	if (ast_strlen_zero(o->receive_signaling_method))
		ast_copy_string(o->receive_signaling_method, "carrier",
				sizeof(o->receive_signaling_method));
	if (ast_strlen_zero(o->transmit_signaling_method))
		ast_copy_string(o->transmit_signaling_method, "carrier",
				sizeof(o->transmit_signaling_method));
	if (!strcasecmp(o->receive_signaling_method, "carrier")) {
		store_rxsdtype(o, "no");
	} else if (!strcasecmp(o->receive_signaling_method, "ctcss")) {
		if (o->rxsdtype == SD_IGNORE) {
			signaling_configuration_error(category,
						      "CTCSS receive_source must not be no");
			goto invalid;
		}
		if (!ctcss_frequency_list_valid(o->rxctcssfreqs, NULL)) {
			signaling_configuration_error(
				category,
				"receive signaling_method=ctcss requires supported [ctcss] "
				"receive_frequencies");
			goto invalid;
		}
		if (!strcasecmp(o->transmit_signaling_method, "ctcss") &&
		    !ctcss_frequency_lists_mapped(o->rxctcssfreqs, o->txctcssfreqs)) {
			signaling_configuration_error(
				category,
				"CTCSS receive-to-transmit operation requires equal-length "
				"[ctcss] receive_frequencies and transmit_frequencies "
				"lists of supported tones");
			goto invalid;
		}
	} else if (!strcasecmp(o->receive_signaling_method, "dcs")) {
		store_rxsdtype(o, "no");
		if (!dcs_code_valid(o->dcs_receive_code)) {
			signaling_configuration_error(
				category,
				"receive signaling_method=dcs requires a valid [dcs] receive_code");
			goto invalid;
		}
	} else {
		goto invalid;
	}
	if (!strcasecmp(o->transmit_signaling_method, "carrier")) {
	} else if (!strcasecmp(o->transmit_signaling_method, "ctcss")) {
		if (!ctcss_frequency_valid(o->txctcssdefault)) {
			signaling_configuration_error(category,
						      "CTCSS transmit_default_hz is not supported");
			goto invalid;
		}
	} else if (!strcasecmp(o->transmit_signaling_method, "dcs")) {
		if (!dcs_code_valid(o->dcs_transmit_code)) {
			signaling_configuration_error(category, "DCS transmit_code is invalid");
			goto invalid;
		}
	} else {
		goto invalid;
	}
	/* This control is a CTCSS decoder bypass, not a generic receive bypass.
	 * Inactive CTCSS settings must not accidentally qualify DCS or carrier RX. */
	if (strcasecmp(o->receive_signaling_method, "ctcss"))
		o->rxctcssoverride = 0;
	if ((!strcasecmp(o->transmit_signaling_method, "ctcss") ||
	     !strcasecmp(o->transmit_signaling_method, "dcs")) &&
	    !urp_tx_pair_has_tone((enum urp_tx_output_mode)o->txmixa,
				  (enum urp_tx_output_mode)o->txmixb)) {
		signaling_configuration_error(category,
					      "transmit signaling requires [hardware] output A or "
					      "B to carry ctcss or voice_ctcss");
		goto invalid;
	}
	BOOLEAN("receive", "lsd_polarity_inverted", lsdrxpolarity);
	BOOLEAN("transmit", "lsd_polarity_inverted", lsdtxpolarity);
	BOOLEAN("transmit", "preemphasis_enabled", txpreemphasis);
	INTEGER("transmit", "settle_ms", txsettletime);
	INTEGER("transmit", "rx_blanking_ms", txrxblankingtime);
	INTEGER("transmit", "off_delay_frames", txoffdelay);
	if (o->txoffdelay > MS_TO_FRAMES(TX_OFF_DELAY_MAX))
		o->txoffdelay = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
	BOOLEAN("transmit", "polarity_inverted", txpolarity);
	BOOLEAN("hardware", "hardware_ptt_inverted", invertptt);
	INTEGER("receive", "frequency_hz", rxfreq);
	INTEGER("transmit", "frequency_hz", txfreq);
	INTEGER("hardware", "hardware_repeater_number", rptnum);
	INTEGER("hardware", "hardware_area", area);
	STRING("hardware", "hardware_user_key", ukey);
	INTEGER("hardware", "hardware_idle_interval", idleinterval);
	INTEGER("hardware", "hardware_turnoff_count", turnoffs);
	INTEGER("hardware", "hardware_voter_reporting", sendvoter);
	INTEGER("hardware", "hardware_clip_led_gpio", clipledgpio);
	for (i = 0; i < GPIO_PINCOUNT; ++i) {
		snprintf(option, sizeof(option), "hardware_gpio_%d_mode", i + 1);
		if (GET("hardware", option)) {
			ast_free(o->gpios[i]);
			o->gpios[i] = ast_strdup(value);
			if (!o->gpios[i])
				goto invalid;
		}
	}
	for (option_index = 0; option_index < ARRAY_LEN(parallel_pins); ++option_index) {
		int pin = parallel_pins[option_index];
		snprintf(option, sizeof(option), "hardware_parallel_pin_%d_assignment", pin);
		if (GET("hardware", option)) {
			ast_free(o->pps[pin]);
			o->pps[pin] = ast_strdup(value);
			if (!o->pps[pin])
				goto invalid;
			haspp = 1;
		}
	}
	INTEGER("duplex", "duplex_radio_mode", radioduplex);
	INTEGER("duplex", "duplex_local_repeat_level", duplex3);
	if (GET("duplex", "duplex_local_repeat_mode")) {
		if (!strcasecmp(value, "hardware"))
			o->duplex3mode = DUPLEX3_MODE_HARDWARE;
		else if (!strcasecmp(value, "software"))
			o->duplex3mode = DUPLEX3_MODE_SOFTWARE;
		else
			goto invalid;
	}
	if (GET("hardware", "hardware_deemphasis_corner_hz")) {
		double frequency = strtod(value, &end);
		if (end == value || *end || !isfinite(frequency))
			goto invalid;
		o->plus_deemphasis_corner_hz = frequency;
	}
	if (GET("hardware", "hardware_preemphasis_corner_hz")) {
		double frequency = strtod(value, &end);
		if (end == value || *end || !isfinite(frequency))
			goto invalid;
		o->plus_preemphasis_corner_hz = frequency;
	}
	BOOLEAN("general", "channel_enabled", radioactive);
	INTEGER("diagnostics", "diagnostics_trace_type", tracetype);
	INTEGER("diagnostics", "diagnostics_trace_level", tracelevel);
	INTEGER("diagnostics", "diagnostics_fever", fever);
	for (option_index = 0; option_index < ARRAY_LEN(asterisk_jitter_options); ++option_index)
		if (GET("asterisk", modern_jitter_options[option_index]) &&
		    ast_jb_read_conf(&global_jbconf, asterisk_jitter_options[option_index], value))
			goto invalid;
#undef STRING
#undef BOOLEAN
#undef INTEGER
#undef GET
	return 0;
invalid:
	ast_log(LOG_ERROR, "RadioPlus/%s: invalid processing configuration override\n", category);
#undef STRING
#undef BOOLEAN
#undef INTEGER
#undef GET
	return -1;
}

int save_tuning_config(struct chan_usbradio_pvt *o)
{
	static const char *const demodulation[] = {"no", "speaker", "flat"};
	static const char *const assignments[] = {"off", "voice", "ctcss", "voice_ctcss",
						  "auxvoice"};
	struct usbradioplus_config_update updates[24];
	enum urp_ctcss_source configured_ctcss_source;
	char ctcss_receive_source[64];
	char values[24][64];
	size_t count = 0;

#define ADD_TEXT(group, key, text)                                                                 \
	do {                                                                                       \
		updates[count] = (struct usbradioplus_config_update){(group), (key), (text)};      \
		++count;                                                                           \
	} while (0)

#define ADD_NUMBER(group, key, format, number)                                                     \
	do {                                                                                       \
		snprintf(values[count], sizeof(values[count]), (format), (number));                \
		ADD_TEXT((group), (key), values[count]);                                           \
	} while (0)
	/* rxsdtype is intentionally SD_IGNORE unless CTCSS is the active receive
	 * method. Persist the profile setting instead, so changing away from CTCSS
	 * for a while cannot erase its configured indication source on a tune save. */
	ast_copy_string(ctcss_receive_source, sd_signal_type[o->rxsdtype],
			sizeof(ctcss_receive_source));
	if (!usbradioplus_processing_get_option(o->name, "ctcss", "receive_source",
						ctcss_receive_source,
						sizeof(ctcss_receive_source)) &&
	    urp_parse_ctcss_source(ctcss_receive_source, &configured_ctcss_source))
		ast_copy_string(ctcss_receive_source, sd_signal_type[o->rxsdtype],
				sizeof(ctcss_receive_source));
	if (!ast_strlen_zero(o->devstr))
		ADD_TEXT("hardware", "hardware_device_identifier", o->devstr);
	if (!ast_strlen_zero(o->serial))
		ADD_TEXT("hardware", "hardware_serial", o->serial);
	ADD_NUMBER("hardware", "hardware_input_gain_db", "%.3f",
		   urp_mixer_to_gain_db(effective_rxmixerset(o)));
	ADD_NUMBER("hardware", "hardware_output_a_gain_db", "%.3f",
		   urp_mixer_to_gain_db(effective_txmixaset(o)));
	ADD_NUMBER("hardware", "hardware_output_b_gain_db", "%.3f",
		   urp_mixer_to_gain_db(effective_txmixbset(o)));
	ADD_NUMBER("ctcss", "receive_decoder_gain_db", "%.3f",
		   20.0 * log10(fmax(o->rxctcssadj, 1.0e-6)));
	ADD_NUMBER("ctcss", "transmit_peak_dbfs", "%.3f",
		   20.0 * log10(fmax(o->ctcss_level / 32767.0, 1.0e-6)));
	ADD_NUMBER("receive", "squelch_level", "%d", o->rxsquelchadj);
	ADD_TEXT("receive", "cos_assignment", cd_signal_type[o->rxcdtype]);
	ADD_TEXT("ctcss", "receive_source", ctcss_receive_source);
	ADD_NUMBER("receive", "on_delay_frames", "%d", o->rxondelay);
	ADD_NUMBER("transmit", "off_delay_frames", "%d", o->txoffdelay);
	ADD_TEXT("transmit", "preemphasis_enabled", o->txpreemphasis ? "yes" : "no");
	ADD_TEXT("receive", "audio_source", demodulation[o->rxdemod]);
	ADD_TEXT("hardware", "hardware_output_a_assignment", assignments[o->txmixa]);
	ADD_TEXT("hardware", "hardware_output_b_assignment", assignments[o->txmixb]);
	ADD_NUMBER("diagnostics", "diagnostics_fever", "%d", o->fever);
	ADD_NUMBER("duplex", "duplex_local_repeat_level", "%d", o->duplex3);
	ADD_TEXT("duplex", "duplex_local_repeat_mode",
		 o->duplex3mode == DUPLEX3_MODE_SOFTWARE ? "software" : "hardware");
	ADD_NUMBER("local", "input_gain_db", "%.3f", effective_rx_input_gain_db(o));
#undef ADD_NUMBER
#undef ADD_TEXT
	return usbradioplus_processing_save_options(o->name, updates, count);
}

/** @brief Build the fixed native de-emphasis graph configuration.
 * @param o Radio channel whose flat-discriminator and de-emphasis settings apply.
 * @param config Receives a zero-initialized FFmpeg graph configuration.
 */
static void native_receive_deemphasis_config(const struct chan_usbradio_pvt *o,
					     struct txagc_config *config)
{
	memset(config, 0, sizeof(*config));
	config->deemphasis_enabled = o->rxdemod == RX_AUDIO_FLAT;
	config->emphasis_corner_hz = o->plus_deemphasis_corner_hz;
	config->emphasis_reference_hz = 1000.0;
}

/** @brief Build the always-selected receive band-pass and PL-filter graph.
 * @param chain Resolved local processing chain.
 * @param config Receives the fixed band-pass and PL-filter configuration.
 *
 * Notch mode deliberately leaves the frequency list empty.  It records the
 * selected mode in this graph while the native callback dispatches to one
 * separately prepared graph for the currently decoded CTCSS code.  That
 * retains the original one-tone-at-a-time semantics without graph rebuilding
 * or allocation in the audio callback.
 */
static void native_receive_filter_config(const struct txagc_chain *chain,
					 struct txagc_config *config)
{
	memset(config, 0, sizeof(*config));
	config->receive_bandpass_enabled = chain->agc.receive_bandpass_enabled;
	config->receive_bandpass_highpass_hz = chain->agc.receive_bandpass_highpass_hz;
	config->receive_bandpass_lowpass_hz = chain->agc.receive_bandpass_lowpass_hz;
	config->ctcss_filter_mode = chain->agc.ctcss_filter_mode;
	config->ctcss_highpass_hz = chain->agc.ctcss_highpass_hz;
	config->ctcss_notch_width_hz = chain->agc.ctcss_notch_width_hz;
}

/** @brief Find one CTCSS table entry in the configured receive-code list.
 * @param frequencies Comma-separated configured receive CTCSS frequencies.
 * @param code CTCSS table index to find.
 * @param result_frequency Receives the configured tone frequency when the code is found.
 * @return Nonzero when the requested code is explicitly configured.
 */
static int native_ctcss_code_frequency(const char *frequencies, int code, double *result_frequency)
{
	const char *cursor = frequencies;

	if (!cursor || !result_frequency || code < 0 || code >= CTCSS_NUM_CODES)
		return 0;
	while (*cursor) {
		char *end;
		double parsed_frequency = strtod(cursor, &end);

		if (end == cursor)
			return 0;
		if (urp_ctcss_frequency_index((float)parsed_frequency) == code) {
			*result_frequency = parsed_frequency;
			return 1;
		}
		cursor = end;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (*cursor != ',')
			return 0;
		++cursor;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
	}
	return 0;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_native_ctcss_code_frequency(const char *frequencies, int code,
						  double *frequency)
{
	return native_ctcss_code_frequency(frequencies, code, frequency);
}
#endif

/** @brief Build a prepared receive graph that rejects exactly one CTCSS code.
 * @param chain Resolved local processing chain.
 * @param frequency CTCSS frequency in Hz rejected by this graph.
 * @param config Receives the fixed band-pass and one-tone notch configuration.
 */
static void native_receive_notch_config(const struct txagc_chain *chain, double frequency,
					struct txagc_config *config)
{
	/* The base slot has already applied the receive band-pass.  This slot must
	 * add only the decoded tone's notch; repeating the band-pass here changes
	 * its response and makes a decoded-tone transition audible. */
	memset(config, 0, sizeof(*config));
	config->ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	config->ctcss_notch_width_hz = chain->agc.ctcss_notch_width_hz;
	snprintf(config->ctcss_notch_frequencies, sizeof(config->ctcss_notch_frequencies), "%.1f",
		 frequency);
}

/** @brief Build the optional local-dynamics graph after fixed receive filtering.
 * @param chain Resolved local processing chain.
 * @param config Receives the optional shared-graph stages.
 */
static void native_local_dynamics_config(const struct txagc_chain *chain,
					 struct txagc_config *config)
{
	*config = chain->agc;
	config->input_gain_db = 0.0;
	/* De-emphasis and PL filtering are fixed native graphs before RNNoise and
	 * the optional graph, so the dynamics graph must not reproduce either one. */
	config->deemphasis_enabled = 0;
	config->ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	config->receive_bandpass_enabled = 0;
}

/** @brief Build the final transmitter graph configuration.
 * @param o Radio channel supplying transmitter pre-emphasis settings.
 * @param chain Resolved voice/telemetry processing chain.
 * @param config Receives the final composite graph configuration.
 */
static void native_final_config(const struct chan_usbradio_pvt *o, const struct txagc_chain *chain,
				struct txagc_config *config)
{
	*config = chain->agc;
	/* The source master gates only optional reorderable stages. */
	if (!chain->enabled) {
		config->agc_enabled = 0;
		config->expander_enabled = 0;
		config->compressor_enabled = 0;
		config->limiter_enabled = 0;
	}
	config->preemphasis_enabled = o->txpreemphasis;
	config->emphasis_corner_hz = o->plus_preemphasis_corner_hz;
	config->emphasis_reference_hz = 1000.0;
	/* Voice/telemetry spectral shaping belongs to the configured processing
	 * graph. The DCS-only shaper is built separately below. */
	config->dcs_spectral_shaping_enabled = 0;
	config->dcs_spectral_lowpass_hz = 0.0;
}

/** @brief Destroy a complete native graph generation outside the audio callback.
 * @param graphs Complete generation allocated by native_graph_set_build().
 */
static void native_graph_set_destroy(struct usbradioplus_native_graph_set *graphs)
{
	int code;

	if (!graphs)
		return;
	txagc_avfilter_destroy(&graphs->receive_deemphasis);
	txagc_avfilter_destroy(&graphs->receive_filter);
	txagc_avfilter_destroy(&graphs->local_dynamics);
	txagc_avfilter_destroy(&graphs->final);
	txagc_avfilter_destroy(&graphs->dcs);
	txagc_avfilter_destroy(&graphs->dcs_turnoff);
	for (code = 0; code < CTCSS_NUM_CODES; ++code)
		txagc_avfilter_destroy(&graphs->ctcss_notch[code]);
	ast_free(graphs);
}

/** @brief Initialize every processor in an unprepared native graph generation.
 * @param graphs Zeroed generation whose FFmpeg statistics are initialized.
 */
static void native_graph_set_init(struct usbradioplus_native_graph_set *graphs)
{
	int code;

	txagc_avfilter_init(&graphs->receive_deemphasis);
	txagc_avfilter_init(&graphs->receive_filter);
	txagc_avfilter_init(&graphs->local_dynamics);
	txagc_avfilter_init(&graphs->final);
	txagc_avfilter_init(&graphs->dcs);
	txagc_avfilter_init(&graphs->dcs_turnoff);
	for (code = 0; code < CTCSS_NUM_CODES; ++code)
		txagc_avfilter_init(&graphs->ctcss_notch[code]);
}

/** @brief Acquire one control-plane writer token for a native graph slot.
 * @param slot Published native graph slot.
 */
static void native_graph_slot_lock(struct usbradioplus_native_graph_slot *slot)
{
	while (atomic_flag_test_and_set_explicit(&slot->writer, memory_order_acquire)) {
		sched_yield();
#ifdef URP_PROCESSING_TESTING
		atomic_flag_clear_explicit(&slot->writer, memory_order_release);
#endif
	}
}

/** @brief Release a native graph-slot control-plane writer token.
 * @param slot Published native graph slot.
 */
static void native_graph_slot_unlock(struct usbradioplus_native_graph_slot *slot)
{
	atomic_flag_clear_explicit(&slot->writer, memory_order_release);
}

/** @brief Initialize an empty native graph slot before its first control-plane build.
 * @param slot Channel-owned slot.
 */
static void native_graph_slot_init(struct usbradioplus_native_graph_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
	atomic_init(&slot->active, NULL);
	atomic_init(&slot->readers, 0U);
	atomic_flag_clear_explicit(&slot->writer, memory_order_relaxed);
}

/** @brief Destroy active and retired graph generations after the native callback stops.
 * @param slot Channel-owned slot with no future native callback entry.
 */
static void native_graph_slot_destroy(struct usbradioplus_native_graph_slot *slot)
{
	struct usbradioplus_native_graph_set *graphs;

	native_graph_slot_lock(slot);
	atomic_store_explicit(&slot->active, NULL, memory_order_seq_cst);
	while (atomic_load_explicit(&slot->readers, memory_order_seq_cst) != 0U) {
		sched_yield();
#ifdef URP_PROCESSING_TESTING
		atomic_store_explicit(&slot->readers, 0U, memory_order_seq_cst);
#endif
	}
	graphs = slot->owned;
	slot->owned = NULL;
	native_graph_slot_unlock(slot);
	while (graphs) {
		struct usbradioplus_native_graph_set *next = graphs->next_retired;
		native_graph_set_destroy(graphs);
		graphs = next;
	}
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_native_graph_slot_contention_paths(void)
{
	struct usbradioplus_native_graph_slot slot;

	native_graph_slot_init(&slot);
	atomic_flag_test_and_set_explicit(&slot.writer, memory_order_relaxed);
	native_graph_slot_lock(&slot);
	native_graph_slot_unlock(&slot);
	atomic_store_explicit(&slot.readers, 1U, memory_order_seq_cst);
	native_graph_slot_destroy(&slot);
	return (int)atomic_load_explicit(&slot.readers, memory_order_seq_cst);
}
#endif

/** @brief Reclaim retired graphs or reject a third live generation.
 * @param slot Destination channel slot.
 * @return Zero when a prepared generation may be published, or nonzero while
 *         a prior callback reader prevents bounded reclamation.
 *
 * The callback has one global reader reference spanning its acquired graph.
 * If it has not drained, preserving a single retired generation is safe. A
 * later reload is deliberately rejected rather than accumulating unbounded
 * generations or making the callback wait for the control plane.
 */
static int native_graph_slot_preflight(struct usbradioplus_native_graph_slot *slot)
{
	struct usbradioplus_native_graph_set *retired = NULL;

	native_graph_slot_lock(slot);
	if (slot->owned && slot->owned->next_retired) {
		if (atomic_load_explicit(&slot->readers, memory_order_seq_cst) != 0U) {
			native_graph_slot_unlock(slot);
			return -1;
		}
		retired = slot->owned->next_retired;
		slot->owned->next_retired = NULL;
	}
	native_graph_slot_unlock(slot);
	while (retired) {
		struct usbradioplus_native_graph_set *next = retired->next_retired;
		native_graph_set_destroy(retired);
		retired = next;
	}
	return 0;
}

/** @brief Publish a complete, already prepared native graph generation.
 * @param slot Destination channel slot preflighted by native_graph_slot_preflight().
 * @param graphs Complete candidate generation.
 *
 * At most one retired generation remains while a native reader is active. The
 * callback never waits, allocates, or frees; a later control-plane reload
 * reclaims the retired generation after the reader count reaches zero. The
 * active-pointer store and reader-count load are sequentially consistent with
 * acquire's count increment then pointer load. If publication sees zero first,
 * a later acquire must observe this new active pointer; if acquire increments
 * first, publication retains the old generation. Thus no callback can load a
 * generation that this publication reclaims.
 */
static void native_graph_slot_publish(struct usbradioplus_native_graph_slot *slot,
				      struct usbradioplus_native_graph_set *graphs)
{
	struct usbradioplus_native_graph_set *retired = NULL;

	native_graph_slot_lock(slot);
	graphs->next_retired = slot->owned;
	slot->owned = graphs;
	atomic_store_explicit(&slot->active, graphs, memory_order_seq_cst);
	if (atomic_load_explicit(&slot->readers, memory_order_seq_cst) == 0U) {
		retired = graphs->next_retired;
		graphs->next_retired = NULL;
	}
	native_graph_slot_unlock(slot);
	while (retired) {
		struct usbradioplus_native_graph_set *next = retired->next_retired;
		native_graph_set_destroy(retired);
		retired = next;
	}
}

struct usbradioplus_native_graph_set *
usbradioplus_native_graphs_acquire(struct chan_usbradio_pvt *channel)
{
	struct usbradioplus_native_graph_slot *slot;
	struct usbradioplus_native_graph_set *graphs;

	if (!channel)
		return NULL;
	slot = &channel->plus_native_graphs;
	/* Pair sequentially-consistent operations with publication's active-store
	 * followed by reader-count load. The global order either pins the old graph
	 * before its publisher observes readers, or makes this load observe the new
	 * active graph after publisher saw zero. */
	atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst);
	graphs = atomic_load_explicit(&slot->active, memory_order_seq_cst);
	if (!graphs)
		atomic_fetch_sub_explicit(&slot->readers, 1U, memory_order_seq_cst);
	return graphs;
}

void usbradioplus_native_graphs_release(struct chan_usbradio_pvt *channel)
{
	if (channel)
		atomic_fetch_sub_explicit(&channel->plus_native_graphs.readers, 1U,
					  memory_order_seq_cst);
}

/** @brief Build every stage of a native graph generation before publication.
 * @param o Radio channel whose resolved settings are used.
 * @param graphs Receives a fully configured graph generation on success.
 * @return Zero on success; nonzero with no published graph touched on failure.
 */
static int native_graph_set_build(const struct chan_usbradio_pvt *o,
				  struct usbradioplus_native_graph_set **graphs)
{
	static const struct txagc_config dcs_shaping_config = {
		.dcs_spectral_shaping_enabled = 1,
		.dcs_spectral_lowpass_hz = 250.0,
		/* The steep 250-Hz spectral filter has +3.41 dB passband gain at the
		 * DCS/EOT fundamentals. Normalize that fixed response so dcs_peak_dbfs
		 * remains the requested transmitter peak without a general limiter. */
		.output_gain_db = -3.42,
	};
	static const struct txagc_config dcs_turnoff_shaping_config = {
		.dcs_spectral_shaping_enabled = 1,
		.dcs_spectral_lowpass_hz = 250.0,
		/* The 134.4-Hz sine does not have the NRZ waveform's +3.41 dB
		 * shaped passband rise. Keep its independently calibrated peak at
		 * dcs_peak_dbfs without putting a limiter in the signaling path. */
		.output_gain_db = 0.0,
	};
	struct txagc_chain local;
	struct txagc_chain composite;
	struct txagc_config receive_deemphasis;
	struct txagc_config receive_filter;
	struct txagc_config local_dynamics;
	struct txagc_config final;
	struct usbradioplus_native_graph_set *candidate;
	int code;

	if (!o || !o->name || !graphs || usbradioplus_processing_get_local(o->name, &local) ||
	    usbradioplus_processing_get_composite(o->name, &composite))
		return -1;
	native_receive_deemphasis_config(o, &receive_deemphasis);
	native_receive_filter_config(&local, &receive_filter);
	native_local_dynamics_config(&local, &local_dynamics);
	native_final_config(o, &composite, &final);
	candidate = ast_calloc(1, sizeof(*candidate));
	if (!candidate)
		return -1;
	native_graph_set_init(candidate);
	/* Native ticks consume this generation as one immutable transaction.  Keep
	 * the few non-FFmpeg local controls beside their prepared graphs so a reload
	 * cannot combine a new setting snapshot with an older graph generation. */
	candidate->app_rpt_rate = o->plus_app_rpt_rate;
	candidate->app_rpt_samples = o->plus_app_rpt_samples;
	candidate->program_target_samples = o->plus_program_target_samples;
	candidate->legacy_interface = !o->plus_advanced;
	candidate->receive_cpu_saver = o->rxcpusaver;
	candidate->noise_squelch_gate = o->radio && o->radio->rxCdType == CD_XPMR_NOISE;
	candidate->receive_squelch_delay_samples =
		o->rxsquelchdelay ? (size_t)o->rxsquelchdelay * (URP_RATE_NATIVE / 1000U) : 0U;
	candidate->echo_mode = o->echomode;
	candidate->software_repeat_enabled =
		o->duplex3 > 0 && o->duplex3mode == DUPLEX3_MODE_SOFTWARE;
	candidate->software_repeat_gain = (double)o->duplex3 / DUPLEX3_LEVEL_MAX;
	candidate->local_chain_enabled = local.enabled;
	candidate->local_rnnoise_enabled = local.rnnoise_enabled;
	/* Gain conversion is configuration work.  The native callback only applies
	 * this immutable multiplier sample by sample and never evaluates pow(). */
	candidate->local_input_gain_linear = pow(10.0, local.agc.input_gain_db / 20.0);
	if (txagc_avfilter_prepare(&candidate->receive_deemphasis, &receive_deemphasis,
				   URP_RATE_NATIVE) < 0 ||
	    txagc_avfilter_prepare(&candidate->receive_filter, &receive_filter, URP_RATE_NATIVE) <
		    0 ||
	    txagc_avfilter_prepare(&candidate->local_dynamics, &local_dynamics, URP_RATE_NATIVE) <
		    0 ||
	    txagc_avfilter_prepare(&candidate->final, &final, URP_RATE_NATIVE) < 0 ||
	    txagc_avfilter_prepare(&candidate->dcs, &dcs_shaping_config, URP_RATE_NATIVE) < 0 ||
	    txagc_avfilter_prepare(&candidate->dcs_turnoff, &dcs_turnoff_shaping_config,
				   URP_RATE_NATIVE) < 0)
		goto failed;
	if (receive_filter.ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH) {
		for (code = 0; code < CTCSS_NUM_CODES; ++code) {
			struct txagc_config notch;
			double frequency;

			if (!native_ctcss_code_frequency(o->rxctcssfreqs, code, &frequency))
				continue;
			native_receive_notch_config(&local, frequency, &notch);
			if (txagc_avfilter_prepare(&candidate->ctcss_notch[code], &notch,
						   URP_RATE_NATIVE) < 0)
				goto failed;
		}
	}
	*graphs = candidate;
	return 0;

failed:
	ast_log(LOG_ERROR, "RadioPlus/%s: unable to prepare native FFmpeg processing\n", o->name);
	native_graph_set_destroy(candidate);
	return -1;
}

#ifdef URP_PROCESSING_TESTING
int usbradioplus_test_native_graph_set_build(const struct chan_usbradio_pvt *channel,
					     struct usbradioplus_native_graph_set **graphs)
{
	return native_graph_set_build(channel, graphs);
}
#endif

int usbradioplus_prepare_native_processing(struct chan_usbradio_pvt *o)
{
	struct usbradioplus_native_graph_set *candidate;

	if (native_graph_set_build(o, &candidate))
		return -1;
	if (native_graph_slot_preflight(&o->plus_native_graphs)) {
		native_graph_set_destroy(candidate);
		return -1;
	}
	/* The callback acquires this one pointer for its whole 20-ms block. A bad
	 * candidate therefore leaves every old stage active rather than creating a
	 * transient mixture of generations. */
	native_graph_slot_publish(&o->plus_native_graphs, candidate);
	return 0;
}

/** One native channel graph prepared as part of a reload-wide transaction. */
struct native_graph_plan {
	/** Channel whose slot receives graphs at transaction publication. */
	struct chan_usbradio_pvt *channel;
	/** Fully prepared immutable native graph generation. */
	struct usbradioplus_native_graph_set *graphs;
};

/** Opaque all-channel native graph transaction owned by the control plane. */
struct usbradioplus_native_graph_transaction {
	/** Candidate graph generation for every initialized native channel. */
	struct native_graph_plan *plans;
	/** Number of populated plans. */
	size_t count;
};

/* Destroy a native graph transaction that was not published. The public
 * declaration carries the API documentation without duplicating it here. */
void usbradioplus_discard_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction)
{
	size_t index;

	if (!transaction)
		return;
	for (index = 0; index < transaction->count; ++index)
		native_graph_set_destroy(transaction->plans[index].graphs);
	ast_free(transaction->plans);
	ast_free(transaction);
}

/* Build and preflight every native graph without publishing a generation. */
int usbradioplus_stage_all_native_processing(
	struct usbradioplus_native_graph_transaction **transaction)
{
	struct chan_usbradio_pvt *channel;
	struct usbradioplus_native_graph_transaction *candidate;
	size_t count = 0;
	size_t index = 0;

	if (!transaction)
		return -1;
	*transaction = NULL;
	for (channel = usbradioplus_channel_first(); channel; channel = channel->next)
		if (channel->plus_dsp_initialized)
			++count;
	candidate = ast_calloc(1, sizeof(*candidate));
	if (!candidate)
		return -1;
	if (!count) {
		*transaction = candidate;
		return 0;
	}
	candidate->plans = ast_calloc(count, sizeof(*candidate->plans));
	if (!candidate->plans) {
		ast_free(candidate);
		return -1;
	}
	/* Build every replacement first. A failed later graph must never leave an
	 * earlier channel on new settings while the rest retain the old generation. */
	for (channel = usbradioplus_channel_first(); channel; channel = channel->next) {
		if (!channel->plus_dsp_initialized)
			continue;
		candidate->plans[index].channel = channel;
		if (native_graph_set_build(channel, &candidate->plans[index].graphs)) {
			candidate->count = index;
			usbradioplus_discard_native_processing_transaction(candidate);
			return -1;
		}
		++index;
	}
	candidate->count = count;
	for (index = 0; index < count; ++index) {
		if (native_graph_slot_preflight(
			    &candidate->plans[index].channel->plus_native_graphs)) {
			usbradioplus_discard_native_processing_transaction(candidate);
			return -1;
		}
	}
	*transaction = candidate;
	return 0;
}

/* Publish an already staged all-channel native graph transaction. Publication
 * has no fallible work: all allocation and slot preflight happens
 * in usbradioplus_stage_all_native_processing(). Each callback therefore sees
 * either its former immutable generation or its complete replacement. */
void usbradioplus_publish_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction)
{
	size_t index;

	if (!transaction)
		return;
	for (index = 0; index < transaction->count; ++index) {
		native_graph_slot_publish(&transaction->plans[index].channel->plus_native_graphs,
					  transaction->plans[index].graphs);
		transaction->plans[index].graphs = NULL;
	}
	usbradioplus_discard_native_processing_transaction(transaction);
}

int usbradioplus_prepare_all_native_processing(void)
{
	struct usbradioplus_native_graph_transaction *transaction;

	if (usbradioplus_stage_all_native_processing(&transaction))
		return -1;
	usbradioplus_publish_native_processing_transaction(transaction);
	return 0;
}

int usbradioplus_dsp_init(struct chan_usbradio_pvt *o)
{
	o->plus_dsp_initialized = 0;
	if (rpcr_init(&o->plus_program_ring, URP_PROGRAM_RING_SAMPLES, RPCR_SINC_BEST) ||
	    rpcr_set_rates(&o->plus_program_ring, URP_APP_RPT_RATE_DEFAULT, URP_RATE_NATIVE)) {
		ast_log(LOG_ERROR, "RadioPlus/%s: unable to create native program ring\n", o->name);
		return -1;
	}
	native_graph_slot_init(&o->plus_native_graphs);
	/* A CTCSS/DCS parser reload can replace decoder-owned memory. Initialize
	 * this separate gate before the hardware callback can enter a native tick. */
	atomic_init(&o->plus_radio_access.readers, 0U);
	atomic_init(&o->plus_radio_access.reconfiguring, 0);
	atomic_flag_clear_explicit(&o->plus_radio_access.writer, memory_order_relaxed);
	usbradioplus_tx_playout_hold_reset(o);
	atomic_init(&o->plus_radio_tx_active, 0);
	atomic_init(&o->plus_hardware_ptt_request, 0);
	atomic_init(&o->plus_hardware_ptt_applied, 0);
	atomic_init(&o->plus_hardware_online, 0);
	atomic_init(&o->plus_hardware_inputs, 0U);
	atomic_init(&o->plus_clip_led_request, 0);
	atomic_init(&o->plus_radio_program_generation, 0U);
	atomic_init(&o->plus_radio_program_rx_frequency, 0U);
	atomic_init(&o->plus_radio_program_tx_frequency, 0U);
	atomic_init(&o->plus_radio_program_high_power, 0);
	atomic_init(&o->txkeyed, 0);
	atomic_init(&o->txtestkey, 0);
	/* The HID/setup thread replaces this baseline before starting device I/O.
	 * Initializing it here lets an early native tick safely render the legacy
	 * routing instead of touching an uninitialized atomic object. */
	atomic_init(&o->plus_applied_txmixa, o->txmixa);
	atomic_init(&o->plus_applied_txmixb, o->txmixb);
	atomic_init(&o->plus_applied_tx_output_gain_a, M_Q8);
	atomic_init(&o->plus_applied_tx_output_gain_b, M_Q8);
	atomic_init(&o->plus_hardware_generation, 0U);
	atomic_init(&o->plus_test_tone_enabled, 0);
	o->plus_adc_peak_dbfs = o->plus_adc_max_peak_dbfs = -INFINITY;
	o->plus_deemphasis_peak_dbfs = o->plus_deemphasis_max_peak_dbfs = -INFINITY;
	o->plus_preemphasis_input_peak_dbfs = o->plus_preemphasis_input_max_peak_dbfs = -INFINITY;
	o->plus_tx_program_peak_dbfs = -INFINITY;
	o->plus_tx_program_max_peak_dbfs = -INFINITY;
	o->plus_local_tx_peak_dbfs = -INFINITY;
	o->plus_local_tx_max_peak_dbfs = -INFINITY;
	if (usbradioplus_prepare_native_processing(o)) {
		native_graph_slot_destroy(&o->plus_native_graphs);
		rpcr_destroy(&o->plus_program_ring);
		return -1;
	}
	o->plus_dsp_initialized = 1;
	if (usbradioplus_native_renderer_start(o)) {
		ast_log(LOG_ERROR, "RadioPlus/%s: unable to create native audio renderer\n",
			o->name);
		o->plus_dsp_initialized = 0;
		native_graph_slot_destroy(&o->plus_native_graphs);
		rpcr_destroy(&o->plus_program_ring);
		return -1;
	}
	return 0;
}

void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o)
{
	/* Callers quiesce their hardware callback before retiring renderer-owned
	 * graphs, SRC state, RNNoise state, and echo storage. */
	usbradioplus_tx_playout_hold_reset(o);
	usbradioplus_native_renderer_stop(o);
	o->plus_dsp_initialized = 0;
	rpcr_destroy(&o->plus_program_ring);
	native_graph_slot_destroy(&o->plus_native_graphs);
	ast_free(o->plus_parrot);
	o->plus_parrot = NULL;
	o->plus_parrot_capacity = o->plus_parrot_count = o->plus_parrot_play = 0;
}

void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o)
{
	const short *input = (short *)(o->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	size_t i;
	for (i = 0; i < ARRAY_LEN(o->plus_squelch_native); ++i) {
		o->plus_squelch_native[i] = input[i];
	}
}

int usbradioplus_carrier_detected(const struct chan_usbradio_pvt *o,
				  enum radio_carrier_detect source)
{
	switch (source) {
	case CD_HID:
		return o->rxhidsq;
	case CD_HID_INVERT:
		return !o->rxhidsq;
	case CD_XPMR_NOISE:
	case CD_XPMR_VOX:
		return o->radio->rxCarrierDetect;
	case CD_PP:
		return o->rxppsq;
	case CD_PP_INVERT:
		return !o->rxppsq;
	default:
		return 0;
	}
}

int usbradioplus_ctcss_detected(const struct chan_usbradio_pvt *o)
{
	if (o->radio->dcs.enabled_receive)
		return o->radio->dcs.valid && o->radio->smode == SMODE_DCS;
	return !o->radio->b.ctcssRxEnable ||
	       (o->radio->rxCtcss->decode > CTCSS_NULL && o->radio->smode == SMODE_CTCSS);
}

void usbradioplus_refresh_ctcss_decode(struct chan_usbradio_pvt *o)
{
	if (!o->radio->b.ctcssRxEnable || o->radio->rxCtcss->decode == o->rxctcssdecode)
		return;
	/* This runs in the hardware-paced audio callback. Publishing the transition
	 * must remain a bounded copy; Asterisk logging can take locks. */
	o->rxctcssdecode = o->radio->rxCtcss->decode;
	ast_copy_string(o->rxctcssfreq, o->radio->rxctcssfreq, sizeof(o->rxctcssfreq));
}

void usbradioplus_wait_for_eeprom_idle(struct chan_usbradio_pvt *o)
{
	while (o->eepromctl) {
		ast_mutex_unlock(&o->eepromlock);
		usleep(10000);
		ast_mutex_lock(&o->eepromlock);
	}
}

int usbradioplus_ensure_parrot_capacity(struct chan_usbradio_pvt *o)
{
	size_t capacity = (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES;
	double *buffer;

	if (!o)
		return -1;
	/* The direct renderer preallocates native echo storage before the callback
	 * can render. Never reallocate fallback storage while it is active. */
	if (o->plus_native_renderer)
		return 0;
	if (o->plus_parrot_capacity >= capacity && o->plus_parrot)
		return 0;
	buffer = ast_realloc(o->plus_parrot, capacity * sizeof(*buffer));
	if (!buffer)
		return -1;
	o->plus_parrot = buffer;
	o->plus_parrot_capacity = capacity;
	return 0;
}

void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed)
{
	/* The direct callback detects native echo transitions from its per-frame
	 * snapshot. This fallback must not mutate callback-owned recording state. */
	if (!o || o->plus_native_renderer)
		return;
	if (urp_parrot_rx_transition(&o->plus_parrot_state, was_keyed, o->rxkeyed))
		atomic_store_explicit(&o->echoing, 1, memory_order_release);
}

/* Distinguish named radio channels from flat and scoped settings sections.
 * @param section Configuration section name.
 * @return Nonzero when the stated condition holds; zero otherwise.
 */
int usbradioplus_is_radio_channel_section(const char *section)
{
	static const char *const reserved[] = {
		"general", "asterisk", "hardware",    "receive", "transmit", "ctcss",
		"dcs",	   "duplex",   "diagnostics", "local",	 "link",     "voice_telemetry"};
	size_t i;

	if (!section || strchr(section, ' '))
		return 0;
	for (i = 0; i < ARRAY_LEN(reserved); ++i)
		if (!strcasecmp(section, reserved[i]))
			return 0;
	return 1;
}

int load_config(int reload)
{
	struct ast_config *cfg = NULL;
	char *ctg = NULL;
	const char *val;
	char processing_value[64];
	struct ast_flags zeroflag = {reload ? CONFIG_FLAG_FILEUNCHANGED : 0};

	/* load config file */
	cfg = ast_config_load(CONFIG, zeroflag);
	if (!cfg) {
		ast_log(LOG_NOTICE, "Unable to load config %s.\n", CONFIG);
		return AST_MODULE_LOAD_DECLINE;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		ast_log(LOG_NOTICE, "Config file %s unchanged, skipping.\n", CONFIG);
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file %s is in an invalid format. Aborting.\n", CONFIG);
		return -1;
	}

	/* store the configuration */
	store_config(NULL);
	while ((ctg = ast_category_browse(cfg, ctg)) != NULL) {
		/* Scoped profile sections supply settings; only bare section names
		 * instantiate RadioPlus channels. */
		if (usbradioplus_is_radio_channel_section(ctg))
			store_config(ctg);
	}

	/* load parallel port information */
	ppfd = -1;
	pbase = 0;
	if (usbradio_active && !usbradioplus_processing_get_option(
				       usbradio_active, "hardware", "hardware_parallel_port_device",
				       processing_value, sizeof(processing_value)))
		val = processing_value;
	else
		val = NULL;
	if (val) {
		ast_copy_string(pport, val, sizeof(pport));
	} else {
		ast_copy_string(pport, PP_PORT, sizeof(pport));
	}
	if (usbradio_active &&
	    !usbradioplus_processing_get_option(usbradio_active, "hardware",
						"hardware_parallel_port_base_address",
						processing_value, sizeof(processing_value)))
		val = processing_value;
	else
		val = NULL;
	if (val) {
		pbase = strtoul(val, NULL, 0);
	}
	if (!pbase) {
		pbase = PP_IOPORT;
	}
	ast_radio_load_parallel_port(&haspp, &ppfd, &pbase, pport, reload);
	ast_config_destroy(cfg);
	return 0;
}

int reload_module(void)
{
	int result = usbradioplus_processing_reload();
	if (!result)
		result = load_config(1);
	/* load_config resolves the channel-side signaling and emphasis fields. Build
	 * one complete native generation only after those values are final; a failed
	 * candidate leaves the prior generation active in the audio callback. */
	if (!result)
		result = usbradioplus_prepare_all_native_processing();
	if (!result)
		(void)usbradioplus_refresh_all_processing_hardware();
	return result;
}

/** @name File-local and build-time constants
 * @{ */
/** @def NFLASH
 * @brief Number of transmitter calibration flash bursts.
 */
/** @def GET
 * @brief Resolve a channel-specific option into a bounded temporary string.
 */
/** @def INTEGER
 * @brief Apply a resolved integer radio option.
 */
/** @def BOOLEAN
 * @brief Apply a resolved boolean radio option.
 */
/** @def STRING
 * @brief Apply a resolved text radio option.
 */
/** @def ADD_TEXT
 * @brief Append a text-valued setting to the tuning-save list.
 */
/** @def ADD_NUMBER
 * @brief Append a numeric setting to the tuning-save list.
 */
/** @} */
