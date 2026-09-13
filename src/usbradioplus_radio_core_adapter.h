/**
 * @file usbradioplus_radio_core_adapter.h
 * @brief Narrow control-plane bridge to the released portable radio core.
 *
 * The Rust core owns portable radio operations.  This C boundary validates
 * its immutable descriptor once during setup and retains the Asterisk-facing
 * compatibility code's ownership of buffers and lifecycle.
 */

#ifndef USBRADIOPLUS_RADIO_CORE_ADAPTER_H
#define USBRADIOPLUS_RADIO_CORE_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include <rptadvradio/rptadvradio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque-to-callers status returned by the appended native-parrot ABI. */
struct rptadv_radio_native_parrot_status;

/** @brief Preallocated canonical-F32 workspace for one legacy delay stage.
 *
 * The compatibility caller owns the stage cursor and the preallocated F32
 * storage. The F32 circular storage is authoritative; no signed-16 history is
 * retained solely for fallback.
 */
struct urp_radio_delay_workspace {
	/** Canonical-F32 conversion storage for one input span. */
	float *input;
	/** Canonical-F32 conversion storage for one output span. */
	float *output;
	/** Caller-owned canonical-F32 circular delay storage. */
	float *storage;
	/** Maximum scalar input or output samples per callback. */
	size_t frame_capacity;
	/** Scalar capacity of @ref storage. */
	size_t storage_capacity;
};

/** @brief Preallocated canonical-F32 workspace for one legacy center slicer.
 *
 * The C compatibility stage owns all detector state. This workspace contains
 * only exact signed-16/F32 conversion spans for its input, centered output,
 * and limited output, so the required shared-core call allocates nothing in
 * the receive callback.
 */
struct urp_radio_center_slicer_workspace {
	/** Canonical-F32 conversion storage for one signed-16 input span. */
	float *input;
	/** Canonical-F32 centered output before the legacy limiter. */
	float *centered_output;
	/** Canonical-F32 output after the legacy limiter. */
	float *limited_output;
	/** Maximum scalar samples in every workspace span. */
	size_t capacity;
};

/** @brief Preallocated canonical-F32 workspace for receiver deemphasis.
 *
 * The compatibility stage retains the signed-32 accumulator and coefficients.
 * This workspace carries only exact signed-16/F32 input and output spans into
 * the required shared-core primitive, so the receive callback never allocates.
 */
struct urp_radio_deemphasis_integrator_workspace {
	/** Canonical-F32 conversion storage for one signed-16 input span. */
	float *input;
	/** Canonical-F32 output after historical signed-16 narrowing. */
	float *output;
	/** Maximum scalar samples in both workspace spans. */
	size_t capacity;
};

/** @brief Preallocated bridge storage for one portable mono FIR call.
 *
 * The legacy stage continues to own its coefficient table and signed-16
 * history. The bridge copies that history into @ref history before calling the
 * F32 primitive, so a rejected call leaves the retained C state untouched. No
 * receive-callback allocation occurs.
 */
struct urp_radio_fir_workspace {
	/** Canonical-F32 conversion storage for one signed-16 input span. */
	float *input;
	/** Canonical-F32 output after historical signed-16 clipping. */
	float *output;
	/** Temporary signed-16 FIR history committed only after success. */
	int16_t *history;
	/** Maximum scalar samples in @ref input and @ref output. */
	size_t frame_capacity;
	/** Maximum signed-16 history entries. */
	size_t history_capacity;
};

/** @brief Preallocated bridge storage for the native receive frontend.
 *
 * The compatibility radio engine retains its signed-16 coefficient table,
 * history, and detector state. These buffers carry a transactional F32
 * request to the required portable core, so a rejected operation leaves the
 * established C frontend eligible to run unchanged.
 */
struct urp_radio_receive_frontend_workspace {
	/** Canonical-F32 stereo conversion storage for native input frames. */
	float *input;
	/** Canonical-F32 decimated mono output storage. */
	float *baseband_output;
	/** Temporary signed-16 history committed only after portable success. */
	int16_t *history;
	/** Temporary native-frame carrier gate committed only after success. */
	uint8_t *carrier_gate;
	/** Native callback frame capacity. */
	size_t native_frame_capacity;
	/** Decimated output capacity. */
	size_t baseband_output_capacity;
	/** Signed-16 history capacity. */
	size_t history_capacity;
};

/** @brief Initialization outcomes for the selected radio-core descriptor. */
enum urp_radio_core_initialize_status {
	/** Every selected operation was validated and published. */
	URP_RADIO_CORE_INITIALIZE_OK = 0,
	/** No descriptor candidate was available from the selected provider. */
	URP_RADIO_CORE_INITIALIZE_UNAVAILABLE = -1,
	/** The candidate declares an incompatible ABI version. */
	URP_RADIO_CORE_INITIALIZE_ABI_MISMATCH = -2,
	/** The candidate omits a selected operation or its descriptor tail. */
	URP_RADIO_CORE_INITIALIZE_INCOMPLETE = -3,
};

/** @brief Validate and publish the complete selected radio-core descriptor.
 * @return One value from @ref urp_radio_core_initialize_status.
 *
 * This control-plane operation rejects an unavailable, ABI-incompatible, or
 * incomplete composition before native audio runs. A failed revalidation
 * retains any earlier validated descriptor. Native audio only acquire-loads
 * the immutable descriptor after successful validation.
 */
int urp_radio_core_initialize(void);

/** @brief Retrieve the validated immutable radio-core descriptor.
 * @return The descriptor after successful initialization, otherwise NULL.
 */
const struct rptadv_radio_descriptor *urp_radio_core_descriptor_get(void);

/** @brief Parse one established receive-audio-source token through the portable core.
 * @param text NUL-terminated symbolic value.
 * @param mode Receives the compatible numeric assignment.
 * @return Zero on success, otherwise nonzero with @p mode unchanged.
 *
 * Configuration parsing can precede explicit native-renderer setup, so this
 * control-plane wrapper validates and publishes the immutable descriptor on
 * first use. It retains the legacy numeric assignment at this boundary.
 */
int urp_radio_core_parse_rx_audio_mode(const char *text, uint32_t *mode);

/** @brief Parse one established carrier-source token through the portable core.
 * @param text NUL-terminated symbolic value.
 * @param source Receives the compatible numeric assignment.
 * @return Zero on success, otherwise nonzero with @p source unchanged.
 */
int urp_radio_core_parse_carrier_source(const char *text, uint32_t *source);

/** @brief Parse one established CTCSS-source token through the portable core.
 * @param text NUL-terminated symbolic value.
 * @param source Receives the compatible numeric assignment.
 * @return Zero on success, otherwise nonzero with @p source unchanged.
 */
int urp_radio_core_parse_ctcss_source(const char *text, uint32_t *source);

/** @brief Parse one established CTCSS turn-off token through the portable core.
 * @param text NUL-terminated symbolic value.
 * @param mode Receives the compatible numeric assignment.
 * @return Zero on success, otherwise nonzero with @p mode unchanged.
 */
int urp_radio_core_parse_tone_off_mode(const char *text, uint32_t *mode);

/** @brief Create one fixed-rate Rust transmitter-renderer context.
 * @param native_sample_rate_hz Stream rate fixed for the context lifetime.
 * @param maximum_frame_count Largest native callback block to render.
 * @param radio Receives the opaque core object on success.
 * @return Zero on success, otherwise nonzero with @p radio cleared.
 */
int urp_radio_core_create(uint32_t native_sample_rate_hz, uint32_t maximum_frame_count,
			  struct rptadv_radio **radio);

/** @brief Destroy one core object created by @ref urp_radio_core_create.
 * @param radio Object to destroy; NULL is accepted.
 */
void urp_radio_core_destroy(struct rptadv_radio *radio);

/** @brief Render one legacy-quantized CTCSS block through the portable core.
 * @param radio Fixed-rate core context that owns the oscillator phase.
 * @param output Writable normalized f32 output, or NULL for zero samples.
 * @param frame_count Number of native mono samples to render.
 * @param frequency_hz Requested CTCSS frequency before legacy quantization.
 * @param peak Normalized oscillator amplitude.
 * @param enabled Nonzero emits CTCSS; zero emits silence without advancing phase.
 * @param phase_shift_degrees One-shot turn-off phase adjustment.
 * @return Zero on success, otherwise nonzero with a silent caller-owned fallback required.
 */
int urp_radio_core_generate_ctcss(struct rptadv_radio *radio, float *output, size_t frame_count,
				  double frequency_hz, float peak, int enabled,
				  double phase_shift_degrees);

/** @brief Render one exact-frequency CTCSS tail block through the portable core.
 * @param radio Fixed-rate core context that owns the oscillator phase.
 * @param output Writable normalized f32 output, or NULL for zero samples.
 * @param frame_count Number of native mono samples to render.
 * @param frequency_hz Exact tail-tone frequency.
 * @param peak Normalized oscillator amplitude.
 * @param enabled Nonzero emits the tail; zero emits silence without advancing phase.
 * @return Zero on success, otherwise nonzero with a silent caller-owned fallback required.
 */
int urp_radio_core_generate_ctcss_tail(struct rptadv_radio *radio, float *output,
				       size_t frame_count, double frequency_hz, float peak,
				       int enabled);

/** @brief Read the portable core's CTCSS oscillator phase for diagnostics.
 * @param radio Fixed-rate core context that owns the oscillator phase.
 * @param phase_radians Receives the phase in radians.
 * @return Zero on success, otherwise nonzero.
 */
int urp_radio_core_ctcss_phase(const struct rptadv_radio *radio, double *phase_radians);

/** @brief Apply the current native DCS transmitter configuration.
 * @param radio Fixed-rate core context that owns the DCS state.
 * @param code Three-octal-digit DCS value, or a negative value to disable it.
 * @param inverted Nonzero selects inverted transmit polarity.
 * @return Zero on success, otherwise nonzero.
 */
int urp_radio_core_configure_dcs(struct rptadv_radio *radio, int code, int inverted);

/** @brief Render one native DCS or DCS turn-off block through the portable core.
 * @param radio Fixed-rate core context that owns the DCS state.
 * @param output Writable normalized f32 output, or NULL for zero samples.
 * @param frame_count Number of native mono samples to render.
 * @param peak Normalized DCS peak amplitude.
 * @param enabled Nonzero emits DCS; zero emits silence without advancing state.
 * @param turnoff Nonzero selects the 134.4 Hz turn-off waveform.
 * @return Zero on success, otherwise nonzero with a silent caller-owned fallback required.
 */
int urp_radio_core_generate_dcs(struct rptadv_radio *radio, float *output, size_t frame_count,
				double peak, int enabled, int turnoff);

/** @brief Apply the current native DCS receiver configuration.
 * @param radio Fixed-rate core context that owns the DCS receiver state.
 * @param code Three-octal-digit DCS value, or a negative value to disable it.
 * @param inverted Nonzero selects inverted receive polarity.
 * @return Zero on success, otherwise nonzero.
 *
 * This is called only at a native callback boundary after a configuration
 * generation changes. It clears receiver qualification without changing the
 * independent DCS transmitter state.
 */
int urp_radio_core_configure_dcs_receive(struct rptadv_radio *radio, int code, int inverted);

/** @brief Decode raw native discriminator PCM through the portable DCS receiver.
 * @param radio Fixed-rate core context that owns DCS receiver state.
 * @param stereo Readable interleaved canonical-F32 ADC PCM.
 * @param frame_count Number of native frames in @p stereo.
 * @param valid Receives nonzero only while the configured code qualifies.
 * @return Zero on success, otherwise nonzero with @p valid cleared.
 *
 * The input is the direct signed-16-to-F32 hardware boundary conversion. It
 * therefore preserves the established left-channel selection and decoder
 * ordering before legacy radio signaling evaluates DCS qualification.
 */
int urp_radio_core_process_dcs_receive(struct rptadv_radio *radio, const float *stereo,
				       size_t frame_count, int *valid);

/** @brief Apply one post-filter CTCSS detector configuration to the portable core.
 * @param radio Fixed-rate core context that owns the detector bank.
 * @param tone_mask One bit for each configured legacy CTCSS table index.
 * @param relax Nonzero selects the established relaxed talk-off behavior.
 * @return Zero on success, otherwise nonzero.
 *
 * The caller invokes this at a native callback boundary whenever its CTCSS
 * receive mapping or relax setting changes. It resets only portable CTCSS
 * qualification and leaves transmit signaling and DCS state unchanged.
 */
int urp_radio_core_configure_ctcss_receive(struct rptadv_radio *radio, uint64_t tone_mask,
					   int relax);

/** @brief Decode filtered mono CTCSS input through the portable core.
 * @param radio Fixed-rate core context that owns the detector bank.
 * @param samples Readable canonical-F32 8 kHz center-slicer PCM.
 * @param sample_count Number of samples in @p samples.
 * @param carrier_detect Current compatibility carrier decision.
 * @param decoded Receives a CTCSS table index or -1 when no tone qualifies.
 * @return Zero on success, otherwise nonzero with @p decoded set to -1.
 *
 * This intentionally accepts the existing post-frontend input at the exact
 * radio-signaling decision point. The surrounding C state retains diagnostic
 * buffers and uses its legacy decoder only when this operation reports error.
 */
int urp_radio_core_process_ctcss_receive(struct rptadv_radio *radio, const float *samples,
					 size_t sample_count, int carrier_detect, int *decoded);

/** @brief Extract delayed left-channel native receive PCM through the portable core.
 * @param radio Fixed-rate core context that bounds the native span.
 * @param stereo Readable interleaved normalized-f32 native stereo input.
 * @param mono Receives the delayed normalized-f32 left channel.
 * @param frame_count Number of native frames to extract.
 * @param delay Caller-owned normalized-f32 circular delay storage, or NULL when disabled.
 * @param delay_frame_count Number of samples in @p delay.
 * @param delay_index In/out circular delay cursor.
 * @param peak Receives the undelayed input peak on the canonical f32 scale.
 * @param rail_samples Receives the signed-16-equivalent input rail count.
 * @return Zero on success, otherwise nonzero with all outputs reset safely.
 *
 * The adapter intentionally retains the Asterisk-facing statistics units at
 * this boundary.  It does not apply filtering, gain, qualification, or DSP.
 */
int urp_radio_core_extract_receive(const struct rptadv_radio *radio, const float *stereo,
				   float *mono, size_t frame_count, float *delay,
				   size_t delay_frame_count, unsigned int *delay_index, float *peak,
				   unsigned long *rail_samples);

/** @brief Measure legacy signed-16 raw PCM through the portable F32 meter.
 * @param samples Readable raw signed-16 PCM samples, or NULL for zero samples.
 * @param sample_count Scalar samples in @p samples, not audio frames.
 * @param channels One for 48 kHz mono or two for interleaved 48 kHz stereo.
 * @param statistics Caller-owned ASL3-compatible retained meter state.
 * @param f32_workspace Preallocated canonical-F32 storage for @p sample_count samples.
 * @param f32_workspace_capacity Scalar capacity of @p f32_workspace.
 * @param clipping Receives nonzero when the historical clip condition occurs.
 * @return Zero on success, otherwise nonzero with @p clipping cleared.
 *
 * Every signed-16 sample maps exactly to canonical F32 by division by
 * 32768.0F before calling the portable primitive. The API deliberately does
 * not require a radio object because legacy meter state is caller-owned. The
 * selected descriptor is validated to include this meter before the renderer
 * is created, so meter ownership never silently changes at run time.
 */
int urp_radio_core_measure_audio_s16(const int16_t *samples, size_t sample_count,
				     unsigned int channels,
				     struct rptadv_radio_audio_statistics *statistics,
				     float *f32_workspace, size_t f32_workspace_capacity,
				     int *clipping);

/** @brief Advance one sample-clocked MICOR noise-squelch comparator through the portable core.
 * @param state Caller-owned persistent compatibility state.
 * @param squelched Previous comparator output; nonzero means closed.
 * @param sample_power Squared noise-filter sample in established calibration units.
 * @param open_level Calibrated squelch threshold.
 * @param hysteresis Additional margin while the receiver is open.
 * @param closed Receives the next comparator output as zero or one.
 * @return Zero on a successful portable update, otherwise nonzero.
 *
 * The selected descriptor is validated to include this primitive. A rejected
 * request leaves caller-owned state unchanged for existing failure handling.
 */
int urp_radio_core_micor_squelch_update(struct rptadv_radio_micor_squelch_state *state,
					int squelched, double sample_power, uint32_t open_level,
					uint32_t hysteresis, int *closed);

/** @brief Measure a signed-16 compatibility span through the portable F32 envelope meter.
 * @param input Readable signed-16 scalar PCM, or NULL for zero samples.
 * @param output Optional writable signed-16 half-peak samples.
 * @param sample_count Number of scalar samples to measure.
 * @param decay_factor Legacy envelope decay interval in samples.
 * @param threshold Legacy PCM-code comparator threshold.
 * @param state In/out caller-owned extrema and decay counters.
 * @param f32_input Preallocated canonical-F32 input workspace.
 * @param f32_output Preallocated canonical-F32 output workspace when @p output is non-NULL.
 * @param f32_capacity Scalar capacity of each F32 workspace.
 * @param comparator Receives the final threshold result as zero or one.
 * @return Zero after a portable update, otherwise nonzero with all caller state unchanged.
 *
 * The adapter makes the exact signed-16/F32 boundary conversion before the
 * required shared-core call. A rejected request leaves caller-owned state
 * unchanged without changing VOX or tuning results.
 */
int urp_radio_core_measure_envelope_s16(const int16_t *input, int16_t *output, size_t sample_count,
					int32_t decay_factor, int16_t threshold,
					struct rptadv_radio_envelope_state *state, float *f32_input,
					float *f32_output, size_t f32_capacity, int *comparator);

/** @brief Process a legacy signed-16 delay stage through the portable F32 core.
 * @param input Readable signed-16 input, required for an enabled stage.
 * @param output Writable signed-16 stage output.
 * @param sample_count Number of scalar samples in this callback span.
 * @param storage_capacity Active scalar length in @p workspace storage.
 * @param lead Input-to-output delay in samples.
 * @param input_index In/out existing C circular-storage cursor.
 * @param dirty In/out existing C reset indicator.
 * @param enabled Existing stage enable value.
 * @param outzero Existing forced-silent stage value.
 * @param workspace Preallocated F32 conversion and circular storage.
 * @return Zero after a portable update, otherwise nonzero with caller state unchanged.
 *
 * No callback-time allocation occurs. Input conversion completes before the
 * shared call, preserving a legitimate in-place signed-16 input/output alias.
 */
int urp_radio_core_delay_line_s16(const int16_t *input, int16_t *output, size_t sample_count,
				  size_t storage_capacity, uint32_t lead, uint32_t *input_index,
				  unsigned int *dirty, unsigned int enabled, unsigned int outzero,
				  struct urp_radio_delay_workspace *workspace);

/** @brief Center and limit a legacy signed-16 low-speed-data stage through the portable core.
 * @param input Readable signed-16 detector PCM.
 * @param centered_output Writable signed-16 centered PCM.
 * @param limited_output Writable signed-16 centered-and-limited PCM.
 * @param sample_count Number of scalar samples in the active callback span.
 * @param limit Existing signed-PCM limiter magnitude.
 * @param setpoint Existing min/max tracking set point.
 * @param decay_factor Existing per-sample extrema discharge amount.
 * @param state In/out caller-owned C-compatible center-slicer state.
 * @param workspace Preallocated canonical-F32 conversion spans.
 * @return Zero after a portable update, otherwise nonzero with C fallback required.
 *
 * The bridge validates both F32 outputs before changing C output or state, so
 * the historical stage can safely run unchanged after a rejected call.
 */
int urp_radio_core_center_slicer_s16(const int16_t *input, int16_t *centered_output,
				     int16_t *limited_output, size_t sample_count, int32_t limit,
				     int16_t setpoint, int32_t decay_factor,
				     struct rptadv_radio_center_slicer_state *state,
				     struct urp_radio_center_slicer_workspace *workspace);

/** @brief Process legacy signed-16 receiver deemphasis through the portable core.
 * @param input Readable signed-16 receiver PCM, or NULL for zero samples.
 * @param output Writable signed-16 receiver PCM after compatibility narrowing.
 * @param sample_count Number of scalar samples in the active callback span.
 * @param output_coefficient Historical feed-forward coefficient.
 * @param feedback_coefficient Historical recursive feedback coefficient.
 * @param output_gain Historical Q8 output gain.
 * @param state In/out caller-owned recursive signed-32 accumulator.
 * @param workspace Preallocated canonical-F32 conversion spans.
 * @return Zero after a portable update, otherwise nonzero with C fallback required.
 *
 * Both input conversion and output validation complete before C output or
 * state changes, preserving the retained recursive-filter fallback exactly.
 */
int urp_radio_core_deemphasis_integrator_s16(
	const int16_t *input, int16_t *output, size_t sample_count, int16_t output_coefficient,
	int16_t feedback_coefficient, int32_t output_gain,
	struct rptadv_radio_deemphasis_integrator_state *state,
	struct urp_radio_deemphasis_integrator_workspace *workspace);

/** @brief Process a legacy mono, unit-rate FIR through the portable core.
 * @param input Readable signed-16 scalar PCM.
 * @param output Writable signed-16 scalar PCM.
 * @param sample_count Number of scalar samples in the active callback span.
 * @param coefficients Readable signed-16 FIR coefficient table.
 * @param history In/out signed-16 FIR history.
 * @param history_count Active coefficient and history count.
 * @param input_gain Historical Q8 input gain.
 * @param output_gain Historical post-accumulator Q8 output gain.
 * @param calc_adjust Historical FIR normalization divisor.
 * @param workspace Preallocated conversion and temporary history storage.
 * @return Zero after portable processing, otherwise nonzero with C state unchanged.
 *
 * This narrow bridge deliberately excludes legacy interpolation, routed
 * output mixing, and amplitude detection. Its caller retains those uncommon
 * compatibility shapes in C. A rejected portable call changes neither output
 * nor history, so the exact C fallback remains valid.
 */
int urp_radio_core_fir_mono_s16(const int16_t *input, int16_t *output, size_t sample_count,
				const int16_t *coefficients, int16_t *history, size_t history_count,
				int32_t input_gain, int32_t output_gain, int32_t calc_adjust,
				struct urp_radio_fir_workspace *workspace);

/** @brief Process the ordinary native DSP-squelch frontend through the portable core.
 * @param input Readable interleaved native signed-16 stereo PCM.
 * @param baseband_output Writable decimated signed-16 mono baseband PCM.
 * @param baseband_output_capacity Scalar capacity of @p baseband_output.
 * @param carrier_gate Writable per-native-frame carrier gate; one means open.
 * @param native_frame_count Native stereo frames in @p input.
 * @param history In/out signed-16 frontend history.
 * @param history_count Coefficient and history entry count.
 * @param baseband_coefficients Readable signed-16 frontend low-pass table.
 * @param baseband_calc_adjust Historical frontend normalization divisor.
 * @param baseband_output_gain Historical Q8 frontend output gain.
 * @param noise_coefficients Readable signed-16 discriminator-noise FIR table.
 * @param noise_coefficient_count Entry count in @p noise_coefficients.
 * @param noise_divisor Historical noise-filter normalization divisor.
 * @param decimate Native samples per emitted baseband sample.
 * @param calibration_window Fixed native RSSI sample window.
 * @param open_level DSP noise-squelch threshold.
 * @param hysteresis DSP noise-squelch hysteresis.
 * @param state In/out caller-owned frontend state.
 * @param baseband_output_count Receives emitted baseband sample count.
 * @param rssi_updated Receives nonzero after an RSSI window completes.
 * @param workspace Preallocated conversion, history, and gate workspace.
 * @return Zero after portable processing; nonzero with all C state unchanged.
 *
 * The C compatibility engine retains VOX and diagnostic frontend modes.
 * Invalid F32 output or a rejected request leaves the caller's PCM, history, gates, and state
 * untouched so it may invoke the retained C implementation exactly once.
 */
int urp_radio_core_receive_frontend_s16(
	const int16_t *input, int16_t *baseband_output, size_t baseband_output_capacity,
	uint8_t *carrier_gate, size_t native_frame_count, int16_t *history, size_t history_count,
	const int16_t *baseband_coefficients, int32_t baseband_calc_adjust,
	int32_t baseband_output_gain, const int16_t *noise_coefficients,
	size_t noise_coefficient_count, int32_t noise_divisor, uint32_t decimate,
	uint32_t calibration_window, uint32_t open_level, uint32_t hysteresis,
	struct rptadv_radio_receive_frontend_state *state, size_t *baseband_output_count,
	int *rssi_updated, struct urp_radio_receive_frontend_workspace *workspace);

/** @brief Convert one native PCM span to elapsed whole milliseconds in the portable core.
 * @param remainder In/out sub-millisecond native-frame remainder.
 * @param native_frame_count Native PCM frames in this callback span.
 * @param milliseconds Receives whole elapsed milliseconds.
 * @return Zero after a portable update, otherwise nonzero with C fallback required.
 *
 * This bridge migrates only the sample-clocked timer arithmetic. A rejected
 * call leaves @p remainder unchanged, allowing the retained C calculation to
 * preserve compatibility exactly.
 */
int urp_radio_core_elapsed_ms(uint32_t *remainder, size_t native_frame_count,
			      int32_t *milliseconds);

/** @brief Consume one compatibility timer through the portable core.
 * @param timer In/out timer in milliseconds.
 * @param milliseconds Elapsed duration to consume.
 * @param remaining Receives duration remaining after timer expiry.
 * @return Zero after a portable update, otherwise nonzero with C fallback required.
 *
 * The bridge validates every scalar before it publishes caller state, so a
 * rejected operation cannot perturb transmitter or receiver signaling timing.
 */
int urp_radio_core_timer_consume(int32_t *timer, int32_t milliseconds, int32_t *remaining);

/** @brief Advance the narrow legacy CTCSS/DCS signaling-mode resolver in Rust.
 * @param config Immutable parsed compatibility configuration.
 * @param input One elapsed-time and decoder snapshot.
 * @param state In/out caller-owned signaling-mode state.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * This append-only operation owns only the receive signaling-mode hold and
 * CTCSS transmit-selection decision.  The channel's transmitter state machine
 * remains in the compatibility adapter until it can move as one tested unit.
 * A rejected descriptor operation leaves @p state untouched so the
 * established C resolver can run unchanged.
 */
int urp_radio_core_signal_mode_advance(const struct rptadv_radio_signal_mode_config *config,
				       const struct rptadv_radio_signal_mode_input *input,
				       struct rptadv_radio_signal_mode_state *state);

/** @brief Advance the final legacy CTCSS render-control state in Rust.
 * @param config Immutable turn-off parameters selected by the C transmitter state machine.
 * @param input Elapsed native PCM duration for this callback span.
 * @param state In/out caller-owned CTCSS oscillator control state.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * The operation starts, turns off, or disables an already selected CTCSS
 * source. It deliberately does not select a tone, key PTT, generate PCM, or
 * change DCS state. A rejected descriptor operation leaves
 * @p state untouched so the established C transition remains the fallback.
 */
int urp_radio_core_ctcss_render_state_advance(
	const struct rptadv_radio_ctcss_render_state_config *config,
	const struct rptadv_radio_ctcss_render_state_input *input,
	struct rptadv_radio_ctcss_render_state *state);

/** @brief Enter the normal legacy transmitter finishing drain through Rust.
 * @param input Elapsed native PCM duration for the entry callback.
 * @param state In/out caller-owned transmitter finishing state.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * The narrow operation owns the ordinary fixed 80 ms output drain only. It
 * does not inspect PTT, select CTCSS/DCS signaling, render PCM, or perform
 * device I/O. A rejected descriptor operation leaves
 * @p state untouched so the retained C helper remains the exact fallback.
 */
int urp_radio_core_tx_finish_advance(const struct rptadv_radio_tx_finish_input *input,
				     struct rptadv_radio_tx_finish_state *state);

/** @brief Continue a legacy transmitter finishing drain through Rust.
 * @param input Elapsed native PCM duration for this callback.
 * @param state In/out caller-owned transmitter finishing state.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * This operation owns only an already-entered finishing interval, including
 * the legacy timer reseed rule used by restored state and the 55 Hz tail. A
 * rejected descriptor operation leaves @p state untouched so
 * the retained C continuation remains the exact fallback.
 */
int urp_radio_core_tx_finish_continue(const struct rptadv_radio_tx_finish_input *input,
				      struct rptadv_radio_tx_finish_state *state);

/** @brief Apply post-drain scalar transmitter cleanup through Rust.
 * @param config Immutable receiver-blanking configuration.
 * @param state In/out caller-owned completion state.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * This narrow operation clears logical PTT, requests CTCSS disable, arms
 * receiver blanking, returns the transmitter to idle, and marks CTCSS ready.
 * It does not perform device I/O, clear compatibility display strings, select
 * signaling, or generate PCM. A rejected descriptor operation leaves @p state
 * untouched so the retained C cleanup is exact.
 */
int urp_radio_core_tx_complete(const struct rptadv_radio_tx_complete_config *config,
			       struct rptadv_radio_tx_complete_state *state);

/** @brief Advance pure post-transmit receive-blanking timing through the core.
 * \param input Elapsed duration, native span, and pre-call sample remainder.
 * \param state Caller-owned blanking state committed only on success.
 * \return Zero on success; nonzero selects the exact C compatibility fallback.
 *
 * The compatibility caller retains signed-16 capture PCM ownership and mutes
 * the returned prefix immediately before its receive frontend.  It must
 * advance its fractional remainder exactly once before this call.
 */
int urp_radio_core_rx_blanking_advance(const struct rptadv_radio_rx_blanking_input *input,
				       struct rptadv_radio_rx_blanking_state *state);

/** @brief Advance one legacy VOX carrier-hang decision through the portable core.
 * @param input Detector, configured hang, and elapsed-time snapshot.
 * @param state Caller-owned timer and carrier decision committed only on success.
 * @return Zero on success; nonzero selects the exact C compatibility fallback.
 *
 * The portable operation deliberately preserves the legacy ordering where a
 * positive pre-consume timer reports carrier for the current callback even
 * when its elapsed duration expires the timer. It owns no PCM, device, or
 * stage state; a rejected operation leaves @p state untouched.
 */
int urp_radio_core_vox_carrier_advance(const struct rptadv_radio_vox_carrier_input *input,
				       struct rptadv_radio_vox_carrier_state *state);

/**
 * \brief Advance one legacy transmitter CPU-saver halt decision through the portable core.
 * \param input Saver, PTT, and idle-state snapshot.
 * \param state Caller-owned halt state committed only on success.
 * \return Zero on success; nonzero selects the exact C compatibility fallback.
 *
 * This operation owns only the scalar CPU-saver decision. It has no PCM,
 * renderer, device, or signaling ownership, so a rejected operation leaves
 * the compatibility state untouched for the retained C path.
 */
int urp_radio_core_tx_cpu_saver_advance(const struct rptadv_radio_tx_cpu_saver_input *input,
					struct rptadv_radio_tx_cpu_saver_state *state);

/**
 * \brief Advance one legacy receiver CPU-saver transition through the portable core.
 * \param input Saver, carrier, signaling, and PTT snapshot.
 * \param state Caller-owned halt/action result committed only on success.
 * \return Zero on success; nonzero selects the exact C compatibility fallback.
 *
 * This operation owns only the scalar halt predicate and transition label.
 * The compatibility caller retains the historical HPF and
 * deemphasis enable writes immediately before the receive frontend.
 */
int urp_radio_core_rx_cpu_saver_advance(const struct rptadv_radio_rx_cpu_saver_input *input,
					struct rptadv_radio_rx_cpu_saver_state *state);

/** @brief Advance one selected DCS transmitter turn-off transition through Rust.
 * @param config Immutable selected DCS turn-off duration.
 * @param input Elapsed duration, PTT, and new-tail selection snapshot.
 * @param state In/out compatibility ACTIVE/TOC state and finishing request.
 * @return Zero after a transactional portable update, otherwise nonzero.
 *
 * This append-only operation owns only scalar timer/state arithmetic. The C
 * compatibility state machine retains DCS selection, waveform generation,
 * PTT, hardware, and its existing finishing helper on success and fallback.
 */
int urp_radio_core_dcs_turnoff_advance(const struct rptadv_radio_dcs_turnoff_config *config,
				       const struct rptadv_radio_dcs_turnoff_input *input,
				       struct rptadv_radio_dcs_turnoff_state *state);

/** @brief Replace one final native program block with the calibrated 1 kHz source.
 * @param radio Fixed-rate core context that owns the calibration oscillator phase.
 * @param program Writable canonical f32 mono program workspace.
 * @param frame_count Number of native samples in @p program.
 * @param enabled Nonzero replaces @p program; zero preserves it and resets phase.
 * @return Zero on success, otherwise nonzero with the caller retaining safe output.
 *
 * The source is generated after the configured final FFmpeg graph, exactly as
 * the established tuning procedure requires.  Calling this when disabled is
 * intentional: it resets the oscillator at the same block boundary as the
 * former C implementation.
 */

int urp_radio_core_render_calibrated_test_tone(struct rptadv_radio *radio, float *program,
					       size_t frame_count, int enabled);

/** @brief Read the calibrated test-tone oscillator phase for existing diagnostics.
 * @param radio Fixed-rate core context that owns the oscillator.
 * @param phase_radians Receives the current phase in radians.
 * @return Zero on success, otherwise nonzero.
 */
int urp_radio_core_calibrated_test_tone_phase(const struct rptadv_radio *radio,
					      double *phase_radians);

/** @brief Bind preallocated F32 recording storage to one native-parrot core.
 * @param radio Fixed-rate core context that owns callback-local parrot cursors.
 * @param storage Stable writable canonical-F32 recording storage.
 * @param storage_capacity Number of allocated samples in @p storage.
 * @return Zero on success, otherwise nonzero with no storage ownership transfer.
 *
 * The caller binds only while idle and retains the storage until the core is
 * destroyed.  The function allocates nothing and runs during setup.
 */
int urp_radio_core_native_parrot_bind(struct rptadv_radio *radio, float *storage,
				      size_t storage_capacity);

/** @brief Clear native-parrot cursors while retaining caller-owned storage.
 * @param radio Fixed-rate core context that owns callback-local parrot state.
 * @return Zero on success, otherwise nonzero.
 */
int urp_radio_core_native_parrot_reset(struct rptadv_radio *radio);

/** @brief Apply one receiver-key transition to native-parrot state.
 * @param radio Fixed-rate core context that owns callback-local parrot state.
 * @param was_keyed Previous receiver qualification.
 * @param is_keyed Current receiver qualification.
 * @param playback_started Receives nonzero when unkeying starts playback.
 * @return Zero on success, otherwise nonzero with @p playback_started cleared.
 */
int urp_radio_core_native_parrot_rx_transition(struct rptadv_radio *radio, int was_keyed,
					       int is_keyed, int *playback_started);

/** @brief Record a bounded canonical-F32 native audio span for parrot playback.
 * @param radio Fixed-rate core context that owns callback-local parrot state.
 * @param input Readable canonical-F32 native samples.
 * @param frame_count Number of samples available at @p input.
 * @param recording_limit Maximum samples to retain for this recording.
 * @param recorded Receives samples retained from this call.
 * @return Zero on success, otherwise nonzero with @p recorded cleared.
 */
int urp_radio_core_native_parrot_record(struct rptadv_radio *radio, const float *input,
					size_t frame_count, size_t recording_limit,
					size_t *recorded);

/** @brief Copy one native-parrot playback span into canonical-F32 storage.
 * @param radio Fixed-rate core context that owns callback-local parrot state.
 * @param output Writable canonical-F32 native samples.
 * @param frame_count Number of samples available at @p output.
 * @param played Receives samples copied to @p output.
 * @return Zero on success, otherwise nonzero with @p played cleared.
 *
 * Samples beyond @p played remain unchanged; callers initialize their complete
 * output span before this bounded copy, preserving the legacy behavior.
 */
int urp_radio_core_native_parrot_play(struct rptadv_radio *radio, float *output, size_t frame_count,
				      size_t *played);

/** @brief Copy a callback-local native-parrot status snapshot.
 * @param radio Fixed-rate core context that owns callback-local parrot state.
 * @param status Receives current cursors and flags.
 * @return Zero on success, otherwise nonzero with @p status cleared.
 */
int urp_radio_core_native_parrot_status(const struct rptadv_radio *radio,
					struct rptadv_radio_native_parrot_status *status);

#ifdef __cplusplus
}
#endif

#endif
