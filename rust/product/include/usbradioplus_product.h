/** @file
 * @brief Shared USBRadioPlus product ABI. Handles are opaque and host callbacks
 * and provider descriptors must outlive every object created from them.
 */
#ifndef USBRADIOPLUS_PRODUCT_H
#define USBRADIOPLUS_PRODUCT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct rptadv_radio_session_config;

/** Resolved native request. All byte spans and radio are borrowed during create.
 * Native creation never changes mixer settings or reads tuning EEPROM.
 * Radio generation/frame bounds/publication cadence are set by native lifecycle;
 * effective released ABI-4 radio settings are otherwise preserved. Inactive
 * signaling fields are normalized by the typed radio configuration.
 */
typedef struct UrpNativeStationConfig {
	/** Exact structure size in bytes. */
	uint32_t struct_size;
	/** Native request ABI, currently one. */
	uint32_t abi_version;
	/** Borrowed released ABI-4 radio settings template. */
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
} UrpNativeStationConfig;

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

/** Borrowed exact-frame native callbacks, independent of Asterisk frame queues. */
typedef struct UrpAstDirectCallbacks {
	/**
	 * Exact byte size of this descriptor.
	 */
	uint32_t struct_size;
	/**
	 * Exact direct-callback contract version, currently three.
	 */
	uint32_t abi_version;
	/**
	 * Borrowed receive context, valid until synchronous stream shutdown.
	 */
	void *receive_context;
	/**
	 * Process USB receive DSP output and its qualified receiver key.
	 */
	int (*receive)(void *, uint32_t, float *, uint32_t);
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

/** Versioned shared-radio product operations. */
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
	/** Start native hardware service and audio; rolls hardware back on failure. */
	int (*native_start)(void *native);
	/** Stop native audio and unkey hardware; returns provider failures. */
	int (*native_stop)(void *native);
	/** Uses the audio provider's synchronous stream-destroy contract. */
	void (*native_destroy)(void *native);
} UrpAstDescriptor;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * Return the immutable process-lifetime product descriptor (ABI 1).
 * @return Pointer to the process-lifetime descriptor.
 */
const struct UrpAstDescriptor *usbradioplus_product_descriptor_v1(void);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif /* USBRADIOPLUS_PRODUCT_H */
