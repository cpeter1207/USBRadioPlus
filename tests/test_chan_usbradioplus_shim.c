/** @file
 * @brief Host-stub checks for the Asterisk ABI shim.
 */

#include "asterisk.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

#include "asterisk/audiohook.h"
#include "asterisk/datastore.h"
#include "shim_test_internals.h"
#include "usbradioplus_asterisk.h"

#define TEST_APP_RPT_RATE UINT32_C(8000)
#define TEST_ADVANCED_RATE UINT32_C(48000)

static const char *forced_length_text;
static size_t forced_length;
static int force_all_lengths;
static unsigned int force_length_call;
static unsigned int shim_strlen_calls;
size_t __real_strlen(const char *text);

enum fake_operation {
	FAKE_DRIVER_CREATE,
	FAKE_DRIVER_RELOAD,
	FAKE_DRIVER_RELOAD_FINISH,
	FAKE_SET_ACTIVE,
	FAKE_LINK_PREPARE,
	FAKE_LINK_PREPARE_RELOAD,
	FAKE_LINK_PROCESS,
	FAKE_LINK_OBSERVE,
	FAKE_RESERVE,
	FAKE_START,
	FAKE_STOP,
	FAKE_RELOAD_PREPARE,
	FAKE_RELOAD_ACTIVATE,
	FAKE_RELOAD_FINISH,
	FAKE_VOICE,
	FAKE_TEXT,
	FAKE_TRANSMIT,
	FAKE_DTMF,
	FAKE_ECHO,
	FAKE_JITTER,
	FAKE_COMMAND,
	FAKE_STATUS,
	FAKE_SERVICE,
	FAKE_OPERATION_COUNT,
};

static int fake_results[FAKE_OPERATION_COUNT];
static unsigned int fake_fail_call[FAKE_OPERATION_COUNT];
static int fake_fail_result[FAKE_OPERATION_COUNT];
static enum fake_operation last_fake_operation;
static unsigned int fake_calls[FAKE_OPERATION_COUNT];
static struct urp_ast_jitter_config fake_jitter;
static struct urp_ast_channel_status fake_status;
static struct urp_ast_channel_command fake_command;
static const char *fake_channel_names[] = {"alpha", "bravo"};
static const char *fake_active_name = "alpha";
static struct ast_frame queued_frame;
static char queued_text[128];
static int queued_frames;
static int fail_calloc;
static int fail_malloc;
static int fail_strdup;
static int fail_asprintf;
static int fail_channel_allocation;
static int fail_compatible;
static int fail_dsp_allocation;
static int fail_format_allocation;
static int fail_format_append;
static int fail_format_allocation_call;
static int format_allocation_calls;
static int fail_format_append_call;
static int format_append_calls;
static int fail_queue_frame;
static int fail_registration_call;
static int registration_calls;
static int fail_cli_registration;
static int fail_taskprocessor_get;
static int fail_taskprocessor_push;
static int fail_thread_create;
static int fail_trylock;
static int swap_owner_during_lock;
static int fail_sem_init;
static int sem_wait_interrupts;
static int sem_wait_errors;
static int dsp_result_type = AST_FRAME_VOICE;
static int dsp_result_digit;
static int dsp_returns_null;
static int dsp_returns_copy;
static int fail_clock;
static int stop_service_after_call = 1;
static int name_query_result;
static int name_copy_result;
static int zero_name_length;
static uint32_t channel_name_error_index = UINT32_MAX;
static int channel_name_error_result;
static int advanced_format_unavailable;
static unsigned int advanced_format_rate = TEST_ADVANCED_RATE;
static int poll_results[8];
static size_t poll_index;
static unsigned int thread_create_calls;
static unsigned int thread_join_calls;
static unsigned int unregister_calls;
static unsigned int driver_destroy_calls;
static unsigned int channel_destroy_calls;
static unsigned int link_destroy_calls;
static struct urp_ast_link_observation fake_link_observation;
static struct ast_channel *iterated_channels[8];
static size_t iterated_channel_count;
static int fail_iterator;
static int fail_datastore_allocation;
static int fail_audiohook_init;
static int fail_audiohook_attach;
static unsigned int fail_rwlock_tryrdlock_calls;
static unsigned int audiohook_attach_calls;
static unsigned int audiohook_detach_calls;
static unsigned int hangup_calls;
static unsigned int moh_start_calls;
static unsigned int moh_stop_calls;
static unsigned int jitter_configure_calls;
static struct ast_channel *mutex_order_owner;
static int observed_owner_unlock;
static int owner_unlocked_before_first_mutex;
static unsigned int observed_mutex_locks;
static int mutex_depth;
static int channel_destroy_mutex_depth;
static unsigned int log_calls[4];
static uint32_t last_transmit_keyed;
static uint32_t last_transmit_ctcss;
static uint32_t last_enabled;
static void *captured_thread_data;
static void *(*captured_thread_start)(void *);
static struct urp_channel *owner_swap_channel;
static char cli_output[32768];
static size_t cli_output_length;
static const char *configuration_text = "[general]\n";
static int configuration_missing;
static struct ast_channel *reload_probe_owner;
static int reload_probe_text_result;
static int reload_probe_service_result;
static int reload_probe_key_result;
static int reload_probe_unkey_result;
static unsigned int reload_probe_transmit_calls;
static struct ast_jb_conf applied_jitter;
static struct ast_format app_format = {.sample_rate = TEST_APP_RPT_RATE};
static struct ast_format advanced_format = {.sample_rate = TEST_ADVANCED_RATE};
struct ast_format *ast_format_slin = &app_format;
struct ast_frame ast_null_frame;
const char *ast_config_AST_CONFIG_DIR = "/etc/asterisk";
static struct ast_module fake_module;
static struct ast_module_info_fixture fake_module_info = {.self = &fake_module};
struct ast_module_info_fixture *ast_module_info = &fake_module_info;
static struct ast_taskprocessor fake_taskprocessor;
static struct ast_dsp fake_dsp;
static struct urp_ast_descriptor fake_adapter;
static const struct urp_ast_descriptor *descriptor_override = &fake_adapter;
static const struct ast_channel_tech *registered_technologies[2];
static struct ast_cli_entry *registered_cli;
static int registered_cli_count;

size_t __wrap_strlen(const char *text)
{
	shim_strlen_calls++;
	return force_all_lengths || shim_strlen_calls == force_length_call ||
			       text == forced_length_text
		       ? forced_length
		       : __real_strlen(text);
}

static int fake_result(enum fake_operation operation)
{
	last_fake_operation = operation;
	fake_calls[operation]++;
	if (fake_calls[operation] == fake_fail_call[operation])
		return fake_fail_result[operation];
	return fake_results[operation];
}

static int fake_driver_create(const struct urp_ast_driver_create_args *args, void **output)
{
	int result;

	assert(args && args->operations && args->providers);
	result = fake_result(FAKE_DRIVER_CREATE);
	if (!result)
		*output = &fake_adapter;
	return result;
}

static int fake_driver_reload(void *driver, const uint8_t *source, uint32_t source_length,
			      const uint8_t *text, uint32_t text_length)
{
	struct urp_control_task task = {.operation = URP_CONTROL_SERVICE};
	struct urp_channel *channel;

	(void)driver;
	assert(source && source_length && text && text_length);
	if (reload_probe_owner) {
		channel = ast_channel_tech_pvt(reload_probe_owner);
		reload_probe_text_result = urp_send_text(reload_probe_owner, "during reload");
		reload_probe_service_result = urp_control_run(channel, &task);
		reload_probe_key_result =
			urp_indicate(reload_probe_owner, AST_CONTROL_RADIO_KEY, NULL, 0);
		reload_probe_unkey_result =
			urp_indicate(reload_probe_owner, AST_CONTROL_RADIO_UNKEY, NULL, 0);
		reload_probe_transmit_calls = fake_calls[FAKE_TRANSMIT];
	}
	return fake_result(FAKE_DRIVER_RELOAD);
}

static int fake_driver_reload_finish(void *driver, uint32_t commit)
{
	(void)driver;
	last_enabled = commit;
	return fake_result(FAKE_DRIVER_RELOAD_FINISH);
}

static int fake_copy_name(const char *name, uint8_t *output, uint32_t capacity,
			  uint32_t *output_length)
{
	uint32_t length = (uint32_t)strlen(name);

	*output_length = zero_name_length ? 0 : length;
	if (!output)
		return name_query_result;
	if (capacity < length)
		return URP_AST_INVALID_ARGUMENT;
	memcpy(output, name, length);
	return name_copy_result;
}

static int fake_driver_channel_name(void *driver, uint32_t index, uint8_t *output,
				    uint32_t capacity, uint32_t *output_length)
{
	(void)driver;
	if (index == channel_name_error_index)
		return channel_name_error_result;
	if (index >= ARRAY_LEN(fake_channel_names))
		return URP_AST_CHANNEL_NOT_FOUND;
	return fake_copy_name(fake_channel_names[index], output, capacity, output_length);
}

static int fake_driver_active_channel(void *driver, uint8_t *output, uint32_t capacity,
				      uint32_t *output_length)
{
	(void)driver;
	if (!fake_active_name)
		return URP_AST_CHANNEL_NOT_FOUND;
	return fake_copy_name(fake_active_name, output, capacity, output_length);
}

static int fake_driver_set_active_channel(void *driver, const uint8_t *name, uint32_t length)
{
	(void)driver;
	assert(name && length);
	return fake_result(FAKE_SET_ACTIVE);
}

static void fake_driver_destroy(void *driver)
{
	(void)driver;
	driver_destroy_calls++;
}

static int fake_link_prepare(void *driver, const uint8_t *name, uint32_t length,
			     uint32_t sample_rate, uint32_t maximum_frames, void **output)
{
	int result;

	(void)driver;
	assert(name && length && sample_rate && maximum_frames && output);
	result = fake_result(FAKE_LINK_PREPARE);
	if (!result)
		*output = &fake_adapter;
	return result;
}

static int fake_link_prepare_reload(void *driver, const uint8_t *name, uint32_t length,
				    uint32_t sample_rate, uint32_t maximum_frames, void **output)
{
	int result;

	(void)driver;
	assert(name && length && sample_rate && maximum_frames && output);
	result = fake_result(FAKE_LINK_PREPARE_RELOAD);
	if (!result)
		*output = &fake_adapter;
	return result;
}

static int fake_link_process(void *link, uint32_t direction, uint32_t sample_rate, int16_t *samples,
			     uint32_t sample_count)
{
	(void)link;
	assert(direction == URP_AST_LINK_DIRECTION_READ ||
	       direction == URP_AST_LINK_DIRECTION_WRITE);
	assert(sample_rate && samples && sample_count);
	return fake_result(FAKE_LINK_PROCESS);
}

static int fake_link_observe(void *link, struct urp_ast_link_observation *output)
{
	(void)link;
	*output = fake_link_observation;
	return fake_result(FAKE_LINK_OBSERVE);
}

static void fake_link_destroy(void *link)
{
	(void)link;
	link_destroy_calls++;
}

static int fake_channel_reserve(void *driver, const struct urp_ast_channel_reserve_args *args,
				void **output)
{
	int result;

	(void)driver;
	result = fake_result(FAKE_RESERVE);
	if (!result)
		*output = args->channel_context;
	return result;
}

static int fake_channel_start(void *channel)
{
	(void)channel;
	return fake_result(FAKE_START);
}

static int fake_channel_stop(void *channel)
{
	(void)channel;
	return fake_result(FAKE_STOP);
}

static int fake_channel_reload_prepare(void *channel)
{
	(void)channel;
	return fake_result(FAKE_RELOAD_PREPARE);
}

static int fake_channel_reload_activate(void *channel)
{
	(void)channel;
	return fake_result(FAKE_RELOAD_ACTIVATE);
}

static int fake_channel_reload_finish(void *channel, uint32_t commit)
{
	(void)channel;
	last_enabled = commit;
	return fake_result(FAKE_RELOAD_FINISH);
}

static int fake_channel_write_voice(void *channel, const int16_t *samples, uint32_t count)
{
	(void)channel;
	assert(samples && count);
	return fake_result(FAKE_VOICE);
}

static int fake_channel_write_text(void *channel, const uint8_t *text, uint32_t length)
{
	(void)channel;
	assert(text || !length);
	return fake_result(FAKE_TEXT);
}

static int fake_channel_set_transmit(void *channel, uint32_t keyed, uint32_t ctcss)
{
	(void)channel;
	last_transmit_keyed = keyed;
	last_transmit_ctcss = ctcss;
	return fake_result(FAKE_TRANSMIT);
}

static int fake_channel_set_dtmf(void *channel, uint32_t enabled)
{
	(void)channel;
	last_enabled = enabled;
	return fake_result(FAKE_DTMF);
}

static int fake_channel_set_echo(void *channel, uint32_t enabled)
{
	(void)channel;
	last_enabled = enabled;
	return fake_result(FAKE_ECHO);
}

static int fake_channel_get_jitter(void *channel, struct urp_ast_jitter_config *output)
{
	(void)channel;
	*output = fake_jitter;
	return fake_result(FAKE_JITTER);
}

static int fake_channel_command(void *channel, struct urp_ast_channel_command *command)
{
	(void)channel;
	fake_command = *command;
	command->value = 321;
	return fake_result(FAKE_COMMAND);
}

static int fake_channel_status(void *channel, struct urp_ast_channel_status *output)
{
	(void)channel;
	*output = fake_status;
	return fake_result(FAKE_STATUS);
}

static int fake_channel_service(void *channel)
{
	struct urp_channel *wrapper = channel;
	int result = fake_result(FAKE_SERVICE);

	if (!result && stop_service_after_call)
		atomic_store(&wrapper->delivery_stop, 1);
	return result;
}

static void fake_channel_destroy(void *channel)
{
	(void)channel;
	channel_destroy_mutex_depth = mutex_depth;
	channel_destroy_calls++;
}

static struct urp_ast_descriptor fake_adapter = {
	.struct_size = sizeof(fake_adapter),
	.abi_version = URP_AST_ABI_VERSION,
	.capability_name = "test",
	.driver_create = fake_driver_create,
	.driver_reload = fake_driver_reload,
	.driver_reload_finish = fake_driver_reload_finish,
	.driver_channel_name = fake_driver_channel_name,
	.driver_active_channel = fake_driver_active_channel,
	.driver_set_active_channel = fake_driver_set_active_channel,
	.driver_destroy = fake_driver_destroy,
	.link_prepare = fake_link_prepare,
	.link_prepare_reload = fake_link_prepare_reload,
	.link_process = fake_link_process,
	.link_observe = fake_link_observe,
	.link_destroy = fake_link_destroy,
	.channel_reserve = fake_channel_reserve,
	.channel_start = fake_channel_start,
	.channel_stop = fake_channel_stop,
	.channel_reload_prepare = fake_channel_reload_prepare,
	.channel_reload_activate = fake_channel_reload_activate,
	.channel_reload_finish = fake_channel_reload_finish,
	.channel_write_voice = fake_channel_write_voice,
	.channel_write_text = fake_channel_write_text,
	.channel_set_transmit = fake_channel_set_transmit,
	.channel_set_dtmf = fake_channel_set_dtmf,
	.channel_set_echo = fake_channel_set_echo,
	.channel_get_jitter_config = fake_channel_get_jitter,
	.channel_command = fake_channel_command,
	.channel_get_status = fake_channel_status,
	.channel_service = fake_channel_service,
	.channel_destroy = fake_channel_destroy,
};

const struct urp_ast_descriptor *usbradioplus_asterisk_descriptor(void)
{
	return descriptor_override;
}

const void *rpcr2_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_ffmpeg_adapter_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_gpio_adapter_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_radio_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_rnnoise_adapter_descriptor(void)
{
	return &fake_adapter;
}
const void *rptadv_samplerate_adapter_descriptor(void)
{
	return &fake_adapter;
}

void *ast_calloc(size_t count, size_t size)
{
	return fail_calloc ? NULL : calloc(count, size);
}

void *ast_malloc(size_t size)
{
	return fail_malloc ? NULL : malloc(size);
}

char *ast_strdup(const char *text)
{
	return fail_strdup ? NULL : strdup(text);
}

void ast_free(void *pointer)
{
	free(pointer);
}
void ao2_cleanup(void *object)
{
	free(object);
}

int ast_asprintf(char **output, const char *format, ...)
{
	va_list arguments;
	int result;

	if (fail_asprintf) {
		*output = NULL;
		return -1;
	}
	va_start(arguments, format);
	result = vasprintf(output, format, arguments);
	va_end(arguments);
	return result;
}

char *ast_read_textfile(const char *path)
{
	assert(path);
	return configuration_missing ? NULL : strdup(configuration_text);
}

struct ast_channel *
ast_channel_alloc(int need_queue, enum ast_channel_state state, const char *caller_number,
		  const char *caller_name, const char *account_code, const char *extension,
		  const char *context, const struct ast_assigned_ids *assigned_ids,
		  const struct ast_channel *requestor, int ama_flags, const char *name_format, ...)
{
	struct ast_channel *channel;
	char name[128];
	va_list arguments;

	(void)need_queue;
	(void)caller_number;
	(void)caller_name;
	(void)account_code;
	(void)extension;
	(void)context;
	(void)assigned_ids;
	(void)requestor;
	(void)ama_flags;
	if (fail_channel_allocation)
		return NULL;
	channel = calloc(1, sizeof(*channel));
	assert(channel);
	va_start(arguments, name_format);
	(void)vsnprintf(name, sizeof(name), name_format, arguments);
	va_end(arguments);
	channel->name = strdup(name);
	assert(channel->name);
	channel->state = state;
	channel->locked = 1;
	return channel;
}

void ast_channel_internal_fd_set(struct ast_channel *channel, int which, int value)
{
	(void)channel;
	(void)which;
	(void)value;
}

const char *ast_channel_name(const struct ast_channel *channel)
{
	return channel->name;
}

const char *ast_channel_appl(const struct ast_channel *channel)
{
	return channel->application;
}

const char *ast_channel_data(const struct ast_channel *channel)
{
	return channel->data;
}

struct ast_format *ast_channel_rawreadformat(const struct ast_channel *channel)
{
	return channel->raw_read_format;
}

void ast_channel_lock(struct ast_channel *channel)
{
	channel->locked++;
}

struct ast_channel_iterator *ast_channel_iterator_all_new(void)
{
	return fail_iterator ? NULL : calloc(1, sizeof(struct ast_channel_iterator));
}

struct ast_channel *ast_channel_iterator_next(struct ast_channel_iterator *iterator)
{
	struct ast_channel *channel;

	if (iterator->index >= iterated_channel_count)
		return NULL;
	channel = iterated_channels[iterator->index++];
	channel->references++;
	return channel;
}

void ast_channel_iterator_destroy(struct ast_channel_iterator *iterator)
{
	free(iterator);
}

void ast_channel_unref(struct ast_channel *channel)
{
	assert(channel->references);
	channel->references--;
}

struct ast_datastore *ast_datastore_alloc(const struct ast_datastore_info *info, const char *uid)
{
	struct ast_datastore *datastore;

	(void)uid;
	if (fail_datastore_allocation)
		return NULL;
	datastore = calloc(1, sizeof(*datastore));
	datastore->info = info;
	return datastore;
}

void ast_datastore_free(struct ast_datastore *datastore)
{
	if (!datastore)
		return;
	if (datastore->info && datastore->info->destroy)
		datastore->info->destroy(datastore->data);
	free(datastore);
}

struct ast_datastore *ast_channel_datastore_find(struct ast_channel *channel,
						 const struct ast_datastore_info *info,
						 const char *uid)
{
	(void)uid;
	return channel->datastore && channel->datastore->info == info ? channel->datastore : NULL;
}

void ast_channel_datastore_add(struct ast_channel *channel, struct ast_datastore *datastore)
{
	channel->datastore = datastore;
}

void ast_channel_datastore_remove(struct ast_channel *channel, struct ast_datastore *datastore)
{
	if (channel->datastore == datastore)
		channel->datastore = NULL;
}

int ast_audiohook_init(struct ast_audiohook *audiohook, enum ast_audiohook_type type,
		       const char *source, unsigned int flags)
{
	(void)type;
	(void)source;
	(void)flags;
	audiohook->status = AST_AUDIOHOOK_STATUS_NEW;
	return fail_audiohook_init;
}

int ast_audiohook_attach(struct ast_channel *channel, struct ast_audiohook *audiohook)
{
	audiohook_attach_calls++;
	if (fail_audiohook_attach)
		return -1;
	channel->audiohook = audiohook;
	audiohook->status = AST_AUDIOHOOK_STATUS_RUNNING;
	return 0;
}

void ast_audiohook_detach(struct ast_audiohook *audiohook)
{
	audiohook_detach_calls++;
	audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
}

void ast_audiohook_destroy(struct ast_audiohook *audiohook)
{
	audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
}

void ast_audiohook_lock(struct ast_audiohook *audiohook)
{
	audiohook->locked++;
}

void ast_audiohook_unlock(struct ast_audiohook *audiohook)
{
	audiohook->locked--;
}

void ast_channel_nativeformats_set(struct ast_channel *channel, struct ast_format_cap *formats)
{
	(void)channel;
	(void)formats;
}

void ast_channel_set_readformat(struct ast_channel *channel, struct ast_format *format)
{
	(void)channel;
	(void)format;
}

void ast_channel_set_writeformat(struct ast_channel *channel, struct ast_format *format)
{
	(void)channel;
	(void)format;
}

void ast_channel_tech_set(struct ast_channel *channel, const struct ast_channel_tech *technology)
{
	channel->technology = technology;
}

void *ast_channel_tech_pvt(const struct ast_channel *channel)
{
	return channel ? channel->technology_private : NULL;
}

void ast_channel_tech_pvt_set(struct ast_channel *channel, void *value)
{
	channel->technology_private = value;
}

enum ast_channel_state ast_channel_state(const struct ast_channel *channel)
{
	return channel->state;
}

int ast_channel_trylock(struct ast_channel *channel)
{
	if (fail_trylock)
		return -1;
	channel->locked = 1;
	if (swap_owner_during_lock && owner_swap_channel)
		atomic_store(&owner_swap_channel->owner, NULL);
	return 0;
}

void ast_channel_unlock(struct ast_channel *channel)
{
	if (channel == mutex_order_owner)
		observed_owner_unlock = 1;
	channel->locked = 0;
}

int ast_setstate(struct ast_channel *channel, enum ast_channel_state state)
{
	channel->state = state;
	return 0;
}

int ast_format_cap_iscompatible(const struct ast_format_cap *first,
				const struct ast_format_cap *second)
{
	(void)first;
	(void)second;
	return !fail_compatible;
}

struct ast_format_cap *ast_format_cap_alloc(int flags)
{
	(void)flags;
	format_allocation_calls++;
	return fail_format_allocation || format_allocation_calls == fail_format_allocation_call
		       ? NULL
		       : calloc(1, sizeof(struct ast_format_cap));
}

int ast_format_cap_append(struct ast_format_cap *capabilities, struct ast_format *format,
			  unsigned int framing)
{
	(void)framing;
	format_append_calls++;
	capabilities->format = format;
	return fail_format_append || format_append_calls == fail_format_append_call;
}

struct ast_format *ast_format_cache_get_slin_by_rate(unsigned int rate)
{
	advanced_format.sample_rate = advanced_format_rate;
	return rate == URP_ADVANCED_RATE && !advanced_format_unavailable ? &advanced_format : NULL;
}

unsigned int ast_format_get_sample_rate(const struct ast_format *format)
{
	return format->sample_rate;
}

int ast_channel_register(const struct ast_channel_tech *technology)
{
	registration_calls++;
	if (fail_registration_call == registration_calls)
		return -1;
	assert(registration_calls <= (int)ARRAY_LEN(registered_technologies));
	registered_technologies[registration_calls - 1] = technology;
	return 0;
}

void ast_channel_unregister(const struct ast_channel_tech *technology)
{
	for (size_t index = 0; index < ARRAY_LEN(registered_technologies); ++index) {
		if (registered_technologies[index] == technology)
			registered_technologies[index] = NULL;
	}
	unregister_calls++;
}

int ast_cli_register_multiple(struct ast_cli_entry *entries, int count)
{
	registered_cli = entries;
	registered_cli_count = count;
	return fail_cli_registration;
}

int ast_cli_unregister_multiple(struct ast_cli_entry *entries, int count)
{
	assert(!registered_cli || entries == registered_cli);
	assert(!registered_cli_count || count == registered_cli_count);
	registered_cli = NULL;
	registered_cli_count = 0;
	return 0;
}

struct ast_taskprocessor *ast_taskprocessor_get(const char *name, int reference_type)
{
	(void)name;
	(void)reference_type;
	return fail_taskprocessor_get ? NULL : &fake_taskprocessor;
}

int ast_taskprocessor_push(struct ast_taskprocessor *processor, int (*callback)(void *), void *data)
{
	(void)processor;
	if (fail_taskprocessor_push)
		return -1;
	return callback(data);
}

struct ast_taskprocessor *ast_taskprocessor_unreference(struct ast_taskprocessor *processor)
{
	(void)processor;
	return NULL;
}

int ast_sem_init(struct ast_sem *semaphore, int shared, unsigned int value)
{
	(void)shared;
	semaphore->posted = (int)value;
	return fail_sem_init;
}

int ast_sem_post(struct ast_sem *semaphore)
{
	semaphore->posted++;
	return 0;
}

int ast_sem_wait(struct ast_sem *semaphore)
{
	if (sem_wait_interrupts) {
		sem_wait_interrupts--;
		errno = EINTR;
		return -1;
	}
	if (sem_wait_errors) {
		sem_wait_errors--;
		errno = EIO;
		return -1;
	}
	if (semaphore->posted)
		semaphore->posted--;
	return 0;
}

int ast_sem_destroy(struct ast_sem *semaphore)
{
	(void)semaphore;
	return 0;
}

int ast_pthread_create_background(pthread_t *thread, const pthread_attr_t *attributes,
				  void *(*start)(void *), void *data)
{
	(void)attributes;
	thread_create_calls++;
	captured_thread_start = start;
	captured_thread_data = data;
	*thread = (pthread_t)1;
	return fail_thread_create;
}

int pthread_join(pthread_t thread, void **result)
{
	(void)thread;
	(void)result;
	thread_join_calls++;
	return 0;
}

struct ast_dsp *ast_dsp_new(void)
{
	return fail_dsp_allocation ? NULL : &fake_dsp;
}

void ast_dsp_free(struct ast_dsp *dsp)
{
	(void)dsp;
}
void ast_dsp_set_features(struct ast_dsp *dsp, int features)
{
	(void)dsp;
	(void)features;
}
int ast_dsp_set_digitmode(struct ast_dsp *dsp, int mode)
{
	(void)dsp;
	(void)mode;
	return 0;
}

struct ast_frame *ast_dsp_process(struct ast_channel *channel, struct ast_dsp *dsp,
				  struct ast_frame *frame)
{
	static struct ast_frame result;

	(void)channel;
	(void)dsp;
	if (dsp_returns_null)
		return NULL;
	if (!dsp_returns_copy)
		return frame;
	result = *frame;
	result.frametype = dsp_result_type;
	result.subclass.integer = dsp_result_digit;
	return &result;
}

void ast_frfree(struct ast_frame *frame)
{
	(void)frame;
}

int ast_queue_frame(struct ast_channel *channel, struct ast_frame *frame)
{
	(void)channel;
	queued_frame = *frame;
	if ((frame->frametype == AST_FRAME_TEXT || frame->frametype == AST_FRAME_CONTROL) &&
	    frame->data.ptr) {
		snprintf(queued_text, sizeof(queued_text), "%s", (char *)frame->data.ptr);
		queued_frame.data.ptr = queued_text;
	}
	queued_frames++;
	return fail_queue_frame;
}

void ast_hangup(struct ast_channel *channel)
{
	hangup_calls++;
	if (channel->technology_private && channel->technology && channel->technology->hangup)
		channel->technology->hangup(channel);
	free((void *)channel->name);
	free(channel);
}

struct ast_module *ast_module_ref(struct ast_module *module)
{
	return module;
}
void ast_module_unref(struct ast_module *module)
{
	(void)module;
}

int ast_moh_start(struct ast_channel *channel, const char *music_class,
		  const char *interpreter_class)
{
	(void)channel;
	(void)music_class;
	(void)interpreter_class;
	moh_start_calls++;
	return 7;
}

void ast_moh_stop(struct ast_channel *channel)
{
	(void)channel;
	moh_stop_calls++;
}

void ast_jb_conf_default(struct ast_jb_conf *configuration)
{
	memset(configuration, 0, sizeof(*configuration));
}

void ast_jb_configure(struct ast_channel *channel, const struct ast_jb_conf *configuration)
{
	(void)channel;
	applied_jitter = *configuration;
	jitter_configure_calls++;
}

void ast_copy_string(char *destination, const char *source, size_t size)
{
	if (size)
		snprintf(destination, size, "%s", source);
}

void ast_mutex_lock(ast_mutex_t *mutex)
{
	if (mutex_order_owner && !observed_mutex_locks++)
		owner_unlocked_before_first_mutex = observed_owner_unlock;
	(*mutex)++;
	mutex_depth++;
}
void ast_mutex_unlock(ast_mutex_t *mutex)
{
	(*mutex)--;
	mutex_depth--;
}

int ast_rwlock_tryrdlock(ast_rwlock_t *lock)
{
	if (fail_rwlock_tryrdlock_calls) {
		fail_rwlock_tryrdlock_calls--;
		return -1;
	}
	if (*lock < 0)
		return -1;
	(*lock)++;
	return 0;
}

void ast_rwlock_wrlock(ast_rwlock_t *lock)
{
	assert(!*lock);
	*lock = -1;
}

void ast_rwlock_unlock(ast_rwlock_t *lock)
{
	assert(*lock);
	if (*lock < 0)
		*lock = 0;
	else
		(*lock)--;
}

static void append_output(const char *format, va_list arguments)
{
	if (cli_output_length < sizeof(cli_output)) {
		int written = vsnprintf(cli_output + cli_output_length,
					sizeof(cli_output) - cli_output_length, format, arguments);
		if (written > 0)
			cli_output_length += (size_t)written;
	}
}

void ast_cli(int fd, const char *format, ...)
{
	va_list arguments;

	(void)fd;
	va_start(arguments, format);
	append_output(format, arguments);
	va_end(arguments);
}

void ast_verbose(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	append_output(format, arguments);
	va_end(arguments);
}

void ast_log(int level, const char *format, ...)
{
	va_list arguments;

	assert(level >= LOG_ERROR && level <= LOG_WARNING);
	log_calls[level]++;
	va_start(arguments, format);
	append_output(format, arguments);
	va_end(arguments);
}

int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	(void)descriptors;
	(void)count;
	(void)timeout;
	return poll_index < ARRAY_LEN(poll_results) ? poll_results[poll_index++] : 1;
}

int usleep(useconds_t microseconds)
{
	(void)microseconds;
	return 0;
}

int clock_gettime(clockid_t clock, struct timespec *time)
{
	(void)clock;
	if (fail_clock)
		return -1;
	time->tv_sec = 12;
	time->tv_nsec = 345000000;
	return 0;
}

static void reset_fakes(void)
{
	memset(fake_results, 0, sizeof(fake_results));
	memset(fake_fail_call, 0, sizeof(fake_fail_call));
	memset(fake_fail_result, 0, sizeof(fake_fail_result));
	memset(fake_calls, 0, sizeof(fake_calls));
	memset(&fake_jitter, 0, sizeof(fake_jitter));
	memset(&fake_status, 0, sizeof(fake_status));
	memset(&fake_command, 0, sizeof(fake_command));
	memset(&fake_link_observation, 0, sizeof(fake_link_observation));
	memset(iterated_channels, 0, sizeof(iterated_channels));
	memset(&queued_frame, 0, sizeof(queued_frame));
	memset(queued_text, 0, sizeof(queued_text));
	memset(poll_results, 0, sizeof(poll_results));
	memset(cli_output, 0, sizeof(cli_output));
	memset(log_calls, 0, sizeof(log_calls));
	fake_active_name = "alpha";
	configuration_text = "[general]\n";
	configuration_missing = 0;
	reload_probe_owner = NULL;
	reload_probe_text_result = 0;
	reload_probe_service_result = 0;
	reload_probe_key_result = -1;
	reload_probe_unkey_result = -1;
	reload_probe_transmit_calls = 0;
	fail_calloc = 0;
	fail_malloc = 0;
	fail_strdup = 0;
	fail_asprintf = 0;
	fail_channel_allocation = 0;
	fail_compatible = 0;
	fail_dsp_allocation = 0;
	fail_format_allocation = 0;
	fail_format_append = 0;
	fail_format_allocation_call = 0;
	format_allocation_calls = 0;
	fail_format_append_call = 0;
	format_append_calls = 0;
	fail_queue_frame = 0;
	fail_registration_call = 0;
	registration_calls = 0;
	fail_cli_registration = 0;
	fail_taskprocessor_get = 0;
	fail_taskprocessor_push = 0;
	fail_thread_create = 0;
	fail_iterator = 0;
	fail_datastore_allocation = 0;
	fail_audiohook_init = 0;
	fail_audiohook_attach = 0;
	fail_rwlock_tryrdlock_calls = 0;
	fail_trylock = 0;
	swap_owner_during_lock = 0;
	fail_sem_init = 0;
	sem_wait_interrupts = 0;
	sem_wait_errors = 0;
	dsp_result_type = AST_FRAME_VOICE;
	dsp_result_digit = 0;
	dsp_returns_null = 0;
	dsp_returns_copy = 0;
	fail_clock = 0;
	stop_service_after_call = 1;
	name_query_result = URP_AST_OK;
	name_copy_result = URP_AST_OK;
	zero_name_length = 0;
	channel_name_error_index = UINT32_MAX;
	channel_name_error_result = URP_AST_CHANNEL_NOT_FOUND;
	advanced_format_unavailable = 0;
	advanced_format_rate = URP_ADVANCED_RATE;
	poll_index = 0;
	queued_frames = 0;
	thread_create_calls = 0;
	thread_join_calls = 0;
	unregister_calls = 0;
	driver_destroy_calls = 0;
	channel_destroy_calls = 0;
	link_destroy_calls = 0;
	iterated_channel_count = 0;
	audiohook_attach_calls = 0;
	audiohook_detach_calls = 0;
	hangup_calls = 0;
	moh_start_calls = 0;
	moh_stop_calls = 0;
	jitter_configure_calls = 0;
	mutex_order_owner = NULL;
	observed_owner_unlock = 0;
	owner_unlocked_before_first_mutex = 0;
	observed_mutex_locks = 0;
	mutex_depth = 0;
	channel_destroy_mutex_depth = 0;
	last_transmit_keyed = 0;
	last_transmit_ctcss = 0;
	last_enabled = 0;
	captured_thread_data = NULL;
	captured_thread_start = NULL;
	owner_swap_channel = NULL;
	forced_length_text = NULL;
	forced_length = 0;
	force_all_lengths = 0;
	force_length_call = 0;
	shim_strlen_calls = 0;
	descriptor_override = &fake_adapter;
	cli_output_length = 0;
	channel_list = NULL;
	rust_driver = &fake_adapter;
	rust_adapter = &fake_adapter;
	atomic_store(&active_channels, 0);
	atomic_store(&link_scan_running, 0);
	atomic_store(&link_scan_stop, 0);
}

static struct ast_channel *fixture_channel(uint32_t transport)
{
	struct urp_channel *wrapper = calloc(1, sizeof(*wrapper));
	struct ast_channel *owner = calloc(1, sizeof(*owner));

	assert(wrapper && owner);
	wrapper->name = strdup("alpha");
	wrapper->rust_channel = wrapper;
	wrapper->control = &fake_taskprocessor;
	wrapper->sample_rate_hz =
		transport == URP_AST_TRANSPORT_APP_RPT ? URP_APP_RPT_RATE : URP_ADVANCED_RATE;
	wrapper->frame_samples = wrapper->sample_rate_hz * URP_FRAME_MILLISECONDS / 1000U;
	wrapper->format = transport == URP_AST_TRANSPORT_APP_RPT ? &app_format : &advanced_format;
	wrapper->dsp = transport == URP_AST_TRANSPORT_APP_RPT ? &fake_dsp : NULL;
	atomic_init(&wrapper->owner, owner);
	atomic_init(&wrapper->delivery_running, 0);
	atomic_init(&wrapper->delivery_stop, 0);
	atomic_init(&wrapper->service_failed, 0);
	atomic_init(&wrapper->jitter_pending, 0);
	atomic_init(&wrapper->pending_transmit, 0);
	owner->technology_private = wrapper;
	owner->technology = transport == URP_AST_TRANSPORT_APP_RPT ? &app_rpt_tech : &advanced_tech;
	owner->state = AST_STATE_UP;
	owner->name = transport == URP_AST_TRANSPORT_APP_RPT ? "RadioPlus/alpha"
							     : "RadioPlusAdvanced/alpha";
	owner->raw_read_format = wrapper->format;
	wrapper->next = channel_list;
	channel_list = wrapper;
	atomic_fetch_add(&active_channels, 1);
	return owner;
}

static void destroy_fixture(struct ast_channel *owner)
{
	assert(!urp_hangup(owner));
	free(owner);
}

static void test_technology_and_parsers(void)
{
	uint32_t rate = 0;
	uint32_t transport = 0;
	uint32_t value = 99;
	uint16_t words[64];
	char valid_words[384] = "0";
	size_t length = 1;
	unsigned int index;

	assert(urp_technology("RadioPlusAdvanced", &transport, &rate) == &advanced_tech);
	assert(transport == URP_AST_TRANSPORT_RPT_ADVANCED && rate == URP_ADVANCED_RATE);
	assert(urp_technology("radioplus", &transport, &rate) == &app_rpt_tech);
	assert(transport == URP_AST_TRANSPORT_APP_RPT && rate == URP_APP_RPT_RATE);
	assert(!urp_technology(NULL, &transport, &rate));
	assert(!urp_technology("unknown", &transport, &rate));

	assert(urp_parse_unsigned(NULL, 9, &value));
	assert(urp_parse_unsigned("", 9, &value));
	assert(urp_parse_unsigned("-1", 9, &value));
	assert(urp_parse_unsigned("x", 9, &value));
	assert(urp_parse_unsigned("1x", 9, &value));
	assert(urp_parse_unsigned("999999999999999999999999999999999999", UINT32_MAX, &value));
	assert(urp_parse_unsigned("10", 9, &value));
	assert(!urp_parse_unsigned("9", 9, &value) && value == 9);
	assert(urp_mixer_target("receive") == URP_AST_MIXER_RECEIVE);
	assert(urp_mixer_target("TRANSMIT-A") == URP_AST_MIXER_TRANSMIT_A);
	assert(urp_mixer_target("transmit-b") == URP_AST_MIXER_TRANSMIT_B);
	assert(!urp_mixer_target("other"));

	for (index = 1; index < 64; ++index)
		length += (size_t)snprintf(valid_words + length, sizeof(valid_words) - length,
					   ",%u", index);
	assert(!urp_parse_eeprom_words(valid_words, words));
	assert(words[0] == 0 && words[63] == 63);
	assert(urp_parse_eeprom_words("1,2", words));
	assert(urp_parse_eeprom_words("x", words));
	assert(urp_parse_eeprom_words(
		"1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,"
		"31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,"
		"58,59,60,61,62,63,64,65",
		words));
	fail_strdup = 1;
	assert(urp_parse_eeprom_words(valid_words, words));
	fail_strdup = 0;
	fake_command.flags = 3;
	memcpy(fake_command.eeprom_words, words, sizeof(words));
	urp_print_eeprom(1, &fake_command);
	assert(strstr(cli_output, "flags=3\nwords="));
}

static void test_delivery_callbacks(void)
{
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	int16_t samples[160] = {1};
	struct urp_ast_dtmf_result dtmf = {.struct_size = sizeof(dtmf)};
	const uint8_t text[] = "hello";

	assert(!urp_queue_voice(NULL, channel, samples, 160, 8000));
	assert(queued_frame.frametype == AST_FRAME_VOICE && queued_frame.samples == 160);
	assert(urp_queue_voice(NULL, NULL, samples, 160, 8000) == -1);
	assert(urp_queue_voice(NULL, channel, NULL, 160, 8000) == -1);
	assert(urp_queue_voice(NULL, channel, samples, 159, 8000) == -1);
	assert(urp_queue_voice(NULL, channel, samples, 160, 48000) == -1);
	atomic_store(&channel->owner, NULL);
	assert(!urp_queue_voice(NULL, channel, samples, 160, 8000));
	atomic_store(&channel->owner, owner);
	fail_trylock = 1;
	assert(!urp_queue_voice(NULL, channel, samples, 160, 8000));
	fail_trylock = 0;
	owner_swap_channel = channel;
	swap_owner_during_lock = 1;
	assert(!urp_queue_voice(NULL, channel, samples, 160, 8000));
	swap_owner_during_lock = 0;
	atomic_store(&channel->owner, owner);
	owner->state = AST_STATE_DOWN;
	assert(!urp_queue_voice(NULL, channel, samples, 160, 8000));
	owner->state = AST_STATE_UP;
	fail_queue_frame = 9;
	assert(urp_queue_voice(NULL, channel, samples, 160, 8000) == 9);
	fail_queue_frame = 0;

	assert(urp_queue_control(NULL, NULL, URP_AST_CONTROL_NULL, 0, 0) == -1);
	atomic_store(&channel->owner, NULL);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_NULL, 0, 0));
	atomic_store(&channel->owner, owner);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_RECEIVER_KEY, 0, 0));
	assert(queued_frame.subclass.integer == AST_CONTROL_RADIO_KEY && !queued_frame.data.ptr);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_RECEIVER_KEY, 1000, 0));
	assert(!strcmp(queued_frame.data.ptr, "100.0"));
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_RECEIVER_UNKEY, 0, 0));
	assert(queued_frame.subclass.integer == AST_CONTROL_RADIO_UNKEY);
	assert(urp_queue_control(NULL, channel, URP_AST_CONTROL_DTMF_BEGIN, 0, 0) == -1);
	assert(urp_queue_control(NULL, channel, URP_AST_CONTROL_DTMF_END, 256, 0) == -1);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_DTMF_BEGIN, '1', 20));
	assert(queued_frame.frametype == AST_FRAME_DTMF_BEGIN && queued_frame.len == 20);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_DTMF_END, '#', UINT64_MAX));
	assert(queued_frame.frametype == AST_FRAME_DTMF_END && queued_frame.len == LONG_MAX);
	assert(!urp_queue_control(NULL, channel, URP_AST_CONTROL_NULL, 0, 0));
	assert(urp_queue_control(NULL, channel, 999, 0, 0) == -1);
	fail_queue_frame = 8;
	assert(urp_queue_control(NULL, channel, URP_AST_CONTROL_NULL, 0, 0) == 8);
	fail_queue_frame = 0;

	assert(urp_queue_text(NULL, NULL, text, 5) == -1);
	assert(urp_queue_text(NULL, channel, NULL, 1) == -1);
	assert(urp_queue_text(NULL, channel, text, (uint32_t)INT_MAX) == -1);
	atomic_store(&channel->owner, NULL);
	assert(!urp_queue_text(NULL, channel, text, 5));
	atomic_store(&channel->owner, owner);
	fail_malloc = 1;
	assert(urp_queue_text(NULL, channel, text, 5) == -1);
	fail_malloc = 0;
	assert(!urp_queue_text(NULL, channel, NULL, 0));
	assert(!strcmp(queued_text, ""));
	assert(!urp_queue_text(NULL, channel, text, 5));
	assert(!strcmp(queued_text, "hello"));
	fail_queue_frame = 7;
	assert(urp_queue_text(NULL, channel, text, 5) == 7);
	fail_queue_frame = 0;

	assert(urp_analyze_dtmf(NULL, NULL, samples, 160, 8000, &dtmf) == -1);
	channel->dsp = NULL;
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf) == -1);
	channel->dsp = &fake_dsp;
	assert(urp_analyze_dtmf(NULL, channel, NULL, 160, 8000, &dtmf) == -1);
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, NULL) == -1);
	dtmf.struct_size = 0;
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf) == -1);
	dtmf.struct_size = sizeof(dtmf);
	assert(urp_analyze_dtmf(NULL, channel, samples, 159, 8000, &dtmf) == -1);
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 48000, &dtmf) == -1);
	atomic_store(&channel->owner, NULL);
	assert(!urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf));
	atomic_store(&channel->owner, owner);
	dsp_returns_null = 1;
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf) == -1);
	dsp_returns_null = 0;
	assert(!urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf));
	dsp_returns_copy = 1;
	dsp_result_type = AST_FRAME_DTMF_BEGIN;
	dsp_result_digit = '5';
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf) == 1);
	assert(dtmf.event_kind == URP_AST_DTMF_BEGIN && dtmf.digit == '5');
	dsp_result_type = AST_FRAME_DTMF_END;
	assert(urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf) == 1);
	assert(dtmf.event_kind == URP_AST_DTMF_END);
	dsp_result_type = AST_FRAME_TEXT;
	assert(!urp_analyze_dtmf(NULL, channel, samples, 160, 8000, &dtmf));

	assert(urp_monotonic_milliseconds(NULL) == 12345);
	fail_clock = 1;
	assert(!urp_monotonic_milliseconds(NULL));
	fail_clock = 0;
	urp_log(NULL, URP_AST_LOG_INFO, NULL, 1);
	urp_log(NULL, URP_AST_LOG_INFO, text, UINT32_MAX);
	urp_log(NULL, URP_AST_LOG_WARNING, text, 5);
	urp_log(NULL, URP_AST_LOG_ERROR, text, 5);
	assert(log_calls[LOG_NOTICE] && log_calls[LOG_WARNING] && log_calls[LOG_ERROR]);

	destroy_fixture(owner);
}

static void test_control_and_jitter(void)
{
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct urp_ast_jitter_config jitter = {0};
	struct urp_ast_channel_command command = {0};
	struct urp_ast_channel_status status = {0};
	struct urp_control_task task = {.channel = channel};
	enum urp_control_operation operation;

	for (operation = URP_CONTROL_START; operation <= URP_CONTROL_DESTROY; ++operation) {
		channel->rust_channel = channel;
		task.operation = operation;
		task.argument.text.text = (const uint8_t *)"x";
		task.argument.text.length = 1;
		if (operation == URP_CONTROL_JITTER)
			task.argument.jitter = &jitter;
		else if (operation == URP_CONTROL_COMMAND)
			task.argument.command = &command;
		else if (operation == URP_CONTROL_STATUS)
			task.argument.status = &status;
		assert(!urp_control_execute(&task));
	}
	channel->rust_channel = channel;
	task.operation = (enum urp_control_operation)999;
	task.result = 123;
	assert(!urp_control_execute(&task) && task.result == 123);
	channel->rust_channel = channel;
	assert(fake_calls[FAKE_START] && fake_calls[FAKE_STOP] && fake_calls[FAKE_TEXT]);
	assert(fake_calls[FAKE_TRANSMIT] && fake_calls[FAKE_DTMF] && fake_calls[FAKE_ECHO]);
	assert(fake_calls[FAKE_JITTER] && fake_calls[FAKE_COMMAND] && fake_calls[FAKE_STATUS]);
	assert(fake_calls[FAKE_SERVICE] && channel_destroy_calls);

	task.operation = URP_CONTROL_START;
	fail_sem_init = 1;
	assert(urp_control_run(channel, &task) == URP_AST_ASTERISK_FAILURE);
	fail_sem_init = 0;
	fail_taskprocessor_push = 1;
	assert(urp_control_run(channel, &task) == URP_AST_ASTERISK_FAILURE);
	fail_taskprocessor_push = 0;
	sem_wait_interrupts = 1;
	sem_wait_errors = 2;
	assert(!urp_control_run(channel, &task));
	assert(log_calls[LOG_ERROR] == 1);

	atomic_store(&channel->owner, NULL);
	assert(urp_configure_jitter(channel) == -1);
	atomic_store(&channel->owner, owner);
	fake_results[FAKE_JITTER] = URP_AST_SETUP_FAILED;
	assert(urp_configure_jitter(channel) == -1);
	fake_results[FAKE_JITTER] = URP_AST_OK;
	fake_jitter = (struct urp_ast_jitter_config){
		.enabled = 1,
		.force_enabled = 1,
		.logging_enabled = 1,
		.video_sync_enabled = 1,
		.maximum_size_ms = 500,
		.resync_threshold_ms = 1000,
		.target_extra_ms = 40,
		.implementation = URP_AST_JITTER_FIXED,
	};
	assert(!urp_configure_jitter(channel));
	assert(applied_jitter.flags ==
	       (AST_JB_ENABLED | AST_JB_FORCED | AST_JB_LOG | AST_JB_SYNC_VIDEO));
	assert(!strcmp(applied_jitter.impl, "fixed"));
	fake_jitter = (struct urp_ast_jitter_config){0};
	assert(!urp_configure_jitter(channel));
	assert(!applied_jitter.flags && !strcmp(applied_jitter.impl, "adaptive"));

	atomic_store(&channel->delivery_stop, 1);
	assert(!urp_delivery_worker(channel));
	atomic_store(&channel->delivery_stop, 0);
	atomic_store(&channel->jitter_pending, 1);
	stop_service_after_call = 1;
	assert(!urp_delivery_worker(channel));
	assert(!atomic_load(&channel->jitter_pending));
	atomic_store(&channel->delivery_stop, 0);
	atomic_store(&channel->jitter_pending, 1);
	fake_results[FAKE_JITTER] = URP_AST_SETUP_FAILED;
	assert(!urp_delivery_worker(channel));
	assert(atomic_load(&channel->jitter_pending));
	fake_results[FAKE_JITTER] = URP_AST_OK;
	atomic_store(&channel->delivery_stop, 0);
	atomic_store(&channel->jitter_pending, 0);
	fake_results[FAKE_SERVICE] = URP_AST_SETUP_FAILED;
	assert(!urp_delivery_worker(channel));
	assert(atomic_load(&channel->service_failed));
	fake_results[FAKE_SERVICE] = URP_AST_OK;
	atomic_store(&channel->delivery_stop, 0);
	atomic_store(&channel->service_failed, 0);
	fail_rwlock_tryrdlock_calls = 1;
	assert(!urp_delivery_worker(channel));
	assert(fake_calls[FAKE_SERVICE] == 5);
	assert(!atomic_load(&channel->service_failed));

	destroy_fixture(owner);

	channel = calloc(1, sizeof(*channel));
	channel->name = strdup("partial");
	channel->dsp = &fake_dsp;
	urp_abandon_channel(channel);
	channel = calloc(1, sizeof(*channel));
	channel->name = strdup("reserved");
	channel->rust_channel = channel;
	channel->control = &fake_taskprocessor;
	urp_abandon_channel(channel);
}

static void test_channel_operations(void)
{
	struct ast_channel empty = {0};
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct urp_channel *channel = ast_channel_tech_pvt(owner);
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.data.ptr = (int16_t[160]){0},
		.datalen = 320,
		.samples = 160,
	};
	uint32_t tone;
	uint8_t mode;

	assert(urp_call(&empty, NULL, 0) == -1);
	atomic_store(&channel->jitter_pending, 1);
	fake_results[FAKE_JITTER] = URP_AST_SETUP_FAILED;
	assert(urp_call(owner, NULL, 0) == -1);
	fake_results[FAKE_JITTER] = URP_AST_OK;
	fake_results[FAKE_START] = URP_AST_SETUP_FAILED;
	assert(urp_call(owner, NULL, 0) == -1);
	fake_results[FAKE_START] = URP_AST_OK;
	fail_thread_create = 1;
	assert(urp_call(owner, NULL, 0) == -1);
	fail_thread_create = 0;
	assert(!urp_call(owner, NULL, 0));
	assert(owner->state == AST_STATE_UP && atomic_load(&channel->delivery_running));
	assert(captured_thread_start == urp_delivery_worker && captured_thread_data == channel);
	assert(!urp_answer(owner));

	assert(urp_read(&empty) == NULL);
	assert(urp_read(owner) == &ast_null_frame);
	atomic_store(&channel->service_failed, 1);
	assert(urp_read(owner) == NULL);
	atomic_store(&channel->service_failed, 0);
	assert(urp_write(&empty, &frame) == -1);
	assert(urp_write(owner, NULL) == -1);
	frame.frametype = AST_FRAME_TEXT;
	assert(urp_write(owner, &frame) == -1);
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = NULL;
	assert(urp_write(owner, &frame) == -1);
	frame.data.ptr = (int16_t[160]){0};
	frame.datalen = 319;
	assert(urp_write(owner, &frame) == -1);
	frame.datalen = 318;
	assert(urp_write(owner, &frame) == -1);
	frame.datalen = 320;
	frame.samples = 159;
	assert(urp_write(owner, &frame) == -1);
	frame.samples = 160;
	fake_results[FAKE_VOICE] = URP_AST_SETUP_FAILED;
	assert(urp_write(owner, &frame) == -1);
	fake_results[FAKE_VOICE] = URP_AST_OK;
	assert(!urp_write(owner, &frame));

	assert(urp_send_text(&empty, "x") == -1);
	assert(urp_send_text(owner, NULL) == -1);
	forced_length_text = "x";
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_send_text(owner, "x") == -1);
	forced_length_text = NULL;
	fake_results[FAKE_TEXT] = URP_AST_SETUP_FAILED;
	assert(urp_send_text(owner, "x") == -1);
	fake_results[FAKE_TEXT] = URP_AST_OK;
	assert(!urp_send_text(owner, "x"));

	assert(!urp_forced_ctcss(NULL, 0, &tone) && !tone);
	assert(!urp_forced_ctcss("", 0, &tone) && !tone);
	assert(urp_forced_ctcss("12345678901234567890123456789012", 32, &tone) == -1);
	assert(urp_forced_ctcss("x", 1, &tone) == -1);
	assert(urp_forced_ctcss("1x", 2, &tone) == -1);
	assert(urp_forced_ctcss("nan", 3, &tone) == -1);
	assert(urp_forced_ctcss("-1", 2, &tone) == -1);
	assert(urp_forced_ctcss("429496730", 9, &tone) == -1);
	assert(!urp_forced_ctcss("100.05", 6, &tone) && tone == 1001);

	assert(urp_indicate(&empty, AST_CONTROL_BUSY, NULL, 0) == -1);
	assert(!urp_indicate(owner, AST_CONTROL_BUSY, NULL, 0));
	assert(!urp_indicate(owner, AST_CONTROL_CONGESTION, NULL, 0));
	assert(!urp_indicate(owner, AST_CONTROL_RINGING, NULL, 0));
	assert(!urp_indicate(owner, AST_CONTROL_VIDUPDATE, NULL, 0));
	assert(urp_indicate(owner, AST_CONTROL_HOLD, "music", 6) == 7);
	assert(!urp_indicate(owner, AST_CONTROL_UNHOLD, NULL, 0));
	assert(!urp_indicate(owner, AST_CONTROL_PROCEEDING, NULL, 0));
	assert(!urp_indicate(owner, AST_CONTROL_PROGRESS, NULL, 0));
	assert(moh_start_calls == 1 && moh_stop_calls == 3);
	assert(urp_indicate(owner, AST_CONTROL_RADIO_KEY, "bad", 3) == -1);
	assert(!urp_indicate(owner, AST_CONTROL_RADIO_KEY, "123.0", 5));
	assert(last_transmit_keyed == 1 && last_transmit_ctcss == 1230);
	assert(!urp_indicate(owner, AST_CONTROL_RADIO_UNKEY, NULL, 0));
	assert(!last_transmit_keyed && !last_transmit_ctcss);
	fake_results[FAKE_TRANSMIT] = URP_AST_SETUP_FAILED;
	assert(urp_indicate(owner, AST_CONTROL_RADIO_UNKEY, NULL, 0) == -1);
	fake_results[FAKE_TRANSMIT] = URP_AST_CHANNEL_BUSY;
	assert(urp_indicate(owner, AST_CONTROL_RADIO_UNKEY, NULL, 0) == -1);
	assert(!atomic_load(&channel->pending_transmit));
	fake_results[FAKE_TRANSMIT] = URP_AST_OK;
	assert(urp_indicate(owner, 999, NULL, 0) == -1);

	assert(urp_fixup(owner, &empty) == -1);
	empty.technology_private = channel;
	assert(!urp_fixup(owner, &empty));
	assert(atomic_load(&channel->owner) == &empty);
	atomic_store(&channel->owner, owner);
	empty.technology_private = NULL;

	errno = 0;
	assert(urp_setoption(&empty, AST_OPTION_TONE_VERIFY, &mode, 1) == -1 && errno == EINVAL);
	assert(urp_setoption(owner, AST_OPTION_TONE_VERIFY, NULL, 1) == -1 && errno == EINVAL);
	assert(urp_setoption(owner, AST_OPTION_TONE_VERIFY, &mode, 0) == -1 && errno == EINVAL);
	mode = 0;
	assert(!urp_setoption(owner, 999, &mode, 1));
	assert(!urp_setoption(owner, AST_OPTION_TONE_VERIFY, &mode, 1) && last_enabled == 1);
	mode = 3;
	assert(!urp_setoption(owner, AST_OPTION_TONE_VERIFY, &mode, 1) && last_enabled == 0);
	fake_results[FAKE_DTMF] = URP_AST_SETUP_FAILED;
	assert(urp_setoption(owner, AST_OPTION_TONE_VERIFY, &mode, 1) == -1);
	fake_results[FAKE_DTMF] = URP_AST_OK;
	assert(!urp_digit_begin(owner, '1'));
	assert(!urp_digit_end(owner, '1', 50));
	assert(strstr(cli_output, "received digit 1"));

	destroy_fixture(owner);
	assert(thread_join_calls == 1);
	assert(!urp_hangup(&empty));
}

static void test_request_and_hangup_paths(void)
{
	struct ast_format_cap requested = {.format = &app_format};
	struct ast_format_cap app_capability = {.format = &app_format};
	struct ast_format_cap advanced_capability = {.format = &advanced_format};
	struct ast_channel *owner;
	struct ast_channel *older;
	struct ast_channel *newer;
	struct urp_channel *channel;
	int cause = 0;

	app_rpt_tech.capabilities = &app_capability;
	advanced_tech.capabilities = &advanced_capability;
	assert(!urp_request("unknown", &requested, NULL, NULL, "alpha", &cause));
	assert(!urp_request("RadioPlus", NULL, NULL, NULL, "alpha", &cause));
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, NULL, &cause));
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "", &cause));
	fail_compatible = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fail_compatible = 0;
	forced_length_text = "alpha";
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	forced_length_text = NULL;
	fail_calloc = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fail_calloc = 0;
	fail_strdup = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fail_strdup = 0;
	fail_taskprocessor_get = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fail_taskprocessor_get = 0;
	fail_dsp_allocation = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fail_dsp_allocation = 0;
	fake_results[FAKE_RESERVE] = URP_AST_CHANNEL_BUSY;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	assert(cause == AST_CAUSE_BUSY);
	fake_results[FAKE_RESERVE] = URP_AST_CHANNEL_NOT_FOUND;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", NULL));
	fake_results[FAKE_RESERVE] = URP_AST_SETUP_FAILED;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	fake_results[FAKE_RESERVE] = URP_AST_OK;
	fail_channel_allocation = 1;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	assert(channel_destroy_mutex_depth > 0);
	fail_channel_allocation = 0;
	fake_results[FAKE_JITTER] = URP_AST_SETUP_FAILED;
	assert(!urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause));
	assert(hangup_calls == 1);
	fake_results[FAKE_JITTER] = URP_AST_OK;
	owner = urp_request("RadioPlus", &requested, NULL, NULL, "alpha", &cause);
	assert(owner && ast_channel_tech_pvt(owner));
	destroy_fixture(owner);
	owner = urp_request("RadioPlusAdvanced", &requested, NULL, NULL, "alpha", &cause);
	assert(owner && !((struct urp_channel *)ast_channel_tech_pvt(owner))->dsp);
	destroy_fixture(owner);

	older = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	newer = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	destroy_fixture(older);
	destroy_fixture(newer);
	older = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	newer = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	destroy_fixture(older);
	destroy_fixture(owner);
	destroy_fixture(newer);
	owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	channel = ast_channel_tech_pvt(owner);
	channel_list = NULL;
	destroy_fixture(owner);
	owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	channel = ast_channel_tech_pvt(owner);
	channel->rust_channel = NULL;
	destroy_fixture(owner);
	owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	fake_results[FAKE_STOP] = URP_AST_SETUP_FAILED;
	assert(urp_hangup(owner) == -1);
	free(owner);
	fake_results[FAKE_STOP] = URP_AST_OK;
}

static void test_hangup_drops_owner_lock_before_reload_exclusion(void)
{
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);

	owner->locked = 1;
	mutex_order_owner = owner;
	assert(!urp_hangup(owner));
	mutex_order_owner = NULL;
	assert(observed_mutex_locks && owner_unlocked_before_first_mutex);
	assert(owner->locked == 1);
	free(owner);
}

static void test_configuration_and_selection(void)
{
	struct ast_channel *owner;
	struct urp_channel *channel;
	struct urp_ast_channel_status status = {
		.struct_size = sizeof(status),
		.abi_version = URP_AST_ABI_VERSION,
	};
	struct urp_ast_channel_command command = {
		.struct_size = sizeof(command),
		.abi_version = URP_AST_ABI_VERSION,
	};
	struct urp_control_task task = {.operation = URP_CONTROL_ECHO};
	char *source = NULL;
	char *name;
	int result;

	fail_asprintf = 1;
	assert(!urp_read_configuration(&source));
	fail_asprintf = 0;
	configuration_missing = 1;
	assert(!urp_read_configuration(&source));
	free(source);
	configuration_missing = 0;
	assert((name = urp_read_configuration(&source)) != NULL);
	assert(!strcmp(name, configuration_text));
	free(name);
	free(source);

	configuration_missing = 1;
	assert(urp_reload_configuration() == -1);
	configuration_missing = 0;
	fail_asprintf = 1;
	assert(urp_reload_configuration() == -1);
	fail_asprintf = 0;
	force_all_lengths = 1;
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_reload_configuration() == -1);
	force_all_lengths = 0;
	shim_strlen_calls = 0;
	force_length_call = 2;
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_reload_configuration() == -1);
	force_length_call = 0;
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_INVALID_CONFIGURATION;
	assert(urp_reload_configuration() == -1);
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_OK;
	owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	channel = ast_channel_tech_pvt(owner);
	assert(!urp_reload_configuration());
	assert(atomic_load(&channel->jitter_pending));

	name_query_result = URP_AST_SETUP_FAILED;
	assert(!urp_driver_channel_name(0, 0, &result) && result == URP_AST_SETUP_FAILED);
	name_query_result = URP_AST_OK;
	zero_name_length = 1;
	assert(!urp_driver_channel_name(0, 0, &result));
	zero_name_length = 0;
	fail_malloc = 1;
	assert(!urp_driver_channel_name(0, 0, &result) && result == URP_AST_ASTERISK_FAILURE);
	fail_malloc = 0;
	name_copy_result = URP_AST_SETUP_FAILED;
	assert(!urp_driver_channel_name(0, 0, &result) && result == URP_AST_SETUP_FAILED);
	name_copy_result = URP_AST_OK;
	name = urp_driver_channel_name(0, 0, &result);
	assert(name && !strcmp(name, "alpha"));
	free(name);
	name = urp_driver_channel_name(0, 1, &result);
	assert(name && !strcmp(name, "alpha"));
	free(name);

	assert(urp_lock_channel("ALPHA") == channel);
	urp_unlock_channel();
	assert(!urp_lock_channel("missing"));
	fake_active_name = NULL;
	name = (char *)1;
	assert(!urp_lock_active_channel(&name, &result) && !name);
	fake_active_name = "bravo";
	assert(!urp_lock_active_channel(&name, &result));
	assert(result == URP_AST_NOT_READY && !name);
	fake_active_name = "alpha";
	assert(urp_lock_active_channel(&name, &result) == channel);
	urp_unlock_channel();
	free(name);

	fake_results[FAKE_STATUS] = URP_AST_SETUP_FAILED;
	assert(urp_get_active_status(&status, &name) == URP_AST_SETUP_FAILED);
	free(name);
	fake_results[FAKE_STATUS] = URP_AST_OK;
	fake_status.transport = URP_AST_TRANSPORT_APP_RPT;
	fake_status.ring_ratio = 1.0;
	fake_active_name = NULL;
	assert(urp_get_active_status(&status, &name) == URP_AST_CHANNEL_NOT_FOUND);
	fake_active_name = "alpha";
	assert(!urp_print_status(1));
	assert(strstr(cli_output, "channel=alpha") && strstr(cli_output, "ring_ratio=1"));
	fake_results[FAKE_STATUS] = URP_AST_SETUP_FAILED;
	assert(urp_print_status(1) == URP_AST_SETUP_FAILED);
	fake_results[FAKE_STATUS] = URP_AST_OK;

	fake_active_name = NULL;
	assert(urp_run_command(&command) == URP_AST_CHANNEL_NOT_FOUND);
	assert(urp_run_active_control(&task) == URP_AST_CHANNEL_NOT_FOUND);
	fake_active_name = "alpha";
	assert(!urp_run_command(&command));
	assert(last_fake_operation == FAKE_COMMAND);
	assert(!urp_run_active_control(&task));
	assert(last_fake_operation == FAKE_ECHO);

	assert(!urp_print_channel_list(1));
	assert(strstr(cli_output, "alpha,bravo"));
	channel_name_error_index = 1;
	channel_name_error_result = URP_AST_SETUP_FAILED;
	assert(urp_print_channel_list(1) == URP_AST_SETUP_FAILED);

	destroy_fixture(owner);
}

static void test_multi_channel_reload_transaction(void)
{
	struct ast_channel *alpha = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct ast_channel *bravo = fixture_channel(URP_AST_TRANSPORT_RPT_ADVANCED);
	struct urp_channel *bravo_wrapper = ast_channel_tech_pvt(bravo);
	struct urp_channel *alpha_wrapper = ast_channel_tech_pvt(alpha);

	free(bravo_wrapper->name);
	bravo_wrapper->name = strdup("bravo");
	assert(bravo_wrapper->name);
	fake_fail_call[FAKE_RELOAD_ACTIVATE] = 2;
	fake_fail_result[FAKE_RELOAD_ACTIVATE] = URP_AST_SETUP_FAILED;
	assert(urp_reload_configuration() == -1);
	assert(fake_calls[FAKE_DRIVER_RELOAD] == 1);
	assert(fake_calls[FAKE_RELOAD_PREPARE] == 2);
	assert(fake_calls[FAKE_RELOAD_ACTIVATE] == 2);
	assert(fake_calls[FAKE_RELOAD_FINISH] == 2 && last_enabled == 0);
	assert(fake_calls[FAKE_DRIVER_RELOAD_FINISH] == 1);
	assert(!atomic_load(&alpha_wrapper->jitter_pending));
	assert(!atomic_load(&bravo_wrapper->jitter_pending));

	fake_fail_call[FAKE_RELOAD_ACTIVATE] = 0;
	assert(!urp_reload_configuration());
	assert(fake_calls[FAKE_DRIVER_RELOAD] == 2);
	assert(fake_calls[FAKE_RELOAD_PREPARE] == 4);
	assert(fake_calls[FAKE_RELOAD_ACTIVATE] == 4);
	assert(fake_calls[FAKE_RELOAD_FINISH] == 4 && last_enabled == 1);
	assert(fake_calls[FAKE_DRIVER_RELOAD_FINISH] == 2);
	assert(atomic_load(&alpha_wrapper->jitter_pending));
	assert(atomic_load(&bravo_wrapper->jitter_pending));

	destroy_fixture(bravo);
	destroy_fixture(alpha);
}

static void test_reload_gates_control_and_replays_radio_unkey(void)
{
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct urp_channel *channel = ast_channel_tech_pvt(owner);

	reload_probe_owner = owner;
	assert(!urp_reload_configuration());
	reload_probe_owner = NULL;
	assert(reload_probe_text_result == -1);
	assert(reload_probe_service_result == URP_AST_CHANNEL_BUSY);
	assert(!reload_probe_key_result && !reload_probe_unkey_result &&
	       !reload_probe_transmit_calls);
	atomic_store(&channel->delivery_stop, 0);
	fail_rwlock_tryrdlock_calls = 1;
	assert(!urp_delivery_worker(channel));
	assert(fake_calls[FAKE_TRANSMIT] == 1 && !last_transmit_keyed);
	assert(!atomic_load(&channel->pending_transmit));
	assert(!atomic_load(&channel->service_failed));
	destroy_fixture(owner);
}

static void test_reload_prepares_new_link_before_publication(void)
{
	struct ast_channel *radio = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct ast_channel link = {
		.name = "IAX2/506316-prepare",
		.application = "Rpt",
		.data = "Remote Rx",
		.raw_read_format = &app_format,
	};
	struct urp_link_hook *hook;

	iterated_channels[0] = &link;
	iterated_channel_count = 1;
	fail_audiohook_attach = 1;
	assert(urp_reload_configuration() == -1);
	assert(!link.datastore);
	assert(fake_calls[FAKE_LINK_PREPARE_RELOAD] == 1);
	assert(fake_calls[FAKE_LINK_PREPARE] == 0);
	assert(fake_calls[FAKE_DRIVER_RELOAD_FINISH] == 1 && !last_enabled);
	assert(fake_calls[FAKE_RELOAD_FINISH] == 1 && !last_enabled);

	fail_audiohook_attach = 0;
	assert(!urp_reload_configuration());
	assert(link.datastore && link.audiohook);
	hook = link.datastore->data;
	assert(hook->rust_link && !hook->reload_link && !hook->reload_pending);
	assert(fake_calls[FAKE_LINK_PREPARE_RELOAD] == 2);
	assert(fake_calls[FAKE_LINK_PREPARE] == 0);
	assert(fake_calls[FAKE_DRIVER_RELOAD_FINISH] == 2 && last_enabled);
	urp_detach_all_links();
	destroy_fixture(radio);
}

static void test_link_reload_survives_masquerade(void)
{
	struct ast_channel *radio = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct ast_channel link = {
		.name = "IAX2/506316-before",
		.application = "Rpt",
		.data = "Remote Rx",
		.raw_read_format = &app_format,
	};
	struct ast_channel replacement = {
		.name = "IAX2/506316-after",
		.application = "Rpt",
		.data = "Remote Rx",
		.raw_read_format = &app_format,
	};
	struct urp_link_reload_entry *entries = NULL;
	struct urp_link_hook *hook;

	iterated_channels[0] = &link;
	iterated_channel_count = 1;
	urp_scan_links();
	hook = link.datastore->data;
	assert(!urp_prepare_link_reload(&entries));
	assert(hook->reload_pending && entries);
	replacement.datastore = link.datastore;
	replacement.audiohook = link.audiohook;
	link.datastore = NULL;
	link.audiohook = NULL;
	urp_finish_link_reload(entries, 1);
	assert(!hook->reload_pending && !hook->reload_link);
	assert(link_destroy_calls == 1);

	iterated_channels[0] = &replacement;
	urp_detach_all_links();
	destroy_fixture(radio);
}

static void test_link_audiohook_lifecycle(void)
{
	struct ast_channel *radio = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct ast_channel link = {
		.name = "IAX2/506316-1",
		.application = "Rpt",
		.data = "Remote Rx",
		.raw_read_format = &app_format,
	};
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = &app_format,
		.samples = 160,
		.datalen = 320,
	};
	int16_t samples[160] = {1};
	struct urp_link_hook *hook;
	struct urp_link_reload_entry *reload_entries = NULL;
	void *original_link;

	frame.data.ptr = samples;
	iterated_channels[0] = &link;
	iterated_channel_count = 1;
	urp_scan_links();
	assert(link.datastore && link.audiohook && audiohook_attach_calls == 1);
	assert(fake_calls[FAKE_LINK_PREPARE] == 1);
	hook = link.datastore->data;
	assert(hook && !strcmp(hook->profile, "alpha"));
	assert(!hook->audiohook.manipulate_callback(&hook->audiohook, &link, &frame,
						    AST_AUDIOHOOK_DIRECTION_READ));
	assert(fake_calls[FAKE_LINK_PROCESS] == 1);
	assert(!hook->audiohook.manipulate_callback(&hook->audiohook, &link, &frame,
						    AST_AUDIOHOOK_DIRECTION_WRITE));
	assert(fake_calls[FAKE_LINK_PROCESS] == 2);

	/* An existing datastore prevents duplicate graph preparation. */
	urp_scan_links();
	assert(fake_calls[FAKE_LINK_PREPARE] == 1 && audiohook_attach_calls == 1);
	fake_link_observation.processed_blocks = 7;
	fake_link_observation.bypassed_blocks = 2;
	fake_link_observation.failed_blocks = 1;
	urp_print_link_statistics(1);
	assert(strstr(cli_output, "processed=7 bypassed=2 failed=1"));

	/* A complete failed reload retains the attached processing object. */
	original_link = hook->rust_link;
	fake_results[FAKE_LINK_PREPARE_RELOAD] = URP_AST_SETUP_FAILED;
	assert(urp_reload_configuration() == -1);
	assert(hook->rust_link == original_link && link_destroy_calls == 0);
	assert(fake_calls[FAKE_RELOAD_PREPARE] == 1);
	assert(fake_calls[FAKE_RELOAD_ACTIVATE] == 0);
	assert(fake_calls[FAKE_RELOAD_FINISH] == 1);
	assert(fake_calls[FAKE_DRIVER_RELOAD_FINISH] == 1 && last_enabled == 0);
	fake_results[FAKE_LINK_PREPARE_RELOAD] = URP_AST_OK;

	/* Successful commit swaps in place without detaching the audiohook. */
	assert(!urp_prepare_link_reload(&reload_entries));
	assert(fake_calls[FAKE_LINK_PREPARE_RELOAD] == 2);
	urp_finish_link_reload(reload_entries, 1);
	reload_entries = NULL;
	assert(link_destroy_calls == 1 && fake_calls[FAKE_LINK_PREPARE] == 1);
	assert(audiohook_detach_calls == 0 && audiohook_attach_calls == 1);
	urp_detach_all_links();
	assert(!link.datastore && link_destroy_calls == 2);
	urp_print_link_statistics(1);
	assert(strstr(cli_output, "No USBRadioPlus link-processing hook is attached."));

	/* Ineligible channels and absent live profiles are clean no-ops. */
	link.application = "Other";
	urp_scan_links();
	assert(!link.datastore);
	link.application = "Rpt";
	destroy_fixture(radio);
	urp_scan_links();
	assert(!link.datastore);

	/* Callback guards bypass non-audio and quiesced frames without entering Rust. */
	radio = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	urp_scan_links();
	hook = link.datastore->data;
	memset(fake_calls, 0, sizeof(fake_calls));
	hook->audiohook.status = AST_AUDIOHOOK_STATUS_DONE;
	assert(!hook->audiohook.manipulate_callback(&hook->audiohook, &link, &frame,
						    AST_AUDIOHOOK_DIRECTION_READ));
	hook->audiohook.status = AST_AUDIOHOOK_STATUS_RUNNING;
	assert(!hook->audiohook.manipulate_callback(&hook->audiohook, &link, NULL,
						    AST_AUDIOHOOK_DIRECTION_READ));
	frame.frametype = AST_FRAME_CONTROL;
	assert(!hook->audiohook.manipulate_callback(&hook->audiohook, &link, &frame,
						    AST_AUDIOHOOK_DIRECTION_READ));
	frame.frametype = AST_FRAME_VOICE;
	assert(fake_calls[FAKE_LINK_PROCESS] == 0);
	urp_detach_all_links();
	destroy_fixture(radio);
	urp_link_hook_destroy(NULL);
}

static void assert_cli_phases(char *(*handler)(struct ast_cli_entry *, int, struct ast_cli_args *),
			      struct ast_cli_args *args)
{
	struct ast_cli_entry entry = {0};

	assert(handler(&entry, CLI_INIT, args) == NULL);
	assert(entry.command && entry.usage);
	assert(handler(&entry, CLI_GENERATE, args) == NULL);
}

static void test_cli_commands(void)
{
	struct ast_channel *owner = fixture_channel(URP_AST_TRANSPORT_APP_RPT);
	struct ast_cli_entry entry = {0};
	char *argv[7] = {"radioplus", "channel", "command", NULL, NULL, NULL, NULL};
	struct ast_cli_args args = {.fd = 1, .argc = 0, .argv = argv};
	char words[384] = "0";
	size_t length = 1;
	unsigned int index;

	for (index = 1; index < 64; ++index)
		length += (size_t)snprintf(words + length, sizeof(words) - length, ",%u", index);

	assert_cli_phases(urp_cli_active, &args);
	args.argc = 2;
	assert(urp_cli_active(&entry, 2, &args) == CLI_SUCCESS);
	fake_active_name = NULL;
	assert(urp_cli_active(&entry, 2, &args) == CLI_FAILURE);
	fake_active_name = "alpha";
	args.argc = 1;
	assert(urp_cli_active(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	argv[2] = "alpha";
	forced_length_text = "alpha";
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_cli_active(&entry, 2, &args) == CLI_SHOWUSAGE);
	forced_length_text = NULL;
	fake_results[FAKE_SET_ACTIVE] = URP_AST_SETUP_FAILED;
	assert(urp_cli_active(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_SET_ACTIVE] = URP_AST_OK;
	assert(urp_cli_active(&entry, 2, &args) == CLI_SUCCESS);

	assert_cli_phases(urp_cli_channel_list, &args);
	args.argc = 2;
	assert(urp_cli_channel_list(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	assert(urp_cli_channel_list(&entry, 2, &args) == CLI_SUCCESS);
	channel_name_error_index = 0;
	channel_name_error_result = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_list(&entry, 2, &args) == CLI_FAILURE);
	channel_name_error_index = UINT32_MAX;

	assert_cli_phases(urp_cli_channel_status, &args);
	args.argc = 4;
	argv[3] = "bad";
	assert(urp_cli_channel_status(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 2;
	assert(urp_cli_channel_status(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	fake_results[FAKE_STATUS] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_status(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_STATUS] = URP_AST_OK;
	assert(urp_cli_channel_status(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 4;
	argv[3] = "follow";
	poll_results[0] = 0;
	poll_results[1] = 1;
	poll_index = 0;
	assert(urp_cli_channel_status(&entry, 2, &args) == CLI_SUCCESS);

	assert_cli_phases(urp_cli_channel_echo, &args);
	args.argc = 3;
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_SUCCESS);
	fake_results[FAKE_STATUS] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_STATUS] = URP_AST_OK;
	args.argc = 4;
	argv[3] = "x";
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[3] = "1";
	fake_results[FAKE_ECHO] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_ECHO] = URP_AST_OK;
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 5;
	assert(urp_cli_channel_echo(&entry, 2, &args) == CLI_SHOWUSAGE);

	assert_cli_phases(urp_cli_channel_transmit, &args);
	args.argc = 3;
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 4;
	argv[3] = "x";
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[3] = "0";
	args.argc = 5;
	argv[4] = "1";
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[3] = "1";
	argv[4] = "x";
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "1230";
	fake_results[FAKE_TRANSMIT] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_TRANSMIT] = URP_AST_OK;
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 4;
	argv[3] = "0";
	assert(urp_cli_channel_transmit(&entry, 2, &args) == CLI_SUCCESS);

	assert_cli_phases(urp_cli_channel_command, &args);
	args.argc = 3;
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	{
		static const struct {
			const char *name;
			int wrong_argc;
		} wrong_sizes[] = {
			{"get-mixer", 4},     {"set-mixer", 5},		  {"test-tone", 4},
			{"eeprom-read", 5},   {"eeprom-write", 5},	  {"eeprom-save-tuning", 5},
			{"ctcss-inhibit", 4}, {"subaudible-override", 4},
		};

		for (index = 0; index < ARRAY_LEN(wrong_sizes); ++index) {
			args.argc = wrong_sizes[index].wrong_argc;
			argv[3] = (char *)wrong_sizes[index].name;
			assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
		}
	}
	args.argc = 5;
	argv[3] = "get-mixer";
	argv[4] = "bad";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "receive";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 6;
	argv[3] = "set-mixer";
	argv[4] = "transmit-a";
	argv[5] = "bad";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "bad";
	argv[5] = "999";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "transmit-b";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 5;
	argv[3] = "test-tone";
	argv[4] = "2";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "1";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 4;
	argv[3] = "eeprom-read";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 6;
	argv[3] = "eeprom-write";
	argv[4] = "4";
	argv[5] = words;
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "3";
	argv[5] = "bad";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[5] = words;
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 4;
	argv[3] = "eeprom-save-tuning";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 5;
	argv[3] = "ctcss-inhibit";
	argv[4] = "x";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "1";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	argv[3] = "subaudible-override";
	argv[4] = "x";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	argv[4] = "0";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SUCCESS);
	args.argc = 4;
	argv[3] = "bad";
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 5;
	argv[3] = "get-mixer";
	argv[4] = "receive";
	fake_results[FAKE_COMMAND] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_command(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_COMMAND] = URP_AST_OK;

	assert_cli_phases(urp_cli_channel_flash, &args);
	args.argc = 2;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	poll_index = 0;
	memset(poll_results, 0, sizeof(poll_results));
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_SUCCESS);
	fake_results[FAKE_COMMAND] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_COMMAND] = URP_AST_OK;
	fake_results[FAKE_TRANSMIT] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_TRANSMIT] = URP_AST_OK;
	poll_index = 0;
	poll_results[0] = 1;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_SUCCESS);
	memset(poll_results, 0, sizeof(poll_results));
	poll_index = 0;
	memset(fake_calls, 0, sizeof(fake_calls));
	fake_fail_call[FAKE_COMMAND] = 2;
	fake_fail_result[FAKE_COMMAND] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_FAILURE);
	fake_fail_call[FAKE_COMMAND] = 0;
	poll_index = 0;
	memset(fake_calls, 0, sizeof(fake_calls));
	fake_fail_call[FAKE_TRANSMIT] = 2;
	fake_fail_result[FAKE_TRANSMIT] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_FAILURE);
	fake_fail_call[FAKE_TRANSMIT] = 0;
	poll_index = 0;
	memset(fake_calls, 0, sizeof(fake_calls));
	fake_fail_call[FAKE_COMMAND] = 2;
	fake_fail_result[FAKE_COMMAND] = URP_AST_SETUP_FAILED;
	fake_fail_call[FAKE_TRANSMIT] = 2;
	fake_fail_result[FAKE_TRANSMIT] = URP_AST_SETUP_FAILED;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_FAILURE);
	fake_fail_call[FAKE_COMMAND] = 0;
	fake_fail_call[FAKE_TRANSMIT] = 0;
	poll_index = 0;
	poll_results[0] = 0;
	poll_results[1] = 1;
	assert(urp_cli_channel_flash(&entry, 2, &args) == CLI_SUCCESS);

	assert_cli_phases(urp_cli_processing_stats, &args);
	args.argc = 2;
	assert(urp_cli_processing_stats(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	assert(urp_cli_processing_stats(&entry, 2, &args) == CLI_SUCCESS);
	fake_results[FAKE_STATUS] = URP_AST_SETUP_FAILED;
	assert(urp_cli_processing_stats(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_STATUS] = URP_AST_OK;

	assert_cli_phases(urp_cli_reload, &args);
	args.argc = 2;
	assert(urp_cli_reload(&entry, 2, &args) == CLI_SHOWUSAGE);
	args.argc = 3;
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_INVALID_CONFIGURATION;
	assert(urp_cli_reload(&entry, 2, &args) == CLI_FAILURE);
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_OK;
	assert(urp_cli_reload(&entry, 2, &args) == CLI_SUCCESS);

	destroy_fixture(owner);
}

#define ASSERT_INVALID_DESCRIPTOR(field)                                                           \
	do {                                                                                       \
		__typeof__(fake_adapter.field) saved = fake_adapter.field;                         \
		fake_adapter.field = NULL;                                                         \
		assert(!urp_adapter_valid(&fake_adapter));                                         \
		fake_adapter.field = saved;                                                        \
	} while (0)

static void clear_module_state(void)
{
	ao2_cleanup(app_rpt_tech.capabilities);
	ao2_cleanup(advanced_tech.capabilities);
	app_rpt_tech.capabilities = NULL;
	advanced_tech.capabilities = NULL;
	rust_driver = NULL;
}

static void test_module_lifecycle(void)
{
	uint32_t saved_size = fake_adapter.struct_size;
	uint32_t saved_version = fake_adapter.abi_version;

	app_rpt_tech.capabilities = NULL;
	advanced_tech.capabilities = NULL;
	assert(!urp_adapter_valid(NULL));
	fake_adapter.struct_size = sizeof(fake_adapter) - 1U;
	assert(!urp_adapter_valid(&fake_adapter));
	fake_adapter.struct_size = saved_size;
	fake_adapter.abi_version++;
	assert(!urp_adapter_valid(&fake_adapter));
	fake_adapter.abi_version = saved_version;
	ASSERT_INVALID_DESCRIPTOR(driver_create);
	ASSERT_INVALID_DESCRIPTOR(driver_reload);
	ASSERT_INVALID_DESCRIPTOR(driver_reload_finish);
	ASSERT_INVALID_DESCRIPTOR(driver_channel_name);
	ASSERT_INVALID_DESCRIPTOR(driver_active_channel);
	ASSERT_INVALID_DESCRIPTOR(driver_set_active_channel);
	ASSERT_INVALID_DESCRIPTOR(driver_destroy);
	ASSERT_INVALID_DESCRIPTOR(link_prepare);
	ASSERT_INVALID_DESCRIPTOR(link_prepare_reload);
	ASSERT_INVALID_DESCRIPTOR(link_process);
	ASSERT_INVALID_DESCRIPTOR(link_observe);
	ASSERT_INVALID_DESCRIPTOR(link_destroy);
	ASSERT_INVALID_DESCRIPTOR(channel_reserve);
	ASSERT_INVALID_DESCRIPTOR(channel_start);
	ASSERT_INVALID_DESCRIPTOR(channel_stop);
	ASSERT_INVALID_DESCRIPTOR(channel_reload_prepare);
	ASSERT_INVALID_DESCRIPTOR(channel_reload_activate);
	ASSERT_INVALID_DESCRIPTOR(channel_reload_finish);
	ASSERT_INVALID_DESCRIPTOR(channel_write_voice);
	ASSERT_INVALID_DESCRIPTOR(channel_write_text);
	ASSERT_INVALID_DESCRIPTOR(channel_set_transmit);
	ASSERT_INVALID_DESCRIPTOR(channel_set_dtmf);
	ASSERT_INVALID_DESCRIPTOR(channel_set_echo);
	ASSERT_INVALID_DESCRIPTOR(channel_get_jitter_config);
	ASSERT_INVALID_DESCRIPTOR(channel_command);
	ASSERT_INVALID_DESCRIPTOR(channel_get_status);
	ASSERT_INVALID_DESCRIPTOR(channel_service);
	ASSERT_INVALID_DESCRIPTOR(channel_destroy);
	assert(urp_adapter_valid(&fake_adapter));

	fail_asprintf = 1;
	assert(urp_create_driver() == -1);
	fail_asprintf = 0;
	configuration_missing = 1;
	assert(urp_create_driver() == -1);
	configuration_missing = 0;
	force_all_lengths = 1;
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_create_driver() == -1);
	force_all_lengths = 0;
	shim_strlen_calls = 0;
	force_length_call = 2;
	forced_length = (size_t)UINT32_MAX + 1U;
	assert(urp_create_driver() == -1);
	force_length_call = 0;
	fake_results[FAKE_DRIVER_CREATE] = URP_AST_SETUP_FAILED;
	assert(urp_create_driver() == -1);
	fake_results[FAKE_DRIVER_CREATE] = URP_AST_OK;
	assert(!urp_create_driver() && rust_driver);
	rust_adapter->driver_destroy(rust_driver);
	rust_driver = NULL;

	fail_format_allocation = 1;
	assert(urp_prepare_capability(&app_rpt_tech, &app_format) == -1);
	fail_format_allocation = 0;
	fail_format_append = 1;
	assert(urp_prepare_capability(&app_rpt_tech, &app_format) == -1);
	fail_format_append = 0;
	assert(!urp_prepare_capability(&app_rpt_tech, &app_format));
	assert(app_rpt_tech.capabilities->format == &app_format);
	ao2_cleanup(app_rpt_tech.capabilities);
	app_rpt_tech.capabilities = NULL;

	descriptor_override = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	descriptor_override = &fake_adapter;
	configuration_missing = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	configuration_missing = 0;
	advanced_format_unavailable = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	advanced_format_unavailable = 0;
	advanced_format_rate = 44100;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	advanced_format_rate = URP_ADVANCED_RATE;
	format_allocation_calls = 0;
	fail_format_allocation_call = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	fail_format_allocation_call = 0;
	format_allocation_calls = 0;
	fail_format_allocation_call = 2;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	fail_format_allocation_call = 0;
	format_allocation_calls = 0;
	fail_registration_call = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	fail_registration_call = 2;
	registration_calls = 0;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	fail_registration_call = 0;
	registration_calls = 0;
	fail_cli_registration = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	clear_module_state();
	fail_cli_registration = 0;
	registration_calls = 0;
	fail_thread_create = 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	assert(!rust_driver && !app_rpt_tech.capabilities && !advanced_tech.capabilities);
	fail_thread_create = 0;
	registration_calls = 0;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_SUCCESS);
	assert(atomic_load(&link_scan_running));
	assert(captured_thread_start == urp_link_scanner && captured_thread_data == NULL);
	atomic_store(&active_channels, 1);
	assert(ast_module_entry_points.unload() == -1);
	atomic_store(&active_channels, 0);
	assert(!ast_module_entry_points.unload());
	assert(thread_join_calls == 1 && !atomic_load(&link_scan_running));
	assert(!app_rpt_tech.capabilities && !advanced_tech.capabilities && !rust_driver);

	rust_adapter = &fake_adapter;
	rust_driver = &fake_adapter;
	assert(!ast_module_entry_points.reload());
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_INVALID_CONFIGURATION;
	assert(ast_module_entry_points.reload() == -1);
	fake_results[FAKE_DRIVER_RELOAD] = URP_AST_OK;
}

#undef ASSERT_INVALID_DESCRIPTOR

int main(void)
{
	reset_fakes();
	test_technology_and_parsers();
	reset_fakes();
	test_delivery_callbacks();
	reset_fakes();
	test_control_and_jitter();
	reset_fakes();
	test_channel_operations();
	reset_fakes();
	test_request_and_hangup_paths();
	reset_fakes();
	test_hangup_drops_owner_lock_before_reload_exclusion();
	reset_fakes();
	test_configuration_and_selection();
	reset_fakes();
	test_multi_channel_reload_transaction();
	reset_fakes();
	test_reload_gates_control_and_replays_radio_unkey();
	reset_fakes();
	test_reload_prepares_new_link_before_publication();
	reset_fakes();
	test_link_reload_survives_masquerade();
	reset_fakes();
	test_link_audiohook_lifecycle();
	reset_fakes();
	test_cli_commands();
	reset_fakes();
	test_module_lifecycle();
	return 0;
}
