/*
 * USBRadioPlus -- minimal Asterisk channel boundary
 *
 * Copyright (C) 2026 USBRadioPlus contributors
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2. See COPYING for details.
 */

/**
 * @file
 * @brief Asterisk ABI shim for the Rust-owned USBRadioPlus driver.
 *
 * Asterisk owns channel objects, frames, its DTMF detector, music on hold, and
 * module registration. Rust owns configuration, hardware, media, signaling,
 * and channel lifecycle. One Asterisk taskprocessor serializes each channel's
 * control endpoint; `write()` is the sole independent PCM producer.
 */

#include "asterisk.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rate_adjusting_pcm_ring2/rate_adjusting_pcm_ring2.h>
#include <rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h>
#include <rptadv_gpio_adapter.h>
#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>
#include <rptadv_rnnoise_adapter/rptadv_rnnoise_adapter.h>
#include <rptadv_samplerate_adapter/rptadv_samplerate_adapter.h>
#include <rptadvradio/rptadvradio.h>

#include "asterisk/abstract_jb.h"
#include "asterisk/app.h"
#include "asterisk/astobj2.h"
#include "asterisk/audiohook.h"
#include "asterisk/causes.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/datastore.h"
#include "asterisk/dsp.h"
#include "asterisk/format.h"
#include "asterisk/format_cache.h"
#include "asterisk/format_cap.h"
#include "asterisk/frame.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/musiconhold.h"
#include "asterisk/paths.h"
#include "asterisk/sem.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/utils.h"

#include "usbradioplus_asterisk.h"

/** Asterisk configuration file parsed by the Rust driver. */
#define URP_CONFIG_FILE "usbradioplus.conf"
/** Fixed app_rpt compatibility rate in hertz. */
#define URP_APP_RPT_RATE UINT32_C(8000)
/** Native rpt_advanced controller rate in hertz. */
#define URP_ADVANCED_RATE UINT32_C(48000)
/** Asterisk compatibility-frame duration in milliseconds. */
#define URP_FRAME_MILLISECONDS UINT32_C(20)
/** Largest exact-rate Asterisk link block accepted by the prepared graph. */
#define URP_LINK_MAXIMUM_FRAME_COUNT UINT32_C(960)
/** Delay between control-plane scans for newly created incoming link channels. */
#define URP_LINK_SCAN_INTERVAL_US 250000U
/** Marks one deferred transmitter intent in the high bit. */
#define URP_PENDING_TRANSMIT_VALID (UINT64_C(1) << 63)
/** Encodes the keyed state below the validity bit. */
#define URP_PENDING_TRANSMIT_KEYED (UINT64_C(1) << 32)

#ifndef URP_AGC_PLUGIN_PATH
/** Installed path of the private USBRadioPlus LADSPA AGC plugin. */
#define URP_AGC_PLUGIN_PATH "/usr/lib/usbradioplus/usbradioplus_agc.so"
#endif

/** Asterisk-side state which cannot be represented by the portable ABI. */
struct urp_channel {
	struct urp_channel *next;	       /**< Next live wrapper under channel_list_lock. */
	char *name;			       /**< Allocated configured channel name. */
	void *rust_channel;		       /**< Rust-owned channel handle. */
	struct ast_taskprocessor *control;     /**< Sole owner of Rust control operations. */
	struct ast_dsp *dsp;		       /**< Asterisk DTMF detector for app_rpt only. */
	struct ast_format *format;	       /**< Fixed Asterisk boundary format. */
	uint32_t sample_rate_hz;	       /**< Fixed boundary sample rate. */
	uint32_t frame_samples;		       /**< Samples in one Asterisk frame. */
	pthread_t delivery_thread;	       /**< Non-real-time Rust delivery worker. */
	atomic_int delivery_running;	       /**< Worker has been created and needs joining. */
	atomic_int delivery_stop;	       /**< Worker stop request. */
	atomic_int service_failed;	       /**< Rust delivery endpoint failed. */
	atomic_int jitter_pending;	       /**< Reload requires Asterisk jitter reconfigure. */
	atomic_uint_fast64_t pending_transmit; /**< Latest transmitter intent gated by reload. */
	_Atomic(struct ast_channel *) owner;   /**< Current delivery destination. */
};

/** Asterisk audiohook glue around one Rust-owned incoming-link graph. */
struct urp_link_hook {
	struct ast_audiohook audiohook; /**< First member for callback owner recovery. */
	void *rust_link;		/**< Rust-owned prepared graph and workspaces. */
	void *reload_link;		/**< Candidate graph prepared for the current reload. */
	int reload_pending;		/**< Candidate is ready, including intentional NULL. */
	atomic_uint references;		/**< Datastore plus any staged-reload ownership. */
	atomic_int attachment_state;	/**< Building, attached, or detached lifecycle state. */
	char *asterisk_channel;		/**< Channel name retained for diagnostics. */
	char *profile;			/**< Named USBRadioPlus configuration channel. */
};

/** Direct hook references used to finish a link reload across channel masquerades. */
struct urp_link_reload_entry {
	struct urp_link_reload_entry *next; /**< Next attached link captured before activation. */
	struct urp_link_hook *hook;	    /**< Retained hook whose candidate will be adopted. */
};

/** Link-hook attachment states shared with datastore teardown. */
enum urp_link_attachment_state {
	URP_LINK_BUILDING, /**< Datastore/audiohook installation has not completed. */
	URP_LINK_ATTACHED, /**< Audiohook is live and may process its current graph. */
	URP_LINK_DETACHED, /**< Datastore teardown won the lifecycle race. */
};

/** Operations serialized by one channel's Asterisk taskprocessor. */
enum urp_control_operation {
	URP_CONTROL_START,	     /**< Start the reserved Rust channel. */
	URP_CONTROL_STOP,	     /**< Stop all Rust channel activity. */
	URP_CONTROL_RELOAD_PREPARE,  /**< Prepare a candidate channel generation. */
	URP_CONTROL_RELOAD_ACTIVATE, /**< Adopt the candidate while retaining the prior one. */
	URP_CONTROL_RELOAD_FINISH,   /**< Commit or roll back a channel candidate. */
	URP_CONTROL_TEXT,	     /**< Apply one controller text message. */
	URP_CONTROL_TRANSMIT,	     /**< Apply transmitter state. */
	URP_CONTROL_DTMF,	     /**< Enable or disable DTMF detection. */
	URP_CONTROL_ECHO,	     /**< Enable or disable echo mode. */
	URP_CONTROL_JITTER,	     /**< Read resolved Asterisk jitter settings. */
	URP_CONTROL_COMMAND,	     /**< Run one typed hardware command. */
	URP_CONTROL_STATUS,	     /**< Read one typed status snapshot. */
	URP_CONTROL_SERVICE,	     /**< Drain Rust-to-Asterisk delivery. */
	URP_CONTROL_DESTROY,	     /**< Destroy the stopped Rust channel. */
};

/** Synchronous stack request submitted to a channel taskprocessor. */
struct urp_control_task {
	struct urp_channel *channel;	      /**< Target wrapper. */
	enum urp_control_operation operation; /**< Serialized operation. */
	/** Operation-specific input or output pointer. */
	union {
		/** Byte-counted controller text. */
		struct {
			const uint8_t *text; /**< Text bytes. */
			uint32_t length;     /**< Number of text bytes. */
		} text;
		/** Transmitter key state and optional forced CTCSS. */
		struct {
			uint32_t keyed;		  /**< One to key, zero to unkey. */
			uint32_t ctcss_tenths_hz; /**< Forced frequency or zero. */
		} transmit;
		uint32_t enabled;			 /**< Boolean setting. */
		struct urp_ast_jitter_config *jitter;	 /**< Jitter output. */
		struct urp_ast_channel_command *command; /**< Typed command/result. */
		struct urp_ast_channel_status *status;	 /**< Status output. */
	} argument;					 /**< Selected operation data. */
	struct ast_sem complete;			 /**< Synchronous completion. */
	int result;					 /**< Portable ABI result. */
};

/** Validated immutable Rust adapter descriptor. */
static const struct urp_ast_descriptor *rust_adapter;
/** Process-lifetime Rust driver handle. */
static void *rust_driver;
/** Monotonic suffix used to give each taskprocessor a unique name. */
static atomic_uint taskprocessor_sequence;
/** Live Asterisk channels which prevent module unload. */
static atomic_uint active_channels;
/** Live wrappers protected from CLI lookup and teardown races. */
static struct urp_channel *channel_list;
/** Lock protecting only the non-real-time live-channel list. */
AST_MUTEX_DEFINE_STATIC(channel_list_lock);
/** Freezes request/hangup membership for a complete reload transaction. */
AST_MUTEX_DEFINE_STATIC(reload_lock);
/** Prevents ordinary taskprocessor admission while a reload may roll back state. */
AST_RWLOCK_DEFINE_STATIC(reload_control_gate);
/** Serializes link scanning, reload detachment, and module teardown. */
AST_MUTEX_DEFINE_STATIC(link_scan_lock);
/** Background control-plane scanner for incoming IAX link channels. */
static pthread_t link_scan_thread;
/** Nonzero after the link scanner was created and until it is joined. */
static atomic_int link_scan_running;
/** Scanner stop request observed outside audio callbacks. */
static atomic_int link_scan_stop;

static struct ast_channel *urp_request(const char *type, struct ast_format_cap *cap,
				       const struct ast_assigned_ids *assignedids,
				       const struct ast_channel *requestor, const char *data,
				       int *cause);
static int urp_call(struct ast_channel *owner, const char *destination, int timeout);
static int urp_hangup(struct ast_channel *owner);
static int urp_answer(struct ast_channel *channel);
static struct ast_frame *urp_read(struct ast_channel *owner);
static int urp_write(struct ast_channel *owner, struct ast_frame *frame);
static int urp_send_text(struct ast_channel *owner, const char *text);
static int urp_indicate(struct ast_channel *owner, int condition, const void *data,
			size_t data_length);
static int urp_fixup(struct ast_channel *old_channel, struct ast_channel *new_channel);
static int urp_setoption(struct ast_channel *owner, int option, void *data, int data_length);
static int urp_digit_begin(struct ast_channel *channel, char digit);
static int urp_digit_end(struct ast_channel *channel, char digit, unsigned int duration);
static void urp_scan_links(void);
static void urp_detach_all_links(void);
static int urp_prepare_link_reload(struct urp_link_reload_entry **entries);
static void urp_finish_link_reload(struct urp_link_reload_entry *entries, int commit);

/** Ordinary fixed-8-kHz app_rpt channel boundary. */
static struct ast_channel_tech app_rpt_tech = {
	.type = "RadioPlus",
	.description = "USBRadioPlus app_rpt compatibility channel",
	.requester = urp_request,
	.send_digit_begin = urp_digit_begin,
	.send_digit_end = urp_digit_end,
	.send_text = urp_send_text,
	.hangup = urp_hangup,
	.answer = urp_answer,
	.read = urp_read,
	.call = urp_call,
	.write = urp_write,
	.indicate = urp_indicate,
	.fixup = urp_fixup,
	.setoption = urp_setoption,
};

/** Native fixed-48-kHz rpt_advanced channel boundary. */
static struct ast_channel_tech advanced_tech = {
	.type = "RadioPlusAdvanced",
	.description = "USBRadioPlus native rpt_advanced channel",
	.requester = urp_request,
	.send_digit_begin = urp_digit_begin,
	.send_digit_end = urp_digit_end,
	.send_text = urp_send_text,
	.hangup = urp_hangup,
	.answer = urp_answer,
	.read = urp_read,
	.call = urp_call,
	.write = urp_write,
	.indicate = urp_indicate,
	.fixup = urp_fixup,
	.setoption = urp_setoption,
};

/**
 * @brief Try to lock the current owner without waiting behind Asterisk hangup.
 * @param channel Channel wrapper whose current owner is requested.
 * @return The locked owner, or NULL when it is unavailable or changed.
 */
static struct ast_channel *urp_lock_owner(const struct urp_channel *channel)
{
	struct ast_channel *owner = atomic_load_explicit(&channel->owner, memory_order_acquire);

	if (!owner || ast_channel_trylock(owner))
		return NULL;
	if (owner != atomic_load_explicit(&channel->owner, memory_order_acquire)) {
		ast_channel_unlock(owner);
		return NULL;
	}
	return owner;
}

/**
 * @brief Queue one Rust-owned voice span through Asterisk's copying queue API.
 * @param application_context Unused process context supplied to Rust.
 * @param channel_context Channel wrapper supplied when the channel was reserved.
 * @param samples Complete signed-linear frame to copy.
 * @param sample_count Number of samples in @p samples.
 * @param sample_rate_hz Sample rate of @p samples.
 * @return Zero when queued or no owner exists, otherwise an Asterisk error.
 */
static int urp_queue_voice(void *application_context, void *channel_context, const int16_t *samples,
			   uint32_t sample_count, uint32_t sample_rate_hz)
{
	struct urp_channel *channel = channel_context;
	struct ast_channel *owner;
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.src = __PRETTY_FUNCTION__,
	};
	int result;

	(void)application_context;
	if (!channel || !samples || sample_count != channel->frame_samples ||
	    sample_rate_hz != channel->sample_rate_hz)
		return -1;
	owner = urp_lock_owner(channel);
	if (!owner)
		return 0;
	if (ast_channel_state(owner) != AST_STATE_UP) {
		ast_channel_unlock(owner);
		return 0;
	}
	frame.subclass.format = channel->format;
	frame.samples = (int)sample_count;
	frame.datalen = (int)(sample_count * sizeof(*samples));
	frame.data.ptr = (void *)samples;
	result = ast_queue_frame(owner, &frame);
	ast_channel_unlock(owner);
	return result;
}

/**
 * @brief Queue a receiver, DTMF, or null event translated from the portable ABI.
 * @param application_context Unused process context supplied to Rust.
 * @param channel_context Channel wrapper supplied when the channel was reserved.
 * @param kind Portable control-event identifier.
 * @param value Event payload, such as a digit or CTCSS frequency.
 * @param duration_ms DTMF duration in milliseconds.
 * @return Zero when queued or no owner exists, otherwise an Asterisk error.
 */
static int urp_queue_control(void *application_context, void *channel_context, uint32_t kind,
			     int32_t value, uint64_t duration_ms)
{
	const struct urp_channel *channel = channel_context;
	struct ast_channel *owner;
	struct ast_frame frame = {
		.src = __PRETTY_FUNCTION__,
	};
	char tone[24];
	int result;

	(void)application_context;
	if (!channel)
		return -1;
	owner = urp_lock_owner(channel);
	if (!owner)
		return 0;
	switch (kind) {
	case URP_AST_CONTROL_RECEIVER_KEY:
		frame.frametype = AST_FRAME_CONTROL;
		frame.subclass.integer = AST_CONTROL_RADIO_KEY;
		if (value > 0) {
			snprintf(tone, sizeof(tone), "%" PRId32 ".%" PRId32, value / 10,
				 value % 10);
			frame.data.ptr = tone;
			frame.datalen = strlen(tone) + 1U;
		}
		break;
	case URP_AST_CONTROL_RECEIVER_UNKEY:
		frame.frametype = AST_FRAME_CONTROL;
		frame.subclass.integer = AST_CONTROL_RADIO_UNKEY;
		break;
	case URP_AST_CONTROL_DTMF_BEGIN:
	case URP_AST_CONTROL_DTMF_END:
		if (value <= 0 || value > UINT8_MAX) {
			ast_channel_unlock(owner);
			return -1;
		}
		frame.frametype = kind == URP_AST_CONTROL_DTMF_BEGIN ? AST_FRAME_DTMF_BEGIN
								     : AST_FRAME_DTMF_END;
		frame.subclass.integer = value;
		frame.len = duration_ms > LONG_MAX ? LONG_MAX : (long)duration_ms;
		break;
	case URP_AST_CONTROL_NULL:
		frame = ast_null_frame;
		break;
	default:
		ast_channel_unlock(owner);
		return -1;
	}
	result = ast_queue_frame(owner, &frame);
	ast_channel_unlock(owner);
	return result;
}

/**
 * @brief Queue one byte-counted text event; `ast_queue_frame()` copies its storage.
 * @param application_context Unused process context supplied to Rust.
 * @param channel_context Channel wrapper supplied when the channel was reserved.
 * @param text Byte-counted text payload.
 * @param text_length Number of bytes in @p text.
 * @return Zero when queued or no owner exists, otherwise an Asterisk error.
 */
static int urp_queue_text(void *application_context, void *channel_context, const uint8_t *text,
			  uint32_t text_length)
{
	const struct urp_channel *channel = channel_context;
	struct ast_channel *owner;
	struct ast_frame frame = {
		.frametype = AST_FRAME_TEXT,
		.src = __PRETTY_FUNCTION__,
	};
	char *terminated;
	int result;

	(void)application_context;
	if (!channel || (!text && text_length) || text_length > INT_MAX - 1U)
		return -1;
	owner = urp_lock_owner(channel);
	if (!owner)
		return 0;
	terminated = ast_malloc((size_t)text_length + 1U);
	if (!terminated) {
		ast_channel_unlock(owner);
		return -1;
	}
	if (text_length)
		memcpy(terminated, text, text_length);
	terminated[text_length] = '\0';
	frame.data.ptr = terminated;
	frame.datalen = (int)text_length + 1;
	result = ast_queue_frame(owner, &frame);
	ast_free(terminated);
	ast_channel_unlock(owner);
	return result;
}

/**
 * @brief Run Asterisk's retained app_rpt DTMF detector on mutable PCM.
 * @param application_context Unused process context supplied to Rust.
 * @param channel_context Channel wrapper supplied when the channel was reserved.
 * @param samples Mutable signed-linear app_rpt frame.
 * @param sample_count Number of samples in @p samples.
 * @param sample_rate_hz Sample rate of @p samples.
 * @param result Portable event result filled when a digit edge is detected.
 * @return One for a digit event, zero for none, or -1 on invalid input/failure.
 */
static int urp_analyze_dtmf(void *application_context, void *channel_context, int16_t *samples,
			    uint32_t sample_count, uint32_t sample_rate_hz,
			    struct urp_ast_dtmf_result *result)
{
	struct urp_channel *channel = channel_context;
	struct ast_channel *owner;
	struct ast_frame input = {
		.frametype = AST_FRAME_VOICE,
		.src = __PRETTY_FUNCTION__,
	};
	struct ast_frame *detected;
	int event = 0;

	(void)application_context;
	if (!channel || !channel->dsp || !samples || !result ||
	    result->struct_size < sizeof(*result) || sample_count != channel->frame_samples ||
	    sample_rate_hz != URP_APP_RPT_RATE)
		return -1;
	owner = urp_lock_owner(channel);
	if (!owner)
		return 0;
	input.subclass.format = channel->format;
	input.samples = (int)sample_count;
	input.datalen = (int)(sample_count * sizeof(*samples));
	input.data.ptr = samples;
	detected = ast_dsp_process(owner, channel->dsp, &input);
	if (!detected) {
		ast_channel_unlock(owner);
		return -1;
	}
	if (detected->frametype == AST_FRAME_DTMF_BEGIN ||
	    detected->frametype == AST_FRAME_DTMF_END) {
		result->event_kind = detected->frametype == AST_FRAME_DTMF_BEGIN
					     ? URP_AST_DTMF_BEGIN
					     : URP_AST_DTMF_END;
		result->digit = (uint8_t)detected->subclass.integer;
		event = 1;
	}
	if (detected != &input)
		ast_frfree(detected);
	ast_channel_unlock(owner);
	return event;
}

/**
 * @brief Return monotonic milliseconds for DTMF duration accounting.
 * @param application_context Unused process context supplied to Rust.
 * @return Monotonic milliseconds, or zero when the host clock call fails.
 */
static uint64_t urp_monotonic_milliseconds(void *application_context)
{
	struct timespec now = {0};

	(void)application_context;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

/**
 * @brief Route a bounded Rust diagnostic through Asterisk's logger.
 * @param application_context Unused process context supplied to Rust.
 * @param level Portable log severity.
 * @param message Byte-counted message.
 * @param message_length Number of bytes in @p message.
 */
static void urp_log(void *application_context, uint32_t level, const uint8_t *message,
		    uint32_t message_length)
{
	int printable_length;

	(void)application_context;
	if (!message)
		return;
	printable_length = message_length > INT_MAX ? INT_MAX : (int)message_length;
	if (level == URP_AST_LOG_WARNING)
		ast_log(LOG_WARNING, "%.*s\n", printable_length, (const char *)message);
	else if (level == URP_AST_LOG_ERROR)
		ast_log(LOG_ERROR, "%.*s\n", printable_length, (const char *)message);
	else
		ast_log(LOG_NOTICE, "%.*s\n", printable_length, (const char *)message);
}

/**
 * @brief Process one request on the channel's sole Rust control owner.
 * @param opaque Pointer to the synchronous control request.
 * @return Zero as required by the Asterisk taskprocessor callback contract.
 */
static int urp_control_execute(void *opaque)
{
	struct urp_control_task *task = opaque;
	struct urp_channel *channel = task->channel;

	switch (task->operation) {
	case URP_CONTROL_START:
		task->result = rust_adapter->channel_start(channel->rust_channel);
		break;
	case URP_CONTROL_STOP:
		task->result = rust_adapter->channel_stop(channel->rust_channel);
		break;
	case URP_CONTROL_RELOAD_PREPARE:
		task->result = rust_adapter->channel_reload_prepare(channel->rust_channel);
		break;
	case URP_CONTROL_RELOAD_ACTIVATE:
		task->result = rust_adapter->channel_reload_activate(channel->rust_channel);
		break;
	case URP_CONTROL_RELOAD_FINISH:
		task->result = rust_adapter->channel_reload_finish(channel->rust_channel,
								   task->argument.enabled);
		break;
	case URP_CONTROL_TEXT:
		task->result = rust_adapter->channel_write_text(channel->rust_channel,
								task->argument.text.text,
								task->argument.text.length);
		break;
	case URP_CONTROL_TRANSMIT:
		task->result = rust_adapter->channel_set_transmit(
			channel->rust_channel, task->argument.transmit.keyed,
			task->argument.transmit.ctcss_tenths_hz);
		break;
	case URP_CONTROL_DTMF:
		task->result = rust_adapter->channel_set_dtmf(channel->rust_channel,
							      task->argument.enabled);
		break;
	case URP_CONTROL_ECHO:
		task->result = rust_adapter->channel_set_echo(channel->rust_channel,
							      task->argument.enabled);
		break;
	case URP_CONTROL_JITTER:
		task->result = rust_adapter->channel_get_jitter_config(channel->rust_channel,
								       task->argument.jitter);
		break;
	case URP_CONTROL_COMMAND:
		task->result = rust_adapter->channel_command(channel->rust_channel,
							     task->argument.command);
		break;
	case URP_CONTROL_STATUS:
		task->result = rust_adapter->channel_get_status(channel->rust_channel,
								task->argument.status);
		break;
	case URP_CONTROL_SERVICE:
		task->result = rust_adapter->channel_service(channel->rust_channel);
		break;
	case URP_CONTROL_DESTROY:
		rust_adapter->channel_destroy(channel->rust_channel);
		channel->rust_channel = NULL;
		task->result = URP_AST_OK;
		break;
	}
	(void)ast_sem_post(&task->complete);
	return 0;
}

/**
 * @brief Submit one synchronous operation without concurrent Rust control access.
 * @param channel Destination channel and taskprocessor.
 * @param task Stack request which remains valid through completion.
 * @return Portable ABI result, or an Asterisk-host failure.
 */
static int urp_control_run_admitted(struct urp_channel *channel, struct urp_control_task *task)
{
	int reported_wait_error = 0;

	if (ast_sem_init(&task->complete, 0, 0))
		return URP_AST_ASTERISK_FAILURE;
	task->channel = channel;
	task->result = URP_AST_ASTERISK_FAILURE;
	if (ast_taskprocessor_push(channel->control, urp_control_execute, task)) {
		(void)ast_sem_destroy(&task->complete);
		return URP_AST_ASTERISK_FAILURE;
	}
	while (ast_sem_wait(&task->complete)) {
		if (errno != EINTR && !reported_wait_error) {
			ast_log(LOG_ERROR, "USBRadioPlus control wait failed: %s\n",
				strerror(errno));
			reported_wait_error = 1;
		}
	}
	(void)ast_sem_destroy(&task->complete);
	return task->result;
}

/**
 * @brief Submit one ordinary synchronous operation outside a reload transaction.
 * @param channel Destination channel and taskprocessor.
 * @param task Stack request which remains valid through completion.
 * @return Portable ABI result, including busy while reload owns admission.
 */
static int urp_control_run(struct urp_channel *channel, struct urp_control_task *task)
{
	int result;

	if (ast_rwlock_tryrdlock(&reload_control_gate))
		return URP_AST_CHANNEL_BUSY;
	result = urp_control_run_admitted(channel, task);
	ast_rwlock_unlock(&reload_control_gate);
	return result;
}

/**
 * @brief Replay the latest transmitter intent accepted while reload owned admission.
 * @param channel Live wrapper whose delivery worker owns retries.
 * @return Portable ABI result, or success when no intent is pending.
 */
static int urp_replay_transmit(struct urp_channel *channel)
{
	uint_fast64_t pending =
		atomic_load_explicit(&channel->pending_transmit, memory_order_acquire);
	struct urp_control_task task = {
		.operation = URP_CONTROL_TRANSMIT,
	};
	int result;

	if (!(pending & URP_PENDING_TRANSMIT_VALID))
		return URP_AST_OK;
	task.argument.transmit.keyed = !!(pending & URP_PENDING_TRANSMIT_KEYED);
	task.argument.transmit.ctcss_tenths_hz = (uint32_t)pending;
	result = urp_control_run(channel, &task);
	if (result == URP_AST_OK)
		(void)atomic_compare_exchange_strong_explicit(&channel->pending_transmit, &pending,
							      0, memory_order_acq_rel,
							      memory_order_acquire);
	return result;
}

/**
 * @brief Translate and apply the active generation's Asterisk jitter settings.
 * @param channel Channel whose resolved settings are applied.
 * @return Zero on success, or -1 when the channel or Rust settings are unavailable.
 */
static int urp_configure_jitter(struct urp_channel *channel)
{
	struct urp_ast_jitter_config resolved = {
		.struct_size = sizeof(resolved),
		.abi_version = URP_AST_ABI_VERSION,
	};
	struct urp_control_task task = {
		.operation = URP_CONTROL_JITTER,
		.argument.jitter = &resolved,
	};
	struct ast_channel *owner = urp_lock_owner(channel);
	struct ast_jb_conf config;

	if (!owner)
		return -1;
	if (urp_control_run(channel, &task) != URP_AST_OK) {
		ast_channel_unlock(owner);
		return -1;
	}
	ast_jb_conf_default(&config);
	config.flags = 0;
	if (resolved.enabled)
		config.flags |= AST_JB_ENABLED;
	if (resolved.force_enabled)
		config.flags |= AST_JB_FORCED;
	if (resolved.logging_enabled)
		config.flags |= AST_JB_LOG;
	if (resolved.video_sync_enabled)
		config.flags |= AST_JB_SYNC_VIDEO;
	config.max_size = resolved.maximum_size_ms;
	config.resync_threshold = resolved.resync_threshold_ms;
	config.target_extra = resolved.target_extra_ms;
	ast_copy_string(config.impl,
			resolved.implementation == URP_AST_JITTER_FIXED ? "fixed" : "adaptive",
			sizeof(config.impl));
	ast_jb_configure(owner, &config);
	ast_channel_unlock(owner);
	return 0;
}

/**
 * @brief Poll the Rust delivery endpoint outside Asterisk and PortAudio callbacks.
 * @param opaque Channel wrapper owned by this worker until it is joined.
 * @return NULL after stop or a delivery failure.
 */
static void *urp_delivery_worker(void *opaque)
{
	struct urp_channel *channel = opaque;

	while (!atomic_load_explicit(&channel->delivery_stop, memory_order_acquire)) {
		struct urp_control_task task = {
			.operation = URP_CONTROL_SERVICE,
		};
		int result;

		result = urp_replay_transmit(channel);
		if (result == URP_AST_CHANNEL_BUSY) {
			usleep(1000U);
			continue;
		}
		if (result != URP_AST_OK) {
			atomic_store_explicit(&channel->service_failed, 1, memory_order_release);
			break;
		}
		if (atomic_load_explicit(&channel->jitter_pending, memory_order_acquire) &&
		    !urp_configure_jitter(channel))
			atomic_store_explicit(&channel->jitter_pending, 0, memory_order_release);
		result = urp_control_run(channel, &task);
		if (result == URP_AST_CHANNEL_BUSY) {
			usleep(1000U);
			continue;
		}
		if (result != URP_AST_OK) {
			atomic_store_explicit(&channel->service_failed, 1, memory_order_release);
			break;
		}
		usleep(1000U);
	}
	return NULL;
}

/**
 * @brief Return the technology and fixed PCM contract selected by Asterisk.
 * @param type Requested Asterisk channel technology name.
 * @param transport Receives the portable transport identifier.
 * @param sample_rate_hz Receives the fixed boundary sample rate.
 * @return Matching channel technology, or NULL for an unknown name.
 */
static struct ast_channel_tech *urp_technology(const char *type, uint32_t *transport,
					       uint32_t *sample_rate_hz)
{
	if (!type)
		return NULL;
	if (!strcasecmp(type, advanced_tech.type)) {
		*transport = URP_AST_TRANSPORT_RPT_ADVANCED;
		*sample_rate_hz = URP_ADVANCED_RATE;
		return &advanced_tech;
	}
	if (!strcasecmp(type, app_rpt_tech.type)) {
		*transport = URP_AST_TRANSPORT_APP_RPT;
		*sample_rate_hz = URP_APP_RPT_RATE;
		return &app_rpt_tech;
	}
	return NULL;
}

/**
 * @brief Release a reservation which has not entered Asterisk's hangup path.
 * @param channel Partially constructed channel wrapper.
 */
static void urp_abandon_channel(struct urp_channel *channel)
{
	if (channel->rust_channel) {
		struct urp_control_task task = {
			.operation = URP_CONTROL_STOP,
		};

		(void)urp_control_run(channel, &task);
		task.operation = URP_CONTROL_DESTROY;
		(void)urp_control_run(channel, &task);
	}
	if (channel->control)
		channel->control = ast_taskprocessor_unreference(channel->control);
	if (channel->dsp)
		ast_dsp_free(channel->dsp);
	ast_free(channel->name);
	ast_free(channel);
}

/**
 * @brief Reserve one Rust channel and wrap it in the selected Asterisk technology.
 * @param type Requested channel technology name.
 * @param cap Requested Asterisk media capabilities.
 * @param assignedids Optional Asterisk channel identifiers.
 * @param requestor Optional requesting channel.
 * @param data Configured USBRadioPlus channel name.
 * @param cause Optional Asterisk failure cause output.
 * @return Locked allocated channel, or NULL on failure.
 */
static struct ast_channel *urp_request(const char *type, struct ast_format_cap *cap,
				       const struct ast_assigned_ids *assignedids,
				       const struct ast_channel *requestor, const char *data,
				       int *cause)
{
	struct urp_ast_channel_reserve_args reserve = {
		.struct_size = sizeof(reserve),
		.abi_version = URP_AST_ABI_VERSION,
	};
	struct urp_channel *channel;
	struct ast_channel_tech *technology;
	struct ast_channel *owner;
	uint32_t sample_rate_hz = 0;
	uint32_t transport = 0;
	unsigned int sequence;
	char taskprocessor_name[AST_TASKPROCESSOR_MAX_NAME + 1];
	int status;

	technology = urp_technology(type, &transport, &sample_rate_hz);
	if (!technology || !cap || ast_strlen_zero(data) ||
	    !ast_format_cap_iscompatible(cap, technology->capabilities))
		return NULL;
	if (strlen(data) > UINT32_MAX)
		return NULL;
	channel = ast_calloc(1, sizeof(*channel));
	if (!channel)
		return NULL;
	channel->name = ast_strdup(data);
	if (!channel->name) {
		urp_abandon_channel(channel);
		return NULL;
	}
	channel->sample_rate_hz = sample_rate_hz;
	channel->frame_samples = sample_rate_hz * URP_FRAME_MILLISECONDS / 1000U;
	channel->format = transport == URP_AST_TRANSPORT_APP_RPT
				  ? ast_format_slin
				  : ast_format_cache_get_slin_by_rate(sample_rate_hz);
	atomic_init(&channel->owner, NULL);
	atomic_init(&channel->delivery_running, 0);
	atomic_init(&channel->delivery_stop, 0);
	atomic_init(&channel->service_failed, 0);
	atomic_init(&channel->jitter_pending, 0);
	atomic_init(&channel->pending_transmit, 0);
	sequence = atomic_fetch_add_explicit(&taskprocessor_sequence, 1U, memory_order_relaxed);
	snprintf(taskprocessor_name, sizeof(taskprocessor_name), "usbradioplus/channel/%u",
		 sequence);
	channel->control = ast_taskprocessor_get(taskprocessor_name, TPS_REF_DEFAULT);
	if (!channel->control) {
		urp_abandon_channel(channel);
		return NULL;
	}
	if (transport == URP_AST_TRANSPORT_APP_RPT) {
		channel->dsp = ast_dsp_new();
		if (!channel->dsp) {
			urp_abandon_channel(channel);
			return NULL;
		}
		ast_dsp_set_features(channel->dsp, DSP_FEATURE_DIGIT_DETECT);
		ast_dsp_set_digitmode(channel->dsp, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MUTECONF |
							    DSP_DIGITMODE_RELAXDTMF);
	}
	reserve.channel_name = (const uint8_t *)data;
	reserve.channel_name_length = (uint32_t)strlen(data);
	reserve.transport = transport;
	reserve.channel_context = channel;
	ast_mutex_lock(&reload_lock);
	status = rust_adapter->channel_reserve(rust_driver, &reserve, &channel->rust_channel);
	if (status != URP_AST_OK) {
		if (cause && status == URP_AST_CHANNEL_BUSY)
			*cause = AST_CAUSE_BUSY;
		if (status == URP_AST_CHANNEL_NOT_FOUND)
			ast_log(LOG_WARNING, "%s/%s: Rust channel was not configured\n", type,
				data);
		else
			ast_log(LOG_ERROR, "%s/%s: Rust channel reservation failed (%d)\n", type,
				data, status);
		ast_mutex_unlock(&reload_lock);
		urp_abandon_channel(channel);
		return NULL;
	}
	owner = ast_channel_alloc(1, AST_STATE_DOWN, NULL, NULL, "", NULL, NULL, assignedids,
				  requestor, 0, "%s/%s", technology->type, data);
	if (!owner) {
		urp_abandon_channel(channel);
		ast_mutex_unlock(&reload_lock);
		return NULL;
	}
	ast_channel_tech_set(owner, technology);
	ast_channel_internal_fd_set(owner, 0, -1);
	ast_channel_nativeformats_set(owner, technology->capabilities);
	ast_channel_set_readformat(owner, channel->format);
	ast_channel_set_writeformat(owner, channel->format);
	ast_channel_tech_pvt_set(owner, channel);
	atomic_store_explicit(&channel->owner, owner, memory_order_release);
	atomic_fetch_add_explicit(&active_channels, 1U, memory_order_relaxed);
	ast_module_ref(ast_module_info->self);
	ast_mutex_lock(&channel_list_lock);
	channel->next = channel_list;
	channel_list = channel;
	ast_mutex_unlock(&channel_list_lock);
	if (urp_configure_jitter(channel)) {
		ast_channel_unlock(owner);
		ast_mutex_unlock(&reload_lock);
		ast_hangup(owner);
		return NULL;
	}
	ast_channel_unlock(owner);
	ast_mutex_unlock(&reload_lock);
	return owner;
}

/**
 * @brief Start hardware and the non-real-time Asterisk delivery worker.
 * @param owner Asterisk channel being called.
 * @param destination Unused destination supplied by Asterisk.
 * @param timeout Unused timeout supplied by Asterisk.
 * @return Zero on success, or -1 when startup fails.
 */
static int urp_call(struct ast_channel *owner, const char *destination, int timeout)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct urp_control_task task = {
		.operation = URP_CONTROL_START,
	};

	(void)destination;
	(void)timeout;
	if (!channel)
		return -1;
	if (atomic_load_explicit(&channel->jitter_pending, memory_order_acquire)) {
		if (urp_configure_jitter(channel))
			return -1;
		atomic_store_explicit(&channel->jitter_pending, 0, memory_order_release);
	}
	if (urp_control_run(channel, &task) != URP_AST_OK)
		return -1;
	atomic_store_explicit(&channel->delivery_stop, 0, memory_order_release);
	atomic_store_explicit(&channel->service_failed, 0, memory_order_release);
	if (ast_pthread_create_background(&channel->delivery_thread, NULL, urp_delivery_worker,
					  channel)) {
		task.operation = URP_CONTROL_STOP;
		(void)urp_control_run(channel, &task);
		return -1;
	}
	atomic_store_explicit(&channel->delivery_running, 1, memory_order_release);
	ast_setstate(owner, AST_STATE_UP);
	return 0;
}

/**
 * @brief Stop all producers, serialize stop/destroy, and release the wrapper.
 * @param owner Asterisk channel being hung up.
 * @return Zero on success, or -1 when Rust reported a stop failure.
 */
static int urp_hangup(struct ast_channel *owner)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	int result = 0;

	if (!channel)
		return 0;
	/* ast_hangup() holds this channel's lock. New delivery attempts must fail
	 * before it is dropped so reload can never invert owner -> reload_lock. */
	atomic_store_explicit(&channel->owner, NULL, memory_order_release);
	ast_channel_unlock(owner);
	ast_mutex_lock(&reload_lock);
	ast_channel_lock(owner);
	ast_mutex_lock(&channel_list_lock);
	if (channel_list == channel) {
		channel_list = channel->next;
	} else {
		struct urp_channel *previous;

		for (previous = channel_list; previous && previous->next != channel;
		     previous = previous->next)
			;
		if (previous)
			previous->next = channel->next;
	}
	ast_mutex_unlock(&channel_list_lock);
	if (atomic_exchange_explicit(&channel->delivery_running, 0, memory_order_acq_rel)) {
		atomic_store_explicit(&channel->delivery_stop, 1, memory_order_release);
		pthread_join(channel->delivery_thread, NULL);
	}
	if (channel->rust_channel) {
		struct urp_control_task task = {
			.operation = URP_CONTROL_STOP,
		};
		if (urp_control_run(channel, &task) != URP_AST_OK)
			result = -1;
		task.operation = URP_CONTROL_DESTROY;
		(void)urp_control_run(channel, &task);
	}
	ast_channel_tech_pvt_set(owner, NULL);
	channel->control = ast_taskprocessor_unreference(channel->control);
	if (channel->dsp)
		ast_dsp_free(channel->dsp);
	ast_free(channel->name);
	ast_free(channel);
	atomic_fetch_sub_explicit(&active_channels, 1U, memory_order_relaxed);
	ast_module_unref(ast_module_info->self);
	ast_mutex_unlock(&reload_lock);
	return result;
}

/**
 * @brief Mark an answered channel up; media remains hardware paced.
 * @param channel Asterisk channel being answered.
 * @return Zero.
 */
static int urp_answer(struct ast_channel *channel)
{
	ast_setstate(channel, AST_STATE_UP);
	return 0;
}

/**
 * @brief Return a null frame; Rust queues hardware-paced frames asynchronously.
 * @param owner Asterisk channel being read.
 * @return A null frame, or NULL after the delivery worker fails.
 */
static struct ast_frame *urp_read(struct ast_channel *owner)
{
	const struct urp_channel *channel = ast_channel_tech_pvt(owner);

	if (!channel || atomic_load_explicit(&channel->service_failed, memory_order_acquire))
		return NULL;
	return &ast_null_frame;
}

/**
 * @brief Publish one exact controller frame to Rust's lock-free producer endpoint.
 * @param owner Destination Asterisk channel.
 * @param frame Complete signed-linear controller frame.
 * @return Zero on success, or -1 for invalid media or a Rust failure.
 */
static int urp_write(struct ast_channel *owner, struct ast_frame *frame)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	uint32_t samples;

	if (!channel || !frame || frame->frametype != AST_FRAME_VOICE || !frame->data.ptr ||
	    frame->datalen % sizeof(int16_t))
		return -1;
	samples = frame->datalen / sizeof(int16_t);
	if (samples != channel->frame_samples || frame->samples != (int)samples)
		return -1;
	return rust_adapter->channel_write_voice(channel->rust_channel, frame->data.ptr, samples) ==
			       URP_AST_OK
		       ? 0
		       : -1;
}

/**
 * @brief Forward one app_rpt radio-control message to Rust's serialized endpoint.
 * @param owner Destination Asterisk channel.
 * @param text NUL-terminated controller message.
 * @return Zero on success, or -1 on invalid input/failure.
 */
static int urp_send_text(struct ast_channel *owner, const char *text)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct urp_control_task task = {
		.operation = URP_CONTROL_TEXT,
	};
	size_t length;

	if (!channel || !text)
		return -1;
	length = strlen(text);
	if (length > UINT32_MAX)
		return -1;
	task.argument.text.text = (const uint8_t *)text;
	task.argument.text.length = length;
	return urp_control_run(channel, &task) == URP_AST_OK ? 0 : -1;
}

/**
 * @brief Parse Asterisk's optional decimal CTCSS payload as tenths of one hertz.
 * @param data Optional byte-counted decimal frequency.
 * @param data_length Number of bytes available at @p data.
 * @param tenths_hz Receives the rounded frequency or zero when omitted.
 * @return Zero on success, or -1 when the payload is invalid.
 */
static int urp_forced_ctcss(const void *data, size_t data_length, uint32_t *tenths_hz)
{
	char text[32];
	char *end;
	double frequency;
	double rounded;

	*tenths_hz = 0;
	if (!data || !data_length)
		return 0;
	if (data_length >= sizeof(text))
		return -1;
	memcpy(text, data, data_length);
	text[data_length] = '\0';
	frequency = strtod(text, &end);
	if (end == text || *end || !isfinite(frequency) || frequency < 0.0 ||
	    frequency > (double)UINT32_MAX / 10.0)
		return -1;
	rounded = floor(frequency * 10.0 + 0.5);
	*tenths_hz = (uint32_t)rounded;
	return 0;
}

/**
 * @brief Preserve Asterisk indications and route transmitter state through Rust.
 * @param owner Destination Asterisk channel.
 * @param condition Asterisk control-frame condition.
 * @param data Optional condition payload.
 * @param data_length Number of bytes available at @p data.
 * @return Zero when handled, or -1 for unsupported/invalid conditions.
 */
static int urp_indicate(struct ast_channel *owner, int condition, const void *data,
			size_t data_length)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct urp_control_task task = {
		.operation = URP_CONTROL_TRANSMIT,
	};

	if (!channel)
		return -1;
	switch ((enum ast_control_frame_type)condition) {
	case AST_CONTROL_BUSY:
	case AST_CONTROL_CONGESTION:
	case AST_CONTROL_RINGING:
	case AST_CONTROL_VIDUPDATE:
		return 0;
	case AST_CONTROL_HOLD:
		return ast_moh_start(owner, data, "default");
	case AST_CONTROL_UNHOLD:
	case AST_CONTROL_PROCEEDING:
	case AST_CONTROL_PROGRESS:
		ast_moh_stop(owner);
		return 0;
	case AST_CONTROL_RADIO_KEY:
		task.argument.transmit.keyed = 1;
		if (urp_forced_ctcss(data, data_length, &task.argument.transmit.ctcss_tenths_hz))
			return -1;
		break;
	case AST_CONTROL_RADIO_UNKEY:
		task.argument.transmit.keyed = 0;
		task.argument.transmit.ctcss_tenths_hz = 0;
		break;
	default:
		return -1;
	}
	{
		uint_fast64_t pending =
			URP_PENDING_TRANSMIT_VALID |
			(task.argument.transmit.keyed ? URP_PENDING_TRANSMIT_KEYED : 0) |
			task.argument.transmit.ctcss_tenths_hz;
		int result;

		if (atomic_load_explicit(&channel->pending_transmit, memory_order_acquire)) {
			atomic_store_explicit(&channel->pending_transmit, pending,
					      memory_order_release);
			return 0;
		}
		if (ast_rwlock_tryrdlock(&reload_control_gate)) {
			atomic_store_explicit(&channel->pending_transmit, pending,
					      memory_order_release);
			return 0;
		}
		result = urp_control_run_admitted(channel, &task);
		ast_rwlock_unlock(&reload_control_gate);
		return result == URP_AST_OK ? 0 : -1;
	}
}

/**
 * @brief Point asynchronous Rust delivery at a masqueraded Asterisk channel.
 * @param old_channel Replaced Asterisk channel.
 * @param new_channel New Asterisk channel owning the wrapper.
 * @return Zero on success, or -1 when no wrapper is present.
 */
static int urp_fixup(struct ast_channel *old_channel, struct ast_channel *new_channel)
{
	struct urp_channel *channel = ast_channel_tech_pvt(new_channel);

	(void)old_channel;
	if (!channel)
		return -1;
	atomic_store_explicit(&channel->owner, new_channel, memory_order_release);
	return 0;
}

/**
 * @brief Retain app_rpt's established TONE_VERIFY mapping.
 * @param owner Destination Asterisk channel.
 * @param option Asterisk option identifier.
 * @param data Byte-counted option payload.
 * @param data_length Number of bytes available at @p data.
 * @return Zero when accepted, or -1 for invalid input/failure.
 */
static int urp_setoption(struct ast_channel *owner, int option, void *data, int data_length)
{
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct urp_control_task task = {
		.operation = URP_CONTROL_DTMF,
	};
	const uint8_t *mode = data;

	if (!channel || !data || data_length < 1) {
		errno = EINVAL;
		return -1;
	}
	if (option != AST_OPTION_TONE_VERIFY)
		return 0;
	task.argument.enabled = *mode == 3U ? 0U : 1U;
	return urp_control_run(channel, &task) == URP_AST_OK ? 0 : -1;
}

/**
 * @brief Accept a DTMF begin which requires no radio-channel action.
 * @param channel Source Asterisk channel.
 * @param digit DTMF digit.
 * @return Zero.
 */
static int urp_digit_begin(struct ast_channel *channel, char digit)
{
	(void)channel;
	(void)digit;
	return 0;
}

/**
 * @brief Retain the historical diagnostic for an outgoing Asterisk digit.
 * @param channel Source Asterisk channel.
 * @param digit DTMF digit.
 * @param duration Digit duration in milliseconds.
 * @return Zero.
 */
static int urp_digit_end(struct ast_channel *channel, char digit, unsigned int duration)
{
	(void)channel;
	ast_verbose(" << RadioPlus received digit %c of duration %u ms >>\n", digit, duration);
	return 0;
}

/**
 * @brief Read the complete configuration file through Asterisk's path policy.
 * @param source Receives an allocated absolute configuration path.
 * @return Allocated file contents, or NULL on failure.
 */
static char *urp_read_configuration(char **source)
{
	if (ast_asprintf(source, "%s/%s", ast_config_AST_CONFIG_DIR, URP_CONFIG_FILE) < 0)
		return NULL;
	return ast_read_textfile(*source);
}

/**
 * @brief Run one reload phase for every membership-frozen live channel.
 * @param operation Prepare, activate, or rollback operation to serialize.
 * @param commit Boolean finish argument; zero for every fallible phase.
 * @return Zero when every channel completes the phase, otherwise -1.
 */
static int urp_reload_channels(enum urp_control_operation operation, uint32_t commit)
{
	struct urp_channel *channel;
	int result = 0;

	for (channel = channel_list; channel; channel = channel->next) {
		struct urp_control_task task = {
			.operation = operation,
		};
		int status;

		task.argument.enabled = commit;
		status = urp_control_run_admitted(channel, &task);
		if (status == URP_AST_OK)
			continue;
		ast_log(LOG_ERROR, "%s: configuration reload phase %d failed (%d)\n", channel->name,
			operation, status);
		result = -1;
		if (operation != URP_CONTROL_RELOAD_FINISH)
			break;
	}
	return result;
}

/**
 * @brief Atomically prepare and adopt one complete configuration generation.
 * @return Zero only after every live channel and attached link has adopted it.
 */
static int urp_reload_configuration(void)
{
	char *source = NULL;
	char *text = urp_read_configuration(&source);
	struct urp_link_reload_entry *links = NULL;
	int result = -1;

	if (!text) {
		ast_log(LOG_ERROR, "Unable to read %s\n", source ? source : URP_CONFIG_FILE);
		ast_free(source);
		return -1;
	}
	if (strlen(source) > UINT32_MAX || strlen(text) > UINT32_MAX) {
		ast_log(LOG_ERROR, "%s is too large for the USBRadioPlus ABI\n", source);
		ast_free(text);
		ast_free(source);
		return -1;
	}
	ast_mutex_lock(&reload_lock);
	ast_rwlock_wrlock(&reload_control_gate);
	ast_mutex_lock(&link_scan_lock);
	result = rust_adapter->driver_reload(rust_driver, (const uint8_t *)source,
					     (uint32_t)strlen(source), (const uint8_t *)text,
					     (uint32_t)strlen(text));
	if (result != URP_AST_OK)
		goto finished;
	if (urp_reload_channels(URP_CONTROL_RELOAD_PREPARE, 0) || urp_prepare_link_reload(&links) ||
	    urp_reload_channels(URP_CONTROL_RELOAD_ACTIVATE, 0))
		goto rollback;
	result = rust_adapter->driver_reload_finish(rust_driver, 1);
	if (result != URP_AST_OK)
		goto rollback;
	/* These commits only retire detached rollback ownership and cannot fail. */
	urp_finish_link_reload(links, 1);
	links = NULL;
	{
		struct urp_channel *channel;

		for (channel = channel_list; channel; channel = channel->next)
			(void)rust_adapter->channel_reload_finish(channel->rust_channel, 1);
	}
	{
		struct urp_channel *channel;

		for (channel = channel_list; channel; channel = channel->next)
			atomic_store_explicit(&channel->jitter_pending, 1, memory_order_release);
	}
	result = URP_AST_OK;
	goto finished;

rollback:
	urp_finish_link_reload(links, 0);
	links = NULL;
	if (urp_reload_channels(URP_CONTROL_RELOAD_FINISH, 0))
		ast_log(LOG_ERROR, "Configuration reload rollback degraded; affected station "
				   "remains RF-safe and stopped\n");
	if (rust_adapter->driver_reload_finish(rust_driver, 0) != URP_AST_OK)
		ast_log(LOG_ERROR, "Unable to discard the staged configuration generation\n");
	result = -1;

finished:
	ast_mutex_unlock(&link_scan_lock);
	ast_rwlock_unlock(&reload_control_gate);
	ast_mutex_unlock(&reload_lock);
	ast_free(text);
	ast_free(source);
	return result == URP_AST_OK ? 0 : -1;
}

/**
 * @brief Return one configured or active channel name allocated with ast_malloc().
 * @param index Configured-channel index when @p active is false.
 * @param active Select the active channel instead of @p index.
 * @param result Receives the portable ABI result.
 * @return Allocated NUL-terminated name, or NULL.
 */
static char *urp_driver_channel_name(uint32_t index, int active, int *result)
{
	uint32_t length = 0;
	char *name;

	*result = active ? rust_adapter->driver_active_channel(rust_driver, NULL, 0, &length)
			 : rust_adapter->driver_channel_name(rust_driver, index, NULL, 0, &length);
	if (*result != URP_AST_OK || !length)
		return NULL;
	name = ast_malloc((size_t)length + 1U);
	if (!name) {
		*result = URP_AST_ASTERISK_FAILURE;
		return NULL;
	}
	*result = active ? rust_adapter->driver_active_channel(rust_driver, (uint8_t *)name, length,
							       &length)
			 : rust_adapter->driver_channel_name(rust_driver, index, (uint8_t *)name,
							     length, &length);
	if (*result != URP_AST_OK) {
		ast_free(name);
		return NULL;
	}
	name[length] = '\0';
	return name;
}

/**
 * @brief Find one live channel while holding its teardown exclusion lock.
 * @param name Case-insensitive configured channel name.
 * @return Matching channel with the list lock held, or NULL.
 */
static struct urp_channel *urp_lock_channel(const char *name)
{
	struct urp_channel *channel;

	ast_mutex_lock(&channel_list_lock);
	for (channel = channel_list; channel; channel = channel->next) {
		if (!strcasecmp(channel->name, name))
			return channel;
	}
	ast_mutex_unlock(&channel_list_lock);
	return NULL;
}

/**
 * @brief Select the active live channel and hold its teardown exclusion lock.
 * @param name Receives the allocated active channel name.
 * @param result Receives the portable ABI result.
 * @return Active channel with the list lock held, or NULL.
 */
static struct urp_channel *urp_lock_active_channel(char **name, int *result)
{
	struct urp_channel *channel;

	*name = urp_driver_channel_name(0, 1, result);
	if (!*name)
		return NULL;
	channel = urp_lock_channel(*name);
	if (!channel) {
		ast_free(*name);
		*name = NULL;
		*result = URP_AST_NOT_READY;
	}
	return channel;
}

/** Release a channel selected by urp_lock_channel(). */
static void urp_unlock_channel(void)
{
	ast_mutex_unlock(&channel_list_lock);
}

/**
 * @brief Retain one link hook while its owning channel may masquerade or hang up.
 * @param hook Live link hook protected by datastore or staged-reload ownership.
 */
static void urp_link_hook_ref(struct urp_link_hook *hook)
{
	(void)atomic_fetch_add_explicit(&hook->references, 1, memory_order_relaxed);
}

/**
 * @brief Release one hook and destroy it after Asterisk has quiesced its callback.
 * @param hook Retained link hook.
 */
static void urp_link_hook_unref(struct urp_link_hook *hook)
{
	if (atomic_fetch_sub_explicit(&hook->references, 1, memory_order_acq_rel) != 1)
		return;
	if (atomic_exchange_explicit(&hook->attachment_state, URP_LINK_DETACHED,
				     memory_order_acq_rel) == URP_LINK_ATTACHED)
		ast_audiohook_detach(&hook->audiohook);
	ast_audiohook_destroy(&hook->audiohook);
	if (hook->rust_link)
		rust_adapter->link_destroy(hook->rust_link);
	if (hook->reload_link)
		rust_adapter->link_destroy(hook->reload_link);
	ast_free(hook->asterisk_channel);
	ast_free(hook->profile);
	ast_free(hook);
}

/**
 * @brief Drop the datastore's link-hook reference and stop future callbacks.
 * @param data Link-hook datastore payload, or NULL.
 */
static void urp_link_hook_destroy(void *data)
{
	struct urp_link_hook *hook = data;

	if (!hook)
		return;
	if (atomic_exchange_explicit(&hook->attachment_state, URP_LINK_DETACHED,
				     memory_order_acq_rel) == URP_LINK_ATTACHED)
		ast_audiohook_detach(&hook->audiohook);
	urp_link_hook_unref(hook);
}

/** Asterisk datastore owner for each Rust incoming-link graph. */
static const struct ast_datastore_info urp_link_datastore = {
	.type = "usbradioplus-link",
	.destroy = urp_link_hook_destroy,
};

/* Asterisk requires a mutable audiohook pointer for this callback ABI. */
// cppcheck-suppress constParameterCallback
/**
 * @brief Process one eligible incoming-link frame in place.
 *
 * Graphs and conversion workspaces are prepared before attachment. The callback
 * performs one bounded Rust operation and never allocates, locks, or logs.
 * @param audiohook Owning Asterisk audiohook and link wrapper.
 * @param channel Asterisk link channel supplying the frame.
 * @param frame Current audio frame, or NULL.
 * @param direction Audiohook read or write direction.
 * @return Zero; processing failures retain the original frame.
 */
static int urp_link_callback(struct ast_audiohook *audiohook, struct ast_channel *channel,
			     struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	struct urp_link_hook *hook = (struct urp_link_hook *)audiohook;
	uint32_t rust_direction;
	unsigned int sample_rate;

	(void)channel;
	if (!hook->rust_link || audiohook->status == AST_AUDIOHOOK_STATUS_DONE || !frame ||
	    frame->frametype != AST_FRAME_VOICE || !frame->data.ptr || frame->samples <= 0)
		return 0;
	rust_direction = direction == AST_AUDIOHOOK_DIRECTION_READ ? URP_AST_LINK_DIRECTION_READ
								   : URP_AST_LINK_DIRECTION_WRITE;
	sample_rate = ast_format_get_sample_rate(frame->subclass.format);
	if (!sample_rate)
		sample_rate = URP_APP_RPT_RATE;
	(void)rust_adapter->link_process(hook->rust_link, rust_direction, sample_rate,
					 frame->data.ptr, (uint32_t)frame->samples);
	return 0;
}

/**
 * @brief Test whether a locked channel is one ASL3 incoming link source.
 * @param channel Locked candidate Asterisk channel.
 * @return Nonzero when the channel is an eligible incoming link.
 */
static int urp_link_eligible(const struct ast_channel *channel)
{
	const char *name = ast_channel_name(channel);
	const char *application = ast_channel_appl(channel);
	const char *data = ast_channel_data(channel);

	return name && !strncmp(name, "IAX2/", 5) && application && !strcmp(application, "Rpt") &&
	       data && !strcmp(data, "Remote Rx");
}

/**
 * @brief Find the first configured profile with a live USBRadioPlus channel.
 * @return Allocated profile name, or NULL when no live profile exists.
 */
static char *urp_link_profile(void)
{
	uint32_t index;

	for (index = 0; index != UINT32_MAX; ++index) {
		const struct urp_channel *channel;
		char *name;
		int result;

		name = urp_driver_channel_name(index, 0, &result);
		if (!name)
			return NULL;
		channel = urp_lock_channel(name);
		if (channel) {
			urp_unlock_channel();
			return name;
		}
		ast_free(name);
	}
	return NULL;
}

/**
 * @brief Prepare either an active-generation or staged-generation link graph.
 * @param profile Configured live radio profile.
 * @param sample_rate Negotiated Asterisk link sample rate.
 * @param reload Select the private staged driver generation.
 * @param output Receives the prepared graph or NULL for disabled processing.
 * @return Portable ABI result.
 */
static int urp_prepare_link_graph(const char *profile, unsigned int sample_rate, int reload,
				  void **output)
{
	*output = NULL;
	return reload ? rust_adapter->link_prepare_reload(rust_driver, (const uint8_t *)profile,
							  (uint32_t)strlen(profile), sample_rate,
							  URP_LINK_MAXIMUM_FRAME_COUNT, output)
		      : rust_adapter->link_prepare(rust_driver, (const uint8_t *)profile,
						   (uint32_t)strlen(profile), sample_rate,
						   URP_LINK_MAXIMUM_FRAME_COUNT, output);
}

/**
 * @brief Build and attach a new hook whose graph is already prepared.
 * @param channel Referenced eligible Asterisk link channel.
 * @param profile Live radio profile used to resolve processing.
 * @param sample_rate Link input rate.
 * @param reload Select staged-generation preparation and dormant attachment.
 * @param retained Receives the builder reference for staged commit, or NULL.
 * @return Zero for success/not-ready, otherwise -1. A non-NULL result is fully attached.
 */
static int urp_install_link(struct ast_channel *channel, const char *profile,
			    unsigned int sample_rate, int reload, struct urp_link_hook **retained)
{
	struct ast_datastore *datastore;
	struct urp_link_hook *hook = ast_calloc(1, sizeof(*hook));
	int audiohook_ready = 0;
	int expected;
	int result;

	if (retained)
		*retained = NULL;
	if (!hook)
		return -1;
	hook->profile = ast_strdup(profile);
	hook->asterisk_channel = ast_strdup(ast_channel_name(channel));
	if (!hook->profile || !hook->asterisk_channel)
		goto failed_uninitialized;
	result = urp_prepare_link_graph(profile, sample_rate, reload,
					reload ? &hook->reload_link : &hook->rust_link);
	if (result == URP_AST_NOT_READY) {
		ast_free(hook->asterisk_channel);
		ast_free(hook->profile);
		ast_free(hook);
		return 0;
	}
	if (result != URP_AST_OK) {
		ast_log(LOG_ERROR, "Unable to prepare%s link processing for %s (%d)\n",
			reload ? " replacement" : "", hook->asterisk_channel, result);
		goto failed_uninitialized;
	}
	if (ast_audiohook_init(&hook->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "USBRadioPlus",
			       AST_AUDIOHOOK_MANIPULATE_ALL_RATES))
		goto failed_uninitialized;
	audiohook_ready = 1;
	hook->audiohook.manipulate_callback = urp_link_callback;
	datastore = ast_datastore_alloc(&urp_link_datastore, NULL);
	if (!datastore)
		goto failed_uninitialized;
	atomic_init(&hook->references, 2);
	atomic_init(&hook->attachment_state, URP_LINK_BUILDING);
	hook->reload_pending = reload;
	datastore->data = hook;

	ast_channel_lock(channel);
	if (ast_channel_datastore_find(channel, &urp_link_datastore, NULL)) {
		ast_channel_unlock(channel);
		ast_datastore_free(datastore);
		urp_link_hook_unref(hook);
		return 0;
	}
	ast_channel_datastore_add(channel, datastore);
	ast_channel_unlock(channel);
	if (ast_audiohook_attach(channel, &hook->audiohook)) {
		ast_channel_lock(channel);
		if (ast_channel_datastore_find(channel, &urp_link_datastore, NULL) == datastore)
			ast_channel_datastore_remove(channel, datastore);
		else
			datastore = NULL;
		ast_channel_unlock(channel);
		if (datastore)
			ast_datastore_free(datastore);
		urp_link_hook_unref(hook);
		return -1;
	}
	expected = URP_LINK_BUILDING;
	if (!atomic_compare_exchange_strong_explicit(&hook->attachment_state, &expected,
						     URP_LINK_ATTACHED, memory_order_acq_rel,
						     memory_order_acquire)) {
		ast_audiohook_detach(&hook->audiohook);
		urp_link_hook_unref(hook);
		return -1;
	}
	ast_log(LOG_NOTICE, "USBRadioPlus link processing attached to %s%s\n",
		hook->asterisk_channel, reload ? " for staged reload" : "");
	if (retained)
		*retained = hook;
	else
		urp_link_hook_unref(hook);
	return 0;

failed_uninitialized:
	if (audiohook_ready)
		ast_audiohook_destroy(&hook->audiohook);
	if (hook->rust_link)
		rust_adapter->link_destroy(hook->rust_link);
	if (hook->reload_link)
		rust_adapter->link_destroy(hook->reload_link);
	ast_free(hook->asterisk_channel);
	ast_free(hook->profile);
	ast_free(hook);
	return -1;
}

/**
 * @brief Prepare and attach one Rust graph to an eligible incoming link channel.
 * @param channel Eligible Asterisk link channel.
 * @param profile Live USBRadioPlus profile used to resolve link processing.
 * @return Zero when attached or already present, otherwise -1.
 */
static int urp_attach_link(struct ast_channel *channel, const char *profile)
{
	struct ast_datastore *datastore;
	struct urp_link_hook *hook = NULL;
	const struct ast_format *format;
	void *prepared = NULL;
	unsigned int sample_rate;
	int result;

	ast_channel_lock(channel);
	datastore = ast_channel_datastore_find(channel, &urp_link_datastore, NULL);
	if (datastore && datastore->data) {
		hook = datastore->data;
		urp_link_hook_ref(hook);
	}
	format = ast_channel_rawreadformat(channel);
	sample_rate = format ? ast_format_get_sample_rate(format) : 0;
	ast_channel_unlock(channel);
	if (!sample_rate)
		sample_rate = URP_APP_RPT_RATE;
	if (!hook)
		return urp_install_link(channel, profile, sample_rate, 0, NULL);

	ast_audiohook_lock(&hook->audiohook);
	result = atomic_load_explicit(&hook->attachment_state, memory_order_acquire) ==
			 URP_LINK_ATTACHED &&
		 !hook->rust_link && !hook->reload_pending;
	ast_audiohook_unlock(&hook->audiohook);
	if (!result) {
		urp_link_hook_unref(hook);
		return 0;
	}
	result = urp_prepare_link_graph(profile, sample_rate, 0, &prepared);
	if (result != URP_AST_OK && result != URP_AST_NOT_READY) {
		ast_log(LOG_ERROR, "Unable to prepare link processing for %s (%d)\n",
			hook->asterisk_channel, result);
		urp_link_hook_unref(hook);
		return -1;
	}
	ast_audiohook_lock(&hook->audiohook);
	if (atomic_load_explicit(&hook->attachment_state, memory_order_acquire) ==
		    URP_LINK_ATTACHED &&
	    !hook->rust_link && !hook->reload_pending) {
		hook->rust_link = prepared;
		prepared = NULL;
	}
	ast_audiohook_unlock(&hook->audiohook);
	if (prepared)
		rust_adapter->link_destroy(prepared);
	urp_link_hook_unref(hook);
	return 0;
}

/** Discover eligible incoming links for the first live configured radio. */
static void urp_scan_links(void)
{
	struct ast_channel_iterator *iterator;
	struct ast_channel *channel;
	char *profile = urp_link_profile();

	if (!profile)
		return;
	iterator = ast_channel_iterator_all_new();
	if (!iterator) {
		ast_free(profile);
		return;
	}
	while ((channel = ast_channel_iterator_next(iterator))) {
		ast_channel_lock(channel);
		int eligible = urp_link_eligible(channel);
		ast_channel_unlock(channel);
		if (eligible)
			(void)urp_attach_link(channel, profile);
		ast_channel_unref(channel);
	}
	ast_channel_iterator_destroy(iterator);
	ast_free(profile);
}

/** Remove every incoming-link hook after quiescing its Asterisk callback. */
static void urp_detach_all_links(void)
{
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	struct ast_channel *channel;

	if (!iterator)
		return;
	while ((channel = ast_channel_iterator_next(iterator))) {
		struct ast_datastore *datastore;

		ast_channel_lock(channel);
		datastore = ast_channel_datastore_find(channel, &urp_link_datastore, NULL);
		if (datastore)
			ast_channel_datastore_remove(channel, datastore);
		ast_channel_unlock(channel);
		if (datastore)
			ast_datastore_free(datastore);
		ast_channel_unref(channel);
	}
	ast_channel_iterator_destroy(iterator);
}

/**
 * @brief Prepare every existing graph and attach new graphs dormant before publication.
 * @param entries Receives direct retained hooks, safe across channel masquerades.
 * @return Zero when every graph/hook is ready, otherwise -1; finish consumes the list.
 */
static int urp_prepare_link_reload(struct urp_link_reload_entry **entries)
{
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	struct ast_channel *channel;
	char *profile = NULL;
	int result = 0;

	*entries = NULL;
	if (!iterator)
		return -1;
	while ((channel = ast_channel_iterator_next(iterator))) {
		struct ast_datastore *datastore;
		struct urp_link_hook *hook = NULL;
		struct urp_link_reload_entry *entry;
		const struct ast_format *format;
		unsigned int sample_rate;
		int eligible;
		int status;

		ast_channel_lock(channel);
		eligible = urp_link_eligible(channel);
		datastore = ast_channel_datastore_find(channel, &urp_link_datastore, NULL);
		if (datastore && datastore->data) {
			hook = datastore->data;
			urp_link_hook_ref(hook);
		}
		format = ast_channel_rawreadformat(channel);
		sample_rate = format ? ast_format_get_sample_rate(format) : 0;
		ast_channel_unlock(channel);
		if (!sample_rate)
			sample_rate = URP_APP_RPT_RATE;
		if (!hook && !eligible) {
			ast_channel_unref(channel);
			continue;
		}
		entry = ast_calloc(1, sizeof(*entry));
		if (!entry) {
			if (hook)
				urp_link_hook_unref(hook);
			ast_channel_unref(channel);
			result = -1;
			break;
		}
		if (hook) {
			void *candidate = NULL;

			status = urp_prepare_link_graph(hook->profile, sample_rate, 1, &candidate);
			if (status == URP_AST_OK || status == URP_AST_NOT_READY) {
				hook->reload_link = candidate;
				hook->reload_pending = 1;
				entry->hook = hook;
			} else {
				ast_log(LOG_ERROR,
					"Unable to prepare replacement link processing for %s "
					"(%d)\n",
					hook->asterisk_channel, status);
				urp_link_hook_unref(hook);
				result = -1;
			}
		} else {
			if (!profile)
				profile = urp_link_profile();
			status = profile ? urp_install_link(channel, profile, sample_rate, 1,
							    &entry->hook)
					 : URP_AST_NOT_READY;
			if (status)
				result = -1;
		}
		ast_channel_unref(channel);
		if (result || !entry->hook) {
			ast_free(entry);
		} else {
			entry->next = *entries;
			*entries = entry;
		}
		if (result)
			break;
	}
	ast_channel_iterator_destroy(iterator);
	ast_free(profile);
	return result;
}

/**
 * @brief Commit or discard every prepared graph using direct hook ownership.
 * @param entries Prepared hook list returned by urp_prepare_link_reload().
 * @param commit Nonzero to swap candidates in place, zero to retain active graphs.
 */
static void urp_finish_link_reload(struct urp_link_reload_entry *entries, int commit)
{
	while (entries) {
		struct urp_link_reload_entry *entry = entries;
		struct urp_link_hook *hook = entry->hook;
		void *retired = NULL;

		ast_audiohook_lock(&hook->audiohook);
		if (hook->reload_pending) {
			if (commit &&
			    atomic_load_explicit(&hook->attachment_state, memory_order_acquire) ==
				    URP_LINK_ATTACHED) {
				retired = hook->rust_link;
				hook->rust_link = hook->reload_link;
				hook->reload_link = NULL;
			} else {
				retired = hook->reload_link;
				hook->reload_link = NULL;
			}
			hook->reload_pending = 0;
		}
		ast_audiohook_unlock(&hook->audiohook);
		if (retired)
			rust_adapter->link_destroy(retired);
		urp_link_hook_unref(hook);
		entries = entry->next;
		ast_free(entry);
	}
}

/**
 * @brief Periodically discover incoming links until module shutdown.
 * @param unused Required pthread argument; ignored.
 * @return NULL after the scanner stop request.
 */
static void *urp_link_scanner(void *unused)
{
	(void)unused;
	while (!atomic_load_explicit(&link_scan_stop, memory_order_acquire)) {
		ast_mutex_lock(&reload_lock);
		ast_mutex_lock(&link_scan_lock);
		urp_scan_links();
		ast_mutex_unlock(&link_scan_lock);
		ast_mutex_unlock(&reload_lock);
		usleep(URP_LINK_SCAN_INTERVAL_US);
	}
	return NULL;
}

/**
 * @brief Print counters for each currently attached incoming-link graph.
 * @param fd Asterisk CLI output descriptor.
 */
static void urp_print_link_statistics(int fd)
{
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	struct ast_channel *channel;
	int found = 0;

	if (!iterator)
		return;
	while ((channel = ast_channel_iterator_next(iterator))) {
		struct urp_ast_link_observation observation = {
			.struct_size = sizeof(observation),
			.abi_version = URP_AST_ABI_VERSION,
		};
		struct ast_datastore *datastore;
		struct urp_link_hook *hook;

		ast_channel_lock(channel);
		datastore = ast_channel_datastore_find(channel, &urp_link_datastore, NULL);
		hook = datastore ? datastore->data : NULL;
		if (hook && hook->rust_link) {
			ast_audiohook_lock(&hook->audiohook);
			if (rust_adapter->link_observe(hook->rust_link, &observation) ==
			    URP_AST_OK) {
				ast_cli(fd,
					"%s/link: processed=%" PRIu64 " bypassed=%" PRIu64
					" failed=%" PRIu64 " blocks\n",
					hook->asterisk_channel, observation.processed_blocks,
					observation.bypassed_blocks, observation.failed_blocks);
				found = 1;
			}
			ast_audiohook_unlock(&hook->audiohook);
		}
		ast_channel_unlock(channel);
		ast_channel_unref(channel);
	}
	ast_channel_iterator_destroy(iterator);
	if (!found)
		ast_cli(fd, "No USBRadioPlus link-processing hook is attached.\n");
}

/**
 * @brief Read one complete status snapshot from the active live channel.
 * @param status Caller-initialized portable status output.
 * @param name Receives the allocated active channel name.
 * @return Portable ABI result.
 */
static int urp_get_active_status(struct urp_ast_channel_status *status, char **name)
{
	struct urp_control_task task = {
		.operation = URP_CONTROL_STATUS,
		.argument.status = status,
	};
	struct urp_channel *channel;
	int result;

	channel = urp_lock_active_channel(name, &result);
	if (!channel)
		return result;
	result = urp_control_run(channel, &task);
	urp_unlock_channel();
	return result;
}

/**
 * @brief Read and print one complete machine-readable channel status snapshot.
 * @param fd Asterisk CLI output descriptor.
 * @return Portable ABI result.
 */
static int urp_print_status(int fd)
{
	struct urp_ast_channel_status status = {
		.struct_size = sizeof(status),
		.abi_version = URP_AST_ABI_VERSION,
	};
	char *name = NULL;
	int result = urp_get_active_status(&status, &name);

	if (result == URP_AST_OK) {
		ast_cli(fd,
			"status_abi=%u\n"
			"channel=%s\n"
			"transport=%u\n"
			"running=%u\n"
			"echo_enabled=%u\n"
			"dtmf_enabled=%u\n"
			"receive_handoff_available=%" PRIu64 "\n"
			"receive_handoff_discarded=%" PRIu64 "\n"
			"program_handoff_available=%" PRIu64 "\n"
			"program_handoff_discarded=%" PRIu64 "\n"
			"receive_input_peak=%.9g\n"
			"receive_input_rms=%.9g\n"
			"receive_output_peak=%.9g\n"
			"receive_output_rms=%.9g\n"
			"receive_input_rail_samples=%" PRIu64 "\n"
			"receive_output_rail_samples=%" PRIu64 "\n"
			"receive_ctcss_decoder_peak=%.9g\n"
			"receive_rssi_peak=%" PRId32 "\n"
			"receive_rssi_updated=%u\n"
			"transmit_program_peak=%.9g\n"
			"transmit_program_rms=%.9g\n"
			"transmit_output_peak=%.9g\n"
			"transmit_output_rms=%.9g\n"
			"input_overflow_count=%" PRIu64 "\n"
			"output_underflow_count=%" PRIu64 "\n"
			"input_clip_sample_count=%" PRIu64 "\n"
			"output_clip_sample_count=%" PRIu64 "\n"
			"callback_last_duration_ns=%" PRIu64 "\n"
			"callback_max_duration_ns=%" PRIu64 "\n"
			"callback_last_start_delay_ns=%" PRIu64 "\n"
			"callback_max_start_delay_ns=%" PRIu64 "\n"
			"callback_late_start_count=%" PRIu64 "\n"
			"last_input_xrun_monotonic_ns=%" PRIu64 "\n"
			"last_output_xrun_monotonic_ns=%" PRIu64 "\n"
			"input_latency_seconds=%.17g\n"
			"output_latency_seconds=%.17g\n"
			"native_sample_rate_hz=%.17g\n"
			"ring_occupancy_frames=%u\n"
			"ring_reserve_frames=%u\n"
			"ring_target_frames=%u\n"
			"ring_capacity_frames=%u\n"
			"ring_ratio=%.17g\n"
			"ring_underrun_samples=%" PRIu64 "\n"
			"ring_overrun_samples=%" PRIu64 "\n"
			"ring_concealment_samples=%" PRIu64 "\n"
			"carrier_active=%u\n"
			"subaudible_active=%u\n"
			"receiver_keyed=%u\n"
			"logical_ptt=%u\n"
			"ctcss_decode_index=%" PRId32 "\n"
			"dcs_valid=%u\n"
			"receive_mixer_level=%u\n"
			"transmit_a_mixer_level=%u\n"
			"transmit_b_mixer_level=%u\n",
			URP_AST_ABI_VERSION, name, status.transport, status.running,
			status.echo_enabled, status.dtmf_enabled, status.receive_handoff_available,
			status.receive_handoff_discarded, status.program_handoff_available,
			status.program_handoff_discarded, (double)status.receive_input_peak,
			(double)status.receive_input_rms, (double)status.receive_output_peak,
			(double)status.receive_output_rms, status.receive_input_rail_samples,
			status.receive_output_rail_samples,
			(double)status.receive_ctcss_decoder_peak, status.receive_rssi_peak,
			status.receive_rssi_updated, (double)status.transmit_program_peak,
			(double)status.transmit_program_rms, (double)status.transmit_output_peak,
			(double)status.transmit_output_rms, status.input_overflow_count,
			status.output_underflow_count, status.input_clip_sample_count,
			status.output_clip_sample_count, status.callback_last_duration_ns,
			status.callback_max_duration_ns, status.callback_last_start_delay_ns,
			status.callback_max_start_delay_ns, status.callback_late_start_count,
			status.last_input_xrun_monotonic_ns, status.last_output_xrun_monotonic_ns,
			status.input_latency_seconds, status.output_latency_seconds,
			status.native_sample_rate_hz, status.ring_occupancy_frames,
			status.ring_reserve_frames, status.ring_target_frames,
			status.ring_capacity_frames, status.ring_ratio,
			status.ring_underrun_samples, status.ring_overrun_samples,
			status.ring_concealment_samples, status.carrier_active,
			status.subaudible_active, status.receiver_keyed, status.logical_ptt,
			status.ctcss_decode_index, status.dcs_valid, status.receive_mixer_level,
			status.transmit_a_mixer_level, status.transmit_b_mixer_level);
	}
	ast_free(name);
	return result;
}

/**
 * @brief Parse one bounded unsigned decimal CLI value without truncation.
 * @param text NUL-terminated decimal input.
 * @param maximum Largest accepted value.
 * @param value Receives the parsed value.
 * @return Zero on success, or -1 for invalid/out-of-range input.
 */
static int urp_parse_unsigned(const char *text, uint32_t maximum, uint32_t *value)
{
	char *end;
	unsigned long parsed;

	if (ast_strlen_zero(text) || *text == '-')
		return -1;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *end || parsed > maximum)
		return -1;
	*value = (uint32_t)parsed;
	return 0;
}

/**
 * @brief Run one serialized channel operation against the active live channel.
 * @param task Caller-initialized control request.
 * @return Portable ABI result.
 */
static int urp_run_active_control(struct urp_control_task *task)
{
	struct urp_channel *channel;
	char *name = NULL;
	int result;

	channel = urp_lock_active_channel(&name, &result);
	if (channel) {
		result = urp_control_run(channel, task);
		urp_unlock_channel();
	}
	ast_free(name);
	return result;
}

/**
 * @brief Run one typed command against the active live channel.
 * @param command Caller-initialized portable command/result.
 * @return Portable ABI result.
 */
static int urp_run_command(struct urp_ast_channel_command *command)
{
	struct urp_control_task task = {
		.operation = URP_CONTROL_COMMAND,
		.argument.command = command,
	};

	return urp_run_active_control(&task);
}

/**
 * @brief Translate one stable mixer CLI name to the portable target identifier.
 * @param name Mixer name from the CLI.
 * @return Portable mixer identifier, or zero for an unknown name.
 */
static uint32_t urp_mixer_target(const char *name)
{
	if (!strcasecmp(name, "receive"))
		return URP_AST_MIXER_RECEIVE;
	if (!strcasecmp(name, "transmit-a"))
		return URP_AST_MIXER_TRANSMIT_A;
	if (!strcasecmp(name, "transmit-b"))
		return URP_AST_MIXER_TRANSMIT_B;
	return 0;
}

/**
 * @brief Parse the 64 comma-separated words used by the typed EEPROM command.
 * @param text Comma-separated unsigned words.
 * @param words Receives exactly 64 parsed words.
 * @return Zero on success, or -1 for malformed input/allocation failure.
 */
static int urp_parse_eeprom_words(const char *text, uint16_t words[64])
{
	char *copy = ast_strdup(text);
	char *cursor = copy;
	uint32_t parsed;
	unsigned int index;
	int result = -1;

	if (!copy)
		return -1;
	for (index = 0; index < 64U; ++index) {
		const char *word = strsep(&cursor, ",");

		if (!word || urp_parse_unsigned(word, UINT16_MAX, &parsed))
			goto done;
		words[index] = (uint16_t)parsed;
	}
	if (!cursor)
		result = 0;

done:
	ast_free(copy);
	return result;
}

/**
 * @brief Print the complete EEPROM image without an Asterisk-specific encoding.
 * @param fd Asterisk CLI output descriptor.
 * @param command Completed EEPROM command/result.
 */
static void urp_print_eeprom(int fd, const struct urp_ast_channel_command *command)
{
	unsigned int index;

	ast_cli(fd, "flags=%u\nwords=", command->flags);
	for (index = 0; index < 64U; ++index)
		ast_cli(fd, "%s%u", index ? "," : "", command->eeprom_words[index]);
	ast_cli(fd, "\n");
}

/**
 * @brief Select or report the channel used by live tuning commands.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_active(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	int result;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus active";
		entry->usage = "Usage: radioplus active [channel-name]\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc == 2) {
		char *name = urp_driver_channel_name(0, 1, &result);
		if (!name) {
			ast_cli(args->fd, "No active USB radio channel is available (%d).\n",
				result);
			return CLI_FAILURE;
		}
		ast_cli(args->fd, "Active USB Radio device is [%s].\n", name);
		ast_free(name);
		return CLI_SUCCESS;
	}
	if (args->argc != 3 || strlen(args->argv[2]) > UINT32_MAX)
		return CLI_SHOWUSAGE;
	result = rust_adapter->driver_set_active_channel(
		rust_driver, (const uint8_t *)args->argv[2], (uint32_t)strlen(args->argv[2]));
	if (result != URP_AST_OK) {
		ast_cli(args->fd, "Unable to select USB radio channel %s (%d).\n", args->argv[2],
			result);
		return CLI_FAILURE;
	}
	ast_cli(args->fd, "Active radio set to [%s].\n", args->argv[2]);
	return CLI_SUCCESS;
}

/**
 * @brief Print every configured channel name.
 * @param fd Asterisk CLI output descriptor.
 * @return Portable ABI result.
 */
static int urp_print_channel_list(int fd)
{
	uint32_t index;
	int result = URP_AST_OK;
	int printed = 0;

	for (index = 0;; ++index) {
		char *name = urp_driver_channel_name(index, 0, &result);

		if (!name)
			break;
		ast_cli(fd, "%s%s", printed ? "," : "", name);
		printed = 1;
		ast_free(name);
	}
	ast_cli(fd, "\n");
	return result == URP_AST_CHANNEL_NOT_FOUND ? URP_AST_OK : result;
}

/**
 * @brief List configured channels from Rust's immutable configuration snapshot.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_list(struct ast_cli_entry *entry, int command,
				  struct ast_cli_args *args)
{

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus channel list";
		entry->usage = "Usage: radioplus channel list\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3)
		return CLI_SHOWUSAGE;
	return urp_print_channel_list(args->fd) == URP_AST_OK ? CLI_SUCCESS : CLI_FAILURE;
}

/**
 * @brief Report one typed active-channel snapshot, optionally continuously.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_status(struct ast_cli_entry *entry, int command,
				    struct ast_cli_args *args)
{
	struct pollfd input = {
		.fd = args->fd,
		.events = POLLIN,
	};
	int follow;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus channel status";
		entry->usage = "Usage: radioplus channel status [follow]\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3 && (args->argc != 4 || strcasecmp(args->argv[3], "follow")))
		return CLI_SHOWUSAGE;
	follow = args->argc == 4;
	do {
		if (urp_print_status(args->fd) != URP_AST_OK) {
			ast_cli(args->fd, "Active channel status is unavailable.\n");
			return CLI_FAILURE;
		}
	} while (follow && poll(&input, 1, 1000) == 0);
	return CLI_SUCCESS;
}

/**
 * @brief Read or change active-channel echo through the serialized Rust endpoint.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_echo(struct ast_cli_entry *entry, int command,
				  struct ast_cli_args *args)
{
	struct urp_ast_channel_status status = {
		.struct_size = sizeof(status),
		.abi_version = URP_AST_ABI_VERSION,
	};
	struct urp_control_task task = {
		.operation = URP_CONTROL_ECHO,
	};
	char *name = NULL;
	uint32_t enabled;
	int result;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus channel echo";
		entry->usage = "Usage: radioplus channel echo [0|1]\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc == 3) {
		result = urp_get_active_status(&status, &name);
		ast_free(name);
		if (result != URP_AST_OK)
			return CLI_FAILURE;
		ast_cli(args->fd, "echo_enabled=%u\n", status.echo_enabled);
		return CLI_SUCCESS;
	}
	if (args->argc != 4 || urp_parse_unsigned(args->argv[3], 1, &enabled))
		return CLI_SHOWUSAGE;
	task.argument.enabled = enabled;
	result = urp_run_active_control(&task);
	if (result != URP_AST_OK)
		return CLI_FAILURE;
	ast_cli(args->fd, "echo_enabled=%u\n", enabled);
	return CLI_SUCCESS;
}

/**
 * @brief Key or unkey the active transmitter through the typed Rust endpoint.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_transmit(struct ast_cli_entry *entry, int command,
				      struct ast_cli_args *args)
{
	struct urp_control_task task = {
		.operation = URP_CONTROL_TRANSMIT,
	};
	uint32_t keyed;
	uint32_t ctcss = 0;
	int result;

	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus channel transmit";
		entry->usage = "Usage: radioplus channel transmit <0|1> [ctcss-tenths-hz]\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if ((args->argc != 4 && args->argc != 5) || urp_parse_unsigned(args->argv[3], 1, &keyed) ||
	    (args->argc == 5 && urp_parse_unsigned(args->argv[4], UINT32_MAX, &ctcss)))
		return CLI_SHOWUSAGE;
	if (!keyed && ctcss)
		return CLI_SHOWUSAGE;
	task.argument.transmit.keyed = keyed;
	task.argument.transmit.ctcss_tenths_hz = ctcss;
	result = urp_run_active_control(&task);
	if (result != URP_AST_OK)
		return CLI_FAILURE;
	ast_cli(args->fd, "transmit_keyed=%u\nforced_ctcss_tenths_hz=%u\n", keyed, ctcss);
	return CLI_SUCCESS;
}

/**
 * @brief Execute one primitive hardware operation owned by the Rust adapter.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param cli_command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_command(struct ast_cli_entry *entry, int cli_command,
				     struct ast_cli_args *args)
{
	struct urp_ast_channel_command command = {
		.struct_size = sizeof(command),
		.abi_version = URP_AST_ABI_VERSION,
	};
	uint32_t value;
	int result;

	switch (cli_command) {
	case CLI_INIT:
		entry->command = "radioplus channel command";
		entry->usage = "Usage: radioplus channel command get-mixer "
			       "<receive|transmit-a|transmit-b>\n"
			       "       radioplus channel command set-mixer "
			       "<receive|transmit-a|transmit-b> <0-999>\n"
			       "       radioplus channel command test-tone <0|1>\n"
			       "       radioplus channel command eeprom-read\n"
			       "       radioplus channel command eeprom-write <flags> "
			       "<64-comma-separated-words>\n"
			       "       radioplus channel command eeprom-save-tuning\n"
			       "       radioplus channel command ctcss-inhibit <0|1>\n"
			       "       radioplus channel command subaudible-override <0|1>\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc < 4)
		return CLI_SHOWUSAGE;
	if (!strcasecmp(args->argv[3], "get-mixer") && args->argc == 5) {
		command.command = URP_AST_COMMAND_GET_MIXER;
		command.target = urp_mixer_target(args->argv[4]);
		if (!command.target)
			return CLI_SHOWUSAGE;
	} else if (!strcasecmp(args->argv[3], "set-mixer") && args->argc == 6) {
		command.command = URP_AST_COMMAND_SET_MIXER;
		command.target = urp_mixer_target(args->argv[4]);
		if (!command.target || urp_parse_unsigned(args->argv[5], 999, &value))
			return CLI_SHOWUSAGE;
		command.value = value;
	} else if (!strcasecmp(args->argv[3], "test-tone") && args->argc == 5) {
		command.command = URP_AST_COMMAND_SET_TEST_TONE;
		if (urp_parse_unsigned(args->argv[4], 1, &value))
			return CLI_SHOWUSAGE;
		command.value = value;
	} else if (!strcasecmp(args->argv[3], "eeprom-read") && args->argc == 4) {
		command.command = URP_AST_COMMAND_READ_EEPROM;
	} else if (!strcasecmp(args->argv[3], "eeprom-write") && args->argc == 6) {
		command.command = URP_AST_COMMAND_WRITE_EEPROM;
		if (urp_parse_unsigned(args->argv[4],
				       URP_AST_EEPROM_CHECKSUM_VALID | URP_AST_EEPROM_MAGIC_VALID,
				       &value) ||
		    urp_parse_eeprom_words(args->argv[5], command.eeprom_words))
			return CLI_SHOWUSAGE;
		command.flags = value;
	} else if (!strcasecmp(args->argv[3], "eeprom-save-tuning") && args->argc == 4) {
		command.command = URP_AST_COMMAND_SAVE_TUNING_EEPROM;
	} else if (!strcasecmp(args->argv[3], "ctcss-inhibit") && args->argc == 5) {
		command.command = URP_AST_COMMAND_SET_CTCSS_INHIBIT;
		if (urp_parse_unsigned(args->argv[4], 1, &value))
			return CLI_SHOWUSAGE;
		command.value = value;
	} else if (!strcasecmp(args->argv[3], "subaudible-override") && args->argc == 5) {
		command.command = URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE;
		if (urp_parse_unsigned(args->argv[4], 1, &value))
			return CLI_SHOWUSAGE;
		command.value = value;
	} else {
		return CLI_SHOWUSAGE;
	}
	result = urp_run_command(&command);
	if (result != URP_AST_OK) {
		ast_cli(args->fd, "Channel command failed (%d).\n", result);
		return CLI_FAILURE;
	}
	if (command.command == URP_AST_COMMAND_READ_EEPROM ||
	    command.command == URP_AST_COMMAND_WRITE_EEPROM) {
		urp_print_eeprom(args->fd, &command);
	} else {
		ast_cli(args->fd, "value=%" PRId64 "\n", command.value);
	}
	return CLI_SUCCESS;
}

/**
 * @brief Enable or disable Rust's calibrated test tone on the active channel.
 * @param enabled One to enable the tone, zero to disable it.
 * @return Portable ABI result.
 */
static int urp_set_test_tone(uint32_t enabled)
{
	struct urp_ast_channel_command command = {
		.struct_size = sizeof(command),
		.abi_version = URP_AST_ABI_VERSION,
		.command = URP_AST_COMMAND_SET_TEST_TONE,
		.value = enabled,
	};

	return urp_run_command(&command);
}

/**
 * @brief Key or unkey the active transmitter without a forced CTCSS selection.
 * @param keyed One to key, zero to unkey.
 * @return Portable ABI result.
 */
static int urp_set_transmit(uint32_t keyed)
{
	struct urp_control_task task = {
		.operation = URP_CONTROL_TRANSMIT,
		.argument.transmit =
			{
				.keyed = keyed,
			},
	};

	return urp_run_active_control(&task);
}

/**
 * @brief Generate the established three one-second calibration flashes.
 * @param fd Asterisk CLI descriptor used for output and cancellation input.
 * @return Portable ABI result.
 */
static int urp_flash_transmitter(int fd)
{
	struct pollfd input = {
		.fd = fd,
		.events = POLLIN,
	};
	unsigned int burst;
	int result = URP_AST_OK;

	ast_cli(fd, "USB Device Flash starting.\n");
	for (burst = 0;; ++burst) {
		result = urp_set_test_tone(1);
		if (result == URP_AST_OK)
			result = urp_set_transmit(1);
		if (result != URP_AST_OK)
			break;
		if (poll(&input, 1, 1000) != 0)
			break;
		result = urp_set_transmit(0);
		if (urp_set_test_tone(0) != URP_AST_OK && result == URP_AST_OK)
			result = URP_AST_SETUP_FAILED;
		if (result != URP_AST_OK || burst == 2U)
			break;
		if (poll(&input, 1, 1500) != 0)
			break;
	}
	(void)urp_set_transmit(0);
	(void)urp_set_test_tone(0);
	if (result == URP_AST_OK)
		ast_cli(fd, "USB Device Flash completed.\n");
	return result;
}

/**
 * @brief Flash three calibrated one-second bursts for transmitter adjustment.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_channel_flash(struct ast_cli_entry *entry, int command,
				   struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus channel flash";
		entry->usage = "Usage: radioplus channel flash\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3)
		return CLI_SHOWUSAGE;
	return urp_flash_transmitter(args->fd) == URP_AST_OK ? CLI_SUCCESS : CLI_FAILURE;
}

/**
 * @brief Print one processing-statistics snapshot for command-line users.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_processing_stats(struct ast_cli_entry *entry, int command,
				      struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing stats";
		entry->usage = "Usage: radioplus processing stats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3)
		return CLI_SHOWUSAGE;
	if (urp_print_status(args->fd) != URP_AST_OK)
		return CLI_FAILURE;
	urp_print_link_statistics(args->fd);
	return CLI_SUCCESS;
}

/**
 * @brief Reload validated Rust configuration through the Asterisk CLI.
 * @param entry Asterisk CLI entry being initialized or invoked.
 * @param command Asterisk CLI callback phase.
 * @param args Parsed Asterisk CLI arguments.
 * @return Asterisk CLI callback result.
 */
static char *urp_cli_reload(struct ast_cli_entry *entry, int command, struct ast_cli_args *args)
{
	switch (command) {
	case CLI_INIT:
		entry->command = "radioplus processing reload";
		entry->usage = "Usage: radioplus processing reload\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		break;
	}
	if (args->argc != 3)
		return CLI_SHOWUSAGE;
	if (urp_reload_configuration()) {
		ast_cli(args->fd, "Configuration reload failed; existing settings retained.\n");
		return CLI_FAILURE;
	}
	ast_cli(args->fd, "Configuration reloaded in place.\n");
	return CLI_SUCCESS;
}

/** Typed command surface used by operators and the Rust tuner. */
static struct ast_cli_entry urp_cli[] = {
	AST_CLI_DEFINE(urp_cli_active, "Select the USBRadioPlus tuning channel"),
	AST_CLI_DEFINE(urp_cli_channel_list, "List configured USBRadioPlus channels"),
	AST_CLI_DEFINE(urp_cli_channel_status, "Show typed USBRadioPlus channel status"),
	AST_CLI_DEFINE(urp_cli_channel_echo, "Read or change channel echo"),
	AST_CLI_DEFINE(urp_cli_channel_transmit, "Key or unkey the channel transmitter"),
	AST_CLI_DEFINE(urp_cli_channel_flash, "Flash the channel transmitter"),
	AST_CLI_DEFINE(urp_cli_channel_command, "Run a typed USBRadioPlus hardware command"),
	AST_CLI_DEFINE(urp_cli_processing_stats, "Show USBRadioPlus processing statistics"),
	AST_CLI_DEFINE(urp_cli_reload, "Reload USBRadioPlus configuration"),
};

/**
 * @brief Validate the complete Rust descriptor before exposing either technology.
 * @param adapter Candidate process-lifetime descriptor.
 * @return Nonzero only when the ABI and every required operation are available.
 */
static int urp_adapter_valid(const struct urp_ast_descriptor *adapter)
{
	return adapter && adapter->struct_size >= sizeof(*adapter) &&
	       adapter->abi_version == URP_AST_ABI_VERSION && adapter->driver_create &&
	       adapter->driver_reload && adapter->driver_reload_finish &&
	       adapter->driver_channel_name && adapter->driver_active_channel &&
	       adapter->driver_set_active_channel && adapter->driver_destroy &&
	       adapter->link_prepare && adapter->link_prepare_reload && adapter->link_process &&
	       adapter->link_observe && adapter->link_destroy && adapter->channel_reserve &&
	       adapter->channel_start && adapter->channel_stop && adapter->channel_reload_prepare &&
	       adapter->channel_reload_activate && adapter->channel_reload_finish &&
	       adapter->channel_write_voice && adapter->channel_write_text &&
	       adapter->channel_set_transmit && adapter->channel_set_dtmf &&
	       adapter->channel_set_echo && adapter->channel_get_jitter_config &&
	       adapter->channel_command && adapter->channel_get_status &&
	       adapter->channel_service && adapter->channel_destroy;
}

/**
 * @brief Build the Rust driver from the installed providers and current config.
 * @return Zero on success, or -1 on read, validation, or construction failure.
 */
static int urp_create_driver(void)
{
	static const struct urp_ast_operations operations = {
		.struct_size = sizeof(operations),
		.abi_version = URP_AST_ABI_VERSION,
		.queue_voice = urp_queue_voice,
		.queue_control = urp_queue_control,
		.queue_text = urp_queue_text,
		.analyze_dtmf = urp_analyze_dtmf,
		.monotonic_milliseconds = urp_monotonic_milliseconds,
		.log = urp_log,
	};
	const struct urp_ast_provider_manifest providers = {
		.struct_size = sizeof(providers),
		.abi_version = URP_AST_ABI_VERSION,
		.ffmpeg = rptadv_ffmpeg_adapter_descriptor(),
		.rnnoise = rptadv_rnnoise_adapter_descriptor(),
		.ring = rpcr2_descriptor(),
		.radio = rptadv_radio_descriptor(),
		.samplerate = rptadv_samplerate_adapter_descriptor(),
		.audio = rptadv_portaudio_alsa_adapter_descriptor(),
		.gpio = rptadv_gpio_adapter_descriptor(),
	};
	struct urp_ast_driver_create_args args = {
		.struct_size = sizeof(args),
		.abi_version = URP_AST_ABI_VERSION,
		.agc_plugin_path = (const uint8_t *)URP_AGC_PLUGIN_PATH,
		.agc_plugin_path_length = sizeof(URP_AGC_PLUGIN_PATH) - 1U,
		.operations = &operations,
		.providers = &providers,
	};
	char *source = NULL;
	char *text = urp_read_configuration(&source);
	int result;

	if (!text) {
		ast_log(LOG_ERROR, "Unable to read %s\n", source ? source : URP_CONFIG_FILE);
		ast_free(source);
		return -1;
	}
	if (strlen(source) > UINT32_MAX || strlen(text) > UINT32_MAX) {
		ast_log(LOG_ERROR, "%s is too large for the USBRadioPlus ABI\n", source);
		ast_free(text);
		ast_free(source);
		return -1;
	}
	args.config_source = (const uint8_t *)source;
	args.config_source_length = (uint32_t)strlen(source);
	args.config_text = (const uint8_t *)text;
	args.config_text_length = (uint32_t)strlen(text);
	result = rust_adapter->driver_create(&args, &rust_driver);
	ast_free(text);
	ast_free(source);
	return result == URP_AST_OK ? 0 : -1;
}

/**
 * @brief Allocate the one fixed PCM capability for a technology.
 * @param technology Technology whose capability set is initialized.
 * @param format Sole signed-linear format exposed by the technology.
 * @return Zero on success, or -1 on allocation/append failure.
 */
static int urp_prepare_capability(struct ast_channel_tech *technology, struct ast_format *format)
{
	technology->capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!technology->capabilities)
		return -1;
	if (ast_format_cap_append(technology->capabilities, format, 0)) {
		ao2_cleanup(technology->capabilities);
		technology->capabilities = NULL;
		return -1;
	}
	return 0;
}

/**
 * @brief Load and register the Rust-backed channel technologies.
 * @return An Asterisk module-load result.
 */
static int load_module(void)
{
	struct ast_format *advanced_format;
	int app_registered = 0;

	rust_adapter = usbradioplus_asterisk_descriptor();
	if (!urp_adapter_valid(rust_adapter)) {
		ast_log(LOG_ERROR, "USBRadioPlus Rust adapter ABI is unavailable\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	if (urp_create_driver())
		return AST_MODULE_LOAD_DECLINE;
	advanced_format = ast_format_cache_get_slin_by_rate(URP_ADVANCED_RATE);
	if (!advanced_format || ast_format_get_sample_rate(advanced_format) != URP_ADVANCED_RATE ||
	    urp_prepare_capability(&app_rpt_tech, ast_format_slin) ||
	    urp_prepare_capability(&advanced_tech, advanced_format))
		goto failed;
	if (ast_channel_register(&app_rpt_tech))
		goto failed;
	app_registered = 1;
	if (ast_channel_register(&advanced_tech))
		goto failed;
	if (ast_cli_register_multiple(urp_cli, ARRAY_LEN(urp_cli))) {
		ast_channel_unregister(&advanced_tech);
		goto failed;
	}
	atomic_store_explicit(&link_scan_stop, 0, memory_order_release);
	if (ast_pthread_create_background(&link_scan_thread, NULL, urp_link_scanner, NULL)) {
		ast_cli_unregister_multiple(urp_cli, ARRAY_LEN(urp_cli));
		ast_channel_unregister(&advanced_tech);
		goto failed;
	}
	atomic_store_explicit(&link_scan_running, 1, memory_order_release);
	return AST_MODULE_LOAD_SUCCESS;

failed:
	if (app_registered)
		ast_channel_unregister(&app_rpt_tech);
	ao2_cleanup(advanced_tech.capabilities);
	advanced_tech.capabilities = NULL;
	ao2_cleanup(app_rpt_tech.capabilities);
	app_rpt_tech.capabilities = NULL;
	rust_adapter->driver_destroy(rust_driver);
	rust_driver = NULL;
	return AST_MODULE_LOAD_FAILURE;
}

/**
 * @brief Unregister and destroy an idle Rust-backed module.
 * @return Zero on success, or -1 while a channel remains active.
 */
static int unload_module(void)
{
	if (atomic_load_explicit(&active_channels, memory_order_acquire))
		return -1;
	if (atomic_exchange_explicit(&link_scan_running, 0, memory_order_acq_rel)) {
		atomic_store_explicit(&link_scan_stop, 1, memory_order_release);
		pthread_join(link_scan_thread, NULL);
	}
	ast_mutex_lock(&link_scan_lock);
	urp_detach_all_links();
	ast_mutex_unlock(&link_scan_lock);
	ast_cli_unregister_multiple(urp_cli, ARRAY_LEN(urp_cli));
	ast_channel_unregister(&advanced_tech);
	ast_channel_unregister(&app_rpt_tech);
	ao2_cleanup(advanced_tech.capabilities);
	advanced_tech.capabilities = NULL;
	ao2_cleanup(app_rpt_tech.capabilities);
	app_rpt_tech.capabilities = NULL;
	rust_adapter->driver_destroy(rust_driver);
	rust_driver = NULL;
	return 0;
}

/**
 * @brief Validate and publish a new configuration generation.
 * @return Zero on success, or -1 when validation fails.
 */
static int reload_module(void)
{
	return urp_reload_configuration();
}

// cppcheck-suppress unknownMacro -- expanded only by Asterisk's build headers.
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "USBRadioPlus channel driver",
		.support_level = AST_MODULE_SUPPORT_EXTENDED, .load = load_module,
		.unload = unload_module, .reload = reload_module, .requires = "");
