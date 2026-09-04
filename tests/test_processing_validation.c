struct ast_config;
struct ast_category;
#include "../src/usbradioplus_processing_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

struct fake_option {
	const char *section;
	const char *name;
	const char *value;
};

static struct fake_option fake_options[128];
static size_t fake_option_count;
static char *fake_categories[16];
static size_t fake_category_count;
static struct ast_variable *fake_variables[16];
static struct ast_config *fake_config_load_result;
static int fake_config_destroy_count;
static struct ast_category *fake_hardware_category;
static struct ast_category *fake_local_category;
static int fake_category_new_failure;
static int fake_category_append_count;
static int fake_save_result;
static int fake_tune_update_failure_call;
static int fake_tune_update_calls;
static int fake_cli_register_result;
static int fake_cli_unregister_calls;
static int fake_thread_create_result;
static int fake_cli_print_calls;
static const char *fake_channel_name = "IAX2/test";
static const char *fake_channel_application = "Rpt";
static const char *fake_channel_data = "Remote Rx";
static struct ast_datastore *fake_channel_datastore;
static unsigned int fake_sample_rate = 8000;
static int fake_processor_result;
static int fake_processor_saturate;
static int fake_audiohook_detach_calls;
static int fake_audiohook_destroy_calls;
static int fake_processor_destroy_calls;
static int fake_datastore_alloc_failure;
static int fake_calloc_failure;
static int fake_datastore_free_calls;
static int fake_audiohook_init_result;
static int fake_audiohook_attach_result;
static int fake_datastore_add_calls;
static int fake_datastore_remove_calls;
static struct ast_datastore *fake_find_sequence[4];
static size_t fake_find_sequence_count;
static size_t fake_find_sequence_index;
static struct ast_datastore *fake_last_allocated_datastore;
static int fake_primary_channel_available;
static int fake_iterator_available;
static int fake_iterator_channels_remaining;
static int fake_iterator_destroy_calls;
static int fake_pthread_join_calls;

struct ast_module *test_module_self(void)
{
	return NULL;
}

#undef calloc
#undef free

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

struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category)
{
	(void)config;
	for (size_t index = 0; index < fake_category_count; ++index)
		if (!strcmp(category, fake_categories[index]))
			return fake_variables[index];
	return NULL;
}

struct ast_config *ast_config_load2(const char *filename, const char *who_asked,
				    struct ast_flags flags)
{
	(void)filename;
	(void)who_asked;
	(void)flags;
	return fake_config_load_result;
}

void ast_config_destroy(struct ast_config *config)
{
	(void)config;
	++fake_config_destroy_count;
}

struct ast_category *ast_category_get(const struct ast_config *config, const char *name,
				      const char *filter)
{
	(void)config;
	(void)filter;
	return !strcmp(name, "hardware") ? fake_hardware_category : fake_local_category;
}

struct ast_category *ast_category_new(const char *name, const char *filename, int line)
{
	(void)name;
	(void)filename;
	(void)line;
	if (fake_category_new_failure)
		return NULL;
	return (struct ast_category *)(uintptr_t)3;
}

void ast_category_append(struct ast_config *config, struct ast_category *category)
{
	(void)config;
	(void)category;
	++fake_category_append_count;
}

int ast_config_text_file_save2(const char *filename, const struct ast_config *config,
			       const char *generator, uint32_t flags)
{
	(void)filename;
	(void)config;
	(void)generator;
	(void)flags;
	return fake_save_result;
}

int __ast_pthread_mutex_lock(const char *filename, int line, const char *function,
			     const char *mutex_name, ast_mutex_t *mutex)
{
	(void)filename;
	(void)line;
	(void)function;
	(void)mutex_name;
	(void)mutex;
	return 0;
}

int __ast_pthread_mutex_unlock(const char *filename, int line, const char *function,
			       const char *mutex_name, ast_mutex_t *mutex)
{
	(void)filename;
	(void)line;
	(void)function;
	(void)mutex_name;
	(void)mutex;
	return 0;
}

int __ast_cli_register_multiple(struct ast_cli_entry *entries, int count, struct ast_module *module)
{
	(void)entries;
	(void)count;
	(void)module;
	return fake_cli_register_result;
}

int ast_cli_unregister_multiple(struct ast_cli_entry *entries, int count)
{
	(void)entries;
	(void)count;
	++fake_cli_unregister_calls;
	return 0;
}

int ast_background_stacksize(void)
{
	return 0;
}

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

struct ast_channel_iterator *ast_channel_iterator_all_new(void)
{
	return fake_iterator_available ? (struct ast_channel_iterator *)(uintptr_t)1 : NULL;
}

struct ast_channel *ast_channel_iterator_next(struct ast_channel_iterator *iterator)
{
	(void)iterator;
	if (fake_iterator_channels_remaining-- > 0)
		return (struct ast_channel *)(uintptr_t)1;
	return NULL;
}

struct ast_channel_iterator *ast_channel_iterator_destroy(struct ast_channel_iterator *iterator)
{
	++fake_iterator_destroy_calls;
	return iterator;
}

struct ast_channel *ast_channel_get_by_name(const char *name)
{
	(void)name;
	return fake_primary_channel_available ? (struct ast_channel *)(uintptr_t)1 : NULL;
}

const char *ast_channel_name(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_name;
}

const char *ast_channel_appl(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_application;
}

const char *ast_channel_data(const struct ast_channel *channel)
{
	(void)channel;
	return fake_channel_data;
}

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

int __ao2_unlock(void *object, const char *file, const char *function, int line, const char *name)
{
	(void)object;
	(void)file;
	(void)function;
	(void)line;
	(void)name;
	return 0;
}

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

int ast_channel_datastore_add(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	++fake_datastore_add_calls;
	return 0;
}

int ast_channel_datastore_remove(struct ast_channel *channel, struct ast_datastore *datastore)
{
	(void)channel;
	(void)datastore;
	++fake_datastore_remove_calls;
	return 0;
}

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

int ast_datastore_free(struct ast_datastore *datastore)
{
	++fake_datastore_free_calls;
	free(datastore);
	return 0;
}

void *__ast_calloc(size_t count, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	if (fake_calloc_failure)
		return NULL;
	return calloc(count, size);
}

void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

int ast_audiohook_init(struct ast_audiohook *audiohook, enum ast_audiohook_type type,
		       const char *source, enum ast_audiohook_init_flags flags)
{
	(void)audiohook;
	(void)type;
	(void)source;
	(void)flags;
	return fake_audiohook_init_result;
}

int ast_audiohook_destroy(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	++fake_audiohook_destroy_calls;
	return 0;
}

int ast_audiohook_attach(struct ast_channel *channel, struct ast_audiohook *audiohook)
{
	(void)channel;
	(void)audiohook;
	return fake_audiohook_attach_result;
}

int ast_audiohook_detach(struct ast_audiohook *audiohook)
{
	(void)audiohook;
	++fake_audiohook_detach_calls;
	return 0;
}

unsigned int ast_format_get_sample_rate(const struct ast_format *format)
{
	(void)format;
	return fake_sample_rate;
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

int txagc_avfilter_process(struct txagc_avfilter *processor, const struct txagc_config *config,
			   double *samples, size_t sample_count, unsigned int sample_rate)
{
	(void)processor;
	(void)config;
	(void)samples;
	(void)sample_count;
	(void)sample_rate;
	if (fake_processor_saturate && sample_count >= 3) {
		samples[0] = 40000.0;
		samples[1] = -40000.0;
		samples[2] = 1.6;
	}
	return fake_processor_result;
}

void ast_cli(int fd, const char *format, ...)
{
	(void)fd;
	(void)format;
	++fake_cli_print_calls;
}

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

int ast_true(const char *value)
{
	return !strcasecmp(value, "yes") || !strcmp(value, "1");
}

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

struct range_case {
	size_t offset;
	double minimum;
	double maximum;
};

#define RANGE(field, low, high) offsetof(struct txagc_config, field), (low), (high)

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
	{RANGE(agc_floor_dbfs, -100.0, -3.1)},
	{RANGE(attack_ms, 1.0, 10000.0)},
	{RANGE(release_ms, 1.0, 30000.0)},
	{RANGE(reset_after_ms, 100.0, 60000.0)},
	{RANGE(sidechain_highpass_hz, 50.0, 2000.0)},
	{RANGE(sidechain_lowpass_hz, 50.0, 3500.0)},
	{RANGE(expander_threshold_dbfs, -100.0, -10.0)},
	{RANGE(expander_ratio, 1.0, 10.0)},
	{RANGE(expander_max_attenuation_db, 0.0, 40.0)},
	{RANGE(expander_attack_ms, 1.0, 1000.0)},
	{RANGE(expander_release_ms, 1.0, 10000.0)},
	{RANGE(expander_sidechain_highpass_hz, 50.0, 2000.0)},
	{RANGE(expander_sidechain_lowpass_hz, 50.0, 3500.0)},
	{RANGE(compressor_threshold_dbfs, -60.0, 0.0)},
	{RANGE(compressor_ratio, 1.0, 20.0)},
	{RANGE(compressor_makeup_gain_db, -30.0, 30.0)},
	{RANGE(compressor_attack_ms, 1.0, 1000.0)},
	{RANGE(compressor_release_ms, 1.0, 10000.0)},
	{RANGE(compressor_sidechain_highpass_hz, 50.0, 2000.0)},
	{RANGE(compressor_sidechain_lowpass_hz, 50.0, 3500.0)},
	{RANGE(limiter_low_crossover_hz, 100.0, 2000.0)},
	{RANGE(limiter_high_crossover_hz, 100.0, 5000.0)},
	{RANGE(low_limiter_threshold_dbfs, -40.0, -1.0)},
	{RANGE(low_limiter_ratio, 1.0, 20.0)},
	{RANGE(low_limiter_knee_db, 0.0, 24.0)},
	{RANGE(low_limiter_attack_ms, 0.1, 1000.0)},
	{RANGE(low_limiter_release_ms, 1.0, 10000.0)},
	{RANGE(mid_limiter_threshold_dbfs, -40.0, -1.0)},
	{RANGE(mid_limiter_ratio, 1.0, 20.0)},
	{RANGE(mid_limiter_knee_db, 0.0, 24.0)},
	{RANGE(mid_limiter_attack_ms, 0.1, 1000.0)},
	{RANGE(mid_limiter_release_ms, 1.0, 10000.0)},
	{RANGE(high_limiter_threshold_dbfs, -30.0, -1.0)},
	{RANGE(high_limiter_ratio, 1.0, 20.0)},
	{RANGE(high_limiter_knee_db, 0.0, 24.0)},
	{RANGE(high_limiter_attack_ms, 0.1, 100.0)},
	{RANGE(high_limiter_release_ms, 1.0, 1000.0)},
	{RANGE(lookahead_limit_dbfs, -30.0, -0.1)},
	{RANGE(lookahead_ms, 0.1, 20.0)},
	{RANGE(lookahead_attack_ms, 0.1, 20.0)},
	{RANGE(lookahead_release_ms, 1.0, 5000.0)},
	{RANGE(post_limiter_lowpass_hz, 5000.0, 20000.0)},
	{RANGE(output_highpass_hz, 20.0, 2000.0)},
	{RANGE(output_lowpass_hz, 20.0, 6000.0)},
	{RANGE(output_gain_db, -30.0, 30.0)},
};

static void expect_invalid_field(size_t offset, double value)
{
	struct txagc_settings settings_value;
	struct txagc_chain *chain;
	settings_defaults(&settings_value);
	chain = &settings_value.chains[TXAGC_LOCAL];
	*(double *)((char *)&chain->agc + offset) = value;
	assert(validate_chain(chain) < 0);
}

static void test_every_numeric_boundary(void)
{
	for (size_t index = 0; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
		expect_invalid_field(ranges[index].offset, NAN);
		expect_invalid_field(ranges[index].offset, ranges[index].minimum - 1.0);
		expect_invalid_field(ranges[index].offset, ranges[index].maximum + 1.0);
	}
	struct txagc_settings value;
	settings_defaults(&value);
	value.chains[TXAGC_LOCAL].agc.low_limiter_threshold_dbfs = NAN;
	assert(validate_chain(&value.chains[TXAGC_LOCAL]) < 0);
	settings_defaults(&value);
	value.chains[TXAGC_LOCAL].agc.low_limiter_attack_ms = NAN;
	assert(validate_chain(&value.chains[TXAGC_LOCAL]) < 0);
}

static void test_stage_and_relationship_validation(void)
{
	struct txagc_settings settings_value;
	struct txagc_chain *chain;
	settings_defaults(&settings_value);
	chain = &settings_value.chains[TXAGC_LOCAL];
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
		chain = &settings_value.chains[TXAGC_LOCAL];                                       \
		chain->agc.field = chain->agc.other;                                               \
		assert(validate_chain(chain) < 0);                                                 \
	} while (0)
	INVALID_RELATION(receive_bandpass_lowpass_hz, receive_bandpass_highpass_hz);
	INVALID_RELATION(agc_floor_dbfs, target_dbfs);
	INVALID_RELATION(sidechain_lowpass_hz, sidechain_highpass_hz);
	INVALID_RELATION(expander_sidechain_lowpass_hz, expander_sidechain_highpass_hz);
	INVALID_RELATION(compressor_sidechain_lowpass_hz, compressor_sidechain_highpass_hz);
	INVALID_RELATION(limiter_high_crossover_hz, limiter_low_crossover_hz);
	INVALID_RELATION(output_lowpass_hz, output_highpass_hz);
#undef INVALID_RELATION
}

static void test_settings_scope_and_hardware_validation(void)
{
	struct txagc_settings value;
	struct usbradioplus_hardware_settings *hardware;
	struct txagc_chain *link;

	settings_defaults(&value);
	value.channel[0] = '\0';
	assert(validate_settings(&value) < 0);

#define INVALID_HARDWARE(field, configured, number)                                                \
	do {                                                                                       \
		settings_defaults(&value);                                                         \
		hardware = &value.hardware;                                                        \
		hardware->configured = 1;                                                          \
		hardware->field = number;                                                          \
		assert(validate_settings(&value) < 0);                                             \
	} while (0)
	INVALID_HARDWARE(input_gain_db, input_gain_configured, -31.0);
	INVALID_HARDWARE(input_gain_db, input_gain_configured, 31.0);
	INVALID_HARDWARE(output_a_gain_db, output_a_gain_configured, -31.0);
	INVALID_HARDWARE(output_a_gain_db, output_a_gain_configured, 31.0);
	INVALID_HARDWARE(output_b_gain_db, output_b_gain_configured, -31.0);
	INVALID_HARDWARE(output_b_gain_db, output_b_gain_configured, 31.0);
#undef INVALID_HARDWARE
	settings_defaults(&value);
	value.hardware.input_gain_configured = 1;
	value.hardware.output_a_gain_configured = 1;
	value.hardware.output_b_gain_configured = 1;
	assert(!validate_settings(&value));

	settings_defaults(&value);
	value.chains[TXAGC_LINK].agc.target_dbfs = 0.0;
	assert(validate_settings(&value) < 0);
	settings_defaults(&value);
	link = &value.chains[TXAGC_LINK];
	link->rnnoise_enabled = 1;
	assert(validate_settings(&value) < 0);
	settings_defaults(&value);
	link = &value.chains[TXAGC_LINK];
	link->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	assert(validate_settings(&value) < 0);
	settings_defaults(&value);
	link = &value.chains[TXAGC_LINK];
	link->agc.receive_bandpass_enabled = 1;
	assert(validate_settings(&value) < 0);

	settings_defaults(&value);
	value.chains[TXAGC_LOCAL].agc.splatter_filter_enabled = 1;
	assert(validate_settings(&value) < 0);
	settings_defaults(&value);
	value.chains[TXAGC_LOCAL].agc.lookahead_limiter_enabled = 1;
	assert(validate_settings(&value) < 0);
	settings_defaults(&value);
	value.chains[TXAGC_LOCAL].agc.post_limiter_lowpass_enabled = 1;
	assert(validate_settings(&value) < 0);
}

static void set_fake_options(const struct fake_option *options, size_t count)
{
	memcpy(fake_options, options, count * sizeof(*options));
	fake_option_count = count;
}

static void test_primitive_configuration_parsers(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	double number = 7.0;
	int boolean = 7;
	int assignment = -1;
	int configured = 0;
	const struct fake_option values[] = {
		{"test", "number", "12.5"},  {"test", "bad_number", "12x"},
		{"test", "infinite", "inf"}, {"test", "modern", "3.5"},
		{"test", "legacy", "4.5"},   {"test", "yes", "yes"},
		{"test", "no", "no"},	     {"test", "bad_bool", "maybe"},
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
	assert(!strcmp(read_option_alias(config, "test", "modern", NULL), "3.5"));
	assert(!strcmp(read_option_alias(config, "test", "missing", "legacy"), "4.5"));
	assert(!strcmp(read_option_alias(config, "test", "modern", "legacy"), "3.5"));
	assert(settings_parse_error);
	settings_parse_error = 0;
	read_double_alias(config, "test", "missing", NULL, &number);
	read_double_alias(config, "test", "modern", NULL, &number);
	assert(number == 3.5);
	read_double_alias(config, "test", "bad_number", NULL, &number);
	assert(settings_parse_error);
	settings_parse_error = 0;
	read_double_alias(config, "test", "infinite", NULL, &number);
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
	assert(known_chain_option("target_dbfs"));
	assert(!option_in_list("missing", asterisk_override_options,
			       ARRAY_LEN(asterisk_override_options)));
	assert(valid_frequency_list("67.0, 100.0, 254.1"));
	assert(!valid_frequency_list(""));
	assert(!valid_frequency_list("  "));
	assert(!valid_frequency_list("tone"));
	assert(valid_frequency_list("49"));
	assert(valid_frequency_list("301"));
	assert(!valid_frequency_list("nan"));
	assert(!valid_frequency_list("-1"));
	assert(!valid_frequency_list("100 200"));
	assert(!valid_frequency_list("100,"));
	assert(valid_frequency_list("100\t,\t200"));

	fake_option_count = 0;
	assert(!read_assignment(config, "hardware_output_a_assignment", &assignment, &configured));
	assert(!configured);
	const char *names[] = {"off", "voice", "ctcss", "voice_ctcss", "auxvoice", "invalid"};
	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		const struct fake_option option = {"hardware", "hardware_output_a_assignment",
						   names[index]};
		set_fake_options(&option, 1);
		configured = 0;
		int result = read_assignment(config, "hardware_output_a_assignment", &assignment,
					     &configured);
		assert((index < 5 && !result && configured) || (index == 5 && result < 0));
	}
	const char *aliases[] = {"no", "tone", "composite"};
	for (size_t index = 0; index < ARRAY_LEN(aliases); ++index) {
		const struct fake_option option = {"hardware", "hardware_output_a_assignment",
						   aliases[index]};
		set_fake_options(&option, 1);
		assert(!read_assignment(config, "hardware_output_a_assignment", &assignment,
					&configured));
	}
}

static void test_hardware_configuration_parser(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct usbradioplus_hardware_settings hardware = {0};
	const struct fake_option values[] = {
		{"hardware", "hardware_input_gain_db", "-12.5"},
		{"hardware", "hardware_output_a_gain_db", "3.0"},
		{"hardware", "hardware_output_b_gain_db", "4.0"},
		{"hardware", "hardware_cos_assignment", "usb"},
		{"hardware", "hardware_rx_ctcss_frequencies", "67.0,100.0"},
		{"hardware", "hardware_tx_ctcss_frequencies", "88.5"},
		{"hardware", "hardware_output_a_assignment", "voice"},
		{"hardware", "hardware_output_b_assignment", "ctcss"},
	};

	set_fake_options(values, ARRAY_LEN(values));
	settings_parse_error = 0;
	assert(!read_hardware(config, &hardware));
	assert(!settings_parse_error);
	assert(hardware.input_gain_configured && hardware.input_gain_db == -12.5);
	assert(hardware.output_a_gain_configured && hardware.output_a_gain_db == 3.0);
	assert(hardware.output_b_gain_configured && hardware.output_b_gain_db == 4.0);
	assert(hardware.cos_assignment_configured && !strcmp(hardware.cos_assignment, "usb"));
	assert(hardware.rx_ctcss_frequencies_configured);
	assert(!strcmp(hardware.rx_ctcss_frequencies, "67.0,100.0"));
	assert(hardware.tx_ctcss_frequencies_configured);
	assert(hardware.output_a_assignment == USBRADIOPLUS_HW_VOICE);
	assert(hardware.output_b_assignment == USBRADIOPLUS_HW_CTCSS);

	const char *valid_cos[] = {"no", "usbinvert", "dsp", "vox", "pp", "ppinvert"};
	for (size_t index = 0; index < ARRAY_LEN(valid_cos); ++index) {
		const struct fake_option option = {"hardware", "hardware_cos_assignment",
						   valid_cos[index]};
		memset(&hardware, 0, sizeof(hardware));
		set_fake_options(&option, 1);
		assert(!read_hardware(config, &hardware));
	}

	const struct fake_option invalid_cases[] = {
		{"hardware", "hardware_cos_assignment", "invalid"},
		{"hardware", "hardware_rx_ctcss_frequencies", "bad"},
		{"hardware", "hardware_tx_ctcss_frequencies", "bad"},
		{"hardware", "hardware_output_a_assignment", "bad"},
		{"hardware", "hardware_output_b_assignment", "bad"},
	};
	for (size_t index = 0; index < ARRAY_LEN(invalid_cases); ++index) {
		memset(&hardware, 0, sizeof(hardware));
		set_fake_options(&invalid_cases[index], 1);
		assert(read_hardware(config, &hardware) != 0);
	}

	const struct fake_option bad_gain = {"hardware", "hardware_input_gain_db", "bad"};
	memset(&hardware, 0, sizeof(hardware));
	set_fake_options(&bad_gain, 1);
	settings_parse_error = 0;
	assert(!read_hardware(config, &hardware));
	assert(settings_parse_error && hardware.input_gain_configured);
}

static void test_chain_configuration_parser(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct txagc_settings value;
	const struct fake_option values[] = {
		{"local", "enabled", "yes"},
		{"local", "rnnoise_enabled", "yes"},
		{"local", "receive_bandpass_enabled", "yes"},
		{"local", "receive_bandpass_highpass_hz", "25"},
		{"local", "receive_bandpass_lowpass_hz", "5500"},
		{"local", "ctcss_filter_mode", "notch"},
		{"local", "input_gain_db", "2.5"},
		{"local", "agc_target_dbfs", "-10"},
		{"local", "target_dbfs", "-11"},
		{"local", "splatter_filter_highpass_hz", "100"},
		{"local", "stage_order", "equalizer,expander,agc,deesser,compressor,limiter"},
	};

	settings_defaults(&value);
	set_fake_options(values, ARRAY_LEN(values));
	settings_parse_error = 0;
	assert(!read_chain(config, "local", &value.chains[TXAGC_LOCAL]));
	assert(settings_parse_error); /* Conflicting modern and alias names are rejected. */
	assert(value.chains[TXAGC_LOCAL].enabled);
	assert(value.chains[TXAGC_LOCAL].rnnoise_enabled);
	assert(value.chains[TXAGC_LOCAL].input_gain_configured);
	assert(value.chains[TXAGC_LOCAL].splatter_filter_configured);
	assert(value.chains[TXAGC_LOCAL].agc.ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH);
	assert(value.chains[TXAGC_LOCAL].agc.stage_count == TXAGC_MAX_DYNAMICS_STAGES);

	const char *valid_modes[] = {"highpass", "disabled", "off"};
	for (size_t index = 0; index < ARRAY_LEN(valid_modes); ++index) {
		const struct fake_option option = {"local", "ctcss_filter_mode",
						   valid_modes[index]};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		settings_parse_error = 0;
		assert(!read_chain(config, "local", &value.chains[TXAGC_LOCAL]));
		assert(!settings_parse_error);
	}

	const struct fake_option invalid_mode = {"local", "ctcss_filter_mode", "comb"};
	settings_defaults(&value);
	set_fake_options(&invalid_mode, 1);
	settings_parse_error = 0;
	assert(!read_chain(config, "local", &value.chains[TXAGC_LOCAL]));
	assert(settings_parse_error);

	const struct fake_option invalid_order = {"local", "stage_order", "agc,agc"};
	settings_defaults(&value);
	set_fake_options(&invalid_order, 1);
	assert(read_chain(config, "local", &value.chains[TXAGC_LOCAL]) < 0);

	fake_option_count = 0;
	settings_defaults(&value);
	assert(!read_chain(config, "local", &value.chains[TXAGC_LOCAL]));
	const char *splatter_names[] = {"splatter_filter_enabled", "splatter_filter_highpass_hz",
					"output_highpass_hz", "splatter_filter_lowpass_hz",
					"output_lowpass_hz"};
	for (size_t index = 0; index < ARRAY_LEN(splatter_names); ++index) {
		const struct fake_option option = {"local", splatter_names[index],
						   index ? "100" : "yes"};
		settings_defaults(&value);
		set_fake_options(&option, 1);
		assert(!read_chain(config, "local", &value.chains[TXAGC_LOCAL]));
		assert(value.chains[TXAGC_LOCAL].splatter_filter_configured);
	}
}

static int add_single_override(const char *section, const char *name, const char *text)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct txagc_settings value;
	const struct fake_option option = {section, name, text};
	settings_defaults(&value);
	set_fake_options(&option, 1);
	return add_override(&value, config, section, name);
}

static void test_section_override_parser(void)
{
	struct txagc_settings value;
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	const struct {
		char *section;
		const char *name;
		const char *valid;
		const char *invalid;
	} cases[] = {
		{"asterisk", "asterisk_jitter_buffer_enabled", "yes", "maybe"},
		{"asterisk", "asterisk_jitter_buffer_force_enabled", "no", "maybe"},
		{"hardware", "hardware_rx_polarity_inverted", "yes", "maybe"},
		{"asterisk", "asterisk_jitter_buffer_implementation", "adaptive", "other"},
		{"hardware", "hardware_emphasis_corner_hz", "120", "300"},
		{"hardware", "hardware_gpio_1_mode", "in", "bad"},
		{"hardware", "hardware_parallel_pin_10_assignment", "cor", "out0"},
		{"hardware", "hardware_parallel_pin_2_assignment", "ptt", "in"},
		{"hardware", "hardware_parallel_port_device", "/dev/parport0", ""},
		{"hardware", "hardware_parallel_port_base_address", "0x378", "bad"},
		{"hardware", "hardware_rx_audio_source", "flat", "bad"},
		{"hardware", "hardware_rx_ctcss_source", "dsp", "bad"},
		{"hardware", "hardware_ctcss_turnoff_mode", "phase", "bad"},
		{"duplex", "duplex_local_repeat_mode", "software", "bad"},
		{"hardware", "hardware_device_identifier", "usb", NULL},
		{"hardware", "hardware_serial", "serial", NULL},
		{"hardware", "hardware_user_key", "key", NULL},
		{"hardware", "hardware_audio_fragment_count", "2", "-1"},
		{"hardware", "hardware_squelch_level", "999", "1000"},
		{"hardware", "hardware_tx_ctcss_level", "0", "1000"},
		{"duplex", "duplex_local_repeat_level", "500", "1000"},
		{"hardware", "hardware_clip_led_gpio", "8", "9"},
		{"hardware", "hardware_interface_type", "1", "2"},
		{"duplex", "duplex_radio_mode", "0", "2"},
		{"hardware", "hardware_tx_soft_limiter_setpoint", "5000", "4999"},
	};

	fake_option_count = 0;
	settings_defaults(&value);
	assert(!add_override(&value, config, "hardware", "hardware_serial"));
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
	assert(!add_single_override("hardware", "hardware_rx_audio_source", "no"));
	assert(!add_single_override("hardware", "hardware_rx_audio_source", "speaker"));
	const char *ctcss_sources[] = {"no", "usb", "usbinvert", "pp", "ppinvert"};
	for (size_t index = 0; index < ARRAY_LEN(ctcss_sources); ++index)
		assert(!add_single_override("hardware", "hardware_rx_ctcss_source",
					    ctcss_sources[index]));
	assert(!add_single_override("hardware", "hardware_ctcss_turnoff_mode", "no"));
	assert(!add_single_override("hardware", "hardware_ctcss_turnoff_mode", "notone"));
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
	assert(add_single_override("hardware", "hardware_tx_soft_limiter_setpoint", "13001") < 0);

	settings_defaults(&value);
	value.override_count = MAX_SECTION_OVERRIDES;
	const struct fake_option full = {"hardware", "hardware_serial", "serial"};
	set_fake_options(&full, 1);
	assert(add_override(&value, config, "hardware", "hardware_serial") < 0);

	const struct fake_option all_sections[] = {
		{"asterisk", asterisk_override_options[0], "yes"},
		{"hardware", hardware_override_options[0], "usb"},
		{"duplex", duplex_override_options[0], "1"},
		{"diagnostics", diagnostics_override_options[0], "1"},
		{"general", "channel_enabled", "yes"},
	};
	settings_defaults(&value);
	set_fake_options(all_sections, ARRAY_LEN(all_sections));
	assert(!read_section_overrides(&value, config));
	assert(value.override_count == ARRAY_LEN(all_sections));
}

static void test_option_name_validation(void)
{
	struct ast_config *config = (struct ast_config *)(uintptr_t)1;
	struct ast_variable variable = {0};

	fake_category_count = 1;
	fake_categories[0] = "unknown";
	fake_variables[0] = NULL;
	assert(validate_option_names(config) < 0);

	const struct {
		char *section;
		const char *name;
		int valid;
	} cases[] = {
		{"general", "channel", 1},
		{"general", "local_enabled", 1},
		{"general", "link_enabled", 1},
		{"general", "channel_enabled", 1},
		{"general", "enabled", 1},
		{"asterisk", asterisk_override_options[0], 1},
		{"hardware", "hardware_input_gain_db", 1},
		{"hardware", hardware_override_options[0], 1},
		{"duplex", duplex_override_options[0], 1},
		{"diagnostics", diagnostics_override_options[0], 1},
		{"local", "output_gain_db", 1},
		{"link", "splatter_filter_enabled", 0},
		{"voice_telemetry", "receive_bandpass_enabled", 0},
		{"general", "unknown", 0},
	};
	for (size_t index = 0; index < ARRAY_LEN(cases); ++index) {
		memset(&variable, 0, sizeof(variable));
		variable.name = cases[index].name;
		fake_category_count = 1;
		fake_categories[0] = cases[index].section;
		fake_variables[0] = &variable;
		assert((validate_option_names(config) == 0) == cases[index].valid);
	}
	const char *link_filters[] = {"splatter_filter_highpass_hz", "splatter_filter_lowpass_hz",
				      "output_highpass_hz", "output_lowpass_hz"};
	for (size_t index = 0; index < ARRAY_LEN(link_filters); ++index) {
		memset(&variable, 0, sizeof(variable));
		variable.name = link_filters[index];
		fake_categories[0] = "link";
		fake_variables[0] = &variable;
		assert(validate_option_names(config) < 0);
	}
	memset(&variable, 0, sizeof(variable));
	variable.name = "equalizer_enabled";
	fake_categories[0] = "link";
	fake_variables[0] = &variable;
	assert(!validate_option_names(config));
	const char *unknown_sections[] = {"asterisk", "hardware", "duplex", "diagnostics"};
	for (size_t index = 0; index < ARRAY_LEN(unknown_sections); ++index) {
		memset(&variable, 0, sizeof(variable));
		variable.name = "unknown";
		fake_categories[0] = (char *)unknown_sections[index];
		fake_variables[0] = &variable;
		assert(validate_option_names(config) < 0);
	}
	fake_category_count = 0;
}

static void test_settings_loader(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;

	fake_option_count = 0;
	fake_category_count = 0;
	fake_config_destroy_count = 0;
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(!load_settings());
	assert(!settings.enabled && !fake_config_destroy_count);

	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(load_settings() < 0);

	fake_config_load_result = valid;
	assert(!load_settings());
	assert(fake_config_destroy_count == 1);
	assert(!strcmp(settings.channel, "RadioPlus/"));

	const struct fake_option configured[] = {
		{"general", "enabled", "yes"},
		{"general", "channel", "RadioPlus/test"},
		{"general", "local_enabled", "no"},
		{"general", "link_enabled", "yes"},
		{"local", "agc_enabled", "yes"},
		{"link", "splatter_filter_enabled", "no"},
		{"voice_telemetry", "compressor_enabled", "yes"},
	};
	set_fake_options(configured, ARRAY_LEN(configured));
	assert(!load_settings());
	assert(settings.enabled);
	assert(!strcmp(settings.channel, "RadioPlus/test"));
	assert(!settings.chains[TXAGC_LOCAL].enabled);
	assert(settings.chains[TXAGC_LINK].enabled);
	assert(settings.chains[TXAGC_LOCAL].agc.agc_enabled);
	assert(settings.chains[TXAGC_VOICE_TELEMETRY].agc.compressor_enabled);
	assert(!settings.chains[TXAGC_LINK].agc.splatter_filter_enabled);

	const struct fake_option bad_hardware = {"hardware", "hardware_cos_assignment", "bad"};
	set_fake_options(&bad_hardware, 1);
	assert(load_settings() < 0);

	const struct fake_option bad_override = {"hardware", "hardware_eeprom_enabled", "bad"};
	set_fake_options(&bad_override, 1);
	assert(load_settings() < 0);

	const struct fake_option bad_graph = {"local", "stage_order", "agc,agc"};
	set_fake_options(&bad_graph, 1);
	assert(load_settings() < 0);
	const struct fake_option bad_general_graph = {"general", "stage_order", "agc,agc"};
	set_fake_options(&bad_general_graph, 1);
	assert(load_settings() < 0);

	const struct fake_option bad_asterisk = {"asterisk", "asterisk_jitter_buffer_enabled",
						 "bad"};
	set_fake_options(&bad_asterisk, 1);
	assert(load_settings() < 0);
	const struct fake_option bad_duplex = {"duplex", "duplex_local_repeat_mode", "bad"};
	set_fake_options(&bad_duplex, 1);
	assert(load_settings() < 0);
	const struct fake_option bad_diagnostics = {"diagnostics", "diagnostics_trace_level",
						    "bad"};
	set_fake_options(&bad_diagnostics, 1);
	assert(load_settings() < 0);

	const struct fake_option bad_boolean = {"general", "enabled", "bad"};
	set_fake_options(&bad_boolean, 1);
	assert(load_settings() < 0);

	const struct fake_option empty_channel = {"general", "channel", ""};
	set_fake_options(&empty_channel, 1);
	assert(load_settings() < 0);
	const struct fake_option bad_link_graph = {"link", "stage_order", "agc,agc"};
	set_fake_options(&bad_link_graph, 1);
	assert(load_settings() < 0);
	const struct fake_option bad_voice_graph = {"voice_telemetry", "stage_order", "agc,agc"};
	set_fake_options(&bad_voice_graph, 1);
	assert(load_settings() < 0);
	const struct fake_option voice_disabled = {"voice_telemetry", "enabled", "no"};
	set_fake_options(&voice_disabled, 1);
	assert(!load_settings());
	assert(!settings.chains[TXAGC_VOICE_TELEMETRY].enabled);
	const struct fake_option link_disabled = {"general", "link_enabled", "no"};
	set_fake_options(&link_disabled, 1);
	assert(!load_settings());
	assert(!settings.chains[TXAGC_LINK].enabled);

	struct ast_variable unknown = {0};
	unknown.name = "unknown";
	fake_option_count = 0;
	fake_category_count = 1;
	fake_categories[0] = "general";
	fake_variables[0] = &unknown;
	assert(load_settings() < 0);
	fake_category_count = 0;
}

static void set_override_value(size_t index, const char *section, const char *name,
			       const char *value)
{
	ast_copy_string(settings.overrides[index].section, section,
			sizeof(settings.overrides[index].section));
	ast_copy_string(settings.overrides[index].name, name,
			sizeof(settings.overrides[index].name));
	ast_copy_string(settings.overrides[index].value, value,
			sizeof(settings.overrides[index].value));
}

static void test_public_setting_accessors(void)
{
	struct txagc_chain chain;
	struct usbradioplus_hardware_settings hardware;
	char text[32];

	settings_defaults(&settings);
	assert(usbradioplus_processing_get_local(NULL) < 0);
	assert(!usbradioplus_processing_get_local(&chain));
	assert(!chain.enabled);
	settings.enabled = 1;
	assert(!usbradioplus_processing_get_local(&chain) && chain.enabled);
	settings.chains[TXAGC_LOCAL].enabled = 0;
	assert(!usbradioplus_processing_get_local(&chain) && !chain.enabled);
	assert(usbradioplus_processing_get_hardware(NULL) < 0);
	assert(!usbradioplus_processing_get_hardware(&hardware));
	assert(!hardware.input_gain_configured);
	assert(usbradioplus_processing_get_composite(NULL) < 0);
	assert(!usbradioplus_processing_get_composite(&chain));
	assert(chain.enabled);
	settings.chains[TXAGC_VOICE_TELEMETRY].enabled = 0;
	assert(!usbradioplus_processing_get_composite(&chain) && !chain.enabled);
	settings.chains[TXAGC_VOICE_TELEMETRY].enabled = 1;
	settings.enabled = 0;
	assert(!usbradioplus_processing_get_composite(&chain) && !chain.enabled);

	assert(usbradioplus_processing_set_local_input_gain(NAN) < 0);
	assert(usbradioplus_processing_set_local_input_gain(-31.0) < 0);
	assert(usbradioplus_processing_set_local_input_gain(31.0) < 0);
	assert(!usbradioplus_processing_set_local_input_gain(2.0));
	assert(settings.chains[TXAGC_LOCAL].agc.input_gain_db == 2.0);
	assert(settings.chains[TXAGC_LOCAL].input_gain_configured);
	assert(settings.agc.input_gain_db == 2.0);
	assert(usbradioplus_processing_set_hardware_input_gain(NAN) < 0);
	assert(usbradioplus_processing_set_hardware_input_gain(-31.0) < 0);
	assert(usbradioplus_processing_set_hardware_input_gain(31.0) < 0);
	assert(!usbradioplus_processing_set_hardware_input_gain(-2.0));
	assert(settings.hardware.input_gain_db == -2.0);
	assert(settings.hardware.input_gain_configured);

	assert(usbradioplus_processing_get_option(NULL, "x", text, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("x", NULL, text, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("x", "x", NULL, sizeof(text)) < 0);
	assert(usbradioplus_processing_get_option("x", "x", text, 0) < 0);
	settings.override_count = 5;
	set_override_value(0, "asterisk", asterisk_override_options[0], "yes");
	set_override_value(1, "hardware", hardware_override_options[0], "usb");
	set_override_value(2, "duplex", duplex_override_options[0], "1");
	set_override_value(3, "diagnostics", diagnostics_override_options[0], "2");
	set_override_value(4, "general", "channel_enabled", "no");
	assert(!usbradioplus_processing_get_option("asterisk", asterisk_legacy_options[0], text,
						   sizeof(text)) &&
	       !strcmp(text, "yes"));
	assert(!usbradioplus_processing_get_option("hardware", hardware_legacy_options[0], text,
						   sizeof(text)) &&
	       !strcmp(text, "usb"));
	assert(!usbradioplus_processing_get_option("duplex", duplex_legacy_options[0], text,
						   sizeof(text)) &&
	       !strcmp(text, "1"));
	assert(!usbradioplus_processing_get_option("diagnostics", diagnostics_legacy_options[0],
						   text, sizeof(text)) &&
	       !strcmp(text, "2"));
	assert(!usbradioplus_processing_get_option("general", "radioactive", text, sizeof(text)) &&
	       !strcmp(text, "no"));
	assert(usbradioplus_processing_get_option("hardware", "missing", text, sizeof(text)) == 1);
	assert(!usbradioplus_processing_get_option("asterisk", asterisk_override_options[0], text,
						   sizeof(text)));
	assert(!usbradioplus_processing_get_option("hardware", hardware_override_options[0], text,
						   sizeof(text)));
	assert(!usbradioplus_processing_get_option("duplex", duplex_override_options[0], text,
						   sizeof(text)));
	assert(!usbradioplus_processing_get_option("diagnostics", diagnostics_override_options[0],
						   text, sizeof(text)));
	assert(usbradioplus_processing_get_option("general", "missing", text, sizeof(text)) == 1);
	assert(usbradioplus_processing_get_option("other", "missing", text, sizeof(text)) == 1);
}

static void reset_save_doubles(void)
{
	fake_config_destroy_count = 0;
	fake_hardware_category = (struct ast_category *)(uintptr_t)1;
	fake_local_category = (struct ast_category *)(uintptr_t)2;
	fake_category_new_failure = 0;
	fake_category_append_count = 0;
	fake_save_result = 0;
	fake_tune_update_failure_call = 0;
	fake_tune_update_calls = 0;
}

static void test_input_gain_persistence(void)
{
	struct ast_config *valid = (struct ast_config *)(uintptr_t)1;

	assert(usbradioplus_processing_save_input_gains(NAN, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains(-31.0, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains(31.0, 0.0) < 0);
	assert(usbradioplus_processing_save_input_gains(0.0, NAN) < 0);
	assert(usbradioplus_processing_save_input_gains(0.0, -31.0) < 0);
	assert(usbradioplus_processing_save_input_gains(0.0, 31.0) < 0);

	reset_save_doubles();
	fake_config_load_result = NULL;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	assert(!usbradioplus_processing_save_input_gains(1.25, -2.5));
	assert(fake_tune_update_calls == 2 && fake_config_destroy_count == 1);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_hardware_category = NULL;
	fake_local_category = NULL;
	assert(!usbradioplus_processing_save_input_gains(1.0, 2.0));
	assert(fake_category_append_count == 2);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_hardware_category = NULL;
	fake_category_new_failure = 1;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);
	assert(fake_config_destroy_count == 1);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_tune_update_failure_call = 1;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_local_category = NULL;
	fake_category_new_failure = 1;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_tune_update_failure_call = 2;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);

	reset_save_doubles();
	fake_config_load_result = valid;
	fake_save_result = -1;
	assert(usbradioplus_processing_save_input_gains(0.0, 0.0) < 0);
}

static void test_module_lifecycle_and_simple_cli(void)
{
	struct ast_cli_entry entry = {0};
	struct ast_cli_args arguments = {.fd = 1, .argc = 3};
	struct ast_cli_args bad_arguments = {.fd = 1, .argc = 2};

	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_load() == AST_MODULE_LOAD_DECLINE);
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
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
	assert(!usbradioplus_processing_prime());
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(usbradioplus_processing_prime() < 0);
	assert(usbradioplus_processing_reload() < 0);
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(!usbradioplus_processing_reload());

	assert(cli_enable(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_enable(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_enable(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	assert(cli_enable(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(settings.enabled);

	assert(cli_disable(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_disable(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_disable(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	assert(cli_disable(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(!settings.enabled);

	assert(cli_reload(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_reload(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_reload(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	fake_config_load_result = CONFIG_STATUS_FILEINVALID;
	assert(cli_reload(&entry, 99, &arguments) == CLI_FAILURE);
	fake_config_load_result = CONFIG_STATUS_FILEMISSING;
	assert(cli_reload(&entry, 99, &arguments) == CLI_SUCCESS);
	assert(fake_cli_print_calls > 0);
	stopping = 0;
	assert(scanner(NULL) == NULL);
	assert(stopping);
}

static void test_channel_eligibility(void)
{
	struct txagc_settings value;
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	settings_defaults(&value);
	value.chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	assert(channel_is_eligible(channel, &value));
	value.chains[TXAGC_LINK].enabled = 0;
	assert(!channel_is_eligible(channel, &value));
	value.chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "PJSIP/test";
	assert(!channel_is_eligible(channel, &value));
	fake_channel_name = "IAX2/test";
	fake_channel_application = NULL;
	assert(!channel_is_eligible(channel, &value));
	fake_channel_application = "Other";
	assert(!channel_is_eligible(channel, &value));
	fake_channel_application = "Rpt";
	fake_channel_data = NULL;
	assert(!channel_is_eligible(channel, &value));
	fake_channel_data = "Other";
	assert(!channel_is_eligible(channel, &value));
	fake_channel_data = "Remote Rx";
}

static void test_audiohook_callback_and_destroy(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_audiohook audiohook = {0};
	struct ast_frame frame = {0};
	struct ast_datastore datastore = {0};
	struct txagc_hook hook = {0};
	int16_t pcm[] = {100, -100, 2};

	settings_defaults(&settings);
	settings.enabled = 1;
	settings.chains[TXAGC_LINK].enabled = 1;
	fake_channel_name = "IAX2/test";
	fake_channel_application = "Rpt";
	fake_channel_data = "Remote Rx";
	fake_channel_datastore = &datastore;
	datastore.data = &hook;
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = pcm;
	frame.samples = ARRAY_LEN(pcm);
	frame.subclass.format = (struct ast_format *)(uintptr_t)1;

	audiohook.status = AST_AUDIOHOOK_STATUS_DONE;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	audiohook.status = AST_AUDIOHOOK_STATUS_RUNNING;
	frame.frametype = AST_FRAME_DTMF;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.frametype = AST_FRAME_VOICE;
	frame.data.ptr = NULL;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.data.ptr = pcm;
	frame.samples = 0;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	frame.samples = ARRAY_LEN(pcm);
	fake_channel_datastore = NULL;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	fake_channel_datastore = &datastore;
	datastore.data = NULL;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	datastore.data = &hook;
	fake_channel_name = "RadioPlus/";
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	fake_channel_name = "IAX2/test";
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_WRITE));
	settings.enabled = 0;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	settings.enabled = 1;
	settings.chains[TXAGC_LINK].enabled = 0;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	settings.chains[TXAGC_LINK].enabled = 1;
	fake_sample_rate = 0;
	fake_processor_result = -1;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(pcm[0] == 100 && pcm[1] == -100);
	fake_sample_rate = 48000;
	fake_processor_result = 0;
	fake_processor_saturate = 1;
	assert(!txagc_callback(&audiohook, channel, &frame, AST_AUDIOHOOK_DIRECTION_READ));
	assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[2] == 2);
	fake_processor_saturate = 0;

	fake_audiohook_detach_calls = 0;
	fake_audiohook_destroy_calls = 0;
	fake_processor_destroy_calls = 0;
	hook_destroy(NULL);
	struct txagc_hook *allocated = calloc(1, sizeof(*allocated));
	assert(allocated);
	hook_destroy(allocated);
	assert(fake_audiohook_detach_calls == 1);
	assert(fake_audiohook_destroy_calls == 1);
	assert(fake_processor_destroy_calls == TXAGC_SOURCE_COUNT);
}

static void reset_attach_doubles(void)
{
	fake_channel_datastore = NULL;
	fake_find_sequence_count = 0;
	fake_find_sequence_index = 0;
	fake_datastore_alloc_failure = 0;
	fake_calloc_failure = 0;
	fake_datastore_free_calls = 0;
	fake_audiohook_init_result = 0;
	fake_audiohook_attach_result = 0;
	fake_datastore_add_calls = 0;
	fake_datastore_remove_calls = 0;
	fake_last_allocated_datastore = NULL;
	fake_channel_name = "IAX2/test";
}

static void test_audiohook_attachment(void)
{
	struct ast_channel *channel = (struct ast_channel *)(uintptr_t)1;
	struct ast_datastore existing = {0};

	reset_attach_doubles();
	fake_channel_datastore = &existing;
	assert(!attach_hook(channel));
	assert(!fake_datastore_add_calls);

	reset_attach_doubles();
	fake_datastore_alloc_failure = 1;
	assert(attach_hook(channel) < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_calloc_failure = 1;
	assert(attach_hook(channel) < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_audiohook_init_result = -1;
	assert(attach_hook(channel) < 0);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_find_sequence[0] = NULL;
	fake_find_sequence[1] = &existing;
	fake_find_sequence_count = 2;
	assert(!attach_hook(channel));
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	fake_audiohook_attach_result = -1;
	assert(attach_hook(channel) < 0);
	assert(fake_datastore_add_calls == 1);
	assert(fake_datastore_remove_calls == 1);
	assert(fake_datastore_free_calls == 1);

	reset_attach_doubles();
	assert(!attach_hook(channel));
	assert(fake_datastore_add_calls == 1);
	assert(fake_last_allocated_datastore);
	hook_destroy(fake_last_allocated_datastore->data);
	ast_datastore_free(fake_last_allocated_datastore);
}

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

static void test_channel_scanning_and_detachment(void)
{
	settings_defaults(&settings);
	reset_iterator_doubles();
	scan_channels();
	assert(!fake_iterator_destroy_calls);

	settings.enabled = 1;
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

static void test_reporting_cli(void)
{
	struct ast_cli_entry entry = {0};
	struct ast_cli_args arguments = {.fd = 1, .argc = 3};
	struct ast_cli_args bad_arguments = {.fd = 1, .argc = 2};
	struct ast_datastore datastore = {0};
	struct txagc_hook hook = {0};

	assert(cli_show(&entry, CLI_INIT, &arguments) == NULL);
	assert(cli_show(&entry, CLI_GENERATE, &arguments) == NULL);
	assert(cli_show(&entry, 99, &bad_arguments) == CLI_SHOWUSAGE);
	settings_defaults(&settings);
	settings.chains[TXAGC_LOCAL].ctcss_filter_configured = 1;
	settings.chains[TXAGC_LOCAL].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	settings.enabled = 1;
	settings.local_enabled = 1;
	settings.link_enabled = 1;
	settings.rnnoise_enabled = 1;
	settings.agc.receive_bandpass_enabled = 1;
	settings.agc.equalizer_enabled = 1;
	settings.agc.agc_enabled = 1;
	settings.agc.expander_enabled = 1;
	settings.agc.deesser_enabled = 1;
	settings.agc.compressor_enabled = 1;
	settings.agc.limiter_enabled = 1;
	settings.agc.splatter_filter_enabled = 1;
	settings.agc.lookahead_limiter_enabled = 1;
	for (size_t source = 0; source < TXAGC_SOURCE_COUNT; ++source) {
		settings.chains[source].enabled = 1;
		settings.chains[source].rnnoise_enabled = 1;
		settings.chains[source].agc.equalizer_enabled = 1;
		settings.chains[source].agc.agc_enabled = 1;
		settings.chains[source].agc.expander_enabled = 1;
		settings.chains[source].agc.deesser_enabled = 1;
		settings.chains[source].agc.compressor_enabled = 1;
		settings.chains[source].agc.limiter_enabled = 1;
		settings.chains[source].agc.lookahead_limiter_enabled = 1;
	}
	settings.chains[TXAGC_LOCAL].agc.receive_bandpass_enabled = 1;
	assert(cli_show(&entry, 99, &arguments) == CLI_SUCCESS);
	for (size_t source = 0; source < TXAGC_SOURCE_COUNT; ++source)
		settings.chains[source].enabled = settings.chains[source].rnnoise_enabled = 0;
	settings.chains[TXAGC_LOCAL].agc.receive_bandpass_enabled = 0;
	settings.chains[TXAGC_LOCAL].agc.equalizer_enabled = 0;
	settings.chains[TXAGC_VOICE_TELEMETRY].agc.splatter_filter_enabled = 0;
	settings.chains[TXAGC_LOCAL].ctcss_filter_configured = 0;
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
	hook.avfilter[TXAGC_LOCAL].input_samples = 1;
	hook.avfilter[TXAGC_LOCAL].input_peak_dbfs = -10.0;
	hook.avfilter[TXAGC_LOCAL].input_rms_dbfs = -20.0;
	hook.avfilter[TXAGC_LOCAL].output_samples = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);
	datastore.data = NULL;
	fake_iterator_channels_remaining = 1;
	assert(cli_stats(&entry, 99, &arguments) == CLI_SUCCESS);
	fake_channel_datastore = NULL;
}

int main(void)
{
	struct txagc_settings value;
	settings_defaults(&value);
	assert(!validate_settings(&value));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_NOTCH), "notch"));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_HIGHPASS), "highpass"));
	assert(!strcmp(ctcss_filter_name(TXAGC_CTCSS_FILTER_DISABLED), "disabled"));
	test_every_numeric_boundary();
	test_stage_and_relationship_validation();
	test_settings_scope_and_hardware_validation();
	test_primitive_configuration_parsers();
	test_hardware_configuration_parser();
	test_chain_configuration_parser();
	test_section_override_parser();
	test_option_name_validation();
	test_settings_loader();
	test_public_setting_accessors();
	test_input_gain_persistence();
	test_module_lifecycle_and_simple_cli();
	test_channel_eligibility();
	test_audiohook_callback_and_destroy();
	test_audiohook_attachment();
	test_channel_scanning_and_detachment();
	test_reporting_cli();
	puts("processing validation tests passed");
	return 0;
}
