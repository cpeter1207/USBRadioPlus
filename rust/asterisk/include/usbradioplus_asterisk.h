/** @file usbradioplus_asterisk.h
 *  @brief Narrow versioned ABI for the USBRadioPlus Asterisk shim.
 */

#ifndef USBRADIOPLUS_ASTERISK_H
#define USBRADIOPLUS_ASTERISK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current version of every structure in this ABI. */
#define URP_AST_ABI_VERSION UINT32_C(3)
/** Operation completed successfully. */
#define URP_AST_OK 0
/** A pointer, value, string, or sample span was invalid. */
#define URP_AST_INVALID_ARGUMENT (-1)
/** A supplied ABI table has an unsupported size or version. */
#define URP_AST_INCOMPATIBLE_ABI (-2)
/** The supplied channel configuration is invalid. */
#define URP_AST_INVALID_CONFIGURATION (-3)
/** The requested configured channel does not exist. */
#define URP_AST_CHANNEL_NOT_FOUND (-4)
/** The requested configured channel is already reserved. */
#define URP_AST_CHANNEL_BUSY (-5)
/** Station preparation or hardware setup failed. */
#define URP_AST_SETUP_FAILED (-6)
/** The operation requires a running station. */
#define URP_AST_NOT_READY (-7)
/** An injected Asterisk operation failed. */
#define URP_AST_ASTERISK_FAILURE (-8)
/** A Rust panic was contained at the ABI boundary. */
#define URP_AST_INTERNAL_FAILURE (-9)

/** Fixed 8 kHz app_rpt compatibility transport. */
#define URP_AST_TRANSPORT_APP_RPT UINT32_C(1)
/** Fixed 48 kHz rpt_advanced transport. */
#define URP_AST_TRANSPORT_RPT_ADVANCED UINT32_C(2)

/** Incoming audio read from a remote Asterisk link. */
#define URP_AST_LINK_DIRECTION_READ UINT32_C(1)
/** Audio written toward a remote Asterisk link; link processing bypasses it. */
#define URP_AST_LINK_DIRECTION_WRITE UINT32_C(2)

/** Fixed Asterisk jitter-buffer implementation. */
#define URP_AST_JITTER_FIXED UINT32_C(1)
/** Adaptive Asterisk jitter-buffer implementation. */
#define URP_AST_JITTER_ADAPTIVE UINT32_C(2)

/** No DTMF event was detected. */
#define URP_AST_DTMF_NONE UINT32_C(0)
/** Start of a detected DTMF digit. */
#define URP_AST_DTMF_BEGIN UINT32_C(1)
/** End of a detected DTMF digit. */
#define URP_AST_DTMF_END UINT32_C(2)

/** Queue an Asterisk radio-key control frame. */
#define URP_AST_CONTROL_RECEIVER_KEY UINT32_C(1)
/** Queue an Asterisk radio-unkey control frame. */
#define URP_AST_CONTROL_RECEIVER_UNKEY UINT32_C(2)
/** Queue the first begin frame for a detected DTMF digit. */
#define URP_AST_CONTROL_DTMF_BEGIN UINT32_C(3)
/** Queue a DTMF end frame with its elapsed duration. */
#define URP_AST_CONTROL_DTMF_END UINT32_C(4)
/** Replace an internal pseudo-digit with an Asterisk null frame. */
#define URP_AST_CONTROL_NULL UINT32_C(5)

/** Informational log message. */
#define URP_AST_LOG_INFO UINT32_C(1)
/** Warning log message. */
#define URP_AST_LOG_WARNING UINT32_C(2)
/** Error log message. */
#define URP_AST_LOG_ERROR UINT32_C(3)

/** Read one normalized hardware mixer level. */
#define URP_AST_COMMAND_GET_MIXER UINT32_C(1)
/** Set one normalized hardware mixer level. */
#define URP_AST_COMMAND_SET_MIXER UINT32_C(2)
/** Enable or disable the calibrated transmitter test tone. */
#define URP_AST_COMMAND_SET_TEST_TONE UINT32_C(3)
/** Read the complete CM119 tuning EEPROM image. */
#define URP_AST_COMMAND_READ_EEPROM UINT32_C(4)
/** Write the complete CM119 tuning EEPROM image. */
#define URP_AST_COMMAND_WRITE_EEPROM UINT32_C(5)
/** Temporarily inhibit or restore transmit CTCSS. */
#define URP_AST_COMMAND_SET_CTCSS_INHIBIT UINT32_C(7)
/** Temporarily bypass or restore receive subaudible qualification. */
#define URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE UINT32_C(8)
/** Save the current live tuning values to CM119 EEPROM. */
#define URP_AST_COMMAND_SAVE_TUNING_EEPROM UINT32_C(9)

/** Normalized half peak-to-peak target equivalent to 2,400 signed-PCM codes. */
#define URP_AST_CTCSS_CALIBRATION_TARGET (2400.0F / 32768.0F)

/** CM119 receive mixer selector. */
#define URP_AST_MIXER_RECEIVE UINT32_C(1)
/** CM119 transmitter-A mixer selector. */
#define URP_AST_MIXER_TRANSMIT_A UINT32_C(2)
/** CM119 transmitter-B mixer selector. */
#define URP_AST_MIXER_TRANSMIT_B UINT32_C(3)

/** EEPROM checksum validation flag. */
#define URP_AST_EEPROM_CHECKSUM_VALID (UINT32_C(1) << 0)
/** EEPROM magic validation flag. */
#define URP_AST_EEPROM_MAGIC_VALID (UINT32_C(1) << 1)

/** @brief Result written by the Asterisk-owned DTMF analyzer. */
struct urp_ast_dtmf_result {
	uint32_t struct_size; /**< Caller-provided structure size in bytes. */
	uint32_t event_kind;  /**< One of the @c URP_AST_DTMF_* values. */
	uint8_t digit;	      /**< ASCII DTMF digit. */
	uint8_t reserved[7];  /**< Reserved bytes; initialize to zero. */
};

/** @brief Queue one complete signed-linear frame; the callee copies it. */
typedef int (*urp_ast_queue_voice_fn)(void *application_context, void *channel_context,
				      const int16_t *samples, uint32_t sample_count,
				      uint32_t sample_rate_hz);

/** @brief Queue one translated Asterisk control frame. */
typedef int (*urp_ast_queue_control_fn)(void *application_context, void *channel_context,
					uint32_t kind, int32_t value, uint64_t duration_ms);

/** @brief Queue one byte-counted Asterisk text frame; the callee copies it. */
typedef int (*urp_ast_queue_text_fn)(void *application_context, void *channel_context,
				     const uint8_t *text, uint32_t text_length);

/**
 * @brief Analyze one voice frame with Asterisk's DTMF detector.
 * @return Zero for no event, positive after filling result, or negative on failure.
 */
typedef int (*urp_ast_analyze_dtmf_fn)(void *application_context, void *channel_context,
				       int16_t *samples, uint32_t sample_count,
				       uint32_t sample_rate_hz, struct urp_ast_dtmf_result *result);

/** @brief Read monotonic elapsed time in milliseconds. */
typedef uint64_t (*urp_ast_monotonic_milliseconds_fn)(void *application_context);

/** @brief Emit one byte-counted message outside real-time callbacks. */
typedef void (*urp_ast_log_fn)(void *application_context, uint32_t level, const uint8_t *message,
			       uint32_t message_length);

/** @brief Asterisk-owned operations injected into the Rust adapter. */
struct urp_ast_operations {
	uint32_t struct_size;			/**< Caller-provided structure size in bytes. */
	uint32_t abi_version;			/**< @c URP_AST_ABI_VERSION. */
	void *application_context;		/**< Opaque context passed to every callback. */
	urp_ast_queue_voice_fn queue_voice;	/**< Required voice-delivery callback. */
	urp_ast_queue_control_fn queue_control; /**< Required control-delivery callback. */
	urp_ast_queue_text_fn queue_text;	/**< Required text-delivery callback. */
	urp_ast_analyze_dtmf_fn analyze_dtmf;	/**< Required DTMF-analysis callback. */
	urp_ast_monotonic_milliseconds_fn monotonic_milliseconds; /**< Required monotonic clock. */
	urp_ast_log_fn log; /**< Required non-real-time logger. */
};

/** @brief Process-lifetime descriptors for the selected product composition. */
struct urp_ast_provider_manifest {
	uint32_t struct_size;	/**< Caller-provided structure size in bytes. */
	uint32_t abi_version;	/**< @c URP_AST_ABI_VERSION. */
	const void *ffmpeg;	/**< Released FFmpeg-graph descriptor. */
	const void *rnnoise;	/**< Released RNNoise descriptor. */
	const void *ring;	/**< Released rate-adjusting-ring descriptor. */
	const void *radio;	/**< Released radio-core descriptor. */
	const void *samplerate; /**< Released sample-rate-adapter descriptor. */
	const void *audio;	/**< Released audio-adapter descriptor. */
	const void *gpio;	/**< Released GPIO-adapter descriptor. */
};

/** @brief Arguments for constructing one Rust-owned driver generation. */
struct urp_ast_driver_create_args {
	uint32_t struct_size;		 /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;		 /**< @c URP_AST_ABI_VERSION. */
	const uint8_t *config_source;	 /**< Configuration source-name bytes. */
	uint32_t config_source_length;	 /**< Configuration source-name byte count. */
	const uint8_t *config_text;	 /**< Complete configuration bytes. */
	uint32_t config_text_length;	 /**< Complete configuration byte count. */
	const uint8_t *agc_plugin_path;	 /**< Absolute AGC plug-in path bytes. */
	uint32_t agc_plugin_path_length; /**< AGC plug-in path byte count. */
	const struct urp_ast_operations *operations; /**< Process-lifetime Asterisk callbacks. */
	const struct urp_ast_provider_manifest *providers; /**< Process-lifetime provider tables. */
};

/** @brief Arguments for reserving one configured channel. */
struct urp_ast_channel_reserve_args {
	uint32_t struct_size;	      /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;	      /**< @c URP_AST_ABI_VERSION. */
	const uint8_t *channel_name;  /**< Configured channel-name bytes. */
	uint32_t channel_name_length; /**< Channel-name byte count. */
	uint32_t transport;	      /**< One of the @c URP_AST_TRANSPORT_* values. */
	void *channel_context;	      /**< Opaque context passed to channel callbacks. */
};

/** @brief Resolved Asterisk jitter-buffer settings for one channel. */
struct urp_ast_jitter_config {
	uint32_t struct_size;	      /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;	      /**< @c URP_AST_ABI_VERSION. */
	uint32_t enabled;	      /**< Nonzero when Asterisk jitter buffering is enabled. */
	uint32_t maximum_size_ms;     /**< Maximum jitter-buffer size in milliseconds. */
	uint32_t resync_threshold_ms; /**< Resynchronization threshold in milliseconds. */
	uint32_t implementation;      /**< One of the @c URP_AST_JITTER_* values. */
	uint32_t logging_enabled;     /**< Nonzero when Asterisk jitter logging is enabled. */
	uint32_t force_enabled;	      /**< Nonzero when jitter buffering is forced. */
	uint32_t target_extra_ms;     /**< Additional adaptive target delay in milliseconds. */
	uint32_t video_sync_enabled;  /**< Nonzero when video follows delayed audio. */
};

/** @brief One typed, non-real-time hardware or tuning operation. */
struct urp_ast_channel_command {
	uint32_t struct_size;	   /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;	   /**< @c URP_AST_ABI_VERSION. */
	uint32_t command;	   /**< One of the @c URP_AST_COMMAND_* values. */
	uint32_t target;	   /**< Command-specific mixer or input selector. */
	int64_t value;		   /**< Command-specific integer input or result. */
	uint32_t flags;		   /**< Command-specific bit flags. */
	uint16_t eeprom_words[64]; /**< Complete CM119 EEPROM image words. */
};

/** @brief Best-effort typed status for one active channel. */
struct urp_ast_channel_status {
	uint32_t struct_size;		     /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;		     /**< @c URP_AST_ABI_VERSION. */
	uint32_t transport;		     /**< One of the @c URP_AST_TRANSPORT_* values. */
	uint32_t running;		     /**< Nonzero while native hardware is running. */
	uint32_t echo_enabled;		     /**< Nonzero while app_rpt echo is enabled. */
	uint32_t dtmf_enabled;		     /**< Nonzero while app_rpt DTMF analysis is enabled. */
	uint64_t receive_handoff_available;  /**< Receive frames awaiting delivery. */
	uint64_t receive_handoff_discarded;  /**< Cumulative discarded receive frames. */
	uint64_t program_handoff_available;  /**< Program frames awaiting consumption. */
	uint64_t program_handoff_discarded;  /**< Cumulative discarded program frames. */
	float receive_input_peak;	     /**< Latest normalized receive-input peak. */
	float receive_input_rms;	     /**< Latest normalized receive-input RMS level. */
	float receive_output_peak;	     /**< Latest normalized receive-output peak. */
	float receive_output_rms;	     /**< Latest normalized receive-output RMS level. */
	uint64_t receive_input_rail_samples; /**< Cumulative railed receive-input samples. */
	uint64_t receive_output_rail_samples; /**< Cumulative railed receive-output samples. */
	/** Post-decoder-gain CTCSS half peak-to-peak level, normalized to full-scale PCM. */
	float receive_ctcss_decoder_peak;
	int32_t receive_rssi_peak;	       /**< Latest discriminator-noise RSSI peak. */
	uint32_t receive_rssi_updated;	       /**< Nonzero when the RSSI value was updated. */
	float transmit_program_peak;	       /**< Latest normalized transmit-program peak. */
	float transmit_program_rms;	       /**< Latest normalized transmit-program RMS level. */
	float transmit_output_peak;	       /**< Latest normalized transmit-output peak. */
	float transmit_output_rms;	       /**< Latest normalized transmit-output RMS level. */
	uint64_t input_overflow_count;	       /**< Cumulative capture overruns. */
	uint64_t output_underflow_count;       /**< Cumulative playback underruns. */
	uint64_t input_clip_sample_count;      /**< Cumulative clipped capture samples. */
	uint64_t output_clip_sample_count;     /**< Cumulative clipped playback samples. */
	uint64_t callback_last_duration_ns;    /**< Most recent callback duration in nanoseconds. */
	uint64_t callback_max_duration_ns;     /**< Maximum callback duration in nanoseconds. */
	uint64_t callback_last_start_delay_ns; /**< Most recent callback start delay. */
	uint64_t callback_max_start_delay_ns;  /**< Maximum callback start delay. */
	uint64_t callback_late_start_count;    /**< Cumulative late callback starts. */
	uint64_t last_input_xrun_monotonic_ns; /**< Monotonic time of the last input xrun. */
	uint64_t last_output_xrun_monotonic_ns; /**< Monotonic time of the last output xrun. */
	double input_latency_seconds;		/**< Reported hardware input latency. */
	double output_latency_seconds;		/**< Reported hardware output latency. */
	double native_sample_rate_hz;		/**< Fixed native stream rate in hertz. */
	uint32_t ring_occupancy_frames;		/**< Current program-ring occupancy. */
	uint32_t ring_reserve_frames;		/**< Protected program-ring reserve. */
	uint32_t ring_target_frames;		/**< Program-ring occupancy target. */
	uint32_t ring_capacity_frames;		/**< Program-ring capacity. */
	double ring_ratio;			/**< Current elastic conversion ratio. */
	uint64_t ring_underrun_samples;		/**< Cumulative missing program samples. */
	uint64_t ring_overrun_samples;		/**< Cumulative discarded program samples. */
	uint64_t ring_concealment_samples;	/**< Cumulative concealed output samples. */
	uint32_t carrier_active;		/**< Nonzero while carrier is detected. */
	uint32_t subaudible_active;	 /**< Nonzero while CTCSS or DCS qualifies receive. */
	uint32_t receiver_keyed;	 /**< Nonzero while receive audio is admitted. */
	uint32_t logical_ptt;		 /**< Nonzero while transmitter keying is requested. */
	int32_t ctcss_decode_index;	 /**< Zero-based decoded-tone index, or negative. */
	uint32_t dcs_valid;		 /**< Nonzero while the configured DCS code is valid. */
	uint32_t receive_mixer_level;	 /**< CM119 receive mixer on the 0--999 scale. */
	uint32_t transmit_a_mixer_level; /**< CM119 output-A mixer on the 0--999 scale. */
	uint32_t transmit_b_mixer_level; /**< CM119 output-B mixer on the 0--999 scale. */
};

/** @brief Lock-free processing counters for one incoming-link audiohook. */
struct urp_ast_link_observation {
	uint32_t struct_size;	   /**< Caller-provided structure size in bytes. */
	uint32_t abi_version;	   /**< @c URP_AST_ABI_VERSION. */
	uint64_t processed_blocks; /**< Blocks successfully processed in place. */
	uint64_t bypassed_blocks;  /**< Blocks bypassed for a direction/rate/size mismatch. */
	uint64_t failed_blocks;	   /**< Graph failures which retained the original PCM. */
};

/** @brief Validate providers/configuration and create one opaque driver. */
typedef int (*urp_ast_driver_create_fn)(const struct urp_ast_driver_create_args *args,
					void **output);
/** @brief Validate and stage configuration for a serialized reload transaction. */
typedef int (*urp_ast_driver_reload_fn)(void *driver, const uint8_t *source, uint32_t source_length,
					const uint8_t *text, uint32_t text_length);
/** @brief Publish or discard staged configuration after all live owners adopt it. */
typedef int (*urp_ast_driver_reload_finish_fn)(void *driver, uint32_t commit);
/** @brief Copy the configured channel name at a zero-based index. */
typedef int (*urp_ast_driver_channel_name_fn)(void *driver, uint32_t index, uint8_t *output,
					      uint32_t output_capacity, uint32_t *output_length);
/** @brief Copy the current tuning-interface channel name. */
typedef int (*urp_ast_driver_active_channel_fn)(void *driver, uint8_t *output,
						uint32_t output_capacity, uint32_t *output_length);
/** @brief Select one reserved channel as the tuning-interface target. */
typedef int (*urp_ast_driver_set_active_channel_fn)(void *driver, const uint8_t *channel_name,
						    uint32_t channel_name_length);
/** @brief Destroy a driver after all of its channels have been destroyed. */
typedef void (*urp_ast_driver_destroy_fn)(void *driver);
/** @brief Prepare one negotiated-rate incoming-link graph outside its callback. */
typedef int (*urp_ast_link_prepare_fn)(void *driver, const uint8_t *channel_name,
				       uint32_t channel_name_length, uint32_t sample_rate_hz,
				       uint32_t maximum_frame_count, void **output);
/** @brief Prepare one incoming-link graph against staged configuration. */
typedef urp_ast_link_prepare_fn urp_ast_link_prepare_reload_fn;
/** @brief Process one signed-linear link frame in place without blocking or allocation. */
typedef int (*urp_ast_link_process_fn)(void *link, uint32_t direction, uint32_t sample_rate_hz,
				       int16_t *samples, uint32_t sample_count);
/** @brief Copy one best-effort incoming-link observation. */
typedef int (*urp_ast_link_observe_fn)(void *link, struct urp_ast_link_observation *output);
/** @brief Destroy one detached and quiesced incoming-link graph. */
typedef void (*urp_ast_link_destroy_fn)(void *link);
/** @brief Reserve and prepare one configured channel. */
typedef int (*urp_ast_channel_reserve_fn)(void *driver,
					  const struct urp_ast_channel_reserve_args *args,
					  void **output);
/** @brief Start the reserved channel's hardware and audio callbacks. */
typedef int (*urp_ast_channel_start_fn)(void *channel);
/** @brief Stop the channel after its service owner has quiesced. */
typedef int (*urp_ast_channel_stop_fn)(void *channel);
/** @brief Prepare a live channel against staged configuration without replacing it. */
typedef int (*urp_ast_channel_reload_prepare_fn)(void *channel);
/** @brief Adopt a prepared channel while retaining the prior generation for rollback. */
typedef int (*urp_ast_channel_reload_activate_fn)(void *channel);
/**
 * @brief Commit detached rollback ownership or roll back an adopted generation.
 *
 * Commit is an infallible control-plane ownership release and need not use the
 * channel taskprocessor. Rollback mutates the live generation and must use it.
 */
typedef int (*urp_ast_channel_reload_finish_fn)(void *channel, uint32_t commit);
/** @brief Publish one complete Asterisk voice frame without blocking. */
typedef int (*urp_ast_channel_write_voice_fn)(void *channel, const int16_t *samples,
					      uint32_t sample_count);
/** @brief Apply one byte-counted app_rpt text-control message. */
typedef int (*urp_ast_channel_write_text_fn)(void *channel, const uint8_t *text,
					     uint32_t text_length);
/** @brief Set logical transmitter state and optional forced CTCSS. */
typedef int (*urp_ast_channel_set_transmit_fn)(void *channel, uint32_t keyed,
					       uint32_t forced_ctcss_tenths_hz);
/** @brief Enable or disable app_rpt DTMF analysis. */
typedef int (*urp_ast_channel_set_dtmf_fn)(void *channel, uint32_t enabled);
/** @brief Enable or disable app_rpt echo recording and playback. */
typedef int (*urp_ast_channel_set_echo_fn)(void *channel, uint32_t enabled);
/** @brief Return the adopted channel generation's jitter settings. */
typedef int (*urp_ast_channel_get_jitter_config_fn)(void *channel,
						    struct urp_ast_jitter_config *output);
/** @brief Execute one typed non-real-time tuning operation. */
typedef int (*urp_ast_channel_command_fn)(void *channel, struct urp_ast_channel_command *command);
/** @brief Read one best-effort active-channel status snapshot. */
typedef int (*urp_ast_channel_get_status_fn)(void *channel, struct urp_ast_channel_status *output);
/** @brief Drain bounded program, receive, control, and input handoffs. */
typedef int (*urp_ast_channel_service_fn)(void *channel);
/** @brief Destroy a stopped channel handle exactly once. */
typedef void (*urp_ast_channel_destroy_fn)(void *channel);

/** @brief Versioned Asterisk-entry adapter descriptor. */
struct urp_ast_descriptor {
	uint32_t struct_size;			/**< Descriptor size in bytes. */
	uint32_t abi_version;			/**< @c URP_AST_ABI_VERSION. */
	const char *capability_name;		/**< Stable NUL-terminated capability name. */
	urp_ast_driver_create_fn driver_create; /**< Driver constructor. */
	urp_ast_driver_reload_fn driver_reload; /**< Configuration transaction starter. */
	urp_ast_driver_reload_finish_fn driver_reload_finish; /**< Transaction publisher/aborter. */
	urp_ast_driver_channel_name_fn driver_channel_name;   /**< Channel enumerator. */
	urp_ast_driver_active_channel_fn driver_active_channel; /**< Active-channel query. */
	urp_ast_driver_set_active_channel_fn
		driver_set_active_channel;		    /**< Active-channel selector. */
	urp_ast_driver_destroy_fn driver_destroy;	    /**< Driver destructor. */
	urp_ast_link_prepare_fn link_prepare;		    /**< Incoming-link graph constructor. */
	urp_ast_link_prepare_reload_fn link_prepare_reload; /**< Staged link constructor. */
	urp_ast_link_process_fn link_process;	    /**< Incoming-link callback operation. */
	urp_ast_link_observe_fn link_observe;	    /**< Incoming-link counter reader. */
	urp_ast_link_destroy_fn link_destroy;	    /**< Incoming-link graph destructor. */
	urp_ast_channel_reserve_fn channel_reserve; /**< Channel reservation function. */
	urp_ast_channel_start_fn channel_start;	    /**< Channel start function. */
	urp_ast_channel_stop_fn channel_stop;	    /**< Channel stop function. */
	urp_ast_channel_reload_prepare_fn channel_reload_prepare;   /**< Reload preflight. */
	urp_ast_channel_reload_activate_fn channel_reload_activate; /**< Reload adoption. */
	urp_ast_channel_reload_finish_fn channel_reload_finish;	    /**< Reload commit/rollback. */
	urp_ast_channel_write_voice_fn channel_write_voice;   /**< Sole-producer voice input. */
	urp_ast_channel_write_text_fn channel_write_text;     /**< Serialized text-control input. */
	urp_ast_channel_set_transmit_fn channel_set_transmit; /**< Serialized transmit control. */
	urp_ast_channel_set_dtmf_fn channel_set_dtmf;	      /**< Serialized DTMF control. */
	urp_ast_channel_set_echo_fn channel_set_echo;	      /**< Serialized echo control. */
	urp_ast_channel_get_jitter_config_fn channel_get_jitter_config; /**< Jitter query. */
	urp_ast_channel_command_fn channel_command;	  /**< Typed tuning command dispatcher. */
	urp_ast_channel_get_status_fn channel_get_status; /**< Typed status query. */
	urp_ast_channel_service_fn channel_service;	  /**< Serialized delivery service. */
	urp_ast_channel_destroy_fn channel_destroy;	  /**< Channel destructor. */
};

/**
 * @brief Return the immutable process-lifetime adapter descriptor.
 *
 * The selected shared objects and every injected callback must remain loaded
 * until all channel and driver handles have been destroyed. Destroy every
 * channel and detached link graph returned by a driver before destroying that driver. Exactly one
 * Asterisk write callback may call channel_write_voice. One per-channel
 * taskprocessor must serialize every other channel operation except the
 * commit-only form of channel_reload_finish. A reload owner
 * must stage the driver, prepare every live channel and attached link, activate
 * every channel, publish the driver, and then commit channels and link swaps.
 * On any fallible-phase failure it must roll back all prepared channels and
 * links before discarding the driver candidate. It must complete channel_stop
 * as a barrier before channel_destroy.
 * @return Process-lifetime immutable descriptor for @c URP_AST_ABI_VERSION.
 */
const struct urp_ast_descriptor *usbradioplus_asterisk_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif
