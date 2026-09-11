/** @file
 * @brief Executable processing validation regression and failure-path checks.
 */

struct ast_config;
struct ast_category;
#include "../src/usbradioplus_processing_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

/* The production allocator wrapper is intentionally tested below.  Its test
 * double must call the C allocator rather than recursively expanding itself. */
#undef realloc

/** @brief Host-API test double for ast_log; effects are recorded in this harness.
 * @param level Requested level or normalized tuning level, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

/** One synthetic Asterisk configuration option. */
struct fake_option {
	/** Configuration section name. */
	const char *section;
	/** Symbolic name used to identify this entry. */
	const char *name;
	/** Configured value or current output word, as declared. */
	const char *value;
};

/** Harness options used to script and verify host behavior. */
static struct fake_option fake_options[128];
/** Recorded fake option count for assertions. */
static size_t fake_option_count;
/** Harness categories used to script and verify host behavior. */
static char *fake_categories[40];
/** Recorded fake category count for assertions. */
static size_t fake_category_count;
/** Harness variables used to script and verify host behavior. */
static struct ast_variable *fake_variables[40];
/** Harness config load result used to script and verify host behavior. */
static struct ast_config *fake_config_load_result;
/** Recorded fake config destroy count for assertions. */
static int fake_config_destroy_count;
/** Controls injected category new failure failure for this test. */
static int fake_category_new_failure;
/** Recorded fake category append count for assertions. */
static int fake_category_append_count;
/** Harness save result used to script and verify host behavior. */
static int fake_save_result;
/** Controls injected tune update failure call failure for this test. */
static int fake_tune_update_failure_call;
/** Recorded fake tune update calls for assertions. */
static int fake_tune_update_calls;
/** Harness cli register result used to script and verify host behavior. */
static int fake_cli_register_result;
/** Recorded fake cli unregister calls for assertions. */
static int fake_cli_unregister_calls;
/** Harness thread create result used to script and verify host behavior. */
static int fake_thread_create_result;
/** Recorded fake cli print calls for assertions. */
static int fake_cli_print_calls;
/** Harness channel name used to script and verify host behavior. */
static const char *fake_channel_name = "IAX2/test";
/** Harness channel application used to script and verify host behavior. */
static const char *fake_channel_application = "Rpt";
/** Harness channel data used to script and verify host behavior. */
static const char *fake_channel_data = "Remote Rx";
/** Harness channel datastore used to script and verify host behavior. */
static struct ast_datastore *fake_channel_datastore;
/** Harness sample rate used to script and verify host behavior. */
static unsigned int fake_sample_rate = 8000;
/** Causes the channel raw-read format query to report no format. */
static int fake_rawreadformat_null;
/** Harness processor result used to script and verify host behavior. */
static int fake_processor_result;
/** Harness processor saturate used to script and verify host behavior. */
static int fake_processor_saturate;
/** Result injected into the next control-plane prepared-slot replacement. */
static int fake_slot_prepare_result;
/** Result injected into the next staged-slot publication. */
static int fake_slot_publish_result;
/** Controls injected staged-plan allocation growth failure. */
static int fake_realloc_failure;
/** Causes control-plane inspection of a prepared slot to report no active graph. */
static int fake_slot_active_null;
/** Causes a prepared test graph to advertise no callback input capacity. */
static int fake_slot_zero_input_capacity;
/** Last graph configuration received from the incoming-link callback. */
static struct txagc_config fake_processor_config;
/** Number of incoming audio blocks submitted to the graph. */
static unsigned int fake_processor_calls;
/** Sample rate submitted to the graph by the audiohook. */
static unsigned int fake_processor_sample_rate;
/** Recorded fake audiohook detach calls for assertions. */
static int fake_audiohook_detach_calls;
/** Recorded fake audiohook destroy calls for assertions. */
static int fake_audiohook_destroy_calls;
/** Recorded fake processor destroy calls for assertions. */
static int fake_processor_destroy_calls;
/** Controls injected datastore alloc failure failure for this test. */
static int fake_datastore_alloc_failure;
/** Controls injected calloc failure failure for this test. */
static int fake_calloc_failure;
/** Harness calloc call used to script and verify host behavior. */
static int fake_calloc_call;
/** Controls injected calloc fail call failure for this test. */
static int fake_calloc_fail_call;
/** Recorded fake datastore free calls for assertions. */
static int fake_datastore_free_calls;
/** Harness audiohook init result used to script and verify host behavior. */
static int fake_audiohook_init_result;
/** Harness audiohook attach result used to script and verify host behavior. */
static int fake_audiohook_attach_result;
/** Recorded fake datastore add calls for assertions. */
static int fake_datastore_add_calls;
/** Recorded fake datastore remove calls for assertions. */
static int fake_datastore_remove_calls;
/** Harness find sequence used to script and verify host behavior. */
static struct ast_datastore *fake_find_sequence[4];
/** Recorded fake find sequence count for assertions. */
static size_t fake_find_sequence_count;
/** Harness find sequence index used to script and verify host behavior. */
static size_t fake_find_sequence_index;
/** Harness last allocated datastore used to script and verify host behavior. */
static struct ast_datastore *fake_last_allocated_datastore;
/** Harness primary channel available used to script and verify host behavior. */
static int fake_primary_channel_available;
/** Exact primary channel requested by the link scanner. */
static char fake_primary_channel_name[AST_CHANNEL_NAME];
/** Harness iterator available used to script and verify host behavior. */
static int fake_iterator_available;
/** Harness iterator channels remaining used to script and verify host behavior. */
static int fake_iterator_channels_remaining;
/** Recorded fake iterator destroy calls for assertions. */
static int fake_iterator_destroy_calls;
/** Recorded fake pthread join calls for assertions. */
static int fake_pthread_join_calls;
/** Harness mutex depth used to script and verify host behavior. */
static int fake_mutex_depth;
/** Result returned by the native control-plane graph preparation stub. */
static int fake_native_prepare_result;
/** Number of native control-plane graph preparation requests. */
static unsigned int fake_native_prepare_calls;
/** Number of native transactions committed after all preparation succeeded. */
static unsigned int fake_native_publish_calls;
/** Number of native transactions discarded after a failed reload. */
static unsigned int fake_native_discard_calls;
/** Requests that native staging observe the callback-visible settings snapshot. */
static int fake_native_stage_observe_audio_settings;
/** Local input gain observed by the staged native control-plane test double. */
static double fake_native_stage_observed_local_gain;
/** Number of control-plane hardware refresh requests. */
static unsigned int fake_hardware_refresh_calls;
/** Number of live signaling refresh requests made after candidate graph staging. */
static unsigned int fake_signaling_refresh_calls;
/** Result injected into the live signaling refresh test double. */
static int fake_signaling_refresh_result;
/** Candidate receive signaling method observed by the live-refresh double. */
static char fake_signaling_refresh_method[16];

/** Maximum independently prepared link slots exercised by this harness. */
#define FAKE_SLOT_COUNT 32U

/** One test-owned graph backing an atomically published prepared slot. */
struct fake_slot {
	/** Slot whose active pointer references filter. */
	struct txagc_avfilter_slot *slot;
	/** Preconfigured filter state observed by the callback and statistics CLI. */
	struct txagc_avfilter filter;
};

/** Fixed test backing for slots; production ownership is covered by avfilter failures tests. */
static struct fake_slot fake_slots[FAKE_SLOT_COUNT];

/* The production implementation keeps this node private.  The processing-only
 * harness owns a minimal equivalent so staged multi-hook reloads can exercise
 * the public candidate API without invoking an FFmpeg allocator. */
struct txagc_avfilter_slot_node {
	struct txagc_avfilter filter;
};

/** @brief Locate the fixed harness graph backing a prepared slot.
 * @param slot Slot supplied by the processing module.
 * @param create Nonzero reserves a backing entry when one is not present.
 * @return Fixed backing entry, or NULL when creation is not requested or exhausted.
 *
 * The production implementation owns heap nodes.  This harness deliberately
 * uses a fixed table so callback tests prove the prepared path without
 * introducing an allocator into the test double.
 */
static struct fake_slot *fake_slot_find(struct txagc_avfilter_slot *slot, int create)
{
	size_t index;

	for (index = 0; index < ARRAY_LEN(fake_slots); ++index) {
		if (fake_slots[index].slot == slot)
			return &fake_slots[index];
	}
	if (!create)
		return NULL;
	for (index = 0; index < ARRAY_LEN(fake_slots); ++index) {
		if (!fake_slots[index].slot) {
			fake_slots[index].slot = slot;
			memset(&fake_slots[index].filter, 0, sizeof(fake_slots[index].filter));
			return &fake_slots[index];
		}
	}
	return NULL;
}

/** @brief Verify module self.
 * @return Result used by the test's assertions.
 */
struct ast_module *test_module_self(void)
{
	return NULL;
}

#undef calloc
#undef free

/** @brief Host-API test double for ast_variable_retrieve; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category Asterisk category or category name, as declared.
 * @param variable Configuration variable to inspect or update.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_variable_retrieve(struct ast_config *config, const char *category,
				  const char *variable)
{
	(void)config;
	for (size_t index = 0; index < fake_option_count; ++index) {
		if (!strcmp(fake_options[index].section, category) &&
		    !strcmp(fake_options[index].name, variable))
			return fake_options[index].value;
	}
	return NULL;
}

/** @brief Host-API test double for ast_category_browse; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param previous Previously returned category name, or NULL to start.
 * @return Scripted host result for the current test scenario.
 */
char *ast_category_browse(struct ast_config *config, const char *previous)
{
	(void)config;
	if (!fake_category_count)
		return NULL;
	if (!previous)
		return fake_categories[0];
	for (size_t index = 0; index + 1 < fake_category_count; ++index)
		if (!strcmp(previous, fake_categories[index]))
			return fake_categories[index + 1];
	return NULL;
}

/** @brief Host-API test double for ast_variable_browse; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category Asterisk category or category name, as declared.
 * @return Scripted host result for the current test scenario.
 */
struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category)
{
	(void)config;
	for (size_t index = 0; index < fake_category_count; ++index)
		if (!strcmp(category, fake_categories[index]))
			return fake_variables[index];
	return NULL;
}

/** @brief Host-API test double for ast_config_load2; effects are recorded in this harness.
 * @param filename Configuration or diagnostic source filename.
 * @param who_asked Module requesting configuration.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
struct ast_config *ast_config_load2(const char *filename, const char *who_asked,
				    struct ast_flags flags)
{
	(void)filename;
	(void)who_asked;
	(void)flags;
	return fake_config_load_result;
}

/** @brief Host-API test double for ast_config_destroy; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 */
void ast_config_destroy(struct ast_config *config)
{
	(void)config;
	++fake_config_destroy_count;
}

/** @brief Host-API test double for ast_category_get; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param name Option, metadata field, or channel name.
 * @param filter FFmpeg dynamics filter name.
 * @return Scripted host result for the current test scenario.
 */
struct ast_category *ast_category_get(const struct ast_config *config, const char *name,
				      const char *filter)
{
	(void)config;
	(void)filter;
	for (size_t index = 0; index < fake_category_count; ++index)
		if (!strcmp(name, fake_categories[index]))
			return (struct ast_category *)(uintptr_t)2;
	return NULL;
}

/** @brief Host-API test double for ast_category_new; effects are recorded in this harness.
 * @param name Option, metadata field, or channel name.
 * @param filename Configuration or diagnostic source filename.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @return Scripted host result for the current test scenario.
 */
struct ast_category *ast_category_new(const char *name, const char *filename, int line)
{
	(void)name;
	(void)filename;
	(void)line;
	if (fake_category_new_failure)
		return NULL;
	return (struct ast_category *)(uintptr_t)3;
}

/** @brief Host-API test double for ast_category_append; effects are recorded in this harness.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param category Asterisk category or category name, as declared.
 */
void ast_category_append(struct ast_config *config, struct ast_category *category)
{
	(void)config;
	(void)category;
	++fake_category_append_count;
}

/** @brief Host-API test double for ast_config_text_file_save2; effects are recorded in this
 * harness.
 * @param filename Configuration or diagnostic source filename.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 * @param generator Name of the component saving configuration.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
int ast_config_text_file_save2(const char *filename, const struct ast_config *config,
			       const char *generator, uint32_t flags)
{
	(void)filename;
	(void)config;
	(void)generator;
	(void)flags;
	return fake_save_result;
}

/** @brief Host-API test double for __ast_pthread_mutex_lock; effects are recorded in this harness.
 * @param filename Configuration or diagnostic source filename.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param mutex_name Diagnostic mutex name.
 * @param mutex Mutex tracked by the harness.
 * @return Scripted host result for the current test scenario.
 */
int __ast_pthread_mutex_lock(const char *filename, int line, const char *function,
			     const char *mutex_name, ast_mutex_t *mutex)
{
	(void)filename;
	(void)line;
	(void)function;
	(void)mutex_name;
	(void)mutex;
	++fake_mutex_depth;
	return 0;
}

/** @brief Host-API test double for __ast_pthread_mutex_unlock; effects are recorded in this
 * harness.
 * @param filename Configuration or diagnostic source filename.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param mutex_name Diagnostic mutex name.
 * @param mutex Mutex tracked by the harness.
 * @return Scripted host result for the current test scenario.
 */
int __ast_pthread_mutex_unlock(const char *filename, int line, const char *function,
			       const char *mutex_name, ast_mutex_t *mutex)
{
	(void)filename;
	(void)line;
	(void)function;
	(void)mutex_name;
	(void)mutex;
	assert(fake_mutex_depth > 0);
	--fake_mutex_depth;
	return 0;
}

/** @brief Host-API test double for __ast_cli_register_multiple; effects are recorded in this
 * harness.
 * @param entries entries supplied by the test scenario.
 * @param count Number of elements available in the supplied block.
 * @param module Asterisk module reference.
 * @return Scripted host result for the current test scenario.
 */
int __ast_cli_register_multiple(struct ast_cli_entry *entries, int count, struct ast_module *module)
{
	(void)entries;
	(void)count;
	(void)module;
	return fake_cli_register_result;
}

/** @brief Host-API test double for ast_cli_unregister_multiple; effects are recorded in this
 * harness.
 * @param entries entries supplied by the test scenario.
 * @param count Number of elements available in the supplied block.
 * @return Scripted host result for the current test scenario.
 */
int ast_cli_unregister_multiple(struct ast_cli_entry *entries, int count)
{
	(void)entries;
	(void)count;
	++fake_cli_unregister_calls;
	return 0;
}

/** @brief Host-API test double for ast_background_stacksize; effects are recorded in this harness.
 * @return Scripted host result for the current test scenario.
 */
int ast_background_stacksize(void)
{
	return 0;
}

/** @brief Host-API test double for ast_pthread_create_stack; effects are recorded in this harness.
 * @param thread Worker thread identifier supplied by the harness.
 * @param attributes POSIX thread creation attributes.
 * @param start_routine Worker entry point supplied by the module.
 * @param data Input payload or owned state being released, as declared.
 * @param stack_size Requested worker stack size in bytes.
 * @param filename Configuration or diagnostic source filename.
 * @param caller Calling function name.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param start_name Worker entry-point name for diagnostics.
 * @return Scripted host result for the current test scenario.
 */
int ast_pthread_create_stack(pthread_t *thread, pthread_attr_t *attributes,
			     void *(*start_routine)(void *), void *data, size_t stack_size,
			     const char *filename, const char *caller, int line,
			     const char *start_name)
{
	(void)thread;
	(void)attributes;
	(void)start_routine;
	(void)data;
	(void)stack_size;
	(void)filename;
	(void)caller;
	(void)line;
	(void)start_name;
	return fake_thread_create_result;
}

/** @brief Host-API test double for ast_channel_iterator_all_new; effects are recorded in this
 * harness.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel_iterator *ast_channel_iterator_all_new(void)
{
	return fake_iterator_available ? (struct ast_channel_iterator *)(uintptr_t)1 : NULL;
}

/** @brief Host-API test double for ast_channel_iterator_next; effects are recorded in this harness.
 * @param iterator Harness channel iterator.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel *ast_channel_iterator_next(struct ast_channel_iterator *iterator)
{
	(void)iterator;
	if (fake_iterator_channels_remaining-- > 0)
		return (struct ast_channel *)(uintptr_t)1;
	return NULL;
}

/** @brief Host-API test double for ast_channel_iterator_destroy; effects are recorded in this
 * harness.
 * @param iterator Harness channel iterator.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel_iterator *ast_channel_iterator_destroy(struct ast_channel_iterator *iterator)
{
	++fake_iterator_destroy_calls;
	return iterator;
}

/** @brief Host-API test double for ast_channel_get_by_name; effects are recorded in this harness.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
struct ast_channel *ast_channel_get_by_name(const char *name)
{
	ast_copy_string(fake_primary_channel_name, name, sizeof(fake_primary_channel_name));
	/* Channel lookup takes Asterisk container locks.  The processing settings
	 * lock must never be held here or app_rpt can form the reciprocal order. */
	assert(fake_mutex_depth == 0);
	return fake_primary_channel_available ? (struct ast_channel *)(uintptr_t)1 : NULL;
}

/** @brief Host-API test double for ast_channel_name; effects are recorded in this harness.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_name(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_name;
}

/** @brief Host-API test double for ast_channel_appl; effects are recorded in this harness.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_appl(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_application;
}

/** @brief Host-API test double for ast_channel_data; effects are recorded in this harness.
 * @param channel Radio channel or channel index, as declared.
 * @return Scripted host result for the current test scenario.
 */
const char *ast_channel_data(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_data;
}

/** @brief Host-API test double for __ao2_lock; effects are recorded in this harness.
 * @param object Host reference-counted object.
 * @param request Host I/O or locking operation.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_lock(void *object, enum ao2_lock_req request, const char *file, const char *function,
	       int line, const char *name)
{
	(void)object;
	(void)request;
	(void)file;
	(void)function;
	(void)line;
	(void)name;
	return 0;
}

/** @brief Host-API test double for __ao2_unlock; effects are recorded in this harness.
 * @param object Host reference-counted object.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param name Option, metadata field, or channel name.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_unlock(void *object, const char *file, const char *function, int line, const char *name)
{
	(void)object;
	(void)file;
	(void)function;
	(void)line;
	(void)name;
	return 0;
}

/** @brief Host-API test double for __ao2_ref; effects are recorded in this harness.
 * @param object Host reference-counted object.
 * @param delta Requested reference-count change.
 * @param tag Host reference-tracking tag.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
int __ao2_ref(void *object, int delta, const char *tag, const char *file, int line,
	      const char *function)
{
	(void)object;
	(void)delta;
	(void)tag;
	(void)file;
	(void)line;
	(void)function;
	return 0;
}

/** @brief Host-API test double for ast_channel_datastore_find; effects are recorded in this
 * harness.
 * @param channel Radio channel or channel index, as declared.
 * @param info Test module metadata.
 * @param uid Datastore identifier.
 * @return Scripted host result for the current test scenario.
 */
struct ast_datastore *ast_channel_datastore_find(struct ast_channel *channel,
						 const struct ast_datastore_info *info,
						 const char *uid)
{
	(void)channel;
	(void)info;
	(void)uid;
	if (fake_find_sequence_index < fake_find_sequence_count)
		return fake_find_sequence[fake_find_sequence_index++];
	return fake_channel_datastore;
}

/** @brief Host-API test double for ast_channel_datastore_add; effects are recorded in this harness.
 * @param channel Radio channel or channel index, as declared.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_channel_datastore_add(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	++fake_datastore_add_calls;
	return 0;
}

/** @brief Host-API test double for ast_channel_datastore_remove; effects are recorded in this
 * harness.
 * @param channel Radio channel or channel index, as declared.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_channel_datastore_remove(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	++fake_datastore_remove_calls;
	return 0;
}

/** @brief Host-API test double for __ast_datastore_alloc; effects are recorded in this harness.
 * @param info Test module metadata.
 * @param uid Datastore identifier.
 * @param module Asterisk module reference.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
struct ast_datastore *__ast_datastore_alloc(const struct ast_datastore_info *info, const char *uid,
					    struct ast_module *module, const char *file, int line,
					    const char *function)
{
	(void)info;
	(void)uid;
	(void)module;
	(void)file;
	(void)line;
	(void)function;
	if (fake_datastore_alloc_failure)
		return NULL;
	fake_last_allocated_datastore = calloc(1, sizeof(struct ast_datastore));
	return fake_last_allocated_datastore;
}

/** @brief Host-API test double for ast_datastore_free; effects are recorded in this harness.
 * @param datastore Channel-owned datastore.
 * @return Scripted host result for the current test scenario.
 */
int ast_datastore_free(struct ast_datastore *datastore)
{
	++fake_datastore_free_calls;
	free(datastore);
	return 0;
}

/** @brief Host-API test double for __ast_calloc; effects are recorded in this harness.
 * @param count Number of elements available in the supplied block.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
void *__ast_calloc(size_t count, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	++fake_calloc_call;
	if (fake_calloc_failure || fake_calloc_call == fake_calloc_fail_call)
		return NULL;
	return calloc(count, size);
}

/** @brief Host-API test double for __ast_free; effects are recorded in this harness.
 * @param pointer Allocated buffer passed through the failure-injection shim.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 */
void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

/** @brief Host-API test double for __ast_realloc used by staged hook planning.
 * @param pointer Existing plan allocation, or NULL for its first allocation.
 * @param size Requested capacity in bytes.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Reallocated storage, or NULL when the C allocator cannot grow it.
 */
void *__ast_realloc(void *pointer, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	if (fake_realloc_failure)
		return NULL;
	return realloc(pointer, size);
}

/** @brief Host-API test double for ast_audiohook_init; effects are recorded in this harness.
 * @param audiohook Attached link-processing hook.
 * @param type Requested Asterisk channel technology.
 * @param source Processing source or source text, as declared.
 * @param flags Host API option bit mask.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_init(struct ast_audiohook *audiohook, enum ast_audiohook_type type,
		       const char *source, enum ast_audiohook_init_flags flags)
{
	(void)audiohook;
	(void)type;
	(void)source;
	(void)flags;
	return fake_audiohook_init_result;
}

/** @brief Host-API test double for ast_audiohook_destroy; effects are recorded in this harness.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_destroy(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	++fake_audiohook_destroy_calls;
	return 0;
}

/** @brief Host-API test double for ast_audiohook_attach; effects are recorded in this harness.
 * @param channel Radio channel or channel index, as declared.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_attach(struct ast_channel *channel, struct ast_audiohook *audiohook)
{
	(void)channel;
	(void)audiohook;
	return fake_audiohook_attach_result;
}

/** @brief Host-API test double for ast_audiohook_detach; effects are recorded in this harness.
 * @param audiohook Attached link-processing hook.
 * @return Scripted host result for the current test scenario.
 */
int ast_audiohook_detach(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	++fake_audiohook_detach_calls;
	return 0;
}

/** @brief Host-API test double for ast_format_get_sample_rate; effects are recorded in this
 * harness.
 * @param format printf-style message format.
 * @return Scripted host result for the current test scenario.
 */
unsigned int ast_format_get_sample_rate(const struct ast_format *format)
{
	(void)format;
	return fake_sample_rate;
}

/** @brief Return a non-null fake source format while attaching a link hook.
 * @param channel Asterisk channel supplied by the hook owner.
 * @return Opaque format identity consumed by ast_format_get_sample_rate().
 */
struct ast_format *ast_channel_rawreadformat(struct ast_channel *channel)
{
	(void)channel;
	if (fake_rawreadformat_null)
		return NULL;
	return (struct ast_format *)(uintptr_t)1;
}

void txagc_avfilter_init(struct txagc_avfilter *processor)
{
	memset(processor, 0, sizeof(*processor));
}

void txagc_avfilter_destroy(struct txagc_avfilter *processor)
{
	(void)processor;
	++fake_processor_destroy_calls;
}

/** @brief Initialize one empty harness graph slot.
 * @param slot Atomically published graph slot owned by an audiohook.
 */
void txagc_avfilter_slot_init(struct txagc_avfilter_slot *slot)
{
	struct fake_slot *entry;

	assert(slot);
	memset(slot, 0, sizeof(*slot));
	atomic_init(&slot->active, NULL);
	atomic_init(&slot->readers, 0);
	atomic_flag_clear(&slot->writer);
	entry = fake_slot_find(slot, 1);
	assert(entry);
	memset(&entry->filter, 0, sizeof(entry->filter));
}

/** @brief Destroy one test-owned slot after its callback has stopped.
 * @param slot Graph slot to clear.
 */
void txagc_avfilter_slot_destroy(struct txagc_avfilter_slot *slot)
{
	struct fake_slot *entry;

	if (!slot)
		return;
	entry = fake_slot_find(slot, 0);
	if (!entry)
		return;
	if (atomic_load_explicit(&slot->active, memory_order_acquire))
		txagc_avfilter_destroy(&entry->filter);
	atomic_store_explicit(&slot->active, NULL, memory_order_release);
	entry->slot = NULL;
	memset(&entry->filter, 0, sizeof(entry->filter));
}

/** @brief Build one preallocated harness graph.
 * @param processor Destination graph state.
 * @param config Control-plane graph configuration.
 * @param sample_rate Input sample rate in Hz.
 * @return Zero after recording the prepared configuration.
 */
int txagc_avfilter_prepare(struct txagc_avfilter *processor, const struct txagc_config *config,
			   unsigned int sample_rate)
{
	if (!processor || !config || !sample_rate)
		return -1;
	memset(processor, 0, sizeof(*processor));
	processor->config = *config;
	processor->sample_rate = sample_rate;
	/* Match the production contract: callback workspace is fixed before use. */
	processor->input_capacity = 960;
	processor->fifo_capacity = 960;
	processor->configured = 1;
	return 0;
}

/** @brief Process an already prepared harness graph without rebuilding it.
 * @param processor Prebuilt graph state.
 * @param samples Mutable audio samples.
 * @param sample_count Number of samples in samples.
 * @return Scripted graph result.
 */
int txagc_avfilter_process_prepared(struct txagc_avfilter *processor, double *samples,
				    size_t sample_count)
{
	if (!processor || !processor->configured || sample_count > processor->input_capacity)
		return -1;
	fake_processor_config = processor->config;
	fake_processor_sample_rate = processor->sample_rate;
	++fake_processor_calls;
	if (fake_processor_saturate && sample_count >= 3) {
		samples[0] = 40000.0;
		samples[1] = -40000.0;
		samples[2] = 1.6;
	}
	return fake_processor_result;
}

/** @brief Atomically publish a fully prepared harness graph replacement.
 * @param slot Destination slot.
 * @param config Candidate graph configuration.
 * @param sample_rate Candidate source rate in Hz.
 * @return Zero on replacement, or an injected failure retaining the old graph.
 */
int txagc_avfilter_slot_prepare(struct txagc_avfilter_slot *slot, const struct txagc_config *config,
				unsigned int sample_rate)
{
	struct fake_slot *entry;
	struct txagc_avfilter candidate;

	if (!slot || fake_slot_prepare_result)
		return fake_slot_prepare_result ? fake_slot_prepare_result : -1;
	entry = fake_slot_find(slot, 1);
	if (!entry || txagc_avfilter_prepare(&candidate, config, sample_rate))
		return -1;
	if (fake_slot_zero_input_capacity)
		candidate.input_capacity = 0;
	/* Do not touch entry until preparation has succeeded: replacement is transactional. */
	entry->filter = candidate;
	atomic_store_explicit(&slot->active, &entry->filter, memory_order_release);
	return 0;
}

/** @brief Prepare one unpublished harness graph for a staged reload. */
int txagc_avfilter_slot_candidate_prepare(struct txagc_avfilter_slot_candidate *candidate,
					  const struct txagc_config *config,
					  unsigned int sample_rate)
{
	struct txagc_avfilter_slot_node *node;

	if (!candidate || !config || candidate->node || fake_slot_prepare_result)
		return fake_slot_prepare_result ? fake_slot_prepare_result : -1;
	node = calloc(1, sizeof(*node));
	if (!node || txagc_avfilter_prepare(&node->filter, config, sample_rate)) {
		free(node);
		return -1;
	}
	candidate->node = node;
	return 0;
}

/** @brief Destroy one unpublished harness graph candidate. */
void txagc_avfilter_slot_candidate_destroy(struct txagc_avfilter_slot_candidate *candidate)
{
	if (!candidate || !candidate->node)
		return;
	txagc_avfilter_destroy(&candidate->node->filter);
	free(candidate->node);
	candidate->node = NULL;
}

/** @brief Publish one staged harness graph after all peer candidates succeeded. */
int txagc_avfilter_slot_publish_candidate(struct txagc_avfilter_slot *slot,
					  struct txagc_avfilter_slot_candidate *candidate)
{
	struct fake_slot *entry;

	if (fake_slot_publish_result)
		return fake_slot_publish_result;
	if (!slot || !candidate || !candidate->node)
		return -1;
	entry = fake_slot_find(slot, 1);
	if (!entry)
		return -1;
	entry->filter = candidate->node->filter;
	free(candidate->node);
	candidate->node = NULL;
	atomic_store_explicit(&slot->active, &entry->filter, memory_order_release);
	return 0;
}

/** @brief Return a slot graph for control-plane inspection.
 * @param slot Slot to inspect.
 * @return Current prepared graph or NULL.
 */
struct txagc_avfilter *txagc_avfilter_slot_active(const struct txagc_avfilter_slot *slot)
{
	if (fake_slot_active_null)
		return NULL;
	return slot ? atomic_load_explicit(&slot->active, memory_order_acquire) : NULL;
}

/** @brief Retain the current harness graph for a statistics reader.
 * @param slot Slot to acquire.
 * @return Current graph or NULL.
 */
struct txagc_avfilter *txagc_avfilter_slot_acquire(struct txagc_avfilter_slot *slot)
{
	struct txagc_avfilter *filter;

	if (!slot)
		return NULL;
	atomic_fetch_add_explicit(&slot->readers, 1, memory_order_acq_rel);
	filter = atomic_load_explicit(&slot->active, memory_order_acquire);
	if (!filter)
		atomic_fetch_sub_explicit(&slot->readers, 1, memory_order_acq_rel);
	return filter;
}

/** @brief Release one harness graph reader.
 * @param slot Slot whose reader is complete.
 */
void txagc_avfilter_slot_release(struct txagc_avfilter_slot *slot)
{
	if (slot)
		atomic_fetch_sub_explicit(&slot->readers, 1, memory_order_acq_rel);
}

/** @brief Run a current slot graph through the prepared-only callback path.
 * @param slot Prepared graph slot.
 * @param samples Mutable samples.
 * @param sample_count Number of samples.
 * @return Graph processing result.
 */
int txagc_avfilter_slot_process_prepared(struct txagc_avfilter_slot *slot, double *samples,
					 size_t sample_count)
{
	struct txagc_avfilter *processor = txagc_avfilter_slot_acquire(slot);
	int result;

	if (!processor)
		return -1;
	result = txagc_avfilter_process_prepared(processor, samples, sample_count);
	txagc_avfilter_slot_release(slot);
	return result;
}

int txagc_avfilter_process(struct txagc_avfilter *processor, const struct txagc_config *config,
			   double *samples, size_t sample_count, unsigned int sample_rate)
{
	if (txagc_avfilter_prepare(processor, config, sample_rate))
		return -1;
	return txagc_avfilter_process_prepared(processor, samples, sample_count);
}

/** Minimal opaque native graph transaction used by processing-only reload tests. */
struct usbradioplus_native_graph_transaction {
	int unused;
};

/** @brief Control-plane native graph staging stub used by reload tests.
 * @param transaction Receives the fixed staged transaction on success.
 * @return Scripted preparation result.
 */
int usbradioplus_stage_all_native_processing(
	struct usbradioplus_native_graph_transaction **transaction)
{
	++fake_native_prepare_calls;
	if (fake_native_stage_observe_audio_settings) {
		struct txagc_chain observed;

		assert(!usbradioplus_processing_get_local_rt("usb", &observed));
		fake_native_stage_observed_local_gain = observed.agc.input_gain_db;
	}
	if (transaction) {
		static struct usbradioplus_native_graph_transaction staged;
		*transaction = fake_native_prepare_result ? NULL : &staged;
	}
	return fake_native_prepare_result;
}

/** @brief Record native transaction publication after all peer graphs staged.
 * @param transaction Fixed test transaction.
 */
void usbradioplus_publish_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction)
{
	assert(transaction);
	++fake_native_publish_calls;
}

/** @brief Record transactional native graph discard on reload failure.
 * @param transaction Fixed test transaction, possibly NULL.
 */
void usbradioplus_discard_native_processing_transaction(
	struct usbradioplus_native_graph_transaction *transaction)
{
	if (transaction)
		++fake_native_discard_calls;
}

/** @brief Compatibility native graph prepare stub retained for direct callers. */
int usbradioplus_prepare_all_native_processing(void)
{
	struct usbradioplus_native_graph_transaction *transaction;

	if (usbradioplus_stage_all_native_processing(&transaction))
		return -1;
	usbradioplus_publish_native_processing_transaction(transaction);
	return 0;
}

/** @brief Control-plane hardware publication stub used by reload tests.
 * @return Always succeeds in this processing-only harness.
 */
int usbradioplus_refresh_all_processing_hardware(void)
{
	++fake_hardware_refresh_calls;
	return 0;
}

/** @brief Observe the candidate signaling settings used by a processing reload. */
int usbradioplus_refresh_all_processing_signaling(void)
{
	++fake_signaling_refresh_calls;
	if (usbradioplus_processing_get_option("usb", "receive", "signaling_method",
					       fake_signaling_refresh_method,
					       sizeof(fake_signaling_refresh_method)))
		return -1;
	return fake_signaling_refresh_result;
}

/** @brief Prepare a stack-owned link hook exactly as the attach control plane does.
 * @param hook Hook fixture whose callback storage is initialized.
 * @param profile Resolved processing profile name.
 * @param samples Callback-owned conversion workspace.
 * @param capacity Number of samples available in the test block.
 * @param sample_rate Input sample rate used to prepare the graph.
 */
static void prepare_test_link_hook(struct txagc_hook *hook, const char *profile, double *samples,
				   size_t capacity, unsigned int sample_rate)
{
	struct txagc_avfilter *filter;

	assert(hook && profile && samples && capacity && sample_rate);
	txagc_avfilter_slot_init(&hook->avfilter);
	hook->samples = samples;
	hook->samples_capacity = capacity;
	atomic_init(&hook->link_enabled, 1);
	atomic_init(&hook->statistics_index, 0U);
	atomic_init(&hook->statistics_readers[0], 0U);
	atomic_init(&hook->statistics_readers[1], 0U);
	hook->sample_rate = sample_rate;
	ast_copy_string(hook->profile, profile, sizeof(hook->profile));
	hook->audiohook.status = AST_AUDIOHOOK_STATUS_RUNNING;
	assert(!txagc_avfilter_slot_prepare(
		&hook->avfilter, &settings.profiles[0].chains[TXAGC_LINK].agc, sample_rate));
	filter = txagc_avfilter_slot_active(&hook->avfilter);
	/* Production attaches a full fixed frame workspace.  This stack fixture
	 * deliberately supplies only the short block it submits, while the prepared
	 * graph still advertises its larger no-allocation capacity. */
	assert(filter && filter->configured && capacity <= filter->input_capacity);
	link_callback_publish_statistics(hook);
}

/** @brief Release prepared slots belonging to a stack-owned hook fixture.
 * @param hook Hook whose slot storage must be released without freeing the stack object.
 */
static void destroy_test_hook_slots(struct txagc_hook *hook)
{
	txagc_avfilter_slot_destroy(&hook->avfilter);
	/* Stack fixtures lend their workspace to the callback; hook_destroy() owns
	 * heap workspaces created by attach_hook(), so do not free this one. */
	hook->samples = NULL;
	hook->samples_capacity = 0U;
}

/** @brief Deliver one synthetic frame through the synchronous prepared graph. */
static int test_link_callback(struct ast_audiohook *audiohook, struct ast_channel *channel,
			      struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	return txagc_callback(audiohook, channel, frame, direction);
}

/** @brief Host-API test double for ast_cli; effects are recorded in this harness.
 * @param fd Asterisk CLI output descriptor.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void ast_cli(int fd, const char *format, ...)
{
	(void)fd;
	(void)format;
	++fake_cli_print_calls;
}

/** @brief Host-API test double for usleep; observable effects are recorded in harness state.
 * @param microseconds Requested sleep interval in microseconds.
 * @return Scripted host result for the current test scenario.
 */
int usleep(useconds_t microseconds)
{
	(void)microseconds;
	stopping = 1;
	return 0;
}

int pthread_join(pthread_t thread, void **result)
{
	(void)thread;
	(void)result;
	++fake_pthread_join_calls;
	return 0;
}

/** @brief Host-API test double for ast_true; effects are recorded in this harness.
 * @param value Input value or writable result, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_true(const char *value)
{
	return !strcasecmp(value, "yes") || !strcmp(value, "1");
}

/** @brief Host-API test double for ast_false; effects are recorded in this harness.
 * @param value Input value or writable result, as declared.
 * @return Scripted host result for the current test scenario.
 */
int ast_false(const char *value)
{
	return !strcasecmp(value, "no") || !strcmp(value, "0");
}

int usbradioplus_config_variable_update(struct ast_config *config, const char *filename,
					struct ast_category *category, const char *variable,
					const char *value)
{
	(void)config;
	(void)filename;
	(void)category;
	(void)variable;
	(void)value;
	++fake_tune_update_calls;
	return fake_tune_update_calls == fake_tune_update_failure_call;
}

/** Configuration field offset and allowed numeric bounds. */
struct range_case {
	/** DC offset applied to each diagnostic trace channel. */
	size_t offset;
	/** Harness minimum used to script and verify host behavior. */
	double minimum;
	/** Harness maximum used to script and verify host behavior. */
	double maximum;
};

#define RANGE(field, low, high) offsetof(struct txagc_config, field), (low), (high)

/** Harness ranges used to script and verify host behavior. */
static const struct range_case ranges[] = {
	{RANGE(ctcss_notch_width_hz, 0.2, 10.0)},
	{RANGE(ctcss_highpass_hz, 50.0, 500.0)},
	{RANGE(receive_bandpass_highpass_hz, 20.0, 2000.0)},
	{RANGE(receive_bandpass_lowpass_hz, 20.0, 6000.0)},
	{RANGE(input_gain_db, -30.0, 30.0)},
	{RANGE(equalizer_low_gain_db, -12.0, 12.0)},
	{RANGE(equalizer_low_frequency_hz, 20.0, 1000.0)},
	{RANGE(equalizer_low_slope, 0.1, 1.0)},
	{RANGE(equalizer_mid_gain_db, -12.0, 12.0)},
	{RANGE(equalizer_mid_frequency_hz, 100.0, 4000.0)},
	{RANGE(equalizer_mid_width_octaves, 0.1, 4.0)},
	{RANGE(equalizer_high_gain_db, -12.0, 12.0)},
	{RANGE(equalizer_high_frequency_hz, 1000.0, 5000.0)},
	{RANGE(equalizer_high_slope, 0.1, 1.0)},
	{RANGE(deesser_frequency_hz, 2000.0, 8000.0)},
	{RANGE(deesser_width_octaves, 0.1, 4.0)},
	{RANGE(deesser_threshold_dbfs, -60.0, -1.0)},
	{RANGE(deesser_ratio, 1.0, 20.0)},
	{RANGE(deesser_max_reduction_db, 0.1, 20.0)},
	{RANGE(deesser_attack_ms, 0.1, 100.0)},
	{RANGE(deesser_release_ms, 1.0, 2000.0)},
	{RANGE(target_dbfs, -40.0, -3.0)},
	{RANGE(max_gain_db, 0.0, 30.0)},
	{RANGE(max_attenuation_db, 0.0, 60.0)},
	{RANGE(agc_rms_averaging_ms, 10.0, 5000.0)},
	{RANGE(agc_gain_increase_db_per_second, 0.1, 100.0)},
	{RANGE(agc_gain_decrease_db_per_second, 0.1, 100.0)},
	{RANGE(agc_activity_threshold_dbfs, -100.0, -3.0)},
	{RANGE(agc_activity_hysteresis_db, 0.0, 12.0)},
	{RANGE(agc_hold_ms, 0.0, 10000.0)},
	{RANGE(agc_deadband_db, 0.0, 6.0)},
	{RANGE(sidechain_highpass_hz, 0.0, 2000.0)},
	{RANGE(sidechain_lowpass_hz, 0.0, 3500.0)},
	{RANGE(expander_threshold_dbfs, -100.0, -10.0)},
	{RANGE(expander_ratio, 1.0, 10.0)},
	{RANGE(expander_max_attenuation_db, 0.0, 40.0)},
	{RANGE(expander_attack_ms, 1.0, 1000.0)},
	{RANGE(expander_release_ms, 1.0, 10000.0)},
	{RANGE(expander_sidechain_highpass_hz, 50.0, 2000.0)},
	{RANGE(expander_sidechain_lowpass_hz, 50.0, 3500.0)},
	{RANGE(compressor_threshold_dbfs, -60.0, 0.0)},
	{RANGE(compressor_low_crossover_hz, 100.0, 2000.0)},
	{RANGE(compressor_high_crossover_hz, 100.0, 5000.0)},
	{RANGE(compressor_low_threshold_dbfs, -60.0, 0.0)},
	{RANGE(compressor_low_ratio, 1.0, 20.0)},
	{RANGE(compressor_low_makeup_gain_db, -30.0, 30.0)},
	{RANGE(compressor_low_knee_db, 0.0, 18.0)},
	{RANGE(compressor_low_attack_ms, 1.0, 1000.0)},
	{RANGE(compressor_low_release_ms, 1.0, 9000.0)},
	{RANGE(compressor_mid_threshold_dbfs, -60.0, 0.0)},
	{RANGE(compressor_mid_ratio, 1.0, 20.0)},
	{RANGE(compressor_mid_makeup_gain_db, -30.0, 30.0)},
	{RANGE(compressor_mid_knee_db, 0.0, 18.0)},
	{RANGE(compressor_mid_attack_ms, 1.0, 1000.0)},
	{RANGE(compressor_mid_release_ms, 1.0, 9000.0)},
	{RANGE(compressor_high_threshold_dbfs, -60.0, 0.0)},
	{RANGE(compressor_high_ratio, 1.0, 20.0)},
	{RANGE(compressor_high_makeup_gain_db, -30.0, 30.0)},
	{RANGE(compressor_high_knee_db, 0.0, 18.0)},
	{RANGE(compressor_high_attack_ms, 1.0, 1000.0)},
	{RANGE(compressor_high_release_ms, 1.0, 9000.0)},
	{RANGE(limiter_threshold_dbfs, -40.0, -1.0)},
	{RANGE(limiter_ratio, 1.0, 20.0)},
	{RANGE(limiter_knee_db, 0.0, 18.0)},
	{RANGE(limiter_attack_ms, 0.1, 1000.0)},
	{RANGE(limiter_release_ms, 1.0, 9000.0)},
	{RANGE(compressor_ratio, 1.0, 20.0)},
	{RANGE(compressor_makeup_gain_db, -30.0, 30.0)},
	{RANGE(compressor_attack_ms, 1.0, 1000.0)},
	{RANGE(compressor_release_ms, 1.0, 9000.0)},
	{RANGE(compressor_sidechain_highpass_hz, 50.0, 2000.0)},
	{RANGE(compressor_sidechain_lowpass_hz, 50.0, 3500.0)},
	{RANGE(limiter_low_crossover_hz, 100.0, 2000.0)},
	{RANGE(limiter_high_crossover_hz, 100.0, 5000.0)},
	{RANGE(low_limiter_threshold_dbfs, -40.0, -1.0)},
	{RANGE(low_limiter_ratio, 1.0, 20.0)},
	{RANGE(low_limiter_knee_db, 0.0, 18.0)},
	{RANGE(low_limiter_attack_ms, 0.1, 1000.0)},
	{RANGE(low_limiter_release_ms, 1.0, 9000.0)},
	{RANGE(mid_limiter_threshold_dbfs, -40.0, -1.0)},
	{RANGE(mid_limiter_ratio, 1.0, 20.0)},
	{RANGE(mid_limiter_knee_db, 0.0, 18.0)},
	{RANGE(mid_limiter_attack_ms, 0.1, 1000.0)},
	{RANGE(mid_limiter_release_ms, 1.0, 9000.0)},
	{RANGE(high_limiter_threshold_dbfs, -30.0, -1.0)},
	{RANGE(high_limiter_ratio, 1.0, 20.0)},
	{RANGE(high_limiter_knee_db, 0.0, 18.0)},
	{RANGE(high_limiter_attack_ms, 0.1, 100.0)},
	{RANGE(high_limiter_release_ms, 1.0, 1000.0)},
	{RANGE(lookahead_limit_dbfs, -30.0, -0.1)},
	{RANGE(lookahead_ms, 0.1, 20.0)},
	{RANGE(lookahead_attack_ms, 0.1, 20.0)},
	{RANGE(lookahead_release_ms, 1.0, 5000.0)},
	{RANGE(post_limiter_lowpass_hz, 5000.0, 20000.0)},
	{RANGE(output_gain_db, -30.0, 30.0)},
};

/** @brief Assert the chain validator rejects a selected field value.
 * @param offset Byte offset of the configuration field under test.
 * @param value Input value or writable result, as declared.
 */
static void expect_invalid_field(size_t offset, double value)
{
	static struct txagc_settings settings_value;
	struct txagc_chain *chain;
	settings_defaults(&settings_value);
	chain = &settings_value.profiles[0].chains[TXAGC_LOCAL];
	*(double *)((char *)&chain->agc + offset) = value;
	assert(validate_chain(chain) < 0);
}

/** @brief Verify every numeric boundary. */
static void test_every_numeric_boundary(void)
{
	for (size_t index = 0; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
		expect_invalid_field(ranges[index].offset, NAN);
		expect_invalid_field(ranges[index].offset, INFINITY);
		expect_invalid_field(ranges[index].offset, -INFINITY);
		expect_invalid_field(ranges[index].offset, ranges[index].minimum - 1.0);
		expect_invalid_field(ranges[index].offset, ranges[index].maximum + 1.0);
	}
	static struct txagc_settings value;
	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LOCAL].agc.low_limiter_threshold_dbfs = NAN;
	assert(validate_chain(&value.profiles[0].chains[TXAGC_LOCAL]) < 0);
	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LOCAL].agc.low_limiter_attack_ms = NAN;
	assert(validate_chain(&value.profiles[0].chains[TXAGC_LOCAL]) < 0);
}

/** @brief Verify stage and relationship validation. */
static void test_stage_and_relationship_validation(void)
{
	static struct txagc_settings settings_value;
	struct txagc_chain *chain;
	settings_defaults(&settings_value);
	chain = &settings_value.profiles[0].chains[TXAGC_LOCAL];
	chain->agc.stage_count = TXAGC_MAX_DYNAMICS_STAGES + 1;
	assert(validate_chain(chain) < 0);
	settings_defaults(&settings_value);
	chain->agc.stage_order[0] = (enum txagc_stage) - 1;
	assert(validate_chain(chain) < 0);
	settings_defaults(&settings_value);
	chain->agc.stage_order[0] = (enum txagc_stage)99;
	assert(validate_chain(chain) < 0);
	settings_defaults(&settings_value);
	chain->agc.stage_order[1] = chain->agc.stage_order[0];
	assert(validate_chain(chain) < 0);
	settings_defaults(&settings_value);
	chain->agc.ctcss_filter_mode = (enum txagc_ctcss_filter_mode) - 1;
	assert(validate_chain(chain) < 0);
	settings_defaults(&settings_value);
	chain->agc.ctcss_filter_mode = (enum txagc_ctcss_filter_mode)99;
	assert(validate_chain(chain) < 0);

#define INVALID_RELATION(field, other)                                                             \
	do {                                                                                       \
		settings_defaults(&settings_value);                                                \
		chain = &settings_value.profiles[0].chains[TXAGC_LOCAL];                           \
		chain->agc.field = chain->agc.other;                                               \
		assert(validate_chain(chain) < 0);                                                 \
	} while (0)
	INVALID_RELATION(receive_bandpass_lowpass_hz, receive_bandpass_highpass_hz);
	INVALID_RELATION(agc_activity_threshold_dbfs, target_dbfs);
	INVALID_RELATION(sidechain_lowpass_hz, sidechain_highpass_hz);
	INVALID_RELATION(expander_sidechain_lowpass_hz, expander_sidechain_highpass_hz);
	INVALID_RELATION(compressor_sidechain_lowpass_hz, compressor_sidechain_highpass_hz);
	INVALID_RELATION(compressor_high_crossover_hz, compressor_low_crossover_hz);
	INVALID_RELATION(limiter_high_crossover_hz, limiter_low_crossover_hz);
#undef INVALID_RELATION
}

/** @brief Install a bounded option list in the fake Asterisk configuration.
 * @param options Filter option string or allowed-name table, as declared.
 * @param count Number of elements available in the supplied block.
 */
static void set_fake_options(const struct fake_option *options, size_t count)
{
	memcpy(fake_options, options, count * sizeof(*options));
	fake_option_count = count;
}

/** @brief Verify gated-AGC defaults, option mappings, and independent detector filters. */
static void test_agc_configuration(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	static const struct {
		const char *name;
		size_t offset;
		double default_value;
		double minimum;
		double maximum;
		const char *replacement;
	} controls[] = {
		{"agc_target_dbfs", offsetof(struct txagc_config, target_dbfs), -24.0, -40.0, -3.0,
		 "-20"},
		{"agc_max_gain_db", offsetof(struct txagc_config, max_gain_db), 6.0, 0.0, 30.0,
		 "9"},
		{"agc_max_attenuation_db", offsetof(struct txagc_config, max_attenuation_db), 6.0,
		 0.0, 60.0, "12"},
		{"agc_rms_averaging_ms", offsetof(struct txagc_config, agc_rms_averaging_ms), 200.0,
		 10.0, 5000.0, "250"},
		{"agc_gain_increase_db_per_second",
		 offsetof(struct txagc_config, agc_gain_increase_db_per_second), 2.0, 0.1, 100.0,
		 "1"},
		{"agc_gain_decrease_db_per_second",
		 offsetof(struct txagc_config, agc_gain_decrease_db_per_second), 6.0, 0.1, 100.0,
		 "8"},
		{"agc_activity_threshold_dbfs",
		 offsetof(struct txagc_config, agc_activity_threshold_dbfs), -50.0, -100.0, -3.1,
		 "-45"},
		{"agc_activity_hysteresis_db",
		 offsetof(struct txagc_config, agc_activity_hysteresis_db), 3.0, 0.0, 12.0, "4"},
		{"agc_hold_ms", offsetof(struct txagc_config, agc_hold_ms), 500.0, 0.0, 10000.0,
		 "600"},
		{"agc_deadband_db", offsetof(struct txagc_config, agc_deadband_db), 1.0, 0.0, 6.0,
		 "2"},
		{"agc_sidechain_highpass_hz", offsetof(struct txagc_config, sidechain_highpass_hz),
		 800.0, 0.0, 2000.0, "300"},
		{"agc_sidechain_lowpass_hz", offsetof(struct txagc_config, sidechain_lowpass_hz),
		 1500.0, 0.0, 3500.0, "2000"},
	};
	static const char *const sections[] = {"local", "link", "voice_telemetry"};
	for (size_t index = 0; index < ARRAY_LEN(controls); ++index) {
		assert(known_chain_option(controls[index].name));
		for (size_t source = 0; source < ARRAY_LEN(sections); ++source) {
			const struct fake_option option = {sections[source], controls[index].name,
							   controls[index].replacement};
			struct txagc_chain *chain;
			double *field;
			settings_defaults(&value);
			chain = &value.profiles[0].chains[source];
			field = (double *)((char *)&chain->agc + controls[index].offset);
			assert(!chain->agc.agc_enabled);
			assert(*field == controls[index].default_value);
			set_fake_options(&option, 1);
			settings_parse_error = 0;
			assert(!read_chain(config, sections[source], chain));
			assert(!settings_parse_error);
			assert(*field == strtod(controls[index].replacement, NULL));
			assert(!validate_chain(chain));
			/* Keep dependent bounds clear while checking each accepted endpoint. */
			chain->agc.target_dbfs = -3.0;
			chain->agc.sidechain_highpass_hz = 0.0;
			chain->agc.sidechain_lowpass_hz = 3500.0;
			*field = controls[index].minimum;
			assert(!validate_chain(chain));
			*field = controls[index].maximum;
			assert(!validate_chain(chain));
			static const char *const invalid_numbers[] = {"nan", "inf", "-inf", "12x"};
			for (size_t invalid = 0; invalid < ARRAY_LEN(invalid_numbers); ++invalid) {
				const struct fake_option bad = {sections[source],
								controls[index].name,
								invalid_numbers[invalid]};
				set_fake_options(&bad, 1);
				settings_parse_error = 0;
				assert(!read_chain(config, sections[source], chain));
				assert(settings_parse_error);
			}
		}
	}
	settings_defaults(&value);
	struct txagc_chain *chain = &value.profiles[0].chains[TXAGC_LOCAL];
	chain->agc.sidechain_highpass_hz = 0.0;
	chain->agc.sidechain_lowpass_hz = 0.0;
	assert(!validate_chain(chain));
	chain->agc.sidechain_highpass_hz = 50.0;
	assert(!validate_chain(chain));
	chain->agc.sidechain_highpass_hz = 49.0;
	assert(validate_chain(chain) < 0);
	chain->agc.sidechain_highpass_hz = 0.0;
	chain->agc.sidechain_lowpass_hz = 1.0;
	assert(!validate_chain(chain));
	fake_option_count = 0;
	settings_parse_error = 0;
}

/** @brief Verify all dynamics band controls parse, retain defaults, and reject bad values. */
static void test_dynamics_band_configuration(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	static const struct {
		const char *name;
		size_t offset;
		double default_value;
		double minimum;
		double maximum;
	} controls[] = {
		{"compressor_low_crossover_hz",
		 offsetof(struct txagc_config, compressor_low_crossover_hz), 500.0, 100.0, 2000.0},
		{"compressor_high_crossover_hz",
		 offsetof(struct txagc_config, compressor_high_crossover_hz), 2000.0, 100.0,
		 5000.0},
		{"compressor_low_threshold_dbfs",
		 offsetof(struct txagc_config, compressor_low_threshold_dbfs), -6.0, -60.0, 0.0},
		{"compressor_low_ratio", offsetof(struct txagc_config, compressor_low_ratio), 2.0,
		 1.0, 20.0},
		{"compressor_low_makeup_gain_db",
		 offsetof(struct txagc_config, compressor_low_makeup_gain_db), 0.0, -30.0, 30.0},
		{"compressor_low_knee_db", offsetof(struct txagc_config, compressor_low_knee_db),
		 9.0, 0.0, 18.0},
		{"compressor_low_attack_ms",
		 offsetof(struct txagc_config, compressor_low_attack_ms), 75.0, 1.0, 1000.0},
		{"compressor_low_release_ms",
		 offsetof(struct txagc_config, compressor_low_release_ms), 300.0, 1.0, 9000.0},
		{"compressor_mid_threshold_dbfs",
		 offsetof(struct txagc_config, compressor_mid_threshold_dbfs), -6.0, -60.0, 0.0},
		{"compressor_mid_ratio", offsetof(struct txagc_config, compressor_mid_ratio), 2.0,
		 1.0, 20.0},
		{"compressor_mid_makeup_gain_db",
		 offsetof(struct txagc_config, compressor_mid_makeup_gain_db), 0.0, -30.0, 30.0},
		{"compressor_mid_knee_db", offsetof(struct txagc_config, compressor_mid_knee_db),
		 9.0, 0.0, 18.0},
		{"compressor_mid_attack_ms",
		 offsetof(struct txagc_config, compressor_mid_attack_ms), 75.0, 1.0, 1000.0},
		{"compressor_mid_release_ms",
		 offsetof(struct txagc_config, compressor_mid_release_ms), 300.0, 1.0, 9000.0},
		{"compressor_high_threshold_dbfs",
		 offsetof(struct txagc_config, compressor_high_threshold_dbfs), -6.0, -60.0, 0.0},
		{"compressor_high_ratio", offsetof(struct txagc_config, compressor_high_ratio), 2.0,
		 1.0, 20.0},
		{"compressor_high_makeup_gain_db",
		 offsetof(struct txagc_config, compressor_high_makeup_gain_db), 0.0, -30.0, 30.0},
		{"compressor_high_knee_db", offsetof(struct txagc_config, compressor_high_knee_db),
		 9.0, 0.0, 18.0},
		{"compressor_high_attack_ms",
		 offsetof(struct txagc_config, compressor_high_attack_ms), 75.0, 1.0, 1000.0},
		{"compressor_high_release_ms",
		 offsetof(struct txagc_config, compressor_high_release_ms), 300.0, 1.0, 9000.0},
		{"limiter_threshold_dbfs", offsetof(struct txagc_config, limiter_threshold_dbfs),
		 -1.5, -40.0, -1.0},
		{"limiter_ratio", offsetof(struct txagc_config, limiter_ratio), 20.0, 1.0, 20.0},
		{"limiter_knee_db", offsetof(struct txagc_config, limiter_knee_db), 0.0, 0.0, 18.0},
		{"limiter_attack_ms", offsetof(struct txagc_config, limiter_attack_ms), 1.0, 0.1,
		 1000.0},
		{"limiter_release_ms", offsetof(struct txagc_config, limiter_release_ms), 50.0, 1.0,
		 9000.0},
	};
	static const char *const sections[] = {"local", "link", "voice_telemetry"};
	static const char *const bad_numbers[] = {"", " ", "nan", "inf", "-inf", "12x"};
	for (size_t index = 0; index < ARRAY_LEN(controls); ++index) {
		assert(known_chain_option(controls[index].name));
		for (size_t source = 0; source < ARRAY_LEN(sections); ++source) {
			struct txagc_chain *chain;
			double *field;
			char replacement[32];
			double replacement_value =
				controls[index].default_value +
				(controls[index].default_value == controls[index].maximum ? -0.125
											  : 0.125);
			snprintf(replacement, sizeof(replacement), "%.17g", replacement_value);
			const struct fake_option option = {sections[source], controls[index].name,
							   replacement};
			settings_defaults(&value);
			chain = &value.profiles[0].chains[source];
			field = (double *)((char *)&chain->agc + controls[index].offset);
			assert(*field == controls[index].default_value);
			assert(!chain->agc.compressor_enabled && !chain->agc.limiter_enabled);
			set_fake_options(&option, 1);
			settings_parse_error = 0;
			assert(!read_chain(config, sections[source], chain));
			assert(!settings_parse_error);
			assert(*field == replacement_value);
			assert(!validate_chain(chain));
			char scoped_section[64];
			snprintf(scoped_section, sizeof(scoped_section), "%s usb",
				 sections[source]);
			const struct fake_option scoped_option = {
				scoped_section, controls[index].name, replacement};
			fake_config_load_result = config;
			fake_category_count = 2;
			fake_categories[0] = "usb";
			fake_categories[1] = scoped_section;
			fake_variables[0] = fake_variables[1] = NULL;
			set_fake_options(&scoped_option, 1);
			assert(!load_settings());
			assert(*(double *)((char *)&settings.profiles[0].chains[source].agc +
					   controls[index].offset) == replacement_value);
			fake_category_count = 0;
			/* Clear the companion edge before testing either crossover endpoint. */
			chain->agc.compressor_low_crossover_hz = 100.0;
			chain->agc.compressor_high_crossover_hz = 5000.0;
			*field = controls[index].minimum;
			if (!strcmp(controls[index].name, "compressor_high_crossover_hz"))
				*field += 0.125;
			assert(!validate_chain(chain));
			*field = controls[index].maximum;
			assert(!validate_chain(chain));
			for (size_t invalid = 0; invalid < ARRAY_LEN(bad_numbers); ++invalid) {
				const struct fake_option bad = {sections[source],
								controls[index].name,
								bad_numbers[invalid]};
				set_fake_options(&bad, 1);
				settings_parse_error = 0;
				assert(!read_chain(config, sections[source], chain));
				assert(settings_parse_error);
			}
		}
	}
	static const struct {
		const char *name;
		size_t offset;
	} modes[] = {
		{"compressor_bands", offsetof(struct txagc_config, compressor_bands)},
		{"limiter_bands", offsetof(struct txagc_config, limiter_bands)},
	};
	static const char *const bad_modes[] = {
		"", "0", "2", "4", "-1", "1.5", "3x", "nan", "999999999999999999999999"};
	for (size_t mode = 0; mode < ARRAY_LEN(modes); ++mode) {
		assert(known_chain_option(modes[mode].name));
		for (size_t source = 0; source < ARRAY_LEN(sections); ++source) {
			settings_defaults(&value);
			struct txagc_chain *chain = &value.profiles[0].chains[source];
			int *field = (int *)((char *)&chain->agc + modes[mode].offset);
			assert(*field == 3);
			for (int count = 1; count <= 3; count += 2) {
				const struct fake_option option = {
					sections[source], modes[mode].name, count == 1 ? "1" : "3"};
				set_fake_options(&option, 1);
				settings_parse_error = 0;
				assert(!read_chain(config, sections[source], chain));
				assert(!settings_parse_error && *field == count);
				assert(!validate_chain(chain));
				char scoped_section[64];
				snprintf(scoped_section, sizeof(scoped_section), "%s usb",
					 sections[source]);
				const struct fake_option scoped_option = {
					scoped_section, modes[mode].name, count == 1 ? "1" : "3"};
				fake_config_load_result = config;
				fake_category_count = 2;
				fake_categories[0] = "usb";
				fake_categories[1] = scoped_section;
				fake_variables[0] = fake_variables[1] = NULL;
				set_fake_options(&scoped_option, 1);
				assert(!load_settings());
				assert(*(int *)((char *)&settings.profiles[0].chains[source].agc +
						modes[mode].offset) == count);
				fake_category_count = 0;
			}
			for (size_t invalid = 0; invalid < ARRAY_LEN(bad_modes); ++invalid) {
				const struct fake_option option = {
					sections[source], modes[mode].name, bad_modes[invalid]};
				set_fake_options(&option, 1);
				settings_parse_error = 0;
				assert(!read_chain(config, sections[source], chain));
				assert(settings_parse_error && *field == 3);
			}
			for (int count = -1; count <= 4; ++count) {
				*field = count;
				assert((validate_chain(chain) == 0) == (count == 1 || count == 3));
			}
		}
	}
	fake_option_count = 0;
	settings_parse_error = 0;
}

/** @brief Verify settings scope and hardware validation. */
static void test_settings_scope_and_hardware_validation(void)
{
	static struct txagc_settings value;
	struct usbradioplus_hardware_settings *hardware;
	struct txagc_chain *link;

	settings_defaults(&value);
	value.profiles[0].channel[0] = '\0';
	assert(validate_profile(&value.profiles[0]) < 0);

#define INVALID_HARDWARE(field, configured, number)                                                \
	do {                                                                                       \
		settings_defaults(&value);                                                         \
		hardware = &value.profiles[0].hardware;                                            \
		hardware->configured = 1;                                                          \
		hardware->field = number;                                                          \
		assert(validate_profile(&value.profiles[0]) < 0);                                  \
	} while (0)
	INVALID_HARDWARE(input_gain_db, input_gain_configured, -31.0);
	INVALID_HARDWARE(input_gain_db, input_gain_configured, 31.0);
	INVALID_HARDWARE(output_a_gain_db, output_a_gain_configured, -31.0);
	INVALID_HARDWARE(output_a_gain_db, output_a_gain_configured, 31.0);
	INVALID_HARDWARE(output_b_gain_db, output_b_gain_configured, -31.0);
	INVALID_HARDWARE(output_b_gain_db, output_b_gain_configured, 31.0);
#undef INVALID_HARDWARE
	settings_defaults(&value);
	value.profiles[0].hardware.input_gain_configured = 1;
	value.profiles[0].hardware.output_a_gain_configured = 1;
	value.profiles[0].hardware.output_b_gain_configured = 1;
	assert(!validate_profile(&value.profiles[0]));
	value.profiles[0].hardware.input_gain_configured = 0;
	value.profiles[0].hardware.output_a_gain_configured = 0;
	value.profiles[0].hardware.output_b_gain_configured = 0;
	assert(!validate_profile(&value.profiles[0]));

	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LINK].agc.target_dbfs = 0.0;
	assert(validate_profile(&value.profiles[0]) < 0);
	settings_defaults(&value);
	link = &value.profiles[0].chains[TXAGC_LINK];
	link->rnnoise_enabled = 1;
	assert(validate_profile(&value.profiles[0]) < 0);
	settings_defaults(&value);
	link = &value.profiles[0].chains[TXAGC_LINK];
	link->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	assert(validate_profile(&value.profiles[0]) < 0);
	settings_defaults(&value);
	link = &value.profiles[0].chains[TXAGC_LINK];
	link->agc.receive_bandpass_enabled = 1;
	assert(validate_profile(&value.profiles[0]) < 0);

	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LOCAL].agc.lookahead_limiter_enabled = 1;
	assert(validate_profile(&value.profiles[0]) < 0);
	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LOCAL].agc.post_limiter_lowpass_enabled = 1;
	assert(validate_profile(&value.profiles[0]) < 0);
}

/** @brief Verify primitive configuration parsers. */
static void test_primitive_configuration_parsers(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	double number = 7.0;
	int boolean = 7;
	int assignment = -1;
	int configured = 0;
	const struct fake_option values[] = {
		{"test", "number", "12.5"},    {"test", "bad_number", "12x"},
		{"test", "infinite", "inf"},   {"test", "modern", "3.5"},
		{"test", "yes", "yes"},	       {"test", "no", "no"},
		{"test", "bad_bool", "maybe"},
	};

	set_fake_options(values, sizeof(values) / sizeof(values[0]));
	settings_parse_error = 0;
	read_double(config, "test", "missing", &number);
	assert(number == 7.0);
	read_double(config, "test", "number", &number);
	assert(number == 12.5);
	read_double(config, "test", "bad_number", &number);
	assert(settings_parse_error);
	settings_parse_error = 0;
	read_double(config, "test", "infinite", &number);
	assert(settings_parse_error);
	settings_parse_error = 0;
	read_bool(config, "test", "missing", &boolean);
	assert(boolean == 7);
	read_bool(config, "test", "yes", &boolean);
	assert(boolean == 1);
	read_bool(config, "test", "no", &boolean);
	assert(boolean == 0);
	read_bool(config, "test", "bad_bool", &boolean);
	assert(settings_parse_error);

	assert(known_chain_option("enabled"));
	assert(known_chain_option("output_gain_db"));
	assert(!known_chain_option("not_an_option"));
	assert(!known_chain_option("target_dbfs"));
	assert(!option_in_list("missing", asterisk_override_options,
			       ARRAY_LEN(asterisk_override_options)));
	assert(valid_frequency_list("67.0, 100.0, 250.3"));
	assert(!valid_frequency_list(""));
	assert(!valid_frequency_list("  "));
	assert(!valid_frequency_list("tone"));
	assert(!valid_frequency_list("49"));
	assert(!valid_frequency_list("301"));
	assert(!valid_frequency_list("nan"));
	assert(!valid_frequency_list("-1"));
	assert(!valid_frequency_list("100.0001"));
	assert(!valid_frequency_list("100 200"));
	assert(!valid_frequency_list("100,"));
	assert(!valid_frequency_list("100\t,\t200"));

	fake_option_count = 0;
	assert(!read_assignment(config, "hardware", "hardware_output_a_assignment", &assignment,
				&configured));
	assert(!configured);
	const char *names[] = {"off", "voice", "ctcss", "voice_ctcss", "auxvoice", "invalid"};
	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		const struct fake_option option = {"hardware", "hardware_output_a_assignment",
						   names[index]};
		set_fake_options(&option, 1);
		configured = 0;
		int result = read_assignment(config, "hardware", "hardware_output_a_assignment",
					     &assignment, &configured);
		assert((index < 5 && !result && configured) || (index == 5 && result < 0));
	}
	const char *obsolete[] = {"no", "tone", "composite"};
	for (size_t index = 0; index < ARRAY_LEN(obsolete); ++index) {
		const struct fake_option option = {"hardware", "hardware_output_a_assignment",
						   obsolete[index]};
		set_fake_options(&option, 1);
		configured = 0;
		assert(read_assignment(config, "hardware", "hardware_output_a_assignment",
				       &assignment, &configured) < 0);
		assert(configured);
	}
}

/** @brief Verify hardware configuration parser. */
static void test_hardware_configuration_parser(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct usbradioplus_hardware_settings hardware = {0};
	const struct fake_option values[] = {
		{"hardware", "hardware_input_gain_db", "-12.5"},
		{"hardware", "hardware_output_a_gain_db", "3.0"},
		{"hardware", "hardware_output_b_gain_db", "4.0"},
		{"hardware", "hardware_output_a_assignment", "voice"},
		{"hardware", "hardware_output_b_assignment", "ctcss"},
	};

	set_fake_options(values, ARRAY_LEN(values));
	settings_parse_error = 0;
	assert(!read_hardware(config, "hardware", &hardware));
	assert(!settings_parse_error);
	assert(hardware.input_gain_configured && hardware.input_gain_db == -12.5);
	assert(hardware.output_a_gain_configured && hardware.output_a_gain_db == 3.0);
	assert(hardware.output_b_gain_configured && hardware.output_b_gain_db == 4.0);
	assert(hardware.output_a_assignment == USBRADIOPLUS_HW_VOICE);
	assert(hardware.output_b_assignment == USBRADIOPLUS_HW_CTCSS);

	const struct fake_option invalid_cases[] = {
		{"hardware", "hardware_output_a_assignment", "bad"},
		{"hardware", "hardware_output_b_assignment", "bad"},
	};
	for (size_t index = 0; index < ARRAY_LEN(invalid_cases); ++index) {
		memset(&hardware, 0, sizeof(hardware));
		set_fake_options(&invalid_cases[index], 1);
		assert(read_hardware(config, "hardware", &hardware) != 0);
	}

	const struct fake_option bad_gain = {"hardware", "hardware_input_gain_db", "bad"};
	memset(&hardware, 0, sizeof(hardware));
	set_fake_options(&bad_gain, 1);
	settings_parse_error = 0;
	assert(!read_hardware(config, "hardware", &hardware));
	assert(settings_parse_error && hardware.input_gain_configured);
}

/** @brief Verify chain configuration parser. */
static void test_chain_configuration_parser(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	const struct fake_option values[] = {
		{"local", "enabled", "yes"},
		{"local", "rnnoise_enabled", "yes"},
		{"local", "receive_bandpass_enabled", "yes"},
		{"local", "receive_bandpass_highpass_hz", "25"},
		{"local", "receive_bandpass_lowpass_hz", "5500"},
		{"local", "ctcss_filter_mode", "notch"},
		{"local", "input_gain_db", "2.5"},
		{"local", "agc_target_dbfs", "-10"},
		{"local", "stage_order", "equalizer,expander,agc,deesser,compressor,limiter"},
	};

	settings_defaults(&value);
	set_fake_options(values, ARRAY_LEN(values));
	settings_parse_error = 0;
	assert(!read_chain(config, "local", &value.profiles[0].chains[TXAGC_LOCAL]));
	assert(!settings_parse_error);
	assert(value.profiles[0].chains[TXAGC_LOCAL].enabled);
	assert(value.profiles[0].chains[TXAGC_LOCAL].rnnoise_enabled);
	assert(value.profiles[0].chains[TXAGC_LOCAL].input_gain_configured);
	assert(value.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode ==
	       TXAGC_CTCSS_FILTER_NOTCH);
	assert(value.profiles[0].chains[TXAGC_LOCAL].agc.stage_count == TXAGC_MAX_DYNAMICS_STAGES);

	const char *valid_modes[] = {"highpass", "disabled", "off"};
	for (size_t index = 0; index < ARRAY_LEN(valid_modes); ++index) {
		const struct fake_option option = {"local", "ctcss_filter_mode",
						   valid_modes[index]};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		settings_parse_error = 0;
		assert(!read_chain(config, "local", &value.profiles[0].chains[TXAGC_LOCAL]));
		assert(!settings_parse_error);
	}

	const struct fake_option invalid_mode = {"local", "ctcss_filter_mode", "comb"};
	settings_defaults(&value);
	set_fake_options(&invalid_mode, 1);
	settings_parse_error = 0;
	assert(!read_chain(config, "local", &value.profiles[0].chains[TXAGC_LOCAL]));
	assert(settings_parse_error);

	const struct fake_option invalid_order = {"local", "stage_order", "agc,agc"};
	settings_defaults(&value);
	set_fake_options(&invalid_order, 1);
	assert(read_chain(config, "local", &value.profiles[0].chains[TXAGC_LOCAL]) < 0);
}

/** @brief Install a single synthetic section override.
 * @param section Flat or resolved configuration section name.
 * @param name Option, metadata field, or channel name.
 * @param text Complete configuration text.
 * @return Result used by the test's assertions.
 */
static int add_single_override(const char *section, const char *name, const char *text)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	const struct fake_option option = {section, name, text};
	settings_defaults(&value);
	set_fake_options(&option, 1);
	return add_override(&value.profiles[0], config, section, name);
}

/** @brief Verify section override parser. */
static void test_section_override_parser(void)
{
	static struct txagc_settings value;
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	const struct {
		char *section;
		const char *name;
		const char *valid;
		const char *invalid;
	} cases[] = {
		{"asterisk", "asterisk_jitter_buffer_enabled", "yes", "maybe"},
		{"asterisk", "asterisk_jitter_buffer_force_enabled", "no", "maybe"},
		{"receive", "polarity_inverted", "yes", "maybe"},
		{"transmit", "preemphasis_enabled", "yes", "maybe"},
		{"asterisk", "asterisk_jitter_buffer_implementation", "adaptive", "other"},
		{"hardware", "hardware_emphasis_corner_hz", "120", "300"},
		{"hardware", "hardware_gpio_1_mode", "in", "bad"},
		{"hardware", "hardware_parallel_pin_10_assignment", "cor", "out0"},
		{"hardware", "hardware_parallel_pin_2_assignment", "ptt", "in"},
		{"hardware", "hardware_parallel_port_device", "/dev/parport0", ""},
		{"hardware", "hardware_parallel_port_base_address", "0x378", "bad"},
		{"receive", "audio_source", "flat", "bad"},
		{"receive", "cos_assignment", "dsp", "bad"},
		{"receive", "vox_hang_ms", "32767", "32768"},
		{"receive", "vox_threshold", "32767", "32768"},
		{"receive", "noise_squelch_hysteresis", "32767", "32768"},
		{"receive", "noise_filter_type", "1", "2"},
		{"receive", "squelch_delay_ms", "511", "512"},
		{"receive", "on_delay_frames", "3000", "3001"},
		{"receive", "frequency_hz", "146520000", "146.5"},
		{"ctcss", "receive_source", "dsp", "bad"},
		{"ctcss", "receive_relax", "1", "2"},
		{"ctcss", "turnoff_mode", "ctcss_phase_shift", "bad"},
		{"ctcss", "receive_frequencies", "88.5, 100.0", "bad"},
		{"ctcss", "transmit_default_hz", "100.0", "0"},
		{"ctcss", "phase_shift_degrees", "120", "0"},
		/* The state machine reserves two 20 ms frames before tail signaling;
		 * the signed 16-bit timer then bounds the other end of the range. */
		{"ctcss", "tail_duration_ms", "40", "39"},
		{"dcs", "receive_code", "000N", "bad"},
		{"dcs", "transmit_code", "777I", "888N"},
		{"dcs", "turnoff_duration_ms", "180", "149"},
		{"transmit", "rx_blanking_ms", "32767", "32768"},
		{"transmit", "off_delay_frames", "3000", "3001"},
		{"ctcss", "receive_decoder_gain_db", "12.5", "61"},
		{"duplex", "duplex_local_repeat_mode", "software", "bad"},
		{"hardware", "hardware_device_identifier", "usb", NULL},
		{"hardware", "hardware_serial", "serial", NULL},
		{"hardware", "hardware_user_key", "key", NULL},
		{"hardware", "hardware_audio_fragment_count", "2", "-1"},
		{"receive", "squelch_level", "999", "1000"},
		{"ctcss", "transmit_peak_dbfs", "-24", "-91"},
		{"dcs", "peak_dbfs", "-24", "-91"},
		{"duplex", "duplex_local_repeat_level", "500", "1000"},
		{"hardware", "hardware_clip_led_gpio", "8", "9"},
		{"hardware", "hardware_interface_type", "1", "2"},
		{"duplex", "duplex_radio_mode", "0", "2"},
	};

	fake_option_count = 0;
	settings_defaults(&value);
	assert(!add_override(&value.profiles[0], config, "hardware", "hardware_serial"));
	for (size_t index = 0; index < ARRAY_LEN(cases); ++index) {
		assert(!add_single_override(cases[index].section, cases[index].name,
					    cases[index].valid));
		if (cases[index].invalid)
			assert(add_single_override(cases[index].section, cases[index].name,
						   cases[index].invalid) < 0);
	}
	assert(!add_single_override("asterisk", "asterisk_jitter_buffer_implementation", "fixed"));
	assert(!add_single_override("hardware", "hardware_gpio_1_mode", "out0"));
	assert(!add_single_override("hardware", "hardware_gpio_1_mode", "out1"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_10_assignment", "in"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_10_assignment", "ctcss"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_2_assignment", "out0"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_2_assignment", "out1"));
	assert(!add_single_override("receive", "audio_source", "no"));
	assert(!add_single_override("receive", "audio_source", "speaker"));
	assert(!add_single_override("receive", "cos_assignment", "usb"));
	const char *ctcss_sources[] = {"no", "usb", "usbinvert", "pp", "ppinvert"};
	for (size_t index = 0; index < ARRAY_LEN(ctcss_sources); ++index)
		assert(!add_single_override("ctcss", "receive_source", ctcss_sources[index]));
	assert(!add_single_override("ctcss", "turnoff_mode", "no"));
	assert(!add_single_override("ctcss", "turnoff_mode", "ctcss_tone_remove"));
	assert(!add_single_override("ctcss", "turnoff_mode", "ctcss_phase_shift"));
	assert(!add_single_override("ctcss", "turnoff_mode", "ctcss_tail_tone"));
	assert(add_single_override("ctcss", "turnoff_mode", "ste") < 0);
	assert(add_single_override("ctcss", "tail_duration_ms", "32768") < 0);
	assert(!add_single_override("duplex", "duplex_local_repeat_mode", "hardware"));
	assert(add_single_override("hardware", "hardware_emphasis_corner_hz", "") < 0);
	assert(add_single_override("hardware", "hardware_emphasis_corner_hz", "120x") < 0);
	assert(add_single_override("hardware", "hardware_emphasis_corner_hz", "0") < 0);
	assert(add_single_override("hardware", "hardware_parallel_port_base_address", "") < 0);
	assert(add_single_override("hardware", "hardware_parallel_port_base_address", "1x") < 0);
	assert(add_single_override("hardware", "hardware_audio_fragment_count", "1x") < 0);
	assert(!add_single_override("hardware", "hardware_parallel_pin_12_assignment", "in"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_13_assignment", "in"));
	assert(!add_single_override("hardware", "hardware_parallel_pin_15_assignment", "in"));
	assert(add_single_override("hardware", "hardware_emphasis_corner_hz", "nan") < 0);
	assert(add_single_override("hardware", "hardware_parallel_port_base_address",
				   "0x100000000") < 0);
	assert(add_single_override("hardware", "hardware_audio_fragment_count", "nan") < 0);

	settings_defaults(&value);
	value.profiles[0].override_count = MAX_SECTION_OVERRIDES;
	const struct fake_option full = {"hardware", "hardware_serial", "serial"};
	set_fake_options(&full, 1);
	assert(add_override(&value.profiles[0], config, "hardware", "hardware_serial") < 0);

	const struct fake_option all_sections[] = {
		{"asterisk", asterisk_override_options[0], "yes"},
		{"hardware", hardware_override_options[0], "usb"},
		{"duplex", duplex_override_options[0], "1"},
		{"diagnostics", diagnostics_override_options[0], "1"},
		{"general", "channel_enabled", "yes"},
	};
	settings_defaults(&value);
	set_fake_options(all_sections, ARRAY_LEN(all_sections));
	assert(!read_section_overrides(&value.profiles[0], config, "asterisk", "hardware",
				       "receive", "transmit", "ctcss", "dcs", "duplex",
				       "diagnostics"));
	assert(value.profiles[0].override_count == ARRAY_LEN(all_sections) - 1);
	/* Each section loop must stop at its own malformed entry. */
	const struct fake_option rejected_sections[] = {
		{"receive", "signaling_method", "invalid"},
		{"dcs", "receive_code", "invalid"},
		{"ctcss", "receive_frequencies", "invalid"},
	};
	for (size_t index = 0; index < ARRAY_LEN(rejected_sections); ++index) {
		settings_defaults(&value);
		set_fake_options(&rejected_sections[index], 1);
		assert(read_section_overrides(&value.profiles[0], config, "asterisk", "hardware",
					      "receive", "transmit", "ctcss", "dcs", "duplex",
					      "diagnostics") < 0);
	}
}

/** @brief Verify option name validation. */
static void test_option_name_validation(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct ast_variable variable = {0};

	fake_category_count = 1;
	fake_categories[0] = "unknown";
	fake_variables[0] = NULL;
	assert(!validate_option_names(config));
	static const char *const removed_chain_options[] = {
		"agc_floor_dbfs",
		"agc_attack_ms",
		"agc_release_ms",
		"agc_reset_after_ms",
		"splatter_filter_enabled",
		"splatter_filter_highpass_hz",
		"splatter_filter_lowpass_hz",
		"output_highpass_hz",
		"output_lowpass_hz",
	};
	static char *const agc_sections[] = {"local",	   "link",	"voice_telemetry",
					     "local test", "link test", "voice_telemetry test"};
	for (size_t option = 0; option < ARRAY_LEN(removed_chain_options); ++option) {
		assert(!known_chain_option(removed_chain_options[option]));
		for (size_t section = 0; section < ARRAY_LEN(agc_sections); ++section) {
			variable.name = removed_chain_options[option];
			fake_categories[0] = agc_sections[section];
			fake_variables[0] = &variable;
			assert(validate_option_names(config) < 0);
		}
	}

	const struct {
		char *section;
		const char *name;
		int valid;
	} cases[] = {
		{"test", "channel_enabled", 1},
		{"test", "hardware_profile", 1},
		{"test", "channel", 0},
		{"asterisk test", asterisk_override_options[0], 1},
		{"hardware test", "hardware_input_gain_db", 1},
		{"ctcss test", "receive_frequencies", 1},
		{"dcs test", "receive_code", 1},
		{"transmit test", "preemphasis_enabled", 1},
		{"transmit test", "hardware_tx_preemphasis_enabled", 0},
		{"transmit test", "hardware_tx_preemphasis_limiter_enabled", 0},
		{"transmit test", "hardware_tx_limiter_only_enabled", 0},
		{"transmit test", "hardware_tx_soft_limiter_setpoint", 0},
		{"hardware test", hardware_override_options[0], 1},
		{"duplex test", duplex_override_options[0], 1},
		{"diagnostics test", diagnostics_override_options[0], 1},
		{"local test", "output_gain_db", 1},
		{"voice_telemetry test", "receive_bandpass_enabled", 0},
		{"test", "unknown", 0},
	};
	for (size_t index = 0; index < ARRAY_LEN(cases); ++index) {
		memset(&variable, 0, sizeof(variable));
		variable.name = cases[index].name;
		fake_category_count = 1;
		fake_categories[0] = cases[index].section;
		fake_variables[0] = &variable;
		assert((validate_option_names(config) == 0) == cases[index].valid);
	}
	memset(&variable, 0, sizeof(variable));
	variable.name = "equalizer_enabled";
	fake_categories[0] = "link test";
	fake_variables[0] = &variable;
	assert(!validate_option_names(config));
	const char *unknown_sections[] = {"asterisk test", "hardware test", "duplex test",
					  "diagnostics test"};
	for (size_t index = 0; index < ARRAY_LEN(unknown_sections); ++index) {
		memset(&variable, 0, sizeof(variable));
		variable.name = "unknown";
		fake_categories[0] = (char *)unknown_sections[index];
		fake_variables[0] = &variable;
		assert(validate_option_names(config) < 0);
	}
	fake_category_count = 0;
}

/** @brief Verify the three explicit per-direction signaling selections. */
static void test_signaling_methods(void)
{
	static const char *const methods[] = {"carrier", "ctcss", "dcs"};
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	size_t index;

	for (index = 0; index < ARRAY_LEN(methods); ++index) {
		const struct fake_option option = {"receive", "signaling_method", methods[index]};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		assert(!read_section_overrides(&value.profiles[0], config, "asterisk", "hardware",
					       "receive", "transmit", "ctcss", "dcs", "duplex",
					       "diagnostics"));
	}
	{
		const struct fake_option option = {"transmit", "signaling_method", "both"};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		assert(read_section_overrides(&value.profiles[0], config, "asterisk", "hardware",
					      "receive", "transmit", "ctcss", "dcs", "duplex",
					      "diagnostics") < 0);
	}
	fake_option_count = 0;
}

/** @brief Verify unified configuration edge paths. */
static void test_unified_configuration_edge_paths(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	static struct txagc_settings value;
	struct ast_variable variable = {.name = "channel_enabled"};
	char section[MAX_CONFIG_SECTION];
	char long_kind[80];

	assert(!validate_named_option("general", "general", &variable));
	variable.name = "unknown";
	assert(validate_named_option("general", "general", &variable) < 0);
	assert(validate_named_option("unknown", "unknown", &variable) < 0);

	memset(long_kind, 'x', sizeof(long_kind) - 1);
	long_kind[sizeof(long_kind) - 1] = '\0';
	assert(resolve_profile_section(config, "usb", long_kind, section, sizeof(section)) < 0);
	assert(resolve_profile_section(config, "usb", "hardware", section, 2) < 0);
	const struct fake_option missing_profile = {"usb", "hardware_profile", "missing"};
	set_fake_options(&missing_profile, 1);
	assert(resolve_profile_section(config, "usb", "hardware", section, sizeof(section)) < 0);
	const struct fake_option existing_profile = {"usb", "hardware_profile", "shared"};
	fake_category_count = 1;
	fake_categories[0] = "hardware shared";
	fake_variables[0] = NULL;
	set_fake_options(&existing_profile, 1);
	assert(!resolve_profile_section(config, "usb", "hardware", section, sizeof(section)));
	struct ast_variable enabled_variable = {.name = "enabled"};
	fake_categories[0] = "local ";
	fake_variables[0] = &enabled_variable;
	fake_option_count = 0;
	assert(validate_option_names(config) < 0);
	char long_category[MAX_CONFIG_SECTION + 16];
	memset(long_category, 'x', sizeof(long_category) - 1);
	long_category[40] = ' ';
	long_category[sizeof(long_category) - 1] = '\0';
	fake_categories[0] = long_category;
	assert(validate_option_names(config) < 0);

	const struct {
		const char *section;
		const char *name;
		const char *value;
	} invalid_overrides[] = {
		{"asterisk", "asterisk_jitter_buffer_enabled", "maybe"},
		{"hardware", "hardware_eeprom_enabled", "maybe"},
		{"duplex", "duplex_local_repeat_mode", "invalid"},
		{"diagnostics", "diagnostics_trace_type", "invalid"},
	};
	for (size_t index = 0; index < ARRAY_LEN(invalid_overrides); ++index) {
		const struct fake_option option = {invalid_overrides[index].section,
						   invalid_overrides[index].name,
						   invalid_overrides[index].value};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		assert(read_section_overrides(&value.profiles[0], config, "asterisk", "hardware",
					      "receive", "transmit", "ctcss", "dcs", "duplex",
					      "diagnostics") < 0);
	}
	const struct fake_option scoped = {"hardware usb", "hardware_serial", "serial"};
	settings_defaults(&value);
	set_fake_options(&scoped, 1);
	assert(!add_override(&value.profiles[0], config, "hardware usb", "hardware_serial"));
	assert(!strcmp(value.profiles[0].overrides[0].section, "hardware"));
	settings_defaults(&value);
	value.profiles[0].override_count = MAX_SECTION_OVERRIDES;
	const struct fake_option enabled = {"usb", "channel_enabled", "yes"};
	set_fake_options(&enabled, 1);
	assert(read_profile_overrides(&value.profiles[0], config, "usb", "asterisk usb",
				      "hardware usb", "receive usb", "transmit usb", "ctcss usb",
				      "dcs usb", "duplex usb", "diagnostics usb") < 0);
	fake_option_count = 0;
}

/** @brief Call the private override parser with one isolated value.
 * @param section Configuration section which owns the option.
 * @param name Option to parse.
 * @param text Candidate text.
 * @param expected_valid Nonzero when the text must be retained.
 *
 * Each check starts with a fresh profile so successful values cannot consume
 * the finite override table needed by later validation cases.
 */
static void assert_override_validity(const char *section, const char *name, const char *text,
				     int expected_valid)
{
	static struct txagc_settings value;
	const struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	const struct fake_option option = {section, name, text};

	settings_defaults(&value);
	set_fake_options(&option, 1);
	assert((!add_override(&value.profiles[0], (struct ast_config *)config, section, name)) ==
	       expected_valid);
	fake_option_count = 0;
}

/** @brief Exercise every short-circuit arm in clean-slate value validation. */
static void test_override_validation_short_circuits(void)
{
	struct ast_variable variable = {.name = "audio_source"};
	static const struct {
		const char *section;
		const char *name;
		const char *text;
		int valid;
	} cases[] = {
		{"hardware", "hardware_emphasis_corner_hz", "", 0},
		{"hardware", "hardware_emphasis_corner_hz", "100Hz", 0},
		{"hardware", "hardware_emphasis_corner_hz", "nan", 0},
		{"hardware", "hardware_emphasis_corner_hz", "0", 0},
		{"hardware", "hardware_emphasis_corner_hz", "300", 0},
		{"hardware", "hardware_emphasis_corner_hz", "100", 1},
		{"ctcss", "transmit_frequencies", "100.0", 1},
		{"ctcss", "transmit_default_hz", "", 0},
		{"ctcss", "transmit_default_hz", "100Hz", 0},
		{"ctcss", "transmit_default_hz", "nan", 0},
		{"ctcss", "transmit_default_hz", "0", 0},
		{"ctcss", "transmit_default_hz", "123.4", 0},
		{"ctcss", "transmit_default_hz", "100", 1},
		{"ctcss", "tail_frequency_hz", "", 0},
		{"ctcss", "tail_frequency_hz", "55Hz", 0},
		{"ctcss", "tail_frequency_hz", "nan", 0},
		{"ctcss", "tail_frequency_hz", "0", 0},
		{"ctcss", "tail_frequency_hz", "55", 1},
		{"ctcss", "tail_duration_ms", "", 0},
		{"ctcss", "tail_duration_ms", "40ms", 0},
		{"ctcss", "tail_duration_ms", "0", 0},
		{"ctcss", "tail_duration_ms", "32768", 0},
		{"ctcss", "tail_duration_ms", "40", 1},
		{"ctcss", "turnoff_mode", "no", 1},
		{"ctcss", "turnoff_mode", "ctcss_phase_shift", 1},
		{"ctcss", "turnoff_mode", "ctcss_tone_remove", 1},
		{"ctcss", "turnoff_mode", "ctcss_tail_tone", 1},
		{"ctcss", "turnoff_mode", "invalid", 0},
		{"dcs", "turnoff_duration_ms", "", 0},
		{"dcs", "turnoff_duration_ms", "180ms", 0},
		{"dcs", "turnoff_duration_ms", "0", 0},
		{"dcs", "turnoff_duration_ms", "201", 0},
		{"dcs", "turnoff_duration_ms", "150", 1},
		{"ctcss", "receive_source", "no", 1},
		{"ctcss", "receive_source", "usb", 1},
		{"ctcss", "receive_source", "usbinvert", 1},
		{"ctcss", "receive_source", "dsp", 1},
		{"ctcss", "receive_source", "pp", 1},
		{"ctcss", "receive_source", "ppinvert", 1},
		{"ctcss", "receive_source", "invalid", 0},
		{"receive", "cos_assignment", "no", 1},
		{"receive", "cos_assignment", "usb", 1},
		{"receive", "cos_assignment", "usbinvert", 1},
		{"receive", "cos_assignment", "dsp", 1},
		{"receive", "cos_assignment", "vox", 1},
		{"receive", "cos_assignment", "pp", 1},
		{"receive", "cos_assignment", "ppinvert", 1},
		{"receive", "cos_assignment", "invalid", 0},
		{"ctcss", "receive_decoder_gain_db", "", 0},
		{"ctcss", "receive_decoder_gain_db", "1dB", 0},
		{"ctcss", "receive_decoder_gain_db", "nan", 0},
		{"ctcss", "receive_decoder_gain_db", "-61", 0},
		{"ctcss", "receive_decoder_gain_db", "61", 0},
		{"ctcss", "receive_decoder_gain_db", "0", 1},
		{"ctcss", "transmit_peak_dbfs", "", 0},
		{"ctcss", "transmit_peak_dbfs", "1dB", 0},
		{"ctcss", "transmit_peak_dbfs", "nan", 0},
		{"ctcss", "transmit_peak_dbfs", "-91", 0},
		{"ctcss", "transmit_peak_dbfs", "1", 0},
		{"ctcss", "transmit_peak_dbfs", "-24", 1},
	};

	/* The named-section switch has a success edge for every supported chain. */
	assert(!validate_named_option("receive", "receive", &variable));
	variable.name = "enabled";
	assert(!validate_named_option("local", "local", &variable));
	assert(!validate_named_option("link", "link", &variable));
	assert(!validate_named_option("voice_telemetry", "voice_telemetry", &variable));
	variable.name = "unknown";
	assert(validate_named_option("dcs", "dcs", &variable) < 0);
	assert(validate_named_option("ctcss", "ctcss", &variable) < 0);

	/* These spellings deliberately force each DCS validation conjunction arm. */
	assert(!valid_dcs_code(NULL));
	assert(!valid_dcs_code(""));
	assert(!valid_dcs_code("000"));
	assert(!valid_dcs_code("/00i"));
	assert(!valid_dcs_code("x00i"));
	assert(!valid_dcs_code("800i"));
	assert(!valid_dcs_code("0/0i"));
	assert(!valid_dcs_code("080i"));
	assert(!valid_dcs_code("00/i"));
	assert(!valid_dcs_code("008i"));
	assert(!valid_dcs_code("000x"));
	assert(valid_dcs_code("000n"));
	assert(valid_dcs_code("000I"));
	assert(valid_dcs_code("000i"));

	assert(!valid_nonnegative_integer("", 1));
	assert(!valid_nonnegative_integer("1x", 1));
	assert(!valid_nonnegative_integer("999999999999999999999999", LONG_MAX));
	assert(!valid_nonnegative_integer("-1", 1));
	assert(!valid_nonnegative_integer("2", 1));
	assert(valid_nonnegative_integer("1", 1));

	for (size_t index = 0; index < ARRAY_LEN(cases); ++index)
		assert_override_validity(cases[index].section, cases[index].name, cases[index].text,
					 cases[index].valid);
}

/** @brief Reject each unresolved profile reference before any later section is read. */
static void test_profile_reference_failure_short_circuits(void)
{
	static const char *const kinds[] = {
		"asterisk", "hardware",	   "receive", "transmit", "ctcss",	     "dcs",
		"duplex",   "diagnostics", "local",   "link",	  "voice_telemetry",
	};
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;

	fake_config_load_result = config;
	fake_category_count = 1;
	fake_categories[0] = "test";
	fake_variables[0] = NULL;
	for (size_t index = 0; index < ARRAY_LEN(kinds); ++index) {
		struct txagc_audio_snapshot *snapshot = NULL;
		struct fake_option option;
		char name[64];

		assert(snprintf(name, sizeof(name), "%s_profile", kinds[index]) > 0);
		option = (struct fake_option){"test", name, "missing"};
		set_fake_options(&option, 1);
		assert(load_settings_candidate(&snapshot) < 0);
		assert(!snapshot);
	}
	fake_option_count = 0;
	fake_category_count = 0;
}

/** @brief Exercise callback-safe accessors with an immutable published snapshot. */
static void test_realtime_snapshot_accessors(void)
{
	struct txagc_chain chain;
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct fake_option options[] = {
		{"snapshot", "channel_enabled", "yes"},
		{"local snapshot", "enabled", "yes"},
		{"voice_telemetry snapshot", "enabled", "yes"},
	};

	fake_iterator_available = 0;
	(void)usbradioplus_processing_unload();
	fake_config_load_result = config;
	fake_category_count = 3;
	fake_categories[0] = "snapshot";
	fake_categories[1] = "local snapshot";
	fake_categories[2] = "voice_telemetry snapshot";
	fake_variables[0] = fake_variables[1] = fake_variables[2] = NULL;
	set_fake_options(options, ARRAY_LEN(options));
	assert(!load_settings());
	assert(!usbradioplus_processing_get_local_rt("snapshot", &chain));
	assert(chain.enabled);
	assert(!usbradioplus_processing_get_composite_rt("snapshot", &chain));
	assert(chain.enabled);

	/* A new published snapshot changes only profile qualification. */
	options[0].value = "no";
	set_fake_options(options, ARRAY_LEN(options));
	assert(!load_settings());
	assert(!usbradioplus_processing_get_local_rt("snapshot", &chain));
	assert(!chain.enabled);
	assert(!usbradioplus_processing_get_composite_rt("snapshot", &chain));
	assert(!chain.enabled);
	options[0].value = "yes";
	options[2].value = "no";
	set_fake_options(options, ARRAY_LEN(options));
	assert(!load_settings());
	assert(!usbradioplus_processing_get_composite_rt("snapshot", &chain));
	assert(!chain.enabled);
	(void)usbradioplus_processing_unload();
	fake_option_count = 0;
	fake_category_count = 0;
}

/** @brief Verify settings loader. */
static void test_settings_loader(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;

	fake_calloc_call = 0;
	fake_calloc_fail_call = 1;
	assert(load_settings() < 0);
	fake_calloc_call = 0;
	fake_calloc_fail_call = 2;
	assert(load_settings() < 0);
	fake_calloc_fail_call = 0;
	fake_option_count = 0;
	fake_category_count = 0;
	fake_config_destroy_count = 0;
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(load_settings() < 0);
	assert(!fake_config_destroy_count);

	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(load_settings() < 0);

	fake_config_load_result = valid;
	assert(load_settings() < 0);
	assert(fake_config_destroy_count == 1);

	const struct fake_option configured[] = {
		{"general", "channel_enabled", "yes"},
		{"local", "enabled", "yes"},
		{"test", "channel_enabled", "yes"},
		{"local test", "enabled", "no"},
		{"link test", "enabled", "yes"},
		{"local test", "agc_enabled", "yes"},
		{"voice_telemetry test", "compressor_enabled", "yes"},
	};
	static char *const categories[] = {"general",
					   "local",
					   "test",
					   "asterisk test",
					   "hardware test",
					   "duplex test",
					   "diagnostics test",
					   "local test",
					   "link test",
					   "voice_telemetry test"};
	fake_category_count = ARRAY_LEN(categories);
	for (size_t index = 0; index < fake_category_count; ++index) {
		fake_categories[index] = categories[index];
		fake_variables[index] = NULL;
	}
	set_fake_options(configured, ARRAY_LEN(configured));
	assert(!load_settings());
	assert(settings.profiles[0].enabled);
	assert(!strcmp(settings.profiles[0].channel, "RadioPlus/test"));
	assert(!settings.profiles[0].chains[TXAGC_LOCAL].enabled);
	assert(settings.profiles[0].chains[TXAGC_LINK].enabled);
	assert(settings.profiles[0].chains[TXAGC_LOCAL].agc.agc_enabled);
	assert(settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].agc.compressor_enabled);
	{
		struct txagc_chain chain;
		struct usbradioplus_hardware_settings hardware;

		assert(!usbradioplus_processing_get_local_rt("test", &chain));
		assert(!usbradioplus_processing_get_hardware_rt("test", &hardware));
	}
	fake_calloc_call = 0;
	fake_calloc_fail_call = 3;
	assert(load_settings() < 0);
	fake_calloc_fail_call = 0;
	const struct fake_option disabled = {"usb", "channel_enabled", "no"};
	fake_category_count = 1;
	fake_categories[0] = "usb";
	fake_variables[0] = NULL;
	set_fake_options(&disabled, 1);
	assert(!load_settings());
	assert(!settings.profiles[0].enabled);
	const struct fake_option flat_disabled[] = {{"general", "channel_enabled", "no"},
						    {"usb", "channel_enabled", "yes"}};
	fake_category_count = 2;
	fake_categories[0] = "general";
	fake_categories[1] = "usb";
	fake_variables[0] = fake_variables[1] = NULL;
	set_fake_options(flat_disabled, ARRAY_LEN(flat_disabled));
	assert(!load_settings());
	assert(settings.profiles[0].enabled);

	fake_option_count = 0;
	fake_category_count = 0;
}

/** @brief Verify settings loader rejections. */
static void test_settings_loader_rejections(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;
	static char names[MAX_RADIO_PROFILES + 1][16];
	const struct fake_option bad_general = {"general", "channel_enabled", "maybe"};
	static char *const flat_categories[] = {"usb",	 "asterisk", "hardware",
						"local", "link",     "voice_telemetry"};
	static char *const scoped_categories_all[] = {"usb",	      "asterisk usb",
						      "hardware usb", "local usb",
						      "link usb",     "voice_telemetry usb"};

	fake_config_load_result = valid;
	fake_category_count = 1;
	fake_categories[0] = "general";
	fake_variables[0] = NULL;
	set_fake_options(&bad_general, 1);
	assert(load_settings() < 0);
	struct ast_variable unknown = {.name = "unknown"};
	fake_categories[0] = "hardware";
	fake_variables[0] = &unknown;
	fake_option_count = 0;
	assert(load_settings() < 0);
	fake_variables[0] = NULL;
	const struct fake_option bad_flat_hardware = {"hardware", "hardware_output_a_assignment",
						      "invalid"};
	set_fake_options(&bad_flat_hardware, 1);
	assert(load_settings() < 0);

	const struct fake_option flat_failures[] = {
		{"asterisk", "asterisk_jitter_buffer_enabled", "maybe"},
		{"local", "stage_order", "invalid"},
		{"link", "stage_order", "invalid"},
		{"voice_telemetry", "stage_order", "invalid"},
		{"local", "agc_target_dbfs", "bad"},
	};
	for (size_t index = 0; index < ARRAY_LEN(flat_failures); ++index) {
		fake_category_count = ARRAY_LEN(flat_categories);
		for (size_t category = 0; category < fake_category_count; ++category) {
			fake_categories[category] = flat_categories[category];
			fake_variables[category] = NULL;
		}
		set_fake_options(&flat_failures[index], 1);
		assert(load_settings() < 0);
	}

	for (size_t index = 0; index < ARRAY_LEN(names); ++index) {
		snprintf(names[index], sizeof(names[index]), "radio%zu", index);
		fake_categories[index] = names[index];
		fake_variables[index] = NULL;
	}
	fake_category_count = ARRAY_LEN(names);
	fake_option_count = 0;
	assert(load_settings() < 0);

	const struct fake_option bad_channel = {"usb", "channel_enabled", "maybe"};
	fake_category_count = 1;
	fake_categories[0] = "usb";
	set_fake_options(&bad_channel, 1);
	assert(load_settings() < 0);

	const struct fake_option scoped_failures[] = {
		{"asterisk usb", "asterisk_jitter_buffer_enabled", "maybe"},
		{"local usb", "stage_order", "invalid"},
		{"link usb", "stage_order", "invalid"},
		{"voice_telemetry usb", "stage_order", "invalid"},
		{"local usb", "agc_target_dbfs", "bad"},
	};
	for (size_t index = 0; index < ARRAY_LEN(scoped_failures); ++index) {
		fake_category_count = ARRAY_LEN(scoped_categories_all);
		for (size_t category = 0; category < fake_category_count; ++category) {
			fake_categories[category] = scoped_categories_all[category];
			fake_variables[category] = NULL;
		}
		set_fake_options(&scoped_failures[index], 1);
		assert(load_settings() < 0);
	}

	const char *const profile_options[] = {"asterisk_profile",	 "hardware_profile",
					       "duplex_profile",	 "diagnostics_profile",
					       "local_profile",		 "link_profile",
					       "voice_telemetry_profile"};
	for (size_t index = 0; index < ARRAY_LEN(profile_options); ++index) {
		const struct fake_option missing = {"usb", profile_options[index], "missing"};
		set_fake_options(&missing, 1);
		assert(load_settings() < 0);
	}
	static char *const scoped_categories[] = {"usb", "hardware usb"};
	for (size_t index = 0; index < ARRAY_LEN(scoped_categories); ++index) {
		fake_categories[index] = scoped_categories[index];
		fake_variables[index] = NULL;
	}
	fake_category_count = ARRAY_LEN(scoped_categories);
	const struct fake_option bad_scoped_hardware = {"hardware usb",
							"hardware_output_a_assignment", "invalid"};
	set_fake_options(&bad_scoped_hardware, 1);
	assert(load_settings() < 0);
	static char *const chain_categories[] = {"usb", "local usb"};
	for (size_t index = 0; index < ARRAY_LEN(chain_categories); ++index)
		fake_categories[index] = chain_categories[index];
	fake_category_count = ARRAY_LEN(chain_categories);
	const struct fake_option invalid_profile = {"local usb", "equalizer_low_frequency_hz",
						    "5000"};
	set_fake_options(&invalid_profile, 1);
	assert(load_settings() < 0);
	fake_option_count = 0;
	fake_category_count = 0;
}

/** @brief Replace one fake section override at the requested index.
 * @param index Sample position within the trace block.
 * @param section Flat or resolved configuration section name.
 * @param name Option, metadata field, or channel name.
 * @param value Input value or writable result, as declared.
 */
static void set_override_value(size_t index, const char *section, const char *name,
			       const char *value)
{
	ast_copy_string(settings.profiles[0].overrides[index].section, section,
			sizeof(settings.profiles[0].overrides[index].section));
	ast_copy_string(settings.profiles[0].overrides[index].name, name,
			sizeof(settings.profiles[0].overrides[index].name));
	ast_copy_string(settings.profiles[0].overrides[index].value, value,
			sizeof(settings.profiles[0].overrides[index].value));
}

/** @brief Verify public setting accessors. */
static void test_public_setting_accessors(void)
{
	static const struct {
		const char *section;
		const char *name;
		const char *value;
	} clean_slate_defaults[] = {
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
	struct txagc_chain chain;
	struct usbradioplus_hardware_settings hardware;
	char text[32];
	size_t index;

	settings_defaults(&settings);
	for (index = 0; index < ARRAY_LEN(clean_slate_defaults); ++index) {
		assert(!usbradioplus_processing_get_option(
			"usb", clean_slate_defaults[index].section,
			clean_slate_defaults[index].name, text, sizeof(text)));
		assert(!strcmp(text, clean_slate_defaults[index].value));
	}
	settings.profiles[0].enabled = 0;
	assert(usbradioplus_processing_get_local("usb", NULL) < 0);
	assert(usbradioplus_processing_get_local_rt("usb", NULL) < 0);
	assert(usbradioplus_processing_get_local(NULL, &chain) == 1);
	assert(!usbradioplus_processing_get_local("usb", &chain));
	assert(!usbradioplus_processing_get_local("RadioPlus/usb", &chain));
	assert(!usbradioplus_processing_get_local_rt("RadioPlus/usb", &chain));
	memset(&chain, 0xa5, sizeof(chain));
	assert(usbradioplus_processing_get_local("missing", &chain) == 1);
	assert(!chain.enabled);
	assert(!chain.agc.stage_count && chain.agc.input_gain_db == 0.0);
	settings.profiles[0].enabled = 1;
	assert(!usbradioplus_processing_get_local("usb", &chain) && chain.enabled);
	settings.profiles[0].chains[TXAGC_LOCAL].enabled = 0;
	assert(!usbradioplus_processing_get_local("usb", &chain) && !chain.enabled);
	assert(usbradioplus_processing_get_hardware("usb", NULL) < 0);
	assert(usbradioplus_processing_get_hardware_rt("usb", NULL) < 0);
	assert(!usbradioplus_processing_get_hardware("usb", &hardware));
	assert(hardware.input_gain_configured);
	memset(&hardware, 0xa5, sizeof(hardware));
	assert(usbradioplus_processing_get_hardware("missing", &hardware) == 1);
	assert(!hardware.input_gain_configured && hardware.input_gain_db == 0.0);
	assert(!hardware.output_a_assignment && !hardware.output_b_assignment);
	assert(usbradioplus_processing_get_composite("usb", NULL) < 0);
	assert(!usbradioplus_processing_get_composite("usb", &chain));
	assert(chain.enabled);
	memset(&chain, 0xa5, sizeof(chain));
	assert(usbradioplus_processing_get_composite("missing", &chain) == 1);
	assert(!chain.enabled && !chain.agc.stage_count);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].enabled = 0;
	assert(!usbradioplus_processing_get_composite("usb", &chain) && !chain.enabled);
	settings.profiles[0].chains[TXAGC_VOICE_TELEMETRY].enabled = 1;
	settings.profiles[0].enabled = 0;
	assert(!usbradioplus_processing_get_composite("usb", &chain) && !chain.enabled);

	assert(usbradioplus_processing_set_local_input_gain("usb", NAN) < 0);
	assert(usbradioplus_processing_set_local_input_gain("usb", -31.0) < 0);
	assert(usbradioplus_processing_set_local_input_gain("usb", 31.0) < 0);
	fake_calloc_failure = 1;
	assert(usbradioplus_processing_set_local_input_gain("usb", 2.0) < 0);
	fake_calloc_failure = 0;
	assert(!usbradioplus_processing_set_local_input_gain("usb", 2.0));
	assert(usbradioplus_processing_set_local_input_gain("missing", 2.0) == 1);
	assert(settings.profiles[0].chains[TXAGC_LOCAL].agc.input_gain_db == 2.0);
	assert(settings.profiles[0].chains[TXAGC_LOCAL].input_gain_configured);
	assert(settings.profiles[0].agc.input_gain_db == 2.0);
	assert(usbradioplus_processing_set_hardware_input_gain("usb", NAN) < 0);
	assert(usbradioplus_processing_set_hardware_input_gain("usb", -31.0) < 0);
	assert(usbradioplus_processing_set_hardware_input_gain("usb", 31.0) < 0);
	fake_calloc_failure = 1;
	assert(usbradioplus_processing_set_hardware_input_gain("usb", -2.0) < 0);
	fake_calloc_failure = 0;
	assert(!usbradioplus_processing_set_hardware_input_gain("usb", -2.0));
	assert(usbradioplus_processing_set_hardware_input_gain("missing", -2.0) == 1);
	assert(settings.profiles[0].hardware.input_gain_db == -2.0);
	assert(settings.profiles[0].hardware.input_gain_configured);

	assert(usbradioplus_processing_get_option(NULL, "x", "x", text, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("usb", NULL, "x", text, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("usb", "x", NULL, text, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("usb", "x", "x", NULL, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("usb", "x", "x", text, 0) < 0);
	settings.profiles[0].override_count = 2;
	set_override_value(0, "receive", "squelch_level", "500");
	set_override_value(1, "receive", "squelch_level", "450");
	assert(!usbradioplus_processing_get_option("usb", "receive", "squelch_level", text,
						   sizeof(text)));
	assert(!strcmp(text, "450"));
	settings.profiles[0].override_count = 5;
	set_override_value(0, "asterisk", asterisk_override_options[0], "yes");
	set_override_value(1, "hardware", hardware_override_options[0], "usb");
	set_override_value(2, "duplex", duplex_override_options[0], "1");
	set_override_value(3, "diagnostics", diagnostics_override_options[0], "2");
	set_override_value(4, "general", "channel_enabled", "no");
	assert(usbradioplus_processing_get_option("usb", "hardware", "rxmixerset", text,
						  sizeof(text)) == 1);
	assert(usbradioplus_processing_get_option("usb", "hardware", "missing", text,
						  sizeof(text)) == 1);
	assert(!usbradioplus_processing_get_option("usb", "asterisk", asterisk_override_options[0],
						   text, sizeof(text)));
	assert(!usbradioplus_processing_get_option("usb", "hardware", hardware_override_options[0],
						   text, sizeof(text)));
	assert(!usbradioplus_processing_get_option("usb", "duplex", duplex_override_options[0],
						   text, sizeof(text)));
	assert(!usbradioplus_processing_get_option(
		"usb", "diagnostics", diagnostics_override_options[0], text, sizeof(text)));
	assert(usbradioplus_processing_get_option("usb", "general", "missing", text,
						  sizeof(text)) == 1);
	assert(usbradioplus_processing_get_option("usb", "other", "missing", text, sizeof(text)) ==
	       1);
	assert(usbradioplus_processing_get_option("missing", "other", "missing", text,
						  sizeof(text)) == 1);
}

/** @brief Reset configuration-save stub state. */
static void reset_save_doubles(void)
{
	static char *const categories[] = {"usb", "hardware usb", "local usb"};
	fake_config_destroy_count = 0;
	fake_category_count = ARRAY_LEN(categories);
	for (size_t index = 0; index < fake_category_count; ++index) {
		fake_categories[index] = categories[index];
		fake_variables[index] = NULL;
	}
	fake_category_new_failure = 0;
	fake_category_append_count = 0;
	fake_save_result = 0;
	fake_tune_update_failure_call = 0;
	fake_tune_update_calls = 0;
}

/** @brief Verify input gain persistence. */
static void test_input_gain_persistence(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;

	assert(usbradioplus_processing_save_input_gains("usb", NAN, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains("usb", -31.0, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains("usb", 31.0, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, NAN) < 0);
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, -31.0) < 0);
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 31.0) < 0);

	reset_save_doubles();
	fake_config_load_result = NULL;
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 0.0) < 0);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	assert(!usbradioplus_processing_save_input_gains("usb", 1.25, -2.5));
	assert(fake_tune_update_calls == 2 && fake_config_destroy_count == 1);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_tune_update_failure_call = 1;
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_tune_update_failure_call = 2;
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_save_result = -1;
	assert(usbradioplus_processing_save_input_gains("usb", 0.0, 0.0) < 0);
}

/** @brief Verify generic option persistence. */
static void test_generic_option_persistence(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;
	const struct usbradioplus_config_update valid_update = {"hardware", "setting", "value"};
	char long_section[MAX_CONFIG_SECTION + 1];
	const struct usbradioplus_config_update invalid_updates[] = {
		{NULL, "setting", "value"},
		{"hardware", NULL, "value"},
		{"hardware", "setting", NULL},
	};

	assert(usbradioplus_processing_save_options(NULL, NULL, 0) < 0);
	assert(usbradioplus_processing_save_options("usb", NULL, 1) < 0);
	reset_save_doubles();
	fake_config_load_result = valid;
	assert(!usbradioplus_processing_save_options("usb", NULL, 0));
	reset_save_doubles();
	fake_config_load_result = NULL;
	assert(usbradioplus_processing_save_options("usb", &valid_update, 1) < 0);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_save_options("usb", &valid_update, 1) < 0);

	for (size_t index = 0; index < ARRAY_LEN(invalid_updates); ++index) {
		reset_save_doubles();
		fake_config_load_result = valid;
		assert(usbradioplus_processing_save_options("usb", &invalid_updates[index], 1) < 0);
	}
	memset(long_section, 'x', sizeof(long_section) - 1);
	long_section[sizeof(long_section) - 1] = '\0';
	const struct usbradioplus_config_update unresolved = {long_section, "setting", "value"};
	reset_save_doubles();
	fake_config_load_result = valid;
	assert(usbradioplus_processing_save_options("usb", &unresolved, 1) < 0);
	reset_save_doubles();
	fake_config_load_result = valid;
	fake_category_count = 1;
	fake_categories[0] = "usb";
	fake_category_new_failure = 1;
	assert(usbradioplus_processing_save_options("usb", &valid_update, 1) < 0);
	reset_save_doubles();
	fake_config_load_result = valid;
	fake_category_count = 1;
	fake_categories[0] = "usb";
	assert(!usbradioplus_processing_save_options("usb", &valid_update, 1));
	assert(fake_category_append_count == 1);
}

/** @brief Verify module lifecycle and simple cli. */
static void test_module_lifecycle_and_simple_cli(void)
{
	struct ast_cli_entry entry = {0};
	struct ast_cli_args arguments = {.fd = 1, .argc = 3};
	struct ast_cli_args bad_arguments = {.fd = 1, .argc = 2};

	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_load() == AST_MODULE_LOAD_DECLINE);
	reset_save_doubles();
	fake_config_load_result = (struct ast_config *)(uintptr_t)1;
	fake_option_count = 0;
	fake_cli_register_result = -1;
	assert(usbradioplus_processing_load() == AST_MODULE_LOAD_DECLINE);
	fake_cli_register_result = 0;
	fake_thread_create_result = -1;
	fake_cli_unregister_calls = 0;
	assert(usbradioplus_processing_load() == AST_MODULE_LOAD_FAILURE);
	assert(fake_cli_unregister_calls == 1);
	fake_thread_create_result = 0;
	assert(usbradioplus_processing_load() == AST_MODULE_LOAD_SUCCESS);
	fake_pthread_join_calls = 0;
	scan_thread = (pthread_t)1;
	assert(!usbradioplus_processing_unload());
	assert(fake_pthread_join_calls == 1 && scan_thread == AST_PTHREADT_NULL);
	assert(!usbradioplus_processing_unload());

	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(usbradioplus_processing_prime() < 0);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_prime() < 0);
	assert(usbradioplus_processing_reload() < 0);
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(usbradioplus_processing_reload() < 0);

	assert(cli_enable(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_enable(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_enable(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	fake_calloc_failure = 1;
	assert(cli_enable(&entry, 99, &arguments) == CLI_FAILURE);
	fake_calloc_failure = 0;
	assert(cli_enable(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(settings.profiles[0].enabled);

	assert(cli_disable(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_disable(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_disable(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	fake_calloc_failure = 1;
	assert(cli_disable(&entry, 99, &arguments) == CLI_FAILURE);
	fake_calloc_failure = 0;
	assert(cli_disable(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(!settings.profiles[0].enabled);

	assert(cli_reload(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_reload(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_reload(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(cli_reload(&entry, 99, &arguments) == CLI_FAILURE);
	reset_save_doubles();
	fake_config_load_result = (struct ast_config *)(uintptr_t)1;
	assert(cli_reload(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(fake_cli_print_calls > 0);
	assert(!usbradioplus_processing_reload());
	stopping = 0;
	assert(scanner(NULL) == NULL);
	assert(stopping);
}

/** @brief Verify channel eligibility. */
static void test_channel_eligibility(void)
{
	static struct txagc_settings value;
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	settings_defaults(&value);
	value.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	assert(channel_is_eligible(channel, &value.profiles[0]));
	value.profiles[0].chains[TXAGC_LINK].enabled = 0;
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	value.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "PJSIP/test";
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	fake_channel_name = "IAX2/test";
	fake_channel_application = NULL;
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	fake_channel_application = "Other";
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	fake_channel_application = "Rpt";
	fake_channel_data = NULL;
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	fake_channel_data = "Other";
	assert(!channel_is_eligible(channel, &value.profiles[0]));
	fake_channel_data = "Remote Rx";
}

/** @brief Verify audiohook callback and destroy. */
static void test_audiohook_callback_and_destroy(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_frame frame = {0};
	struct txagc_hook hook = {0};
	struct txagc_hook oversized = {0};
	int16_t pcm[] = {100, -100, 2};
	double samples[ARRAY_LEN(pcm)];
	struct ast_audiohook *audiohook = &hook.audiohook;

	settings_defaults(&settings);
	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	prepare_test_link_hook(&hook, "usb", samples, ARRAY_LEN(samples), 8000);
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = pcm;
	frame.samples = ARRAY_LEN(pcm);
	frame.subclass.format = (struct ast_format *)(uintptr_t)1;

	audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	audiohook->status = AST_AUDIOHOOK_STATUS_RUNNING;
	frame.frametype = AST_FRAME_DTMF;
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = NULL;
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.data.ptr = pcm;
	frame.samples = 0;
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.samples = ARRAY_LEN(pcm);
	atomic_store_explicit(&hook.link_enabled, 0, memory_order_release);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	atomic_store_explicit(&hook.link_enabled, 1, memory_order_release);
	fake_channel_name = "RadioPlus/usb";
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	fake_channel_name = "IAX2/test";
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_WRITE));
	atomic_store_explicit(&hook.link_enabled, 0, memory_order_release);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	atomic_store_explicit(&hook.link_enabled, 1, memory_order_release);
	fake_sample_rate = 0;
	fake_processor_result = -1;
	pcm[0] = 100;
	pcm[1] = -100;
	pcm[2] = 2;
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	/* A rejected prepared graph leaves this current PCM frame intact. */
	assert(pcm[0] == 100 && pcm[1] == -100 && pcm[2] == 2);
	fake_sample_rate = 48000;
	fake_processor_result = 0;
	fake_processor_saturate = 1;
	hook.sample_rate = 48000;
	assert(!txagc_avfilter_slot_prepare(&hook.avfilter,
					    &settings.profiles[0].chains[TXAGC_LINK].agc, 48000));
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[2] == 2);
	fake_processor_saturate = 0;
	destroy_test_hook_slots(&hook);

	/* A same-rate oversized frame is bypassed without touching a fixed workspace. */
	fake_sample_rate = 8000;
	prepare_test_link_hook(&oversized, "usb", samples, ARRAY_LEN(samples), 8000);
	frame.data.ptr = pcm;
	frame.samples = ARRAY_LEN(pcm) + 1U;
	assert(!txagc_callback(&oversized.audiohook, channel, &frame,
			       AST_AUDIOHOOK_DIRECTION_READ));
	destroy_test_hook_slots(&oversized);
	fake_sample_rate = 8000;

	fake_audiohook_detach_calls = 0;
	fake_audiohook_destroy_calls = 0;
	fake_processor_destroy_calls = 0;
	hook_destroy(NULL);
	struct txagc_hook *allocated = calloc(1, sizeof(*allocated));
	assert(allocated);
	hook_destroy(allocated);
	assert(fake_audiohook_detach_calls == 1);
	assert(fake_audiohook_destroy_calls == 1);
	assert(fake_processor_destroy_calls == 0);
}

/** @brief Verify synchronous link processing replaces its current callback frame. */
static void test_link_callback_synchronous_contract(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct txagc_hook hook = {0};
	struct txagc_link_statistics statistics;
	struct ast_frame frame = {.frametype = AST_FRAME_VOICE,
				  .samples = 3,
				  .subclass.format = (struct ast_format *)(uintptr_t)1};
	int16_t first[] = {300, -600, 900};
	int16_t saturated[] = {-200, 400, -1200};
	int16_t failed[] = {50, 60, 70};
	int16_t bypassed[] = {-20, -30, -40};
	int16_t oversized[] = {10, 20, 30, 40};
	double samples[ARRAY_LEN(first)];
	unsigned int calls_before;

	settings_defaults(&settings);
	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	fake_sample_rate = 44100;
	fake_processor_calls = 0;
	fake_processor_result = 0;
	fake_processor_saturate = 0;
	prepare_test_link_hook(&hook, "usb", samples, ARRAY_LEN(samples), 44100);

	/* The prepared graph runs on the admitted frame itself: there is no queued
	 * previous frame, worker wakeup, or output concealment interval. */
	frame.data.ptr = first;
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(first[0] == 300 && first[1] == -600 && first[2] == 900);
	assert(fake_processor_calls == 1U);

	/* The callback converts the prepared graph's direct result back to PCM and
	 * clips only the Asterisk integer representation at its rails. */
	fake_processor_saturate = 1;
	frame.data.ptr = saturated;
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(saturated[0] == 32767 && saturated[1] == -32768 && saturated[2] == 2);
	fake_processor_saturate = 0;

	/* A prepared graph failure is fail-open: keep the current link PCM intact,
	 * publish the error, and let a later block run normally. */
	fake_processor_result = -1;
	frame.data.ptr = failed;
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(failed[0] == 50 && failed[1] == 60 && failed[2] == 70);
	fake_processor_result = 0;
	assert(!txagc_link_statistics_read(&hook, &statistics));
	assert(statistics.processing_errors == 1U);

	/* Format and workspace changes bypass rather than allocating or rebuilding
	 * from Asterisk's real-time callback. */
	calls_before = fake_processor_calls;
	fake_sample_rate = 48000;
	frame.data.ptr = bypassed;
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(bypassed[0] == -20 && bypassed[1] == -30 && bypassed[2] == -40);
	assert(fake_processor_calls == calls_before);
	fake_sample_rate = 44100;
	frame.samples = ARRAY_LEN(oversized);
	frame.data.ptr = oversized;
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(oversized[0] == 10 && oversized[1] == 20 && oversized[2] == 30 &&
	       oversized[3] == 40);
	assert(fake_processor_calls == calls_before);

	/* Only incoming link frames enter the graph. */
	frame.samples = 3;
	frame.data.ptr = bypassed;
	assert(!test_link_callback(&hook.audiohook, channel, &frame,
				   AST_AUDIOHOOK_DIRECTION_WRITE));
	atomic_store_explicit(&hook.link_enabled, 0, memory_order_release);
	assert(!test_link_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(fake_processor_calls == calls_before);
	destroy_test_hook_slots(&hook);
	fake_sample_rate = 8000;
}

/** @brief Verify bounded synchronous link-callback diagnostics and guards. */
static void test_link_callback_statistics_and_guards(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct txagc_hook hook = {0};
	struct txagc_link_statistics statistics = {0};
	struct txagc_avfilter filter = {0};
	struct ast_frame frame = {.frametype = AST_FRAME_VOICE,
				  .samples = 3,
				  .subclass.format = (struct ast_format *)(uintptr_t)1};
	int16_t pcm[] = {11, 12, 13};
	double samples[ARRAY_LEN(pcm)];

	/* Copy and publication helpers stay null-safe and never block a callback
	 * behind a concurrent diagnostics reader. */
	link_callback_copy_filter_statistics(NULL, &filter);
	link_callback_copy_filter_statistics(&statistics, NULL);
	filter.input_samples = 11U;
	filter.output_samples = 12U;
	filter.input_peak_dbfs = -10.0;
	filter.output_peak_dbfs = -11.0;
	link_callback_copy_filter_statistics(&statistics, &filter);
	assert(statistics.input_samples == 11U && statistics.output_samples == 12U);
	assert(statistics.input_peak_dbfs == -10.0 && statistics.output_peak_dbfs == -11.0);
	atomic_init(&hook.statistics_index, 0U);
	atomic_init(&hook.statistics_readers[0], 0U);
	atomic_init(&hook.statistics_readers[1], 1U);
	hook.statistics = statistics;
	link_callback_publish_statistics(&hook);
	assert(atomic_load_explicit(&hook.statistics_index, memory_order_acquire) == 0U);
	atomic_store_explicit(&hook.statistics_readers[1], 0U, memory_order_release);
	link_callback_publish_statistics(&hook);
	assert(atomic_load_explicit(&hook.statistics_index, memory_order_acquire) == 1U);
	assert(!txagc_link_statistics_read(&hook, &statistics));
	processing_test_statistics_flip_index = 1;
	assert(!txagc_link_statistics_read(&hook, &statistics));
	processing_test_statistics_flip_index = 3;
	assert(txagc_link_statistics_read(&hook, &statistics) < 0);
	processing_test_statistics_flip_index = 0;
	assert(txagc_link_statistics_read(NULL, &statistics) < 0);
	assert(txagc_link_statistics_read(&hook, NULL) < 0);

	settings_defaults(&settings);
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_sample_rate = 8000;
	fake_processor_result = 0;
	prepare_test_link_hook(&hook, "usb", samples, ARRAY_LEN(samples), 8000);
	frame.data.ptr = pcm;
	/* A graph can disappear only during control-plane replacement. The callback
	 * makes the current frame a clean bypass and records the condition. */
	atomic_store_explicit(&hook.avfilter.active, NULL, memory_order_release);
	assert(!txagc_callback(&hook.audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(pcm[0] == 11 && pcm[1] == 12 && pcm[2] == 13);
	assert(!txagc_link_statistics_read(&hook, &statistics));
	assert(statistics.processing_errors == 1U);
	destroy_test_hook_slots(&hook);
}

/** @brief Exercise private control-plane and callback diagnostics paths directly.
 *
 * Production reaches these cases only after an allocator failure, a stale
 * diagnostics reader, or unavailable graph snapshot. Keeping the assertions
 * here makes their recovery contract explicit without weakening the bounded
 * real-time paths that normally prevent them.
 */
static void test_processing_private_edge_paths(void)
{
	struct txagc_settings candidate;
	struct txagc_chain chain;
	struct usbradioplus_hardware_settings hardware;
	struct txagc_hook staged_hook = {0};
	struct ast_datastore staged_datastore = {.data = &staged_hook};
	struct ast_variable receive_unknown = {.name = "unknown"};
	struct link_graph_transaction *transaction = NULL;
	char value[32];

	/* Null inputs are rejected before any state mutation. */
	assert(find_profile_const(&settings, NULL) == NULL);
	assert(!valid_nonnegative_integer(NULL, 1));
	assert(load_settings_candidate(NULL) < 0);
	assert(validate_named_option("receive", "receive", &receive_unknown) < 0);

	/* Candidate-only accessors must never publish their prospective settings. */
	settings_defaults(&candidate);
	candidate.profiles[0].enabled = 0;
	staged_settings = &candidate;
	assert(!usbradioplus_processing_get_local("usb", &chain));
	assert(!chain.enabled);
	assert(!usbradioplus_processing_get_hardware("usb", &hardware));
	assert(!usbradioplus_processing_get_composite("usb", &chain));
	assert(!chain.enabled);
	assert(usbradioplus_processing_get_local("missing", &chain) == 1);
	assert(usbradioplus_processing_get_hardware("missing", &hardware) == 1);
	assert(usbradioplus_processing_get_composite("missing", &chain) == 1);
	candidate.profiles[0].enabled = 1;
	candidate.profiles[0].chains[TXAGC_LOCAL].enabled = 0;
	assert(!usbradioplus_processing_get_local("usb", &chain));
	assert(!chain.enabled);
	candidate.profiles[0].chains[TXAGC_LOCAL].enabled = 1;
	assert(!usbradioplus_processing_get_local("usb", &chain));
	assert(chain.enabled);
	candidate.profiles[0].chains[TXAGC_VOICE_TELEMETRY].enabled = 0;
	assert(!usbradioplus_processing_get_composite("usb", &chain));
	assert(!chain.enabled);
	candidate.profiles[0].chains[TXAGC_VOICE_TELEMETRY].enabled = 1;
	assert(!usbradioplus_processing_get_composite("usb", &chain));
	assert(chain.enabled);
	candidate.profiles[0].override_count = 1;
	ast_copy_string(candidate.profiles[0].overrides[0].section, "receive",
			sizeof(candidate.profiles[0].overrides[0].section));
	ast_copy_string(candidate.profiles[0].overrides[0].name, "different_option",
			sizeof(candidate.profiles[0].overrides[0].name));
	ast_copy_string(candidate.profiles[0].overrides[0].value, "different_value",
			sizeof(candidate.profiles[0].overrides[0].value));
	assert(!usbradioplus_processing_get_option("usb", "receive", "signaling_method", value,
						   sizeof(value)));
	assert(!strcmp(value, "carrier"));
	assert(usbradioplus_processing_get_option("usb", "receive", "unknown", value,
						  sizeof(value)) == 1);
	assert(usbradioplus_processing_get_option("missing", "receive", "signaling_method", value,
						  sizeof(value)) == 1);
	candidate.profiles[0].override_count = 0;
	staged_settings = NULL;
	/* Link-transaction tests below need an admitted candidate graph. */
	candidate.profiles[0].enabled = 1;
	candidate.profiles[0].chains[TXAGC_LOCAL].enabled = 1;
	candidate.profiles[0].chains[TXAGC_VOICE_TELEMETRY].enabled = 1;
	candidate.profiles[0].chains[TXAGC_LINK].enabled = 1;

	/* Real-time getters use their immutable snapshot or the live fallback. */
	fake_iterator_available = 0;
	(void)usbradioplus_processing_unload();
	settings_defaults(&settings);
	assert(usbradioplus_processing_get_local_rt("missing", &chain) == 1);
	assert(usbradioplus_processing_get_hardware_rt("missing", &hardware) == 1);
	assert(usbradioplus_processing_get_composite_rt("missing", &chain) == 1);
	assert(!usbradioplus_processing_get_composite_rt("usb", &chain));
	assert(usbradioplus_processing_get_composite_rt("usb", NULL) < 0);

	/* Staging rejects invalid inputs, handles an empty startup transaction, and
	 * safely ignores iterator entries without a link datastore. */
	assert(stage_active_link_hooks(NULL, &transaction) < 0);
	assert(stage_active_link_hooks(&candidate, NULL) < 0);
	fake_iterator_available = 0;
	scan_thread = (pthread_t)1;
	assert(stage_active_link_hooks(&candidate, &transaction) < 0);
	scan_thread = AST_PTHREADT_NULL;
	fake_calloc_call = 0;
	fake_calloc_fail_call = 1;
	assert(stage_active_link_hooks(&candidate, &transaction) < 0);
	fake_calloc_fail_call = 0;
	fake_calloc_call = 0;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	assert(transaction);
	discard_link_graph_transaction(transaction);
	transaction = NULL;
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	fake_channel_datastore = NULL;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	discard_link_graph_transaction(transaction);
	transaction = NULL;
	/* Allocation and growth failures leave every retained hook untouched. */
	fake_iterator_channels_remaining = 0;
	fake_calloc_call = 0;
	fake_calloc_fail_call = 1;
	assert(stage_active_link_hooks(&candidate, &transaction) < 0);
	fake_calloc_fail_call = 0;
	fake_calloc_call = 0;
	txagc_avfilter_slot_init(&staged_hook.avfilter);
	atomic_init(&staged_hook.link_enabled, 1);
	staged_hook.sample_rate = 8000;
	ast_copy_string(staged_hook.profile, "usb", sizeof(staged_hook.profile));
	fake_channel_datastore = &staged_datastore;
	/* A datastore with no hook is skipped without treating the channel as live. */
	staged_datastore.data = NULL;
	fake_iterator_channels_remaining = 1;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	discard_link_graph_transaction(transaction);
	transaction = NULL;
	staged_datastore.data = &staged_hook;
	/* Nine synthetic channels exercise both transaction growth capacities. */
	fake_iterator_channels_remaining = 9;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	discard_link_graph_transaction(transaction);
	transaction = NULL;
	/* Disabled or unmatched profiles retain a plan but never prepare or publish a graph. */
	candidate.profiles[0].enabled = 0;
	fake_iterator_channels_remaining = 1;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	publish_link_graph_transaction(transaction);
	transaction = NULL;
	candidate.profiles[0].enabled = 1;
	candidate.profiles[0].chains[TXAGC_LINK].enabled = 0;
	fake_iterator_channels_remaining = 1;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	publish_link_graph_transaction(transaction);
	transaction = NULL;
	candidate.profiles[0].chains[TXAGC_LINK].enabled = 1;
	ast_copy_string(staged_hook.profile, "missing", sizeof(staged_hook.profile));
	fake_iterator_channels_remaining = 1;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	publish_link_graph_transaction(transaction);
	transaction = NULL;
	ast_copy_string(staged_hook.profile, "usb", sizeof(staged_hook.profile));
	fake_iterator_channels_remaining = 1;
	fake_realloc_failure = 1;
	assert(stage_active_link_hooks(&candidate, &transaction) < 0);
	fake_realloc_failure = 0;
	fake_slot_prepare_result = -1;
	fake_iterator_channels_remaining = 1;
	assert(stage_active_link_hooks(&candidate, &transaction) < 0);
	fake_slot_prepare_result = 0;
	fake_iterator_channels_remaining = 1;
	assert(!stage_active_link_hooks(&candidate, &transaction));
	fake_slot_publish_result = -1;
	publish_link_graph_transaction(transaction);
	transaction = NULL;
	fake_slot_publish_result = 0;
	txagc_avfilter_slot_destroy(&staged_hook.avfilter);
	fake_channel_datastore = NULL;
	fake_iterator_available = 0;
	publish_link_graph_transaction(NULL);
}

/** @brief An established link uses current stage flags on its next incoming audio frame. */
static void test_link_live_stage_flags(void)
{
	static const size_t flag_offsets[] = {
		offsetof(struct txagc_config, equalizer_enabled),
		offsetof(struct txagc_config, expander_enabled),
		offsetof(struct txagc_config, agc_enabled),
		offsetof(struct txagc_config, deesser_enabled),
		offsetof(struct txagc_config, compressor_enabled),
		offsetof(struct txagc_config, limiter_enabled),
	};
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct txagc_hook hook = {0};
	int16_t pcm[160] = {100};
	double samples[ARRAY_LEN(pcm)];
	struct ast_audiohook *audiohook = &hook.audiohook;
	struct ast_frame frame = {.frametype = AST_FRAME_VOICE,
				  .samples = ARRAY_LEN(pcm),
				  .data.ptr = pcm,
				  .subclass.format = (struct ast_format *)(uintptr_t)1};

	settings_defaults(&settings);
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;
	ast_copy_string(hook.profile, "usb", sizeof(hook.profile));
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	fake_sample_rate = 8000;
	fake_processor_calls = 0;
	prepare_test_link_hook(&hook, "usb", samples, ARRAY_LEN(samples), 8000);
	for (size_t index = 0; index < ARRAY_LEN(flag_offsets); ++index) {
		int *flag = (int *)((char *)&settings.profiles[0].chains[TXAGC_LINK].agc +
				    flag_offsets[index]);
		for (int enabled = 0; enabled <= 1; ++enabled) {
			*flag = enabled;
			/* Rebuilding belongs to the reload control plane, never this callback. */
			assert(!txagc_avfilter_slot_prepare(
				&hook.avfilter, &settings.profiles[0].chains[TXAGC_LINK].agc,
				8000));
			assert(!test_link_callback(audiohook, channel, &frame,
						   AST_AUDIOHOOK_DIRECTION_READ));
			assert(*(int *)((char *)&fake_processor_config + flag_offsets[index]) ==
			       enabled);
			assert(fake_processor_sample_rate == 8000);
		}
	}
	assert(fake_processor_calls == 2 * ARRAY_LEN(flag_offsets));
	/* Outgoing network audio and a disabled link chain must remain untouched. */
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_WRITE));
	atomic_store_explicit(&hook.link_enabled, 0, memory_order_release);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(fake_processor_calls == 2 * ARRAY_LEN(flag_offsets));
	destroy_test_hook_slots(&hook);
}

/** @brief Reload band modes atomically and deliver every selected and inactive setting live. */
static void test_link_live_band_modes(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct txagc_hook hook = {0};
	struct ast_datastore datastore = {.data = &hook};
	int16_t pcm[160] = {100};
	double samples[ARRAY_LEN(pcm)];
	struct ast_audiohook *audiohook = &hook.audiohook;
	struct ast_frame frame = {.frametype = AST_FRAME_VOICE,
				  .samples = ARRAY_LEN(pcm),
				  .data.ptr = pcm,
				  .subclass.format = (struct ast_format *)(uintptr_t)1};
	struct fake_option configured[] = {
		{"link usb", "enabled", "yes"},
		{"link usb", "compressor_enabled", "yes"},
		{"link usb", "limiter_enabled", "yes"},
		{"link usb", "compressor_bands", "3"},
		{"link usb", "limiter_bands", "3"},
		{"link usb", "compressor_low_threshold_dbfs", "-17"},
		{"link usb", "compressor_threshold_dbfs", "-9"},
		{"link usb", "limiter_low_threshold_dbfs", "-7"},
		{"link usb", "limiter_threshold_dbfs", "-3.5"},
		{"link usb", "compressor_low_crossover_hz", "650"},
		{"link usb", "compressor_high_crossover_hz", "2300"},
		{"link usb", "limiter_low_crossover_hz", "700"},
		{"link usb", "limiter_high_crossover_hz", "2500"},
		{"local usb", "input_gain_db", "1"},
	};
	static const int band_modes[] = {3, 1, 3};
	fake_config_load_result = (struct ast_config *)(uintptr_t)1;
	fake_category_count = 3;
	fake_categories[0] = "usb";
	fake_categories[1] = "link usb";
	fake_categories[2] = "local usb";
	fake_variables[0] = fake_variables[1] = fake_variables[2] = NULL;
	ast_copy_string(hook.profile, "usb", sizeof(hook.profile));
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	fake_channel_datastore = &datastore;
	fake_sample_rate = 8000;
	fake_processor_calls = 0;
	fake_native_publish_calls = 0;
	fake_native_discard_calls = 0;
	settings_defaults(&settings);
	prepare_test_link_hook(&hook, "usb", samples, ARRAY_LEN(samples), 8000);
	fake_iterator_available = 1;
	for (size_t index = 0; index < ARRAY_LEN(band_modes); ++index) {
		configured[3].value = configured[4].value = band_modes[index] == 1 ? "1" : "3";
		set_fake_options(configured, ARRAY_LEN(configured));
		fake_iterator_channels_remaining = 1;
		assert(!usbradioplus_processing_reload());
		assert(!test_link_callback(audiohook, channel, &frame,
					   AST_AUDIOHOOK_DIRECTION_READ));
		assert(fake_processor_config.compressor_bands == band_modes[index]);
		assert(fake_processor_config.limiter_bands == band_modes[index]);
		assert(fake_processor_config.compressor_low_threshold_dbfs == -17.0);
		assert(fake_processor_config.compressor_threshold_dbfs == -9.0);
		assert(fake_processor_config.low_limiter_threshold_dbfs == -7.0);
		assert(fake_processor_config.limiter_threshold_dbfs == -3.5);
		assert(!memcmp(&fake_processor_config, &settings.profiles[0].chains[TXAGC_LINK].agc,
			       sizeof(fake_processor_config)));
	}
	struct txagc_config previous = fake_processor_config;
	unsigned int prior_native_publishes = fake_native_publish_calls;
	double previous_local_input_gain =
		settings.profiles[0].chains[TXAGC_LOCAL].agc.input_gain_db;
	configured[3].value = "2";
	set_fake_options(configured, ARRAY_LEN(configured));
	assert(usbradioplus_processing_reload() < 0);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(!memcmp(&previous, &fake_processor_config, sizeof(previous)));
	assert(fake_processor_calls == ARRAY_LEN(band_modes) + 1);
	/* A failed native generation is detected before link staging and restores
	 * the pre-reload settings snapshot. */
	configured[3].value = configured[4].value = "3";
	configured[6].value = "-8";
	configured[13].value = "8";
	set_fake_options(configured, ARRAY_LEN(configured));
	fake_native_prepare_result = -1;
	fake_native_stage_observe_audio_settings = 1;
	fake_iterator_channels_remaining = 1;
	assert(usbradioplus_processing_reload() < 0);
	assert(fake_native_stage_observed_local_gain == previous_local_input_gain);
	assert(!memcmp(&previous, &settings.profiles[0].chains[TXAGC_LINK].agc, sizeof(previous)));
	assert(fake_native_publish_calls == prior_native_publishes);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(!memcmp(&previous, &fake_processor_config, sizeof(previous)));
	fake_native_prepare_result = 0;
	/* A valid candidate whose staged link graph cannot be prepared must roll
	 * settings back as well as retaining every active link graph. */
	configured[6].value = "-6";
	configured[13].value = "9";
	set_fake_options(configured, ARRAY_LEN(configured));
	fake_slot_prepare_result = -1;
	fake_iterator_channels_remaining = 1;
	assert(usbradioplus_processing_reload() < 0);
	assert(fake_native_stage_observed_local_gain == previous_local_input_gain);
	assert(!memcmp(&previous, &settings.profiles[0].chains[TXAGC_LINK].agc, sizeof(previous)));
	assert(fake_native_publish_calls == prior_native_publishes);
	assert(fake_native_discard_calls > 0);
	assert(!test_link_callback(audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(!memcmp(&previous, &fake_processor_config, sizeof(previous)));
	fake_slot_prepare_result = 0;
	fake_native_stage_observe_audio_settings = 0;
	configured[6].value = "-7";
	fake_channel_datastore = NULL;
	destroy_test_hook_slots(&hook);
	fake_category_count = 0;
	fake_option_count = 0;
}

/** @brief A successful processing reload applies the candidate signaling snapshot live. */
static void test_live_signaling_reload(void)
{
	struct fake_option configured[] = {{"receive usb", "signaling_method", "ctcss"}};
	char active_method[16];

	settings_defaults(&settings);
	fake_config_load_result = (struct ast_config *)(uintptr_t)1;
	fake_category_count = 2;
	fake_categories[0] = "usb";
	fake_categories[1] = "receive usb";
	fake_variables[0] = fake_variables[1] = NULL;
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 0;
	fake_native_prepare_result = 0;
	fake_signaling_refresh_calls = 0;
	fake_signaling_refresh_result = 0;
	fake_signaling_refresh_method[0] = '\0';
	set_fake_options(configured, ARRAY_LEN(configured));
	assert(!usbradioplus_processing_reload());
	assert(fake_signaling_refresh_calls == 1);
	assert(!strcmp(fake_signaling_refresh_method, "ctcss"));
	assert(!usbradioplus_processing_get_option("usb", "receive", "signaling_method",
						   active_method, sizeof(active_method)));
	assert(!strcmp(active_method, "ctcss"));

	/* A failed graph stage happens before signaling publication. */
	configured[0].value = "dcs";
	set_fake_options(configured, ARRAY_LEN(configured));
	fake_native_prepare_result = -1;
	assert(usbradioplus_processing_reload() < 0);
	assert(fake_signaling_refresh_calls == 1);
	assert(!usbradioplus_processing_get_option("usb", "receive", "signaling_method",
						   active_method, sizeof(active_method)));
	assert(!strcmp(active_method, "ctcss"));
	fake_native_prepare_result = 0;

	/* A rejected live signaling application rolls the settings snapshot back. */
	fake_signaling_refresh_result = -1;
	assert(usbradioplus_processing_reload() < 0);
	assert(fake_signaling_refresh_calls == 2);
	assert(!usbradioplus_processing_get_option("usb", "receive", "signaling_method",
						   active_method, sizeof(active_method)));
	assert(!strcmp(active_method, "ctcss"));
	fake_signaling_refresh_result = 0;
	fake_category_count = 0;
	fake_option_count = 0;
	fake_iterator_available = 0;
}

/** @brief Reset audiohook attachment and allocation stub state. */
static void reset_attach_doubles(void)
{
	fake_channel_datastore = NULL;
	fake_find_sequence_count = 0;
	fake_find_sequence_index = 0;
	fake_datastore_alloc_failure = 0;
	fake_calloc_failure = 0;
	fake_calloc_call = 0;
	fake_calloc_fail_call = 0;
	fake_slot_prepare_result = 0;
	fake_slot_active_null = 0;
	fake_slot_zero_input_capacity = 0;
	fake_rawreadformat_null = 0;
	fake_datastore_free_calls = 0;
	fake_audiohook_init_result = 0;
	fake_audiohook_attach_result = 0;
	fake_datastore_add_calls = 0;
	fake_datastore_remove_calls = 0;
	fake_last_allocated_datastore = NULL;
	fake_channel_name = "IAX2/test";
}

/** @brief Verify audiohook attachment. */
static void test_audiohook_attachment(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_datastore existing = {0};

	settings_defaults(&settings);
	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;

	reset_attach_doubles();
	fake_channel_datastore = &existing;
	assert(!attach_hook(channel, "usb"));
	assert(!fake_datastore_add_calls);

	reset_attach_doubles();
	fake_datastore_alloc_failure = 1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_calloc_failure = 1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	/* A channel with no advertised format uses the documented 8 kHz fallback. */
	fake_sample_rate = 0;
	assert(!attach_hook(channel, "usb"));
	assert(fake_last_allocated_datastore);
	hook_destroy(fake_last_allocated_datastore->data);
	ast_datastore_free(fake_last_allocated_datastore);
	fake_sample_rate = 8000;

	reset_attach_doubles();
	/* A NULL source format follows the same documented 8 kHz fallback. */
	fake_rawreadformat_null = 1;
	assert(!attach_hook(channel, "usb"));
	assert(fake_last_allocated_datastore);
	hook_destroy(fake_last_allocated_datastore->data);
	ast_datastore_free(fake_last_allocated_datastore);

	reset_attach_doubles();
	assert(attach_hook(channel, "missing") < 0);
	settings.profiles[0].enabled = 0;
	assert(attach_hook(channel, "usb") < 0);
	settings.profiles[0].enabled = 1;
	settings.profiles[0].chains[TXAGC_LINK].enabled = 0;
	assert(attach_hook(channel, "usb") < 0);
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;

	reset_attach_doubles();
	fake_slot_prepare_result = -1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_slot_active_null = 1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_slot_zero_input_capacity = 1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_calloc_fail_call = 2;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_audiohook_init_result = -1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_find_sequence[0] = NULL;
	fake_find_sequence[1] = &existing;
	fake_find_sequence_count = 2;
	assert(!attach_hook(channel, "usb"));
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_audiohook_attach_result = -1;
	assert(attach_hook(channel, "usb") < 0);
	assert(fake_datastore_add_calls == 1);
	assert(fake_datastore_remove_calls == 1);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	assert(!attach_hook(channel, "usb"));
	assert(fake_datastore_add_calls == 1);
	assert(fake_last_allocated_datastore);
	hook_destroy(fake_last_allocated_datastore->data);
	ast_datastore_free(fake_last_allocated_datastore);
}

/** @brief Reset fake channel-iteration state. */
static void reset_iterator_doubles(void)
{
	fake_primary_channel_available = 0;
	fake_iterator_available = 0;
	fake_iterator_channels_remaining = 0;
	fake_iterator_destroy_calls = 0;
	fake_channel_datastore = NULL;
	fake_find_sequence_count = 0;
	fake_find_sequence_index = 0;
}

/** @brief Verify channel scanning and detachment. */
static void test_channel_scanning_and_detachment(void)
{
	reset_iterator_doubles();
	fake_calloc_failure = 1;
	scan_channels();
	fake_calloc_failure = 0;
	settings_defaults(&settings);
	settings.profiles[0].enabled = 0;
	scan_channels();

	settings_defaults(&settings);
	reset_iterator_doubles();
	scan_channels();
	assert(!fake_iterator_destroy_calls);

	settings.profiles[0].enabled = 1;
	scan_channels();
	assert(!fake_iterator_destroy_calls);
	fake_primary_channel_available = 1;
	scan_channels();
	assert(!fake_iterator_destroy_calls);

	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	fake_channel_name = "PJSIP/test";
	scan_channels();
	assert(fake_iterator_destroy_calls == 1);

	reset_attach_doubles();
	fake_primary_channel_available = 1;
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	settings.profiles[0].chains[TXAGC_LINK].enabled = 0;
	scan_channels();
	assert(!fake_datastore_add_calls);
	assert(!strcmp(fake_primary_channel_name, "RadioPlus/usb"));
	/* Enabling the chain discovers the same already-connected incoming link. */
	settings.profiles[0].chains[TXAGC_LINK].enabled = 1;
	fake_iterator_channels_remaining = 1;
	scan_channels();
	assert(fake_datastore_add_calls == 1);
	hook_destroy(fake_last_allocated_datastore->data);
	ast_datastore_free(fake_last_allocated_datastore);

	reset_iterator_doubles();
	detach_all();
	assert(!fake_iterator_destroy_calls);
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	detach_all();
	assert(fake_iterator_destroy_calls == 1);

	reset_iterator_doubles();
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	fake_channel_datastore = calloc(1, sizeof(*fake_channel_datastore));
	assert(fake_channel_datastore);
	fake_datastore_remove_calls = 0;
	detach_all();
	assert(fake_datastore_remove_calls == 1);
	fake_channel_datastore = NULL;
}

/** @brief Verify reporting cli. */
static void test_reporting_cli(void)
{
	struct ast_cli_entry entry = {0};
	struct ast_cli_args arguments = {.fd = 1, .argc = 3};
	struct ast_cli_args bad_arguments = {.fd = 1, .argc = 2};
	struct ast_datastore datastore = {0};
	struct txagc_hook hook = {0};
	struct txagc_avfilter *filter;

	assert(cli_show(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_show(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_show(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	memset(&settings, 0, sizeof(settings));
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	settings_defaults(&settings);
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 1;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	settings.profiles[0].enabled = 0;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	settings.profiles[0].enabled = 1;
	settings.profiles[0].local_enabled = 1;
	settings.profiles[0].link_enabled = 1;
	settings.profiles[0].rnnoise_enabled = 1;
	settings.profiles[0].agc.receive_bandpass_enabled = 1;
	settings.profiles[0].agc.equalizer_enabled = 1;
	settings.profiles[0].agc.agc_enabled = 1;
	settings.profiles[0].agc.expander_enabled = 1;
	settings.profiles[0].agc.deesser_enabled = 1;
	settings.profiles[0].agc.compressor_enabled = 1;
	settings.profiles[0].agc.limiter_enabled = 1;
	settings.profiles[0].agc.lookahead_limiter_enabled = 1;
	for (size_t source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		settings.profiles[0].chains[source].enabled = 1;
		settings.profiles[0].chains[source].rnnoise_enabled = 1;
		settings.profiles[0].chains[source].agc.equalizer_enabled = 1;
		settings.profiles[0].chains[source].agc.agc_enabled = 1;
		settings.profiles[0].chains[source].agc.expander_enabled = 1;
		settings.profiles[0].chains[source].agc.deesser_enabled = 1;
		settings.profiles[0].chains[source].agc.compressor_enabled = 1;
		settings.profiles[0].chains[source].agc.limiter_enabled = 1;
		settings.profiles[0].chains[source].agc.lookahead_limiter_enabled = 1;
	}
	settings.profiles[0].chains[TXAGC_LOCAL].agc.receive_bandpass_enabled = 1;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	for (size_t source = 0; source < TXAGC_SOURCE_COUNT; ++source)
		settings.profiles[0].chains[source].enabled =
			settings.profiles[0].chains[source].rnnoise_enabled = 0;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.receive_bandpass_enabled = 0;
	settings.profiles[0].chains[TXAGC_LOCAL].agc.equalizer_enabled = 0;
	settings.profiles[0].chains[TXAGC_LOCAL].ctcss_filter_configured = 0;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);

	assert(cli_stats(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_stats(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_stats(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	reset_iterator_doubles();
	assert(cli_stats(&entry, 99, &arguments) == CLI_FAILURE);
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);

	reset_iterator_doubles();
	fake_iterator_available = 1;
	fake_iterator_channels_remaining = 1;
	datastore.data = &hook;
	fake_channel_datastore = &datastore;
	txagc_avfilter_slot_init(&hook.avfilter);
	assert(!txagc_avfilter_slot_prepare(&hook.avfilter,
					    &settings.profiles[0].chains[TXAGC_LINK].agc, 8000));
	filter = txagc_avfilter_slot_active(&hook.avfilter);
	assert(filter);
	filter->input_samples = 1;
	filter->input_peak_dbfs = -10.0;
	filter->input_rms_dbfs = -20.0;
	filter->output_samples = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);
	/* An unstable statistics handoff is omitted rather than partially printed. */
	processing_test_statistics_flip_index = 3;
	fake_iterator_channels_remaining = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);
	processing_test_statistics_flip_index = 0;
	datastore.data = NULL;
	fake_iterator_channels_remaining = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);
	fake_channel_datastore = NULL;
	txagc_avfilter_slot_destroy(&hook.avfilter);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	/* Keep this large fixture out of the thread stack: boundary helpers also
	 * construct complete settings snapshots while they exercise validation. */
	static struct txagc_settings value;
	settings_defaults(&value);
	assert(!validate_profile(&value.profiles[0]));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_NOTCH), "notch"));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_HIGHPASS), "highpass"));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_DISABLED), "disabled"));
	test_every_numeric_boundary();
	test_stage_and_relationship_validation();
	test_agc_configuration();
	test_dynamics_band_configuration();
	test_settings_scope_and_hardware_validation();
	test_primitive_configuration_parsers();
	test_hardware_configuration_parser();
	test_chain_configuration_parser();
	test_section_override_parser();
	test_option_name_validation();
	test_signaling_methods();
	test_unified_configuration_edge_paths();
	test_override_validation_short_circuits();
	test_profile_reference_failure_short_circuits();
	test_realtime_snapshot_accessors();
	test_settings_loader();
	test_settings_loader_rejections();
	test_public_setting_accessors();
	test_input_gain_persistence();
	test_generic_option_persistence();
	test_module_lifecycle_and_simple_cli();
	test_channel_eligibility();
	test_audiohook_callback_and_destroy();
	test_link_callback_synchronous_contract();
	test_link_callback_statistics_and_guards();
	test_processing_private_edge_paths();
	test_link_live_stage_flags();
	test_link_live_band_modes();
	test_live_signaling_reload();
	test_audiohook_attachment();
	test_channel_scanning_and_detachment();
	test_reporting_cli();
	puts("processing validation tests passed");
	return 0;
}

/** @def RANGE
 * @brief RANGE selection for this isolated test harness.
 */
/** @def INVALID_RELATION
 * @brief INVALID RELATION selection for this isolated test harness.
 */
/** @def INVALID_HARDWARE
 * @brief INVALID HARDWARE selection for this isolated test harness.
 */
