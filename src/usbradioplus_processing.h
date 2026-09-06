/** @file
 * @brief Named-channel processing configuration, audiohooks, live controls, and meters.
 */

#ifndef USBRADIOPLUS_PROCESSING_H
#define USBRADIOPLUS_PROCESSING_H

#include <stddef.h>

#include "./txagc/agc_core.h"

struct txagc_avfilter;
/** Source-chain indices for local, link, and voice/telemetry processing. */
enum txagc_source {
	TXAGC_LOCAL /**< Native local-receiver processing source. */,
	TXAGC_LINK /**< Incoming app_rpt link processing source. */,
	TXAGC_VOICE_TELEMETRY /**< Combined transmitter voice and telemetry source. */,
	TXAGC_SOURCE_COUNT /**< Number of processing sources. */
};

/** One source chain's enable flags and shared FFmpeg processing settings. */
struct txagc_chain {
	/** Nonzero enables this channel, stage, or detector. */
	int enabled;
	/** Nonzero enables rnnoise. */
	int rnnoise_enabled;
	/** Nonzero when input gain is explicitly configured. */
	int input_gain_configured;
	/** Nonzero when the PL-filter setting is explicitly configured. */
	int ctcss_filter_configured;
	/** Nonzero when the transmitter band-pass is explicitly configured. */
	int splatter_filter_configured;
	/** Nonzero when final-limiter settings are explicitly configured. */
	int lookahead_limiter_configured;
	/** Settings for all optional stages and the fixed FFmpeg filtering stages. */
	struct txagc_config agc;
};

/** Hardware voice/CTCSS routing values shared with channel adapters. */
enum usbradioplus_hardware_assignment {
	USBRADIOPLUS_HW_OFF = 0 /**< Silence on this hardware output. */,
	USBRADIOPLUS_HW_VOICE = 1 /**< Processed voice without CTCSS. */,
	USBRADIOPLUS_HW_CTCSS = 2 /**< CTCSS without voice. */,
	USBRADIOPLUS_HW_VOICE_CTCSS = 3 /**< Processed voice mixed with CTCSS. */,
	USBRADIOPLUS_HW_AUX_VOICE = 4 /**< Auxiliary voice routing. */
};

/** Resolved CM119 gains, routing, carrier source, and CTCSS frequency maps. */
struct usbradioplus_hardware_settings {
	/** Input gain in DB. */
	double input_gain_db;
	/** Output a gain in DB. */
	double output_a_gain_db;
	/** Output b gain in DB. */
	double output_b_gain_db;
	/** Nonzero when input gain is explicitly configured. */
	int input_gain_configured;
	/** Nonzero when output a gain is explicitly configured. */
	int output_a_gain_configured;
	/** Nonzero when output b gain is explicitly configured. */
	int output_b_gain_configured;
	/** Voice/CTCSS routing for hardware output A. */
	int output_a_assignment;
	/** Voice/CTCSS routing for hardware output B. */
	int output_b_assignment;
	/** Nonzero when output a assignment is explicitly configured. */
	int output_a_assignment_configured;
	/** Nonzero when output b assignment is explicitly configured. */
	int output_b_assignment_configured;
	/** Carrier source and polarity assignment. */
	char cos_assignment[16];
	/** Comma-separated receive CTCSS frequencies in Hz. */
	char rx_ctcss_frequencies[512];
	/** Comma-separated transmit CTCSS frequencies in Hz. */
	char tx_ctcss_frequencies[512];
	/** Nonzero when cos assignment is explicitly configured. */
	int cos_assignment_configured;
	/** Nonzero when receiver ctcss frequencies is explicitly configured. */
	int rx_ctcss_frequencies_configured;
	/** Nonzero when transmitter ctcss frequencies is explicitly configured. */
	int tx_ctcss_frequencies_configured;
};

/** One section/name/value assignment to save in the unified configuration. */
struct usbradioplus_config_update {
	/** Configuration section name. */
	const char *section;
	/** Symbolic name used to identify this entry. */
	const char *name;
	/** Textual value to write to the selected configuration option. */
	const char *value;
};

/** @brief Copy a channel's local-receiver chain under the settings mutex.
 * @param channel Configured radio channel name.
 * @param chain Processing-chain settings copied or updated by this operation.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_get_local(const char *channel, struct txagc_chain *chain);
/** @brief Copy a channel's final voice/telemetry chain under the settings mutex.
 * @param channel Configured radio channel name.
 * @param chain Processing-chain settings copied or updated by this operation.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_get_composite(const char *channel, struct txagc_chain *chain);
/** @brief Copy a channel's hardware settings under the settings mutex.
 * @param channel Configured radio channel name.
 * @param hardware Receives the resolved hardware settings.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_get_hardware(const char *channel,
					 struct usbradioplus_hardware_settings *hardware);
/** @brief Copy a resolved non-audio option from a channel profile.
 * @param channel Configured radio channel name.
 * @param section Configuration section name.
 * @param name Configuration option name.
 * @param value Receives the NUL-terminated option value.
 * @param value_size Output value capacity in bytes.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_get_option(const char *channel, const char *section, const char *name,
				       char *value, size_t value_size);
/** @brief Apply a live local-receiver gain adjustment under the settings mutex.
 * @param channel Configured radio channel name.
 * @param gain_db Gain in dB.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_set_local_input_gain(const char *channel, double gain_db);
/** @brief Apply a live CM119 input-gain adjustment under the settings mutex.
 * @param channel Configured radio channel name.
 * @param gain_db Gain in dB.
 * @return Zero on success, one if the channel/option is absent, or -1 for invalid arguments.
 */
int usbradioplus_processing_set_hardware_input_gain(const char *channel, double gain_db);
/** @brief Persist the hardware and local-receiver input gains.
 * @param channel Configured radio channel name.
 * @param hardware_gain_db Hardware capture gain in dB relative to mixer midpoint.
 * @param local_gain_db Post-deemphasis local receive gain in dB.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_processing_save_input_gains(const char *channel, double hardware_gain_db,
					     double local_gain_db);
/** @brief Persist tuning values to the selected channel's configuration sections.
 * @param channel Configured radio channel name.
 * @param updates Array of configuration assignments to persist.
 * @param update_count Number of assignments in updates.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_processing_save_options(const char *channel,
					 const struct usbradioplus_config_update *updates,
					 size_t update_count);
/** @brief Load settings, register commands, and start the link-hook scanner.
 * @return Asterisk module-load success, decline, or failure status.
 */
int usbradioplus_processing_load(void);
/** @brief Parse processing settings before radio channel creation.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_processing_prime(void);
/** @brief Stop the scanner, detach audiohooks, and unregister processing commands.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_processing_unload(void);
/** @brief Validate and replace live processing settings.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_processing_reload(void);

#endif
