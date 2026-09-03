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

static int fail_graph_alloc;
static int fail_create_filter;
static int create_filter_calls;
static int fail_inout_alloc;
static int inout_alloc_calls;
static int fail_parse;
static int fail_graph_config;
static int fail_fifo_alloc;
static int fail_frame_alloc;
static int frame_alloc_calls;
static int fail_frame_buffer;
static int fail_buffer_source;
static AVFilterContext *fail_buffer_sink;
static AVFilterContext *eagain_buffer_sink;
static AVFilterContext *eof_buffer_sink;
static int fail_fifo_realloc;
static int fail_fifo_write;

AVFilterGraph *__real_avfilter_graph_alloc(void);
AVFilterGraph *__wrap_avfilter_graph_alloc(void)
{
	return fail_graph_alloc ? NULL : __real_avfilter_graph_alloc();
}

int __real_avfilter_graph_create_filter(AVFilterContext **filter_context, const AVFilter *filter,
					const char *name, const char *arguments, void *opaque,
					AVFilterGraph *graph);
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

AVFilterInOut *__real_avfilter_inout_alloc(void);
AVFilterInOut *__wrap_avfilter_inout_alloc(void)
{
	inout_alloc_calls++;
	return inout_alloc_calls == fail_inout_alloc ? NULL : __real_avfilter_inout_alloc();
}

int __real_avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters,
				    AVFilterInOut **inputs, AVFilterInOut **outputs,
				    void *log_context);
int __wrap_avfilter_graph_parse_ptr(AVFilterGraph *graph, const char *filters,
				    AVFilterInOut **inputs, AVFilterInOut **outputs,
				    void *log_context)
{
	if (fail_parse)
		return AVERROR(EINVAL);
	return __real_avfilter_graph_parse_ptr(graph, filters, inputs, outputs, log_context);
}

int __real_avfilter_graph_config(AVFilterGraph *graph, void *log_context);
int __wrap_avfilter_graph_config(AVFilterGraph *graph, void *log_context)
{
	return fail_graph_config ? AVERROR(EINVAL)
				 : __real_avfilter_graph_config(graph, log_context);
}

AVAudioFifo *__real_av_audio_fifo_alloc(enum AVSampleFormat format, int channels, int samples);
AVAudioFifo *__wrap_av_audio_fifo_alloc(enum AVSampleFormat format, int channels, int samples)
{
	return fail_fifo_alloc ? NULL : __real_av_audio_fifo_alloc(format, channels, samples);
}

AVFrame *__real_av_frame_alloc(void);
AVFrame *__wrap_av_frame_alloc(void)
{
	frame_alloc_calls++;
	return frame_alloc_calls == fail_frame_alloc ? NULL : __real_av_frame_alloc();
}

int __real_av_frame_get_buffer(AVFrame *frame, int align);
int __wrap_av_frame_get_buffer(AVFrame *frame, int align)
{
	return fail_frame_buffer ? AVERROR(ENOMEM) : __real_av_frame_get_buffer(frame, align);
}

int __real_av_buffersrc_add_frame_flags(AVFilterContext *context, AVFrame *frame, int flags);
int __wrap_av_buffersrc_add_frame_flags(AVFilterContext *context, AVFrame *frame, int flags)
{
	return fail_buffer_source ? AVERROR(EIO)
				  : __real_av_buffersrc_add_frame_flags(context, frame, flags);
}

int __real_av_buffersink_get_frame(AVFilterContext *context, AVFrame *frame);
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

int __real_av_audio_fifo_realloc(AVAudioFifo *fifo, int samples);
int __wrap_av_audio_fifo_realloc(AVAudioFifo *fifo, int samples)
{
	return fail_fifo_realloc ? AVERROR(ENOMEM) : __real_av_audio_fifo_realloc(fifo, samples);
}

int __real_av_audio_fifo_write(AVAudioFifo *fifo, void *const *data, int samples);
int __wrap_av_audio_fifo_write(AVAudioFifo *fifo, void *const *data, int samples)
{
	return fail_fifo_write ? samples - 1 : __real_av_audio_fifo_write(fifo, data, samples);
}

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

static void expect_failure(const struct txagc_config *config)
{
	struct txagc_avfilter state;
	double samples[960] = {0};
	txagc_avfilter_init(&state);
	assert(txagc_avfilter_process(&state, config, samples, 960, 48000) < 0);
	txagc_avfilter_destroy(&state);
}

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

int main(void)
{
	test_configuration_failures();
	test_frame_failures();
	test_sink_and_fifo_failures();
	return 0;
}
