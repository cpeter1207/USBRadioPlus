/** @file
 * @brief Shared USBRadioPlus product ABI. Handles are opaque and host callbacks
 * and provider descriptors must outlive every object created from them.
 */
#ifndef USBRADIOPLUS_PRODUCT_H
#define USBRADIOPLUS_PRODUCT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct rptadv_radio_session_config;

/** Current resolved native station request ABI. */
#define URP_NATIVE_CONFIG_ABI_VERSION 4U
/** Apply the configured hardware capture gain at native startup. */
#define URP_NATIVE_HARDWARE_GAIN_INPUT 1U
/** Apply the configured hardware output-A gain at native startup. */
#define URP_NATIVE_HARDWARE_GAIN_OUTPUT_A 2U
/** Apply the configured hardware output-B gain at native startup. */
#define URP_NATIVE_HARDWARE_GAIN_OUTPUT_B 4U

/** Resolved native request. All byte spans and radio are borrowed during create.
 * Native creation never changes mixer settings. It reads tuning EEPROM only
 * when shared source configuration explicitly enables it. Each stopped-to-started
 * transition applies selected gains and valid EEPROM tuning before GPIO or audio
 * starts; omitted controls and all switches are preserved.
 * Radio generation/frame bounds/publication cadence are set by native lifecycle;
 * effective released ABI-5 radio settings are otherwise preserved. Inactive
 * signaling fields are normalized by the typed radio configuration.
 */
typedef struct UrpNativeStationConfig {
	/** Exact structure size in bytes. */
	uint32_t struct_size;
	/** Native request ABI, URP_NATIVE_CONFIG_ABI_VERSION. */
	uint32_t abi_version;
	/** Borrowed released ABI-5 radio settings template. */
	const struct rptadv_radio_session_config *radio;
	/** Released audio selection policy: exact zero or automatic one. */
	uint32_t device_selection;
	/** Optional UTF-8 device identifier, not NUL terminated. */
	const uint8_t *device_identifier;
	/** Device identifier byte count. */
	uint32_t device_identifier_length;
	/** Optional UTF-8 exact USB serial, not NUL terminated. */
	const uint8_t *usb_serial;
	/** USB serial byte count. */
	uint32_t usb_serial_length;
	/** Physical capture channels, one or two. */
	uint32_t input_device_channels;
	/** Physical playback channels, one or two. */
	uint32_t output_device_channels;
	/** Additional capture buffering, zero through 500 milliseconds. */
	uint32_t input_extra_buffer_ms;
	/** Additional playback buffering, zero through 500 milliseconds. */
	uint32_t output_extra_buffer_ms;
	/** Released GPIO CM119 profile value. */
	uint32_t cm119_profile;
	/** Dedicated PTT inversion, zero or one. */
	uint32_t ptt_inverted;
	/** Ordinary GPIO output-enable bits, GPIO one in bit zero. */
	uint32_t gpio_output_enable_mask;
	/** Ordinary GPIO initial-high bits. */
	uint32_t gpio_output_initial_mask;
	/** Optional clipping-indicator GPIO bit, zero when disabled. */
	uint32_t clip_led_mask;
	/** Explicit UTF-8 mono 48 kHz receive graph, not NUL terminated. */
	const uint8_t *receive_graph;
	/** Receive graph byte count. */
	uint32_t receive_graph_length;
	/** Explicit UTF-8 mono 48 kHz transmit graph, not NUL terminated. */
	const uint8_t *transmit_graph;
	/** Transmit graph byte count. */
	uint32_t transmit_graph_length;
	/** Enable the established 300 Hz receive deemphasis, zero or one. */
	uint32_t receive_deemphasis;
	/** Receive output gain following filtering, minus 30 through 30 decibels. */
	int32_t receive_output_gain_db;
	/** Selected URP_NATIVE_HARDWARE_GAIN_* bits; zero preserves all controls. */
	uint32_t hardware_gain_mask;
	/** Capture hardware gain, finite minus 30 through 30 dB when selected. */
	double hardware_input_gain_db;
	/** Output-A hardware gain, finite minus 30 through 30 dB when selected. */
	double hardware_output_a_gain_db;
	/** Output-B hardware gain, finite minus 30 through 30 dB when selected. */
	double hardware_output_b_gain_db;
	/** UTF-8 shared processing sections, borrowed during create. */
	const uint8_t *processing_configuration;
	/** Processing configuration byte count; zero preserves raw graphs. */
	uint32_t processing_configuration_length;
	/** UTF-8 node selecting scoped or named processing profiles. */
	const uint8_t *processing_node;
	/** Processing node byte count. */
	uint32_t processing_node_length;
	/** Enable pre-emphasis for an explicit voice/telemetry profile, zero or one. */
	uint32_t transmit_preemphasis;
	/** Transmit pre-emphasis corner, greater than zero through 500 Hz. */
	double transmit_preemphasis_corner_hz;
	/** Receive de-emphasis corner, greater than zero through 500 Hz. */
	double receive_deemphasis_corner_hz;
} UrpNativeStationConfig;

/** Borrowed processing sections and node selectors, without hardware settings. */
typedef struct UrpNativeProcessingArgs {
	/** Complete structure size in bytes. */
	uint32_t struct_size;
	/** Processing request ABI, one. */
	uint32_t abi_version;
	/** UTF-8 processing configuration, not NUL terminated. */
	const uint8_t *configuration;
	/** Configuration byte count; zero selects no explicit profiles. */
	uint32_t configuration_length;
	/** UTF-8 node name, not NUL terminated. */
	const uint8_t *node;
	/** Node-name byte count. */
	uint32_t node_length;
} UrpNativeProcessingArgs;

/** Synchronous non-audio diagnostic; text is borrowed only for the call. */
typedef void (*UrpNativeProcessingDiagnostic)(void *context, const uint8_t *text, uint32_t length);

/** CTCSS decoder calibration reference in normalized F32 PCM. */
#define URP_AST_CTCSS_CALIBRATION_TARGET (2400.0f / 32768.0f)

/**
 * Successful C ABI operation.
 */
#define URP_AST_OK 0

/**
 * An asynchronous reload phase is progressing without blocking media delivery.
 */
#define URP_AST_RELOAD_PENDING 1

/**
 * A pointer, structure, enum, string, or PCM span was invalid.
 */
#define URP_AST_INVALID_ARGUMENT -1

/**
 * A required descriptor or function-table ABI was incompatible.
 */
#define URP_AST_INCOMPATIBLE_ABI -2

/**
 * The supplied USBRadioPlus configuration was invalid.
 */
#define URP_AST_INVALID_CONFIGURATION -3

/**
 * The requested configured channel was not found.
 */
#define URP_AST_CHANNEL_NOT_FOUND -4

/**
 * The requested configured channel is already reserved.
 */
#define URP_AST_CHANNEL_BUSY -5

/**
 * Station planning or provider setup failed.
 */
#define URP_AST_SETUP_FAILED -6

/**
 * The requested operation requires runtime hardware binding not yet active.
 */
#define URP_AST_NOT_READY -7

/**
 * An injected Asterisk operation failed.
 */
#define URP_AST_ASTERISK_FAILURE -8

/**
 * A Rust panic was contained at the C ABI boundary.
 */
#define URP_AST_INTERNAL_FAILURE -9

/**
 * Fixed 8 kHz interface used by `app_rpt`.
 */
#define URP_AST_TRANSPORT_APP_RPT 1

/**
 * Fixed 48 kHz interface used by `rpt_advanced`.
 */
#define URP_AST_TRANSPORT_RPT_ADVANCED 2

/**
 * Audio read from an incoming Asterisk link before controller mixing.
 */
#define URP_AST_LINK_DIRECTION_READ 1

/**
 * Audio written toward an Asterisk link; intentionally bypassed by link processing.
 */
#define URP_AST_LINK_DIRECTION_WRITE 2
/** Advance one bounded idle interval with silence and discard graph output. */
#define URP_AST_LINK_DIRECTION_SILENCE 3

/**
 * Fixed Asterisk jitter-buffer implementation.
 */
#define URP_AST_JITTER_FIXED 1

/**
 * Adaptive Asterisk jitter-buffer implementation.
 */
#define URP_AST_JITTER_ADAPTIVE 2

/**
 * No DTMF event was detected in a voice frame.
 */
#define URP_AST_DTMF_NONE 0

/**
 * Start of one detected DTMF digit.
 */
#define URP_AST_DTMF_BEGIN 1

/**
 * End of one detected DTMF digit.
 */
#define URP_AST_DTMF_END 2

/**
 * Queue a radio-key control frame.
 */
#define URP_AST_CONTROL_RECEIVER_KEY 1

/**
 * Queue a radio-unkey control frame.
 */
#define URP_AST_CONTROL_RECEIVER_UNKEY 2

/**
 * Queue a DTMF-begin frame.
 */
#define URP_AST_CONTROL_DTMF_BEGIN 3

/**
 * Queue a DTMF-end frame.
 */
#define URP_AST_CONTROL_DTMF_END 4

/**
 * Queue an Asterisk null frame in place of a muted pseudo digit.
 */
#define URP_AST_CONTROL_NULL 5

/**
 * Informational adapter log message.
 */
#define URP_AST_LOG_INFO 1

/**
 * Configuration or runtime warning.
 */
#define URP_AST_LOG_WARNING 2

/**
 * Operation failure.
 */
#define URP_AST_LOG_ERROR 3

/**
 * Read one hardware mixer level into the command value.
 */
#define URP_AST_COMMAND_GET_MIXER 1

/**
 * Set one hardware mixer level from the command value.
 */
#define URP_AST_COMMAND_SET_MIXER 2

/**
 * Enable or disable the calibrated transmitter test tone.
 */
#define URP_AST_COMMAND_SET_TEST_TONE 3

/**
 * Read the complete hardware tuning EEPROM image.
 */
#define URP_AST_COMMAND_READ_EEPROM 4

/**
 * Write the complete hardware tuning EEPROM image.
 */
#define URP_AST_COMMAND_WRITE_EEPROM 5

/**
 * Temporarily inhibit or restore transmit CTCSS generation.
 */
#define URP_AST_COMMAND_SET_CTCSS_INHIBIT 7

/**
 * Temporarily bypass or restore receive subaudible qualification.
 */
#define URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE 8

/**
 * Persist the current live mixer, CTCSS, and squelch tuning to EEPROM.
 */
#define URP_AST_COMMAND_SAVE_TUNING_EEPROM 9

/** Read resolved receive policy before hardware activation. Target must be zero.
 * Value returns VAD mode 0 Off, 1 Kerchunk, or 2 On; flags returns the minimum
 * native preroll: zero for Off, 7680 frames for either active mode. */
#define URP_AST_COMMAND_GET_RECEIVE_POLICY 10

/** Read receive policy through Asterisk with one exact UrpAstChannelCommand,
 * block=0. Only GET_RECEIVE_POLICY with target zero is accepted. Initialize
 * value=-1 and flags=UINT32_MAX and validate both outputs: an older driver may
 * report success for an unknown option without changing the payload. */
#define URP_AST_OPTION_RECEIVE_POLICY 0x52505250

/**
 * Receiver ADC mixer.
 */
#define URP_AST_MIXER_RECEIVE 1

/**
 * Transmitter DAC A mixer.
 */
#define URP_AST_MIXER_TRANSMIT_A 2

/**
 * Transmitter DAC B mixer.
 */
#define URP_AST_MIXER_TRANSMIT_B 3

/**
 * EEPROM image has a valid checksum.
 */
#define URP_AST_EEPROM_CHECKSUM_VALID (1 << 0)

/**
 * EEPROM image has the established tuning magic value.
 */
#define URP_AST_EEPROM_MAGIC_VALID (1 << 1)

/**
 * Result storage supplied to the injected DTMF analyzer.
 */
typedef struct UrpAstDtmfResult {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * One of `URP_AST_DTMF_*`.
	 */
	uint32_t event_kind;
	/**
	 * ASCII digit for a begin or end event.
	 */
	uint8_t digit;
	/**
	 * Reserved; callers initialize these bytes to zero.
	 */
	uint8_t reserved[7];
} UrpAstDtmfResult;

/**
 * Asterisk-owned operations injected into the Rust adapter.
 */
typedef struct UrpAstOperations {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * Opaque context returned to every operation.
	 */
	void *application_context;
	/**
	 * Queue one complete voice frame.
	 */
	int (*queue_voice)(void *application_context, void *channel_context, const int16_t *samples,
			   uint32_t sample_count, uint32_t sample_rate_hz);
	/**
	 * Queue one translated control frame.
	 */
	int (*queue_control)(void *application_context, void *channel_context, uint32_t kind,
			     int32_t value, uint64_t duration_ms);
	/**
	 * Queue one translated text frame.
	 */
	int (*queue_text)(void *application_context, void *channel_context, const uint8_t *text,
			  uint32_t text_length);
	/**
	 * Invoke Asterisk's DTMF analyzer.
	 */
	int (*analyze_dtmf)(void *application_context, void *channel_context, int16_t *samples,
			    uint32_t sample_count, uint32_t sample_rate_hz,
			    struct UrpAstDtmfResult *result);
	/**
	 * Read Asterisk's monotonic clock.
	 */
	uint64_t (*monotonic_milliseconds)(void *application_context);
	/**
	 * Emit a non-real-time diagnostic.
	 */
	void (*log)(void *application_context, uint32_t level, const uint8_t *message,
		    uint32_t message_length);
} UrpAstOperations;

/**
 * Process-lifetime descriptors for the selected product composition.
 */
typedef struct UrpAstProviderManifest {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * Released FFmpeg graph adapter descriptor.
	 */
	const void *ffmpeg;
	/**
	 * Released RNNoise adapter descriptor.
	 */
	const void *rnnoise;
	/**
	 * Released F32 rate-adjusting ring descriptor.
	 */
	const void *ring;
	/**
	 * Released radio-core descriptor.
	 */
	const void *radio;
	/**
	 * Versioned bounded sample-rate adapter descriptor.
	 */
	const void *samplerate;
	/**
	 * Released PortAudio/ALSA adapter descriptor.
	 */
	const void *audio;
	/**
	 * Released CM119/parallel GPIO adapter descriptor.
	 */
	const void *gpio;
} UrpAstProviderManifest;

/**
 * Arguments for constructing one Rust-owned driver generation.
 */
typedef struct UrpAstDriverCreateArgs {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * UTF-8 configuration source name; not NUL terminated.
	 */
	const uint8_t *config_source;
	/**
	 * Byte length of `config_source`.
	 */
	uint32_t config_source_length;
	/**
	 * UTF-8 configuration document; not NUL terminated.
	 */
	const uint8_t *config_text;
	/**
	 * Byte length of `config_text`.
	 */
	uint32_t config_text_length;
	/**
	 * UTF-8 path to the installed AGC LADSPA object; not NUL terminated.
	 */
	const uint8_t *agc_plugin_path;
	/**
	 * Byte length of `agc_plugin_path`.
	 */
	uint32_t agc_plugin_path_length;
	/**
	 * Required Asterisk operation table.
	 */
	const struct UrpAstOperations *operations;
	/**
	 * Complete selected provider manifest.
	 */
	const struct UrpAstProviderManifest *providers;
} UrpAstDriverCreateArgs;

/**
 * Lock-free processing counters for one attached incoming-link graph.
 */
typedef struct UrpAstLinkObservation {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * Successfully processed blocks.
	 */
	uint64_t processed_blocks;
	/**
	 * Frames bypassed because their direction, rate, or size did not match.
	 */
	uint64_t bypassed_blocks;
	/**
	 * Graph failures which retained the original PCM.
	 */
	uint64_t failed_blocks;
	/** Mandatory output-meter epoch; silence does not reset it. */
	uint64_t loudness_epoch;
	/** Observed samples in the epoch, including idle silence. */
	uint64_t loudness_sample_count;
	/** Nonzero when integrated_lufs is measured. */
	uint32_t loudness_valid;
	/** Nonzero when true_peak_dbtp is measured. */
	uint32_t true_peak_valid;
	/** Gated integrated output LUFS, meaningful only when loudness_valid is set. */
	double integrated_lufs;
	/** Maximum output dBTP, meaningful only when true_peak_valid is set. */
	double true_peak_dbtp;
} UrpAstLinkObservation;

/**
 * Arguments for reserving one configured channel.
 */
typedef struct UrpAstChannelReserveArgs {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * UTF-8 configured channel name; not NUL terminated.
	 */
	const uint8_t *channel_name;
	/**
	 * Byte length of `channel_name`.
	 */
	uint32_t channel_name_length;
	/**
	 * One of `URP_AST_TRANSPORT_*`.
	 */
	uint32_t transport;
	/**
	 * Opaque Asterisk channel context returned to queue operations.
	 */
	void *channel_context;
} UrpAstChannelReserveArgs;

/** Exact native receive identity and qualification record version. */
#define URP_NATIVE_RECEIVE_METADATA_ABI_VERSION 1U

/** Borrowed metadata for exactly the accompanying native receive PCM span. */
typedef struct UrpNativeReceiveMetadata {
	uint32_t struct_size;	     /**< Exact complete record size. */
	uint32_t abi_version;	     /**< URP_NATIVE_RECEIVE_METADATA_ABI_VERSION. */
	uint64_t generation_id;	     /**< Source radio generation. */
	uint64_t first_sample_index; /**< First source-native frame in this span. */
	uint32_t frame_count;	     /**< Exact accompanying mono sample count. */
	uint32_t raw_receiver_keyed; /**< RF and guard qualification before VAD, zero or one. */
	uint32_t receiver_keyed;     /**< Speech-admitted receive, zero or one. */
	uint32_t vad_mode;	     /**< Effective mode: off0, kerchunk1, on2. */
	uint64_t rf_burst_id; /**< Identity of the current or most recent qualified RF burst. */
	uint64_t vad_voice_sample_index; /**< Last fresh voiced decision, UINT64_MAX if absent. */
	uint32_t vad_preroll_frames;	 /**< Minimum external-ring reserve, native frames. */
	uint32_t vad_voice_active;	 /**< Latest actual neural decision, zero or one. */
	float vad_score; /**< Latest actual normalized score; priming is not speech. */
	int32_t status; /**< Zero success; negative cancels receive, with optional zero identity. */
} UrpNativeReceiveMetadata;

/** Process one exact span while this immutable metadata remains borrowed. */
typedef int (*UrpNativeReceive)(void *, const UrpNativeReceiveMetadata *, float *, uint32_t);

/** Typed programme-buffering contract, independent of exact source provenance. */
#define URP_PROGRAMME_BUFFERING_ABI_VERSION 1U
/** Actual graph accepted/delivered/FIFO counters are valid. */
#define URP_PROGRAMME_COUNTS_VALID 1U
/** Audited recipe and backend qualify the prepared conservative bound. */
#define URP_PROGRAMME_PREPARED_BOUND_VALID 2U
/** Current conservative delay bound belongs to the last completed live block. */
#define URP_PROGRAMME_CURRENT_BOUND_VALID 4U
/** Bridge padding fields belong to the last completed live block. */
#define URP_PROGRAMME_LAST_BLOCK_VALID 8U
/** No wall-clock or exact source sample is inferred. Bounds count retained/delayed
 * programme, including lookahead/conversion and bridge queues; ordinary IIR/gain
 * adaptation state is separate. Unknown raw/custom graphs remain unqualified. */
typedef struct UrpProgrammeBuffering {
	uint32_t struct_size;		    /**< Readable prefix, at least 64. */
	uint32_t abi_version;		    /**< One. */
	uint32_t flags;			    /**< Independent URP_PROGRAMME_* validity bits. */
	uint32_t reserved;		    /**< Zero. */
	uint64_t prepared_max_delay_frames; /**< Conservative maximum in graph frames. */
	uint64_t current_max_delay_frames;  /**< Conservative last-block delay. */
	uint64_t accepted_frames;	    /**< Actual accepted input, including setup warmup. */
	uint64_t delivered_frames;	    /**< Graph output; bridge padding is excluded. */
	uint64_t output_fifo_frames;	    /**< Actual current bridge FIFO occupancy. */
	uint32_t zero_prefix_frames;	    /**< Leading adapter zeros, not all graph silence. */
	uint32_t zero_suffix_frames;	    /**< Trailing adapter zeros. */
} UrpProgrammeBuffering;

/** Optional radio feedback contract, independent of the exact PCM callback ABI. */
#define URP_RADIO_FEEDBACK_ABI_VERSION 1U
/** Explicit hardware radio-duplex declaration is available. */
#define URP_RADIO_FEEDBACK_DUPLEX_VALID 1U
/** Controller/rendered PTT and request identity are available. */
#define URP_RADIO_FEEDBACK_REQUEST_VALID 2U
/** Device output service succeeded; this is not independent readback. */
#define URP_RADIO_FEEDBACK_SERVICE_VALID 4U
/** Independent PTT readback succeeded. */
#define URP_RADIO_FEEDBACK_READBACK_VALID 8U
/** Existing radio transmitter state is available; finer phase is unavailable. */
#define URP_RADIO_FEEDBACK_PHASE_VALID 16U
/** Acquisition position and age share this generation's TX sample clock. */
#define URP_RADIO_FEEDBACK_ACQUISITION_VALID 32U

/** Immutable prefix borrowed immediately before the serial controller TX call.
 * Positions refer to completed radio renders, not DAC or measured RF time.
 * GPIO completed writes remain service evidence; offline/default false never
 * becomes observed OFF. Acquisition age advances without refreshing cached facts.
 * Transmitter state is the existing 0/1/2/4/5 state after the preceding render;
 * fine STE, receiver settling and rekey completion are not measured here.
 * Consumers require matching lifecycle/request identities and their own freshness
 * policy before interpreting service or readback as current-request evidence.
 */
typedef struct UrpRadioFeedback {
	uint32_t struct_size;		 /**< Published prefix size; may grow. */
	uint32_t abi_version;		 /**< URP_RADIO_FEEDBACK_ABI_VERSION. */
	uint64_t generation_id;		 /**< Physical radio lifecycle generation. */
	uint64_t sample_index;		 /**< End of the latest successful native TX render. */
	uint32_t frame_count;		 /**< Upcoming controller callback frame count. */
	uint32_t flags;			 /**< Independent URP_RADIO_FEEDBACK_* validity bits. */
	uint64_t request_id;		 /**< Changes with controller or rendered PTT demand. */
	uint64_t service_request_id;	 /**< Request associated with service; zero if unknown. */
	uint64_t readback_request_id;	 /**< Request current at independent readback. */
	uint64_t service_generation;	 /**< Service lifecycle generation, zero if absent. */
	uint64_t readback_generation;	 /**< Readback lifecycle generation, zero if absent. */
	uint64_t acquired_sample_index;	 /**< TX position sampled before acquisition. */
	uint64_t acquisition_age_frames; /**< Native frames since that acquisition. */
	uint32_t physical_duplex;	 /**< Configured hardware duplex: half0, full1. */
	uint32_t requested_ptt;		 /**< Controller demand, before signaling tails. */
	uint32_t rendered_ptt;		 /**< Radio demand, including signaling tails. */
	uint32_t serviced_ptt;		 /**< Completed output service value when valid. */
	uint32_t observed_ptt;		 /**< Independent readback value when valid. */
	int32_t transmitter_state;	 /**< Existing radio state after prior render. */
	UrpProgrammeBuffering programme_buffering; /**< Optional TX-only conservative bound. */
	uint64_t programme_retirement_id;	   /**< Committed fresh-history request, or zero. */
} UrpRadioFeedback;

/** Complete unchanged SDK33 physical prefix; no programme tail is required. */
#define URP_RADIO_FEEDBACK_PHYSICAL_MIN_SIZE offsetof(UrpRadioFeedback, programme_buffering)
/** Complete optional programme bound and retirement identity. */
#define URP_RADIO_FEEDBACK_PROGRAMME_MIN_SIZE                                                      \
	(offsetof(UrpRadioFeedback, programme_retirement_id) +                                     \
	 sizeof(((UrpRadioFeedback *)0)->programme_retirement_id))

/** Bounded allocation-free no-unwind TX-owner callback; never retain the record. */
typedef void (*UrpRadioFeedbackPublish)(void *, const UrpRadioFeedback *);

/** Borrowed stopped-only binding. A null publish callback unbinds. */
typedef struct UrpRadioFeedbackPort {
	uint32_t struct_size; /**< Complete readable prefix; larger tails are ignored. */
	uint32_t abi_version; /**< URP_RADIO_FEEDBACK_ABI_VERSION. */
	void *context;	      /**< Borrowed serial TX context, permitted to be null. */
	UrpRadioFeedbackPublish
		publish; /**< Retained until stopped unbind/rebind or destruction. */
} UrpRadioFeedbackPort;

/** Acknowledged stopped-only feedback option on an existing Asterisk radio channel. */
#define URP_AST_OPTION_RADIO_FEEDBACK 0x52504642U
/** Exact feedback option envelope ABI; the nested binding remains sized. */
#define URP_AST_FEEDBACK_OPTION_ABI_VERSION 1U
/** Borrowed ast_channel_setoption(block=0) envelope. Require zero return and
 * accepted_abi_version=1: older Asterisk technologies can accept unknown options.
 * The taskprocessor copies the port synchronously before acknowledging retention.
 */
typedef struct UrpAstFeedbackBinding {
	uint32_t struct_size;	       /**< Exact size of this envelope. */
	uint32_t abi_version;	       /**< URP_AST_FEEDBACK_OPTION_ABI_VERSION. */
	UrpRadioFeedbackPort port;     /**< Sized prefix copied only after stopped admission. */
	uint32_t accepted_abi_version; /**< Initialize zero; set to one only on successful bind. */
	uint32_t reserved;	       /**< Must be zero. */
} UrpAstFeedbackBinding;

/** Acknowledged programme retirement through the existing channel taskprocessor. */
#define URP_AST_OPTION_PROGRAMME_RETIRE 0x52505052U
/** Sized RPPR option prefix ABI. */
#define URP_AST_PROGRAMME_RETIRE_OPTION_ABI_VERSION 1U
/** Require zero setoption return and accepted_abi_version=1, then inspect result.
 * Dispatch acceptance is distinct from retirement completion. No pointer/context
 * is retained; larger readable envelopes preserve their additional tail. */
typedef struct UrpAstProgrammeRetirement {
	uint32_t struct_size; /**< Readable prefix, at least 32 and no larger than option data. */
	uint32_t abi_version; /**< URP_AST_PROGRAMME_RETIRE_OPTION_ABI_VERSION. */
	uint64_t expected_radio_generation; /**< Nonzero physical lifecycle generation. */
	uint64_t request_id; /**< Nonzero increasing station-scoped retirement identity. */
	uint32_t accepted_abi_version; /**< Initialize zero; one only after actual SDK dispatch. */
	int32_t result; /**< SDK result: zero committed, RELOAD_PENDING, or a negative failure. */
} UrpAstProgrammeRetirement;

/** Acknowledged stopped-only post-DSP programme fence on the existing channel owner. */
#define URP_AST_OPTION_PROGRAMME_FENCE 0x52505046U
/** Sized RPPF binding prefix ABI. */
#define URP_PROGRAMME_FENCE_ABI_VERSION 1U
/** Called on the existing serial TX owner after its final programme graph.
 * Only exactly one permits the block; all other results suppress programme.
 * Generation and frames are actual radio/graph identities, never source timing.
 * Bounded, allocation-free, no-unwind; never call a control or hardware API.
 * Independent radio signaling is not gated. Inseparable voice telemetry is
 * suppressed too: the controller must schedule/retry mandatory IDs separately. */
typedef uint32_t (*UrpProgrammePermit)(void *, uint64_t generation, uint32_t frames);
/** Writable sized prefix shared by descriptor binds and ast_channel_setoption.
 * Require zero return AND accepted_abi_version=1. Bind/rebind/unbind only after
 * confirmed callback quiescence. Successful stop retains the binding for restart;
 * failed start/stop does not authorize releasing it. Context/code remain live
 * through stopped unbind/rebind or quiescent destruction/module unload.
 * A missing capability cannot qualify private programme. An absent binding
 * preserves ordinary PCM behavior; it is not an implicit private-media permit. */
typedef struct UrpProgrammeFenceBinding {
    uint32_t struct_size; /**< At least this prefix; no larger than option data. */
    uint32_t abi_version; /**< URP_PROGRAMME_FENCE_ABI_VERSION. */
    void *context; /**< Borrowed state; may be null. */
    UrpProgrammePermit permit; /**< Null explicitly unbinds while stopped. */
    uint32_t accepted_abi_version; /**< Initialize zero; one only after retention. */
    uint32_t reserved; /**< Must be zero. */
} UrpProgrammeFenceBinding;

/** Exact native PCM/metadata callback contract shared by every host adapter. */
#define URP_AST_DIRECT_CALLBACKS_ABI_VERSION 6U

/** Borrowed exact-frame native callbacks, independent of Asterisk frame queues. */
typedef struct UrpAstDirectCallbacks {
	/**
	 * Exact byte size of this descriptor.
	 */
	uint32_t struct_size;
	/**
	 * Exact direct-callback contract version, currently six.
	 */
	uint32_t abi_version;
	/**
	 * Borrowed receive context, valid until synchronous stream shutdown.
	 */
	void *receive_context;
	/**
	 * Process USB receive DSP output and its source-associated qualification.
	 */
	UrpNativeReceive receive;
	/**
	 * Borrowed transmit context, valid until synchronous stream shutdown.
	 */
	void *transmit_context;
	/**
	 * Fill PCM and write zero/one transmitter-key and CTCSS-enable results.
	 */
	int (*transmit)(void *, float *, uint32_t, uint32_t *, uint32_t *);
	/**
	 * Host writes the accepted version only after successful synchronous retention.
	 */
	uint32_t accepted_abi_version;
	/** TX-owner metadata exchange. Controls: remote enabled1, RX permitted2,
	 * TX permitted4, non-FM8, unqualified local open16. Output is zero/one linked-source
	 * activity before controller muting. Required only for a configured remote-base
	 * composition. */
	int (*metadata)(void *, uint32_t, uint32_t *);
} UrpAstDirectCallbacks;

/**
 * Resolved Asterisk jitter-buffer settings for one channel.
 */
typedef struct UrpAstJitterConfig {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * Nonzero enables Asterisk jitter buffering.
	 */
	uint32_t enabled;
	/**
	 * Maximum buffer length in milliseconds.
	 */
	uint32_t maximum_size_ms;
	/**
	 * Timestamp discontinuity which resets the buffer, in milliseconds.
	 */
	uint32_t resync_threshold_ms;
	/**
	 * One of `URP_AST_JITTER_*`.
	 */
	uint32_t implementation;
	/**
	 * Nonzero enables Asterisk jitter-buffer diagnostics.
	 */
	uint32_t logging_enabled;
	/**
	 * Nonzero forces buffering even when Asterisk considers it unnecessary.
	 */
	uint32_t force_enabled;
	/**
	 * Extra adaptive target depth in milliseconds.
	 */
	uint32_t target_extra_ms;
	/**
	 * Nonzero delays video with buffered audio.
	 */
	uint32_t video_sync_enabled;
} UrpAstJitterConfig;

/**
 * One typed, non-real-time hardware or tuning operation.
 */
typedef struct UrpAstChannelCommand {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * One of `URP_AST_COMMAND_*`.
	 */
	uint32_t command;
	/**
	 * Command-specific target such as `URP_AST_MIXER_*`.
	 */
	uint32_t target;
	/**
	 * Command input or result value.
	 */
	int64_t value;
	/**
	 * Command-specific input or result flags.
	 */
	uint32_t flags;
	/**
	 * Complete physical EEPROM image for EEPROM commands.
	 */
	uint16_t eeprom_words[64];
} UrpAstChannelCommand;

/**
 * Best-effort typed status for one active channel.
 */
typedef struct UrpAstChannelStatus {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Must equal the adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * One of `URP_AST_TRANSPORT_*`.
	 */
	uint32_t transport;
	/**
	 * Nonzero while the physical station is started.
	 */
	uint32_t running;
	/**
	 * Nonzero while legacy echo is enabled.
	 */
	uint32_t echo_enabled;
	/**
	 * Nonzero while Asterisk DTMF analysis is enabled.
	 */
	uint32_t dtmf_enabled;
	/**
	 * Pending receive-to-Asterisk handoff frames.
	 */
	uint64_t receive_handoff_available;
	/**
	 * Receive-to-Asterisk handoff frames discarded to bound latency.
	 */
	uint64_t receive_handoff_discarded;
	/**
	 * Pending Asterisk-to-program handoff frames.
	 */
	uint64_t program_handoff_available;
	/**
	 * Asterisk-to-program frames discarded to bound latency.
	 */
	uint64_t program_handoff_discarded;
	/**
	 * Latest raw receive peak.
	 */
	float receive_input_peak;
	/**
	 * Latest raw receive RMS.
	 */
	float receive_input_rms;
	/**
	 * Latest processed receive peak.
	 */
	float receive_output_peak;
	/**
	 * Latest processed receive RMS.
	 */
	float receive_output_rms;
	/**
	 * Raw receive samples at a hardware rail in the latest callback.
	 */
	uint64_t receive_input_rail_samples;
	/**
	 * Processed receive samples at a hardware rail in the latest callback.
	 */
	uint64_t receive_output_rail_samples;
	/**
	 * Post-decoder-gain CTCSS half peak-to-peak level as normalized PCM.
	 */
	float receive_ctcss_decoder_peak;
	/**
	 * Latest compatibility discriminator-noise measurement.
	 */
	int32_t receive_rssi_peak;
	/**
	 * Nonzero when the latest callback completed an RSSI integration window.
	 */
	uint32_t receive_rssi_updated;
	/**
	 * Latest post-processing transmitter-program peak.
	 */
	float transmit_program_peak;
	/**
	 * Latest post-processing transmitter-program RMS.
	 */
	float transmit_program_rms;
	/**
	 * Latest routed transmitter peak.
	 */
	float transmit_output_peak;
	/**
	 * Latest routed transmitter RMS.
	 */
	float transmit_output_rms;
	/**
	 * Cumulative PortAudio input overflows.
	 */
	uint64_t input_overflow_count;
	/**
	 * Cumulative PortAudio output underflows.
	 */
	uint64_t output_underflow_count;
	/**
	 * Cumulative raw input samples at or beyond full scale.
	 */
	uint64_t input_clip_sample_count;
	/**
	 * Cumulative output samples at or beyond full scale.
	 */
	uint64_t output_clip_sample_count;
	/**
	 * Most recent audio-callback duration in nanoseconds.
	 */
	uint64_t callback_last_duration_ns;
	/**
	 * Maximum audio-callback duration in nanoseconds.
	 */
	uint64_t callback_max_duration_ns;
	/**
	 * Most recent callback start-gap excess in nanoseconds.
	 */
	uint64_t callback_last_start_delay_ns;
	/**
	 * Maximum callback start-gap excess in nanoseconds.
	 */
	uint64_t callback_max_start_delay_ns;
	/**
	 * Cumulative late callback starts.
	 */
	uint64_t callback_late_start_count;
	/**
	 * Latest input-xrun monotonic timestamp in nanoseconds.
	 */
	uint64_t last_input_xrun_monotonic_ns;
	/**
	 * Latest output-xrun monotonic timestamp in nanoseconds.
	 */
	uint64_t last_output_xrun_monotonic_ns;
	/**
	 * PortAudio input-latency estimate in seconds.
	 */
	double input_latency_seconds;
	/**
	 * PortAudio output-latency estimate in seconds.
	 */
	double output_latency_seconds;
	/**
	 * Actual hardware-stream sample rate in hertz.
	 */
	double native_sample_rate_hz;
	/**
	 * Latest program-ring occupancy in PCM frames.
	 */
	uint32_t ring_occupancy_frames;
	/**
	 * Program-ring protected reserve in PCM frames.
	 */
	uint32_t ring_reserve_frames;
	/**
	 * Program-ring target in PCM frames.
	 */
	uint32_t ring_target_frames;
	/**
	 * Allocated program-ring capacity in PCM frames.
	 */
	uint32_t ring_capacity_frames;
	/**
	 * Current program-ring input-to-output conversion ratio.
	 */
	double ring_ratio;
	/**
	 * Program output samples absent at the ring.
	 */
	uint64_t ring_underrun_samples;
	/**
	 * Program input samples discarded by a full ring.
	 */
	uint64_t ring_overrun_samples;
	/**
	 * Program output samples supplied by concealment.
	 */
	uint64_t ring_concealment_samples;
	/**
	 * Nonzero while carrier is detected.
	 */
	uint32_t carrier_active;
	/**
	 * Nonzero while CTCSS or DCS qualification is detected.
	 */
	uint32_t subaudible_active;
	/**
	 * Nonzero while the local receiver is qualified.
	 */
	uint32_t receiver_keyed;
	/**
	 * Nonzero while logical PTT is active.
	 */
	uint32_t logical_ptt;
	/**
	 * Decoded CTCSS table index, or -1 when absent.
	 */
	int32_t ctcss_decode_index;
	/**
	 * Nonzero while the configured DCS code is valid.
	 */
	uint32_t dcs_valid;
	/**
	 * Normalized receiver hardware mixer level.
	 */
	uint32_t receive_mixer_level;
	/**
	 * Normalized transmitter-A hardware mixer level.
	 */
	uint32_t transmit_a_mixer_level;
	/**
	 * Normalized transmitter-B hardware mixer level.
	 */
	uint32_t transmit_b_mixer_level;
} UrpAstChannelStatus;

/** Native stopped station creation. All requests are copied synchronously.
 * Only ffmpeg/radio/audio/gpio provider entries are required. Callback contexts
 * and provider DSOs remain alive until native_destroy returns.
 */
typedef struct UrpNativeCreateArgs {
	/** Exact structure size in bytes. */
	uint32_t struct_size;
	/** Native request ABI, currently one. */
	uint32_t abi_version;
	/** Resolved frontend configuration. */
	const struct UrpNativeStationConfig *config;
	/** Released radio, graph, audio and GPIO descriptors; other entries unused. */
	const struct UrpAstProviderManifest *providers;
	/** Exact-frame native callback pair, retained until synchronous destroy. */
	const struct UrpAstDirectCallbacks *callbacks;
	/** Nonzero lifecycle generation. */
	uint64_t generation_id;
	/** Prepared frame bound, 1 through 4096; device callbacks stay at most 960. */
	uint32_t maximum_frames;
} UrpNativeCreateArgs;

/** Replace ownership on the existing hardware worker; acceptance only means queued. */
typedef struct UrpAstSiteBinding {
	uint32_t struct_size;	    /**< Complete structure size. */
	uint32_t abi_version;	    /**< Product record ABI, one. */
	uint64_t generation;	    /**< Expected current owner generation. */
	uint32_t output_mask;	    /**< Spare pins to claim; reserved pins are rejected. */
	uint32_t initial_high_mask; /**< Physical high levels for newly claimed pins. */
	uint32_t safe_high_mask;    /**< Physical high levels when pins are released. */
} UrpAstSiteBinding;

/** Steady output intent; pulse timing belongs to the site controller. */
typedef struct UrpAstSiteRequest {
	uint32_t struct_size; /**< Complete structure size. */
	uint32_t abi_version; /**< Product record ABI, one. */
	uint64_t generation;  /**< Acknowledged owner generation. */
	uint32_t output_mask; /**< Owned pins to change. */
	uint32_t high_mask;   /**< Physical high levels within output_mask. */
} UrpAstSiteRequest;

/** Cached physical-owner observation; querying never performs device I/O. */
typedef struct UrpAstSiteSnapshot {
	uint32_t struct_size;	      /**< Complete structure size. */
	uint32_t abi_version;	      /**< Product record ABI, one. */
	uint64_t generation;	      /**< Current owner generation; zero means retired. */
	uint32_t input_mask;	      /**< Available configured physical input pins. */
	uint32_t output_mask;	      /**< Available configured physical output pins. */
	uint32_t reserved_mask;	      /**< Pins reserved by radio signaling or existing controls. */
	uint32_t claimed_mask;	      /**< Pins owned by this site configuration. */
	uint32_t requested_high_mask; /**< Latest persistent output intent. */
	uint32_t submitted_high_mask; /**< Last output levels submitted to the provider. */
	uint64_t submitted_generation; /**< Owner generation of that submission. */
	uint64_t serviced_generation;  /**< Generation whose submission completed service. */
	uint32_t serviced_high_mask;   /**< Output levels at that successful service, not electrical
					  readback. */
	uint32_t readback_valid_mask;  /**< Pins with independent output readback; zero for these
					  providers. */
	uint32_t input_high_mask;      /**< Last successfully acquired physical input levels. */
	uint32_t input_quality;	 /**< Input acquisition quality: zero unavailable, one valid, two
				    fault. */
	uint64_t observed_at_ms; /**< Last successful acquisition on the hardware owner's monotonic
				    clock. */
	uint32_t observed_at_valid; /**< One if observed_at_ms is valid; zero otherwise. */
	uint64_t snapshot_at_ms; /**< Current time on the same clock; subtract observation time for
				    age. */
	uint64_t rejected_commands;  /**< Commands rejected by the hardware owner. */
	uint32_t last_command_error; /**< Zero none, one unavailable, two stale, three mask, four
					busy, five fault. */
} UrpAstSiteSnapshot;

/** Control-only Asterisk site I/O on the reserved radio; use block=0. */
#define URP_AST_OPTION_SITE_IO 0x52505349
/** Exact site option envelope ABI. */
#define URP_AST_SITE_OPTION_ABI_VERSION 1u
/** Replace spare-pin ownership; queued acceptance requires snapshot confirmation. */
#define URP_AST_SITE_BIND 0u
/** Publish steady output intent on the acknowledged generation. */
#define URP_AST_SITE_PUBLISH 1u
/** Read the current owner's cached observation without physical I/O. */
#define URP_AST_SITE_SNAPSHOT 2u

/**
 * @brief Synchronous taskprocessor envelope; never call from an audio callback.
 *
 * Only the selected input record is used. Snapshot storage is initialized by
 * the host. A zero result and accepted ABI confirm success; otherwise the
 * result is a product error or -1 for malformed/unsupported transport. No new
 * device is opened. A successful write means queued, not physical readback.
 */
typedef struct UrpAstSiteControl {
	uint32_t struct_size;	       /**< Exact sizeof(UrpAstSiteControl). */
	uint32_t abi_version;	       /**< URP_AST_SITE_OPTION_ABI_VERSION. */
	uint32_t operation;	       /**< URP_AST_SITE_BIND, PUBLISH, or SNAPSHOT. */
	uint32_t bank;		       /**< Zero CM119, one parallel. */
	UrpAstSiteBinding binding;     /**< Ownership request for BIND. */
	UrpAstSiteRequest request;     /**< Output request for PUBLISH. */
	UrpAstSiteSnapshot snapshot;   /**< Returned only after a successful SNAPSHOT. */
	uint32_t accepted_abi_version; /**< Initialize to zero; success writes one. */
} UrpAstSiteControl;

/** Immutable shared-product entry points. Site operations are serialized control-only. */
/** Physical-only native measurements; controller rings remain with the controller. */
typedef struct UrpNativePhysicalStatus {
	/** Size of this structure in bytes. */
	uint32_t struct_size;
	/** Must equal the adapter ABI version. */
	uint32_t abi_version;
	/** Nonzero while audio callbacks may remain active after a start attempt. */
	uint32_t running;
	/** Latest raw receive peak. */
	float receive_input_peak;
	/** Latest raw receive RMS. */
	float receive_input_rms;
	/** Latest processed receive peak. */
	float receive_output_peak;
	/** Latest processed receive RMS. */
	float receive_output_rms;
	/** Raw receive samples at a hardware rail in the latest callback. */
	uint64_t receive_input_rail_samples;
	/** Processed receive samples at a hardware rail in the latest callback. */
	uint64_t receive_output_rail_samples;
	/** Post-decoder-gain CTCSS half peak-to-peak level as normalized PCM. */
	float receive_ctcss_decoder_peak;
	/** Latest compatibility discriminator-noise measurement. */
	int32_t receive_rssi_peak;
	/** Nonzero when the latest callback completed an RSSI integration window. */
	uint32_t receive_rssi_updated;
	/** Latest post-processing transmitter-program peak. */
	float transmit_program_peak;
	/** Latest post-processing transmitter-program RMS. */
	float transmit_program_rms;
	/** Latest routed transmitter peak. */
	float transmit_output_peak;
	/** Latest routed transmitter RMS. */
	float transmit_output_rms;
	/** Cumulative PortAudio input overflows. */
	uint64_t input_overflow_count;
	/** Cumulative PortAudio output underflows. */
	uint64_t output_underflow_count;
	/** Cumulative raw input samples at or beyond full scale. */
	uint64_t input_clip_sample_count;
	/** Cumulative output samples at or beyond full scale. */
	uint64_t output_clip_sample_count;
	/** Most recent audio-callback duration in nanoseconds. */
	uint64_t callback_last_duration_ns;
	/** Maximum audio-callback duration in nanoseconds. */
	uint64_t callback_max_duration_ns;
	/** Most recent callback start-gap excess in nanoseconds. */
	uint64_t callback_last_start_delay_ns;
	/** Maximum callback start-gap excess in nanoseconds. */
	uint64_t callback_max_start_delay_ns;
	/** Cumulative late callback starts. */
	uint64_t callback_late_start_count;
	/** Latest input-xrun monotonic timestamp in nanoseconds. */
	uint64_t last_input_xrun_monotonic_ns;
	/** Latest output-xrun monotonic timestamp in nanoseconds. */
	uint64_t last_output_xrun_monotonic_ns;
	/** PortAudio input-latency estimate in seconds. */
	double input_latency_seconds;
	/** PortAudio output-latency estimate in seconds. */
	double output_latency_seconds;
	/** Actual hardware-stream sample rate in hertz. */
	double native_sample_rate_hz;
	/** Nonzero while carrier is detected. */
	uint32_t carrier_active;
	/** Nonzero while CTCSS or DCS qualification is detected. */
	uint32_t subaudible_active;
	/** Nonzero while the local receiver is qualified. */
	uint32_t receiver_keyed;
	/** Nonzero while logical PTT is active. */
	uint32_t logical_ptt;
	/** Decoded CTCSS table index, or -1 when absent. */
	int32_t ctcss_decode_index;
	/** Nonzero while the configured DCS code is valid. */
	uint32_t dcs_valid;
	/** Normalized receiver hardware mixer level. */
	uint32_t receive_mixer_level;
	/** Normalized transmitter-A hardware mixer level. */
	uint32_t transmit_a_mixer_level;
	/** Normalized transmitter-B hardware mixer level. */
	uint32_t transmit_b_mixer_level;
} UrpNativePhysicalStatus;

/** Synchronous resolved-request visitor; borrowed storage expires on return. */
typedef int (*UrpNativeConfigurationSink)(void *context,
					  const struct UrpNativeStationConfig *config);

/** Copied remote operation. Identifiers and units match the declared operating
 * catalog; no caller-owned pointer is retained. */
typedef struct UrpRemoteOperation {
	uint32_t struct_size;	 /**< Exact complete record size, 40 bytes. */
	uint32_t abi_version;	 /**< One. */
	uint32_t operation;	 /**< Shared remote operation identifier, 1 through 27. */
	uint32_t selector;	 /**< Setting, memory, scan, preset or catalog selector. */
	int64_t signed_value;	 /**< Signed step in Hz. */
	uint64_t magnitude;	 /**< Unsigned operand in the operation's declared units. */
	uint32_t decimal_places; /**< Exact input precision, zero through six. */
	uint32_t flags;		 /**< Operation-specific direction/enable/sign flags. */
} UrpRemoteOperation;

/** Owned nonblocking remote ticket; callback functions remain live through destroy. */
typedef struct UrpRemoteTicket {
	uint32_t struct_size; /**< Exact complete record size. */
	uint32_t abi_version; /**< One. */
	void *context;	      /**< Unique SDK ticket, never the channel or peer. */
	int (*source)(void *ticket, uint32_t flags); /**< Active1/valid2, terminal revocation. */
	int (*take)(void *ticket, UrpNativeProcessingDiagnostic sink,
		    void *context);    /**< Copy JSON. */
	void (*destroy)(void *ticket); /**< Cancel and release without waiting. */
} UrpRemoteTicket;

/** Serialized control-only Asterisk option, using block zero. */
#define URP_AST_OPTION_REMOTE_CONTROL 0x52505243
/** Exact remote-control envelope version. */
#define URP_AST_REMOTE_OPTION_ABI_VERSION 1u
/** Pump the existing DSP-control mailbox. */
#define URP_AST_REMOTE_SERVICE 1u
/** Poll safe retirement; one means pending and retains channel/context ownership. */
#define URP_AST_REMOTE_QUIESCE 2u
/** Submit a copied operation and return an independently owned ticket. */
#define URP_AST_REMOTE_SUBMIT 3u
/** Observe activation: zero ready, one opening, negative original cause. */
#define URP_AST_REMOTE_READY 4u
/** Synchronous taskprocessor envelope; contains no retained caller buffer.
 * accepted_abi_version acknowledges recognized dispatch even when pending/error.
 */
typedef struct UrpAstRemoteControl {
	uint32_t struct_size;	       /**< Exact complete envelope size. */
	uint32_t abi_version;	       /**< One. */
	uint32_t accepted_abi_version; /**< Zero on input, one after recognized dispatch. */
	uint32_t operation;	       /**< SERVICE, QUIESCE, SUBMIT, or READY. */
	uint32_t source_flags;	       /**< SUBMIT: local1, active2. */
	UrpRemoteOperation request;    /**< Borrowed operation copied during SUBMIT. */
	UrpRemoteTicket ticket;	       /**< Output only; success transfers unique ownership. */
} UrpAstRemoteControl;

/** Command policy from the shared source resolver, with no physical I/O. */
typedef struct UrpRemoteCommandOptions {
	uint32_t struct_size;	/**< Exact complete record size. Initialize before inspection. */
	uint32_t abi_version;	/**< One. */
	uint32_t enabled;	/**< One only for a configured remote composition. */
	uint32_t access;	/**< Zero disabled, one open, two source-specific unlock. */
	uint32_t prefix_length; /**< ASCII digit count, excluding the leading star. */
	uint8_t prefix[64];	/**< At most 63 digits; unused bytes are zero. */
} UrpRemoteCommandOptions;

/** Versioned product operations. Check struct_size before accessing an appended tail. */
typedef struct UrpAstDescriptor {
	/**
	 * Size of this structure in bytes.
	 */
	uint32_t struct_size;
	/**
	 * Adapter ABI version.
	 */
	uint32_t abi_version;
	/**
	 * NUL-terminated capability name.
	 */
	const char *capability_name;
	/**
	 * Construct a driver.
	 */
	int (*driver_create)(const struct UrpAstDriverCreateArgs *args, void **output);
	/**
	 * Stage configuration for a reload transaction.
	 */
	int (*driver_reload)(void *driver, const uint8_t *source, uint32_t source_length,
			     const uint8_t *text, uint32_t text_length);
	/**
	 * Publish or discard staged configuration.
	 */
	int (*driver_reload_finish)(void *driver, uint32_t commit);
	/**
	 * Enumerate configured channel names.
	 */
	int (*driver_channel_name)(void *driver, uint32_t index, uint8_t *output,
				   uint32_t output_capacity, uint32_t *output_length);
	/**
	 * Read the selected tuning channel.
	 */
	int (*driver_active_channel)(void *driver, uint8_t *output, uint32_t output_capacity,
				     uint32_t *output_length);
	/**
	 * Select a reserved tuning channel.
	 */
	int (*driver_set_active_channel)(void *driver, const uint8_t *channel_name,
					 uint32_t channel_name_length);
	/**
	 * Destroy a driver.
	 */
	void (*driver_destroy)(void *driver);
	/**
	 * Prepare an incoming-link graph.
	 */
	int (*link_prepare)(void *driver, const uint8_t *channel_name, uint32_t channel_name_length,
			    uint32_t sample_rate_hz, uint32_t maximum_frame_count, void **output);
	/**
	 * Prepare an incoming-link graph against staged configuration.
	 */
	int (*link_prepare_reload)(void *driver, const uint8_t *channel_name,
				   uint32_t channel_name_length, uint32_t sample_rate_hz,
				   uint32_t maximum_frame_count, void **output);
	/**
	 * Retain a live link graph when its effective processing is unchanged.
	 */
	int (*link_reload_unchanged)(void *driver, void *link, const uint8_t *name,
				     uint32_t name_length, uint32_t sample_rate_hz,
				     uint32_t maximum_frame_count, uint32_t *output);
	/**
	 * Process an incoming-link frame.
	 */
	int (*link_process)(void *link, uint32_t direction, uint32_t sample_rate_hz,
			    int16_t *samples, uint32_t sample_count);
	/**
	 * Read incoming-link counters.
	 */
	int (*link_observe)(void *link, struct UrpAstLinkObservation *output);
	/**
	 * Destroy an incoming-link graph.
	 */
	void (*link_destroy)(void *link);
	/**
	 * Reserve and prepare a channel.
	 */
	int (*channel_reserve)(void *driver, const struct UrpAstChannelReserveArgs *args,
			       void **output);
	/**
	 * Start a channel.
	 */
	int (*channel_start)(void *channel);
	/**
	 * Stop a channel.
	 */
	int (*channel_stop)(void *channel);
	/**
	 * Prepare a channel reload.
	 */
	int (*channel_reload_prepare)(void *channel);
	/**
	 * Adopt a prepared channel reload.
	 */
	int (*channel_reload_activate)(void *channel);
	/**
	 * Commit or roll back a channel reload.
	 */
	int (*channel_reload_finish)(void *channel, uint32_t commit);
	/**
	 * Submit a voice frame.
	 */
	int (*channel_write_voice)(void *channel, const int16_t *samples, uint32_t sample_count);
	/**
	 * Submit a text frame.
	 */
	int (*channel_write_text)(void *channel, const uint8_t *text, uint32_t text_length);
	/**
	 * Publish transmitter key state.
	 */
	int (*channel_set_transmit)(void *channel, uint32_t keyed, uint32_t forced_ctcss_tenths_hz);
	/**
	 * Configure DTMF detection.
	 */
	int (*channel_set_dtmf)(void *channel, uint32_t enabled);
	/**
	 * Configure retained legacy echo.
	 */
	int (*channel_set_echo)(void *channel, uint32_t enabled);
	/**
	 * Attach direct native callbacks before the first station start.
	 */
	int (*channel_set_direct_callbacks)(void *channel,
					    const struct UrpAstDirectCallbacks *callbacks);
	/**
	 * Read resolved jitter-buffer settings.
	 */
	int (*channel_get_jitter_config)(void *channel, struct UrpAstJitterConfig *output);
	/**
	 * Execute one typed tuning or hardware primitive.
	 */
	int (*channel_command)(void *channel, struct UrpAstChannelCommand *command);
	/**
	 * Read one typed status snapshot.
	 */
	int (*channel_get_status)(void *channel, struct UrpAstChannelStatus *output);
	/**
	 * Deliver pending frames.
	 */
	int (*channel_service)(void *channel);
	/**
	 * Release a channel.
	 */
	void (*channel_destroy)(void *channel);
	/** Prepare a stopped native station and clear output on failure. */
	int (*native_create)(const struct UrpNativeCreateArgs *args, void **output);
	/** Apply selected gains, then start GPIO and audio. Failure stops GPIO;
	 * successful mixer writes remain applied and the station can be retried. */
	int (*native_start)(void *native);
	/** Stop native audio and unkey hardware; returns provider failures. */
	int (*native_stop)(void *native);
	/** Uses the audio provider's synchronous stream-destroy contract. */
	void (*native_destroy)(void *native);
	/** Queue native ownership, bank 0 CM119 or 1 parallel; poll for the new generation. */
	int (*native_site_bind)(void *native, uint32_t bank,
				const struct UrpAstSiteBinding *request);
	/** Queue native output intent; success is acceptance, not electrical confirmation. */
	int (*native_site_publish)(void *native, uint32_t bank,
				   const struct UrpAstSiteRequest *request);
	/** Read native cached state; never physical device I/O. */
	int (*native_site_snapshot)(void *native, uint32_t bank, struct UrpAstSiteSnapshot *output);
	/** Queue channel ownership on its serialized control owner; banks as above. */
	int (*channel_site_bind)(void *channel, uint32_t bank,
				 const struct UrpAstSiteBinding *request);
	/** Queue channel output intent on the same owner. */
	int (*channel_site_publish)(void *channel, uint32_t bank,
				    const struct UrpAstSiteRequest *request);
	/** Read channel cached state on the same owner, never on audio callbacks. */
	int (*channel_site_snapshot)(void *channel, uint32_t bank,
				     struct UrpAstSiteSnapshot *output);
	/** Validate shared source profiles without opening devices or graphs. */
	int (*native_processing_validate)(const struct UrpNativeProcessingArgs *args,
					  UrpNativeProcessingDiagnostic diagnostic, void *context);
	/** Prepare a native 48 kHz link graph; absent profiles return a null handle.
	 * The caller retains the FFmpeg provider until link_destroy has returned.
	 */
	int (*native_link_prepare)(const struct UrpNativeProcessingArgs *args, const void *ffmpeg,
				   uint32_t maximum_frames, void **output);
	/** Process normalized-F32 PCM with the same direction and bounds as link_process.
	 * One serial owner calls this operation; it must quiesce before link_destroy.
	 */
	int (*link_process_f32)(void *link, uint32_t direction, uint32_t sample_rate_hz,
				float *samples, uint32_t sample_count);
	/** Resolve native settings without hardware access. Output is cleared on failure;
	 * success transfers an owner released only through native_configuration_destroy. */
	int (*native_configuration_resolve)(const struct UrpNativeProcessingArgs *args,
					    UrpNativeProcessingDiagnostic diagnostic,
					    void *diagnostic_context, void **output);
	/** Visit a borrowed request; retain no pointer after the callback returns. */
	int (*native_configuration_visit)(const void *configuration,
					  UrpNativeConfigurationSink sink, void *context);
	/** Release an owned resolution after its visitors return; null is harmless. */
	void (*native_configuration_destroy)(void *configuration);
	/** Read a physical snapshot on the serialized control owner. */
	int (*native_get_status)(void *native, struct UrpNativePhysicalStatus *output);
	/** Execute the shared maintenance catalog, with mutable command results. */
	int (*native_command)(void *native, struct UrpAstChannelCommand *command);
	/** Publish only manual PTT and optional forced CTCSS; neither changes controller demand. */
	int (*native_set_transmit)(void *native, uint32_t keyed, uint32_t forced_ctcss_tenths_hz);
	/** Release temporary maintenance intent; ordinary controller activity remains. */
	int (*native_clear_manual)(void *native);
	/** Prepare a same-device update using this station's retained providers/callbacks. */
	int (*native_prepare_update)(void *native, const struct UrpNativeCreateArgs *args,
				     void **output);
	/** Transfer prepared ownership into the existing callback mailbox. */
	int (*native_begin_update)(void *native, void *prepared);
	/** Return ReloadPending until both callbacks acknowledge adoption. */
	int (*native_accept_update)(void *native);
	/** Commit or roll back, retaining rollback state while callback acknowledgments are
	 * pending. */
	int (*native_finish_update)(void *native, uint32_t commit);
	/** Release an unconsumed or already transferred prepared token; null is harmless. */
	void (*native_destroy_update)(void *prepared);
	/** Emit existing physical measurement keys; borrowed text expires on callback return. */
	int (*native_status_text)(void *native, UrpNativeProcessingDiagnostic sink, void *context);
	/** Emit the existing native callback counters; borrowed text expires on callback return. */
	int (*native_processing_status_text)(void *native, UrpNativeProcessingDiagnostic sink,
					     void *context);
	/** Retain existing counters before publishing a quiescent link to PCM processing.
	 * Success transfers one independent observer lease. Output is cleared on failure.
	 * This control-plane allocation neither retains nor accesses the graph/provider.
	 */
	int (*link_observer_create)(void *link, void **output);
	/** Read counters concurrently with PCM processing, other reads or graph destruction.
	 * The observer remains valid after link_destroy; retain it until all reads return.
	 * Output must be initialized UrpAstLinkObservation ABI 1 storage.
	 */
	int (*link_observer_read)(const void *observer, struct UrpAstLinkObservation *output);
	/** Destroy one independent observer after its reads return; null is harmless. */
	void (*link_observer_destroy)(void *observer);
	/** Advance remote DSP adoption on the original serialized control owner.
	 * This never waits for CAT I/O, graph preparation or callback acknowledgments. */
	int (*native_service)(void *native);
	/** Submit without physical I/O. Source flags: local1, active2. Success
	 * transfers a ticket retained independently of the originating host object. */
	int (*native_remote_submit)(void *native, const UrpRemoteOperation *operation,
				    uint32_t source_flags, void **ticket);
	/** Same nonblocking submission on an ASL channel's taskprocessor. */
	int (*channel_remote_submit)(void *channel, const UrpRemoteOperation *operation,
				     uint32_t source_flags, void **ticket);
	/** Publish active1/valid2; revocation is terminal for this ticket. */
	int (*remote_source)(void *ticket, uint32_t flags);
	/** Copy a version1 JSON completion through the synchronous UTF-8 sink.
	 * Zero means copied; URP_AST_RELOAD_PENDING means no result. The document's
	 * kind identifies applied, error, value, status, memories, scans, scan or
	 * capabilities. It contains machine values, never rendered telemetry.
	 * Status flags distinguish requested/applied/observed presence; observed_fields
	 * alone identifies which observed state fields are physical readings. */
	int (*remote_take)(void *ticket, UrpNativeProcessingDiagnostic sink, void *context);
	/** Revoke and release after source/take calls stop; never waits for CAT. */
	void (*remote_destroy)(void *ticket);
	/** Poll safe physical retirement after stopping callbacks. Zero permits
	 * native_destroy; ReloadPending or any error retains handle and contexts. */
	int (*native_quiesce)(void *native);
	/** Same retirement on the channel's serialized taskprocessor. */
	int (*channel_quiesce)(void *channel);
	/** Inspect controller command policy on an existing native_configuration_resolve owner.
	 * Output is an initialized complete ABI-one record; no device or rig is opened. */
	int (*native_configuration_remote_options)(void *configuration,
						   UrpRemoteCommandOptions *output);
	/** Copy activation readiness without I/O: zero after initial RF/DSP admission,
	 * one while opening, or the original negative failure. Pump native_service
	 * separately; thread creation alone is not readiness. */
	int (*native_activation_status)(void *native);
	/** Same readiness observation on the existing channel control owner. */
	int (*channel_activation_status)(void *channel);
	/** Synchronous UTF-8 processing diagnostics on the serialized channel owner.
	 * Includes final transmit programme integrated LUFS and maximum dBTP since
	 * the reported graph meter epoch; unavailable values are explicit text. */
	int (*channel_processing_status_text)(void *channel, UrpNativeProcessingDiagnostic sink,
					      void *context);
	/** Optional stopped-only binding; copies the readable port prefix. Does no I/O.
	 * Null/short/version-mismatched records fail without changing the prior binding.
	 * Any start attempt fences rebinding until successful synchronous stop.
	 * The callback runs on the existing serial TX owner before controller rendering.
	 * Callback/context remain live until stopped unbind/rebind or quiescent destruction;
	 * successful stop retains the binding for a subsequent restart.
	 */
	int (*native_feedback_bind)(void *native, const struct UrpRadioFeedbackPort *port);
	/** Same capability on an existing serialized Asterisk station; survives reload. */
	int (*channel_feedback_bind)(void *channel, const struct UrpRadioFeedbackPort *port);
	/** Optional no-allocation query on the same serial owner as link processing.
	 * Output is an initialized ABI-one record with reserved zero. Invalid requests
	 * change neither output nor link state; larger output tails remain untouched.
	 * Missing capability/unknown bounds never prevent ordinary graph processing. */
	int (*link_programme_buffering)(void *link, struct UrpProgrammeBuffering *output);
	/** Explicit final-TX programme retirement on the active serialized native owner.
	 * expected_radio_generation must match the immutable physical session, including
	 * on repeated polls; replaced/unknown generations fail before preparation.
	 * Nonzero IDs increase monotonically within a physical station lifetime. Repeat
	 * the same ID to poll one transaction: RELOAD_PENDING retains all ownership;
	 * zero acknowledges a committed fresh TX graph with no predecessor crossfade
	 * or rollback to revoked programme. Prior/superseded IDs fail. Ordinary updates
	 * and remote DSP work serialize with this request. RX/RNNoise, signaling,
	 * hardware gains and ordinary unkey behavior remain unchanged. The caller gates
	 * programme until matching completed-render feedback; no new callback is bound.
	 */
	int (*native_programme_retire)(void *native, uint64_t expected_radio_generation,
				       uint64_t request_id);
	/** Same operation on the existing channel taskprocessor owner. */
	int (*channel_programme_retire)(void *channel, uint64_t expected_radio_generation,
					uint64_t request_id);
    /** Retain a final-programme fence on the quiescent native owner; require ACK. */
    int (*native_programme_fence_bind)(void *native, UrpProgrammeFenceBinding *binding);
    /** Same stopped-only operation on the existing channel taskprocessor owner. */
    int (*channel_programme_fence_bind)(void *channel, UrpProgrammeFenceBinding *binding);
} UrpAstDescriptor;

/** Complete optional feedback capability, preserving every preceding descriptor field. */
#define URP_AST_DESCRIPTOR_FEEDBACK_MIN_SIZE                                                       \
	(offsetof(UrpAstDescriptor, channel_feedback_bind) +                                       \
	 sizeof(((UrpAstDescriptor *)0)->channel_feedback_bind))

/** Complete optional owner-serialized link buffering query. */
#define URP_AST_DESCRIPTOR_PROGRAMME_BUFFERING_MIN_SIZE                                            \
	(offsetof(UrpAstDescriptor, link_programme_buffering) +                                    \
	 sizeof(((UrpAstDescriptor *)0)->link_programme_buffering))

/** Complete optional native/channel TX programme retirement capability. */
#define URP_AST_DESCRIPTOR_PROGRAMME_RETIRE_MIN_SIZE                                               \
	(offsetof(UrpAstDescriptor, channel_programme_retire) +                                    \
	 sizeof(((UrpAstDescriptor *)0)->channel_programme_retire))

/** Complete optional native/channel post-DSP programme fence capability. */
#define URP_AST_DESCRIPTOR_PROGRAMME_FENCE_MIN_SIZE \
    (offsetof(UrpAstDescriptor, channel_programme_fence_bind) + \
     sizeof(((UrpAstDescriptor *)0)->channel_programme_fence_bind))

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Return the immutable process-lifetime product descriptor (ABI 1).
 * @return Pointer to the process-lifetime descriptor.
 */
const struct UrpAstDescriptor *usbradioplus_product_descriptor_v1(void);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif /* USBRADIOPLUS_PRODUCT_H */
