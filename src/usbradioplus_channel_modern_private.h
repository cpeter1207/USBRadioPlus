/** @file
 * @brief Per-channel state for the ASL radio-device API adapter.
 */

#ifndef USBRADIOPLUS_CHANNEL_MODERN_PRIVATE_H
#define USBRADIOPLUS_CHANNEL_MODERN_PRIVATE_H

#include <stdatomic.h>

#include <rate_adjusting_pcm_ring.h>

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
	/** PortAudio hardware audio thread. */
	pthread_t audiothread;
	int stophid;
	/** Stop request observed by the PortAudio audio thread. */
	volatile sig_atomic_t stopaudiothread;
	/** Nonzero while a USB interface is acquired. */
	volatile sig_atomic_t hasusb; /* HID/audio liveness; not a bit-field (cross-thread) */
	/** Nonzero after the PortAudio audio thread completes initialization. */
	char audio_thread_ready;
	/** Most recent successful PortAudio audio-thread timestamp. */
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
	/** Native hardware-clocked controller owns repeat and transmitter audio. */
	int plus_advanced;
	/** Nonzero once native graph and SRC resources can be rebuilt safely. */
	int plus_dsp_initialized;
	/** Callback-owned persistent native DSP renderer. */
	struct usbradioplus_native_renderer *plus_native_renderer;
	short plus_link_native[URP_NATIVE_SAMPLES];
	short plus_link_8k[URP_NATIVE_SAMPLES];
	/** Shared app_rpt-input program ring with native-rate clock recovery. */
	struct rpcr_ring plus_program_ring;
	/** Program-ring source occupancy target for clock recovery in samples. */
	unsigned int plus_program_target_samples;
	uint64_t plus_link_queue_underflows;
	uint64_t plus_link_queue_overflows;
	short plus_squelch_native[URP_NATIVE_SAMPLES * 2];
	short plus_rx_delay[RXSQDELAYBUFSIZE * 6];
	unsigned int plus_rx_delay_index;
	unsigned int plus_local_preemphasis_active;
	unsigned int plus_link_preemphasis_active;
	/** Complete native graph generation atomically published at setup/reload. */
	struct usbradioplus_native_graph_slot plus_native_graphs;
	/** Lock-free exclusion for rare signaling-parser reconfiguration. */
	struct usbradioplus_radio_access_slot plus_radio_access;
	/** Callback-owned PTT hold while the CM119 playback queue drains. */
	struct usbradioplus_tx_playout_hold plus_tx_playout_hold;
	/** Cached PortAudio output-latency hold in native callback blocks. */
	unsigned int plus_portaudio_playout_hold_callbacks;
	/** Last signaling-engine PTT state safe for DAC-side silence selection. */
	_Atomic int plus_radio_tx_active;
	/** Desired physical PTT state published synchronously from the signaling engine. */
	atomic_int plus_hardware_ptt_request;
	/** Physical PTT state acknowledged by the HID hardware worker. */
	atomic_int plus_hardware_ptt_applied;
	/** Nonzero while the HID worker owns a live hardware interface. */
	atomic_int plus_hardware_online;
	/** Packed HID and parallel receiver inputs published by the HID worker. */
	atomic_uint plus_hardware_inputs;
	/** Native-audio clip indication consumed by the HID worker. */
	atomic_int plus_clip_led_request;
	/** Odd while the control plane publishes a radio-programming request. */
	atomic_uint plus_radio_program_generation;
	/** Radio-programming snapshot consumed by the HID worker. */
	atomic_uint plus_radio_program_rx_frequency;
	/** Radio-programming snapshot consumed by the HID worker. */
	atomic_uint plus_radio_program_tx_frequency;
	/** Radio-programming snapshot consumed by the HID worker. */
	atomic_int plus_radio_program_high_power;
	double plus_emphasis_corner_hz;
	int plus_hardware_applied;
	int plus_applied_rxmixer, plus_applied_txmixaset, plus_applied_txmixbset;
	_Atomic int plus_applied_txmixa, plus_applied_txmixb;
	/** CTCSS output multipliers consumed by the direct native renderer. */
	_Atomic int plus_applied_tx_output_gain_a, plus_applied_tx_output_gain_b;
	/** Even when stable; brackets the lock-free hardware audio snapshot. */
	_Atomic unsigned int plus_hardware_generation;
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
	/** PTT request from app_rpt or other Asterisk control-plane users. */
	atomic_char txkeyed;
	/** PTT request from calibration controls. */
	atomic_char txtestkey;
	/** Atomically selected native calibration tone source. */
	atomic_char plus_test_tone_enabled;
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
	char dcs_receive_code[5];
	char receive_signaling_method[8];
	char transmit_signaling_method[8];
	char dcs_transmit_code[5];
	int dcs_turnoff_enabled;
	/** DCS turn-off duration in milliseconds, constrained to 150 through 200. */
	int dcs_turnoff_duration_ms;
	double dcs_level;
	/** CTCSS modulation peak in PCM codes. */
	double ctcss_level;
	double ctcss_phase_shift_degrees;
	int ctcss_tail_duration_ms;
	double ctcss_tail_frequency_hz;

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
	atomic_int echoing;
	struct urp_sample_queue echo_queue;
	short echo_samples[URP_ECHO_QUEUE_SAMPLES];
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
	unsigned int remoted : 1;	  /* indicator if rx/tx frequency adjusted */
	unsigned int forcetxcode : 1;	  /* indicator to force use of first ctcss code */
	unsigned int rxpolarity : 1;	  /* indicator for receive polarity */
	unsigned int txpolarity : 1;	  /* indicator for transmit polarity */
	unsigned int lsdrxpolarity : 1;	  /* indicator for lsd receive polarity */
	unsigned int lsdtxpolarity : 1;	  /* indicator for lsd transmit polarity */
	unsigned int radioactive : 1;	  /* indicator for active radio channel */
	unsigned int wanteeprom : 1;	  /* indicator if we should use EEPROM */
	unsigned int usedtmf : 1;	  /* indicator is we should decode DTMF */
	unsigned int invertptt : 1;	  /* indicator if we need to invert ptt */
	unsigned int rxcpusaver : 1;	  /* indicator if receive cpu save is enabled */
	unsigned int txcpusaver : 1;	  /* indicator if transmit cpu save is enabled */
	unsigned int txpreemphasis : 1;	  /* transmit pre-emphasis enabled */
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
