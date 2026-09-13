/** @file
 * @brief Native renderer lifecycle, decoder, and released-provider failure regressions.
 */

/** @brief One selected shared operation fails; all other operations retain real behavior. */
static const char *native_release_failure;
static unsigned int native_release_failure_hits;
static int native_release_hold_signaling, native_release_force_parrot;
static unsigned int native_release_reserve_failure, native_release_reserve_calls;

/** @brief Real urp_radio_core_create behind the fault injection boundary. */
extern int __real_urp_radio_core_create(uint32_t native_sample_rate_hz,
					uint32_t maximum_frame_count, struct rptadv_radio **radio);
/** @brief Inject urp_radio_core_create failure only for the selected no-hardware scenario. */
int __wrap_urp_radio_core_create(uint32_t native_sample_rate_hz, uint32_t maximum_frame_count,
				 struct rptadv_radio **radio)
{
	if (native_release_failure && !strcmp(native_release_failure, "urp_radio_core_create")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_create(native_sample_rate_hz, maximum_frame_count, radio);
}

/** @brief Real urp_radio_core_generate_ctcss behind the fault injection boundary. */
extern int __real_urp_radio_core_generate_ctcss(struct rptadv_radio *radio, float *output,
						size_t frame_count, double frequency_hz, float peak,
						int enabled, double phase_shift_degrees);
/** @brief Inject urp_radio_core_generate_ctcss failure only for the selected no-hardware scenario.
 */
int __wrap_urp_radio_core_generate_ctcss(struct rptadv_radio *radio, float *output,
					 size_t frame_count, double frequency_hz, float peak,
					 int enabled, double phase_shift_degrees)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_generate_ctcss")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_generate_ctcss(radio, output, frame_count, frequency_hz, peak,
						    enabled, phase_shift_degrees);
}

/** @brief Real urp_radio_core_generate_ctcss_tail behind the fault injection boundary. */
extern int __real_urp_radio_core_generate_ctcss_tail(struct rptadv_radio *radio, float *output,
						     size_t frame_count, double frequency_hz,
						     float peak, int enabled);
/** @brief Inject urp_radio_core_generate_ctcss_tail failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_generate_ctcss_tail(struct rptadv_radio *radio, float *output,
					      size_t frame_count, double frequency_hz, float peak,
					      int enabled)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_generate_ctcss_tail")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_generate_ctcss_tail(radio, output, frame_count, frequency_hz,
							 peak, enabled);
}

/** @brief Real urp_radio_core_ctcss_phase behind the fault injection boundary. */
extern int __real_urp_radio_core_ctcss_phase(const struct rptadv_radio *radio,
					     double *phase_radians);
/** @brief Inject urp_radio_core_ctcss_phase failure only for the selected no-hardware scenario. */
int __wrap_urp_radio_core_ctcss_phase(const struct rptadv_radio *radio, double *phase_radians)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_ctcss_phase")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_ctcss_phase(radio, phase_radians);
}

/** @brief Real urp_radio_core_configure_dcs behind the fault injection boundary. */
extern int __real_urp_radio_core_configure_dcs(struct rptadv_radio *radio, int code, int inverted);
/** @brief Inject urp_radio_core_configure_dcs failure only for the selected no-hardware scenario.
 */
int __wrap_urp_radio_core_configure_dcs(struct rptadv_radio *radio, int code, int inverted)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_configure_dcs")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_configure_dcs(radio, code, inverted);
}

/** @brief Real urp_radio_core_generate_dcs behind the fault injection boundary. */
extern int __real_urp_radio_core_generate_dcs(struct rptadv_radio *radio, float *output,
					      size_t frame_count, double peak, int enabled,
					      int turnoff);
/** @brief Inject urp_radio_core_generate_dcs failure only for the selected no-hardware scenario. */
int __wrap_urp_radio_core_generate_dcs(struct rptadv_radio *radio, float *output,
				       size_t frame_count, double peak, int enabled, int turnoff)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_generate_dcs")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_generate_dcs(radio, output, frame_count, peak, enabled,
						  turnoff);
}

/** @brief Real urp_radio_core_configure_dcs_receive behind the fault injection boundary. */
extern int __real_urp_radio_core_configure_dcs_receive(struct rptadv_radio *radio, int code,
						       int inverted);
/** @brief Inject urp_radio_core_configure_dcs_receive failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_configure_dcs_receive(struct rptadv_radio *radio, int code, int inverted)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_configure_dcs_receive")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_configure_dcs_receive(radio, code, inverted);
}

/** @brief Real urp_radio_core_configure_ctcss_receive behind the fault injection boundary. */
extern int __real_urp_radio_core_configure_ctcss_receive(struct rptadv_radio *radio,
							 uint64_t tone_mask, int relax);
/** @brief Inject urp_radio_core_configure_ctcss_receive failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_configure_ctcss_receive(struct rptadv_radio *radio, uint64_t tone_mask,
						  int relax)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_configure_ctcss_receive")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_configure_ctcss_receive(radio, tone_mask, relax);
}

/** @brief Real urp_radio_core_extract_receive behind the fault injection boundary. */
extern int __real_urp_radio_core_extract_receive(const struct rptadv_radio *radio,
						 const float *stereo, float *mono,
						 size_t frame_count, float *delay,
						 size_t delay_frame_count,
						 unsigned int *delay_index, float *peak,
						 unsigned long *rail_samples);
/** @brief Inject urp_radio_core_extract_receive failure only for the selected no-hardware scenario.
 */
int __wrap_urp_radio_core_extract_receive(const struct rptadv_radio *radio, const float *stereo,
					  float *mono, size_t frame_count, float *delay,
					  size_t delay_frame_count, unsigned int *delay_index,
					  float *peak, unsigned long *rail_samples)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_extract_receive")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_extract_receive(radio, stereo, mono, frame_count, delay,
						     delay_frame_count, delay_index, peak,
						     rail_samples);
}

/** @brief Real urp_radio_core_render_calibrated_test_tone behind the fault injection boundary. */
extern int __real_urp_radio_core_render_calibrated_test_tone(struct rptadv_radio *radio,
							     float *program, size_t frame_count,
							     int enabled);
/** @brief Inject urp_radio_core_render_calibrated_test_tone failure only for the selected
 * no-hardware scenario. */
int __wrap_urp_radio_core_render_calibrated_test_tone(struct rptadv_radio *radio, float *program,
						      size_t frame_count, int enabled)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_render_calibrated_test_tone")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_render_calibrated_test_tone(radio, program, frame_count,
								 enabled);
}

/** @brief Real urp_radio_core_calibrated_test_tone_phase behind the fault injection boundary. */
extern int __real_urp_radio_core_calibrated_test_tone_phase(const struct rptadv_radio *radio,
							    double *phase_radians);
/** @brief Inject urp_radio_core_calibrated_test_tone_phase failure only for the selected
 * no-hardware scenario. */
int __wrap_urp_radio_core_calibrated_test_tone_phase(const struct rptadv_radio *radio,
						     double *phase_radians)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_calibrated_test_tone_phase")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_calibrated_test_tone_phase(radio, phase_radians);
}

/** @brief Real urp_radio_core_native_parrot_bind behind the fault injection boundary. */
extern int __real_urp_radio_core_native_parrot_bind(struct rptadv_radio *radio, float *storage,
						    size_t storage_capacity);
/** @brief Inject urp_radio_core_native_parrot_bind failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_native_parrot_bind(struct rptadv_radio *radio, float *storage,
					     size_t storage_capacity)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_native_parrot_bind")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_native_parrot_bind(radio, storage, storage_capacity);
}

/** @brief Real urp_radio_core_native_parrot_rx_transition behind the fault injection boundary. */
extern int __real_urp_radio_core_native_parrot_rx_transition(struct rptadv_radio *radio,
							     int was_keyed, int is_keyed,
							     int *playback_started);
/** @brief Inject urp_radio_core_native_parrot_rx_transition failure only for the selected
 * no-hardware scenario. */
int __wrap_urp_radio_core_native_parrot_rx_transition(struct rptadv_radio *radio, int was_keyed,
						      int is_keyed, int *playback_started)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_native_parrot_rx_transition")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_native_parrot_rx_transition(radio, was_keyed, is_keyed,
								 playback_started);
}

/** @brief Real urp_radio_core_native_parrot_play behind the fault injection boundary. */
extern int __real_urp_radio_core_native_parrot_play(struct rptadv_radio *radio, float *output,
						    size_t frame_count, size_t *played);
/** @brief Inject urp_radio_core_native_parrot_play failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_native_parrot_play(struct rptadv_radio *radio, float *output,
					     size_t frame_count, size_t *played)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_native_parrot_play")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_core_native_parrot_play(radio, output, frame_count, played);
}

/** @brief Real urp_radio_core_native_parrot_status behind the fault injection boundary. */
extern int
__real_urp_radio_core_native_parrot_status(const struct rptadv_radio *radio,
					   struct rptadv_radio_native_parrot_status *status);
/** @brief Inject urp_radio_core_native_parrot_status failure only for the selected no-hardware
 * scenario. */
int __wrap_urp_radio_core_native_parrot_status(const struct rptadv_radio *radio,
					       struct rptadv_radio_native_parrot_status *status)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_core_native_parrot_status")) {
		native_release_failure_hits++;
		return -1;
	}
	int result = __real_urp_radio_core_native_parrot_status(radio, status);
	if (!result && native_release_force_parrot)
		status->playing = 1U;
	return result;
}

/** @brief Real urp_native_repeat_initialize behind the fault injection boundary. */
extern int __real_urp_native_repeat_initialize(void);
/** @brief Inject urp_native_repeat_initialize failure only for the selected no-hardware scenario.
 */
int __wrap_urp_native_repeat_initialize(void)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_native_repeat_initialize")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_native_repeat_initialize();
}

/** @brief Real urp_render_transmit_block behind the fault injection boundary. */
extern int __real_urp_render_transmit_block(
	const struct rptadv_radio *radio, const double *program, const float *ctcss,
	const float *dcs, size_t count, enum urp_tx_output_mode output_a,
	enum urp_tx_output_mode output_b, double ctcss_peak_a, double ctcss_bias_a,
	double ctcss_peak_b, double ctcss_bias_b, struct urp_transmit_render_workspace *workspace,
	short *stereo, short *meter_stereo, unsigned long *rail_samples);
/** @brief Inject urp_render_transmit_block failure only for the selected no-hardware scenario. */
int __wrap_urp_render_transmit_block(const struct rptadv_radio *radio, const double *program,
				     const float *ctcss, const float *dcs, size_t count,
				     enum urp_tx_output_mode output_a,
				     enum urp_tx_output_mode output_b, double ctcss_peak_a,
				     double ctcss_bias_a, double ctcss_peak_b, double ctcss_bias_b,
				     struct urp_transmit_render_workspace *workspace, short *stereo,
				     short *meter_stereo, unsigned long *rail_samples)
{
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_render_transmit_block")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_render_transmit_block(
		radio, program, ctcss, dcs, count, output_a, output_b, ctcss_peak_a, ctcss_bias_a,
		ctcss_peak_b, ctcss_bias_b, workspace, stereo, meter_stereo, rail_samples);
}

/** @brief Real urp_src_reserve behind the fault injection boundary. */
extern int __real_urp_src_reserve(struct urp_src *src, size_t input_capacity,
				  size_t output_capacity);
/** @brief Inject urp_src_reserve failure only for the selected no-hardware scenario. */
int __wrap_urp_src_reserve(struct urp_src *src, size_t input_capacity, size_t output_capacity)
{
	if (native_release_reserve_failure &&
	    ++native_release_reserve_calls == native_release_reserve_failure)
		return -1;
	if (native_release_failure && !strcmp(native_release_failure, "urp_src_reserve")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_src_reserve(src, input_capacity, output_capacity);
}

/** @brief Real urp_radio_process_native_timed behind the fault injection boundary. */
extern i16 __real_urp_radio_process_native_timed(urp_radio_state *radio, i16 *input, i16 *outputrx,
						 i16 *outputtx, size_t native_frame_count,
						 int advance_tx);
/** @brief Inject urp_radio_process_native_timed failure only for the selected no-hardware scenario.
 */
i16 __wrap_urp_radio_process_native_timed(urp_radio_state *radio, i16 *input, i16 *outputrx,
					  i16 *outputtx, size_t native_frame_count, int advance_tx)
{
	if (native_release_hold_signaling)
		return 0;
	if (native_release_failure &&
	    !strcmp(native_release_failure, "urp_radio_process_native_timed")) {
		native_release_failure_hits++;
		return -1;
	}
	return __real_urp_radio_process_native_timed(radio, input, outputrx, outputtx,
						     native_frame_count, advance_tx);
}

/** @brief Real RNNoise preparation behind the renderer startup failure boundary. */
extern int __real_txagc_rnnoise_prepare(struct txagc_rnnoise *state, unsigned int sample_rate);
/** @brief Reject RNNoise startup only when the renderer recovery test requests it. */
int __wrap_txagc_rnnoise_prepare(struct txagc_rnnoise *state, unsigned int sample_rate)
{
	if (native_release_failure && !strcmp(native_release_failure, "txagc_rnnoise_prepare"))
		return -1;
	return __real_txagc_rnnoise_prepare(state, sample_rate);
}

/** @brief Validate installed receiver callbacks and renderer fail-silent responses. */
static void test_native_tick_release_boundaries(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state config = {
		.pRxCodeSrc = "100.0", .pTxCodeSrc = "100.0", .pTxCodeDefault = "100.0"};
	short program[URP_NATIVE_SAMPLES], samples[URP_NATIVE_STEREO_SAMPLES] = {0};
	float output[(URP_NATIVE_MAX_SAMPLES + 1U) * 2U];
	struct usbradioplus_native_graph_set *graphs;
	urp_radio_state *saved_radio;
	urp_ctcss_decoder *saved_ctcss;
	int valid, decoded;
	settings_defaults(&settings);
	strcpy(settings.profiles[0].name, "portaudio-poc-callback");
	strcpy(settings.profiles[0].channel, "RadioPlus/portaudio-poc-callback");
	settings.profiles[0].enabled = 0;
	portaudio_poc_callback_channel_init(&channel, &config, program, ARRAY_LEN(program), 0);
	assert(channel.radio->rxCtcss);
	urp_dcs_receive_callback dcs = channel.radio->dcs.receive_callback;
	urp_ctcss_receive_callback ctcss = channel.radio->rxCtcss->receive_callback;
	void *context = channel.radio->dcs.receive_callback_context;
	assert(dcs && ctcss && context);
	assert(dcs(NULL, samples, 1U, 2U, URP_RATE_NATIVE, &valid) == -1);
	assert(dcs(context, NULL, 1U, 2U, URP_RATE_NATIVE, &valid) == -1);
	assert(dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, NULL) == -1);
	assert(dcs(context, samples, 1U, 1U, URP_RATE_NATIVE, &valid) == -1);
	assert(dcs(context, samples, 1U, 2U, 8000U, &valid) == -1);
	assert(dcs(context, samples, URP_NATIVE_MAX_SAMPLES + 1U, 2U, URP_RATE_NATIVE, &valid) ==
	       -1);
	assert(dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, &valid) == -1);
	channel.radio->dcs.enabled_receive = 1;
	channel.radio->dcs.receive_code = 23;
	native_release_failure = "urp_radio_core_configure_dcs_receive";
	assert(dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, &valid) == -1);
	native_release_failure = NULL;
	assert(!dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, &valid));
	assert(!dcs(context, samples, 0U, 2U, URP_RATE_NATIVE, &valid));
	channel.radio->dcs.receive_inverted = 1;
	assert(!dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, &valid));
	assert(ctcss(NULL, samples, 1U, 1U, 0, 1, &decoded) == -1);
	assert(ctcss(context, NULL, 1U, 1U, 0, 1, &decoded) == -1);
	assert(ctcss(context, samples, 1U, 1U, 0, 1, NULL) == -1);
	assert(ctcss(context, samples, SAMPLES_PER_BLOCK + 1U, 1U, 0, 1, &decoded) == -1);
	native_release_failure = "urp_radio_core_configure_ctcss_receive";
	assert(ctcss(context, samples, 1U, 1U, 0, 1, &decoded) == -1);
	native_release_failure = NULL;
	assert(!ctcss(context, samples, 0U, 1U, 0, 1, &decoded));
	assert(!ctcss(context, samples, 1U, 1U, 1, 1, &decoded));
	saved_radio = channel.radio;
	channel.radio = NULL;
	assert(dcs(context, samples, 1U, 2U, URP_RATE_NATIVE, &valid) == -1);
	assert(ctcss(context, samples, 1U, 1U, 0, 1, &decoded) == -1);
	channel.radio = saved_radio;
	saved_ctcss = channel.radio->rxCtcss;
	channel.radio->rxCtcss = NULL;
	assert(ctcss(context, samples, 1U, 1U, 0, 1, &decoded) == -1);
	usbradioplus_native_renderer_bind_radio(&channel);
	channel.radio->rxCtcss = saved_ctcss;
	channel.radio->dcs.receive_callback_context = NULL;
	channel.radio->rxCtcss->receive_callback_context = NULL;
	usbradioplus_native_renderer_bind_radio(&channel);
	usbradioplus_native_renderer_bind_radio(NULL);

	graphs = usbradioplus_native_graphs_acquire(&channel);
	assert(graphs);
	native_release_hold_signaling = 1;
	graphs->echo_mode = 1;
	channel.radio->dcs.enabled_transmit = 1;
	channel.radio->dcs.transmit_code = 23;
	channel.radio->txState = CHAN_TXSTATE_ACTIVE;
	channel.radio->txPttOut = 1;
	{
		const char *failures[] = {
			"urp_radio_core_native_parrot_status",
			"urp_radio_core_extract_receive",
			"urp_radio_core_native_parrot_rx_transition",
			"urp_radio_core_generate_ctcss",
			"urp_radio_core_configure_dcs",
			"urp_radio_core_generate_dcs",
			"urp_radio_core_native_parrot_play",
			"urp_radio_core_render_calibrated_test_tone",
			"urp_render_transmit_block",
			"urp_radio_core_ctcss_phase",
			"urp_radio_core_calibrated_test_tone_phase",
		};
		for (size_t failure = 0U; failure < ARRAY_LEN(failures); ++failure) {
			native_release_failure = failures[failure];
			native_release_failure_hits = 0U;
			native_release_force_parrot = 1;
			channel.radio->dcs.transmit_inverted = (int)(failure & 1U);
			(void)usbradioplus_native_tick(&channel, 48U);
			assert(native_release_failure_hits);
		}
	}
	native_release_failure = "urp_radio_core_render_calibrated_test_tone";
	atomic_store(&channel.plus_test_tone_enabled, 1);
	(void)usbradioplus_native_tick(&channel, 48U);
	atomic_store(&channel.plus_test_tone_enabled, 0);
	native_release_failure = "urp_radio_core_generate_ctcss_tail";
	channel.radio->txCtcssTailToneHz = 55.0;
	(void)usbradioplus_native_tick(&channel, 48U);
	channel.radio->txCtcssTailToneHz = 0.0;
	native_release_failure = NULL;
	native_release_force_parrot = 0;
	channel.radio->txState = CHAN_TXSTATE_TOC;
	channel.radio->dcsTurnoffTimer = 1;
	fail_ffmpeg_adapter_process_state = &graphs->dcs_turnoff;
	(void)usbradioplus_native_tick(&channel, 48U);
	fail_ffmpeg_adapter_process_state = NULL;
	channel.radio->dcsTurnoffTimer = 0;
	(void)usbradioplus_native_tick(&channel, 48U);
	{
		size_t capacity = channel.plus_program_ring.capacity;
		channel.plus_program_ring.capacity = 0U;
		(void)usbradioplus_native_tick(&channel, 48U);
		channel.plus_program_ring.capacity = capacity;
	}
	graphs->app_rpt_rate = 0U;
	assert(!usbradioplus_native_tick(&channel, 48U));
	graphs->app_rpt_rate = URP_RATE_LINK;
	usbradioplus_native_graphs_release(&channel);
	native_release_hold_signaling = 0;
	usbradioplus_native_renderer_stop(&channel);
	assert(!usbradioplus_native_tick(&channel, 48U));
	{
		const char *failures[] = {"urp_native_repeat_initialize", "urp_radio_core_create",
					  "urp_radio_core_native_parrot_bind",
					  "txagc_rnnoise_prepare"};
		for (size_t failure = 0U; failure < ARRAY_LEN(failures); ++failure) {
			native_release_failure = failures[failure];
			assert(usbradioplus_native_renderer_start(&channel) == -1);
			assert(!channel.plus_native_renderer);
		}
	}
	native_release_failure = NULL;
	native_release_reserve_calls = 0U;
	native_release_reserve_failure = 2U;
	assert(usbradioplus_native_renderer_start(&channel) == -1);
	native_release_reserve_failure = 0U;
	assert(!usbradioplus_native_tick(&channel, 0U));
	channel.plus_native_max_frames = 1U;
	assert(!usbradioplus_native_tick(&channel, 2U));
	channel.plus_native_max_frames = URP_NATIVE_MAX_SAMPLES + 1U;
	assert(!usbradioplus_native_tick(&channel, URP_NATIVE_MAX_SAMPLES + 1U));
	assert(!usbradioplus_native_tick_f32(&channel, NULL, output, URP_NATIVE_MAX_SAMPLES + 1U,
					     NULL, NULL));
	channel.plus_native_max_frames = 0U;
	assert(!usbradioplus_native_tick_f32(&channel, NULL, output, 1U, NULL, NULL));
	assert(!usbradioplus_native_tick_f32(&channel, NULL, output, 0U, NULL, NULL));
	assert(!usbradioplus_native_tick_f32(NULL, NULL, output, 0U, NULL, NULL));
	assert(!usbradioplus_native_tick_f32(NULL, NULL, output, SIZE_MAX, NULL, NULL));
	channel.plus_app_rpt_rate = UINT_MAX;
	assert(!usbradioplus_native_tick(&channel, URP_NATIVE_SAMPLES));
	channel.plus_app_rpt_rate = URP_RATE_NATIVE * 2U;
	assert(!usbradioplus_native_tick(&channel, URP_NATIVE_SAMPLES));
	channel.plus_app_rpt_rate = URP_RATE_LINK;
	usbradioplus_dsp_destroy(&channel);
	assert(!urp_radio_destroy(channel.radio));
}
