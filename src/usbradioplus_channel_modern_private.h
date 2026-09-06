/** @file
 * @brief Per-channel state for the ASL radio-device API adapter.
 */

#ifndef USBRADIOPLUS_CHANNEL_MODERN_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_MODERN_PRIVATE_H

struct chan_usbradio_pvt {
	struct chan_usbradio_pvt *next;

	char *name;	/* the internal name of our channel */
	int devtype;	/* actual type of device */
	int pttkick[2]; /* ptt kick pipe */
	/** PortAudio stream state for this radio. */
	struct ast_radio_pa_stream pa;
	/** Audio-device open mode. */
	enum {
		M_UNSET /**< Audio device is closed. */,
		M_FULL /**< Full-duplex capture and playback. */,
		M_READ /**< Capture-only device mode. */,
		M_WRITE /**< Playback-only device mode. */
	} duplex;
	int hookstate;

	char devstr[128];
	char serial[128];
	/* Retained as accepted compatibility settings. PortAudio owns buffering in
	 * the shared-device API and therefore does not consume OSS fragment sizes. */
	unsigned int queuesize;
	unsigned int frags;

	pthread_t hidthread;
	/** Native audio worker thread. */
	pthread_t audiothread;
	int stophid;
	/** Stop request observed by the audio worker. */
	volatile sig_atomic_t stopaudiothread;
	/** Nonzero while a USB interface is acquired. */
	volatile sig_atomic_t hasusb; /* HID/audio liveness; not a bit-field (cross-thread) */
	/** Nonzero after the audio worker completes initialization. */
	char audio_thread_ready;
	/** Most recent successful audio-worker timestamp. */
	time_t lastaudiotime;
	enum {
		DEVICE_SWAP_IDLE /**< DEVICE SWAP IDLE. */,
		/*!< No device swap requested */
		DEVICE_SWAP_QUIESCING,
		/*!< Device handles are stopping */
		DEVICE_SWAP_READY,
		/*!< Device is ready for lease exchange */
		/** USB assignment swap handshake state. */
	} swap_state; /**< Audio/HID worker pause handshake for USB reassignment. */
	unsigned int swap_audio_ready : 1; /*!< PortAudio stopped for pending swap */
	/** Mutex protecting the worker swap handshake. */
	ast_mutex_t swap_lock; /* protects device swap state */

	struct ast_channel *owner;

	/* Shared USB radio device lease */
	/** Acquired ASL radio-device reference. */
	struct ast_radio_device *radio_device;
	/** Mutex protecting the acquired ASL radio-device reference. */
	ast_mutex_t device_lock;
	/** Latched USB/audio device error text. */
	enum ast_radio_device_result device_error;

	/* One native-rate stereo frame rendered for the PortAudio hardware tick. */
	/** Interleaved native-rate transmitter PCM workspace. */
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
	/** Nonzero until one silence frame releases the pending transmitter SRC tail. */
	unsigned int plus_link_src_pending;
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
		/** Nonzero while a reported USB/audio failure awaits recovery. */
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

#endif
