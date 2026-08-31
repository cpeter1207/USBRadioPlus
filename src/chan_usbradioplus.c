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
#include "asterisk/rpt_chan_shared.h"

#ifdef HAVE_SYS_IO
#include <sys/io.h>
#endif

#define CHAN_USBRADIO 1 /* Enable the channel-driver trace interface. */
#define DEBUG_USBRADIO 0
#define DEBUG_CAPTURES 1
#define DEBUG_CAP_RX_OUT 0
#define DEBUG_CAP_TX_OUT 0
#define DEBUG_FILETEST 0
#define PLUS_LINK_QUEUE_FRAMES 8
#define PLUS_LINK_NATIVE_FIFO_SAMPLES (URP_NATIVE_SAMPLES * 8)
#define PLUS_LINK_NATIVE_TARGET_SAMPLES (URP_NATIVE_SAMPLES * 3)
#define PLUS_DYNAMICS_SAMPLES 48 /* 1 ms control blocks at 48 kHz */
#define DUPLEX3_LEVEL_MAX 999
#define RX_CAP_RAW_FILE "/tmp/rx_cap_in.pcm"
#define RX_CAP_TRACE_FILE "/tmp/rx_trace.pcm"
#define RX_CAP_OUT_FILE "/tmp/rx_cap_out.pcm"
#define TX_CAP_RAW_FILE "/tmp/tx_cap_in.pcm"
#define TX_CAP_TRACE_FILE "/tmp/tx_trace.pcm"
#define TX_CAP_OUT_FILE "/tmp/tx_cap_out.pcm"

#define DELIMCHR ','
#define QUOTECHR 34

#define READERR_THRESHOLD 50
#define DEFAULT_ECHO_MAX 1000 /* 20 secs of echo buffer, max */
#define DEFAULT_TX_SOFT_LIMITER_SETPOINT 12000
#define PP_MASK 0xbffc
#define PP_PORT "/dev/parport0"
#define PP_IOPORT 0x378
#define RPT_TO_STRING(x) #x
#define S_FMT(x) "%" RPT_TO_STRING(x) "s "
#define N_FMT(duf) "%30" #duf				   /* Maximum sscanf conversion to numeric strings */
#define RX_ON_DELAY_MAX 60000				   /* in ms, 60000ms, 60 seconds, 1 minute */
#define TX_OFF_DELAY_MAX 60000				   /* in ms 60000ms, 60 seconds, 1 minute */
#define MS_PER_FRAME 20						   /* 20 ms frames */
#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME) /* convert ms to frames */

enum duplex3_mode {
	DUPLEX3_MODE_HARDWARE = 0,
	DUPLEX3_MODE_SOFTWARE,
};

#include "usbradioplus_radio.h"
#include "usbradioplus_dsp.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_hardware.h"
#include "./txagc/agc_core.h"
#include "./txagc/avfilter_processor.h"
#include "./txagc/rnnoise_processor.h"
#include "usbradioplus_processing.h"
#include "usbradioplus_repeat.h"
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

/*! \brief Global jitterbuffer configuration - by default, jb is disabled */
static struct ast_jb_conf default_jbconf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
};

static struct ast_jb_conf global_jbconf;

#define QUEUE_SIZE 20 /* 400 milliseconds of sound card output buffer */
#define URP_LEGACY_TEST_TONE_PEAK 7518.0

#define CONFIG "usbradioplus.conf" /* default config file */

/* file handles for writing debug audio packets */
static FILE *frxcapraw = NULL, *frxcaptrace = NULL, *frxoutraw = NULL;
static FILE *ftxcapraw = NULL, *ftxcaptrace = NULL, *ftxoutraw = NULL;

AST_MUTEX_DEFINE_STATIC(usb_dev_lock);
AST_MUTEX_DEFINE_STATIC(pp_lock);

/* variables for communicating with the parallel port */
static int8_t pp_val;
static int8_t pp_pulsemask;
static int8_t pp_lastmask;
static int pp_pulsetimer[32];
static int haspp;
static int ppfd;
static char pport[50];
static int pbase;
static char stoppulser;
static char hasout;
pthread_t pulserid;

/*! \brief type of signal detection used for carrier (cd) or ctcss (sd) */
static const char *const cd_signal_type[] = { "no", "dsp", "vox", "usb", "usbinvert", "pp", "ppinvert" };
static const char *const sd_signal_type[] = { "no", "usb", "usbinvert", "dsp", "pp", "ppinvert" };

/*! \brief demodulation type */
static const char *const demodulation_type[] = { "no", "speaker", "flat" };

/*! \brief mixer type */
static const char *const mixer_type[] = { "no", "voice", "tone", "composite", "auxvoice" };

/*!
 * \brief Descriptor for one of our channels.
 * There is one used for 'default' values (from the [general] entry in
 * the configuration file), and then one instance for each device
 * (the default is cloned from [general], others are only created
 * if the relevant section exists).
 */
struct chan_usbradio_pvt {
	struct chan_usbradio_pvt *next;

	char *name;		  /* the internal name of our channel */
	int devtype;	  /* actual type of device */
	int pttkick[2];	  /* ptt kick pipe */
	int total_blocks; /* total blocks in the output device */
	int sounddev;
	enum {
		M_UNSET,
		M_FULL,
		M_READ,
		M_WRITE
	} duplex;
	int hookstate;
	unsigned int queuesize; /* max fragments in queue */
	unsigned int frags;		/* parameter for SETFRAGMENT */

	int warned; /* various flags used for warnings */
#define WARN_used_blocks 1
#define WARN_speed 2
#define WARN_frag 4

	char devicenum;
	char devstr[128];
	char serial[128];
	int spkrmax;
	int micmax;
	int micplaymax;

	pthread_t hidthread;
	int stophid;

	struct ast_channel *owner;

	/* buffer used in usbradio_write, 2 per int by 2 channels by 6 times oversampling (48KS/s) */
	char usbradio_write_buf[FRAME_SIZE * 2 * 2 * 6];
	short plus_rx_native[URP_NATIVE_SAMPLES];
	double plus_local_native[URP_NATIVE_SAMPLES];
	unsigned int plus_app_rpt_rate;
	unsigned int plus_app_rpt_samples;
	short plus_link_native[URP_NATIVE_SAMPLES];
	short plus_link_resampled[URP_NATIVE_SAMPLES * 2];
	short plus_link_native_fifo[PLUS_LINK_NATIVE_FIFO_SAMPLES];
	unsigned int plus_link_native_head;
	unsigned int plus_link_native_count;
	short plus_link_8k[URP_NATIVE_SAMPLES];
	short plus_link_queue[PLUS_LINK_QUEUE_FRAMES][URP_NATIVE_SAMPLES];
	unsigned int plus_link_queue_head;
	unsigned int plus_link_queue_tail;
	unsigned int plus_link_queue_count;
	unsigned int plus_link_queue_high_water;
	unsigned int plus_link_native_primed:1;
	uint64_t plus_link_queue_underflows;
	uint64_t plus_link_queue_overflows;
	ast_mutex_t plus_link_lock;
	short plus_squelch_native[URP_NATIVE_SAMPLES * 2];
	short plus_rx_delay[RXSQDELAYBUFSIZE * 6];
	unsigned int plus_rx_delay_index;
	struct urp_src *plus_up;
	struct urp_src *plus_down;
	struct urp_clock_recovery plus_link_clock;
	struct urp_biquad plus_tx_hpf;
	struct urp_biquad plus_link_hpf;
	struct urp_deemphasis plus_deemphasis;
	struct urp_deemphasis plus_preemphasis;
	struct urp_deemphasis plus_link_preemphasis;
	unsigned int plus_local_preemphasis_active;
	unsigned int plus_link_preemphasis_active;
	struct txagc_core plus_final_core;
	struct txagc_avfilter plus_local_avfilter;
	struct txagc_avfilter plus_rx_filter;
	struct txagc_avfilter plus_rx_filter_after;
	struct txagc_avfilter plus_final_avfilter;
	struct txagc_rnnoise plus_local_rnnoise;
	unsigned int plus_tx_hpf_enabled:1;
	unsigned int plus_link_hpf_enabled:1;
	double plus_tx_hpf_hz;
	double plus_link_hpf_hz;
	double plus_emphasis_corner_hz;
	double plus_presquelch_gain_db;
	double plus_postsquelch_gain_db;
	double plus_tx_ceiling_dbfs;
	double plus_preemphasis_headroom_db;
	double plus_rxlevel_presquelch_target_dbfs;
	double plus_rxlevel_post_target_dbfs;
	uint64_t plus_rxlevel_noise_samples;
	uint64_t plus_rxlevel_signal_samples;
	unsigned int plus_rxlevel_noise_peak;
	unsigned int plus_rxlevel_signal_peak;
	uint64_t plus_rxlevel_positive_rail_samples;
	uint64_t plus_rxlevel_negative_rail_samples;
	unsigned int plus_rxlevel_raw_clip_frames;
	unsigned int plus_rxlevel_frames_left;
	unsigned int plus_rxlevel_active:1;
	uint64_t plus_native_frames;
	uint64_t plus_src_errors;
	double plus_adc_peak_dbfs;
	double plus_adc_max_peak_dbfs;
	uint64_t plus_adc_rail_samples;
	double plus_deemphasis_peak_dbfs;
	double plus_deemphasis_max_peak_dbfs;
	double plus_preemphasis_input_peak_dbfs;
	double plus_preemphasis_input_max_peak_dbfs;
	uint64_t plus_preemphasis_input_ceiling_samples;
	double plus_tx_program_peak_dbfs;
	double plus_tx_program_max_peak_dbfs;
	uint64_t plus_tx_program_rail_samples;
	double plus_local_tx_peak_dbfs;
	double plus_local_tx_max_peak_dbfs;
	uint64_t plus_local_tx_rail_samples;
	uint64_t plus_sound_zero_fill_frames;
	uint64_t plus_sound_dropped_frames;
	uint64_t plus_sound_short_writes;
	uint64_t plus_parrot_playback_frames;
	double *plus_parrot;
	double *plus_parrot_raw;
	size_t plus_parrot_capacity;
	size_t plus_parrot_count;
	size_t plus_parrot_play;
	unsigned int plus_parrot_enabled:1;
	unsigned int plus_parrot_playing:1;
	unsigned int plus_parrot_play_raw:1;
	unsigned int plus_parrot_truncated:1;
	unsigned int plus_parrot_max_seconds;

	/* buffers used in usbradio_read - AST_FRIENDLY_OFFSET space for headers
	 * plus enough room for a full frame
	 */
	char usbradio_read_buf[FRAME_SIZE * (2 * 12) + AST_FRIENDLY_OFFSET]; /* 2 bytes * 2 channels * 6 for 48K */
	char usbradio_read_buf_8k[URP_NATIVE_SAMPLES * 2 + AST_FRIENDLY_OFFSET];
	int readpos;			 /* read position above */
	struct ast_frame read_f; /* returned by usbradio_read */

	char lastrx;
	char rxhidsq;
	char rxhidctcss;
	char rxcarrierdetect; /* status from native radio detector */
	char rxctcssdecode;	  /* status from native CTCSS decoder */
	char rxppsq;
	char rxppctcss;

	char rxkeyed; /* Indicates rx signal is present */

	char lasttx;
	char txkeyed; /* tx key request from upper layers */
	char txtestkey;
	char plus_test_tone_enabled;
	double plus_test_tone_phase;
	struct urp_ctcss_generator plus_ctcss_generator;

	time_t lasthidtime;
	struct ast_dsp *dsp;

	char radioduplex; /* parameter for radio duplex setting */


	int tracetype;
	int tracelevel;
	char area;
	char rptnum;
	int idleinterval;
	int turnoffs;
	int txsettletime;
	int txrxblankingtime;
	char ukey[48];

	int rxdcsdecode;
	int rxlsddecode;

	int rxoncnt;	/* Counts the number of 20 ms intervals after RX activity */
	int txoffcnt;	/* Counts the number of 20 ms intervals after TX unkey */
	int rxondelay;	/* This is the value which RX is ignored after RX activity */
	int txoffdelay; /* This is the value which RX is ignored after TX unkey */

	urp_radio_state *radio;

	enum radio_rx_audio rxdemod;
	float rxgain;
	enum radio_carrier_detect rxcdtype;
	int voxhangtime; /* if rxcdtype=vox, ms to wait detecting RX audio before setting CD=0 */
	enum radio_squelch_detect rxsdtype;
	int rxsquelchadj; /* this copy needs to be here for initialization */
	int rxsqhyst;
	int rxsqvoxadj;
	int rxnoisefiltype;
	int rxsquelchdelay;
	int txslimsp;
	enum usbradio_carrier_type txtoctype;

	float txctcssgain;
	enum radio_tx_mix txmixa;
	enum radio_tx_mix txmixb;
	int rxlpf;
	int rxhpf;
	int txlpf;
	int txhpf;
	int plus_rxlpf_enabled, plus_rxhpf_enabled;
	int plus_txlpf_enabled, plus_txhpf_enabled;
	int plus_rxlpf_exact, plus_rxhpf_exact;
	int plus_txlpf_exact, plus_txhpf_exact;
	double plus_rxlpf_hz, plus_rxhpf_hz;
	double plus_txlpf_hz, plus_txhpf_hz;

	char rxctcssrelax;
	float rxctcssgain;

	char txctcssdefault[16]; /* for repeater operation */
	char rxctcssfreqs[512];	 /* a string */
	char txctcssfreqs[512];

	char txctcssfreq[32]; /* encode now */
	char rxctcssfreq[32]; /* decode now */

	char numrxctcssfreqs; /* how many */
	char numtxctcssfreqs;

	char *rxctcss[CTCSS_NUM_CODES]; /* pointers to strings */
	char *txctcss[CTCSS_NUM_CODES];

	int txfreq; /* in Hz */
	int rxfreq;

	/*      start remote operation info */
	char set_txctcssdefault[16]; /* for remote operation */
	char set_txctcssfreq[16];	 /* encode now */
	char set_rxctcssfreq[16];	 /* decode now */

	char set_numrxctcssfreqs; /* how many */
	char set_numtxctcssfreqs;

	char set_rxctcssfreqs[16]; /* a string */
	char set_txctcssfreqs[16];

	char *set_rxctcss; /* pointers to strings */
	char *set_txctcss;

	int set_txfreq; /* in Hz */
	int set_rxfreq;
	int set_txpower;

	/*      end remote operation info */

	int rxmixerset;
	int txboost;
	float rxvoiceadj;
	int txmixaset;
	int txmixbset;
	int txctcssadj;

	/*! \brief Settings for echoing received audio */
	int echomode;
	int echoing;
	ast_mutex_t echolock;
	struct qelem echoq;
	int echomax;

	/*! \brief Settings for HID interface */
	int hdwtype;
	int hid_gpio_ctl;
	int hid_gpio_ctl_loc;
	int hid_io_cor;
	int hid_io_cor_loc;
	int hid_io_ctcss;
	int hid_io_ctcss_loc;
	int hid_io_ptt;
	int hid_gpio_loc;
	int32_t hid_gpio_val;
	int32_t valid_gpios;
	int32_t gpio_set;
	int32_t last_gpios_in;
	int had_gpios_in;
	int hid_gpio_pulsetimer[GPIO_PINCOUNT];
	int32_t hid_gpio_pulsemask;
	int32_t hid_gpio_lastmask;

	/*! \brief Track parallel port values */
	int8_t last_pp_in;
	char had_pp_in;

	/* bit fields */
	unsigned int rxcapraw:1;		/* indicator if receive capture is enabled */
	unsigned int txcapraw:1;		/* indicator if transmit capture is enabled */
	unsigned int rxcap2:1;			/* indicator if receive capture 2 is enabled */
	unsigned int txcap2:1;			/* indicator if transmit capture 2 is enabled */
	unsigned int remoted:1;			/* indicator if rx/tx frequency adjusted */
	unsigned int forcetxcode:1;		/* indicator to force use of first ctcss code */
	unsigned int rxpolarity:1;		/* indicator for receive polarity */
	unsigned int txpolarity:1;		/* indicator for transmit polarity */
	unsigned int dcsrxpolarity:1;	/* indicator for dcs receive polarity */
	unsigned int dcstxpolarity:1;	/* indicator for dcs transmit polarity */
	unsigned int lsdrxpolarity:1;	/* indicator for lsd receive polarity */
	unsigned int lsdtxpolarity:1;	/* indicator for lsd transmit polarity */
	unsigned int radioactive:1;		/* indicator for active radio channel */
	unsigned int device_error:1;	/* indicator set when we cannot find the USB device */
	unsigned int newname:1;			/* indicator that we should use MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW */
	unsigned int hasusb:1;			/* indicator for has a USB device */
	unsigned int usbass:1;			/* indicator for USB device assigned */
	unsigned int wanteeprom:1;		/* indicator if we should use EEPROM */
	unsigned int usedtmf:1;			/* indicator is we should decode DTMF */
	unsigned int invertptt:1;		/* indicator if we need to invert ptt */
	unsigned int rxboost:1;			/* indicator if receive boost is needed */
	unsigned int rxcpusaver:1;		/* indicator if receive cpu save is enabled */
	unsigned int txcpusaver:1;		/* indicator if transmit cpu save is enabled */
	unsigned int txprelim:1;		/* indicator if tx pre lim is enabled */
	unsigned int txlimonly:1;		/* indicator if tx lim only is enabled */
	unsigned int rxctcssoverride:1; /* indicator if receive ctcss override is enabled */
	unsigned int rx_cos_active:1;	/* indicator if cos is active - active state after processing */
	unsigned int rx_ctcss_active:1; /* indicator if ctcss is active - active state after processing */

	/* EEPROM access variables */
	unsigned short eeprom[EEPROM_USER_LEN];
	char eepromctl;
	ast_mutex_t eepromlock;

	struct usb_dev_handle *usb_handle;
	int readerrs;
	struct timeval tonetime;
	int toneflag;
	int duplex3;
	enum duplex3_mode duplex3mode;
	int clipledgpio; /* enables ADC Clip Detect feature to output on a specified GPIO# */

	int fever;
	int count_rssi_update;

	int32_t cur_gpios;
	char *gpios[GPIO_PINCOUNT];
	char *pps[32];
	int sendvoter;

	struct audiostatistics rxaudiostats;
	struct audiostatistics txaudiostats;

	int legacyaudioscaling;

	ast_mutex_t usblock;
};

/*!
 * \brief Default channel descriptor
 */
static struct chan_usbradio_pvt usbradio_default = {
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
	.rxaudiostats.index = 0,
	/* After the vast majority of existing installs have had a chance to review their
	   audio settings and the associated old scaling/clipping hacks are no longer in
	   significant use the following cfg and all related code should be deleted. */
	.legacyaudioscaling = 1,
	/* app_rpt currently supplies 8 kHz frames; keep the rate explicit so a
	 * future native-rate app_rpt path can bypass conversion. */
	.plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT,
	.plus_app_rpt_samples = URP_LINK_SAMPLES,
	.plus_rxlpf_enabled = 1,
	.plus_rxhpf_enabled = 1,
	.plus_txlpf_enabled = 1,
	.plus_txhpf_enabled = 1,
	.plus_rxlpf_hz = 3000.0,
	.plus_rxhpf_hz = 300.0,
	.plus_txlpf_hz = 3000.0,
	.plus_txhpf_hz = 300.0,
	.plus_tx_hpf_enabled = 1,
	.plus_link_hpf_enabled = 1,
	.plus_tx_hpf_hz = 250.0,
	.plus_link_hpf_hz = 250.0,
	/* 750 us land-mobile pre/deemphasis: fc = 1 / (2*pi*750 us). */
	.plus_emphasis_corner_hz = 212.206590789,
	.plus_presquelch_gain_db = 0.0,
	.plus_postsquelch_gain_db = 0.0,
	.plus_tx_ceiling_dbfs = 0.0,
	.plus_preemphasis_headroom_db = 24.0,
	.plus_rxlevel_presquelch_target_dbfs = -1.0,
	.plus_rxlevel_post_target_dbfs = -1.0,
	.plus_parrot_max_seconds = 30,
};

/*	DECLARE FUNCTION PROTOTYPES	*/

static int hidhdwconfig(struct chan_usbradio_pvt *o);
static void mixer_write(struct chan_usbradio_pvt *o);
static int setformat(struct chan_usbradio_pvt *o, int mode);
static struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
	const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause);
static int usbradio_digit_begin(struct ast_channel *c, char digit);
static int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration);
static int usbradio_text(struct ast_channel *c, const char *text);
static int usbradio_hangup(struct ast_channel *c);
static int usbradio_answer(struct ast_channel *c);
static struct ast_frame *usbradio_read(struct ast_channel *chan);
static int usbradio_call(struct ast_channel *c, const char *dest, int timeout);
static int usbradio_write(struct ast_channel *chan, struct ast_frame *f);
static void usbradioplus_queue_program(struct chan_usbradio_pvt *o,
	const short *samples, size_t count);
static int usbradio_indicate(struct ast_channel *chan, int cond_in, const void *data, size_t datalen);
static int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen);
static void store_rxvoiceadj(struct chan_usbradio_pvt *o, const char *s);
static int set_txctcss_level(struct chan_usbradio_pvt *o);
static void radio_dump(struct chan_usbradio_pvt *o, int fd);
static void mult_set(struct chan_usbradio_pvt *o);
static int mult_calc(int value);
static void tune_rxinput(int fd, struct chan_usbradio_pvt *o, int setsql, int flag);
static void tune_rxvoice(int fd, struct chan_usbradio_pvt *o, int flag);
static void tune_menusupport(int fd, struct chan_usbradio_pvt *o, const char *cmd);
static void tune_rxctcss(int fd, struct chan_usbradio_pvt *o, int flag);
static void tune_txoutput(struct chan_usbradio_pvt *o, int value, int fd, int flag);
static void tune_write(struct chan_usbradio_pvt *o);
static int radio_config(struct chan_usbradio_pvt *o);
static void usbradioplus_parallel_program_write(void *opaque, uint8_t value);
static void usbradioplus_program_radio(struct chan_usbradio_pvt *o);
static void usbradioplus_set_channel(uint8_t channel);
static int legacy_set_tx_soft_limiter(struct chan_usbradio_pvt *o, int setpoint);
static int usbradioplus_dsp_init(struct chan_usbradio_pvt *o);
static double usbradioplus_legacy_cutoff(const char *name, int selector);
static void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o);
static void usbradioplus_native_tick(struct chan_usbradio_pvt *o);
static void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed);
static void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o);
#if DEBUG_FILETEST == 1
static int RxTestIt(struct chan_usbradio_pvt *o);
#endif

static char *usbradio_active; /* the active device */

static const int ppinshift[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 5, 4, 0, 3 };

static const char tdesc[] = "USB (CM108) Radio Channel Driver";

/*!
 * \brief Asterisk channel technology struct.
 * This tells Asterisk the functions to call when
 * it needs to interact with our module.
 */
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

/*!
 * \brief Configure our private structure based on the
 * found hardware type.
 * \param o		Channel private data.
 * \returns 0	Always returns zero.
 */
static int hidhdwconfig(struct chan_usbradio_pvt *o)
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
		o->hid_io_cor = 4;		 /* GPIO3 is COR */
		o->hid_io_cor_loc = 1;	 /* GPIO3 is COR */
		o->hid_io_ctcss = 2;	 /* GPIO 2 is External CTCSS */
		o->hid_io_ctcss_loc = 1; /* is GPIO 2 */
		o->hid_io_ptt = 8;		 /* GPIO 4 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 1;		 /* for GPIO 1 */
	} else if (o->hdwtype == 0) {
		/* dudeusb */
		o->hid_gpio_ctl = 0x04;	 /* set GPIO 3 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;		 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 1;	 /* VOL UP External CTCSS */
		o->hid_io_ctcss_loc = 0; /* VOL UP External CTCSS */
		o->hid_io_ptt = 4;		 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 0xfb;	 /* for GPIO 1,2,4,5,6,7,8 (5,6,7,8 for CM-119 only) */
	} else if (o->hdwtype == 2) {
		/* NHRC (N1KDO) (dudeusb w/o user GPIO) */
		o->hid_gpio_ctl = 0x04;	 /* set GPIO 3 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;		 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 1;	 /* VOL UP is External CTCSS */
		o->hid_io_ctcss_loc = 0; /* VOL UP CTCSS */
		o->hid_io_ptt = 4;		 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 0;		 /* for GPIO 1,2,4 */
	} else if (o->hdwtype == 3) {
		/* custom version */
		o->hid_gpio_ctl = 0x0c;	 /* set GPIO 3 & 4 to output mode */
		o->hid_gpio_ctl_loc = 2; /* For CTL of GPIO */
		o->hid_io_cor = 2;		 /* VOLD DN is COR */
		o->hid_io_cor_loc = 0;	 /* VOL DN COR */
		o->hid_io_ctcss = 2;	 /* GPIO 2 is External CTCSS */
		o->hid_io_ctcss_loc = 1; /* is GPIO 2 */
		o->hid_io_ptt = 4;		 /* GPIO 3 is PTT */
		o->hid_gpio_loc = 1;	 /* For ALL GPIO */
		o->valid_gpios = 1;		 /* for GPIO 1 */
	}
	/* validate clipledgpio setting (Clip LED GPIO#) */
	if (o->clipledgpio) {
		if (o->clipledgpio >= GPIO_PINCOUNT || !(o->valid_gpios & (1 << (o->clipledgpio - 1)))) {
			ast_log(LOG_ERROR, "Channel %s: clipledgpio = GPIO%d not supported\n", o->name, o->clipledgpio);
			o->clipledgpio = 0;
		} else {
			o->hid_gpio_ctl |= 1 << (o->clipledgpio - 1); /* confirm Clip LED GPIO set to output mode */
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
			ast_log(LOG_ERROR, "Channel %s: You can't specify gpio%d, since its the PTT.\n", o->name, i + 1);
			continue;
		}
		/* skip if not a valid GPIO */
		if (!(o->valid_gpios & (1 << i))) {
			ast_log(LOG_ERROR, "Channel %s: You can't specify gpio%d, it is not valid in this configuration.\n", o->name, i + 1);
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

/*!
 * \brief Indicate that PTT is activate.
 *	This causes the hidthead to to exit from the loop timer and
 *	evaluate the gpio pins.
 * \param o		Channel private data.
 */
static void kickptt(const struct chan_usbradio_pvt *o)
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

/*!
 * \brief Search our configured channels to find the
 *	one with the matching USB descriptor.
 *	Print a message if the descriptor was not found.
 * \param o		chan_usbradio_pvt.
 * \returns		Private structure that matches or NULL if not found.
 */
static struct chan_usbradio_pvt *find_desc(const char *dev)
{
	struct chan_usbradio_pvt *o = NULL;

	for (o = usbradio_default.next; o && o->name && dev && strcmp(o->name, dev) != 0; o = o->next)
		;
	if (!o) {
		ast_log(LOG_WARNING, "Cannot find USB descriptor <%s>.\n", dev ? dev : "-- Null Descriptor --");
		return NULL;
	}
	return o;
}

/*!
 * \brief Search our configured channels to find the
 *	one with the matching USB descriptor.
 * \param o		chan_usbradio_pvt.
 * \returns		Private structure that matches or NULL if not found.
 */
static struct chan_usbradio_pvt *find_desc_usb(const char *devstr)
{
	struct chan_usbradio_pvt *o = NULL;

	if (!devstr) {
		ast_log(LOG_WARNING, "USB Descriptor is null.\n");
	}

	for (o = usbradio_default.next; o && devstr && strcmp(o->devstr, devstr) != 0; o = o->next)
		;

	return o;
}

/*!
 * \brief Search installed devices for a match with
 *	one of our configured channels.
 * \returns		Matching device string, or NULL.
 */
static char *find_installed_usb_match(void)
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

/*!
 * \brief Parallel port processing thread.
 *	This thread evaluates the timers configured for each
 *  configured parallel port pin.
 * \param arg	Arguments - this is always NULL.
 */
static void *pulserthread(void *arg)
{
	(void) arg;
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
		/* make output inversion mask (for pulseage) */
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
		if (pp_pulsemask != pp_lastmask) { /* if anything inverted (temporarily) */
			pp_val ^= pp_lastmask ^ pp_pulsemask;
			ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
		}
		ast_mutex_unlock(&pp_lock);
	}
	pthread_exit(0);
}

/*!
 * \brief Load settings for a specific node
 * \param o
 * \param cfg If provided, will use the provided config. If NULL, cfg will be opened automatically.
 * \param reload 0 for first load, 1 for reload
 */
static int load_tune_config(struct chan_usbradio_pvt *o, const struct ast_config *cfg, int reload)
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
	o->rxvoiceadj = 0.5;
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
		struct ast_flags zeroflag = { 0 };
		cfg2 = ast_config_load(CONFIG, zeroflag);
		if (!cfg2) {
			ast_log(LOG_WARNING, "Can't %sload settings for %s, using default parameters\n", reload ? "re" : "", o->name);
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
		CV_UINT("txctcssadj", o->txctcssadj);
		CV_UINT("rxsquelchadj", o->rxsquelchadj);
		CV_UINT("txslimsp", o->txslimsp);
		CV_UINT("fever", o->fever);
		CV_STR("devstr", devstr);
		CV_STR("serial", serial);
		CV_END;
	}
	if (!reload) {
		/* Using the ternary operator in CV_STR won't work, due to butchering the sizeof, so copy after if needed */
		strcpy(o->devstr, devstr); /* Safe */
		strcpy(o->serial, serial); /* Safe */
	}
	if (opened) {
		ast_config_destroy(cfg2);
	}
	if (!configured) {
		ast_log(LOG_WARNING, "Can't %sload settings for %s (no section available), using default parameters\n", reload ? "re" : "", o->name);
		return -1;
	}
	return 0;
}

/*!
 * \brief USB sound device GPIO processing thread
 * This thread is responsible for finding and associating the node with the
 * associated usb sound card device.  It performs setup and initialization of
 * the USB device.
 *
 * The CM-XXX USB devices can support up to 8 GPIO pins that can be input or output.
 * It continuously polls the input GPIO pins on the device to see if they have changed.
 * The default GPIOs for COS, and CTCSS provide the basic functionality. An asterisk
 * text frame is raised in the format 'GPIO%d %d' when GPIOs change. Polling generally
 * occurs every 50 milliseconds.
 *
 * The output PTT (push to talk) GPIO, along with other GPIO outputs are updated as
 * required.
 *
 * If the user has enabled the parallel port for GPIOs, they are polled and updated
 * as appropriate.  An asterisk text frame is raised in the format 'PP%d %d' when
 * GPIOs change. (Parallel port support is not available for all platforms.)
 *
 * This routine also reads and writes to the EPROM attached to the USB device.  The
 * EPROM holds the configuration information (sound level settings) for this device.
 *
 * This routine updates the lasthidtimer during setup and processing.  In the event
 * that this timer update does not occur over a period of 3 seconds, app_rpt will
 * kill the node and restart everything.  This helps to detect problems with a
 * hung USB device.
 *
 * \param argv		chan_usbradio_pvt structure associated with this thread.
 */
static void *hidthread(void *arg)
{
	unsigned char buf[4], bufsave[4], keyed, ctcssed;
	char *s, lasttxtmp;
	register int i, j, k;
	int res;
	struct usb_device *usb_dev;
	struct usb_dev_handle *usb_handle;
	struct chan_usbradio_pvt *o = arg, *ao;
	struct timeval then;
	struct pollfd rfds[1];

	usb_dev = NULL;
	usb_handle = NULL;
	/* enable gpio_set so that we will write GPIO information upon start up */
	o->gpio_set = 1;

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
		char serial[sizeof(o->serial)] = { '\0' };

		ast_radio_time(&o->lasthidtime);
		ast_mutex_lock(&usb_dev_lock);
		o->hasusb = 0;
		o->usbass = 0;
		o->devicenum = 0;
		if (usb_handle) {
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
				if (ast_radio_usb_get_serial(index_devstr, serial, sizeof(serial)) == 0) {
					/* if no serial number */
					continue;
				}

				if (strcmp(o->serial, serial) == 0) {
					/*
					 * We found a device with the matching serial number, set
					 * the devstr to the matching device.
					 */
					ast_log(LOG_NOTICE, "Matched device serial %s to %s\n", o->serial, o->name);
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
						ast_log(LOG_ERROR, "Channel %s: No USB devices are available for assignment.\n", o->name);
						o->device_error = 1;
					}
					ast_mutex_unlock(&usb_dev_lock);
					usleep(500000);
					break;
				}
				/* We found an available device - see if it already in use */
				for (ao = usbradio_default.next; ao && ao->name; ao = ao->next) {
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
				ast_log(LOG_NOTICE, "Channel %s: Automatically assigned USB device %s to USBRadio channel\n", o->name, o->devstr);
				if (ast_radio_usb_get_serial(index_devstr, serial, sizeof(serial)) > 0) {
					ast_copy_string(o->serial, serial, sizeof(o->serial));
				}
				break;
			}
			if (ast_strlen_zero(o->devstr)) {
				continue;
			}
		}

		if ((!ast_radio_usb_list_check(o->devstr)) || (!find_desc_usb(o->devstr))) {
			/* The device string did not match.
			 * Now look through the attached devices and see
			 * one of those is associated with one of our
			 * configured channels.
			 */
			s = find_installed_usb_match();
			if (ast_strlen_zero(s)) {
				if (!o->device_error) {
					ast_log(LOG_ERROR, "Channel %s: Device string %s was not found.\n", o->name, o->devstr);
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
			for (ao = usbradio_default.next; ao && ao->name; ao = ao->next) {
				if (ao->usbass && (!strcmp(ao->devstr, s))) {
					break;
				}
			}
			if (ao) {
				ast_log(LOG_ERROR, "Channel %s: Device string %s is already assigned to channel %s", o->name, s, ao->name);
				ast_mutex_unlock(&usb_dev_lock);
				usleep(500000);
				continue;
			}
			ast_log(LOG_NOTICE, "Channel %s: Assigned USB device %s to usbradio channel\n", o->name, s);
			ast_copy_string(o->devstr, s, sizeof(o->devstr));
		}
		/* Double check to see if the device string is assigned to another usb channel */
		for (ao = usbradio_default.next; ao && ao->name; ao = ao->next) {
			if (ao->usbass && (!strcmp(ao->devstr, o->devstr))) {
				break;
			}
		}
		if (ao) {
			ast_log(LOG_ERROR, "Channel %s: Device string %s is already assigned to channel %s", o->name, o->devstr, ao->name);
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
			o->spkrmax = ast_radio_amixer_max(o->devicenum, MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW);
		}
		/* initialize the usb device */
		usb_dev = ast_radio_hid_device_init(o->devstr);
		if (usb_dev == NULL) {
			ast_log(LOG_ERROR, "Channel %s: Cannot initialize device %s\n", o->name, o->devstr);
			usleep(500000);
			continue;
		}
		/* open the usb device device */
		usb_handle = usb_open(usb_dev);
		if (usb_handle == NULL) {
			ast_log(LOG_ERROR, "Channel %s: Cannot open device %s\n", o->name, o->devstr);
			usleep(500000);
			continue;
		}
		/* attempt to claim the usb hid interface and detach from the kernel */
		if (usb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
			if (usb_detach_kernel_driver_np(usb_handle, C108_HID_INTERFACE) < 0) {
				ast_log(LOG_ERROR, "Channel %s: Is not able to detach the USB device\n", o->name);
				usleep(500000);
				continue;
			}
			if (usb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
				ast_log(LOG_ERROR, "Channel %s: Is not able to claim the USB device\n", o->name);
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
			pthread_exit(NULL);
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
			tChan.rxCdType = o->rxcdtype;
			tChan.voxHangTime = o->voxhangtime;
			tChan.rxSqVoxAdj = o->rxsqvoxadj;

			if (o->txlimonly) {
				tChan.txMod = 1;
			}
			if (o->txprelim) {
				tChan.txMod = 2;
			}

			tChan.txMixA = o->txmixa;
			tChan.txMixB = o->txmixb;

			tChan.rxCpuSaver = o->rxcpusaver;
			tChan.txCpuSaver = o->txcpusaver;

			tChan.b.rxpolarity = o->rxpolarity;
			tChan.b.txpolarity = o->txpolarity;

			tChan.b.dcsrxpolarity = o->dcsrxpolarity;
			tChan.b.dcstxpolarity = o->dcstxpolarity;

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

			o->radio->radioDuplex = o->radioduplex;
			o->radio->b.loopback = 0;
			o->radio->txsettletime = o->txsettletime;
			o->radio->txrxblankingtime = o->txrxblankingtime;
			o->radio->rxCpuSaver = o->rxcpusaver;
			o->radio->txCpuSaver = o->txcpusaver;

			*(o->radio->prxSquelchAdjust) = ((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
			*(o->radio->prxVoiceAdjust) = o->rxvoiceadj * M_Q8;
			o->radio->rxCtcss->relax = o->rxctcssrelax;
			o->radio->txTocType = o->txtoctype;

			if ((o->txmixa == TX_OUT_LSD) || (o->txmixa == TX_OUT_COMPOSITE) || (o->txmixb == TX_OUT_LSD) || (o->txmixb == TX_OUT_COMPOSITE)) {
				set_txctcss_level(o);
			}

			if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) && (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
				ast_log(LOG_ERROR, "Channel %s: No txvoice output configured.\n", o->name);
			}

			if (o->txctcssfreq[0] && o->txmixa != TX_OUT_LSD && o->txmixa != TX_OUT_COMPOSITE && o->txmixb != TX_OUT_LSD &&
				o->txmixb != TX_OUT_COMPOSITE) {
				ast_log(LOG_ERROR, "No txtone output configured.\n");
			}

			if (o->radioactive) {
				struct chan_usbradio_pvt *ao;
				for (ao = usbradio_default.next; ao && ao->name; ao = ao->next)
					ao->radioactive = 0;
				usbradio_active = o->name;
				o->radioactive = 1;
				ast_log(LOG_NOTICE, "radio active set to [%s]\n", o->name);
			}
		}
		radio_config(o);
		mixer_write(o);
		mult_set(o);

		/* reload the settings from the tune file */
		load_tune_config(o, NULL, 1);

		mixer_write(o);
		mult_set(o);
		set_txctcss_level(o);
		/* Sync the native limiter fallback with the tuning configuration. */
		if (legacy_set_tx_soft_limiter(o, o->txslimsp)) {
			/* Invalid setting in config file. Set default */
			ast_log(LOG_WARNING, "Invalid value for txslimsp in radio settings section of usbradio.c, using default");
			o->txslimsp = DEFAULT_TX_SOFT_LIMITER_SETPOINT;
			legacy_set_tx_soft_limiter(o, o->txslimsp);
		}

		ast_mutex_lock(&o->eepromlock);
		if (o->wanteeprom) {
			o->eepromctl = 1;
		}
		ast_mutex_unlock(&o->eepromlock);

		setformat(o, O_RDWR);
		o->hasusb = 1;
		o->had_gpios_in = 0;

		memset(&rfds, 0, sizeof(rfds));
		rfds[0].fd = o->pttkick[1];
		rfds[0].events = POLLIN;

		ast_radio_time(&o->lasthidtime);
		/* Main processing loop for GPIO
		 * This loop process every 50 milliseconds.
		 * The timer can be interrupted by writing to
		 * the pttkick pipe.
		 */
		while ((!o->stophid) && o->hasusb) {
			then = ast_radio_tvnow();
			/* poll the pttkick pipe - timeout after 50 milliseconds */
			res = ast_poll(rfds, 1, 50);
			if (res < 0) {
				ast_log(LOG_WARNING, "Channel %s: Poll failed: %s\n", o->name, strerror(errno));
				usleep(10000);
				continue;
			}
			if (rfds[0].revents) {
				char c;

				int bytes = read(o->pttkick[0], &c, 1);
				if (bytes <= 0) {
					ast_log(LOG_ERROR, "Channel %s: pttkick read failed: %s\n", o->name, strerror(errno));
				}
			}
			/* see if we need to process an eeprom read or write */
			if (o->wanteeprom) {
				ast_mutex_lock(&o->eepromlock);
				if (o->eepromctl == 1) { /* to read */
					/* if CS okay */
					if (!ast_radio_get_eeprom(usb_handle, o->eeprom)) {
						if (o->eeprom[EEPROM_USER_MAGIC_ADDR] != EEPROM_MAGIC) {
							ast_log(LOG_ERROR, "Channel %s: EEPROM bad magic number\n", o->name);
						} else {
							o->rxmixerset = o->eeprom[EEPROM_USER_RXMIXERSET];
							o->txmixaset = o->eeprom[EEPROM_USER_TXMIXASET];
							o->txmixbset = o->eeprom[EEPROM_USER_TXMIXBSET];
							memcpy(&o->rxvoiceadj, &o->eeprom[EEPROM_USER_RXVOICEADJ], sizeof(float));
							o->txctcssadj = o->eeprom[EEPROM_USER_TXCTCSSADJ];
							o->rxsquelchadj = o->eeprom[EEPROM_USER_RXSQUELCHADJ];
							ast_log(LOG_NOTICE, "Channel %s: EEPROM Loaded\n", o->name);
							mixer_write(o);
							mult_set(o);
							set_txctcss_level(o);
						}
					} else {
						ast_log(LOG_ERROR, "Channel %s: USB adapter has no EEPROM installed or Checksum is bad\n", o->name);
					}
					ast_radio_hid_set_outputs(usb_handle, bufsave);
				}
				if (o->eepromctl == 2) { /* to write */
					ast_radio_put_eeprom(usb_handle, o->eeprom);
					ast_radio_hid_set_outputs(usb_handle, bufsave);
					ast_log(LOG_NOTICE, "Channel %s: USB parameters written to EEPROM\n", o->name);
				}
				o->eepromctl = 0;
				ast_mutex_unlock(&o->eepromlock);
			}
			ast_mutex_lock(&o->usblock);
			buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
			ast_radio_hid_get_inputs(usb_handle, buf);
			/* See if we are keyed */
			keyed = !(buf[o->hid_io_cor_loc] & o->hid_io_cor);
			if (keyed != o->rxhidsq) {
				ast_debug(2, "Channel %s: Update rxhidsq = %d\n", o->name, keyed);
				o->rxhidsq = keyed;
			}
			/* See if we are receiving ctcss */
			ctcssed = !(buf[o->hid_io_ctcss_loc] & o->hid_io_ctcss);
			if (ctcssed != o->rxhidctcss) {
				ast_debug(2, "Channel %s: Update rxhidctcss = %d\n", o->name, ctcssed);
				o->rxhidctcss = ctcssed;
			}
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
				if ((o->gpios[i]) && (!strcasecmp(o->gpios[i], "in")) && (o->valid_gpios & (1 << i))) {
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
					if ((!o->had_gpios_in) || ((o->last_gpios_in & (1 << i)) != (j & (1 << i)))) {
						snprintf(buf1, sizeof(buf1), "GPIO%d %d\n", i + 1, (j & (1 << i)) ? 1 : 0);
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
				j = k = ast_radio_ppread(haspp, ppfd, pbase, pport) ^ 0x80; /* get PP input */
				ast_mutex_unlock(&pp_lock);
				for (i = 10; i <= 15; i++) {
					/* if a valid input bit, dont clear it */
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "in")) && (PP_MASK & (1 << i))) {
						continue;
					}
					j &= ~(1 << ppinshift[i]); /* clear the bit, since its not an input */
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
						/* skip if not valid */
						if (!(PP_MASK & (1 << i))) {
							continue;
						}
						/* if bit has changed, or never reported */
						if ((!o->had_pp_in) || ((o->last_pp_in & (1 << ppinshift[i])) != (j & (1 << ppinshift[i])))) {
							snprintf(buf1, sizeof(buf1), "PP%d %d\n", i, (j & (1 << ppinshift[i])) ? 1 : 0);
							fr.data.ptr = buf1;
							fr.datalen = strlen(buf1);
							ast_queue_frame(o->owner, &fr);
						}
					}
					o->had_pp_in = 1;
					o->last_pp_in = j;
				}
				o->rxppsq = o->rxppctcss = 0;
				for (i = 10; i <= 15; i++) {
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "cor")) && (PP_MASK & (1 << i))) {
						j = k & (1 << ppinshift[i]); /* set the bit accordingly */
						if (j != o->rxppsq) {
							ast_debug(2, "Channel %s: update rxppsq = %d\n", o->name, j);
							o->rxppsq = j;
						}
					} else if ((o->pps[i]) && (!strcasecmp(o->pps[i], "ctcss")) && (PP_MASK & (1 << i))) {
						o->rxppctcss = k & (1 << ppinshift[i]); /* set the bit accordingly */
					}
				}
			}
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
			if (o->hid_gpio_pulsemask || o->hid_gpio_lastmask) { /* if anything inverted (temporarily) */
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
			/* Apply transmitter state changes from the signaling engine. */
			lasttxtmp = o->radio->txPttOut;

			k = 0;
			if (haspp) {
				for (i = 2; i <= 9; i++) {
					/* skip if this one not specified */
					if (!o->pps[i]) {
						continue;
					}
					/* skip if not ptt */
					if (strncasecmp(o->pps[i], "ptt", 3)) {
						continue;
					}
					k |= (1 << (i - 2)); /* make mask */
				}
			}
			if (o->lasttx != lasttxtmp) {
				o->radio->txPttHid = o->lasttx = lasttxtmp;
				ast_debug(2, "Channel %s: tx set to %d\n", o->name, o->lasttx);
				o->hid_gpio_val &= ~o->hid_io_ptt;
				ast_mutex_lock(&pp_lock);
				if (k) {
					pp_val &= ~k;
				}
				if (!o->invertptt) {
					if (lasttxtmp) {
						o->hid_gpio_val |= o->hid_io_ptt;
						if (k) {
							pp_val |= k;
						}
					}
				} else {
					if (!lasttxtmp) {
						o->hid_gpio_val |= o->hid_io_ptt;
						if (k) {
							pp_val |= k;
						}
					}
				}
				if (k) {
					ast_radio_ppwrite(haspp, ppfd, pbase, pport, pp_val);
				}
				ast_mutex_unlock(&pp_lock);
				buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
				buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
				memcpy(bufsave, buf, sizeof(buf));
				ast_radio_hid_set_outputs(usb_handle, buf);
			}
			ast_radio_time(&o->lasthidtime);
			ast_mutex_unlock(&o->usblock);
		}
		lasttxtmp = o->radio->txPttOut = 0;
		o->lasttx = 0;
		ast_mutex_lock(&o->usblock);
		o->hid_gpio_val &= ~o->hid_io_ptt;
		if (o->invertptt) {
			o->hid_gpio_val |= o->hid_io_ptt;
		}
		buf[o->hid_gpio_loc] = o->hid_gpio_val ^ o->hid_gpio_pulsemask;
		buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
		ast_radio_hid_set_outputs(usb_handle, buf);
		ast_mutex_unlock(&o->usblock);
	}
	/* clean up before exiting the thread */
	lasttxtmp = o->radio->txPttOut = 0;
	o->lasttx = 0;
	if (usb_handle) {
		ast_mutex_lock(&o->usblock);
		o->hid_gpio_val &= ~o->hid_io_ptt;
		if (o->invertptt) {
			o->hid_gpio_val |= o->hid_io_ptt;
		}
		buf[o->hid_gpio_loc] = o->hid_gpio_val;
		buf[o->hid_gpio_ctl_loc] = o->hid_gpio_ctl;
		ast_radio_hid_set_outputs(usb_handle, buf);
		ast_mutex_unlock(&o->usblock);
	}
	pthread_exit(0);
}

/*!
 * \brief Get the number of blocks used in the audio output channel.
 * \param o		Channel private data.
 * \returns		Number of blocks that have been used.
 */
static int used_blocks(struct chan_usbradio_pvt *o)
{
	struct audio_buf_info info;

	if (ioctl(o->sounddev, SNDCTL_DSP_GETOSPACE, &info)) {
		if (!(o->warned & WARN_used_blocks)) {
			ast_log(LOG_WARNING, "Channel %s: Error reading output space.\n", o->name);
			o->warned |= WARN_used_blocks;
		}
		return 1;
	}

	/* Set the total blocks */
	if (o->total_blocks == 0) {
		ast_debug(1, "Channel %s: fragment total %d, size %d, available %d, bytes %d\n", o->name, info.fragstotal, info.fragsize,
			info.fragments, info.bytes);
		o->total_blocks = info.fragments;
		/* Check the queue size, it cannot exceed the total fragments */
		if (o->queuesize >= (unsigned int) info.fragstotal) {
			o->queuesize = info.fragstotal - 1;
			if (o->queuesize < 2) {
				o->queuesize = QUEUE_SIZE;
			}
			ast_debug(1, "Channel %s: Queue size reset to %d\n", o->name, o->queuesize);
		}
	}

	return o->total_blocks - info.fragments;
}

/*!
 * \brief Write a full frame of audio data to the sound card device.
 * \note The input data must be formatted as stereo at 48000 samples per second.
 *		 FRAME_SIZE * 2 * 2 * 6 (2 bytes per sample, 2 channels, 6 for upsample to 48K)
 * \param o		chan_usbradio_pvt.
 * \param data	Audio data to write.
 * \returns		Number bytes written.
 */
static int soundcard_writeframe(struct chan_usbradio_pvt *o, short *data)
{
	int res;
	short outbuf[FRAME_SIZE * 2 * 6];

	/* If the sound device is not open, setformat will open the device */
	if (o->sounddev < 0) {
		setformat(o, O_RDWR);
	}
	if (o->sounddev < 0) {
		return 0; /* not fatal */
	}
	/*  This may or may not be a good thing
	 *  drop the frame if not transmitting, this keeps from gradually
	 *  filling the buffer when asterisk clock > usb sound clock
	 */
	if (!o->radio->txPttIn && !o->radio->txPttOut) {
		return 0;
	}
	/*
	 * Nothing complex to manage the audio device queue.
	 * If the buffer is full just drop the extra, otherwise write.
	 * In some cases it might be useful to write anyways after
	 * a number of failures, to restart the output chain.
	 */
	res = used_blocks(o);
	if ((unsigned int) res > o->queuesize) { /* no room to write a block */
		o->plus_sound_dropped_frames++;
		/* Only report a buffer overflow when we are transmitting */
		if (o->radio->txPttIn || o->radio->txPttOut) {
			ast_log(LOG_WARNING, "Channel %s: Sound device write buffer overflow - used %d blocks\n", o->name, res);
		}
		return 0;
	}
	if (res == 0) { /* We are not keeping the buffer full, add 1 frame */
		o->plus_sound_zero_fill_frames++;
		memset(outbuf, 0, sizeof(outbuf));
		res = write(o->sounddev, ((void *) outbuf), sizeof(outbuf));
		if (res < 0) {
			o->plus_sound_short_writes++;
			ast_log(LOG_ERROR, "Channel %s: Sound card write error %s\n", o->name, strerror(errno));
		}
		ast_debug(7, "A null frame has been added");
	}
	res = write(o->sounddev, ((void *) data), FRAME_SIZE * 2 * 2 * 6);
	if (res < 0) {
		o->plus_sound_short_writes++;
		ast_log(LOG_ERROR, "Channel %s: Sound card write error %s\n", o->name, strerror(errno));
	} else if (res != FRAME_SIZE * 2 * 2 * 6) {
		o->plus_sound_short_writes++;
		ast_log(LOG_ERROR, "Channel %s: Sound card wrote %d bytes of %d\n", o->name, res, (FRAME_SIZE * 2 * 2 * 6));
	}

	return res;
}

/*!
 * \brief Open the sound card device.
 * If the device is already open, this will close the device
 * and open it again.
 * It initializes the device based on our requirements and triggers
 * reads and writes.
 * \param o		Channel private data.
 * \param mode	The mode to open the file.  This is the flags argument to open.
 * \retval 0	Success.
 * \retval -1	Failed.
 */
static int setformat(struct chan_usbradio_pvt *o, int mode)
{
	int fmt, desired, res, fd;
	char device[100];

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

	strcpy(device, "/dev/dsp");
	if (o->devicenum) {
		sprintf(device, "/dev/dsp%d", o->devicenum);
	}
	/* open the device */
	fd = o->sounddev = open(device, mode | O_NONBLOCK);
	if (fd < 0) {
		ast_log(LOG_ERROR, "Channel %s: Unable to open DSP device %d: %s.\n", o->name, o->devicenum, strerror(errno));
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
		ast_log(LOG_WARNING, "Channel %s: Unable to set format to 16-bit signed\n", o->name);
		return -1;
	}
	/* set our duplex mode based on the way we opened the device. */
	switch (mode) {
	case O_RDWR:
		res = ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);
		/* Check to see if duplex set (FreeBSD Bug) */
		res = ioctl(fd, SNDCTL_DSP_GETCAPS, &fmt);
		if (res == 0 && (fmt & DSP_CAP_DUPLEX)) {
			o->duplex = M_FULL;
		};
		break;
	case O_WRONLY:
		o->duplex = M_WRITE;
		break;
	case O_RDONLY:
		o->duplex = M_READ;
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
		ast_log(LOG_WARNING, "Channel %s: Failed to set audio device sample rate.\n", o->name);
		return -1;
	}
	if (fmt != desired) {
		if (!(o->warned & WARN_speed)) {
			ast_log(LOG_WARNING, "Channel %s: Requested %d Hz, got %d Hz -- sound may be choppy.\n", o->name, desired, fmt);
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
				ast_log(LOG_WARNING, "Channel %s: Unable to set fragment size -- sound may be choppy.\n", o->name);
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

/*!
 * \brief Asterisk digit begin function.
 * \param c				Asterisk channel.
 * \param digit			Digit processed.
 * \retval 0
 */
static int usbradio_digit_begin(struct ast_channel *c, char digit)
{
	(void) c;
	(void) digit;
	return 0;
}

/*!
 * \brief Asterisk digit end function.
 * \param c				Asterisk channel.
 * \param digit			Digit processed.
 * \param duration		Duration of the digit.
 * \retval -1
 */
static int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration)
{
	(void) c;
	/* no better use for received digits than print them */
	ast_verbose(" << Console Received digit %c of duration %u ms >> \n", digit, duration);
	return 0;
}

/*!
 * \brief Asterisk text function.
 * \note SETFREQ - sets spi programmable transceiver
 *  	 SETCHAN - sets binary parallel transceiver
 * \param c				Asterisk channel.
 * \param text			Text message to process.
 * \retval 0			If successful.
 * \retval -1			If unsuccessful.
 */
static int usbradio_text(struct ast_channel *c, const char *text)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	char *cmd, pwr;
	int cnt, i, j;
	double tx, rx;
#define STR_SZ 15 /* Size of text strings */
	char rxs[STR_SZ + 1], txs[STR_SZ + 1], txpl[STR_SZ + 1], rxpl[STR_SZ + 1];

#ifdef HAVE_SYS_IO
	if (haspp == 2) {
		ioperm(pbase, 2, 1);
	}
#endif

	cmd = ast_alloca(strlen(text) + 10);

	/* print received messages */
	ast_debug(3, "Channel %s: Console Received usbradio text %s >>\n", o->name, text);

	cnt = sscanf(text, "%s " S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) "%c", cmd, rxs, txs, rxpl, txpl, &pwr);

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

	/* set transmit CTCSS */
	if (strcmp(cmd, "TXCTCSS") == 0) {
		u8 x;
		x = strtod(rxs, NULL);
		if (o && o->radio) {
			o->radio->b.txCtcssOff = !x;
		}
		ast_debug(3, "Channel %s: TXCTCSS cmd: %s\n", o->name, text);
		return 0;
	}

	/* GPIO command */
	if (!strncmp(text, "GPIO", 4)) {
		cnt = sscanf(text, "%s " N_FMT(d) " " N_FMT(d), cmd, &i, &j);
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
		cnt = sscanf(text, "%s " N_FMT(d) " " N_FMT(d), cmd, &i, &j);
		if (cnt < 3) {
			return 0;
		}
		if ((i < 2) || (i > 9)) {
			return 0;
		}
		/* skip if not valid */
		if (!(PP_MASK & (1 << i))) {
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
		ast_debug(3, "Channel %s: << %s %s %s %s %s %c >> \n", o->name, cmd, rxs, txs, rxpl, txpl, pwr);
	}

	/* set frequency command */
	if (strcmp(cmd, "SETFREQ") == 0) {
		ast_debug(3, "Channel %s: SETFREQ cmd: %s\n", o->name, text);
		tx = strtod(txs, NULL);
		rx = strtod(rxs, NULL);
		o->set_txfreq = round(tx * (double) 1000000);
		o->set_rxfreq = round(rx * (double) 1000000);
		o->set_txpower = (pwr == 'H');
		strcpy(o->set_rxctcssfreqs, rxpl); /* Safe */
		strcpy(o->set_txctcssfreqs, txpl); /* Safe */

		o->remoted = 1;
		radio_config(o);
		return 0;
	}
	ast_log(LOG_ERROR, "Channel %s: Cannot parse usbradio cmd: %s\n", o->name, text);
	return 0;
}

/*!
 * \brief USBRadio call.
 * \param c				Asterisk channel.
 * \param dest			Destination.
 * \param timeout		Timeout.
 * \retval -1 			if not successful.
 * \retval 0 			if successful.
 */
static int usbradio_call(struct ast_channel *c, const char *dest, int timeout)
{
	(void) dest;
	(void) timeout;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	o->stophid = 0;
	ast_radio_time(&o->lasthidtime);
	ast_pthread_create(&o->hidthread, NULL, hidthread, o);
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

/*!
 * \brief Answer the call.
 * \param c				Asterisk channel.
 * \retval 0 			If successful.
 */
static int usbradio_answer(struct ast_channel *c)
{
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

/*!
 * \brief Asterisk hangup function.
 * \param c			Asterisk channel.
 * \retval 0		Always returns 0.
 */
static int usbradio_hangup(struct ast_channel *c)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	ast_channel_tech_pvt_set(c, NULL);
	o->owner = NULL;
	ast_module_unref(ast_module_info->self);
	if (o->hookstate) {
		o->hookstate = 0;
		setformat(o, O_CLOSE);
	}
	o->stophid = 1;
	pthread_join(o->hidthread, NULL);
	return 0;
}

/*!
 * \brief Asterisk write function.
 * This routine handles asterisk to radio frames.
 * \param ast			Asterisk channel.
 * \param frame			Asterisk frame to process.
 * \retval 0			Successful.
 */
static void usbradioplus_queue_program(struct chan_usbradio_pvt *o,
	const short *samples, size_t count)
{
	if (count > o->plus_app_rpt_samples) count = o->plus_app_rpt_samples;
	ast_mutex_lock(&o->plus_link_lock);
	if (o->plus_link_queue_count == PLUS_LINK_QUEUE_FRAMES) {
		o->plus_link_queue_head = (o->plus_link_queue_head + 1)
			% PLUS_LINK_QUEUE_FRAMES;
		o->plus_link_queue_count--;
		o->plus_link_queue_overflows++;
	}
	memset(o->plus_link_queue[o->plus_link_queue_tail], 0,
		sizeof(o->plus_link_queue[0]));
	memcpy(o->plus_link_queue[o->plus_link_queue_tail], samples,
		count * sizeof(*samples));
	o->plus_link_queue_tail = (o->plus_link_queue_tail + 1)
		% PLUS_LINK_QUEUE_FRAMES;
	o->plus_link_queue_count++;
	if (o->plus_link_queue_count > o->plus_link_queue_high_water)
		o->plus_link_queue_high_water = o->plus_link_queue_count;
	ast_mutex_unlock(&o->plus_link_lock);
}

static int usbradio_write(struct ast_channel *c, struct ast_frame *f)
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

#if DEBUG_CAPTURES == 1
	/* Write input data to a file.
	 * Left channel has the audio, right channel shows txkeyed
	 */
	if (ftxcapraw && o->txcapraw) {
		short i, tbuff[f->datalen];
		for (i = 0; i < f->datalen; i += 2) {
			tbuff[i] = ((short *) (f->data.ptr))[i / 2];
			tbuff[i + 1] = o->txkeyed * M_Q13;
		}
		fwrite(tbuff, 2, f->datalen, ftxcapraw);
	}
#endif

	/* The signaling engine does not render audio. Preserve app_rpt's program frame for the
	 * native-rate transmitter graph in the next CM119 hardware tick. */
	if (!o->echoing) {
		usbradioplus_queue_program(o, f->data.ptr,
			f->datalen / sizeof(short));
	}

	return 0;
}

/*!
 * \brief Asterisk read function.
 * \param ast			Asterisk channel.
 * \retval 				Asterisk frame.
 */
static struct ast_frame *usbradio_read(struct ast_channel *c)
{
	int res, oldpttout;
	int cd, sd;
	int was_rxkeyed;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);
	struct ast_frame *f = &o->read_f, *f1;
	time_t now;

	/* check to the if the hid thread is still processing */
	if (o->lasthidtime) {
		ast_radio_time(&now);
		if ((now - o->lasthidtime) > 3) {
			ast_log(LOG_ERROR, "Channel %s: HID process has died or is not responding.\n", o->name);
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
			if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
				ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 0, 0);
			}
		}
		return &ast_null_frame;
	}

	/* If we have stopped echoing, clear the echo queue */
	if (!o->echomode) {
		struct qelem *q;

		ast_mutex_lock(&o->echolock);
		o->echoing = 0;
		while (o->echoq.q_forw != &o->echoq) {
			q = o->echoq.q_forw;
			remque(q);
			ast_free(q);
		}
		ast_mutex_unlock(&o->echolock);
	}

	/* If we are in echomode and we have stopped receiving audio
	 * queue up the packets we have stored in the echo queue
	 * for playback.
	 */
	if (o->echomode && (!o->rxkeyed)) {
		struct usbecho *u;

		ast_mutex_lock(&o->echolock);
		/* if there is something in the queue */
		if (o->echoq.q_forw != &o->echoq) {
			u = (struct usbecho *) o->echoq.q_forw;
			remque((struct qelem *) u);
			usbradioplus_queue_program(o, u->data, FRAME_SIZE);
			ast_free(u);
			o->echoing = 1;
		} else {
			o->echoing = 0;
		}
		ast_mutex_unlock(&o->echolock);
	}

	/* Read audio data from the USB sound device.
	 * Sound data will arrive at 48000 samples per second
	 * in stereo format.
	 */
	res = read(o->sounddev, o->usbradio_read_buf + o->readpos, sizeof(o->usbradio_read_buf) - o->readpos);
	if (res < 0) { /* Audio data not ready, return a NULL frame */
		if (errno != EAGAIN) {
			o->readerrs = 0;
			o->hasusb = 0;
			return &ast_null_frame;
		}
		if (o->readerrs++ > READERR_THRESHOLD) {
			ast_log(LOG_ERROR, "Stuck USB read channel [%s], un-sticking it!\n", o->name);
			o->readerrs = 0;
			o->hasusb = 0;
			return &ast_null_frame;
		}
		if (o->readerrs == 1) {
			ast_log(LOG_WARNING, "Possibly stuck USB read channel. [%s]\n", o->name);
		}
		return &ast_null_frame;
	}

#if DEBUG_CAPTURES == 1
	if (o->rxcapraw && frxcapraw) {
		fwrite(o->usbradio_read_buf + o->readpos, 1, res, frxcapraw);
	}
#endif

	if (o->readerrs) {
		ast_log(LOG_WARNING, "USB read channel [%s] was not stuck.\n", o->name);
	}

	o->readerrs = 0;
	o->readpos += res;
	if ((size_t) o->readpos < sizeof(o->usbradio_read_buf)) { /* not enough samples */
		return &ast_null_frame;
	}

	/* Check for ADC clipping and input audio statistics before any filtering is done.
	 * FRAME_SIZE define refers to 8Ksps mono which is 160 samples per 20mS USB frame.
	 * ast_radio_check_audio() takes the read buffer as received (48K stereo),
	 * extracts the mono 48K channel, checks amplitude and distortion characteristics,
	 * and returns true if clipping was detected.
	 */
	if (ast_radio_check_audio((short *) o->usbradio_read_buf, &o->rxaudiostats, 12 * FRAME_SIZE)) {
		if (o->clipledgpio) {
			/* Set Clip LED GPIO pulsetimer if not already set */
			if (!o->hid_gpio_pulsetimer[o->clipledgpio - 1]) {
				o->hid_gpio_pulsetimer[o->clipledgpio - 1] = CLIP_LED_HOLD_TIME_MS;
			}
		}
	}

	/* Below is an attempt to match levels to the original CM108 IC which has been
	 * out of production for over 10 years. Scaling all rx audio to 80% results in a 20%
	 * loss in dynamic range, added quantization noise, a 2dB reduction in outgoing IAX
	 * audio levels, and inconsistency with Simpleusb. Adjustments for CM1xxx IC gain
	 * differences should be made in the mixer settings, not in the audio stream.
	 * TODO: After the vast majority of existing installs have had a chance to review their
	 * audio settings and these old scaling/clipping hacks are no longer in significant use
	 * the legacyaudioscaling cfg and related code should be deleted.
	 */
	/* Decrease the audio level for CM119 A/B devices */
	if (o->legacyaudioscaling && o->devtype != C108_PRODUCT_ID) {
		/* Subtract res from o->readpos in below assignment (o->readpos was incremented
		   above prior to check of if enough samples were received) */
		register short *sp = (short *) (o->usbradio_read_buf + (o->readpos - res));
		register float v;
		register int i;

		for (i = 0; i < res / 2; i++) {
			v = ((float) *sp) * 0.800;
			*sp++ = (int) v;
		}
	}

	was_rxkeyed = o->rxkeyed;
	if (o->txkeyed || o->txtestkey || o->echoing || o->plus_parrot_playing
		|| o->rxkeyed) {
		if (!o->radio->txPttIn) {
			o->radio->txPttIn = 1;
			ast_debug(3, "Channel %s: txPttIn = %i.\n", o->name, o->radio->txPttIn);
		}
	} else if (o->radio->txPttIn) {
		o->radio->txPttIn = 0;
		ast_debug(3, "Channel %s: txPttIn = %i.\n", o->name, o->radio->txPttIn);
	}
	oldpttout = o->radio->txPttOut;

	usbradioplus_prepare_squelch_audio(o);
	urp_radio_process(o->radio, (i16 *) o->plus_squelch_native,
		(i16 *) (o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET), (i16 *) (o->usbradio_write_buf));
	usbradioplus_native_tick(o);

	if (oldpttout != o->radio->txPttOut) {
		usbradioplus_program_radio(o);
		ast_debug(3, "Channel %s: txPttOut = %i.\n", o->name, o->radio->txPttOut);
		kickptt(o);
	}

#if DEBUG_CAPTURES == 1 && URP_RADIO_DEBUG == 1
	if (o->txcap2 && ftxcaptrace) {
		fwrite((o->radio->ptxDebug), 1, FRAME_SIZE * 2 * 16, ftxcaptrace);
	}
#endif

	/* Below is an attempt to match levels to the original CM108 IC which has been
	 * out of production for over 10 years. Scaling audio to 110% will result in clipping!
	 * Any adjustments for CM1xxx IC gain differences should be made in the mixer
	 * settings, not in the audio stream.
	 * TODO: After the vast majority of existing installs have had a chance to review their
	 * audio settings and these old scaling/clipping hacks are no longer in significant use
	 * the legacyaudioscaling cfg and related code should be deleted.
	 */
	/* For the CM108 adjust the audio level */
	if (o->legacyaudioscaling && o->devtype != C108_PRODUCT_ID) {
		register short *sp = (short *) o->usbradio_write_buf;
		register float v;
		register int i;

		for (i = 0; i < (int) (sizeof(o->usbradio_write_buf) / 2); i++) {
			v = ((float) *sp) * 1.10;
			if (v > 32765.0) {
				v = 32765.0;
			} else if (v < -32765.0) {
				v = -32765.0;
			}
			*sp++ = (int) v;
		}
	}

	/* Write the received audio to the sound card */
	soundcard_writeframe(o, (short *) o->usbradio_write_buf);

#if DEBUG_CAPTURES == 1 && URP_RADIO_DEBUG == 1
	if (frxcaptrace && o->rxcap2 && o->radioactive) {
		fwrite((o->radio->prxDebug), 1, FRAME_SIZE * 2 * 16, frxcaptrace);
	}
#endif

	/* Check for carrier detect - COR active */
	cd = 0;
	if (o->rxcdtype == CD_HID && (o->radio->rxExtCarrierDetect != o->rxhidsq)) {
		o->radio->rxExtCarrierDetect = o->rxhidsq;
	}

	if (o->rxcdtype == CD_HID_INVERT && (o->radio->rxExtCarrierDetect == o->rxhidsq)) {
		o->radio->rxExtCarrierDetect = !o->rxhidsq;
	}

	if ((o->rxcdtype == CD_HID && o->rxhidsq) || (o->rxcdtype == CD_HID_INVERT && !o->rxhidsq) ||
		(o->rxcdtype == CD_XPMR_NOISE && o->radio->rxCarrierDetect) || (o->rxcdtype == CD_PP && o->rxppsq) ||
		(o->rxcdtype == CD_PP_INVERT && !o->rxppsq) || (o->rxcdtype == CD_XPMR_VOX && o->radio->rxCarrierDetect)) {
		if (!o->radio->txPttOut || o->radioduplex) {
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

	if (o->radio->b.ctcssRxEnable && o->radio->rxCtcss->decode != o->rxctcssdecode) {
		ast_debug(3, "Channel %s: rxctcssdecode = %i.\n", o->name, o->radio->rxCtcss->decode);
		o->rxctcssdecode = o->radio->rxCtcss->decode;
		strcpy(o->rxctcssfreq, o->radio->rxctcssfreq);
	}

	/* Check for SD - CTCSS active. */
	if (!o->radio->b.ctcssRxEnable ||
		(o->radio->b.ctcssRxEnable && o->radio->rxCtcss->decode > CTCSS_NULL && o->radio->smode == SMODE_CTCSS)) {
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
	if (o->rxcdtype == CD_IGNORE && o->rxsdtype == SD_IGNORE) {
		cd = 0;
		sd = 0;
	}

	/* Timer for how long TX has been unkeyed - used with txoffdelay */
	if (o->txoffdelay) {
		if (o->txkeyed == 1) {
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
		if (o->rxkeyed || ((o->txoffcnt >= o->txoffdelay) && (o->rxoncnt >= o->rxondelay))) {
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
	if (o->plus_parrot_enabled) {
		usbradioplus_parrot_rx_transition(o, was_rxkeyed);
	}

	/* If we are in echomode and receiving audio, store
	 * it in the echo queue for later playback.
	 */
	if (o->echomode && o->rxkeyed && (!o->echoing)) {
		register int x;
		struct usbecho *u;

		ast_mutex_lock(&o->echolock);
		x = 0;
		/* get count of frames */
		for (u = (struct usbecho *) o->echoq.q_forw; u != (struct usbecho *) &o->echoq; u = (struct usbecho *) u->q_forw)
			x++;
		if (x < o->echomax) {
			u = ast_calloc(1, sizeof(struct usbecho));
			if (u) {
				memcpy(u->data, (o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET), FRAME_SIZE * 2);
				insque((struct qelem *) u, o->echoq.q_back);
			}
		}
		ast_mutex_unlock(&o->echolock);
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
		if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
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
		if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
			ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 1, 0);
		}
	}

	/* reset read pointer for next frame */
	o->readpos = AST_FRIENDLY_OFFSET;
	/* Do not return the frame if the channel is not up */
	if (ast_channel_state(c) != AST_STATE_UP) {
		return &ast_null_frame;
	}
	/* ok we can build and deliver the frame to the caller */
	f->frametype = AST_FRAME_VOICE;
	f->subclass.format = ast_format_slin;
	f->offset = AST_FRIENDLY_OFFSET;
	f->samples = o->plus_app_rpt_samples;
	f->datalen = f->samples * sizeof(short);
	f->data.ptr = o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET;
	f->src = __PRETTY_FUNCTION__;
	if (!o->rxkeyed) {
		memset(f->data.ptr, 0, f->datalen);
	}
	/* Process the audio to see if contains DTMF */
	if (o->usedtmf && o->dsp) {
		f1 = ast_dsp_process(c, o->dsp, f);
		if ((f1->frametype == AST_FRAME_DTMF_END) || (f1->frametype == AST_FRAME_DTMF_BEGIN)) {
			if ((f1->subclass.integer == 'm') || (f1->subclass.integer == 'u')) {
				f1->frametype = AST_FRAME_NULL;
				f1->subclass.integer = 0;
				return f1;
			}
			if (f1->frametype == AST_FRAME_DTMF_END) {
				f1->len = ast_tvdiff_ms(ast_radio_tvnow(), o->tonetime);
				if (option_verbose) {
					ast_log(LOG_NOTICE, "Channel %s: Got DTMF char %c duration %ld ms\n", o->name, f1->subclass.integer, f1->len);
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

		ast_debug(3, "Channel %s: got b.txCtcssReady %s.\n", o->name, o->radio->txctcssfreq);
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

			snprintf(msg, sizeof(msg), "R %i", ((32767 - o->radio->rxRssi) * 1000) / 32767);
			wf.data.ptr = msg;
			wf.datalen = strlen(msg) + 1;
			ast_queue_frame(o->owner, &wf);

			o->count_rssi_update = 10;
			ast_debug(4, "Channel %s: Count_rssi_update %i\n", o->name, ((32767 - o->radio->rxRssi) * 1000 / 32767));
		}
	}

	return f;
}

/*!
 * \brief Asterisk fixup function.
 * \param oldchan		Old asterisk channel.
 * \param newchan		New asterisk channel.
 * \retval 0			Always returns 0.
 */
static int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	(void) oldchan;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(newchan);
	ast_log(LOG_WARNING, "Channel %s: Fixup received.\n", o->name);
	o->owner = newchan;
	return 0;
}

/*!
 * \brief Asterisk indicate function.
 * This is used to indicate tx key / unkey.
 * \param c				Asterisk channel.
 * \param cond			Condition.
 * \param data			Data.
 * \param datalen		Data length.
 * \retval 0			If successful.
 * \retval -1			For hangup.
 */
static int usbradio_indicate(struct ast_channel *c, int cond_in, const void *data, size_t datalen)
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
		ast_debug(1, "Channel %s: ACRK code=%s TX ON.\n", o->name, (char *) data);
		if (datalen && ((char *) (data))[0] != '0') {
			o->forcetxcode = 1;
			memset(o->set_txctcssfreq, 0, sizeof(o->set_txctcssfreq)); /* Possibly unnecessary, if this is used as a string? */
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
		ast_log(LOG_WARNING, "Channel %s: Don't know how to display condition %d.\n", o->name, cond);
		return -1;
	}

	return 0;
}

/*!
 * \brief Asterisk setoption function.
 * \param chan			Asterisk channel.
 * \param option		Option.
 * \param data			Data.
 * \param datalen		Data length.
 * \retval 0			If successful.
 * \retval -1			If failed.
 */
static int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen)
{
	char *cp;
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
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: OFF(0).\n", o->name);
			o->usedtmf = 1;
			break;
		case 2:
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: MUTECONF/MAX(2).\n", o->name);
			o->usedtmf = 1;
			break;
		case 3:
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: DISABLE DETECT(3).\n", o->name);
			o->usedtmf = 0;
			break;
		default:
			ast_log(LOG_NOTICE, "Channel %s: Set option TONE VERIFY, mode: OFF(0).\n", o->name);
			o->usedtmf = 1;
			break;
		}
		break;
	}
	errno = 0;
	return 0;
}

/*!
 * \brief Start a new usbradio call.
 * \param o				Private structure.
 * \param ext			Extension.
 * \param ctx			Context.
 * \param state			State.
 * \param assignedids	Unique ID string assigned to the channel.
 * \param requestor		Asterisk channel.
 * \return 				Asterisk channel.
 */
static struct ast_channel *usbradio_new(struct chan_usbradio_pvt *o, char *ext, char *ctx, int state,
	const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor)
{
	struct ast_channel *c;

	c = ast_channel_alloc(1, state, NULL, NULL, "", ext, ctx, assignedids, requestor, 0, "RadioPlus/%s", o->name);
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
			ast_log(LOG_WARNING, "Channel %s: Unable to start PBX.\n", ast_channel_name(c));
			ast_hangup(c);
			o->owner = c = NULL;
			/* XXX what about the channel itself ? */
		}
	}

	return c;
}

/*!
 * \brief USBRadio request from Asterisk.
 * This is a standard Asterisk function - requester.
 * Asterisk calls this function to to setup private data structures.
 * \param type			Type of channel to request.
 * \param cap			Format capabilities for the channel.
 * \param assignedids	Unique ID string to assign to the channel.
 * \param requestor		Channel asking for data.
 * \param data			Destination of the call.
 * \param cause			Cause of failure.
 * \retval NULL			Failure
 * \return				ast_channel if successful
 */
static struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
	const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause)
{
	(void) type;
	struct ast_channel *c;
	struct chan_usbradio_pvt *o = find_desc(data);

	if (!o) {
		ast_log(LOG_WARNING, "Device %s not found.\n", (char *) data);
		return NULL;
	}

	if (!(ast_format_cap_iscompatible(cap, usbradio_tech.capabilities))) {
		struct ast_str *cap_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);
		ast_log(LOG_NOTICE, "Channel %s: Channel requested with unsupported format(s): '%s'\n", o->name,
			ast_format_cap_get_names(cap, &cap_buf));
		return NULL;
	}

	if (o->owner) {
		ast_log(LOG_NOTICE, "Channel %s: Already have a call (chan %p) on the usb channel\n", o->name, o->owner);
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}
	c = usbradio_new(o, NULL, NULL, AST_STATE_DOWN, assignedids, requestor);
	if (!c) {
		ast_log(LOG_ERROR, "Channel %s: Unable to create new usb channel\n", o->name);
		return NULL;
	}

	o->remoted = 0;
	radio_config(o);

	return c;
}

/*!
 * \brief Process Asterisk CLI request to key radio.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */
static int console_key(int fd, int argc, const char *const *argv)
{
	(void) fd;
	(void) argv;
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);

	if (argc != 2) {
		return RESULT_SHOWUSAGE;
	}
	o->txtestkey = 1;
	kickptt(o);
	return RESULT_SUCCESS;
}

/*!
 * \brief Process Asterisk CLI request to unkey radio.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */
static int console_unkey(int fd, int argc, const char *const *argv)
{
	(void) fd;
	(void) argv;
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);

	if (argc != 2) {
		return RESULT_SHOWUSAGE;
	}
	o->txtestkey = 0;
	kickptt(o);
	return RESULT_SUCCESS;
}

/*!
 * \brief Process Asterisk CLI request to show or set active USB device.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	Cli success, showusage, or failure.
 */
static int radio_active(int fd, int argc, const char *const *argv)
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
				ast_cli(fd, "Device [%s] exists as device=%s card=%d\n", o->name, o->devstr, ast_radio_usb_get_usbdev(o->devstr));
			}
			ast_mutex_unlock(&usb_dev_lock);
			return RESULT_SUCCESS;
		}
		o = find_desc(argv[2]);
		if (!o) {
			ast_cli(fd, "No device [%s] exists\n", argv[2]);
		} else {
			struct chan_usbradio_pvt *ao;
			for (ao = usbradio_default.next; ao && ao->name; ao = ao->next) {
				ao->radioactive = 0;
			}
			usbradio_active = o->name;
			o->radioactive = 1;
			ast_cli(fd, "Active (command) USB Radio device set to [%s]\n", usbradio_active);
		}
	}
	return RESULT_SUCCESS;
}

/*!
 * \brief Process Asterisk CLI request to swap usb devices
 * \param fd			Asterisk CLI fd
 * \param other			Other device.
 * \return	Cli success, showusage, or failure.
 */
static int usb_device_swap(int fd, const char *other)
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
	strcpy(tmp, p->devstr);
	d = p->devicenum;
	strcpy(p->devstr, o->devstr);
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

/*!
 * \brief Send 3 second test tone.
 * \param fd			Asterisk cli fd
 * \param o				Private struct.
 * \param intflag		Flag to indicate the type of wait.
 */
static void tune_flash(int fd, struct chan_usbradio_pvt *o, int intflag)
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
		if (i == (NFLASH - 1)) {
			break;
		}
		if ((fd > 0) && intflag) {
			if (ast_radio_wait_or_poll(fd, 1500, intflag)) {
				o->radio->txPttIn = 0;
				o->txtestkey = 0;
				break;
			}
		} else {
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

/*!
 * \brief Process asterisk CLI request radio tune.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */
static int radio_tune(int fd, int argc, const char *const *argv)
{
	struct chan_usbradio_pvt *o = find_desc(usbradio_active);
	int i = 0;

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
			ast_cli(fd, "Current Signal Strength is %d\n", ((32767 - o->radio->rxRssi) * 1000 / 32767));
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

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) && (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		} else if (argc == 3) {
			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				ast_cli(fd, "Current txvoice setting on Channel A is %d\n", o->txmixaset);
			} else {
				ast_cli(fd, "Current txvoice setting on Channel B is %d\n", o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}

			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				o->txmixaset = i;
				ast_cli(fd, "Changed txvoice setting on Channel A to %d\n", o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed txvoice setting on Channel B to %d\n", o->txmixbset);
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

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) && (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		} else if (argc == 3) {
			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				ast_cli(fd, "Current txvoice setting on Channel A is %d\n", o->txmixaset);
			} else {
				ast_cli(fd, "Current txvoice setting on Channel B is %d\n", o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}

			if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
				o->txmixaset = i;
				ast_cli(fd, "Changed txvoice setting on Channel A to %d\n", o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed txvoice setting on Channel B to %d\n", o->txmixbset);
			}
			mixer_write(o);
			mult_set(o);
			ast_cli(fd, "Changed Tx Voice Output setting to %d\n", i);
		}
		tune_txoutput(o, i, fd, 0);
	} else if (!strcasecmp(argv[2], "auxvoice")) {
		i = 0;
		if ((o->txmixa != TX_OUT_AUX) && (o->txmixb != TX_OUT_AUX)) {
			ast_log(LOG_WARNING, "No auxvoice output configured.\n");
		} else if (argc == 3) {
			if (o->txmixa == TX_OUT_AUX) {
				ast_cli(fd, "Current auxvoice setting on Channel A is %d\n", o->txmixaset);
			} else {
				ast_cli(fd, "Current auxvoice setting on Channel B is %d\n", o->txmixbset);
			}
		} else {
			i = atoi(argv[3]);
			if ((i < 0) || (i > 999)) {
				return RESULT_SHOWUSAGE;
			}
			if (o->txmixa == TX_OUT_AUX) {
				o->txmixaset = i;
				ast_cli(fd, "Changed auxvoice setting on Channel A to %d\n", o->txmixaset);
			} else {
				o->txmixbset = i;
				ast_cli(fd, "Changed auxvoice setting on Channel B to %d\n", o->txmixbset);
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
		ast_cli(fd, "File capture (trace) was rx=%d tx=%d and now off.\n", o->rxcap2, o->txcap2);
		ast_cli(fd, "File capture (raw)   was rx=%d tx=%d and now off.\n", o->rxcapraw, o->txcapraw);
		o->rxcapraw = o->txcapraw = o->rxcap2 = o->txcap2 = o->radio->b.rxCapture = o->radio->b.txCapture = 0;
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
		ast_cli(fd, "Saved radio tuning settings to usbradioplus.conf\n");
	} else if (!strcasecmp(argv[2], "load")) {
		ast_mutex_lock(&o->eepromlock);
		while (o->eepromctl) {
			ast_mutex_unlock(&o->eepromlock);
			usleep(10000);
			ast_mutex_lock(&o->eepromlock);
		}
		o->eepromctl = 1; /* request a load */
		ast_mutex_unlock(&o->eepromlock);

		ast_cli(fd, "Requesting loading of tuning settings from EEPROM for channel %s\n", o->name);
	} else if (!strcasecmp(argv[2], "txslimsp")) {
		if (argc == 3) {
			ast_cli(fd, "Current tx limiter setpoint: %i\n", (int) o->txslimsp);
		} else {
			int new_slsetpoint = atoi(argv[3]);
			if (legacy_set_tx_soft_limiter(o, new_slsetpoint)) {
				ast_cli(fd, "Limiter set point out of range, needs to be between 5000 and 13000\n");
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

/*!
 * \brief Set transmit CTCSS modulation level.
 *	Set the transmit CTCSS modulation level.  Adjust the mixer output or
 *	internal gain depending on the output type.
 *	Setting ranges is 0.0 to 0.9.
 *
 * \param o				chan_usbradio structure.
 * \return	0			Always returns zero.
 */
static int set_txctcss_level(struct chan_usbradio_pvt *o)
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

/*!
 * \brief Set transmit soft limiting threshold.
 * Validate the legacy soft-limiter setpoint used by the native fallback.
 *
 *
 * \param o				chan_usbradio structure.
 * \param setpoint      A value which indicates the onset of soft limiting.
 * \return			    zero if successful, -1 if otherwise
 */

static int legacy_set_tx_soft_limiter(struct chan_usbradio_pvt *o, int setpoint)
{
	(void) o;
	return setpoint < 5000 || setpoint > 13000 ? -1 : 0;
}

/*!
 * \brief Process Asterisk CLI request to set detector debug level.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */
static int radio_set_dsp_debug(int fd, int argc, const char *const *argv)
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

/*!
 * \brief Store receive demodulator setting.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_rxdemod(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no")) {
		o->rxdemod = RX_AUDIO_NONE;
	} else if (!strcasecmp(s, "speaker")) {
		o->rxdemod = RX_AUDIO_SPEAKER;
	} else if (!strcasecmp(s, "flat")) {
		o->rxdemod = RX_AUDIO_FLAT;
	} else {
		ast_log(LOG_WARNING, "Unrecognized rxdemod parameter: %s\n", s);
	}
}

/*!
 * \brief Store tx mixer A setting.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_txmixa(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no")) {
		o->txmixa = TX_OUT_OFF;
	} else if (!strcasecmp(s, "voice")) {
		o->txmixa = TX_OUT_VOICE;
	} else if (!strcasecmp(s, "tone")) {
		o->txmixa = TX_OUT_LSD;
	} else if (!strcasecmp(s, "composite")) {
		o->txmixa = TX_OUT_COMPOSITE;
	} else if (!strcasecmp(s, "auxvoice")) {
		o->txmixa = TX_OUT_AUX;
	} else {
		ast_log(LOG_WARNING, "Unrecognized txmixa parameter: %s\n", s);
	}
}

/*!
 * \brief Store tx mixer B setting.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_txmixb(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no")) {
		o->txmixb = TX_OUT_OFF;
	} else if (!strcasecmp(s, "voice")) {
		o->txmixb = TX_OUT_VOICE;
	} else if (!strcasecmp(s, "tone")) {
		o->txmixb = TX_OUT_LSD;
	} else if (!strcasecmp(s, "composite")) {
		o->txmixb = TX_OUT_COMPOSITE;
	} else if (!strcasecmp(s, "auxvoice")) {
		o->txmixb = TX_OUT_AUX;
	} else {
		ast_log(LOG_WARNING, "Unrecognized txmixb parameter: %s\n", s);
	}
}

/*!
 * \brief Store receive carrier detect type.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_rxcdtype(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no")) {
		o->rxcdtype = CD_IGNORE;
	} else if (!strcasecmp(s, "usb")) {
		o->rxcdtype = CD_HID;
	} else if (!strcasecmp(s, "dsp")) {
		o->rxcdtype = CD_XPMR_NOISE;
	} else if (!strcasecmp(s, "vox")) {
		o->rxcdtype = CD_XPMR_VOX;
	} else if (!strcasecmp(s, "usbinvert")) {
		o->rxcdtype = CD_HID_INVERT;
	} else if (!strcasecmp(s, "pp")) {
		o->rxcdtype = CD_PP;
	} else if (!strcasecmp(s, "ppinvert")) {
		o->rxcdtype = CD_PP_INVERT;
	} else {
		ast_log(LOG_WARNING, "Unrecognized rxcdtype parameter: %s\n", s);
	}
}

static void store_rxsdtype(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no") || !strcasecmp(s, "SD_IGNORE")) {
		o->rxsdtype = SD_IGNORE;
	} else if (!strcasecmp(s, "usb") || !strcasecmp(s, "SD_HID")) {
		o->rxsdtype = SD_HID;
	} else if (!strcasecmp(s, "usbinvert") || !strcasecmp(s, "SD_HID_INVERT")) {
		o->rxsdtype = SD_HID_INVERT;
	} else if (!strcasecmp(s, "dsp") || !strcasecmp(s, "SD_XPMR")) {
		o->rxsdtype = SD_XPMR;
	} else if (!strcasecmp(s, "pp")) {
		o->rxsdtype = SD_PP;
	} else if (!strcasecmp(s, "ppinvert")) {
		o->rxsdtype = SD_PP_INVERT;
	} else {
		ast_log(LOG_WARNING, "Unrecognized rxsdtype parameter: %s\n", s);
	}
}

/*!
 * \brief Store receiver gain setting.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_rxgain(struct chan_usbradio_pvt *o, const char *s)
{
	float f;
	sscanf(s, N_FMT(f), &f);
	o->rxgain = f;
}

/*!
 * \brief Store receive voice adjustment.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_rxvoiceadj(struct chan_usbradio_pvt *o, const char *s)
{
	float f;
	sscanf(s, N_FMT(f), &f);
	o->rxvoiceadj = f;
}

/*!
 * \brief Store transmit output tone turn off type.
 * \param o				Private struct.
 * \param s				New setting.
 */
static void store_txtoctype(struct chan_usbradio_pvt *o, const char *s)
{
	if (!strcasecmp(s, "no") || !strcasecmp(s, "TOC_NONE")) {
		o->txtoctype = TOC_NONE;
	} else if (!strcasecmp(s, "phase") || !strcasecmp(s, "TOC_PHASE")) {
		o->txtoctype = TOC_PHASE;
	} else if (!strcasecmp(s, "notone") || !strcasecmp(s, "TOC_NOTONE")) {
		o->txtoctype = TOC_NOTONE;
	} else {
		ast_log(LOG_WARNING, "Unrecognized txtoctype parameter: %s\n", s);
	}
}

/*!
 * \brief Send test tone.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 * \param intflag		Flag to indicate the type of wait.
 */
static void tune_txoutput(struct chan_usbradio_pvt *o, int value, int fd, int intflag)
{
	(void) value;
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

/*!
 * \brief Adjust input attenuator with maximum signal input.
 *
 * \param fd			Asterisk CLI fd
 * \param o				chan_usbradio structure.
 * \param setsql		Setting for squelch.
 * \param intflag		Flag to indicate how ast_radio_wait_or_poll waits.
 */
static void tune_rxinput(int fd, struct chan_usbradio_pvt *o, int setsql, int intflag)
{
	const int settingmin = 1;
	const int settingstart = 2;
	const int maxtries = 12;

	int target;
	int tolerance = 2750;
	int setting = 0, tries = 0, tmpdiscfactor, meas, measnoise;
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

	ast_cli(fd, "tune rxnoise maxtries=%i, target=%i, tolerance=%i\n", maxtries, target, tolerance);

	while (tries < maxtries) {
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, setting, 0);
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, o->rxboost, 0);

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
		ast_cli(fd, "tries=%i, setting=%i, meas=%i\n", tries, setting, meas);

		if ((meas < (target - tolerance) || meas > (target + tolerance)) && tries <= 2) {
			f = (float) (setting * target) / meas;
			setting = (int) (f + 0.5);
		} else if (meas < (target - tolerance) && tries > 2) {
			setting++;
		} else if (meas > (target + tolerance) && tries > 2) {
			setting--;
		} else if (tries > 5 && meas > (target - tolerance) && meas < (target + tolerance)) {
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
	o->radio->spsRx->discfactor = (i16) 2000;
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

	ast_cli(fd, "DONE tries=%i, setting=%i, meas=%i, sqnoise=%i\n", tries, ((setting * 1000) + (o->micmax / 2)) / o->micmax, meas, measnoise);

	if (meas < (target - tolerance) || meas > (target + tolerance)) {
		ast_cli(fd, "ERROR: RX INPUT ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX INPUT ADJUST SUCCESS.\n");
		o->rxmixerset = ((setting * 1000) + (o->micmax / 2)) / o->micmax;

		if (o->rxcdtype == CD_XPMR_NOISE) {
			int normRssi = ((32767 - o->radio->rxRssi) * AUDIO_ADJUSTMENT / 32767);

			if ((meas / (measnoise / 10)) > 26) {
				ast_cli(fd, "WARNING: Insufficient high frequency noise from receiver.\n");
				ast_cli(fd, "WARNING: Rx input point may be de-emphasized and not flat.\n");
				ast_cli(fd, "         usbradioplus.conf setting of 'carrierfrom=dsp' not recommended.\n");
			} else {
				ast_cli(fd, "Rx noise input seems sufficient for squelch.\n");
			}
			if (setsql) {
				o->rxsquelchadj = normRssi + 150;
				if (o->rxsquelchadj > 999) {
					o->rxsquelchadj = 999;
				}
				*(o->radio->prxSquelchAdjust) = ((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
				ast_cli(fd, "Rx Squelch set to %d (RSSI=%d).\n", o->rxsquelchadj, normRssi);
			} else {
				if (o->rxsquelchadj < normRssi) {
					ast_cli(fd, "WARNING: RSSI=%i SQUELCH=%i and is set too loose.\n", normRssi, o->rxsquelchadj);
					ast_cli(fd, "         Use 'radio tune rxsquelch' to adjust.\n");
				}
			}
		}
	}
	o->radio->b.tuning = 0;
}

/*!
 * \brief Process Asterisk CLI request for receiver deviation display.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct
 * \return	CLI success, showusage, or failure.
 */
static void tune_rxdisplay(int fd, struct chan_usbradio_pvt *o)
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

/*!
 * \brief Process asterisk cli request for cos, ctcss, and ptt live display.
 * \param fd			Asterisk cli fd
 * \param o				Private struct
 * \return	Cli success, showusage, or failure.
 */
static void tune_rxtx_status(int fd, struct chan_usbradio_pvt *o)
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
		ast_cli(fd, " %s  | %s  | %s | %s\r", o->rxcdtype ? (o->rx_cos_active ? "Keyed" : "Clear") : "Off  ",
			o->rxsdtype ? (o->rx_ctcss_active ? "Keyed" : "Clear") : "Off  ", o->rxkeyed ? "Keyed" : "Clear",
			(o->txkeyed || o->txtestkey) ? "Keyed" : "Clear");
	}

	option_verbose = wasverbose;
}

/*!
 * \brief Set received voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */
static void _menu_rxvoice(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i, x;
	float f, f1;
	int adjustment;

	if (!str[0]) {
		if (o->rxdemod == RX_AUDIO_FLAT) {
			ast_cli(fd, "Current Rx voice setting: %d\n", (int) ((o->rxvoiceadj * 200.0) + .5));
		} else {
			ast_cli(fd, "Current Rx voice setting: %d\n", o->rxmixerset);
		}
		return;
	}
	for (x = 0; str[x]; x++) {
		if (!isdigit(str[x])) {
			break;
		}
	}
	if (str[x] || (sscanf(str, N_FMT(d), &i) < 1) || (i < 0) || (i > 999)) {
		ast_cli(fd, "Entry Error, Rx voice setting not changed\n");
		return;
	}
	if (o->rxdemod == RX_AUDIO_FLAT) {
		o->rxvoiceadj = (float) i / 200.0;
	} else {
		o->rxmixerset = i;
		/* adjust settings based on the device */
		if (o->devtype == C119B_PRODUCT_ID) {
			o->rxboost = 1; /*rxboost is always set for this device */
		}
		adjustment = o->rxmixerset * o->micmax / AUDIO_ADJUSTMENT;
		/* get interval step size */
		f = AUDIO_ADJUSTMENT / (float) o->micmax;

		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, adjustment, 0);
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, o->rxboost, 0);
		o->rxvoiceadj = 0.5 + (modff(((float) i) / f, &f1) * .093981);
	}
	*(o->radio->prxVoiceAdjust) = o->rxvoiceadj * M_Q8;
	ast_cli(fd, "Changed rx voice setting to %d\n", i);
}

/*!
 * \brief Print settings.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 */
static void _menu_print(int fd, struct chan_usbradio_pvt *o)
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
		ast_cli(fd, "Rx Level currently set to %d\n", (int) ((o->rxvoiceadj * 200.0) + .5));
	} else {
		ast_cli(fd, "Rx Level currently set to %d\n", o->rxmixerset);
	}
	ast_cli(fd, "Rx Squelch currently set to %d\n", o->rxsquelchadj);
	ast_cli(fd, "Tx Voice Level currently set to %d\n", o->txmixaset);
	ast_cli(fd, "Tx Tone Level currently set to %d\n", o->txctcssadj);
	if (o->legacyaudioscaling) {
		ast_cli(fd, "legacyaudioscaling is enabled\n");
	}
}

/*!
 * \brief Set squelch level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New squelch level.
 */
static void _menu_rxsquelch(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i, x;

	if (!str[0]) {
		ast_cli(fd, "Current Signal Strength is %d\n", ((32767 - o->radio->rxRssi) * 1000 / 32767));
		ast_cli(fd, "Current Squelch setting is %d\n", o->rxsquelchadj);
		return;
	}
	for (x = 0; str[x]; x++) {
		if (!isdigit(str[x])) {
			break;
		}
	}
	if (str[x] || (sscanf(str, N_FMT(d), &i) < 1) || (i < 0) || (i > 999)) {
		ast_cli(fd, "Entry Error, Rx Squelch Level setting not changed\n");
		return;
	}
	ast_cli(fd, "Changed Rx Squelch Level setting to %d\n", i);
	o->rxsquelchadj = i;
	/* adjust settings based on the device */
	*(o->radio->prxSquelchAdjust) = ((999 - i) * 32767) / AUDIO_ADJUSTMENT;
}

/*!
 * \brief Set tx voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */
static void _menu_txvoice(int fd, struct chan_usbradio_pvt *o, const char *cstr)
{
	const char *str = cstr;
	int i, j, x, dokey, withctcss;

	if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) && (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
		ast_cli(fd, "Error, No txvoice output configured.\n");
		return;
	}
	if (!str[0]) {
		if ((o->txmixa == TX_OUT_VOICE) || (o->txmixa == TX_OUT_COMPOSITE)) {
			ast_cli(fd, "Current Tx Voice Level setting on Channel A is %d\n", o->txmixaset);
		} else {
			ast_cli(fd, "Current Tx Voice Level setting on Channel B is %d\n", o->txmixbset);
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
	for (x = 0; str[x]; x++) {
		if (!isdigit(str[x])) {
			break;
		}
	}
	if (str[x] || (sscanf(str, N_FMT(d), &i) < 1) || (i < 0) || (i > 999)) {
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

/*!
 * \brief Set aux voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */
static void _menu_auxvoice(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i, x;

	if ((o->txmixa != TX_OUT_AUX) && (o->txmixb != TX_OUT_AUX)) {
		ast_cli(fd, "Error, No Auxvoice output configured.\n");
		return;
	}
	if (!str[0]) {
		if (o->txmixa == TX_OUT_AUX) {
			ast_cli(fd, "Current Aux Voice Level setting on Channel A is %d\n", o->txmixaset);
		} else {
			ast_cli(fd, "Current Aux Voice Level setting on Channel B is %d\n", o->txmixbset);
		}
		return;
	}
	for (x = 0; str[x]; x++) {
		if (!isdigit(str[x])) {
			break;
		}
	}
	if (str[x] || (sscanf(str, N_FMT(d), &i) < 1) || (i < 0) || (i > 999)) {
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

/*!
 * \brief Set tx tone level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */
static void _menu_txtone(int fd, struct chan_usbradio_pvt *o, const char *cstr)
{
	const char *str = cstr;
	int i, x, dokey;

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
		for (x = 0; str[x]; x++) {
			if (!isdigit(str[x])) {
				break;
			}
		}
		if (str[x] || (sscanf(str, N_FMT(d), &i) < 1) || (i < 0) || (i > 999)) {
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

/*!
 * \brief Process tune menu commands.
 *
 * susb tune menusupport X - where X is one of the following:
 *		0 - get flatrx, ctcssenable, echomode
 *		1 - get node names that are configured in usbradioplus.conf
 *		2 - print parameters
 *		3 - get node names that are configured in usbradioplus.conf, except current device
 *		a - receive rx level
 *		b - receiver tune display
 *		c - receive level
 *		d - receive ctcss level
 *		e - squelch level
 *		f - voice level
 *		g - aux level
 *		h - transmit a test tone
 *		i - tune receive level
 *		j - save current settings for the selected node
 *		k - change echo mode
 *		l - generate test tone
 *		m - change rxboost
 *		n - change txboost
 *		o - change carrier from
 *		p - change ctcss from
 *		q - change rx on delay
 *		r - change tx off delay
 *		s - change tx pre limiting
 *		t - change tx limiting only
 *		u - change rx demodulation
 *		v - view cos, ctcss and ptt status
 *		w - change tx mixer a
 *		x - change tx mixer b
 *		y - receive audio statistics display
 *		z - transmit audio statistics display
 *
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 * \param cmd			Command to process.
 */
static void tune_menusupport(int fd, struct chan_usbradio_pvt *o, const char *cmd)
{
	int x, oldverbose, flatrx, txhasctcss;
	struct chan_usbradio_pvt *oy = NULL;

	oldverbose = option_verbose;
	option_verbose = 0;
	flatrx = 0;
	if (o->rxdemod == RX_AUDIO_FLAT) {
		flatrx = 1;
	}
	txhasctcss = 0;
	if ((o->txmixa == TX_OUT_LSD) || (o->txmixa == TX_OUT_COMPOSITE) || (o->txmixb == TX_OUT_LSD) || (o->txmixb == TX_OUT_COMPOSITE)) {
		txhasctcss = 1;
	}
	switch (cmd[0]) {
	case '0': /* return audio processing configuration */
		/* note: to maintain backward compatibility for those expecting a specific # of
		   values to be returned (and in a specific order).  So, we only add to the end
		   of the returned list.  Also, once an update has been released we can't change
		   the format/content of any previously returned string */
		if (!strcmp(cmd, "0+10")) { /* With o->txslimsp tx soft limiter set point */
			ast_cli(fd, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,%d,%d\n", flatrx, txhasctcss,
				o->echomode, o->rxboost, o->txboost, o->rxcdtype, o->rxsdtype, o->rxondelay, o->txoffdelay, o->txprelim,
				o->txlimonly, o->rxdemod, o->txmixa, o->txmixb, o->rxmixerset, o->rxvoiceadj, o->rxsquelchadj, o->txmixaset,
				o->txmixbset, o->txctcssadj, o->micplaymax, o->spkrmax, o->micmax, o->txslimsp);
		} else if (!strcmp(cmd, "0+9")) {
			ast_cli(fd, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,%d\n", flatrx, txhasctcss, o->echomode,
				o->rxboost, o->txboost, o->rxcdtype, o->rxsdtype, o->rxondelay, o->txoffdelay, o->txprelim, o->txlimonly,
				o->rxdemod, o->txmixa, o->txmixb, o->rxmixerset, o->rxvoiceadj, o->rxsquelchadj, o->txmixaset, o->txmixbset,
				o->txctcssadj, o->micplaymax, o->spkrmax, o->micmax);
		} else {
			ast_cli(fd, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", flatrx, txhasctcss, o->echomode, o->rxboost, o->txboost,
				o->rxcdtype, o->rxsdtype, o->rxondelay, o->txoffdelay, o->txprelim, o->txlimonly, o->rxdemod, o->txmixa, o->txmixb);
		}
		break;
	case '1': /* return usb device name list */
		for (x = 0, oy = usbradio_default.next; oy && oy->name; oy = oy->next, x++) {
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
		for (x = 0, oy = usbradio_default.next; oy && oy->name; oy = oy->next) {
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
		ast_cli(fd, "Saved radio tuning settings to usbradioplus.conf\n");
		break;
	case 'k': /* change echo mode */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->echomode = 1;
			} else {
				o->echomode = 0;
			}
			ast_cli(fd, "Echo Mode changed to %s\n", (o->echomode) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "Echo Mode is currently %s\n", (o->echomode) ? "Enabled" : "Disabled");
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
				ast_debug(3, "TX soft limiter set failed in tune menu-support\n");
				break;
			} else {
				o->txslimsp = setpoint;
			}

			ast_cli(fd, "TX soft limiting setpoint changed to %i\n", setpoint);
		} else {
			ast_cli(fd, "TX soft limiting setpoint currently set to: %i\n", o->txslimsp);
		}
		break;

	case 'm': /* change rxboost */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->rxboost = 1;
			} else {
				o->rxboost = 0;
			}
			ast_cli(fd, "RxBoost changed to %s\n", (o->rxboost) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "RxBoost is currently %s\n", (o->rxboost) ? "Enabled" : "Disabled");
		}
		break;
	case 'n': /* change txboost */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->txboost = 1;
			} else {
				o->txboost = 0;
			}
			ast_cli(fd, "TxBoost changed to %s\n", (o->txboost) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "TxBoost is currently %s\n", (o->txboost) ? "Enabled" : "Disabled");
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
			ast_cli(fd, "TxPrelim changed to %s\n", (o->txprelim) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "TxPrelim is currently %s\n", (o->txprelim) ? "Enabled" : "Disabled");
		}
		break;
	case 't': /* change txlimonly */
		if (cmd[1]) {
			if (cmd[1] > '0') {
				o->txlimonly = 1;
			} else {
				o->txlimonly = 0;
			}
			ast_cli(fd, "TxLimonly changed to %s\n", (o->txlimonly) ? "Enabled" : "Disabled");
		} else {
			ast_cli(fd, "TxLimonly is currently %s\n", (o->txlimonly) ? "Enabled" : "Disabled");
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
static void tune_rxvoice(int fd, struct chan_usbradio_pvt *o, int intflag)
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
	}

	if (!o->radio->spsMeasure->source || !o->radio->prxVoiceAdjust) {
		ast_cli(fd, "ERROR: NO SOURCE OR MEASURE SETTING.\n");
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
		} else if (tries > 4 && meas > (target - tolerance) && meas < (target + tolerance)) {
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

	ast_cli(fd, "DONE tries=%i, setting=%f, meas=%f\n", tries, setting, (float) meas);
	if (meas < (target - tolerance) || meas > (target + tolerance)) {
		ast_cli(fd, "ERROR: RX VOICE GAIN ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX VOICE GAIN ADJUST SUCCESS.\n");
		o->rxvoiceadj = setting;
	}
	o->radio->b.tuning = 0;
}

/*!
 * \brief Determine the receive CTCSS level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param intflag		Flag to indicate how ast_radio_wait_or_poll waits.
 */
static void tune_rxctcss(int fd, struct chan_usbradio_pvt *o, int intflag)
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
		} else if (tries > 4 && meas > (target - tolerance) && meas < (target + tolerance)) {
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
	ast_cli(fd, "DONE tries=%i, setting=%f, meas=%.2f\n", tries, setting, (float) meas);
	if (meas < (target - tolerance) || meas > (target + tolerance)) {
		ast_cli(fd, "ERROR: RX CTCSS GAIN ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX CTCSS GAIN ADJUST SUCCESS.\n");
	}

	if (o->rxcdtype == CD_XPMR_NOISE) {
		int normRssi;

		if (ast_radio_wait_or_poll(fd, 200, intflag)) {
			o->radio->b.tuning = 0;
			return;
		}

		normRssi = ((32767 - o->radio->rxRssi) * AUDIO_ADJUSTMENT / 32767);

		if (o->rxsquelchadj > normRssi) {
			ast_cli(fd, "WARNING: RSSI=%i SQUELCH=%i and is too tight. Use 'radio tune rxsquelch'.\n", normRssi, o->rxsquelchadj);
		} else {
			ast_cli(fd, "INFO: RX RSSI=%i\n", normRssi);
		}
	}
	o->radio->b.tuning = 0;
}

/*!
 * \brief Update the tune settings to the configuration file.
 * \param config	The (opened) config to use
 * \param filename	The configuration file being updated (e.g. "usbradioplus.conf").
 * \param category	The category being updated (e.g. "12345").
 * \param variable	The variable being updated (e.g. "rxboost").
 * \param value		The value being updated (e.g. "yes").
 * \retval 0		If successful.
 * \retval -1		If unsuccessful.
 */
static int tune_variable_update(struct ast_config *config, const char *filename, struct ast_category *category,
	const char *variable, const char *value)
{
	int res;
	struct ast_variable *v, *var = NULL;

	/* ast_variable_retrieve, but returning the variable struct */
	for (v = ast_variable_browse(config, ast_category_get_name(category)); v; v = v->next) {
		if (!strcasecmp(variable, v->name)) {
			var = v;
		}
	}

	if (var && !strcmp(var->value, value)) {
		/* no need to update a matching value */
		return 0;
	}

	if (var && !var->inherited) {
		/* the variable is defined and not inherited from a template category */
		res = ast_variable_update(category, variable, value, var->value, var->object);
		if (res == 0) {
			return 0;
		}
	}

	/* create and add the variable / value to the category */
	var = ast_variable_new(variable, value, filename);
	if (var == NULL) {
		return -1;
	}

	/* and append */
	ast_variable_append(category, var);
	return 0;
}

/*!
 * \brief Write tune settings to the configuration file. If the device EEPROM is enabled, the settings are  saved to EEPROM.
 * \param o Channel private.
 */
static void tune_write(struct chan_usbradio_pvt *o)
{
	struct ast_config *cfg;
	struct ast_category *category = NULL;
	struct ast_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE };
	const float old_rxctcssadj = 0.5; /* for backward EEPROM format compatibility */

	if (!(cfg = ast_config_load2(CONFIG, "chan_usbradio", config_flags))) {
		ast_log(LOG_ERROR, "Config file not found: %s\n", CONFIG);
		return;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file has invalid format: %s\n", CONFIG);
		return;
	}

#define CONFIG_UPDATE_STR(field) \
	if (tune_variable_update(cfg, CONFIG, category, #field, o->field)) { \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field); \
	}

#define CONFIG_UPDATE_INT(field) \
	{ \
		char _buf[15]; \
		snprintf(_buf, sizeof(_buf), "%d", o->field); \
		if (tune_variable_update(cfg, CONFIG, category, #field, _buf)) { \
			ast_log(LOG_WARNING, "Failed to update %s\n", #field); \
		} \
	}

#define CONFIG_UPDATE_BOOL(field) \
	if (tune_variable_update(cfg, CONFIG, category, #field, o->field ? "yes" : "no")) { \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field); \
	}

#define CONFIG_UPDATE_FLOAT(field) \
	{ \
		char _buf[15]; \
		snprintf(_buf, sizeof(_buf), "%f", o->field); \
		if (tune_variable_update(cfg, CONFIG, category, #field, _buf)) { \
			ast_log(LOG_WARNING, "Failed to update %s\n", #field); \
		} \
	}

#define CONFIG_UPDATE_SIGNAL(key, field, signal_type) \
	if (tune_variable_update(cfg, CONFIG, category, #key, signal_type[o->field])) { \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field); \
	}

	category = ast_category_get(cfg, o->name, NULL);
	if (!category) {
		ast_log(LOG_ERROR, "No category '%s' exists?\n", o->name);
	} else {
		/*
		 * To simplify channel driver setup we allow the "devstr=" value
		 * to be empty/blank indicating that we should match the first
		 * available interface.
		 *
		 * This works (and will continue to work) well as long as the
		 * "devstr=" value in the configuration file remains empty/blank.
		 * But, if the value is ever provided then we only match interfaces
		 * with the specified string.  Moving the interface (accidentally
		 * or intentionally) to a different "port" will result in not
		 * finding/matching the interface.
		 *
		 * To minimize conflicts, we want to avoid writing out the specific
		 * "devstr=" value to the configuration file unless needed.  Here,
		 * we check if the current "devstr=" value is empty/blank and
		 * that there is only a single audio interface connected to the
		 * system.  If so, we leave the value empty/blank.
		 */
		const char *val;
		char *dev;

		val = ast_variable_retrieve(cfg, o->name, "devstr");
		dev = ast_radio_usb_get_devstr(1);
		if (!ast_strlen_zero(val) || !ast_strlen_zero(dev)) {
			/* if the "devstr=" value exists or there is more than 1 sound device */
			CONFIG_UPDATE_STR(devstr);
			if (!ast_strlen_zero(o->serial)) {
				CONFIG_UPDATE_STR(serial);
			}
		}
		CONFIG_UPDATE_INT(rxmixerset);
		CONFIG_UPDATE_INT(txmixaset);
		CONFIG_UPDATE_INT(txmixbset);
		CONFIG_UPDATE_FLOAT(rxvoiceadj);
		CONFIG_UPDATE_INT(txctcssadj);
		CONFIG_UPDATE_INT(rxsquelchadj);
		CONFIG_UPDATE_INT(fever);
		CONFIG_UPDATE_BOOL(rxboost);
		CONFIG_UPDATE_BOOL(txboost);
		CONFIG_UPDATE_SIGNAL(carrierfrom, rxcdtype, cd_signal_type);
		CONFIG_UPDATE_SIGNAL(ctcssfrom, rxsdtype, sd_signal_type);
		CONFIG_UPDATE_INT(rxondelay);
		CONFIG_UPDATE_INT(txoffdelay);
		CONFIG_UPDATE_BOOL(txprelim);
		CONFIG_UPDATE_BOOL(txlimonly);
		CONFIG_UPDATE_SIGNAL(rxdemod, rxdemod, demodulation_type);
		CONFIG_UPDATE_SIGNAL(txmixa, txmixa, mixer_type);
		CONFIG_UPDATE_SIGNAL(txmixb, txmixb, mixer_type);
		CONFIG_UPDATE_INT(txslimsp);
		{
			char gainbuf[32];
			snprintf(gainbuf, sizeof(gainbuf), "%.3f", o->plus_presquelch_gain_db);
			if (tune_variable_update(cfg, CONFIG, category, "presquelch_gain_db", gainbuf)) {
				ast_log(LOG_WARNING, "Failed to update presquelch_gain_db\n");
			}
			snprintf(gainbuf, sizeof(gainbuf), "%.3f", o->plus_postsquelch_gain_db);
			if (tune_variable_update(cfg, CONFIG, category, "postsquelch_gain_db", gainbuf)) {
				ast_log(LOG_WARNING, "Failed to update postsquelch_gain_db\n");
			}
		}
		if (ast_config_text_file_save2(CONFIG, cfg, "chan_usbradio", 0)) {
			ast_log(LOG_WARNING, "Failed to save config %s\n", CONFIG);
		}
	}

	ast_config_destroy(cfg);
#undef CONFIG_UPDATE_STR
#undef CONFIG_UPDATE_INT
#undef CONFIG_UPDATE_BOOL
#undef CONFIG_UPDATE_FLOAT
#undef CONFIG_UPDATE_SIGNAL

	if (o->wanteeprom) {
		ast_mutex_lock(&o->eepromlock);
		while (o->eepromctl) {
			ast_mutex_unlock(&o->eepromlock);
			usleep(10000);
			ast_mutex_lock(&o->eepromlock);
		}
		memset(o->eeprom, 0, sizeof(o->eeprom));
		o->eeprom[EEPROM_USER_RXMIXERSET] = o->rxmixerset;
		o->eeprom[EEPROM_USER_TXMIXASET] = o->txmixaset;
		o->eeprom[EEPROM_USER_TXMIXBSET] = o->txmixbset;
		memcpy(&o->eeprom[EEPROM_USER_RXVOICEADJ], &o->rxvoiceadj, sizeof(float));
		memcpy(&o->eeprom[EEPROM_USER_RXCTCSSADJ], &old_rxctcssadj, sizeof(float));
		o->eeprom[EEPROM_USER_TXCTCSSADJ] = o->txctcssadj;
		o->eeprom[EEPROM_USER_RXSQUELCHADJ] = o->rxsquelchadj;
		o->eepromctl = 2; /* request a write */
		ast_mutex_unlock(&o->eepromlock);
	}
}

/*!
 * \brief Update the ALSA mixer settings
 * Update the ALSA mixer settings.
 *
 * \param		chan_usbradio structure.
 */
static void mixer_write(struct chan_usbradio_pvt *o)
{
	int mic_setting;

	if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
		/* Scale the portable 0--999 setting to this CM119 mixer's range. */
		int mixer_level = (o->duplex3 * o->micplaymax
			+ DUPLEX3_LEVEL_MAX / 2) / DUPLEX3_LEVEL_MAX;
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_VOL, mixer_level, 0);
	} else {
		ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_VOL, 0, 0);
	}
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_PLAYBACK_SW, 0, 0);
	ast_radio_setamixer(o->devicenum, (o->newname) ? MIXER_PARAM_SPKR_PLAYBACK_SW_NEW : MIXER_PARAM_SPKR_PLAYBACK_SW, 1, 0);
	ast_radio_setamixer(o->devicenum, (o->newname) ? MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW : MIXER_PARAM_SPKR_PLAYBACK_VOL,
		ast_radio_make_spkr_playback_value(o->spkrmax, o->txmixaset, o->devtype),
		ast_radio_make_spkr_playback_value(o->spkrmax, o->txmixbset, o->devtype));
	/* adjust settings based on the device */
	if (o->devtype == C119B_PRODUCT_ID) {
		o->rxboost = 1; /*rxboost is always set for this device */
	}
	mic_setting = o->rxmixerset * o->micmax / AUDIO_ADJUSTMENT;
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL, mic_setting, 0);
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_BOOST, o->rxboost, 0);
	ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_SW, 1, 0);
}

/*!
 * \brief Adjust DSP multiplier
 * Adjusts the DSP multiplier to add resolution to the tx level adjustment
 *
 * \param		chan_usbradio structure.
 */
static void mult_set(struct chan_usbradio_pvt *o)
{
	o->radio->txOutputGainA = mult_calc((o->txmixaset * 152) / AUDIO_ADJUSTMENT);
	/* Preserve the legacy rule: matching routes use channel A gain. */
	o->radio->txOutputGainB = o->txmixa == o->txmixb
		? o->radio->txOutputGainA
		: mult_calc((o->txmixbset * 152) / AUDIO_ADJUSTMENT);
}

/*!
 * \brief Calculate multiplier.
 * \param value		Level to calculate.
 * \returns			Multiplier.
 */
static int mult_calc(int value)
{
	const int multx = M_Q8;
	int pot, mult;

	pot = ((int) (value / 4) * 4) + 2;
	mult = multx - ((multx * (3 - (value % 4))) / (pot + 2));
	return mult;
}

#define pd(x) \
	{ \
		ast_cli(fd, #x " = %d\n", x); \
	}
#define pp(x) \
	{ \
		ast_cli(fd, #x " = %p\n", x); \
	}
#define ps(x) \
	{ \
		ast_cli(fd, #x " = %s\n", x); \
	}
#define pf(x) \
	{ \
		ast_cli(fd, #x " = %f\n", x); \
	}

/*!
 * \brief Dump radio configuration and detector state.
 * \param o				Private struct.
 */
static void radio_dump(struct chan_usbradio_pvt *o, int fd)
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

	pd(o->rxmixerset);
	pd(o->rxboost);
	pd(o->txboost);

	pf(o->rxvoiceadj);
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

	pd(o->txlimonly);
	pd(o->txprelim);
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
	pd(p->b.dcsrxpolarity);
	pd(p->b.dcstxpolarity);
	pd(p->b.lsdrxpolarity);
	pd(p->b.lsdtxpolarity);

	pd(p->txMixA);
	pd(p->txMixB);

	pd(p->rxDeEmpEnable);
	pd(p->rxCenterSlicerEnable);
	pd(p->rxCtcssDecodeEnable);
	pd(p->rxDcsDecodeEnable);
	pd(p->b.ctcssRxEnable);
	pd(p->b.dcsRxEnable);
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

static void usbradioplus_program_radio(struct chan_usbradio_pvt *o)
{
	struct urp_parallel_bus bus;
	uint32_t rx_freq = o->remoted ? o->set_rxfreq : o->rxfreq;
	uint32_t tx_freq = o->remoted ? o->set_txfreq : o->txfreq;
	int high_power = o->remoted ? o->set_txpower : 0;

	if (!haspp)
		return;
	ast_mutex_lock(&pp_lock);
	bus.value = (uint8_t) pp_val;
	bus.write = usbradioplus_parallel_program_write;
	bus.opaque = NULL;
	urp_hardware_program_radio(&bus, rx_freq, tx_freq,
		o->radio && o->radio->txPttOut, high_power);
	pp_val = (int8_t) bus.value;
	ast_mutex_unlock(&pp_lock);
}

static void usbradioplus_parallel_program_write(void *opaque, uint8_t value)
{
	(void) opaque;
	pp_val = (int8_t) value;
	ast_radio_ppwrite(haspp, ppfd, pbase, pport, value);
}

static void usbradioplus_set_channel(uint8_t channel)
{
	struct urp_parallel_bus bus;
	if (!haspp)
		return;
	ast_mutex_lock(&pp_lock);
	bus.value = (uint8_t) pp_val;
	bus.write = usbradioplus_parallel_program_write;
	bus.opaque = NULL;
	urp_hardware_set_channel(&bus, channel);
	pp_val = (int8_t) bus.value;
	ast_mutex_unlock(&pp_lock);
}

/*
	takes data from a chan_usbradio_pvt struct (e.g. o->)
	and configures the native radio detector
*/
/*!
 * \brief Configure the native radio detector and signaling subsystem.
 * \param o			Private struct.
 * \retval 0		Success.
 * \retval 1		Failure.
 */

static int radio_config(struct chan_usbradio_pvt *o)
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

		o->radio->pTxCodeDefault = o->txctcssdefault;
		o->radio->pRxCodeSrc = o->rxctcssfreqs;
		o->radio->pTxCodeSrc = o->txctcssfreqs;

	}

	if (o->forcetxcode) {
		o->radio->pTxCodeDefault = o->set_txctcssfreq;
		ast_debug(3, "Channel %s: Forced Tx Squelch Code code=%s.\n", o->name, o->radio->pTxCodeDefault);
	}

	urp_radio_parse_codes(o->radio);
	usbradioplus_program_radio(o);

	return 0;
}

/*!
 * \brief Store configuration.
 *	Initializes chan_usbradio and loads it with the configuration data.
 * \param cfg			ast_config structure.
 * \param ctg			Category.
 * \return				chan_usbradio_pvt.
 */
static int store_cutoff(struct chan_usbradio_pvt *o, const char *name,
	const char *text)
{
	int *legacy = NULL, *enabled = NULL, *exact = NULL;
	double *frequency = NULL;
	double defaults = 0.0;
	struct urp_cutoff_setting setting;

#define SELECT_CUTOFF(field, hz) do { \
	legacy = &o->field; enabled = &o->plus_##field##_enabled; \
	exact = &o->plus_##field##_exact; frequency = &o->plus_##field##_hz; \
	defaults = (hz); \
} while (0)
	if (!strcasecmp(name, "rxlpf")) SELECT_CUTOFF(rxlpf, 3000.0);
	else if (!strcasecmp(name, "rxhpf")) SELECT_CUTOFF(rxhpf, 300.0);
	else if (!strcasecmp(name, "txlpf")) SELECT_CUTOFF(txlpf, 3000.0);
	else if (!strcasecmp(name, "txhpf")) SELECT_CUTOFF(txhpf, 300.0);
	else return 0;
#undef SELECT_CUTOFF

	if (urp_parse_cutoff(text, defaults, URP_RATE_NATIVE / 2.0, &setting)) return -1;
	*legacy = setting.selector;
	*enabled = setting.enabled;
	*exact = setting.exact;
	*frequency = setting.frequency_hz;
	return 1;
}

static struct chan_usbradio_pvt *store_config(struct ast_config *cfg, const char *ctg)
{
	const struct ast_variable *v;
	struct chan_usbradio_pvt *o;
	char buf[100];
	int i;

	if (ctg == NULL) {
		o = &usbradio_default;
		ctg = "general";
	} else {
		/* "general" is also the default thing */
		if (strcmp(ctg, "general") == 0) {
			o = &usbradio_default;
		} else {
			if (!(o = ast_calloc(1, sizeof(*o)))) {
				return NULL;
			}
			*o = usbradio_default;
			o->name = ast_strdup(ctg);
			o->pttkick[0] = -1;
			o->pttkick[1] = -1;
			if (!usbradio_active) {
				usbradio_active = o->name;
			}
		}
	}
	o->echoq.q_forw = o->echoq.q_back = &o->echoq;
	ast_mutex_init(&o->echolock);
	ast_mutex_init(&o->eepromlock);
	ast_mutex_init(&o->usblock);
	ast_mutex_init(&o->plus_link_lock);
	o->echomax = DEFAULT_ECHO_MAX;
	/* fill other fields from configuration */
	for (v = ast_variable_browse(cfg, ctg); v; v = v->next) {
		int cutoff_result = store_cutoff(o, v->name, v->value);
		if (cutoff_result < 0) {
			ast_log(LOG_ERROR, "RadioPlus/%s: invalid %s cutoff '%s'\n",
				ctg, v->name, v->value);
			if (o != &usbradio_default) ast_free(o);
			return NULL;
		}
		if (cutoff_result > 0) continue;
		CV_START((char *) v->name, (char *) v->value);

		/* handle jb conf */
		if (!ast_jb_read_conf(&global_jbconf, v->name, v->value)) {
			continue;
		}

		CV_UINT("frags", o->frags);
		CV_UINT("queuesize", o->queuesize);
		CV_BOOL("rxcpusaver", o->rxcpusaver);
		CV_BOOL("txcpusaver", o->txcpusaver);
		CV_BOOL("invertptt", o->invertptt);
		CV_F("rxdemod", store_rxdemod(o, (char *) v->value));
		CV_BOOL("txlimonly", o->txlimonly);
		CV_BOOL("txprelim", o->txprelim);
		CV_F("txmixa", store_txmixa(o, (char *) v->value));
		CV_F("txmixb", store_txmixb(o, (char *) v->value));
		CV_F("carrierfrom", store_rxcdtype(o, (char *) v->value));
		CV_UINT("voxhangtime", o->voxhangtime);
		CV_F("ctcssfrom", store_rxsdtype(o, (char *) v->value));
		CV_UINT("rxsqvox", o->rxsqvoxadj);
		CV_UINT("rxsqhyst", o->rxsqhyst);
		CV_UINT("rxnoisefiltype", o->rxnoisefiltype);
		CV_UINT("rxsquelchdelay", o->rxsquelchdelay);
		CV_STR("txctcssdefault", o->txctcssdefault);
		CV_STR("rxctcssfreqs", o->rxctcssfreqs);
		CV_STR("txctcssfreqs", o->txctcssfreqs);
		CV_BOOL("rxctcssoverride", o->rxctcssoverride);
		CV_UINT("rxfreq", o->rxfreq);
		CV_UINT("txfreq", o->txfreq);
		CV_F("rxgain", store_rxgain(o, (char *) v->value));
		CV_BOOL("rxboost", o->rxboost);
		CV_BOOL("txboost", o->txboost);
		CV_UINT("rxctcssrelax", o->rxctcssrelax);
		CV_F("txtoctype", store_txtoctype(o, (char *) v->value));
		CV_UINT("hdwtype", o->hdwtype);
		CV_UINT("eeprom", o->wanteeprom);
		CV_UINT("duplex", o->radioduplex);
		CV_UINT("txsettletime", o->txsettletime);
		CV_UINT("txrxblankingtime", o->txrxblankingtime);
		CV_BOOL("rxpolarity", o->rxpolarity);
		CV_BOOL("txpolarity", o->txpolarity);
		CV_BOOL("dcsrxpolarity", o->dcsrxpolarity);
		CV_BOOL("dcstxpolarity", o->dcstxpolarity);
		CV_BOOL("lsdrxpolarity", o->lsdrxpolarity);
		CV_BOOL("lsdtxpolarity", o->lsdtxpolarity);
		CV_BOOL("radioactive", o->radioactive);
		CV_UINT("rptnum", o->rptnum);
		CV_UINT("idleinterval", o->idleinterval);
		CV_UINT("turnoffs", o->turnoffs);
		CV_UINT("tracetype", o->tracetype);
		CV_UINT("tracelevel", o->tracelevel);
		CV_UINT("rxondelay", o->rxondelay);
		if (o->rxondelay > MS_TO_FRAMES(RX_ON_DELAY_MAX)) {
			o->rxondelay = MS_TO_FRAMES(RX_ON_DELAY_MAX);
		}
		CV_UINT("txoffdelay", o->txoffdelay);
		if (o->txoffdelay > MS_TO_FRAMES(TX_OFF_DELAY_MAX)) {
			o->txoffdelay = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
		}
		CV_UINT("area", o->area);
		CV_STR("ukey", o->ukey);
		CV_UINT("duplex3", o->duplex3);
		if (!strcasecmp(v->name, "duplex3mode")) {
			if (!strcasecmp(v->value, "hardware")) {
				o->duplex3mode = DUPLEX3_MODE_HARDWARE;
			} else if (!strcasecmp(v->value, "software")) {
				o->duplex3mode = DUPLEX3_MODE_SOFTWARE;
			} else {
				ast_log(LOG_ERROR, "RadioPlus/%s: duplex3mode must be hardware or software, not '%s'\n",
					ctg, v->value);
				if (o != &usbradio_default) ast_free(o);
				return NULL;
			}
			continue;
		}
		CV_BOOL("txvoicehighpass", o->plus_tx_hpf_enabled);
		CV_BOOL("linkhighpass", o->plus_link_hpf_enabled);
		CV_BOOL("nativeparrot", o->plus_parrot_enabled);
		CV_UINT("parrotmaxseconds", o->plus_parrot_max_seconds);
		if (!strcasecmp(v->name, "txvoicehighpass_hz")) {
			o->plus_tx_hpf_hz = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "linkhighpass_hz")) {
			o->plus_link_hpf_hz = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "emphasis_corner_hz")) {
			o->plus_emphasis_corner_hz = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "presquelch_gain_db")) {
			o->plus_presquelch_gain_db = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "postsquelch_gain_db")) {
			o->plus_postsquelch_gain_db = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "tx_ceiling_dbfs")) {
			o->plus_tx_ceiling_dbfs = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "preemphasis_headroom_db")) {
			o->plus_preemphasis_headroom_db = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "rxlevel_presquelch_target_dbfs")) {
			o->plus_rxlevel_presquelch_target_dbfs = strtod(v->value, NULL);
			continue;
		}
		if (!strcasecmp(v->name, "rxlevel_post_target_dbfs")) {
			o->plus_rxlevel_post_target_dbfs = strtod(v->value, NULL);
			continue;
		}
		CV_UINT("sendvoter", o->sendvoter);
		CV_UINT("clipledgpio", o->clipledgpio);
		CV_BOOL("legacyaudioscaling", o->legacyaudioscaling);
		CV_END;

		for (i = 0; i < GPIO_PINCOUNT; i++) {
			sprintf(buf, "gpio%d", i + 1);
			if (!strcmp(v->name, buf)) {
				o->gpios[i] = ast_strdup(v->value);
			}
		}
		for (i = 2; i <= 15; i++) {
			if (!((1 << i) & PP_MASK)) {
				continue;
			}
			sprintf(buf, "pp%d", i);
			if (!strcasecmp(v->name, buf)) {
				o->pps[i] = ast_strdup(v->value);
				haspp = 1;
			}
		}
	}
	if (o->plus_rxhpf_enabled && o->plus_rxlpf_enabled
		&& (o->plus_rxhpf_exact ? o->plus_rxhpf_hz : usbradioplus_legacy_cutoff("rxhpf", o->rxhpf))
		>= (o->plus_rxlpf_exact ? o->plus_rxlpf_hz : usbradioplus_legacy_cutoff("rxlpf", o->rxlpf))) {
		ast_log(LOG_ERROR, "RadioPlus/%s: rxhpf cutoff must be below rxlpf cutoff\n", ctg);
		if (o != &usbradio_default) ast_free(o);
		return NULL;
	}
	if (o->duplex3 < 0 || o->duplex3 > DUPLEX3_LEVEL_MAX) {
		ast_log(LOG_ERROR, "RadioPlus/%s: duplex3 must be between 0 and %d\n",
			ctg, DUPLEX3_LEVEL_MAX);
		if (o != &usbradio_default) ast_free(o);
		return NULL;
	}
	if (o->plus_txhpf_enabled && o->plus_txlpf_enabled
		&& (o->plus_txhpf_exact ? o->plus_txhpf_hz : usbradioplus_legacy_cutoff("txhpf", o->txhpf))
		>= (o->plus_txlpf_exact ? o->plus_txlpf_hz : usbradioplus_legacy_cutoff("txlpf", o->txlpf))) {
		ast_log(LOG_ERROR, "RadioPlus/%s: txhpf cutoff must be below txlpf cutoff\n", ctg);
		if (o != &usbradio_default) ast_free(o);
		return NULL;
	}

	if (o->rxsdtype != SD_XPMR) {
		o->rxctcssfreqs[0] = 0;
		o->txctcssfreqs[0] = 0;
	}

	if ((o->txmixa == TX_OUT_COMPOSITE) && (o->txmixb == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have B channel be Voice with A channel being Composite!!\n");
	}
	if ((o->txmixb == TX_OUT_COMPOSITE) && (o->txmixa == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have A channel be Voice with B channel being Composite!!\n");
	}

	if (o == &usbradio_default) { /* we are done with the default */
		return NULL;
	}
	if (o->plus_tx_hpf_hz < 0.0 || o->plus_tx_hpf_hz >= URP_RATE_NATIVE / 2.0
		|| o->plus_link_hpf_hz < 0.0 || o->plus_link_hpf_hz >= URP_RATE_NATIVE / 2.0
		|| o->plus_emphasis_corner_hz <= 0.0 || o->plus_emphasis_corner_hz >= 300.0
		|| o->plus_presquelch_gain_db < -30.0 || o->plus_presquelch_gain_db > 30.0
		|| o->plus_postsquelch_gain_db < -30.0 || o->plus_postsquelch_gain_db > 30.0
		|| o->plus_tx_ceiling_dbfs > 0.0 || o->plus_tx_ceiling_dbfs < -20.0
		|| o->plus_preemphasis_headroom_db < 0.0
		|| o->plus_preemphasis_headroom_db > 60.0
		|| o->plus_rxlevel_presquelch_target_dbfs > -0.1 || o->plus_rxlevel_presquelch_target_dbfs < -20.0
		|| o->plus_rxlevel_post_target_dbfs > -0.1 || o->plus_rxlevel_post_target_dbfs < -20.0
		|| !o->plus_parrot_max_seconds || o->plus_parrot_max_seconds > 300) {
		ast_log(LOG_ERROR, "RadioPlus/%s: invalid native DSP configuration\n", o->name);
		return NULL;
	}
	if (usbradioplus_dsp_init(o)) {
		ast_log(LOG_ERROR, "RadioPlus/%s: native DSP initialization failed\n", o->name);
		usbradioplus_dsp_destroy(o);
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

	load_tune_config(o, NULL, 0);

	/* if we are using the EEPROM, request hidthread load the EEPROM */
	if (o->wanteeprom) {
		ast_mutex_lock(&o->eepromlock);
		while (o->eepromctl) {
			ast_mutex_unlock(&o->eepromlock);
			usleep(10000);
			ast_mutex_lock(&o->eepromlock);
		}
		o->eepromctl = 1; /* request a load */
		ast_mutex_unlock(&o->eepromlock);
	}
	o->dsp = ast_dsp_new();
	if (o->dsp) {
		ast_dsp_set_features(o->dsp, DSP_FEATURE_DIGIT_DETECT);
		ast_dsp_set_digitmode(o->dsp, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_RELAXDTMF);
	}
	if (o->rxsqhyst == 0) {
		o->rxsqhyst = 3000;
	}

	if (o->rxsquelchdelay > RXSQDELAYBUFSIZE / 8 - 1) {
		ast_log(LOG_WARNING, "rxsquelchdelay of %i is > maximum of %i. Set to maximum.\n", o->rxsquelchdelay, RXSQDELAYBUFSIZE / 8 - 1);
		o->rxsquelchdelay = RXSQDELAYBUFSIZE / 8 - 1;
	}
	if (o->radio == NULL) {
		urp_radio_state tChan;

		memset(&tChan, 0, sizeof(urp_radio_state));

		tChan.pTxCodeDefault = o->txctcssdefault;
		tChan.pRxCodeSrc = o->rxctcssfreqs;
		tChan.pTxCodeSrc = o->txctcssfreqs;

		tChan.rxDemod = o->rxdemod;
		tChan.rxCdType = o->rxcdtype;
		tChan.voxHangTime = o->voxhangtime;
		tChan.rxCarrierHyst = o->rxsqhyst;
		tChan.rxSqVoxAdj = o->rxsqvoxadj;
		tChan.rxSquelchDelay = o->rxsquelchdelay;

		if (o->txlimonly) {
			tChan.txMod = 1;
		}
		if (o->txprelim) {
			tChan.txMod = 2;
		}

		tChan.txMixA = o->txmixa;
		tChan.txMixB = o->txmixb;

		tChan.rxCpuSaver = o->rxcpusaver;
		tChan.txCpuSaver = o->txcpusaver;

		tChan.b.rxpolarity = o->rxpolarity;
		tChan.b.txpolarity = o->txpolarity;

		tChan.b.dcsrxpolarity = o->dcsrxpolarity;
		tChan.b.dcstxpolarity = o->dcstxpolarity;

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

		o->radio->radioDuplex = o->radioduplex;
		o->radio->b.loopback = 0;
		o->radio->txsettletime = o->txsettletime;
		o->radio->txrxblankingtime = o->txrxblankingtime;
		o->radio->rxCpuSaver = o->rxcpusaver;
		o->radio->txCpuSaver = o->txcpusaver;

		*(o->radio->prxSquelchAdjust) = ((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
		*(o->radio->prxVoiceAdjust) = o->rxvoiceadj * M_Q8;
		o->radio->rxCtcss->relax = o->rxctcssrelax;
		o->radio->txTocType = o->txtoctype;

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) && (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		}

		if (o->txctcssfreq[0] && o->txmixa != TX_OUT_LSD && o->txmixa != TX_OUT_COMPOSITE && o->txmixb != TX_OUT_LSD &&
			o->txmixb != TX_OUT_COMPOSITE) {
			ast_log(LOG_ERROR, "No txtone output configured.\n");
		}

		if (o->radioactive) {
			struct chan_usbradio_pvt *ao;
			for (ao = usbradio_default.next; ao && ao->name; ao = ao->next) {
				ao->radioactive = 0;
			}
			usbradio_active = o->name;
			o->radioactive = 1;
			ast_log(LOG_NOTICE, "radio active set to [%s]\n", o->name);
		}
	}

	hidhdwconfig(o);

	/* link into list of devices */
	if (o != &usbradio_default) {
		o->next = usbradio_default.next;
		usbradio_default.next = o;
	}
	return o;
}

/*!
 * \brief Turns integer response to char CLI response
 * \param r				Response.
 * \return	CLI success, showusage, or failure.
 */
static char *res2cli(int r)
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

/*!
 * \brief Handle Asterisk CLI request to key transmitter.
 * \param e				Asterisk CLI entry.
 * \param cmd			Cli command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_console_key(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus key";
		e->usage = "Usage: radio key\n"
				   "       Simulates COR active.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	return res2cli(console_key(a->fd, a->argc, a->argv));
}

/*!
 * \brief Handle Asterisk CLI request to unkey transmitter.
 * \param e				Asterisk CLI entry.
 * \param cmd			CLI command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_console_unkey(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus unkey";
		e->usage = "Usage: radio unkey\n"
				   "       Simulates COR un-active.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	return res2cli(console_unkey(a->fd, a->argc, a->argv));
}

/*!
 * \brief Handle Asterisk CLI request for usb tune command.
 * \param e				Asterisk CLI entry.
 * \param cmd			CLI command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_radio_tune(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus tune "
					 "{auxvoice|dump|swap|rxnoise|rxvoice|rxtone|txvoice|txtone|txall|flash|rxsquelch|nocap|rxtracecap|"
					 "txtracecap|rxcap|txcap|save|load|menu-support|txslimsp}";
		e->usage = "Usage: radio tune <function>\n"
				   "       rxnoise\n"
				   "       rxvoice\n"
				   "       rxtone\n"
				   "       rxsquelch [newsetting]\n"
				   "       txvoice [newsetting]\n"
				   "       txtone [newsetting]\n"
				   "       txslimsp [setpoint]\n"
				   "       auxvoice [newsetting]\n"
				   "       save (settings to tuning file)\n"
				   "       load (tuning settings from EEPROM)\n\n"
				   "       All [newsetting]'s are values 0-999\n"
				   "       [setpoint] is 5000 to 13000\n\n";

		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	return res2cli(radio_tune(a->fd, a->argc, a->argv));
}

/*!
 * \brief Handle Asterisk CLI request active device command.
 * \param e				Asterisk CLI entry.
 * \param cmd			CLI command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_radio_active(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus active";
		e->usage = "Usage: radio active [device-name]\n"
				   "       If used without a parameter, displays which device is the current\n"
				   "       one being commanded.  If a device is specified, the commanded radio device is changed\n"
				   "       to the device specified.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	return res2cli(radio_active(a->fd, a->argc, a->argv));
}

/*!
 * \brief Handle Asterisk CLI request for radio show settings.
 * \param e				Asterisk CLI entry.
 * \param cmd			CLI command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_show_settings(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus show settings";
		e->usage = "Usage: radio show settings\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	o = find_desc(usbradio_active);
	if (o) {
		_menu_print(a->fd, o);
	}
	return RESULT_SUCCESS;
}

/*!
 * \brief Handle Asterisk CLI request to set xdebug.
 * \param e				Asterisk CLI entry.
 * \param cmd			CLI command type.
 * \param a				Asterisk CLI arguments.
 * \return	CLI success or failure.
 */
static char *handle_set_dsp_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
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
	}
	return res2cli(radio_set_dsp_debug(a->fd, a->argc, a->argv));
}

static short plus_saturating_add(short a, short b)
{
	int value = (int) a + (int) b;
	if (value > 32767) {
		return 32767;
	}
	if (value < -32768) {
		return -32768;
	}
	return (short) value;
}

static int plus_mix_has_program(enum radio_tx_mix mix)
{
	return mix == TX_OUT_VOICE || mix == TX_OUT_COMPOSITE || mix == TX_OUT_AUX;
}

static int usbradioplus_dsp_init(struct chan_usbradio_pvt *o)
{
	double tau_us;

	o->plus_up = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	o->plus_down = urp_src_create(SRC_SINC_BEST_QUALITY, 1);
	if (!o->plus_up || !o->plus_down) {
		ast_log(LOG_ERROR, "RadioPlus/%s: unable to create native sample-rate converters\n", o->name);
		return -1;
	}
	urp_biquad_highpass(&o->plus_tx_hpf, URP_RATE_NATIVE,
		o->plus_tx_hpf_hz, o->plus_tx_hpf_enabled);
	urp_biquad_highpass(&o->plus_link_hpf, URP_RATE_NATIVE,
		o->plus_link_hpf_hz, o->plus_link_hpf_enabled);
	/* A very low corner closely realizes the conventional 6 dB/octave
	 * land-mobile curve across the voice band. */
	tau_us = 1000000.0 / (2.0 * M_PI * o->plus_emphasis_corner_hz);
	urp_deemphasis_configure(&o->plus_deemphasis, URP_RATE_NATIVE,
		tau_us, o->rxdemod == RX_AUDIO_FLAT);
	urp_preemphasis_configure(&o->plus_preemphasis, URP_RATE_NATIVE,
		tau_us, o->txprelim && !o->txlimonly);
	urp_preemphasis_configure(&o->plus_link_preemphasis, URP_RATE_NATIVE,
		tau_us, o->txprelim && !o->txlimonly);
	txagc_core_init(&o->plus_final_core);
	txagc_avfilter_init(&o->plus_local_avfilter);
	txagc_avfilter_init(&o->plus_rx_filter);
	txagc_avfilter_init(&o->plus_rx_filter_after);
	txagc_avfilter_init(&o->plus_final_avfilter);
	txagc_rnnoise_init(&o->plus_local_rnnoise);
	o->plus_adc_peak_dbfs = o->plus_adc_max_peak_dbfs = -INFINITY;
	o->plus_deemphasis_peak_dbfs = o->plus_deemphasis_max_peak_dbfs = -INFINITY;
	o->plus_preemphasis_input_peak_dbfs =
		o->plus_preemphasis_input_max_peak_dbfs = -INFINITY;
	o->plus_tx_program_peak_dbfs = -INFINITY;
	o->plus_tx_program_max_peak_dbfs = -INFINITY;
	o->plus_local_tx_peak_dbfs = -INFINITY;
	o->plus_local_tx_max_peak_dbfs = -INFINITY;
	if (o->plus_parrot_enabled) {
		o->plus_parrot_capacity = (size_t) o->plus_parrot_max_seconds * URP_RATE_NATIVE;
		o->plus_parrot = ast_calloc(o->plus_parrot_capacity, sizeof(*o->plus_parrot));
		o->plus_parrot_raw = ast_calloc(o->plus_parrot_capacity,
			sizeof(*o->plus_parrot_raw));
		if (!o->plus_parrot || !o->plus_parrot_raw) {
			ast_free(o->plus_parrot);
			ast_free(o->plus_parrot_raw);
			o->plus_parrot = o->plus_parrot_raw = NULL;
			ast_log(LOG_ERROR, "RadioPlus/%s: unable to allocate native parrot buffer\n", o->name);
			return -1;
		}
	}
	return 0;
}

static void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o)
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
	ast_free(o->plus_parrot_raw);
	o->plus_parrot = NULL;
	o->plus_parrot_raw = NULL;
	o->plus_parrot_capacity = o->plus_parrot_count = o->plus_parrot_play = 0;
}

static short plus_apply_gain(short sample, double linear)
{
	double value = sample * linear;
	if (value > 32767.0) return 32767;
	if (value < -32768.0) return -32768;
	return (short) lrint(value);
}

static void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o)
{
	const short *input = (short *) (o->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	double gain = pow(10.0, o->plus_presquelch_gain_db / 20.0);
	size_t i;
	for (i = 0; i < ARRAY_LEN(o->plus_squelch_native); ++i) {
		o->plus_squelch_native[i] = plus_apply_gain(input[i], gain);
	}
}

static unsigned int plus_peak(const short *samples, size_t count)
{
	unsigned int peak = 0;
	size_t i;
	for (i = 0; i < count; ++i) {
		unsigned int value = samples[i] == INT16_MIN ? 32768U
			: (unsigned int) abs(samples[i]);
		if (value > peak) peak = value;
	}
	return peak;
}

static int plus_count_adc_rails(struct chan_usbradio_pvt *o,
	const short *samples, size_t count)
{
	int clipped = 0;
	size_t i;
	for (i = 0; i < count; ++i) {
		if (samples[i] == INT16_MAX) {
			o->plus_rxlevel_positive_rail_samples++;
			clipped = 1;
		} else if (samples[i] == INT16_MIN) {
			o->plus_rxlevel_negative_rail_samples++;
			clipped = 1;
		}
	}
	return clipped;
}

static double plus_peak_dbfs(unsigned int peak)
{
	return peak ? 20.0 * log10((double) peak / 32768.0) : -INFINITY;
}

static double plus_peak_double(const double *samples, size_t count)
{
	double peak = 0.0;
	size_t i;
	for (i = 0; i < count; ++i) {
		double value = fabs(samples[i]);
		if (value > peak) peak = value;
	}
	return peak;
}

static void plus_link_native_push(struct chan_usbradio_pvt *o,
	const short *samples, size_t count)
{
	while (count--) {
		unsigned int tail;
		if (o->plus_link_native_count == PLUS_LINK_NATIVE_FIFO_SAMPLES) {
			o->plus_link_native_head = (o->plus_link_native_head + 1)
				% PLUS_LINK_NATIVE_FIFO_SAMPLES;
			o->plus_link_native_count--;
			o->plus_link_queue_overflows++;
		}
		tail = (o->plus_link_native_head + o->plus_link_native_count)
			% PLUS_LINK_NATIVE_FIFO_SAMPLES;
		o->plus_link_native_fifo[tail] = *samples++;
		o->plus_link_native_count++;
	}
}

static int plus_link_native_pop(struct chan_usbradio_pvt *o, short *samples)
{
	size_t i;
	if (o->plus_link_native_count < URP_NATIVE_SAMPLES) return 0;
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		samples[i] = o->plus_link_native_fifo[o->plus_link_native_head];
		o->plus_link_native_head = (o->plus_link_native_head + 1)
			% PLUS_LINK_NATIVE_FIFO_SAMPLES;
	}
	o->plus_link_native_count -= URP_NATIVE_SAMPLES;
	return 1;
}

static uint64_t plus_ceiling_dbfs(double *samples, size_t count,
	double ceiling_dbfs)
{
	double limit = 32768.0 * pow(10.0, ceiling_dbfs / 20.0);
	uint64_t limited = 0;
	size_t i;
	for (i = 0; i < count; ++i) {
		if (samples[i] > limit) {
			samples[i] = limit;
			limited++;
		} else if (samples[i] < -limit) {
			samples[i] = -limit;
			limited++;
		}
	}
	return limited;
}

static double plus_clamp_gain(double gain)
{
	return gain > 30.0 ? 30.0 : (gain < -30.0 ? -30.0 : gain);
}

static void usbradioplus_rxlevel_finish(struct chan_usbradio_pvt *o)
{
	double pre, total;
	o->plus_rxlevel_active = 0;
	if (o->plus_rxlevel_noise_samples < URP_RATE_NATIVE
		|| o->plus_rxlevel_signal_samples < URP_RATE_NATIVE
		|| !o->plus_rxlevel_noise_peak || !o->plus_rxlevel_signal_peak) {
		ast_log(LOG_WARNING, "RadioPlus/%s: receive calibration rejected: need at least one second each of unsquelched noise and COR-qualified audio\n", o->name);
		return;
	}
	pre = o->plus_rxlevel_presquelch_target_dbfs
		- plus_peak_dbfs(o->plus_rxlevel_noise_peak);
	total = o->plus_rxlevel_post_target_dbfs
		- plus_peak_dbfs(o->plus_rxlevel_signal_peak);
	o->plus_presquelch_gain_db = plus_clamp_gain(pre);
	o->plus_postsquelch_gain_db = plus_clamp_gain(total - o->plus_presquelch_gain_db);
	ast_log(LOG_NOTICE, "RadioPlus/%s: receive calibration complete: noise %.2f dBFS, signal %.2f dBFS, pre-squelch gain %.2f dB, post-squelch gain %.2f dB, raw clipped frames %u\n",
		o->name, plus_peak_dbfs(o->plus_rxlevel_noise_peak),
		plus_peak_dbfs(o->plus_rxlevel_signal_peak), o->plus_presquelch_gain_db,
		o->plus_postsquelch_gain_db, o->plus_rxlevel_raw_clip_frames);
	if (o->plus_rxlevel_raw_clip_frames) {
		ast_log(LOG_WARNING, "RadioPlus/%s: CM119 ADC reached a PCM rail: +rail %" PRIu64 ", -rail %" PRIu64 " samples in %u frames; reduce analog/capture gain and recalibrate\n",
			o->name, o->plus_rxlevel_positive_rail_samples,
			o->plus_rxlevel_negative_rail_samples, o->plus_rxlevel_raw_clip_frames);
	}
}

static double usbradioplus_legacy_cutoff(const char *name, int selector)
{
	enum urp_legacy_filter filter = URP_FILTER_TX_HIGHPASS;
	if (!strcmp(name, "rxlpf")) filter = URP_FILTER_RX_LOWPASS;
	else if (!strcmp(name, "rxhpf")) filter = URP_FILTER_RX_HIGHPASS;
	else if (!strcmp(name, "txlpf")) filter = URP_FILTER_TX_LOWPASS;
	return urp_legacy_cutoff(filter, selector);
}

static void usbradioplus_native_tick(struct chan_usbradio_pvt *o)
{
	struct txagc_chain chain;
	double program[URP_NATIVE_SAMPLES];
	double local_program[URP_NATIVE_SAMPLES];
	double parrot_raw[URP_NATIVE_SAMPLES];
	double ctcss[URP_NATIVE_SAMPLES];
	short network_program[URP_NATIVE_SAMPLES];
	short *stereo = (short *) o->usbradio_write_buf;
	size_t used = 0, made = 0, i;
	int local_chain_enabled;
	int ctcss_phase_reverse;
	double ctcss_frequency, ctcss_peak_a, ctcss_peak_b;
	double ctcss_bias_a, ctcss_bias_b;
	int ctcss_filter_250, ctcss_tone_gain;

	local_chain_enabled = !usbradioplus_processing_get_local(&chain)
		&& chain.enabled;
	ctcss_phase_reverse = o->radio->txCtcssPhaseShift;
	ctcss_frequency = o->radio->txCtcssFreq10 / 10.0;
	ctcss_filter_250 = o->radio->txCtcssFilter250;
	ctcss_tone_gain = o->radio->txCtcssGainQ8;
	urp_ctcss_legacy_scaled_levels(ctcss_frequency,
		ctcss_filter_250, ctcss_tone_gain,
		o->radio->txOutputGainA, &ctcss_peak_a, &ctcss_bias_a);
	urp_ctcss_legacy_scaled_levels(ctcss_frequency,
		ctcss_filter_250, ctcss_tone_gain, o->radio->txOutputGainB,
		&ctcss_peak_b,
		&ctcss_bias_b);
	urp_ctcss_generate(&o->plus_ctcss_generator, ctcss, URP_NATIVE_SAMPLES,
		ctcss_frequency, 1.0,
		o->radio->txCtcssEnabled && !o->radio->b.txCtcssOff,
		ctcss_phase_reverse);

	urp_extract_mono((short *) (o->usbradio_read_buf + AST_FRIENDLY_OFFSET),
		o->plus_rx_native, URP_NATIVE_SAMPLES, 0);
	{
		unsigned int peak = plus_peak(o->plus_rx_native, URP_NATIVE_SAMPLES);
		o->plus_adc_peak_dbfs = plus_peak_dbfs(peak);
		if (o->plus_adc_peak_dbfs > o->plus_adc_max_peak_dbfs)
			o->plus_adc_max_peak_dbfs = o->plus_adc_peak_dbfs;
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
			if (o->plus_rx_native[i] == INT16_MAX || o->plus_rx_native[i] == INT16_MIN)
				o->plus_adc_rail_samples++;
	}
	if (o->rxsquelchdelay) {
		/* Preserve usbradio's millisecond delay while running at the CM119 rate.
		 * Detection remains on the current block so delayed audio loses its tail. */
		const unsigned int delay_samples = o->rxsquelchdelay * (URP_RATE_NATIVE / 1000);
		if (o->plus_rx_delay_index >= delay_samples) o->plus_rx_delay_index = 0;
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			short delayed = o->plus_rx_delay[o->plus_rx_delay_index];
			o->plus_rx_delay[o->plus_rx_delay_index] = o->plus_rx_native[i];
			o->plus_rx_native[i] = delayed;
			if (++o->plus_rx_delay_index == delay_samples) o->plus_rx_delay_index = 0;
		}
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		o->plus_local_native[i] = o->plus_rx_native[i];
	}
	{
		struct txagc_config receive_cfg;
		double rx_high = o->plus_rxhpf_exact ? o->plus_rxhpf_hz
			: usbradioplus_legacy_cutoff("rxhpf", o->rxhpf);
		double rx_low = o->plus_rxlpf_exact ? o->plus_rxlpf_hz
			: usbradioplus_legacy_cutoff("rxlpf", o->rxlpf);
		if (o->plus_rxhpf_enabled && o->plus_rxlpf_enabled
			&& rx_high >= rx_low) {
			ast_log(LOG_ERROR, "RadioPlus/%s: rxhpf cutoff must be below rxlpf cutoff\n",
				o->name);
			memset(o->plus_local_native, 0, sizeof(o->plus_local_native));
			return;
		}
		/* Keep de-emphasis separate so RNNoise can run immediately after the
		 * receiver gate and before any optional dynamics. */
		memset(&receive_cfg, 0, sizeof(receive_cfg));
		receive_cfg.deemphasis_enabled = o->rxdemod == RX_AUDIO_FLAT;
		receive_cfg.emphasis_corner_hz = o->plus_emphasis_corner_hz;
		receive_cfg.emphasis_reference_hz = 1000.0;
		receive_cfg.stage_count = 0;
		if (txagc_avfilter_process(&o->plus_rx_filter, &receive_cfg,
				o->plus_local_native, URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: receive de-emphasis failed\n", o->name);
	}
	if (o->plus_rxlevel_active) {
		unsigned int peak = (unsigned int) fmin(32768.0,
			plus_peak_double(o->plus_local_native, URP_NATIVE_SAMPLES));
		unsigned int raw_peak = plus_peak(o->plus_rx_native, URP_NATIVE_SAMPLES);
		if (plus_count_adc_rails(o, o->plus_rx_native, URP_NATIVE_SAMPLES)) {
			o->plus_rxlevel_raw_clip_frames++;
		}
		if (o->rxkeyed) {
			if (peak > o->plus_rxlevel_signal_peak) o->plus_rxlevel_signal_peak = peak;
			o->plus_rxlevel_signal_samples += URP_NATIVE_SAMPLES;
		} else {
			if (raw_peak > o->plus_rxlevel_noise_peak) o->plus_rxlevel_noise_peak = raw_peak;
			o->plus_rxlevel_noise_samples += URP_NATIVE_SAMPLES;
		}
		if (o->plus_rxlevel_frames_left && !--o->plus_rxlevel_frames_left) {
			usbradioplus_rxlevel_finish(o);
		}
	}
	{
		/* Pre-squelch gain is part of both the detector copy and the
		 * recovered program path.  Post-squelch gain must never raise idle
		 * receiver noise; apply it only after COR/CTCSS have qualified audio. */
		double program_gain_db = 20.0 * log10(fmax(0.000001,
			2.0 * o->rxvoiceadj)) + o->plus_presquelch_gain_db
			+ (local_chain_enabled && o->rxkeyed
				? o->plus_postsquelch_gain_db : 0.0);
		double gain = pow(10.0, program_gain_db / 20.0);
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			o->plus_local_native[i] *= gain;
		}
	}
	memcpy(parrot_raw, o->plus_local_native, sizeof(parrot_raw));
	if (local_chain_enabled && o->rxkeyed) {
		/* De-emphasis and the selected fixed receive filter have already run,
		 * and rxkeyed is the squelch gate. RNNoise is therefore always the
		 * first optional local stage and feeds one intact FFmpeg graph. */
		if (chain.rnnoise_enabled
			&& txagc_rnnoise_process_double(&o->plus_local_rnnoise,
				o->plus_local_native, URP_NATIVE_SAMPLES, URP_RATE_NATIVE)) {
			ast_log(LOG_WARNING, "RadioPlus/%s: local RNNoise processing failed\n",
				o->name);
		}
		if (!chain.rnnoise_enabled) txagc_rnnoise_bypass(&o->plus_local_rnnoise);
		chain.agc.deemphasis_enabled = 0;
		chain.agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
		if (txagc_avfilter_process(&o->plus_local_avfilter, &chain.agc,
				o->plus_local_native, URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: local dynamics processing failed\n",
				o->name);
	} else {
		txagc_rnnoise_bypass(&o->plus_local_rnnoise);
	}
	{
		struct txagc_config filter_cfg;
		double rx_high = o->plus_rxhpf_exact ? o->plus_rxhpf_hz
			: usbradioplus_legacy_cutoff("rxhpf", o->rxhpf);
		double rx_low = o->plus_rxlpf_exact ? o->plus_rxlpf_hz
			: usbradioplus_legacy_cutoff("rxlpf", o->rxlpf);
		memset(&filter_cfg, 0, sizeof(filter_cfg));
		filter_cfg.ctcss_notch_width_hz = 2.0;
		filter_cfg.ctcss_highpass_hz = rx_high;
		filter_cfg.ctcss_filter_mode = o->plus_rxhpf_enabled
			? TXAGC_CTCSS_FILTER_HIGHPASS : TXAGC_CTCSS_FILTER_DISABLED;
		if (local_chain_enabled
			&& chain.agc.ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH) {
			filter_cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
			filter_cfg.ctcss_notch_width_hz = chain.agc.ctcss_notch_width_hz;
			ast_copy_string(filter_cfg.ctcss_notch_frequencies, o->rxctcssfreqs,
				sizeof(filter_cfg.ctcss_notch_frequencies));
		}
		filter_cfg.splatter_filter_enabled = o->plus_rxlpf_enabled;
		filter_cfg.output_lowpass_hz = rx_low;
		if (txagc_avfilter_process(&o->plus_rx_filter_after, &filter_cfg,
				o->plus_local_native, URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0)
			ast_log(LOG_WARNING, "RadioPlus/%s: fixed receive filter failed\n", o->name);
	}

	/* The network copy is made from the conditioned local signal, with its
	 * own optional CTCSS-rejection high-pass, then converted exactly once. */
	memcpy(program, o->plus_local_native, sizeof(program));
	urp_biquad_process_double(&o->plus_link_hpf, program, URP_NATIVE_SAMPLES);
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		network_program[i] = plus_apply_gain((short) fmax(-32768.0,
			fmin(32767.0, program[i])), 1.0);
	}
	if (urp_rate_convert(o->plus_down, network_program, URP_NATIVE_SAMPLES,
		URP_RATE_NATIVE,
		(short *) (o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
		o->plus_app_rpt_samples, o->plus_app_rpt_rate, &used, &made)) {
		o->plus_src_errors++;
	}

	memset(o->plus_link_native, 0, sizeof(o->plus_link_native));
	/* app_rpt and the CM119 use independent clocks. Convert queued frames into
	 * an elastic native-rate FIFO and trim the ratio gently around its target. */
	while (o->plus_link_native_count < PLUS_LINK_NATIVE_TARGET_SAMPLES) {
		unsigned int queued_frames;
		double correction, ratio;
		int have_frame = 0;
		memset(o->plus_link_8k, 0, sizeof(o->plus_link_8k));
		ast_mutex_lock(&o->plus_link_lock);
		queued_frames = o->plus_link_queue_count;
		if (queued_frames) {
			memcpy(o->plus_link_8k,
				o->plus_link_queue[o->plus_link_queue_head],
				o->plus_app_rpt_samples * sizeof(*o->plus_link_8k));
			o->plus_link_queue_head = (o->plus_link_queue_head + 1)
				% PLUS_LINK_QUEUE_FRAMES;
			o->plus_link_queue_count--;
			have_frame = 1;
		}
		ast_mutex_unlock(&o->plus_link_lock);
		if (!have_frame) break;
		correction = urp_clock_recovery_update(&o->plus_link_clock,
			o->plus_link_native_count + queued_frames * URP_NATIVE_SAMPLES,
			PLUS_LINK_NATIVE_TARGET_SAMPLES);
		if (o->plus_app_rpt_rate == URP_RATE_NATIVE) {
			plus_link_native_push(o, o->plus_link_8k,
				o->plus_app_rpt_samples);
			continue;
		}
		ratio = (double) URP_RATE_NATIVE / o->plus_app_rpt_rate
			* (1.0 + correction);
		used = made = 0;
		if (urp_src_process(o->plus_up, o->plus_link_8k,
			o->plus_app_rpt_samples, o->plus_link_resampled,
			sizeof(o->plus_link_resampled) / sizeof(o->plus_link_resampled[0]),
			ratio, &used, &made) || used != o->plus_app_rpt_samples) {
			o->plus_src_errors++;
			break;
		}
		plus_link_native_push(o, o->plus_link_resampled, made);
	}
	if (!o->plus_link_native_primed
		&& o->plus_link_native_count >= PLUS_LINK_NATIVE_TARGET_SAMPLES) {
		o->plus_link_native_primed = 1;
	}
	if (o->plus_link_native_primed
		&& !plus_link_native_pop(o, o->plus_link_native)) {
		o->plus_link_native_primed = 0;
		o->plus_link_queue_underflows++;
		urp_src_reset(o->plus_up);
		urp_clock_recovery_reset(&o->plus_link_clock);
		o->plus_link_native_head = o->plus_link_native_count = 0;
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		program[i] = o->plus_link_native[i];
	}
	memset(local_program, 0, sizeof(local_program));

	if (o->plus_parrot_playing) {
		size_t remaining = o->plus_parrot_count - o->plus_parrot_play;
		size_t count = remaining < URP_NATIVE_SAMPLES ? remaining : URP_NATIVE_SAMPLES;
		memcpy(local_program,
			(o->plus_parrot_play_raw ? o->plus_parrot_raw : o->plus_parrot)
				+ o->plus_parrot_play,
			count * sizeof(double));
		o->plus_parrot_playback_frames++;
		o->plus_parrot_play += count;
		if (o->plus_parrot_play >= o->plus_parrot_count) {
			o->plus_parrot_playing = 0;
		}
	} else if (o->rxkeyed && o->duplex3 > 0
		&& o->duplex3mode == DUPLEX3_MODE_SOFTWARE) {
		double duplex3_gain = (double) o->duplex3 / DUPLEX3_LEVEL_MAX;
		urp_native_repeat_prepare(local_program, o->plus_local_native,
			URP_NATIVE_SAMPLES, duplex3_gain,
			o->usedtmf && o->dsp && o->toneflag);
		if (o->plus_parrot_enabled && o->plus_parrot
			&& o->plus_parrot_count < o->plus_parrot_capacity) {
			size_t space = o->plus_parrot_capacity - o->plus_parrot_count;
			size_t count = space < URP_NATIVE_SAMPLES ? space : URP_NATIVE_SAMPLES;
			memcpy(o->plus_parrot + o->plus_parrot_count,
				o->plus_local_native, count * sizeof(double));
			memcpy(o->plus_parrot_raw + o->plus_parrot_count,
				parrot_raw, count * sizeof(double));
			o->plus_parrot_count += count;
			if (count != URP_NATIVE_SAMPLES) {
				o->plus_parrot_truncated = 1;
			}
		}
	}
	{
		double peak = plus_peak_double(local_program, URP_NATIVE_SAMPLES);
		o->plus_preemphasis_input_peak_dbfs = peak > 0.0
			? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_preemphasis_input_peak_dbfs >
			o->plus_preemphasis_input_max_peak_dbfs)
			o->plus_preemphasis_input_max_peak_dbfs =
				o->plus_preemphasis_input_peak_dbfs;
	}
	if (!local_chain_enabled) {
		double gain = pow(10.0, o->plus_postsquelch_gain_db / 20.0);
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			local_program[i] *= gain;
		}
	}
	/* Keep unlimited floating-point headroom through preemphasis, mixing, and
	 * transmit high-pass filtering. Low-frequency energy that will be removed
	 * must never hit a ceiling first and create broadband clipping products. */
	{
		double peak = plus_peak_double(local_program, URP_NATIVE_SAMPLES);
		o->plus_local_tx_peak_dbfs = peak > 0.0
			? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_local_tx_peak_dbfs > o->plus_local_tx_max_peak_dbfs) {
			o->plus_local_tx_max_peak_dbfs = o->plus_local_tx_peak_dbfs;
		}
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			program[i] += local_program[i];
		}
	}
	urp_biquad_process_double(&o->plus_tx_hpf, program, URP_NATIVE_SAMPLES);
	{
		struct txagc_config final_cfg;
		struct txagc_chain composite_chain;

		if (!usbradioplus_processing_get_composite(&composite_chain)
				&& composite_chain.enabled) {
			final_cfg = composite_chain.agc;
		} else {
			memset(&final_cfg, 0, sizeof(final_cfg));
			final_cfg.input_gain_db = 6.0;
			final_cfg.sidechain_highpass_hz = 800.0;
			final_cfg.sidechain_lowpass_hz = 1500.0;
			final_cfg.expander_sidechain_highpass_hz = 800.0;
			final_cfg.expander_sidechain_lowpass_hz = 1500.0;
			final_cfg.compressor_sidechain_highpass_hz = 300.0;
			final_cfg.compressor_sidechain_lowpass_hz = 1500.0;
			final_cfg.attack_ms = final_cfg.release_ms = final_cfg.reset_after_ms = 100.0;
			final_cfg.expander_attack_ms = final_cfg.expander_release_ms = 100.0;
			final_cfg.compressor_attack_ms = final_cfg.compressor_release_ms = 100.0;
			final_cfg.expander_ratio = final_cfg.compressor_ratio = 1.0;
			final_cfg.low_limiter_ratio = final_cfg.high_limiter_ratio = 1.0;
			final_cfg.low_limiter_attack_ms = final_cfg.low_limiter_release_ms = 10.0;
			final_cfg.high_limiter_attack_ms = final_cfg.high_limiter_release_ms = 10.0;
			final_cfg.limiter_crossover_hz = 1000.0;
			final_cfg.output_highpass_hz = 300.0;
			final_cfg.output_lowpass_hz = 3000.0;
			final_cfg.splatter_filter_enabled = 1;
			final_cfg.lookahead_limiter_enabled = 0;
			final_cfg.lookahead_limit_dbfs = -3.0;
			final_cfg.lookahead_ms = 5.0;
			final_cfg.lookahead_attack_ms = 1.0;
			final_cfg.lookahead_release_ms = 100.0;
		}
		/* Pre-emphasis belongs to the native composite graph regardless of which
		 * source supplied the audio. */
		final_cfg.preemphasis_enabled = o->txprelim && !o->txlimonly;
		final_cfg.emphasis_corner_hz = o->plus_emphasis_corner_hz;
		final_cfg.emphasis_reference_hz = 1000.0;
		/* Preserve txboost as a relative 6 dB increase above the native
		 * transmitter chain's established baseline. */
		if (o->txboost) final_cfg.input_gain_db += 6.0;
		final_cfg.splatter_filter_enabled =
			o->plus_txhpf_enabled || o->plus_txlpf_enabled;
		final_cfg.output_highpass_hz = o->plus_txhpf_enabled
			? (o->plus_txhpf_exact ? o->plus_txhpf_hz
				: usbradioplus_legacy_cutoff("txhpf", o->txhpf)) : 0.0;
		final_cfg.output_lowpass_hz = o->plus_txlpf_enabled
			? (o->plus_txlpf_exact ? o->plus_txlpf_hz
				: usbradioplus_legacy_cutoff("txlpf", o->txlpf)) : 0.0;
		/* A configured RadioPlus yes/no is authoritative.  Only an omitted
		 * setting inherits txprelim, txlimonly, and txslimsp semantics. */
		if (!composite_chain.lookahead_limiter_configured
			&& (o->txprelim || o->txlimonly)) {
			final_cfg.lookahead_limiter_enabled = 1;
			final_cfg.lookahead_limit_dbfs =
				urp_legacy_limiter_ceiling_dbfs(o->txslimsp);
		}
		if (final_cfg.output_highpass_hz > 0.0
			&& final_cfg.output_lowpass_hz > 0.0
			&& final_cfg.output_highpass_hz >= final_cfg.output_lowpass_hz) {
			ast_log(LOG_ERROR, "RadioPlus/%s: txhpf cutoff must be below txlpf cutoff\n",
				o->name);
			memset(program, 0, sizeof(program));
			return;
		}
		if (txagc_avfilter_process(&o->plus_final_avfilter, &final_cfg,
				program, URP_NATIVE_SAMPLES, URP_RATE_NATIVE) < 0) {
			txagc_core_process_double(&o->plus_final_core, &final_cfg,
				program, URP_NATIVE_SAMPLES, URP_RATE_NATIVE);
		}
	}
	/* Match the established deviation reference: bypass voice dynamics and emphasis,
	 * but retain the final PCM ceiling and configured output routing. */
	if (o->plus_test_tone_enabled) {
		const double step = 2.0 * M_PI * 1000.0 / URP_RATE_NATIVE;
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			/* The legacy 59/256 generator gain followed by its 8-to-48 kHz FIR
			 * produces exactly +/-7518 PCM codes at steady-state. */
			program[i] = URP_LEGACY_TEST_TONE_PEAK * sin(o->plus_test_tone_phase);
			o->plus_test_tone_phase += step;
			if (o->plus_test_tone_phase >= 2.0 * M_PI)
				o->plus_test_tone_phase -= 2.0 * M_PI;
		}
	} else {
		o->plus_test_tone_phase = 0.0;
	}
	/* This is the sole native transmitter ceiling: after all filtering and
	 * immediately before conversion to CM119 integer PCM. */
	o->plus_tx_program_rail_samples += plus_ceiling_dbfs(program,
		URP_NATIVE_SAMPLES, o->plus_tx_ceiling_dbfs);
	{
		double peak = plus_peak_double(program, URP_NATIVE_SAMPLES);
		o->plus_tx_program_peak_dbfs = peak > 0.0
			? 20.0 * log10(peak / 32768.0) : -INFINITY;
		if (o->plus_tx_program_peak_dbfs > o->plus_tx_program_max_peak_dbfs) {
			o->plus_tx_program_max_peak_dbfs = o->plus_tx_program_peak_dbfs;
		}
	}
	{
		short stats_stereo[URP_NATIVE_SAMPLES * 2];
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
			short sample = (short) lrint(fmax(-32767.0,
				fmin(32766.0, program[i])));
			stats_stereo[i * 2] = sample;
			stats_stereo[i * 2 + 1] = sample;
		}
		/* Meter program audio before CM119 mixer gain, regardless of whether
		 * the configured voice/composite output is channel A or channel B. */
		ast_radio_check_audio(stats_stereo, &o->txaudiostats,
			URP_NATIVE_SAMPLES * 2);
	}
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		/* Quantize once, at the CM119 boundary. Reserve one code at each rail
		 * so valid transmitter PCM can never be mistaken for hardware clipping. */
		short output = (short) lrint(fmax(-32767.0,
			fmin(32766.0, program[i])));
		if (plus_mix_has_program(o->txmixa)) {
			stereo[i * 2] = plus_saturating_add(stereo[i * 2], output);
		}
		if (plus_mix_has_program(o->txmixb)) {
			stereo[i * 2 + 1] = plus_saturating_add(stereo[i * 2 + 1], output);
		}
		if (o->txmixa == TX_OUT_LSD || o->txmixa == TX_OUT_COMPOSITE) {
			short tone = (short) lrint(fmax(-32767.0, fmin(32767.0,
				ctcss[i] * ctcss_peak_a + ctcss_bias_a)));
			stereo[i * 2] = plus_saturating_add(stereo[i * 2], tone);
		}
		if (o->txmixb == TX_OUT_LSD || o->txmixb == TX_OUT_COMPOSITE) {
			short tone = (short) lrint(fmax(-32767.0, fmin(32767.0,
				ctcss[i] * ctcss_peak_b + ctcss_bias_b)));
			stereo[i * 2 + 1] = plus_saturating_add(stereo[i * 2 + 1], tone);
		}
	}
	o->plus_native_frames++;
}

static void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed)
{
	if (!was_keyed && o->rxkeyed) {
		o->plus_parrot_count = 0;
		o->plus_parrot_play = 0;
		o->plus_parrot_playing = 0;
		o->plus_parrot_play_raw = 0;
		o->plus_parrot_truncated = 0;
	} else if (was_keyed && !o->rxkeyed && o->plus_parrot_count) {
		o->plus_parrot_play = 0;
		o->plus_parrot_play_raw = 0;
		o->plus_parrot_playing = 1;
		ast_log(LOG_NOTICE, "RadioPlus/%s: replaying %.2f seconds of native parrot audio%s\n",
			o->name, (double) o->plus_parrot_count / URP_RATE_NATIVE,
			o->plus_parrot_truncated ? " (truncated)" : "");
	}
}

static char *handle_radioplus_parrot(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;
	int enable;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus parrot";
		e->usage = "Usage: radioplus parrot [on|off|toggle|clear|status]\n"
			"       radioplus parrot replay <raw|processed>\n"
			"       Control native 48 kHz local-only receiver playback.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	o = find_desc(usbradio_active);
	if (!o) {
		ast_cli(a->fd, "No active RadioPlus channel.\n");
		return CLI_FAILURE;
	}
	if (a->argc == 2 || (a->argc == 3 && !strcasecmp(a->argv[2], "status"))) {
		ast_cli(a->fd, "Native parrot is %s; buffered %.2f seconds; playback %s%s.\n",
			o->plus_parrot_enabled ? "on" : "off",
			(double) o->plus_parrot_count / URP_RATE_NATIVE,
			o->plus_parrot_playing ? "active" : "idle",
			o->plus_parrot_truncated ? "; recording truncated" : "");
		return CLI_SUCCESS;
	}
	if (a->argc == 4 && !strcasecmp(a->argv[2], "replay")) {
		if (!o->plus_parrot_count) {
			ast_cli(a->fd, "No native parrot recording is buffered.\n");
			return CLI_FAILURE;
		}
		if (!strcasecmp(a->argv[3], "raw")) {
			o->plus_parrot_play_raw = 1;
		} else if (!strcasecmp(a->argv[3], "processed")) {
			o->plus_parrot_play_raw = 0;
		} else {
			return CLI_SHOWUSAGE;
		}
		o->plus_parrot_play = 0;
		o->plus_parrot_playing = 1;
		ast_cli(a->fd, "Replaying %s native parrot capture.\n",
			o->plus_parrot_play_raw ? "raw pre-dynamics" : "processed");
		return CLI_SUCCESS;
	}
	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	if (!strcasecmp(a->argv[2], "clear")) {
		o->plus_parrot_count = o->plus_parrot_play = 0;
		o->plus_parrot_playing = o->plus_parrot_truncated = 0;
		ast_cli(a->fd, "Native parrot buffer cleared.\n");
		return CLI_SUCCESS;
	}
	if (!strcasecmp(a->argv[2], "toggle")) {
		enable = !o->plus_parrot_enabled;
	} else if (ast_true(a->argv[2])) {
		enable = 1;
	} else if (ast_false(a->argv[2])) {
		enable = 0;
	} else {
		return CLI_SHOWUSAGE;
	}
	if (enable && !o->plus_parrot) {
		o->plus_parrot_capacity = (size_t) o->plus_parrot_max_seconds * URP_RATE_NATIVE;
		o->plus_parrot = ast_calloc(o->plus_parrot_capacity, sizeof(*o->plus_parrot));
		o->plus_parrot_raw = ast_calloc(o->plus_parrot_capacity,
			sizeof(*o->plus_parrot_raw));
		if (!o->plus_parrot || !o->plus_parrot_raw) {
			ast_free(o->plus_parrot);
			ast_free(o->plus_parrot_raw);
			o->plus_parrot = o->plus_parrot_raw = NULL;
			ast_cli(a->fd, "Unable to allocate native parrot buffer.\n");
			return CLI_FAILURE;
		}
	}
	o->plus_parrot_enabled = enable;
	if (!enable) {
		o->plus_parrot_playing = 0;
	}
	ast_cli(a->fd, "Native parrot is now %s.\n", enable ? "on" : "off");
	return CLI_SUCCESS;
}

static char *handle_radioplus_rxlevel(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;
	unsigned int seconds;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus rxlevel";
		e->usage = "Usage: radioplus rxlevel [status|calibrate [seconds]|cancel|set pre|post dB|set capture 0-999|save]\n"
			"       Deterministically calibrate native pre/post-squelch gain.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	o = find_desc(usbradio_active);
	if (!o) return CLI_FAILURE;
	if (a->argc == 2 || (a->argc == 3 && !strcasecmp(a->argv[2], "status"))) {
		ast_cli(a->fd, "RX level calibration: %s; pre %.2f dB; post %.2f dB; noise peak %.2f dBFS (%" PRIu64 " samples); signal peak %.2f dBFS (%" PRIu64 " samples); ADC rails +%" PRIu64 "/-%" PRIu64 " samples in %u frames.\n",
			o->plus_rxlevel_active ? "running" : "idle",
			o->plus_presquelch_gain_db, o->plus_postsquelch_gain_db,
			plus_peak_dbfs(o->plus_rxlevel_noise_peak), o->plus_rxlevel_noise_samples,
			plus_peak_dbfs(o->plus_rxlevel_signal_peak), o->plus_rxlevel_signal_samples,
			o->plus_rxlevel_positive_rail_samples, o->plus_rxlevel_negative_rail_samples,
			o->plus_rxlevel_raw_clip_frames);
		return CLI_SUCCESS;
	}
	if (a->argc == 3 && !strcasecmp(a->argv[2], "cancel")) {
		o->plus_rxlevel_active = 0;
		ast_cli(a->fd, "RX level calibration cancelled; gains unchanged.\n");
		return CLI_SUCCESS;
	}
	if (a->argc == 3 && !strcasecmp(a->argv[2], "save")) {
		tune_write(o);
		ast_cli(a->fd, "RX pre/post-squelch gains saved to %s.\n", CONFIG);
		return CLI_SUCCESS;
	}
	if ((a->argc == 3 || a->argc == 4) && !strcasecmp(a->argv[2], "calibrate")) {
		seconds = a->argc == 4 ? strtoul(a->argv[3], NULL, 10) : 30;
		if (seconds < 2 || seconds > 600) return CLI_SHOWUSAGE;
		o->plus_rxlevel_noise_samples = o->plus_rxlevel_signal_samples = 0;
		o->plus_rxlevel_noise_peak = o->plus_rxlevel_signal_peak = 0;
		o->plus_rxlevel_positive_rail_samples = o->plus_rxlevel_negative_rail_samples = 0;
		o->plus_rxlevel_raw_clip_frames = 0;
		o->plus_rxlevel_frames_left = seconds * 50;
		o->plus_rxlevel_active = 1;
		ast_cli(a->fd, "RX level calibration started for %u seconds; include receiver noise and at least one second of COR-qualified speech.\n", seconds);
		return CLI_SUCCESS;
	}
	if (a->argc == 5 && !strcasecmp(a->argv[2], "set")) {
		double value = strtod(a->argv[4], NULL);
		if (!strcasecmp(a->argv[3], "capture")) {
			unsigned int setting = strtoul(a->argv[4], NULL, 10);
			int adjustment;
			if (setting > 999) return CLI_SHOWUSAGE;
			o->rxmixerset = setting;
			adjustment = o->rxmixerset * o->micmax / AUDIO_ADJUSTMENT;
			ast_radio_setamixer(o->devicenum, MIXER_PARAM_MIC_CAPTURE_VOL,
				adjustment, 0);
			ast_cli(a->fd, "CM119 capture set to %u/999 (hardware step %d/%d).\n",
				setting, adjustment, o->micmax);
			return CLI_SUCCESS;
		}
		if (value < -30.0 || value > 30.0) return CLI_SHOWUSAGE;
		if (!strcasecmp(a->argv[3], "pre")) o->plus_presquelch_gain_db = value;
		else if (!strcasecmp(a->argv[3], "post")) o->plus_postsquelch_gain_db = value;
		else return CLI_SHOWUSAGE;
		ast_cli(a->fd, "RX gains now pre %.2f dB, post %.2f dB.\n",
			o->plus_presquelch_gain_db, o->plus_postsquelch_gain_db);
		return CLI_SUCCESS;
	}
	return CLI_SHOWUSAGE;
}

static char *handle_radioplus_native_stats(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct chan_usbradio_pvt *o;
	switch (cmd) {
	case CLI_INIT:
		e->command = "radioplus native stats";
		e->usage = "Usage: radioplus native stats [reset]\n"
			"       Show native 48 kHz RadioPlus measurements.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 3 && a->argc != 4) return CLI_SHOWUSAGE;
	o = find_desc(usbradio_active);
	if (!o) return CLI_FAILURE;
	if (a->argc == 4) {
		if (strcasecmp(a->argv[3], "reset")) return CLI_SHOWUSAGE;
		o->plus_tx_program_peak_dbfs = -INFINITY;
		o->plus_tx_program_max_peak_dbfs = -INFINITY;
		o->plus_tx_program_rail_samples = 0;
		o->plus_local_tx_peak_dbfs = -INFINITY;
		o->plus_local_tx_max_peak_dbfs = -INFINITY;
		o->plus_local_tx_rail_samples = 0;
		o->plus_adc_peak_dbfs = o->plus_adc_max_peak_dbfs = -INFINITY;
		o->plus_adc_rail_samples = 0;
		o->plus_deemphasis_peak_dbfs = o->plus_deemphasis_max_peak_dbfs = -INFINITY;
		o->plus_preemphasis_input_peak_dbfs =
			o->plus_preemphasis_input_max_peak_dbfs = -INFINITY;
		o->plus_preemphasis_input_ceiling_samples = 0;
		o->plus_link_queue_underflows = o->plus_link_queue_overflows = 0;
		o->plus_sound_zero_fill_frames = 0;
		o->plus_sound_dropped_frames = 0;
		o->plus_sound_short_writes = 0;
		o->plus_parrot_playback_frames = 0;
		o->plus_final_core.lookahead_limited_samples = 0;
		o->plus_local_avfilter.input_max_peak_dbfs = -INFINITY;
		o->plus_local_avfilter.input_max_rms_dbfs = -INFINITY;
		o->plus_local_avfilter.output_max_peak_dbfs = -INFINITY;
		o->plus_local_avfilter.output_max_rms_dbfs = -INFINITY;
		o->plus_local_avfilter.runtime_underrun_samples = 0;
		o->plus_final_avfilter.input_max_peak_dbfs = -INFINITY;
		o->plus_final_avfilter.input_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.output_max_peak_dbfs = -INFINITY;
			o->plus_final_avfilter.output_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_pre_max_peak_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_pre_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_pre_5_8_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_pre_8_plus_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_post_5_8_max_rms_dbfs = -INFINITY;
			o->plus_final_avfilter.cleanup_post_8_plus_max_rms_dbfs = -INFINITY;
		o->plus_final_avfilter.runtime_underrun_samples = 0;
		o->plus_link_queue_high_water = o->plus_link_queue_count;
		ast_cli(a->fd, "Native peak and FIFO event counters reset.\n");
		return CLI_SUCCESS;
	}
	ast_cli(a->fd, "RadioPlus/%s native: frames %" PRIu64 ", SRC errors %" PRIu64
		", ADC peak %.1f/max %.1f dBFS, ADC rails %" PRIu64
		", deemphasis peak %.1f/max %.1f dBFS"
		", preemphasis input %.1f/max %.1f dBFS, input ceiling interventions %" PRIu64
		", local TX peak %.1f dBFS, local TX max %.1f dBFS, local ceiling interventions %" PRIu64
		", final TX peak %.1f dBFS, final TX max %.1f dBFS, final lookahead limited %" PRIu64
		", final ceiling interventions %" PRIu64
		", pre gain %.2f dB, post gain %.2f dB, FIFO %u/%u (high %u, underruns %" PRIu64 ", overruns %" PRIu64
		"), sound queue zero-fills %" PRIu64 ", dropped frames %" PRIu64 ", short/errors %" PRIu64
		", parrot %s/%s, playback frames %" PRIu64 ", buffered %.2f seconds.\n",
		o->name, o->plus_native_frames, o->plus_src_errors,
		o->plus_adc_peak_dbfs, o->plus_adc_max_peak_dbfs, o->plus_adc_rail_samples,
		o->plus_deemphasis_peak_dbfs, o->plus_deemphasis_max_peak_dbfs,
		o->plus_preemphasis_input_peak_dbfs,
		o->plus_preemphasis_input_max_peak_dbfs,
		o->plus_preemphasis_input_ceiling_samples,
		o->plus_local_tx_peak_dbfs,
		o->plus_local_tx_max_peak_dbfs, o->plus_local_tx_rail_samples,
		o->plus_tx_program_peak_dbfs,
		o->plus_tx_program_max_peak_dbfs,
		o->plus_final_core.lookahead_limited_samples,
		o->plus_tx_program_rail_samples,
		o->plus_presquelch_gain_db,
		o->plus_postsquelch_gain_db, o->plus_link_queue_count,
		PLUS_LINK_QUEUE_FRAMES, o->plus_link_queue_high_water,
		o->plus_link_queue_underflows, o->plus_link_queue_overflows,
		o->plus_sound_zero_fill_frames, o->plus_sound_dropped_frames,
		o->plus_sound_short_writes,
		o->plus_parrot_enabled ? "on" : "off",
		o->plus_parrot_playing ? "playing" : "idle",
		o->plus_parrot_playback_frames,
		(double) o->plus_parrot_count / URP_RATE_NATIVE);
	ast_cli(a->fd,
		"Link clock recovery: app FIFO %u frames, native FIFO %u samples/%.2f ms, "
		"ratio correction %+.4f%%.\n",
		o->plus_link_queue_count, o->plus_link_native_count,
		1000.0 * o->plus_link_native_count / URP_RATE_NATIVE,
		100.0 * o->plus_link_clock.correction);
	ast_cli(a->fd,
		"FFmpeg local: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		o->plus_local_avfilter.input_peak_dbfs,
		o->plus_local_avfilter.input_max_peak_dbfs,
		o->plus_local_avfilter.input_rms_dbfs,
		o->plus_local_avfilter.input_max_rms_dbfs,
		o->plus_local_avfilter.output_peak_dbfs,
		o->plus_local_avfilter.output_max_peak_dbfs,
		o->plus_local_avfilter.output_rms_dbfs,
		o->plus_local_avfilter.output_max_rms_dbfs,
		o->plus_local_avfilter.latency_samples,
		1000.0 * o->plus_local_avfilter.latency_samples / URP_RATE_NATIVE,
		o->plus_local_avfilter.buffered_samples,
		o->plus_local_avfilter.startup_fill_samples,
		o->plus_local_avfilter.runtime_underrun_samples);
	ast_cli(a->fd,
		"FFmpeg final: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		o->plus_final_avfilter.input_peak_dbfs,
		o->plus_final_avfilter.input_max_peak_dbfs,
		o->plus_final_avfilter.input_rms_dbfs,
		o->plus_final_avfilter.input_max_rms_dbfs,
		o->plus_final_avfilter.output_peak_dbfs,
		o->plus_final_avfilter.output_max_peak_dbfs,
		o->plus_final_avfilter.output_rms_dbfs,
		o->plus_final_avfilter.output_max_rms_dbfs,
		o->plus_final_avfilter.latency_samples,
		1000.0 * o->plus_final_avfilter.latency_samples / URP_RATE_NATIVE,
		o->plus_final_avfilter.buffered_samples,
		o->plus_final_avfilter.startup_fill_samples,
		o->plus_final_avfilter.runtime_underrun_samples);
	ast_cli(a->fd,
		"FFmpeg final cleanup: pre-filter peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"5-8 kHz pre %.1f/max %.1f, post %.1f/max %.1f dBFS; "
		">8 kHz pre %.1f/max %.1f, post %.1f/max %.1f dBFS.\n",
		o->plus_final_avfilter.cleanup_pre_peak_dbfs,
		o->plus_final_avfilter.cleanup_pre_max_peak_dbfs,
		o->plus_final_avfilter.cleanup_pre_rms_dbfs,
		o->plus_final_avfilter.cleanup_pre_max_rms_dbfs,
		o->plus_final_avfilter.cleanup_pre_5_8_rms_dbfs,
		o->plus_final_avfilter.cleanup_pre_5_8_max_rms_dbfs,
		o->plus_final_avfilter.cleanup_post_5_8_rms_dbfs,
		o->plus_final_avfilter.cleanup_post_5_8_max_rms_dbfs,
		o->plus_final_avfilter.cleanup_pre_8_plus_rms_dbfs,
		o->plus_final_avfilter.cleanup_pre_8_plus_max_rms_dbfs,
		o->plus_final_avfilter.cleanup_post_8_plus_rms_dbfs,
		o->plus_final_avfilter.cleanup_post_8_plus_max_rms_dbfs);
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_usbradio[] = { AST_CLI_DEFINE(handle_console_key, "Simulate Rx Signal Present"),
	AST_CLI_DEFINE(handle_console_unkey, "Simulate Rx Signal Loss"), AST_CLI_DEFINE(handle_radio_tune, "Change radio settings"),
	AST_CLI_DEFINE(handle_radio_active, "Change commanded device"),
	AST_CLI_DEFINE(handle_set_dsp_debug, "Radio set detector debug level"), AST_CLI_DEFINE(handle_show_settings, "Show device settings"),
	AST_CLI_DEFINE(handle_radioplus_parrot, "Control native local-only parrot playback"),
	AST_CLI_DEFINE(handle_radioplus_rxlevel, "Calibrate native receive levels"),
	AST_CLI_DEFINE(handle_radioplus_native_stats, "Show native RadioPlus statistics") };

#include "usbradioplus_radio.c"
#include "usbradioplus_dsp.c"
#include "usbradioplus_ctcss.c"
#include "usbradioplus_hardware.c"
#include "usbradioplus_repeat.c"
#include "./txagc/agc_core.c"
#include "./txagc/avfilter_processor.c"
#include "./txagc/rnnoise_processor.c"
#include "usbradioplus_processing.c"

/*!
 * \brief Load configuration.
 * \param reload		Flag to indicate if we are reloading.
 * \return				Success or failure.
 */
static int load_config(int reload)
{
	struct ast_config *cfg = NULL;
	char *ctg = NULL;
	const char *val;
	struct ast_flags zeroflag = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	/* load config file */
	if (!(cfg = ast_config_load(CONFIG, zeroflag))) {
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
	val = ast_variable_retrieve(cfg, "general", "pport");
	if (val) {
		ast_copy_string(pport, val, sizeof(pport));
	} else {
		strcpy(pport, PP_PORT);
	}
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

static int reload_module(void)
{
	int result = load_config(1);
	if (!result) {
		result = usbradioplus_processing_reload();
	}
	return result;
}

static int load_module(void)
{
	if (!(usbradio_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT))) {
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

	ast_cli_register_multiple(cli_usbradio, sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));
	if (usbradioplus_processing_load() != AST_MODULE_LOAD_SUCCESS) {
		ast_log(LOG_ERROR, "Unable to start RadioPlus processing engine\n");
		ast_cli_unregister_multiple(cli_usbradio, ARRAY_LEN(cli_usbradio));
		ast_channel_unregister(&usbradio_tech);
		return AST_MODULE_LOAD_FAILURE;
	}

	if (haspp && hasout) {
		ast_pthread_create_background(&pulserid, NULL, pulserthread, NULL);
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	struct chan_usbradio_pvt *o;

	stoppulser = 1;
	usbradioplus_processing_unload();

	ast_channel_unregister(&usbradio_tech);
	ast_cli_unregister_multiple(cli_usbradio, sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));

	for (o = usbradio_default.next; o; o = o->next) {
		usbradioplus_dsp_destroy(o);
		if (o->radio) {
			urp_radio_destroy(o->radio);
		}

#if DEBUG_CAPTURES == 1
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
#endif

		if (o->sounddev >= 0) {
			close(o->sounddev);
			o->sounddev = -1;
		}
		if (o->dsp) {
			ast_dsp_free(o->dsp);
		}
		if (o->owner) {
			ast_softhangup(o->owner, AST_SOFTHANGUP_APPUNLOAD);
		}
		if (o->owner) { /* XXX how ??? */
			return -1;
		}
		/* XXX what about the thread ? */
		/* XXX what about the memory allocated ? */
	}

	ao2_cleanup(usbradio_tech.capabilities);
	usbradio_tech.capabilities = NULL;

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "USB Radio Plus Channel Driver",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.requires = "res_usbradio",
);
