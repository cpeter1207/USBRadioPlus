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
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>
#include <usb.h>
#include <search.h>
#include <alsa/asoundlib.h>
#include <linux/ppdev.h>
#include <linux/parport.h>
#include <linux/version.h>

#include "asterisk/res_usbradio.h"

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

#ifdef URP_CHANNEL_UNIT_TEST
#define URP_CHANNEL_LOCAL
#else

#define URP_CHANNEL_LOCAL static
#endif
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_config.h"
#ifdef __linux
#include <linux/soundcard.h>
#elif defined(__FreeBSD__)
#include <sys/soundcard.h>
#else
#include <soundcard.h>
#endif

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

#define URP_LEGACY_TEST_TONE_PEAK 7518.0

#define CONFIG "usbradioplus.conf" /* default config file */

/** Mutex protecting legacy USB-device allocation. */
AST_MUTEX_DEFINE_STATIC(usb_dev_lock);
/** Mutex protecting shared parallel-port output state. */
ast_mutex_t pp_lock = AST_MUTEX_INIT_VALUE;

/* variables for communicating with the parallel port */
/** Cached parallel-port output byte. */
int8_t pp_val;
/** Parallel outputs with active timed pulses. */
int8_t pp_pulsemask;
/** Previously applied parallel-port pulse mask. */
int8_t pp_lastmask;
/** Remaining pulse duration for each parallel output. */
int pp_pulsetimer[32];
/** Nonzero when parallel-port hardware is available. */
int haspp;
/** Open parallel-port device descriptor. */
int ppfd;
/** Parallel-port device path. */
char pport[50];
/** Parallel-port I/O base address. */
int pbase;
/** Stop request observed by the parallel-port pulse worker. */
char stoppulser;
/** Nonzero when any parallel-port output is configured. */
URP_CHANNEL_LOCAL char hasout;
/** Parallel-port pulse worker thread. */
pthread_t pulserid;

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
	.frags = FRAGS,
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
	/* 750 us land-mobile pre/deemphasis: fc = 1 / (2*pi*750 us). */
	.plus_emphasis_corner_hz = 212.206590789,
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
URP_CHANNEL_LOCAL int setformat(struct chan_usbradio_pvt *o, int mode);
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
URP_CHANNEL_LOCAL const int ppinshift[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 5, 4, 0, 3};

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

URP_CHANNEL_LOCAL char *find_installed_usb_match(void)
{
	struct chan_usbradio_pvt *o = NULL;
	char *match = NULL;

	for (o = usbradio_default.next; o; o = o->next) {
		if (ast_radio_usb_list_check(o->devstr)) {
			match = o->devstr;
			break;
		}
	}

	return match;
}

/** @brief Build the configured parallel-port PTT mask for one hardware worker.
 * @param o Private channel whose parallel assignments are inspected.
 * @return Bit mask of parallel pins assigned to PTT.
 */
static uint8_t hidthread_parallel_ptt_mask(const struct chan_usbradio_pvt *o)
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

/** @brief Apply a PTT request on every configured physical output.
 * @param o Private channel receiving the physical output state.
 * @param usb_handle Open USB interface used for HID output.
 * @param buf Current HID output report.
 * @param bufsave Receives the applied HID output report.
 * @param asserted Nonzero asserts PTT.
 * @param request Radio-programming snapshot, nonnull when @p reprogram is nonzero.
 * @param reprogram Nonzero applies the supplied radio-programming request.
 */
static void hidthread_apply_ptt(struct chan_usbradio_pvt *o, struct usb_dev_handle *usb_handle,
				unsigned char *buf, unsigned char *bufsave, int asserted,
				const struct usbradioplus_radio_program_request *request,
				int reprogram)
{
	const uint8_t parallel_mask = hidthread_parallel_ptt_mask(o);

	if (haspp) {
		ast_mutex_lock(&pp_lock);
		if (reprogram) {
			struct urp_parallel_bus bus = {
				.value = (uint8_t)pp_val,
				.write = usbradioplus_parallel_program_write,
			};

			urp_hardware_program_radio(&bus, request->rx_frequency,
						   request->tx_frequency, asserted,
						   request->high_power);
			pp_val = (int8_t)bus.value;
		} else if (!asserted) {
			struct urp_parallel_bus bus = {
				.value = (uint8_t)pp_val,
				.write = usbradioplus_parallel_program_write,
			};

			/* A concurrent control-plane publication must never postpone an unkey. */
			urp_hardware_clear_transmit(&bus);
			pp_val = (int8_t)bus.value;
		}
		urp_apply_ptt_outputs(asserted, o->invertptt, parallel_mask, o->hid_io_ptt,
				      &o->hid_gpio_val, &pp_val);
		if (parallel_mask)
			ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
		ast_mutex_unlock(&pp_lock);
	} else {
		/* HID PTT remains available on interfaces without a parallel port. */
		urp_apply_ptt_outputs(asserted, o->invertptt, 0, o->hid_io_ptt, &o->hid_gpio_val,
				      &pp_val);
	}
	buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
	buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
	memcpy(bufsave, buf, 4);
	ast_radio_hid_set_outputs(usb_handle, buf);
	o->lasttx = !!asserted;
	/* Publish only after both physical output paths have received their state. */
	atomic_store_explicit(&o->plus_hardware_ptt_applied, !!asserted, memory_order_release);
}

/** @brief Fail safe to an unkeyed state before losing a hardware interface.
 * @param o Private channel whose transmitter must be released.
 * @param usb_handle Optional open USB interface.
 * @param buf HID output report updated to the unkeyed state.
 */
static void hidthread_failsafe_unkey(struct chan_usbradio_pvt *o, struct usb_dev_handle *usb_handle,
				     unsigned char *buf)
{
	const uint8_t parallel_mask = hidthread_parallel_ptt_mask(o);

	if (haspp) {
		struct urp_parallel_bus bus;

		ast_mutex_lock(&pp_lock);
		bus.value = (uint8_t)pp_val;
		bus.write = usbradioplus_parallel_program_write;
		bus.opaque = NULL;
		/* Clear the RTX PTT bit immediately; a serial reprogram cycle must never
		 * be a prerequisite for transmitter release. */
		urp_hardware_clear_transmit(&bus);
		pp_val = (int8_t)bus.value;
		urp_apply_ptt_outputs(0, o->invertptt, parallel_mask, o->hid_io_ptt,
				      &o->hid_gpio_val, &pp_val);
		ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
		ast_mutex_unlock(&pp_lock);
	}
	if (usb_handle) {
		ast_mutex_lock(&o->usblock);
		o->hid_gpio_val &= ~o->hid_io_ptt;
		if (o->invertptt)
			o->hid_gpio_val |= o->hid_io_ptt;
		buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
		buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
		ast_radio_hid_set_outputs(usb_handle, buf);
		ast_mutex_unlock(&o->usblock);
	}
	o->lasttx = 0;
	atomic_store_explicit(&o->plus_hardware_ptt_applied, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);
}

URP_CHANNEL_LOCAL void *hidthread(void *arg)
{
	unsigned char buf[4], bufsave[4], keyed, ctcssed;
	unsigned int program_generation = ~0U;
	struct usbradioplus_radio_program_request program_request;
	char *s;
	register int i, j, k;
	int res;
	struct usb_device *usb_dev;
	struct usb_dev_handle *usb_handle;
	struct chan_usbradio_pvt *o = arg, *ao;
	struct timeval then;
	struct pollfd rfds[1];

	usb_dev = NULL;
	usb_handle = NULL;
	memset(buf, 0, sizeof(buf));
	memset(bufsave, 0, sizeof(bufsave));
	/* enable gpio_set so that we will write GPIO information upon start up */
	o->gpio_set = 1;
	atomic_store_explicit(&o->plus_hardware_ptt_applied, 0, memory_order_release);
	atomic_store_explicit(&o->plus_hardware_online, 0, memory_order_release);

#ifdef HAVE_SYS_IO
	if (haspp == 2) {
		ioperm(pbase, 2, 1);
	}
#endif
	/* This is the main loop for this thread.
	 * It performs setup and initialization of the usb device.
	 * After setup is complete and the device can be accessed,
	 * it enters a processing loop responsible for interacting
	 * with the usb hid device
	 */
	while (!o->stophid) {
		char serial[sizeof(o->serial)] = {'\0'};

		ast_radio_time(&o->lasthidtime);
		ast_mutex_lock(&usb_dev_lock);
		o->hasusb = 0;
		o->usbass = 0;
		o->devicenum = 0;
		if (usb_handle) {
			hidthread_failsafe_unkey(o, usb_handle, buf);
			usb_close(usb_handle);
		}
		usb_handle = NULL;
		usb_dev = NULL;
		ast_radio_hid_device_mklist();

		/* Check to see if our specified device string
		 * matches to a device that is attached to this system, or exists
		 * in our channel configuration.
		 *
		 * If no device string is specified, attempt to assign the first
		 * found device.
		 */
		ast_radio_time(&o->lasthidtime);

		/* If configuration has a serial number defined, find the device */
		if (!ast_strlen_zero(o->serial)) {
			int index;
			char *index_devstr = NULL;

			for (index = 0;; index++) {
				index_devstr = ast_radio_usb_get_devstr(index);
				if (ast_strlen_zero(index_devstr)) {
					/* if no more devices */
					break;
				}

				/* get the device serial number */
				if (ast_radio_usb_get_serial(index_devstr, serial,
							     sizeof(serial)) == 0) {
					/* if no serial number */
					continue;
				}

				if (strcmp(o->serial, serial) == 0) {
					/*
					 * We found a device with the matching serial number, set
					 * the devstr to the matching device.
					 */
					ast_log(LOG_NOTICE, "Matched device serial %s to %s\n",
						o->serial, o->name);
					ast_copy_string(o->devstr, index_devstr, sizeof(o->devstr));
					break;
				}
			}
		}

		/* Automatically assign a devstr if one was not specified in the configuration. */
		if (ast_strlen_zero(o->devstr)) {
			int index = 0;
			char *index_devstr = NULL;

			for (;;) {
				index_devstr = ast_radio_usb_get_devstr(index);
				if (ast_strlen_zero(index_devstr)) {
					if (!o->device_error) {
						ast_log(LOG_ERROR,
							"Channel %s: No USB devices are available "
							"for assignment.\n",
							o->name);
						o->device_error = 1;
					}
					ast_mutex_unlock(&usb_dev_lock);
					usleep(500000);
					break;
				}
				/* We found an available device - see if it already in use */
				for (ao = usbradio_default.next; ao; ao = ao->next) {
					if (ao->usbass && (!strcmp(ao->devstr, index_devstr))) {
						break;
					}
				}
				if (ao) {
					index++;
					continue;
				}
				/* We found an unused device assign it to our node */
				ast_copy_string(o->devstr, index_devstr, sizeof(o->devstr));
				ast_log(LOG_NOTICE,
					"Channel %s: Automatically assigned USB device %s to "
					"USBRadio channel\n",
					o->name, o->devstr);
				if (ast_radio_usb_get_serial(index_devstr, serial, sizeof(serial)) >
				    0) {
					ast_copy_string(o->serial, serial, sizeof(o->serial));
				}
				break;
			}
			if (ast_strlen_zero(o->devstr)) {
				continue;
			}
		}

		if (!ast_radio_usb_list_check(o->devstr)) {
			/* The device string did not match.
			 * Now look through the attached devices and see
			 * one of those is associated with one of our
			 * configured channels.
			 */
			s = find_installed_usb_match();
			if (ast_strlen_zero(s)) {
				if (!o->device_error) {
					ast_log(LOG_ERROR,
						"Channel %s: Device string %s was not found.\n",
						o->name, o->devstr);
					o->device_error = 1;
				}
				ast_mutex_unlock(&usb_dev_lock);
				usleep(500000);
				continue;
			}
			i = ast_radio_usb_get_usbdev(s);
			if (i < 0) {
				ast_mutex_unlock(&usb_dev_lock);
				usleep(500000);
				continue;
			}
			/* See if this device is already assigned to another usb channel */
			for (ao = usbradio_default.next; ao; ao = ao->next) {
				if (ao->usbass && (!strcmp(ao->devstr, s))) {
					break;
				}
			}
			if (ao) {
				ast_log(LOG_ERROR,
					"Channel %s: Device string %s is already assigned to "
					"channel %s",
					o->name, s, ao->name);
				ast_mutex_unlock(&usb_dev_lock);
				usleep(500000);
				continue;
			}
			ast_log(LOG_NOTICE,
				"Channel %s: Assigned USB device %s to usbradio channel\n", o->name,
				s);
			ast_copy_string(o->devstr, s, sizeof(o->devstr));
		}
		/* Double check to see if the device string is assigned to another usb channel */
		for (ao = usbradio_default.next; ao; ao = ao->next) {
			if (ao->usbass && (!strcmp(ao->devstr, o->devstr))) {
				break;
			}
		}
		if (ao) {
			ast_log(LOG_ERROR,
				"Channel %s: Device string %s is already assigned to channel %s",
				o->name, o->devstr, ao->name);
			ast_mutex_unlock(&usb_dev_lock);
			usleep(500000);
			continue;
		}
		/* get the index to the device and assign it to our channel */
		i = ast_radio_usb_get_usbdev(o->devstr);
		if (i < 0) {
			ast_mutex_unlock(&usb_dev_lock);
			usleep(500000);
			continue;
		}
		o->devicenum = i;
		o->device_error = 0;
		ast_radio_time(&o->lasthidtime);
		o->usbass = 1;
		ast_mutex_unlock(&usb_dev_lock);
		/* set the audio mixer values */
		o->micmax = ast_radio_amixer_max(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL);
		o->spkrmax = ast_radio_amixer_max(o->devicenum, MIXER_PARAM_SPKR_PLAYBACK_VOL);
		if (o->spkrmax == -1) {
			o->newname = 1;
			o->spkrmax = ast_radio_amixer_max(o->devicenum,
							  MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW);
		}
		/* initialize the usb device */
		usb_dev = ast_radio_hid_device_init(o->devstr);
		if (usb_dev == NULL) {
			ast_log(LOG_ERROR, "Channel %s: Cannot initialize device %s\n", o->name,
				o->devstr);
			usleep(500000);
			continue;
		}
		/* open the usb device device */
		usb_handle = usb_open(usb_dev);
		if (usb_handle == NULL) {
			ast_log(LOG_ERROR, "Channel %s: Cannot open device %s\n", o->name,
				o->devstr);
			usleep(500000);
			continue;
		}
		/* attempt to claim the usb hid interface and detach from the kernel */
		if (usb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
			if (usb_detach_kernel_driver_np(usb_handle, C108_HID_INTERFACE) < 0) {
				ast_log(LOG_ERROR,
					"Channel %s: Is not able to detach the USB device\n",
					o->name);
				usleep(500000);
				continue;
			}
			if (usb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
				ast_log(LOG_ERROR,
					"Channel %s: Is not able to claim the USB device\n",
					o->name);
				usleep(500000);
				continue;
			}
		}
		/* write initial value to GPIO */
		memset(buf, 0, sizeof(buf));
		buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
		buf[o->hid_gpio_loc] = o->hid_gpio_val;
		ast_radio_hid_set_outputs(usb_handle, buf);
		memcpy(bufsave, buf, sizeof(buf));
		/* setup the pttkick pipe
		 * this pipe is used for timing the main processing loop
		 * it also signaled when the ptt changes to exit the timer
		 */
		if (o->pttkick[0] != -1) {
			close(o->pttkick[0]);
			o->pttkick[0] = -1;
		}
		if (o->pttkick[1] != -1) {
			close(o->pttkick[1]);
			o->pttkick[1] = -1;
		}
		if (pipe(o->pttkick) == -1) {
			ast_log(LOG_ERROR, "Channel %s: Is not able to create a pipe\n", o->name);
			hidthread_failsafe_unkey(o, usb_handle, buf);
			usb_close(usb_handle);
			return NULL;
		}
		/* A control-plane wake is advisory: the worker also polls atomics every
		 * 50 ms.  Never let a full wake pipe block an Asterisk control thread. */
		if (fcntl(o->pttkick[0], F_SETFL, fcntl(o->pttkick[0], F_GETFL) | O_NONBLOCK) ||
		    fcntl(o->pttkick[1], F_SETFL, fcntl(o->pttkick[1], F_GETFL) | O_NONBLOCK)) {
			ast_log(LOG_ERROR,
				"Channel %s: Is not able to make the wake pipe nonblocking\n",
				o->name);
			close(o->pttkick[0]);
			close(o->pttkick[1]);
			o->pttkick[0] = -1;
			o->pttkick[1] = -1;
			hidthread_failsafe_unkey(o, usb_handle, buf);
			usb_close(usb_handle);
			return NULL;
		}
		if ((usb_dev->descriptor.idProduct & 0xfffc) == C108_PRODUCT_ID) {
			o->devtype = C108_PRODUCT_ID;
		} else {
			o->devtype = usb_dev->descriptor.idProduct;
		}
		ast_debug(5, "Channel %s: Starting normally.\n", o->name);
		ast_debug(5, "Channel %s: Attached to usb device %s.\n", o->name, o->devstr);
		/* setup the xmpr subsystem */
		if (o->radio == NULL) {
			urp_radio_state tChan;

			memset(&tChan, 0, sizeof(urp_radio_state));

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

			ast_copy_string(tChan.dcsRxCode, o->dcs_receive_code,
					sizeof(tChan.dcsRxCode));
			ast_copy_string(tChan.dcsTxCode, o->dcs_transmit_code,
					sizeof(tChan.dcsTxCode));
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
				ast_log(LOG_ERROR,
					"Channel %s: signaling engine initialization failed\n",
					o->name);
				usleep(500000);
				continue;
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
						 (enum urp_tx_output_mode)o->txmixb)) {
				set_txctcss_level(o);
			}

			if (!urp_tx_pair_has_voice((enum urp_tx_output_mode)o->txmixa,
						   (enum urp_tx_output_mode)o->txmixb)) {
				ast_log(LOG_ERROR, "Channel %s: No txvoice output configured.\n",
					o->name);
			}

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

		/* Processing hardware overrides are applied by the HID/control thread.
		 * The native callback only consumes the published atomics. */
		refresh_processing_hardware(o);
		mult_set(o);
		set_txctcss_level(o);

		ast_mutex_lock(&o->eepromlock);
		if (o->wanteeprom) {
			o->eepromctl = 1;
		}
		ast_mutex_unlock(&o->eepromlock);

		setformat(o, O_RDWR);
		o->hasusb = 1;
		o->had_gpios_in = 0;
		o->lasttx = 0;
		program_generation = ~0U;
		atomic_store_explicit(&o->plus_hardware_ptt_applied, 0, memory_order_release);
		atomic_store_explicit(&o->plus_hardware_online, 1, memory_order_release);

		memset(&rfds, 0, sizeof(rfds));
		/* The read end is pollable.  Polling the writer can never observe a kick. */
		rfds[0].fd = o->pttkick[0];
		rfds[0].events = POLLIN;

		ast_radio_time(&o->lasthidtime);
		/* Main processing loop for GPIO
		 * This loop process every 50 milliseconds.
		 * The timer can be interrupted by writing to
		 * the pttkick pipe.
		 */
		while ((!o->stophid) && o->hasusb) {
			unsigned int hardware_inputs = 0;

			then = ast_radio_tvnow();
			/* poll the pttkick pipe - timeout after 50 milliseconds */
			res = ast_poll(rfds, 1, 50);
			if (res < 0) {
				ast_log(LOG_WARNING, "Channel %s: Poll failed: %s\n", o->name,
					strerror(errno));
				usleep(10000);
				continue;
			}
			if (rfds[0].revents) {
				char c;
				int bytes;

				do {
					bytes = read(o->pttkick[0], &c, 1);
				} while (bytes > 0);
				if (bytes < 0 && errno != EAGAIN
#if EWOULDBLOCK != EAGAIN
				    && errno != EWOULDBLOCK
#endif
				) {
					ast_log(LOG_ERROR, "Channel %s: pttkick read failed: %s\n",
						o->name, strerror(errno));
				}
			}
			/* see if we need to process an eeprom read or write */
			if (o->wanteeprom) {
				ast_mutex_lock(&o->eepromlock);
				if (o->eepromctl == 1) { /* to read */
					/* if CS okay */
					if (!ast_radio_get_eeprom(usb_handle, o->eeprom)) {
						if (o->eeprom[EEPROM_USER_MAGIC_ADDR] !=
						    EEPROM_MAGIC) {
							ast_log(LOG_ERROR,
								"Channel %s: EEPROM bad magic "
								"number\n",
								o->name);
						} else {
							o->rxmixerset =
								o->eeprom[EEPROM_USER_RXMIXERSET];
							o->txmixaset =
								o->eeprom[EEPROM_USER_TXMIXASET];
							o->txmixbset =
								o->eeprom[EEPROM_USER_TXMIXBSET];
							o->txctcssadj =
								o->eeprom[EEPROM_USER_TXCTCSSADJ];
							o->rxsquelchadj =
								o->eeprom[EEPROM_USER_RXSQUELCHADJ];
							ast_log(LOG_NOTICE,
								"Channel %s: EEPROM Loaded\n",
								o->name);
							mixer_write(o);
							mult_set(o);
							set_txctcss_level(o);
						}
					} else {
						ast_log(LOG_ERROR,
							"Channel %s: USB adapter has no EEPROM "
							"installed or Checksum is bad\n",
							o->name);
					}
					ast_radio_hid_set_outputs(usb_handle, bufsave);
				}
				if (o->eepromctl == 2) { /* to write */
					ast_radio_put_eeprom(usb_handle, o->eeprom);
					ast_radio_hid_set_outputs(usb_handle, bufsave);
					ast_log(LOG_NOTICE,
						"Channel %s: USB parameters written to EEPROM\n",
						o->name);
				}
				o->eepromctl = 0;
				ast_mutex_unlock(&o->eepromlock);
			}
			ast_mutex_lock(&o->usblock);
			buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
			ast_radio_hid_get_inputs(usb_handle, buf);
			/* See if we are keyed */
			keyed = !(buf[o->hid_io_cor_loc] & o->hid_io_cor);
			if (keyed)
				hardware_inputs |= URP_HARDWARE_INPUT_HID_CARRIER;
			/* See if we are receiving ctcss */
			ctcssed = !(buf[o->hid_io_ctcss_loc] & o->hid_io_ctcss);
			if (ctcssed)
				hardware_inputs |= URP_HARDWARE_INPUT_HID_CTCSS;
			/* Get the GPIO information */
			j = buf[o->hid_gpio_loc];
			/* If this device is a CM108AH, map the "HOOK" bit (which used to
			   be GPIO2 in the CM108 into the GPIO position */
			if (o->devtype == C108AH_PRODUCT_ID) {
				j |= 2; /* set GPIO2 bit */
				/* if HOOK is asserted, clear GPIO bit */
				if (buf[o->hid_io_cor_loc] & 0x10) {
					j &= ~2;
				}
			}
			for (i = 0; i < GPIO_PINCOUNT; i++) {
				/* if a valid input bit, dont clear it */
				if ((o->gpios[i]) && (!strcasecmp(o->gpios[i], "in")) &&
				    (o->valid_gpios & (1 << i))) {
					continue;
				}
				j &= ~(1 << i); /* clear the bit, since its not an input */
			}
			if ((!o->had_gpios_in) || (o->last_gpios_in != j)) {
				char buf1[100];
				struct ast_frame fr = {
					.frametype = AST_FRAME_TEXT,
					.src = __PRETTY_FUNCTION__,
				};

				for (i = 0; i < GPIO_PINCOUNT; i++) {
					/* skip if not specified */
					if (!o->gpios[i]) {
						continue;
					}
					/* skip if not input */
					if (strcasecmp(o->gpios[i], "in")) {
						continue;
					}
					/* skip if not a valid GPIO */
					if (!(o->valid_gpios & (1 << i))) {
						continue;
					}
					/* if bit has changed, or never reported */
					if ((!o->had_gpios_in) ||
					    ((o->last_gpios_in & (1 << i)) != (j & (1 << i)))) {
						snprintf(buf1, sizeof(buf1), "GPIO%d %d\n", i + 1,
							 (j & (1 << i)) ? 1 : 0);
						fr.data.ptr = buf1;
						fr.datalen = strlen(buf1);
						ast_queue_frame(o->owner, &fr);
					}
				}
				o->had_gpios_in = 1;
				o->last_gpios_in = j;
			}
			/* process the parallel port GPIO */
			if (haspp) {
				ast_mutex_lock(&pp_lock);
				j = k = ast_radio_ppread(haspp, ppfd, pbase, pport) ^
					0x80; /* get PP input */
				ast_mutex_unlock(&pp_lock);
				for (i = 10; i <= 15; i++) {
					/* if a valid input bit, dont clear it */
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "in"))) {
						continue;
					}
					j &= ~(1 << ppinshift[i]); /* clear the bit, since its not
								      an input */
				}
				if ((!o->had_pp_in) || (o->last_pp_in != j)) {
					char buf1[100];
					struct ast_frame fr = {
						.frametype = AST_FRAME_TEXT,
						.src = __PRETTY_FUNCTION__,
					};

					for (i = 10; i <= 15; i++) {
						/* skip if not specified */
						if (!o->pps[i]) {
							continue;
						}
						/* skip if not input */
						if (strcasecmp(o->pps[i], "in")) {
							continue;
						}
						/* if bit has changed, or never reported */
						if ((!o->had_pp_in) ||
						    ((o->last_pp_in & (1 << ppinshift[i])) !=
						     (j & (1 << ppinshift[i])))) {
							snprintf(buf1, sizeof(buf1), "PP%d %d\n", i,
								 (j & (1 << ppinshift[i])) ? 1 : 0);
							fr.data.ptr = buf1;
							fr.datalen = strlen(buf1);
							ast_queue_frame(o->owner, &fr);
						}
					}
					o->had_pp_in = 1;
					o->last_pp_in = j;
				}
				for (i = 10; i <= 15; i++) {
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "cor"))) {
						j = k &
						    (1
						     << ppinshift[i]); /* set the bit accordingly */
						if (j)
							hardware_inputs |=
								URP_HARDWARE_INPUT_PARALLEL_CARRIER;
					} else if ((o->pps[i]) &&
						   (!strcasecmp(o->pps[i], "ctcss"))) {
						if (k & (1 << ppinshift[i]))
							hardware_inputs |=
								URP_HARDWARE_INPUT_PARALLEL_CTCSS;
					}
				}
			}
			usbradioplus_publish_hardware_inputs(o, hardware_inputs);
			j = ast_tvdiff_ms(ast_radio_tvnow(), then);
			/* make output inversion mask (for pulseage) */
			o->hid_gpio_lastmask = o->hid_gpio_pulsemask;
			o->hid_gpio_pulsemask = 0;
			for (i = 0; i < GPIO_PINCOUNT; i++) {
				k = o->hid_gpio_pulsetimer[i];
				if (k) {
					k -= j;
					if (k < 0) {
						k = 0;
					}
					o->hid_gpio_pulsetimer[i] = k;
				}
				if (k) {
					o->hid_gpio_pulsemask |= 1 << i;
				}
			}
			if (atomic_exchange_explicit(&o->plus_clip_led_request, 0,
						     memory_order_acq_rel) &&
			    o->clipledgpio && !o->hid_gpio_pulsetimer[o->clipledgpio - 1]) {
				/* The callback posts only a request.  This worker owns GPIO timing.
				 */
				o->hid_gpio_pulsetimer[o->clipledgpio - 1] = CLIP_LED_HOLD_TIME_MS;
			}
			if (o->hid_gpio_pulsemask ||
			    o->hid_gpio_lastmask) { /* if anything inverted (temporarily) */
				buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
				buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
				ast_radio_hid_set_outputs(usb_handle, buf);
			}
			if (o->gpio_set) {
				o->gpio_set = 0;
				buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
				buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
				ast_radio_hid_set_outputs(usb_handle, buf);
			}
			{
				const int desired_ptt = atomic_load_explicit(
					&o->plus_hardware_ptt_request, memory_order_acquire);
				const int have_program = usbradioplus_read_radio_program_request(
					o, &program_request);
				const int program_changed =
					have_program &&
					program_request.generation != program_generation;
				const int ptt_changed = o->lasttx != !!desired_ptt;
				/* A missing in-progress program snapshot must never defer PTT
				 * release. */
				const int reprogram =
					have_program && (program_changed || ptt_changed);

				if (program_changed || ptt_changed) {
					hidthread_apply_ptt(
						o, usb_handle, buf, bufsave, desired_ptt,
						have_program ? &program_request : NULL, reprogram);
					if (have_program)
						program_generation = program_request.generation;
				}
			}
			ast_radio_time(&o->lasthidtime);
			ast_mutex_unlock(&o->usblock);
		}
		/* Every disconnect and stop transitions physical PTT through this same
		 * fail-safe path before the worker can reopen or exit. */
		hidthread_failsafe_unkey(o, usb_handle, buf);
	}
	/* clean up before exiting the thread */
	hidthread_failsafe_unkey(o, usb_handle, buf);
	if (usb_handle) {
		usb_close(usb_handle);
		usb_handle = NULL;
	}
	pthread_exit(0);
}

URP_CHANNEL_LOCAL int used_blocks(struct chan_usbradio_pvt *o)
{
	struct audio_buf_info info;

	if (ioctl(o->sounddev, SNDCTL_DSP_GETOSPACE, &info)) {
		if (!(o->warned & WARN_used_blocks)) {
			o->warned |= WARN_used_blocks;
		}
		return 1;
	}

	/* Cache capacity, not the free-space snapshot: idle playback remains active. */
	if (o->total_blocks == 0) {
		o->total_blocks = info.fragstotal;
		/* Check the queue size, it cannot exceed the total fragments */
		if (o->queuesize >= (unsigned int)info.fragstotal) {
			o->queuesize = info.fragstotal - 1;
			if (o->queuesize < 2) {
				o->queuesize = QUEUE_SIZE;
			}
		}
	}

	return o->total_blocks - info.fragments;
}

URP_CHANNEL_LOCAL int soundcard_writeframe(struct chan_usbradio_pvt *o, short *data)
{
	static const short silence[URP_NATIVE_SAMPLES * 2] = {0};
	const short *output = data;
	int res;

	/* If the sound device is not open, setformat will open the device */
	if (o->sounddev < 0) {
		setformat(o, O_RDWR);
	}
	if (o->sounddev < 0) {
		return 0; /* not fatal */
	}
	/* Keep the DAC fed at the capture clock's cadence, including while idle.
	 * PTT selects audio or silence; it never starts or stops device writes. */
	if (!atomic_load_explicit(&o->plus_radio_tx_active, memory_order_acquire)) {
		output = silence;
	}
	/* Bound device latency without adding an extra silent block on an empty queue. */
	res = used_blocks(o);
	if ((unsigned int)res > o->queuesize) { /* no room to write a block */
		o->plus_sound_dropped_frames++;
		return 0;
	}
	res = write(o->sounddev, output, sizeof(silence));
	if (res < 0) {
		o->plus_sound_short_writes++;
	} else if (res != (int)sizeof(silence)) {
		o->plus_sound_short_writes++;
	}

	return res;
}

URP_CHANNEL_LOCAL int setformat(struct chan_usbradio_pvt *o, int mode)
{
	int fmt, desired, res, fd;
	char device[100];

	/* A reopened device may have a different playback-buffer capacity. */
	o->total_blocks = 0;
	/* If the device is open, close it */
	if (o->sounddev >= 0) {
		ioctl(o->sounddev, SNDCTL_DSP_RESET, 0);
		close(o->sounddev);
		o->duplex = M_UNSET;
		o->sounddev = -1;
	}
	if (mode == O_CLOSE) { /* we are done */
		return 0;
	}

	ast_copy_string(device, "/dev/dsp", sizeof(device));
	if (o->devicenum) {
		sprintf(device, "/dev/dsp%d", o->devicenum);
	}
	/* open the device */
	fd = o->sounddev = open(device, mode | O_NONBLOCK);
	if (fd < 0) {
		ast_log(LOG_ERROR, "Channel %s: Unable to open DSP device %d: %s.\n", o->name,
			o->devicenum, strerror(errno));
		return -1;
	}
	if (o->owner) {
		ast_channel_internal_fd_set(o->owner, 0, fd);
	}

#if __BYTE_ORDER == __LITTLE_ENDIAN
	fmt = AFMT_S16_LE;
#else
	fmt = AFMT_S16_BE;
#endif
	res = ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Channel %s: Unable to set format to 16-bit signed\n",
			o->name);
		return -1;
	}
	/* set our duplex mode based on the way we opened the device. */
	switch (mode) {
	case O_RDWR:
		(void)ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);
		/* Check to see if duplex set (FreeBSD Bug) */
		res = ioctl(fd, SNDCTL_DSP_GETCAPS, &fmt);
		if (res == 0 && (fmt & DSP_CAP_DUPLEX)) {
			o->duplex = M_FULL;
		}
		break;
	case O_WRONLY:
		o->duplex = M_WRITE;
		break;
	case O_RDONLY:
		o->duplex = M_READ;
		break;
	default:
		break;
	}

	fmt = 1;
	res = ioctl(fd, SNDCTL_DSP_STEREO, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Channel %s: Failed to set audio device to stereo\n", o->name);
		return -1;
	}
	fmt = desired = 48000; /* 48000 Hz desired */
	res = ioctl(fd, SNDCTL_DSP_SPEED, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Channel %s: Failed to set audio device sample rate.\n",
			o->name);
		return -1;
	}
	if (fmt != desired) {
		if (!(o->warned & WARN_speed)) {
			ast_log(LOG_WARNING,
				"Channel %s: Requested %d Hz, got %d Hz -- sound may be choppy.\n",
				o->name, desired, fmt);
			o->warned |= WARN_speed;
		}
	}
	/*
	 * on Freebsd, SETFRAGMENT does not work very well on some cards.
	 * Default to use 256 bytes, let the user override
	 */
	if (o->frags) {
		fmt = o->frags;
		res = ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fmt);
		if (res < 0) {
			if (!(o->warned & WARN_frag)) {
				ast_log(LOG_WARNING,
					"Channel %s: Unable to set fragment size -- sound may be "
					"choppy.\n",
					o->name);
				o->warned |= WARN_frag;
			}
		}
	}
	/* on some cards, we need SNDCTL_DSP_SETTRIGGER to start outputting */
	res = PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT;
	res = ioctl(fd, SNDCTL_DSP_SETTRIGGER, &res);
	/* it may fail if we are in half duplex, never mind */
	return 0;
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

#ifdef HAVE_SYS_IO
	if (haspp == 2) {
		ioperm(pbase, 2, 1);
	}
#endif

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
		ast_mutex_lock(&pp_lock);
		if (j > 1) { /* if to request pulse-age */
			pp_pulsetimer[i] = j - 1;
		} else {
			/* clear pulsetimer, if in the middle of running */
			pp_pulsetimer[i] = 0;
			pp_val &= ~(1 << (i - 2));
			if (j) {
				pp_val |= 1 << (i - 2);
			}
			ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
		}
		ast_mutex_unlock(&pp_lock);
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

	o->stophid = 0;
	ast_radio_time(&o->lasthidtime);
	ast_pthread_create(&o->hidthread, NULL, hidthread, o);
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

URP_CHANNEL_LOCAL int usbradio_hangup(struct ast_channel *c)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	ast_channel_tech_pvt_set(c, NULL);
	ast_module_unref(ast_module_info->self);
	if (o->hookstate) {
		o->hookstate = 0;
		setformat(o, O_CLOSE);
	}
	o->stophid = 1;
	pthread_join(o->hidthread, NULL);
	/* Keep ownership observable until device and callback activity has quiesced.
	 * Module unload uses owner as its lifetime barrier before it frees the native
	 * worker and signaling state. */
	o->owner = NULL;
	return 0;
}

/* The Asterisk channel callback ABI requires a mutable frame pointer. */
// cppcheck-suppress constParameterCallback
URP_CHANNEL_LOCAL int usbradio_write(struct ast_channel *c, struct ast_frame *f)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	if (!o->hasusb) {
		return 0;
	}
	if (o->sounddev < 0) {
		setformat(o, O_RDWR);
	}
	if (o->sounddev < 0) {
		return 0; /* not fatal */
	}
	/*
	 * we could receive a block which is not a multiple of our
	 * FRAME_SIZE, so buffer it locally and write to the device
	 * in FRAME_SIZE chunks.
	 * Keep the residue stored for future use.
	 */

	/* Preserve app_rpt's continuous stream, including idle silence, so PTT
	 * transitions do not interrupt clock recovery. Echo owns the input while playing. */
	if (o->plus_advanced || !atomic_load_explicit(&o->echoing, memory_order_acquire)) {
		usbradioplus_queue_program(o, f->data.ptr, f->datalen / sizeof(short));
	}

	return 0;
}

URP_CHANNEL_LOCAL struct ast_frame *usbradio_read(struct ast_channel *c)
{
	int res;
	int cd, sd;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	struct ast_frame *f = &o->read_f;
	time_t now;

	/* check to the if the hid thread is still processing */
	if (o->lasthidtime) {
		ast_radio_time(&now);
		if ((now - o->lasthidtime) > 3) {
			ast_log(LOG_ERROR,
				"Channel %s: HID process has died or is not responding.\n",
				o->name);
			return NULL;
		}
	}
	/* Set frame defaults */
	memset(f, 0, sizeof(struct ast_frame));
	f->frametype = AST_FRAME_NULL;
	f->src = __PRETTY_FUNCTION__;

	/* if USB device not ready, just return NULL frame */
	if (!o->hasusb) {
		if (o->rxkeyed) {
			struct ast_frame wf = {
				.frametype = AST_FRAME_CONTROL,
				.subclass.integer = AST_CONTROL_RADIO_UNKEY,
				.src = __PRETTY_FUNCTION__,
			};

			o->lastrx = 0;
			o->rxkeyed = 0;
			ast_queue_frame(o->owner, &wf);
			if (!o->plus_advanced && o->duplex3 &&
			    o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
				ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 0,
						    0);
			}
		}
		return &ast_null_frame;
	}

	/* If we have stopped echoing, clear the echo queue */
	if (o->plus_advanced || !o->echomode) {
		usbradioplus_echo_clear(o);
	}

	/* If we are in echomode and we have stopped receiving audio
	 * queue up the packets we have stored in the echo queue
	 * for playback.
	 */
	if (!o->plus_advanced && o->echomode && !usbradioplus_native_echo(o) && (!o->rxkeyed)) {
		(void)usbradioplus_echo_start(o);
	}

	/* Read audio data from the USB sound device.
	 * Sound data will arrive at 48000 samples per second
	 * in stereo format.
	 */
	res = read(o->sounddev, o->usbradio_read_buf + o->readpos,
		   sizeof(o->usbradio_read_buf) - o->readpos);
	if (res < 0) { /* Audio data not ready, return a NULL frame */
		if (errno != EAGAIN) {
			o->readerrs = 0;
			o->hasusb = 0;
			return &ast_null_frame;
		}
		if (o->readerrs++ > READERR_THRESHOLD) {
			ast_log(LOG_ERROR, "Stuck USB read channel [%s], un-sticking it!\n",
				o->name);
			o->readerrs = 0;
			o->hasusb = 0;
			return &ast_null_frame;
		}
		if (o->readerrs == 1) {
			ast_log(LOG_WARNING, "Possibly stuck USB read channel. [%s]\n", o->name);
		}
		return &ast_null_frame;
	}

	if (o->readerrs) {
		ast_log(LOG_WARNING, "USB read channel [%s] was not stuck.\n", o->name);
	}

	o->readerrs = 0;
	o->readpos += res;
	if ((size_t)o->readpos < sizeof(o->usbradio_read_buf)) { /* not enough samples */
		return &ast_null_frame;
	}

	/* Check for ADC clipping and input audio statistics before any filtering is done.
	 * FRAME_SIZE define refers to 8Ksps mono which is 160 samples per 20mS USB frame.
	 * ast_radio_check_audio() takes the read buffer as received (48K stereo),
	 * extracts the mono 48K channel, checks amplitude and distortion characteristics,
	 * and returns true if clipping was detected.
	 */
	if (ast_radio_check_audio((short *)o->usbradio_read_buf, &o->rxaudiostats,
				  12 * FRAME_SIZE)) {
		usbradioplus_request_clip_led(o);
	}

	/* A processing reload may briefly replace parser-owned CTCSS state.  Do not
	 * wait in this hardware-paced worker: keep the DAC cadence with silence and
	 * resume the normal signaling engine on the next native block. */
	if (!usbradioplus_radio_access_acquire(o)) {
		memset(o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET, 0,
		       o->plus_app_rpt_samples * sizeof(short));
		memset(o->usbradio_write_buf, 0, sizeof(o->usbradio_write_buf));
		atomic_store_explicit(&o->plus_radio_tx_active, 0, memory_order_release);
		soundcard_writeframe(o, (short *)o->usbradio_write_buf);
		o->readpos = AST_FRIENDLY_OFFSET;
		return &ast_null_frame;
	}
	/* HID sampling and physical PTT completion are independent workers. Import
	 * their published snapshot before the audio-owned signaling tick. */
	usbradioplus_audio_load_hardware_state(o);
	/* Only app_rpt and tuning own PTT. */
	if (atomic_load_explicit(&o->txkeyed, memory_order_acquire) ||
	    atomic_load_explicit(&o->txtestkey, memory_order_acquire)) {
		if (!o->radio->txPttIn) {
			o->radio->txPttIn = 1;
		}
	} else if (o->radio->txPttIn) {
		o->radio->txPttIn = 0;
	}
	usbradioplus_prepare_squelch_audio(o);
	urp_radio_process(o->radio, (i16 *)o->plus_squelch_native,
			  (i16 *)(o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
			  (i16 *)(o->usbradio_write_buf));
	atomic_store_explicit(&o->plus_radio_tx_active, o->radio->txPttIn || o->radio->txPttOut,
			      memory_order_release);
	/* The HID worker owns all physical output and consumes this request. */
	usbradioplus_publish_hardware_ptt(o, o->radio->txPttOut);
	usbradioplus_refresh_ctcss_decode(o);
	usbradioplus_native_tick(o);

	/* The native TX queue retires a frame only after OSS accepted the complete
	 * native block. A full device queue therefore retries its existing head
	 * instead of silently discarding program audio. */
	if (soundcard_writeframe(o, (short *)o->usbradio_write_buf) ==
	    (int)(URP_NATIVE_SAMPLES * 2U * sizeof(short)))
		usbradioplus_native_tx_output_ack(o);

	/* Check for carrier detect - COR active */
	{
		enum radio_carrier_detect rxcdtype = effective_rxcdtype(o);
		cd = 0;
		if (rxcdtype == CD_HID && (o->radio->rxExtCarrierDetect != o->rxhidsq)) {
			o->radio->rxExtCarrierDetect = o->rxhidsq;
		}

		if (rxcdtype == CD_HID_INVERT && (o->radio->rxExtCarrierDetect == o->rxhidsq)) {
			o->radio->rxExtCarrierDetect = !o->rxhidsq;
		}

		if (usbradioplus_carrier_detected(o, rxcdtype)) {
			if (!o->radio->txPttOut || o->plus_advanced || o->radioduplex) {
				cd = 1;
			}
		} else {
			cd = 0;
		}

		if (cd != o->rxcarrierdetect) {
			o->rxcarrierdetect = cd;
			ast_debug(3, "Channel %s: rxcarrierdetect = %i.\n", o->name, cd);
		}
		o->rx_cos_active = cd;
	}

	/* Check for SD - CTCSS active. */
	if (usbradioplus_ctcss_detected(o)) {
		sd = 1;
	} else {
		sd = 0;
	}
	if (o->rxsdtype == SD_HID) {
		sd = o->rxhidctcss;
	} else if (o->rxsdtype == SD_HID_INVERT) {
		sd = !o->rxhidctcss;
	} else if (o->rxsdtype == SD_PP) {
		sd = o->rxppctcss;
	} else if (o->rxsdtype == SD_PP_INVERT) {
		sd = !o->rxppctcss;
	}
	/* See if we are overriding CTCSS to active */
	if (o->rxctcssoverride) {
		sd = 1;
	}
	o->rx_ctcss_active = sd;

	/* Special case where cd and sd have been configured for no */
	if (effective_rxcdtype(o) == CD_IGNORE && o->rxsdtype == SD_IGNORE) {
		cd = 0;
		sd = 0;
	}

	/* Timer for how long TX has been unkeyed - used with txoffdelay */
	if (o->txoffdelay) {
		if (atomic_load_explicit(&o->txkeyed, memory_order_acquire) == 1) {
			o->txoffcnt = 0; /* If keyed, set this to zero. */
		} else {
			o->txoffcnt++;
			if (o->txoffcnt > MS_TO_FRAMES(TX_OFF_DELAY_MAX)) {
				o->txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX); /* Limit the count */
			}
		}
	}

	/* Check conditions and set receiver active */
	if (cd && sd) {
		if (!o->rxkeyed) {
			ast_debug(3, "Channel %s: o->rxkeyed = 1.\n", o->name);
		}
		if (o->rxkeyed ||
		    ((o->txoffcnt >= o->txoffdelay) && (o->rxoncnt >= o->rxondelay))) {
			o->rxkeyed = 1;
		} else {
			o->rxoncnt++;
		}
	} else {
		if (o->rxkeyed) {
			ast_debug(3, "Channel %s: o->rxkeyed = 0.\n", o->name);
		}
		o->rxkeyed = 0;
		o->rxoncnt = 0;
	}
	/* If we are in echomode and receiving audio, store
	 * it in the echo queue for later playback.
	 */
	if (!o->plus_advanced && o->echomode && !usbradioplus_native_echo(o) && o->rxkeyed &&
	    (!atomic_load_explicit(&o->echoing, memory_order_acquire))) {
		usbradioplus_echo_record(
			o, (const short *)(o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
			FRAME_SIZE);
	}

	/* Send a message to indicate rx signal detect conditions */
	if (o->lastrx && (!o->rxkeyed)) {
		struct ast_frame wf = {
			.frametype = AST_FRAME_CONTROL,
			.subclass.integer = AST_CONTROL_RADIO_UNKEY,
			.src = __PRETTY_FUNCTION__,
		};

		o->lastrx = 0;
		ast_queue_frame(o->owner, &wf);
		if (!o->plus_advanced && o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
			ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 0, 0);
		}
	} else if ((!o->lastrx) && (o->rxkeyed)) {
		struct ast_frame wf = {
			.frametype = AST_FRAME_CONTROL,
			.subclass.integer = AST_CONTROL_RADIO_KEY,
			.src = __PRETTY_FUNCTION__,
		};

		o->lastrx = 1;
		if (o->rxctcssdecode) {
			wf.data.ptr = o->rxctcssfreq;
			wf.datalen = strlen(o->rxctcssfreq) + 1;
			ast_debug(7, "Radio Key - CTCSS frequency=%s.\n", o->rxctcssfreq);
		}
		ast_queue_frame(o->owner, &wf);
		o->count_rssi_update = 1;
		if (!o->plus_advanced && o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
			ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 1, 0);
		}
	}

	/* reset read pointer for next frame */
	o->readpos = AST_FRIENDLY_OFFSET;
	/* Do not return the frame if the channel is not up */
	if (ast_channel_state(c) != AST_STATE_UP) {
		usbradioplus_radio_access_release(o);
		return &ast_null_frame;
	}
	/* ok we can build and deliver the frame to the caller */
	f->frametype = AST_FRAME_VOICE;
	f->subclass.format = o->plus_advanced
				     ? ast_format_cache_get_slin_by_rate(o->plus_app_rpt_rate)
				     : ast_format_slin;
	f->offset = AST_FRIENDLY_OFFSET;
	f->samples = o->plus_app_rpt_samples;
	f->datalen = f->samples * sizeof(short);
	f->data.ptr = o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET;
	f->src = __PRETTY_FUNCTION__;
	if (!o->rxkeyed) {
		memset(f->data.ptr, 0, f->datalen);
	}
	/* Process the audio to see if contains DTMF */
	if (!o->plus_advanced && o->usedtmf && o->dsp) {
		struct ast_frame *f1 = ast_dsp_process(c, o->dsp, f);
		if ((f1->frametype == AST_FRAME_DTMF_END) ||
		    (f1->frametype == AST_FRAME_DTMF_BEGIN)) {
			if ((f1->subclass.integer == 'm') || (f1->subclass.integer == 'u')) {
				f1->frametype = AST_FRAME_NULL;
				f1->subclass.integer = 0;
				usbradioplus_radio_access_release(o);
				return f1;
			}
			if (f1->frametype == AST_FRAME_DTMF_END) {
				f1->len = ast_tvdiff_ms(ast_radio_tvnow(), o->tonetime);
				if (option_verbose) {
					ast_log(LOG_NOTICE,
						"Channel %s: Got DTMF char %c duration %ld ms\n",
						o->name, f1->subclass.integer, f1->len);
				}
				o->toneflag = 0;
			} else {
				if (o->toneflag) {
					ast_frfree(f1);
					f1 = NULL;
				} else {
					o->tonetime = ast_radio_tvnow();
					o->toneflag = 1;
				}
			}
			if (f1) {
				usbradioplus_radio_access_release(o);
				return f1;
			}
		}
	}

	if (o->radio->b.txCtcssReady) {
		struct ast_frame wf = {
			.frametype = AST_FRAME_TEXT,
			.src = __PRETTY_FUNCTION__,
		};
		char msg[32];

		snprintf(msg, sizeof(msg), "cstx=%.26s", o->radio->txctcssfreq);
		wf.data.ptr = msg;
		wf.datalen = strlen(msg) + 1;
		ast_queue_frame(o->owner, &wf);

		ast_debug(3, "Channel %s: got b.txCtcssReady %s.\n", o->name,
			  o->radio->txctcssfreq);
		o->radio->b.txCtcssReady = 0;
	}
	/* report channel rssi */
	if (o->sendvoter && o->count_rssi_update && o->rxkeyed) {
		if (--o->count_rssi_update <= 0) {
			struct ast_frame wf = {
				.frametype = AST_FRAME_TEXT,
				.src = __PRETTY_FUNCTION__,
			};
			char msg[32];

			snprintf(msg, sizeof(msg), "R %i",
				 ((32767 - o->radio->rxRssi) * 1000) / 32767);
			wf.data.ptr = msg;
			wf.datalen = strlen(msg) + 1;
			ast_queue_frame(o->owner, &wf);

			o->count_rssi_update = 10;
			ast_debug(4, "Channel %s: Count_rssi_update %i\n", o->name,
				  ((32767 - o->radio->rxRssi) * 1000 / 32767));
		}
	}

	usbradioplus_radio_access_release(o);
	return f;
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
	if ((o->sounddev < 0) && o->hasusb) {
		setformat(o, O_RDWR);
	}
	ast_channel_internal_fd_set(c, 0, o->sounddev); /* -1 if device closed, override later */
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
					o->devstr, ast_radio_usb_get_usbdev(o->devstr));
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
	char tmp[128];
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
	ast_mutex_lock(&usb_dev_lock);
	ast_copy_string(tmp, p->devstr, sizeof(tmp));
	d = p->devicenum;
	ast_copy_string(p->devstr, o->devstr, sizeof(p->devstr));
	p->devicenum = o->devicenum;
	ast_copy_string(o->devstr, tmp, sizeof(o->devstr));
	o->devicenum = d;
	o->hasusb = 0;
	o->usbass = 0;
	p->hasusb = 0;
	p->usbass = 0;
	ast_cli(fd, "USB Devices successfully swapped.\n");
	ast_mutex_unlock(&usb_dev_lock);
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

	o->fever = 1;
	o->radio->fever = 1;

	o->radio->b.tuning = 1;

	setting = settingstart;

	ast_cli(fd, "tune rxnoise maxtries=%i, target=%i, tolerance=%i\n", maxtries, target,
		tolerance);

	while (tries < maxtries) {
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, setting, 0);
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, 1, 0);

		if (ast_radio_wait_or_poll(fd, 100, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		o->radio->spsMeasure->source = o->radio->spsRx->source;
		o->radio->spsMeasure->discfactor = 2000;
		o->radio->spsMeasure->enabled = 1;
		o->radio->spsMeasure->amax = o->radio->spsMeasure->amin = 0;
		if (ast_radio_wait_or_poll(fd, 400, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}
		meas = o->radio->spsMeasure->apeak;
		o->radio->spsMeasure->enabled = 0;

		if (!meas) {
			meas++;
		}
		unsigned int stats_index =
			(o->rxaudiostats.index + AUDIO_STATS_LEN - 1) % AUDIO_STATS_LEN;
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
	if (ast_radio_wait_or_poll(fd, 200, intflag)) {
		o->radio->b.tuning = 0;
		return;
	}
	measnoise = o->radio->rxRssi;

	/* Measure RSSI */
	o->radio->spsRx->discfactor = tmpdiscfactor;
	o->radio->spsRx->discounteru = o->radio->spsRx->discounterl = 0;
	o->radio->spsRx->amax = o->radio->spsRx->amin = 0;
	if (ast_radio_wait_or_poll(fd, 200, intflag)) {
		o->radio->b.tuning = 0;
		return;
	}

	ast_cli(fd,
		"DONE tries=%i, setting=%i, Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS), sqnoise=%i\n",
		tries, ((setting * 1000) + (o->micmax / 2)) / o->micmax, meas, peak_dbfs, rms,
		rms_dbfs, measnoise);

	if (meas < target - tolerance || meas > target + tolerance) {
		ast_cli(fd, "ERROR: RX INPUT ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX INPUT ADJUST SUCCESS.\n");
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
		int adjustment = effective_rxmixerset(o) * o->micmax / AUDIO_ADJUSTMENT;
		/* get interval step size */
		f = AUDIO_ADJUSTMENT / (float)o->micmax;

		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, adjustment, 0);
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, 1, 0);
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
	ast_cli(fd, "Card is %i\n", ast_radio_usb_get_usbdev(o->devstr));
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
	ast_cli(fd, "Tx Voice Level currently set to %d\n", o->txmixaset);
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
	int mic_setting;

	if (!o->plus_advanced && o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
		/* Scale the portable 0--999 setting to this CM119 mixer's range. */
		int mixer_level =
			(o->duplex3 * o->micplaymax + DUPLEX3_LEVEL_MAX / 2) / DUPLEX3_LEVEL_MAX;
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_VOL, mixer_level, 0);
	} else {
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_VOL, 0, 0);
	}
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 0, 0);
	ast_radio_setamixer(o->devicenum,
			    (o->newname) ? MIXER_PARAM_SPKR_PLAYBACK_SW_NEW
					 : MIXER_PARAM_SPKR_PLAYBACK_SW,
			    1, 0);
	ast_radio_setamixer(
		o->devicenum,
		(o->newname) ? MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW : MIXER_PARAM_SPKR_PLAYBACK_VOL,
		ast_radio_make_spkr_playback_value(o->spkrmax, effective_txmixaset(o), o->devtype),
		ast_radio_make_spkr_playback_value(o->spkrmax, effective_txmixbset(o), o->devtype));
	/* adjust settings based on the device */
	mic_setting = effective_rxmixerset(o) * o->micmax / AUDIO_ADJUSTMENT;
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, mic_setting, 0);
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, 1, 0);
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_SW, 1, 0);
}

/**
 * \brief Print an integer expression for radio_dump().
 * \param x Expression to print.
 */
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

	if (o->plus_emphasis_corner_hz <= 0.0 || o->plus_emphasis_corner_hz >= 300.0) {
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
		hasout = 1;
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

URP_CHANNEL_LOCAL char *handle_radioplus_native_stats(struct ast_cli_entry *e, int cmd,
						      struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;
	struct rpcr_observation program_observation;
	const struct usbradioplus_native_graph_set *graphs;
	struct usbradioplus_native_worker_stats statistics;
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
	if (usbradioplus_native_worker_stats_read(o, &statistics)) {
		ast_cli(a->fd, "Native worker measurements are not available.\n");
		return CLI_FAILURE;
	}
	local_filter = &statistics.local_filter;
	final_filter = &statistics.final_filter;
	if (a->argc == 4) {
		if (strcasecmp(a->argv[3], "reset")) {
			return CLI_SHOWUSAGE;
		}
		usbradioplus_native_worker_stats_reset(o);
		ast_cli(a->fd, "Native worker meter reset requested.\n");
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
		"Native worker queue: input drops %" PRIu64 ", output silences %" PRIu64
		", malformed records %" PRIu64 "; RNNoise frames %" PRIu64 ", output %" PRIu64
		", startup %" PRIu64 ", errors %" PRIu64 ", VAD %.2f.\n",
		statistics.worker_input_overflows, statistics.worker_output_underflows,
		statistics.worker_output_malformed, statistics.rnnoise_frames,
		statistics.rnnoise_output_samples, statistics.rnnoise_startup_samples,
		statistics.rnnoise_errors, statistics.rnnoise_vad_probability);
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
		"FFmpeg final cleanup: pre-filter peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
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

/** Start the parallel-port pulse worker when an output is configured. */
URP_CHANNEL_LOCAL void usbradio_start_parallel_pulser(void)
{
	if (urp_parallel_pulser_needed(haspp, hasout))
		ast_pthread_create_background(&pulserid, NULL, pulserthread, NULL);
}

URP_CHANNEL_LOCAL int load_module(void)
{
	usbradio_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!usbradio_tech.capabilities) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_append(usbradio_tech.capabilities, ast_format_slin, 0);

	if (ast_radio_hid_device_mklist()) {
		ast_log(LOG_ERROR, "Unable to make hid list\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	usbradio_active = NULL;

	/* Copy the default jb config over global_jbconf */
	memcpy(&global_jbconf, &default_jbconf, sizeof(struct ast_jb_conf));

	pp_val = 0;
	hasout = 0;
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
	if (usbradioplus_advanced_register(&usbradio_tech, URP_RATE_NATIVE,
					   usbradioplus_configure_advanced)) {
		usbradioplus_processing_unload();
		ast_cli_unregister_multiple(cli_usbradio, ARRAY_LEN(cli_usbradio));
		ast_channel_unregister(&usbradio_tech);
		return AST_MODULE_LOAD_FAILURE;
	}
	usbradio_start_parallel_pulser();

	return AST_MODULE_LOAD_SUCCESS;
}

URP_CHANNEL_LOCAL int unload_module(void)
{
	struct chan_usbradio_pvt *o;

	stoppulser = 1;
	usbradioplus_advanced_unregister();
	usbradioplus_processing_unload();

	ast_channel_unregister(&usbradio_tech);
	ast_cli_unregister_multiple(cli_usbradio,
				    sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));

	/* Do not free native-worker state while an Asterisk channel can still enter
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
		usbradioplus_dsp_destroy(o);
		if (o->radio) {
			urp_radio_destroy(o->radio);
		}

		if (o->sounddev >= 0) {
			close(o->sounddev);
			o->sounddev = -1;
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
		.unload = unload_module, .reload = reload_module, .requires = "res_usbradio", );
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
/** @def URP_LEGACY_TEST_TONE_PEAK
 * @brief PCM peak of the calibrated 1 kHz transmitter test tone.
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
