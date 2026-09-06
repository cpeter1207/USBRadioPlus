/** @file
 * @brief Per-channel state for the OSS and libusb radio adapter.
 */

#ifndef USBRADIOPLUS_CHANNEL_LEGACY_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_LEGACY_PRIVATE_H

/** Private per-radio state shared by channel callbacks and device workers. */
struct chan_usbradio_pvt {
	/** Next configured radio in the channel list. */
	struct chan_usbradio_pvt *next;

	/** Symbolic name used to identify this entry. */
	char *name; /* the internal name of our channel */
	/** Detected USB interface device type. */
	int devtype; /* actual type of device */
	/** Pipe used to wake the HID worker after PTT changes. */
	int pttkick[2]; /* ptt kick pipe */
	/** Total fragments available in the audio output device. */
	int total_blocks; /* total blocks in the output device */
	/** Open OSS audio device descriptor. */
	int sounddev;
	/** Audio-device open mode. */
	enum {
		M_UNSET /**< Audio device is closed. */,
		M_FULL /**< Full-duplex capture and playback. */,
		M_READ /**< Capture-only device mode. */,
		M_WRITE /**< Playback-only device mode. */
	} duplex;
	/** Current telephone-style channel hook state. */
	int hookstate;
	/** Maximum queued audio fragments. */
	unsigned int queuesize; /* max fragments in queue */
	/** OSS fragment count/size request. */
	unsigned int frags; /* parameter for SETFRAGMENT */

	/** Bit mask of device warnings already reported. */
	int warned; /* various flags used for warnings */

#define WARN_used_blocks 1

#define WARN_speed 2

#define WARN_frag 4

	/** Assigned ALSA sound card index. */
	char devicenum;
	/** Configured USB device identifier. */
	char devstr[128];
	/** Configured USB serial number. */
	char serial[128];
	/** Maximum ALSA playback mixer step. */
	int spkrmax;
	/** Maximum ALSA microphone capture step. */
	int micmax;
	/** Maximum ALSA microphone-playback mixer step. */
	int micplaymax;

	/** USB HID worker thread. */
	pthread_t hidthread;
	/** Stop request observed by the HID worker. */
	int stophid;

	/** Asterisk channel currently owning this radio. */
	struct ast_channel *owner;

	/* buffer used in usbradio_write, 2 per int by 2 channels by 6 times oversampling (48KS/s)
	 */
	/** Interleaved native-rate transmitter PCM workspace. */
	char usbradio_write_buf[FRAME_SIZE * 2 * 2 * 6];
	/** Raw native-rate mono PCM from the CM119 ADC. */
	short plus_rx_native[URP_NATIVE_SAMPLES];
	/** Floating-point local-receiver processing workspace. */
	double plus_local_native[URP_NATIVE_SAMPLES];
	/** Configured app_rpt sample rate in Hz. */
	unsigned int plus_app_rpt_rate;
	/** Samples in one app_rpt frame at the configured rate. */
	unsigned int plus_app_rpt_samples;
	/** One native-rate app_rpt transmitter block. */
	short plus_link_native[URP_NATIVE_SAMPLES];
	/** Workspace for elastic app_rpt-to-native resampling. */
	short plus_link_resampled[URP_NATIVE_SAMPLES * 2];
	/** Elastic native-rate transmitter FIFO. */
	struct urp_native_fifo plus_native_fifo;
	/** App_rpt-rate program input workspace. */
	short plus_link_8k[URP_NATIVE_SAMPLES];
	/** Pending app_rpt voice frames. */
	struct urp_program_queue plus_program_queue;
	/** Count of empty app_rpt queue reads. */
	uint64_t plus_link_queue_underflows;
	/** Count of app_rpt queue overflow corrections. */
	uint64_t plus_link_queue_overflows;
	/** Mutex protecting app_rpt queue access. */
	ast_mutex_t plus_link_lock;
	/** Undelayed detector-input copy of native receive audio. */
	short plus_squelch_native[URP_NATIVE_SAMPLES * 2];
	/** Native receiver squelch-tail delay ring. */
	short plus_rx_delay[RXSQDELAYBUFSIZE * 6];
	/** Current receiver delay-ring position. */
	unsigned int plus_rx_delay_index;
	/** App_rpt-to-native streaming resampler. */
	struct urp_src *plus_up;
	/** Nonzero until one silence frame releases the pending transmitter SRC tail. */
	unsigned int plus_link_src_pending;
	/** Native-to-app_rpt streaming resampler. */
	struct urp_src *plus_down;
	/** Smoothed clock correction derived from transmitter FIFO occupancy. */
	struct urp_clock_recovery plus_link_clock;
	/** Tracks emphasis selection for local native repeat audio. */
	unsigned int plus_local_preemphasis_active;
	/** Tracks emphasis selection for app_rpt transmitter audio. */
	unsigned int plus_link_preemphasis_active;
	/** Local-receiver optional FFmpeg processing graph. */
	struct txagc_avfilter plus_local_avfilter;
	/** Fixed receive deemphasis and input-gain graph. */
	struct txagc_avfilter plus_rx_filter;
	/** Fixed receive band-pass and PL-filter graph. */
	struct txagc_avfilter plus_rx_filter_after;
	/** Fixed transmitter and voice/telemetry FFmpeg graph. */
	struct txagc_avfilter plus_final_avfilter;
	/** Local-receiver RNNoise stream state. */
	struct txagc_rnnoise plus_local_rnnoise;
	/** Resolved emphasis corner frequency in Hz. */
	double plus_emphasis_corner_hz;
	/** Nonzero after the first resolved hardware settings update. */
	int plus_hardware_applied;
	/** plus_applied_txmixbset: Last applied normalized output-B mixer step. */
	/** plus_applied_txmixaset: Last applied normalized output-A mixer step. */
	/** Last applied normalized capture mixer step. */
	int plus_applied_rxmixer;
	/** Last applied normalized output-A mixer setting. */
	int plus_applied_txmixaset;
	/** Last applied normalized output-B mixer setting. */
	int plus_applied_txmixbset;
	/** plus_applied_txmixb: Last applied output-B routing assignment. */
	/** Last applied output-A routing assignment. */
	int plus_applied_txmixa;
	/** Last applied output-B routing assignment. */
	int plus_applied_txmixb;
	/** plus_applied_txctcssfreqs: Last applied transmit CTCSS frequency list. */
	/** Last applied receive CTCSS frequency list. */
	char plus_applied_rxctcssfreqs[512];
	/** Last applied transmit CTCSS frequency list. */
	char plus_applied_txctcssfreqs[512];
	/** Number of native audio blocks processed. */
	uint64_t plus_native_frames;
	/** Cumulative sample-rate-conversion failures. */
	uint64_t plus_src_errors;
	/** ADC peak in DBFS. */
	double plus_adc_peak_dbfs;
	/** ADC max peak in DBFS. */
	double plus_adc_max_peak_dbfs;
	/** Cumulative ADC samples at either signed PCM rail. */
	uint64_t plus_adc_rail_samples;
	/** Deemphasis peak in DBFS. */
	double plus_deemphasis_peak_dbfs;
	/** Deemphasis max peak in DBFS. */
	double plus_deemphasis_max_peak_dbfs;
	/** Preemphasis input peak in DBFS. */
	double plus_preemphasis_input_peak_dbfs;
	/** Preemphasis input max peak in DBFS. */
	double plus_preemphasis_input_max_peak_dbfs;
	/** Transmitter emphasis-input samples beyond the measured PCM range. */
	uint64_t plus_preemphasis_input_ceiling_samples;
	/** Transmitter program peak in DBFS. */
	double plus_tx_program_peak_dbfs;
	/** Transmitter program max peak in DBFS. */
	double plus_tx_program_max_peak_dbfs;
	/** Cumulative transmitter program samples beyond signed PCM range. */
	uint64_t plus_tx_program_rail_samples;
	/** Local transmitter peak in DBFS. */
	double plus_local_tx_peak_dbfs;
	/** Local transmitter max peak in DBFS. */
	double plus_local_tx_max_peak_dbfs;
	/** Cumulative local-repeat samples beyond signed PCM range. */
	uint64_t plus_local_tx_rail_samples;
	/** Hardware output frames filled with silence. */
	uint64_t plus_sound_zero_fill_frames;
	/** Hardware output frames discarded. */
	uint64_t plus_sound_dropped_frames;
	/** Incomplete or failed hardware audio writes. */
	uint64_t plus_sound_short_writes;
	/** Native echo blocks transmitted. */
	uint64_t plus_parrot_playback_frames;
	/** Owned native echo recording and playback cursor. */
	struct urp_parrot_state plus_parrot_state;

	/* buffers used in usbradio_read - AST_FRIENDLY_OFFSET space for headers
	 * plus enough room for a full frame
	 */
	/** Native stereo receive PCM with Asterisk frame headroom. */
	_Alignas(short) char usbradio_read_buf[FRAME_SIZE * (2 * 12) +
					       AST_FRIENDLY_OFFSET]; /* 2 bytes * 2 channels * 6 for
									48K */
	/** App_rpt-rate receive PCM with Asterisk frame headroom. */
	char usbradio_read_buf_8k[URP_NATIVE_SAMPLES * 2 + AST_FRIENDLY_OFFSET];
	/** Bytes accumulated in the current receive block. */
	int readpos; /* read position above */
	/** Asterisk frame returned by the receive callback. */
	struct ast_frame read_f; /* returned by usbradio_read */

	/** Previous receiver indication state. */
	char lastrx;
	/** Raw USB GPIO carrier indication. */
	char rxhidsq;
	/** Raw USB GPIO CTCSS indication. */
	char rxhidctcss;
	/** Carrier state reported by the native detector. */
	char rxcarrierdetect; /* status from native radio detector */
	/** CTCSS state reported by the native decoder. */
	char rxctcssdecode; /* status from native CTCSS decoder */
	/** Raw parallel-port carrier indication. */
	char rxppsq;
	/** Raw parallel-port CTCSS indication. */
	char rxppctcss;

	/** Qualified receiver key state. */
	char rxkeyed; /* Indicates rx signal is present */

	/** Previous transmitter indication state. */
	char lasttx;
	/** PTT request received from app_rpt. */
	char txkeyed; /* tx key request from upper layers */
	/** PTT request from the calibration utility. */
	char txtestkey;
	/** Nonzero selects the native calibration tone. */
	char plus_test_tone_enabled;
	/** Native calibration oscillator phase in radians. */
	double plus_test_tone_phase;
	/** Continuous-phase native CTCSS oscillator. */
	struct urp_ctcss_generator plus_ctcss_generator;

	/** Most recent successful HID worker timestamp. */
	time_t lasthidtime;
	/** Asterisk DSP instance used for DTMF detection. */
	struct ast_dsp *dsp;

	/** Whether the radio interface can receive while transmitting. */
	char radioduplex; /* parameter for radio duplex setting */

	/** Selected radio diagnostic trace format. */
	int tracetype;
	/** Maximum enabled radio trace verbosity. */
	int tracelevel;
	/** Configured signaling area identifier. */
	char area;
	/** Configured signaling repeater number. */
	char rptnum;
	/** Configured signaling idle interval. */
	int idleinterval;
	/** Configured signaling turn-off count. */
	int turnoffs;
	/** Delay after PTT assertion in milliseconds. */
	int txsettletime;
	/** Radio-signaling blanking interval in milliseconds. */
	int txrxblankingtime;
	/** Configured signaling user key. */
	char ukey[48];

	/** Received DCS indication retained in radio state. */
	int rxdcsdecode;
	/** Received low-speed-data indication retained in radio state. */
	int rxlsddecode;

	/** Counts the number of 20 ms intervals after RX activity */
	int rxoncnt; /* Counts the number of 20 ms intervals after RX activity */
	/** Counts the number of 20 ms intervals after TX unkey */
	int txoffcnt; /* Counts the number of 20 ms intervals after TX unkey */
	/** This is the value which RX is ignored after RX activity */
	int rxondelay; /* This is the value which RX is ignored after RX activity */
	/** This is the value which RX is ignored after TX unkey */
	int txoffdelay; /* This is the value which RX is ignored after TX unkey */

	/** Owned native radio-signaling engine. */
	urp_radio_state *radio;

	/** Receiver audio-source assignment. */
	enum radio_rx_audio rxdemod;
	/** Carrier-detection source assignment. */
	enum radio_carrier_detect rxcdtype;
	/** VOX carrier hold time in milliseconds. */
	int voxhangtime; /* if rxcdtype=vox, ms to wait detecting RX audio before setting CD=0 */
	/** CTCSS indication-source assignment. */
	enum radio_squelch_detect rxsdtype;
	/** Normalized DSP noise-squelch threshold. */
	int rxsquelchadj; /* this copy needs to be here for initialization */
	/** Noise-squelch close margin above the opening threshold. */
	int rxsqhyst;
	/** VOX detector threshold. */
	int rxsqvoxadj;
	/** Selected discriminator-noise detector filter. */
	int rxnoisefiltype;
	/** Receiver squelch-tail audio delay. */
	int rxsquelchdelay;
	/** Transmitter soft-limiter calibration setpoint. */
	int txslimsp;
	/** Transmit CTCSS turn-off behavior. */
	enum usbradio_carrier_type txtoctype;

	/** Transmit CTCSS amplitude multiplier. */
	float txctcssgain;
	/** Output-A voice/CTCSS routing assignment. */
	enum radio_tx_mix txmixa;
	/** Output-B voice/CTCSS routing assignment. */
	enum radio_tx_mix txmixb;
	/** Receiver detector low-pass filter selector. */
	int rxlpf;
	/** Receiver detector high-pass filter selector. */
	int rxhpf;
	/** Transmitter low-pass filter selector retained in radio settings. */
	int txlpf;
	/** Transmitter high-pass filter selector retained in radio settings. */
	int txhpf;

	/** CTCSS decoder qualification/talk-off tolerance. */
	char rxctcssrelax;
	/** Linear gain applied only to the receive CTCSS decoder. */
	float rxctcssadj;

	/** Default transmitted CTCSS frequency. */
	char txctcssdefault[16]; /* for repeater operation */
	/** Receive CTCSS frequency list in Hz. */
	char rxctcssfreqs[512]; /* a string */
	/** Transmit CTCSS frequency list in Hz. */
	char txctcssfreqs[512];

	/** Selected transmitted CTCSS frequency. */
	char txctcssfreq[32]; /* encode now */
	/** Currently decoded receive CTCSS frequency. */
	char rxctcssfreq[32]; /* decode now */

	/** Number of receive CTCSS frequencies. */
	char numrxctcssfreqs; /* how many */
	/** Number of transmit CTCSS frequencies. */
	char numtxctcssfreqs;

	/** Configured receive CTCSS code values. */
	char *rxctcss[CTCSS_NUM_CODES]; /* pointers to strings */
	/** Configured transmit CTCSS code values. */
	char *txctcss[CTCSS_NUM_CODES];

	/** Configured transmitter frequency in Hz. */
	int txfreq; /* in Hz */
	/** Configured receiver frequency in Hz. */
	int rxfreq;

	/*      start remote operation info */
	/** for remote operation */
	char set_txctcssdefault[16]; /* for remote operation */
	/** encode now */
	char set_txctcssfreq[16]; /* encode now */
	/** decode now */
	char set_rxctcssfreq[16]; /* decode now */

	/** Pending remote-control numrxctcssfreqs setting. */
	char set_numrxctcssfreqs; /* how many */
	/** Pending remote-control numtxctcssfreqs setting. */
	char set_numtxctcssfreqs;

	/** Pending remote-control rxctcssfreqs setting. */
	char set_rxctcssfreqs[16]; /* a string */
	/** Pending remote-control txctcssfreqs setting. */
	char set_txctcssfreqs[16];

	/** pointers to strings */
	char *set_rxctcss; /* pointers to strings */
	/** Pending remote-control txctcss setting. */
	char *set_txctcss;

	/** Pending remote-control txfreq setting. */
	int set_txfreq; /* in Hz */
	/** Pending remote-control rxfreq setting. */
	int set_rxfreq;
	/** Pending remote-control txpower setting. */
	int set_txpower;

	/*      end remote operation info */

	/** Normalized hardware capture mixer setting. */
	int rxmixerset;
	/** Normalized output-A hardware mixer setting. */
	int txmixaset;
	/** Normalized output-B hardware mixer setting. */
	int txmixbset;
	/** Normalized transmit CTCSS deviation setting. */
	int txctcssadj;

	/*! \brief Settings for echoing received audio */
	int echomode;
	/** Echo-mode recording/playback enable state. */
	int echoing;
	/** Mutex protecting app_rpt-rate echo frames. */
	ast_mutex_t echolock;
	/** Queued app_rpt-rate echo recording. */
	struct qelem echoq;
	/** Maximum app_rpt-rate echo frames. */
	int echomax;

	/*! \brief Settings for HID interface */
	int hdwtype;
	/** GPIO direction-control bit mask. */
	int hid_gpio_ctl;
	/** HID report byte containing GPIO direction control. */
	int hid_gpio_ctl_loc;
	/** HID carrier-detect input mask. */
	int hid_io_cor;
	/** HID report byte containing carrier detection. */
	int hid_io_cor_loc;
	/** HID CTCSS input mask. */
	int hid_io_ctcss;
	/** HID report byte containing CTCSS detection. */
	int hid_io_ctcss_loc;
	/** HID PTT output mask. */
	int hid_io_ptt;
	/** HID report byte containing GPIO outputs. */
	int hid_gpio_loc;
	/** Cached HID output word. */
	int32_t hid_gpio_val;
	/** GPIO pins available for this interface wiring. */
	int32_t valid_gpios;
	/** Configured GPIO output values. */
	int32_t gpio_set;
	/** Last reported GPIO input word. */
	int32_t last_gpios_in;
	/** Nonzero once a GPIO input sample has been observed. */
	int had_gpios_in;
	/** Remaining pulse duration for each USB GPIO. */
	int hid_gpio_pulsetimer[GPIO_PINCOUNT];
	/** USB GPIO pins with an active timed pulse. */
	int32_t hid_gpio_pulsemask;
	/** Previously applied USB GPIO pulse mask. */
	int32_t hid_gpio_lastmask;

	/*! \brief Track parallel port values */
	int8_t last_pp_in;
	/** Nonzero once a parallel-port input sample has been observed. */
	char had_pp_in;

	/* bit fields */
	/** indicator if receive capture is enabled */
	unsigned int rxcapraw : 1; /* indicator if receive capture is enabled */
	/** indicator if transmit capture is enabled */
	unsigned int txcapraw : 1; /* indicator if transmit capture is enabled */
	/** indicator if receive capture 2 is enabled */
	unsigned int rxcap2 : 1; /* indicator if receive capture 2 is enabled */
	/** indicator if transmit capture 2 is enabled */
	unsigned int txcap2 : 1; /* indicator if transmit capture 2 is enabled */
	/** Nonzero when remote-radio control is active. */
	unsigned int remoted : 1; /* indicator if rx/tx frequency adjusted */
	/** indicator to force use of first ctcss code */
	unsigned int forcetxcode : 1; /* indicator to force use of first ctcss code */
	/** Receive signaling polarity inversion. */
	unsigned int rxpolarity : 1; /* indicator for receive polarity */
	/** Transmit signaling polarity inversion. */
	unsigned int txpolarity : 1; /* indicator for transmit polarity */
	/** Receive DCS polarity inversion. */
	unsigned int dcsrxpolarity : 1; /* indicator for dcs receive polarity */
	/** Transmit DCS polarity inversion. */
	unsigned int dcstxpolarity : 1; /* indicator for dcs transmit polarity */
	/** Receive low-speed-data polarity inversion. */
	unsigned int lsdrxpolarity : 1; /* indicator for lsd receive polarity */
	/** Transmit low-speed-data polarity inversion. */
	unsigned int lsdtxpolarity : 1; /* indicator for lsd transmit polarity */
	/** Whether this configured radio channel is enabled. */
	unsigned int radioactive : 1; /* indicator for active radio channel */
	/** Latched USB/audio device error text. */
	unsigned int device_error : 1; /* indicator set when we cannot find the USB device */
	unsigned int
		/** Pending radio assignment name. */
		newname : 1; /* indicator that we should use MIXER_PARAM_SPKR_PLAYBACK_VOL_NEW */
	/** Nonzero while a USB interface is acquired. */
	unsigned int hasusb : 1; /* indicator for has a USB device */
	/** USB assignment bookkeeping flag. */
	unsigned int usbass : 1; /* indicator for USB device assigned */
	/** Pending EEPROM operation request. */
	unsigned int wanteeprom : 1; /* indicator if we should use EEPROM */
	/** Nonzero enables Asterisk DTMF detection. */
	unsigned int usedtmf : 1; /* indicator is we should decode DTMF */
	/** Nonzero inverts the physical PTT output. */
	unsigned int invertptt : 1; /* indicator if we need to invert ptt */
	/** Receiver idle-processing reduction flag. */
	unsigned int rxcpusaver : 1; /* indicator if receive cpu save is enabled */
	/** Transmitter idle-processing reduction flag. */
	unsigned int txcpusaver : 1; /* indicator if transmit cpu save is enabled */
	/** Transmit preemphasis and final-limiter selection. */
	unsigned int txprelim : 1; /* indicator if tx pre lim is enabled */
	/** Final-limiter-only selection. */
	unsigned int txlimonly : 1; /* indicator if tx lim only is enabled */
	/** Nonzero permits carrier squelch without decoded CTCSS. */
	unsigned int rxctcssoverride : 1; /* indicator if receive ctcss override is enabled */
	unsigned int
		/** Qualified carrier indication. */
		rx_cos_active : 1; /* indicator if cos is active - active state after processing */
	/** Qualified CTCSS indication. */
	unsigned int rx_ctcss_active : 1; /* indicator if ctcss is active - active state after
					     processing */

	/* EEPROM access variables */
	/** Whether interface EEPROM tuning storage is enabled. */
	unsigned short eeprom[EEPROM_USER_LEN];
	/** EEPROM worker command/state. */
	char eepromctl;
	/** Mutex protecting EEPROM commands and tuning words. */
	ast_mutex_t eepromlock;

	/** Acquired libusb device handle. */
	struct usb_dev_handle *usb_handle;
	/** Consecutive hardware audio read errors. */
	int readerrs;
	/** DTMF tone timing state. */
	struct timeval tonetime;
	/** Nonzero while native DTMF audio should be muted. */
	int toneflag;
	/** Normalized local-repeat level from 0 through 999. */
	int duplex3;
	/** Hardware mixer or native software local-repeat assignment. */
	enum duplex3_mode duplex3mode;
	/** GPIO pin assigned to the clipping indicator. */
	int clipledgpio; /* enables ADC Clip Detect feature to output on a specified GPIO# */

	/** Low-level calibration/diagnostic setting. */
	int fever;
	/** Counter used to pace RSSI reports. */
	int count_rssi_update;

	/** Current USB GPIO input word. */
	int32_t cur_gpios;
	/** Configured per-pin USB GPIO modes. */
	char *gpios[GPIO_PINCOUNT];
	/** Configured parallel-port pin assignments. */
	char *pps[32];
	/** Whether voter signal-strength reports are sent. */
	int sendvoter;

	/** Raw receiver peak, RMS, and rail measurements. */
	struct audiostatistics rxaudiostats;
	/** Transmitter peak, RMS, and rail measurements. */
	struct audiostatistics txaudiostats;

	/** Mutex protecting USB-device operations. */
	ast_mutex_t usblock;
};

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def WARN_used_blocks
 * @brief Flag recording an output queue-depth warning.
 */
/** @def WARN_speed
 * @brief Flag recording a sample-rate mismatch warning.
 */
/** @def WARN_frag
 * @brief Flag recording an audio-fragment warning.
 */
/** @} */
