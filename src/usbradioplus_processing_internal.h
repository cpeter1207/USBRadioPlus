/** @file
 * @brief Private channel profiles, parser state, and audiohook test interfaces.
 */

#ifndef USBRADIOPLUS_PROCESSING_INTERNAL_H
#define USBRADIOPLUS_PROCESSING_INTERNAL_H

#include "asterisk.h"

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>

#include "asterisk/audiohook.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/frame.h"
#include "asterisk/module.h"

#include "txagc/avfilter_processor.h"
#include "usbradioplus_processing.h"

#define MAX_SECTION_OVERRIDES 128

#define MAX_RADIO_PROFILES 32

#define MAX_PROFILE_NAME 64

#define MAX_CONFIG_SECTION 96

/** Number of supported non-audio hardware options. */
#define USBRADIOPLUS_HARDWARE_OVERRIDE_OPTION_COUNT 38

/** One resolved non-audio option copied into a channel profile. */
struct section_override {
	/** Configuration section name. */
	char section[MAX_CONFIG_SECTION];
	/** Symbolic name used to identify this entry. */
	char name[64];
	/** Resolved textual configuration value. */
	char value[512];
};

/** Resolved hardware and processing settings for one named RadioPlus channel. */
struct txagc_profile {
	/** Nonzero enables this channel, stage, or detector. */
	int enabled;
	/** Symbolic name used to identify this entry. */
	char name[MAX_PROFILE_NAME];
	/** Associated Asterisk channel name. */
	char channel[AST_CHANNEL_NAME];
	/** Resolved CM119 gain and assignment settings. */
	struct usbradioplus_hardware_settings hardware;
	/** Bounded list of resolved non-audio configuration options. */
	struct section_override overrides[MAX_SECTION_OVERRIDES];
	/** Number of resolved entries in overrides. */
	size_t override_count;
	/** Local, link, and voice/telemetry processing chains. */
	struct txagc_chain chains[TXAGC_SOURCE_COUNT];
	/** Nonzero enables local. */
	int local_enabled;
	/** Nonzero enables link. */
	int link_enabled;
	/** Nonzero enables rnnoise. */
	int rnnoise_enabled;
	/** Settings for all optional stages and the fixed FFmpeg filtering stages. */
	struct txagc_config agc;
};

/** Validated named-channel profiles committed together under settings_lock. */
struct txagc_settings {
	/** Validated per-channel settings snapshots. */
	struct txagc_profile profiles[MAX_RADIO_PROFILES];
	/** Number of configured radio profiles. */
	size_t profile_count;
};

/** Immutable scalar measurements published by the link audiohook callback. */
struct txagc_link_statistics {
	/** Total PCM samples accepted by the callback. */
	unsigned long long input_samples;
	/** Total processed PCM samples produced by the callback. */
	unsigned long long output_samples;
	/** FFmpeg initial-latency fill reported by the graph. */
	unsigned long long startup_fill_samples;
	/** FFmpeg runtime shortfall reported by the graph. */
	unsigned long long runtime_underrun_samples;
	/** Link blocks passed through because the prepared graph could not run. */
	unsigned long long processing_errors;
	/** Current input level measurements from the callback-owned graph. */
	double input_peak_dbfs;
	/** RMS input level for the most recently processed graph block. */
	double input_rms_dbfs;
	/** Maximum input measurements since graph preparation. */
	double input_max_peak_dbfs;
	/** Largest input RMS level since graph preparation. */
	double input_max_rms_dbfs;
	/** Current output level measurements from the callback-owned graph. */
	double output_peak_dbfs;
	/** RMS output level for the most recently processed graph block. */
	double output_rms_dbfs;
	/** Maximum output measurements since graph preparation. */
	double output_max_peak_dbfs;
	/** Largest output RMS level since graph preparation. */
	double output_max_rms_dbfs;
};

/** Asterisk link audiohook and synchronous graph state owned by its datastore. */
struct txagc_hook {
	/** Asterisk hook registered on the incoming link channel. */
	struct ast_audiohook audiohook;
	/** Link graph prepared off the audio callback and atomically published. */
	struct txagc_avfilter_slot avfilter;
	/** Callback-owned mono conversion workspace. */
	double *samples;
	/** Capacity of samples in the callback conversion workspace. */
	size_t samples_capacity;
	/** Callback-owned scalar measurements copied from FFmpeg after each block. */
	struct txagc_link_statistics statistics;
	/** Double-buffered immutable diagnostics for CLI/control-plane readers. */
	struct txagc_link_statistics published_statistics[2];
	/** Index of the currently published statistics buffer. */
	atomic_uint statistics_index;
	/** Readers pinning each published statistics buffer during a copy. */
	atomic_uint statistics_readers[2];
	/** Source sample rate used when preparing the link graph. */
	unsigned int sample_rate;
	/** Link-chain admission state published with the prepared graph generation. */
	_Atomic int link_enabled;
	/** Associated Asterisk channel name. */
	char channel[AST_CHANNEL_NAME];
	/** Name of the resolved channel profile. */
	char profile[MAX_PROFILE_NAME];
};

#ifdef URP_PROCESSING_TESTING

extern struct txagc_settings settings;
extern pthread_t scan_thread;
extern int stopping;
extern int settings_parse_error;
extern _Thread_local const struct txagc_settings *staged_settings;
extern int processing_test_statistics_flip_index;
extern const char *const hardware_override_options[USBRADIOPLUS_HARDWARE_OVERRIDE_OPTION_COUNT];
extern const char *const asterisk_override_options[8];
extern const char *const duplex_override_options[3];
extern const char *const diagnostics_override_options[3];
void settings_defaults(struct txagc_settings *value);
const char *ctcss_filter_name(int mode);
int channel_is_eligible(struct ast_channel *chan, const struct txagc_profile *profile);
int validate_chain(const struct txagc_chain *value);
int validate_profile(const struct txagc_profile *value);
void read_double(struct ast_config *cfg, const char *section, const char *name, double *value);
void read_bool(struct ast_config *cfg, const char *section, const char *name, int *value);
int known_chain_option(const char *name);
int option_in_list(const char *name, const char *const *options, size_t count);
int validate_named_option(const char *category, const char *kind,
			  const struct ast_variable *variable);
int validate_option_names(struct ast_config *cfg);
const struct txagc_profile *find_profile_const(const struct txagc_settings *current,
					       const char *channel);
int valid_dcs_code(const char *text);
int valid_nonnegative_integer(const char *text, long maximum);
struct txagc_audio_snapshot;
int load_settings_candidate(struct txagc_audio_snapshot **snapshot);
int add_override(struct txagc_profile *updated, struct ast_config *cfg, const char *section,
		 const char *name);
int read_section_overrides(struct txagc_profile *updated, struct ast_config *cfg,
			   const char *asterisk_section, const char *hardware_section,
			   const char *receive_section, const char *transmit_section,
			   const char *ctcss_section, const char *dcs_section,
			   const char *duplex_section, const char *diagnostics_section);
int read_profile_overrides(struct txagc_profile *updated, struct ast_config *cfg, const char *radio,
			   const char *asterisk_section, const char *hardware_section,
			   const char *receive_section, const char *transmit_section,
			   const char *ctcss_section, const char *dcs_section,
			   const char *duplex_section, const char *diagnostics_section);
int read_assignment(struct ast_config *cfg, const char *section, const char *name, int *value,
		    int *configured);
int valid_frequency_list(const char *text);
int read_hardware(struct ast_config *cfg, const char *section,
		  struct usbradioplus_hardware_settings *hardware);
int read_stage_order(struct ast_config *cfg, const char *section, struct txagc_chain *chain);
int read_chain(struct ast_config *cfg, const char *section, struct txagc_chain *chain);
int resolve_profile_section(struct ast_config *cfg, const char *radio, const char *kind,
			    char *section, size_t section_size);
int load_settings(void);
void hook_destroy(void *data);
int txagc_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
		   struct ast_frame *frame, enum ast_audiohook_direction direction);
int attach_hook(struct ast_channel *chan, const char *profile);
int txagc_link_statistics_read(struct txagc_hook *hook, struct txagc_link_statistics *statistics);
void link_callback_copy_filter_statistics(struct txagc_link_statistics *destination,
					  const struct txagc_avfilter *source);
void link_callback_publish_statistics(struct txagc_hook *hook);
struct link_graph_transaction;
void discard_link_graph_transaction(struct link_graph_transaction *transaction);
int stage_active_link_hooks(const struct txagc_settings *candidate,
			    struct link_graph_transaction **result);
void publish_link_graph_transaction(struct link_graph_transaction *transaction);
void scan_channels(void);
void detach_all(void);
void *scanner(void *unused);
char *cli_show(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *cli_stats(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *cli_enable(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *cli_disable(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *cli_reload(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
#endif

#endif

/** @name File-local and build-time constants
 * @{ */
/** @def MAX_SECTION_OVERRIDES
 * @brief Maximum resolved non-audio options per channel.
 */
/** @def MAX_RADIO_PROFILES
 * @brief Maximum configured named radio channels.
 */
/** @def MAX_PROFILE_NAME
 * @brief Capacity of a channel/profile name in bytes.
 */
/** @def MAX_CONFIG_SECTION
 * @brief Capacity of a scoped section name in bytes.
 */
/** @} */
