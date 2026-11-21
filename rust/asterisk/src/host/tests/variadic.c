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

#include "../../../../product/include/usbradioplus_product.h"
#include <rptadvradio/rptadvradio.h>

/* The same public typed radio template which a controller supplies to the product. */
int urp_test_physical_station(const UrpAstDescriptor *product, uint32_t vad_mode, uint32_t inverted,
			      uint32_t fixture_kind, void **prepared)
{
	Dl_info origin;
	void *library;
	const UrpPreparationDescriptor *(*getter)(void);
	const struct rptadv_radio_session_config radio = {
		.struct_size = sizeof(radio),
		.abi_version = RPTADV_RADIO_ABI_VERSION,
		.generation_id = 1,
		.native_sample_rate_hz = 48000,
		.interleaved_channels = 2,
		.maximum_receive_frame_count = 960,
		.maximum_transmit_frame_count = 960,
		.publication_interval_ms = 50,
		.receive_input_gain = 1.0f,
		.receive = {.ctcss_decoder_gain = 1.0f,
			    .vox_threshold = 1000,
			    .vox_hang_ms = 500,
			    .vad_external_delay = 1,
			    .vad_mode = vad_mode},
		.qualification = {.advanced_transport = 1,
				  .radio_duplex = 1,
				  .carrier_source = fixture_kind == 2 ? 5 : 4},
		.transmit = {.output_a_route = 1,
			     .output_a_tone_gain = 1.0f,
			     .output_b_tone_gain = 1.0f},
	};
	const UrpNativeStationConfig station = {
		.struct_size = sizeof(station),
		.abi_version = URP_NATIVE_CONFIG_ABI_VERSION,
		.radio = &radio,
		.device_selection = fixture_kind == 2 ? 0 : 1,
		.device_identifier = fixture_kind == 2 ? (const uint8_t *)"hw:3,0" : NULL,
		.device_identifier_length = fixture_kind == 2 ? 6 : 0,
		.hardware_gain_mask = fixture_kind == 1 ? URP_NATIVE_HARDWARE_GAIN_OUTPUT_B : 0,
		.hardware_output_b_gain_db = fixture_kind == 1 ? 3.0f : 0.0f,
		.ptt_inverted = inverted,
		.input_device_channels = 2,
		.output_device_channels = 2,
		.receive_output_gain_db = -6,
		.transmit_preemphasis_corner_hz = 300.0f,
		.receive_deemphasis_corner_hz = 300.0f,
	};
	static const uint8_t settings[] =
		"{\"physical\":{\"ordinary\":{\"audio_source\":\"cm119\",\"ptt_source\":\"usb\"},"
		"\"parallel_carrier_inverted\":false,\"parallel_ctcss_inverted\":false}}";
	static const uint8_t raw_settings[] =
		"{\"physical\":{\"ordinary\":{\"audio_source\":\"alsa\",\"ptt_source\":"
		"\"parallel\"},"
		"\"parallel_carrier_inverted\":false,\"parallel_ctcss_inverted\":false},"
		"\"station\":{\"parallel_port\":{\"output_assignments\":[\"push_to_talk\",null,"
		"null,null,null,null,null,null]}}}";
	const UrpTypedPreparation args = {
		sizeof(args), 1, &station, fixture_kind == 2 ? raw_settings : settings,
		fixture_kind == 2 ? sizeof(raw_settings) - 1 : sizeof(settings) - 1};
	int result = -1;

	*prepared = NULL;
	if (!dladdr(product, &origin) ||
	    !(library = dlopen(origin.dli_fname, RTLD_NOW | RTLD_NOLOAD)))
		return -1;
	getter = dlsym(library, "usbradioplus_physical_preparation_descriptor");
	if (getter)
		result = getter()->prepare(&args, NULL, NULL, prepared);
	dlclose(library);
	return result;
}
