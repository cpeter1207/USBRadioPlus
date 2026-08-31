#include "asterisk.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "asterisk/audiohook.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/datastore.h"
#include "asterisk/format.h"
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/utils.h"

#include "./txagc/agc_core.h"
#include "./txagc/avfilter_processor.h"
#include "usbradioplus_processing.h"

#define CONFIG_FILE "usbradioplus-processing.conf"
#define SCAN_INTERVAL_US 250000

static const char *source_names[TXAGC_SOURCE_COUNT] = {
	"local", "link", "voice_telemetry"
};

struct txagc_settings {
	int enabled;
	char channel[AST_CHANNEL_NAME];
	struct txagc_chain chains[TXAGC_SOURCE_COUNT];
	/* Compatibility view used by the detailed CLI display. */
	int local_enabled;
	int link_enabled;
	int rnnoise_enabled;
	struct txagc_config agc;
};

struct txagc_hook {
	struct ast_audiohook audiohook;
	struct txagc_avfilter avfilter[TXAGC_SOURCE_COUNT];
	char channel[AST_CHANNEL_NAME];
};

AST_MUTEX_DEFINE_STATIC(settings_lock);
static struct txagc_settings settings;
static pthread_t scan_thread = AST_PTHREADT_NULL;
static int stopping;
static int settings_parse_error;

static int channel_is_eligible(struct ast_channel *chan,
	const struct txagc_settings *current)
{
	const char *name = ast_channel_name(chan);
	const char *application = ast_channel_appl(chan);
	const char *data = ast_channel_data(chan);

	/* The RadioPlus channel is processed natively: local receive at 48 kHz
	 * before mixing, and the complete transmitter mix in the final composite
	 * graph.  Audiohooks remain only for incoming link source processing. */
	return current->chains[TXAGC_LINK].enabled && !strncmp(name, "IAX2/", 5)
		&& application && !strcmp(application, "Rpt")
		&& data && !strcmp(data, "Remote Rx");
}

static void settings_defaults(struct txagc_settings *value)
{
	struct txagc_chain *base;

	memset(value, 0, sizeof(*value));
	/* A missing processing file must preserve chan_usbradio behaviour.  All
	 * RadioPlus-only processing is opt-in; there is deliberately no node-
	 * specific default here. */
	value->enabled = 0;
	ast_copy_string(value->channel, "RadioPlus/", sizeof(value->channel));
	base = &value->chains[TXAGC_LOCAL];
	base->enabled = 1;
	base->rnnoise_enabled = 0;
	base->agc.stage_count = 4;
	base->agc.stage_order[0] = TXAGC_STAGE_EXPANDER;
	base->agc.stage_order[1] = TXAGC_STAGE_AGC;
	base->agc.stage_order[2] = TXAGC_STAGE_COMPRESSOR;
	base->agc.stage_order[3] = TXAGC_STAGE_LIMITER;
	base->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	base->agc.ctcss_notch_width_hz = 2.0;
	base->agc.ctcss_highpass_hz = 300.0;
	base->agc.agc_enabled = 0;
	base->agc.input_gain_db = 0.0;
	base->agc.target_dbfs = -10.0;
	base->agc.max_gain_db = 12.0;
	base->agc.max_attenuation_db = 6.0;
	base->agc.agc_floor_dbfs = -60.0;
	base->agc.attack_ms = 750.0;
	base->agc.release_ms = 1500.0;
	base->agc.reset_after_ms = 3000.0;
	base->agc.sidechain_highpass_hz = 800.0;
	base->agc.sidechain_lowpass_hz = 1500.0;
	base->agc.expander_enabled = 0;
	base->agc.expander_threshold_dbfs = -55.0;
	base->agc.expander_ratio = 1.5;
	base->agc.expander_max_attenuation_db = 9.0;
	base->agc.expander_attack_ms = 10.0;
	base->agc.expander_release_ms = 250.0;
	base->agc.expander_sidechain_highpass_hz = 800.0;
	base->agc.expander_sidechain_lowpass_hz = 1500.0;
	base->agc.compressor_enabled = 0;
	base->agc.compressor_threshold_dbfs = -6.0;
	base->agc.compressor_ratio = 2.0;
	base->agc.compressor_makeup_gain_db = 0.0;
	base->agc.compressor_attack_ms = 75.0;
	base->agc.compressor_release_ms = 300.0;
	base->agc.compressor_sidechain_highpass_hz = 800.0;
	base->agc.compressor_sidechain_lowpass_hz = 1500.0;
	base->agc.limiter_enabled = 0;
	base->agc.splatter_filter_enabled = 0;
	base->agc.limiter_crossover_hz = 1000.0;
	base->agc.low_limiter_threshold_dbfs = -1.5;
	base->agc.low_limiter_ratio = 10.0;
	base->agc.low_limiter_knee_db = 6.0;
	base->agc.low_limiter_attack_ms = 50.0;
	base->agc.low_limiter_release_ms = 250.0;
	base->agc.high_clip_dbfs = -1.5;
	base->agc.high_limiter_ratio = 20.0;
	base->agc.high_limiter_knee_db = 6.0;
	base->agc.high_limiter_attack_ms = 0.5;
	base->agc.high_limiter_release_ms = 25.0;
	base->agc.final_clipper_enabled = 0;
	base->agc.final_clip_dbfs = -3.0;
	base->agc.lookahead_limiter_enabled = 0;
	base->agc.lookahead_limit_dbfs = -3.0;
	base->agc.lookahead_ms = 5.0;
	base->agc.lookahead_attack_ms = 1.0;
	base->agc.lookahead_release_ms = 100.0;
	base->agc.post_limiter_lowpass_enabled = 0;
	base->agc.post_limiter_lowpass_hz = 8000.0;
	base->agc.output_highpass_hz = 300.0;
	base->agc.output_lowpass_hz = 3000.0;
	base->agc.output_gain_db = -6.2;
	value->chains[TXAGC_LINK] = *base;
	value->chains[TXAGC_VOICE_TELEMETRY] = *base;
	value->chains[TXAGC_LINK].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	value->chains[TXAGC_VOICE_TELEMETRY].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	base = &value->chains[TXAGC_VOICE_TELEMETRY];
	base->rnnoise_enabled = 0;
	base->agc.agc_enabled = 0;
	base->agc.expander_enabled = 0;
	base->agc.compressor_enabled = 0;
	base->agc.limiter_enabled = 0;
	base->agc.input_gain_db = 6.0;
	base->agc.splatter_filter_enabled = 1;
	base->agc.final_clipper_enabled = 0;
	base->agc.lookahead_limiter_enabled = 0;
	base->agc.post_limiter_lowpass_enabled = 0;
	base->agc.output_gain_db = 0.0;
}

static int validate_chain(const struct txagc_chain *value)
{
	unsigned int index;
	unsigned int seen = 0;
	if (value->agc.stage_count > TXAGC_MAX_DYNAMICS_STAGES) return -1;
	for (index = 0; index < value->agc.stage_count; ++index) {
		if (value->agc.stage_order[index] < TXAGC_STAGE_EXPANDER
			|| value->agc.stage_order[index] > TXAGC_STAGE_LIMITER
			|| (seen & (1U << value->agc.stage_order[index]))) return -1;
		seen |= 1U << value->agc.stage_order[index];
	}
	if (value->agc.ctcss_filter_mode < TXAGC_CTCSS_FILTER_DISABLED
		|| value->agc.ctcss_filter_mode > TXAGC_CTCSS_FILTER_HIGHPASS
		|| value->agc.ctcss_notch_width_hz < 0.2
		|| value->agc.ctcss_notch_width_hz > 10.0
		|| value->agc.ctcss_highpass_hz < 50.0
		|| value->agc.ctcss_highpass_hz > 500.0
		|| value->agc.input_gain_db < -30.0 || value->agc.input_gain_db > 30.0
		|| value->agc.target_dbfs > -3.0 || value->agc.target_dbfs < -40.0
		|| value->agc.max_gain_db < 0.0 || value->agc.max_gain_db > 30.0
		|| value->agc.max_attenuation_db < 0.0 || value->agc.max_attenuation_db > 60.0
		|| value->agc.agc_floor_dbfs >= value->agc.target_dbfs || value->agc.agc_floor_dbfs < -100.0
		|| value->agc.attack_ms < 1.0 || value->agc.attack_ms > 10000.0
		|| value->agc.release_ms < 1.0 || value->agc.release_ms > 30000.0
		|| value->agc.reset_after_ms < 100.0 || value->agc.reset_after_ms > 60000.0
		|| value->agc.sidechain_highpass_hz < 50.0 || value->agc.sidechain_highpass_hz > 2000.0
		|| value->agc.sidechain_lowpass_hz <= value->agc.sidechain_highpass_hz || value->agc.sidechain_lowpass_hz > 3500.0
		|| value->agc.expander_threshold_dbfs < -100.0 || value->agc.expander_threshold_dbfs > -10.0
		|| value->agc.expander_ratio < 1.0 || value->agc.expander_ratio > 10.0
		|| value->agc.expander_max_attenuation_db < 0.0 || value->agc.expander_max_attenuation_db > 40.0
		|| value->agc.expander_attack_ms < 1.0 || value->agc.expander_attack_ms > 1000.0
		|| value->agc.expander_release_ms < 1.0 || value->agc.expander_release_ms > 10000.0
		|| value->agc.expander_sidechain_highpass_hz < 50.0 || value->agc.expander_sidechain_highpass_hz > 2000.0
		|| value->agc.expander_sidechain_lowpass_hz <= value->agc.expander_sidechain_highpass_hz || value->agc.expander_sidechain_lowpass_hz > 3500.0
		|| value->agc.compressor_threshold_dbfs < -60.0 || value->agc.compressor_threshold_dbfs > 0.0
		|| value->agc.compressor_ratio < 1.0 || value->agc.compressor_ratio > 20.0
		|| value->agc.compressor_makeup_gain_db < -30.0 || value->agc.compressor_makeup_gain_db > 30.0
		|| value->agc.compressor_attack_ms < 1.0 || value->agc.compressor_attack_ms > 1000.0
		|| value->agc.compressor_release_ms < 1.0 || value->agc.compressor_release_ms > 10000.0
		|| value->agc.compressor_sidechain_highpass_hz < 50.0 || value->agc.compressor_sidechain_highpass_hz > 2000.0
		|| value->agc.compressor_sidechain_lowpass_hz <= value->agc.compressor_sidechain_highpass_hz || value->agc.compressor_sidechain_lowpass_hz > 3500.0
		|| value->agc.limiter_crossover_hz < 300.0 || value->agc.limiter_crossover_hz > 2000.0
		|| value->agc.low_limiter_threshold_dbfs < -40.0
		|| value->agc.low_limiter_threshold_dbfs > -1.0
		|| value->agc.low_limiter_ratio < 1.0 || value->agc.low_limiter_ratio > 20.0
		|| value->agc.low_limiter_knee_db < 0.0 || value->agc.low_limiter_knee_db > 24.0
		|| value->agc.low_limiter_attack_ms < 0.1 || value->agc.low_limiter_attack_ms > 1000.0
		|| value->agc.low_limiter_release_ms < 1.0 || value->agc.low_limiter_release_ms > 10000.0
		|| value->agc.high_clip_dbfs < -30.0 || value->agc.high_clip_dbfs > -1.0
		|| value->agc.high_limiter_ratio < 1.0 || value->agc.high_limiter_ratio > 20.0
		|| value->agc.high_limiter_knee_db < 0.0 || value->agc.high_limiter_knee_db > 24.0
		|| value->agc.high_limiter_attack_ms < 0.1 || value->agc.high_limiter_attack_ms > 100.0
		|| value->agc.high_limiter_release_ms < 1.0 || value->agc.high_limiter_release_ms > 1000.0
		|| value->agc.final_clip_dbfs < -30.0 || value->agc.final_clip_dbfs > -0.5
		|| value->agc.lookahead_limit_dbfs < -30.0 || value->agc.lookahead_limit_dbfs > -0.1
		|| value->agc.lookahead_ms < 0.1 || value->agc.lookahead_ms > 20.0
		|| value->agc.lookahead_attack_ms < 0.1 || value->agc.lookahead_attack_ms > 20.0
		|| value->agc.lookahead_release_ms < 1.0 || value->agc.lookahead_release_ms > 5000.0
		|| value->agc.post_limiter_lowpass_hz < 5000.0
		|| value->agc.post_limiter_lowpass_hz > 20000.0
		|| value->agc.output_highpass_hz < 20.0 || value->agc.output_highpass_hz > 2000.0
		|| value->agc.output_lowpass_hz <= value->agc.output_highpass_hz
		|| value->agc.output_lowpass_hz > 5000.0
		|| value->agc.output_gain_db < -30.0 || value->agc.output_gain_db > 30.0) {
		return -1;
	}
	return 0;
}

static int validate_settings(const struct txagc_settings *value)
{
	int source;
	if (ast_strlen_zero(value->channel)) {
		return -1;
	}
	for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		if (validate_chain(&value->chains[source])) {
			ast_log(LOG_ERROR, "RadioPlus [%s]: one or more values are out of range\n",
				source_names[source]);
			return -1;
		}
		if (source != TXAGC_LOCAL && value->chains[source].rnnoise_enabled) {
			ast_log(LOG_ERROR, "RadioPlus [%s]: RNNoise is local-receiver-only\n",
				source_names[source]);
			return -1;
		}
		if (source != TXAGC_LOCAL
			&& value->chains[source].agc.ctcss_filter_mode
				!= TXAGC_CTCSS_FILTER_DISABLED) {
			ast_log(LOG_ERROR, "RadioPlus [%s]: CTCSS receive filtering is local-receiver-only\n",
				source_names[source]);
			return -1;
		}
		if (source != TXAGC_VOICE_TELEMETRY
			&& (value->chains[source].agc.splatter_filter_enabled
				|| value->chains[source].agc.lookahead_limiter_enabled
				|| value->chains[source].agc.post_limiter_lowpass_enabled
				|| value->chains[source].agc.final_clipper_enabled)) {
			ast_log(LOG_ERROR, "RadioPlus [%s]: transmitter-tail stages are valid only in [voice_telemetry]\n",
				source_names[source]);
			return -1;
		}
	}
	return 0;
}

static void read_double(struct ast_config *cfg, const char *section,
	const char *name, double *value)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	char *end = NULL;
	double parsed;

	if (!text) {
		return;
	}
	parsed = strtod(text, &end);
	if (end && *end == '\0' && isfinite(parsed)) {
		*value = parsed;
	} else {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s requires a finite number, got '%s'\n",
			section, name, text);
		settings_parse_error = 1;
	}
}

static void read_bool(struct ast_config *cfg, const char *section,
	const char *name, int *value)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	if (!text) return;
	if (ast_true(text)) *value = 1;
	else if (ast_false(text)) *value = 0;
	else {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s requires yes or no, got '%s'\n",
			section, name, text);
		settings_parse_error = 1;
	}
}

static int known_chain_option(const char *name)
{
	static const char *const names[] = {
		"enabled", "stage_order", "rnnoise_enabled", "ctcss_filter_mode",
		"ctcss_notch_width_hz", "ctcss_highpass_hz", "agc_enabled",
		"input_gain_db", "target_dbfs", "max_gain_db", "max_attenuation_db",
		"agc_floor_dbfs", "attack_ms", "release_ms", "reset_after_ms",
		"sidechain_highpass_hz", "sidechain_lowpass_hz", "expander_enabled",
		"expander_threshold_dbfs", "expander_ratio", "expander_max_attenuation_db",
		"expander_attack_ms", "expander_release_ms",
		"expander_sidechain_highpass_hz", "expander_sidechain_lowpass_hz",
		"compressor_enabled", "compressor_threshold_dbfs", "compressor_ratio",
		"compressor_makeup_gain_db", "compressor_attack_ms", "compressor_release_ms",
		"compressor_sidechain_highpass_hz", "compressor_sidechain_lowpass_hz",
		"limiter_enabled", "splatter_filter_enabled", "limiter_crossover_hz",
		"low_limiter_threshold_dbfs", "low_limiter_ratio", "low_limiter_knee_db",
		"low_limiter_attack_ms", "low_limiter_release_ms", "high_clip_dbfs",
		"high_limiter_ratio", "high_limiter_knee_db", "high_limiter_attack_ms",
		"high_limiter_release_ms", "final_clipper_enabled", "final_clip_dbfs",
		"lookahead_limiter_enabled", "lookahead_limit_dbfs", "lookahead_ms",
		"lookahead_attack_ms", "lookahead_release_ms",
		"post_limiter_lowpass_enabled", "post_limiter_lowpass_hz",
		"output_highpass_hz", "output_lowpass_hz", "output_gain_db",
	};
	size_t index;
	for (index = 0; index < ARRAY_LEN(names); ++index)
		if (!strcasecmp(name, names[index])) return 1;
	return 0;
}

static int validate_option_names(struct ast_config *cfg)
{
	static const char *const sections[] = { "general", "local", "link", "voice_telemetry" };
	size_t section;
	const char *category = NULL;
	const struct ast_variable *variable;
	while ((category = ast_category_browse(cfg, category))) {
		int known = 0;
		for (section = 0; section < ARRAY_LEN(sections); ++section)
			if (!strcasecmp(category, sections[section])) known = 1;
		if (!known) {
			ast_log(LOG_ERROR, "RadioPlus: unknown processing section [%s]\n", category);
			return -1;
		}
	}
	for (section = 0; section < ARRAY_LEN(sections); ++section) {
		for (variable = ast_variable_browse(cfg, sections[section]); variable;
			variable = variable->next) {
			if (known_chain_option(variable->name)) continue;
			if (!strcmp(sections[section], "general")
				&& (!strcasecmp(variable->name, "channel")
					|| !strcasecmp(variable->name, "local_enabled")
					|| !strcasecmp(variable->name, "link_enabled"))) continue;
			ast_log(LOG_ERROR, "RadioPlus [%s]: unknown option '%s'\n",
				sections[section], variable->name);
			return -1;
		}
	}
	return 0;
}

static int read_stage_order(struct ast_config *cfg, const char *section,
	struct txagc_chain *chain)
{
	const char *configured = ast_variable_retrieve(cfg, section, "stage_order");
	char error[128];

	if (!configured) return 0;
	if (txagc_parse_stage_order(configured, &chain->agc, error, sizeof(error))) {
		ast_log(LOG_ERROR, "RadioPlus [%s]: invalid stage_order: %s\n",
			section, error);
		return -1;
	}
	return 0;
}

static int read_chain(struct ast_config *cfg, const char *section,
	struct txagc_chain *chain)
{
#define READ_BOOL(name, field) read_bool(cfg, section, name, &(field))
	READ_BOOL("enabled", chain->enabled);
	READ_BOOL("rnnoise_enabled", chain->rnnoise_enabled);
	{
		const char *mode = ast_variable_retrieve(cfg, section, "ctcss_filter_mode");
		if (mode) {
			if (!strcasecmp(mode, "notch") || !strcasecmp(mode, "comb"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
			else if (!strcasecmp(mode, "highpass"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
			else if (!strcasecmp(mode, "disabled") || !strcasecmp(mode, "off"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
			else {
				ast_log(LOG_ERROR, "RadioPlus [%s]: invalid ctcss_filter_mode '%s'\n",
					section, mode);
				settings_parse_error = 1;
			}
		}
	}
	read_double(cfg, section, "ctcss_notch_width_hz", &chain->agc.ctcss_notch_width_hz);
	read_double(cfg, section, "ctcss_highpass_hz", &chain->agc.ctcss_highpass_hz);
	READ_BOOL("agc_enabled", chain->agc.agc_enabled);
	read_double(cfg, section, "input_gain_db", &chain->agc.input_gain_db);
	read_double(cfg, section, "target_dbfs", &chain->agc.target_dbfs);
	read_double(cfg, section, "max_gain_db", &chain->agc.max_gain_db);
	read_double(cfg, section, "max_attenuation_db", &chain->agc.max_attenuation_db);
	read_double(cfg, section, "agc_floor_dbfs", &chain->agc.agc_floor_dbfs);
	read_double(cfg, section, "attack_ms", &chain->agc.attack_ms);
	read_double(cfg, section, "release_ms", &chain->agc.release_ms);
	read_double(cfg, section, "reset_after_ms", &chain->agc.reset_after_ms);
	read_double(cfg, section, "sidechain_highpass_hz", &chain->agc.sidechain_highpass_hz);
	read_double(cfg, section, "sidechain_lowpass_hz", &chain->agc.sidechain_lowpass_hz);
	READ_BOOL("expander_enabled", chain->agc.expander_enabled);
	read_double(cfg, section, "expander_threshold_dbfs", &chain->agc.expander_threshold_dbfs);
	read_double(cfg, section, "expander_ratio", &chain->agc.expander_ratio);
	read_double(cfg, section, "expander_max_attenuation_db", &chain->agc.expander_max_attenuation_db);
	read_double(cfg, section, "expander_attack_ms", &chain->agc.expander_attack_ms);
	read_double(cfg, section, "expander_release_ms", &chain->agc.expander_release_ms);
	read_double(cfg, section, "expander_sidechain_highpass_hz", &chain->agc.expander_sidechain_highpass_hz);
	read_double(cfg, section, "expander_sidechain_lowpass_hz", &chain->agc.expander_sidechain_lowpass_hz);
	READ_BOOL("compressor_enabled", chain->agc.compressor_enabled);
	read_double(cfg, section, "compressor_threshold_dbfs", &chain->agc.compressor_threshold_dbfs);
	read_double(cfg, section, "compressor_ratio", &chain->agc.compressor_ratio);
	read_double(cfg, section, "compressor_makeup_gain_db", &chain->agc.compressor_makeup_gain_db);
	read_double(cfg, section, "compressor_attack_ms", &chain->agc.compressor_attack_ms);
	read_double(cfg, section, "compressor_release_ms", &chain->agc.compressor_release_ms);
	read_double(cfg, section, "compressor_sidechain_highpass_hz", &chain->agc.compressor_sidechain_highpass_hz);
	read_double(cfg, section, "compressor_sidechain_lowpass_hz", &chain->agc.compressor_sidechain_lowpass_hz);
	READ_BOOL("limiter_enabled", chain->agc.limiter_enabled);
	READ_BOOL("splatter_filter_enabled", chain->agc.splatter_filter_enabled);
	read_double(cfg, section, "limiter_crossover_hz", &chain->agc.limiter_crossover_hz);
	read_double(cfg, section, "low_limiter_threshold_dbfs", &chain->agc.low_limiter_threshold_dbfs);
	read_double(cfg, section, "low_limiter_ratio", &chain->agc.low_limiter_ratio);
	read_double(cfg, section, "low_limiter_knee_db", &chain->agc.low_limiter_knee_db);
	read_double(cfg, section, "low_limiter_attack_ms", &chain->agc.low_limiter_attack_ms);
	read_double(cfg, section, "low_limiter_release_ms", &chain->agc.low_limiter_release_ms);
	read_double(cfg, section, "high_clip_dbfs", &chain->agc.high_clip_dbfs);
	read_double(cfg, section, "high_limiter_ratio", &chain->agc.high_limiter_ratio);
	read_double(cfg, section, "high_limiter_knee_db", &chain->agc.high_limiter_knee_db);
	read_double(cfg, section, "high_limiter_attack_ms", &chain->agc.high_limiter_attack_ms);
	read_double(cfg, section, "high_limiter_release_ms", &chain->agc.high_limiter_release_ms);
	READ_BOOL("final_clipper_enabled", chain->agc.final_clipper_enabled);
	read_double(cfg, section, "final_clip_dbfs", &chain->agc.final_clip_dbfs);
	chain->lookahead_limiter_configured =
		ast_variable_retrieve(cfg, section, "lookahead_limiter_enabled") != NULL;
	READ_BOOL("lookahead_limiter_enabled", chain->agc.lookahead_limiter_enabled);
	read_double(cfg, section, "lookahead_limit_dbfs", &chain->agc.lookahead_limit_dbfs);
	read_double(cfg, section, "lookahead_ms", &chain->agc.lookahead_ms);
	read_double(cfg, section, "lookahead_attack_ms", &chain->agc.lookahead_attack_ms);
	read_double(cfg, section, "lookahead_release_ms", &chain->agc.lookahead_release_ms);
	READ_BOOL("post_limiter_lowpass_enabled", chain->agc.post_limiter_lowpass_enabled);
	read_double(cfg, section, "post_limiter_lowpass_hz", &chain->agc.post_limiter_lowpass_hz);
	read_double(cfg, section, "output_highpass_hz", &chain->agc.output_highpass_hz);
	read_double(cfg, section, "output_lowpass_hz", &chain->agc.output_lowpass_hz);
	read_double(cfg, section, "output_gain_db", &chain->agc.output_gain_db);
#undef READ_BOOL
	return read_stage_order(cfg, section, chain);
}

static int load_settings(void)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg;
	struct txagc_settings updated;
	const char *text;

	settings_defaults(&updated);
	settings_parse_error = 0;
	cfg = ast_config_load2(CONFIG_FILE, "chan_usbradioplus", flags);
	if (cfg == CONFIG_STATUS_FILEMISSING) {
		ast_mutex_lock(&settings_lock);
		settings = updated;
		ast_mutex_unlock(&settings_lock);
		ast_log(LOG_NOTICE, "%s not present; optional RadioPlus processing is disabled\n",
			CONFIG_FILE);
		return 0;
	}
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Unable to load valid %s\n", CONFIG_FILE);
		return -1;
	}
	if (validate_option_names(cfg)) {
		ast_config_destroy(cfg);
		return -1;
	}

	read_bool(cfg, "general", "enabled", &updated.enabled);
	text = ast_variable_retrieve(cfg, "general", "channel");
	if (text) {
		ast_copy_string(updated.channel, text, sizeof(updated.channel));
	}
	/* Legacy flat settings remain accepted and seed local/link chains. */
	if (read_chain(cfg, "general", &updated.chains[TXAGC_LOCAL])) goto invalid;
	read_bool(cfg, "general", "local_enabled", &updated.chains[TXAGC_LOCAL].enabled);
	updated.chains[TXAGC_LINK] = updated.chains[TXAGC_LOCAL];
	updated.chains[TXAGC_LINK].rnnoise_enabled = 0;
	updated.chains[TXAGC_LINK].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	read_bool(cfg, "general", "link_enabled", &updated.chains[TXAGC_LINK].enabled);
	if (read_chain(cfg, "local", &updated.chains[TXAGC_LOCAL])
		|| read_chain(cfg, "link", &updated.chains[TXAGC_LINK])
		|| read_chain(cfg, "voice_telemetry", &updated.chains[TXAGC_VOICE_TELEMETRY])) goto invalid;
	if (settings_parse_error) goto invalid;
	updated.local_enabled = updated.chains[TXAGC_LOCAL].enabled;
	updated.link_enabled = updated.chains[TXAGC_LINK].enabled;
	updated.rnnoise_enabled = updated.chains[TXAGC_LOCAL].rnnoise_enabled;
	updated.agc = updated.chains[TXAGC_LOCAL].agc;
	ast_config_destroy(cfg);

	if (validate_settings(&updated)) {
		ast_log(LOG_ERROR, "Invalid setting in %s; keeping existing configuration\n", CONFIG_FILE);
		return -1;
	}

	ast_mutex_lock(&settings_lock);
	settings = updated;
	ast_mutex_unlock(&settings_lock);
	ast_log(LOG_NOTICE, "RadioPlus processing %s (local %s, link %s, voice+telemetry %s for %s)\n",
		updated.enabled ? "enabled" : "disabled",
		updated.chains[TXAGC_LOCAL].enabled ? "enabled" : "disabled",
		updated.chains[TXAGC_LINK].enabled ? "enabled" : "disabled",
		updated.chains[TXAGC_VOICE_TELEMETRY].enabled ? "enabled" : "disabled",
		updated.channel);
	return 0;

invalid:
	ast_config_destroy(cfg);
	ast_log(LOG_ERROR, "Invalid graph definition in %s; keeping existing configuration\n", CONFIG_FILE);
	return -1;
}

static void hook_destroy(void *data)
{
	struct txagc_hook *hook = data;
	int source;

	if (!hook) {
		return;
	}
	ast_audiohook_detach(&hook->audiohook);
	ast_audiohook_destroy(&hook->audiohook);
	for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		txagc_avfilter_destroy(&hook->avfilter[source]);
	}
	ast_free(hook);
}

static const struct ast_datastore_info txagc_datastore = {
	.type = "txagc",
	.destroy = hook_destroy,
};

static int txagc_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
	struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	struct ast_datastore *datastore;
	struct txagc_hook *hook;
	struct txagc_settings current;
	struct txagc_chain *chain;
	enum txagc_source source;
	unsigned int sample_rate;
	double *samples;
	int16_t *pcm;
	int i;

	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE
		|| frame->frametype != AST_FRAME_VOICE
		|| !frame->data.ptr || frame->samples <= 0) {
		return 0;
	}

	datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
	if (!datastore || !(hook = datastore->data)) {
		return 0;
	}
	ast_mutex_lock(&settings_lock);
	current = settings;
	ast_mutex_unlock(&settings_lock);
	if (!strcmp(ast_channel_name(chan), current.channel)) {
		/* Native RadioPlus processing owns both directions.  This also makes
		 * stale hooks harmless across an in-place configuration reload. */
		return 0;
	} else if (direction == AST_AUDIOHOOK_DIRECTION_READ) {
		source = TXAGC_LINK;
	} else {
		return 0;
	}
	chain = &current.chains[source];
	/* RadioPlus processes local receiver audio internally at 48 kHz after
	 * de-emphasis. Processing its 8 kHz Asterisk copy here would both waste
	 * bandwidth and apply the chain twice. */
	if (source == TXAGC_LOCAL) {
		return 0;
	}
	if (!current.enabled || !chain->enabled) {
		return 0;
	}

	sample_rate = ast_format_get_sample_rate(frame->subclass.format);
	if (!sample_rate) {
		sample_rate = 8000;
	}
	samples = ast_alloca(frame->samples * sizeof(*samples));
	pcm = frame->data.ptr;
	for (i = 0; i < frame->samples; ++i) samples[i] = pcm[i];
	if (txagc_avfilter_process(&hook->avfilter[source], &chain->agc,
			samples, frame->samples, sample_rate) < 0) {
		ast_log(LOG_WARNING, "RadioPlus processing failed on %s; leaving frame unchanged\n",
			hook->channel);
		return 0;
	}
	for (i = 0; i < frame->samples; ++i) {
		double value = samples[i];
		if (value > 32767.0) value = 32767.0;
		else if (value < -32768.0) value = -32768.0;
		pcm[i] = (int16_t) lrint(value);
	}
	return 0;
}

static int attach_hook(struct ast_channel *chan)
{
	struct ast_datastore *datastore;
	struct txagc_hook *hook;
	int source;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
	ast_channel_unlock(chan);
	if (datastore) {
		return 0;
	}

	datastore = ast_datastore_alloc(&txagc_datastore, NULL);
	hook = ast_calloc(1, sizeof(*hook));
	if (!datastore || !hook) {
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		txagc_avfilter_init(&hook->avfilter[source]);
	}
	ast_copy_string(hook->channel, ast_channel_name(chan), sizeof(hook->channel));
	if (ast_audiohook_init(&hook->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE,
		"TXAGC", AST_AUDIOHOOK_MANIPULATE_ALL_RATES)) {
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	hook->audiohook.manipulate_callback = txagc_callback;
	datastore->data = hook;

	ast_channel_lock(chan);
	if (ast_channel_datastore_find(chan, &txagc_datastore, NULL)) {
		ast_channel_unlock(chan);
		ast_datastore_free(datastore);
		return 0;
	}
	ast_channel_datastore_add(chan, datastore);
	ast_channel_unlock(chan);
	if (ast_audiohook_attach(chan, &hook->audiohook)) {
		ast_channel_lock(chan);
		ast_channel_datastore_remove(chan, datastore);
		ast_channel_unlock(chan);
		ast_datastore_free(datastore);
		return -1;
	}
	ast_log(LOG_NOTICE, "RadioPlus processing attached to %s\n", ast_channel_name(chan));
	return 0;
}

static void scan_channels(void)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;
	struct ast_channel *primary;
	struct txagc_settings current;

	ast_mutex_lock(&settings_lock);
	current = settings;
	ast_mutex_unlock(&settings_lock);
	if (!current.enabled) {
		return;
	}
	/* Do not attach to unrelated live IAX channels when RadioPlus is merely
	 * loaded side-by-side for validation and its primary channel is absent. */
	primary = ast_channel_get_by_name(current.channel);
	if (!primary) {
		return;
	}
	primary = ast_channel_unref(primary);
	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		return;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		ast_channel_lock(chan);
		if (channel_is_eligible(chan, &current)) {
			ast_channel_unlock(chan);
			attach_hook(chan);
		} else {
			ast_channel_unlock(chan);
		}
		chan = ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
}

static void detach_all(void)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;
	struct ast_datastore *datastore;

	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		return;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		ast_channel_lock(chan);
		datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
		if (datastore) {
			ast_channel_datastore_remove(chan, datastore);
		}
		ast_channel_unlock(chan);
		if (datastore) {
			ast_datastore_free(datastore);
		}
		chan = ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
}

static void *scanner(void *unused)
{
	(void) unused;
	while (!stopping) {
		scan_channels();
		usleep(SCAN_INTERVAL_US);
	}
	return NULL;
}

static char *cli_show(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	struct txagc_settings current;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing show";
		entry->usage = "Usage: radioplus processing show\n       Show RadioPlus processing configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	ast_mutex_lock(&settings_lock);
	current = settings;
	ast_mutex_unlock(&settings_lock);
	for (int source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		struct txagc_chain *chain = &current.chains[source];
		ast_cli(args->fd, "Chain %s: %s, RNNoise %s, AGC %s, expander %s, "
			"compressor %s, two-band limiter %s, brick-wall band-pass %s, "
			"lookahead limiter %s, input gain %.1f dB, output gain %.1f dB\n",
			source_names[source], chain->enabled ? "enabled" : "disabled",
			chain->rnnoise_enabled ? "enabled" : "disabled",
			chain->agc.agc_enabled ? "enabled" : "disabled",
			chain->agc.expander_enabled ? "enabled" : "disabled",
			chain->agc.compressor_enabled ? "enabled" : "disabled",
			chain->agc.limiter_enabled ? "enabled" : "disabled",
			chain->agc.splatter_filter_enabled ? "enabled" : "disabled",
			chain->agc.lookahead_limiter_enabled ? "enabled" : "disabled",
			chain->agc.input_gain_db,
			chain->agc.output_gain_db);
	}
	ast_cli(args->fd, "\nDetailed local-chain settings:\n");
	ast_cli(args->fd, "Enabled: %s\nLocal receiver: %s\nLinked audio: %s\n"
		"Channel: %s\nRNNoise: %s\n"
		"AGC stage: %s\nTarget: %.1f dBFS\nMax gain: %.1f dB\n"
		"Max attenuation: %.1f dB\nAGC detector floor: %.1f dBFS\nAttack: %.0f ms\n"
		"Release: %.0f ms\nReset after: %.0f ms\nSidechain band-pass: %.0f-%.0f Hz\n"
		"Downward expander: %s\nExpander threshold: %.1f dBFS\nExpander ratio: %.1f:1\n"
		"Expander maximum attenuation: %.1f dB\nExpander attack: %.0f ms\n"
		"Expander release: %.0f ms\nExpander sidechain band-pass: %.0f-%.0f Hz\n"
		"Compressor: %s\nCompressor threshold: %.1f dBFS\nCompressor ratio: %.1f:1\n"
		"Compressor make-up gain: %.1f dB\nCompressor attack: %.0f ms\n"
		"Compressor release: %.0f ms\nCompressor sidechain band-pass: %.0f-%.0f Hz\n"
		"Two-band limiter: %s\nBrick-wall band-pass: %s\nCrossover: %.0f Hz\nLow-band threshold: %.1f dBFS\n"
		"Low-band ratio: %.1f:1\nLow-band knee: %.1f dB\n"
		"Low-band attack: %.1f ms\nLow-band release: %.0f ms\n"
		"High-band limit: %.1f dBFS\nHigh-band ratio: %.1f:1\nHigh-band knee: %.1f dB\n"
		"High-band attack: %.1f ms\nHigh-band release: %.1f ms\n"
		"Final clipper: %s\nFinal clip: %.1f dBFS\n"
		"Lookahead limiter: %s\nLookahead ceiling: %.1f dBFS\nLookahead: %.1f ms\n"
		"Lookahead attack: %.1f ms\nLookahead release: %.0f ms\nOutput band-pass: %.0f-%.0f Hz\n"
		"Final output gain: %.1f dB\n",
		current.enabled ? "yes" : "no",
		current.local_enabled ? "enabled" : "disabled",
		current.link_enabled ? "enabled" : "disabled", current.channel,
		current.rnnoise_enabled ? "enabled" : "disabled",
		current.agc.agc_enabled ? "enabled" : "disabled",
		current.agc.target_dbfs, current.agc.max_gain_db,
		current.agc.max_attenuation_db, current.agc.agc_floor_dbfs,
		current.agc.attack_ms, current.agc.release_ms,
		current.agc.reset_after_ms, current.agc.sidechain_highpass_hz,
		current.agc.sidechain_lowpass_hz,
		current.agc.expander_enabled ? "enabled" : "disabled",
		current.agc.expander_threshold_dbfs, current.agc.expander_ratio,
		current.agc.expander_max_attenuation_db, current.agc.expander_attack_ms,
		current.agc.expander_release_ms, current.agc.expander_sidechain_highpass_hz,
		current.agc.expander_sidechain_lowpass_hz,
		current.agc.compressor_enabled ? "enabled" : "disabled",
		current.agc.compressor_threshold_dbfs, current.agc.compressor_ratio,
		current.agc.compressor_makeup_gain_db, current.agc.compressor_attack_ms,
		current.agc.compressor_release_ms, current.agc.compressor_sidechain_highpass_hz,
		current.agc.compressor_sidechain_lowpass_hz,
		current.agc.limiter_enabled ? "enabled" : "disabled",
		current.agc.splatter_filter_enabled ? "enabled" : "disabled",
		current.agc.limiter_crossover_hz, current.agc.low_limiter_threshold_dbfs,
		current.agc.low_limiter_ratio, current.agc.low_limiter_knee_db,
		current.agc.low_limiter_attack_ms, current.agc.low_limiter_release_ms,
		current.agc.high_clip_dbfs,
		current.agc.high_limiter_ratio, current.agc.high_limiter_knee_db,
		current.agc.high_limiter_attack_ms, current.agc.high_limiter_release_ms,
		current.agc.final_clipper_enabled ? "enabled" : "disabled",
		current.agc.final_clip_dbfs,
		current.agc.lookahead_limiter_enabled ? "enabled" : "disabled",
		current.agc.lookahead_limit_dbfs, current.agc.lookahead_ms,
		current.agc.lookahead_attack_ms, current.agc.lookahead_release_ms,
		current.agc.output_highpass_hz, current.agc.output_lowpass_hz,
		current.agc.output_gain_db);
	return CLI_SUCCESS;
}

static char *cli_stats(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;
	struct ast_datastore *datastore;
	struct txagc_hook *hook;
	struct txagc_avfilter *filter;
	int found = 0;
	int source;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing stats";
		entry->usage = "Usage: radioplus processing stats\n       Show live RadioPlus processing measurements.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		return CLI_FAILURE;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		ast_channel_lock(chan);
		datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
		if (datastore && (hook = datastore->data)) {
			ast_audiohook_lock(&hook->audiohook);
			for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
				filter = &hook->avfilter[source];
				if (!filter->input_samples) continue;
				ast_cli(args->fd,
					"%s/%s: input peak %.1f dBFS RMS %.1f dBFS; "
					"output peak %.1f dBFS RMS %.1f dBFS; max peak %.1f dBFS; "
					"input %llu output %llu startup fill %llu runtime underrun %llu samples\n",
					hook->channel, source_names[source],
					filter->input_peak_dbfs, filter->input_rms_dbfs,
					filter->output_peak_dbfs, filter->output_rms_dbfs,
					filter->output_max_peak_dbfs,
					(unsigned long long) filter->input_samples,
					(unsigned long long) filter->output_samples,
					(unsigned long long) filter->startup_fill_samples,
					(unsigned long long) filter->runtime_underrun_samples);
			}
			ast_audiohook_unlock(&hook->audiohook);
			found = 1;
		}
		ast_channel_unlock(chan);
		chan = ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
	if (!found) {
		ast_cli(args->fd, "No RadioPlus processing hook is currently attached.\n");
	}
	return CLI_SUCCESS;
}

static char *cli_enable(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing enable";
		entry->usage = "Usage: radioplus processing enable\n       Enable RadioPlus processing until reload or restart.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	ast_mutex_lock(&settings_lock);
	settings.enabled = 1;
	ast_mutex_unlock(&settings_lock);
	scan_channels();
	ast_cli(args->fd, "RadioPlus processing enabled.\n");
	return CLI_SUCCESS;
}

static char *cli_disable(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing disable";
		entry->usage = "Usage: radioplus processing disable\n       Disable RadioPlus processing and remove active hooks.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	ast_mutex_lock(&settings_lock);
	settings.enabled = 0;
	ast_mutex_unlock(&settings_lock);
	detach_all();
	ast_cli(args->fd, "RadioPlus processing disabled and detached.\n");
	return CLI_SUCCESS;
}

static char *cli_reload(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing reload";
		entry->usage = "Usage: radioplus processing reload\n       Reload usbradioplus-processing.conf.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	if (load_settings()) {
		ast_cli(args->fd, "RadioPlus processing configuration reload failed.\n");
		return CLI_FAILURE;
	}
	/*
	 * The audio callback reads the current settings for every frame, so active
	 * hooks adopt a valid replacement configuration immediately.  Detaching
	 * here can block behind an active manipulate callback and needlessly
	 * interrupts audio.  A scan is still needed to attach hooks to sources that
	 * have just become eligible.
	 */
	scan_channels();
	ast_cli(args->fd, "RadioPlus processing configuration reloaded in place.\n");
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_show, "Show RadioPlus processing configuration"),
	AST_CLI_DEFINE(cli_stats, "Show RadioPlus processing statistics"),
	AST_CLI_DEFINE(cli_enable, "Enable RadioPlus processing"),
	AST_CLI_DEFINE(cli_disable, "Disable RadioPlus processing"),
	AST_CLI_DEFINE(cli_reload, "Reload RadioPlus processing configuration"),
};

int usbradioplus_processing_get_local(struct txagc_chain *chain)
{
	if (!chain) return -1;
	ast_mutex_lock(&settings_lock);
	*chain = settings.chains[TXAGC_LOCAL];
	chain->enabled = chain->enabled && settings.enabled;
	ast_mutex_unlock(&settings_lock);
	return 0;
}

int usbradioplus_processing_get_composite(struct txagc_chain *chain)
{
	if (!chain) return -1;
	ast_mutex_lock(&settings_lock);
	*chain = settings.chains[TXAGC_VOICE_TELEMETRY];
	chain->enabled = chain->enabled && settings.enabled;
	ast_mutex_unlock(&settings_lock);
	return 0;
}

static int usbradioplus_processing_unload(void)
{
	stopping = 1;
	if (scan_thread != AST_PTHREADT_NULL) {
		pthread_join(scan_thread, NULL);
		scan_thread = AST_PTHREADT_NULL;
	}
	detach_all();
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));
	return 0;
}

static int usbradioplus_processing_load(void)
{
	settings_defaults(&settings);
	if (load_settings()) {
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries))) {
		return AST_MODULE_LOAD_DECLINE;
	}
	stopping = 0;
	if (ast_pthread_create_background(&scan_thread, NULL, scanner, NULL)) {
		ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));
		return AST_MODULE_LOAD_FAILURE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int usbradioplus_processing_reload(void)
{
	if (load_settings()) return -1;
	scan_channels();
	return 0;
}
