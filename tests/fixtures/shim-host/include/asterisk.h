#ifndef USBRADIOPLUS_TEST_ASTERISK_H
#define USBRADIOPLUS_TEST_ASTERISK_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))
#define AST_CAUSE_BUSY 17
#define AST_FORMAT_CAP_FLAG_DEFAULT 0
#define AST_JB_ENABLED (1U << 0)
#define AST_JB_FORCED (1U << 1)
#define AST_JB_LOG (1U << 2)
#define AST_JB_SYNC_VIDEO (1U << 3)
#define AST_MODULE_LOAD_DECLINE (-1)
#define AST_MODULE_LOAD_FAILURE (-2)
#define AST_MODULE_LOAD_SUCCESS 0
#define AST_OPTION_TONE_VERIFY 1
#define AST_TASKPROCESSOR_MAX_NAME 79
#define CLI_FAILURE ((char *)-1)
#define CLI_GENERATE 1
#define CLI_INIT 0
#define CLI_SHOWUSAGE ((char *)-2)
#define CLI_SUCCESS ((char *)0)
#define DSP_DIGITMODE_DTMF (1U << 0)
#define DSP_DIGITMODE_MUTECONF (1U << 1)
#define DSP_DIGITMODE_RELAXDTMF (1U << 2)
#define DSP_FEATURE_DIGIT_DETECT (1U << 0)
#define LOG_ERROR 1
#define LOG_NOTICE 2
#define LOG_WARNING 3
#define TPS_REF_DEFAULT 0

enum ast_channel_state {
	AST_STATE_DOWN,
	AST_STATE_UP,
};

enum ast_frame_type {
	AST_FRAME_VOICE = 1,
	AST_FRAME_CONTROL,
	AST_FRAME_TEXT,
	AST_FRAME_DTMF_BEGIN,
	AST_FRAME_DTMF_END,
};

enum ast_control_frame_type {
	AST_CONTROL_BUSY = 1,
	AST_CONTROL_CONGESTION,
	AST_CONTROL_RINGING,
	AST_CONTROL_VIDUPDATE,
	AST_CONTROL_HOLD,
	AST_CONTROL_UNHOLD,
	AST_CONTROL_PROCEEDING,
	AST_CONTROL_PROGRESS,
	AST_CONTROL_RADIO_KEY,
	AST_CONTROL_RADIO_UNKEY,
};

struct ast_format {
	unsigned int sample_rate;
};

struct ast_format_cap {
	struct ast_format *format;
};

struct ast_frame {
	enum ast_frame_type frametype;
	union {
		int integer;
		struct ast_format *format;
	} subclass;
	union {
		void *ptr;
	} data;
	int datalen;
	int samples;
	long len;
	const char *src;
};

struct ast_assigned_ids {
	int unused;
};

struct ast_audiohook;
struct ast_channel;
struct ast_datastore;
struct ast_datastore_info;
struct ast_channel_tech {
	const char *type;
	const char *description;
	struct ast_format_cap *capabilities;
	struct ast_channel *(*requester)(const char *, struct ast_format_cap *,
					 const struct ast_assigned_ids *,
					 const struct ast_channel *, const char *, int *);
	int (*send_digit_begin)(struct ast_channel *, char);
	int (*send_digit_end)(struct ast_channel *, char, unsigned int);
	int (*send_text)(struct ast_channel *, const char *);
	int (*hangup)(struct ast_channel *);
	int (*answer)(struct ast_channel *);
	struct ast_frame *(*read)(struct ast_channel *);
	int (*call)(struct ast_channel *, const char *, int);
	int (*write)(struct ast_channel *, struct ast_frame *);
	int (*indicate)(struct ast_channel *, int, const void *, size_t);
	int (*fixup)(struct ast_channel *, struct ast_channel *);
	int (*setoption)(struct ast_channel *, int, void *, int);
};

struct ast_channel {
	void *technology_private;
	const struct ast_channel_tech *technology;
	enum ast_channel_state state;
	int locked;
	const char *name;
	const char *application;
	const char *data;
	struct ast_format *raw_read_format;
	struct ast_datastore *datastore;
	struct ast_audiohook *audiohook;
	unsigned int references;
};

struct ast_channel_iterator {
	size_t index;
};

struct ast_cli_args;

struct ast_cli_entry {
	const char *command;
	const char *usage;
	char *(*handler)(struct ast_cli_entry *, int, struct ast_cli_args *);
	const char *summary;
};

struct ast_cli_args {
	int fd;
	int argc;
	char **argv;
};

struct ast_dsp {
	int unused;
};

struct ast_jb_conf {
	unsigned int flags;
	unsigned int max_size;
	unsigned int resync_threshold;
	unsigned int target_extra;
	char impl[16];
};

struct ast_module {
	int unused;
};

struct ast_module_info_fixture {
	struct ast_module *self;
};

/** Module lifecycle entry points captured from the fixture-only Asterisk macro. */
struct ast_module_entry_points_fixture {
	int (*load)(void);
	int (*unload)(void);
	int (*reload)(void);
};

struct ast_taskprocessor {
	int unused;
};

struct ast_sem {
	int posted;
};

typedef int ast_mutex_t;
typedef int ast_rwlock_t;

extern struct ast_format *ast_format_slin;
extern struct ast_frame ast_null_frame;
extern const char *ast_config_AST_CONFIG_DIR;
extern struct ast_module_info_fixture *ast_module_info;
extern const struct ast_module_entry_points_fixture ast_module_entry_points;

#define AST_CLI_DEFINE(function, text) {.handler = (function), .summary = (text)}
#define AST_MODULE_INFO(...)                                                                       \
	const struct ast_module_entry_points_fixture ast_module_entry_points = {                   \
		.load = load_module, .unload = unload_module, .reload = reload_module}
#define AST_MUTEX_DEFINE_STATIC(name) static ast_mutex_t name
#define AST_RWLOCK_DEFINE_STATIC(name) static ast_rwlock_t name

void ao2_cleanup(void *object);
int ast_asprintf(char **output, const char *format, ...);
void *ast_calloc(size_t count, size_t size);
struct ast_channel *
ast_channel_alloc(int need_queue, enum ast_channel_state state, const char *caller_number,
		  const char *caller_name, const char *account_code, const char *extension,
		  const char *context, const struct ast_assigned_ids *assigned_ids,
		  const struct ast_channel *requestor, int ama_flags, const char *name_format, ...);
void ast_channel_internal_fd_set(struct ast_channel *channel, int which, int value);
const char *ast_channel_appl(const struct ast_channel *channel);
const char *ast_channel_data(const struct ast_channel *channel);
struct ast_datastore *ast_channel_datastore_find(struct ast_channel *channel,
						 const struct ast_datastore_info *info,
						 const char *uid);
void ast_channel_datastore_add(struct ast_channel *channel, struct ast_datastore *datastore);
void ast_channel_datastore_remove(struct ast_channel *channel, struct ast_datastore *datastore);
struct ast_channel_iterator *ast_channel_iterator_all_new(void);
void ast_channel_iterator_destroy(struct ast_channel_iterator *iterator);
struct ast_channel *ast_channel_iterator_next(struct ast_channel_iterator *iterator);
void ast_channel_lock(struct ast_channel *channel);
const char *ast_channel_name(const struct ast_channel *channel);
void ast_channel_nativeformats_set(struct ast_channel *channel, struct ast_format_cap *formats);
struct ast_format *ast_channel_rawreadformat(const struct ast_channel *channel);
void ast_channel_unref(struct ast_channel *channel);
int ast_channel_register(const struct ast_channel_tech *technology);
void ast_channel_set_readformat(struct ast_channel *channel, struct ast_format *format);
void ast_channel_set_writeformat(struct ast_channel *channel, struct ast_format *format);
enum ast_channel_state ast_channel_state(const struct ast_channel *channel);
void *ast_channel_tech_pvt(const struct ast_channel *channel);
void ast_channel_tech_pvt_set(struct ast_channel *channel, void *value);
void ast_channel_tech_set(struct ast_channel *channel, const struct ast_channel_tech *technology);
int ast_channel_trylock(struct ast_channel *channel);
void ast_channel_unlock(struct ast_channel *channel);
void ast_channel_unregister(const struct ast_channel_tech *technology);
void ast_cli(int fd, const char *format, ...);
int ast_cli_register_multiple(struct ast_cli_entry *entries, int count);
int ast_cli_unregister_multiple(struct ast_cli_entry *entries, int count);
void ast_copy_string(char *destination, const char *source, size_t size);
void ast_dsp_free(struct ast_dsp *dsp);
struct ast_dsp *ast_dsp_new(void);
struct ast_frame *ast_dsp_process(struct ast_channel *channel, struct ast_dsp *dsp,
				  struct ast_frame *frame);
int ast_dsp_set_digitmode(struct ast_dsp *dsp, int mode);
void ast_dsp_set_features(struct ast_dsp *dsp, int features);
struct ast_format *ast_format_cache_get_slin_by_rate(unsigned int rate);
struct ast_format_cap *ast_format_cap_alloc(int flags);
int ast_format_cap_append(struct ast_format_cap *capabilities, struct ast_format *format,
			  unsigned int framing);
int ast_format_cap_iscompatible(const struct ast_format_cap *first,
				const struct ast_format_cap *second);
unsigned int ast_format_get_sample_rate(const struct ast_format *format);
void ast_frfree(struct ast_frame *frame);
void ast_free(void *pointer);
void ast_hangup(struct ast_channel *channel);
void ast_jb_conf_default(struct ast_jb_conf *configuration);
void ast_jb_configure(struct ast_channel *channel, const struct ast_jb_conf *configuration);
void ast_log(int level, const char *format, ...);
void *ast_malloc(size_t size);
struct ast_module *ast_module_ref(struct ast_module *module);
void ast_module_unref(struct ast_module *module);
int ast_moh_start(struct ast_channel *channel, const char *music_class,
		  const char *interpreter_class);
void ast_moh_stop(struct ast_channel *channel);
void ast_mutex_lock(ast_mutex_t *mutex);
void ast_mutex_unlock(ast_mutex_t *mutex);
int ast_rwlock_tryrdlock(ast_rwlock_t *lock);
void ast_rwlock_unlock(ast_rwlock_t *lock);
void ast_rwlock_wrlock(ast_rwlock_t *lock);
int ast_pthread_create_background(pthread_t *thread, const pthread_attr_t *attributes,
				  void *(*start)(void *), void *data);
int ast_queue_frame(struct ast_channel *channel, struct ast_frame *frame);
char *ast_read_textfile(const char *path);
int ast_sem_destroy(struct ast_sem *semaphore);
int ast_sem_init(struct ast_sem *semaphore, int shared, unsigned int value);
int ast_sem_post(struct ast_sem *semaphore);
int ast_sem_wait(struct ast_sem *semaphore);
int ast_setstate(struct ast_channel *channel, enum ast_channel_state state);
char *ast_strdup(const char *text);
struct ast_taskprocessor *ast_taskprocessor_get(const char *name, int reference_type);
int ast_taskprocessor_push(struct ast_taskprocessor *processor, int (*callback)(void *),
			   void *data);
struct ast_taskprocessor *ast_taskprocessor_unreference(struct ast_taskprocessor *processor);
void ast_verbose(const char *format, ...);

#define ast_strlen_zero(text) (!(text) || !*(text))

#endif
