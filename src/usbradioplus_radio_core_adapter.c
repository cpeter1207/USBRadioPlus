/**
 * @file usbradioplus_radio_core_adapter.c
 * @brief Control-plane validation and lifecycle for librptadvradio.
 */

#include "usbradioplus_radio_core_adapter.h"

#include "usbradioplus_ctcss.h"

#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

/**
 * @brief Descriptor tail containing every radio-core operation USBRadioPlus calls.
 *
 * The generic @c radio_tick operation is not selected by this composition.
 */
#define URP_RADIO_CORE_REQUIRED_DESCRIPTOR_END                                                     \
	(offsetof(struct rptadv_radio_descriptor, radio_dcs_turnoff_advance) +                     \
	 sizeof(((struct rptadv_radio_descriptor *)0)->radio_dcs_turnoff_advance))

/** Immutable descriptor published after compatibility validation. */
static _Atomic(const struct rptadv_radio_descriptor *) radio_core_descriptor;

#if defined(URP_RADIO_CORE_ADAPTER_TESTING)
/** @brief Return the test-controlled descriptor candidate. */
extern const struct rptadv_radio_descriptor *urp_radio_core_adapter_test_descriptor(void);
#endif

/** @brief Obtain the descriptor candidate from the selected control-plane provider.
 * @return Borrowed candidate descriptor, or NULL when unavailable.
 */
static const struct rptadv_radio_descriptor *radio_core_provider_descriptor(void)
{
#if defined(URP_RADIO_CORE_ADAPTER_TESTING)
	return urp_radio_core_adapter_test_descriptor();
#else
	return rptadv_radio_descriptor();
#endif
}

/** @brief Check that a candidate exposes every operation selected by this composition.
 * @param descriptor Non-NULL candidate checked by the initialization boundary.
 * @return Nonzero when all required operations are present.
 */
static int radio_core_descriptor_complete(const struct rptadv_radio_descriptor *descriptor)
{
	if (descriptor->struct_size < URP_RADIO_CORE_REQUIRED_DESCRIPTOR_END)
		return 0;
	return descriptor->radio_create && descriptor->radio_destroy &&
	       descriptor->radio_repeat_f32 && descriptor->radio_render_transmit_f32 &&
	       descriptor->radio_ctcss_frequency_supported &&
	       descriptor->radio_ctcss_legacy_frequency && descriptor->radio_ctcss_legacy_peak &&
	       descriptor->radio_ctcss_legacy_scaled_peak &&
	       descriptor->radio_ctcss_legacy_scaled_levels &&
	       descriptor->radio_ctcss_generate_f32 && descriptor->radio_ctcss_generate_tail_f32 &&
	       descriptor->radio_ctcss_phase_radians && descriptor->radio_dcs_code_supported &&
	       descriptor->radio_dcs_configure_transmit && descriptor->radio_dcs_generate_f32 &&
	       descriptor->radio_dcs_tail_phase_radians && descriptor->radio_extract_receive_f32 &&
	       descriptor->radio_render_calibrated_test_tone_f32 &&
	       descriptor->radio_calibrated_test_tone_phase_radians &&
	       descriptor->radio_native_parrot_bind_f32 && descriptor->radio_native_parrot_reset &&
	       descriptor->radio_native_parrot_rx_transition &&
	       descriptor->radio_native_parrot_record_f32 &&
	       descriptor->radio_native_parrot_play_f32 && descriptor->radio_native_parrot_status &&
	       descriptor->radio_parse_rx_audio_mode && descriptor->radio_parse_carrier_source &&
	       descriptor->radio_parse_ctcss_source && descriptor->radio_parse_tone_off_mode &&
	       descriptor->radio_dcs_configure_receive &&
	       descriptor->radio_dcs_process_receive_f32 &&
	       descriptor->radio_ctcss_configure_receive &&
	       descriptor->radio_ctcss_process_receive_f32 &&
	       descriptor->radio_measure_raw_pcm_f32 && descriptor->radio_micor_squelch_update &&
	       descriptor->radio_measure_envelope_f32 && descriptor->radio_delay_line_f32 &&
	       descriptor->radio_center_slicer_f32 && descriptor->radio_deemphasis_integrator_f32 &&
	       descriptor->radio_fir_mono_f32 && descriptor->radio_receive_frontend_f32 &&
	       descriptor->radio_elapsed_ms && descriptor->radio_timer_consume &&
	       descriptor->radio_signal_mode_advance &&
	       descriptor->radio_ctcss_render_state_advance &&
	       descriptor->radio_tx_finish_advance && descriptor->radio_tx_finish_continue &&
	       descriptor->radio_tx_complete && descriptor->radio_rx_blanking_advance &&
	       descriptor->radio_vox_carrier_advance && descriptor->radio_tx_cpu_saver_advance &&
	       descriptor->radio_rx_cpu_saver_advance && descriptor->radio_dcs_turnoff_advance;
}

int urp_radio_core_initialize(void)
{
	const struct rptadv_radio_descriptor *descriptor = radio_core_provider_descriptor();

	if (!descriptor)
		return URP_RADIO_CORE_INITIALIZE_UNAVAILABLE;
	if (descriptor->abi_version != RPTADV_RADIO_ABI_VERSION)
		return URP_RADIO_CORE_INITIALIZE_ABI_MISMATCH;
	if (!radio_core_descriptor_complete(descriptor))
		return URP_RADIO_CORE_INITIALIZE_INCOMPLETE;
	atomic_store_explicit(&radio_core_descriptor, descriptor, memory_order_release);
	return URP_RADIO_CORE_INITIALIZE_OK;
}

const struct rptadv_radio_descriptor *urp_radio_core_descriptor_get(void)
{
	return atomic_load_explicit(&radio_core_descriptor, memory_order_acquire);
}

/** @brief Signature shared by append-only portable configuration parsers. */
typedef enum rptadv_radio_result (*urp_radio_core_parse_assignment_fn)(const char *text,
								       uint32_t *value);

/** @brief Obtain the descriptor for parsing before native-renderer setup.
 * @return Validated descriptor, or NULL if initialization fails.
 *
 * Configuration parsing occurs before a renderer exists on some compatibility
 * paths. Descriptor validation is idempotent control-plane work, so a parser
 * may perform it lazily without changing the parser's public contract.
 */
static const struct rptadv_radio_descriptor *radio_core_parse_descriptor(void)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!descriptor && urp_radio_core_initialize() == URP_RADIO_CORE_INITIALIZE_OK)
		descriptor = urp_radio_core_descriptor_get();
	return descriptor;
}

/** @brief Parse into a temporary so all failure paths preserve caller storage.
 * @param text Non-NULL configuration value checked by the public parser.
 * @param value Non-NULL output checked by the public parser.
 * @param parse Portable parser implementing the selected assignment.
 * @return Zero on success, or minus one on invalid arguments or parse failure.
 */
static int radio_core_parse_assignment(const char *text, uint32_t *value,
				       urp_radio_core_parse_assignment_fn parse)
{
	uint32_t parsed = 0U;

	if (!parse || parse(text, &parsed) != RPTADV_RADIO_OK)
		return -1;
	*value = parsed;
	return 0;
}

int urp_radio_core_parse_rx_audio_mode(const char *text, uint32_t *mode)
{
	const struct rptadv_radio_descriptor *descriptor;

	if (!text || !mode)
		return -1;
	descriptor = radio_core_parse_descriptor();

	return radio_core_parse_assignment(
		text, mode, descriptor ? descriptor->radio_parse_rx_audio_mode : NULL);
}

int urp_radio_core_parse_carrier_source(const char *text, uint32_t *source)
{
	const struct rptadv_radio_descriptor *descriptor;

	if (!text || !source)
		return -1;
	descriptor = radio_core_parse_descriptor();

	return radio_core_parse_assignment(
		text, source, descriptor ? descriptor->radio_parse_carrier_source : NULL);
}

int urp_radio_core_parse_ctcss_source(const char *text, uint32_t *source)
{
	const struct rptadv_radio_descriptor *descriptor;

	if (!text || !source)
		return -1;
	descriptor = radio_core_parse_descriptor();

	return radio_core_parse_assignment(
		text, source, descriptor ? descriptor->radio_parse_ctcss_source : NULL);
}

int urp_radio_core_parse_tone_off_mode(const char *text, uint32_t *mode)
{
	const struct rptadv_radio_descriptor *descriptor;

	if (!text || !mode)
		return -1;
	descriptor = radio_core_parse_descriptor();

	return radio_core_parse_assignment(
		text, mode, descriptor ? descriptor->radio_parse_tone_off_mode : NULL);
}

int urp_ctcss_frequency_supported(float frequency)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	return descriptor ? !!descriptor->radio_ctcss_frequency_supported(frequency) : 0;
}

double urp_ctcss_legacy_frequency(double frequency)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	return descriptor ? descriptor->radio_ctcss_legacy_frequency(frequency) : 0.0;
}

double urp_ctcss_legacy_peak(double frequency, int filter_250)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	return descriptor ? descriptor->radio_ctcss_legacy_peak(frequency, !!filter_250) : 0.0;
}

double urp_ctcss_legacy_scaled_peak(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	return descriptor ? descriptor->radio_ctcss_legacy_scaled_peak(frequency, !!filter_250,
								       tone_gain_q8, output_gain_q8)
			  : 0.0;
}

void urp_ctcss_legacy_scaled_levels(double frequency, int filter_250, int tone_gain_q8,
				    int output_gain_q8, double *amplitude, double *bias)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (amplitude)
		*amplitude = 0.0;
	if (bias)
		*bias = 0.0;
	if (!descriptor || !amplitude || !bias)
		return;
	if (descriptor->radio_ctcss_legacy_scaled_levels(frequency, !!filter_250, tone_gain_q8,
							 output_gain_q8, amplitude,
							 bias) != RPTADV_RADIO_OK) {
		*amplitude = 0.0;
		*bias = 0.0;
	}
}

int urp_radio_core_create(uint32_t native_sample_rate_hz, uint32_t maximum_frame_count,
			  struct rptadv_radio **radio)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	const struct rptadv_radio_config config = {
		.struct_size = sizeof(config),
		.abi_version = RPTADV_RADIO_ABI_VERSION,
		.native_sample_rate_hz = native_sample_rate_hz,
		.maximum_frame_count = maximum_frame_count,
		.interleaved_channels = RPTADV_RADIO_CANONICAL_CHANNELS,
	};

	if (!radio)
		return -1;
	*radio = NULL;
	if (!descriptor || !native_sample_rate_hz || !maximum_frame_count)
		return -1;
	return descriptor->radio_create(&config, radio) == RPTADV_RADIO_OK && *radio ? 0 : -1;
}

void urp_radio_core_destroy(struct rptadv_radio *radio)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (descriptor)
		descriptor->radio_destroy(radio);
}

/** @brief Convert one C size into the fixed ABI's bounded frame count.
 * @param frame_count Native C frame count.
 * @param bounded Non-NULL local storage receiving the ABI-sized count.
 * @return Zero on success, or minus one when the count cannot be represented.
 */
static int radio_core_frame_count(size_t frame_count, uint32_t *bounded)
{
	if (frame_count > UINT32_MAX)
		return -1;
	*bounded = (uint32_t)frame_count;
	return 0;
}

int urp_radio_core_generate_ctcss(struct rptadv_radio *radio, float *output, size_t frame_count,
				  double frequency_hz, float peak, int enabled,
				  double phase_shift_degrees)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;

	if (!radio || (frame_count && !output) || radio_core_frame_count(frame_count, &bounded) ||
	    !descriptor)
		return -1;
	return descriptor->radio_ctcss_generate_f32(radio, output, bounded, frequency_hz, peak,
						    !!enabled,
						    phase_shift_degrees) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_generate_ctcss_tail(struct rptadv_radio *radio, float *output,
				       size_t frame_count, double frequency_hz, float peak,
				       int enabled)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;

	if (!radio || (frame_count && !output) || radio_core_frame_count(frame_count, &bounded) ||
	    !descriptor)
		return -1;
	return descriptor->radio_ctcss_generate_tail_f32(radio, output, bounded, frequency_hz, peak,
							 !!enabled) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_ctcss_phase(const struct rptadv_radio *radio, double *phase_radians)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!radio || !phase_radians || !descriptor)
		return -1;
	return descriptor->radio_ctcss_phase_radians(radio, phase_radians) == RPTADV_RADIO_OK ? 0
											      : -1;
}

int urp_radio_core_configure_dcs(struct rptadv_radio *radio, int code, int inverted)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!radio || !descriptor)
		return -1;
	return descriptor->radio_dcs_configure_transmit(radio, code, !!inverted) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_generate_dcs(struct rptadv_radio *radio, float *output, size_t frame_count,
				double peak, int enabled, int turnoff)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;

	if (!radio || (frame_count && !output) || radio_core_frame_count(frame_count, &bounded) ||
	    !descriptor)
		return -1;
	return descriptor->radio_dcs_generate_f32(radio, output, bounded, (float)peak, !!enabled,
						  !!turnoff) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_configure_dcs_receive(struct rptadv_radio *radio, int code, int inverted)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!radio || !descriptor)
		return -1;
	return descriptor->radio_dcs_configure_receive(radio, code, !!inverted) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_process_dcs_receive(struct rptadv_radio *radio, const float *stereo,
				       size_t frame_count, int *valid)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;
	uint32_t decoded = 0U;

	if (valid)
		*valid = 0;
	if (!radio || !valid || (frame_count && !stereo) ||
	    radio_core_frame_count(frame_count, &bounded) || !descriptor)
		return -1;
	if (descriptor->radio_dcs_process_receive_f32(radio, stereo, bounded, &decoded) !=
	    RPTADV_RADIO_OK)
		return -1;
	*valid = !!decoded;
	return 0;
}

int urp_radio_core_configure_ctcss_receive(struct rptadv_radio *radio, uint64_t tone_mask,
					   int relax)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	const struct rptadv_radio_ctcss_receive_config config = {
		.struct_size = sizeof(config),
		.tone_mask = tone_mask,
		.relax = !!relax,
	};

	if (!radio || !descriptor)
		return -1;
	return descriptor->radio_ctcss_configure_receive(radio, &config) == RPTADV_RADIO_OK ? 0
											    : -1;
}

int urp_radio_core_process_ctcss_receive(struct rptadv_radio *radio, const float *samples,
					 size_t sample_count, int carrier_detect, int *decoded)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;
	int32_t result = -1;

	if (decoded)
		*decoded = -1;
	if (!radio || !decoded || (sample_count && !samples) ||
	    radio_core_frame_count(sample_count, &bounded) || !descriptor)
		return -1;
	if (descriptor->radio_ctcss_process_receive_f32(radio, samples, bounded, !!carrier_detect,
							&result) != RPTADV_RADIO_OK)
		return -1;
	if (result < -1 || result >= (int32_t)RPTADV_RADIO_CTCSS_RECEIVE_TONE_COUNT)
		return -1;
	*decoded = result;
	return 0;
}

int urp_radio_core_extract_receive(const struct rptadv_radio *radio, const float *stereo,
				   float *mono, size_t frame_count, float *delay,
				   size_t delay_frame_count, unsigned int *delay_index, float *peak,
				   unsigned long *rail_samples)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	struct rptadv_radio_receive_extract_stats stats = {0};
	uint32_t bounded_frames;
	uint32_t bounded_delay;
	uint32_t index;
	int result;

	if (peak)
		*peak = 0.0F;
	if (rail_samples)
		*rail_samples = 0U;
	if (!radio || !peak || !rail_samples || (frame_count && (!stereo || !mono)) ||
	    (delay_frame_count && !delay) || !delay_index || !descriptor ||
	    radio_core_frame_count(frame_count, &bounded_frames) ||
	    radio_core_frame_count(delay_frame_count, &bounded_delay))
		return -1;
	index = *delay_index;
	result = descriptor->radio_extract_receive_f32(radio, stereo, mono, bounded_frames, delay,
						       bounded_delay, &index, &stats);
	if (result != RPTADV_RADIO_OK)
		return -1;
	*delay_index = index;
	*peak = stats.peak;
	*rail_samples =
		stats.rail_samples > ULONG_MAX ? ULONG_MAX : (unsigned long)stats.rail_samples;
	return 0;
}

int urp_radio_core_measure_audio_s16(const int16_t *samples, size_t sample_count,
				     unsigned int channels,
				     struct rptadv_radio_audio_statistics *statistics,
				     float *f32_workspace, size_t f32_workspace_capacity,
				     int *clipping)
{
	const struct rptadv_radio_descriptor *descriptor;
	uint32_t bounded_samples;
	uint32_t detected = 0U;
	size_t index;

	if (clipping)
		*clipping = 0;
	if (!statistics || !clipping || (sample_count && (!samples || !f32_workspace)) ||
	    sample_count > f32_workspace_capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < sample_count; ++index)
		f32_workspace[index] = (float)samples[index] / 32768.0F;
	if (descriptor->radio_measure_raw_pcm_f32(f32_workspace, bounded_samples, channels,
						  statistics, &detected) != RPTADV_RADIO_OK ||
	    detected > 1U)
		return -1;
	*clipping = (int)detected;
	return 0;
}

int urp_radio_core_micor_squelch_update(struct rptadv_radio_micor_squelch_state *state,
					int squelched, double sample_power, uint32_t open_level,
					uint32_t hysteresis, int *closed)
{
	const struct rptadv_radio_descriptor *descriptor;
	uint32_t portable_closed = 1U;

	if (closed)
		*closed = 1;
	if (!state || !closed)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor ||
	    descriptor->radio_micor_squelch_update(state, !!squelched, sample_power, open_level,
						   hysteresis,
						   &portable_closed) != RPTADV_RADIO_OK ||
	    portable_closed > 1U)
		return -1;
	*closed = (int)portable_closed;
	return 0;
}

int urp_radio_core_measure_envelope_s16(const int16_t *input, int16_t *output, size_t sample_count,
					int32_t decay_factor, int16_t threshold,
					struct rptadv_radio_envelope_state *state, float *f32_input,
					float *f32_output, size_t f32_capacity, int *comparator)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_envelope_state next;
	uint32_t bounded_samples;
	uint32_t portable_comparator = 0U;
	size_t index;

	if (comparator)
		*comparator = 0;
	if (!state || !comparator || (sample_count && (!input || !f32_input)) ||
	    (output && !f32_output) || sample_count > f32_capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < sample_count; ++index)
		f32_input[index] = (float)input[index] / 32768.0F;
	next = *state;
	if (descriptor->radio_measure_envelope_f32(f32_input, output ? f32_output : NULL,
						   bounded_samples, decay_factor, threshold, &next,
						   &portable_comparator) != RPTADV_RADIO_OK ||
	    portable_comparator > 1U)
		return -1;
	/* Validate all converted output before publishing any of it. The portable
	 * primitive emits nonnegative exact legacy PCM values, so this protects the
	 * C fallback from a malformed shared object without changing normal output. */
	for (index = 0U; output && index < sample_count; ++index) {
		float codes = f32_output[index] * 32768.0F;
		int32_t quantized;

		if (!(codes >= 0.0F && codes <= 32767.0F))
			return -1;
		quantized = (int32_t)codes;
		if (codes != (float)quantized)
			return -1;
	}
	for (index = 0U; output && index < sample_count; ++index)
		output[index] = (int16_t)(f32_output[index] * 32768.0F);
	*state = next;
	*comparator = (int)portable_comparator;
	return 0;
}

int urp_radio_core_delay_line_s16(const int16_t *input, int16_t *output, size_t sample_count,
				  size_t storage_capacity, uint32_t lead, uint32_t *input_index,
				  unsigned int *dirty, unsigned int enabled, unsigned int outzero,
				  struct urp_radio_delay_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_delay_line_state next;
	uint32_t bounded_samples;
	uint32_t bounded_storage;
	unsigned int active;
	unsigned int publish_output;
	size_t index;

	if (!input_index || !dirty || !workspace || !workspace->storage || !storage_capacity ||
	    lead > storage_capacity || storage_capacity > workspace->storage_capacity ||
	    sample_count > workspace->frame_capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples) ||
	    radio_core_frame_count(storage_capacity, &bounded_storage))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	active = !!enabled && !outzero;
	publish_output = active || !!*dirty;
	if (active) {
		if (sample_count && (!input || !output || !workspace->input || !workspace->output))
			return -1;
		for (index = 0U; index < sample_count; ++index)
			workspace->input[index] = (float)input[index] / 32768.0F;
	} else if (publish_output && sample_count && (!output || !workspace->output)) {
		return -1;
	}
	next.input_index = *input_index;
	next.dirty = !!*dirty;
	if (descriptor->radio_delay_line_f32(active ? workspace->input : NULL,
					     publish_output ? workspace->output : NULL,
					     bounded_samples, workspace->storage, bounded_storage,
					     lead, &next, !!enabled, !!outzero) != RPTADV_RADIO_OK)
		return -1;
	for (index = 0U; publish_output && index < sample_count; ++index) {
		float codes;
		int32_t quantized;

		codes = workspace->output[index] * 32768.0F;
		if (!(codes >= (float)INT16_MIN && codes <= (float)INT16_MAX))
			return -1;
		quantized = (int32_t)codes;
		if (codes != (float)quantized)
			return -1;
	}
	for (index = 0U; publish_output && index < sample_count; ++index)
		output[index] = (int16_t)(workspace->output[index] * 32768.0F);
	*input_index = next.input_index;
	*dirty = !!next.dirty;
	return 0;
}

/** @brief Validate one portable F32 output span before exact signed-16 conversion.
 * @param samples Normalized portable output samples.
 * @param sample_count Number of samples to validate.
 * @return Zero when every value is an exact signed-16 PCM code, otherwise nonzero.
 */
static int radio_core_validate_s16_output(const float *samples, size_t sample_count)
{
	size_t index;

	if (sample_count && !samples)
		return -1;
	for (index = 0U; index < sample_count; ++index) {
		float codes = samples[index] * 32768.0F;
		int32_t quantized;

		if (!(codes >= (float)INT16_MIN && codes <= (float)INT16_MAX))
			return -1;
		quantized = (int32_t)codes;
		if (codes != (float)quantized)
			return -1;
	}
	return 0;
}

int urp_radio_core_center_slicer_s16(const int16_t *input, int16_t *centered_output,
				     int16_t *limited_output, size_t sample_count, int32_t limit,
				     int16_t setpoint, int32_t decay_factor,
				     struct rptadv_radio_center_slicer_state *state,
				     struct urp_radio_center_slicer_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_center_slicer_state next;
	uint32_t bounded_samples;
	size_t index;

	if (!state || !workspace || sample_count > workspace->capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples) ||
	    (sample_count && (!input || !centered_output || !limited_output || !workspace->input ||
			      !workspace->centered_output || !workspace->limited_output)))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < sample_count; ++index)
		workspace->input[index] = (float)input[index] / 32768.0F;
	next = *state;
	if (descriptor->radio_center_slicer_f32(workspace->input, workspace->centered_output,
						workspace->limited_output, bounded_samples, limit,
						setpoint, decay_factor, &next) != RPTADV_RADIO_OK ||
	    radio_core_validate_s16_output(workspace->centered_output, sample_count) ||
	    radio_core_validate_s16_output(workspace->limited_output, sample_count))
		return -1;
	for (index = 0U; index < sample_count; ++index) {
		centered_output[index] = (int16_t)(workspace->centered_output[index] * 32768.0F);
		limited_output[index] = (int16_t)(workspace->limited_output[index] * 32768.0F);
	}
	*state = next;
	return 0;
}

int urp_radio_core_deemphasis_integrator_s16(
	const int16_t *input, int16_t *output, size_t sample_count, int16_t output_coefficient,
	int16_t feedback_coefficient, int32_t output_gain,
	struct rptadv_radio_deemphasis_integrator_state *state,
	struct urp_radio_deemphasis_integrator_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_deemphasis_integrator_state next;
	uint32_t bounded_samples;
	size_t index;

	if (!state || !workspace || sample_count > workspace->capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples) ||
	    (sample_count && (!input || !output || !workspace->input || !workspace->output)))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < sample_count; ++index)
		workspace->input[index] = (float)input[index] / 32768.0F;
	next = *state;
	if (descriptor->radio_deemphasis_integrator_f32(
		    workspace->input, workspace->output, bounded_samples, output_coefficient,
		    feedback_coefficient, output_gain, &next) != RPTADV_RADIO_OK ||
	    radio_core_validate_s16_output(workspace->output, sample_count))
		return -1;
	for (index = 0U; index < sample_count; ++index)
		output[index] = (int16_t)(workspace->output[index] * 32768.0F);
	*state = next;
	return 0;
}

int urp_radio_core_fir_mono_s16(const int16_t *input, int16_t *output, size_t sample_count,
				const int16_t *coefficients, int16_t *history, size_t history_count,
				int32_t input_gain, int32_t output_gain, int32_t calc_adjust,
				struct urp_radio_fir_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	uint32_t bounded_samples;
	uint32_t bounded_history;
	size_t index;

	if (!workspace || !calc_adjust || sample_count > workspace->frame_capacity ||
	    history_count > workspace->history_capacity ||
	    radio_core_frame_count(sample_count, &bounded_samples) ||
	    radio_core_frame_count(history_count, &bounded_history) ||
	    (sample_count && (!input || !output || !workspace->input || !workspace->output)) ||
	    (!history_count || !coefficients || !history || !workspace->history))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < sample_count; ++index)
		workspace->input[index] = (float)input[index] / 32768.0F;
	memcpy(workspace->history, history, history_count * sizeof(*history));
	if (descriptor->radio_fir_mono_f32(workspace->input, workspace->output, bounded_samples,
					   workspace->history, bounded_history, coefficients,
					   input_gain, output_gain,
					   calc_adjust) != RPTADV_RADIO_OK ||
	    radio_core_validate_s16_output(workspace->output, sample_count))
		return -1;
	for (index = 0U; index < sample_count; ++index)
		output[index] = (int16_t)(workspace->output[index] * 32768.0F);
	memcpy(history, workspace->history, history_count * sizeof(*history));
	return 0;
}

int urp_radio_core_receive_frontend_s16(
	const int16_t *input, int16_t *baseband_output, size_t baseband_output_capacity,
	uint8_t *carrier_gate, size_t native_frame_count, int16_t *history, size_t history_count,
	const int16_t *baseband_coefficients, int32_t baseband_calc_adjust,
	int32_t baseband_output_gain, const int16_t *noise_coefficients,
	size_t noise_coefficient_count, int32_t noise_divisor, uint32_t decimate,
	uint32_t calibration_window, uint32_t open_level, uint32_t hysteresis,
	struct rptadv_radio_receive_frontend_state *state, size_t *baseband_output_count,
	int *rssi_updated, struct urp_radio_receive_frontend_workspace *workspace)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_receive_frontend_state next;
	uint32_t bounded_native;
	uint32_t bounded_output_capacity;
	uint32_t bounded_history;
	uint32_t bounded_noise;
	uint32_t output_count = 0U;
	uint32_t rssi_completed = 0U;
	size_t index;

	if (baseband_output_count)
		*baseband_output_count = 0U;
	if (rssi_updated)
		*rssi_updated = 0;
	if (!workspace || !state || !baseband_output_count || !rssi_updated || !decimate ||
	    !calibration_window || !baseband_calc_adjust || !noise_divisor ||
	    native_frame_count > workspace->native_frame_capacity ||
	    baseband_output_capacity > workspace->baseband_output_capacity ||
	    history_count > workspace->history_capacity || native_frame_count > SIZE_MAX / 2U ||
	    radio_core_frame_count(native_frame_count, &bounded_native) ||
	    radio_core_frame_count(baseband_output_capacity, &bounded_output_capacity) ||
	    radio_core_frame_count(history_count, &bounded_history) ||
	    radio_core_frame_count(noise_coefficient_count, &bounded_noise) || !history_count ||
	    !noise_coefficient_count || noise_coefficient_count > history_count || !history ||
	    !baseband_coefficients || !noise_coefficients || !workspace->history ||
	    (native_frame_count &&
	     (!input || !baseband_output || !carrier_gate || !workspace->input ||
	      !workspace->baseband_output || !workspace->carrier_gate)))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	for (index = 0U; index < native_frame_count * 2U; ++index)
		workspace->input[index] = (float)input[index] / 32768.0F;
	memcpy(workspace->history, history, history_count * sizeof(*history));
	next = *state;
	if (descriptor->radio_receive_frontend_f32(
		    workspace->input, workspace->baseband_output, bounded_output_capacity,
		    workspace->carrier_gate, bounded_native, bounded_native, workspace->history,
		    bounded_history, baseband_coefficients, baseband_calc_adjust,
		    baseband_output_gain, noise_coefficients, bounded_noise, noise_divisor,
		    decimate, calibration_window, open_level, hysteresis, &next, &output_count,
		    &rssi_completed) != RPTADV_RADIO_OK ||
	    output_count > bounded_output_capacity || rssi_completed > 1U ||
	    radio_core_validate_s16_output(workspace->baseband_output, output_count))
		return -1;
	for (index = 0U; index < native_frame_count; ++index) {
		if (workspace->carrier_gate[index] > 1U)
			return -1;
	}
	for (index = 0U; index < output_count; ++index)
		baseband_output[index] = (int16_t)(workspace->baseband_output[index] * 32768.0F);
	memcpy(carrier_gate, workspace->carrier_gate, native_frame_count * sizeof(*carrier_gate));
	memcpy(history, workspace->history, history_count * sizeof(*history));
	*state = next;
	*baseband_output_count = output_count;
	*rssi_updated = (int)rssi_completed;
	return 0;
}

int urp_radio_core_elapsed_ms(uint32_t *remainder, size_t native_frame_count, int32_t *milliseconds)
{
	const struct rptadv_radio_descriptor *descriptor;
	uint32_t bounded_frames;
	uint32_t next_remainder;
	int32_t next_milliseconds = 0;

	if (milliseconds)
		*milliseconds = 0;
	if (!remainder || !milliseconds ||
	    radio_core_frame_count(native_frame_count, &bounded_frames))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next_remainder = *remainder;
	if (descriptor->radio_elapsed_ms(&next_remainder, bounded_frames, &next_milliseconds) !=
	    RPTADV_RADIO_OK)
		return -1;
	*remainder = next_remainder;
	*milliseconds = next_milliseconds;
	return 0;
}

int urp_radio_core_timer_consume(int32_t *timer, int32_t milliseconds, int32_t *remaining)
{
	const struct rptadv_radio_descriptor *descriptor;
	int32_t next_timer;
	int32_t next_remaining = 0;

	if (remaining)
		*remaining = 0;
	if (!timer || !remaining)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next_timer = *timer;
	if (descriptor->radio_timer_consume(&next_timer, milliseconds, &next_remaining) !=
	    RPTADV_RADIO_OK)
		return -1;
	*timer = next_timer;
	*remaining = next_remaining;
	return 0;
}

int urp_radio_core_signal_mode_advance(const struct rptadv_radio_signal_mode_config *config,
				       const struct rptadv_radio_signal_mode_input *input,
				       struct rptadv_radio_signal_mode_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_signal_mode_state next;

	if (!config || !input || !state ||
	    config->struct_size < sizeof(struct rptadv_radio_signal_mode_config))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_signal_mode_advance(config, input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_ctcss_render_state_advance(
	const struct rptadv_radio_ctcss_render_state_config *config,
	const struct rptadv_radio_ctcss_render_state_input *input,
	struct rptadv_radio_ctcss_render_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_ctcss_render_state next;

	if (!config || !input || !state ||
	    config->struct_size < sizeof(struct rptadv_radio_ctcss_render_state_config))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_ctcss_render_state_advance(config, input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_tx_finish_advance(const struct rptadv_radio_tx_finish_input *input,
				     struct rptadv_radio_tx_finish_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_tx_finish_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_tx_finish_advance(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_tx_finish_continue(const struct rptadv_radio_tx_finish_input *input,
				      struct rptadv_radio_tx_finish_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_tx_finish_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_tx_finish_continue(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_tx_complete(const struct rptadv_radio_tx_complete_config *config,
			       struct rptadv_radio_tx_complete_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_tx_complete_state next;

	if (!config || !state ||
	    config->struct_size < sizeof(struct rptadv_radio_tx_complete_config))
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_tx_complete(config, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_rx_blanking_advance(const struct rptadv_radio_rx_blanking_input *input,
				       struct rptadv_radio_rx_blanking_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_rx_blanking_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_rx_blanking_advance(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_vox_carrier_advance(const struct rptadv_radio_vox_carrier_input *input,
				       struct rptadv_radio_vox_carrier_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_vox_carrier_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_vox_carrier_advance(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_tx_cpu_saver_advance(const struct rptadv_radio_tx_cpu_saver_input *input,
					struct rptadv_radio_tx_cpu_saver_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_tx_cpu_saver_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_tx_cpu_saver_advance(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_rx_cpu_saver_advance(const struct rptadv_radio_rx_cpu_saver_input *input,
					struct rptadv_radio_rx_cpu_saver_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_rx_cpu_saver_state next;

	if (!input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_rx_cpu_saver_advance(input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_dcs_turnoff_advance(const struct rptadv_radio_dcs_turnoff_config *config,
				       const struct rptadv_radio_dcs_turnoff_input *input,
				       struct rptadv_radio_dcs_turnoff_state *state)
{
	const struct rptadv_radio_descriptor *descriptor;
	struct rptadv_radio_dcs_turnoff_state next;

	if (!config || !input || !state)
		return -1;
	descriptor = urp_radio_core_descriptor_get();
	if (!descriptor)
		return -1;
	next = *state;
	if (descriptor->radio_dcs_turnoff_advance(config, input, &next) != RPTADV_RADIO_OK)
		return -1;
	*state = next;
	return 0;
}

int urp_radio_core_render_calibrated_test_tone(struct rptadv_radio *radio, float *program,
					       size_t frame_count, int enabled)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;

	if (!radio || (frame_count && !program) || radio_core_frame_count(frame_count, &bounded) ||
	    !descriptor)
		return -1;
	return descriptor->radio_render_calibrated_test_tone_f32(radio, program, bounded,
								 !!enabled) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_calibrated_test_tone_phase(const struct rptadv_radio *radio,
					      double *phase_radians)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!radio || !phase_radians || !descriptor)
		return -1;
	return descriptor->radio_calibrated_test_tone_phase_radians(radio, phase_radians) ==
			       RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

/** @brief Convert a storage bound into the ABI's unsigned 32-bit capacity.
 * @param capacity Native C storage capacity.
 * @param bounded Receives the ABI-sized capacity.
 * @return Zero on success, or minus one when the capacity cannot be represented.
 */
static int radio_core_capacity(size_t capacity, uint32_t *bounded)
{
	return radio_core_frame_count(capacity, bounded);
}

int urp_radio_core_native_parrot_bind(struct rptadv_radio *radio, float *storage,
				      size_t storage_capacity)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;

	if (!radio || !storage || !storage_capacity || !descriptor ||
	    radio_core_capacity(storage_capacity, &bounded))
		return -1;
	return descriptor->radio_native_parrot_bind_f32(radio, storage, bounded) == RPTADV_RADIO_OK
		       ? 0
		       : -1;
}

int urp_radio_core_native_parrot_reset(struct rptadv_radio *radio)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (!radio || !descriptor)
		return -1;
	return descriptor->radio_native_parrot_reset(radio) == RPTADV_RADIO_OK ? 0 : -1;
}

int urp_radio_core_native_parrot_rx_transition(struct rptadv_radio *radio, int was_keyed,
					       int is_keyed, int *playback_started)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t started = 0U;

	if (playback_started)
		*playback_started = 0;
	if (!radio || !descriptor || !playback_started)
		return -1;
	if (descriptor->radio_native_parrot_rx_transition(radio, !!was_keyed, !!is_keyed,
							  &started) != RPTADV_RADIO_OK)
		return -1;
	*playback_started = !!started;
	return 0;
}

int urp_radio_core_native_parrot_record(struct rptadv_radio *radio, const float *input,
					size_t frame_count, size_t recording_limit,
					size_t *recorded)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded_frames;
	uint32_t bounded_limit;
	uint32_t retained = 0U;

	if (recorded)
		*recorded = 0U;
	if (!radio || (frame_count && !input) || !recorded || !descriptor ||
	    radio_core_frame_count(frame_count, &bounded_frames) ||
	    radio_core_capacity(recording_limit, &bounded_limit))
		return -1;
	if (descriptor->radio_native_parrot_record_f32(radio, input, bounded_frames, bounded_limit,
						       &retained) != RPTADV_RADIO_OK)
		return -1;
	*recorded = retained;
	return 0;
}

int urp_radio_core_native_parrot_play(struct rptadv_radio *radio, float *output, size_t frame_count,
				      size_t *played)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	uint32_t bounded;
	uint32_t copied = 0U;

	if (played)
		*played = 0U;
	if (!radio || (frame_count && !output) || !played || !descriptor ||
	    radio_core_frame_count(frame_count, &bounded))
		return -1;
	if (descriptor->radio_native_parrot_play_f32(radio, output, bounded, &copied) !=
	    RPTADV_RADIO_OK)
		return -1;
	*played = copied;
	return 0;
}

int urp_radio_core_native_parrot_status(const struct rptadv_radio *radio,
					struct rptadv_radio_native_parrot_status *status)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();

	if (status)
		memset(status, 0, sizeof(*status));
	if (!radio || !status || !descriptor)
		return -1;
	return descriptor->radio_native_parrot_status(radio, status) == RPTADV_RADIO_OK ? 0 : -1;
}
