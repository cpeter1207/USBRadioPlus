/** @file
 * @brief Executable rnnoise failures regression and failure-path checks.
 */

#include "../src/txagc/rnnoise_processor_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Harness create call used to script and verify host behavior. */
static int create_call;
/** Controls injected create call failure for this test. */
static int fail_create_call;
/** Harness process call used to script and verify host behavior. */
static int process_call;
/** Controls injected process call failure for this test. */
static int fail_process_call;
/** Harness short input used to script and verify host behavior. */
static int short_input;

/** @brief Test wrapper for rnnoise_create controlled by the harness's failure-injection state.
 * @param model Model supplied by the test scenario.
 * @return Wrapped API result, including the failure selected by the harness.
 */
DenoiseState *__wrap_rnnoise_create(RNNModel *model)
{
	(void)model;
	create_call++;
	return create_call == fail_create_call ? NULL : (DenoiseState *)(uintptr_t)1;
}

/** @brief Test wrapper for rnnoise_destroy controlled by the harness's failure-injection state.
 * @param state Processor or stream state owned by the caller.
 */
void __wrap_rnnoise_destroy(DenoiseState *state)
{
	(void)state;
}

/** @brief Test wrapper for rnnoise_process_frame controlled by the harness's failure-injection
 * state.
 * @param state Processor or stream state owned by the caller.
 * @param output Destination sample buffer owned by the caller.
 * @param input Input samples; the caller retains ownership.
 * @return Wrapped API result, including the failure selected by the harness.
 */
float __wrap_rnnoise_process_frame(DenoiseState *state, float *output, const float *input)
{
	(void)state;
	memcpy(output, input, TXAGC_RNNOISE_FRAME * sizeof(*output));
	return 0.5F;
}

/** @brief Test wrapper for src_new controlled by the harness's failure-injection state.
 * @param converter_type libsamplerate converter type.
 * @param channels Number of interleaved audio channels.
 * @param error Receives a diagnostic for invalid input.
 * @return Wrapped API result, including the failure selected by the harness.
 */
SRC_STATE *__wrap_src_new(int converter_type, int channels, int *error)
{
	(void)converter_type;
	(void)channels;
	create_call++;
	*error = 0;
	return create_call == fail_create_call ? NULL : (SRC_STATE *)(uintptr_t)1;
}

/** @brief Test wrapper for src_delete controlled by the harness's failure-injection state.
 * @param state Processor or stream state owned by the caller.
 * @return Wrapped API result, including the failure selected by the harness.
 */
SRC_STATE *__wrap_src_delete(SRC_STATE *state)
{
	return state;
}

/** @brief Test wrapper for src_reset controlled by the harness's failure-injection state.
 * @param state Processor or stream state owned by the caller.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_src_reset(SRC_STATE *state)
{
	(void)state;
	return 0;
}

/** @brief Test wrapper for src_process controlled by the harness's failure-injection state.
 * @param state Processor or stream state owned by the caller.
 * @param data Input payload or owned state being released, as declared.
 * @return Wrapped API result, including the failure selected by the harness.
 */
int __wrap_src_process(SRC_STATE *state, SRC_DATA *data)
{
	long generated;
	(void)state;
	process_call++;
	if (process_call == fail_process_call)
		return 1;
	generated = data->input_frames;
	if (generated > data->output_frames)
		generated = data->output_frames;
	memcpy(data->data_out, data->data_in, (size_t)generated * sizeof(*data->data_out));
	data->input_frames_used = short_input ? data->input_frames - 1 : data->input_frames;
	data->output_frames_gen = generated;
	return 0;
}

/** @brief Clear failure-injection state before the next independent test. */
static void reset_failures(void)
{
	create_call = 0;
	fail_create_call = 0;
	process_call = 0;
	fail_process_call = 0;
	short_input = 0;
}

/** @brief Populate a complete, non-owning prepared state for guard-path tests.
 * @param state State whose mandatory prepared members will be supplied.
 *
 * The wrapped library entry points make these sentinel handles safe for tests
 * that intentionally stop at a public precondition.  They are never passed to
 * an unwrapped library routine.
 */
static void set_guard_ready_state(struct txagc_rnnoise *state)
{
	memset(state, 0, sizeof(*state));
	state->prepared = 1;
	state->input_rate = 48000;
	state->denoise = (DenoiseState *)(uintptr_t)1;
	state->upsampler = (SRC_STATE *)(uintptr_t)1;
	state->downsampler = (SRC_STATE *)(uintptr_t)1;
}

/** @brief Verify configuration failures. */
static void test_configuration_failures(void)
{
	struct txagc_rnnoise state;
	double samples[480] = {0};
	int failure;

	for (failure = 1; failure <= 3; ++failure) {
		reset_failures();
		fail_create_call = failure;
		txagc_rnnoise_init(&state);
		assert(txagc_rnnoise_process_double(&state, samples, 480, 48000) < 0);
		assert(state.errors == 1);
		txagc_rnnoise_destroy(&state);
	}

	reset_failures();
	memset(&state, 0, sizeof(state));
	state.active = 1;
	state.input_rate = 48000;
	fail_create_call = 1;
	assert(configure_rate(&state, 48000) < 0);
	reset_failures();
	memset(&state, 0, sizeof(state));
	state.active = 1;
	state.input_rate = 48000;
	state.denoise = (DenoiseState *)(uintptr_t)1;
	fail_create_call = 1;
	assert(configure_rate(&state, 48000) < 0);
	reset_failures();
	memset(&state, 0, sizeof(state));
	state.active = 1;
	state.input_rate = 48000;
	state.denoise = (DenoiseState *)(uintptr_t)1;
	state.upsampler = (SRC_STATE *)(uintptr_t)1;
	fail_create_call = 1;
	assert(configure_rate(&state, 48000) < 0);
}

/** @brief Exercise every mandatory prepared-state component before reconfiguration.
 *
 * A partial prepared state can result from a failed control-plane allocation.
 * Verify that each incomplete combination is rebuilt rather than accepted as
 * ready, while a fully populated state at a different rate is also rebuilt.
 */
static void test_prepared_state_reconfiguration_guards(void)
{
	struct txagc_rnnoise state;

	reset_failures();
	set_guard_ready_state(&state);
	state.input_rate = 8000;
	assert(!configure_rate(&state, 48000));
	txagc_rnnoise_destroy(&state);

	reset_failures();
	set_guard_ready_state(&state);
	state.denoise = NULL;
	assert(!configure_rate(&state, 48000));
	txagc_rnnoise_destroy(&state);

	reset_failures();
	set_guard_ready_state(&state);
	state.upsampler = NULL;
	assert(!configure_rate(&state, 48000));
	txagc_rnnoise_destroy(&state);

	reset_failures();
	set_guard_ready_state(&state);
	state.downsampler = NULL;
	assert(!configure_rate(&state, 48000));
	txagc_rnnoise_destroy(&state);
}

/** @brief Verify processing failures and fifo helpers. */
static void test_processing_failures_and_fifo_helpers(void)
{
	struct txagc_rnnoise state;
	double samples[960] = {0};
	int16_t pcm[1] = {0};
	float fifo[4] = {1, 2, 3, 4};
	size_t count = 4;

	reset_failures();
	txagc_rnnoise_init(&state);
	fail_process_call = 1;
	assert(txagc_rnnoise_process_double(&state, samples, 960, 48000) < 0);
	assert(state.errors == 1 && !state.active);
	reset_failures();
	short_input = 1;
	assert(txagc_rnnoise_process_double(&state, samples, 960, 48000) < 0);
	reset_failures();
	txagc_rnnoise_init(&state);
	assert(!configure_rate(&state, 48000));
	state.up_count = TXAGC_RNNOISE_FIFO;
	assert(txagc_rnnoise_process_double(&state, samples, 1, 48000) < 0);

	reset_failures();
	assert(!txagc_rnnoise_process_double(&state, samples, 960, 48000));
	fail_process_call = process_call + 1;
	assert(txagc_rnnoise_process_double(&state, samples, 960, 48000) < 0);

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));
	state.primed = 1;
	state.up_count = TXAGC_RNNOISE_FRAME;
	state.down_count = TXAGC_RNNOISE_FIFO;
	assert(txagc_rnnoise_process_double(&state, samples, 0, 48000) < 0);

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(!configure_rate(&state, 48000));
	state.primed = 1;
	state.up_count = TXAGC_RNNOISE_FRAME;
	fail_process_call = 2;
	assert(txagc_rnnoise_process_double(&state, samples, 0, 48000) < 0);

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(!configure_rate(&state, 48000));
	state.down_fifo[0] = 1.0F;
	state.down_count = 1;
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));

	reset_failures();
	txagc_rnnoise_init(&state);
	fail_process_call = 1;
	assert(txagc_rnnoise_process(&state, pcm, 1, 48000) < 0);

	assert(append(fifo, &count, fifo, TXAGC_RNNOISE_FIFO) < 0);
	consume(fifo, &count, 2);
	assert(count == 2 && fifo[0] == 3);
	assert(pcm_from_double(40000.0) == 32767);
	assert(pcm_from_double(-40000.0) == -32768);
	assert(pcm_from_double(1.6) == 2);
	memset(&state, 0, sizeof(state));
	reset_stream(&state);
	txagc_rnnoise_destroy(&state);
}

/** @brief Verify the native prepared path never creates or replaces RNNoise state. */
static void test_prepared_callback_has_no_lifecycle_work(void)
{
	struct txagc_rnnoise state;
	double samples[960] = {0};
	int prepared_creates;

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(NULL, 48000) < 0);
	assert(txagc_rnnoise_prepare(&state, 0) < 0);
	assert(!txagc_rnnoise_prepare(&state, 48000));
	prepared_creates = create_call;
	assert(prepared_creates == 3);
	/* A prepared stream at the same rate must retain its converters rather than
	 * rebuild them in the audio-control path. */
	assert(!txagc_rnnoise_prepare(&state, 48000));
	assert(create_call == prepared_creates);
	assert(txagc_rnnoise_process_prepared(NULL, samples, sizeof(samples) / sizeof(samples[0])) <
	       0);
	assert(txagc_rnnoise_process_prepared(&state, NULL, sizeof(samples) / sizeof(samples[0])) <
	       0);
	assert(!txagc_rnnoise_process_prepared(&state, samples,
					       sizeof(samples) / sizeof(samples[0])));
	assert(create_call == prepared_creates);
	txagc_rnnoise_bypass(&state);
	assert(state.prepared && !state.active && create_call == prepared_creates);
	fail_process_call = process_call + 1;
	assert(txagc_rnnoise_process_prepared(&state, samples,
					      sizeof(samples) / sizeof(samples[0])) < 0);
	assert(state.prepared && !state.active && create_call == prepared_creates);
	txagc_rnnoise_destroy(&state);
}

/** @brief Exercise each short-circuit guard in both floating-point entry points. */
static void test_public_processing_guards(void)
{
	struct txagc_rnnoise state;
	double samples[2049] = {0};

	assert(txagc_rnnoise_process_double(NULL, samples, 1, 48000) < 0);
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_process_double(&state, NULL, 1, 48000) < 0);
	assert(txagc_rnnoise_process_double(&state, samples, 1, 0) < 0);
	assert(txagc_rnnoise_process_double(&state, samples, 2049, 48000) < 0);

	assert(txagc_rnnoise_process_prepared(NULL, samples, 1) < 0);
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_process_prepared(&state, NULL, 1) < 0);
	assert(txagc_rnnoise_process_prepared(&state, samples, 1) < 0);

	set_guard_ready_state(&state);
	state.input_rate = 0;
	assert(txagc_rnnoise_process_prepared(&state, samples, 1) < 0);
	set_guard_ready_state(&state);
	state.denoise = NULL;
	assert(txagc_rnnoise_process_prepared(&state, samples, 1) < 0);
	set_guard_ready_state(&state);
	state.upsampler = NULL;
	assert(txagc_rnnoise_process_prepared(&state, samples, 1) < 0);
	set_guard_ready_state(&state);
	state.downsampler = NULL;
	assert(txagc_rnnoise_process_prepared(&state, samples, 1) < 0);
	set_guard_ready_state(&state);
	assert(txagc_rnnoise_process_prepared(&state, samples, 2049) < 0);

	/* Each incomplete prepared state must force setup rather than entering the
	 * sample callback with a missing converter or denoiser. */
	reset_failures();
	set_guard_ready_state(&state);
	state.input_rate = 8000;
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));
	txagc_rnnoise_destroy(&state);
	reset_failures();
	set_guard_ready_state(&state);
	state.denoise = NULL;
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));
	txagc_rnnoise_destroy(&state);
	reset_failures();
	set_guard_ready_state(&state);
	state.upsampler = NULL;
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));
	txagc_rnnoise_destroy(&state);
	reset_failures();
	set_guard_ready_state(&state);
	state.downsampler = NULL;
	assert(!txagc_rnnoise_process_double(&state, samples, 0, 48000));
	txagc_rnnoise_destroy(&state);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_configuration_failures();
	test_prepared_state_reconfiguration_guards();
	test_processing_failures_and_fifo_helpers();
	test_prepared_callback_has_no_lifecycle_work();
	test_public_processing_guards();
	puts("RNNoise failure-path tests passed");
	return 0;
}
