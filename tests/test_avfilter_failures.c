/** @file
 * @brief Executable avfilter failures regression and failure-path checks.
 */

#include "../src/txagc/avfilter_processor.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

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
/** Recorded frame-buffer allocation calls for steady-state assertions. */
static int frame_buffer_calls;
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
/** Recorded FIFO growth calls; prepared processing must leave this at zero. */
static int fifo_realloc_calls;
/** Controls injected fifo write failure for this test. */
static int fail_fifo_write;
/** Controls injected unpublished graph-node allocation failure for this test. */
static int fail_slot_node_alloc;
/** Counts control-plane yield calls made while a slot writer is deliberately held. */
static _Atomic unsigned int scheduler_yield_calls;

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

/** @brief Linker entry point for the real zeroed FFmpeg allocator. */
void *__real_av_mallocz(size_t size);
/** @brief Test wrapper for the unpublished slot-node allocator. */
void *__wrap_av_mallocz(size_t size)
{
	return fail_slot_node_alloc ? NULL : __real_av_mallocz(size);
}

/** @brief Linker entry point for the real scheduler yield operation. */
int __real_sched_yield(void);
/** @brief Count control-plane yields without changing scheduling behavior. */
int __wrap_sched_yield(void)
{
	atomic_fetch_add_explicit(&scheduler_yield_calls, 1U, memory_order_relaxed);
	return __real_sched_yield();
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
	frame_buffer_calls++;
	return frame_buffer_calls == fail_frame_buffer ? AVERROR(ENOMEM)
						       : __real_av_frame_get_buffer(frame, align);
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
	fifo_realloc_calls++;
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
	frame_buffer_calls = 0;
	fail_buffer_source = 0;
	fail_buffer_sink = NULL;
	eagain_buffer_sink = NULL;
	eof_buffer_sink = NULL;
	fail_fifo_realloc = 0;
	fifo_realloc_calls = 0;
	fail_fifo_write = 0;
	fail_slot_node_alloc = 0;
	atomic_store_explicit(&scheduler_yield_calls, 0U, memory_order_relaxed);
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

/** @brief Wait for a control-plane slot operation to reach its yield path. */
static void wait_for_control_plane_yield(void)
{
	for (unsigned int attempt = 0; attempt < 1000000U; ++attempt) {
		if (atomic_load_explicit(&scheduler_yield_calls, memory_order_acquire) != 0U)
			return;
		(void)__real_sched_yield();
	}
	assert(atomic_load_explicit(&scheduler_yield_calls, memory_order_acquire) != 0U);
}

/** @brief Wait until a publication has exchanged the slot's active graph pointer.
 * @param slot Slot being published by a control-plane worker.
 * @param prior Active graph held by the simulated reader.
 */
static void wait_for_active_replacement(const struct txagc_avfilter_slot *slot,
					const struct txagc_avfilter *prior)
{
	for (unsigned int attempt = 0; attempt < 1000000U; ++attempt) {
		if (txagc_avfilter_slot_active(slot) != prior)
			return;
		(void)__real_sched_yield();
	}
	assert(txagc_avfilter_slot_active(slot) != prior);
}

/** Worker context for a control-plane prepare operation. */
struct slot_prepare_thread {
	/** Slot being prepared by the worker. */
	struct txagc_avfilter_slot *slot;
	/** Immutable graph settings selected by the worker. */
	const struct txagc_config *config;
	/** Result returned by txagc_avfilter_slot_prepare(). */
	int result;
};

/** @brief Run one slot prepare operation from a control-plane worker thread.
 * @param opaque Pointer to a slot_prepare_thread context.
 * @return Always NULL as required by pthread_create().
 */
static void *prepare_slot_thread(void *opaque)
{
	struct slot_prepare_thread *thread = opaque;

	thread->result = txagc_avfilter_slot_prepare(thread->slot, thread->config, 48000);
	return NULL;
}

/** Worker context for a publication that waits for a held reader. */
struct slot_publish_thread {
	/** Slot receiving the prepared candidate. */
	struct txagc_avfilter_slot *slot;
	/** Candidate consumed by publication. */
	struct txagc_avfilter_slot_candidate *candidate;
	/** Result returned by txagc_avfilter_slot_publish_candidate(). */
	int result;
	/** Set after publication returns, so the test can prove it was blocked. */
	_Atomic int finished;
};

/** @brief Publish one candidate from a control-plane worker thread.
 * @param opaque Pointer to a slot_publish_thread context.
 * @return Always NULL as required by pthread_create().
 */
static void *publish_slot_thread(void *opaque)
{
	struct slot_publish_thread *thread = opaque;

	thread->result = txagc_avfilter_slot_publish_candidate(thread->slot, thread->candidate);
	atomic_store_explicit(&thread->finished, 1, memory_order_release);
	return NULL;
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
	for (call = 1; call <= TXAGC_AVFILTER_INPUT_FRAME_COUNT + 1; ++call) {
		reset_failures();
		fail_frame_alloc = call;
		expect_failure(&base);
	}
	for (call = 1; call <= TXAGC_AVFILTER_INPUT_FRAME_COUNT; ++call) {
		reset_failures();
		fail_frame_buffer = call;
		expect_failure(&base);
	}
	reset_failures();
	fail_buffer_source = 1;
	expect_failure(&base);
}

/** @brief Exercise a configured graph's runtime sink/FIFO-write failure path.
 * @param sink FFmpeg filter sink.
 * @param fifo_write_failure Nonzero selects FIFO-write failure injection.
 */
static void expect_runtime_failure(AVFilterContext **sink, int fifo_write_failure)
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
	if (fifo_write_failure)
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

/** @brief Verify prepared graph processing has no per-block host allocations or rebuilds. */
static void test_prepared_processing_uses_fixed_storage(void)
{
	struct txagc_config config = configuration(0);
	struct txagc_config unchanged;
	struct txagc_avfilter state;
	double samples[960] = {0};
	void *fifo_data[1];
	AVFilterGraph *graph;
	int frame_allocations;
	int frame_buffers;
	int graph_filters;

	txagc_avfilter_init(&state);
	assert(txagc_avfilter_prepare(NULL, &config, 48000) == AVERROR(EINVAL));
	assert(txagc_avfilter_prepare(&state, NULL, 48000) == AVERROR(EINVAL));
	assert(txagc_avfilter_process_prepared(
		       NULL, samples, sizeof(samples) / sizeof(samples[0])) == AVERROR(EINVAL));
	assert(txagc_avfilter_process_prepared(
		       &state, NULL, sizeof(samples) / sizeof(samples[0])) == AVERROR(EINVAL));
	assert(txagc_avfilter_process_prepared(&state, samples, 0) == AVERROR(EINVAL));
	assert(txagc_avfilter_process_prepared(
		       &state, samples, sizeof(samples) / sizeof(samples[0])) == AVERROR(EINVAL));
	txagc_avfilter_destroy(&state);

	reset_failures();
	txagc_avfilter_init(&state);
	assert(!txagc_avfilter_prepare(&state, &config, 48000));
	assert(state.input_capacity >= sizeof(samples) / sizeof(samples[0]));
	assert(state.fifo_capacity >= state.input_capacity);
	{
		AVAudioFifo *fifo = state.fifo;
		AVFrame *output_frame = state.output_frame;

		state.fifo = NULL;
		assert(txagc_avfilter_process_prepared(&state, samples,
						       sizeof(samples) / sizeof(samples[0])) ==
		       AVERROR(EINVAL));
		state.fifo = fifo;
		state.output_frame = NULL;
		assert(txagc_avfilter_process_prepared(&state, samples,
						       sizeof(samples) / sizeof(samples[0])) ==
		       AVERROR(EINVAL));
		state.output_frame = output_frame;
	}
	graph = state.graph;
	frame_allocations = frame_alloc_calls;
	frame_buffers = frame_buffer_calls;
	graph_filters = create_filter_calls;
	/* The direct control-plane prepare API uses the same semantic comparison as
	 * graph slots.  Representation noise must retain the already-built graph. */
	unchanged = config;
	unchanged.stage_order[0] = TXAGC_STAGE_DEESSER;
	unchanged.ctcss_notch_frequencies[1] = 'x';
	assert(!txagc_avfilter_prepare(&state, &unchanged, 48000));
	assert(state.graph == graph);
	assert(frame_alloc_calls == frame_allocations);
	assert(frame_buffer_calls == frame_buffers);
	assert(create_filter_calls == graph_filters);

	/* A named configuration change, in contrast, constructs a replacement. */
	config.output_gain_db = 1.0;
	assert(!txagc_avfilter_prepare(&state, &config, 48000));
	/* The filter-create counter establishes replacement even if the allocator
	 * reuses the same graph address after freeing the original. */
	assert(create_filter_calls > graph_filters);
	assert(state.config.output_gain_db == config.output_gain_db);
	graph = state.graph;
	assert(graph);
	frame_allocations = frame_alloc_calls;
	frame_buffers = frame_buffer_calls;
	graph_filters = create_filter_calls;
	/* Fail any unexpected per-block host allocation.  The calls below must
	 * continue because prepared processing only uses the fixed state. */
	fail_frame_alloc = frame_allocations + 1;
	fail_frame_buffer = frame_buffers + 1;
	fail_fifo_realloc = 1;
	for (int block = 0; block < 32; ++block)
		assert(!txagc_avfilter_process_prepared(&state, samples,
							sizeof(samples) / sizeof(samples[0])));
	assert(state.graph == graph);
	assert(frame_alloc_calls == frame_allocations);
	assert(frame_buffer_calls == frame_buffers);
	assert(create_filter_calls == graph_filters);
	assert(!fifo_realloc_calls);
	assert(txagc_avfilter_process_prepared(&state, samples, state.input_capacity + 1) < 0);
	/* Hold a reference to every preallocated source frame. The callback must
	 * report bounded-pool exhaustion instead of allocating another frame. */
	{
		AVFrame *held[TXAGC_AVFILTER_INPUT_FRAME_COUNT] = {0};

		fail_frame_alloc = 0;
		fail_frame_buffer = 0;
		for (size_t index = 0; index < sizeof(held) / sizeof(held[0]); ++index) {
			held[index] = av_frame_alloc();
			assert(held[index]);
			assert(!av_frame_ref(held[index], state.input_frames[index]));
		}
		assert(txagc_avfilter_process_prepared(&state, samples,
						       sizeof(samples) / sizeof(samples[0])) ==
		       AVERROR(EAGAIN));
		for (size_t index = 0; index < sizeof(held) / sizeof(held[0]); ++index)
			av_frame_free(&held[index]);
	}
	/* A full fixed FIFO reports exhaustion rather than growing in the callback. */
	fifo_data[0] = state.input_frames[0]->data[0];
	for (unsigned int filled = 0; filled < state.fifo_capacity;) {
		unsigned int chunk = state.fifo_capacity - filled;
		if (chunk > state.input_capacity)
			chunk = state.input_capacity;
		assert(av_audio_fifo_write(state.fifo, fifo_data, (int)chunk) == (int)chunk);
		filled += chunk;
	}
	assert(txagc_avfilter_process_prepared(
		       &state, samples, sizeof(samples) / sizeof(samples[0])) == AVERROR(ENOSPC));
	assert(!fifo_realloc_calls);
	txagc_avfilter_destroy(&state);
}

/** @brief Verify atomic prepared-graph slot lifecycle and failed replacement safety. */
static void test_prepared_slot_lifecycle(void)
{
	struct txagc_avfilter_slot slot;
	struct txagc_config config = configuration(0);
	struct txagc_avfilter *first;
	struct txagc_avfilter *replacement;
	struct txagc_avfilter_slot_candidate candidate = {0};
	struct txagc_config unchanged;
	double samples[960] = {0};
	int graph_filters;

	/* Public null and unprepared paths are deliberately harmless for lifecycle
	 * callers and must not turn a callback failure into a crash. */
	txagc_avfilter_slot_init(NULL);
	txagc_avfilter_slot_destroy(NULL);
	assert(txagc_avfilter_slot_candidate_prepare(NULL, &config, 48000) == AVERROR(EINVAL));
	assert(txagc_avfilter_slot_candidate_prepare(&candidate, NULL, 48000) == AVERROR(EINVAL));
	assert(txagc_avfilter_slot_publish_candidate(NULL, &candidate) == AVERROR(EINVAL));
	txagc_avfilter_slot_candidate_destroy(NULL);
	assert(txagc_avfilter_slot_prepare(NULL, &config, 48000) == AVERROR(EINVAL));
	txagc_avfilter_slot_init(&slot);
	assert(!txagc_avfilter_slot_active(&slot));
	assert(!txagc_avfilter_slot_active(NULL));
	assert(!txagc_avfilter_slot_acquire(NULL));
	assert(!txagc_avfilter_slot_acquire(&slot));
	txagc_avfilter_slot_release(NULL);
	assert(txagc_avfilter_slot_prepare(&slot, NULL, 48000) == AVERROR(EINVAL));
	assert(txagc_avfilter_slot_publish_candidate(&slot, NULL) == AVERROR(EINVAL));
	assert(txagc_avfilter_slot_publish_candidate(&slot, &candidate) == AVERROR(EINVAL));
	txagc_avfilter_slot_candidate_destroy(&candidate);
	assert(txagc_avfilter_slot_process_prepared(
		       &slot, samples, sizeof(samples) / sizeof(samples[0])) == AVERROR(EINVAL));

	reset_failures();
	fail_slot_node_alloc = 1;
	assert(txagc_avfilter_slot_candidate_prepare(&candidate, &config, 48000) ==
	       AVERROR(ENOMEM));
	assert(!candidate.node);
	fail_slot_node_alloc = 0;
	assert(!txagc_avfilter_slot_candidate_prepare(&candidate, &config, 48000));
	assert(txagc_avfilter_slot_candidate_prepare(&candidate, &config, 48000) ==
	       AVERROR(EINVAL));
	txagc_avfilter_slot_candidate_destroy(&candidate);
	assert(!txagc_avfilter_slot_prepare(&slot, &config, 48000));
	first = txagc_avfilter_slot_acquire(&slot);
	assert(first && first == txagc_avfilter_slot_active(&slot));
	/* Acquiring and releasing establishes the exact reader lifetime used by
	 * control-plane replacement; the callback path has no writer lock. */
	assert(atomic_load_explicit(&slot.readers, memory_order_relaxed) == 1U);
	txagc_avfilter_slot_release(&slot);
	assert(atomic_load_explicit(&slot.readers, memory_order_relaxed) == 0U);
	assert(!txagc_avfilter_slot_process_prepared(&slot, samples,
						     sizeof(samples) / sizeof(samples[0])));

	/* Staged candidates do not affect a slot until an explicit publish. This is
	 * the building block used to prepare every active link before a reload
	 * changes any one of their callback graphs. */
	config.output_gain_db = 2.0;
	strcpy(config.ctcss_notch_frequencies, "100.0");
	assert(!txagc_avfilter_slot_candidate_prepare(&candidate, &config, 48000));
	assert(txagc_avfilter_slot_active(&slot) == first);
	assert(!txagc_avfilter_slot_process_prepared(&slot, samples,
						     sizeof(samples) / sizeof(samples[0])));
	assert(!txagc_avfilter_slot_publish_candidate(&slot, &candidate));
	assert(!candidate.node);
	replacement = txagc_avfilter_slot_acquire(&slot);
	assert(replacement && replacement != first && replacement->config.output_gain_db == 2.0);
	txagc_avfilter_slot_release(&slot);
	unchanged = config;

	/* An unchanged graph is a no-op: no new FFmpeg graph is allocated. */
	graph_filters = create_filter_calls;
	assert(!txagc_avfilter_slot_prepare(&slot, &unchanged, 48000));
	assert(txagc_avfilter_slot_active(&slot) == replacement);
	assert(create_filter_calls == graph_filters);

	/* Inactive stage slots and bytes after the CTCSS string terminator are not
	 * graph configuration.  They must not make a semantically unchanged reload
	 * allocate or replace the prepared graph. */
	unchanged.stage_order[0] = TXAGC_STAGE_DEESSER;
	unchanged.ctcss_notch_frequencies[strlen(unchanged.ctcss_notch_frequencies) + 1] = 'x';
	assert(!txagc_avfilter_slot_prepare(&slot, &unchanged, 48000));
	assert(txagc_avfilter_slot_active(&slot) == replacement);
	assert(create_filter_calls == graph_filters);
	assert(!txagc_avfilter_slot_candidate_prepare(&candidate, &unchanged, 48000));
	assert(!txagc_avfilter_slot_publish_candidate(&slot, &candidate));
	assert(!candidate.node);
	assert(txagc_avfilter_slot_active(&slot) == replacement);

	/* A rate change is a real replacement even when every graph option matches. */
	assert(!txagc_avfilter_slot_candidate_prepare(&candidate, &unchanged, 16000));
	assert(!txagc_avfilter_slot_publish_candidate(&slot, &candidate));
	replacement = txagc_avfilter_slot_acquire(&slot);
	assert(replacement && replacement->sample_rate == 16000);
	txagc_avfilter_slot_release(&slot);

	/* A same-rate option change must bypass the no-op fast path. */
	config.output_gain_db = 1.0;
	assert(!txagc_avfilter_slot_prepare(&slot, &config, 16000));
	replacement = txagc_avfilter_slot_acquire(&slot);
	assert(replacement && replacement->sample_rate == 16000 &&
	       replacement->config.output_gain_db == config.output_gain_db);
	txagc_avfilter_slot_release(&slot);

	/* Build failure leaves the published graph untouched and usable. */
	graph_filters = create_filter_calls;
	fail_graph_alloc = 1;
	assert(txagc_avfilter_slot_prepare(&slot, &config, 48000) < 0);
	fail_graph_alloc = 0;
	assert(txagc_avfilter_slot_active(&slot) == replacement);
	assert(!txagc_avfilter_slot_process_prepared(&slot, samples,
						     sizeof(samples) / sizeof(samples[0])));

	assert(!txagc_avfilter_slot_prepare(&slot, &config, 48000));
	replacement = txagc_avfilter_slot_acquire(&slot);
	assert(replacement && replacement->config.output_gain_db == config.output_gain_db);
	assert(create_filter_calls > graph_filters);
	txagc_avfilter_slot_release(&slot);
	txagc_avfilter_slot_destroy(&slot);
	assert(!txagc_avfilter_slot_active(&slot));
	assert(!txagc_avfilter_slot_acquire(&slot));
	/* Teardown is idempotent after a failed or already completed reload. */
	txagc_avfilter_slot_destroy(&slot);
}

/** @brief Verify control-plane slot waits never move into a real-time reader path. */
static void test_slot_control_plane_waits(void)
{
	struct txagc_avfilter_slot slot;
	struct txagc_config config = configuration(0);
	struct txagc_avfilter_slot_candidate candidate = {0};
	struct txagc_avfilter *held;
	struct slot_prepare_thread preparer = {.slot = &slot, .config = &config};
	struct slot_publish_thread publisher = {.slot = &slot, .candidate = &candidate};
	pthread_t thread;

	reset_failures();
	txagc_avfilter_slot_init(&slot);
	assert(!txagc_avfilter_slot_prepare(&slot, &config, 48000));

	/* A competing control-plane prepare yields while the explicit writer token
	 * is held. The callback never observes or takes this token. */
	assert(!atomic_flag_test_and_set_explicit(&slot.writer, memory_order_acquire));
	assert(!pthread_create(&thread, NULL, prepare_slot_thread, &preparer));
	wait_for_control_plane_yield();
	atomic_flag_clear_explicit(&slot.writer, memory_order_release);
	assert(!pthread_join(thread, NULL));
	assert(!preparer.result);

	/* Publish a replacement while a simulated callback holds the original. The
	 * control-plane worker yields until release; the reader itself never waits. */
	held = txagc_avfilter_slot_acquire(&slot);
	assert(held);
	config.output_gain_db = 1.0;
	assert(!txagc_avfilter_slot_candidate_prepare(&candidate, &config, 48000));
	atomic_store_explicit(&scheduler_yield_calls, 0U, memory_order_relaxed);
	assert(!pthread_create(&thread, NULL, publish_slot_thread, &publisher));
	wait_for_active_replacement(&slot, held);
	wait_for_control_plane_yield();
	assert(!atomic_load_explicit(&publisher.finished, memory_order_acquire));
	txagc_avfilter_slot_release(&slot);
	assert(!pthread_join(thread, NULL));
	assert(!publisher.result && !candidate.node);
	txagc_avfilter_slot_destroy(&slot);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_configuration_failures();
	test_frame_failures();
	test_sink_and_fifo_failures();
	test_prepared_processing_uses_fixed_storage();
	test_prepared_slot_lifecycle();
	test_slot_control_plane_waits();
	return 0;
}
