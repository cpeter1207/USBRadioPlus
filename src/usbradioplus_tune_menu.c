#include "asterisk.h"

#include <stdlib.h>
#include <search.h>
#include <string.h>

#include "asterisk/cli.h"
#include "asterisk/frame.h"
#include "asterisk/options.h"
#include "asterisk/res_usbradio.h"

#include "txagc/avfilter_processor.h"
#include "txagc/rnnoise_processor.h"
#include "usbradioplus_channel_core.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_processing.h"
#include "usbradioplus_radio.h"
#include "usbradioplus_repeat.h"
#include "usbradioplus_channel_private.h"

void tune_menusupport(int fd, struct chan_usbradio_pvt *o, const char *cmd)
{
	int x, oldverbose, flatrx, txhasctcss;
	int micmax, spkrmax, micplaymax;
	struct chan_usbradio_pvt *oy = NULL;

	oldverbose = option_verbose;
	option_verbose = 0;
	flatrx = 0;
	if (o->rxdemod == RX_AUDIO_FLAT) {
		flatrx = 1;
	}
	txhasctcss = 0;
	if ((o->txmixa == TX_OUT_LSD) || (o->txmixa == TX_OUT_COMPOSITE) ||
	    (o->txmixb == TX_OUT_LSD) || (o->txmixb == TX_OUT_COMPOSITE)) {
		txhasctcss = 1;
	}
	switch (cmd[0]) {
	case '0': /* return audio processing configuration */
		usbradioplus_tune_mixer_limits(o, &micmax, &spkrmax, &micplaymax);
		/* note: to maintain backward compatibility for those expecting a specific # of
		   values to be returned (and in a specific order).  So, we only add to the end
		   of the returned list.  Also, once an update has been released we can't change
		   the format/content of any previously returned string */
		if (!strcmp(cmd, "0+10")) { /* With o->txslimsp tx soft limiter set point */
			ast_cli(fd,
				"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,"
				"%d,%d\n",
				flatrx, txhasctcss, o->echomode, 0, 0, o->rxcdtype, o->rxsdtype,
				o->rxondelay, o->txoffdelay, o->txprelim, o->txlimonly, o->rxdemod,
				o->txmixa, o->txmixb, effective_rxmixerset(o),
				effective_legacy_rxvoiceadj(o), o->rxsquelchadj, o->txmixaset,
				o->txmixbset, o->txctcssadj, micplaymax, spkrmax, micmax,
				o->txslimsp);
		} else if (!strcmp(cmd, "0+9")) {
			ast_cli(fd,
				"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,"
				"%d\n",
				flatrx, txhasctcss, o->echomode, 0, 0, o->rxcdtype, o->rxsdtype,
				o->rxondelay, o->txoffdelay, o->txprelim, o->txlimonly, o->rxdemod,
				o->txmixa, o->txmixb, effective_rxmixerset(o),
				effective_legacy_rxvoiceadj(o), o->rxsquelchadj, o->txmixaset,
				o->txmixbset, o->txctcssadj, micplaymax, spkrmax, micmax);
		} else {
			ast_cli(fd, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", flatrx,
				txhasctcss, o->echomode, 0, 0, o->rxcdtype, o->rxsdtype,
				o->rxondelay, o->txoffdelay, o->txprelim, o->txlimonly, o->rxdemod,
				o->txmixa, o->txmixb);
		}
		break;
	case '1': /* return usb device name list */
		for (x = 0, oy = usbradioplus_channel_first(); oy && oy->name; oy = oy->next, x++) {
			if (x) {
				ast_cli(fd, ",");
			}
			ast_cli(fd, "%s", oy->name);
		}
		ast_cli(fd, "\n");
		break;
	case '2': /* print parameters */
		_menu_print(fd, o);
		break;
	case '3': /* return usb device name list except current */
		for (x = 0, oy = usbradioplus_channel_first(); oy && oy->name; oy = oy->next) {
			if (!strcmp(oy->name, o->name)) {
				continue;
			}
			if (x) {
				ast_cli(fd, ",");
			}
			ast_cli(fd, "%s", oy->name);
			x++;
		}
		ast_cli(fd, "\n");
		break;
	case 'a': /* receive tune */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_rxinput(fd, o, 1, 1);
		break;
	case 'b': /* receive tune display */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_rxdisplay(fd, o);
		break;
	case 'c': /* set receive voice level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		_menu_rxvoice(fd, o, cmd + 1);
		break;
	case 'd': /* set receive ctcss level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_rxctcss(fd, o, 1);
		break;
	case 'e': /* set squelch level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		_menu_rxsquelch(fd, o, cmd + 1);
		break;
	case 'f': /* set voice transmit level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		_menu_txvoice(fd, o, cmd + 1);
		break;
	case 'g': /* set aux transmit level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		_menu_auxvoice(fd, o, cmd + 1);
		break;
	case 'h': /* transmit a test tone */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		_menu_txtone(fd, o, cmd + 1);
		break;
	case 'i': /* tune receive level */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_rxvoice(fd, o, 1);
		break;
	case 'j': /* save tune settings */
		tune_write(o);
		ast_cli(fd, "Saved radio tuning settings.\n");
		break;
	case 'k': /* change echo mode */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				if (usbradioplus_native_echo(o) &&
				    usbradioplus_ensure_parrot_capacity(o)) {
					ast_cli(fd, "Unable to allocate native echo buffer\n");
					break;
				}
				o->echomode = 1;
			} else {
				o->echomode = 0;
				o->plus_parrot_playing = 0;
				o->echoing = 0;
				o->plus_parrot_count = o->plus_parrot_play = 0;
			}
			ast_cli(fd, "Echo Mode changed to %s\n",
				(o->echomode) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "Echo Mode is currently %s\n",
				(o->echomode) ? "Enabled" : "Disabled");
		}
		break;
	case 'l': /* transmit test tone */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_flash(fd, o, 1);
		break;

	case 'L': /* Set TX soft limiter when operating with preemphasized and limited tx audio */
		if (cmd[1]) {
			int setpoint = atoi(cmd + 1);
			if (legacy_set_tx_soft_limiter(o, setpoint)) {
				ast_cli(fd, "TX soft limiting setpoint must be between 5000 and "
					    "13000\n");
				break;
			} else {
				o->txslimsp = setpoint;
			}

			ast_cli(fd, "TX soft limiting setpoint changed to %i\n", setpoint);
		} else {
			ast_cli(fd, "TX soft limiting setpoint currently set to: %i\n",
				o->txslimsp);
		}
		break;
	case 'D': /* Set local repeat level for duplex=3 operation. */
		if (cmd[1]) {
			char *end = NULL;
			long level = strtol(cmd + 1, &end, 10);
			if (*end || level < 0 || level > DUPLEX3_LEVEL_MAX) {
				ast_cli(fd, "Duplex 3 level must be between 0 and %d\n",
					DUPLEX3_LEVEL_MAX);
				break;
			}
			o->duplex3 = (int)level;
			mixer_write(o);
			ast_cli(fd, "Duplex 3 level changed to %ld\n", level);
		} else {
			ast_cli(fd, "Duplex 3 level currently set to: %d\n", o->duplex3);
		}
		break;
	case 'M': /* Select hardware-mixer or native software local repeat. */
		if (cmd[1]) {
			if (cmd[1] != '0' && cmd[1] != '1') {
				ast_cli(fd, "Duplex 3 mode must be hardware or software\n");
				break;
			}
			o->duplex3mode =
				cmd[1] == '1' ? DUPLEX3_MODE_SOFTWARE : DUPLEX3_MODE_HARDWARE;
			mixer_write(o);
			ast_cli(fd, "Duplex 3 mode changed to %s\n",
				o->duplex3mode == DUPLEX3_MODE_SOFTWARE ? "software" : "hardware");
		} else {
			ast_cli(fd, "Duplex 3 mode currently set to: %s\n",
				o->duplex3mode == DUPLEX3_MODE_SOFTWARE ? "software" : "hardware");
		}
		break;

	case 'o': /* change carrier from */
		if (cmd[1]) {
			o->rxcdtype = atoi(&cmd[1]);
			ast_cli(fd, "Carrier From changed to %s\n", cd_signal_type[o->rxcdtype]);
		} else {
			ast_cli(fd, "Carrier From is currently %s\n", cd_signal_type[o->rxcdtype]);
		}
		break;
	case 'p': /* change ctcss from */
		if (cmd[1]) {
			o->rxsdtype = atoi(&cmd[1]);
			ast_cli(fd, "CTCSS From changed to %s\n", sd_signal_type[o->rxsdtype]);
		} else {
			ast_cli(fd, "CTCSS From is currently %s\n", sd_signal_type[o->rxsdtype]);
		}
		break;
	case 'q': /* change rx on delay */
		if (cmd[1]) {
			o->rxondelay = atoi(&cmd[1]);
			if (o->rxondelay > MS_TO_FRAMES(RX_ON_DELAY_MAX)) {
				o->rxondelay = MS_TO_FRAMES(RX_ON_DELAY_MAX);
			}
			ast_cli(fd, "RX On Delay From changed to %d\n", o->rxondelay);
		} else {
			ast_cli(fd, "RX On Delay is currently %d\n", o->rxondelay);
		}
		break;
	case 'r': /* change tx off delay */
		if (cmd[1]) {
			o->txoffdelay = atoi(&cmd[1]);
			if (o->txoffdelay > MS_TO_FRAMES(TX_OFF_DELAY_MAX)) {
				o->txoffdelay = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
			}
			ast_cli(fd, "TX Off Delay From changed to %d\n", o->txoffdelay);
		} else {
			ast_cli(fd, "TX Off Delay is currently %d\n", o->txoffdelay);
		}
		break;
	case 's': /* change txprelim */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->txprelim = 1;
			} else {
				o->txprelim = 0;
			}
			ast_cli(fd, "TxPrelim changed to %s\n",
				(o->txprelim) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "TxPrelim is currently %s\n",
				(o->txprelim) ? "Enabled" : "Disabled");
		}
		break;
	case 't': /* change txlimonly */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->txlimonly = 1;
			} else {
				o->txlimonly = 0;
			}
			ast_cli(fd, "TxLimonly changed to %s\n",
				(o->txlimonly) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "TxLimonly is currently %s\n",
				(o->txlimonly) ? "Enabled" : "Disabled");
		}
		break;
	case 'u': /* change rxdemod */
		if (cmd[1]) {
			o->rxdemod = atoi(&cmd[1]);
			ast_cli(fd, "RX Demodulation changed to %d\n", o->rxdemod);
		} else {
			ast_cli(fd, "RX Demodulation is currently %d\n", o->rxdemod);
		}
		break;
	case 'v': /* receiver/transmitter status display */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		tune_rxtx_status(fd, o);
		break;
	case 'w': /* change txmixa */
		if (cmd[1]) {
			o->txmixa = atoi(&cmd[1]);
			ast_cli(fd, "TX Mixer A changed to %d\n", o->txmixa);
		} else {
			ast_cli(fd, "TX Mixer A is currently %d\n", o->txmixa);
		}
		break;
	case 'x': /* change txmixb */
		if (cmd[1]) {
			o->txmixb = atoi(&cmd[1]);
			ast_cli(fd, "TX Mixer B changed to %d\n", o->txmixb);
		} else {
			ast_cli(fd, "TX Mixer B is currently %d\n", o->txmixb);
		}
		break;
	case 'y': /* display receive audio statistics (interactive) */
	case 'Y': /* display receive audio statistics (once only) */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		for (;;) {
			ast_radio_print_audio_stats(fd, &o->rxaudiostats, "Rx");
			if (cmd[0] == 'Y') {
				break;
			}
			if (ast_radio_poll_input(fd, 1000)) {
				break;
			}
		}
		break;
	case 'z': /* display transmit audio statistics (interactive) */
	case 'Z': /* display transmit audio statistics (once only) */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		x = 1;
		for (;;) {
			if (o->txkeyed || o->txtestkey) {
				ast_radio_print_audio_stats(fd, &o->txaudiostats, "Tx");
				x = 1;
			} else if (x == 1) {
				ast_cli(fd, "Tx not keyed\n");
				x = 0;
			}
			if (cmd[0] == 'Z') {
				break;
			}
			if (ast_radio_poll_input(fd, 1000)) {
				break;
			}
		}
		break;
	case 'A': /* combined signaling status and audio statistics */
		if (!o->hasusb) {
			ast_cli(fd, USB_UNASSIGNED_FMT, o->name, o->devstr);
			break;
		}
		ast_cli(fd, "COS, CTCSS, PTT, and audio levels. Press Enter to return.\n");
		for (;;) {
			ast_cli(fd, "COS input: %s; CTCSS input: %s; COS output: %s; PTT: %s\n",
				o->rxcdtype ? (o->rx_cos_active ? "keyed" : "clear") : "off",
				o->rxsdtype ? (o->rx_ctcss_active ? "keyed" : "clear") : "off",
				o->rxkeyed ? "keyed" : "clear",
				(o->txkeyed || o->txtestkey) ? "keyed" : "clear");
			ast_radio_print_audio_stats(fd, &o->rxaudiostats, "Rx");
			if (o->txkeyed || o->txtestkey) {
				ast_radio_print_audio_stats(fd, &o->txaudiostats, "Tx");
			} else {
				ast_cli(fd, "Tx not keyed\n");
			}
			if (ast_radio_poll_input(fd, 1000)) {
				break;
			}
		}
		break;
	default:
		ast_cli(fd, "Invalid Command\n");
		break;
	}
	o->radio->b.tuning = 0;
	option_verbose = oldverbose;
}

/*!
 * \brief Tune receive voice level.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 * \param intflag		Flag to indicate the type of wait.
 */

/*!
 * \brief Determine the receive CTCSS level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param intflag		Flag to indicate how ast_radio_wait_or_poll waits.
 */

/*!
 * \brief Update the tune settings to the configuration file.
 * \param config	The (opened) config to use
 * \param filename	The configuration file being updated (e.g. "usbradioplus.conf").
 * \param category	The category being updated (e.g. "12345").
 * \param variable	The variable being updated.
 * \param value		The value being updated (e.g. "yes").
 * \retval 0		If successful.
 * \retval -1		If unsuccessful.
 */

/*!
 * \brief Write tune settings to the configuration file. If the device EEPROM is enabled, the
 * settings are  saved to EEPROM. \param o Channel private.
 */
