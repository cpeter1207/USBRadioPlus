#ifndef USBRADIOPLUS_PROCESSING_INTERNAL_H
#define USBRADIOPLUS_PROCESSING_INTERNAL_H

#include "asterisk.h"

#include <pthread.h>
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

#define MAX_SECTION_OVERRIDES 96

struct section_override {
	char section[16];
	char name[64];
	char value[512];
};

struct txagc_settings {
	int enabled;
	char channel[AST_CHANNEL_NAME];
	struct usbradioplus_hardware_settings hardware;
	struct section_override overrides[MAX_SECTION_OVERRIDES];
	size_t override_count;
	struct txagc_chain chains[TXAGC_SOURCE_COUNT];
	int local_enabled;
	int link_enabled;
	int rnnoise_enabled;
	struct txagc_config agc;
};

#ifdef URP_PROCESSING_TESTING
struct txagc_hook {
	struct ast_audiohook audiohook;
	struct txagc_avfilter avfilter[TXAGC_SOURCE_COUNT];
	char channel[AST_CHANNEL_NAME];
};

extern struct txagc_settings settings;
extern pthread_t scan_thread;
extern int stopping;
extern int settings_parse_error;
extern const char *const hardware_override_options[68];
extern const char *const asterisk_override_options[8];
extern const char *const duplex_override_options[3];
extern const char *const diagnostics_override_options[3];
extern const char *const hardware_legacy_options[];
extern const char *const asterisk_legacy_options[];
extern const char *const duplex_legacy_options[];
extern const char *const diagnostics_legacy_options[];
void settings_defaults(struct txagc_settings *value);
const char *ctcss_filter_name(int mode);
int channel_is_eligible(struct ast_channel *chan, const struct txagc_settings *current);
int validate_chain(const struct txagc_chain *value);
int validate_settings(const struct txagc_settings *value);
void read_double(struct ast_config *cfg, const char *section, const char *name, double *value);
const char *read_option_alias(struct ast_config *cfg, const char *section, const char *name,
			      const char *legacy_name);
void read_double_alias(struct ast_config *cfg, const char *section, const char *name,
		       const char *legacy_name, double *value);
void read_bool(struct ast_config *cfg, const char *section, const char *name, int *value);
int known_chain_option(const char *name);
int option_in_list(const char *name, const char *const *options, size_t count);
int validate_option_names(struct ast_config *cfg);
int add_override(struct txagc_settings *updated, struct ast_config *cfg, const char *section,
		 const char *name);
int read_section_overrides(struct txagc_settings *updated, struct ast_config *cfg);
int read_assignment(struct ast_config *cfg, const char *name, int *value, int *configured);
int valid_frequency_list(const char *text);
int read_hardware(struct ast_config *cfg, struct usbradioplus_hardware_settings *hardware);
int read_stage_order(struct ast_config *cfg, const char *section, struct txagc_chain *chain);
int read_chain(struct ast_config *cfg, const char *section, struct txagc_chain *chain);
int load_settings(void);
void hook_destroy(void *data);
int txagc_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
		   struct ast_frame *frame, enum ast_audiohook_direction direction);
int attach_hook(struct ast_channel *chan);
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
