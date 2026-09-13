/**
 * @file test_receive_frontend_adapter.c
 * @brief Verify the transactional signed-16 bridge to the Rust RX frontend.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"

/** @brief Maximum native samples used by this bounded bridge regression. */
#define TEST_NATIVE_FRAMES 6U
/** @brief Fixed signed-16 frontend history/table size. */
#define TEST_HISTORY_COUNT 4U

/** @brief Construct preallocated compatibility storage for one bridge call. */
static struct urp_radio_receive_frontend_workspace
frontend_workspace(float *input, float *baseband_output, int16_t *history, uint8_t *carrier_gate)
{
	return (struct urp_radio_receive_frontend_workspace){
		.input = input,
		.baseband_output = baseband_output,
		.history = history,
		.carrier_gate = carrier_gate,
		.native_frame_capacity = TEST_NATIVE_FRAMES,
		.baseband_output_capacity = TEST_NATIVE_FRAMES,
		.history_capacity = TEST_HISTORY_COUNT,
	};
}

/** @brief Assert that two portable frontend states have equal visible state. */
static void assert_states_equal(const struct rptadv_radio_receive_frontend_state *left,
				const struct rptadv_radio_receive_frontend_state *right)
{
	assert(left->decimator == right->decimator);
	assert(left->comparator_output == right->comparator_output);
	assert(left->rssi_peak == right->rssi_peak);
	assert(left->rssi_power == right->rssi_power);
	assert(left->rssi_samples == right->rssi_samples);
	assert(left->micor_squelch.noise_power == right->micor_squelch.noise_power);
	assert(left->micor_squelch.idle_power == right->micor_squelch.idle_power);
	assert(left->micor_squelch.hold_charge == right->micor_squelch.hold_charge);
	assert(left->micor_squelch.settling_samples == right->micor_squelch.settling_samples);
}

/** @brief Exercise one valid bridge call with a caller-selected native span. */
static void run_frontend(const int16_t *input, size_t native_frame_count, int16_t *output,
			 size_t output_capacity, uint8_t *gate, int16_t *history,
			 struct rptadv_radio_receive_frontend_state *state,
			 struct urp_radio_receive_frontend_workspace *workspace,
			 size_t *output_count, int *rssi_updated)
{
	static const int16_t baseband_coefficients[TEST_HISTORY_COUNT] = {1, 2, -3, 4};
	static const int16_t noise_coefficients[2] = {2, -1};

	assert(!urp_radio_core_receive_frontend_s16(
		input, output, output_capacity, gate, native_frame_count, history,
		TEST_HISTORY_COUNT, baseband_coefficients, 1, 256, noise_coefficients,
		sizeof(noise_coefficients) / sizeof(noise_coefficients[0]), 1, 3U, 6U, 1000U, 100U,
		state, output_count, rssi_updated, workspace));
}

/** @brief Verify FFI bridge output and state are independent of callback partitioning. */
static void test_partition_parity(void)
{
	const int16_t input[TEST_NATIVE_FRAMES * 2U] = {
		100, 3000, -200, 2000, 300, 1000, -400, 0, 500, -1000, -600, -2000,
	};
	float whole_input[TEST_NATIVE_FRAMES * 2U];
	float whole_output_f32[TEST_NATIVE_FRAMES];
	int16_t whole_history[TEST_HISTORY_COUNT] = {0x1234, -0x2345, 0x3456, -0x4567};
	uint8_t whole_gate[TEST_NATIVE_FRAMES] = {0};
	int16_t whole_output[TEST_NATIVE_FRAMES] = {0};
	struct rptadv_radio_receive_frontend_state whole_state = {
		.decimator = 3,
		.comparator_output = 1,
	};
	struct urp_radio_receive_frontend_workspace whole_workspace =
		frontend_workspace(whole_input, whole_output_f32, whole_history, whole_gate);
	size_t whole_output_count = 0U;
	int whole_rssi_updated = 0;
	float split_input[TEST_NATIVE_FRAMES * 2U];
	float split_output_f32[TEST_NATIVE_FRAMES];
	int16_t split_history[TEST_HISTORY_COUNT] = {0x1234, -0x2345, 0x3456, -0x4567};
	uint8_t split_gate[TEST_NATIVE_FRAMES] = {0};
	int16_t split_output[TEST_NATIVE_FRAMES] = {0};
	struct rptadv_radio_receive_frontend_state split_state = whole_state;
	struct urp_radio_receive_frontend_workspace split_workspace =
		frontend_workspace(split_input, split_output_f32, split_history, split_gate);
	size_t first_output_count = 0U;
	size_t second_output_count = 0U;
	int first_rssi_updated = 0;
	int second_rssi_updated = 0;

	run_frontend(input, TEST_NATIVE_FRAMES, whole_output, TEST_NATIVE_FRAMES, whole_gate,
		     whole_history, &whole_state, &whole_workspace, &whole_output_count,
		     &whole_rssi_updated);
	run_frontend(input, 2U, split_output, TEST_NATIVE_FRAMES, split_gate, split_history,
		     &split_state, &split_workspace, &first_output_count, &first_rssi_updated);
	run_frontend(input + 2U * 2U, TEST_NATIVE_FRAMES - 2U, split_output + first_output_count,
		     TEST_NATIVE_FRAMES - first_output_count, split_gate + 2U, split_history,
		     &split_state, &split_workspace, &second_output_count, &second_rssi_updated);
	assert(whole_output_count == first_output_count + second_output_count);
	assert(!memcmp(whole_output, split_output, whole_output_count * sizeof(*whole_output)));
	assert(!memcmp(whole_gate, split_gate, sizeof(whole_gate)));
	assert(!memcmp(whole_history, split_history, sizeof(whole_history)));
	assert_states_equal(&whole_state, &split_state);
	assert(whole_rssi_updated == first_rssi_updated + second_rssi_updated);
}

/** @brief Verify rejected calls do not publish partial PCM, history, gates, or state. */
static void test_rejected_call_is_transactional(void)
{
	int16_t output[TEST_NATIVE_FRAMES] = {1, 2, 3, 4, 5, 6};
	uint8_t gate[TEST_NATIVE_FRAMES] = {7, 7, 7, 7, 7, 7};
	int16_t history[TEST_HISTORY_COUNT] = {11, 22, 33, 44};
	const int16_t expected_output[TEST_NATIVE_FRAMES] = {1, 2, 3, 4, 5, 6};
	const uint8_t expected_gate[TEST_NATIVE_FRAMES] = {7, 7, 7, 7, 7, 7};
	const int16_t expected_history[TEST_HISTORY_COUNT] = {11, 22, 33, 44};
	float workspace_input[TEST_NATIVE_FRAMES * 2U];
	float workspace_output[TEST_NATIVE_FRAMES];
	int16_t workspace_history[TEST_HISTORY_COUNT];
	uint8_t workspace_gate[TEST_NATIVE_FRAMES];
	struct rptadv_radio_receive_frontend_state state = {
		.decimator = 2,
		.comparator_output = 1,
		.rssi_power = 88,
		.rssi_samples = 3U,
	};
	const struct rptadv_radio_receive_frontend_state expected_state = state;
	struct urp_radio_receive_frontend_workspace workspace = frontend_workspace(
		workspace_input, workspace_output, workspace_history, workspace_gate);
	size_t output_count = 99U;
	int rssi_updated = 99;
	static const int16_t coefficients[TEST_HISTORY_COUNT] = {1, 2, 3, 4};
	static const int16_t noise[2] = {1, -1};

	assert(urp_radio_core_receive_frontend_s16(
		       NULL, output, TEST_NATIVE_FRAMES, gate, 1U, history, TEST_HISTORY_COUNT,
		       coefficients, 1, 256, noise, sizeof(noise) / sizeof(noise[0]), 1, 3U, 6U,
		       1000U, 100U, &state, &output_count, &rssi_updated, &workspace) == -1);
	assert(output_count == 0U && rssi_updated == 0);
	assert(!memcmp(output, expected_output, sizeof(output)));
	assert(!memcmp(gate, expected_gate, sizeof(gate)));
	assert(!memcmp(history, expected_history, sizeof(history)));
	assert_states_equal(&state, &expected_state);
}

/** @brief Run receive-frontend bridge regressions. */
int main(void)
{
	assert(!urp_radio_core_initialize());
	test_partition_parity();
	test_rejected_call_is_transactional();
	return 0;
}
