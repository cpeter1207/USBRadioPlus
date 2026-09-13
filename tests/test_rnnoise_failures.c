/** @file
 * @brief Executable fixed-rate RNNoise setup and guard-path checks.
 */

#include "../src/txagc/rnnoise_processor.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** Harness create call used to script and verify RNNoise setup behavior. */
static int create_call;
/** Controls injected RNNoise-create failure for this test. */
static int fail_create_call;
/** Counts processed inference frames for lifecycle assertions. */
static int process_call;

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

/** @brief Test wrapper for rnnoise_destroy controlled by the harness's fake state.
 * @param state Processor or stream state owned by the caller.
 */
void __wrap_rnnoise_destroy(DenoiseState *state)
{
	(void)state;
}

/** @brief Test wrapper for rnnoise_process_frame with transparent PCM-code output.
 * @param state Processor or stream state owned by the caller.
 * @param output Destination sample buffer owned by the caller.
 * @param input Input samples; the caller retains ownership.
 * @return Deterministic speech probability for the test stream.
 */
float __wrap_rnnoise_process_frame(DenoiseState *state, float *output, const float *input)
{
	(void)state;
	process_call++;
	memcpy(output, input, TXAGC_RNNOISE_FRAME * sizeof(*output));
	return 0.5F;
}

/** @brief Clear failure-injection state before the next independent test. */
static void reset_failures(void)
{
	create_call = 0;
	fail_create_call = 0;
	process_call = 0;
}

/** @brief Populate a complete non-owning prepared state for guard-path tests.
 * @param state State whose mandatory prepared members will be supplied.
 */
static void set_guard_ready_state(struct txagc_rnnoise *state)
{
	memset(state, 0, sizeof(*state));
	state->prepared = 1;
	state->denoise = (DenoiseState *)(uintptr_t)1;
}

/** @brief Verify fixed-rate setup failure and invalid-rate rejection. */
static void test_configuration_failures(void)
{
	struct txagc_rnnoise state;

	reset_failures();
	fail_create_call = 1;
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE) < 0);
	assert(state.errors == 1U && create_call == 1);
	txagc_rnnoise_destroy(&state);

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(&state, 8000U) < 0);
	assert(!state.prepared && !state.denoise && !state.errors && !create_call);
	txagc_rnnoise_destroy(&state);
}

/** @brief Verify incomplete prepared state is rebuilt only at the control boundary. */
static void test_prepared_state_reconfiguration_guards(void)
{
	struct txagc_rnnoise state;

	reset_failures();
	set_guard_ready_state(&state);
	state.prepared = 0;
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	assert(create_call == 1);
	txagc_rnnoise_destroy(&state);

	reset_failures();
	set_guard_ready_state(&state);
	state.denoise = NULL;
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	assert(create_call == 1);
	txagc_rnnoise_destroy(&state);

	reset_failures();
	set_guard_ready_state(&state);
	assert(txagc_rnnoise_prepare(&state, 8000U) < 0);
	assert(state.prepared && state.denoise);
	txagc_rnnoise_destroy(&state);
}

/** @brief Verify the prepared callback never creates or replaces RNNoise state. */
static void test_prepared_callback_has_no_lifecycle_work(void)
{
	struct txagc_rnnoise state;
	double samples[960] = {0};
	int prepared_creates;

	reset_failures();
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(NULL, TXAGC_RNNOISE_RATE) < 0);
	assert(txagc_rnnoise_prepare(&state, 0U) < 0);
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	prepared_creates = create_call;
	assert(prepared_creates == 1 && process_call == 2);
	assert(!state.active && !state.primed && !state.input_count && !state.output_count &&
	       !state.rnnoise_frames && !state.output_samples && !state.startup_samples);
	for (size_t index = 0; index < TXAGC_RNNOISE_FRAME; ++index)
		assert(state.input_frame[index] == 0.0F && state.output_frame[index] == 0.0F);
	assert(!txagc_rnnoise_prepare(&state, TXAGC_RNNOISE_RATE));
	assert(create_call == prepared_creates && process_call == 2);
	assert(txagc_rnnoise_process_prepared(NULL, samples, sizeof(samples) / sizeof(samples[0])) <
	       0);
	assert(txagc_rnnoise_process_prepared(&state, NULL, sizeof(samples) / sizeof(samples[0])) <
	       0);
	assert(!txagc_rnnoise_process_prepared(&state, samples,
					       sizeof(samples) / sizeof(samples[0])));
	assert(create_call == prepared_creates && process_call == 4);
	txagc_rnnoise_bypass(&state);
	assert(state.prepared && !state.active && !state.input_count && !state.output_count &&
	       create_call == prepared_creates);
	txagc_rnnoise_destroy(&state);
}

/** @brief Exercise each short-circuit guard in the prepared entry point. */
static void test_prepared_processing_guards(void)
{
	struct txagc_rnnoise state;
	double variable_span[2049] = {0};

	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_prepare(NULL, TXAGC_RNNOISE_RATE) < 0);
	assert(txagc_rnnoise_prepare(&state, 0U) < 0);
	assert(txagc_rnnoise_process_prepared(NULL, variable_span, 1U) < 0);
	txagc_rnnoise_init(&state);
	assert(txagc_rnnoise_process_prepared(&state, NULL, 1U) < 0);
	assert(txagc_rnnoise_process_prepared(&state, variable_span, 1U) < 0);

	set_guard_ready_state(&state);
	state.denoise = NULL;
	assert(txagc_rnnoise_process_prepared(&state, variable_span, 1U) < 0);
	set_guard_ready_state(&state);
	assert(!txagc_rnnoise_process_prepared(&state, variable_span,
					       sizeof(variable_span) / sizeof(variable_span[0])));
	txagc_rnnoise_destroy(&state);
}

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_configuration_failures();
	test_prepared_state_reconfiguration_guards();
	test_prepared_callback_has_no_lifecycle_work();
	test_prepared_processing_guards();
	puts("RNNoise failure-path tests passed");
	return 0;
}
