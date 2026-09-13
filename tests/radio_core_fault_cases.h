/** @file
 * @brief Malformed released-descriptor results must not corrupt facade-owned state.
 */

#include <math.h>

/** @brief Configured fake ABI result and malformed payload fields. */
static enum rptadv_radio_result core_fault_result;
static uint32_t core_fault_scalar, core_fault_count;
static int32_t core_fault_decoded;
static uint8_t core_fault_gate;
static float core_fault_sample, core_fault_alt_sample;

/** @brief Return a scripted ctcss legacy scaled levels result. */
static enum rptadv_radio_result
fault_radio_ctcss_legacy_scaled_levels(double frequency_hz, uint32_t filter_250,
				       int32_t tone_gain_q8, int32_t output_gain_q8,
				       double *amplitude_pcm_codes, double *bias_pcm_codes)
{
	(void)frequency_hz;
	(void)filter_250;
	(void)tone_gain_q8;
	(void)output_gain_q8;
	(void)amplitude_pcm_codes;
	(void)bias_pcm_codes;
	*amplitude_pcm_codes = 1.0;
	*bias_pcm_codes = 2.0;
	return core_fault_result;
}

/** @brief Return a scripted extract receive f32 result. */
static enum rptadv_radio_result
fault_radio_extract_receive_f32(const struct rptadv_radio *radio, const float *stereo, float *mono,
				uint32_t frame_count, float *delay, uint32_t delay_frame_count,
				uint32_t *delay_index,
				struct rptadv_radio_receive_extract_stats *stats)
{
	(void)radio;
	(void)stereo;
	(void)mono;
	(void)frame_count;
	(void)delay;
	(void)delay_frame_count;
	(void)delay_index;
	(void)stats;
	return core_fault_result;
}

/** @brief Return a scripted native parrot rx transition result. */
static enum rptadv_radio_result fault_radio_native_parrot_rx_transition(struct rptadv_radio *radio,
									uint32_t was_keyed,
									uint32_t is_keyed,
									uint32_t *playback_started)
{
	(void)radio;
	(void)was_keyed;
	(void)is_keyed;
	(void)playback_started;
	return core_fault_result;
}

/** @brief Return a scripted native parrot record f32 result. */
static enum rptadv_radio_result fault_radio_native_parrot_record_f32(struct rptadv_radio *radio,
								     const float *input,
								     uint32_t frame_count,
								     uint32_t recording_limit,
								     uint32_t *recorded)
{
	(void)radio;
	(void)input;
	(void)frame_count;
	(void)recording_limit;
	(void)recorded;
	return core_fault_result;
}

/** @brief Return a scripted native parrot play f32 result. */
static enum rptadv_radio_result fault_radio_native_parrot_play_f32(struct rptadv_radio *radio,
								   float *output,
								   uint32_t frame_count,
								   uint32_t *played)
{
	(void)radio;
	(void)output;
	(void)frame_count;
	(void)played;
	return core_fault_result;
}

/** @brief Return a scripted dcs process receive f32 result. */
static enum rptadv_radio_result fault_radio_dcs_process_receive_f32(struct rptadv_radio *radio,
								    const float *stereo,
								    uint32_t frame_count,
								    uint32_t *valid)
{
	(void)radio;
	(void)stereo;
	(void)frame_count;
	(void)valid;
	*valid = core_fault_scalar;
	return core_fault_result;
}

/** @brief Return a scripted ctcss process receive f32 result. */
static enum rptadv_radio_result fault_radio_ctcss_process_receive_f32(struct rptadv_radio *radio,
								      const float *samples,
								      uint32_t sample_count,
								      uint32_t carrier_detect,
								      int32_t *decoded)
{
	(void)radio;
	(void)samples;
	(void)sample_count;
	(void)carrier_detect;
	(void)decoded;
	*decoded = core_fault_decoded;
	return core_fault_result;
}

/** @brief Return a scripted measure raw pcm f32 result. */
static enum rptadv_radio_result
fault_radio_measure_raw_pcm_f32(const float *samples, uint32_t sample_count, uint32_t channels,
				struct rptadv_radio_audio_statistics *statistics,
				uint32_t *clipping)
{
	(void)samples;
	(void)sample_count;
	(void)channels;
	(void)statistics;
	(void)clipping;
	*clipping = core_fault_scalar;
	return core_fault_result;
}

/** @brief Return a scripted micor squelch update result. */
static enum rptadv_radio_result
fault_radio_micor_squelch_update(struct rptadv_radio_micor_squelch_state *state, uint32_t squelched,
				 double sample_power, uint32_t open_level, uint32_t hysteresis,
				 uint32_t *closed)
{
	(void)state;
	(void)squelched;
	(void)sample_power;
	(void)open_level;
	(void)hysteresis;
	(void)closed;
	*closed = core_fault_scalar;
	return core_fault_result;
}

/** @brief Return a scripted measure envelope f32 result. */
static enum rptadv_radio_result
fault_radio_measure_envelope_f32(const float *input, float *output, uint32_t sample_count,
				 int32_t decay_factor, int16_t threshold,
				 struct rptadv_radio_envelope_state *state, uint32_t *comparator)
{
	(void)input;
	(void)output;
	(void)sample_count;
	(void)decay_factor;
	(void)threshold;
	(void)state;
	(void)comparator;
	if (output && sample_count)
		output[0] = core_fault_sample;
	*comparator = core_fault_scalar;
	return core_fault_result;
}

/** @brief Return a scripted delay line f32 result. */
static enum rptadv_radio_result
fault_radio_delay_line_f32(const float *input, float *output, uint32_t sample_count, float *storage,
			   uint32_t storage_capacity, uint32_t lead,
			   struct rptadv_radio_delay_line_state *state, uint32_t enabled,
			   uint32_t outzero)
{
	(void)input;
	(void)output;
	(void)sample_count;
	(void)storage;
	(void)storage_capacity;
	(void)lead;
	(void)state;
	(void)enabled;
	(void)outzero;
	if (output && sample_count)
		output[0] = core_fault_sample;
	return core_fault_result;
}

/** @brief Return a scripted center slicer f32 result. */
static enum rptadv_radio_result
fault_radio_center_slicer_f32(const float *input, float *centered_output, float *limited_output,
			      uint32_t sample_count, int32_t limit, int16_t setpoint,
			      int32_t decay_factor, struct rptadv_radio_center_slicer_state *state)
{
	(void)input;
	(void)centered_output;
	(void)limited_output;
	(void)sample_count;
	(void)limit;
	(void)setpoint;
	(void)decay_factor;
	(void)state;
	if (sample_count) {
		centered_output[0] = core_fault_sample;
		limited_output[0] = core_fault_alt_sample;
	}
	return core_fault_result;
}

/** @brief Return a scripted deemphasis integrator f32 result. */
static enum rptadv_radio_result
fault_radio_deemphasis_integrator_f32(const float *input, float *output, uint32_t sample_count,
				      int16_t output_coefficient, int16_t feedback_coefficient,
				      int32_t output_gain,
				      struct rptadv_radio_deemphasis_integrator_state *state)
{
	(void)input;
	(void)output;
	(void)sample_count;
	(void)output_coefficient;
	(void)feedback_coefficient;
	(void)output_gain;
	(void)state;
	if (sample_count)
		output[0] = core_fault_sample;
	return core_fault_result;
}

/** @brief Return a scripted fir mono f32 result. */
static enum rptadv_radio_result
fault_radio_fir_mono_f32(const float *input, float *output, uint32_t sample_count, int16_t *history,
			 uint32_t history_count, const int16_t *coefficients, int32_t input_gain,
			 int32_t output_gain, int32_t calc_adjust)
{
	(void)input;
	(void)output;
	(void)sample_count;
	(void)history;
	(void)history_count;
	(void)coefficients;
	(void)input_gain;
	(void)output_gain;
	(void)calc_adjust;
	if (sample_count)
		output[0] = core_fault_sample;
	return core_fault_result;
}

/** @brief Return a scripted receive frontend f32 result. */
static enum rptadv_radio_result fault_radio_receive_frontend_f32(
	const float *input, float *baseband_output, uint32_t baseband_output_capacity,
	uint8_t *carrier_gate, uint32_t carrier_gate_capacity, uint32_t native_frame_count,
	int16_t *history, uint32_t history_count, const int16_t *baseband_coefficients,
	int32_t baseband_calc_adjust, int32_t baseband_output_gain,
	const int16_t *noise_coefficients, uint32_t noise_coefficient_count, int32_t noise_divisor,
	uint32_t decimate, uint32_t calibration_window, uint32_t open_level, uint32_t hysteresis,
	struct rptadv_radio_receive_frontend_state *state, uint32_t *baseband_output_count,
	uint32_t *rssi_updated)
{
	(void)input;
	(void)baseband_output;
	(void)baseband_output_capacity;
	(void)carrier_gate;
	(void)carrier_gate_capacity;
	(void)native_frame_count;
	(void)history;
	(void)history_count;
	(void)baseband_coefficients;
	(void)baseband_calc_adjust;
	(void)baseband_output_gain;
	(void)noise_coefficients;
	(void)noise_coefficient_count;
	(void)noise_divisor;
	(void)decimate;
	(void)calibration_window;
	(void)open_level;
	(void)hysteresis;
	(void)state;
	(void)baseband_output_count;
	(void)rssi_updated;
	if (baseband_output && core_fault_count)
		baseband_output[0] = core_fault_sample;
	if (native_frame_count)
		carrier_gate[0] = core_fault_gate;
	*baseband_output_count = core_fault_count;
	*rssi_updated = core_fault_scalar;
	return core_fault_result;
}

/** @brief Return a scripted elapsed ms result. */
static enum rptadv_radio_result
fault_radio_elapsed_ms(uint32_t *remainder, uint32_t native_frame_count, int32_t *milliseconds)
{
	(void)remainder;
	(void)native_frame_count;
	(void)milliseconds;
	return core_fault_result;
}

/** @brief Return a scripted timer consume result. */
static enum rptadv_radio_result fault_radio_timer_consume(int32_t *timer, int32_t milliseconds,
							  int32_t *remaining)
{
	(void)timer;
	(void)milliseconds;
	(void)remaining;
	return core_fault_result;
}

/** @brief Return a scripted tx finish continue result. */
static enum rptadv_radio_result
fault_radio_tx_finish_continue(const struct rptadv_radio_tx_finish_input *input,
			       struct rptadv_radio_tx_finish_state *state)
{
	(void)input;
	(void)state;
	return core_fault_result;
}

/** @brief Return a scripted tx complete result. */
static enum rptadv_radio_result
fault_radio_tx_complete(const struct rptadv_radio_tx_complete_config *config,
			struct rptadv_radio_tx_complete_state *state)
{
	(void)config;
	(void)state;
	return core_fault_result;
}

/** @brief Script the create failure boundary. */
static enum rptadv_radio_result fault_radio_create(const struct rptadv_radio_config *config,
						   struct rptadv_radio **radio)
{
	(void)config;
	(void)radio;
	*radio = NULL;
	return core_fault_result;
}

/** @brief Script the ctcss generate f32 failure boundary. */
static enum rptadv_radio_result fault_radio_ctcss_generate_f32(struct rptadv_radio *radio,
							       float *output, uint32_t frame_count,
							       double frequency_hz, float peak,
							       uint32_t enabled,
							       double phase_shift_degrees)
{
	(void)radio;
	(void)output;
	(void)frame_count;
	(void)frequency_hz;
	(void)peak;
	(void)enabled;
	(void)phase_shift_degrees;
	return core_fault_result;
}

/** @brief Script the ctcss generate tail f32 failure boundary. */
static enum rptadv_radio_result
fault_radio_ctcss_generate_tail_f32(struct rptadv_radio *radio, float *output, uint32_t frame_count,
				    double frequency_hz, float peak, uint32_t enabled)
{
	(void)radio;
	(void)output;
	(void)frame_count;
	(void)frequency_hz;
	(void)peak;
	(void)enabled;
	return core_fault_result;
}

/** @brief Script the ctcss phase radians failure boundary. */
static enum rptadv_radio_result fault_radio_ctcss_phase_radians(const struct rptadv_radio *radio,
								double *phase_radians)
{
	(void)radio;
	(void)phase_radians;
	return core_fault_result;
}

/** @brief Script the dcs configure transmit failure boundary. */
static enum rptadv_radio_result fault_radio_dcs_configure_transmit(struct rptadv_radio *radio,
								   int32_t code, uint32_t inverted)
{
	(void)radio;
	(void)code;
	(void)inverted;
	return core_fault_result;
}

/** @brief Script the dcs generate f32 failure boundary. */
static enum rptadv_radio_result fault_radio_dcs_generate_f32(struct rptadv_radio *radio,
							     float *output, uint32_t frame_count,
							     float peak, uint32_t enabled,
							     uint32_t turnoff)
{
	(void)radio;
	(void)output;
	(void)frame_count;
	(void)peak;
	(void)enabled;
	(void)turnoff;
	return core_fault_result;
}

/** @brief Script the render calibrated test tone f32 failure boundary. */
static enum rptadv_radio_result
fault_radio_render_calibrated_test_tone_f32(struct rptadv_radio *radio, float *program,
					    uint32_t frame_count, uint32_t enabled)
{
	(void)radio;
	(void)program;
	(void)frame_count;
	(void)enabled;
	return core_fault_result;
}

/** @brief Script the calibrated test tone phase radians failure boundary. */
static enum rptadv_radio_result
fault_radio_calibrated_test_tone_phase_radians(const struct rptadv_radio *radio,
					       double *phase_radians)
{
	(void)radio;
	(void)phase_radians;
	return core_fault_result;
}

/** @brief Script the native parrot bind f32 failure boundary. */
static enum rptadv_radio_result fault_radio_native_parrot_bind_f32(struct rptadv_radio *radio,
								   float *storage,
								   uint32_t storage_capacity)
{
	(void)radio;
	(void)storage;
	(void)storage_capacity;
	return core_fault_result;
}

/** @brief Script the native parrot reset failure boundary. */
static enum rptadv_radio_result fault_radio_native_parrot_reset(struct rptadv_radio *radio)
{
	(void)radio;
	return core_fault_result;
}

/** @brief Script the native parrot status failure boundary. */
static enum rptadv_radio_result
fault_radio_native_parrot_status(const struct rptadv_radio *radio,
				 struct rptadv_radio_native_parrot_status *status)
{
	(void)radio;
	(void)status;
	return core_fault_result;
}

/** @brief Script the dcs configure receive failure boundary. */
static enum rptadv_radio_result fault_radio_dcs_configure_receive(struct rptadv_radio *radio,
								  int32_t code, uint32_t inverted)
{
	(void)radio;
	(void)code;
	(void)inverted;
	return core_fault_result;
}

/** @brief Script the ctcss configure receive failure boundary. */
static enum rptadv_radio_result
fault_radio_ctcss_configure_receive(struct rptadv_radio *radio,
				    const struct rptadv_radio_ctcss_receive_config *config)
{
	(void)radio;
	(void)config;
	return core_fault_result;
}

/** @brief Check failure mapping and malformed value rejection through a complete descriptor. */
static void test_radio_core_malformed_adapter(void)
{
	const struct rptadv_radio_descriptor *saved = urp_radio_core_descriptor_get();
	struct rptadv_radio_descriptor faulty = *saved;
	int token, flag = 0;
	struct rptadv_radio *radio = (struct rptadv_radio *)&token;
	struct rptadv_radio *created;
	struct rptadv_radio_native_parrot_status parrot_status;
	float f32[2] = {0.0F, 0.0F};
	int16_t pcm[2] = {0, 0};
	uint8_t gates[2] = {0U, 0U};
	unsigned int index = 0U, dirty = 0U;
	uint32_t cursor = 0U;
	int32_t milliseconds = 0;
	float peak = 1.0F;
	unsigned long rails = 1UL;
	double amplitude = 1.0, bias = 1.0;
	size_t count = 0U;
	struct rptadv_radio_audio_statistics statistics = {0};
	struct rptadv_radio_micor_squelch_state squelch = {0};
	struct rptadv_radio_envelope_state envelope = {0};
	struct rptadv_radio_center_slicer_state center = {0};
	struct rptadv_radio_deemphasis_integrator_state deemphasis = {0};
	struct rptadv_radio_receive_frontend_state frontend = {0};
	struct urp_radio_delay_workspace delay = {f32, f32, f32, 2U, 2U};
	struct urp_radio_center_slicer_workspace slice = {f32, f32, f32 + 1, 2U};
	struct urp_radio_deemphasis_integrator_workspace deemph = {f32, f32, 2U};
	struct urp_radio_fir_workspace fir = {f32, f32, pcm, 2U, 2U};
	struct urp_radio_receive_frontend_workspace front = {f32, f32, pcm, gates, 2U, 2U, 2U};
	struct rptadv_radio_tx_finish_input finish_input = {0};
	struct rptadv_radio_tx_finish_state finish_state = {0};
	struct rptadv_radio_tx_complete_config complete_config = {.struct_size =
									  sizeof(complete_config)};
	struct rptadv_radio_tx_complete_state complete_state = {0};

	faulty.radio_ctcss_legacy_scaled_levels = fault_radio_ctcss_legacy_scaled_levels;
	faulty.radio_extract_receive_f32 = fault_radio_extract_receive_f32;
	faulty.radio_native_parrot_rx_transition = fault_radio_native_parrot_rx_transition;
	faulty.radio_native_parrot_record_f32 = fault_radio_native_parrot_record_f32;
	faulty.radio_native_parrot_play_f32 = fault_radio_native_parrot_play_f32;
	faulty.radio_dcs_process_receive_f32 = fault_radio_dcs_process_receive_f32;
	faulty.radio_ctcss_process_receive_f32 = fault_radio_ctcss_process_receive_f32;
	faulty.radio_measure_raw_pcm_f32 = fault_radio_measure_raw_pcm_f32;
	faulty.radio_micor_squelch_update = fault_radio_micor_squelch_update;
	faulty.radio_measure_envelope_f32 = fault_radio_measure_envelope_f32;
	faulty.radio_delay_line_f32 = fault_radio_delay_line_f32;
	faulty.radio_center_slicer_f32 = fault_radio_center_slicer_f32;
	faulty.radio_deemphasis_integrator_f32 = fault_radio_deemphasis_integrator_f32;
	faulty.radio_fir_mono_f32 = fault_radio_fir_mono_f32;
	faulty.radio_receive_frontend_f32 = fault_radio_receive_frontend_f32;
	faulty.radio_elapsed_ms = fault_radio_elapsed_ms;
	faulty.radio_timer_consume = fault_radio_timer_consume;
	faulty.radio_tx_finish_continue = fault_radio_tx_finish_continue;
	faulty.radio_tx_complete = fault_radio_tx_complete;
	test_descriptor = &faulty;
	faulty.radio_create = fault_radio_create;
	faulty.radio_ctcss_generate_f32 = fault_radio_ctcss_generate_f32;
	faulty.radio_ctcss_generate_tail_f32 = fault_radio_ctcss_generate_tail_f32;
	faulty.radio_ctcss_phase_radians = fault_radio_ctcss_phase_radians;
	faulty.radio_dcs_configure_transmit = fault_radio_dcs_configure_transmit;
	faulty.radio_dcs_generate_f32 = fault_radio_dcs_generate_f32;
	faulty.radio_render_calibrated_test_tone_f32 = fault_radio_render_calibrated_test_tone_f32;
	faulty.radio_calibrated_test_tone_phase_radians =
		fault_radio_calibrated_test_tone_phase_radians;
	faulty.radio_native_parrot_bind_f32 = fault_radio_native_parrot_bind_f32;
	faulty.radio_native_parrot_reset = fault_radio_native_parrot_reset;
	faulty.radio_native_parrot_status = fault_radio_native_parrot_status;
	faulty.radio_dcs_configure_receive = fault_radio_dcs_configure_receive;
	faulty.radio_ctcss_configure_receive = fault_radio_ctcss_configure_receive;
	assert(!urp_radio_core_initialize());
	core_fault_result = RPTADV_RADIO_INVALID_ARGUMENT;
	core_fault_scalar = core_fault_count = 0U;
	core_fault_sample = core_fault_alt_sample = 0.0F;
	core_fault_gate = 0U;
	core_fault_decoded = -1;
	assert(urp_radio_core_generate_ctcss(radio, NULL, 0U, 100.0, 0.5F, 1, 0.0) == -1);
	assert(urp_radio_core_generate_ctcss_tail(radio, NULL, 0U, 100.0, 0.5F, 1) == -1);
	assert(urp_radio_core_ctcss_phase(radio, &amplitude) == -1);
	assert(urp_radio_core_configure_dcs(radio, 23, 0) == -1);
	assert(urp_radio_core_generate_dcs(radio, NULL, 0U, 0.5, 1, 0) == -1);
	assert(urp_radio_core_configure_dcs_receive(radio, 23, 0) == -1);
	assert(urp_radio_core_configure_ctcss_receive(radio, 1U, 0) == -1);
	assert(urp_radio_core_process_dcs_receive(radio, NULL, 0U, &flag) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, NULL, 0U, 1, &flag) == -1);
	assert(urp_radio_core_render_calibrated_test_tone(radio, NULL, 0U, 1) == -1);
	assert(urp_radio_core_calibrated_test_tone_phase(radio, &amplitude) == -1);
	assert(urp_radio_core_native_parrot_bind(radio, f32, 2U) == -1);
	assert(urp_radio_core_native_parrot_reset(radio) == -1);
	assert(urp_radio_core_native_parrot_status(radio, &parrot_status) == -1);
	assert(urp_radio_core_native_parrot_record(radio, NULL, 0U, 2U, &count) == -1);
	assert(urp_radio_core_native_parrot_play(radio, NULL, 0U, &count) == -1);
	assert(urp_radio_core_extract_receive(radio, NULL, NULL, 0U, NULL, 0U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, (size_t)UINT32_MAX + 1U, f32, 1U,
					      &index, &peak, &rails) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, (size_t)UINT32_MAX + 1U,
					      &index, &peak, &rails) == -1);
	assert(urp_radio_core_create(48000U, 960U, &created) == -1 && !created);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256, NULL, &bias);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256, &amplitude, NULL);
	urp_ctcss_legacy_scaled_levels(100.0, 0, 256, 256, &amplitude, &bias);
	assert(amplitude == 0.0 && bias == 0.0);
	assert(urp_radio_core_process_dcs_receive(radio, f32, 1U, &flag) == -1);
	assert(urp_radio_core_process_ctcss_receive(radio, f32, 1U, 1, &flag) == -1);
	assert(urp_radio_core_extract_receive(radio, f32, f32, 1U, f32, 1U, &index, &peak,
					      &rails) == -1);
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, f32, 2U, &flag) == -1);
	assert(urp_radio_core_micor_squelch_update(&squelch, 0, 0.0, 1U, 1U, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, f32, 2U,
						   &flag) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center, &slice) == -1);
	assert(urp_radio_core_deemphasis_integrator_s16(pcm, pcm, 1U, 1, 1, 1, &deemphasis,
							&deemph) == -1);
	assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1, &fir) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	assert(urp_radio_core_elapsed_ms(&cursor, 1U, &milliseconds) == -1);
	assert(urp_radio_core_timer_consume(&milliseconds, 1, &milliseconds) == -1);
	assert(urp_radio_core_tx_finish_continue(&finish_input, &finish_state) == -1);
	assert(urp_radio_core_tx_complete(&complete_config, &complete_state) == -1);
	assert(urp_radio_core_native_parrot_rx_transition(radio, 0, 1, &flag) == -1);
	assert(urp_radio_core_native_parrot_record(radio, f32, 1U, 2U, &count) == -1);
	assert(urp_radio_core_native_parrot_play(radio, f32, 1U, &count) == -1);
	core_fault_result = RPTADV_RADIO_OK;
	assert(urp_radio_core_create(48000U, 960U, &created) == -1 && !created);
	assert(urp_radio_core_measure_envelope_s16(NULL, NULL, 0U, 1, 1, &envelope, NULL, NULL, 0U,
						   &flag) == 0);
	assert(urp_radio_core_delay_line_s16(NULL, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(pcm, NULL, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	delay.input = NULL;
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	delay.input = f32;
	delay.output = NULL;
	assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == -1);
	dirty = 1U;
	assert(urp_radio_core_delay_line_s16(NULL, pcm, 1U, 1U, 1U, &cursor, &dirty, 0U, 0U,
					     &delay) == -1);
	delay.output = f32;
	assert(urp_radio_core_delay_line_s16(NULL, NULL, 1U, 1U, 1U, &cursor, &dirty, 0U, 0U,
					     &delay) == -1);
	assert(urp_radio_core_delay_line_s16(NULL, NULL, 0U, 1U, 1U, &cursor, &dirty, 0U, 0U,
					     &delay) == 0);
	assert(urp_radio_core_delay_line_s16(NULL, NULL, 0U, 1U, 1U, &cursor, &dirty, 1U, 0U,
					     &delay) == 0);
	core_fault_scalar = 2U;
	assert(urp_radio_core_measure_audio_s16(pcm, 1U, 1U, &statistics, f32, 2U, &flag) == -1);
	assert(urp_radio_core_micor_squelch_update(&squelch, 0, 0.0, 1U, 1U, &flag) == -1);
	assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope, f32, f32, 2U,
						   &flag) == -1);
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	core_fault_scalar = 0U;
	core_fault_decoded = -2;
	assert(urp_radio_core_process_ctcss_receive(radio, f32, 1U, 1, &flag) == -1);
	core_fault_decoded = RPTADV_RADIO_CTCSS_RECEIVE_TONE_COUNT;
	assert(urp_radio_core_process_ctcss_receive(radio, f32, 1U, 1, &flag) == -1);
	core_fault_count = 2U;
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	core_fault_count = 1U;
	core_fault_gate = 2U;
	assert(urp_radio_core_receive_frontend_s16(pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm,
						   1U, 1, 1U, 1U, 1U, 1U, &frontend, &count, &flag,
						   &front) == -1);
	core_fault_gate = 0U;
	{
		const float invalid[] = {-2.0F, 1.0F, 0.5F / 32768.0F, NAN};
		for (size_t sample = 0U; sample < sizeof(invalid) / sizeof(invalid[0]); ++sample) {
			core_fault_sample = invalid[sample];
			assert(urp_radio_core_measure_envelope_s16(pcm, pcm, 1U, 1, 1, &envelope,
								   f32, f32, 2U, &flag) == -1);
			assert(urp_radio_core_delay_line_s16(pcm, pcm, 1U, 1U, 1U, &cursor, &dirty,
							     1U, 0U, &delay) == -1);
			assert(urp_radio_core_deemphasis_integrator_s16(
				       pcm, pcm, 1U, 1, 1, 1, &deemphasis, &deemph) == -1);
			assert(urp_radio_core_fir_mono_s16(pcm, pcm, 1U, pcm, pcm, 1U, 1, 1, 1,
							   &fir) == -1);
			assert(urp_radio_core_receive_frontend_s16(
				       pcm, pcm, 1U, gates, 1U, pcm, 1U, pcm, 1, 1, pcm, 1U, 1, 1U,
				       1U, 1U, 1U, &frontend, &count, &flag, &front) == -1);
			core_fault_alt_sample = invalid[sample];
			assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
								&slice) == -1);
			core_fault_sample = 0.0F;
			assert(urp_radio_core_center_slicer_s16(pcm, pcm, pcm, 1U, 1, 1, 1, &center,
								&slice) == -1);
		}
	}
	core_fault_sample = core_fault_alt_sample = 0.0F;
	core_fault_count = 0U;
	front.baseband_output = NULL;
	core_fault_count = 1U;
	assert(urp_radio_core_receive_frontend_s16(NULL, NULL, 1U, NULL, 0U, pcm, 1U, pcm, 1, 1,
						   pcm, 1U, 1, 1U, 1U, 1U, 1U, &frontend, &count,
						   &flag, &front) == -1);
	test_descriptor = saved;
	assert(!urp_radio_core_initialize());
}
