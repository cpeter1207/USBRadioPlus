/** @file
 * @brief Exact installed-Asterisk variadic ABI for the Rust unit-test harness.
 *
 * This object is linked only by the cfg(test) support module. Rendered capture
 * strings are limited to 4095 bytes; Rust copies them before these calls return.
 */
#include "../../../wrapper.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static _Thread_local unsigned int fail_thread_create;
static _Thread_local unsigned int thread_create_calls;
static _Thread_local int fail_clock_read;
static _Thread_local int float_format_armed;
static _Thread_local int float_format_result;

void urp_test_float_format_result(int result)
{
	float_format_result = result;
	float_format_armed = 1;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int result;
	if (float_format_armed && (!strcmp(format, "%.9g") || !strcmp(format, "%.17g"))) {
		float_format_armed = 0;
		return float_format_result;
	}
	va_start(arguments, format);
	result = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return result;
}

void urp_test_fail_thread_create(unsigned int nth)
{
	fail_thread_create = nth;
	thread_create_calls = 0;
}

unsigned int urp_test_thread_create_calls(void)
{
	return thread_create_calls;
}

void urp_test_fail_clock_read(void)
{
	fail_clock_read = 1;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *),
		   void *argument)
{
	typedef int (*create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
	create_fn create;
	++thread_create_calls;
	if (fail_thread_create && !--fail_thread_create)
		return EAGAIN;
	create = (create_fn)dlsym(RTLD_NEXT, "pthread_create");
	return create ? create(thread, attributes, start, argument) : ENOSYS;
}

int clock_gettime(clockid_t clock, struct timespec *value)
{
	typedef int (*clock_fn)(clockid_t, struct timespec *);
	clock_fn read_clock;
	if (fail_clock_read) {
		fail_clock_read = 0;
		errno = EIO;
		return -1;
	}
	read_clock = (clock_fn)dlsym(RTLD_NEXT, "clock_gettime");
	if (!read_clock) {
		errno = ENOSYS;
		return -1;
	}
	return read_clock(clock, value);
}

extern void urp_test_message(int kind, int value, const char *text);
extern struct ast_channel *urp_test_channel_alloc(int state, const char *name);

static void capture(int kind, int value, const char *format, va_list arguments)
{
	char text[4096];
	(void)vsnprintf(text, sizeof(text), format, arguments);
	urp_test_message(kind, value, text);
}

void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	va_list arguments;
	(void)file;
	(void)line;
	(void)function;
	va_start(arguments, format);
	capture(0, level, format, arguments);
	va_end(arguments);
}

void ast_cli(int fd, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	capture(1, fd, format, arguments);
	va_end(arguments);
}

void __ast_verbose(const char *file, int line, const char *function, int level, const char *format,
		   ...)
{
	va_list arguments;
	(void)file;
	(void)line;
	(void)function;
	va_start(arguments, format);
	capture(2, level, format, arguments);
	va_end(arguments);
}

struct ast_channel *__ast_channel_alloc(int needqueue, int state, const char *cid_num,
					const char *cid_name, const char *acctcode,
					const char *exten, const char *context,
					const struct ast_assigned_ids *assignedids,
					const struct ast_channel *requestor, enum ama_flags amaflag,
					struct ast_endpoint *endpoint, const char *file, int line,
					const char *function, const char *name_format, ...)
{
	char name[4096];
	va_list arguments;
	(void)needqueue;
	(void)cid_num;
	(void)cid_name;
	(void)acctcode;
	(void)exten;
	(void)context;
	(void)assignedids;
	(void)requestor;
	(void)amaflag;
	(void)endpoint;
	(void)file;
	(void)line;
	(void)function;
	va_start(arguments, name_format);
	(void)vsnprintf(name, sizeof(name), name_format, arguments);
	va_end(arguments);
	return urp_test_channel_alloc(state, name);
}
