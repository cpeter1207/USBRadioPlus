/**
 * @file test_radio_core_descriptor.c
 * @brief Verify complete selected radio-core descriptor validation.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "usbradioplus_radio_core_adapter.h"
#include "radio_core_boundary_cases.h"

/** Test-selected descriptor returned to the adapter's control-plane hook. */
static const struct rptadv_radio_descriptor *test_descriptor;

#include "radio_core_fault_cases.h"

/** @brief Supply the descriptor candidate selected by this focused harness. */
const struct rptadv_radio_descriptor *urp_radio_core_adapter_test_descriptor(void)
{
	return test_descriptor;
}

/** @brief Copy the released complete descriptor before introducing one defect. */
static struct rptadv_radio_descriptor descriptor_copy(void)
{
	const struct rptadv_radio_descriptor *released = rptadv_radio_descriptor();
	struct rptadv_radio_descriptor copy;

	assert(released);
	assert(released->struct_size >= sizeof(copy));
	memcpy(&copy, released, sizeof(copy));
	return copy;
}

/** @brief Reject each missing selected operation without replacing the live descriptor. */
static void reject_missing_operations(const struct rptadv_radio_descriptor *published)
{
#define ASSERT_REQUIRED_MEMBER(member)                                                             \
	do {                                                                                       \
		struct rptadv_radio_descriptor candidate = *published;                             \
		candidate.member = NULL;                                                           \
		test_descriptor = &candidate;                                                      \
		assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_INCOMPLETE);       \
		assert(urp_radio_core_descriptor_get() == published);                              \
	} while (0)

	ASSERT_REQUIRED_MEMBER(radio_create);
	ASSERT_REQUIRED_MEMBER(radio_destroy);
	ASSERT_REQUIRED_MEMBER(radio_repeat_f32);
	ASSERT_REQUIRED_MEMBER(radio_render_transmit_f32);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_frequency_supported);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_legacy_frequency);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_legacy_peak);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_legacy_scaled_peak);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_legacy_scaled_levels);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_generate_f32);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_generate_tail_f32);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_phase_radians);
	ASSERT_REQUIRED_MEMBER(radio_dcs_code_supported);
	ASSERT_REQUIRED_MEMBER(radio_dcs_configure_transmit);
	ASSERT_REQUIRED_MEMBER(radio_dcs_generate_f32);
	ASSERT_REQUIRED_MEMBER(radio_dcs_tail_phase_radians);
	ASSERT_REQUIRED_MEMBER(radio_extract_receive_f32);
	ASSERT_REQUIRED_MEMBER(radio_render_calibrated_test_tone_f32);
	ASSERT_REQUIRED_MEMBER(radio_calibrated_test_tone_phase_radians);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_bind_f32);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_reset);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_rx_transition);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_record_f32);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_play_f32);
	ASSERT_REQUIRED_MEMBER(radio_native_parrot_status);
	ASSERT_REQUIRED_MEMBER(radio_parse_rx_audio_mode);
	ASSERT_REQUIRED_MEMBER(radio_parse_carrier_source);
	ASSERT_REQUIRED_MEMBER(radio_parse_ctcss_source);
	ASSERT_REQUIRED_MEMBER(radio_parse_tone_off_mode);
	ASSERT_REQUIRED_MEMBER(radio_dcs_configure_receive);
	ASSERT_REQUIRED_MEMBER(radio_dcs_process_receive_f32);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_configure_receive);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_process_receive_f32);
	ASSERT_REQUIRED_MEMBER(radio_measure_raw_pcm_f32);
	ASSERT_REQUIRED_MEMBER(radio_micor_squelch_update);
	ASSERT_REQUIRED_MEMBER(radio_measure_envelope_f32);
	ASSERT_REQUIRED_MEMBER(radio_delay_line_f32);
	ASSERT_REQUIRED_MEMBER(radio_center_slicer_f32);
	ASSERT_REQUIRED_MEMBER(radio_deemphasis_integrator_f32);
	ASSERT_REQUIRED_MEMBER(radio_fir_mono_f32);
	ASSERT_REQUIRED_MEMBER(radio_receive_frontend_f32);
	ASSERT_REQUIRED_MEMBER(radio_elapsed_ms);
	ASSERT_REQUIRED_MEMBER(radio_timer_consume);
	ASSERT_REQUIRED_MEMBER(radio_signal_mode_advance);
	ASSERT_REQUIRED_MEMBER(radio_ctcss_render_state_advance);
	ASSERT_REQUIRED_MEMBER(radio_tx_finish_advance);
	ASSERT_REQUIRED_MEMBER(radio_tx_finish_continue);
	ASSERT_REQUIRED_MEMBER(radio_tx_complete);
	ASSERT_REQUIRED_MEMBER(radio_rx_blanking_advance);
	ASSERT_REQUIRED_MEMBER(radio_vox_carrier_advance);
	ASSERT_REQUIRED_MEMBER(radio_tx_cpu_saver_advance);
	ASSERT_REQUIRED_MEMBER(radio_rx_cpu_saver_advance);
	ASSERT_REQUIRED_MEMBER(radio_dcs_turnoff_advance);

#undef ASSERT_REQUIRED_MEMBER
	test_descriptor = published;
}

int main(void)
{
	struct rptadv_radio_descriptor truncated = descriptor_copy();
	struct rptadv_radio_descriptor missing_meter = descriptor_copy();
	struct rptadv_radio_descriptor complete = descriptor_copy();
	struct rptadv_radio_descriptor incompatible = descriptor_copy();
	uint32_t parsed = 99U;

	test_descriptor = NULL;
	test_radio_core_unavailable_boundaries();
	test_radio_core_control_boundaries();
	test_radio_core_workspace_boundaries();
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_UNAVAILABLE);
	assert(!urp_radio_core_descriptor_get());

	truncated.struct_size = offsetof(struct rptadv_radio_descriptor, radio_dcs_turnoff_advance);
	test_descriptor = &truncated;
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_INCOMPLETE);
	assert(!urp_radio_core_descriptor_get());

	missing_meter.radio_measure_raw_pcm_f32 = NULL;
	test_descriptor = &missing_meter;
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_INCOMPLETE);
	assert(!urp_radio_core_descriptor_get());

	test_descriptor = &complete;
	assert(!urp_radio_core_parse_rx_audio_mode("no", &parsed));
	assert(parsed == 0U);
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_OK);
	assert(urp_radio_core_descriptor_get() == &complete);

	reject_missing_operations(&complete);
	test_radio_core_opaque_boundaries();
	test_radio_core_malformed_adapter();

	test_descriptor = &missing_meter;
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_INCOMPLETE);
	assert(urp_radio_core_descriptor_get() == &complete);

	incompatible.abi_version = RPTADV_RADIO_ABI_VERSION + 1U;
	test_descriptor = &incompatible;
	assert(urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_ABI_MISMATCH);
	assert(urp_radio_core_descriptor_get() == &complete);
	return 0;
}
