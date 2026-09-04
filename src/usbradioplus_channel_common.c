#include "asterisk.h"
#include "asterisk/res_usbradio.h"

#include <errno.h>
#include <math.h>
#include <search.h>
#include <stdio.h>
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
		ast_log(LOG_ERROR, "Channel %s: Write failed: %s\n", o->name, strerror(errno));
	}
}

int load_tune_config(struct chan_usbradio_pvt *o, const struct ast_config *cfg, int reload)
{
	struct ast_variable *v;
	struct ast_config *cfg2;
	int opened = 0;
	int configured = 0;
	char devstr[sizeof(o->devstr)];
	char serial[sizeof(o->serial)];

	/* No load defaults */
	o->rxmixerset = 500;
	o->txmixaset = 500;
	o->txmixbset = 500;
	o->legacy_rxvoiceadj = 0.5;
	o->legacy_rxvoiceadj_configured = 0;
	o->rxctcssadj = 1.0;
	o->txctcssadj = 200;
	o->rxsquelchadj = 500;
	o->txslimsp = DEFAULT_TX_SOFT_LIMITER_SETPOINT;

	devstr[0] = '\0';
	serial[0] = '\0';
	if (!reload) {
		o->devstr[0] = 0;
		o->serial[0] = 0;
	}

	if (!cfg) {
		struct ast_flags zeroflag = {0};
		cfg2 = ast_config_load(CONFIG, zeroflag);
		if (!cfg2) {
			ast_log(LOG_WARNING,
				"Can't %sload settings for %s, using default parameters\n",
				reload ? "re" : "", o->name);
			return -1;
		}
		opened = 1;
		cfg = cfg2;
	}

	for (v = ast_variable_browse(cfg, o->name); v; v = v->next) {
		configured = 1;
		CV_START(v->name, v->value);
		CV_UINT("rxmixerset", o->rxmixerset);
		CV_UINT("txmixaset", o->txmixaset);
		CV_UINT("txmixbset", o->txmixbset);
		CV_F("rxvoiceadj", store_rxvoiceadj(o, v->value));
		CV_F("rxctcssadj", sscanf(v->value, N_FMT(f), &o->rxctcssadj));
		CV_UINT("txctcssadj", o->txctcssadj);
		CV_UINT("rxsquelchadj", o->rxsquelchadj);
		CV_UINT("txslimsp", o->txslimsp);
		CV_UINT("fever", o->fever);
		CV_STR("devstr", devstr);
		CV_STR("serial", serial);
		CV_END;
	}
	if (!reload) {
		/* Using the ternary operator in CV_STR won't work, due to butchering the sizeof, so
		 * copy after if needed */
		ast_copy_string(o->devstr, devstr, sizeof(o->devstr));
		ast_copy_string(o->serial, serial, sizeof(o->serial));
	}
	if (opened) {
		ast_config_destroy(cfg2);
	}
	if (!configured) {
		ast_log(LOG_WARNING,
			"Can't %sload settings for %s (no section available), using default "
			"parameters\n",
			reload ? "re" : "", o->name);
		return -1;
	}
	return 0;
}

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

void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples, size_t count)
{
	unsigned int seed_frames;
	ast_mutex_lock(&o->plus_link_lock);
	/* Lead a new or recovered burst with the target safety margin. PTT remains
	 * asserted while it drains, so even a one-frame telemetry burst is kept. */
	seed_frames = !o->plus_native_fifo.primed && !o->plus_native_fifo.count &&
				      !urp_program_queue_pending(&o->plus_program_queue)
			      ? PLUS_LINK_NATIVE_TARGET_SAMPLES / URP_NATIVE_SAMPLES - 1
			      : 0;
	if (urp_program_queue_push(&o->plus_program_queue, samples, count, o->plus_app_rpt_samples,
				   seed_frames)) {
		o->plus_link_queue_overflows++;
	}
	ast_mutex_unlock(&o->plus_link_lock);
}

int usbradioplus_program_pending(struct chan_usbradio_pvt *o)
{
	int pending;
	ast_mutex_lock(&o->plus_link_lock);
	pending = urp_program_queue_pending(&o->plus_program_queue);
	ast_mutex_unlock(&o->plus_link_lock);
	return pending || o->plus_native_fifo.count != 0;
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
		o->txkeyed = 1;
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
		o->txkeyed = 0;
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
	o->txtestkey = 1;
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
	o->txtestkey = 0;
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
		o->txtestkey = 1;
		o->plus_test_tone_enabled = 1;
		o->radio->txPttIn = 1;
		if ((fd > 0) && intflag) {
			if (ast_radio_wait_or_poll(fd, 1000, intflag)) {
				o->radio->txPttIn = 0;
				o->txtestkey = 0;
				o->plus_test_tone_enabled = 0;
				break;
			}
		} else {
			usleep(1000000);
		}
		o->plus_test_tone_enabled = 0;
		o->radio->txPttIn = 0;
		o->txtestkey = 0;
		if (i < (NFLASH - 1) && (fd > 0) && intflag) {
			if (ast_radio_wait_or_poll(fd, 1500, intflag)) {
				o->radio->txPttIn = 0;
				o->txtestkey = 0;
				break;
			}
		} else if (i < (NFLASH - 1)) {
			usleep(1500000);
		}
	}
	if (fd > 0) {
		ast_cli(fd, "Channel %s: USB Device Flash completed.\n", o->name);
	}
	o->radio->txPttIn = 0;
	o->txtestkey = 0;
	o->plus_test_tone_enabled = 0;
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
			set_txctcss_level(o);
			ast_cli(fd, "Changed Tx CTCSS modulation setting to %i\n", i);
		}
		o->txtestkey = 1;
		usleep(5000000);
		o->txtestkey = 0;
	} else if (!strcasecmp(argv[2], "nocap")) {
		ast_cli(fd, "File capture (trace) was rx=%d tx=%d and now off.\n", o->rxcap2,
			o->txcap2);
		ast_cli(fd, "File capture (raw)   was rx=%d tx=%d and now off.\n", o->rxcapraw,
			o->txcapraw);
		o->rxcapraw = o->txcapraw = o->rxcap2 = o->txcap2 = o->radio->b.rxCapture =
			o->radio->b.txCapture = 0;
		if (frxcapraw) {
			fclose(frxcapraw);
			frxcapraw = NULL;
		}
		if (frxcaptrace) {
			fclose(frxcaptrace);
			frxcaptrace = NULL;
		}
		if (frxoutraw) {
			fclose(frxoutraw);
			frxoutraw = NULL;
		}
		if (ftxcapraw) {
			fclose(ftxcapraw);
			ftxcapraw = NULL;
		}
		if (ftxcaptrace) {
			fclose(ftxcaptrace);
			ftxcaptrace = NULL;
		}
		if (ftxoutraw) {
			fclose(ftxoutraw);
			ftxoutraw = NULL;
		}
	} else if (!strcasecmp(argv[2], "rxtracecap")) {
		if (!frxcaptrace) {
			frxcaptrace = fopen(RX_CAP_TRACE_FILE, "w");
		}
		ast_cli(fd, "Trace rx on.\n");
		o->rxcap2 = o->radio->b.rxCapture = 1;
	} else if (!strcasecmp(argv[2], "txtracecap")) {
		if (!ftxcaptrace) {
			ftxcaptrace = fopen(TX_CAP_TRACE_FILE, "w");
		}
		ast_cli(fd, "Trace tx on.\n");
		o->txcap2 = o->radio->b.txCapture = 1;
	} else if (!strcasecmp(argv[2], "rxcap")) {
		if (!frxcapraw) {
			frxcapraw = fopen(RX_CAP_RAW_FILE, "w");
		}
		ast_cli(fd, "cap rx raw on.\n");
		o->rxcapraw = 1;
	} else if (!strcasecmp(argv[2], "txcap")) {
		if (!ftxcapraw) {
			ftxcapraw = fopen(TX_CAP_RAW_FILE, "w");
		}
		ast_cli(fd, "cap tx raw on.\n");
		o->txcapraw = 1;
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
	} else if (!strcasecmp(argv[2], "txslimsp")) {
		if (argc == 3) {
			ast_cli(fd, "Current tx limiter setpoint: %i\n", (int)o->txslimsp);
		} else {
			int new_slsetpoint = atoi(argv[3]);
			if (legacy_set_tx_soft_limiter(o, new_slsetpoint)) {
				ast_cli(fd, "Limiter set point out of range, needs to be between "
					    "5000 and 13000\n");
				return RESULT_SHOWUSAGE;
			}
			o->txslimsp = new_slsetpoint;
		}
	} else {
		o->radio->b.tuning = 0;
		return RESULT_SHOWUSAGE;
	}
	o->radio->b.tuning = 0;
	return RESULT_SUCCESS;
}

int set_txctcss_level(struct chan_usbradio_pvt *o)
{
	if (o->txmixa == TX_OUT_LSD) {
		o->txmixaset = o->txctcssadj;
		mixer_write(o);
		mult_set(o);
	} else if (o->txmixb == TX_OUT_LSD) {
		o->txmixbset = o->txctcssadj;
		mixer_write(o);
		mult_set(o);
	} else {
		if (o->radio->ptxCtcssAdjust) { /* Ignore if ptr not defined */
			*o->radio->ptxCtcssAdjust = (o->txctcssadj * M_Q8) / AUDIO_ADJUSTMENT;
		}
	}
	return 0;
}

int legacy_set_tx_soft_limiter(struct chan_usbradio_pvt *o, int setpoint)
{
	(void)o;
	return setpoint < 5000 || setpoint > 13000 ? -1 : 0;
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

void store_txmixa(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_tx_output_mode mode;
	if (urp_parse_tx_output_mode(s, &mode))
		ast_log(LOG_WARNING, "Unrecognized txmixa parameter: %s\n", s);
	else
		o->txmixa = (enum radio_tx_mix)mode;
}

void store_txmixb(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_tx_output_mode mode;
	if (urp_parse_tx_output_mode(s, &mode))
		ast_log(LOG_WARNING, "Unrecognized txmixb parameter: %s\n", s);
	else
		o->txmixb = (enum radio_tx_mix)mode;
}

void store_rxcdtype(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_carrier_source source;
	if (urp_parse_carrier_source(s, &source))
		ast_log(LOG_WARNING, "Unrecognized rxcdtype parameter: %s\n", s);
	else
		o->rxcdtype = (enum radio_carrier_detect)source;
}

void store_rxsdtype(struct chan_usbradio_pvt *o, const char *s)
{
	enum urp_ctcss_source source;
	if (urp_parse_ctcss_source(s, &source))
		ast_log(LOG_WARNING, "Unrecognized rxsdtype parameter: %s\n", s);
	else
		o->rxsdtype = (enum radio_squelch_detect)source;
}

void store_rxvoiceadj(struct chan_usbradio_pvt *o, const char *s)
{
	float f;
	sscanf(s, N_FMT(f), &f);
	o->legacy_rxvoiceadj = f;
	o->legacy_rxvoiceadj_configured = 1;
}

double effective_rx_input_gain_db(const struct chan_usbradio_pvt *o)
{
	struct txagc_chain chain;
	usbradioplus_processing_get_local(&chain);
	if (chain.input_gain_configured)
		return chain.agc.input_gain_db;
	return 20.0 * log10(fmax(0.000001, 2.0 * o->legacy_rxvoiceadj));
}

float effective_legacy_rxvoiceadj(const struct chan_usbradio_pvt *o)
{
	return (float)(pow(10.0, effective_rx_input_gain_db(o) / 20.0) / 2.0);
}

int effective_rxmixerset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware(&hardware);
	return hardware.input_gain_configured ? urp_gain_db_to_mixer(hardware.input_gain_db)
					      : o->rxmixerset;
}

int effective_txmixaset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware(&hardware);
	return hardware.output_a_gain_configured ? urp_gain_db_to_mixer(hardware.output_a_gain_db)
						 : o->txmixaset;
}

int effective_txmixbset(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware(&hardware);
	return hardware.output_b_gain_configured ? urp_gain_db_to_mixer(hardware.output_b_gain_db)
						 : o->txmixbset;
}

enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware(&hardware);
	return hardware.output_a_assignment_configured
		       ? (enum radio_tx_mix)hardware.output_a_assignment
		       : o->txmixa;
}

enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	usbradioplus_processing_get_hardware(&hardware);
	return hardware.output_b_assignment_configured
		       ? (enum radio_tx_mix)hardware.output_b_assignment
		       : o->txmixb;
}

enum radio_carrier_detect effective_rxcdtype(const struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	const char *value;
	usbradioplus_processing_get_hardware(&hardware);
	if (!hardware.cos_assignment_configured)
		return o->rxcdtype;
	value = hardware.cos_assignment;
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

void refresh_processing_hardware(struct chan_usbradio_pvt *o)
{
	struct usbradioplus_hardware_settings hardware;
	const char *rx_frequencies = o->rxctcssfreqs;
	const char *tx_frequencies = o->txctcssfreqs;
	int rx = effective_rxmixerset(o), a = effective_txmixaset(o), b = effective_txmixbset(o);
	int route_a = effective_txmixa(o), route_b = effective_txmixb(o);
	usbradioplus_processing_get_hardware(&hardware);
	if (hardware.rx_ctcss_frequencies_configured)
		rx_frequencies = hardware.rx_ctcss_frequencies;
	if (hardware.tx_ctcss_frequencies_configured)
		tx_frequencies = hardware.tx_ctcss_frequencies;
	o->radio->rxCdType = effective_rxcdtype(o);
	if (!o->remoted && (strcmp(rx_frequencies, o->plus_applied_rxctcssfreqs) ||
			    strcmp(tx_frequencies, o->plus_applied_txctcssfreqs))) {
		ast_copy_string(o->plus_applied_rxctcssfreqs, rx_frequencies,
				sizeof(o->plus_applied_rxctcssfreqs));
		ast_copy_string(o->plus_applied_txctcssfreqs, tx_frequencies,
				sizeof(o->plus_applied_txctcssfreqs));
		o->radio->pRxCodeSrc = o->plus_applied_rxctcssfreqs;
		o->radio->pTxCodeSrc = o->plus_applied_txctcssfreqs;
		urp_radio_parse_codes(o->radio);
	}
	if (o->plus_hardware_applied && rx == o->plus_applied_rxmixer &&
	    a == o->plus_applied_txmixaset && b == o->plus_applied_txmixbset &&
	    route_a == o->plus_applied_txmixa && route_b == o->plus_applied_txmixb)
		return;
	o->plus_applied_rxmixer = rx;
	o->plus_applied_txmixaset = a;
	o->plus_applied_txmixbset = b;
	o->plus_applied_txmixa = route_a;
	o->plus_applied_txmixb = route_b;
	o->plus_hardware_applied = 1;
	mixer_write(o);
	mult_set(o);
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
	o->txtestkey = 1;
	o->plus_test_tone_enabled = 1;
	o->radio->txPttIn = 1;
	if (fd > 0) {
		ast_cli(fd, "Tone output starting on channel %s...\n", o->name);
		if (ast_radio_wait_or_poll(fd, 5000, intflag)) {
			o->radio->txPttIn = 0;
			o->txtestkey = 0;
			o->plus_test_tone_enabled = 0;
		}
	} else
		usleep(5000000);
	o->plus_test_tone_enabled = 0;
	if (fd > 0) {
		ast_cli(fd, "Tone output ending on channel %s...\n", o->name);
	}
	o->radio->txPttIn = 0;
	o->txtestkey = 0;
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
			(o->txkeyed || o->txtestkey) ? "Keyed" : "Clear");
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
		set_txctcss_level(o);
		ast_cli(fd, "Changed Tx CTCSS Modulation Level setting to %i\n", i);
	}
	if (dokey) {
		ast_cli(fd, "Keying Radio and sending CTCSS tone for 5 seconds...\n");
		o->txtestkey = 1;
		ast_radio_wait_or_poll(fd, 5000, 1);
		o->txtestkey = 0;
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
		usbradioplus_processing_set_local_input_gain(20.0 *
							     log10(fmax(0.000001, 2.0 * setting)));
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
		urp_legacy_multiplier((effective_txmixaset(o) * 152) / AUDIO_ADJUSTMENT);
	/* Preserve the legacy rule: matching routes use channel A gain. */
	o->radio->txOutputGainB =
		effective_txmixa(o) == effective_txmixb(o)
			? o->radio->txOutputGainA
			: urp_legacy_multiplier((effective_txmixbset(o) * 152) / AUDIO_ADJUSTMENT);
}

void usbradioplus_program_radio(struct chan_usbradio_pvt *o)
{
	struct urp_parallel_bus bus;
	uint32_t rx_freq = o->remoted ? o->set_rxfreq : o->rxfreq;
	uint32_t tx_freq = o->remoted ? o->set_txfreq : o->txfreq;
	int high_power = o->remoted ? o->set_txpower : 0;

	if (!haspp)
		return;
	ast_mutex_lock(&pp_lock);
	bus.value = (uint8_t)pp_val;
	bus.write = usbradioplus_parallel_program_write;
	bus.opaque = NULL;
	urp_hardware_program_radio(&bus, rx_freq, tx_freq, o->radio && o->radio->txPttOut,
				   high_power);
	pp_val = (int8_t)bus.value;
	ast_mutex_unlock(&pp_lock);
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

int radio_config(struct chan_usbradio_pvt *o)
{
	if (o->radio == NULL) {
		ast_log(LOG_ERROR, "native radio state is unavailable\n");
		return 1;
	}

	o->radio->rxCtcss->relax = o->rxctcssrelax;
	if (o->remoted) {
		o->radio->pTxCodeDefault = o->set_txctcssdefault;
		o->radio->pRxCodeSrc = o->set_rxctcssfreqs;
		o->radio->pTxCodeSrc = o->set_txctcssfreqs;

	} else {
		/* Select the configured signaling-code strings. */
		struct usbradioplus_hardware_settings hardware;

		o->radio->pTxCodeDefault = o->txctcssdefault;
		usbradioplus_processing_get_hardware(&hardware);
		if (hardware.rx_ctcss_frequencies_configured) {
			ast_copy_string(o->plus_applied_rxctcssfreqs, hardware.rx_ctcss_frequencies,
					sizeof(o->plus_applied_rxctcssfreqs));
			o->radio->pRxCodeSrc = o->plus_applied_rxctcssfreqs;
		} else
			o->radio->pRxCodeSrc = o->rxctcssfreqs;
		if (hardware.tx_ctcss_frequencies_configured) {
			ast_copy_string(o->plus_applied_txctcssfreqs, hardware.tx_ctcss_frequencies,
					sizeof(o->plus_applied_txctcssfreqs));
			o->radio->pTxCodeSrc = o->plus_applied_txctcssfreqs;
		} else
			o->radio->pTxCodeSrc = o->txctcssfreqs;
	}

	if (o->forcetxcode) {
		o->radio->pTxCodeDefault = o->set_txctcssfreq;
		ast_debug(3, "Channel %s: Forced Tx Squelch Code code=%s.\n", o->name,
			  o->radio->pTxCodeDefault);
	}

	urp_radio_parse_codes(o->radio);
	usbradioplus_program_radio(o);

	return 0;
}

int store_cutoff(struct chan_usbradio_pvt *o, const char *name, const char *text)
{
	int *legacy = NULL, *enabled = NULL, *exact = NULL;
	double *frequency = NULL;
	double defaults = 0.0;
	struct urp_cutoff_setting setting;

#define SELECT_CUTOFF(field, hz)                                                                   \
	do {                                                                                       \
		legacy = &o->field;                                                                \
		enabled = &o->plus_##field##_enabled;                                              \
		exact = &o->plus_##field##_exact;                                                  \
		frequency = &o->plus_##field##_hz;                                                 \
		defaults = (hz);                                                                   \
	} while (0)
	if (!strcasecmp(name, "rxlpf"))
		SELECT_CUTOFF(rxlpf, 3000.0);
	else if (!strcasecmp(name, "rxhpf"))
		SELECT_CUTOFF(rxhpf, 300.0);
	else if (!strcasecmp(name, "txlpf"))
		SELECT_CUTOFF(txlpf, 3000.0);
	else if (!strcasecmp(name, "txhpf"))
		SELECT_CUTOFF(txhpf, 300.0);
	else
		return 0;
#undef SELECT_CUTOFF

	if (urp_parse_cutoff(text, defaults, URP_RATE_NATIVE / 2.0, &setting))
		return -1;
	*legacy = setting.selector;
	*enabled = setting.enabled;
	*exact = setting.exact;
	*frequency = setting.frequency_hz;
	return 1;
}

int apply_processing_config_overrides(struct chan_usbradio_pvt *o, const char *category)
{
	char value[512];
	char option[32];
	char *end;
	int i;
	long number;
	size_t option_index;
	static const int parallel_pins[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15};
	static const char *const jitter_options[] = {
		"jbenable", "jbmaxsize", "jbresyncthreshold", "jbimpl",
		"jblog",    "jbforce",	 "jbtargetextra",     "jbsyncvideo",
	};

#define GET(section, name)                                                                         \
	(!usbradioplus_processing_get_option((section), (name), value, sizeof(value)))
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

	STRING("hardware", "devstr", devstr);
	STRING("hardware", "serial", serial);
	INTEGER("hardware", "hdwtype", hdwtype);
	BOOLEAN("hardware", "eeprom", wanteeprom);
	INTEGER("hardware", "frags", frags);
	INTEGER("hardware", "queuesize", queuesize);
	BOOLEAN("hardware", "rxcpusaver", rxcpusaver);
	BOOLEAN("hardware", "txcpusaver", txcpusaver);
	if (GET("hardware", "rxdemod"))
		store_rxdemod(o, value);
	if (GET("hardware", "ctcssfrom"))
		store_rxsdtype(o, value);
	INTEGER("hardware", "voxhangtime", voxhangtime);
	INTEGER("hardware", "rxsqvox", rxsqvoxadj);
	INTEGER("hardware", "rxsqhyst", rxsqhyst);
	INTEGER("hardware", "rxnoisefiltype", rxnoisefiltype);
	INTEGER("hardware", "rxsquelchdelay", rxsquelchdelay);
	INTEGER("hardware", "rxondelay", rxondelay);
	if (o->rxondelay > MS_TO_FRAMES(RX_ON_DELAY_MAX))
		o->rxondelay = MS_TO_FRAMES(RX_ON_DELAY_MAX);
	BOOLEAN("hardware", "rxpolarity", rxpolarity);
	INTEGER("hardware", "rxsquelchadj", rxsquelchadj);
	if (GET("hardware", "rxctcssadj")) {
		double adjustment = strtod(value, &end);
		if (end == value || *end || !isfinite(adjustment))
			goto invalid;
		o->rxctcssadj = adjustment;
	}
	BOOLEAN("hardware", "rxctcssoverride", rxctcssoverride);
	INTEGER("hardware", "rxctcssrelax", rxctcssrelax);
	STRING("hardware", "txctcssdefault", txctcssdefault);
	INTEGER("hardware", "txctcssadj", txctcssadj);
	if (GET("hardware", "txtoctype"))
		store_txtoctype(o, value);
	BOOLEAN("hardware", "dcsrxpolarity", dcsrxpolarity);
	BOOLEAN("hardware", "dcstxpolarity", dcstxpolarity);
	BOOLEAN("hardware", "lsdrxpolarity", lsdrxpolarity);
	BOOLEAN("hardware", "lsdtxpolarity", lsdtxpolarity);
	BOOLEAN("hardware", "txprelim", txprelim);
	BOOLEAN("hardware", "txlimonly", txlimonly);
	INTEGER("hardware", "txslimsp", txslimsp);
	INTEGER("hardware", "txsettletime", txsettletime);
	INTEGER("hardware", "txrxblankingtime", txrxblankingtime);
	INTEGER("hardware", "txoffdelay", txoffdelay);
	if (o->txoffdelay > MS_TO_FRAMES(TX_OFF_DELAY_MAX))
		o->txoffdelay = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
	BOOLEAN("hardware", "txpolarity", txpolarity);
	BOOLEAN("hardware", "invertptt", invertptt);
	INTEGER("hardware", "rxfreq", rxfreq);
	INTEGER("hardware", "txfreq", txfreq);
	INTEGER("hardware", "rptnum", rptnum);
	INTEGER("hardware", "area", area);
	STRING("hardware", "ukey", ukey);
	INTEGER("hardware", "idleinterval", idleinterval);
	INTEGER("hardware", "turnoffs", turnoffs);
	INTEGER("hardware", "sendvoter", sendvoter);
	INTEGER("hardware", "clipledgpio", clipledgpio);
	for (i = 0; i < GPIO_PINCOUNT; ++i) {
		snprintf(option, sizeof(option), "gpio%d", i + 1);
		if (GET("hardware", option)) {
			ast_free(o->gpios[i]);
			o->gpios[i] = ast_strdup(value);
			if (!o->gpios[i])
				goto invalid;
		}
	}
	for (option_index = 0; option_index < ARRAY_LEN(parallel_pins); ++option_index) {
		int pin = parallel_pins[option_index];
		snprintf(option, sizeof(option), "pp%d", pin);
		if (GET("hardware", option)) {
			ast_free(o->pps[pin]);
			o->pps[pin] = ast_strdup(value);
			if (!o->pps[pin])
				goto invalid;
			haspp = 1;
		}
	}
	INTEGER("duplex", "duplex", radioduplex);
	INTEGER("duplex", "duplex3", duplex3);
	if (GET("duplex", "duplex3mode")) {
		if (!strcasecmp(value, "hardware"))
			o->duplex3mode = DUPLEX3_MODE_HARDWARE;
		else if (!strcasecmp(value, "software"))
			o->duplex3mode = DUPLEX3_MODE_SOFTWARE;
		else
			goto invalid;
	}
	if (GET("hardware", "emphasis_corner_hz")) {
		double frequency = strtod(value, &end);
		if (end == value || *end || !isfinite(frequency))
			goto invalid;
		o->plus_emphasis_corner_hz = frequency;
	}
	BOOLEAN("general", "radioactive", radioactive);
	INTEGER("diagnostics", "tracetype", tracetype);
	INTEGER("diagnostics", "tracelevel", tracelevel);
	INTEGER("diagnostics", "fever", fever);
	for (option_index = 0; option_index < ARRAY_LEN(jitter_options); ++option_index)
		if (GET("asterisk", jitter_options[option_index]) &&
		    ast_jb_read_conf(&global_jbconf, jitter_options[option_index], value))
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

int usbradioplus_dsp_init(struct chan_usbradio_pvt *o)
{
	o->plus_up = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	o->plus_down = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	if (!o->plus_up || !o->plus_down) {
		ast_log(LOG_ERROR, "RadioPlus/%s: unable to create native sample-rate converters\n",
			o->name);
		return -1;
	}
	txagc_avfilter_init(&o->plus_local_avfilter);
	txagc_avfilter_init(&o->plus_rx_filter);
	txagc_avfilter_init(&o->plus_rx_filter_after);
	txagc_avfilter_init(&o->plus_final_avfilter);
	txagc_rnnoise_init(&o->plus_local_rnnoise);
	o->plus_adc_peak_dbfs = o->plus_adc_max_peak_dbfs = -INFINITY;
	o->plus_deemphasis_peak_dbfs = o->plus_deemphasis_max_peak_dbfs = -INFINITY;
	o->plus_preemphasis_input_peak_dbfs = o->plus_preemphasis_input_max_peak_dbfs = -INFINITY;
	o->plus_tx_program_peak_dbfs = -INFINITY;
	o->plus_tx_program_max_peak_dbfs = -INFINITY;
	o->plus_local_tx_peak_dbfs = -INFINITY;
	o->plus_local_tx_max_peak_dbfs = -INFINITY;
	return 0;
}

void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o)
{
	urp_src_destroy(o->plus_up);
	urp_src_destroy(o->plus_down);
	o->plus_up = o->plus_down = NULL;
	txagc_rnnoise_destroy(&o->plus_local_rnnoise);
	txagc_avfilter_destroy(&o->plus_local_avfilter);
	txagc_avfilter_destroy(&o->plus_rx_filter);
	txagc_avfilter_destroy(&o->plus_rx_filter_after);
	txagc_avfilter_destroy(&o->plus_final_avfilter);
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
	return !o->radio->b.ctcssRxEnable ||
	       (o->radio->rxCtcss->decode > CTCSS_NULL && o->radio->smode == SMODE_CTCSS);
}

void usbradioplus_refresh_ctcss_decode(struct chan_usbradio_pvt *o)
{
	if (!o->radio->b.ctcssRxEnable || o->radio->rxCtcss->decode == o->rxctcssdecode)
		return;
	ast_debug(3, "Channel %s: rxctcssdecode = %i.\n", o->name, o->radio->rxCtcss->decode);
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

void plus_link_native_push(struct chan_usbradio_pvt *o, const short *samples, size_t count)
{
	o->plus_link_queue_overflows += urp_native_fifo_push(&o->plus_native_fifo, samples, count);
}

int plus_link_native_pop(struct chan_usbradio_pvt *o, short *samples)
{
	return urp_native_fifo_pop(&o->plus_native_fifo, samples);
}

int usbradioplus_ensure_parrot_capacity(struct chan_usbradio_pvt *o)
{
	size_t capacity = (size_t)DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES;
	double *buffer;

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
	if (urp_parrot_rx_transition(&o->plus_parrot_state, was_keyed, o->rxkeyed)) {
		o->echoing = 1;
		ast_log(LOG_NOTICE, "RadioPlus/%s: replaying %.2f seconds of native echo audio%s\n",
			o->name, (double)o->plus_parrot_count / URP_RATE_NATIVE,
			o->plus_parrot_truncated ? " (truncated)" : "");
	}
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
	do {
		store_config(cfg, ctg);
	} while ((ctg = ast_category_browse(cfg, ctg)) != NULL);

	/* load parallel port information */
	ppfd = -1;
	pbase = 0;
	if (!usbradioplus_processing_get_option("hardware", "pport", processing_value,
						sizeof(processing_value)))
		val = processing_value;
	else
		val = ast_variable_retrieve(cfg, "general", "pport");
	if (val) {
		ast_copy_string(pport, val, sizeof(pport));
	} else {
		ast_copy_string(pport, PP_PORT, sizeof(pport));
	}
	if (!usbradioplus_processing_get_option("hardware", "pbase", processing_value,
						sizeof(processing_value)))
		val = processing_value;
	else
		val = ast_variable_retrieve(cfg, "general", "pbase");
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
	return result;
}
