/** @file
 * @brief Deterministic malformed-state and shared-provider boundary checks.
 */

/** Provider replacement used only by this test executable. */
static const struct rptadv_radio_descriptor *boundary_descriptor;
/** Simulate the absent-provider startup path. */
static int boundary_missing_descriptor;

/** @brief Select a released, absent, or intentionally malformed test provider. */
const struct rptadv_radio_descriptor *urp_radio_core_adapter_test_descriptor(void)
{
	return boundary_missing_descriptor ? NULL
	       : boundary_descriptor	   ? boundary_descriptor
					   : rptadv_radio_descriptor();
}

/** @brief Simulate a future provider returning an unsupported enumeration. */
static enum rptadv_radio_result boundary_unknown_mode(const char *text, uint32_t *value)
{
	(void)text;
	*value = UINT32_MAX;
	return RPTADV_RADIO_OK;
}

/** @brief Return a provider rendering error without changing caller buffers. */
static enum rptadv_radio_result
boundary_render_error(const struct rptadv_radio *radio, const float *program, const float *ctcss,
		      const float *dcs, uint32_t frame_count,
		      const struct rptadv_radio_transmit_render_config *config, int16_t *stereo,
		      int16_t *meter_stereo, uint64_t *rails)
{
	(void)radio;
	(void)program;
	(void)ctcss;
	(void)dcs;
	(void)frame_count;
	(void)config;
	(void)stereo;
	(void)meter_stereo;
	(void)rails;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Exercise output-stage guards without invoking a hardware device. */
static void test_output_stage_boundaries(void)
{
	struct urp_native_output_stage stage;
	struct urp_native_output_block finished;
	short pcm[4] = {1, 2, 3, 4};
	urp_native_output_stage_init(NULL, 0, 0);
	urp_native_output_stage_reset(NULL);
	assert(!urp_native_output_stage_set_capacity(NULL, 2));
	assert(urp_native_output_stage_enqueue(NULL, pcm, 2, 0, 0) == -1);
	assert(!urp_native_output_stage_has_ptt(NULL));
	assert(!urp_native_output_stage_peek(NULL));
	assert(!urp_native_output_stage_note_unavailable(NULL, 1));
	urp_native_output_stage_init(&stage, UINT_MAX, SIZE_MAX);
	assert(stage.capacity == URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS);
	assert(stage.maximum_frame_count == URP_NATIVE_MAX_SAMPLES);
	assert(urp_native_output_stage_enqueue(&stage, NULL, 2, 0, 0) == -1);
	assert(urp_native_output_stage_enqueue(&stage, pcm, 0, 0, 0) == -1);
	assert(urp_native_output_stage_enqueue(&stage, pcm, SIZE_MAX, 0, 0) == -1);
	stage.capacity = 0;
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 0) == -1);
	urp_native_output_stage_init(&stage, 2, 2);
	assert(urp_native_output_stage_set_capacity(&stage, 2));
	assert(urp_native_output_stage_commit(&stage, 1, NULL) == -1);
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 1) == 1);
	assert(!urp_native_output_stage_set_capacity(&stage, 2));
	assert(urp_native_output_stage_commit(&stage, 0, NULL) == -1);
	assert(urp_native_output_stage_commit(&stage, 3, NULL) == -1);
	stage.current.submitted_frames = 3;
	assert(urp_native_output_stage_commit(&stage, 1, NULL) == -1);
	stage.current.submitted_frames = 0;
	assert(!urp_native_output_stage_note_unavailable(&stage, 1));
	assert(urp_native_output_stage_commit(&stage, 1, NULL) == 0);
	stage.maximum_frame_count = 0;
	assert(!urp_native_output_stage_note_unavailable(&stage, 1));
	stage.maximum_frame_count = 2;
	assert(!urp_native_output_stage_note_unavailable(&stage, 0));
	assert(!urp_native_output_stage_note_unavailable(&stage, 1));
	stage.stalled_partial_frames = UINT64_MAX - 1;
	assert(urp_native_output_stage_note_unavailable(&stage, 2));
	assert(stage.stalled_partial_frames == UINT64_MAX);
	assert(urp_native_output_stage_note_unavailable(&stage, 0));
	stage.capacity = 1;
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 1, 1) == -1);
	stage.capacity = 2;
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 1, 1) == 1);
	assert(urp_native_output_stage_has_ptt(&stage));
	assert(urp_native_output_stage_commit(&stage, 1, NULL) == 1);
	assert(urp_native_output_stage_has_ptt(&stage));
	assert(urp_native_output_stage_commit(&stage, 2, &finished) == 1);
	assert(finished.logical_ptt && finished.frame_count == 2);
	assert(!urp_native_output_stage_has_ptt(&stage));
	assert(!urp_native_output_stage_note_unavailable(&stage, 1));
	urp_native_output_stage_init(&stage, 0, 0);
	assert(stage.maximum_frame_count == URP_NATIVE_MAX_SAMPLES);
	assert(stage.capacity == 2);
	stage.pending_count = 2;
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 0) == 0);
	assert(!urp_native_output_stage_has_ptt(&stage));
	urp_native_output_stage_reset(&stage);
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 0) == 1);
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 0) == 1);
	assert(urp_native_output_stage_enqueue(&stage, pcm, 2, 0, 0) == 0);
}

/** @brief Reject invalid parser/provider results while preserving caller values. */
static void test_shared_parser_boundaries(void)
{
	struct rptadv_radio_descriptor descriptor = *rptadv_radio_descriptor();
	enum urp_rx_audio_mode rx = URP_RX_AUDIO_FLAT;
	enum urp_carrier_source carrier = URP_CARRIER_USB;
	enum urp_ctcss_source ctcss = URP_CTCSS_DSP;
	enum urp_tone_off_mode tone = URP_TONE_OFF_NONE;
	assert(urp_parse_carrier_source(NULL, &carrier));
	assert(urp_parse_ctcss_source(NULL, &ctcss));
	assert(urp_parse_tone_off_mode(NULL, &tone));
	descriptor.radio_parse_rx_audio_mode = boundary_unknown_mode;
	descriptor.radio_parse_carrier_source = boundary_unknown_mode;
	descriptor.radio_parse_ctcss_source = boundary_unknown_mode;
	descriptor.radio_parse_tone_off_mode = boundary_unknown_mode;
	boundary_descriptor = &descriptor;
	assert(!urp_radio_core_initialize());
	assert(urp_parse_rx_audio_mode("no", &rx));
	assert(urp_parse_carrier_source("no", &carrier));
	assert(urp_parse_ctcss_source("no", &ctcss));
	assert(urp_parse_tone_off_mode("no", &tone));
	assert(rx == URP_RX_AUDIO_FLAT && carrier == URP_CARRIER_USB);
	assert(ctcss == URP_CTCSS_DSP && tone == URP_TONE_OFF_NONE);
	boundary_descriptor = NULL;
	assert(!urp_radio_core_initialize());
}

/** @brief Reject malformed render calls before touching output storage. */
static void test_shared_render_boundaries(void)
{
	struct rptadv_radio_descriptor descriptor = *rptadv_radio_descriptor();
	struct rptadv_radio *radio = NULL;
	struct urp_transmit_render_workspace workspace;
	double program[1] = {0};
	float signaling[1] = {0};
	short output[2] = {0};
	unsigned long rails = 0;
	boundary_missing_descriptor = 1;
	assert(urp_radio_core_initialize());
	/* A nonnull opaque address is safe: the missing-provider guard returns first. */
	assert(urp_render_transmit_block((const struct rptadv_radio *)&workspace, program,
					 signaling, signaling, 0, URP_TX_OUTPUT_VOICE,
					 URP_TX_OUTPUT_VOICE, 0, 0, 0, 0, &workspace, output, NULL,
					 &rails));
	boundary_missing_descriptor = 0;
	assert(!urp_radio_core_initialize());
	assert(!urp_radio_core_create(URP_RATE_NATIVE, 1, &radio));
	for (unsigned int invalid = 0; invalid < 10; ++invalid) {
		assert(urp_render_transmit_block(
			invalid == 0 ? NULL : radio, invalid == 4 ? NULL : program,
			invalid == 5 ? NULL : signaling, invalid == 6 ? NULL : signaling,
			invalid == 3 ? SIZE_MAX : 1,
			invalid == 8 ? (enum urp_tx_output_mode)99 : URP_TX_OUTPUT_VOICE,
			invalid == 9 ? (enum urp_tx_output_mode)99 : URP_TX_OUTPUT_VOICE, 0, 0, 0,
			0, invalid == 1 ? NULL : &workspace, invalid == 7 ? NULL : output, NULL,
			invalid == 2 ? NULL : &rails));
	}
	assert(!urp_render_transmit_block(radio, program, signaling, signaling, 1,
					  URP_TX_OUTPUT_AUX_VOICE, URP_TX_OUTPUT_VOICE, 0, 0, 0, 0,
					  &workspace, output, NULL, &rails));
	descriptor.radio_render_transmit_f32 = boundary_render_error;
	boundary_descriptor = &descriptor;
	assert(!urp_radio_core_initialize());
	assert(urp_render_transmit_block(radio, program, signaling, signaling, 1,
					 URP_TX_OUTPUT_VOICE, URP_TX_OUTPUT_VOICE, NAN, 0, 0, 0,
					 &workspace, output, NULL, &rails));
	boundary_descriptor = NULL;
	assert(!urp_radio_core_initialize());
	urp_radio_core_destroy(radio);
}
