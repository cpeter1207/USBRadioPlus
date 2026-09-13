/** @file
 * @brief Public radio-core facade boundaries before descriptor publication.
 */

#include "usbradioplus_ctcss.h"
#include <limits.h>

/** @brief Validate each scalar conversion/workspace boundary before unavailable dispatch. */
static void test_radio_core_workspace_boundaries(void)
{
	float f32[2] = {0.0F, 0.0F};
	int16_t pcm[2] = {0, 0};
	uint8_t gates[2] = {0U, 0U};
	int token, flag = 0;
	struct rptadv_radio *radio = (struct rptadv_radio *)&token;
	unsigned int index = 0U, dirty = 0U;
	uint32_t cursor = 0U;
	int32_t milliseconds = 0;
	float peak = 0.0F;
	unsigned long rails = 0UL;
	size_t count = 0U;
	const size_t oversized = (size_t)UINT32_MAX + 1U;
	struct rptadv_radio_audio_statistics statistics = {0};
	struct rptadv_radio_micor_squelch_state squelch = {0};
	struct rptadv_radio_envelope_state envelope = {0};
	struct rptadv_radio_center_slicer_state center = {0};
	struct rptadv_radio_deemphasis_integrator_state deemphasis = {0};
	struct rptadv_radio_receive_frontend_state frontend = {0};
	struct urp_radio_delay_workspace delay = {f32, f32, f32, SIZE_MAX, SIZE_MAX};
	struct urp_radio_center_slicer_workspace slice = {f32, f32, f32, SIZE_MAX};
	struct urp_radio_deemphasis_integrator_workspace deemph = {f32, f32, SIZE_MAX};
	struct urp_radio_fir_workspace fir = {f32, f32, pcm, SIZE_MAX, SIZE_MAX};
	struct urp_radio_receive_frontend_workspace front = {f32,      f32,	 pcm,	  gates,
							     SIZE_MAX, SIZE_MAX, SIZE_MAX};

	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(NULL, f32, f32, 1U, f32, 1U, &index, &peak, &rails) ==
	       -1);
	assert(urp_radio_core_extract_receive(radio, NULL, f32, 1U, f32, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, NULL, 1U, f32, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, NULL, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, 1U, NULL, &peak, &rails) ==
	       -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, 1U, &index, NULL, &rails) ==
	       -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, 1U, &index, &peak, NULL) ==
	       -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, oversized, f32, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, oversized, &index, &peak,
					      &rails) == -1);

	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, f32, SIZE_MAX, &flag) ==
	       -1);
	assert(urp_radio_core_measure_audio_s16(NULL, 1U, 1U, &statistics, f32, SIZE_MAX, &flag) ==
	       -1);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, NULL, f32, SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, NULL, SIZE_MAX, &flag) ==
	       -1);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, f32, SIZE_MAX, NULL) ==
	       -1);
	assert(urp_radio_core_measure_audio_s16(pcm, oversized, 1U, &statistics, f32, SIZE_MAX,
						&flag) == -1);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, f32, 0U, &flag) == -1);

	assert(urp_radio_core_micor_squelch_update(&squelch, 0, 0.0, 1U, 1U, &flag) == -1);
	assert(urp_radio_core_micor_squelch_update(NULL, 0, 0.0, 1U, 1U, &flag) == -1);
	assert(urp_radio_core_micor_squelch_update(&squelch, 0, 0.0, 1U, 1U, NULL) == -1);

	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, f32,
						   SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(NULL, pcm, 1U, 1, 1, &envelope, f32, f32,
						   SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, NULL, f32, f32, SIZE_MAX,
						   &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, NULL, f32,
						   SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, NULL,
						   SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, f32,
						   SIZE_MAX, NULL) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, oversized, 1, 1, &envelope, f32, f32,
						   SIZE_MAX, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, f32, 0U,
						   &flag) == -1);

	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, NULL, &dirty, 1U, 0U, &delay) ==
	       -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, NULL, 1U, 0U, &delay) ==
	       -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U, NULL) ==
	       -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 0U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 2U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, oversized, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, oversized, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);

	assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center, &slice) == -1);
	assert(urp_radio_core_center_slicer_s16(NULL, pcm, pcm, 1U, 1, 1, 1, &center, &slice) ==
	       -1);
	assert(urp_radio_core_center_slicer_s16(pcm, NULL, pcm, 1U, 1, 1, 1, &center, &slice) ==
	       -1);
	assert(urp_radio_core_center_slicer_s16(pcm, pcm, NULL, 1U, 1, 1, 1, &center, &slice) ==
	       -1);
	assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, NULL, &slice) == -1);
	assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center, NULL) == -1);
	assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, oversized, 1, 1, 1, &center,
						&slice) == -1);

	assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis,
							&deemph) == -1);
	assert(urp_radio_core_deemphasis_integrator_s16(NULL, pcm, 1U, 1, 1, 1, &deemphasis,
							&deemph) == -1);
	assert(urp_radio_core_deemphasis_integrator_s16(pcm, NULL, 1U, 1, 1, 1, &deemphasis,
							&deemph) == -1);
	assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, NULL, &deemph) ==
	       -1);
	assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis, NULL) ==
	       -1);
	assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, oversized, 1, 1, 1, &deemphasis,
							&deemph) == -1);

	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(NULL, NULL, 0U, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(NULL, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, NULL, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, NULL, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, NULL, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, NULL) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, oversized, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, oversized, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 0U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 0, &fir) == -1);

	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(NULL, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1,
						   pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, NULL, 1U, gates, 1U, pcm, 1U, pcm, 1, 1,
						   pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, NULL, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, NULL, 1U, pcm, 1, 1,
						   pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, NULL, 1, 1,
						   pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1,
						   NULL, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, NULL, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, NULL, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, NULL,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   NULL) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, SIZE_MAX, pcm, 1U, pcm, 1,
						   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, oversized, pcm, 1U, pcm, 1,
						   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, oversized, gates, 1U, pcm, 1U, pcm, 1,
						   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, oversized, pcm, 1,
						   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   oversized, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 0U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   0U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   2U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 0, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 0, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 0U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 0U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);

	assert(urp_radio_core_elapsed_ms(&cursor, 1U, &milliseconds) == -1);
	assert(urp_radio_core_elapsed_ms(NULL, 1U, &milliseconds) == -1);
	assert(urp_radio_core_elapsed_ms(&cursor, 1U, NULL) == -1);
	assert(urp_radio_core_elapsed_ms(&cursor, oversized, &milliseconds) == -1);

	assert(urp_radio_core_timer_consume(&milliseconds, 1, &milliseconds) == -1);
	assert(urp_radio_core_timer_consume(NULL, 1, &milliseconds) == -1);
	assert(urp_radio_core_timer_consume(&milliseconds, 1, NULL) == -1);

	{
		__typeof__(delay) before = delay;
		delay.storage = NULL;
		assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
						     &delay) == -1);
		delay = before;
	}

	{
		__typeof__(delay) before = delay;
		delay.storage_capacity = 0U;
		assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
						     &delay) == -1);
		delay = before;
	}

	{
		__typeof__(delay) before = delay;
		delay.frame_capacity = 0U;
		assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
						     &delay) == -1);
		delay = before;
	}

	{
		__typeof__(slice) before = slice;
		slice.input = NULL;
		assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
							&slice) == -1);
		slice = before;
	}

	{
		__typeof__(slice) before = slice;
		slice.centered_output = NULL;
		assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
							&slice) == -1);
		slice = before;
	}

	{
		__typeof__(slice) before = slice;
		slice.limited_output = NULL;
		assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
							&slice) == -1);
		slice = before;
	}

	{
		__typeof__(slice) before = slice;
		slice.capacity = 0U;
		assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
							&slice) == -1);
		slice = before;
	}

	{
		__typeof__(deemph) before = deemph;
		deemph.input = NULL;
		assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis,
								&deemph) == -1);
		deemph = before;
	}

	{
		__typeof__(deemph) before = deemph;
		deemph.output = NULL;
		assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis,
								&deemph) == -1);
		deemph = before;
	}

	{
		__typeof__(deemph) before = deemph;
		deemph.capacity = 0U;
		assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis,
								&deemph) == -1);
		deemph = before;
	}

	{
		__typeof__(fir) before = fir;
		fir.input = NULL;
		assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) ==
		       -1);
		fir = before;
	}

	{
		__typeof__(fir) before = fir;
		fir.output = NULL;
		assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) ==
		       -1);
		fir = before;
	}

	{
		__typeof__(fir) before = fir;
		fir.history = NULL;
		assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) ==
		       -1);
		fir = before;
	}

	{
		__typeof__(fir) before = fir;
		fir.frame_capacity = 0U;
		assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) ==
		       -1);
		fir = before;
	}

	{
		__typeof__(fir) before = fir;
		fir.history_capacity = 0U;
		assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) ==
		       -1);
		fir = before;
	}

	{
		__typeof__(front) before = front;
		front.input = NULL;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.baseband_output = NULL;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.history = NULL;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.carrier_gate = NULL;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.native_frame_capacity = 0U;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.baseband_output_capacity = 0U;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	{
		__typeof__(front) before = front;
		front.history_capacity = 0U;
		assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1,
							   1, pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend,
							   &count, &flag, &front) == -1);
		front = before;
	}

	assert(urp_radio_core_parse_rx_audio_mode(NULL, &cursor) == -1);
	assert(urp_radio_core_parse_rx_audio_mode("no", NULL) == -1);

	assert(urp_radio_core_parse_carrier_source(NULL, &cursor) == -1);
	assert(urp_radio_core_parse_carrier_source("no", NULL) == -1);

	assert(urp_radio_core_parse_ctcss_source(NULL, &cursor) == -1);
	assert(urp_radio_core_parse_ctcss_source("no", NULL) == -1);

	assert(urp_radio_core_parse_tone_off_mode(NULL, &cursor) == -1);
	assert(urp_radio_core_parse_tone_off_mode("no", NULL) == -1);
}

/** @brief Missing shared descriptors fail closed without touching opaque radio storage. */
static void test_radio_core_unavailable_boundaries(void)
{
	int token;
	struct rptadv_radio *radio = (struct rptadv_radio *)&token;
	struct rptadv_radio *created = radio;
	float samples[2] = {0.0F, 0.0F};
	int16_t pcm[2] = {0, 0};
	double phase = 1.0, amplitude = 1.0, bias = 1.0;
	float peak = 1.0F;
	unsigned long rails = 1UL;
	unsigned int index = 0U;
	uint32_t value = 99U;
	int decoded = 1;
	size_t count = 1U;
	struct rptadv_radio_native_parrot_status parrot = {0};
	struct rptadv_radio_audio_statistics statistics = {0};
	struct rptadv_radio_micor_squelch_state squelch = {0};
	struct rptadv_radio_envelope_state envelope = {0};
	int32_t milliseconds = 0;

	assert(!urp_radio_core_descriptor_get());
	assert(urp_radio_core_parse_rx_audio_mode("speaker", &value) == -1);
	assert(urp_radio_core_parse_carrier_source("no", &value) == -1);
	assert(urp_radio_core_parse_ctcss_source("no", &value) == -1);
	assert(urp_radio_core_parse_tone_off_mode("no", &value) == -1);
	assert(value == 99U);
	assert(!urp_ctcss_frequency_supported(100.0F));
	assert(urp_ctcss_legacy_frequency(100.0) == 0.0);
	assert(urp_ctcss_legacy_peak(100.0, 0) == 0.0);
	assert(urp_ctcss_legacy_scaled_peak(100.0, 0, 256, 256) == 0.0);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256, &amplitude, &bias);
	assert(amplitude == 0.0 && bias == 0.0);
	assert(urp_radio_core_create(48000U, 960U, &created) == -1 && !created);
	urp_radio_core_destroy(radio);
	assert(urp_radio_core_generate_ctcss(radio, samples, 1U, 100.0, 0.5F, 1, 0.0) == -1);
	assert(urp_radio_core_generate_ctcss_tail(radio, samples, 1U, 100.0, 0.5F, 1) == -1);
	assert(urp_radio_core_ctcss_phase(radio, &phase) == -1);
	assert(urp_radio_core_configure_dcs(radio, 23, 0) == -1);
	assert(urp_radio_core_generate_dcs(radio, samples, 1U, 0.5, 1, 0) == -1);
	assert(urp_radio_core_configure_dcs_receive(radio, 23, 0) == -1);
	assert(urp_radio_core_process_dcs_receive(radio, samples, 1U, &decoded) == -1);
	assert(!decoded);
	assert(urp_radio_core_configure_ctcss_receive(radio, 1U, 0) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, samples, 1U, 1, &decoded) == -1);
	assert(decoded == -1);
	assert(urp_radio_core_extract_receive(radio, samples, samples, 1U, samples, 1U, &index,
					      &peak, &rails) == -1);
	assert(peak == 0.0F && !rails);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, samples, 2U, &decoded) ==
	       -1);
	assert(urp_radio_core_micor_squelch_update(&squelch, 0, 0.0, 1U, 1U, &decoded) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, samples, samples,
						   2U, &decoded) == -1);
	assert(urp_radio_core_elapsed_ms(&value, 1U, &milliseconds) == -1);
	assert(urp_radio_core_timer_consume(&milliseconds, 1, &milliseconds) == -1);
	assert(urp_radio_core_render_calibrated_test_tone(radio, samples, 1U, 1) == -1);
	assert(urp_radio_core_calibrated_test_tone_phase(radio, &phase) == -1);
	assert(urp_radio_core_native_parrot_bind(radio, samples, 2U) == -1);
	assert(urp_radio_core_native_parrot_reset(radio) == -1);
	assert(urp_radio_core_native_parrot_rx_transition(radio, 0, 1, &decoded) == -1);
	assert(urp_radio_core_native_parrot_record(radio, samples, 1U, 2U, &count) == -1);
	assert(!count);
	assert(urp_radio_core_native_parrot_play(radio, samples, 1U, &count) == -1);
	assert(urp_radio_core_native_parrot_status(radio, &parrot) == -1);
}

/** @brief Missing descriptors and missing arguments leave portable state unchanged. */
static void test_radio_core_control_boundaries(void)
{
#define ASSERT_INPUT_STATE(operation, input_type, state_type)                                      \
	do {                                                                                       \
		struct input_type input = {0};                                                     \
		struct state_type state = {0};                                                     \
		assert(operation(NULL, &state) == -1);                                             \
		assert(operation(&input, NULL) == -1);                                             \
		assert(operation(&input, &state) == -1);                                           \
	} while (0)
	ASSERT_INPUT_STATE(urp_radio_core_tx_finish_advance, rptadv_radio_tx_finish_input,
			   rptadv_radio_tx_finish_state);
	ASSERT_INPUT_STATE(urp_radio_core_tx_finish_continue, rptadv_radio_tx_finish_input,
			   rptadv_radio_tx_finish_state);
	ASSERT_INPUT_STATE(urp_radio_core_rx_blanking_advance, rptadv_radio_rx_blanking_input,
			   rptadv_radio_rx_blanking_state);
	ASSERT_INPUT_STATE(urp_radio_core_vox_carrier_advance, rptadv_radio_vox_carrier_input,
			   rptadv_radio_vox_carrier_state);
	ASSERT_INPUT_STATE(urp_radio_core_tx_cpu_saver_advance, rptadv_radio_tx_cpu_saver_input,
			   rptadv_radio_tx_cpu_saver_state);
	ASSERT_INPUT_STATE(urp_radio_core_rx_cpu_saver_advance, rptadv_radio_rx_cpu_saver_input,
			   rptadv_radio_rx_cpu_saver_state);
#undef ASSERT_INPUT_STATE

#define ASSERT_CONFIG_INPUT_STATE(operation, config_type, input_type, state_type)                  \
	do {                                                                                       \
		struct config_type config = {.struct_size = sizeof(config)};                       \
		struct input_type input = {0};                                                     \
		struct state_type state = {0};                                                     \
		assert(operation(NULL, &input, &state) == -1);                                     \
		assert(operation(&config, NULL, &state) == -1);                                    \
		assert(operation(&config, &input, NULL) == -1);                                    \
		assert(operation(&config, &input, &state) == -1);                                  \
		config.struct_size = 0U;                                                           \
		assert(operation(&config, &input, &state) == -1);                                  \
	} while (0)
	ASSERT_CONFIG_INPUT_STATE(urp_radio_core_signal_mode_advance,
				  rptadv_radio_signal_mode_config, rptadv_radio_signal_mode_input,
				  rptadv_radio_signal_mode_state);
	ASSERT_CONFIG_INPUT_STATE(
		urp_radio_core_ctcss_render_state_advance, rptadv_radio_ctcss_render_state_config,
		rptadv_radio_ctcss_render_state_input, rptadv_radio_ctcss_render_state);
#undef ASSERT_CONFIG_INPUT_STATE
	{
		struct rptadv_radio_dcs_turnoff_config config = {0};
		struct rptadv_radio_dcs_turnoff_input input = {0};
		struct rptadv_radio_dcs_turnoff_state state = {0};
		assert(urp_radio_core_dcs_turnoff_advance(NULL, &input, &state) == -1);
		assert(urp_radio_core_dcs_turnoff_advance(&config, NULL, &state) == -1);
		assert(urp_radio_core_dcs_turnoff_advance(&config, &input, NULL) == -1);
		assert(urp_radio_core_dcs_turnoff_advance(&config, &input, &state) == -1);
	}
	{
		struct rptadv_radio_tx_complete_config config = {.struct_size = sizeof(config)};
		struct rptadv_radio_tx_complete_state state = {0};
		assert(urp_radio_core_tx_complete(NULL, &state) == -1);
		assert(urp_radio_core_tx_complete(&config, NULL) == -1);
		assert(urp_radio_core_tx_complete(&config, &state) == -1);
		config.struct_size = 0U;
		assert(urp_radio_core_tx_complete(&config, &state) == -1);
	}
}

/** @brief Reject each opaque-radio argument defect before forwarding to shared code. */
static void test_radio_core_opaque_boundaries(void)
{
	struct rptadv_radio *radio = NULL;
	float samples[2] = {0.0F, 0.0F};
	double phase;
	int decoded;
	size_t count;
	const size_t oversized = (size_t)UINT32_MAX + 1U;
	struct rptadv_radio_native_parrot_status status;
	assert(urp_radio_core_create(48000U, 960U, NULL) == -1);
	assert(urp_radio_core_create(0U, 960U, &radio) == -1 && !radio);
	assert(urp_radio_core_create(48000U, 0U, &radio) == -1 && !radio);
	assert(!urp_radio_core_create(48000U, 960U, &radio));

#define CHECK_GENERATE_CT(call)                                                                    \
	do {                                                                                       \
		assert(call(NULL, samples, 1U, 100.0, 0.5F, 1) == -1);                             \
		assert(call(radio, NULL, 1U, 100.0, 0.5F, 1) == -1);                               \
		assert(call(radio, samples, oversized, 100.0, 0.5F, 1) == -1);                     \
	} while (0)
	CHECK_GENERATE_CT(urp_radio_core_generate_ctcss_tail);
#undef CHECK_GENERATE_CT
	assert(urp_radio_core_generate_ctcss(NULL, samples, 1U, 100.0, 0.5F, 1, 0.0) == -1);
	assert(urp_radio_core_generate_ctcss(radio, NULL, 1U, 100.0, 0.5F, 1, 0.0) == -1);
	assert(urp_radio_core_generate_ctcss(radio, samples, oversized, 100.0, 0.5F, 1, 0.0) == -1);
	assert(urp_radio_core_ctcss_phase(NULL, &phase) == -1);
	assert(urp_radio_core_ctcss_phase(radio, NULL) == -1);
	assert(urp_radio_core_configure_dcs(NULL, 23, 0) == -1);
	assert(urp_radio_core_generate_dcs(NULL, samples, 1U, 0.5, 1, 0) == -1);
	assert(urp_radio_core_generate_dcs(radio, NULL, 1U, 0.5, 1, 0) == -1);
	assert(urp_radio_core_generate_dcs(radio, samples, oversized, 0.5, 1, 0) == -1);
	assert(urp_radio_core_configure_dcs_receive(NULL, 23, 0) == -1);
	assert(urp_radio_core_process_dcs_receive(NULL, samples, 1U, &decoded) == -1);
	assert(urp_radio_core_process_dcs_receive(radio, samples, 1U, NULL) == -1);
	assert(urp_radio_core_process_dcs_receive(radio, NULL, 1U, &decoded) == -1);
	assert(urp_radio_core_process_dcs_receive(radio, samples, oversized, &decoded) == -1);
	assert(urp_radio_core_configure_ctcss_receive(NULL, 1U, 0) == -1);
	assert(urp_radio_core_process_ctcss_receive(NULL, samples, 1U, 1, &decoded) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, samples, 1U, 1, NULL) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, NULL, 1U, 1, &decoded) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, samples, oversized, 1, &decoded) == -1);
	assert(urp_radio_core_render_calibrated_test_tone(NULL, samples, 1U, 1) == -1);
	assert(urp_radio_core_render_calibrated_test_tone(radio, NULL, 1U, 1) == -1);
	assert(urp_radio_core_render_calibrated_test_tone(radio, samples, oversized, 1) == -1);
	assert(urp_radio_core_calibrated_test_tone_phase(NULL, &phase) == -1);
	assert(urp_radio_core_calibrated_test_tone_phase(radio, NULL) == -1);
	assert(urp_radio_core_native_parrot_bind(NULL, samples, 2U) == -1);
	assert(urp_radio_core_native_parrot_bind(radio, NULL, 2U) == -1);
	assert(urp_radio_core_native_parrot_bind(radio, samples, 0U) == -1);
	assert(urp_radio_core_native_parrot_bind(radio, samples, oversized) == -1);
	assert(urp_radio_core_native_parrot_reset(NULL) == -1);
	assert(urp_radio_core_native_parrot_rx_transition(NULL, 0, 1, &decoded) == -1);
	assert(urp_radio_core_native_parrot_rx_transition(radio, 0, 1, NULL) == -1);
	assert(urp_radio_core_native_parrot_record(NULL, samples, 1U, 2U, &count) == -1);
	assert(urp_radio_core_native_parrot_record(radio, NULL, 1U, 2U, &count) == -1);
	assert(urp_radio_core_native_parrot_record(radio, samples, 1U, 2U, NULL) == -1);
	assert(urp_radio_core_native_parrot_record(radio, samples, oversized, 2U, &count) == -1);
	assert(urp_radio_core_native_parrot_record(radio, samples, 1U, oversized, &count) == -1);
	assert(urp_radio_core_native_parrot_play(NULL, samples, 1U, &count) == -1);
	assert(urp_radio_core_native_parrot_play(radio, NULL, 1U, &count) == -1);
	assert(urp_radio_core_native_parrot_play(radio, samples, 1U, NULL) == -1);
	assert(urp_radio_core_native_parrot_play(radio, samples, oversized, &count) == -1);
	assert(urp_radio_core_native_parrot_status(NULL, &status) == -1);
	assert(urp_radio_core_native_parrot_status(radio, NULL) == -1);
	urp_radio_core_destroy(radio);
}
