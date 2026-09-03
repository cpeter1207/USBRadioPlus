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
#include <stdarg.h>
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
#define DEBUG_CAPTURES 1
#define DEBUG_CAP_RX_OUT 0
#define DEBUG_CAP_TX_OUT 0
#define DEBUG_FILETEST 0
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
#define DEFAULT_ECHO_SECONDS (DEFAULT_ECHO_MAX / 50)
#define DEFAULT_TX_SOFT_LIMITER_SETPOINT 12000
#define PP_MASK 0xbffc
#define PP_PORT "/dev/parport0"
#define PP_IOPORT 0x378
#define RPT_TO_STRING(x) #x
#define S_FMT(x) "%" RPT_TO_STRING(x) "s "
#define N_FMT(duf) "%30" #duf		       /* Maximum sscanf conversion to numeric strings */
#define RX_ON_DELAY_MAX 60000		       /* in ms, 60000ms, 60 seconds, 1 minute */
#define TX_OFF_DELAY_MAX 60000		       /* in ms 60000ms, 60 seconds, 1 minute */
#define MS_PER_FRAME 20			       /* 20 ms frames */
#define MS_TO_FRAMES(ms) ((ms) / MS_PER_FRAME) /* convert ms to frames */
#define DEVICE_RETRY 500000		       /* Retry time in uS when USB device is missing */

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
#include "usbradioplus_channel_core.h"

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
_Static_assert((int)URP_TONE_OFF_REMOVE == (int)TOC_NOTONE, "tone-off enum mismatch");

/*! \brief Global jitterbuffer configuration - by default, jb is disabled */
static struct ast_jb_conf default_jbconf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
};

static struct ast_jb_conf global_jbconf;

#define URP_LEGACY_TEST_TONE_PEAK 7518.0
#define CONFIG "usbradioplus.conf" /* default config file */

/* file handles for writing debug audio packets */
static FILE *frxcapraw = NULL, *frxcaptrace = NULL, *frxoutraw = NULL;
static FILE *ftxcapraw = NULL, *ftxcaptrace = NULL, *ftxoutraw = NULL;

AST_MUTEX_DEFINE_STATIC(device_swap_lock);
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
static const char *const cd_signal_type[] = {"no",	  "dsp", "vox",	    "usb",
					     "usbinvert", "pp",	 "ppinvert"};
static const char *const sd_signal_type[] = {"no", "usb", "usbinvert", "dsp", "pp", "ppinvert"};

/* Keep the PortAudio TX buffer fed when no outbound audio is available. */
static short silence_buf[AST_RADIO_PA_FRAMES_PER_BUFFER * AST_RADIO_PA_OUTPUT_CHANNELS] = {0};

/*! \brief demodulation type */
static const char *const demodulation_type[] = {"no", "speaker", "flat"};

/*! \brief mixer type */
static const char *const mixer_type[] = {"no", "voice", "tone", "composite", "auxvoice"};

/*!
 * \brief Descriptor for one of our channels.
 * There is one used for 'default' values (from the [general] entry in
 * the configuration file), and then one instance for each device
 * (the default is cloned from [general], others are only created
 * if the relevant section exists).
 */
struct chan_usbradio_pvt {
	struct chan_usbradio_pvt *next;

	char *name;	/* the internal name of our channel */
	int devtype;	/* actual type of device */
	int pttkick[2]; /* ptt kick pipe */
	struct ast_radio_pa_stream pa;
	enum { M_UNSET, M_FULL, M_READ, M_WRITE } duplex;
	int hookstate;

	char devstr[128];
	char serial[128];
	/* Retained as accepted compatibility settings. PortAudio owns buffering in
	 * the shared-device API and therefore does not consume OSS fragment sizes. */
	unsigned int queuesize;
	unsigned int frags;

	pthread_t hidthread;
	pthread_t audiothread;
	int stophid;
	volatile sig_atomic_t stopaudiothread;
	volatile sig_atomic_t hasusb; /* HID/audio liveness; not a bit-field (cross-thread) */
	char audio_thread_ready;
	time_t lastaudiotime;
	enum {
		DEVICE_SWAP_IDLE,      /*!< No device swap requested */
		DEVICE_SWAP_QUIESCING, /*!< Device handles are stopping */
		DEVICE_SWAP_READY,     /*!< Device is ready for lease exchange */
	} swap_state;
	unsigned int swap_audio_ready : 1; /*!< PortAudio stopped for pending swap */
	ast_mutex_t swap_lock;		   /* protects device swap state */

	struct ast_channel *owner;

	/* Shared USB radio device lease */
	struct ast_radio_device *radio_device;
	ast_mutex_t device_lock;
	enum ast_radio_device_result device_error;

	/* One native-rate stereo frame rendered for the PortAudio hardware tick. */
	short usbradio_write_buf[AST_RADIO_PA_48K_STEREO_SAMPLES];
	short plus_rx_native[URP_NATIVE_SAMPLES];
	double plus_local_native[URP_NATIVE_SAMPLES];
	unsigned int plus_app_rpt_rate;
	unsigned int plus_app_rpt_samples;
	short plus_link_native[URP_NATIVE_SAMPLES];
	short plus_link_resampled[URP_NATIVE_SAMPLES * 2];
	struct urp_native_fifo plus_native_fifo;
	short plus_link_8k[URP_NATIVE_SAMPLES];
	struct urp_program_queue plus_program_queue;
	uint64_t plus_link_queue_underflows;
	uint64_t plus_link_queue_overflows;
	ast_mutex_t plus_link_lock;
	short plus_squelch_native[URP_NATIVE_SAMPLES * 2];
	short plus_rx_delay[RXSQDELAYBUFSIZE * 6];
	unsigned int plus_rx_delay_index;
	struct urp_src *plus_up;
	struct urp_src *plus_down;
	struct urp_clock_recovery plus_link_clock;
	unsigned int plus_local_preemphasis_active;
	unsigned int plus_link_preemphasis_active;
	struct txagc_avfilter plus_local_avfilter;
	struct txagc_avfilter plus_rx_filter;
	struct txagc_avfilter plus_rx_filter_after;
	struct txagc_avfilter plus_final_avfilter;
	struct txagc_rnnoise plus_local_rnnoise;
	double plus_emphasis_corner_hz;
	int plus_hardware_applied;
	int plus_applied_rxmixer, plus_applied_txmixaset, plus_applied_txmixbset;
	int plus_applied_txmixa, plus_applied_txmixb;
	char plus_applied_rxctcssfreqs[512], plus_applied_txctcssfreqs[512];
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
	struct urp_parrot_state plus_parrot_state;

	/* buffers used in the audio thread - AST_FRIENDLY_OFFSET space for headers
	 * plus enough room for a full 48 kHz stereo PortAudio frame
	 */
	_Alignas(
		short) char usbradio_read_buf[AST_RADIO_PA_48K_STEREO_SAMPLES * (int)sizeof(short) +
					      AST_FRIENDLY_OFFSET];
	char usbradio_read_buf_8k[URP_NATIVE_SAMPLES * 2 + AST_FRIENDLY_OFFSET];
	int readpos;		 /* read position above */
	struct ast_frame read_f; /* scratch frame used by the audio thread */

	char lastrx;
	char rxhidsq;
	char rxhidctcss;
	char rxcarrierdetect; /* status from native radio detector */
	char rxctcssdecode;   /* status from native CTCSS decoder */
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
	float rxctcssadj;

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
	char set_txctcssfreq[16];    /* encode now */
	char set_rxctcssfreq[16];    /* decode now */

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
	float legacy_rxvoiceadj;
	int legacy_rxvoiceadj_configured;
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
	unsigned int rxcapraw : 1;	  /* indicator if receive capture is enabled */
	unsigned int txcapraw : 1;	  /* indicator if transmit capture is enabled */
	unsigned int rxcap2 : 1;	  /* indicator if receive capture 2 is enabled */
	unsigned int txcap2 : 1;	  /* indicator if transmit capture 2 is enabled */
	unsigned int remoted : 1;	  /* indicator if rx/tx frequency adjusted */
	unsigned int forcetxcode : 1;	  /* indicator to force use of first ctcss code */
	unsigned int rxpolarity : 1;	  /* indicator for receive polarity */
	unsigned int txpolarity : 1;	  /* indicator for transmit polarity */
	unsigned int dcsrxpolarity : 1;	  /* indicator for dcs receive polarity */
	unsigned int dcstxpolarity : 1;	  /* indicator for dcs transmit polarity */
	unsigned int lsdrxpolarity : 1;	  /* indicator for lsd receive polarity */
	unsigned int lsdtxpolarity : 1;	  /* indicator for lsd transmit polarity */
	unsigned int radioactive : 1;	  /* indicator for active radio channel */
	unsigned int wanteeprom : 1;	  /* indicator if we should use EEPROM */
	unsigned int usedtmf : 1;	  /* indicator is we should decode DTMF */
	unsigned int invertptt : 1;	  /* indicator if we need to invert ptt */
	unsigned int rxcpusaver : 1;	  /* indicator if receive cpu save is enabled */
	unsigned int txcpusaver : 1;	  /* indicator if transmit cpu save is enabled */
	unsigned int txprelim : 1;	  /* indicator if tx pre lim is enabled */
	unsigned int txlimonly : 1;	  /* indicator if tx lim only is enabled */
	unsigned int rxctcssoverride : 1; /* indicator if receive ctcss override is enabled */
	unsigned int
		rx_cos_active : 1; /* indicator if cos is active - active state after processing */
	unsigned int rx_ctcss_active : 1; /* indicator if ctcss is active - active state after
					     processing */
	/* Whole-word latch shared by HID/audio paths (not a bit-field). */
	volatile sig_atomic_t
		usb_faulted; /* set after USB/audio failure; cleared on recovery log */

	/* EEPROM access variables */
	unsigned short eeprom[EEPROM_USER_LEN];
	char eepromctl;
	ast_mutex_t eepromlock;

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

	ast_mutex_t usblock;
};

#define plus_parrot plus_parrot_state.audio
#define plus_parrot_capacity plus_parrot_state.capacity
#define plus_parrot_count plus_parrot_state.count
#define plus_parrot_play plus_parrot_state.play
#define plus_parrot_playing plus_parrot_state.playing
#define plus_parrot_truncated plus_parrot_state.truncated

/*!
 * \brief Default channel descriptor
 */
static struct chan_usbradio_pvt usbradio_default = {
	.duplex = M_UNSET,
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
	/* 750 us land-mobile pre/deemphasis: fc = 1 / (2*pi*750 us). */
	.plus_emphasis_corner_hz = 212.206590789,
};

/*	DECLARE FUNCTION PROTOTYPES	*/

static int hidhdwconfig(struct chan_usbradio_pvt *o);
static void mixer_write(struct chan_usbradio_pvt *o);
static int usbradio_start_audio(struct chan_usbradio_pvt *o);
static PaError usbradio_read_pa_stereo(struct chan_usbradio_pvt *o);
static void *usbradio_audio_thread(void *arg);
static void stream_cleanup(struct chan_usbradio_pvt *o);
static struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
					    const struct ast_assigned_ids *assignedids,
					    const struct ast_channel *requestor, const char *data,
					    int *cause);
static int usbradio_digit_begin(struct ast_channel *c, char digit);
static int usbradio_digit_end(struct ast_channel *c, char digit, unsigned int duration);
static int usbradio_text(struct ast_channel *c, const char *text);
static int usbradio_hangup(struct ast_channel *c);
static int usbradio_answer(struct ast_channel *c);
static struct ast_frame *usbradio_read(struct ast_channel *chan);
static int usbradio_call(struct ast_channel *c, const char *dest, int timeout);
static int usbradio_write(struct ast_channel *chan, struct ast_frame *f);
static void usbradioplus_queue_program(struct chan_usbradio_pvt *o, const short *samples,
				       size_t count);
static int usbradio_indicate(struct ast_channel *chan, int cond_in, const void *data,
			     size_t datalen);
static int usbradio_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int usbradio_setoption(struct ast_channel *chan, int option, void *data, int datalen);
static void store_rxvoiceadj(struct chan_usbradio_pvt *o, const char *s);
static double effective_rx_input_gain_db(struct chan_usbradio_pvt *o);
static float effective_legacy_rxvoiceadj(struct chan_usbradio_pvt *o);
static int effective_rxmixerset(const struct chan_usbradio_pvt *o);
static int effective_txmixaset(const struct chan_usbradio_pvt *o);
static int effective_txmixbset(const struct chan_usbradio_pvt *o);
static enum radio_tx_mix effective_txmixa(const struct chan_usbradio_pvt *o);
static enum radio_tx_mix effective_txmixb(const struct chan_usbradio_pvt *o);
static void refresh_processing_hardware(struct chan_usbradio_pvt *o);
static enum radio_carrier_detect effective_rxcdtype(const struct chan_usbradio_pvt *o);
static int set_txctcss_level(struct chan_usbradio_pvt *o);
static void radio_dump(struct chan_usbradio_pvt *o, int fd);
static void mult_set(struct chan_usbradio_pvt *o);
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
static void usbradioplus_dsp_destroy(struct chan_usbradio_pvt *o);
static void usbradioplus_native_tick(struct chan_usbradio_pvt *o);
static void usbradioplus_parrot_rx_transition(struct chan_usbradio_pvt *o, int was_keyed);
static int usbradioplus_ensure_parrot_capacity(struct chan_usbradio_pvt *o);

#define usbradioplus_native_echo(o)                                                                \
	urp_native_echo_enabled((o)->duplex3, (o)->duplex3mode == DUPLEX3_MODE_SOFTWARE)
#define usbradioplus_legacy_cutoff(name, selector)                                                 \
	urp_legacy_cutoff(urp_legacy_filter_name(name), (selector))
#define plus_mix_has_program(mix) urp_tx_output_has_program((enum urp_tx_output_mode)(mix))
static void usbradioplus_prepare_squelch_audio(struct chan_usbradio_pvt *o);
static struct chan_usbradio_pvt *store_config(struct ast_config *cfg, const char *ctg);
#if DEBUG_FILETEST == 1
static int RxTestIt(struct chan_usbradio_pvt *o);
#endif

#define URP_CHANNEL_COMMON_DECLARE
#include "usbradioplus_channel_common.inc"
#undef URP_CHANNEL_COMMON_DECLARE

/*!
 * \brief Log a USB/audio fault and set the recovery latch.
 *
 * First occurrence (already_logged == 0) uses LOG_ERROR; repeats use DEBUG
 * so retry loops do not spam. Returns 1 for storing into a rate-limit latch.
 */
static int __attribute__((format(printf, 3, 4)))
usbradio_log_fault(struct chan_usbradio_pvt *o, int already_logged, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	o->usb_faulted = 1;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (already_logged) {
		ast_debug(1, "%s", buf);
	} else {
		ast_log(LOG_ERROR, "%s", buf);
	}
	return 1;
}

static void usbradio_device_identity(struct chan_usbradio_pvt *o, char *devstr, size_t devstr_size,
				     char *serial, size_t serial_size, int *alsa_card)
{
	if (devstr && devstr_size) {
		devstr[0] = '\0';
	}
	if (serial && serial_size) {
		serial[0] = '\0';
	}
	if (alsa_card) {
		*alsa_card = -1;
	}

	/* Acquired device identity */
	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		if (devstr && devstr_size) {
			ast_copy_string(devstr, o->radio_device->devstr, devstr_size);
		}
		if (serial && serial_size && o->radio_device->serial) {
			ast_copy_string(serial, o->radio_device->serial, serial_size);
		}
		if (alsa_card) {
			*alsa_card = o->radio_device->alsa_card;
		}
	}
	ast_mutex_unlock(&o->device_lock);
}

/*!
 * \brief Log once when USB/audio returns after a prior failure.
 */
static void usbradio_log_usb_recovered(struct chan_usbradio_pvt *o)
{
	char devstr[sizeof(o->devstr)];
	sig_atomic_t was_faulted;

	was_faulted = o->usb_faulted;
	o->usb_faulted = 0;
	if (!was_faulted) {
		return;
	}

	/* Match fault priority so ERROR-level logs pair fault with recovery. */
	usbradio_device_identity(o, devstr, sizeof(devstr), NULL, 0, NULL);
	ast_log(LOG_ERROR, "Channel %s: USB radio device recovered (%s)\n", o->name,
		S_OR(devstr, "unknown"));
}

static char *usbradio_active; /* the active device */

static const int ppinshift[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 5, 4, 0, 3};

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

/*!
 * \brief Indicate that PTT is activate.
 *	This causes the hidthead to to exit from the loop timer and
 *	evaluate the gpio pins.
 * \param o		Channel private data.
 */

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

	for (o = usbradio_default.next; o && o->name && dev && strcmp(o->name, dev) != 0;
	     o = o->next)
		;
	if (!o) {
		ast_log(LOG_WARNING, "Cannot find USB descriptor <%s>.\n",
			dev ? dev : "-- Null Descriptor --");
		return NULL;
	}
	return o;
}

/*!
 * \brief Parallel port processing thread.
 *	This thread evaluates the timers configured for each
 *  configured parallel port pin.
 * \param arg	Arguments - this is always NULL.
 */
static void *pulserthread(void *arg)
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
	return NULL;
}

/*!
 * \brief Load settings for a specific node
 * \param o
 * \param cfg If provided, will use the provided config. If NULL, cfg will be opened automatically.
 * \param reload 0 for first load, 1 for reload
 */

/*!
 * \brief Keep full TX level on mono PortAudio devices.
 *
 * The native renderer writes stereo. Default txmixa=composite / txmixb=no zeros
 * the right channel, so the res_usbradio L/R average drops ~6 dB.
 * Duplicate the active mix onto both sides before PortAudio downmixes it.
 * Differing voice+tone is left on L/R so the average forms composite;
 * other differing pairs keep A and disable B.
 */
static void usbradio_adjust_txmix_for_mono(struct chan_usbradio_pvt *o)
{
	enum radio_tx_mix a = o->txmixa;
	enum radio_tx_mix b = o->txmixb;

	if (o->pa.output_channels != 1) {
		return;
	}

	if (a == TX_OUT_OFF && b == TX_OUT_OFF) {
		return;
	}

	if (a == TX_OUT_OFF) {
		/* B holds the configured source; duplicate it to both channels. */
		a = b;
	} else if (b == TX_OUT_OFF) {
		b = a;
	} else if (a != b) {
		/* Voice+tone on A/B is composite once PortAudio averages L/R. */
		if ((a == TX_OUT_VOICE && b == TX_OUT_LSD) ||
		    (a == TX_OUT_LSD && b == TX_OUT_VOICE)) {
			ast_log(LOG_WARNING,
				"Channel %s: mono TX device; voice+tone A/B averaged into "
				"composite (both sources kept)\n",
				o->name);
			o->txmixa = TX_OUT_COMPOSITE;
			o->txmixb = TX_OUT_COMPOSITE;
			if (o->radio) {
				o->radio->txMixA = TX_OUT_COMPOSITE;
				o->radio->txMixB = TX_OUT_COMPOSITE;
			}
			return;
		}

		ast_log(LOG_WARNING,
			"Channel %s: unsupported txmixa/txmixb pair on mono TX; keeping A, "
			"disabling B\n",
			o->name);
		b = a;
	}

	if (a != o->txmixa || b != o->txmixb) {
		ast_log(LOG_WARNING,
			"Channel %s: mono TX device; forcing equal A/B mix so PortAudio downmix "
			"stays full level\n",
			o->name);
	}

	o->txmixa = a;
	o->txmixb = b;

	if (o->radio) {
		o->radio->txMixA = a;
		o->radio->txMixB = b;
	}
}

static void usbradio_release_device(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		ast_radio_device_release(o->radio_device);
		o->radio_device = NULL;
	}
	ast_mutex_unlock(&o->device_lock);
}

static void usbradio_swap_begin(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&o->swap_lock);
	o->swap_audio_ready = 0;
	o->swap_state = DEVICE_SWAP_QUIESCING;
	ast_mutex_unlock(&o->swap_lock);
}

static void usbradio_swap_audio_stopped(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&o->swap_lock);
	if (o->swap_state == DEVICE_SWAP_QUIESCING) {
		o->swap_audio_ready = 1;
	}
	ast_mutex_unlock(&o->swap_lock);
}

static int usbradio_swap_hid_wait(struct chan_usbradio_pvt *o)
{
	int swapping;

	ast_mutex_lock(&o->swap_lock);
	swapping = o->swap_state == DEVICE_SWAP_QUIESCING;
	while (swapping && !o->swap_audio_ready && !o->stophid) {
		ast_mutex_unlock(&o->swap_lock);
		usleep(10000);
		ast_mutex_lock(&o->swap_lock);
		swapping = o->swap_state == DEVICE_SWAP_QUIESCING;
	}
	if (swapping && o->swap_audio_ready) {
		o->swap_state = DEVICE_SWAP_READY;
	} else {
		swapping = 0;
	}
	while (o->swap_state == DEVICE_SWAP_READY && !o->stophid) {
		ast_mutex_unlock(&o->swap_lock);
		usleep(10000);
		ast_mutex_lock(&o->swap_lock);
	}
	ast_mutex_unlock(&o->swap_lock);
	return swapping;
}

static int usbradio_swap_ready(struct chan_usbradio_pvt *o)
{
	int ready;

	ast_mutex_lock(&o->swap_lock);
	ready = o->swap_state == DEVICE_SWAP_READY;
	ast_mutex_unlock(&o->swap_lock);
	return ready;
}

static void usbradio_swap_finish(struct chan_usbradio_pvt *o)
{
	ast_mutex_lock(&o->swap_lock);
	o->swap_audio_ready = 0;
	o->swap_state = DEVICE_SWAP_IDLE;
	ast_mutex_unlock(&o->swap_lock);
}

static void usbradio_mixer_limits(struct chan_usbradio_pvt *o, int *rx_max, int *tx_max,
				  int *sidetone_max)
{
	*rx_max = 0;
	*tx_max = 0;
	*sidetone_max = 0;

	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		*rx_max =
			ast_radio_device_mixer_max(o->radio_device, o->radio_device->mixer_rx_paths,
						   AST_RADIO_MIXER_CAPTURE_VOLUME);
		*tx_max =
			ast_radio_device_mixer_max(o->radio_device, o->radio_device->mixer_tx_paths,
						   AST_RADIO_MIXER_PLAYBACK_VOLUME);
		*sidetone_max = ast_radio_device_mixer_max(o->radio_device,
							   o->radio_device->mixer_sidetone_paths,
							   AST_RADIO_MIXER_PLAYBACK_VOLUME);
	}
	ast_mutex_unlock(&o->device_lock);
}

/* Control receive-audio monitoring into playback (Mic Playback Switch on CM108) */
static void usbradio_set_sidetone_switch(struct chan_usbradio_pvt *o, int enabled)
{
	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		ast_radio_device_set_mixer_paths(o->radio_device,
						 o->radio_device->mixer_sidetone_paths,
						 o->radio_device->mixer_sidetone_path_count,
						 AST_RADIO_MIXER_PLAYBACK_SWITCH, enabled);
	}
	ast_mutex_unlock(&o->device_lock);
}

/* Control receive input level and optional gain/AGC */
static void usbradio_set_rx_mixer(struct chan_usbradio_pvt *o, long volume)
{
	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		ast_radio_device_set_mixer_paths(o->radio_device, o->radio_device->mixer_rx_paths,
						 o->radio_device->mixer_rx_path_count,
						 AST_RADIO_MIXER_CAPTURE_VOLUME, volume);
		ast_radio_device_set_mixer_paths(o->radio_device,
						 o->radio_device->mixer_rx_boost_paths,
						 o->radio_device->mixer_rx_boost_path_count,
						 AST_RADIO_MIXER_PLAYBACK_SWITCH, 1);
	}
	ast_mutex_unlock(&o->device_lock);
}

/*! \brief Acquire and initialize the configured USB radio device */
static int init_audio_device(struct chan_usbradio_pvt *o)
{
	struct ast_radio_device *radio_device;
	struct ast_radio_device_request request = {
		.devstr = o->devstr,
		.serial = o->serial,
		.owner = o->name,
		.required_caps = AST_RADIO_CAP_CM108_HID,
		.minimum_input_channels = 1,
		.minimum_output_channels = 1,
	};
	enum ast_radio_device_result result;
	int automatic;

	automatic = ast_strlen_zero(request.serial) && ast_strlen_zero(request.devstr);
	result = ast_radio_device_acquire(&request, &radio_device);
	if (result != AST_RADIO_DEVICE_READY) {
		if (o->device_error != result) {
			usbradio_log_fault(o, 0, "Channel %s: %s\n", o->name,
					   ast_radio_device_result_str(result));
		}
		o->device_error = result;
		return -1;
	}

	/* Required receive and transmit mixer limits */
	if (ast_radio_device_mixer_max(radio_device, radio_device->mixer_rx_paths,
				       AST_RADIO_MIXER_CAPTURE_VOLUME) <= 0 ||
	    ast_radio_device_mixer_max(radio_device, radio_device->mixer_tx_paths,
				       AST_RADIO_MIXER_PLAYBACK_VOLUME) <= 0) {
		usbradio_log_fault(
			o, 0, "Channel %s: Cannot use USB radio device %s without mixer limits\n",
			o->name, radio_device->devstr);
		ast_radio_device_release(radio_device);
		o->device_error = AST_RADIO_DEVICE_ERROR;
		return -1;
	}

	/* Acquired device publication without configured selector changes */
	ast_mutex_lock(&o->device_lock);
	o->radio_device = radio_device;
	ast_mutex_unlock(&o->device_lock);
	o->device_error = AST_RADIO_DEVICE_READY;

	if (automatic) {
		ast_log(LOG_NOTICE, "Channel %s: Automatically assigned USB device %s\n", o->name,
			radio_device->devstr);
	}
	return 0;
}

static int usbradio_start_audio(struct chan_usbradio_pvt *o)
{
	char devstr[sizeof(o->devstr)];
	PaError res;

	if (o->pa.active) {
		return 0;
	}

	/* Exact PortAudio endpoints from the active device lease */
	ast_mutex_lock(&o->device_lock);
	if (!o->radio_device) {
		ast_mutex_unlock(&o->device_lock);
		return -1;
	}
	ast_copy_string(devstr, o->radio_device->devstr, sizeof(devstr));
	o->pa.input_channels = o->radio_device->pa_input_channels >= 2 ? 2 : 1;
	res = ast_radio_pa_open_device(&o->pa, o->radio_device);
	ast_mutex_unlock(&o->device_lock);
	if (res != paNoError) {
		ast_log(LOG_WARNING, "Channel %s: Unable to open PortAudio stream for %s (%s)\n",
			o->name, devstr, Pa_GetErrorText(res));
		return -1;
	}

	usbradio_adjust_txmix_for_mono(o);

	res = ast_radio_pa_start(&o->pa);
	if (res != paNoError) {
		ast_log(LOG_WARNING, "Channel %s: Unable to start PortAudio stream for %s (%s)\n",
			o->name, devstr, Pa_GetErrorText(res));
		ast_radio_pa_stop(&o->pa);
		return -1;
	}

	return 0;
}

/*!
 * \brief USB sound device GPIO processing thread
 * This thread uses the USB radio device assigned by res_usbradio and performs
 * setup and initialization of its HID interface.
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
	char lasttxtmp;
	int i, j, k;
	int res;
	int open_device_failed = 0;
	int detach_failed = 0;
	int claim_failed = 0;
	int pipe_failed = 0;
	int start_audio_failed = 0;
	struct libusb_device_handle *usb_handle;
	struct chan_usbradio_pvt *o = arg;
	struct timeval then;
	struct pollfd rfds[1];

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
		ast_radio_time(&o->lasthidtime);

		/* Prior device teardown */
		o->hasusb = 0;

		if (usb_handle) {
			libusb_close(usb_handle);
			usb_handle = NULL;
		}

		/* Park the lease after both HID and audio have stopped for a device swap */
		if (usbradio_swap_hid_wait(o)) {
			continue;
		}
		usbradio_release_device(o);

		/* Shared USB radio device discovery and reservation */
		if (!o->radio_device && init_audio_device(o)) {
			usleep(500000);
			continue;
		}

		/* Open the USB device */
		if (libusb_open(o->radio_device->usb_device, &usb_handle) < 0) {
			open_device_failed = usbradio_log_fault(
				o, open_device_failed, "Channel %s: Cannot open device %s\n",
				o->name, o->radio_device->devstr);
			usleep(500000);
			continue;
		}
		/* attempt to claim the usb hid interface and detach from the kernel */
		if (libusb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
			if (libusb_detach_kernel_driver(usb_handle, C108_HID_INTERFACE) < 0) {
				detach_failed = usbradio_log_fault(
					o, detach_failed,
					"Channel %s: Is not able to detach the USB device\n",
					o->name);
				usleep(500000);
				continue;
			}
			if (libusb_claim_interface(usb_handle, C108_HID_INTERFACE) < 0) {
				claim_failed = usbradio_log_fault(
					o, claim_failed,
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
		if (pipe2(o->pttkick, O_NONBLOCK) == -1) {
			pipe_failed = usbradio_log_fault(
				o, pipe_failed, "Channel %s: Is not able to create a pipe\n",
				o->name);
			libusb_close(usb_handle);
			usb_handle = NULL;
			o->hasusb = 0;
			/* Stay in hidthread and retry; call() only starts the thread once. */
			usleep(500000);
			continue;
		}

		/* Existing HID product-family mapping */
		if ((o->radio_device->product_id & 0xfffc) == C108_PRODUCT_ID) {
			o->devtype = C108_PRODUCT_ID;
		} else {
			o->devtype = o->radio_device->product_id;
		}
		ast_debug(5, "Channel %s: Starting normally.\n", o->name);

		ast_debug(5, "Channel %s: Attached to USB device %s.\n", o->name,
			  o->radio_device->devstr);
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

			if (o->txlimonly) {
				tChan.txMod = 1;
			}
			if (o->txprelim) {
				tChan.txMod = 2;
			}

			tChan.txMixA = effective_txmixa(o);
			tChan.txMixB = effective_txmixb(o);

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

			*(o->radio->prxSquelchAdjust) =
				((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
			*(o->radio->prxVoiceAdjust) = effective_legacy_rxvoiceadj(o) * M_Q8;
			*(o->radio->prxCtcssAdjust) = o->rxctcssadj * M_Q8;
			o->radio->rxCtcss->relax = o->rxctcssrelax;
			o->radio->txTocType = o->txtoctype;

			if ((o->txmixa == TX_OUT_LSD) || (o->txmixa == TX_OUT_COMPOSITE) ||
			    (o->txmixb == TX_OUT_LSD) || (o->txmixb == TX_OUT_COMPOSITE)) {
				set_txctcss_level(o);
			}

			if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) &&
			    (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
				ast_log(LOG_ERROR, "Channel %s: No txvoice output configured.\n",
					o->name);
			}

			if (o->txctcssfreq[0] && o->txmixa != TX_OUT_LSD &&
			    o->txmixa != TX_OUT_COMPOSITE && o->txmixb != TX_OUT_LSD &&
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
			ast_log(LOG_WARNING, "Invalid value for txslimsp in radio settings section "
					     "of usbradio.c, using default");
			o->txslimsp = DEFAULT_TX_SOFT_LIMITER_SETPOINT;
			legacy_set_tx_soft_limiter(o, o->txslimsp);
		}

		ast_mutex_lock(&o->eepromlock);
		if (o->wanteeprom) {
			o->eepromctl = 1;
		}
		ast_mutex_unlock(&o->eepromlock);

		if (usbradio_start_audio(o) < 0) {
			start_audio_failed = usbradio_log_fault(
				o, start_audio_failed, "Channel %s: Unable to start audio stream\n",
				o->name);
			usleep(500000);
			continue;
		}
		/* Reset the failure flags, we succeeded */
		open_device_failed = 0;
		detach_failed = 0;
		claim_failed = 0;
		pipe_failed = 0;
		start_audio_failed = 0;
		usbradio_log_usb_recovered(o);
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
			time_t audio_time_now = 0;

			then = ast_radio_tvnow();
			if (o->lastaudiotime) {
				/* HID thread monitors audio thread */
				ast_radio_time(&audio_time_now);
				if ((audio_time_now - o->lastaudiotime) > 3) {
					usbradio_log_fault(o, 0,
							   "Channel %s: Audio process has died or "
							   "is not responding.\n",
							   o->name);
					o->hasusb = 0;
					break;
				}
			}
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

				int bytes = read(o->pttkick[0], &c, 1);
				if (bytes <= 0) {
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
							if (!o->legacy_rxvoiceadj_configured)
								memcpy(&o->legacy_rxvoiceadj,
								       &o->eeprom
										[EEPROM_USER_RXVOICEADJ],
								       sizeof(float));
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
			if (keyed != o->rxhidsq) {
				ast_debug(2, "Channel %s: Update rxhidsq = %d\n", o->name, keyed);
				o->rxhidsq = keyed;
			}
			/* See if we are receiving ctcss */
			ctcssed = !(buf[o->hid_io_ctcss_loc] & o->hid_io_ctcss);
			if (ctcssed != o->rxhidctcss) {
				ast_debug(2, "Channel %s: Update rxhidctcss = %d\n", o->name,
					  ctcssed);
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
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "in")) &&
					    (PP_MASK & (1 << i))) {
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
						/* skip if not valid */
						if (!(PP_MASK & (1 << i))) {
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
				o->rxppsq = o->rxppctcss = 0;
				for (i = 10; i <= 15; i++) {
					if ((o->pps[i]) && (!strcasecmp(o->pps[i], "cor")) &&
					    (PP_MASK & (1 << i))) {
						j = k &
						    (1
						     << ppinshift[i]); /* set the bit accordingly */
						if (j != o->rxppsq) {
							ast_debug(
								2,
								"Channel %s: update rxppsq = %d\n",
								o->name, j);
							o->rxppsq = j;
						}
					} else if ((o->pps[i]) &&
						   (!strcasecmp(o->pps[i], "ctcss")) &&
						   (PP_MASK & (1 << i))) {
						o->rxppctcss =
							k & (1 << ppinshift[i]); /* set the bit
										    accordingly */
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
		o->radio->txPttOut = 0;
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
	o->radio->txPttOut = 0;
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
		/* Hangup joins this thread; release HID so the next call can reopen it. */
		libusb_close(usb_handle);
		usb_handle = NULL;
	}

	usbradio_release_device(o);

	return NULL;
}

/*!
 * \brief Write a full frame of audio data to the sound card device.
 * \note data is 48 kHz stereo interleaved. ast_radio_pa_write() takes frames
 *       per channel (AST_RADIO_PA_FRAMES_PER_BUFFER); interleaved sample count
 *       is frames * AST_RADIO_PA_OUTPUT_CHANNELS (== AST_RADIO_PA_48K_STEREO_SAMPLES).
 * 		 pa frames are a single sample per frame, while asterisk frames are 160 samples per
 * frame. \param o		chan_usbradio_pvt. \param data	Audio data to write. \returns
 * Byte count written on success, 0 on failure.
 */
static int soundcard_writeframe(struct chan_usbradio_pvt *o, short *data)
{
	PaError res;

	if (!o->pa.active) {
		if (usbradio_start_audio(o) < 0) {
			return 0;
		}
	}

	/*
	 * Always write something so the USB TX buffer does not intentionally
	 * underrun (same idea as simpleusb #1161). Radio PTT is HID-gated;
	 * when unkeyed, feed silence instead of the rendered TX buffer.
	 */
	if (!o->radio->txPttIn && !o->radio->txPttOut) {
		data = silence_buf;
	}

	/*
	 * ast_radio_pa_write() already primes one silence frame on
	 * paOutputUnderflowed (#593 / #598). Treat that as success here.
	 */
	res = ast_radio_pa_write(&o->pa, data, AST_RADIO_PA_FRAMES_PER_BUFFER);
	if (res < 0 && res != paOutputUnderflowed) {
		usbradio_log_fault(
			o, 0, "Channel %s: PortAudio write failed (%s); restarting audio stream\n",
			o->name, Pa_GetErrorText(res));
		/* Force HID reinit so recovery log clears the latch. */
		o->hasusb = 0;
		ast_radio_pa_stop(&o->pa);
		return 0;
	}

	return AST_RADIO_PA_FRAMES_PER_BUFFER * AST_RADIO_PA_OUTPUT_CHANNELS * (int)sizeof(short);
}

/*!
 * \brief Read one PortAudio frame into the 48 kHz stereo workspace.
 *
 * When hardware opened with one input channel, duplicate mono samples to both
 * channels so the native detector and statistics code always see stereo.
 */
static PaError usbradio_read_pa_stereo(struct chan_usbradio_pvt *o)
{
	short *stereo = (short *)(o->usbradio_read_buf + AST_FRIENDLY_OFFSET);
	PaError pa_res;

	if (o->pa.input_channels == 1) {
		short mono_buf[AST_RADIO_PA_FRAMES_PER_BUFFER];
		int i;

		pa_res = ast_radio_pa_read(&o->pa, mono_buf, AST_RADIO_PA_FRAMES_PER_BUFFER, 40,
					   &o->stopaudiothread);
		if (pa_res == paNoError) {
			for (i = AST_RADIO_PA_FRAMES_PER_BUFFER - 1; i >= 0; i--) {
				stereo[i * 2] = mono_buf[i];
				stereo[i * 2 + 1] = mono_buf[i];
			}
		}
		return pa_res;
	}

	return ast_radio_pa_read(&o->pa, stereo, AST_RADIO_PA_FRAMES_PER_BUFFER, 40,
				 &o->stopaudiothread);
}

/*!
 * \brief Asterisk digit begin function.
 * \param c				Asterisk channel.
 * \param digit			Digit processed.
 * \retval 0
 */

/*!
 * \brief Asterisk digit end function.
 * \param c				Asterisk channel.
 * \param digit			Digit processed.
 * \param duration		Duration of the digit.
 * \retval -1
 */

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
	if (!o) {
		return -1;
	}

#ifdef HAVE_SYS_IO
	if (haspp == 2) {
		ioperm(pbase, 2, 1);
	}
#endif

	cmd = ast_alloca(strlen(text) + 10);

	/* print received messages */
	ast_debug(3, "Channel %s: Console Received usbradio text %s >>\n", o->name, text);

	cnt = sscanf(text, "%s " S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) S_FMT(STR_SZ) "%c", cmd,
		     rxs, txs, rxpl, txpl, &pwr);

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

	/* set transmit CTCSS (app_rpt itxctcss → TXCTCSS 0/1) */
	if (strcmp(cmd, "TXCTCSS") == 0) {
		if (cnt < 2 || (strcmp(rxs, "0") && strcmp(rxs, "1"))) {
			ast_log(LOG_WARNING, "Channel %s: Invalid TXCTCSS command: %s\n", o->name,
				text);
			return 0;
		}
		if (o && o->radio) {
			o->radio->b.txCtcssOff = rxs[0] != '1';
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
	(void)dest;
	(void)timeout;
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	if (o->hidthread == AST_PTHREADT_NULL) {
		o->stophid = 0;
		ast_radio_time(&o->lasthidtime);
		if (ast_pthread_create(&o->hidthread, NULL, hidthread, o)) {
			ast_log(LOG_ERROR, "Channel %s: Failed to create HID thread\n", o->name);
			return -1;
		}
	}
	if (o->audiothread == AST_PTHREADT_NULL) {
		o->stopaudiothread = 0;
		o->audio_thread_ready = 0;
		ast_radio_time(&o->lastaudiotime);
		if (ast_pthread_create(&o->audiothread, NULL, usbradio_audio_thread, o)) {
			ast_log(LOG_ERROR, "Channel %s: Failed to create audio thread\n", o->name);
			o->stophid = 1;
			kickptt(o);
			if (o->hidthread != AST_PTHREADT_NULL) {
				pthread_join(o->hidthread, NULL);
				o->hidthread = AST_PTHREADT_NULL;
			}
			return -1;
		}
	}
	ast_setstate(c, AST_STATE_UP);
	return 0;
}

/*!
 * \brief Answer the call.
 * \param c				Asterisk channel.
 * \retval 0 			If successful.
 */

/*!
 * \brief Asterisk hangup function.
 * \param c			Asterisk channel.
 * \retval 0		Always returns 0.
 */
static int usbradio_hangup(struct ast_channel *c)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	o->stopaudiothread = 1;
	o->stophid = 1;
	kickptt(o);
	if (o->audiothread != AST_PTHREADT_NULL) {
		pthread_join(o->audiothread, NULL);
		o->audiothread = AST_PTHREADT_NULL;
	}
	if (o->hidthread != AST_PTHREADT_NULL) {
		pthread_join(o->hidthread, NULL);
		o->hidthread = AST_PTHREADT_NULL;
	}
	ast_channel_tech_pvt_set(c, NULL);
	o->owner = NULL;
	ast_module_unref(ast_module_info->self);
	if (o->hookstate) {
		o->hookstate = 0;
	}
	ast_radio_pa_stop(&o->pa);

	return 0;
}

/*!
 * \brief Asterisk write function.
 * Queues outbound app_rpt frames for native-rate rendering (non-blocking).
 * \param ast			Asterisk channel.
 * \param frame			Asterisk frame to process.
 * \retval 0			Successful.
 */

static int usbradio_write(struct ast_channel *c, struct ast_frame *f)
{
	struct chan_usbradio_pvt *o = ast_channel_tech_pvt(c);

	if (!o->hasusb || !o->audio_thread_ready) {
		return 0;
	}

#if DEBUG_CAPTURES == 1
	/* Write input data to a file.
	 * Left channel has the audio, right channel shows txkeyed
	 */
	if (ftxcapraw && o->txcapraw) {
		short i, tbuff[f->datalen];
		for (i = 0; i < f->datalen; i += 2) {
			tbuff[i] = ((short *)(f->data.ptr))[i / 2];
			tbuff[i + 1] = o->txkeyed * M_Q13;
		}
		fwrite(tbuff, 2, f->datalen, ftxcapraw);
	}
#endif

	/* app_rpt writes silence continuously. Admit only keyed program frames so
	 * an unkeyed stream cannot prevent the accepted audio tail from draining. */
	if (o->echoing || !o->txkeyed || f->frametype != AST_FRAME_VOICE || !f->data.ptr) {
		return 0;
	}
	usbradioplus_queue_program(o, f->data.ptr, f->datalen / sizeof(short));

	return 0;
}

/*!
 * \brief Asterisk read function.
 * Audio is pushed from the audio thread via ast_queue_frame.
 */
static struct ast_frame *usbradio_read(struct ast_channel *c)
{
	(void)c;
	ast_debug(1, "Read function should not be called!\n");
	return &ast_null_frame;
}

/*!
 * \brief Stop PortAudio and flush the TX queue.
 */
static void stream_cleanup(struct chan_usbradio_pvt *o)
{
	ast_radio_pa_stop(&o->pa);
	usbradio_swap_audio_stopped(o);
	o->audio_thread_ready = 0;
}

/*!
 * \brief PortAudio and native radio-processing thread.
 *
 * Owns blocking USB audio I/O so channel .read/.write stay non-blocking.
 * Inbound frames are queued to Asterisk; outbound frames enter the elastic
 * app_rpt-to-CM119 buffer and are rendered on each hardware tick.
 */
static void *usbradio_audio_thread(void *arg)
{
	PaError pa_res;
	int lastpttout;
	int cd, sd;
	int was_rxkeyed;
	int tx_write_ready;
	long frames_available;
	struct chan_usbradio_pvt *o = arg;
	struct ast_frame *f = &o->read_f, *f1;
	time_t now;

	ast_debug(5, "Audio thread is starting\n");
	ast_radio_time(&o->lastaudiotime);

	while (!o->stopaudiothread) {
		ast_radio_time(&o->lastaudiotime);

		if (!o->hasusb) {
			usbradio_swap_audio_stopped(o);
			usleep(DEVICE_RETRY);
			continue;
		}

		if (!o->pa.active && usbradio_start_audio(o) < 0) {
			usbradio_log_fault(o, 0, "Channel %s: Failed to start audio stream\n",
					   o->name);
			o->hasusb = 0;
			usleep(DEVICE_RETRY);
			continue;
		}

		/* Prime one silence frame so a late first wake-up does not underrun. */
		ast_radio_pa_write(&o->pa, silence_buf, AST_RADIO_PA_FRAMES_PER_BUFFER);
		o->audio_thread_ready = 1;

		while (!o->stopaudiothread && o->hasusb) {
			if (o->lasthidtime) {
				ast_radio_time(&now);
				if ((now - o->lasthidtime) > 3) {
					usbradio_log_fault(o, 0,
							   "Channel %s: HID process has died or is "
							   "not responding.\n",
							   o->name);
					o->hasusb = 0;
					stream_cleanup(o);
					break;
				}
			}

			ast_radio_time(&o->lastaudiotime);
			memset(f, 0, sizeof(struct ast_frame));
			f->frametype = AST_FRAME_NULL;
			f->src = __PRETTY_FUNCTION__;

			if (!o->hasusb) {
				if (o->rxkeyed) {
					struct ast_frame wf = {
						.frametype = AST_FRAME_CONTROL,
						.subclass.integer = AST_CONTROL_RADIO_UNKEY,
						.src = __PRETTY_FUNCTION__,
					};

					o->lastrx = 0;
					o->rxkeyed = 0;
					if (o->owner) {
						ast_queue_frame(o->owner, &wf);
					}
					if (o->duplex3) {
						usbradio_set_sidetone_switch(o, 0);
					}
				}
				stream_cleanup(o);
				break;
			}

			/* One PortAudio input block for loop pacing */
			pa_res = usbradio_read_pa_stereo(o);
			if (pa_res != paNoError) {
				if (pa_res == paTimedOut || pa_res == paInputOverflowed) {
					/* RX silence when no input block is available */
					memset(o->usbradio_read_buf + AST_FRIENDLY_OFFSET, 0,
					       sizeof(o->usbradio_read_buf) - AST_FRIENDLY_OFFSET);
				} else {
					usbradio_log_fault(o, 0,
							   "Channel %s: PortAudio read failed "
							   "(%s); restarting audio stream\n",
							   o->name, Pa_GetErrorText(pa_res));
					o->hasusb = 0;
					stream_cleanup(o);
					break;
				}
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

			/* PortAudio output capacity check */
			frames_available = ast_radio_pa_write_available(&o->pa);
			if (frames_available < 0) {
				ast_debug(2, "Channel %s: Pa_GetStreamWriteAvailable error %s\n",
					  o->name, Pa_GetErrorText(frames_available));
				o->usb_faulted = 1;
				o->hasusb = 0;
				stream_cleanup(o);
				break;
			}
			tx_write_ready = frames_available >= AST_RADIO_PA_FRAMES_PER_BUFFER;

			/* Echo playback feeds the same native-rate program queue as app_rpt. */
			if (tx_write_ready && o->echomode && !usbradioplus_native_echo(o) &&
			    (!o->rxkeyed)) {
				struct usbecho *u;

				ast_mutex_lock(&o->echolock);
				if (o->echoq.q_forw != &o->echoq) {
					u = (struct usbecho *)o->echoq.q_forw;
					remque((struct qelem *)u);
					usbradioplus_queue_program(o, u->data, FRAME_SIZE);
					ast_free(u);
					o->echoing = 1;
				} else {
					o->echoing = 0;
				}
				ast_mutex_unlock(&o->echolock);
			}

#if DEBUG_CAPTURES == 1
			if (o->rxcapraw && frxcapraw) {
				fwrite(o->usbradio_read_buf + AST_FRIENDLY_OFFSET, 1,
				       AST_RADIO_PA_FRAMES_PER_BUFFER *
					       AST_RADIO_PA_OUTPUT_CHANNELS * sizeof(short),
				       frxcapraw);
			}
#endif

			o->readerrs = 0;
			o->readpos = sizeof(o->usbradio_read_buf);

			if (ast_radio_check_audio(
				    (short *)(o->usbradio_read_buf + AST_FRIENDLY_OFFSET),
				    &o->rxaudiostats, AST_RADIO_PA_48K_STEREO_SAMPLES, 0)) {
				if (o->clipledgpio) {
					if (!o->hid_gpio_pulsetimer[o->clipledgpio - 1]) {
						o->hid_gpio_pulsetimer[o->clipledgpio - 1] =
							CLIP_LED_HOLD_TIME_MS;
					}
				}
			}

			was_rxkeyed = o->rxkeyed;
			if (o->txkeyed || o->txtestkey || o->echoing || o->plus_parrot_playing ||
			    usbradioplus_program_pending(o) || o->rxkeyed) {
				if (!o->radio->txPttIn) {
					o->radio->txPttIn = 1;
					ast_debug(3, "Channel %s: txPttIn = %i.\n", o->name,
						  o->radio->txPttIn);
				}
			} else if (o->radio->txPttIn) {
				o->radio->txPttIn = 0;
				ast_debug(3, "Channel %s: txPttIn = %i.\n", o->name,
					  o->radio->txPttIn);
			}
			lastpttout = o->radio->txPttOut;

			usbradioplus_prepare_squelch_audio(o);
			urp_radio_process(o->radio, o->plus_squelch_native,
					  (i16 *)(o->usbradio_read_buf_8k + AST_FRIENDLY_OFFSET),
					  o->usbradio_write_buf);
			if (o->radio->b.ctcssRxEnable &&
			    o->radio->rxCtcss->decode != o->rxctcssdecode) {
				ast_debug(3, "Channel %s: rxctcssdecode = %i.\n", o->name,
					  o->radio->rxCtcss->decode);
				o->rxctcssdecode = o->radio->rxCtcss->decode;
				ast_copy_string(o->rxctcssfreq, o->radio->rxctcssfreq,
						sizeof(o->rxctcssfreq));
			}
			usbradioplus_native_tick(o);

			if (lastpttout != o->radio->txPttOut) {
				usbradioplus_program_radio(o);
				ast_debug(3, "Channel %s: txPttOut = %i.\n", o->name,
					  o->radio->txPttOut);
				kickptt(o);
			}

#if DEBUG_CAPTURES == 1 && URP_RADIO_DEBUG == 1
			if (o->txcap2 && ftxcaptrace) {
				fwrite((o->radio->ptxDebug), 1, FRAME_SIZE * 2 * 16, ftxcaptrace);
			}
#endif

			/*
			 * Write one frame when PortAudio has room. When unkeyed,
			 * soundcard_writeframe() substitutes silence. Do not fill remaining
			 * PortAudio room; that adds TX delay.
			 */
			if (tx_write_ready) {
				if (!soundcard_writeframe(o, o->usbradio_write_buf)) {
					stream_cleanup(o);
					break;
				}
				ast_radio_check_audio(o->usbradio_write_buf, &o->txaudiostats,
						      AST_RADIO_PA_48K_STEREO_SAMPLES, 0);
			}

#if DEBUG_CAPTURES == 1 && URP_RADIO_DEBUG == 1
			if (frxcaptrace && o->rxcap2 && o->radioactive) {
				fwrite((o->radio->prxDebug), 1, FRAME_SIZE * 2 * 16, frxcaptrace);
			}
#endif

			{
				enum radio_carrier_detect rxcdtype = effective_rxcdtype(o);
				cd = 0;
				if (rxcdtype == CD_HID &&
				    (o->radio->rxExtCarrierDetect != o->rxhidsq)) {
					o->radio->rxExtCarrierDetect = o->rxhidsq;
				}

				if (rxcdtype == CD_HID_INVERT &&
				    (o->radio->rxExtCarrierDetect == o->rxhidsq)) {
					o->radio->rxExtCarrierDetect = !o->rxhidsq;
				}

				if ((rxcdtype == CD_HID && o->rxhidsq) ||
				    (rxcdtype == CD_HID_INVERT && !o->rxhidsq) ||
				    (rxcdtype == CD_XPMR_NOISE && o->radio->rxCarrierDetect) ||
				    (rxcdtype == CD_PP && o->rxppsq) ||
				    (rxcdtype == CD_PP_INVERT && !o->rxppsq) ||
				    (rxcdtype == CD_XPMR_VOX && o->radio->rxCarrierDetect)) {
					if (!o->radio->txPttOut || o->radioduplex) {
						cd = 1;
					}
				} else {
					cd = 0;
				}

				if (cd != o->rxcarrierdetect) {
					o->rxcarrierdetect = cd;
					ast_debug(3, "Channel %s: rxcarrierdetect = %i.\n", o->name,
						  cd);
				}
				o->rx_cos_active = cd;
			}

#ifndef HAVE_XPMRX
			if (!o->radio->b.ctcssRxEnable ||
			    (o->radio->b.ctcssRxEnable && o->radio->rxCtcss->decode > CTCSS_NULL &&
			     o->radio->smode == SMODE_CTCSS)) {
				sd = 1;
			} else {
				sd = 0;
			}
#else
			if ((!o->radio->b.ctcssRxEnable && !o->radio->b.dcsRxEnable &&
			     !o->radio->b.lmrRxEnable) ||
			    (o->radio->b.ctcssRxEnable && o->radio->rxCtcss->decode > CTCSS_NULL &&
			     o->radio->smode == SMODE_CTCSS) ||
			    (o->radio->b.dcsRxEnable && o->radio->decDcs->decode > 0 &&
			     o->radio->smode == SMODE_DCS)) {
				sd = 1;
			} else {
				sd = 0;
			}

			if (o->radio->decDcs->decode != o->rxdcsdecode) {
				ast_debug(3, "Channel %s: rxdcsdecode = %s.\n", o->name,
					  o->radio->rxctcssfreq);
				o->rxdcsdecode = o->radio->decDcs->decode;
				ast_copy_string(o->rxctcssfreq, o->radio->rxctcssfreq,
						sizeof(o->rxctcssfreq));
			}

			if (o->radio->rptnum &&
			    (o->radio->pLsdCtl->cs[o->radio->rptnum].b.rxkeyed != o->rxlsddecode)) {
				ast_debug(3, "Channel %s: rxLSDecode = %s.\n", o->name,
					  o->radio->rxctcssfreq);
				o->rxlsddecode = o->radio->pLsdCtl->cs[o->radio->rptnum].b.rxkeyed;
				ast_copy_string(o->rxctcssfreq, o->radio->rxctcssfreq,
						sizeof(o->rxctcssfreq));
			}

			if ((o->radio->rptnum > 0 && o->radio->smode == SMODE_LSD &&
			     o->radio->pLsdCtl->cs[o->radio->rptnum].b.rxkeyed) ||
			    (o->radio->smode == SMODE_DCS && o->radio->decDcs->decode > 0)) {
				sd = 1;
			}
#endif
			if (o->rxsdtype == SD_HID) {
				sd = o->rxhidctcss;
			} else if (o->rxsdtype == SD_HID_INVERT) {
				sd = !o->rxhidctcss;
			} else if (o->rxsdtype == SD_PP) {
				sd = o->rxppctcss;
			} else if (o->rxsdtype == SD_PP_INVERT) {
				sd = !o->rxppctcss;
			}
			if (o->rxctcssoverride) {
				sd = 1;
			}
			o->rx_ctcss_active = sd;

			if (effective_rxcdtype(o) == CD_IGNORE && o->rxsdtype == SD_IGNORE) {
				cd = 0;
				sd = 0;
			}

			if (o->txoffdelay) {
				if (o->txkeyed == 1) {
					o->txoffcnt = 0;
				} else {
					o->txoffcnt++;
					if (o->txoffcnt > MS_TO_FRAMES(TX_OFF_DELAY_MAX)) {
						o->txoffcnt = MS_TO_FRAMES(TX_OFF_DELAY_MAX);
					}
				}
			}

			if (cd && sd) {
				if (!o->rxkeyed) {
					ast_debug(3, "Channel %s: o->rxkeyed = 1.\n", o->name);
				}
				if (o->rxkeyed || ((o->txoffcnt >= o->txoffdelay) &&
						   (o->rxoncnt >= o->rxondelay))) {
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
			if (o->echomode && usbradioplus_native_echo(o)) {
				usbradioplus_parrot_rx_transition(o, was_rxkeyed);
			}

			if (o->echomode && !usbradioplus_native_echo(o) && o->rxkeyed &&
			    (!o->echoing)) {
				register int x;
				struct usbecho *u;

				ast_mutex_lock(&o->echolock);
				x = 0;
				for (u = (struct usbecho *)o->echoq.q_forw;
				     u != (struct usbecho *)&o->echoq;
				     u = (struct usbecho *)u->q_forw) {
					x++;
				}
				if (x < o->echomax) {
					u = ast_calloc(1, sizeof(struct usbecho));
					if (u) {
						memcpy(u->data,
						       (o->usbradio_read_buf_8k +
							AST_FRIENDLY_OFFSET),
						       FRAME_SIZE * 2);
						insque((struct qelem *)u, o->echoq.q_back);
					}
				}
				ast_mutex_unlock(&o->echolock);
			}

			if (o->lastrx && (!o->rxkeyed)) {
				struct ast_frame wf = {
					.frametype = AST_FRAME_CONTROL,
					.subclass.integer = AST_CONTROL_RADIO_UNKEY,
					.src = __PRETTY_FUNCTION__,
				};

				o->lastrx = 0;
				if (o->owner) {
					ast_queue_frame(o->owner, &wf);
				}
				if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
					usbradio_set_sidetone_switch(o, 0);
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
					ast_debug(7, "Radio Key - CTCSS frequency=%s.\n",
						  o->rxctcssfreq);
				}
				if (o->owner) {
					ast_queue_frame(o->owner, &wf);
				}
				o->count_rssi_update = 1;
				if (o->duplex3 && o->duplex3mode == DUPLEX3_MODE_HARDWARE) {
					usbradio_set_sidetone_switch(o, 1);
				}
			}

			o->readpos = AST_FRIENDLY_OFFSET;
			if (!o->owner || ast_channel_state(o->owner) != AST_STATE_UP) {
				continue;
			}

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

			if (o->usedtmf && o->dsp) {
				f1 = ast_dsp_process(o->owner, o->dsp, f);
				if ((f1->frametype == AST_FRAME_DTMF_END) ||
				    (f1->frametype == AST_FRAME_DTMF_BEGIN)) {
					if ((f1->subclass.integer == 'm') ||
					    (f1->subclass.integer == 'u')) {
						f1->frametype = AST_FRAME_NULL;
						f1->subclass.integer = 0;
						ast_queue_frame(o->owner, f1);
						continue;
					}
					if (f1->frametype == AST_FRAME_DTMF_END) {
						f1->len = ast_tvdiff_ms(ast_radio_tvnow(),
									o->tonetime);
						if (option_verbose) {
							ast_log(LOG_NOTICE,
								"Channel %s: Got DTMF char %c "
								"duration %ld ms\n",
								o->name, f1->subclass.integer,
								f1->len);
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
						ast_queue_frame(o->owner, f1);
						continue;
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

			ast_queue_frame(o->owner, f);
		}
		stream_cleanup(o);
	}

	stream_cleanup(o);
	ast_debug(2, "Audio thread has exited\n");
	return NULL;
}

/*!
 * \brief Asterisk fixup function.
 * \param oldchan		Old asterisk channel.
 * \param newchan		New asterisk channel.
 * \retval 0			Always returns 0.
 */

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

/*!
 * \brief Asterisk setoption function.
 * \param chan			Asterisk channel.
 * \param option		Option.
 * \param data			Data.
 * \param datalen		Data length.
 * \retval 0			If successful.
 * \retval -1			If failed.
 */

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
static struct ast_channel *usbradio_new(struct chan_usbradio_pvt *o, char *ext, char *ctx,
					int state, const struct ast_assigned_ids *assignedids,
					const struct ast_channel *requestor)
{
	struct ast_channel *c;

	c = ast_channel_alloc(1, state, NULL, NULL, "", ext, ctx, assignedids, requestor, 0,
			      "RadioPlus/%s", o->name);
	if (c == NULL) {
		return NULL;
	}
	ast_channel_tech_set(c, &usbradio_tech);
	ast_channel_nativeformats_set(c, usbradio_tech.capabilities);
	ast_channel_set_readformat(c, ast_format_slin);
	ast_channel_set_writeformat(c, ast_format_slin);
	ast_channel_tech_pvt_set(c, o);
	o->owner = c;
	ast_channel_unlock(c);
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
					    const struct ast_assigned_ids *assignedids,
					    const struct ast_channel *requestor, const char *data,
					    int *cause)
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

/*!
 * \brief Process Asterisk CLI request to unkey radio.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */

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
			for (o = usbradio_default.next; o; o = o->next) {
				char devstr[sizeof(o->devstr)];
				int alsa_card;

				usbradio_device_identity(o, devstr, sizeof(devstr), NULL, 0,
							 &alsa_card);
				ast_cli(fd, "Device [%s] is assigned to device=%s card=%d\n",
					o->name, ast_strlen_zero(devstr) ? "unassigned" : devstr,
					alsa_card);
			}
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
			ast_cli(fd, "Active (command) USB Radio device set to [%s]\n",
				usbradio_active);
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
	struct chan_usbradio_pvt *p, *o;
	int attempts;
	int result;

	if (!other) {
		return -1;
	}
	o = find_desc(usbradio_active);
	if (o == NULL) {
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

	/* Serialize the complete device swap transaction */
	ast_mutex_lock(&device_swap_lock);
	if (!o->hasusb || !p->hasusb) {
		ast_cli(fd, "Both channels must have assigned USB devices before they can be "
			    "swapped.\n");
		ast_mutex_unlock(&device_swap_lock);
		return -1;
	}

	/* Stop both device handles while retaining their shared leases */
	usbradio_swap_begin(o);
	usbradio_swap_begin(p);
	o->hasusb = 0;
	p->hasusb = 0;
	kickptt(o);
	kickptt(p);

	/* Wait for the HID and PortAudio handles to close */
	for (attempts = 0; attempts < 500; attempts++) {
		if (usbradio_swap_ready(o) && usbradio_swap_ready(p)) {
			break;
		}
		usleep(10000);
	}
	if (attempts == 500) {
		usbradio_swap_finish(o);
		usbradio_swap_finish(p);
		ast_cli(fd, "Timed out waiting for both USB devices to stop.\n");
		ast_mutex_unlock(&device_swap_lock);
		return -1;
	}

	/* Exchange the parked leases and use their identities for future requests */
	ast_mutex_lock(&o->device_lock);
	ast_mutex_lock(&p->device_lock);
	result = ast_radio_device_swap(&o->radio_device, &p->radio_device);
	if (!result) {
		ast_copy_string(o->devstr, o->radio_device->devstr, sizeof(o->devstr));
		ast_copy_string(o->serial, S_OR(o->radio_device->serial, ""), sizeof(o->serial));
		ast_copy_string(p->devstr, p->radio_device->devstr, sizeof(p->devstr));
		ast_copy_string(p->serial, S_OR(p->radio_device->serial, ""), sizeof(p->serial));
	}
	ast_mutex_unlock(&p->device_lock);
	ast_mutex_unlock(&o->device_lock);
	usbradio_swap_finish(o);
	usbradio_swap_finish(p);

	if (result) {
		ast_cli(fd, "Unable to exchange USB device leases.\n");
		ast_mutex_unlock(&device_swap_lock);
		return -1;
	}
	ast_cli(fd, "USB Devices successfully swapped.\n");
	ast_mutex_unlock(&device_swap_lock);
	return 0;
}

/*!
 * \brief Send 3 second test tone.
 * \param fd			Asterisk cli fd
 * \param o				Private struct.
 * \param intflag		Flag to indicate the type of wait.
 */

/*!
 * \brief Process asterisk CLI request radio tune.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */

/*!
 * \brief Set transmit CTCSS modulation level.
 *	Set the transmit CTCSS modulation level.  Adjust the mixer output or
 *	internal gain depending on the output type.
 *	Setting ranges is 0.0 to 0.9.
 *
 * \param o				chan_usbradio structure.
 * \return	0			Always returns zero.
 */

/*!
 * \brief Set transmit soft limiting threshold.
 * Validate the legacy soft-limiter setpoint used by the native fallback.
 *
 *
 * \param o				chan_usbradio structure.
 * \param setpoint      A value which indicates the onset of soft limiting.
 * \return			    zero if successful, -1 if otherwise
 */

/*!
 * \brief Process Asterisk CLI request to set detector debug level.
 * \param fd			Asterisk CLI fd
 * \param argc			Number of arguments
 * \param argv			Arguments
 * \return	CLI success, showusage, or failure.
 */

/*!
 * \brief Store receive demodulator setting.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Store tx mixer A setting.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Store tx mixer B setting.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Store receive carrier detect type.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Store receiver gain setting.
 * \param o				Private struct.
 * \param s				New setting.
 */
/*!
 * \brief Store the legacy receive-gain fallback.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Store transmit output tone turn off type.
 * \param o				Private struct.
 * \param s				New setting.
 */

/*!
 * \brief Send test tone.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 * \param intflag		Flag to indicate the type of wait.
 */

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
	unsigned int rms = 0, stats_index;
	double peak_dbfs, rms_dbfs;
	int micmax, spkrmax, micplaymax;
	float settingmax, f;

	if (o->rxdemod == RX_AUDIO_SPEAKER && o->rxcdtype == CD_XPMR_NOISE) {
		ast_cli(fd, "ERROR: usbradioplus.conf rxdemod=speaker vs. carrierfrom=dsp \n");
	}

	if (o->rxdemod == RX_AUDIO_FLAT) {
		target = 27000;
	} else {
		target = 23000;
	}

	usbradio_mixer_limits(o, &micmax, &spkrmax, &micplaymax);
	if (micmax <= 0) {
		ast_cli(fd, "ERROR: RX mixer is unavailable.\n");
		return;
	}
	settingmax = micmax;

	o->fever = 1;
	o->radio->fever = 1;

	o->radio->b.tuning = 1;

	setting = settingstart;

	ast_cli(fd, "tune rxnoise maxtries=%i, target=%i, tolerance=%i\n", maxtries, target,
		tolerance);

	while (tries < maxtries) {
		usbradio_set_rx_mixer(o, setting);

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
		stats_index = (o->rxaudiostats.index + AUDIO_STATS_LEN - 1) % AUDIO_STATS_LEN;
		rms = (unsigned int)(sqrt((double)o->rxaudiostats.pwrbuf[stats_index]) + 0.5);
		peak_dbfs = 20.0 * log10((double)meas / 32768.0);
		rms_dbfs = rms ? 20.0 * log10((double)rms / 32768.0) : -96.0;
		ast_cli(fd, "tries=%i, setting=%i, Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS)\n",
			tries, setting, meas, peak_dbfs, rms, rms_dbfs);

		if ((meas < (target - tolerance) || meas > (target + tolerance)) && tries <= 2) {
			f = (float)(setting * target) / meas;
			setting = (int)(f + 0.5);
		} else if (meas < (target - tolerance) && tries > 2) {
			setting++;
		} else if (meas > (target + tolerance) && tries > 2) {
			setting--;
		} else if (tries > 5 && meas > (target - tolerance) &&
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
		tries, ((setting * 1000) + (micmax / 2)) / micmax, meas, peak_dbfs, rms, rms_dbfs,
		measnoise);

	if (meas < target - tolerance || meas > target + tolerance) {
		ast_cli(fd, "ERROR: RX INPUT ADJUST FAILED.\n");
	} else {
		ast_cli(fd, "INFO: RX INPUT ADJUST SUCCESS.\n");
		setting = ((setting * 1000) + (micmax / 2)) / micmax;
		usbradioplus_processing_set_hardware_input_gain(urp_mixer_to_gain_db(setting));

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

/*!
 * \brief Process Asterisk CLI request for receiver deviation display.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct
 * \return	CLI success, showusage, or failure.
 */

/*!
 * \brief Process asterisk cli request for cos, ctcss, and ptt live display.
 * \param fd			Asterisk cli fd
 * \param o				Private struct
 * \return	Cli success, showusage, or failure.
 */

/*!
 * \brief Set received voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */
static void _menu_rxvoice(int fd, struct chan_usbradio_pvt *o, const char *str)
{
	int i, x;
	int micmax, spkrmax, micplaymax;
	float f, f1;
	int adjustment;

	if (!str[0]) {
		if (o->rxdemod == RX_AUDIO_FLAT) {
			ast_cli(fd, "Current Rx voice setting: %d\n",
				(int)((effective_legacy_rxvoiceadj(o) * 200.0) + .5));
		} else {
			ast_cli(fd, "Current Rx voice setting: %d\n", effective_rxmixerset(o));
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
		f = (float)i / 200.0;
	} else {
		usbradio_mixer_limits(o, &micmax, &spkrmax, &micplaymax);
		if (micmax <= 0) {
			ast_cli(fd, "RX mixer is unavailable.\n");
			return;
		}
		usbradioplus_processing_set_hardware_input_gain(urp_mixer_to_gain_db(i));
		/* adjust settings based on the device */
		adjustment = effective_rxmixerset(o) * micmax / AUDIO_ADJUSTMENT;
		/* get interval step size */
		f = AUDIO_ADJUSTMENT / (float)micmax;

		usbradio_set_rx_mixer(o, adjustment);
		f = 0.5 + (modff(((float)i) / f, &f1) * .093981);
	}
	usbradioplus_processing_set_local_input_gain(20.0 * log10(fmax(0.000001, 2.0 * f)));
	*(o->radio->prxVoiceAdjust) = f * M_Q8;
	ast_cli(fd, "Changed rx voice setting to %d\n", i);
}

/*!
 * \brief Print settings.
 * \param fd			Asterisk CLI fd
 * \param o				Private struct.
 */
static void _menu_print(int fd, struct chan_usbradio_pvt *o)
{
	char devstr[sizeof(o->devstr)];
	char serial[sizeof(o->serial)];
	int alsa_card;

	usbradio_device_identity(o, devstr, sizeof(devstr), serial, sizeof(serial), &alsa_card);
	ast_cli(fd, "Active radio interface is [%s]\n", usbradio_active);
	ast_cli(fd, "Device String is %s\n", ast_strlen_zero(devstr) ? "unassigned" : devstr);
	if (!ast_strlen_zero(serial)) {
		ast_cli(fd, "Device Serial is %s\n", serial);
	}
	ast_cli(fd, "Card is %i\n", alsa_card);
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
			(int)((effective_legacy_rxvoiceadj(o) * 200.0) + .5));
	} else {
		ast_cli(fd, "Rx Level currently set to %d\n", effective_rxmixerset(o));
	}
	ast_cli(fd, "Rx Squelch currently set to %d\n", o->rxsquelchadj);
	ast_cli(fd, "Tx Voice Level currently set to %d\n", o->txmixaset);
	ast_cli(fd, "Tx Tone Level currently set to %d\n", o->txctcssadj);
}

/*!
 * \brief Set squelch level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New squelch level.
 */

/*!
 * \brief Set tx voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */

/*!
 * \brief Set aux voice level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */

/*!
 * \brief Set tx tone level.
 * \param fd			Asterisk CLI fd.
 * \param o				chan_usbradio structure.
 * \param str			New voice level.
 */

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
		usbradio_mixer_limits(o, &micmax, &spkrmax, &micplaymax);
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
				ast_debug(3, "TX soft limiter set failed in tune menu-support\n");
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
			if (!end || *end || level < 0 || level > DUPLEX3_LEVEL_MAX) {
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
static void tune_write(struct chan_usbradio_pvt *o)
{
	struct ast_config *cfg;
	struct ast_category *category = NULL;
	struct ast_flags config_flags = {CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE};
	const float old_rxctcssadj = 0.5; /* for backward EEPROM format compatibility */

	if (!(cfg = ast_config_load2(CONFIG, "chan_usbradio", config_flags))) {
		ast_log(LOG_ERROR, "Config file not found: %s\n", CONFIG);
		return;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file has invalid format: %s\n", CONFIG);
		return;
	}

#define CONFIG_UPDATE_STR(field)                                                                   \
	if (tune_variable_update(cfg, CONFIG, category, #field, o->field)) {                       \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field);                             \
	}

#define CONFIG_UPDATE_INT(field)                                                                   \
	{                                                                                          \
		char _buf[15];                                                                     \
		snprintf(_buf, sizeof(_buf), "%d", o->field);                                      \
		if (tune_variable_update(cfg, CONFIG, category, #field, _buf)) {                   \
			ast_log(LOG_WARNING, "Failed to update %s\n", #field);                     \
		}                                                                                  \
	}

#define CONFIG_UPDATE_BOOL(field)                                                                  \
	if (tune_variable_update(cfg, CONFIG, category, #field, o->field ? "yes" : "no")) {        \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field);                             \
	}

#define CONFIG_UPDATE_FLOAT(field)                                                                 \
	{                                                                                          \
		char _buf[15];                                                                     \
		snprintf(_buf, sizeof(_buf), "%f", o->field);                                      \
		if (tune_variable_update(cfg, CONFIG, category, #field, _buf)) {                   \
			ast_log(LOG_WARNING, "Failed to update %s\n", #field);                     \
		}                                                                                  \
	}

#define CONFIG_UPDATE_SIGNAL(key, field, signal_type)                                              \
	if (tune_variable_update(cfg, CONFIG, category, #key, signal_type[o->field])) {            \
		ast_log(LOG_WARNING, "Failed to update %s\n", #field);                             \
	}

	category = ast_category_get(cfg, o->name, NULL);
	if (!category) {
		ast_log(LOG_ERROR, "No category '%s' exists?\n", o->name);
	} else {
		char assigned_devstr[sizeof(o->devstr)];
		char assigned_serial[sizeof(o->serial)];
		int automatic;

		/*
		 * To simplify channel driver setup, blank "devstr=" and "serial="
		 * values request automatic assignment of the first compatible USB
		 * radio device.
		 *
		 * We preserve that automatic configuration when only one active lease
		 * was assigned automatically. When multiple leases were assigned
		 * automatically, their assignment order is indeterminate, so save this
		 * channel's acquired identity as an explicit selector.
		 *
		 * Explicit selectors are saved unchanged. A topology-based devstr may
		 * stop matching if the interface is moved to another USB port, while a
		 * serial selector continues to take precedence over devstr.
		 */
		automatic = ast_strlen_zero(o->devstr) && ast_strlen_zero(o->serial);
		if (!automatic) {
			CONFIG_UPDATE_STR(devstr);
			if (!ast_strlen_zero(o->serial) ||
			    ast_variable_retrieve(cfg, o->name, "serial")) {
				CONFIG_UPDATE_STR(serial);
			}
		} else if (ast_radio_device_automatic_count() > 1) {
			usbradio_device_identity(o, assigned_devstr, sizeof(assigned_devstr),
						 assigned_serial, sizeof(assigned_serial), NULL);
			if (!ast_strlen_zero(assigned_serial)) {
				if (tune_variable_update(cfg, CONFIG, category, "serial",
							 assigned_serial)) {
					ast_log(LOG_WARNING, "Failed to update serial\n");
				}
			} else if (!ast_strlen_zero(assigned_devstr) &&
				   tune_variable_update(cfg, CONFIG, category, "devstr",
							assigned_devstr)) {
				ast_log(LOG_WARNING, "Failed to update devstr\n");
			}
		}
		CONFIG_UPDATE_INT(rxmixerset);
		CONFIG_UPDATE_INT(txmixaset);
		CONFIG_UPDATE_INT(txmixbset);
		CONFIG_UPDATE_FLOAT(rxctcssadj);
		CONFIG_UPDATE_INT(txctcssadj);
		CONFIG_UPDATE_INT(rxsquelchadj);
		CONFIG_UPDATE_INT(fever);
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
		CONFIG_UPDATE_INT(duplex3);
		if (tune_variable_update(cfg, CONFIG, category, "duplex3mode",
					 o->duplex3mode == DUPLEX3_MODE_SOFTWARE ? "software"
										 : "hardware")) {
			ast_log(LOG_WARNING, "Failed to update duplex3mode\n");
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
	if (usbradioplus_processing_save_input_gains(urp_mixer_to_gain_db(effective_rxmixerset(o)),
						     effective_rx_input_gain_db(o))) {
		ast_log(LOG_WARNING, "Failed to save receive input gains\n");
	}

	if (o->wanteeprom) {
		ast_mutex_lock(&o->eepromlock);
		while (o->eepromctl) {
			ast_mutex_unlock(&o->eepromlock);
			usleep(10000);
			ast_mutex_lock(&o->eepromlock);
		}
		memset(o->eeprom, 0, sizeof(o->eeprom));
		o->eeprom[EEPROM_USER_RXMIXERSET] = effective_rxmixerset(o);
		o->eeprom[EEPROM_USER_TXMIXASET] = o->txmixaset;
		o->eeprom[EEPROM_USER_TXMIXBSET] = o->txmixbset;
		{
			float compatibility_gain = effective_legacy_rxvoiceadj(o);
			memcpy(&o->eeprom[EEPROM_USER_RXVOICEADJ], &compatibility_gain,
			       sizeof(float));
		}
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
	const struct ast_radio_mixer_element *element;
	struct ast_radio_device *device;
	int requested;
	long sidetone_max;
	size_t path_index;

	ast_mutex_lock(&o->device_lock);
	device = o->radio_device;
	if (!device) {
		ast_mutex_unlock(&o->device_lock);
		return;
	}

	/* Hardware duplex-3 uses the portable 0--999 setting on every CM119. */
	sidetone_max = ast_radio_device_mixer_max(device, device->mixer_sidetone_paths,
						  AST_RADIO_MIXER_PLAYBACK_VOLUME);
	ast_radio_device_set_mixer_paths(
		device, device->mixer_sidetone_paths, device->mixer_sidetone_path_count,
		AST_RADIO_MIXER_PLAYBACK_VOLUME,
		o->duplex3mode == DUPLEX3_MODE_HARDWARE
			? (o->duplex3 * sidetone_max + DUPLEX3_LEVEL_MAX / 2) / DUPLEX3_LEVEL_MAX
			: 0);
	ast_radio_device_set_mixer_paths(device, device->mixer_sidetone_paths,
					 device->mixer_sidetone_path_count,
					 AST_RADIO_MIXER_PLAYBACK_SWITCH, 0);

	/* Transmitter output paths (Speaker/Headphone controls on CM108)
	 * txmixaset (MIXA) controls mixer_tx_paths[0], and txmixbset (MIXB) controls
	 * mixer_tx_paths[1] when a second path is present. Additional paths are unchanged.
	 */
	for (path_index = 0; path_index < device->mixer_tx_path_count && path_index < 2;
	     path_index++) {
		element =
			ast_radio_device_mixer_element(device, &device->mixer_tx_paths[path_index]);
		if (!element) {
			continue;
		}
		requested = path_index ? effective_txmixbset(o) : effective_txmixaset(o);
		ast_radio_device_set_mixer(device, &device->mixer_tx_paths[path_index],
					   AST_RADIO_MIXER_PLAYBACK_SWITCH, 1);
		ast_radio_device_set_mixer(
			device, &device->mixer_tx_paths[path_index],
			AST_RADIO_MIXER_PLAYBACK_VOLUME,
			ast_radio_device_mixer_scale(device, &device->mixer_tx_paths[path_index],
						     AST_RADIO_MIXER_PLAYBACK_VOLUME, requested));
	}

	/* adjust settings based on the device */

	/* Receiver input paths (Mic Capture controls on CM108) */
	for (path_index = 0; path_index < device->mixer_rx_path_count; path_index++) {
		element =
			ast_radio_device_mixer_element(device, &device->mixer_rx_paths[path_index]);
		if (!element) {
			continue;
		}
		ast_radio_device_set_mixer(
			device, &device->mixer_rx_paths[path_index], AST_RADIO_MIXER_CAPTURE_VOLUME,
			ast_radio_device_mixer_scale(device, &device->mixer_rx_paths[path_index],
						     AST_RADIO_MIXER_CAPTURE_VOLUME,
						     effective_rxmixerset(o)));
		ast_radio_device_set_mixer(device, &device->mixer_rx_paths[path_index],
					   AST_RADIO_MIXER_CAPTURE_SWITCH, 1);
	}

	/* Optional receiver input gain/AGC paths */
	ast_radio_device_set_mixer_paths(device, device->mixer_rx_boost_paths,
					 device->mixer_rx_boost_path_count,
					 AST_RADIO_MIXER_PLAYBACK_SWITCH, 1);
	ast_mutex_unlock(&o->device_lock);
}

/*!
 * \brief Adjust DSP multiplier
 * Adjusts the DSP multiplier to add resolution to the tx level adjustment
 *
 * \param		chan_usbradio structure.
 */

/*!
 * \brief Calculate multiplier.
 * \param value		Level to calculate.
 * \returns			Multiplier.
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

/*!
 * \brief Dump radio configuration and detector state.
 * \param o				Private struct.
 */
static void radio_dump(struct chan_usbradio_pvt *o, int fd)
{
	urp_radio_state *p;
	int i;
	int micmax, spkrmax, micplaymax;

	p = o->radio;

	ast_cli(fd, "\nodump()\n");

	/* Acquired device identity */
	ast_mutex_lock(&o->device_lock);
	if (o->radio_device) {
		ast_cli(fd, "radio_device->devstr = %s\n", o->radio_device->devstr);
		ast_cli(fd, "radio_device->alsa_card = %d\n", o->radio_device->alsa_card);
	}
	ast_mutex_unlock(&o->device_lock);

	usbradio_mixer_limits(o, &micmax, &spkrmax, &micplaymax);
	pd(micmax);
	pd(spkrmax);

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

	pd(o->txlimonly);
	pd(o->txprelim);
	pd(o->txmixa);
	pd(o->txmixb);

	pd(o->txmixaset);
	pd(o->txmixbset);

	ast_cli(fd, "\nnative radio state\n");

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

/*!
 * \brief Store configuration.
 *	Initializes chan_usbradio and loads it with the configuration data.
 * \param cfg			ast_config structure.
 * \param ctg			Category.
 * \return				chan_usbradio_pvt.
 */

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
			o->hidthread = AST_PTHREADT_NULL;
			o->audiothread = AST_PTHREADT_NULL;
			if (!usbradio_active) {
				usbradio_active = o->name;
			}
		}
	}
	o->echoq.q_forw = o->echoq.q_back = &o->echoq;
	ast_mutex_init(&o->echolock);
	ast_mutex_init(&o->eepromlock);
	ast_mutex_init(&o->usblock);
	ast_mutex_init(&o->device_lock);
	ast_mutex_init(&o->swap_lock);
	ast_mutex_init(&o->plus_link_lock);
	o->echomax = DEFAULT_ECHO_MAX;
	/* fill other fields from configuration */
	for (v = ast_variable_browse(cfg, ctg); v; v = v->next) {
		int cutoff_result = store_cutoff(o, v->name, v->value);
		if (cutoff_result < 0) {
			ast_log(LOG_ERROR, "RadioPlus/%s: invalid %s cutoff '%s'\n", ctg, v->name,
				v->value);
			if (o != &usbradio_default)
				ast_free(o);
			return NULL;
		}
		if (cutoff_result > 0)
			continue;
		CV_START((char *)v->name, (char *)v->value);

		/* handle jb conf */
		if (!ast_jb_read_conf(&global_jbconf, v->name, v->value)) {
			continue;
		}

		CV_BOOL("rxcpusaver", o->rxcpusaver);
		CV_BOOL("txcpusaver", o->txcpusaver);
		CV_BOOL("invertptt", o->invertptt);
		CV_F("rxdemod", store_rxdemod(o, (char *)v->value));
		CV_BOOL("txlimonly", o->txlimonly);
		CV_BOOL("txprelim", o->txprelim);
		CV_F("txmixa", store_txmixa(o, (char *)v->value));
		CV_F("txmixb", store_txmixb(o, (char *)v->value));
		CV_F("carrierfrom", store_rxcdtype(o, (char *)v->value));
		CV_UINT("voxhangtime", o->voxhangtime);
		CV_F("ctcssfrom", store_rxsdtype(o, (char *)v->value));
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
		CV_UINT("rxctcssrelax", o->rxctcssrelax);
		CV_F("txtoctype", store_txtoctype(o, (char *)v->value));
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
				ast_log(LOG_ERROR,
					"RadioPlus/%s: duplex3mode must be hardware or software, "
					"not '%s'\n",
					ctg, v->value);
				if (o != &usbradio_default)
					ast_free(o);
				return NULL;
			}
			continue;
		}
		if (!strcasecmp(v->name, "emphasis_corner_hz")) {
			o->plus_emphasis_corner_hz = strtod(v->value, NULL);
			continue;
		}
		CV_UINT("sendvoter", o->sendvoter);
		CV_UINT("clipledgpio", o->clipledgpio);
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
	if (apply_processing_config_overrides(o, ctg)) {
		if (o != &usbradio_default)
			ast_free(o);
		return NULL;
	}
	if (o->plus_rxhpf_enabled && o->plus_rxlpf_enabled &&
	    (o->plus_rxhpf_exact ? o->plus_rxhpf_hz
				 : usbradioplus_legacy_cutoff("rxhpf", o->rxhpf)) >=
		    (o->plus_rxlpf_exact ? o->plus_rxlpf_hz
					 : usbradioplus_legacy_cutoff("rxlpf", o->rxlpf))) {
		ast_log(LOG_ERROR, "RadioPlus/%s: rxhpf cutoff must be below rxlpf cutoff\n", ctg);
		if (o != &usbradio_default)
			ast_free(o);
		return NULL;
	}
	if (o->duplex3 < 0 || o->duplex3 > DUPLEX3_LEVEL_MAX) {
		ast_log(LOG_ERROR, "RadioPlus/%s: duplex3 must be between 0 and %d\n", ctg,
			DUPLEX3_LEVEL_MAX);
		if (o != &usbradio_default)
			ast_free(o);
		return NULL;
	}
	if (o->plus_txhpf_enabled && o->plus_txlpf_enabled &&
	    (o->plus_txhpf_exact ? o->plus_txhpf_hz
				 : usbradioplus_legacy_cutoff("txhpf", o->txhpf)) >=
		    (o->plus_txlpf_exact ? o->plus_txlpf_hz
					 : usbradioplus_legacy_cutoff("txlpf", o->txlpf))) {
		ast_log(LOG_ERROR, "RadioPlus/%s: txhpf cutoff must be below txlpf cutoff\n", ctg);
		if (o != &usbradio_default)
			ast_free(o);
		return NULL;
	}

	if (o->rxsdtype != SD_XPMR) {
		o->rxctcssfreqs[0] = 0;
		o->txctcssfreqs[0] = 0;
	}

	if ((o->txmixa == TX_OUT_COMPOSITE) && (o->txmixb == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have B channel be Voice with A "
				   "channel being Composite!!\n");
	}
	if ((o->txmixb == TX_OUT_COMPOSITE) && (o->txmixa == TX_OUT_VOICE)) {
		ast_log(LOG_ERROR, "Invalid Configuration: Can not have A channel be Voice with B "
				   "channel being Composite!!\n");
	}

	if (o == &usbradio_default) { /* we are done with the default */
		return NULL;
	}
	if (o->plus_emphasis_corner_hz <= 0.0 || o->plus_emphasis_corner_hz >= 300.0) {
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
	if (apply_processing_config_overrides(o, ctg))
		return NULL;

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
	if (o->radio == NULL) {
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

		if (o->txlimonly) {
			tChan.txMod = 1;
		}
		if (o->txprelim) {
			tChan.txMod = 2;
		}

		tChan.txMixA = effective_txmixa(o);
		tChan.txMixB = effective_txmixb(o);

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

		*(o->radio->prxSquelchAdjust) =
			((999 - o->rxsquelchadj) * 32767) / AUDIO_ADJUSTMENT;
		*(o->radio->prxVoiceAdjust) = effective_legacy_rxvoiceadj(o) * M_Q8;
		*(o->radio->prxCtcssAdjust) = o->rxctcssadj * M_Q8;
		o->radio->rxCtcss->relax = o->rxctcssrelax;
		o->radio->txTocType = o->txtoctype;

		if ((o->txmixa != TX_OUT_VOICE) && (o->txmixb != TX_OUT_VOICE) &&
		    (o->txmixa != TX_OUT_COMPOSITE) && (o->txmixb != TX_OUT_COMPOSITE)) {
			ast_log(LOG_ERROR, "No txvoice output configured.\n");
		}

		if (o->txctcssfreq[0] && o->txmixa != TX_OUT_LSD && o->txmixa != TX_OUT_COMPOSITE &&
		    o->txmixb != TX_OUT_LSD && o->txmixb != TX_OUT_COMPOSITE) {
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
			     "{auxvoice|dump|swap|rxnoise|rxvoice|rxtone|txvoice|txtone|txall|"
			     "flash|rxsquelch|nocap|rxtracecap|"
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
		e->usage =
			"Usage: radio active [device-name]\n"
			"       If used without a parameter, displays which device is the current\n"
			"       one being commanded.  If a device is specified, the commanded "
			"radio device is changed\n"
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

#define URP_CHECK_TX_AUDIO(channel, samples, count)                                                \
	ast_radio_check_audio((samples), &(channel)->txaudiostats, (count), 0)
#include "usbradioplus_native_tick.inc"
#undef URP_CHECK_TX_AUDIO

static char *handle_radioplus_native_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
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
	if (a->argc != 3 && a->argc != 4)
		return CLI_SHOWUSAGE;
	o = find_desc(usbradio_active);
	if (!o)
		return CLI_FAILURE;
	if (a->argc == 4) {
		if (strcasecmp(a->argv[3], "reset"))
			return CLI_SHOWUSAGE;
		o->plus_tx_program_peak_dbfs = -INFINITY;
		o->plus_tx_program_max_peak_dbfs = -INFINITY;
		o->plus_tx_program_rail_samples = 0;
		o->plus_local_tx_peak_dbfs = -INFINITY;
		o->plus_local_tx_max_peak_dbfs = -INFINITY;
		o->plus_local_tx_rail_samples = 0;
		o->plus_adc_peak_dbfs = o->plus_adc_max_peak_dbfs = -INFINITY;
		o->plus_adc_rail_samples = 0;
		o->plus_deemphasis_peak_dbfs = o->plus_deemphasis_max_peak_dbfs = -INFINITY;
		o->plus_preemphasis_input_peak_dbfs = o->plus_preemphasis_input_max_peak_dbfs =
			-INFINITY;
		o->plus_preemphasis_input_ceiling_samples = 0;
		o->plus_link_queue_underflows = o->plus_link_queue_overflows = 0;
		o->plus_sound_zero_fill_frames = 0;
		o->plus_sound_dropped_frames = 0;
		o->plus_sound_short_writes = 0;
		o->plus_parrot_playback_frames = 0;
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
		o->plus_program_queue.high_water = o->plus_program_queue.count;
		ast_cli(a->fd, "Native peak and FIFO event counters reset.\n");
		return CLI_SUCCESS;
	}
	ast_cli(a->fd,
		"RadioPlus/%s native: frames %" PRIu64 ", SRC errors %" PRIu64
		", ADC peak %.1f/max %.1f dBFS, ADC rails %" PRIu64
		", deemphasis peak %.1f/max %.1f dBFS"
		", preemphasis input %.1f/max %.1f dBFS, input ceiling interventions %" PRIu64
		", local TX peak %.1f dBFS, local TX max %.1f dBFS, local ceiling interventions "
		"%" PRIu64 ", final TX peak %.1f dBFS, final TX max %.1f dBFS"
		", final ceiling interventions %" PRIu64
		", pre gain %.2f dB, FIFO %u/%u (high %u, underruns %" PRIu64 ", overruns %" PRIu64
		"), sound queue zero-fills %" PRIu64 ", dropped frames %" PRIu64
		", short/errors %" PRIu64 ", native echo %s, playback frames %" PRIu64
		", buffered %.2f seconds.\n",
		o->name, o->plus_native_frames, o->plus_src_errors, o->plus_adc_peak_dbfs,
		o->plus_adc_max_peak_dbfs, o->plus_adc_rail_samples, o->plus_deemphasis_peak_dbfs,
		o->plus_deemphasis_max_peak_dbfs, o->plus_preemphasis_input_peak_dbfs,
		o->plus_preemphasis_input_max_peak_dbfs, o->plus_preemphasis_input_ceiling_samples,
		o->plus_local_tx_peak_dbfs, o->plus_local_tx_max_peak_dbfs,
		o->plus_local_tx_rail_samples, o->plus_tx_program_peak_dbfs,
		o->plus_tx_program_max_peak_dbfs, o->plus_tx_program_rail_samples,
		urp_mixer_to_gain_db(effective_rxmixerset(o)), o->plus_program_queue.count,
		URP_PROGRAM_QUEUE_FRAMES, o->plus_program_queue.high_water,
		o->plus_link_queue_underflows, o->plus_link_queue_overflows,
		o->plus_sound_zero_fill_frames, o->plus_sound_dropped_frames,
		o->plus_sound_short_writes, o->plus_parrot_playing ? "playing" : "idle",
		o->plus_parrot_playback_frames, (double)o->plus_parrot_count / URP_RATE_NATIVE);
	ast_cli(a->fd,
		"Link clock recovery: app FIFO %u frames, native FIFO %u samples/%.2f ms, "
		"ratio correction %+.4f%%.\n",
		o->plus_program_queue.count, o->plus_native_fifo.count,
		1000.0 * o->plus_native_fifo.count / URP_RATE_NATIVE,
		100.0 * o->plus_link_clock.correction);
	ast_cli(a->fd,
		"FFmpeg local: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		o->plus_local_avfilter.input_peak_dbfs, o->plus_local_avfilter.input_max_peak_dbfs,
		o->plus_local_avfilter.input_rms_dbfs, o->plus_local_avfilter.input_max_rms_dbfs,
		o->plus_local_avfilter.output_peak_dbfs,
		o->plus_local_avfilter.output_max_peak_dbfs, o->plus_local_avfilter.output_rms_dbfs,
		o->plus_local_avfilter.output_max_rms_dbfs, o->plus_local_avfilter.latency_samples,
		1000.0 * o->plus_local_avfilter.latency_samples / URP_RATE_NATIVE,
		o->plus_local_avfilter.buffered_samples,
		o->plus_local_avfilter.startup_fill_samples,
		o->plus_local_avfilter.runtime_underrun_samples);
	ast_cli(a->fd,
		"FFmpeg final: input peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"output peak %.1f/max %.1f dBFS, RMS %.1f/max %.1f dBFS; "
		"latency %u samples/%.2f ms, buffered %u samples, startup fill %llu, "
		"runtime underruns %llu.\n",
		o->plus_final_avfilter.input_peak_dbfs, o->plus_final_avfilter.input_max_peak_dbfs,
		o->plus_final_avfilter.input_rms_dbfs, o->plus_final_avfilter.input_max_rms_dbfs,
		o->plus_final_avfilter.output_peak_dbfs,
		o->plus_final_avfilter.output_max_peak_dbfs, o->plus_final_avfilter.output_rms_dbfs,
		o->plus_final_avfilter.output_max_rms_dbfs, o->plus_final_avfilter.latency_samples,
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

static struct ast_cli_entry cli_usbradio[] = {
	AST_CLI_DEFINE(handle_console_key, "Simulate Rx Signal Present"),
	AST_CLI_DEFINE(handle_console_unkey, "Simulate Rx Signal Loss"),
	AST_CLI_DEFINE(handle_radio_tune, "Change radio settings"),
	AST_CLI_DEFINE(handle_radio_active, "Change commanded device"),
	AST_CLI_DEFINE(handle_set_dsp_debug, "Radio set detector debug level"),
	AST_CLI_DEFINE(handle_show_settings, "Show device settings"),
	AST_CLI_DEFINE(handle_radioplus_native_stats, "Show native RadioPlus statistics")};

#include "usbradioplus_radio.c"
#include "usbradioplus_dsp.c"
#include "usbradioplus_ctcss.c"
#include "usbradioplus_hardware.c"
#include "usbradioplus_repeat.c"
#include "usbradioplus_channel_core.c"
#include "./txagc/agc_core.c"
#include "./txagc/avfilter_processor.c"
#include "./txagc/rnnoise_processor.c"
#include "usbradioplus_processing.c"

/*!
 * \brief Load configuration.
 * \param reload		Flag to indicate if we are reloading.
 * \return				Success or failure.
 */

#include "usbradioplus_channel_common.inc"

static int load_module(void)
{
	if (!(usbradio_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT))) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_append(usbradio_tech.capabilities, ast_format_slin, 0);

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
	if (haspp && hasout) {
		ast_pthread_create_background(&pulserid, NULL, pulserthread, NULL);
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	struct chan_usbradio_pvt *o;
	size_t i;

	stoppulser = 1;
	usbradioplus_processing_unload();

	ast_channel_unregister(&usbradio_tech);
	ast_cli_unregister_multiple(cli_usbradio,
				    sizeof(cli_usbradio) / sizeof(struct ast_cli_entry));

	for (o = usbradio_default.next; o; o = o->next) {
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

		if (o->owner) {
			ast_softhangup(o->owner, AST_SOFTHANGUP_APPUNLOAD);
		}
		o->stopaudiothread = 1;
		o->stophid = 1;
		kickptt(o);
		if (o->audiothread != AST_PTHREADT_NULL) {
			pthread_join(o->audiothread, NULL);
			o->audiothread = AST_PTHREADT_NULL;
		}
		if (o->hidthread != AST_PTHREADT_NULL) {
			pthread_join(o->hidthread, NULL);
			o->hidthread = AST_PTHREADT_NULL;
		}
		ast_radio_pa_stop(&o->pa);
		usbradio_release_device(o);
		usbradioplus_dsp_destroy(o);
		if (o->radio) {
			urp_radio_destroy(o->radio);
			o->radio = NULL;
		}
		if (o->dsp) {
			ast_dsp_free(o->dsp);
		}
		for (i = 0; i < GPIO_PINCOUNT; i++) {
			if (o->gpios[i]) {
				ast_free(o->gpios[i]);
			}
		}
		for (i = 0; i < ARRAY_LEN(o->pps); i++) {
			if (o->pps[i]) {
				ast_free(o->pps[i]);
			}
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
