/** @file
 * @brief Executable avfilter failures regression and failure-path checks.
 */

#include "../src/txagc/avfilter_processor.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

/** Controls injected graph alloc failure for this test. */
static int fail_graph_alloc;
/** Controls injected create filter failure for this test. */
static int fail_create_filter;
/** Recorded create filter calls for assertions. */
static int create_filter_calls;
/** Controls injected inout alloc failure for this test. */
static int fail_inout_alloc;
/** Recorded inout alloc calls for assertions. */
static int inout_alloc_calls;
/** Controls injected parse failure for this test. */
static int fail_parse;
/** Controls injected graph config failure for this test. */
static int fail_graph_config;
/** Controls injected fifo alloc failure for this test. */
static int fail_fifo_alloc;
/** Controls injected frame alloc failure for this test. */
static int fail_frame_alloc;
/** Recorded frame alloc calls for assertions. */
static int frame_alloc_calls;
/** Controls injected frame buffer failure for this test. */
static int fail_frame_buffer;
/** Controls injected buffer source failure for this test. */
static int fail_buffer_source;
/** Controls injected buffer sink failure for this test. */
static AVFilterContext *fail_buffer_sink;
/** Harness eagain buffer sink used to script and verify host behavior. */
static AVFilterContext *eagain_buffer_sink;
/** Harness eof buffer sink used to script and verify host behavior. */
static AVFilterContext *eof_buffer_sink;
/** Controls injected fifo realloc failure for this test. */
static int fail_fifo_realloc;
/** Controls injected fifo write failure for this test. */
static int fail_fifo_write;

/** @brief Linker entry point for the real avfilter_graph_alloc operation behind the test wrapper.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFilterGraph *__real_avfilter_graph_alloc(void);
/** @brief Test wrapper for avfilter_graph_alloc controlled by the harness's failure-injection
 * state.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFilterGraph *__wrap_avfilter_graph_alloc(void)
{
	return fail_graph_alloc ? NULL : __real_avfilter_graph_alloc();
}

/** @brief Linker entry point for the real avfilter_graph_create_filter operation behind the test
 * wrapper.
 * @param filter_context Receives the created FFmpeg filter context.
 * @param filter FFmpeg dynamics filter name.
 * @param name Option, metadata field, or channel name.
 * @param arguments Formatted-message values or filter options.
 * @param opaque Caller-owned hardware callback context.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_avfilter_graph_create_filter(AVFilterContext **filter_context, const AVFilter *filter,
					const char *name, const char *arguments, void *opaque,
					AVFilterGraph *graph);
/** @brief Test wrapper for avfilter_graph_create_filter controlled by the harness's
 * failure-injection state.
 * @param filter_context Receives the created FFmpeg filter context.
 * @param filter FFmpeg dynamics filter name.
 * @param name Option, metadata field, or channel name.
 * @param arguments Formatted-message values or filter options.
 * @param opaque Caller-owned hardware callback context.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_avfilter_graph_create_filter(AVFilterContext **filter_context, const AVFilter *filter,
					const char *name, const char *arguments, void *opaque,
					AVFilterGraph *graph)
{
	create_filter_calls++;
	if (create_filter_calls == fail_create_filter)
		return AVERROR(ENOMEM);
	return __real_avfilter_graph_create_filter(filter_context, filter, name, arguments, opaque,
						   graph);
}

/** @brief Linker entry point for the real avfilter_inout_alloc operation behind the test wrapper.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFilterInOut *__real_avfilter_inout_alloc(void);
/** @brief Test wrapper for avfilter_inout_alloc controlled by the harness's failure-injection
 * state.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFilterInOut *__wrap_avfilter_inout_alloc(void)
{
	inout_alloc_calls++;
	return inout_alloc_calls == fail_inout_alloc ? NULL : __real_avfilter_inout_alloc();
}

/** @brief Linker entry point for the real avfilter_graph_parse_ptr operation behind the test
 * wrapper.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param filters FFmpeg graph text.
 * @param inputs USB HID input report.
 * @param outputs USB HID output report.
 * @param log_context FFmpeg diagnostic context.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters,
				    AVFilterInOut **inputs, AVFilterInOut **outputs,
				    void *log_context);
/** @brief Test wrapper for avfilter_graph_parse_ptr controlled by the harness's failure-injection
 * state.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param filters FFmpeg graph text.
 * @param inputs USB HID input report.
 * @param outputs USB HID output report.
 * @param log_context FFmpeg diagnostic context.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters,
				    AVFilterInOut **inputs, AVFilterInOut **outputs,
				    void *log_context)
{
	if (fail_parse)
		return AVERROR(EINVAL);
	return __real_avfilter_graph_parse_ptr(graph, filters, inputs, outputs, log_context);
}

/** @brief Linker entry point for the real avfilter_graph_config operation behind the test wrapper.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param log_context FFmpeg diagnostic context.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_avfilter_graph_config(AVFilterGraph *graph, void *log_context);
/** @brief Test wrapper for avfilter_graph_config controlled by the harness's failure-injection
 * state.
 * @param graph NUL-terminated FFmpeg graph description being constructed.
 * @param log_context FFmpeg diagnostic context.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_avfilter_graph_config(AVFilterGraph *graph, void *log_context)
{
	return fail_graph_config ? AVERROR(EINVAL)
				 : __real_avfilter_graph_config(graph, log_context);
}

/** @brief Linker entry point for the real av_audio_fifo_alloc operation behind the test wrapper.
 * @param format printf-style message format.
 * @param channels Number of interleaved audio channels.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVAudioFifo *__real_av_audio_fifo_alloc(enum AVSampleFormat format, int channels, int samples);
/** @brief Test wrapper for av_audio_fifo_alloc controlled by the harness's failure-injection state.
 * @param format printf-style message format.
 * @param channels Number of interleaved audio channels.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVAudioFifo *__wrap_av_audio_fifo_alloc(enum AVSampleFormat format, int channels, int samples)
{
	return fail_fifo_alloc ? NULL : __real_av_audio_fifo_alloc(format, channels, samples);
}

/** @brief Linker entry point for the real av_frame_alloc operation behind the test wrapper.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFrame *__real_av_frame_alloc(void);
/** @brief Test wrapper for av_frame_alloc controlled by the harness's failure-injection state.
 * @return Wrapped API result, including the failure selected by the harness.
 */
AVFrame *__wrap_av_frame_alloc(void)
{
	frame_alloc_calls++;
	return frame_alloc_calls == fail_frame_alloc ? NULL : __real_av_frame_alloc();
}

/** @brief Linker entry point for the real av_frame_get_buffer operation behind the test wrapper.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @param align Requested frame-buffer alignment.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_av_frame_get_buffer(AVFrame *frame, int align);
/** @brief Test wrapper for av_frame_get_buffer controlled by the harness's failure-injection state.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @param align Requested frame-buffer alignment.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_av_frame_get_buffer(AVFrame *frame, int align)
{
	return fail_frame_buffer ? AVERROR(ENOMEM) : __real_av_frame_get_buffer(frame, align);
}

/** @brief Linker entry point for the real av_buffersrc_add_frame_flags operation behind the test
 * wrapper.
 * @param context Asterisk dialplan context or FFmpeg filter context.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @param flags Host API option bit mask.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_av_buffersrc_add_frame_flags(AVFilterContext *context, AVFrame *frame, int flags);
/** @brief Test wrapper for av_buffersrc_add_frame_flags controlled by the harness's
 * failure-injection state.
 * @param context Asterisk dialplan context or FFmpeg filter context.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @param flags Host API option bit mask.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_av_buffersrc_add_frame_flags(AVFilterContext *context, AVFrame *frame, int flags)
{
	return fail_buffer_source ? AVERROR(EIO)
				  : __real_av_buffersrc_add_frame_flags(context, frame, flags);
}

/** @brief Linker entry point for the real av_buffersink_get_frame operation behind the test
 * wrapper.
 * @param context Asterisk dialplan context or FFmpeg filter context.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_av_buffersink_get_frame(AVFilterContext *context, AVFrame *frame);
/** @brief Test wrapper for av_buffersink_get_frame controlled by the harness's failure-injection
 * state.
 * @param context Asterisk dialplan context or FFmpeg filter context.
 * @param frame Asterisk or FFmpeg audio frame, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_av_buffersink_get_frame(AVFilterContext *context, AVFrame *frame)
{
	if (context == fail_buffer_sink)
		return AVERROR(EIO);
	if (context == eagain_buffer_sink)
		return AVERROR(EAGAIN);
	if (context == eof_buffer_sink)
		return AVERROR_EOF;
	return __real_av_buffersink_get_frame(context, frame);
}

/** @brief Linker entry point for the real av_audio_fifo_realloc operation behind the test wrapper.
 * @param fifo Bounded audio FIFO.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_av_audio_fifo_realloc(AVAudioFifo *fifo, int samples);
/** @brief Test wrapper for av_audio_fifo_realloc controlled by the harness's failure-injection
 * state.
 * @param fifo Bounded audio FIFO.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_av_audio_fifo_realloc(AVAudioFifo *fifo, int samples)
{
	return fail_fifo_realloc ? AVERROR(ENOMEM) : __real_av_audio_fifo_realloc(fifo, samples);
}

/** @brief Linker entry point for the real av_audio_fifo_write operation behind the test wrapper.
 * @param fifo Bounded audio FIFO.
 * @param data Input payload or owned state being released, as declared.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __real_av_audio_fifo_write(AVAudioFifo *fifo, void *const *data, int samples);
/** @brief Test wrapper for av_audio_fifo_write controlled by the harness's failure-injection state.
 * @param fifo Bounded audio FIFO.
 * @param data Input payload or owned state being released, as declared.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_av_audio_fifo_write(AVAudioFifo *fifo, void *const *data, int samples)
{
	return fail_fifo_write ? samples - 1 : __real_av_audio_fifo_write(fifo, data, samples);
}

/** @brief Clear failure-injection state before the next independent test. */
static void reset_failures(void)
{
	fail_graph_alloc = 0;
	fail_create_filter = 0;
	create_filter_calls = 0;
	fail_inout_alloc = 0;
	inout_alloc_calls = 0;
	fail_parse = 0;
	fail_graph_config = 0;
	fail_fifo_alloc = 0;
	fail_frame_alloc = 0;
	frame_alloc_calls = 0;
	fail_frame_buffer = 0;
	fail_buffer_source = 0;
	fail_buffer_sink = NULL;
	eagain_buffer_sink = NULL;
	eof_buffer_sink = NULL;
	fail_fifo_realloc = 0;
	fail_fifo_write = 0;
}

/** @brief Create a valid graph configuration for the requested failure-path test.
 * @param cleanup Whether the test configuration enables fixed spectral filtering.
 * @return Configuration initialized for this test scenario.
 */
static struct txagc_config configuration(int cleanup)
{
	struct txagc_config config;
	memset(&config, 0, sizeof(config));
	if (cleanup) {
		config.post_limiter_lowpass_enabled = 1;
		config.post_limiter_lowpass_hz = 5000.0;
	}
	return config;
}

/** @brief Assert that the requested invalid configuration or injected operation fails.
 * @param config Configuration or initialized Asterisk configuration tree, as declared.
 */
static void expect_failure(const struct txagc_config *config)
{
	struct txagc_avfilter state;
	double samples[960] = {0};
	txagc_avfilter_init(&state);
	assert(txagc_avfilter_process(&state, config, samples, 960, 48000) < 0);
	txagc_avfilter_destroy(&state);
}

/** @brief Verify configuration failures. */
static void test_configuration_failures(void)
{
	struct txagc_config base = configuration(0);
	struct txagc_config cleanup = configuration(1);
	int call;

	reset_failures();
	fail_graph_alloc = 1;
	expect_failure(&base);
	for (call = 1; call <= 3; ++call) {
		reset_failures();
		fail_create_filter = call;
		expect_failure(&base);
	}
	for (call = 1; call <= 8; ++call) {
		reset_failures();
		fail_create_filter = call;
		expect_failure(&cleanup);
	}
	for (call = 1; call <= 3; ++call) {
		reset_failures();
		fail_inout_alloc = call;
		expect_failure(&base);
	}
	for (call = 1; call <= 8; ++call) {
		reset_failures();
		fail_inout_alloc = call;
		expect_failure(&cleanup);
	}
	reset_failures();
	fail_parse = 1;
	expect_failure(&base);
	reset_failures();
	fail_graph_config = 1;
	expect_failure(&base);
	reset_failures();
	fail_fifo_alloc = 1;
	expect_failure(&base);
}

/** @brief Verify frame failures. */
static void test_frame_failures(void)
{
	struct txagc_config base = configuration(0);
	int call;
	for (call = 1; call <= 2; ++call) {
		reset_failures();
		fail_frame_alloc = call;
		expect_failure(&base);
	}
	reset_failures();
	fail_frame_buffer = 1;
	expect_failure(&base);
	reset_failures();
	fail_buffer_source = 1;
	expect_failure(&base);
}

/** @brief Exercise a configured graph's runtime sink/FIFO failure path.
 * @param sink FFmpeg filter sink.
 * @param fifo_failure Nonzero selects FIFO failure injection.
 */
static void expect_runtime_failure(AVFilterContext **sink, int fifo_failure)
{
	struct txagc_config config = configuration(sink != NULL);
	struct txagc_avfilter state;
	double samples[960] = {0};
	int attempt;

	reset_failures();
	txagc_avfilter_init(&state);
	assert(!txagc_avfilter_process(&state, &config, samples, 960, 48000));
	if (sink)
		fail_buffer_sink = *sink;
	if (fifo_failure == 1)
		fail_fifo_realloc = 1;
	if (fifo_failure == 2)
		fail_fifo_write = 1;
	for (attempt = 0; attempt < 4; ++attempt) {
		if (txagc_avfilter_process(&state, &config, samples, 960, 48000) < 0)
			break;
	}
	assert(attempt < 4);
	txagc_avfilter_destroy(&state);
}

/** @brief Verify sink and fifo failures. */
static void test_sink_and_fifo_failures(void)
{
	struct txagc_config cleanup = configuration(1);
	struct txagc_avfilter state;
	double samples[960] = {0};

	for (size_t index = 0; index < 7; ++index) {
		/* The helper needs the corresponding sink from its own configured state. */
		reset_failures();
		txagc_avfilter_init(&state);
		assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
		AVFilterContext *own_sinks[] = {
			state.meter_sink,
			state.cleanup_pre_sink,
			state.cleanup_pre_5_8_sink,
			state.cleanup_pre_8_plus_sink,
			state.cleanup_post_5_8_sink,
			state.cleanup_post_8_plus_sink,
			state.sink,
		};
		fail_buffer_sink = own_sinks[index];
		assert(txagc_avfilter_process(&state, &cleanup, samples, 960, 48000) < 0);
		txagc_avfilter_destroy(&state);
	}
	expect_runtime_failure(NULL, 1);
	expect_runtime_failure(NULL, 2);

	reset_failures();
	txagc_avfilter_init(&state);
	cleanup = configuration(0);
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	av_audio_fifo_reset(state.fifo);
	state.output_started = 1;
	eagain_buffer_sink = state.sink;
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	assert(state.runtime_underrun_samples == 960);
	/* A buffering stage can also return nothing before its first output. The
	 * causal AGC no longer produces this case incidentally, so inject it. */
	state.output_started = 0;
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	assert(state.startup_fill_samples == 960);
	assert(!state.output_started);
	txagc_avfilter_destroy(&state);

	reset_failures();
	txagc_avfilter_init(&state);
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	eof_buffer_sink = state.meter_sink;
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	eof_buffer_sink = state.sink;
	assert(!txagc_avfilter_process(&state, &cleanup, samples, 960, 48000));
	txagc_avfilter_destroy(&state);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_configuration_failures();
	test_frame_failures();
	test_sink_and_fifo_failures();
	return 0;
}
