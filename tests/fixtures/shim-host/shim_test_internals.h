#ifndef USBRADIOPLUS_SHIM_TEST_INTERNALS_H
#define USBRADIOPLUS_SHIM_TEST_INTERNALS_H

#include "asterisk.h"
#include "usbradioplus_asterisk.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define URP_APP_RPT_RATE UINT32_C(8000)
#define URP_ADVANCED_RATE UINT32_C(48000)
#define URP_FRAME_MILLISECONDS UINT32_C(20)

struct urp_channel {
	struct urp_channel *next;
	char *name;
	void *rust_channel;
	struct ast_taskprocessor *control;
	struct ast_dsp *dsp;
	struct ast_format *format;
	uint32_t sample_rate_hz;
	uint32_t frame_samples;
	pthread_t delivery_thread;
	atomic_int delivery_running;
	atomic_int delivery_stop;
	atomic_int service_failed;
	atomic_int jitter_pending;
	atomic_uint_fast64_t pending_transmit;
	_Atomic(struct ast_channel *) owner;
};

struct urp_link_hook {
	struct ast_audiohook audiohook;
	void *rust_link;
	void *reload_link;
	int reload_pending;
	atomic_uint references;
	atomic_int attachment_state;
	char *asterisk_channel;
	char *profile;
};

struct urp_link_reload_entry {
	struct urp_link_reload_entry *next;
	struct urp_link_hook *hook;
};

enum urp_control_operation {
	URP_CONTROL_START,
	URP_CONTROL_STOP,
	URP_CONTROL_RELOAD_PREPARE,
	URP_CONTROL_RELOAD_ACTIVATE,
	URP_CONTROL_RELOAD_FINISH,
	URP_CONTROL_TEXT,
	URP_CONTROL_TRANSMIT,
	URP_CONTROL_DTMF,
	URP_CONTROL_ECHO,
	URP_CONTROL_JITTER,
	URP_CONTROL_COMMAND,
	URP_CONTROL_STATUS,
	URP_CONTROL_SERVICE,
	URP_CONTROL_DESTROY,
};

struct urp_control_task {
	struct urp_channel *channel;
	enum urp_control_operation operation;
	union {
		struct {
			const uint8_t *text;
			uint32_t length;
		} text;
		struct {
			uint32_t keyed;
			uint32_t ctcss_tenths_hz;
		} transmit;
		uint32_t enabled;
		struct urp_ast_jitter_config *jitter;
		struct urp_ast_channel_command *command;
		struct urp_ast_channel_status *status;
	} argument;
	struct ast_sem complete;
	int result;
};

extern struct ast_channel_tech app_rpt_tech;
extern struct ast_channel_tech advanced_tech;
extern const struct urp_ast_descriptor *rust_adapter;
extern void *rust_driver;
extern struct urp_channel *channel_list;
extern atomic_uint active_channels;
extern atomic_int link_scan_running;
extern atomic_int link_scan_stop;

struct ast_channel *urp_lock_owner(const struct urp_channel *channel);
int urp_queue_voice(void *application_context, void *channel_context, const int16_t *samples,
		    uint32_t sample_count, uint32_t sample_rate_hz);
int urp_queue_control(void *application_context, void *channel_context, uint32_t kind,
		      int32_t value, uint64_t duration_ms);
int urp_queue_text(void *application_context, void *channel_context, const uint8_t *text,
		   uint32_t text_length);
int urp_analyze_dtmf(void *application_context, void *channel_context, int16_t *samples,
		     uint32_t sample_count, uint32_t sample_rate_hz,
		     struct urp_ast_dtmf_result *result);
uint64_t urp_monotonic_milliseconds(void *application_context);
void urp_log(void *application_context, uint32_t level, const uint8_t *message,
	     uint32_t message_length);
int urp_control_execute(void *opaque);
int urp_control_run(struct urp_channel *channel, struct urp_control_task *task);
int urp_configure_jitter(struct urp_channel *channel);
void *urp_delivery_worker(void *opaque);
struct ast_channel_tech *urp_technology(const char *type, uint32_t *transport,
					uint32_t *sample_rate_hz);
void urp_abandon_channel(struct urp_channel *channel);
struct ast_channel *urp_request(const char *type, struct ast_format_cap *cap,
				const struct ast_assigned_ids *assignedids,
				const struct ast_channel *requestor, const char *data, int *cause);
int urp_call(struct ast_channel *owner, const char *destination, int timeout);
int urp_hangup(struct ast_channel *owner);
int urp_answer(struct ast_channel *channel);
struct ast_frame *urp_read(struct ast_channel *owner);
int urp_write(struct ast_channel *owner, struct ast_frame *frame);
int urp_send_text(struct ast_channel *owner, const char *text);
int urp_forced_ctcss(const void *data, size_t data_length, uint32_t *tenths_hz);
int urp_indicate(struct ast_channel *owner, int condition, const void *data, size_t data_length);
int urp_fixup(struct ast_channel *old_channel, struct ast_channel *new_channel);
int urp_setoption(struct ast_channel *owner, int option, void *data, int data_length);
int urp_digit_begin(struct ast_channel *channel, char digit);
int urp_digit_end(struct ast_channel *channel, char digit, unsigned int duration);
char *urp_read_configuration(char **source);
int urp_reload_configuration(void);
char *urp_driver_channel_name(uint32_t index, int active, int *result);
struct urp_channel *urp_lock_channel(const char *name);
struct urp_channel *urp_lock_active_channel(char **name, int *result);
void urp_unlock_channel(void);
void urp_link_hook_destroy(void *data);
void urp_scan_links(void);
void *urp_link_scanner(void *unused);
void urp_detach_all_links(void);
int urp_prepare_link_reload(struct urp_link_reload_entry **entries);
void urp_finish_link_reload(struct urp_link_reload_entry *entries, int commit);
void urp_print_link_statistics(int fd);
int urp_get_active_status(struct urp_ast_channel_status *status, char **name);
int urp_print_status(int fd);
int urp_parse_unsigned(const char *text, uint32_t maximum, uint32_t *value);
int urp_run_active_control(struct urp_control_task *task);
int urp_run_command(struct urp_ast_channel_command *command);
uint32_t urp_mixer_target(const char *name);
int urp_parse_eeprom_words(const char *text, uint16_t words[64]);
void urp_print_eeprom(int fd, const struct urp_ast_channel_command *command);
char *urp_cli_active(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
int urp_print_channel_list(int fd);
char *urp_cli_channel_list(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_channel_status(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_channel_echo(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_channel_transmit(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_channel_command(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_channel_flash(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_processing_stats(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
char *urp_cli_reload(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
int urp_adapter_valid(const struct urp_ast_descriptor *adapter);
int urp_create_driver(void);
int urp_prepare_capability(struct ast_channel_tech *technology, struct ast_format *format);

#endif
