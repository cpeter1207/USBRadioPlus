/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 * Copyright (C) 2007 - 2011, Jim Dixon
 *
 * Jim Dixon, WB6NIL <jim@lambdatel.com>
 * Steve Henke, W9SH  <w9sh@arrl.net>
 * Based upon work by Mark Spencer <markster@digium.com> and Luigi Rizzo
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 * 20160829      inad            added rxlpf rxhpf txlpf txhpf
 */

/*!
 * \file
 *
 * \brief Channel driver for CM108 USB Cards with Radio Interface
 *
 * \author Jim Dixon  <jim@lambdatel.com>
 * \author Steve Henke  <w9sh@arrl.net>
 *
 * \ingroup channel_drivers
 */

/*** MODULEINFO
	<depend>alsa</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <search.h>
#include <linux/version.h>

#include "usbradioplus_host_util.h"

#ifdef HAVE_SYS_IO
#include <sys/io.h>
#endif

#define CHAN_USBRADIO 1 /* Enable the channel-driver trace interface. */

#define DEBUG_USBRADIO 0

#define PLUS_DYNAMICS_SAMPLES 48 /* 1 ms control blocks at 48 kHz */

#define DUPLEX3_LEVEL_MAX 999

#define DELIMCHR ','

#define QUOTECHR 34

#define DEFAULT_ECHO_MAX 1000 /* 20 secs of echo buffer, max */

#define DEFAULT_ECHO_SECONDS (DEFAULT_ECHO_MAX / 50)

#define PP_MASK 0xbffc

#define PP_PORT "/dev/parport0"

#define PP_IOPORT 0x378

#define RPT_TO_STRING(x) #x

#define S_FMT(x) "%" RPT_TO_STRING(x) "s "

#define N_FMT(duf) "%30" #duf /* Maximum sscanf conversion to numeric strings */

#define RX_ON_DELAY_MAX 60000 /* in ms, 60000ms, 60 seconds, 1 minute */

#define TX_OFF_DELAY_MAX 60000 /* in ms 60000ms, 60 seconds, 1 minute */

#define MS_PER_FRAME 20 /* 20 ms frames */

#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME) /* convert ms to frames */

#include "usbradioplus_radio.h"
#include "usbradioplus_dsp.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_hardware.h"
#include "./txagc/agc_core.h"
#include "./txagc/avfilter_processor.h"
#include "./txagc/rnnoise_processor.h"
#include "usbradioplus_processing.h"
#ifdef URP_HAVE_PORTAUDIO_POC
#include "usbradioplus_portaudio_poc.h"
#endif
#ifdef URP_HAVE_GPIO_POC
#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>
#include "usbradioplus_hardware_adapter.h"
#include "usbradioplus_cm119_gpio_poc_worker.h"
#include "usbradioplus_hardware_eeprom_poc.h"
#include "usbradioplus_hardware_gpio_poc.h"
#include "usbradioplus_portaudio_poc_identity.h"
#endif

#ifdef URP_CHANNEL_UNIT_TEST
#define URP_CHANNEL_LOCAL
#else

#define URP_CHANNEL_LOCAL static
#endif
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_config.h"
#include "asterisk/lock.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/options.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/cli.h"
#include "asterisk/utils.h"
#include "asterisk/causes.h"
#include "asterisk/abstract_jb.h"
#include "asterisk/musiconhold.h"
#include "asterisk/dsp.h"
#include "asterisk/format.h"
#include "asterisk/format_cache.h"
#include "asterisk/format_compatibility.h"

_Static_assert((int)URP_RX_AUDIO_FLAT == (int)RX_AUDIO_FLAT, "receive-audio enum mismatch");
_Static_assert((int)URP_TX_OUTPUT_AUX_VOICE == (int)TX_OUT_AUX, "transmit-routing enum mismatch");
_Static_assert((int)URP_CARRIER_PARALLEL_INVERTED == (int)CD_PP_INVERT,
	       "carrier-source enum mismatch");
_Static_assert((int)URP_CTCSS_PARALLEL_INVERTED == (int)SD_PP_INVERT, "CTCSS-source enum mismatch");
_Static_assert((int)URP_TONE_OFF_TONE_REMOVE == (int)TOC_NOTONE, "tone-off enum mismatch");

/** Default Asterisk jitter-buffer settings. */
static struct ast_jb_conf default_jbconf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
};

/** Asterisk jitter-buffer settings applied to newly created channels. */
struct ast_jb_conf global_jbconf;

#define CONFIG "usbradioplus.conf" /* default config file */

/** Mutex protecting legacy USB-device allocation. */
AST_MUTEX_DEFINE_STATIC(usb_dev_lock);
/** Mutex protecting shared parallel-port output state. */
ast_mutex_t pp_lock = AST_MUTEX_INIT_VALUE;
/** Physical parallel transport owner, accessed only while holding pp_lock. */
static struct chan_usbradio_pvt *parallel_owner;

struct chan_usbradio_pvt *usbradioplus_parallel_owner(void)
{
	return parallel_owner;
}

#ifdef URP_CHANNEL_UNIT_TEST
void usbradioplus_test_set_parallel_owner(struct chan_usbradio_pvt *owner)
{
	ast_mutex_lock(&pp_lock);
	parallel_owner = owner;
	ast_mutex_unlock(&pp_lock);
}
#endif

/* variables for communicating with the parallel port */
/** Cached parallel-port output byte. */
int8_t pp_val;
/** Nonzero when parallel-port hardware is available. */
int haspp;
/** Parallel-port device path. */
char pport[50];
/** Parallel-port I/O base address. */
int pbase;

/** Names of supported carrier-detection assignments. */
const char *const cd_signal_type[] = {"no", "dsp", "vox", "usb", "usbinvert", "pp", "ppinvert"};
/** Names of supported subaudible signaling-source assignments. */
const char *const sd_signal_type[] = {"no", "usb", "usbinvert", "dsp", "pp", "ppinvert"};

#include "usbradioplus_channel_private.h"
#ifdef URP_CHANNEL_UNIT_TEST
#include "usbradioplus_channel_test_shims.h"
#endif

#define plus_parrot plus_parrot_state.audio

#define plus_parrot_capacity plus_parrot_state.capacity

#define plus_parrot_count plus_parrot_state.count

#define plus_parrot_play plus_parrot_state.play

#define plus_parrot_playing plus_parrot_state.playing

#define plus_parrot_truncated plus_parrot_state.truncated

/** Template defaults and head of the configured radio-channel list. */
struct chan_usbradio_pvt usbradio_default = {
	.sounddev = -1,
	.duplex = M_UNSET,
	.queuesize = QUEUE_SIZE,
	.frags = 0,
	.readpos = AST_FRIENDLY_OFFSET, /* start here on reads */
	.wanteeprom = 1,
	.usedtmf = 1,
	.rxondelay = 0,
	.txoffdelay = 0,
	.voxhangtime = 2000,
	.area = 0,
	.rptnum = 0,
	.clipledgpio = 0,
	/* Clean-slate signaling defaults: a 0 dB decoder gain and -24 dBFS
	 * modulation peak are usable whenever an operator selects CTCSS or DCS. */
	.rxctcssadj = 1.0F,
	.txctcssadj = 63,
	.ctcss_level = 2068.0,
	.dcs_level = 2068.0,
	.ctcss_phase_shift_degrees = 120.0,
	.ctcss_tail_duration_ms = 180,
	.ctcss_tail_frequency_hz = 55.0,
	.rxaudiostats.index = 0,
	/* app_rpt currently supplies 8 kHz frames; keep the rate explicit so a
	 * future native-rate app_rpt path can bypass conversion. */
	.plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT,
	.plus_app_rpt_samples = URP_LINK_SAMPLES,
	.plus_native_max_frames = URP_NATIVE_SAMPLES,
	.plus_portaudio_input_device_index = -1,
	.plus_portaudio_output_device_index = -1,
	/* Default receiver de-emphasis corner frequency in Hz. */
	.plus_deemphasis_corner_hz = 300.0,
	/* Default transmitter pre-emphasis corner frequency in Hz. */
	.plus_preemphasis_corner_hz = 300.0,
};

struct chan_usbradio_pvt *usbradioplus_channel_first(void)
{
	return usbradio_default.next;
}

#ifdef URP_CHANNEL_UNIT_TEST
void usbradioplus_test_set_module_info(struct ast_module_info *info)
{
	ast_module_info = info;
}
#endif

/*	DECLARE FUNCTION PROTOTYPES	*/

/** @brief Apply the configured USB-interface wiring map to the channel's HID state.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; nonzero if configuration fails.
 */
int hidhdwconfig(struct chan_usbradio_pvt *o);
URP_CHANNEL_LOCAL struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
						       const struct ast_assigned_ids *assignedids,
						       const struct ast_channel *requestor,
						       const char *data, int *cause);
/** @brief Interpret app_rpt radio-control text commands.
 * @param c Asterisk radio channel.
 * @param text Radio-control command text.
 * @return Zero on success; nonzero for an invalid command.
 */
URP_CHANNEL_LOCAL int usbradio_text(struct ast_channel *c, const char *text);
URP_CHANNEL_LOCAL int usbradio_hangup(struct ast_channel *c);
URP_CHANNEL_LOCAL struct ast_frame *usbradio_read(struct ast_channel *chan);
URP_CHANNEL_LOCAL int usbradio_call(struct ast_channel *c, const char *dest, int timeout);
URP_CHANNEL_LOCAL int usbradio_write(struct ast_channel *chan, struct ast_frame *f);
void radio_dump(struct chan_usbradio_pvt *o, int fd);

/** @brief Apply a channel's settings to its radio detector and signaling engine.
 * @param o Private state of the selected radio channel.
 * @return Zero on success; nonzero if configuration fails.
 */
int radio_config(struct chan_usbradio_pvt *o);

struct chan_usbradio_pvt *store_config(const char *ctg);

#include "usbradioplus_channel_common.h"

/** Name of the radio selected for interactive tuning. */
char *usbradio_active; /* the active device */

/** Parallel input-pin to register-bit mapping. */

/** Asterisk channel technology description. */
static const char tdesc[] = "USB (CM108) Radio Channel Driver";

/** Asterisk channel technology callbacks. */
static struct ast_channel_tech usbradio_tech = {
	.type = "RadioPlus",
	.description = tdesc,
	.requester = usbradio_request,
	.send_digit_begin = usbradio_digit_begin,
	.send_digit_end = usbradio_digit_end,
	.send_text = usbradio_text,
	.hangup = usbradio_hangup,
	.answer = usbradio_answer,
	.read = usbradio_read,
	.call = usbradio_call,
	.write = usbradio_write,
	.indicate = usbradio_indicate,
	.fixup = usbradio_fixup,
	.setoption = usbradio_setoption,
};

struct chan_usbradio_pvt *find_desc(const char *dev)
{
	struct chan_usbradio_pvt *o = NULL;

	if (!dev) {
		ast_log(LOG_WARNING, "Cannot find USB descriptor <-- Null Descriptor -->.\n");
		return NULL;
	}
	for (o = usbradio_default.next; o && strcmp(o->name, dev) != 0; o = o->next)
		;
	if (!o) {
		ast_log(LOG_WARNING, "Cannot find USB descriptor <%s>.\n", dev);
		return NULL;
	}
	return o;
}

/** @brief Build the configured parallel-port PTT mask for one hardware worker.
 * @param o Private channel whose parallel assignments are inspected.
 * @return Bit mask of parallel pins assigned to PTT.
 */
URP_CHANNEL_LOCAL uint8_t hidthread_parallel_ptt_mask(const struct chan_usbradio_pvt *o)
{
	uint8_t mask = 0;
	int pin;

	if (!haspp)
		return 0;
	for (pin = 2; pin <= 9; ++pin) {
		if (o->pps[pin] && !strncasecmp(o->pps[pin], "ptt", 3))
			mask |= (uint8_t)(1U << (pin - 2));
	}
	return mask;
}

URP_CHANNEL_LOCAL void hidthread_close_pttkick(struct chan_usbradio_pvt *o);

/**
 * @brief Recreate the nonblocking control wake pipe used by one hardware owner.
 * @param o Channel whose control worker receives wake notifications.
 * @return Zero on success, or minus one when the pipe cannot be prepared.
 *
 * The pipe is only an advisory control-plane wake.  Every hardware owner also
 * polls the published PTT request, so a full pipe can never delay a fail-safe
 * transmitter release indefinitely.
 */
URP_CHANNEL_LOCAL int hidthread_open_pttkick(struct chan_usbradio_pvt *o)
{
	hidthread_close_pttkick(o);
	if (pipe(o->pttkick) == -1) {
		ast_log(LOG_ERROR, "Channel %s: Is not able to create a pipe\n", o->name);
		return -1;
	}
	if (fcntl(o->pttkick[0], F_SETFL, fcntl(o->pttkick[0], F_GETFL) | O_NONBLOCK) ||
	    fcntl(o->pttkick[1], F_SETFL, fcntl(o->pttkick[1], F_GETFL) | O_NONBLOCK)) {
		ast_log(LOG_ERROR, "Channel %s: Is not able to make the wake pipe nonblocking\n",
			o->name);
		close(o->pttkick[0]);
		close(o->pttkick[1]);
		o->pttkick[0] = -1;
		o->pttkick[1] = -1;
		return -1;
	}
	return 0;
}

/**
 * @brief Retire the advisory PTT wake pipe after its hardware worker exits.
 * @param o Channel whose stopped worker no longer observes wake requests.
 */
URP_CHANNEL_LOCAL void hidthread_close_pttkick(struct chan_usbradio_pvt *o)
{
	if (o->pttkick[0] != -1) {
		close(o->pttkick[0]);
		o->pttkick[0] = -1;
	}
	if (o->pttkick[1] != -1) {
		close(o->pttkick[1]);
		o->pttkick[1] = -1;
	}
}

#ifdef URP_HAVE_GPIO_POC
/**
 * @brief Create or refresh the radio engine after a hardware owner is ready.
 * @param o Channel whose device-independent radio state is prepared.
 * @return Zero on success, or minus one when radio initialization fails.
 *
 * Hardware-specific workers call this only after device identity and mixer
 * state are known.  Keeping the preparation here lets a selected hardware
 * adapter replace HID ownership without duplicating radio configuration.
 */
URP_CHANNEL_LOCAL int hidthread_prepare_radio(struct chan_usbradio_pvt *o)
{
	if (o->radio == NULL) {
		urp_radio_state tChan;

		memset(&tChan, 0, sizeof(tChan));
		tChan.pTxCodeDefault = o->txctcssdefault;
		tChan.pRxCodeSrc = o->rxctcssfreqs;
		tChan.pTxCodeSrc = o->txctcssfreqs;
		tChan.rxDemod = o->rxdemod;
		tChan.rxCdType = effective_rxcdtype(o);
		tChan.voxHangTime = o->voxhangtime;
		tChan.rxSqVoxAdj = o->rxsqvoxadj;
		tChan.txMixA = effective_txmixa(o);
		tChan.txMixB = effective_txmixb(o);
		tChan.rxCpuSaver = o->rxcpusaver;
		tChan.txCpuSaver = o->txcpusaver;
		tChan.b.rxpolarity = o->rxpolarity;
		tChan.b.txpolarity = o->txpolarity;
		ast_copy_string(tChan.dcsRxCode, o->dcs_receive_code, sizeof(tChan.dcsRxCode));
		ast_copy_string(tChan.dcsTxCode, o->dcs_transmit_code, sizeof(tChan.dcsTxCode));
		tChan.dcsTurnoffEnabled = o->dcs_turnoff_enabled;
		tChan.dcsTurnoffDuration = o->dcs_turnoff_duration_ms;
		tChan.dcsPeak = o->dcs_level;
		tChan.txCtcssTocShift = o->ctcss_phase_shift_degrees;
		tChan.txCtcssTocTime = o->ctcss_tail_duration_ms;
		tChan.txCtcssTocToneHz = o->ctcss_tail_frequency_hz;
		tChan.b.lsdrxpolarity = o->lsdrxpolarity;
		tChan.b.lsdtxpolarity = o->lsdtxpolarity;
		tChan.tracetype = o->tracetype;
		tChan.tracelevel = o->tracelevel;
		tChan.rptnum = o->rptnum;
		tChan.idleinterval = o->idleinterval;
		tChan.turnoffs = o->turnoffs;
		tChan.area = o->area;
		tChan.ukey = o->ukey;
		tChan.name = o->name;
		tChan.fever = o->fever;

		o->radio = urp_radio_create(&tChan, FRAME_SIZE);
		if (!o->radio) {
			ast_log(LOG_ERROR, "Channel %s: signaling engine initialization failed\n",
				o->name);
			return -1;
		}
		o->radio->radioDuplex = o->plus_advanced || o->radioduplex;
		o->radio->b.loopback = 0;
		o->radio->txsettletime = o->txsettletime;
		o->radio->txrxblankingtime = o->txrxblankingtime;
		o->radio->rxCpuSaver = o->rxcpusaver;
		o->radio->txCpuSaver = o->txcpusaver;
		*(o->radio->prxSquelchAdjust) =
			((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
		*(o->radio->prxVoiceAdjust) = effective_rx_decoder_gain(o) * M_Q8;
		*(o->radio->prxCtcssAdjust) = o->rxctcssadj * M_Q8;
		o->radio->rxCtcss->relax = o->rxctcssrelax;
		o->radio->txTocType = o->txtoctype;
		if (urp_tx_pair_has_tone((enum urp_tx_output_mode)o->txmixa,
					 (enum urp_tx_output_mode)o->txmixb))
			set_txctcss_level(o);
		if (!urp_tx_pair_has_voice((enum urp_tx_output_mode)o->txmixa,
					   (enum urp_tx_output_mode)o->txmixb))
			ast_log(LOG_ERROR, "Channel %s: No txvoice output configured.\n", o->name);
		if (o->radioactive) {
			struct chan_usbradio_pvt *active_channel;

			for (active_channel = usbradio_default.next; active_channel;
			     active_channel = active_channel->next)
				active_channel->radioactive = 0;
			usbradio_active = o->name;
			o->radioactive = 1;
			ast_log(LOG_NOTICE, "radio active set to [%s]\n", o->name);
		}
	}
	radio_config(o);
	mixer_write(o);
	mult_set(o);
	if (apply_processing_config_overrides(o, o->name))
		ast_log(LOG_WARNING, "Unable to apply current RadioPlus settings for %s\n",
			o->name);
	/* Processing hardware overrides are applied by the hardware-control owner.
	 * The native callback only consumes the published atomics. */
	refresh_processing_hardware(o);
	mult_set(o);
	set_txctcss_level(o);
	ast_mutex_lock(&o->eepromlock);
	if (o->wanteeprom)
		o->eepromctl = 1;
	ast_mutex_unlock(&o->eepromlock);
	usbradioplus_native_renderer_bind_radio(o);
	return 0;
}

/**
 * @brief Start the selected PCM owner after radio preparation.
 * @param o Channel whose audio backend is started.
 * @return Zero on success, or minus one when the selected backend fails.
 */
URP_CHANNEL_LOCAL int hidthread_start_audio(struct chan_usbradio_pvt *o)
{
	if (!usbradioplus_portaudio_poc_start(o))
		return 0;
	ast_log(LOG_ERROR, "Channel %s: unable to start PortAudio stream\n", o->name);
	return -1;
}

/**
 * @brief Report whether one channel participates in the shared parallel transport.
 * @param o Candidate configured channel.
 * @return Nonzero when the channel uses the configured shared transport.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_parallel_requested(const struct chan_usbradio_pvt *o)
{
	return o && o->plus_cm119_gpio_poc && haspp;
}

/**
 * @brief Require the selected audio and GPIO hardware composition.
 * @param o Candidate configured channel.
 * @return Nonzero when a retired hardware backend is selected.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_validate(const struct chan_usbradio_pvt *o)
{
	return !o->plus_portaudio_poc || !o->plus_cm119_gpio_poc;
}

/**
 * @brief Prepare the released audio/GPIO composition for one combined POC attempt.
 * @param o Channel selecting the direct PortAudio and CM119 GPIO proofs.
 * @return Zero when the facade is ready, or minus one when identity cannot be proved.
 *
 * A combined proof binds its PCM, HID, and semantic mixer controls to one
 * CM119. It fails closed rather than falling back to an independent numeric
 * ALSA-card selection when the released composition cannot establish that fact.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_prepare_hardware_adapter(struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_adapter_config config = {
		.struct_size = sizeof(config),
		.abi_version = USBRADIOPLUS_HARDWARE_ADAPTER_ABI_VERSION,
		.usb_serial = ast_strlen_zero(o->serial) ? NULL : o->serial,
		.device_selection_policy =
			ast_strlen_zero(o->devstr) && ast_strlen_zero(o->serial) &&
					ast_strlen_zero(o->plus_cm119_gpio_usb_port_path)
				? RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD
				: RPTADV_AUDIO_USB_SELECTION_EXACT,
		.device_identifier = ast_strlen_zero(o->devstr) ? NULL : o->devstr,
		.usb_port_path = ast_strlen_zero(o->devstr) &&
						 !ast_strlen_zero(o->plus_cm119_gpio_usb_port_path)
					 ? o->plus_cm119_gpio_usb_port_path
					 : NULL,
		.cm119_profile = (uint32_t)o->hdwtype,
		.ptt_inverted = !!o->invertptt,
		.input_device_channels = 1U,
		.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS,
		/* The established HID configuration includes PTT in the control mask.
		 * The GPIO adapter owns PTT separately, leaving only ordinary output
		 * bits (including a configured clip LED) in this request. */
		.gpio_output_enable_mask = (uint32_t)(o->hid_gpio_ctl & ~o->hid_io_ptt),
		.gpio_output_initial_mask =
			(uint32_t)(o->hid_gpio_val & (o->hid_gpio_ctl & ~o->hid_io_ptt)),
	};
	enum usbradioplus_hardware_adapter_result result;
	enum usbradioplus_portaudio_poc_identity_result identity_result;

	/* Semantic mixer handles retain facade-owned adapter state and must always
	 * disappear before a retry releases that state. */
	usbradioplus_hardware_mixer_poc_close(&o->plus_hardware_mixer_poc);
	usbradioplus_hardware_adapter_close(&o->plus_hardware_adapter);
	o->plus_hardware_adapter_prepared = 0;
	result = usbradioplus_hardware_adapter_prepare_released(&o->plus_hardware_adapter, &config);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		ast_log(LOG_WARNING,
			"Channel %s: CM119 GPIO proof cannot prove audio/GPIO identity (%d)\n",
			o->name, result);
		return -1;
	}
	identity_result = usbradioplus_portaudio_poc_combined_facade_validate(
		&o->plus_hardware_adapter, 1, o->plus_cm119_gpio_usb_port_path);
	if (identity_result != USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK) {
		ast_log(LOG_ERROR,
			"Channel %s: CM119 GPIO proof rejected unproven audio/GPIO identity (%d)\n",
			o->name, identity_result);
		usbradioplus_hardware_adapter_close(&o->plus_hardware_adapter);
		return -1;
	}
	o->plus_hardware_adapter_prepared = 1;
	ast_log(LOG_NOTICE, "Channel %s: CM119 composition facade bound audio and GPIO at USB %s\n",
		o->name, o->plus_hardware_adapter.usb_port_path);
	return 0;
}

/**
 * @brief Release a prepared composition after its stream and facade owners stop.
 * @param o Channel holding the combined POC composition.
 */
URP_CHANNEL_LOCAL void cm119_gpio_poc_discard_hardware_adapter(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&o->usblock);
	usbradioplus_hardware_mixer_poc_close(&o->plus_hardware_mixer_poc);
	ast_mutex_unlock(&o->usblock);
	ast_mutex_lock(&pp_lock);
	if (parallel_owner == o) {
		struct chan_usbradio_pvt *next;
		parallel_owner = NULL;
		/* Transfer the one physical port without resetting other nodes' pins. */
		for (next = usbradio_default.next; next; next = next->next) {
			if (next != o && next->plus_hardware_adapter_prepared &&
			    cm119_gpio_poc_parallel_requested(next) &&
			    atomic_load_explicit(&next->plus_hardware_online,
						 memory_order_acquire) &&
			    !atomic_load_explicit(&next->plus_hardware_stop_request,
						  memory_order_acquire)) {
				if (usbradioplus_parallel_adapter_poc_transfer(
					    &o->plus_parallel_adapter_poc,
					    &o->plus_hardware_adapter,
					    &next->plus_parallel_adapter_poc,
					    &next->plus_hardware_adapter))
					parallel_owner = next;
				break;
			}
		}
	}
	usbradioplus_parallel_adapter_poc_reset(&o->plus_parallel_adapter_poc);
	usbradioplus_hardware_adapter_close(&o->plus_hardware_adapter);
	o->plus_hardware_adapter_prepared = 0;
	ast_mutex_unlock(&pp_lock);
	o->plus_hardware_gpio_poc_state = (struct usbradioplus_hardware_gpio_poc_state){0};
	o->plus_hardware_gpio_poc_cancel_mask = 0U;
}

/**
 * @brief Apply the selected RX capture and TX A/B mixer state through the facade.
 * @param o Non-NULL channel whose combined proof owns semantic mixer controls.
 * @return Zero on success, or minus one after latching a hardware fault.
 *
 * This is control-plane work. It never touches a legacy mixer card and it is
 * deliberately separate from the native callback, which consumes only the
 * already-published radio state.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_apply_mixer(struct chan_usbradio_pvt *o)
{
	const int rx = effective_rxmixerset(o);
	const int tx_a = effective_txmixaset(o);
	const int tx_b = effective_txmixbset(o);
	enum usbradioplus_hardware_adapter_result result;

	/* Validated finite settings and urp_gain_db_to_mixer bound all gains to 0..999. */
	if (!o->plus_hardware_adapter_prepared || !o->plus_hardware_mixer_poc.opened)
		return -1;
	ast_mutex_lock(&o->usblock);
	result = usbradioplus_hardware_mixer_poc_apply(&o->plus_hardware_mixer_poc, (uint32_t)rx,
						       (uint32_t)tx_a, (uint32_t)tx_b);
	ast_mutex_unlock(&o->usblock);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return 0;
	ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof semantic mixer update failed (%d)\n",
		o->name, result);
	/* A partially applied gain state must not remain live. Retire these handles
	 * now; the hardware worker sees the latch, unkeys, and retries the binding. */
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);
	usbradioplus_hardware_mixer_poc_close(&o->plus_hardware_mixer_poc);
	return -1;
}

/**
 * @brief Set the combined proof's normalized RX capture gain during calibration.
 * @param o Channel whose semantic RX mixer path is open.
 * @param value Inclusive normalized gain from zero through 999.
 * @return Zero on success, or minus one after latching a hardware fault.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_set_rx_mixer(struct chan_usbradio_pvt *o, int value)
{
	enum usbradioplus_hardware_adapter_result result;

	if (!o || !o->plus_hardware_adapter_prepared || !o->plus_hardware_mixer_poc.opened ||
	    value < 0 || value > (int)RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return -1;
	ast_mutex_lock(&o->usblock);
	result = usbradioplus_hardware_mixer_poc_set_normalized(
		&o->plus_hardware_mixer_poc, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE,
		(uint32_t)value);
	ast_mutex_unlock(&o->usblock);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return 0;
	ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof RX capture mixer update failed (%d)\n",
		o->name, result);
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);
	usbradioplus_hardware_mixer_poc_close(&o->plus_hardware_mixer_poc);
	return -1;
}

/**
 * @brief Open and apply every semantic mixer control required by the combined proof.
 * @param o Channel whose facade already identifies the selected CM119.
 * @return Zero on success, or minus one when semantic setup fails closed.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_open_mixer(struct chan_usbradio_pvt *o)
{
	enum usbradioplus_hardware_adapter_result result;

	if (!o || !o->plus_hardware_adapter_prepared)
		return -1;
	result = usbradioplus_hardware_mixer_poc_open(&o->plus_hardware_mixer_poc,
						      &o->plus_hardware_adapter);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		o->micplaymax = usbradioplus_hardware_mixer_poc_sidetone_available(
					&o->plus_hardware_mixer_poc)
					? 999
					: 0;
		if (!o->plus_advanced && o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE &&
		    !o->micplaymax) {
			ast_log(LOG_ERROR,
				"Channel %s: hardware local repeat requires a sidetone control\n",
				o->name);
		} else if (!cm119_gpio_poc_apply_mixer(o)) {
			return 0;
		}
	}
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		ast_log(LOG_ERROR,
			"Channel %s: CM119 GPIO proof cannot open semantic RX/TX mixer controls "
			"(%d)\n",
			o->name, result);
	usbradioplus_hardware_mixer_poc_close(&o->plus_hardware_mixer_poc);
	return -1;
}

/**
 * @brief Reserve a selected CM119 identity without resolving a legacy mixer card.
 * @param o Channel whose adapter-owned USB identity is reserved.
 * @return Zero on success, or minus one when another channel already owns it.
 *
 * The former device allocator reserved a numeric
 * ALSA mixer. The semantic bridge now owns that hardware control. This small
 * registry reservation remains only to keep the normal legacy allocator from
 * claiming the same configured interface while the combined proof is live.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_reserve_device_identity(struct chan_usbradio_pvt *o)
{
	struct chan_usbradio_pvt *other;

	ast_mutex_lock(&usb_dev_lock);
	o->hasusb = 0;
	o->usbass = 0;
	o->devicenum = (int)o->plus_hardware_adapter.audio_selection.alsa_card_index;
	for (other = usbradio_default.next; other; other = other->next) {
		if (other != o && other->usbass &&
		    !strcmp(other->plus_hardware_adapter.usb_port_path,
			    o->plus_hardware_adapter.usb_port_path))
			break;
	}
	if (other) {
		ast_log(LOG_ERROR,
			"Channel %s: Device string %s is already assigned to channel %s\n", o->name,
			o->devstr, other->name);
		ast_mutex_unlock(&usb_dev_lock);
		return -1;
	}
	o->usbass = 1;
	ast_mutex_unlock(&usb_dev_lock);
	/* The direct proof exposes the same portable scale as processing and tuning.
	 * No physical ALSA mixer step count is retained or translated here. */
	o->micmax = (int)RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM;
	o->spkrmax = (int)RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM;
	o->micplaymax = 0;
	o->newname = 0;
	ast_log(LOG_NOTICE, "Channel %s: CM119 GPIO proof reserves adapter identity %s at USB %s\n",
		o->name, o->devstr, o->plus_hardware_adapter.usb_port_path);
	return 0;
}

/**
 * @brief Release a prior adapter-identity reservation after the worker stops.
 * @param o Channel whose device reservation is no longer live.
 */
URP_CHANNEL_LOCAL void cm119_gpio_poc_release_device_identity(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&usb_dev_lock);
	o->hasusb = 0;
	o->usbass = 0;
	o->devicenum = 0;
	ast_mutex_unlock(&usb_dev_lock);
}

/**
 * @brief Drain advisory PTT wake bytes without making wake delivery mandatory.
 * @param o Channel owning the nonblocking wake pipe.
 * @return Zero on success, or minus one on a non-recoverable pipe failure.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_drain_pttkick(const struct chan_usbradio_pvt *o)
{
	char byte;
	int bytes;

	do {
		bytes = read(o->pttkick[0], &byte, 1);
	} while (bytes > 0);
	if (bytes < 0 && errno != EAGAIN
#if EWOULDBLOCK != EAGAIN
	    && errno != EWOULDBLOCK
#endif
	)
		return -1;
	return 0;
}

/**
 * \brief Return a monotonic millisecond timestamp for GPIO pulse scheduling.
 * \return Current monotonic time in milliseconds, with an Asterisk-time fallback.
 *
 * The GPIO facade owns pulse timing, but this bridge retains the legacy rule
 * that an active clip LED pulse is not extended by another clip request.
 */
URP_CHANNEL_LOCAL uint64_t cm119_gpio_poc_monotonic_milliseconds(void)
{
	struct timespec now;
	struct timeval fallback;

	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
	fallback = ast_tvnow();
	return (uint64_t)fallback.tv_sec * 1000U + (uint64_t)fallback.tv_usec / 1000U;
}

/**
 * @brief Service one pending legacy tuning-EEPROM command through the GPIO facade.
 * @param o Channel whose direct CM119 worker owns the GPIO device.
 *
 * The legacy tuner continues to exchange a zero-based 13-word image. The
 * GPIO adapter owns physical addresses, checksum validation, and restoration
 * of its latest output state after the transfer. This control-plane helper is
 * called by the same worker that services GPIO, never by the native callback.
 */
URP_CHANNEL_LOCAL void cm119_gpio_poc_service_eeprom(struct chan_usbradio_pvt *o)
{
	struct rptadv_gpio_eeprom_image image;
	enum usbradioplus_hardware_adapter_result result;
	const size_t user_word_count = sizeof(o->eeprom) / sizeof(o->eeprom[0]);
	unsigned char command;

	_Static_assert(EEPROM_USER_LEN == USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT,
		       "CM119 EEPROM user region must retain the established 13-word layout");
	_Static_assert(sizeof(o->eeprom[0]) == sizeof(uint16_t),
		       "legacy EEPROM words must remain 16-bit");
	if (!o->wanteeprom)
		return;

	ast_mutex_lock(&o->eepromlock);
	command = (unsigned char)o->eepromctl;
	if (!command) {
		ast_mutex_unlock(&o->eepromlock);
		return;
	}
	memset(&image, 0, sizeof(image));
	if (command == 1U) {
		image.struct_size = sizeof(image);
		result = usbradioplus_hardware_adapter_read_eeprom(&o->plus_hardware_adapter,
								   &image);
		if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK &&
		    !usbradioplus_hardware_eeprom_poc_import(&image, (uint16_t *)o->eeprom,
							     user_word_count)) {
			o->rxmixerset = o->eeprom[EEPROM_USER_RXMIXERSET];
			o->txmixaset = o->eeprom[EEPROM_USER_TXMIXASET];
			o->txmixbset = o->eeprom[EEPROM_USER_TXMIXBSET];
			o->txctcssadj = o->eeprom[EEPROM_USER_TXCTCSSADJ];
			o->rxsquelchadj = o->eeprom[EEPROM_USER_RXSQUELCHADJ];
			mixer_write(o);
			mult_set(o);
			set_txctcss_level(o);
			ast_log(LOG_NOTICE, "Channel %s: EEPROM Loaded\n", o->name);
		} else if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
			ast_log(LOG_ERROR, "Channel %s: EEPROM bad magic number or checksum\n",
				o->name);
		} else {
			ast_log(LOG_ERROR,
				"Channel %s: USB adapter has no EEPROM installed or checksum is "
				"bad\n",
				o->name);
		}
	} else if (command == 2U) {
		/* Both arrays and the exact user-word count are guaranteed above. */
		(void)usbradioplus_hardware_eeprom_poc_export((const uint16_t *)o->eeprom,
							      user_word_count, &image);
		if (usbradioplus_hardware_adapter_write_eeprom(&o->plus_hardware_adapter, &image) ==
		    USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
			ast_log(LOG_NOTICE, "Channel %s: USB parameters written to EEPROM\n",
				o->name);
		} else {
			ast_log(LOG_ERROR, "Channel %s: Unable to write tuning EEPROM\n", o->name);
		}
	}
	o->eepromctl = 0;
	ast_mutex_unlock(&o->eepromlock);
}

/** @brief Queue one established parallel-input text event to the Asterisk owner. */
URP_CHANNEL_LOCAL void cm119_gpio_poc_parallel_input_event(void *opaque, unsigned int pin,
							   int value)
{
	struct chan_usbradio_pvt *o = opaque;
	struct ast_frame frame = {
		.frametype = AST_FRAME_TEXT,
		.src = __PRETTY_FUNCTION__,
	};
	char text[32];

	if (!o || !o->owner)
		return;
	snprintf(text, sizeof(text), "PP%u %d\n", pin, value);
	frame.data.ptr = text;
	frame.datalen = strlen(text);
	ast_queue_frame(o->owner, &frame);
}

/**
 * @brief Service the optional adapter-owned legacy parallel transport.
 * @param o Channel whose direct proof owns the worker.
 * @param hardware_inputs Optional recipient for mapped parallel COS and CTCSS bits.
 * @param force_unkey Nonzero performs the fail-safe parallel PTT/RTX release only.
 * @return Zero on success, or minus one after a facade failure.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_service_parallel(struct chan_usbradio_pvt *o,
						      unsigned int *hardware_inputs,
						      int force_unkey)
{
	struct usbradioplus_parallel_adapter_poc_service_request request = {
		.ptt_asserted =
			atomic_load_explicit(&o->plus_hardware_ptt_request, memory_order_acquire),
		.ptt_inverted = o->invertptt,
		.ptt_mask = hidthread_parallel_ptt_mask(o),
		.force_unkey = force_unkey,
	};
	struct rptadv_gpio_parallel_input_snapshot inputs = {
		.struct_size = sizeof(inputs),
	};
	struct rptadv_gpio_parallel_stats stats = {
		.struct_size = sizeof(stats),
	};
	struct usbradioplus_radio_program_request program;
	enum usbradioplus_hardware_adapter_result result;
	uint32_t translated;
	uint8_t output;
	int had_input;
	int last_input;
	struct chan_usbradio_pvt *owner;

	if (!cm119_gpio_poc_parallel_requested(o))
		return 0;
	request.have_program = !force_unkey && usbradioplus_read_radio_program_request(o, &program);
	if (request.have_program) {
		request.program_generation = program.generation;
		request.rx_frequency_hz = program.rx_frequency;
		request.tx_frequency_hz = program.tx_frequency;
		request.high_power = program.high_power;
	}
	ast_mutex_lock(&pp_lock);
	owner = parallel_owner;
	if (!owner) {
		ast_mutex_unlock(&pp_lock);
		return force_unkey ? 0 : -1;
	}
	output = (uint8_t)pp_val;
	result = usbradioplus_parallel_adapter_poc_service(
		&owner->plus_parallel_adapter_poc, &owner->plus_hardware_adapter,
		&o->plus_parallel_adapter_poc, &request, &output, hardware_inputs ? &inputs : NULL,
		hardware_inputs ? &stats : NULL);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		pp_val = (int8_t)usbradioplus_parallel_adapter_poc_persistent_output(
			&owner->plus_parallel_adapter_poc);
	ast_mutex_unlock(&pp_lock);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return -1;
	if (!hardware_inputs)
		return 0;
	had_input = o->had_pp_in;
	last_input = o->last_pp_in;
	translated = usbradioplus_parallel_adapter_poc_translate_inputs(
		(uint8_t)inputs.status_mask, o->pps, &had_input, &last_input,
		cm119_gpio_poc_parallel_input_event, o);
	o->had_pp_in = (char)had_input;
	o->last_pp_in = (int8_t)last_input;
	if (translated & USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CARRIER)
		*hardware_inputs |= URP_HARDWARE_INPUT_PARALLEL_CARRIER;
	if (translated & USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CTCSS)
		*hardware_inputs |= URP_HARDWARE_INPUT_PARALLEL_CTCSS;
	return 0;
}

/**
 * @brief Apply the latest PTT request and publish one CM119 HID input snapshot.
 * @param o Channel whose atomics bridge native audio and the hardware owner.
 * @return Zero after a complete service cycle, or minus one on hardware failure.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_service(struct chan_usbradio_pvt *o)
{
	struct rptadv_gpio_input_snapshot inputs = {
		.struct_size = sizeof(inputs),
	};
	struct rptadv_gpio_device_stats stats = {
		.struct_size = sizeof(stats),
	};
	unsigned int hardware_inputs = 0U;
	enum usbradioplus_hardware_adapter_result result;
	uint64_t monotonic_milliseconds;
	uint32_t clip_requested;

	/* The adapter publishes output state lock-free. Keep the legacy control
	 * fields coherent while copying them, then perform the only HID I/O in the
	 * facade service call below after releasing this control-plane mutex. */
	ast_mutex_lock(&o->usblock);
	result = usbradioplus_hardware_mixer_poc_set_sidetone(
		&o->plus_hardware_mixer_poc, (uint32_t)o->duplex3,
		!o->plus_advanced && o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE &&
			atomic_load_explicit(&o->plus_portaudio_delivered_keyed,
					     memory_order_acquire));
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		ast_mutex_unlock(&o->usblock);
		return -1;
	}
	monotonic_milliseconds = cm119_gpio_poc_monotonic_milliseconds();
	clip_requested = (uint32_t)atomic_exchange_explicit(&o->plus_clip_led_request, 0,
							    memory_order_acq_rel);
	result = usbradioplus_hardware_gpio_poc_publish(
		&o->plus_hardware_adapter, &o->plus_hardware_gpio_poc_state,
		(uint32_t)atomic_load_explicit(&o->plus_hardware_ptt_request, memory_order_acquire),
		o->plus_hardware_adapter.gpio_output_enable_mask, (uint32_t)o->hid_gpio_val,
		o->hid_gpio_pulsetimer, ARRAY_LEN(o->hid_gpio_pulsetimer),
		&o->plus_hardware_gpio_poc_cancel_mask,
		o->clipledgpio > 0 ? UINT32_C(1) << (o->clipledgpio - 1) : 0U, clip_requested,
		CLIP_LED_HOLD_TIME_MS, monotonic_milliseconds);
	o->gpio_set = 0;
	ast_mutex_unlock(&o->usblock);

	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK ||
	    usbradioplus_hardware_adapter_service_gpio(&o->plus_hardware_adapter, &inputs,
						       &stats) != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return -1;
	cm119_gpio_poc_service_eeprom(o);
	if (cm119_gpio_poc_service_parallel(o, &hardware_inputs, 0))
		return -1;
	if (inputs.cor_active)
		hardware_inputs |= URP_HARDWARE_INPUT_HID_CARRIER;
	if (inputs.ctcss_active)
		hardware_inputs |= URP_HARDWARE_INPUT_HID_CTCSS;
	usbradioplus_publish_hardware_inputs(o, hardware_inputs);
	atomic_store_explicit(&o->plus_hardware_ptt_applied, !!stats.ptt_applied,
			      memory_order_release);
	atomic_store_explicit(&o->plus_hardware_online, !!stats.online, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_last_service_time, (long long)time(NULL),
			      memory_order_release);
	return 0;
}

/**
 * @brief Stop direct audio, unkey through the adapter, and clear published state.
 * @param o Channel whose experimental hardware worker is being retired.
 */
URP_CHANNEL_LOCAL void cm119_gpio_poc_stop(struct chan_usbradio_pvt *o)
{
	struct rptadv_gpio_output_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.ptt_asserted = 0U,
	};

	if (o->plus_hardware_adapter_prepared) {
		ast_mutex_lock(&o->usblock);
		action.gpio_output_mask = (uint32_t)o->hid_gpio_val &
					  o->plus_hardware_adapter.gpio_output_enable_mask;
		ast_mutex_unlock(&o->usblock);
	}

	usbradioplus_portaudio_poc_stop(o);
	atomic_store_explicit(&o->plus_hardware_ptt_request, 0, memory_order_release);
	if (o->plus_hardware_adapter_prepared) {
		if (cm119_gpio_poc_service_parallel(o, NULL, 1))
			ast_log(LOG_WARNING,
				"Channel %s: adapter parallel transport could not complete its "
				"unkey\n",
				o->name);
		(void)usbradioplus_hardware_adapter_publish_gpio(&o->plus_hardware_adapter,
								 &action);
		(void)usbradioplus_hardware_adapter_service_gpio(&o->plus_hardware_adapter, NULL,
								 NULL);
	}
	usbradioplus_publish_hardware_inputs(o, 0U);
	atomic_store_explicit(&o->plus_hardware_ptt_applied, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_last_service_time, 0, memory_order_release);
	cm119_gpio_poc_discard_hardware_adapter(o);
}

/** \brief Return whether the legacy channel has requested worker shutdown. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_stop_requested(void *opaque)
{
	const struct chan_usbradio_pvt *o = opaque;

	return atomic_load_explicit(&o->plus_hardware_stop_request, memory_order_acquire);
}

/** \brief Return whether the legacy worker considers its composed device online. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_online(void *opaque)
{
	const struct chan_usbradio_pvt *o = opaque;

	return atomic_load_explicit(&o->plus_hardware_online, memory_order_acquire);
}

/** \brief Clear legacy-published direct-hardware state before one worker lifetime. */
URP_CHANNEL_LOCAL void cm119_gpio_poc_worker_clear_published_state(void *opaque)
{
	struct chan_usbradio_pvt *o = opaque;

	atomic_store_explicit(&o->plus_hardware_ptt_applied, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_last_service_time, 0, memory_order_release);
	usbradioplus_publish_hardware_inputs(o, 0U);
}

/** \brief Validate one legacy CM119 GPIO proof configuration. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_validate(void *opaque)
{
	return cm119_gpio_poc_validate(opaque);
}

/**
 * \brief Start one complete legacy CM119 GPIO ownership attempt.
 * \param opaque Legacy channel-private state.
 * \return Zero on success, or minus one after adapter-specific diagnostics.
 *
 * The common worker owns retry and cleanup.  This thin callback retains the
 * legacy adapter's device reservation, semantic mixer, and Asterisk radio
 * preparation because those operations depend on its private channel layout.
 */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_start_attempt(void *opaque)
{
	struct chan_usbradio_pvt *o = opaque;
	enum usbradioplus_hardware_adapter_result hardware_result;

	if (cm119_gpio_poc_prepare_hardware_adapter(o)) {
		ast_log(LOG_ERROR,
			"Channel %s: CM119 GPIO proof will not start without a proven "
			"audio/GPIO identity\n",
			o->name);
		return -1;
	}
	if (cm119_gpio_poc_reserve_device_identity(o) || cm119_gpio_poc_open_mixer(o))
		return -1;
	hardware_result = usbradioplus_hardware_adapter_open_gpio(&o->plus_hardware_adapter);
	if (hardware_result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		ast_log(LOG_ERROR,
			"Channel %s: CM119 GPIO proof cannot claim composed CM119 %s "
			"(result %d)\n",
			o->name, o->plus_hardware_adapter.usb_port_path, hardware_result);
		return -1;
	}
	if (cm119_gpio_poc_parallel_requested(o)) {
		ast_mutex_lock(&pp_lock);
		usbradioplus_parallel_adapter_poc_init(&o->plus_parallel_adapter_poc);
		hardware_result = USBRADIOPLUS_HARDWARE_ADAPTER_OK;
		if (!parallel_owner) {
			struct chan_usbradio_pvt *channel;
			uint8_t initial_output = (uint8_t)pp_val;

			/* A failed port may retain the last keyed baseline. Reopen with
			 * every configured transmitter released; live workers reapply PTT. */
			for (channel = usbradio_default.next; channel; channel = channel->next) {
				struct usbradioplus_radio_program_request program;
				const uint8_t mask = hidthread_parallel_ptt_mask(channel);

				initial_output &= (uint8_t)~mask;
				if (channel->invertptt)
					initial_output |= mask;
				if (usbradioplus_read_radio_program_request(channel, &program) &&
				    program.rx_frequency)
					initial_output &= (uint8_t)~UINT8_C(0x18);
			}
			hardware_result = usbradioplus_parallel_adapter_poc_open(
				&o->plus_parallel_adapter_poc, &o->plus_hardware_adapter, haspp,
				pport, (uint32_t)pbase, initial_output);
			/* Preserve the configured raw-I/O fallback through the adapter. */
			if (hardware_result != USBRADIOPLUS_HARDWARE_ADAPTER_OK && haspp == 1 &&
			    pbase)
				hardware_result = usbradioplus_parallel_adapter_poc_open(
					&o->plus_parallel_adapter_poc, &o->plus_hardware_adapter, 2,
					NULL, (uint32_t)pbase, initial_output);
			if (hardware_result == USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
				pp_val = (int8_t)initial_output;
				parallel_owner = o;
			}
		}
		ast_mutex_unlock(&pp_lock);
		if (hardware_result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
			ast_log(LOG_ERROR, "Channel %s: cannot claim parallel transport (%d)\n",
				o->name, hardware_result);
			return -1;
		}
	}
	if (hidthread_open_pttkick(o) || hidthread_prepare_radio(o) ||
	    !o->plus_hardware_mixer_poc.opened || hidthread_start_audio(o)) {
		ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof startup failed\n", o->name);
		return -1;
	}
	return 0;
}

/** \brief Service one legacy CM119 GPIO proof cycle. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_service(void *opaque)
{
	return cm119_gpio_poc_service(opaque);
}

/** \brief Publish successful startup and retain the established ownership notice. */
URP_CHANNEL_LOCAL void cm119_gpio_poc_worker_mark_online(void *opaque)
{
	struct chan_usbradio_pvt *o = opaque;

	atomic_store_explicit(&o->plus_hardware_online, 1, memory_order_release);
	ast_log(LOG_NOTICE,
		"Channel %s: CM119 GPIO proof owns USB %s (vendor %04x, product %04x)\n", o->name,
		o->plus_hardware_adapter.usb_port_path,
		o->plus_hardware_adapter.gpio_info.vendor_id,
		o->plus_hardware_adapter.gpio_info.product_id);
}

/** \brief Return the legacy advisory PTT wake pipe's read descriptor. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_wake_read_fd(void *opaque)
{
	return ((const struct chan_usbradio_pvt *)opaque)->pttkick[0];
}

/** \brief Drain pending bytes from the legacy advisory PTT wake pipe. */
URP_CHANNEL_LOCAL int cm119_gpio_poc_worker_drain_wake(void *opaque)
{
	return cm119_gpio_poc_drain_pttkick(opaque);
}

/** \brief Stop one legacy attempt before its identity reservation is released. */
URP_CHANNEL_LOCAL void cm119_gpio_poc_worker_stop_attempt(void *opaque)
{
	cm119_gpio_poc_stop(opaque);
}

/** \brief Release the legacy device reservation after an attempt stops. */
URP_CHANNEL_LOCAL void cm119_gpio_poc_worker_release_identity(void *opaque)
{
	cm119_gpio_poc_release_device_identity(opaque);
}

/** \brief Translate common worker failure classifications into legacy diagnostics. */
URP_CHANNEL_LOCAL void
cm119_gpio_poc_worker_report(void *opaque, enum usbradioplus_cm119_gpio_poc_worker_event event,
			     int detail)
{
	const struct chan_usbradio_pvt *o = opaque;

	switch (event) {
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED:
		ast_log(LOG_ERROR, "Channel %s: CM119 GPIO adapter is unavailable\n", o->name);
		break;
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_INITIAL_SERVICE_FAILED:
		ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof initial HID service failed\n",
			o->name);
		break;
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_POLL_FAILED:
		ast_log(LOG_WARNING, "Channel %s: CM119 GPIO proof poll failed: %s\n", o->name,
			strerror(detail));
		break;
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED:
		ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof wake pipe failed: %s\n", o->name,
			strerror(detail));
		break;
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED:
		ast_log(LOG_ERROR, "Channel %s: CM119 GPIO proof HID service failed\n", o->name);
		break;
	case USBRADIOPLUS_CM119_GPIO_POC_WORKER_START_FAILED:
		/* The start callback retained each established detailed diagnostic. */
		break;
	}
}

/**
 * @brief Own CM119 HID signaling through the opt-in adapter proof.
 * @param arg Private channel state supplied by the Asterisk channel lifecycle.
 * @return Always null after the exclusive hardware worker stops.
 *
 * This is deliberately a narrow ownership replacement: PortAudio owns PCM,
 * this worker owns HID PTT/COR/CTCSS and semantic mixer controls.  No audio
 * callback waits on HID I/O or mixer control I/O.
 */
static void *cm119_gpio_poc_hidthread(void *arg)
{
	const struct usbradioplus_cm119_gpio_poc_worker_ops ops = {
		.struct_size = sizeof(ops),
		.opaque = arg,
		.stop_requested = cm119_gpio_poc_worker_stop_requested,
		.online = cm119_gpio_poc_worker_online,
		.clear_published_state = cm119_gpio_poc_worker_clear_published_state,
		.validate = cm119_gpio_poc_worker_validate,
		.start_attempt = cm119_gpio_poc_worker_start_attempt,
		.service = cm119_gpio_poc_worker_service,
		.mark_online = cm119_gpio_poc_worker_mark_online,
		.wake_read_fd = cm119_gpio_poc_worker_wake_read_fd,
		.drain_wake = cm119_gpio_poc_worker_drain_wake,
		.stop_attempt = cm119_gpio_poc_worker_stop_attempt,
		.release_identity = cm119_gpio_poc_worker_release_identity,
		.report = cm119_gpio_poc_worker_report,
	};

	return usbradioplus_cm119_gpio_poc_worker_run(&ops);
}
#endif

/* Service device controls without involving the native audio callback.
 * The test-visible declaration documents this shared worker entry point. */
URP_CHANNEL_LOCAL void *hidthread(void *arg)
{
	return cm119_gpio_poc_hidthread(arg);
}

/** @brief Start the sole device lifecycle owner after the channel is configured.
 * @param o Configured channel whose device worker will start.
 * @return Zero if started or already running, or minus one on thread creation failure.
 */
static int start_hardware_worker(struct chan_usbradio_pvt *o)
{
	if (o->plus_hardware_worker_started)
		return 0;
	o->stophid = 0;
	atomic_store_explicit(&o->plus_hardware_stop_request, 0, memory_order_release);
	if (ast_pthread_create(&o->hidthread, NULL, hidthread, o)) {
		atomic_store_explicit(&o->plus_hardware_stop_request, 1, memory_order_release);
		return -1;
	}
	o->plus_hardware_worker_started = 1;
	return 0;
}

/** @brief Join the device owner, which first quiesces its callback and delivery worker.
 * @param o Channel whose device worker will stop.
 */
static void stop_hardware_worker(struct chan_usbradio_pvt *o)
{
	if (!o->plus_hardware_worker_started)
		return;
	atomic_store_explicit(&o->plus_hardware_stop_request, 1, memory_order_release);
	o->stophid = 1;
	kickptt(o);
	pthread_join(o->hidthread, NULL);
	o->plus_hardware_worker_started = 0;
	hidthread_close_pttkick(o);
}

URP_CHANNEL_LOCAL int usbradio_text(struct ast_channel *c, const char *text)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	char cmd[16], pwr;
	int cnt, i, j;
	double tx, rx;

#define STR_SZ 15 /* Size of text strings */
	char rxs[STR_SZ + 1], txs[STR_SZ + 1], txpl[STR_SZ + 1], rxpl[STR_SZ + 1];
	if (!o) {
		return -1;
	}

	cmd[0] = rxs[0] = txs[0] = rxpl[0] = txpl[0] = pwr = '\0';

	/* print received messages */
	ast_debug(3, "Channel %s: Console Received usbradio text %s >>\n", o->name, text);

	cnt = sscanf(text, "%15s " S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) "%c",
		     cmd, rxs, txs, rxpl, txpl, &pwr);

	/* set channel on parallel port */
	if (strcmp(cmd, "SETCHAN") == 0) {
		u8 chan;
		chan = strtod(rxs, NULL);
		usbradioplus_set_channel(chan);
		ast_debug(3, "Channel %s: SETCHAN cmd: %s chan: %i\n", o->name, text, chan);
		return 0;
	}

	/* set receive CTCSS */
	if (strcmp(cmd, "RXCTCSS") == 0) {
		u8 x;
		x = strtod(rxs, NULL);
		o->rxctcssoverride = !x;
		ast_debug(3, "Channel %s: RXCTCSS cmd: %s\n", o->name, text);
		return 0;
	}

	/* set transmit CTCSS (app_rpt itxctcss -> TXCTCSS 0/1) */
	if (strcmp(cmd, "TXCTCSS") == 0) {
		if (cnt < 2 || (strcmp(rxs, "0") && strcmp(rxs, "1"))) {
			ast_log(LOG_WARNING, "Channel %s: Invalid TXCTCSS command: %s\n", o->name,
				text);
			return 0;
		}
		if (o->radio)
			o->radio->b.txCtcssOff = rxs[0] != '1';
		ast_debug(3, "Channel %s: TXCTCSS cmd: %s\n", o->name, text);
		return 0;
	}

	/* GPIO command */
	if (!strncmp(text, "GPIO", 4)) {
		cnt = sscanf(text, "%15s " N_FMT(d) " " N_FMT(d), cmd, &i, &j);
		if (cnt < 3) {
			return 0;
		}
		if ((i < 1) || (i > GPIO_PINCOUNT)) {
			return 0;
		}
		i--;
		/* skip if not valid */
		if (!(o->valid_gpios & (1 << i))) {
			return 0;
		}
		ast_mutex_lock(&o->usblock);
		if (j > 1) { /* if to request pulse-age */
			o->hid_gpio_pulsetimer[i] = j - 1;
		} else {
			/* clear pulsetimer, if in the middle of running */
			o->hid_gpio_pulsetimer[i] = 0;
#ifdef URP_HAVE_GPIO_POC
			/* The direct HID proof transfers pulses to the facade immediately, so
			 * retain this explicit legacy cancellation until its next service pass. */
			if (o->plus_cm119_gpio_poc)
				o->plus_hardware_gpio_poc_cancel_mask |= UINT32_C(1) << i;
#endif
			o->hid_gpio_val &= ~(1 << i);
			if (j) {
				o->hid_gpio_val |= 1 << i;
			}
			o->gpio_set = 1;
		}
		ast_mutex_unlock(&o->usblock);
		kickptt(o);
		return 0;
	}

	/* Parallel port command */
	if (!strncmp(text, "PP", 2)) {
		cnt = sscanf(text, "%15s " N_FMT(d) " " N_FMT(d), cmd, &i, &j);
		if (cnt < 3) {
			return 0;
		}
		if ((i < 2) || (i > 9)) {
			return 0;
		}
#ifdef URP_HAVE_GPIO_POC
		{
			struct chan_usbradio_pvt *owner;
			enum usbradioplus_hardware_adapter_result result;
			const uint8_t bit = (uint8_t)(UINT8_C(1) << (i - 2));
			uint8_t output;

			ast_mutex_lock(&pp_lock);
			owner = parallel_owner;
			if (!owner) {
				ast_mutex_unlock(&pp_lock);
				ast_log(LOG_WARNING,
					"Channel %s: parallel transport is unavailable\n", o->name);
				return 0;
			}
			if (j > 1) {
				result = usbradioplus_parallel_adapter_poc_schedule_pulse(
					&owner->plus_parallel_adapter_poc,
					&owner->plus_hardware_adapter, bit, (uint32_t)(j - 1), 0U);
			} else {
				output = usbradioplus_parallel_adapter_poc_persistent_output(
					&owner->plus_parallel_adapter_poc);
				output &= (uint8_t)~bit;
				if (j)
					output |= bit;
				result = usbradioplus_parallel_adapter_poc_schedule_pulse(
					&owner->plus_parallel_adapter_poc,
					&owner->plus_hardware_adapter, 0U, 0U, bit);
				if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
					result = usbradioplus_parallel_adapter_poc_publish_output(
						&owner->plus_parallel_adapter_poc,
						&owner->plus_hardware_adapter, output);
				if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
					pp_val = (int8_t)output;
			}
			kickptt(owner);
			ast_mutex_unlock(&pp_lock);
			if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
				ast_log(LOG_ERROR,
					"Channel %s: adapter parallel output request failed\n",
					o->name);
			return 0;
		}
#endif
		ast_log(LOG_WARNING, "Channel %s: parallel transport is unavailable\n", o->name);
		return 0;
	}

	if (cnt < 6) {
		ast_log(LOG_ERROR, "Channel %s: Cannot parse usbradio text: %s\n", o->name, text);
		return 0;
	} else {
		ast_debug(3, "Channel %s: << %s %s %s %s %s %c >> \n", o->name, cmd, rxs, txs, rxpl,
			  txpl, pwr);
	}

	/* set frequency command */
	if (strcmp(cmd, "SETFREQ") == 0) {
		ast_debug(3, "Channel %s: SETFREQ cmd: %s\n", o->name, text);
		tx = strtod(txs, NULL);
		rx = strtod(rxs, NULL);
		o->set_txfreq = round(tx * (double)1000000);
		o->set_rxfreq = round(rx * (double)1000000);
		o->set_txpower = (pwr == 'H');
		ast_copy_string(o->set_rxctcssfreqs, rxpl, sizeof(o->set_rxctcssfreqs));
		ast_copy_string(o->set_txctcssfreqs, txpl, sizeof(o->set_txctcssfreqs));

		o->remoted = 1;
		radio_config(o);
		return 0;
	}
	ast_log(LOG_ERROR, "Channel %s: Cannot parse usbradio cmd: %s\n", o->name, text);
	return 0;
}

URP_CHANNEL_LOCAL int usbradio_call(struct ast_channel *c, const char *dest, int timeout)
{
	(void)dest;
	(void)timeout;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	if (start_hardware_worker(o))
		return -1;
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

URP_CHANNEL_LOCAL int usbradio_hangup(struct ast_channel *c)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	/* The delivery worker references owner while it queues frames. Quiesce it
	 * before detaching the Asterisk channel from this adapter state. */
	stop_hardware_worker(o);
	ast_channel_tech_pvt_set(c, NULL);
	ast_module_unref(ast_module_info->self);
	/* Keep ownership observable until device and callback activity has quiesced.
	 * Module unload uses owner as its lifetime barrier before it frees native
	 * renderer and signaling state. */
	o->owner = NULL;
	return 0;
}

/* The Asterisk channel callback ABI requires a mutable frame pointer. */
// cppcheck-suppress constParameterCallback
URP_CHANNEL_LOCAL int usbradio_write(struct ast_channel *c, struct ast_frame *f)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	if (!atomic_load_explicit(&o->plus_hardware_online, memory_order_acquire)) {
		return 0;
	}

	/* Preserve app_rpt's continuous stream, including idle silence, so PTT
	 * transitions do not interrupt clock recovery. Echo owns the input while playing. */
	if (o->plus_advanced || !atomic_load_explicit(&o->echoing, memory_order_acquire)) {
		usbradioplus_queue_program(o, f->data.ptr, f->datalen / sizeof(short));
	}

	return 0;
}

URP_CHANNEL_LOCAL struct ast_frame *usbradio_read(struct ast_channel *c)
{
	const struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	const long long last_service =
		atomic_load_explicit(&o->plus_hardware_last_service_time, memory_order_acquire);
	time_t now = time(NULL);

	if (last_service && now - (time_t)last_service > 3) {
		ast_log(LOG_ERROR, "Channel %s: hardware worker is not responding.\n", o->name);
		return NULL;
	}
	/* The callback's bounded handoff is delivered by the non-real-time worker. */
	return &ast_null_frame;
}

URP_CHANNEL_LOCAL struct ast_channel *usbradio_new(struct chan_usbradio_pvt *o, char *ext,
						   char *ctx, int state,
						   const struct ast_assigned_ids *assignedids,
						   const struct ast_channel *requestor)
{
	struct ast_channel *c;

	c = ast_channel_alloc(1, state, NULL, NULL, "", ext, ctx, assignedids, requestor, 0,
			      "RadioPlus/%s", o->name);
	if (c == NULL) {
		return NULL;
	}
	ast_channel_tech_set(c, &usbradio_tech);
	ast_channel_internal_fd_set(c, 0, -1); /* PCM arrives through the delivery queue. */
	ast_channel_nativeformats_set(c, usbradio_tech.capabilities);
	ast_channel_set_readformat(c, ast_format_slin);
	ast_channel_set_writeformat(c, ast_format_slin);
	ast_channel_tech_pvt_set(c, o);
	ast_channel_unlock(c);

	o->owner = c;
	ast_module_ref(ast_module_info->self);
	ast_jb_configure(c, &global_jbconf);
	if (state != AST_STATE_DOWN) {
		if (ast_pbx_start(c)) {
			ast_log(LOG_WARNING, "Channel %s: Unable to start PBX.\n",
				ast_channel_name(c));
			ast_hangup(c);
			o->owner = c = NULL;
			/* XXX what about the channel itself ? */
		}
	}

	return c;
}

URP_CHANNEL_LOCAL struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
						       const struct ast_assigned_ids *assignedids,
						       const struct ast_channel *requestor,
						       const char *data, int *cause)
{
	(void)type;
	struct ast_channel *c;
	struct chan_usbradio_pvt *o = find_desc(data);

	if (!o) {
		ast_log(LOG_WARNING, "Device %s not found.\n", (char *)data);
		return NULL;
	}

	if (!(ast_format_cap_iscompatible(cap, usbradio_tech.capabilities))) {
		struct ast_str *cap_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);
		ast_log(LOG_NOTICE,
			"Channel %s: Channel requested with unsupported format(s): '%s'\n", o->name,
			ast_format_cap_get_names(cap, &cap_buf));
		return NULL;
	}

	if (o->owner) {
		ast_log(LOG_NOTICE,
			"Channel %s: Already have a call (chan %p) on the usb channel\n", o->name,
			o->owner);
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}
	usbradioplus_interface_mode(o, 0);
	c = usbradio_new(o, NULL, NULL, AST_STATE_DOWN, assignedids, requestor);
	if (!c) {
		ast_log(LOG_ERROR, "Channel %s: Unable to create new usb channel\n", o->name);
		return NULL;
	}

	o->remoted = 0;
	radio_config(o);

	return c;
}

URP_CHANNEL_LOCAL int radio_active(int fd, int argc, const char *const *argv)
{
	if (argc == 2) {
		ast_cli(fd, "Active USB Radio device is [%s].\n", usbradio_active);
	} else if (argc != 3) {
		return RESULT_SHOWUSAGE;
	} else {
		struct chan_usbradio_pvt *o;
		if (!strcmp(argv[2], "show")) {
			ast_mutex_lock(&usb_dev_lock);
			for (o = usbradio_default.next; o; o = o->next) {
				ast_cli(fd, "Device [%s] exists as device=%s card=%d\n", o->name,
					o->devstr, o->devicenum);
			}
			ast_mutex_unlock(&usb_dev_lock);
			return RESULT_SUCCESS;
		}
		o = find_desc(argv[2]);
		if (!o) {
			ast_cli(fd, "No device [%s] exists\n", argv[2]);
		} else {
			struct chan_usbradio_pvt *ao;
			for (ao = usbradio_default.next; ao; ao = ao->next) {
				ao->radioactive = 0;
			}
			usbradio_active = o->name;
			o->radioactive = 1;
			ast_cli(fd, "Active (command) USB Radio device set to [%s]\n",
				usbradio_active);
		}
	}
	return RESULT_SUCCESS;
}

int usb_device_swap(int fd, const char *other)
{
	int d;
	int restart_o, restart_p, failed = 0;
	char tmp[128];
	char resolved_o[USBRADIOPLUS_HARDWARE_ADAPTER_USB_PATH_CAPACITY];
	char resolved_p[USBRADIOPLUS_HARDWARE_ADAPTER_USB_PATH_CAPACITY];
	struct chan_usbradio_pvt *p = NULL, *o = find_desc(usbradio_active);

	if (o == NULL) {
		return -1;
	}
	if (!other) {
		return -1;
	}
	p = find_desc(other);
	if (p == NULL) {
		ast_cli(fd, "USB Device %s not found\n", other);
		return -1;
	}
	if (p == o) {
		ast_cli(fd, "You can't swap active device with itself!!\n");
		return -1;
	}
	restart_o = o->plus_hardware_worker_started;
	restart_p = p->plus_hardware_worker_started;
	ast_copy_string(resolved_o, o->plus_hardware_adapter.usb_port_path, sizeof(resolved_o));
	ast_copy_string(resolved_p, p->plus_hardware_adapter.usb_port_path, sizeof(resolved_p));
	/* No callback or control worker may retain either old identity while swapping. */
	stop_hardware_worker(o);
	stop_hardware_worker(p);
	ast_mutex_lock(&usb_dev_lock);
	if (!o->devstr[0] && !o->serial[0] && !o->plus_cm119_gpio_usb_port_path[0])
		ast_copy_string(o->devstr, resolved_o, sizeof(o->devstr));
	if (!p->devstr[0] && !p->serial[0] && !p->plus_cm119_gpio_usb_port_path[0])
		ast_copy_string(p->devstr, resolved_p, sizeof(p->devstr));
	ast_copy_string(tmp, p->devstr, sizeof(tmp));
	d = p->devicenum;
	ast_copy_string(p->devstr, o->devstr, sizeof(p->devstr));
	p->devicenum = o->devicenum;
	ast_copy_string(o->devstr, tmp, sizeof(o->devstr));
	o->devicenum = d;
	ast_copy_string(tmp, p->serial, sizeof(tmp));
	ast_copy_string(p->serial, o->serial, sizeof(p->serial));
	ast_copy_string(o->serial, tmp, sizeof(o->serial));
	ast_copy_string(tmp, p->plus_cm119_gpio_usb_port_path, sizeof(tmp));
	ast_copy_string(p->plus_cm119_gpio_usb_port_path, o->plus_cm119_gpio_usb_port_path,
			sizeof(p->plus_cm119_gpio_usb_port_path));
	ast_copy_string(o->plus_cm119_gpio_usb_port_path, tmp,
			sizeof(o->plus_cm119_gpio_usb_port_path));
	d = p->plus_portaudio_input_device_index;
	p->plus_portaudio_input_device_index = o->plus_portaudio_input_device_index;
	o->plus_portaudio_input_device_index = d;
	d = p->plus_portaudio_output_device_index;
	p->plus_portaudio_output_device_index = o->plus_portaudio_output_device_index;
	o->plus_portaudio_output_device_index = d;
	o->hasusb = 0;
	o->usbass = 0;
	p->hasusb = 0;
	p->usbass = 0;
	ast_mutex_unlock(&usb_dev_lock);
	if (restart_o && start_hardware_worker(o))
		failed = 1;
	if (restart_p && start_hardware_worker(p))
		failed = 1;
	if (failed) {
		ast_cli(fd, "USB assignments swapped, but a hardware worker could not restart.\n");
		return -1;
	}
	ast_cli(fd, "USB Devices successfully swapped.\n");
	return 0;
}

void tune_rxinput(int fd, struct chan_usbradio_pvt *o, int setsql, int intflag)
{
	const int settingmin = 1;
	const int settingstart = 2;
	const int maxtries = 48;

	int target;
	int tolerance = 2750;
	int setting = 0, tries = 0, tmpdiscfactor, meas, measnoise;
	unsigned int rms = 0;
	double peak_dbfs, rms_dbfs;
	float settingmax, f;

	if (o->rxdemod == RX_AUDIO_SPEAKER && o->rxcdtype == CD_XPMR_NOISE) {
		ast_cli(fd, "ERROR: usbradioplus.conf rxdemod=speaker vs. carrierfrom=dsp \n");
	}

	if (o->rxdemod == RX_AUDIO_FLAT) {
		target = 27000;
	} else {
		target = 23000;
	}

	settingmax = o->micmax;
#ifdef URP_HAVE_GPIO_POC
	if (o->plus_cm119_gpio_poc)
		settingmax = (float)RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM;
#endif

	o->fever = 1;
	o->radio->fever = 1;

	o->radio->b.tuning = 1;

	setting = settingstart;

	ast_cli(fd, "tune rxnoise maxtries=%i, target=%i, tolerance=%i\n", maxtries, target,
		tolerance);

	while (tries < maxtries) {
#ifdef URP_HAVE_GPIO_POC
		if (o->plus_cm119_gpio_poc) {
			if (cm119_gpio_poc_set_rx_mixer(o, setting)) {
				ast_cli(fd, "ERROR: RX INPUT ADJUST FAILED: semantic mixer is "
					    "unavailable.\n");
				o->radio->b.tuning = 0;
				return;
			}
		}
#endif

		if (usbradioplus_host_wait_or_poll(fd, 100, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		o->radio->spsMeasure->source = o->radio->spsRx->source;
		o->radio->spsMeasure->discfactor = 2000;
		o->radio->spsMeasure->enabled = 1;
		o->radio->spsMeasure->amax = o->radio->spsMeasure->amin = 0;
		if (usbradioplus_host_wait_or_poll(fd, 400, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		meas = o->radio->spsMeasure->apeak;
		o->radio->spsMeasure->enabled = 0;

		if (!meas) {
			meas++;
		}
		unsigned int stats_index =
			(o->rxaudiostats.index + RPTADV_RADIO_AUDIO_STATS_LEN - 1) %
			RPTADV_RADIO_AUDIO_STATS_LEN;
		rms = (unsigned int)(sqrt((double)o->rxaudiostats.pwrbuf[stats_index]) + 0.5);
		peak_dbfs = 20.0 * log10((double)meas / 32768.0);
		rms_dbfs = rms ? 20.0 * log10((double)rms / 32768.0) : -96.0;
		ast_cli(fd, "tries=%i, setting=%i, Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS)\n",
			tries, setting, meas, peak_dbfs, rms, rms_dbfs);

		if ((meas < (target - tolerance) || meas > (target + tolerance)) && tries <= 2) {
			f = (float)(setting * target) / meas;
			setting = (int)(f + 0.5);
		} else if (meas < (target - tolerance)) {
			setting++;
		} else if (meas > (target + tolerance)) {
			setting--;
		} else if (tries > 5) {
			break;
		}

		if (setting < settingmin) {
			setting = settingmin;
		} else if (setting > settingmax) {
			setting = settingmax;
		}
		tries++;
	}

	/* Measure HF Noise */
	tmpdiscfactor = o->radio->spsRx->discfactor;
	o->radio->spsRx->discfactor = (i16)2000;
	o->radio->spsRx->discounteru = o->radio->spsRx->discounterl = 0;
	o->radio->spsRx->amax = o->radio->spsRx->amin = 0;
	if (usbradioplus_host_wait_or_poll(fd, 200, intflag)) {
		o->radio->b.tuning = 0;
		return;
	}
	measnoise = o->radio->rxRssi;

	/* Measure RSSI */
	o->radio->spsRx->discfactor = tmpdiscfactor;
	o->radio->spsRx->discounteru = o->radio->spsRx->discounterl = 0;
	o->radio->spsRx->amax = o->radio->spsRx->amin = 0;
	if (usbradioplus_host_wait_or_poll(fd, 200, intflag)) {
		o->radio->b.tuning = 0;
		return;
	}

	{
		int normalized_setting = ((setting * 1000) + (o->micmax / 2)) / o->micmax;

#ifdef URP_HAVE_GPIO_POC
		if (o->plus_cm119_gpio_poc)
			normalized_setting = setting;
#endif
		ast_cli(fd,
			"DONE tries=%i, setting=%i, Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS), "
			"sqnoise=%i\n",
			tries, normalized_setting, meas, peak_dbfs, rms, rms_dbfs, measnoise);
	}

	if (meas < target - tolerance || meas > target + tolerance) {
		ast_cli(fd, "ERROR: RX INPUT ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX INPUT ADJUST SUCCESS.\n");
#ifdef URP_HAVE_GPIO_POC
		if (!o->plus_cm119_gpio_poc)
#endif
			setting = ((setting * 1000) + (o->micmax / 2)) / o->micmax;
		usbradioplus_processing_set_hardware_input_gain(o->name,
								urp_mixer_to_gain_db(setting));

		if (o->rxcdtype == CD_XPMR_NOISE) {
			int normRssi = ((32767 - o->radio->rxRssi) * AUDIO_ADJUSTMENT / 32767);

			if ((meas / (measnoise / 10)) > 26) {
				ast_cli(fd, "WARNING: Insufficient high frequency noise from "
					    "receiver.\n");
				ast_cli(fd, "WARNING: Rx input point may be de-emphasized and not "
					    "flat.\n");
				ast_cli(fd, "         usbradioplus.conf setting of "
					    "'carrierfrom=dsp' not recommended.\n");
			} else {
				ast_cli(fd, "Rx noise input seems sufficient for squelch.\n");
			}
			if (setsql) {
				o->rxsquelchadj = normRssi + 150;
				if (o->rxsquelchadj > 999) {
					o->rxsquelchadj = 999;
				}
				*(o->radio->prxSquelchAdjust) =
					((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
				ast_cli(fd, "Rx Squelch set to %d (RSSI=%d).\n", o->rxsquelchadj,
					normRssi);
			} else {
				if (o->rxsquelchadj < normRssi) {
					ast_cli(fd,
						"WARNING: RSSI=%i SQUELCH=%i and is set too "
						"loose.\n",
						normRssi, o->rxsquelchadj);
					ast_cli(fd,
						"         Use 'radio tune rxsquelch' to adjust.\n");
				}
			}
		}
	}
	o->radio->b.tuning = 0;
}

void _menu_rxvoice(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i;
	float f, f1;

	if (!str[0]) {
		if (o->rxdemod == RX_AUDIO_FLAT) {
			ast_cli(fd, "Current Rx voice setting: %d\n",
				(int)((effective_rx_decoder_gain(o) * 200.0) + .5));
		} else {
			ast_cli(fd, "Current Rx voice setting: %d\n", effective_rxmixerset(o));
		}
		return;
	}
	if (parse_tune_level(str, &i)) {
		ast_cli(fd, "Entry Error, Rx voice setting not changed\n");
		return;
	}
	if (o->rxdemod == RX_AUDIO_FLAT) {
		f = (float)i / 200.0;
	} else {
		usbradioplus_processing_set_hardware_input_gain(o->name, urp_mixer_to_gain_db(i));
		/* adjust settings based on the device */
		/* get interval step size */
		f = AUDIO_ADJUSTMENT / (float)o->micmax;

#ifdef URP_HAVE_GPIO_POC
		if (o->plus_cm119_gpio_poc) {
			if (cm119_gpio_poc_set_rx_mixer(o, effective_rxmixerset(o))) {
				ast_cli(fd, "RX mixer is unavailable.\n");
				return;
			}
		}
#endif
		f = 0.5 + (modff(((float)i) / f, &f1) * .093981);
	}
	usbradioplus_processing_set_local_input_gain(o->name,
						     20.0 * log10(fmax(0.000001, 2.0 * f)));
	*(o->radio->prxVoiceAdjust) = f * M_Q8;
	ast_cli(fd, "Changed rx voice setting to %d\n", i);
}

void _menu_print(int fd, struct chan_usbradio_pvt *o)
{
	ast_cli(fd, "Active radio interface is [%s]\n", usbradio_active);
	ast_mutex_lock(&usb_dev_lock);
	ast_cli(fd, "Device String is %s\n", o->devstr);
	if (!ast_strlen_zero(o->serial)) {
		ast_cli(fd, "Device Serial is %s\n", o->serial);
	}
	ast_mutex_unlock(&usb_dev_lock);
#ifdef URP_HAVE_GPIO_POC
	if (o->plus_cm119_gpio_poc && o->plus_hardware_adapter_prepared)
		ast_cli(fd, "Adapter USB topology is %s\n", o->plus_hardware_adapter.usb_port_path);
	else
#endif
		ast_cli(fd, "Card is %i\n", o->devicenum);
	ast_cli(fd, "Output A is currently set to ");
	if (o->txmixa == TX_OUT_COMPOSITE) {
		ast_cli(fd, "composite.\n");
	} else if (o->txmixa == TX_OUT_VOICE) {
		ast_cli(fd, "voice.\n");
	} else if (o->txmixa == TX_OUT_LSD) {
		ast_cli(fd, "tone.\n");
	} else if (o->txmixa == TX_OUT_AUX) {
		ast_cli(fd, "auxvoice.\n");
	} else {
		ast_cli(fd, "off.\n");
	}

	ast_cli(fd, "Output B is currently set to ");
	if (o->txmixb == TX_OUT_COMPOSITE) {
		ast_cli(fd, "composite.\n");
	} else if (o->txmixb == TX_OUT_VOICE) {
		ast_cli(fd, "voice.\n");
	} else if (o->txmixb == TX_OUT_LSD) {
		ast_cli(fd, "tone.\n");
	} else if (o->txmixb == TX_OUT_AUX) {
		ast_cli(fd, "auxvoice.\n");
	} else {
		ast_cli(fd, "off.\n");
	}

	if (o->rxdemod == RX_AUDIO_FLAT) {
		ast_cli(fd, "Rx Level currently set to %d\n",
			(int)((effective_rx_decoder_gain(o) * 200.0) + .5));
	} else {
		ast_cli(fd, "Rx Level currently set to %d\n", effective_rxmixerset(o));
	}
	ast_cli(fd, "Rx Squelch currently set to %d\n", o->rxsquelchadj);
	ast_cli(fd, "Tx Voice Level currently set to %d\n", effective_txmixaset(o));
	ast_cli(fd, "Tx Tone Level currently set to %d\n", o->txctcssadj);
}

// cppcheck-suppress constParameterPointer -- modern backend locks channel device state.
void usbradioplus_tune_mixer_limits(struct chan_usbradio_pvt *channel, int *microphone_max,
				    int *speaker_max, int *microphone_playback_max)
{
	*microphone_max = channel->micmax;
	*speaker_max = channel->spkrmax;
	*microphone_playback_max = channel->micplaymax;
}

void tune_write(struct chan_usbradio_pvt *o)
{
	const float old_rxctcssadj = 0.5; /* for backward EEPROM format compatibility */
	if (save_tuning_config(o))
		ast_log(LOG_WARNING, "Failed to save tuning settings for %s\n", o->name);

	if (o->wanteeprom) {
		ast_mutex_lock(&o->eepromlock);
		usbradioplus_wait_for_eeprom_idle(o);
		memset(o->eeprom, 0, sizeof(o->eeprom));
		o->eeprom[EEPROM_USER_RXMIXERSET] = effective_rxmixerset(o);
		o->eeprom[EEPROM_USER_TXMIXASET] = o->txmixaset;
		o->eeprom[EEPROM_USER_TXMIXBSET] = o->txmixbset;
		memcpy(&o->eeprom[EEPROM_USER_RXCTCSSADJ], &old_rxctcssadj, sizeof(float));
		o->eeprom[EEPROM_USER_TXCTCSSADJ] = o->txctcssadj;
		o->eeprom[EEPROM_USER_RXSQUELCHADJ] = o->rxsquelchadj;
		o->eepromctl = 2; /* request a write */
		ast_mutex_unlock(&o->eepromlock);
	}
}

void mixer_write(struct chan_usbradio_pvt *o)
{
	(void)cm119_gpio_poc_apply_mixer(o);
}

/** @brief Print one integer expression in the diagnostic settings dump. */
#define pd(x)                                                                                      \
	{                                                                                          \
		ast_cli(fd, #x " = %d\n", x);                                                      \
	}

#define pp(x)                                                                                      \
	{                                                                                          \
		ast_cli(fd, #x " = %p\n", x);                                                      \
	}

#define ps(x)                                                                                      \
	{                                                                                          \
		ast_cli(fd, #x " = %s\n", x);                                                      \
	}

#define pf(x)                                                                                      \
	{                                                                                          \
		ast_cli(fd, #x " = %f\n", x);                                                      \
	}

void radio_dump(struct chan_usbradio_pvt *o, int fd)
{
	urp_radio_state *p;
	int i;

	p = o->radio;

	ast_cli(fd, "\nodump()\n");

	pd(o->devicenum);
	ast_mutex_lock(&usb_dev_lock);
	ps(o->devstr);
	ast_mutex_unlock(&usb_dev_lock);

	pd(o->micmax);
	pd(o->spkrmax);

	pd(o->rxdemod);
	pd(o->rxcdtype);
	if (o->rxcdtype == CD_XPMR_VOX) {
		pd(o->voxhangtime);
	}
	pd(o->rxsdtype);
	pd(o->txtoctype);

	ast_cli(fd, "rx mixer = %d\n", effective_rxmixerset(o));

	ast_cli(fd, "rx input gain = %.3f dB\n", effective_rx_input_gain_db(o));
	pd(o->rxsquelchadj);

	ps(o->txctcssdefault);
	ps(o->txctcssfreq);

	pd(o->numrxctcssfreqs);
	pd(o->numtxctcssfreqs);
	if (o->numrxctcssfreqs > 0) {
		for (i = 0; i < o->numrxctcssfreqs; i++) {
			ast_cli(fd, " %i =  %s  %s\n", i, o->rxctcss[i], o->txctcss[i]);
		}
	}
	pd(o->rxpolarity);
	pd(o->txpolarity);

	pd(o->txpreemphasis);
	pd(o->txmixa);
	pd(o->txmixb);

	pd(o->txmixaset);
	pd(o->txmixbset);

	ast_cli(fd, "\nnative radio state\n");

	pd(p->devicenum);

	ast_cli(fd, "prxSquelchAdjust=%i\n", *(o->radio->prxSquelchAdjust));

	pd(p->rxCarrierPoint);
	pd(p->rxCarrierHyst);

	pd(*p->prxVoiceAdjust);
	pd(*p->prxCtcssAdjust);

	pd(o->rxfreq);
	pd(o->txfreq);

	pd(p->rxCtcss->relax);
	pd(p->numrxcodes);
	if (o->radio->numrxcodes > 0) {
		for (i = 0; i < o->radio->numrxcodes; i++) {
			ast_cli(fd, " %i = %s\n", i, o->radio->pRxCode[i]);
		}
	}

	pd(p->txTocType);
	ps(p->pTxCodeDefault);
	pd(p->txcodedefaultsmode);
	pd(p->numtxcodes);
	if (o->radio->numtxcodes > 0) {
		for (i = 0; i < o->radio->numtxcodes; i++) {
			ast_cli(fd, " %i = %s\n", i, o->radio->pTxCode[i]);
		}
	}

	pd(p->b.rxpolarity);
	pd(p->b.txpolarity);
	pd(p->b.lsdrxpolarity);
	pd(p->b.lsdtxpolarity);

	pd(p->txMixA);
	pd(p->txMixB);

	pd(p->rxDeEmpEnable);
	pd(p->rxCenterSlicerEnable);
	pd(p->rxCtcssDecodeEnable);
	pd(p->b.ctcssRxEnable);
	pd(p->b.lmrRxEnable);
	pd(p->b.dstRxEnable);
	pd(p->smode);

	pd(p->txOutputGainA);
	pd(p->txOutputGainB);
	pd(p->txPttIn);
	pd(p->txPttOut);

	pd(p->tracetype);
	pd(p->b.txCtcssOff);
}

/*
	takes data from a chan_usbradio_pvt struct (e.g. o->)
	and configures the native radio detector
*/

struct chan_usbradio_pvt *store_config(const char *ctg)
{
	struct chan_usbradio_pvt *o;
	int i;

	if (ctg == NULL) {
		o = &usbradio_default;
		ctg = "general";
	} else {
		/* "general" is also the default thing */
		if (strcmp(ctg, "general") == 0) {
			o = &usbradio_default;
		} else {
			o = ast_calloc(1, sizeof(*o));
			if (!o) {
				return NULL;
			}
			*o = usbradio_default;
			o->name = ast_strdup(ctg);
			if (!o->name) {
				ast_free(o);
				return NULL;
			}
			o->pttkick[0] = -1;
			o->pttkick[1] = -1;
			if (!usbradio_active) {
				usbradio_active = o->name;
			}
		}
	}
	urp_sample_queue_init(&o->echo_queue, o->echo_samples, URP_ECHO_QUEUE_SAMPLES);
	ast_mutex_init(&o->eepromlock);
	ast_mutex_init(&o->usblock);
	o->echomax = DEFAULT_ECHO_MAX;
	if (o == &usbradio_default) {
		return NULL;
	}
	if (apply_processing_config_overrides(o, ctg)) {
		destroy_unlinked_channel(o);
		return NULL;
	}
	if (o->duplex3 < 0 || o->duplex3 > DUPLEX3_LEVEL_MAX) {
		ast_log(LOG_ERROR, "RadioPlus/%s: duplex3 must be between 0 and %d\n", ctg,
			DUPLEX3_LEVEL_MAX);
		destroy_unlinked_channel(o);
		return NULL;
	}

	/* Keep the configured CTCSS maps even when another receive source is
	 * selected. radio_config() derives active parser inputs per direction. */

	if ((o->txmixa == TX_OUT_COMPOSITE) && (o->txmixb == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have B channel be Voice with A "
				   "channel being Composite!!\n");
	}
	if ((o->txmixb == TX_OUT_COMPOSITE) && (o->txmixa == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have A channel be Voice with B "
				   "channel being Composite!!\n");
	}

	if (o->plus_deemphasis_corner_hz <= 0.0 || o->plus_deemphasis_corner_hz > 500.0 ||
	    o->plus_preemphasis_corner_hz <= 0.0 || o->plus_preemphasis_corner_hz > 500.0) {
		ast_log(LOG_ERROR, "RadioPlus/%s: invalid native DSP configuration\n", o->name);
		destroy_unlinked_channel(o);
		return NULL;
	}
	if (usbradioplus_dsp_init(o)) {
		ast_log(LOG_ERROR, "RadioPlus/%s: native DSP initialization failed\n", o->name);
		destroy_unlinked_channel(o);
		return NULL;
	}

	for (i = 2; i <= 9; i++) {
		/* skip if this one not specified */
		if (!o->pps[i]) {
			continue;
		}
		/* skip if not out or PTT */
		if (strncasecmp(o->pps[i], "out", 3) && strcasecmp(o->pps[i], "ptt")) {
			continue;
		}
		/* if default value is 1, set it */
		if (!strcasecmp(o->pps[i], "out1")) {
			pp_val |= (1 << (i - 2));
		}
	}

	/* if we are using the EEPROM, request hidthread load the EEPROM */
	if (o->wanteeprom) {
		ast_mutex_lock(&o->eepromlock);
		usbradioplus_wait_for_eeprom_idle(o);
		o->eepromctl = 1; /* request a load */
		ast_mutex_unlock(&o->eepromlock);
	}
	o->dsp = ast_dsp_new();
	if (o->dsp) {
		ast_dsp_set_features(o->dsp, DSP_FEATURE_DIGIT_DETECT);
		ast_dsp_set_digitmode(o->dsp, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MUTECONF |
						      DSP_DIGITMODE_RELAXDTMF);
	}
	if (o->rxsqhyst == 0) {
		o->rxsqhyst = 3000;
	}

	if (o->rxsquelchdelay > RXSQDELAYBUFSIZE / 8 - 1) {
		ast_log(LOG_WARNING, "rxsquelchdelay of %i is > maximum of %i. Set to maximum.\n",
			o->rxsquelchdelay, RXSQDELAYBUFSIZE / 8 - 1);
		o->rxsquelchdelay = RXSQDELAYBUFSIZE / 8 - 1;
	}
	{
		urp_radio_state tChan;

		memset(&tChan, 0, sizeof(urp_radio_state));

		tChan.pTxCodeDefault = o->txctcssdefault;
		tChan.pRxCodeSrc = o->rxctcssfreqs;
		tChan.pTxCodeSrc = o->txctcssfreqs;

		tChan.rxDemod = o->rxdemod;
		tChan.rxCdType = effective_rxcdtype(o);
		tChan.voxHangTime = o->voxhangtime;
		tChan.rxCarrierHyst = o->rxsqhyst;
		tChan.rxSqVoxAdj = o->rxsqvoxadj;
		tChan.rxSquelchDelay = o->rxsquelchdelay;

		tChan.txMixA = effective_txmixa(o);
		tChan.txMixB = effective_txmixb(o);

		tChan.rxCpuSaver = o->rxcpusaver;
		tChan.txCpuSaver = o->txcpusaver;

		tChan.b.rxpolarity = o->rxpolarity;
		tChan.b.txpolarity = o->txpolarity;

		ast_copy_string(tChan.dcsRxCode, o->dcs_receive_code, sizeof(tChan.dcsRxCode));
		ast_copy_string(tChan.dcsTxCode, o->dcs_transmit_code, sizeof(tChan.dcsTxCode));
		tChan.dcsTurnoffEnabled = o->dcs_turnoff_enabled;
		tChan.dcsTurnoffDuration = o->dcs_turnoff_duration_ms;
		tChan.dcsPeak = o->dcs_level;
		tChan.txCtcssTocShift = o->ctcss_phase_shift_degrees;
		tChan.txCtcssTocTime = o->ctcss_tail_duration_ms;
		tChan.txCtcssTocToneHz = o->ctcss_tail_frequency_hz;

		tChan.b.lsdrxpolarity = o->lsdrxpolarity;
		tChan.b.lsdtxpolarity = o->lsdtxpolarity;

		tChan.tracetype = o->tracetype;
		tChan.tracelevel = o->tracelevel;

		tChan.rptnum = o->rptnum;
		tChan.idleinterval = o->idleinterval;
		tChan.turnoffs = o->turnoffs;
		tChan.area = o->area;
		tChan.ukey = o->ukey;
		tChan.name = o->name;
		tChan.fever = o->fever;

		tChan.rxhpf = o->rxhpf;
		tChan.rxlpf = o->rxlpf;

		o->radio = urp_radio_create(&tChan, FRAME_SIZE);
		if (!o->radio) {
			ast_log(LOG_ERROR, "RadioPlus/%s: signaling engine initialization failed\n",
				o->name);
			destroy_unlinked_channel(o);
			return NULL;
		}

		o->radio->radioDuplex = o->radioduplex;
		o->radio->b.loopback = 0;
		o->radio->txsettletime = o->txsettletime;
		o->radio->txrxblankingtime = o->txrxblankingtime;
		o->radio->rxCpuSaver = o->rxcpusaver;
		o->radio->txCpuSaver = o->txcpusaver;

		*(o->radio->prxSquelchAdjust) =
			((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
		*(o->radio->prxVoiceAdjust) = effective_rx_decoder_gain(o) * M_Q8;
		*(o->radio->prxCtcssAdjust) = o->rxctcssadj * M_Q8;
		o->radio->rxCtcss->relax = o->rxctcssrelax;
		o->radio->txTocType = o->txtoctype;

		if (!urp_tx_pair_has_voice((enum urp_tx_output_mode)o->txmixa,
					   (enum urp_tx_output_mode)o->txmixb)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		}

		if (o->radioactive) {
			struct chan_usbradio_pvt *ao;
			for (ao = usbradio_default.next; ao; ao = ao->next) {
				ao->radioactive = 0;
			}
			usbradio_active = o->name;
			o->radioactive = 1;
			ast_log(LOG_NOTICE, "radio active set to [%s]\n", o->name);
		}
	}

	hidhdwconfig(o);
	/* DSP is prepared before this late-created signaling engine. Attach its
	 * CTCSS/DCS receive callbacks before the hardware worker starts audio. */
	usbradioplus_native_renderer_bind_radio(o);

	/* The default category returned above; every remaining object is listable. */
	o->next = usbradio_default.next;
	usbradio_default.next = o;
	return o;
}

URP_CHANNEL_LOCAL char *res2cli(int r)
{
	switch (r) {
	case RESULT_SUCCESS:
		return CLI_SUCCESS;
	case RESULT_SHOWUSAGE:
		return CLI_SHOWUSAGE;
	default:
		return CLI_FAILURE;
	}
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_console_key(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus key";
		e->usage = "Usage: radio key\n"
			   "       Simulates COR active.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	return res2cli(console_key(a->fd, a->argc, a->argv));
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_console_unkey(struct ast_cli_entry *e, int cmd,
					     // cppcheck-suppress constParameterCallback
					     struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus unkey";
		e->usage = "Usage: radio unkey\n"
			   "       Simulates COR un-active.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	return res2cli(console_unkey(a->fd, a->argc, a->argv));
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_radio_tune(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus tune "
			     "{auxvoice|dump|swap|rxnoise|rxvoice|rxtone|txvoice|txtone|txall|"
			     "flash|rxsquelch|save|load|menu-support}";
		e->usage = "Usage: radio tune <function>\n"
			   "       rxnoise\n"
			   "       rxvoice\n"
			   "       rxtone\n"
			   "       rxsquelch [newsetting]\n"
			   "       txvoice [newsetting]\n"
			   "       txtone [newsetting]\n"
			   "       auxvoice [newsetting]\n"
			   "       save (settings to tuning file)\n"
			   "       load (tuning settings from EEPROM)\n\n"
			   "       All [newsetting]'s are values 0-999\n\n";

		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	return res2cli(radio_tune(a->fd, a->argc, a->argv));
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_radio_active(struct ast_cli_entry *e, int cmd,
					    // cppcheck-suppress constParameterCallback
					    struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus active";
		e->usage =
			"Usage: radio active [device-name]\n"
			"       If used without a parameter, displays which device is the current\n"
			"       one being commanded.  If a device is specified, the commanded "
			"radio device is changed\n"
			"       to the device specified.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	return res2cli(radio_active(a->fd, a->argc, a->argv));
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_show_settings(struct ast_cli_entry *e, int cmd,
					     // cppcheck-suppress constParameterCallback
					     struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus show settings";
		e->usage = "Usage: radio show settings\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}

	o = find_desc(usbradio_active);
	if (o) {
		_menu_print(a->fd, o);
	}
	return RESULT_SUCCESS;
}

// cppcheck-suppress constParameterCallback -- Asterisk fixes this callback signature.
URP_CHANNEL_LOCAL char *handle_set_dsp_debug(struct ast_cli_entry *e, int cmd,
					     // cppcheck-suppress constParameterCallback
					     struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus set xdebug";
		e->usage = "Usage: radio set xdebug [level]\n"
			   "       Level 0 to 100.\n"
			   "       Set detector debug level.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	return res2cli(radio_set_dsp_debug(a->fd, a->argc, a->argv));
}

#ifdef URP_HAVE_GPIO_POC
/**
 * @brief Append raw stream and callback timing measurements for the CM119 adapter.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Selected channel whose status is being reported.
 *
 * The composition facade owns this immutable adapter snapshot.  This helper
 * runs only from the CLI control plane, never from the PortAudio callback.  It
 * locks the stream lifetime while reading so teardown cannot invalidate the
 * opaque stream handle; an unavailable snapshot is reported without changing
 * channel or callback state.
 */
URP_CHANNEL_LOCAL void radioplus_native_stats_combined_poc(int fd,
							   struct chan_usbradio_pvt *channel)
{
	struct rptadv_audio_stream_stats stream_statistics = {
		.struct_size = sizeof(stream_statistics),
	};
	enum usbradioplus_hardware_adapter_result result;

	if (!channel || !channel->plus_portaudio_poc || !channel->plus_cm119_gpio_poc)
		return;
	ast_mutex_lock(&channel->usblock);
	if (!channel->plus_hardware_adapter_prepared || !channel->plus_portaudio_stream) {
		ast_mutex_unlock(&channel->usblock);
		ast_cli(fd, "PortAudio/CM119 adapter: statistics unavailable.\n");
		return;
	}
	result = usbradioplus_hardware_adapter_stream_get_stats(&channel->plus_hardware_adapter,
								channel->plus_portaudio_stream,
								&stream_statistics);
	ast_mutex_unlock(&channel->usblock);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		ast_cli(fd, "PortAudio/CM119 adapter: statistics unavailable (result %d).\n",
			result);
		return;
	}
	ast_cli(fd,
		"PortAudio/CM119 adapter: input peak %.3f, RMS %.3f, clips %" PRIu64
		", queue %" PRIu64 "/%" PRIu64 "; output peak %.3f, RMS %.3f, clips %" PRIu64
		", queue %" PRIu64 "/%" PRIu64 ", dropped %" PRIu64 "; callbacks %" PRIu64
		"/%" PRIu64 ", oversize %" PRIu64 ", tick failures %" PRIu64
		", input overruns %" PRIu64 ", output underruns %" PRIu64 ", device errors %" PRIu64
		", last error %d.\n",
		stream_statistics.input_peak, stream_statistics.input_rms,
		stream_statistics.input_clip_sample_count,
		stream_statistics.input_queue_occupancy_frames,
		stream_statistics.input_queue_capacity_frames, stream_statistics.output_peak,
		stream_statistics.output_rms, stream_statistics.output_clip_sample_count,
		stream_statistics.output_queue_occupancy_frames,
		stream_statistics.output_queue_capacity_frames,
		stream_statistics.output_queue_dropped_frame_count,
		stream_statistics.callback_count, stream_statistics.callback_frame_count,
		stream_statistics.oversized_callback_count,
		stream_statistics.native_tick_failure_count, stream_statistics.input_overflow_count,
		stream_statistics.output_underflow_count, stream_statistics.device_error_count,
		stream_statistics.last_portaudio_error);
	/* An older adapter writes only its known prefix.  The zero-initialized tail
	 * distinguishes absent timing support from a measured zero-duration sample. */
	if (!stream_statistics.callback_late_start_tolerance_ns) {
		ast_cli(fd, "PortAudio callback timing and xrun timestamps: unavailable.\n");
		return;
	}
	ast_cli(fd,
		"PortAudio callback timing: duration last %.3f/max %.3f ms, start delay last "
		"%.3f/max %.3f ms, late starts %" PRIu64
		" (tolerance %.3f ms), clock errors %" PRIu64 ".\n",
		(double)stream_statistics.callback_last_duration_ns / 1000000.0,
		(double)stream_statistics.callback_max_duration_ns / 1000000.0,
		(double)stream_statistics.callback_last_start_delay_ns / 1000000.0,
		(double)stream_statistics.callback_max_start_delay_ns / 1000000.0,
		stream_statistics.callback_late_start_count,
		(double)stream_statistics.callback_late_start_tolerance_ns / 1000000.0,
		stream_statistics.callback_clock_error_count);
	ast_cli(fd,
		"PortAudio xrun timestamps: last input %.6f s, last output %.6f s "
		"(CLOCK_MONOTONIC since boot; 0 = none recorded).\n",
		(double)stream_statistics.last_input_xrun_monotonic_ns / 1000000000.0,
		(double)stream_statistics.last_output_xrun_monotonic_ns / 1000000000.0);
	/* A zero target identifies adapters predating independent capture. */
	if (stream_statistics.capture_ring_target_frames) {
		ast_cli(fd,
			"PortAudio capture clock: callbacks %" PRIu64 ", ring %" PRIu64 "/%" PRIu64
			" frames, target %" PRIu64 ", correction %+" PRId64 " ppm, missing %" PRIu64
			", dropped %" PRIu64 ", startup wait %" PRIu64 " frames.\n",
			stream_statistics.capture_callback_count,
			stream_statistics.input_queue_occupancy_frames,
			stream_statistics.input_queue_capacity_frames,
			stream_statistics.capture_ring_target_frames,
			stream_statistics.capture_ring_ratio_correction_ppm,
			stream_statistics.capture_ring_missing_frames,
			stream_statistics.capture_ring_dropped_frames,
			stream_statistics.capture_startup_wait_frames);
	}
}
#endif

URP_CHANNEL_LOCAL char *handle_radioplus_native_stats(struct ast_cli_entry *e, int cmd,
						      struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;
	struct rpcr_observation program_observation;
	const struct usbradioplus_native_graph_set *graphs;
	struct usbradioplus_native_renderer_stats statistics;
	const struct usbradioplus_native_filter_statistics *local_filter;
	const struct usbradioplus_native_filter_statistics *final_filter;
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus native stats";
		e->usage = "Usage: radioplus native stats [reset]\n"
			   "       Show native 48 kHz RadioPlus measurements.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (a->argc != 3 && a->argc != 4)
		return CLI_SHOWUSAGE;
	o = find_desc(usbradio_active);
	if (!o)
		return CLI_FAILURE;
	graphs = usbradioplus_native_graphs_acquire(o);
	if (!graphs || !graphs->local_dynamics.configured || !graphs->final.configured) {
		if (graphs)
			usbradioplus_native_graphs_release(o);
		ast_cli(a->fd, "Native FFmpeg processing is not prepared.\n");
		return CLI_FAILURE;
	}
	usbradioplus_native_graphs_release(o);
	if (usbradioplus_native_renderer_stats_read(o, &statistics)) {
		ast_cli(a->fd, "Native renderer measurements are not available.\n");
		return CLI_FAILURE;
	}
	local_filter = &statistics.local_filter;
	final_filter = &statistics.final_filter;
	if (a->argc == 4) {
		if (strcasecmp(a->argv[3], "reset")) {
			return CLI_SHOWUSAGE;
		}
		usbradioplus_native_renderer_stats_reset(o);
		ast_cli(a->fd, "Native renderer meter reset requested.\n");
		return CLI_SUCCESS;
	}
	rpcr_observe(&o->plus_program_ring, &program_observation);
	ast_cli(a->fd,
		"RadioPlus/%s native: frames %" PRIu64 ", SRC errors %" PRIu64
		", ADC peak %.1f/max %.1f dBFS, ADC rails %" PRIu64
		", deemphasis peak %.1f/max %.1f dBFS"
		", preemphasis input %.1f/max %.1f dBFS, input ceiling interventions %" PRIu64
		", local TX peak %.1f dBFS, local TX max %.1f dBFS, local ceiling interventions "
		"%" PRIu64 ", final TX peak %.1f dBFS, final TX max %.1f dBFS"
		", final ceiling interventions %" PRIu64
		", pre gain %.2f dB, program ring %zu/%zu (underruns %" PRIu64 ", overruns %" PRIu64
		"), sound queue dropped frames %" PRIu64 ", short/errors %" PRIu64
		", native echo %s, playback frames %" PRIu64 ", buffered %.2f seconds.\n",
		o->name, statistics.native_frames, statistics.src_errors, statistics.adc_peak_dbfs,
		statistics.adc_max_peak_dbfs, statistics.adc_rail_samples,
		statistics.receive_deemphasis_filter.output_peak_dbfs,
		statistics.receive_deemphasis_filter.output_max_peak_dbfs,
		statistics.preemphasis_input_peak_dbfs, statistics.preemphasis_input_max_peak_dbfs,
		UINT64_C(0), statistics.local_tx_peak_dbfs, statistics.local_tx_max_peak_dbfs,
		UINT64_C(0), statistics.tx_program_peak_dbfs, statistics.tx_program_max_peak_dbfs,
		statistics.tx_program_rail_samples, urp_mixer_to_gain_db(effective_rxmixerset(o)),
		program_observation.available_samples, program_observation.capacity_samples,
		statistics.link_queue_underflows,
		(uint64_t)atomic_load_explicit(&o->plus_program_ring.discarded,
					       memory_order_relaxed),
		o->plus_sound_dropped_frames, o->plus_sound_short_writes,
		statistics.parrot_playing ? "playing" : "idle", statistics.parrot_playback_frames,
		(double)statistics.parrot_samples / URP_RATE_NATIVE);
	ast_cli(a->fd,
		"Program ring: %.2f ms available, clock target %.2f ms, filtered %.2f ms, "
		"ratio %+d ppm.\n",
		1000.0 * program_observation.available_samples / o->plus_app_rpt_rate,
		1000.0 * program_observation.target_samples / o->plus_app_rpt_rate,
		1000.0 * program_observation.filtered_occupancy_samples / o->plus_app_rpt_rate,
		program_observation.ratio_correction_ppm);
	ast_cli(a->fd,
		"Local RNNoise: frames %" PRIu64 ", output %" PRIu64 ", startup %" PRIu64
		", errors %" PRIu64 ", VAD %.2f.\n",
		statistics.rnnoise_frames, statistics.rnnoise_output_samples,
		statistics.rnnoise_startup_samples, statistics.rnnoise_errors,
		statistics.rnnoise_vad_probability);
	ast_cli(a->fd,
		"FFmpeg local: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		local_filter->input_peak_dbfs, local_filter->input_max_peak_dbfs,
		local_filter->input_rms_dbfs, local_filter->input_max_rms_dbfs,
		local_filter->output_peak_dbfs, local_filter->output_max_peak_dbfs,
		local_filter->output_rms_dbfs, local_filter->output_max_rms_dbfs,
		local_filter->latency_samples,
		1000.0 * local_filter->latency_samples / URP_RATE_NATIVE,
		local_filter->buffered_samples, local_filter->startup_fill_samples,
		local_filter->runtime_underrun_samples);
	ast_cli(a->fd,
		"FFmpeg final: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		final_filter->input_peak_dbfs, final_filter->input_max_peak_dbfs,
		final_filter->input_rms_dbfs, final_filter->input_max_rms_dbfs,
		final_filter->output_peak_dbfs, final_filter->output_max_peak_dbfs,
		final_filter->output_rms_dbfs, final_filter->output_max_rms_dbfs,
		final_filter->latency_samples,
		1000.0 * final_filter->latency_samples / URP_RATE_NATIVE,
		final_filter->buffered_samples, final_filter->startup_fill_samples,
		final_filter->runtime_underrun_samples);
	ast_cli(a->fd,
		"FFmpeg final post-limiter band-pass: pre-filter peak %.1f/max %.1f dBFS, RMS "
		"%.1f/max %.1f dBFS; "
		"5-8 kHz pre %.1f/max %.1f, post %.1f/max %.1f dBFS; "
		">8 kHz pre %.1f/max %.1f, post %.1f/max %.1f dBFS.\n",
		final_filter->cleanup_pre_peak_dbfs, final_filter->cleanup_pre_max_peak_dbfs,
		final_filter->cleanup_pre_rms_dbfs, final_filter->cleanup_pre_max_rms_dbfs,
		final_filter->cleanup_pre_5_8_rms_dbfs, final_filter->cleanup_pre_5_8_max_rms_dbfs,
		final_filter->cleanup_post_5_8_rms_dbfs,
		final_filter->cleanup_post_5_8_max_rms_dbfs,
		final_filter->cleanup_pre_8_plus_rms_dbfs,
		final_filter->cleanup_pre_8_plus_max_rms_dbfs,
		final_filter->cleanup_post_8_plus_rms_dbfs,
		final_filter->cleanup_post_8_plus_max_rms_dbfs);
#ifdef URP_HAVE_GPIO_POC
	radioplus_native_stats_combined_poc(a->fd, o);
#endif
	return CLI_SUCCESS;
}

/** Asterisk radio CLI command registrations. */
static struct ast_cli_entry cli_usbradio[] = {
	AST_CLI_DEFINE(handle_console_key, "Simulate Rx Signal Present"),
	AST_CLI_DEFINE(handle_console_unkey, "Simulate Rx Signal Loss"),
	AST_CLI_DEFINE(handle_radio_tune, "Change radio settings"),
	AST_CLI_DEFINE(handle_radio_active, "Change commanded device"),
	AST_CLI_DEFINE(handle_set_dsp_debug, "Radio set detector debug level"),
	AST_CLI_DEFINE(handle_show_settings, "Show device settings"),
	AST_CLI_DEFINE(handle_radioplus_native_stats, "Show native RadioPlus statistics")};

URP_CHANNEL_LOCAL int load_module(void)
{
	usbradio_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!usbradio_tech.capabilities) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_append(usbradio_tech.capabilities, ast_format_slin, 0);

	usbradio_active = NULL;

	/* Copy the default jb config over global_jbconf */
	memcpy(&global_jbconf, &default_jbconf, sizeof(struct ast_jb_conf));

	pp_val = 0;
	if (usbradioplus_processing_prime()) {
		ast_log(LOG_ERROR, "Unable to start RadioPlus processing engine\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	/* load our module configuration */
	if (load_config(0)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (find_desc(usbradio_active) == NULL) {
		ast_log(LOG_NOTICE, "radio active device %s not found\n", usbradio_active);
		/* XXX we could default to 'dsp' perhaps ? */
		/* XXX should cleanup allocated memory etc. */
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_channel_register(&usbradio_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel type 'usb'\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_cli_register_multiple(cli_usbradio,
				  sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));
	if (usbradioplus_processing_load() != AST_MODULE_LOAD_SUCCESS) {
		ast_log(LOG_ERROR, "Unable to start RadioPlus processing engine\n");
		ast_cli_unregister_multiple(cli_usbradio, ARRAY_LEN(cli_usbradio));
		ast_channel_unregister(&usbradio_tech);
		return AST_MODULE_LOAD_FAILURE;
	}
	if (usbradioplus_advanced_register(&usbradio_tech, usbradioplus_configure_advanced)) {
		usbradioplus_processing_unload();
		ast_cli_unregister_multiple(cli_usbradio, ARRAY_LEN(cli_usbradio));
		ast_channel_unregister(&usbradio_tech);
		return AST_MODULE_LOAD_FAILURE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

URP_CHANNEL_LOCAL int unload_module(void)
{
	struct chan_usbradio_pvt *o;

	usbradioplus_advanced_unregister();
	usbradioplus_processing_unload();

	ast_channel_unregister(&usbradio_tech);
	ast_cli_unregister_multiple(cli_usbradio,
				    sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));

	/* Do not free native-renderer state while an Asterisk channel can still enter
	 * the legacy read callback. A soft hangup may finish asynchronously, so keep
	 * the previous unload failure behavior but perform no partial teardown until
	 * every owner has completed usbradio_hangup() and joined its HID worker. */
	for (o = usbradio_default.next; o; o = o->next) {
		if (o->owner) {
			ast_softhangup(o->owner, AST_SOFTHANGUP_APPUNLOAD);
		}
		if (o->owner) { /* XXX how ??? */
			return -1;
		}
	}
	for (o = usbradio_default.next; o; o = o->next) {
#ifdef URP_HAVE_PORTAUDIO_POC
		if (o->plus_portaudio_poc)
			usbradioplus_portaudio_poc_stop(o);
#endif
		usbradioplus_dsp_destroy(o);
		if (o->radio) {
			urp_radio_destroy(o->radio);
		}

		if (o->dsp) {
			ast_dsp_free(o->dsp);
		}
	}

	ao2_cleanup(usbradio_tech.capabilities);
	usbradio_tech.capabilities = NULL;

	return 0;
}

#ifndef URP_CHANNEL_UNIT_TEST
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "USB Radio Plus Channel Driver",
		.support_level = AST_MODULE_SUPPORT_EXTENDED, .load = load_module,
		.unload = unload_module, .reload = reload_module, .requires = "", );
#endif

/** @name File-local and build-time constants
 * @{ */
/** @def CHAN_USBRADIO
 * @brief Enable the channel-driver trace interface.
 */
/** @def DEBUG_USBRADIO
 * @brief Build-time radio driver debug selection.
 */
/** @def PLUS_DYNAMICS_SAMPLES
 * @brief 1 ms control blocks at 48 kHz
 */
/** @def DUPLEX3_LEVEL_MAX
 * @brief Maximum normalized local-repeat level.
 */
/** @def DELIMCHR
 * @brief Configuration token delimiter.
 */
/** @def QUOTECHR
 * @brief Configuration quoted-string delimiter.
 */
/** @def DEFAULT_ECHO_MAX
 * @brief Default app_rpt-rate echo capacity in frames.
 */
/** @def DEFAULT_ECHO_SECONDS
 * @brief Default maximum echo recording duration in seconds.
 */
/** @def PP_MASK
 * @brief Parallel-port bits reserved by radio control.
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
/** @def S_FMT
 * @brief Generate a string-setting format fragment.
 */
/** @def N_FMT
 * @brief Generate a numeric-setting format fragment.
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
/** @def URP_CHANNEL_LOCAL
 * @brief Expose adapter-private functions only to the isolated channel test harness.
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
/** @def STR_SZ
 * @brief Bounded radio text-command field width.
 */
/** @def pp
 * @brief Print a channel pointer field in diagnostic output.
 */
/** @def ps
 * @brief Print a channel string field in diagnostic output.
 */
/** @def pf
 * @brief Print a channel floating-point field in diagnostic output.
 */
/** @} */
