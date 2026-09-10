/** @file
 * @brief Named-channel processing configuration, audiohooks, live controls, and meters.
 */

#include "asterisk.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
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
#include "usbradioplus_config.h"
#include "usbradioplus_ctcss.h"
#include "usbradioplus_processing.h"
#include "usbradioplus_processing_internal.h"
#include "usbradioplus_radio.h"

#define CONFIG_FILE "usbradioplus.conf"

#define SCAN_INTERVAL_US 250000

#ifdef URP_PROCESSING_TESTING
#define PROCESSING_PRIVATE
#else

#define PROCESSING_PRIVATE static
#endif

/** Configuration names for the local, link, and voice/telemetry chains. */
static const char *source_names[TXAGC_SOURCE_COUNT] = {"local", "link", "voice_telemetry"};

/** One shipped default for a clean-slate radio-signaling control. */
struct clean_slate_option_default {
	/** Configuration section which owns the control. */
	const char *section;
	/** Configuration key within the section. */
	const char *name;
	/** Textual value used by the parser and tuner. */
	const char *value;
};

/** @brief Shipped defaults for omitted clean-slate signaling controls.
 *
 * These values resolve after explicit profile overrides, preserving the
 * documented precedence of scoped sections over flat sections.
 */
static const struct clean_slate_option_default clean_slate_option_defaults[] = {
	{"receive", "signaling_method", "carrier"},
	{"receive", "cpu_saver_enabled", "no"},
	{"receive", "audio_source", "flat"},
	{"receive", "cos_assignment", "dsp"},
	{"receive", "vox_hang_ms", "2000"},
	{"receive", "vox_threshold", "0"},
	{"receive", "noise_squelch_hysteresis", "3000"},
	{"receive", "noise_filter_type", "0"},
	{"receive", "squelch_delay_ms", "0"},
	{"receive", "on_delay_frames", "0"},
	{"receive", "polarity_inverted", "no"},
	{"receive", "squelch_level", "500"},
	{"receive", "frequency_hz", "0"},
	{"receive", "lsd_polarity_inverted", "no"},
	{"transmit", "signaling_method", "carrier"},
	{"transmit", "cpu_saver_enabled", "no"},
	{"transmit", "preemphasis_enabled", "yes"},
	{"transmit", "settle_ms", "500"},
	{"transmit", "rx_blanking_ms", "0"},
	{"transmit", "off_delay_frames", "0"},
	{"transmit", "polarity_inverted", "no"},
	{"transmit", "frequency_hz", "0"},
	{"transmit", "lsd_polarity_inverted", "no"},
	{"ctcss", "receive_frequencies", "100.0"},
	{"ctcss", "transmit_frequencies", "100.0"},
	{"ctcss", "receive_source", "dsp"},
	{"ctcss", "receive_decoder_gain_db", "0.0"},
	{"ctcss", "receive_override_enabled", "no"},
	{"ctcss", "receive_relax", "1"},
	{"ctcss", "transmit_default_hz", "100.0"},
	{"ctcss", "transmit_peak_dbfs", "-24.0"},
	{"ctcss", "turnoff_mode", "ctcss_phase_shift"},
	{"ctcss", "phase_shift_degrees", "120.0"},
	{"ctcss", "tail_duration_ms", "180"},
	{"ctcss", "tail_frequency_hz", "55.0"},
	{"dcs", "receive_code", "023N"},
	{"dcs", "transmit_code", "023N"},
	{"dcs", "turnoff_code_enabled", "yes"},
	{"dcs", "turnoff_duration_ms", "180"},
	{"dcs", "peak_dbfs", "-24.0"},
};

/** @brief Find the shipped default for an omitted clean-slate signaling option.
 * @param section Configuration section name.
 * @param name Configuration option name.
 * @return Static default text, or NULL when the option has no clean-slate default.
 */
static const char *clean_slate_option_default(const char *section, const char *name)
{
	size_t index;

	for (index = 0; index < ARRAY_LEN(clean_slate_option_defaults); ++index) {
		const struct clean_slate_option_default *entry =
			&clean_slate_option_defaults[index];

		if (!strcasecmp(entry->section, section) && !strcasecmp(entry->name, name))
			return entry->value;
	}
	return NULL;
}

/** @brief Return the configuration spelling of a PL-filter mode.
 * @param mode PL-filter selection from enum txagc_ctcss_filter_mode.
 * @return Static mode-name string; the caller must not free it.
 */
PROCESSING_PRIVATE const char *ctcss_filter_name(int mode)
{
	switch (mode) {
	case TXAGC_CTCSS_FILTER_NOTCH:
		return "notch";
	case TXAGC_CTCSS_FILTER_HIGHPASS:
		return "highpass";
	default:
		return "disabled";
	}
}

/** Protects live profile settings; release it before Asterisk channel lookup. */
AST_MUTEX_DEFINE_STATIC(settings_lock);
/** Live configuration snapshot protected by settings_lock. */
PROCESSING_PRIVATE struct txagc_settings settings;
/** Immutable settings retained until module unload for lock-free audio readers. */
struct txagc_audio_snapshot {
	/** Complete validated settings copied before publication. */
	struct txagc_settings settings;
	/** Older immutable snapshot retained while audio may still reference it. */
	struct txagc_audio_snapshot *next;
};
/** Currently published audio-thread settings snapshot. */
static _Atomic(struct txagc_audio_snapshot *) audio_settings;
/** Writer-owned list of snapshots reclaimed only after audio processing stops. */
static struct txagc_audio_snapshot *audio_settings_history;
/** Candidate settings visible only while one control-plane reload builds graphs. */
/* The reload control plane alone consults this pointer.  Test builds expose it
 * so the candidate-only accessors can be verified without publishing a
 * half-built generation to an audio callback. */
PROCESSING_PRIVATE _Thread_local const struct txagc_settings *staged_settings;
/** Background thread that attaches eligible link audiohooks. */
PROCESSING_PRIVATE pthread_t scan_thread = AST_PTHREADT_NULL;
/** Scanner stop request observed during module shutdown. */
PROCESSING_PRIVATE int stopping;
/** Set when a candidate configuration contains a malformed value. */
PROCESSING_PRIVATE int settings_parse_error;
#ifdef URP_PROCESSING_TESTING
/* Test-only fault counters make normally impossible SPSC invariant failures
 * observable. They are never compiled into the installed audio callback. */
PROCESSING_PRIVATE int processing_test_pcm_push_fail_after;
PROCESSING_PRIVATE int processing_test_pcm_pop_fail_after;
PROCESSING_PRIVATE int processing_test_statistics_flip_index;
PROCESSING_PRIVATE int processing_test_worker_start_result;
#endif
static int is_flat_section(const char *category);
static int validate_active_crossovers(struct txagc_settings *candidate);
struct usbradioplus_native_graph_transaction;
int usbradioplus_prepare_all_native_processing(void);
int usbradioplus_stage_all_native_processing(
	struct usbradioplus_native_graph_transaction **transaction);
void usbradioplus_publish_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction);
void usbradioplus_discard_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction);
int usbradioplus_refresh_all_processing_hardware(void);
int usbradioplus_refresh_all_processing_signaling(void);

/** @brief Find a profile without modifying an immutable settings snapshot.
 * @param current Immutable settings snapshot to search.
 * @param channel Configured profile name or Asterisk channel name.
 * @return Matching profile, or NULL when no profile matches.
 */
PROCESSING_PRIVATE const struct txagc_profile *
find_profile_const(const struct txagc_settings *current, const char *channel)
{
	size_t i;

	if (!channel)
		return NULL;
	for (i = 0; i < current->profile_count; ++i)
		if (!strcasecmp(current->profiles[i].name, channel) ||
		    !strcasecmp(current->profiles[i].channel, channel))
			return &current->profiles[i];
	return NULL;
}

/** @brief Allocate a settings snapshot before entering the writer critical section.
 * @return A snapshot ready for publication, or NULL if allocation fails.
 */
static struct txagc_audio_snapshot *allocate_audio_snapshot(void)
{
	struct txagc_audio_snapshot *snapshot = ast_calloc(1, sizeof(*snapshot));

	if (!snapshot)
		ast_log(LOG_ERROR, "RadioPlus: unable to publish lock-free audio settings\n");
	return snapshot;
}

/** @brief Publish a complete immutable settings copy while settings_lock is held.
 * @param snapshot Storage allocated before taking settings_lock.
 */
static void publish_audio_snapshot_locked(struct txagc_audio_snapshot *snapshot)
{
	snapshot->next = audio_settings_history;
	audio_settings_history = snapshot;
	atomic_store_explicit(&audio_settings, snapshot, memory_order_release);
}

/** @brief Publish the mutable settings copy after preparing a snapshot.
 * @param snapshot Fully populated immutable snapshot allocated before locking.
 *
 * Callers that change one live setting use this helper. Reload builds a private
 * candidate first and calls commit_candidate_settings() only after every native
 * and link graph has prepared successfully.
 */
static void publish_audio_settings_locked(struct txagc_audio_snapshot *snapshot)
{
	snapshot->settings = settings;
	publish_audio_snapshot_locked(snapshot);
}

/** @brief Commit an already validated and fully prepared reload generation.
 * @param candidate Mutable settings to become live.
 * @param snapshot Immutable callback snapshot containing candidate.
 *
 * Every fallible graph operation completes before this publication.  A failed
 * reload therefore leaves both the mutable control-plane settings and the
 * immutable callback snapshot on their prior generation.
 */
static void commit_candidate_settings(const struct txagc_settings *candidate,
				      struct txagc_audio_snapshot *snapshot)
{
	ast_mutex_lock(&settings_lock);
	settings = *candidate;
	publish_audio_snapshot_locked(snapshot);
	ast_mutex_unlock(&settings_lock);
}

/** @brief Release settings snapshots after all audio hooks and workers are stopped. */
static void clear_audio_settings(void)
{
	struct txagc_audio_snapshot *snapshot;

	atomic_store_explicit(&audio_settings, NULL, memory_order_release);
	while ((snapshot = audio_settings_history) != NULL) {
		audio_settings_history = snapshot->next;
		ast_free(snapshot);
	}
}

/** @brief Find a named channel profile in the supplied settings snapshot.
 * @param current Settings snapshot whose profile table is searched.
 * @param channel Configured radio channel name.
 * @return Borrowed profile pointer, or NULL if no channel matches.
 */
static struct txagc_profile *find_profile(struct txagc_settings *current, const char *channel)
{
	size_t i;
	if (!channel)
		return NULL;
	for (i = 0; i < current->profile_count; ++i)
		if (!strcasecmp(current->profiles[i].name, channel) ||
		    !strcasecmp(current->profiles[i].channel, channel))
			return &current->profiles[i];
	return NULL;
}

/** @brief Identify incoming app_rpt IAX audio that needs a link-processing hook.
 * @param chan Asterisk channel associated with the radio or link.
 * @param current Resolved named-channel profile.
 * @return Nonzero for an incoming link channel whose processing chain is enabled.
 */
PROCESSING_PRIVATE int channel_is_eligible(struct ast_channel *chan,
					   const struct txagc_profile *current)
{
	const char *name = ast_channel_name(chan);
	const char *application = ast_channel_appl(chan);
	const char *data = ast_channel_data(chan);

	/* The RadioPlus channel is processed natively: local receive at 48 kHz
	 * before mixing, and the complete transmitter mix in the final composite
	 * graph.  Audiohooks remain only for incoming link source processing. */
	return current->chains[TXAGC_LINK].enabled && !strncmp(name, "IAX2/", 5) && application &&
	       !strcmp(application, "Rpt") && data && !strcmp(data, "Remote Rx");
}

/** @brief Initialize the complete named-channel settings tree with shipped defaults.
 * @param all Settings tree to initialize.
 */
PROCESSING_PRIVATE void settings_defaults(struct txagc_settings *all)
{
	struct txagc_profile *value;
	struct txagc_chain *base;

	memset(all, 0, sizeof(*all));
	all->profile_count = 1;
	value = &all->profiles[0];
	/* Each named radio starts from the same safe processing defaults. */
	value->enabled = 1;
	ast_copy_string(value->name, "usb", sizeof(value->name));
	ast_copy_string(value->channel, "RadioPlus/usb", sizeof(value->channel));
	base = &value->chains[TXAGC_LOCAL];
	base->enabled = 1;
	base->rnnoise_enabled = 0;
	base->input_gain_configured = 1;
	base->ctcss_filter_configured = 1;
	base->splatter_filter_configured = 1;
	base->agc.stage_count = 6;
	base->agc.stage_order[0] = TXAGC_STAGE_EQUALIZER;
	base->agc.stage_order[1] = TXAGC_STAGE_EXPANDER;
	base->agc.stage_order[2] = TXAGC_STAGE_AGC;
	base->agc.stage_order[3] = TXAGC_STAGE_DEESSER;
	base->agc.stage_order[4] = TXAGC_STAGE_COMPRESSOR;
	base->agc.stage_order[5] = TXAGC_STAGE_LIMITER;
	base->agc.receive_bandpass_enabled = 1;
	base->agc.receive_bandpass_highpass_hz = 20.0;
	base->agc.receive_bandpass_lowpass_hz = 5000.0;
	base->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	base->agc.ctcss_notch_width_hz = 5.0;
	base->agc.ctcss_highpass_hz = 300.0;
	base->agc.agc_enabled = 0;
	base->agc.input_gain_db = 0.0;
	base->agc.equalizer_enabled = 1;
	base->agc.equalizer_low_gain_db = 2.0;
	base->agc.equalizer_low_frequency_hz = 500.0;
	base->agc.equalizer_low_slope = 0.7;
	base->agc.equalizer_mid_gain_db = -0.5;
	base->agc.equalizer_mid_frequency_hz = 1000.0;
	base->agc.equalizer_mid_width_octaves = 1.0;
	base->agc.equalizer_high_gain_db = -1.0;
	base->agc.equalizer_high_frequency_hz = 2000.0;
	base->agc.equalizer_high_slope = 0.7;
	base->agc.deesser_enabled = 0;
	base->agc.deesser_frequency_hz = 4000.0;
	base->agc.deesser_width_octaves = 1.0;
	base->agc.deesser_threshold_dbfs = -18.0;
	base->agc.deesser_ratio = 3.0;
	base->agc.deesser_max_reduction_db = 4.0;
	base->agc.deesser_attack_ms = 2.0;
	base->agc.deesser_release_ms = 60.0;
	base->agc.target_dbfs = -24.0;
	base->agc.max_gain_db = 6.0;
	base->agc.max_attenuation_db = 6.0;
	base->agc.agc_rms_averaging_ms = 200.0;
	base->agc.agc_gain_increase_db_per_second = 2.0;
	base->agc.agc_gain_decrease_db_per_second = 6.0;
	base->agc.agc_activity_threshold_dbfs = -50.0;
	base->agc.agc_activity_hysteresis_db = 3.0;
	base->agc.agc_hold_ms = 500.0;
	base->agc.agc_deadband_db = 1.0;
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
	base->agc.compressor_bands = 3;
	base->agc.compressor_low_crossover_hz = 500.0;
	base->agc.compressor_high_crossover_hz = 2000.0;
	base->agc.compressor_low_threshold_dbfs = -6.0;
	base->agc.compressor_low_ratio = 2.0;
	base->agc.compressor_low_makeup_gain_db = 0.0;
	base->agc.compressor_low_knee_db = 9.0;
	base->agc.compressor_low_attack_ms = 75.0;
	base->agc.compressor_low_release_ms = 300.0;
	base->agc.compressor_mid_threshold_dbfs = -6.0;
	base->agc.compressor_mid_ratio = 2.0;
	base->agc.compressor_mid_makeup_gain_db = 0.0;
	base->agc.compressor_mid_knee_db = 9.0;
	base->agc.compressor_mid_attack_ms = 75.0;
	base->agc.compressor_mid_release_ms = 300.0;
	base->agc.compressor_high_threshold_dbfs = -6.0;
	base->agc.compressor_high_ratio = 2.0;
	base->agc.compressor_high_makeup_gain_db = 0.0;
	base->agc.compressor_high_knee_db = 9.0;
	base->agc.compressor_high_attack_ms = 75.0;
	base->agc.compressor_high_release_ms = 300.0;
	base->agc.compressor_threshold_dbfs = -6.0;
	base->agc.compressor_ratio = 2.0;
	base->agc.compressor_makeup_gain_db = 0.0;
	base->agc.compressor_attack_ms = 75.0;
	base->agc.compressor_release_ms = 300.0;
	base->agc.compressor_sidechain_highpass_hz = 800.0;
	base->agc.compressor_sidechain_lowpass_hz = 1500.0;
	base->agc.limiter_enabled = 0;
	base->agc.limiter_bands = 3;
	base->agc.limiter_threshold_dbfs = -1.5;
	base->agc.limiter_ratio = 20.0;
	base->agc.limiter_knee_db = 0.0;
	base->agc.limiter_attack_ms = 1.0;
	base->agc.limiter_release_ms = 50.0;
	base->agc.splatter_filter_enabled = 0;
	base->agc.limiter_low_crossover_hz = 500.0;
	base->agc.limiter_high_crossover_hz = 2000.0;
	base->agc.low_limiter_threshold_dbfs = -1.5;
	base->agc.low_limiter_ratio = 10.0;
	base->agc.low_limiter_knee_db = 6.0;
	base->agc.low_limiter_attack_ms = 50.0;
	base->agc.low_limiter_release_ms = 250.0;
	base->agc.mid_limiter_threshold_dbfs = -1.5;
	base->agc.mid_limiter_ratio = 10.0;
	base->agc.mid_limiter_knee_db = 6.0;
	base->agc.mid_limiter_attack_ms = 10.0;
	base->agc.mid_limiter_release_ms = 100.0;
	base->agc.high_limiter_threshold_dbfs = -1.5;
	base->agc.high_limiter_ratio = 20.0;
	base->agc.high_limiter_knee_db = 6.0;
	base->agc.high_limiter_attack_ms = 0.5;
	base->agc.high_limiter_release_ms = 25.0;
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
	value->chains[TXAGC_LINK].ctcss_filter_configured = 1;
	value->chains[TXAGC_VOICE_TELEMETRY].ctcss_filter_configured = 1;
	value->chains[TXAGC_LINK].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	value->chains[TXAGC_VOICE_TELEMETRY].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
	value->chains[TXAGC_LINK].agc.receive_bandpass_enabled = 0;
	value->chains[TXAGC_VOICE_TELEMETRY].agc.receive_bandpass_enabled = 0;
	base = &value->chains[TXAGC_VOICE_TELEMETRY];
	base->rnnoise_enabled = 0;
	base->agc.agc_enabled = 0;
	base->agc.expander_enabled = 0;
	base->agc.compressor_enabled = 0;
	base->agc.limiter_enabled = 0;
	base->agc.input_gain_db = 6.0;
	base->agc.stage_count = 6;
	base->agc.stage_order[0] = TXAGC_STAGE_EQUALIZER;
	base->agc.stage_order[1] = TXAGC_STAGE_EXPANDER;
	base->agc.stage_order[2] = TXAGC_STAGE_AGC;
	base->agc.stage_order[3] = TXAGC_STAGE_DEESSER;
	base->agc.stage_order[4] = TXAGC_STAGE_COMPRESSOR;
	base->agc.stage_order[5] = TXAGC_STAGE_LIMITER;
	base->agc.equalizer_enabled = 1;
	base->agc.equalizer_low_gain_db = 2.0;
	base->agc.equalizer_mid_gain_db = -0.5;
	base->agc.equalizer_high_gain_db = -1.0;
	base->agc.splatter_filter_enabled = 1;
	base->splatter_filter_configured = 1;
	base->agc.lookahead_limiter_enabled = 0;
	base->agc.post_limiter_lowpass_enabled = 0;
	base->agc.output_gain_db = 0.0;
	value->hardware.input_gain_db = 0.0;
	value->hardware.output_a_gain_db = 0.0;
	value->hardware.output_b_gain_db = 0.0;
	value->hardware.input_gain_configured = 1;
	value->hardware.output_a_gain_configured = 1;
	value->hardware.output_b_gain_configured = 1;
	value->hardware.output_a_assignment = USBRADIOPLUS_HW_VOICE_CTCSS;
	value->hardware.output_b_assignment = USBRADIOPLUS_HW_OFF;
	value->hardware.output_a_assignment_configured = 1;
	value->hardware.output_b_assignment_configured = 1;
#ifdef URP_PROCESSING_TESTING
	/* Test cases deliberately edit the global settings tree between callbacks. */
	if (all == &settings)
		atomic_store_explicit(&audio_settings, NULL, memory_order_release);
#endif
}

/** @brief Check processing parameter ranges, fixed stages, and optional-stage ordering.
 * @param value Processing-chain settings copied or updated by this operation.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int validate_chain(const struct txagc_chain *value)
{
	unsigned int index;
	unsigned int seen = 0;

#define REQUIRE_FINITE(field)                                                                      \
	do {                                                                                       \
		if (!isfinite(value->agc.field))                                                   \
			return -1;                                                                 \
	} while (0)
	REQUIRE_FINITE(ctcss_notch_width_hz);
	REQUIRE_FINITE(ctcss_highpass_hz);
	REQUIRE_FINITE(receive_bandpass_highpass_hz);
	REQUIRE_FINITE(receive_bandpass_lowpass_hz);
	REQUIRE_FINITE(input_gain_db);
	REQUIRE_FINITE(equalizer_low_gain_db);
	REQUIRE_FINITE(equalizer_low_frequency_hz);
	REQUIRE_FINITE(equalizer_low_slope);
	REQUIRE_FINITE(equalizer_mid_gain_db);
	REQUIRE_FINITE(equalizer_mid_frequency_hz);
	REQUIRE_FINITE(equalizer_mid_width_octaves);
	REQUIRE_FINITE(equalizer_high_gain_db);
	REQUIRE_FINITE(equalizer_high_frequency_hz);
	REQUIRE_FINITE(equalizer_high_slope);
	REQUIRE_FINITE(deesser_frequency_hz);
	REQUIRE_FINITE(deesser_width_octaves);
	REQUIRE_FINITE(deesser_threshold_dbfs);
	REQUIRE_FINITE(deesser_ratio);
	REQUIRE_FINITE(deesser_max_reduction_db);
	REQUIRE_FINITE(deesser_attack_ms);
	REQUIRE_FINITE(deesser_release_ms);
	REQUIRE_FINITE(target_dbfs);
	REQUIRE_FINITE(max_gain_db);
	REQUIRE_FINITE(max_attenuation_db);
	REQUIRE_FINITE(agc_rms_averaging_ms);
	REQUIRE_FINITE(agc_gain_increase_db_per_second);
	REQUIRE_FINITE(agc_gain_decrease_db_per_second);
	REQUIRE_FINITE(agc_activity_threshold_dbfs);
	REQUIRE_FINITE(agc_activity_hysteresis_db);
	REQUIRE_FINITE(agc_hold_ms);
	REQUIRE_FINITE(agc_deadband_db);
	REQUIRE_FINITE(sidechain_highpass_hz);
	REQUIRE_FINITE(sidechain_lowpass_hz);
	REQUIRE_FINITE(expander_threshold_dbfs);
	REQUIRE_FINITE(expander_ratio);
	REQUIRE_FINITE(expander_max_attenuation_db);
	REQUIRE_FINITE(expander_attack_ms);
	REQUIRE_FINITE(expander_release_ms);
	REQUIRE_FINITE(expander_sidechain_highpass_hz);
	REQUIRE_FINITE(expander_sidechain_lowpass_hz);
	REQUIRE_FINITE(compressor_threshold_dbfs);
	REQUIRE_FINITE(compressor_low_crossover_hz);
	REQUIRE_FINITE(compressor_high_crossover_hz);
	REQUIRE_FINITE(compressor_low_threshold_dbfs);
	REQUIRE_FINITE(compressor_low_ratio);
	REQUIRE_FINITE(compressor_low_makeup_gain_db);
	REQUIRE_FINITE(compressor_low_knee_db);
	REQUIRE_FINITE(compressor_low_attack_ms);
	REQUIRE_FINITE(compressor_low_release_ms);
	REQUIRE_FINITE(compressor_mid_threshold_dbfs);
	REQUIRE_FINITE(compressor_mid_ratio);
	REQUIRE_FINITE(compressor_mid_makeup_gain_db);
	REQUIRE_FINITE(compressor_mid_knee_db);
	REQUIRE_FINITE(compressor_mid_attack_ms);
	REQUIRE_FINITE(compressor_mid_release_ms);
	REQUIRE_FINITE(compressor_high_threshold_dbfs);
	REQUIRE_FINITE(compressor_high_ratio);
	REQUIRE_FINITE(compressor_high_makeup_gain_db);
	REQUIRE_FINITE(compressor_high_knee_db);
	REQUIRE_FINITE(compressor_high_attack_ms);
	REQUIRE_FINITE(compressor_high_release_ms);
	REQUIRE_FINITE(limiter_threshold_dbfs);
	REQUIRE_FINITE(limiter_ratio);
	REQUIRE_FINITE(limiter_knee_db);
	REQUIRE_FINITE(limiter_attack_ms);
	REQUIRE_FINITE(limiter_release_ms);
	REQUIRE_FINITE(compressor_ratio);
	REQUIRE_FINITE(compressor_makeup_gain_db);
	REQUIRE_FINITE(compressor_attack_ms);
	REQUIRE_FINITE(compressor_release_ms);
	REQUIRE_FINITE(compressor_sidechain_highpass_hz);
	REQUIRE_FINITE(compressor_sidechain_lowpass_hz);
	REQUIRE_FINITE(limiter_low_crossover_hz);
	REQUIRE_FINITE(limiter_high_crossover_hz);
	REQUIRE_FINITE(low_limiter_threshold_dbfs);
	REQUIRE_FINITE(low_limiter_ratio);
	REQUIRE_FINITE(low_limiter_knee_db);
	REQUIRE_FINITE(low_limiter_attack_ms);
	REQUIRE_FINITE(low_limiter_release_ms);
	REQUIRE_FINITE(mid_limiter_threshold_dbfs);
	REQUIRE_FINITE(mid_limiter_ratio);
	REQUIRE_FINITE(mid_limiter_knee_db);
	REQUIRE_FINITE(mid_limiter_attack_ms);
	REQUIRE_FINITE(mid_limiter_release_ms);
	REQUIRE_FINITE(high_limiter_threshold_dbfs);
	REQUIRE_FINITE(high_limiter_ratio);
	REQUIRE_FINITE(high_limiter_knee_db);
	REQUIRE_FINITE(high_limiter_attack_ms);
	REQUIRE_FINITE(high_limiter_release_ms);
	REQUIRE_FINITE(lookahead_limit_dbfs);
	REQUIRE_FINITE(lookahead_ms);
	REQUIRE_FINITE(lookahead_attack_ms);
	REQUIRE_FINITE(lookahead_release_ms);
	REQUIRE_FINITE(post_limiter_lowpass_hz);
	REQUIRE_FINITE(output_highpass_hz);
	REQUIRE_FINITE(output_lowpass_hz);
	REQUIRE_FINITE(output_gain_db);
#undef REQUIRE_FINITE
	if ((value->agc.compressor_bands != 1 && value->agc.compressor_bands != 3) ||
	    (value->agc.limiter_bands != 1 && value->agc.limiter_bands != 3)) {
		ast_log(LOG_ERROR,
			"RadioPlus: compressor_bands (%d) and limiter_bands (%d) must be 1 or 3\n",
			value->agc.compressor_bands, value->agc.limiter_bands);
		return -1;
	}
#define REQUIRE_BAND_RANGE(field, minimum, maximum)                                                \
	do {                                                                                       \
		if (value->agc.field < (minimum) || value->agc.field > (maximum)) {                \
			ast_log(LOG_ERROR, "RadioPlus: " #field " must be between %g and %g\n",    \
				(double)(minimum), (double)(maximum));                             \
			return -1;                                                                 \
		}                                                                                  \
	} while (0)
	REQUIRE_BAND_RANGE(compressor_low_crossover_hz, 100.0, 2000.0);
	REQUIRE_BAND_RANGE(compressor_high_crossover_hz, 100.0, 5000.0);
	REQUIRE_BAND_RANGE(limiter_low_crossover_hz, 100.0, 2000.0);
	REQUIRE_BAND_RANGE(limiter_high_crossover_hz, 100.0, 5000.0);
	REQUIRE_BAND_RANGE(compressor_low_threshold_dbfs, -60.0, 0.0);
	REQUIRE_BAND_RANGE(compressor_low_ratio, 1.0, 20.0);
	REQUIRE_BAND_RANGE(compressor_low_makeup_gain_db, -30.0, 30.0);
	REQUIRE_BAND_RANGE(compressor_low_knee_db, 0.0, 18.0);
	REQUIRE_BAND_RANGE(compressor_low_attack_ms, 1.0, 1000.0);
	REQUIRE_BAND_RANGE(compressor_low_release_ms, 1.0, 9000.0);
	REQUIRE_BAND_RANGE(compressor_mid_threshold_dbfs, -60.0, 0.0);
	REQUIRE_BAND_RANGE(compressor_mid_ratio, 1.0, 20.0);
	REQUIRE_BAND_RANGE(compressor_mid_makeup_gain_db, -30.0, 30.0);
	REQUIRE_BAND_RANGE(compressor_mid_knee_db, 0.0, 18.0);
	REQUIRE_BAND_RANGE(compressor_mid_attack_ms, 1.0, 1000.0);
	REQUIRE_BAND_RANGE(compressor_mid_release_ms, 1.0, 9000.0);
	REQUIRE_BAND_RANGE(compressor_high_threshold_dbfs, -60.0, 0.0);
	REQUIRE_BAND_RANGE(compressor_high_ratio, 1.0, 20.0);
	REQUIRE_BAND_RANGE(compressor_high_makeup_gain_db, -30.0, 30.0);
	REQUIRE_BAND_RANGE(compressor_high_knee_db, 0.0, 18.0);
	REQUIRE_BAND_RANGE(compressor_high_attack_ms, 1.0, 1000.0);
	REQUIRE_BAND_RANGE(compressor_high_release_ms, 1.0, 9000.0);
	REQUIRE_BAND_RANGE(limiter_threshold_dbfs, -40.0, -1.0);
	REQUIRE_BAND_RANGE(limiter_ratio, 1.0, 20.0);
	REQUIRE_BAND_RANGE(limiter_knee_db, 0.0, 18.0);
	REQUIRE_BAND_RANGE(limiter_attack_ms, 0.1, 1000.0);
	REQUIRE_BAND_RANGE(limiter_release_ms, 1.0, 9000.0);
#undef REQUIRE_BAND_RANGE
	if (value->agc.compressor_high_crossover_hz <= value->agc.compressor_low_crossover_hz) {
		ast_log(LOG_ERROR, "RadioPlus: compressor_high_crossover_hz must exceed "
				   "compressor_low_crossover_hz\n");
		return -1;
	}
	if (value->agc.limiter_high_crossover_hz <= value->agc.limiter_low_crossover_hz) {
		ast_log(LOG_ERROR, "RadioPlus: limiter_high_crossover_hz must exceed "
				   "limiter_low_crossover_hz\n");
		return -1;
	}
	if (value->agc.stage_count > TXAGC_MAX_DYNAMICS_STAGES)
		return -1;
	for (index = 0; index < value->agc.stage_count; ++index) {
		if (value->agc.stage_order[index] < TXAGC_STAGE_EXPANDER ||
		    value->agc.stage_order[index] > TXAGC_STAGE_DEESSER ||
		    (seen & (1U << value->agc.stage_order[index])))
			return -1;
		seen |= 1U << value->agc.stage_order[index];
	}
	if (value->agc.ctcss_filter_mode < TXAGC_CTCSS_FILTER_DISABLED ||
	    value->agc.ctcss_filter_mode > TXAGC_CTCSS_FILTER_HIGHPASS ||
	    value->agc.ctcss_notch_width_hz < 0.2 || value->agc.ctcss_notch_width_hz > 10.0 ||
	    value->agc.ctcss_highpass_hz < 50.0 || value->agc.ctcss_highpass_hz > 500.0 ||
	    value->agc.receive_bandpass_highpass_hz < 20.0 ||
	    value->agc.receive_bandpass_highpass_hz > 2000.0 ||
	    value->agc.receive_bandpass_lowpass_hz <= value->agc.receive_bandpass_highpass_hz ||
	    value->agc.receive_bandpass_lowpass_hz > 6000.0 || value->agc.input_gain_db < -30.0 ||
	    value->agc.input_gain_db > 30.0 || value->agc.equalizer_low_gain_db < -12.0 ||
	    value->agc.equalizer_low_gain_db > 12.0 ||
	    value->agc.equalizer_low_frequency_hz < 20.0 ||
	    value->agc.equalizer_low_frequency_hz > 1000.0 ||
	    value->agc.equalizer_low_slope < 0.1 || value->agc.equalizer_low_slope > 1.0 ||
	    value->agc.equalizer_mid_gain_db < -12.0 || value->agc.equalizer_mid_gain_db > 12.0 ||
	    value->agc.equalizer_mid_frequency_hz < 100.0 ||
	    value->agc.equalizer_mid_frequency_hz > 4000.0 ||
	    value->agc.equalizer_mid_width_octaves < 0.1 ||
	    value->agc.equalizer_mid_width_octaves > 4.0 ||
	    value->agc.equalizer_high_gain_db < -12.0 || value->agc.equalizer_high_gain_db > 12.0 ||
	    value->agc.equalizer_high_frequency_hz < 1000.0 ||
	    value->agc.equalizer_high_frequency_hz > 5000.0 ||
	    value->agc.equalizer_high_slope < 0.1 || value->agc.equalizer_high_slope > 1.0 ||
	    value->agc.deesser_frequency_hz < 2000.0 || value->agc.deesser_frequency_hz > 8000.0 ||
	    value->agc.deesser_width_octaves < 0.1 || value->agc.deesser_width_octaves > 4.0 ||
	    value->agc.deesser_threshold_dbfs < -60.0 || value->agc.deesser_threshold_dbfs > -1.0 ||
	    value->agc.deesser_ratio < 1.0 || value->agc.deesser_ratio > 20.0 ||
	    value->agc.deesser_max_reduction_db < 0.1 ||
	    value->agc.deesser_max_reduction_db > 20.0 || value->agc.deesser_attack_ms < 0.1 ||
	    value->agc.deesser_attack_ms > 100.0 || value->agc.deesser_release_ms < 1.0 ||
	    value->agc.deesser_release_ms > 2000.0 || value->agc.target_dbfs > -3.0 ||
	    value->agc.target_dbfs < -40.0 || value->agc.max_gain_db < 0.0 ||
	    value->agc.max_gain_db > 30.0 || value->agc.max_attenuation_db < 0.0 ||
	    value->agc.max_attenuation_db > 60.0 || value->agc.agc_rms_averaging_ms < 10.0 ||
	    value->agc.agc_rms_averaging_ms > 5000.0 ||
	    value->agc.agc_gain_increase_db_per_second < 0.1 ||
	    value->agc.agc_gain_increase_db_per_second > 100.0 ||
	    value->agc.agc_gain_decrease_db_per_second < 0.1 ||
	    value->agc.agc_gain_decrease_db_per_second > 100.0 ||
	    value->agc.agc_activity_threshold_dbfs < -100.0 ||
	    value->agc.agc_activity_threshold_dbfs > -3.0 ||
	    value->agc.agc_activity_threshold_dbfs >= value->agc.target_dbfs ||
	    value->agc.agc_activity_hysteresis_db < 0.0 ||
	    value->agc.agc_activity_hysteresis_db > 12.0 || value->agc.agc_hold_ms < 0.0 ||
	    value->agc.agc_hold_ms > 10000.0 || value->agc.agc_deadband_db < 0.0 ||
	    value->agc.agc_deadband_db > 6.0 ||
	    (value->agc.sidechain_highpass_hz != 0.0 &&
	     (value->agc.sidechain_highpass_hz < 50.0 ||
	      value->agc.sidechain_highpass_hz > 2000.0)) ||
	    value->agc.sidechain_lowpass_hz < 0.0 || value->agc.sidechain_lowpass_hz > 3500.0 ||
	    (value->agc.sidechain_lowpass_hz != 0.0 &&
	     value->agc.sidechain_lowpass_hz <= value->agc.sidechain_highpass_hz) ||
	    value->agc.expander_threshold_dbfs < -100.0 ||
	    value->agc.expander_threshold_dbfs > -10.0 || value->agc.expander_ratio < 1.0 ||
	    value->agc.expander_ratio > 10.0 || value->agc.expander_max_attenuation_db < 0.0 ||
	    value->agc.expander_max_attenuation_db > 40.0 || value->agc.expander_attack_ms < 1.0 ||
	    value->agc.expander_attack_ms > 1000.0 || value->agc.expander_release_ms < 1.0 ||
	    value->agc.expander_release_ms > 10000.0 ||
	    value->agc.expander_sidechain_highpass_hz < 50.0 ||
	    value->agc.expander_sidechain_highpass_hz > 2000.0 ||
	    value->agc.expander_sidechain_lowpass_hz <= value->agc.expander_sidechain_highpass_hz ||
	    value->agc.expander_sidechain_lowpass_hz > 3500.0 ||
	    value->agc.compressor_threshold_dbfs < -60.0 ||
	    value->agc.compressor_threshold_dbfs > 0.0 || value->agc.compressor_ratio < 1.0 ||
	    value->agc.compressor_ratio > 20.0 || value->agc.compressor_makeup_gain_db < -30.0 ||
	    value->agc.compressor_makeup_gain_db > 30.0 || value->agc.compressor_attack_ms < 1.0 ||
	    value->agc.compressor_attack_ms > 1000.0 || value->agc.compressor_release_ms < 1.0 ||
	    value->agc.compressor_release_ms > 9000.0 ||
	    value->agc.compressor_sidechain_highpass_hz < 50.0 ||
	    value->agc.compressor_sidechain_highpass_hz > 2000.0 ||
	    value->agc.compressor_sidechain_lowpass_hz <=
		    value->agc.compressor_sidechain_highpass_hz ||
	    value->agc.compressor_sidechain_lowpass_hz > 3500.0 ||
	    value->agc.low_limiter_threshold_dbfs < -40.0 ||
	    value->agc.low_limiter_threshold_dbfs > -1.0 || value->agc.low_limiter_ratio < 1.0 ||
	    value->agc.low_limiter_ratio > 20.0 || value->agc.low_limiter_knee_db < 0.0 ||
	    value->agc.low_limiter_knee_db > 18.0 || value->agc.low_limiter_attack_ms < 0.1 ||
	    value->agc.low_limiter_attack_ms > 1000.0 || value->agc.low_limiter_release_ms < 1.0 ||
	    value->agc.low_limiter_release_ms > 9000.0 ||
	    value->agc.mid_limiter_threshold_dbfs < -40.0 ||
	    value->agc.mid_limiter_threshold_dbfs > -1.0 || value->agc.mid_limiter_ratio < 1.0 ||
	    value->agc.mid_limiter_ratio > 20.0 || value->agc.mid_limiter_knee_db < 0.0 ||
	    value->agc.mid_limiter_knee_db > 18.0 || value->agc.mid_limiter_attack_ms < 0.1 ||
	    value->agc.mid_limiter_attack_ms > 1000.0 || value->agc.mid_limiter_release_ms < 1.0 ||
	    value->agc.mid_limiter_release_ms > 9000.0 ||
	    value->agc.high_limiter_threshold_dbfs < -30.0 ||
	    value->agc.high_limiter_threshold_dbfs > -1.0 || value->agc.high_limiter_ratio < 1.0 ||
	    value->agc.high_limiter_ratio > 20.0 || value->agc.high_limiter_knee_db < 0.0 ||
	    value->agc.high_limiter_knee_db > 18.0 || value->agc.high_limiter_attack_ms < 0.1 ||
	    value->agc.high_limiter_attack_ms > 100.0 || value->agc.high_limiter_release_ms < 1.0 ||
	    value->agc.high_limiter_release_ms > 1000.0 ||
	    value->agc.lookahead_limit_dbfs < -30.0 || value->agc.lookahead_limit_dbfs > -0.1 ||
	    value->agc.lookahead_ms < 0.1 || value->agc.lookahead_ms > 20.0 ||
	    value->agc.lookahead_attack_ms < 0.1 || value->agc.lookahead_attack_ms > 20.0 ||
	    value->agc.lookahead_release_ms < 1.0 || value->agc.lookahead_release_ms > 5000.0 ||
	    value->agc.post_limiter_lowpass_hz < 5000.0 ||
	    value->agc.post_limiter_lowpass_hz > 20000.0 || value->agc.output_highpass_hz < 20.0 ||
	    value->agc.output_highpass_hz > 2000.0 ||
	    value->agc.output_lowpass_hz <= value->agc.output_highpass_hz ||
	    value->agc.output_lowpass_hz > 6000.0 || value->agc.output_gain_db < -30.0 ||
	    value->agc.output_gain_db > 30.0) {
		return -1;
	}
	return 0;
}

/** @brief Validate all processing chains and hardware settings in one channel profile.
 * @param value Resolved named-channel profile.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int validate_profile(const struct txagc_profile *value)
{
	int source;
	if (ast_strlen_zero(value->channel)) {
		return -1;
	}
	if ((value->hardware.input_gain_configured &&
	     (value->hardware.input_gain_db < -30.0 || value->hardware.input_gain_db > 30.0)) ||
	    (value->hardware.output_a_gain_configured &&
	     (value->hardware.output_a_gain_db < -30.0 ||
	      value->hardware.output_a_gain_db > 30.0)) ||
	    (value->hardware.output_b_gain_configured &&
	     (value->hardware.output_b_gain_db < -30.0 ||
	      value->hardware.output_b_gain_db > 30.0))) {
		ast_log(LOG_ERROR, "RadioPlus [hardware]: gain must be between -30 and 30 dB\n");
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
		if (source != TXAGC_LOCAL &&
		    value->chains[source].agc.ctcss_filter_mode != TXAGC_CTCSS_FILTER_DISABLED) {
			ast_log(LOG_ERROR,
				"RadioPlus [%s]: CTCSS receive filtering is local-receiver-only\n",
				source_names[source]);
			return -1;
		}
		if (source != TXAGC_LOCAL && value->chains[source].agc.receive_bandpass_enabled) {
			ast_log(LOG_ERROR,
				"RadioPlus [%s]: receive band-pass is local-receiver-only\n",
				source_names[source]);
			return -1;
		}
		if (source != TXAGC_VOICE_TELEMETRY &&
		    (value->chains[source].agc.splatter_filter_enabled ||
		     value->chains[source].agc.lookahead_limiter_enabled ||
		     value->chains[source].agc.post_limiter_lowpass_enabled)) {
			ast_log(LOG_ERROR,
				"RadioPlus [%s]: transmitter-tail stages are valid only in "
				"[voice_telemetry]\n",
				source_names[source]);
			return -1;
		}
	}
	return 0;
}

/** @brief Read a finite numeric option and flag malformed values without committing settings.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param name Numeric option name.
 * @param value Receives the parsed value; unchanged when the option is absent.
 */
PROCESSING_PRIVATE void read_double(struct ast_config *cfg, const char *section, const char *name,
				    double *value)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	char *end = NULL;
	double parsed;

	if (!text) {
		return;
	}
	parsed = strtod(text, &end);
	if (end != text && *end == '\0' && isfinite(parsed)) {
		*value = parsed;
	} else {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s requires a finite number, got '%s'\n",
			section, name, text);
		settings_parse_error = 1;
	}
}

/** @brief Read a yes/no option and flag malformed values without committing settings.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param name Boolean option name.
 * @param value Receives zero or one; unchanged when the option is absent.
 */
PROCESSING_PRIVATE void read_bool(struct ast_config *cfg, const char *section, const char *name,
				  int *value)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	if (!text)
		return;
	if (ast_true(text))
		*value = 1;
	else if (ast_false(text))
		*value = 0;
	else {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s requires yes or no, got '%s'\n", section,
			name, text);
		settings_parse_error = 1;
	}
}

/** @brief Test whether a name belongs to the processing-chain option vocabulary.
 * @param name Option, metadata field, or channel name.
 * @return One for a recognized processing option; zero otherwise.
 */
PROCESSING_PRIVATE int known_chain_option(const char *name)
{
	static const char *const names[] = {
		"enabled",
		"stage_order",
		"rnnoise_enabled",
		"ctcss_filter_mode",
		"receive_bandpass_enabled",
		"receive_bandpass_highpass_hz",
		"receive_bandpass_lowpass_hz",
		"equalizer_enabled",
		"equalizer_low_gain_db",
		"equalizer_low_frequency_hz",
		"equalizer_low_slope",
		"equalizer_mid_gain_db",
		"equalizer_mid_frequency_hz",
		"equalizer_mid_width_octaves",
		"equalizer_high_gain_db",
		"equalizer_high_frequency_hz",
		"equalizer_high_slope",
		"deesser_enabled",
		"deesser_frequency_hz",
		"deesser_width_octaves",
		"deesser_threshold_dbfs",
		"deesser_ratio",
		"deesser_max_reduction_db",
		"deesser_attack_ms",
		"deesser_release_ms",
		"ctcss_notch_width_hz",
		"ctcss_highpass_hz",
		"agc_enabled",
		"input_gain_db",
		"agc_target_dbfs",
		"agc_max_gain_db",
		"agc_max_attenuation_db",
		"agc_rms_averaging_ms",
		"agc_gain_increase_db_per_second",
		"agc_gain_decrease_db_per_second",
		"agc_activity_threshold_dbfs",
		"agc_activity_hysteresis_db",
		"agc_hold_ms",
		"agc_deadband_db",
		"agc_sidechain_highpass_hz",
		"agc_sidechain_lowpass_hz",
		"expander_enabled",
		"expander_threshold_dbfs",
		"expander_ratio",
		"expander_max_attenuation_db",
		"expander_attack_ms",
		"expander_release_ms",
		"expander_sidechain_highpass_hz",
		"expander_sidechain_lowpass_hz",
		"compressor_enabled",
		"compressor_bands",
		"compressor_low_crossover_hz",
		"compressor_high_crossover_hz",
		"compressor_low_threshold_dbfs",
		"compressor_low_ratio",
		"compressor_low_makeup_gain_db",
		"compressor_low_knee_db",
		"compressor_low_attack_ms",
		"compressor_low_release_ms",
		"compressor_mid_threshold_dbfs",
		"compressor_mid_ratio",
		"compressor_mid_makeup_gain_db",
		"compressor_mid_knee_db",
		"compressor_mid_attack_ms",
		"compressor_mid_release_ms",
		"compressor_high_threshold_dbfs",
		"compressor_high_ratio",
		"compressor_high_makeup_gain_db",
		"compressor_high_knee_db",
		"compressor_high_attack_ms",
		"compressor_high_release_ms",
		"compressor_threshold_dbfs",
		"compressor_ratio",
		"compressor_makeup_gain_db",
		"compressor_attack_ms",
		"compressor_release_ms",
		"compressor_sidechain_highpass_hz",
		"compressor_sidechain_lowpass_hz",
		"limiter_enabled",
		"limiter_bands",
		"limiter_threshold_dbfs",
		"limiter_ratio",
		"limiter_knee_db",
		"limiter_attack_ms",
		"limiter_release_ms",
		"splatter_filter_enabled",
		"limiter_low_crossover_hz",
		"limiter_high_crossover_hz",
		"limiter_low_threshold_dbfs",
		"limiter_low_ratio",
		"limiter_low_knee_db",
		"limiter_low_attack_ms",
		"limiter_low_release_ms",
		"limiter_mid_threshold_dbfs",
		"limiter_mid_ratio",
		"limiter_mid_knee_db",
		"limiter_mid_attack_ms",
		"limiter_mid_release_ms",
		"limiter_high_threshold_dbfs",
		"limiter_high_ratio",
		"limiter_high_knee_db",
		"limiter_high_attack_ms",
		"limiter_high_release_ms",
		"lookahead_limiter_enabled",
		"lookahead_limiter_ceiling_dbfs",
		"lookahead_limiter_lookahead_ms",
		"lookahead_limiter_attack_ms",
		"lookahead_limiter_release_ms",
		"post_limiter_lowpass_enabled",
		"post_limiter_lowpass_hz",
		"splatter_filter_highpass_hz",
		"splatter_filter_lowpass_hz",
		"output_gain_db",
	};
	size_t index;
	for (index = 0; index < ARRAY_LEN(names); ++index)
		if (!strcasecmp(name, names[index]))
			return 1;
	return 0;
}

/** Accepted non-audio hardware option names. */
PROCESSING_PRIVATE const char
	*const hardware_override_options[USBRADIOPLUS_HARDWARE_OVERRIDE_OPTION_COUNT] = {
		"hardware_device_identifier",
		"hardware_serial",
		"hardware_interface_type",
		"hardware_eeprom_enabled",
		"hardware_audio_fragment_count",
		"hardware_audio_queue_size",
		"hardware_ptt_inverted",
		"hardware_repeater_number",
		"hardware_area",
		"hardware_user_key",
		"hardware_idle_interval",
		"hardware_turnoff_count",
		"hardware_voter_reporting",
		"hardware_clip_led_gpio",
		"hardware_gpio_1_mode",
		"hardware_gpio_2_mode",
		"hardware_gpio_3_mode",
		"hardware_gpio_4_mode",
		"hardware_gpio_5_mode",
		"hardware_gpio_6_mode",
		"hardware_gpio_7_mode",
		"hardware_gpio_8_mode",
		"hardware_parallel_port_device",
		"hardware_parallel_port_base_address",
		"hardware_parallel_pin_2_assignment",
		"hardware_parallel_pin_3_assignment",
		"hardware_parallel_pin_4_assignment",
		"hardware_parallel_pin_5_assignment",
		"hardware_parallel_pin_6_assignment",
		"hardware_parallel_pin_7_assignment",
		"hardware_parallel_pin_8_assignment",
		"hardware_parallel_pin_9_assignment",
		"hardware_parallel_pin_10_assignment",
		"hardware_parallel_pin_12_assignment",
		"hardware_parallel_pin_13_assignment",
		"hardware_parallel_pin_15_assignment",
		"hardware_emphasis_corner_hz",
};
/** Accepted Asterisk jitter-buffer option names. */
PROCESSING_PRIVATE const char *const asterisk_override_options[] = {
	"asterisk_jitter_buffer_enabled",
	"asterisk_jitter_buffer_max_size_ms",
	"asterisk_jitter_buffer_resync_threshold_ms",
	"asterisk_jitter_buffer_implementation",
	"asterisk_jitter_buffer_logging_enabled",
	"asterisk_jitter_buffer_force_enabled",
	"asterisk_jitter_buffer_target_extra_ms",
	"asterisk_jitter_buffer_video_sync_enabled",
};
/** Accepted local-repeat and duplex option names. */
PROCESSING_PRIVATE const char *const duplex_override_options[] = {
	"duplex_radio_mode",
	"duplex_local_repeat_level",
	"duplex_local_repeat_mode",
};
/** Accepted radio diagnostic option names. */
PROCESSING_PRIVATE const char *const diagnostics_override_options[] = {
	"diagnostics_trace_type",
	"diagnostics_trace_level",
	"diagnostics_fever",
};
/** Accepted DCS signaling options. */
PROCESSING_PRIVATE const char *const dcs_override_options[] = {
	"receive_code", "transmit_code", "turnoff_code_enabled", "turnoff_duration_ms", "peak_dbfs",
};
/** Accepted CTCSS signaling options. */
PROCESSING_PRIVATE const char *const ctcss_override_options[] = {
	"receive_frequencies",	   "transmit_frequencies",     "receive_source",
	"receive_decoder_gain_db", "receive_override_enabled", "receive_relax",
	"transmit_default_hz",	   "transmit_peak_dbfs",       "turnoff_mode",
	"phase_shift_degrees",	   "tail_duration_ms",	       "tail_frequency_hz",
};
/** Accepted receiver controls. Exactly one signaling method is selected here. */
PROCESSING_PRIVATE const char *const receive_override_options[] = {
	"cpu_saver_enabled",	 "audio_source",     "signaling_method",
	"vox_hang_ms",		 "vox_threshold",    "noise_squelch_hysteresis",
	"noise_filter_type",	 "squelch_delay_ms", "on_delay_frames",
	"polarity_inverted",	 "squelch_level",    "frequency_hz",
	"lsd_polarity_inverted", "cos_assignment",
};
/** Accepted transmitter controls. Exactly one signaling method is selected here. */
PROCESSING_PRIVATE const char *const transmit_override_options[] = {
	"cpu_saver_enabled", "signaling_method", "preemphasis_enabled",
	"settle_ms",	     "rx_blanking_ms",	 "off_delay_frames",
	"polarity_inverted", "frequency_hz",	 "lsd_polarity_inverted",
};

/** @brief Compare a configuration name against an allowed-name table.
 * @param name Option name to find.
 * @param options Read-only table of allowed option names.
 * @param count Number of elements available in the supplied block.
 * @return One for a matching name; zero otherwise.
 */
PROCESSING_PRIVATE int option_in_list(const char *name, const char *const *options, size_t count)
{
	size_t option;
	for (option = 0; option < count; ++option)
		if (!strcasecmp(name, options[option]))
			return 1;
	return 0;
}

/** @brief Validate an option against the vocabulary for its section kind.
 * @param category Configuration section name used in diagnostics.
 * @param kind Stage or configuration-section kind.
 * @param variable Candidate configuration option and its value.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int validate_named_option(const char *category, const char *kind,
					     const struct ast_variable *variable)
{
	static const char *const radio_options[] = {
		"channel_enabled", "asterisk_profile", "hardware_profile",
		"receive_profile", "transmit_profile", "ctcss_profile",
		"dcs_profile",	   "duplex_profile",   "diagnostics_profile",
		"local_profile",   "link_profile",     "voice_telemetry_profile",
	};
	static const char *const hardware_options[] = {
		"hardware_input_gain_db",	"hardware_output_a_gain_db",
		"hardware_output_b_gain_db",	"hardware_output_a_assignment",
		"hardware_output_b_assignment",
	};
	if (!strcmp(kind, "radio"))
		return option_in_list(variable->name, radio_options, ARRAY_LEN(radio_options)) ? 0
											       : -1;
	if (!strcmp(kind, "general"))
		return !strcasecmp(variable->name, "channel_enabled") ? 0 : -1;
	if (!strcmp(kind, "asterisk"))
		return option_in_list(variable->name, asterisk_override_options,
				      ARRAY_LEN(asterisk_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "hardware"))
		return (option_in_list(variable->name, hardware_options,
				       ARRAY_LEN(hardware_options)) ||
			option_in_list(variable->name, hardware_override_options,
				       ARRAY_LEN(hardware_override_options)))
			       ? 0
			       : -1;
	if (!strcmp(kind, "receive"))
		return option_in_list(variable->name, receive_override_options,
				      ARRAY_LEN(receive_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "transmit"))
		return option_in_list(variable->name, transmit_override_options,
				      ARRAY_LEN(transmit_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "duplex"))
		return option_in_list(variable->name, duplex_override_options,
				      ARRAY_LEN(duplex_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "diagnostics"))
		return option_in_list(variable->name, diagnostics_override_options,
				      ARRAY_LEN(diagnostics_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "dcs"))
		return option_in_list(variable->name, dcs_override_options,
				      ARRAY_LEN(dcs_override_options))
			       ? 0
			       : -1;
	if (!strcmp(kind, "ctcss"))
		return option_in_list(variable->name, ctcss_override_options,
				      ARRAY_LEN(ctcss_override_options))
			       ? 0
			       : -1;
	if (strcmp(kind, "local") && strcmp(kind, "link") && strcmp(kind, "voice_telemetry"))
		return -1;
	if (strcmp(kind, "local") && !strncasecmp(variable->name, "receive_bandpass_", 17))
		return -1;
	if (!strcmp(kind, "link") && (!strcasecmp(variable->name, "splatter_filter_enabled") ||
				      !strcasecmp(variable->name, "splatter_filter_highpass_hz") ||
				      !strcasecmp(variable->name, "splatter_filter_lowpass_hz")))
		return -1;
	(void)category;
	return known_chain_option(variable->name) ? 0 : -1;
}

/** @brief Reject unknown sections and options in a candidate configuration.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
static int validate_named_sections(struct ast_config *cfg)
{
	const char *category = NULL;
	while ((category = ast_category_browse(cfg, category))) {
		const struct ast_variable *variable;
		const char *separator = strchr(category, ' ');
		char kind[32] = "radio";
		if (separator) {
			size_t length = (size_t)(separator - category);
			if (!separator[1])
				return -1;
			if (length >= sizeof(kind))
				return -1;
			memcpy(kind, category, length);
			kind[length] = '\0';
		} else if (is_flat_section(category))
			ast_copy_string(kind, category, sizeof(kind));
		for (variable = ast_variable_browse(cfg, category); variable;
		     variable = variable->next) {
			if (validate_named_option(category, kind, variable)) {
				ast_log(LOG_ERROR,
					"RadioPlus [%s]: unknown or misplaced option '%s'\n",
					category, variable->name);
				return -1;
			}
		}
	}
	return 0;
}

/** @brief Validate all section option names before parsing their values.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int validate_option_names(struct ast_config *cfg)
{
	return validate_named_sections(cfg);
}

PROCESSING_PRIVATE int valid_frequency_list(const char *text);

/** @brief Check the configured spelling of one DCS code.
 * @param text Candidate three-digit octal code followed by N or I.
 * @return One if the code has valid syntax and polarity; zero otherwise.
 */
PROCESSING_PRIVATE int valid_dcs_code(const char *text)
{
	return text && strlen(text) == 4 && text[0] >= '0' && text[0] <= '7' && text[1] >= '0' &&
	       text[1] <= '7' && text[2] >= '0' && text[2] <= '7' &&
	       (text[3] == 'N' || text[3] == 'n' || text[3] == 'I' || text[3] == 'i');
}

/** @brief Return an exact upper limit for an integer clean-slate control.
 * @param name Configuration option name.
 * @param maximum Receives the inclusive maximum when the option is recognized.
 * @return One when the option requires a nonnegative integral value, otherwise zero.
 *
 * These controls are consumed with strtol() by the channel adapter. Validate
 * their representation here so a successful processing reload cannot defer a
 * decimal, overflowed, or out-of-range value to channel construction.
 */
static int clean_slate_integer_limit(const char *name, long *maximum)
{
	static const struct {
		const char *name;
		long maximum;
	} limits[] = {
		/* These controls are copied into signed 16-bit radio-engine fields. */
		{"vox_hang_ms", INT16_MAX},
		{"vox_threshold", INT16_MAX},
		{"noise_squelch_hysteresis", INT16_MAX},
		/* The receiver implements exactly the two documented detector filters. */
		{"noise_filter_type", 1},
		{"squelch_delay_ms", RXSQDELAYBUFSIZE / 8 - 1},
		/* The channel adapter caps both frame counters at 60 s / 20 ms. */
		{"on_delay_frames", 3000},
		{"squelch_level", 999},
		{"frequency_hz", INT_MAX},
		{"receive_relax", 1},
		{"settle_ms", INT_MAX},
		{"rx_blanking_ms", INT16_MAX},
		{"off_delay_frames", 3000},
	};
	size_t index;

	for (index = 0; index < ARRAY_LEN(limits); ++index) {
		if (!strcasecmp(name, limits[index].name)) {
			*maximum = limits[index].maximum;
			return 1;
		}
	}
	return 0;
}

/** @brief Validate one nonnegative integer that is stored in a channel field.
 * @param text Candidate configuration text.
 * @param maximum Inclusive upper bound imposed by the receiving field.
 * @return One for a complete in-range integer, otherwise zero.
 */
PROCESSING_PRIVATE int valid_nonnegative_integer(const char *text, long maximum)
{
	char *end;
	long value;

	if (!text)
		return 0;
	errno = 0;
	value = strtol(text, &end, 0);
	return end != text && !*end && errno != ERANGE && value >= 0 && value <= maximum;
}

/** @brief Read, validate, and retain one resolved non-audio option.
 * @param updated Profile receiving the resolved option.
 * @param cfg Parsed Asterisk configuration.
 * @param section Section containing the option.
 * @param name Option name.
 * @return Zero when absent or valid, otherwise nonzero.
 */
PROCESSING_PRIVATE int add_override(struct txagc_profile *updated, struct ast_config *cfg,
				    const char *section, const char *name)
{
	const char *value = ast_variable_retrieve(cfg, section, name);
	struct section_override *entry;
	char *end;
	long integer_limit;
	if (!value)
		return 0;
	if (strstr(name, "_enabled") || strstr(name, "_inverted")) {
		if (!ast_true(value) && !ast_false(value))
			goto invalid;
	} else if (!strcasecmp(name, "asterisk_jitter_buffer_implementation")) {
		if (strcasecmp(value, "fixed") && strcasecmp(value, "adaptive"))
			goto invalid;
	} else if (clean_slate_integer_limit(name, &integer_limit)) {
		if (!valid_nonnegative_integer(value, integer_limit))
			goto invalid;
	} else if (!strcasecmp(name, "hardware_emphasis_corner_hz")) {
		double frequency = strtod(value, &end);
		if (end == value || *end || !isfinite(frequency) || frequency <= 0.0 ||
		    frequency >= 300.0)
			goto invalid;
	} else if (!strncasecmp(name, "hardware_gpio_", 14)) {
		if (strcasecmp(value, "in") && strcasecmp(value, "out0") &&
		    strcasecmp(value, "out1"))
			goto invalid;
	} else if (!strncasecmp(name, "hardware_parallel_pin_", 22)) {
		int input_pin = strstr(name, "_10_") || strstr(name, "_12_") ||
				strstr(name, "_13_") || strstr(name, "_15_");
		if (input_pin) {
			if (strcasecmp(value, "in") && strcasecmp(value, "cor") &&
			    strcasecmp(value, "ctcss"))
				goto invalid;
		} else if (strcasecmp(value, "out0") && strcasecmp(value, "out1") &&
			   strcasecmp(value, "ptt"))
			goto invalid;
	} else if (!strcasecmp(name, "hardware_parallel_port_device")) {
		if (ast_strlen_zero(value))
			goto invalid;
	} else if (!strcasecmp(name, "hardware_parallel_port_base_address")) {
		unsigned long address = strtoul(value, &end, 0);
		if (end == value || *end || address > UINT32_MAX)
			goto invalid;
	} else if (!strcasecmp(name, "audio_source")) {
		if (strcasecmp(value, "no") && strcasecmp(value, "speaker") &&
		    strcasecmp(value, "flat"))
			goto invalid;
	} else if (!strcasecmp(name, "receive_source")) {
		if (strcasecmp(value, "no") && strcasecmp(value, "usb") &&
		    strcasecmp(value, "usbinvert") && strcasecmp(value, "dsp") &&
		    strcasecmp(value, "pp") && strcasecmp(value, "ppinvert"))
			goto invalid;
	} else if (!strcasecmp(name, "receive_frequencies") ||
		   !strcasecmp(name, "transmit_frequencies")) {
		if (!valid_frequency_list(value))
			goto invalid;
	} else if (!strcasecmp(name, "transmit_default_hz")) {
		double number = strtod(value, &end);
		/* A zero value is not a disabled setting: the radio engine would
		 * silently replace it with a compatibility default.  Reject it here
		 * so the configuration describes the requested signaling exactly. */
		if (end == value || *end || !isfinite(number) || number <= 0.0 ||
		    !urp_ctcss_frequency_supported((float)number))
			goto invalid;
	} else if (!strcasecmp(name, "phase_shift_degrees") ||
		   !strcasecmp(name, "tail_frequency_hz")) {
		double number = strtod(value, &end);
		if (end == value || *end || !isfinite(number) || number <= 0.0)
			goto invalid;
	} else if (!strcasecmp(name, "tail_duration_ms")) {
		long duration = strtol(value, &end, 0);
		/* Phase and replacement-tone tails reserve two 20 ms state frames. */
		if (end == value || *end || duration < 2L * MS_PER_FRAME || duration > INT16_MAX)
			goto invalid;
	} else if (!strcasecmp(name, "turnoff_mode")) {
		if (strcasecmp(value, "no") && strcasecmp(value, "ctcss_phase_shift") &&
		    strcasecmp(value, "ctcss_tone_remove") && strcasecmp(value, "ctcss_tail_tone"))
			goto invalid;
	} else if (!strcasecmp(name, "receive_code") || !strcasecmp(name, "transmit_code")) {
		if (!valid_dcs_code(value))
			goto invalid;
	} else if (!strcasecmp(name, "turnoff_duration_ms")) {
		long duration = strtol(value, &end, 0);
		if (end == value || *end || duration < 150 || duration > 200)
			goto invalid;
	} else if (!strcasecmp(name, "signaling_method")) {
		if (strcasecmp(value, "carrier") && strcasecmp(value, "ctcss") &&
		    strcasecmp(value, "dcs"))
			goto invalid;
	} else if (!strcasecmp(name, "cos_assignment")) {
		if (strcasecmp(value, "no") && strcasecmp(value, "usb") &&
		    strcasecmp(value, "usbinvert") && strcasecmp(value, "dsp") &&
		    strcasecmp(value, "vox") && strcasecmp(value, "pp") &&
		    strcasecmp(value, "ppinvert"))
			goto invalid;
	} else if (!strcasecmp(name, "receive_decoder_gain_db")) {
		double number = strtod(value, &end);
		if (end == value || *end || !isfinite(number) || number < -60.0 || number > 60.0)
			goto invalid;
	} else if (!strcasecmp(name, "transmit_peak_dbfs") || !strcasecmp(name, "peak_dbfs")) {
		double number = strtod(value, &end);
		if (end == value || *end || !isfinite(number) || number < -90.0 || number > 0.0)
			goto invalid;
	} else if (!strcasecmp(name, "duplex_local_repeat_mode")) {
		if (strcasecmp(value, "hardware") && strcasecmp(value, "software"))
			goto invalid;
	} else if (strcasecmp(name, "hardware_device_identifier") &&
		   strcasecmp(name, "hardware_serial") && strcasecmp(name, "hardware_user_key")) {
		double number = strtod(value, &end);
		if (end == value || *end || !isfinite(number))
			goto invalid;
		if (number < 0.0)
			goto invalid;
		if (!strcasecmp(name, "duplex_local_repeat_level") && number > 999.0)
			goto invalid;
		if (!strcasecmp(name, "hardware_clip_led_gpio") && number > 8.0)
			goto invalid;
		if ((!strcasecmp(name, "hardware_interface_type") ||
		     !strcasecmp(name, "duplex_radio_mode")) &&
		    number > 1.0)
			goto invalid;
	}
	if (updated->override_count >= MAX_SECTION_OVERRIDES)
		return -1;
	entry = &updated->overrides[updated->override_count++];
	ast_copy_string(entry->section, !strcasecmp(name, "channel_enabled") ? "general" : section,
			sizeof(entry->section));
	if (strchr(entry->section, ' '))
		*strchr(entry->section, ' ') = '\0';
	ast_copy_string(entry->name, name, sizeof(entry->name));
	ast_copy_string(entry->value, value, sizeof(entry->value));
	return 0;
invalid:
	ast_log(LOG_ERROR, "RadioPlus [%s]: invalid %s value '%s'\n", section, name, value);
	return -1;
}

/** @brief Read non-audio options from resolved per-channel section names.
 * @param updated Resolved named-channel profile.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param asterisk_section Resolved Asterisk-options section name.
 * @param hardware_section Resolved hardware section name.
 * @param receive_section Resolved receiver-options section name.
 * @param transmit_section Resolved transmitter-options section name.
 * @param ctcss_section Resolved CTCSS-options section name.
 * @param dcs_section Resolved DCS-options section name.
 * @param duplex_section Resolved duplex section name.
 * @param diagnostics_section Resolved diagnostics section name.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int
read_section_overrides(struct txagc_profile *updated, struct ast_config *cfg,
		       const char *asterisk_section, const char *hardware_section,
		       const char *receive_section, const char *transmit_section,
		       const char *ctcss_section, const char *dcs_section,
		       const char *duplex_section, const char *diagnostics_section)
{
	size_t i;
	for (i = 0; i < ARRAY_LEN(asterisk_override_options); ++i)
		if (add_override(updated, cfg, asterisk_section, asterisk_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(hardware_override_options); ++i)
		if (add_override(updated, cfg, hardware_section, hardware_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(receive_override_options); ++i)
		if (add_override(updated, cfg, receive_section, receive_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(transmit_override_options); ++i)
		if (add_override(updated, cfg, transmit_section, transmit_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(duplex_override_options); ++i)
		if (add_override(updated, cfg, duplex_section, duplex_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(diagnostics_override_options); ++i)
		if (add_override(updated, cfg, diagnostics_section,
				 diagnostics_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(dcs_override_options); ++i)
		if (add_override(updated, cfg, dcs_section, dcs_override_options[i]))
			return -1;
	for (i = 0; i < ARRAY_LEN(ctcss_override_options); ++i)
		if (add_override(updated, cfg, ctcss_section, ctcss_override_options[i]))
			return -1;
	return 0;
}

/** @brief Apply shared flat defaults followed by a channel's resolved overrides.
 * @param updated Resolved named-channel profile.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param radio Named radio section supplying channel-level options.
 * @param asterisk_section Resolved Asterisk-options section name.
 * @param hardware_section Resolved hardware section name.
 * @param receive_section Resolved receiver-options section name.
 * @param transmit_section Resolved transmitter-options section name.
 * @param ctcss_section Resolved CTCSS-options section name.
 * @param dcs_section Resolved DCS-options section name.
 * @param duplex_section Resolved duplex section name.
 * @param diagnostics_section Resolved diagnostics section name.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int
read_profile_overrides(struct txagc_profile *updated, struct ast_config *cfg, const char *radio,
		       const char *asterisk_section, const char *hardware_section,
		       const char *receive_section, const char *transmit_section,
		       const char *ctcss_section, const char *dcs_section,
		       const char *duplex_section, const char *diagnostics_section)
{
	if (read_section_overrides(updated, cfg, asterisk_section, hardware_section,
				   receive_section, transmit_section, ctcss_section, dcs_section,
				   duplex_section, diagnostics_section))
		return -1;
	return add_override(updated, cfg, radio, "channel_enabled");
}

/** @brief Read a hardware routing choice and record whether it was explicitly supplied.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param name Hardware routing option name.
 * @param value Receives the parsed output-routing enumeration.
 * @param configured Set when the option is explicitly present.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int read_assignment(struct ast_config *cfg, const char *section,
				       const char *name, int *value, int *configured)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	if (!text)
		return 0;
	*configured = 1;
	if (!strcasecmp(text, "off"))
		*value = USBRADIOPLUS_HW_OFF;
	else if (!strcasecmp(text, "voice"))
		*value = USBRADIOPLUS_HW_VOICE;
	else if (!strcasecmp(text, "ctcss"))
		*value = USBRADIOPLUS_HW_CTCSS;
	else if (!strcasecmp(text, "voice_ctcss"))
		*value = USBRADIOPLUS_HW_VOICE_CTCSS;
	else if (!strcasecmp(text, "auxvoice"))
		*value = USBRADIOPLUS_HW_AUX_VOICE;
	else {
		ast_log(LOG_ERROR, "RadioPlus [hardware]: invalid %s '%s'\n", name, text);
		return -1;
	}
	return 0;
}

/** @brief Check a comma-separated CTCSS frequency list against the tone table.
 * @param text Text to parse; mutable storage may be edited in place.
 * @return One for a nonempty list of supported CTCSS tones; zero otherwise.
 */
PROCESSING_PRIVATE int valid_frequency_list(const char *text)
{
	const char *cursor = text;
	if (ast_strlen_zero(text))
		return 0;
	for (;;) {
		char *end;
		double frequency = strtod(cursor, &end);
		if (end == cursor || !isfinite(frequency) || frequency <= 0.0 ||
		    !urp_ctcss_frequency_supported((float)frequency))
			return 0;
		while (*end == ' ' || *end == '\t')
			++end;
		if (!*end)
			return 1;
		if (*end != ',')
			return 0;
		cursor = end + 1;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (!*cursor)
			return 0;
	}
}

/** @brief Read hardware gains and playback routing.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param hardware Receives the resolved hardware settings.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int read_hardware(struct ast_config *cfg, const char *section,
				     struct usbradioplus_hardware_settings *hardware)
{
	if (ast_variable_retrieve(cfg, section, "hardware_input_gain_db")) {
		hardware->input_gain_configured = 1;
		read_double(cfg, section, "hardware_input_gain_db", &hardware->input_gain_db);
	}
	if (ast_variable_retrieve(cfg, section, "hardware_output_a_gain_db")) {
		hardware->output_a_gain_configured = 1;
		read_double(cfg, section, "hardware_output_a_gain_db", &hardware->output_a_gain_db);
	}
	if (ast_variable_retrieve(cfg, section, "hardware_output_b_gain_db")) {
		hardware->output_b_gain_configured = 1;
		read_double(cfg, section, "hardware_output_b_gain_db", &hardware->output_b_gain_db);
	}
	return read_assignment(cfg, section, "hardware_output_a_assignment",
			       &hardware->output_a_assignment,
			       &hardware->output_a_assignment_configured) ||
	       read_assignment(cfg, section, "hardware_output_b_assignment",
			       &hardware->output_b_assignment,
			       &hardware->output_b_assignment_configured);
}

/** @brief Parse a chain's optional processing order.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param chain Processing-chain settings copied or updated by this operation.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int read_stage_order(struct ast_config *cfg, const char *section,
					struct txagc_chain *chain)
{
	const char *configured = ast_variable_retrieve(cfg, section, "stage_order");
	char error[128];

	if (!configured)
		return 0;
	if (txagc_parse_stage_order(configured, &chain->agc, error, sizeof(error))) {
		ast_log(LOG_ERROR, "RadioPlus [%s]: invalid stage_order: %s\n", section, error);
		return -1;
	}
	return 0;
}

/** @brief Read the explicitly supported one-band or three-band dynamics mode.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param name Band-count option name.
 * @param value Receives the count; unchanged for absent or malformed options.
 */
static void read_band_count(struct ast_config *cfg, const char *section, const char *name,
			    int *value)
{
	const char *text = ast_variable_retrieve(cfg, section, name);
	if (!text)
		return;
	if (!strcmp(text, "1"))
		*value = 1;
	else if (!strcmp(text, "3"))
		*value = 3;
	else {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s must be 1 or 3, not '%s'\n", section, name,
			text);
		settings_parse_error = 1;
	}
}

/** @brief Read the configurable stages belonging to one source chain.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param section Configuration section name.
 * @param chain Receives parsed settings for this source.
 * @return Zero on success; nonzero for an invalid stage order.
 */
PROCESSING_PRIVATE int read_chain(struct ast_config *cfg, const char *section,
				  struct txagc_chain *chain)
{

#define READ_BOOL(name, field) read_bool(cfg, section, name, &(field))
	READ_BOOL("enabled", chain->enabled);
	READ_BOOL("rnnoise_enabled", chain->rnnoise_enabled);
	READ_BOOL("receive_bandpass_enabled", chain->agc.receive_bandpass_enabled);
	read_double(cfg, section, "receive_bandpass_highpass_hz",
		    &chain->agc.receive_bandpass_highpass_hz);
	read_double(cfg, section, "receive_bandpass_lowpass_hz",
		    &chain->agc.receive_bandpass_lowpass_hz);
	{
		const char *mode = ast_variable_retrieve(cfg, section, "ctcss_filter_mode");
		if (mode) {
			chain->ctcss_filter_configured = 1;
			if (!strcasecmp(mode, "notch"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
			else if (!strcasecmp(mode, "highpass"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
			else if (!strcasecmp(mode, "disabled") || !strcasecmp(mode, "off"))
				chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;
			else {
				ast_log(LOG_ERROR,
					"RadioPlus [%s]: invalid ctcss_filter_mode '%s'\n", section,
					mode);
				settings_parse_error = 1;
			}
		}
	}
	read_double(cfg, section, "ctcss_notch_width_hz", &chain->agc.ctcss_notch_width_hz);
	read_double(cfg, section, "ctcss_highpass_hz", &chain->agc.ctcss_highpass_hz);
	READ_BOOL("agc_enabled", chain->agc.agc_enabled);
	READ_BOOL("equalizer_enabled", chain->agc.equalizer_enabled);
	read_double(cfg, section, "equalizer_low_gain_db", &chain->agc.equalizer_low_gain_db);
	read_double(cfg, section, "equalizer_low_frequency_hz",
		    &chain->agc.equalizer_low_frequency_hz);
	read_double(cfg, section, "equalizer_low_slope", &chain->agc.equalizer_low_slope);
	read_double(cfg, section, "equalizer_mid_gain_db", &chain->agc.equalizer_mid_gain_db);
	read_double(cfg, section, "equalizer_mid_frequency_hz",
		    &chain->agc.equalizer_mid_frequency_hz);
	read_double(cfg, section, "equalizer_mid_width_octaves",
		    &chain->agc.equalizer_mid_width_octaves);
	read_double(cfg, section, "equalizer_high_gain_db", &chain->agc.equalizer_high_gain_db);
	read_double(cfg, section, "equalizer_high_frequency_hz",
		    &chain->agc.equalizer_high_frequency_hz);
	read_double(cfg, section, "equalizer_high_slope", &chain->agc.equalizer_high_slope);
	READ_BOOL("deesser_enabled", chain->agc.deesser_enabled);
	read_double(cfg, section, "deesser_frequency_hz", &chain->agc.deesser_frequency_hz);
	read_double(cfg, section, "deesser_width_octaves", &chain->agc.deesser_width_octaves);
	read_double(cfg, section, "deesser_threshold_dbfs", &chain->agc.deesser_threshold_dbfs);
	read_double(cfg, section, "deesser_ratio", &chain->agc.deesser_ratio);
	read_double(cfg, section, "deesser_max_reduction_db", &chain->agc.deesser_max_reduction_db);
	read_double(cfg, section, "deesser_attack_ms", &chain->agc.deesser_attack_ms);
	read_double(cfg, section, "deesser_release_ms", &chain->agc.deesser_release_ms);
	if (ast_variable_retrieve(cfg, section, "input_gain_db")) {
		chain->input_gain_configured = 1;
		read_double(cfg, section, "input_gain_db", &chain->agc.input_gain_db);
	}
	read_double(cfg, section, "agc_target_dbfs", &chain->agc.target_dbfs);
	read_double(cfg, section, "agc_max_gain_db", &chain->agc.max_gain_db);
	read_double(cfg, section, "agc_max_attenuation_db", &chain->agc.max_attenuation_db);
	read_double(cfg, section, "agc_rms_averaging_ms", &chain->agc.agc_rms_averaging_ms);
	read_double(cfg, section, "agc_gain_increase_db_per_second",
		    &chain->agc.agc_gain_increase_db_per_second);
	read_double(cfg, section, "agc_gain_decrease_db_per_second",
		    &chain->agc.agc_gain_decrease_db_per_second);
	read_double(cfg, section, "agc_activity_threshold_dbfs",
		    &chain->agc.agc_activity_threshold_dbfs);
	read_double(cfg, section, "agc_activity_hysteresis_db",
		    &chain->agc.agc_activity_hysteresis_db);
	read_double(cfg, section, "agc_hold_ms", &chain->agc.agc_hold_ms);
	read_double(cfg, section, "agc_deadband_db", &chain->agc.agc_deadband_db);
	read_double(cfg, section, "agc_sidechain_highpass_hz", &chain->agc.sidechain_highpass_hz);
	read_double(cfg, section, "agc_sidechain_lowpass_hz", &chain->agc.sidechain_lowpass_hz);
	READ_BOOL("expander_enabled", chain->agc.expander_enabled);
	read_double(cfg, section, "expander_threshold_dbfs", &chain->agc.expander_threshold_dbfs);
	read_double(cfg, section, "expander_ratio", &chain->agc.expander_ratio);
	read_double(cfg, section, "expander_max_attenuation_db",
		    &chain->agc.expander_max_attenuation_db);
	read_double(cfg, section, "expander_attack_ms", &chain->agc.expander_attack_ms);
	read_double(cfg, section, "expander_release_ms", &chain->agc.expander_release_ms);
	read_double(cfg, section, "expander_sidechain_highpass_hz",
		    &chain->agc.expander_sidechain_highpass_hz);
	read_double(cfg, section, "expander_sidechain_lowpass_hz",
		    &chain->agc.expander_sidechain_lowpass_hz);
	READ_BOOL("compressor_enabled", chain->agc.compressor_enabled);
	read_band_count(cfg, section, "compressor_bands", &chain->agc.compressor_bands);
	read_double(cfg, section, "compressor_low_crossover_hz",
		    &chain->agc.compressor_low_crossover_hz);
	read_double(cfg, section, "compressor_high_crossover_hz",
		    &chain->agc.compressor_high_crossover_hz);
	read_double(cfg, section, "compressor_low_threshold_dbfs",
		    &chain->agc.compressor_low_threshold_dbfs);
	read_double(cfg, section, "compressor_low_ratio", &chain->agc.compressor_low_ratio);
	read_double(cfg, section, "compressor_low_makeup_gain_db",
		    &chain->agc.compressor_low_makeup_gain_db);
	read_double(cfg, section, "compressor_low_knee_db", &chain->agc.compressor_low_knee_db);
	read_double(cfg, section, "compressor_low_attack_ms", &chain->agc.compressor_low_attack_ms);
	read_double(cfg, section, "compressor_low_release_ms",
		    &chain->agc.compressor_low_release_ms);
	read_double(cfg, section, "compressor_mid_threshold_dbfs",
		    &chain->agc.compressor_mid_threshold_dbfs);
	read_double(cfg, section, "compressor_mid_ratio", &chain->agc.compressor_mid_ratio);
	read_double(cfg, section, "compressor_mid_makeup_gain_db",
		    &chain->agc.compressor_mid_makeup_gain_db);
	read_double(cfg, section, "compressor_mid_knee_db", &chain->agc.compressor_mid_knee_db);
	read_double(cfg, section, "compressor_mid_attack_ms", &chain->agc.compressor_mid_attack_ms);
	read_double(cfg, section, "compressor_mid_release_ms",
		    &chain->agc.compressor_mid_release_ms);
	read_double(cfg, section, "compressor_high_threshold_dbfs",
		    &chain->agc.compressor_high_threshold_dbfs);
	read_double(cfg, section, "compressor_high_ratio", &chain->agc.compressor_high_ratio);
	read_double(cfg, section, "compressor_high_makeup_gain_db",
		    &chain->agc.compressor_high_makeup_gain_db);
	read_double(cfg, section, "compressor_high_knee_db", &chain->agc.compressor_high_knee_db);
	read_double(cfg, section, "compressor_high_attack_ms",
		    &chain->agc.compressor_high_attack_ms);
	read_double(cfg, section, "compressor_high_release_ms",
		    &chain->agc.compressor_high_release_ms);
	read_double(cfg, section, "compressor_threshold_dbfs",
		    &chain->agc.compressor_threshold_dbfs);
	read_double(cfg, section, "compressor_ratio", &chain->agc.compressor_ratio);
	read_double(cfg, section, "compressor_makeup_gain_db",
		    &chain->agc.compressor_makeup_gain_db);
	read_double(cfg, section, "compressor_attack_ms", &chain->agc.compressor_attack_ms);
	read_double(cfg, section, "compressor_release_ms", &chain->agc.compressor_release_ms);
	read_double(cfg, section, "compressor_sidechain_highpass_hz",
		    &chain->agc.compressor_sidechain_highpass_hz);
	read_double(cfg, section, "compressor_sidechain_lowpass_hz",
		    &chain->agc.compressor_sidechain_lowpass_hz);
	READ_BOOL("limiter_enabled", chain->agc.limiter_enabled);
	read_band_count(cfg, section, "limiter_bands", &chain->agc.limiter_bands);
	read_double(cfg, section, "limiter_threshold_dbfs", &chain->agc.limiter_threshold_dbfs);
	read_double(cfg, section, "limiter_ratio", &chain->agc.limiter_ratio);
	read_double(cfg, section, "limiter_knee_db", &chain->agc.limiter_knee_db);
	read_double(cfg, section, "limiter_attack_ms", &chain->agc.limiter_attack_ms);
	read_double(cfg, section, "limiter_release_ms", &chain->agc.limiter_release_ms);
	if (ast_variable_retrieve(cfg, section, "splatter_filter_enabled") ||
	    ast_variable_retrieve(cfg, section, "splatter_filter_highpass_hz") ||
	    ast_variable_retrieve(cfg, section, "splatter_filter_lowpass_hz"))
		chain->splatter_filter_configured = 1;
	READ_BOOL("splatter_filter_enabled", chain->agc.splatter_filter_enabled);
	read_double(cfg, section, "limiter_low_crossover_hz", &chain->agc.limiter_low_crossover_hz);
	read_double(cfg, section, "limiter_high_crossover_hz",
		    &chain->agc.limiter_high_crossover_hz);
	read_double(cfg, section, "limiter_low_threshold_dbfs",
		    &chain->agc.low_limiter_threshold_dbfs);
	read_double(cfg, section, "limiter_low_ratio", &chain->agc.low_limiter_ratio);
	read_double(cfg, section, "limiter_low_knee_db", &chain->agc.low_limiter_knee_db);
	read_double(cfg, section, "limiter_low_attack_ms", &chain->agc.low_limiter_attack_ms);
	read_double(cfg, section, "limiter_low_release_ms", &chain->agc.low_limiter_release_ms);
	read_double(cfg, section, "limiter_mid_threshold_dbfs",
		    &chain->agc.mid_limiter_threshold_dbfs);
	read_double(cfg, section, "limiter_mid_ratio", &chain->agc.mid_limiter_ratio);
	read_double(cfg, section, "limiter_mid_knee_db", &chain->agc.mid_limiter_knee_db);
	read_double(cfg, section, "limiter_mid_attack_ms", &chain->agc.mid_limiter_attack_ms);
	read_double(cfg, section, "limiter_mid_release_ms", &chain->agc.mid_limiter_release_ms);
	read_double(cfg, section, "limiter_high_threshold_dbfs",
		    &chain->agc.high_limiter_threshold_dbfs);
	read_double(cfg, section, "limiter_high_ratio", &chain->agc.high_limiter_ratio);
	read_double(cfg, section, "limiter_high_knee_db", &chain->agc.high_limiter_knee_db);
	read_double(cfg, section, "limiter_high_attack_ms", &chain->agc.high_limiter_attack_ms);
	read_double(cfg, section, "limiter_high_release_ms", &chain->agc.high_limiter_release_ms);
	READ_BOOL("lookahead_limiter_enabled", chain->agc.lookahead_limiter_enabled);
	read_double(cfg, section, "lookahead_limiter_ceiling_dbfs",
		    &chain->agc.lookahead_limit_dbfs);
	read_double(cfg, section, "lookahead_limiter_lookahead_ms", &chain->agc.lookahead_ms);
	read_double(cfg, section, "lookahead_limiter_attack_ms", &chain->agc.lookahead_attack_ms);
	read_double(cfg, section, "lookahead_limiter_release_ms", &chain->agc.lookahead_release_ms);
	READ_BOOL("post_limiter_lowpass_enabled", chain->agc.post_limiter_lowpass_enabled);
	read_double(cfg, section, "post_limiter_lowpass_hz", &chain->agc.post_limiter_lowpass_hz);
	read_double(cfg, section, "splatter_filter_highpass_hz", &chain->agc.output_highpass_hz);
	read_double(cfg, section, "splatter_filter_lowpass_hz", &chain->agc.output_lowpass_hz);
	read_double(cfg, section, "output_gain_db", &chain->agc.output_gain_db);
#undef READ_BOOL
	return read_stage_order(cfg, section, chain);
}

/** @brief Construct a bounded channel-scoped section name.
 * @param destination Destination text buffer.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param kind Stage or configuration-section kind.
 * @param name Option, metadata field, or channel name.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
static int scoped_section(char *destination, size_t size, const char *kind, const char *name)
{
	size_t kind_length = strlen(kind);
	size_t name_length = strlen(name);

	if (kind_length + name_length + 2 > size)
		return -1;
	memcpy(destination, kind, kind_length);
	destination[kind_length] = ' ';
	memcpy(destination + kind_length + 1, name, name_length + 1);
	return 0;
}

/** @brief Recognize a scoped processing or hardware profile section.
 * @param category Configuration section name to classify.
 * @return One for a scoped profile section; zero otherwise.
 */
static int is_profile_section(const char *category)
{
	static const char *const prefixes[] = {
		"asterisk ", "hardware ",    "receive ", "transmit ", "duplex ",	 "dcs ",
		"ctcss ",    "diagnostics ", "local ",	 "link ",     "voice_telemetry "};
	size_t i;
	for (i = 0; i < ARRAY_LEN(prefixes); ++i)
		if (!strncasecmp(category, prefixes[i], strlen(prefixes[i])))
			return 1;
	return 0;
}

/** @brief Recognize a shared flat defaults section.
 * @param category Configuration section name to classify.
 * @return One for a shared defaults section; zero otherwise.
 */
static int is_flat_section(const char *category)
{
	static const char *const sections[] = {
		"general", "asterisk", "hardware",    "receive", "transmit", "dcs",
		"ctcss",   "duplex",   "diagnostics", "local",	 "link",     "voice_telemetry"};
	size_t i;
	for (i = 0; i < ARRAY_LEN(sections); ++i)
		if (!strcasecmp(category, sections[i]))
			return 1;
	return 0;
}

/** @brief Resolve an explicit profile reference or the channel's scoped section.
 * @param cfg Asterisk configuration tree owned by the caller.
 * @param radio Named radio section containing optional profile references.
 * @param kind Stage or configuration-section kind.
 * @param section Receives the resolved profile section name.
 * @param section_size Section-name buffer capacity in bytes.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int resolve_profile_section(struct ast_config *cfg, const char *radio,
					       const char *kind, char *section, size_t section_size)
{
	char option[64];
	const char *profile;
	if (snprintf(option, sizeof(option), "%s_profile", kind) >= (int)sizeof(option))
		return -1;
	profile = ast_variable_retrieve(cfg, radio, option);
	if (scoped_section(section, section_size, kind, profile ? profile : radio))
		return -1;
	if (profile && !ast_category_get(cfg, section, NULL)) {
		ast_log(LOG_ERROR, "RadioPlus [%s]: %s references missing section [%s]\n", radio,
			option, section);
		return -1;
	}
	return 0;
}

/** @brief Parse a private, validated settings generation without publishing it.
 * @param snapshot Receives the complete parsed configuration ready for later
 * publication.
 * @return Zero on success; a nonzero status if parsing or validation fails.
 *
 * Reload callers prepare all control-plane graph candidates against this value
 * before calling commit_candidate_settings().  Keeping it private prevents a
 * failed later graph build from becoming observable to either callback path.
 */
PROCESSING_PRIVATE int load_settings_candidate(struct txagc_audio_snapshot **snapshot)
{
	struct ast_flags flags = {0};
	struct ast_config *cfg;
	struct txagc_settings *defaults;
	struct txagc_settings *updated;
	struct txagc_audio_snapshot *candidate_snapshot;
	const char *category = NULL;

	if (!snapshot)
		return -1;
	*snapshot = NULL;

	defaults = ast_calloc(1, sizeof(*defaults));
	updated = ast_calloc(1, sizeof(*updated));
	if (!defaults || !updated) {
		ast_free(defaults);
		ast_free(updated);
		return -1;
	}
	settings_defaults(defaults);
	settings_parse_error = 0;
	cfg = ast_config_load2(CONFIG_FILE, "chan_usbradioplus", flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Unable to load valid %s\n", CONFIG_FILE);
		ast_free(defaults);
		ast_free(updated);
		return -1;
	}
	if (validate_option_names(cfg))
		goto invalid;
	{
		struct txagc_profile *shared = &defaults->profiles[0];
		const char *enabled = ast_variable_retrieve(cfg, "general", "channel_enabled");
		if (enabled) {
			if (ast_true(enabled))
				shared->enabled = 1;
			else if (ast_false(enabled))
				shared->enabled = 0;
			else
				goto invalid;
		}
		if (read_hardware(cfg, "hardware", &shared->hardware) ||
		    read_section_overrides(shared, cfg, "asterisk", "hardware", "receive",
					   "transmit", "ctcss", "dcs", "duplex", "diagnostics") ||
		    read_chain(cfg, "local", &shared->chains[TXAGC_LOCAL]) ||
		    read_chain(cfg, "link", &shared->chains[TXAGC_LINK]) ||
		    read_chain(cfg, "voice_telemetry", &shared->chains[TXAGC_VOICE_TELEMETRY]) ||
		    settings_parse_error)
			goto invalid;
	}

	while ((category = ast_category_browse(cfg, category))) {
		struct txagc_profile *profile;
		char asterisk_section[MAX_CONFIG_SECTION];
		char hardware_section[MAX_CONFIG_SECTION];
		char receive_section[MAX_CONFIG_SECTION];
		char transmit_section[MAX_CONFIG_SECTION];
		char ctcss_section[MAX_CONFIG_SECTION];
		char dcs_section[MAX_CONFIG_SECTION];
		char duplex_section[MAX_CONFIG_SECTION];
		char diagnostics_section[MAX_CONFIG_SECTION];
		char local_section[MAX_CONFIG_SECTION];
		char link_section[MAX_CONFIG_SECTION];
		char voice_section[MAX_CONFIG_SECTION];
		const char *enabled;
		if (is_profile_section(category) || is_flat_section(category))
			continue;
		if (updated->profile_count >= MAX_RADIO_PROFILES) {
			ast_log(LOG_ERROR, "RadioPlus: too many named radio sections\n");
			goto invalid;
		}
		profile = &updated->profiles[updated->profile_count];
		*profile = defaults->profiles[0];
		ast_copy_string(profile->name, category, sizeof(profile->name));
		snprintf(profile->channel, sizeof(profile->channel), "RadioPlus/%s", category);
		if (resolve_profile_section(cfg, category, "asterisk", asterisk_section,
					    sizeof(asterisk_section)) ||
		    resolve_profile_section(cfg, category, "hardware", hardware_section,
					    sizeof(hardware_section)) ||
		    resolve_profile_section(cfg, category, "receive", receive_section,
					    sizeof(receive_section)) ||
		    resolve_profile_section(cfg, category, "transmit", transmit_section,
					    sizeof(transmit_section)) ||
		    resolve_profile_section(cfg, category, "ctcss", ctcss_section,
					    sizeof(ctcss_section)) ||
		    resolve_profile_section(cfg, category, "dcs", dcs_section,
					    sizeof(dcs_section)) ||
		    resolve_profile_section(cfg, category, "duplex", duplex_section,
					    sizeof(duplex_section)) ||
		    resolve_profile_section(cfg, category, "diagnostics", diagnostics_section,
					    sizeof(diagnostics_section)) ||
		    resolve_profile_section(cfg, category, "local", local_section,
					    sizeof(local_section)) ||
		    resolve_profile_section(cfg, category, "link", link_section,
					    sizeof(link_section)) ||
		    resolve_profile_section(cfg, category, "voice_telemetry", voice_section,
					    sizeof(voice_section)))
			goto invalid;
		enabled = ast_variable_retrieve(cfg, category, "channel_enabled");
		if (enabled) {
			if (!ast_true(enabled) && !ast_false(enabled))
				goto invalid;
			profile->enabled = ast_true(enabled);
		}
		if (read_hardware(cfg, hardware_section, &profile->hardware) ||
		    read_profile_overrides(profile, cfg, category, asterisk_section,
					   hardware_section, receive_section, transmit_section,
					   ctcss_section, dcs_section, duplex_section,
					   diagnostics_section) ||
		    read_chain(cfg, local_section, &profile->chains[TXAGC_LOCAL]) ||
		    read_chain(cfg, link_section, &profile->chains[TXAGC_LINK]) ||
		    read_chain(cfg, voice_section, &profile->chains[TXAGC_VOICE_TELEMETRY]))
			goto invalid;
		profile->chains[TXAGC_LINK].agc.splatter_filter_enabled = 0;
		profile->local_enabled = profile->chains[TXAGC_LOCAL].enabled;
		profile->link_enabled = profile->chains[TXAGC_LINK].enabled;
		profile->rnnoise_enabled = profile->chains[TXAGC_LOCAL].rnnoise_enabled;
		profile->agc = profile->chains[TXAGC_LOCAL].agc;
		if (settings_parse_error || validate_profile(profile))
			goto invalid;
		++updated->profile_count;
	}
	if (!updated->profile_count) {
		ast_log(LOG_ERROR, "RadioPlus: %s contains no named radio sections\n", CONFIG_FILE);
		goto invalid;
	}
	/* Before the scanner starts there are no owned link hooks with known rates.
	 * A running reload must preserve the old graph if an active link cannot use
	 * the candidate crossovers. First frames still validate newly opened links. */
	if (scan_thread != AST_PTHREADT_NULL && validate_active_crossovers(updated))
		goto invalid;
	candidate_snapshot = allocate_audio_snapshot();
	if (!candidate_snapshot)
		goto invalid;
	candidate_snapshot->settings = *updated;
	*snapshot = candidate_snapshot;
	ast_config_destroy(cfg);
	ast_free(defaults);
	ast_free(updated);
	return 0;

invalid:
	ast_config_destroy(cfg);
	ast_log(LOG_ERROR, "Invalid configuration in %s; keeping existing configuration\n",
		CONFIG_FILE);
	ast_free(defaults);
	ast_free(updated);
	return -1;
}

/** @brief Load and immediately publish settings when no graph transaction is needed.
 * @return Zero on success; a nonzero status if parsing or validation fails.
 */
PROCESSING_PRIVATE int load_settings(void)
{
	struct txagc_audio_snapshot *snapshot = NULL;

	/* Asterisk runs module loading on a small CLI worker stack. Keep the full
	 * configuration generation in the already required heap snapshot rather than
	 * placing several megabytes of profile storage on that stack. */
	if (load_settings_candidate(&snapshot))
		return -1;
	commit_candidate_settings(&snapshot->settings, snapshot);
	ast_log(LOG_NOTICE, "RadioPlus loaded %zu named radio configuration(s)\n",
		snapshot->settings.profile_count);
	return 0;
}

/** @brief Return the occupancy of a monotonic link-worker descriptor ring.
 * @param read Consumer cursor.
 * @param write Producer cursor.
 * @return Number of published descriptors not yet consumed.
 */
static unsigned int link_worker_control_count(const atomic_uint *read, const atomic_uint *write)
{
	return atomic_load_explicit(write, memory_order_acquire) -
	       atomic_load_explicit(read, memory_order_acquire);
}

/** @brief Return the occupancy of one SPSC PCM queue.
 * @param queue Queue whose monotonic cursors are sampled.
 * @return Number of queued PCM samples.
 */
static unsigned int link_worker_pcm_count(const struct txagc_link_pcm_queue *queue)
{
	return atomic_load_explicit(&queue->write, memory_order_acquire) -
	       atomic_load_explicit(&queue->read, memory_order_acquire);
}

/** @brief Initialize a fixed-storage SPSC PCM queue before either endpoint uses it.
 * @param queue Queue to initialize.
 * @param samples Caller-owned sample storage.
 * @param capacity Number of samples available in storage.
 */
static void link_worker_pcm_init(struct txagc_link_pcm_queue *queue, int16_t *samples,
				 unsigned int capacity)
{
	queue->samples = samples;
	queue->capacity = capacity;
	atomic_init(&queue->read, 0U);
	atomic_init(&queue->write, 0U);
}

/** @brief Push one sample from a queue's fixed producer endpoint.
 * @param queue Queue owned by the producer.
 * @param sample PCM sample to publish.
 * @return Nonzero on success; zero when the queue is full.
 */
PROCESSING_PRIVATE int link_worker_pcm_push(struct txagc_link_pcm_queue *queue, int16_t sample)
{
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_relaxed);
	unsigned int read = atomic_load_explicit(&queue->read, memory_order_acquire);

#ifdef URP_PROCESSING_TESTING
	if (processing_test_pcm_push_fail_after > 0 && --processing_test_pcm_push_fail_after == 0)
		return 0;
#endif
	if (write - read >= queue->capacity)
		return 0;
	queue->samples[write % queue->capacity] = sample;
	atomic_store_explicit(&queue->write, write + 1U, memory_order_release);
	return 1;
}

/** @brief Pop one sample from a queue's fixed consumer endpoint.
 * @param queue Queue owned by the consumer.
 * @param sample Receives the next PCM sample.
 * @return Nonzero on success; zero when the queue is empty.
 */
static int link_worker_pcm_pop(struct txagc_link_pcm_queue *queue, int16_t *sample)
{
	unsigned int read = atomic_load_explicit(&queue->read, memory_order_relaxed);
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_acquire);

#ifdef URP_PROCESSING_TESTING
	if (processing_test_pcm_pop_fail_after > 0 && --processing_test_pcm_pop_fail_after == 0)
		return 0;
#endif
	if (read == write)
		return 0;
	*sample = queue->samples[read % queue->capacity];
	atomic_store_explicit(&queue->read, read + 1U, memory_order_release);
	return 1;
}

/** @brief Inspect the next complete SPSC descriptor without advancing its consumer cursor.
 * @param frames Descriptor ring storage.
 * @param read Consumer cursor.
 * @param write Producer cursor.
 * @param write_limit Snapshot of the latest descriptor visible to this callback.
 * @param frame Receives the next descriptor.
 * @return Nonzero when a descriptor is available before write_limit.
 */
PROCESSING_PRIVATE int link_worker_frame_peek(const struct txagc_link_frame *frames,
					      const atomic_uint *read, const atomic_uint *write,
					      unsigned int write_limit,
					      struct txagc_link_frame *frame)
{
	unsigned int consumed = atomic_load_explicit(read, memory_order_relaxed);
	unsigned int produced = atomic_load_explicit(write, memory_order_acquire);
	unsigned int available = produced - consumed;
	unsigned int limited = write_limit - consumed;

	/* Cursors are deliberately monotonic unsigned integers. Compare occupancy
	 * rather than raw cursor values so the one-frame output snapshot remains
	 * correct after their defined modulo wraparound. */
	if (available > limited)
		produced = write_limit;
	if (consumed == produced)
		return 0;
	*frame = frames[consumed % TXAGC_LINK_WORKER_CONTROL_FRAMES];
	return 1;
}

/** @brief Advance a descriptor consumer after its paired PCM span is consumed or discarded.
 * @param read Consumer cursor to advance.
 */
static void link_worker_frame_pop(atomic_uint *read)
{
	unsigned int consumed = atomic_load_explicit(read, memory_order_relaxed);

	atomic_store_explicit(read, consumed + 1U, memory_order_release);
}

/** @brief Initialize worker measurements before the first graph block.
 * @param statistics Worker measurement storage to reset.
 */
static void link_worker_statistics_init(struct txagc_link_worker_statistics *statistics)
{
	memset(statistics, 0, sizeof(*statistics));
	statistics->input_peak_dbfs = -INFINITY;
	statistics->input_rms_dbfs = -INFINITY;
	statistics->input_max_peak_dbfs = -INFINITY;
	statistics->input_max_rms_dbfs = -INFINITY;
	statistics->output_peak_dbfs = -INFINITY;
	statistics->output_rms_dbfs = -INFINITY;
	statistics->output_max_peak_dbfs = -INFINITY;
	statistics->output_max_rms_dbfs = -INFINITY;
}

/** @brief Refresh worker-owned scalar measurements while the graph is still retained.
 * @param destination Worker measurement snapshot to update.
 * @param source Retained FFmpeg graph providing the measurements.
 */
PROCESSING_PRIVATE void
link_worker_copy_filter_statistics(struct txagc_link_worker_statistics *destination,
				   const struct txagc_avfilter *source)
{
	if (!destination || !source)
		return;
	destination->input_samples = source->input_samples;
	destination->output_samples = source->output_samples;
	destination->startup_fill_samples = source->startup_fill_samples;
	destination->runtime_underrun_samples = source->runtime_underrun_samples;
	destination->input_peak_dbfs = source->input_peak_dbfs;
	destination->input_rms_dbfs = source->input_rms_dbfs;
	destination->input_max_peak_dbfs = source->input_max_peak_dbfs;
	destination->input_max_rms_dbfs = source->input_max_rms_dbfs;
	destination->output_peak_dbfs = source->output_peak_dbfs;
	destination->output_rms_dbfs = source->output_rms_dbfs;
	destination->output_max_peak_dbfs = source->output_max_peak_dbfs;
	destination->output_max_rms_dbfs = source->output_max_rms_dbfs;
}

/** @brief Publish a complete link-worker statistics snapshot without a data race.
 * @param hook Link audiohook owning the worker and published snapshot slots.
 *
 * The worker writes the inactive buffer only after its reader count reaches
 * zero, then release-publishes its index. Readers pin a selected buffer and
 * recheck the index before copying. A temporarily pinned inactive buffer makes
 * diagnostics one block stale instead of introducing a lock in audio work.
 */
PROCESSING_PRIVATE void link_worker_publish_statistics(struct txagc_hook *hook)
{
	unsigned int active = atomic_load_explicit(&hook->statistics_index, memory_order_acquire);
	unsigned int inactive = active ^ 1U;

	if (atomic_load_explicit(&hook->statistics_readers[inactive], memory_order_acquire) != 0U)
		return;
	hook->statistics.input_overflows =
		atomic_load_explicit(&hook->input_overflows, memory_order_relaxed);
	hook->statistics.output_underflows =
		atomic_load_explicit(&hook->output_underflows, memory_order_relaxed);
	hook->statistics.output_malformed =
		atomic_load_explicit(&hook->output_malformed, memory_order_relaxed);
	hook->published_statistics[inactive] = hook->statistics;
	atomic_store_explicit(&hook->statistics_index, inactive, memory_order_release);
}

/** @brief Render a short tapered continuation instead of a hard callback-output gap.
 * @param hook Link audiohook holding the previous output endpoint.
 * @param pcm Destination PCM span.
 * @param samples Number of samples to conceal.
 */
static void link_worker_render_concealment(struct txagc_hook *hook, int16_t *pcm,
					   unsigned int samples)
{
	unsigned int index;
	int16_t previous = hook->concealment_previous;

	for (index = 0; index < samples; ++index) {
		pcm[index] = (int16_t)(((int64_t)previous * (int64_t)(samples - index - 1U)) /
				       (int64_t)samples);
	}
	hook->concealment_previous = 0;
}

/** @brief Discard a complete PCM span paired with a malformed descriptor.
 * @param queue Consumer-side PCM queue.
 * @param samples Number of samples to discard.
 */
static void link_worker_pcm_discard(struct txagc_link_pcm_queue *queue, unsigned int samples)
{
	int16_t ignored;

	while (samples > 0U) {
		--samples;
		if (!link_worker_pcm_pop(queue, &ignored))
			break;
	}
}

/** @brief Submit an untouched Asterisk frame to the link worker's input endpoint.
 * @param hook Link audiohook whose callback owns the producer endpoint.
 * @param pcm Source PCM samples.
 * @param samples Number of source samples.
 * @param generation Graph/admission generation paired with the frame.
 * @return Nonzero when the complete frame was queued; zero on overflow.
 */
PROCESSING_PRIVATE int link_worker_submit_input(struct txagc_hook *hook, const int16_t *pcm,
						unsigned int samples, unsigned int generation)
{
	struct txagc_link_frame frame = {.samples = samples, .generation = generation};
	unsigned int frame_write;
	unsigned int index;

	if (samples > hook->queue_capacity - link_worker_pcm_count(&hook->input_pcm) ||
	    link_worker_control_count(&hook->input_frame_read, &hook->input_frame_write) >=
		    TXAGC_LINK_WORKER_CONTROL_FRAMES) {
		atomic_fetch_add_explicit(&hook->input_overflows, 1U, memory_order_relaxed);
		return 0;
	}
	frame_write = atomic_load_explicit(&hook->input_frame_write, memory_order_relaxed);
	for (index = 0; index < samples; ++index) {
		if (!link_worker_pcm_push(&hook->input_pcm, pcm[index])) {
			/* Publish the exact partial span. The worker will process only those
			 * words, so a later descriptor cannot inherit orphan PCM. The callback
			 * rejects its short result and conceals this one frame. */
			frame.samples = index;
			hook->input_frames[frame_write % TXAGC_LINK_WORKER_CONTROL_FRAMES] = frame;
			atomic_store_explicit(&hook->input_frame_write, frame_write + 1U,
					      memory_order_release);
			atomic_fetch_add_explicit(&hook->input_overflows, 1U, memory_order_relaxed);
			return 0;
		}
	}
	/* The capacity check reserves this sole-producer descriptor slot. Publish it
	 * only after all PCM words are visible, preserving monotonically increasing
	 * cursors and a transactionally paired sample span. */
	hook->input_frames[frame_write % TXAGC_LINK_WORKER_CONTROL_FRAMES] = frame;
	atomic_store_explicit(&hook->input_frame_write, frame_write + 1U, memory_order_release);
	return 1;
}

/** @brief Consume only output that was complete before the current callback began.
 * @param hook Link audiohook whose callback owns the consumer endpoint.
 * @param pcm Destination PCM samples.
 * @param samples Number of destination samples.
 * @param generation Required graph/admission generation.
 * @param write_limit Producer cursor snapshot taken before current input admission.
 */
PROCESSING_PRIVATE void link_worker_consume_output(struct txagc_hook *hook, int16_t *pcm,
						   unsigned int samples, unsigned int generation,
						   unsigned int write_limit)
{
	struct txagc_link_frame frame;
	unsigned int index;

	if (!link_worker_frame_peek(hook->output_frames, &hook->output_frame_read,
				    &hook->output_frame_write, write_limit, &frame)) {
		atomic_fetch_add_explicit(&hook->output_underflows, 1U, memory_order_relaxed);
		link_worker_render_concealment(hook, pcm, samples);
		return;
	}
	if (frame.samples != samples || frame.generation != generation ||
	    link_worker_pcm_count(&hook->output_pcm) < frame.samples) {
		link_worker_pcm_discard(&hook->output_pcm, frame.samples);
		link_worker_frame_pop(&hook->output_frame_read);
		atomic_fetch_add_explicit(&hook->output_malformed, 1U, memory_order_relaxed);
		link_worker_render_concealment(hook, pcm, samples);
		return;
	}
	for (index = 0; index < samples; ++index) {
		if (!link_worker_pcm_pop(&hook->output_pcm, &pcm[index])) {
			/* Metadata is published only after all PCM, so this represents an
			 * internal pairing failure. Drop the descriptor rather than stalling. */
			link_worker_pcm_discard(&hook->output_pcm, samples - index);
			link_worker_frame_pop(&hook->output_frame_read);
			atomic_fetch_add_explicit(&hook->output_malformed, 1U,
						  memory_order_relaxed);
			link_worker_render_concealment(hook, pcm + index, samples - index);
			return;
		}
	}
	link_worker_frame_pop(&hook->output_frame_read);
	hook->concealment_previous = pcm[samples - 1U];
}

/** @brief Process one complete queued link block outside the Asterisk audiohook callback.
 * @param hook Link audiohook whose worker owns graph execution.
 * @return Nonzero when a descriptor was consumed; zero when work cannot proceed yet.
 */
PROCESSING_PRIVATE int txagc_link_worker_process_one(struct txagc_hook *hook)
{
	struct txagc_link_frame frame;
	struct txagc_avfilter *filter;
	unsigned int input_write;
	unsigned int output_frame_write;
	unsigned int index;
	int result = -1;

	if (!hook)
		return 0;
	input_write = atomic_load_explicit(&hook->input_frame_write, memory_order_acquire);
	if (!link_worker_frame_peek(hook->input_frames, &hook->input_frame_read,
				    &hook->input_frame_write, input_write, &frame)) {
		return 0;
	}
	if (!frame.samples || frame.samples > hook->samples_capacity) {
		/* A descriptor is published only after every input sample. An impossible
		 * descriptor must still be consumed so it cannot block every later block. */
		link_worker_pcm_discard(&hook->input_pcm, frame.samples);
		link_worker_frame_pop(&hook->input_frame_read);
		++hook->statistics.input_malformed;
		link_worker_publish_statistics(hook);
		return 1;
	}
	if (frame.samples > hook->queue_capacity - link_worker_pcm_count(&hook->output_pcm) ||
	    link_worker_control_count(&hook->output_frame_read, &hook->output_frame_write) >=
		    TXAGC_LINK_WORKER_CONTROL_FRAMES) {
		return 0;
	}
	if (link_worker_pcm_count(&hook->input_pcm) < frame.samples) {
		/* The output queue has room for the same valid-sized block, so preserve
		 * frame chronology with silence while discarding the malformed input. */
		link_worker_pcm_discard(&hook->input_pcm, frame.samples);
		link_worker_frame_pop(&hook->input_frame_read);
		++hook->statistics.input_malformed;
		output_frame_write =
			atomic_load_explicit(&hook->output_frame_write, memory_order_relaxed);
		for (index = 0; index < frame.samples; ++index) {
			if (!link_worker_pcm_push(&hook->output_pcm, 0)) {
				/* The sole producer reserved this descriptor slot before
				 * pushing PCM. Publish the exact partial span so the callback
				 * can discard it without leaving orphaned output words. */
				frame.samples = index;
				hook->output_frames[output_frame_write %
						    TXAGC_LINK_WORKER_CONTROL_FRAMES] = frame;
				atomic_store_explicit(&hook->output_frame_write,
						      output_frame_write + 1U,
						      memory_order_release);
				link_worker_publish_statistics(hook);
				return 1;
			}
		}
		hook->output_frames[output_frame_write % TXAGC_LINK_WORKER_CONTROL_FRAMES] = frame;
		atomic_store_explicit(&hook->output_frame_write, output_frame_write + 1U,
				      memory_order_release);
		link_worker_publish_statistics(hook);
		return 1;
	}
	for (index = 0; index < frame.samples; ++index) {
		if (!link_worker_pcm_pop(&hook->input_pcm, &hook->source_pcm[index]))
			return 0;
		hook->samples[index] = hook->source_pcm[index];
	}
	link_worker_frame_pop(&hook->input_frame_read);
	filter = txagc_avfilter_slot_acquire(&hook->avfilter[TXAGC_LINK]);
	if (filter) {
		result = txagc_avfilter_process_prepared(filter, hook->samples, frame.samples);
		link_worker_copy_filter_statistics(&hook->statistics, filter);
		txagc_avfilter_slot_release(&hook->avfilter[TXAGC_LINK]);
	}
	output_frame_write = atomic_load_explicit(&hook->output_frame_write, memory_order_relaxed);
	for (index = 0; index < frame.samples; ++index) {
		double value = hook->samples[index];

		if (result >= 0) {
			if (value > 32767.0)
				value = 32767.0;
			else if (value < -32768.0)
				value = -32768.0;
			hook->source_pcm[index] = (int16_t)lrint(value);
		}
		if (!link_worker_pcm_push(&hook->output_pcm, hook->source_pcm[index])) {
			/* Publish the partial span instead of rolling a monotonic PCM cursor
			 * backward. The callback will consume exactly this descriptor and
			 * conceal the incomplete frame before the next one can run. */
			frame.samples = index;
			hook->output_frames[output_frame_write % TXAGC_LINK_WORKER_CONTROL_FRAMES] =
				frame;
			atomic_store_explicit(&hook->output_frame_write, output_frame_write + 1U,
					      memory_order_release);
			link_worker_publish_statistics(hook);
			return 1;
		}
	}
	hook->output_frames[output_frame_write % TXAGC_LINK_WORKER_CONTROL_FRAMES] = frame;
	atomic_store_explicit(&hook->output_frame_write, output_frame_write + 1U,
			      memory_order_release);
	link_worker_publish_statistics(hook);
	return 1;
}

#ifndef URP_PROCESSING_TESTING
/** @brief Run queued FFmpeg graph work away from the Asterisk audiohook lock.
 * @param opaque Link audiohook supplied to pthread_create().
 * @return NULL after the stop request is observed.
 */
static void *link_worker_thread(void *opaque)
{
	struct txagc_hook *hook = opaque;

	while (!atomic_load_explicit(&hook->worker_stopping, memory_order_acquire)) {
		if (!txagc_link_worker_process_one(hook)) {
			link_worker_publish_statistics(hook);
			usleep(1000);
		}
	}
	return NULL;
}
#endif

/** @brief Allocate and initialize all fixed link-worker storage before hook attachment.
 * @param hook Link audiohook receiving worker storage.
 * @param samples_capacity Maximum samples in one graph block.
 * @return Zero on success; nonzero for invalid capacity or allocation failure.
 */
PROCESSING_PRIVATE int txagc_link_worker_initialize(struct txagc_hook *hook,
						    size_t samples_capacity)
{
	unsigned int queue_capacity;

	if (!hook || !samples_capacity ||
	    samples_capacity > UINT_MAX / TXAGC_LINK_WORKER_QUEUE_FRAMES)
		return -1;
	queue_capacity = (unsigned int)samples_capacity * TXAGC_LINK_WORKER_QUEUE_FRAMES;
	hook->samples = ast_calloc(samples_capacity, sizeof(*hook->samples));
	hook->source_pcm = ast_calloc(samples_capacity, sizeof(*hook->source_pcm));
	hook->input_pcm_storage = ast_calloc(queue_capacity, sizeof(*hook->input_pcm_storage));
	hook->output_pcm_storage = ast_calloc(queue_capacity, sizeof(*hook->output_pcm_storage));
	if (!hook->samples || !hook->source_pcm || !hook->input_pcm_storage ||
	    !hook->output_pcm_storage) {
		ast_free(hook->samples);
		ast_free(hook->source_pcm);
		ast_free(hook->input_pcm_storage);
		ast_free(hook->output_pcm_storage);
		hook->samples = NULL;
		hook->source_pcm = NULL;
		hook->input_pcm_storage = NULL;
		hook->output_pcm_storage = NULL;
		return -1;
	}
	hook->samples_capacity = samples_capacity;
	hook->queue_capacity = queue_capacity;
	link_worker_pcm_init(&hook->input_pcm, hook->input_pcm_storage, queue_capacity);
	link_worker_pcm_init(&hook->output_pcm, hook->output_pcm_storage, queue_capacity);
	atomic_init(&hook->input_frame_read, 0U);
	atomic_init(&hook->input_frame_write, 0U);
	atomic_init(&hook->output_frame_read, 0U);
	atomic_init(&hook->output_frame_write, 0U);
	atomic_init(&hook->worker_stopping, 0);
	atomic_init(&hook->generation, 1U);
	atomic_init(&hook->input_overflows, 0U);
	atomic_init(&hook->output_underflows, 0U);
	atomic_init(&hook->output_malformed, 0U);
	atomic_init(&hook->statistics_index, 0U);
	atomic_init(&hook->statistics_readers[0], 0U);
	atomic_init(&hook->statistics_readers[1], 0U);
	link_worker_statistics_init(&hook->statistics);
	link_worker_publish_statistics(hook);
	return 0;
}

/** @brief Start the one worker that owns this hook's FFmpeg graph execution.
 * @param hook Initialized link audiohook.
 * @return Zero on success; nonzero when the worker cannot start.
 */
PROCESSING_PRIVATE int txagc_link_worker_start(struct txagc_hook *hook)
{
	if (!hook || !hook->samples)
		return -1;
#ifdef URP_PROCESSING_TESTING
	if (processing_test_worker_start_result)
		return processing_test_worker_start_result;
	/* The deterministic harness has no worker thread to join during teardown. */
	hook->worker_started = 0;
#else
	if (pthread_create(&hook->worker_thread, NULL, link_worker_thread, hook))
		return -1;
	hook->worker_started = 1;
#endif
	return 0;
}

/** @brief Stop and join a hook worker before its graph or queue storage is destroyed.
 * @param hook Link audiohook whose worker is stopped when running.
 */
PROCESSING_PRIVATE void txagc_link_worker_stop(struct txagc_hook *hook)
{
	if (!hook || !hook->worker_started)
		return;
	atomic_store_explicit(&hook->worker_stopping, 1, memory_order_release);
	pthread_join(hook->worker_thread, NULL);
	hook->worker_started = 0;
}

/** @brief Release fixed link-worker storage after the audiohook and worker have stopped.
 * @param hook Link audiohook whose worker storage is released.
 */
PROCESSING_PRIVATE void txagc_link_worker_destroy(struct txagc_hook *hook)
{
	if (!hook || (!hook->samples && !hook->source_pcm && !hook->input_pcm_storage &&
		      !hook->output_pcm_storage && !hook->worker_started)) {
		return;
	}
	txagc_link_worker_stop(hook);
	ast_free(hook->samples);
	ast_free(hook->source_pcm);
	ast_free(hook->input_pcm_storage);
	ast_free(hook->output_pcm_storage);
	hook->samples = NULL;
	hook->source_pcm = NULL;
	hook->input_pcm_storage = NULL;
	hook->output_pcm_storage = NULL;
	hook->samples_capacity = 0;
	hook->queue_capacity = 0;
}

/** @brief Copy one coherent worker statistics snapshot without reading a live graph.
 * @param hook Link audiohook publishing double-buffered measurements.
 * @param statistics Receives a complete snapshot.
 * @return Zero on success; nonzero when no stable snapshot is available.
 */
PROCESSING_PRIVATE int
txagc_link_worker_statistics_read(struct txagc_hook *hook,
				  struct txagc_link_worker_statistics *statistics)
{
	unsigned int attempt;

	if (!hook || !statistics)
		return -1;
	for (attempt = 0; attempt < 3U; ++attempt) {
		unsigned int index =
			atomic_load_explicit(&hook->statistics_index, memory_order_acquire);

		atomic_fetch_add_explicit(&hook->statistics_readers[index], 1U,
					  memory_order_acquire);
#ifdef URP_PROCESSING_TESTING
		if (processing_test_statistics_flip_index > 0) {
			atomic_store_explicit(&hook->statistics_index, index ^ 1U,
					      memory_order_release);
			--processing_test_statistics_flip_index;
		}
#endif
		if (index == atomic_load_explicit(&hook->statistics_index, memory_order_acquire)) {
			*statistics = hook->published_statistics[index];
			atomic_fetch_sub_explicit(&hook->statistics_readers[index], 1U,
						  memory_order_release);
			return 0;
		}
		atomic_fetch_sub_explicit(&hook->statistics_readers[index], 1U,
					  memory_order_release);
	}
	return -1;
}

/** @brief Detach and destroy graph resources owned by a channel audiohook datastore.
 * @param data Owned txagc_hook datastore payload to destroy.
 */
PROCESSING_PRIVATE void hook_destroy(void *data)
{
	struct txagc_hook *hook = data;
	int source;

	if (!hook) {
		return;
	}
	/* Detachment stops new callback queue writes. Joining next guarantees no
	 * worker owns a graph pointer or queue storage while teardown frees it. */
	ast_audiohook_detach(&hook->audiohook);
	txagc_link_worker_stop(hook);
	ast_audiohook_destroy(&hook->audiohook);
	for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		txagc_avfilter_slot_destroy(&hook->avfilter[source]);
	}
	txagc_link_worker_destroy(hook);
	ast_free(hook);
}

/** Asterisk datastore callbacks owning each link-processing hook. */
static const struct ast_datastore_info txagc_datastore = {
	.type = "txagc",
	.destroy = hook_destroy,
};

/** @brief Reject crossover changes that cannot run at a known active link's sample rate.
 * @param candidate Validated candidate profiles, not yet published to audio callbacks.
 * @return Zero on success; -1 for an incompatible active link or unavailable iterator.
 */
static int validate_active_crossovers(struct txagc_settings *candidate)
{
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	struct ast_channel *channel;
	int invalid = 0;
	if (!iterator) {
		ast_log(LOG_ERROR,
			"RadioPlus: cannot inspect active link rates; keeping settings\n");
		return -1;
	}
	while ((channel = ast_channel_iterator_next(iterator))) {
		const struct ast_datastore *datastore;
		ast_channel_lock(channel);
		datastore = ast_channel_datastore_find(channel, &txagc_datastore, NULL);
		if (datastore && datastore->data) {
			struct txagc_hook *hook = datastore->data;
			struct txagc_profile *profile = find_profile(candidate, hook->profile);
			struct txagc_avfilter *filter;
			ast_audiohook_lock(&hook->audiohook);
			filter = txagc_avfilter_slot_acquire(&hook->avfilter[TXAGC_LINK]);
			unsigned int rate = filter ? filter->sample_rate : 0;
			if (profile && profile->enabled && profile->chains[TXAGC_LINK].enabled &&
			    rate) {
				const struct txagc_config *cfg = &profile->chains[TXAGC_LINK].agc;
				const char *const names[] = {"compressor", "limiter"};
				const int enabled[] = {cfg->compressor_enabled,
						       cfg->limiter_enabled};
				const int bands[] = {cfg->compressor_bands, cfg->limiter_bands};
				const double edges[] = {cfg->compressor_high_crossover_hz,
							cfg->limiter_high_crossover_hz};
				for (size_t stage = 0; stage < ARRAY_LEN(names); ++stage) {
					if (enabled[stage] && bands[stage] == 3 &&
					    edges[stage] >= rate * 0.5) {
						ast_log(LOG_ERROR,
							"RadioPlus [link %s]: %s_high_crossover_hz "
							"%.9g "
							"must be below %.9g Hz at active link rate "
							"%u Hz\n",
							profile->name, names[stage], edges[stage],
							rate * 0.5, rate);
						invalid = 1;
					}
				}
			}
			if (filter)
				txagc_avfilter_slot_release(&hook->avfilter[TXAGC_LINK]);
			ast_audiohook_unlock(&hook->audiohook);
		}
		ast_channel_unlock(channel);
		ast_channel_unref(channel);
	}
	ast_channel_iterator_destroy(iterator);
	return invalid ? -1 : 0;
}

/* The Asterisk callback ABI requires a mutable audiohook pointer. */
// cppcheck-suppress constParameterCallback
/** @brief Queue eligible link voice frames and render only completed worker output.
 * @param audiohook Attached link-processing hook.
 * @param chan Asterisk channel associated with the radio or link.
 * @param frame Asterisk voice frame replaced in place by the prior processed block.
 * @param direction Asterisk audiohook stream direction.
 * @return Zero after bounded queue movement, concealment, or bypass.
 *
 * This callback deliberately owns no FFmpeg, graph, or worker mutable state.
 * It moves preallocated PCM samples through fixed SPSC queues. The hook worker
 * performs graph execution and publishes the next frame asynchronously.
 */
PROCESSING_PRIVATE int txagc_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
				      struct ast_frame *frame,
				      enum ast_audiohook_direction direction)
{
	struct txagc_hook *hook;
	unsigned int sample_rate;
	int16_t *pcm;
	unsigned int output_write;
	unsigned int generation;

	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE || frame->frametype != AST_FRAME_VOICE ||
	    !frame->data.ptr || frame->samples <= 0) {
		return 0;
	}
	/* audiohook is the first member of its datastore payload.  Recovering that
	 * owner avoids channel datastore lookup from Asterisk's audio callback. */
	hook = (struct txagc_hook *)audiohook;
	(void)chan;
	if (direction != AST_AUDIOHOOK_DIRECTION_READ ||
	    !atomic_load_explicit(&hook->link_enabled, memory_order_acquire)) {
		return 0;
	}

	sample_rate = ast_format_get_sample_rate(frame->subclass.format);
	if (!sample_rate) {
		sample_rate = 8000;
	}
	/* The graph and worker storage were prepared while attaching the hook. A
	 * format change is intentionally bypassed rather than rebuilding from this
	 * audiohook callback. Advancing the generation also prevents an older
	 * processed block from being replayed after the format returns. */
	if (sample_rate != hook->sample_rate || (size_t)frame->samples > hook->samples_capacity) {
		atomic_fetch_add_explicit(&hook->generation, 1U, memory_order_acq_rel);
		return 0;
	}
	pcm = frame->data.ptr;
	generation = atomic_load_explicit(&hook->generation, memory_order_acquire);
	/* Snapshot completed output before accepting this frame. The worker can run
	 * at any point afterward, but this callback only renders an older block,
	 * which gives link processing one fixed frame of pipeline latency. */
	output_write = atomic_load_explicit(&hook->output_frame_write, memory_order_acquire);
	link_worker_submit_input(hook, pcm, (unsigned int)frame->samples, generation);
	link_worker_consume_output(hook, pcm, (unsigned int)frame->samples, generation,
				   output_write);
	return 0;
}

/** @brief Attach a link-processing audiohook and its datastore to an Asterisk channel.
 * @param chan Asterisk channel associated with the radio or link.
 * @param profile Named channel profile associated with the hook.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
PROCESSING_PRIVATE int attach_hook(struct ast_channel *chan, const char *profile)
{
	struct ast_datastore *datastore;
	struct txagc_hook *hook;
	struct txagc_profile *configured;
	struct txagc_chain chain;
	struct ast_format *format;
	const struct txagc_avfilter *filter;
	unsigned int sample_rate;
	int profile_enabled = 0;
	int source;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
	format = ast_channel_rawreadformat(chan);
	sample_rate = format ? ast_format_get_sample_rate(format) : 0;
	ast_channel_unlock(chan);
	if (datastore) {
		return 0;
	}
	if (!sample_rate)
		sample_rate = 8000;
	memset(&chain, 0, sizeof(chain));
	ast_mutex_lock(&settings_lock);
	configured = find_profile(&settings, profile);
	if (configured) {
		chain = configured->chains[TXAGC_LINK];
		profile_enabled = configured->enabled;
	}
	ast_mutex_unlock(&settings_lock);
	if (!configured || !profile_enabled || !chain.enabled)
		return -1;

	datastore = ast_datastore_alloc(&txagc_datastore, NULL);
	hook = ast_calloc(1, sizeof(*hook));
	if (!datastore || !hook) {
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	for (source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		txagc_avfilter_slot_init(&hook->avfilter[source]);
	}
	atomic_init(&hook->link_enabled, 1);
	if (txagc_avfilter_slot_prepare(&hook->avfilter[TXAGC_LINK], &chain.agc, sample_rate) < 0) {
		for (source = 0; source < TXAGC_SOURCE_COUNT; ++source)
			txagc_avfilter_slot_destroy(&hook->avfilter[source]);
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	filter = txagc_avfilter_slot_active(&hook->avfilter[TXAGC_LINK]);
	if (!filter || !filter->input_capacity) {
		for (source = 0; source < TXAGC_SOURCE_COUNT; ++source)
			txagc_avfilter_slot_destroy(&hook->avfilter[source]);
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	if (txagc_link_worker_initialize(hook, filter->input_capacity)) {
		for (source = 0; source < TXAGC_SOURCE_COUNT; ++source)
			txagc_avfilter_slot_destroy(&hook->avfilter[source]);
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	hook->samples_capacity = filter->input_capacity;
	hook->sample_rate = sample_rate;
	ast_copy_string(hook->channel, ast_channel_name(chan), sizeof(hook->channel));
	ast_copy_string(hook->profile, profile, sizeof(hook->profile));
	if (ast_audiohook_init(&hook->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "TXAGC",
			       AST_AUDIOHOOK_MANIPULATE_ALL_RATES)) {
		for (source = 0; source < TXAGC_SOURCE_COUNT; ++source)
			txagc_avfilter_slot_destroy(&hook->avfilter[source]);
		txagc_link_worker_destroy(hook);
		ast_datastore_free(datastore);
		ast_free(hook);
		return -1;
	}
	if (txagc_link_worker_start(hook)) {
		ast_audiohook_destroy(&hook->audiohook);
		for (source = 0; source < TXAGC_SOURCE_COUNT; ++source)
			txagc_avfilter_slot_destroy(&hook->avfilter[source]);
		txagc_link_worker_destroy(hook);
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

/** One prepared replacement for an attached link callback. */
struct link_graph_plan {
	/** Retained channel keeps the datastore and hook alive through publication. */
	struct ast_channel *channel;
	/** Audiohook protected by its control-plane lock while the plan is live. */
	struct txagc_hook *hook;
	/** Graph candidate prepared outside the audio callback. */
	struct txagc_avfilter_slot_candidate candidate;
	/** Whether the candidate profile admits link processing after publication. */
	int enabled;
};

/** All active link candidates for one private settings generation. */
struct link_graph_transaction {
	/** Locked and retained plans, released by publish or discard. */
	struct link_graph_plan *plans;
	/** Number of prepared plans. */
	size_t count;
};

/** @brief Release an uncommitted or committed link graph transaction.
 * @param transaction Transaction to release; NULL is accepted.
 */
PROCESSING_PRIVATE void discard_link_graph_transaction(struct link_graph_transaction *transaction)
{
	size_t index;

	if (!transaction)
		return;
	for (index = 0; index < transaction->count; ++index) {
		struct link_graph_plan *plan = &transaction->plans[index];

		txagc_avfilter_slot_candidate_destroy(&plan->candidate);
		ast_audiohook_unlock(&plan->hook->audiohook);
		ast_channel_unref(plan->channel);
	}
	ast_free(transaction->plans);
	ast_free(transaction);
}

/** @brief Stage every active link hook against one private settings generation.
 * @param candidate Validated settings that are not yet visible to callbacks.
 * @param result Receives the all-or-nothing prepared transaction.
 * @return Zero on success; nonzero without changing any callback state on failure.
 */
PROCESSING_PRIVATE int stage_active_link_hooks(const struct txagc_settings *candidate,
					       struct link_graph_transaction **result)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *channel;
	struct link_graph_transaction *transaction;
	size_t capacity = 0;

	if (!candidate || !result)
		return -1;
	*result = NULL;
	iterator = ast_channel_iterator_all_new();
	/* Before the scanner starts, no link hook can exist. A startup/prime reload
	 * may therefore commit an empty link transaction without an iterator. Once
	 * scanning is active, failure to enumerate could hide a live hook, so reject
	 * the candidate rather than publishing a partial generation. */
	if (!iterator) {
		if (scan_thread != AST_PTHREADT_NULL)
			return -1;
		transaction = ast_calloc(1, sizeof(*transaction));
		if (!transaction)
			return -1;
		*result = transaction;
		return 0;
	}
	transaction = ast_calloc(1, sizeof(*transaction));
	if (!transaction) {
		ast_channel_iterator_destroy(iterator);
		return -1;
	}
	while ((channel = ast_channel_iterator_next(iterator))) {
		const struct ast_datastore *datastore;
		struct link_graph_plan *plan;
		const struct txagc_profile *profile;

		ast_channel_lock(channel);
		datastore = ast_channel_datastore_find(channel, &txagc_datastore, NULL);
		if (!datastore || !datastore->data) {
			ast_channel_unlock(channel);
			ast_channel_unref(channel);
			continue;
		}
		if (transaction->count == capacity) {
			size_t new_capacity = capacity ? capacity * 2U : 8U;
			struct link_graph_plan *expanded =
				ast_realloc(transaction->plans, new_capacity * sizeof(*expanded));

			if (!expanded) {
				ast_channel_unlock(channel);
				ast_channel_unref(channel);
				ast_channel_iterator_destroy(iterator);
				discard_link_graph_transaction(transaction);
				return -1;
			}
			transaction->plans = expanded;
			capacity = new_capacity;
		}
		plan = &transaction->plans[transaction->count];
		memset(plan, 0, sizeof(*plan));
		plan->channel = channel;
		plan->hook = datastore->data;
		ast_audiohook_lock(&plan->hook->audiohook);
		ast_channel_unlock(channel);
		profile = find_profile_const(candidate, plan->hook->profile);
		plan->enabled = profile && profile->enabled && profile->chains[TXAGC_LINK].enabled;
		/* Count the retained plan before a fallible prepare so the common
		 * discard path releases this hook and channel on every failure edge. */
		++transaction->count;
		if (plan->enabled && txagc_avfilter_slot_candidate_prepare(
					     &plan->candidate, &profile->chains[TXAGC_LINK].agc,
					     plan->hook->sample_rate) < 0) {
			ast_log(LOG_ERROR, "RadioPlus: keeping previous link graph on %s\n",
				plan->hook->channel);
			ast_channel_iterator_destroy(iterator);
			discard_link_graph_transaction(transaction);
			return -1;
		}
		/* Retain both locks through peer preparation. A later failure therefore
		 * leaves every graph and admission flag on its prior generation. */
	}
	ast_channel_iterator_destroy(iterator);
	*result = transaction;
	return 0;
}

/** @brief Publish an all-or-nothing prepared link graph transaction.
 * @param transaction Fully staged transaction; NULL is accepted.
 *
 * Candidate publication cannot fail after candidate_prepare() succeeds. Graph
 * pointers are published before link_enabled so a callback either uses the old
 * admitted graph or the complete new one, never a candidate settings snapshot.
 */
PROCESSING_PRIVATE void publish_link_graph_transaction(struct link_graph_transaction *transaction)
{
	size_t index;

	if (!transaction)
		return;
	for (index = 0; index < transaction->count; ++index) {
		struct link_graph_plan *plan = &transaction->plans[index];

		if (plan->enabled) {
			if (txagc_avfilter_slot_publish_candidate(&plan->hook->avfilter[TXAGC_LINK],
								  &plan->candidate) < 0) {
				/* A validated candidate is always publishable. This path protects
				 * against an internal slot invariant failure without touching the
				 * callback; retain the old admission state in that impossible case.
				 */
				ast_log(LOG_ERROR,
					"RadioPlus: unable to publish link graph on %s\n",
					plan->hook->channel);
				continue;
			}
		}
		/* A worker result is paired with the generation captured at admission.
		 * Invalidate queued output before changing admission so a disabled or
		 * reconfigured link never replays an older graph block on its next frame. */
		atomic_fetch_add_explicit(&plan->hook->generation, 1U, memory_order_acq_rel);
		atomic_store_explicit(&plan->hook->link_enabled, plan->enabled,
				      memory_order_release);
	}
	discard_link_graph_transaction(transaction);
}

/** @brief Discover eligible link channels without holding settings_lock during Asterisk lookup. */
PROCESSING_PRIVATE void scan_channels(void)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;
	struct txagc_profile *profile;
	int profile_found = 0;
	size_t i;

	profile = ast_calloc(1, sizeof(*profile));
	if (!profile)
		return;
	/* A link audiohook belongs to the first enabled radio whose primary channel
	 * is active. app_rpt exposes no node identifier on its Remote Rx channel.
	 * Copy each candidate while holding settings_lock, then release it before
	 * entering Asterisk's channel container to preserve the global lock order. */
	for (i = 0;; ++i) {
		struct ast_channel *primary;
		ast_mutex_lock(&settings_lock);
		if (i >= settings.profile_count) {
			ast_mutex_unlock(&settings_lock);
			break;
		}
		*profile = settings.profiles[i];
		ast_mutex_unlock(&settings_lock);
		if (!profile->enabled)
			continue;
		primary = ast_channel_get_by_name(profile->channel);
		if (primary) {
			ast_channel_unref(primary);
			profile_found = 1;
			break;
		}
	}
	if (!profile_found) {
		ast_free(profile);
		return;
	}
	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		ast_free(profile);
		return;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		ast_channel_lock(chan);
		if (channel_is_eligible(chan, profile)) {
			ast_channel_unlock(chan);
			attach_hook(chan, profile->name);
		} else {
			ast_channel_unlock(chan);
		}
		ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
	ast_free(profile);
}

/** @brief Remove processing hooks from all channels during shutdown. */
PROCESSING_PRIVATE void detach_all(void)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;

	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		return;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		struct ast_datastore *datastore;

		ast_channel_lock(chan);
		datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
		if (datastore) {
			ast_channel_datastore_remove(chan, datastore);
		}
		ast_channel_unlock(chan);
		if (datastore) {
			ast_datastore_free(datastore);
		}
		ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
}

/** @brief Periodically attach processing hooks until module shutdown is requested.
 * @param unused Unused POSIX thread argument.
 * @return NULL after the scanner exits.
 */
PROCESSING_PRIVATE void *scanner(void *unused)
{
	(void)unused;
	while (!stopping) {
		scan_channels();
		usleep(SCAN_INTERVAL_US);
	}
	return NULL;
}

/** @brief Report resolved per-channel hardware and processing settings.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
PROCESSING_PRIVATE char *cli_show(struct ast_cli_entry *entry, int command,
				  struct ast_cli_args *args)
{
	struct txagc_profile current;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing show";
		entry->usage = "Usage: radioplus processing show\n       Show RadioPlus processing "
			       "configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	ast_mutex_lock(&settings_lock);
	if (!settings.profile_count) {
		ast_mutex_unlock(&settings_lock);
		ast_cli(args->fd, "No named RadioPlus channels are configured.\n");
		return CLI_SUCCESS;
	}
	current = settings.profiles[0];
	ast_mutex_unlock(&settings_lock);
	for (int source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		struct txagc_chain *chain = &current.chains[source];
		ast_cli(args->fd,
			"Chain %s: %s, RNNoise %s, AGC %s, expander %s, "
			"compressor %s (%d band), limiter %s (%d band), ",
			source_names[source], chain->enabled ? "enabled" : "disabled",
			chain->rnnoise_enabled ? "enabled" : "disabled",
			chain->agc.agc_enabled ? "enabled" : "disabled",
			chain->agc.expander_enabled ? "enabled" : "disabled",
			chain->agc.compressor_enabled ? "enabled" : "disabled",
			chain->agc.compressor_bands,
			chain->agc.limiter_enabled ? "enabled" : "disabled",
			chain->agc.limiter_bands);
		if (source == TXAGC_LOCAL)
			ast_cli(args->fd, "receive brick-wall band-pass %s, ",
				chain->agc.receive_bandpass_enabled ? "enabled" : "disabled");
		ast_cli(args->fd, "equalizer %s, ",
			chain->agc.equalizer_enabled ? "enabled" : "disabled");
		ast_cli(args->fd, "de-esser %s, ",
			chain->agc.deesser_enabled ? "enabled" : "disabled");
		if (source == TXAGC_VOICE_TELEMETRY)
			ast_cli(args->fd, "brick-wall band-pass %s, ",
				chain->agc.splatter_filter_enabled ? "enabled" : "disabled");
		ast_cli(args->fd, "final limiter %s, input gain %.1f dB, output gain %.1f dB\n",
			chain->agc.lookahead_limiter_enabled ? "enabled" : "disabled",
			chain->agc.input_gain_db, chain->agc.output_gain_db);
		ast_cli(args->fd,
			"  Three-band crossovers: compressor %.1f/%.1f Hz, limiter %.1f/%.1f Hz\n",
			chain->agc.compressor_low_crossover_hz,
			chain->agc.compressor_high_crossover_hz,
			chain->agc.limiter_low_crossover_hz, chain->agc.limiter_high_crossover_hz);
		ast_cli(args->fd,
			"  Compressor low: threshold %.1f dBFS, ratio %.1f:1, makeup %.1f dB, "
			"knee %.1f dB, attack %.1f ms, release %.1f ms\n",
			chain->agc.compressor_low_threshold_dbfs, chain->agc.compressor_low_ratio,
			chain->agc.compressor_low_makeup_gain_db, chain->agc.compressor_low_knee_db,
			chain->agc.compressor_low_attack_ms, chain->agc.compressor_low_release_ms);
		ast_cli(args->fd,
			"  Compressor mid: threshold %.1f dBFS, ratio %.1f:1, makeup %.1f dB, "
			"knee %.1f dB, attack %.1f ms, release %.1f ms\n",
			chain->agc.compressor_mid_threshold_dbfs, chain->agc.compressor_mid_ratio,
			chain->agc.compressor_mid_makeup_gain_db, chain->agc.compressor_mid_knee_db,
			chain->agc.compressor_mid_attack_ms, chain->agc.compressor_mid_release_ms);
		ast_cli(args->fd,
			"  Compressor high: threshold %.1f dBFS, ratio %.1f:1, makeup %.1f dB, "
			"knee %.1f dB, attack %.1f ms, release %.1f ms\n",
			chain->agc.compressor_high_threshold_dbfs, chain->agc.compressor_high_ratio,
			chain->agc.compressor_high_makeup_gain_db,
			chain->agc.compressor_high_knee_db, chain->agc.compressor_high_attack_ms,
			chain->agc.compressor_high_release_ms);
		ast_cli(args->fd,
			"  Single-band limiter: threshold %.1f dBFS, ratio %.1f:1, knee %.1f dB, "
			"attack %.1f ms, release %.1f ms\n",
			chain->agc.limiter_threshold_dbfs, chain->agc.limiter_ratio,
			chain->agc.limiter_knee_db, chain->agc.limiter_attack_ms,
			chain->agc.limiter_release_ms);
	}
	ast_cli(args->fd, "\nDetailed local-chain settings:\n");
	ast_cli(args->fd,
		"Enabled: %s\nLocal receiver: %s\nLinked audio: %s\n"
		"Channel: %s\nRNNoise: %s\nReceive band-pass: %s (%.0f-%.0f Hz)\n"
		"PL filter: %s\n"
		"AGC stage: %s\nDetector RMS target: %.1f dBFS\nMax gain: %.1f dB\n"
		"Max attenuation: %.1f dB\nRMS averaging: %.0f ms\n"
		"Gain increase rate: %.1f dB/s\nGain decrease rate: %.1f dB/s\n"
		"Activity threshold: %.1f dBFS\nActivity hysteresis: %.1f dB\n"
		"Gain-increase hold: %.0f ms\nTarget deadband: %.1f dB\n"
		"Sidechain high-pass/low-pass: %.0f/%.0f Hz (0 disables an edge)\n"
		"Downward expander: %s\nExpander threshold: %.1f dBFS\nExpander ratio: %.1f:1\n"
		"Expander maximum attenuation: %.1f dB\nExpander attack: %.0f ms\n"
		"Expander release: %.0f ms\nExpander sidechain band-pass: %.0f-%.0f Hz\n"
		"Compressor: %s\nSingle-band compressor threshold: %.1f dBFS\nCompressor ratio: "
		"%.1f:1\n"
		"Compressor make-up gain: %.1f dB\nCompressor attack: %.0f ms\n"
		"Compressor release: %.0f ms\nCompressor sidechain band-pass: %.0f-%.0f Hz\n"
		"Limiter: %s\nBrick-wall band-pass: %s\nThree-band limiter crossovers: %.0f/%.0f "
		"Hz\nLow-band threshold: %.1f dBFS\n"
		"Low-band ratio: %.1f:1\nLow-band knee: %.1f dB\n"
		"Low-band attack: %.1f ms\nLow-band release: %.0f ms\n"
		"Mid-band threshold: %.1f dBFS\nMid-band ratio: %.1f:1\nMid-band knee: %.1f dB\n"
		"Mid-band attack: %.1f ms\nMid-band release: %.1f ms\n"
		"High-band limit: %.1f dBFS\nHigh-band ratio: %.1f:1\nHigh-band knee: %.1f dB\n"
		"High-band attack: %.1f ms\nHigh-band release: %.1f ms\n"
		"Final limiter: %s\nFinal-limiter ceiling: %.1f dBFS\nLookahead: %.1f ms\n"
		"Final-limiter attack: %.1f ms\nFinal-limiter release: %.0f ms\nOutput band-pass: "
		"%.0f-%.0f Hz\n"
		"Final output gain: %.1f dB\n",
		current.enabled ? "yes" : "no", current.local_enabled ? "enabled" : "disabled",
		current.link_enabled ? "enabled" : "disabled", current.channel,
		current.rnnoise_enabled ? "enabled" : "disabled",
		current.agc.receive_bandpass_enabled ? "enabled" : "disabled",
		current.agc.receive_bandpass_highpass_hz, current.agc.receive_bandpass_lowpass_hz,
		ctcss_filter_name(current.agc.ctcss_filter_mode),
		current.agc.agc_enabled ? "enabled" : "disabled", current.agc.target_dbfs,
		current.agc.max_gain_db, current.agc.max_attenuation_db,
		current.agc.agc_rms_averaging_ms, current.agc.agc_gain_increase_db_per_second,
		current.agc.agc_gain_decrease_db_per_second,
		current.agc.agc_activity_threshold_dbfs, current.agc.agc_activity_hysteresis_db,
		current.agc.agc_hold_ms, current.agc.agc_deadband_db,
		current.agc.sidechain_highpass_hz, current.agc.sidechain_lowpass_hz,
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
		current.agc.limiter_low_crossover_hz, current.agc.limiter_high_crossover_hz,
		current.agc.low_limiter_threshold_dbfs, current.agc.low_limiter_ratio,
		current.agc.low_limiter_knee_db, current.agc.low_limiter_attack_ms,
		current.agc.low_limiter_release_ms, current.agc.mid_limiter_threshold_dbfs,
		current.agc.mid_limiter_ratio, current.agc.mid_limiter_knee_db,
		current.agc.mid_limiter_attack_ms, current.agc.mid_limiter_release_ms,
		current.agc.high_limiter_threshold_dbfs, current.agc.high_limiter_ratio,
		current.agc.high_limiter_knee_db, current.agc.high_limiter_attack_ms,
		current.agc.high_limiter_release_ms,
		current.agc.lookahead_limiter_enabled ? "enabled" : "disabled",
		current.agc.lookahead_limit_dbfs, current.agc.lookahead_ms,
		current.agc.lookahead_attack_ms, current.agc.lookahead_release_ms,
		current.agc.output_highpass_hz, current.agc.output_lowpass_hz,
		current.agc.output_gain_db);
	return CLI_SUCCESS;
}

/** @brief Report input, output, and filtering measurements for active processing hooks.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
PROCESSING_PRIVATE char *cli_stats(struct ast_cli_entry *entry, int command,
				   struct ast_cli_args *args)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *chan;
	int found = 0;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing stats";
		entry->usage = "Usage: radioplus processing stats\n       Show live RadioPlus "
			       "processing measurements.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		return CLI_FAILURE;
	}
	while ((chan = ast_channel_iterator_next(iterator))) {
		const struct ast_datastore *datastore;
		struct txagc_hook *hook;

		ast_channel_lock(chan);
		datastore = ast_channel_datastore_find(chan, &txagc_datastore, NULL);
		hook = datastore ? datastore->data : NULL;
		if (hook) {
			struct txagc_link_worker_statistics statistics;

			if (!txagc_link_worker_statistics_read(hook, &statistics)) {
				ast_cli(args->fd,
					"%s/link: input peak %.1f dBFS RMS %.1f dBFS; "
					"output peak %.1f dBFS RMS %.1f dBFS; max peak %.1f dBFS; "
					"input %llu output %llu startup fill %llu runtime underrun "
					"%llu samples; worker input overflow %llu malformed input "
					"%llu "
					"output underflow %llu "
					"malformed output %llu blocks\n",
					hook->channel, statistics.input_peak_dbfs,
					statistics.input_rms_dbfs, statistics.output_peak_dbfs,
					statistics.output_rms_dbfs, statistics.output_max_peak_dbfs,
					statistics.input_samples, statistics.output_samples,
					statistics.startup_fill_samples,
					statistics.runtime_underrun_samples,
					statistics.input_overflows, statistics.input_malformed,
					statistics.output_underflows, statistics.output_malformed);
			}
			found = 1;
		}
		ast_channel_unlock(chan);
		ast_channel_unref(chan);
	}
	ast_channel_iterator_destroy(iterator);
	if (!found) {
		ast_cli(args->fd, "No RadioPlus processing hook is currently attached.\n");
	}
	return CLI_SUCCESS;
}

/** @brief Enable a named processing source in the live settings snapshot.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
PROCESSING_PRIVATE char *cli_enable(struct ast_cli_entry *entry, int command,
				    struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing enable";
		entry->usage = "Usage: radioplus processing enable\n       Enable RadioPlus "
			       "processing until reload or restart.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	{
		struct txagc_audio_snapshot *snapshot = allocate_audio_snapshot();

		if (!snapshot) {
			ast_cli(args->fd, "Unable to publish RadioPlus processing settings.\n");
			return CLI_FAILURE;
		}
		ast_mutex_lock(&settings_lock);
		for (size_t i = 0; i < settings.profile_count; ++i)
			settings.profiles[i].enabled = 1;
		publish_audio_settings_locked(snapshot);
		ast_mutex_unlock(&settings_lock);
	}
	scan_channels();
	ast_cli(args->fd, "RadioPlus processing enabled.\n");
	return CLI_SUCCESS;
}

/** @brief Disable a named processing source in the live settings snapshot.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
PROCESSING_PRIVATE char *cli_disable(struct ast_cli_entry *entry, int command,
				     struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing disable";
		entry->usage = "Usage: radioplus processing disable\n       Disable RadioPlus "
			       "processing and remove active hooks.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	{
		struct txagc_audio_snapshot *snapshot = allocate_audio_snapshot();

		if (!snapshot) {
			ast_cli(args->fd, "Unable to publish RadioPlus processing settings.\n");
			return CLI_FAILURE;
		}
		ast_mutex_lock(&settings_lock);
		for (size_t i = 0; i < settings.profile_count; ++i)
			settings.profiles[i].enabled = 0;
		publish_audio_settings_locked(snapshot);
		ast_mutex_unlock(&settings_lock);
	}
	detach_all();
	ast_cli(args->fd, "RadioPlus processing disabled and detached.\n");
	return CLI_SUCCESS;
}

/** @brief Reload and validate the unified configuration through the Asterisk CLI.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
PROCESSING_PRIVATE char *cli_reload(struct ast_cli_entry *entry, int command,
				    struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing reload";
		entry->usage = "Usage: radioplus processing reload\n       Reload "
			       "usbradioplus.conf.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	if (usbradioplus_processing_reload()) {
		ast_cli(args->fd, "RadioPlus processing configuration reload failed.\n");
		return CLI_FAILURE;
	}
	ast_cli(args->fd, "RadioPlus processing configuration reloaded in place.\n");
	return CLI_SUCCESS;
}

/** Asterisk processing CLI command table. */
static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_show, "Show RadioPlus processing configuration"),
	AST_CLI_DEFINE(cli_stats, "Show RadioPlus processing statistics"),
	AST_CLI_DEFINE(cli_enable, "Enable RadioPlus processing"),
	AST_CLI_DEFINE(cli_disable, "Disable RadioPlus processing"),
	AST_CLI_DEFINE(cli_reload, "Reload RadioPlus processing configuration"),
};

int usbradioplus_processing_get_local(const char *channel, struct txagc_chain *chain)
{
	const struct txagc_profile *profile;
	if (!chain)
		return -1;
	/* A removed or unmatched profile must not leave callers with stale DSP state. */
	memset(chain, 0, sizeof(*chain));
	/* Native graph staging runs on the reload control plane against a private
	 * candidate. It must not publish that candidate merely to resolve a chain. */
	if (staged_settings) {
		profile = find_profile_const(staged_settings, channel);
		if (profile) {
			*chain = profile->chains[TXAGC_LOCAL];
			chain->enabled = chain->enabled && profile->enabled;
		}
		return profile ? 0 : 1;
	}
	ast_mutex_lock(&settings_lock);
	profile = find_profile(&settings, channel);
	if (profile) {
		*chain = profile->chains[TXAGC_LOCAL];
		chain->enabled = chain->enabled && profile->enabled;
	}
	ast_mutex_unlock(&settings_lock);
	return profile ? 0 : 1;
}

int usbradioplus_processing_get_local_rt(const char *channel, struct txagc_chain *chain)
{
	const struct txagc_profile *profile;

	if (!chain)
		return -1;
	memset(chain, 0, sizeof(*chain));
	{
		const struct txagc_audio_snapshot *snapshot =
			atomic_load_explicit(&audio_settings, memory_order_acquire);
		const struct txagc_settings *current_settings =
			snapshot ? &snapshot->settings : &settings;

		profile = find_profile_const(current_settings, channel);
	}
	if (!profile)
		return 1;
	*chain = profile->chains[TXAGC_LOCAL];
	chain->enabled = chain->enabled && profile->enabled;
	return 0;
}

int usbradioplus_processing_get_hardware(const char *channel,
					 struct usbradioplus_hardware_settings *hardware)
{
	const struct txagc_profile *profile;
	if (!hardware)
		return -1;
	/* Empty assignments and tone strings are safe even when a caller has no profile. */
	memset(hardware, 0, sizeof(*hardware));
	/* Reload staging reads only its thread-private candidate.  Taking the live
	 * settings mutex here would both select stale values and defeat the
	 * transaction's all-or-nothing graph/configuration preparation. */
	if (staged_settings) {
		profile = find_profile_const(staged_settings, channel);
		if (profile)
			*hardware = profile->hardware;
		return profile ? 0 : 1;
	}
	ast_mutex_lock(&settings_lock);
	profile = find_profile_const(&settings, channel);
	if (profile)
		*hardware = profile->hardware;
	ast_mutex_unlock(&settings_lock);
	return profile ? 0 : 1;
}

int usbradioplus_processing_get_hardware_rt(const char *channel,
					    struct usbradioplus_hardware_settings *hardware)
{
	const struct txagc_profile *profile;

	if (!hardware)
		return -1;
	memset(hardware, 0, sizeof(*hardware));
	{
		const struct txagc_audio_snapshot *snapshot =
			atomic_load_explicit(&audio_settings, memory_order_acquire);
		const struct txagc_settings *current_settings =
			snapshot ? &snapshot->settings : &settings;

		profile = find_profile_const(current_settings, channel);
	}
	if (!profile)
		return 1;
	*hardware = profile->hardware;
	return 0;
}

int usbradioplus_processing_get_option(const char *channel, const char *section, const char *name,
				       char *value, size_t value_size)
{
	const char *default_value;
	size_t i;
	const struct txagc_profile *profile;
	if (!channel || !section || !name || !value || !value_size)
		return -1;
	/* Candidate-only lookup lets validation and native graph staging see one
	 * prospective configuration without publishing it to callbacks. */
	if (staged_settings) {
		profile = find_profile_const(staged_settings, channel);
		for (i = profile ? profile->override_count : 0; i > 0; --i) {
			const struct section_override *entry = &profile->overrides[i - 1];

			if (!strcasecmp(entry->section, section) &&
			    !strcasecmp(entry->name, name)) {
				ast_copy_string(value, entry->value, value_size);
				return 0;
			}
		}
		default_value = profile ? clean_slate_option_default(section, name) : NULL;
		if (default_value) {
			ast_copy_string(value, default_value, value_size);
			return 0;
		}
		return 1;
	}
	ast_mutex_lock(&settings_lock);
	profile = find_profile_const(&settings, channel);
	/* A scoped profile is appended after its flat profile values. Search backward
	 * so an explicit scoped value takes precedence without duplicating defaults. */
	for (i = profile ? profile->override_count : 0; i > 0; --i) {
		const struct section_override *entry = &profile->overrides[i - 1];

		if (!strcasecmp(entry->section, section) && !strcasecmp(entry->name, name)) {
			ast_copy_string(value, entry->value, value_size);
			ast_mutex_unlock(&settings_lock);
			return 0;
		}
	}
	default_value = profile ? clean_slate_option_default(section, name) : NULL;
	if (default_value) {
		ast_copy_string(value, default_value, value_size);
		ast_mutex_unlock(&settings_lock);
		return 0;
	}
	ast_mutex_unlock(&settings_lock);
	return 1;
}

int usbradioplus_processing_set_local_input_gain(const char *channel, double gain_db)
{
	struct txagc_profile *profile;
	struct txagc_audio_snapshot *snapshot;
	if (!isfinite(gain_db) || gain_db < -30.0 || gain_db > 30.0)
		return -1;
	snapshot = allocate_audio_snapshot();
	if (!snapshot)
		return -1;
	ast_mutex_lock(&settings_lock);
	profile = find_profile(&settings, channel);
	if (profile) {
		profile->chains[TXAGC_LOCAL].agc.input_gain_db = gain_db;
		profile->chains[TXAGC_LOCAL].input_gain_configured = 1;
		profile->agc.input_gain_db = gain_db;
		publish_audio_settings_locked(snapshot);
	} else {
		ast_free(snapshot);
	}
	ast_mutex_unlock(&settings_lock);
	return profile ? 0 : 1;
}

int usbradioplus_processing_set_hardware_input_gain(const char *channel, double gain_db)
{
	struct txagc_profile *profile;
	struct txagc_audio_snapshot *snapshot;
	if (!isfinite(gain_db) || gain_db < -30.0 || gain_db > 30.0)
		return -1;
	snapshot = allocate_audio_snapshot();
	if (!snapshot)
		return -1;
	ast_mutex_lock(&settings_lock);
	profile = find_profile(&settings, channel);
	if (profile) {
		profile->hardware.input_gain_db = gain_db;
		profile->hardware.input_gain_configured = 1;
		publish_audio_settings_locked(snapshot);
	} else {
		ast_free(snapshot);
	}
	ast_mutex_unlock(&settings_lock);
	return profile ? 0 : 1;
}

int usbradioplus_processing_save_options(const char *channel,
					 const struct usbradioplus_config_update *updates,
					 size_t update_count)
{
	struct ast_flags flags = {CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE};
	struct ast_config *cfg;
	size_t i;

	if (!channel || (!updates && update_count))
		return -1;
	cfg = ast_config_load2(CONFIG_FILE, "chan_usbradioplus", flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID)
		return -1;
	for (i = 0; i < update_count; ++i) {
		struct ast_category *category;
		char section[MAX_CONFIG_SECTION];
		if (!updates[i].section || !updates[i].name || !updates[i].value ||
		    resolve_profile_section(cfg, channel, updates[i].section, section,
					    sizeof(section)))
			goto invalid;
		category = ast_category_get(cfg, section, NULL);
		if (!category) {
			category = ast_category_new(section, CONFIG_FILE, -1);
			if (!category)
				goto invalid;
			ast_category_append(cfg, category);
		}
		if (usbradioplus_config_variable_update(cfg, CONFIG_FILE, category, updates[i].name,
							updates[i].value))
			goto invalid;
	}
	if (ast_config_text_file_save2(CONFIG_FILE, cfg, "chan_usbradioplus", 0))
		goto invalid;
	ast_config_destroy(cfg);
	return 0;
invalid:
	ast_config_destroy(cfg);
	return -1;
}

int usbradioplus_processing_save_input_gains(const char *channel, double hardware_gain_db,
					     double local_gain_db)
{
	struct usbradioplus_config_update updates[2];
	char values[2][32];

	if (!isfinite(hardware_gain_db) || hardware_gain_db < -30.0 || hardware_gain_db > 30.0 ||
	    !isfinite(local_gain_db) || local_gain_db < -30.0 || local_gain_db > 30.0)
		return -1;
	snprintf(values[0], sizeof(values[0]), "%.3f", hardware_gain_db);
	snprintf(values[1], sizeof(values[1]), "%.3f", local_gain_db);
	updates[0] = (struct usbradioplus_config_update){"hardware", "hardware_input_gain_db",
							 values[0]};
	updates[1] = (struct usbradioplus_config_update){"local", "input_gain_db", values[1]};
	return usbradioplus_processing_save_options(channel, updates, ARRAY_LEN(updates));
}

int usbradioplus_processing_get_composite(const char *channel, struct txagc_chain *chain)
{
	const struct txagc_profile *profile;
	if (!chain)
		return -1;
	memset(chain, 0, sizeof(*chain));
	/* See usbradioplus_processing_get_local(): a reload builds the native
	 * generation from the private candidate instead of exposing it early. */
	if (staged_settings) {
		profile = find_profile_const(staged_settings, channel);
		if (profile) {
			*chain = profile->chains[TXAGC_VOICE_TELEMETRY];
			chain->enabled = chain->enabled && profile->enabled;
		}
		return profile ? 0 : 1;
	}
	ast_mutex_lock(&settings_lock);
	profile = find_profile(&settings, channel);
	if (profile) {
		*chain = profile->chains[TXAGC_VOICE_TELEMETRY];
		chain->enabled = chain->enabled && profile->enabled;
	}
	ast_mutex_unlock(&settings_lock);
	return profile ? 0 : 1;
}

int usbradioplus_processing_get_composite_rt(const char *channel, struct txagc_chain *chain)
{
	const struct txagc_profile *profile;

	if (!chain)
		return -1;
	memset(chain, 0, sizeof(*chain));
	{
		const struct txagc_audio_snapshot *snapshot =
			atomic_load_explicit(&audio_settings, memory_order_acquire);
		const struct txagc_settings *current_settings =
			snapshot ? &snapshot->settings : &settings;

		profile = find_profile_const(current_settings, channel);
	}
	if (!profile)
		return 1;
	*chain = profile->chains[TXAGC_VOICE_TELEMETRY];
	chain->enabled = chain->enabled && profile->enabled;
	return 0;
}

int usbradioplus_processing_unload(void)
{
	stopping = 1;
	if (scan_thread != AST_PTHREADT_NULL) {
		pthread_join(scan_thread, NULL);
		scan_thread = AST_PTHREADT_NULL;
	}
	detach_all();
	clear_audio_settings();
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));
	return 0;
}

int usbradioplus_processing_load(void)
{
	/* prime() validates before channel registration; no callback can retain its
	 * provisional snapshot when the actual processing engine starts. */
	clear_audio_settings();
	settings_defaults(&settings);
	if (load_settings()) {
		clear_audio_settings();
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries))) {
		clear_audio_settings();
		return AST_MODULE_LOAD_DECLINE;
	}
	stopping = 0;
	if (ast_pthread_create_background(&scan_thread, NULL, scanner, NULL)) {
		ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));
		clear_audio_settings();
		return AST_MODULE_LOAD_FAILURE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

int usbradioplus_processing_prime(void)
{
	clear_audio_settings();
	settings_defaults(&settings);
	return load_settings();
}

int usbradioplus_processing_reload(void)
{
	struct txagc_audio_snapshot *snapshot = NULL;
	struct usbradioplus_native_graph_transaction *native_transaction = NULL;
	struct link_graph_transaction *link_transaction = NULL;
	size_t profile_count;

	if (load_settings_candidate(&snapshot))
		return -1;
	/* This thread-local pointer is visible only to this control-plane thread.
	 * Native graph construction resolves the candidate chains without ever
	 * publishing them to a callback, scanner, or another reload thread. */
	staged_settings = &snapshot->settings;
	if (usbradioplus_stage_all_native_processing(&native_transaction) ||
	    stage_active_link_hooks(&snapshot->settings, &link_transaction) ||
	    usbradioplus_refresh_all_processing_signaling())
		goto failed;
	staged_settings = NULL;
	/* No fallible operation remains. Publish complete graph transactions before
	 * exposing the matching settings snapshot. Native ticks and link callbacks
	 * read only their graph/admission transaction, so neither sees a mixed
	 * candidate configuration during these control-plane stores. */
	usbradioplus_publish_native_processing_transaction(native_transaction);
	native_transaction = NULL;
	publish_link_graph_transaction(link_transaction);
	link_transaction = NULL;
	profile_count = snapshot->settings.profile_count;
	commit_candidate_settings(&snapshot->settings, snapshot);
	snapshot = NULL;
	(void)usbradioplus_refresh_all_processing_hardware();
	scan_channels();
	ast_log(LOG_NOTICE, "RadioPlus loaded %zu named radio configuration(s)\n", profile_count);
	return 0;

failed:
	staged_settings = NULL;
	usbradioplus_discard_native_processing_transaction(native_transaction);
	discard_link_graph_transaction(link_transaction);
	ast_free(snapshot);
	return -1;
}

/** @name File-local and build-time constants
 * @{ */
/** @def CONFIG_FILE
 * @brief Unified processing configuration filename.
 */
/** @def SCAN_INTERVAL_US
 * @brief Delay between link-hook scans in microseconds.
 */
/** @def PROCESSING_PRIVATE
 * @brief Expose processing internals only to the linked test harness.
 */
/** @def REQUIRE_FINITE
 * @brief Reject non-finite or out-of-range graph parameters.
 */
/** @def REQUIRE_BAND_RANGE
 * @brief Reject invalid dynamics controls with the offending option and accepted bounds.
 */
/** @def READ_BOOL
 * @brief Read a boolean processing option into the candidate chain.
 */
/** @} */
