/** @file
 * @brief Radio compatibility regressions with deterministic shared-core faults.
 */
#ifndef USBRADIOPLUS_RADIO_PROCESSING_BOUNDARY_CASES_H
#define USBRADIOPLUS_RADIO_PROCESSING_BOUNDARY_CASES_H

/** Selected immutable descriptor for this single-threaded harness. */
static const struct rptadv_radio_descriptor *processing_test_descriptor;
/** @brief Supply the released descriptor unless a fault fixture selects another. */
const struct rptadv_radio_descriptor *urp_radio_core_adapter_test_descriptor(void)
{
	return processing_test_descriptor ? processing_test_descriptor : rptadv_radio_descriptor();
}

/** @brief Reject elapsed_ms without committing caller state. */
static enum rptadv_radio_result processing_reject_elapsed_ms(uint32_t *remainder,
							     uint32_t native_frame_count,
							     int32_t *milliseconds)
{
	(void)remainder;
	(void)native_frame_count;
	(void)milliseconds;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject timer_consume without committing caller state. */
static enum rptadv_radio_result
processing_reject_timer_consume(int32_t *timer, int32_t milliseconds, int32_t *remaining)
{
	(void)timer;
	(void)milliseconds;
	(void)remaining;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject signal_mode_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_signal_mode_advance(const struct rptadv_radio_signal_mode_config *config,
				      const struct rptadv_radio_signal_mode_input *input,
				      struct rptadv_radio_signal_mode_state *state)
{
	(void)config;
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject ctcss_render_state_advance without committing caller state. */
static enum rptadv_radio_result processing_reject_ctcss_render_state_advance(
	const struct rptadv_radio_ctcss_render_state_config *config,
	const struct rptadv_radio_ctcss_render_state_input *input,
	struct rptadv_radio_ctcss_render_state *state)
{
	(void)config;
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject dcs_turnoff_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_dcs_turnoff_advance(const struct rptadv_radio_dcs_turnoff_config *config,
				      const struct rptadv_radio_dcs_turnoff_input *input,
				      struct rptadv_radio_dcs_turnoff_state *state)
{
	(void)config;
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject tx_finish_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_tx_finish_advance(const struct rptadv_radio_tx_finish_input *input,
				    struct rptadv_radio_tx_finish_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject tx_finish_continue without committing caller state. */
static enum rptadv_radio_result
processing_reject_tx_finish_continue(const struct rptadv_radio_tx_finish_input *input,
				     struct rptadv_radio_tx_finish_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject rx_blanking_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_rx_blanking_advance(const struct rptadv_radio_rx_blanking_input *input,
				      struct rptadv_radio_rx_blanking_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject vox_carrier_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_vox_carrier_advance(const struct rptadv_radio_vox_carrier_input *input,
				      struct rptadv_radio_vox_carrier_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject tx_cpu_saver_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_tx_cpu_saver_advance(const struct rptadv_radio_tx_cpu_saver_input *input,
				       struct rptadv_radio_tx_cpu_saver_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject rx_cpu_saver_advance without committing caller state. */
static enum rptadv_radio_result
processing_reject_rx_cpu_saver_advance(const struct rptadv_radio_rx_cpu_saver_input *input,
				       struct rptadv_radio_rx_cpu_saver_state *state)
{
	(void)input;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Reject tx_complete without committing caller state. */
static enum rptadv_radio_result
processing_reject_tx_complete(const struct rptadv_radio_tx_complete_config *config,
			      struct rptadv_radio_tx_complete_state *state)
{
	(void)config;
	(void)state;
	return RPTADV_RADIO_INVALID_ARGUMENT;
}

/** @brief Replay existing state-machine vectors through retained C fallbacks. */
static void test_processing_rejected_core_fallbacks(void)
{
	struct rptadv_radio_descriptor rejected = *rptadv_radio_descriptor();
	rejected.radio_elapsed_ms = processing_reject_elapsed_ms;
	rejected.radio_timer_consume = processing_reject_timer_consume;
	rejected.radio_signal_mode_advance = processing_reject_signal_mode_advance;
	rejected.radio_ctcss_render_state_advance = processing_reject_ctcss_render_state_advance;
	rejected.radio_dcs_turnoff_advance = processing_reject_dcs_turnoff_advance;
	rejected.radio_tx_finish_advance = processing_reject_tx_finish_advance;
	rejected.radio_tx_finish_continue = processing_reject_tx_finish_continue;
	rejected.radio_rx_blanking_advance = processing_reject_rx_blanking_advance;
	rejected.radio_vox_carrier_advance = processing_reject_vox_carrier_advance;
	rejected.radio_tx_cpu_saver_advance = processing_reject_tx_cpu_saver_advance;
	rejected.radio_rx_cpu_saver_advance = processing_reject_rx_cpu_saver_advance;
	rejected.radio_tx_complete = processing_reject_tx_complete;
	processing_test_descriptor = &rejected;
	assert(!urp_radio_core_initialize());
	test_create_process_destroy();
	test_create_variants();
	test_runtime_state_machine();
	test_transmit_timeline_admission();
	test_native_frame_partitioning();
	test_rx_blanking_partitioning();
	test_dcs_turnoff_duration_bounds();
	test_dcs_radio_state_machine();
	test_ctcss_rekey_remains_transmit_directional();
	test_ctcss_transmit_default_without_decoded_receive_tone();
	test_ctcss_transmit_startup_edges();
	test_cpu_saver_predicates();
	processing_test_descriptor = NULL;
	assert(!urp_radio_core_initialize());
}

extern i32 urp_radio_elapsed_ms(u32 *remainder, size_t native_frames);
extern i32 urp_radio_timer_consume(i32 *timer, i32 milliseconds);
extern int urp_radio_rx_blanking_portable(urp_radio_state *channel, i32 elapsed_ms,
					  u32 remainder_before, size_t native_frame_count,
					  size_t *blanked_frames);
extern int urp_radio_vox_carrier_portable(urp_radio_state *channel, i32 elapsed_ms);
extern int urp_radio_tx_cpu_saver_portable(urp_radio_state *channel);
extern int urp_radio_rx_cpu_saver_portable(urp_radio_state *channel, u32 *action, u32 *next_halted);
extern int urp_radio_signal_mode_portable(urp_radio_state *channel, i32 elapsed_ms,
					  int decoded_ctcss);
extern int urp_radio_ctcss_render_state_portable(urp_radio_state *channel, i32 elapsed_ms);
extern int urp_radio_dcs_turnoff_portable(urp_radio_state *channel, i32 elapsed_ms,
					  int begin_turnoff, int *finish_requested,
					  i32 *finish_elapsed_ms);
extern int urp_radio_complete_tx_portable(urp_radio_state *channel);
extern int urp_radio_enter_finishing_portable(urp_radio_state *channel, i32 elapsed_ms);
extern int urp_radio_enter_finishing(urp_radio_state *channel, i32 elapsed_ms);
extern int urp_radio_continue_finishing_portable(urp_radio_state *channel, i32 elapsed_ms);

/** Malformed-reply field selected independently for each boundary. */
static unsigned int processing_reply_fault;
/** @brief Return one independently malformed rx_blanking result. */
static enum rptadv_radio_result
processing_corrupt_rx_blanking_advance(const struct rptadv_radio_rx_blanking_input *input,
				       struct rptadv_radio_rx_blanking_state *state)
{
	assert(rptadv_radio_descriptor()->radio_rx_blanking_advance(input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->remaining_ms = -1;
		break;
	case 1:
		state->remaining_ms = 101;
		break;
	case 2:
		state->blanked_frame_count = input->native_frame_count + 1U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed vox_carrier result. */
static enum rptadv_radio_result
processing_corrupt_vox_carrier_advance(const struct rptadv_radio_vox_carrier_input *input,
				       struct rptadv_radio_vox_carrier_state *state)
{
	assert(rptadv_radio_descriptor()->radio_vox_carrier_advance(input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->remaining_ms = -1;
		break;
	case 1:
		state->carrier_detect = 2U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed tx_cpu_saver result. */
static enum rptadv_radio_result
processing_corrupt_tx_cpu_saver_advance(const struct rptadv_radio_tx_cpu_saver_input *input,
					struct rptadv_radio_tx_cpu_saver_state *state)
{
	assert(rptadv_radio_descriptor()->radio_tx_cpu_saver_advance(input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->halted = 2U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed rx_cpu_saver result. */
static enum rptadv_radio_result
processing_corrupt_rx_cpu_saver_advance(const struct rptadv_radio_rx_cpu_saver_input *input,
					struct rptadv_radio_rx_cpu_saver_state *state)
{
	assert(rptadv_radio_descriptor()->radio_rx_cpu_saver_advance(input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->halted = 2U;
		break;
	case 1:
		state->action = 3U;
		break;
	case 2:
		state->halted = 0U;
		state->action = 1U;
		break;
	case 3:
		state->halted = 1U;
		state->action = 0U;
		break;
	case 4:
		state->halted = 1U;
		state->action = 2U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed signal_mode result. */
static enum rptadv_radio_result
processing_corrupt_signal_mode_advance(const struct rptadv_radio_signal_mode_config *config,
				       const struct rptadv_radio_signal_mode_input *input,
				       struct rptadv_radio_signal_mode_state *state)
{
	assert(rptadv_radio_descriptor()->radio_signal_mode_advance(config, input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->smode = INT16_MIN - 1;
		break;
	case 1:
		state->smode = INT16_MAX + 1;
		break;
	case 2:
		state->smode_was = INT16_MIN - 1;
		break;
	case 3:
		state->smode_was = INT16_MAX + 1;
		break;
	case 4:
		state->last_rx_ctcss = CTCSS_NULL - 1;
		break;
	case 5:
		state->last_rx_ctcss = CTCSS_NUM_CODES;
		break;
	case 6:
		state->tx_ctcss_option = 4U;
		break;
	case 7:
		state->smode_turnoff = 2U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed ctcss_render_state result. */
static enum rptadv_radio_result processing_corrupt_ctcss_render_state_advance(
	const struct rptadv_radio_ctcss_render_state_config *config,
	const struct rptadv_radio_ctcss_render_state_input *input,
	struct rptadv_radio_ctcss_render_state *state)
{
	assert(rptadv_radio_descriptor()->radio_ctcss_render_state_advance(config, input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->option = 4U;
		break;
	case 1:
		state->oscillator_state = 3U;
		break;
	case 2:
		state->enabled = 2U;
		break;
	case 3:
		state->turnoff_remaining_ms = -1;
		break;
	case 4:
		state->phase_shift_degrees = NAN;
		break;
	case 5:
		state->tail_tone_hz = NAN;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed tx_finish result. */
static enum rptadv_radio_result
processing_corrupt_tx_finish_advance(const struct rptadv_radio_tx_finish_input *input,
				     struct rptadv_radio_tx_finish_state *state)
{
	assert(rptadv_radio_descriptor()->radio_tx_finish_advance(input, state) == RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->tx_state = CHAN_TXSTATE_IDLE;
		break;
	case 1:
		state->buffer_clear_frames = -1;
		break;
	case 2:
		state->buffer_clear_frames = 4;
		break;
	case 3:
		state->finish_remaining_ms = -1;
		break;
	case 4:
		state->finish_remaining_ms = 81;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed tx_finish result. */
static enum rptadv_radio_result
processing_corrupt_tx_finish_continue(const struct rptadv_radio_tx_finish_input *input,
				      struct rptadv_radio_tx_finish_state *state)
{
	assert(rptadv_radio_descriptor()->radio_tx_finish_continue(input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->tx_state = CHAN_TXSTATE_IDLE;
		break;
	case 1:
		state->buffer_clear_frames = -1;
		break;
	case 2:
		state->buffer_clear_frames = 9;
		break;
	case 3:
		state->finish_remaining_ms = -1;
		break;
	case 4:
		state->finish_remaining_ms = 161;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed tx_complete result. */
static enum rptadv_radio_result
processing_corrupt_tx_complete(const struct rptadv_radio_tx_complete_config *config,
			       struct rptadv_radio_tx_complete_state *state)
{
	assert(rptadv_radio_descriptor()->radio_tx_complete(config, state) == RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->tx_state = CHAN_TXSTATE_ACTIVE;
		break;
	case 1:
		state->tx_ptt_out = 1U;
		break;
	case 2:
		state->tx_ctcss_option = 1U;
		break;
	case 3:
		state->txrx_blanking_timer_ms = config->txrx_blanking_time_ms + 1;
		break;
	case 4:
		state->txrx_blanking_sample_remainder = 1U;
		break;
	case 5:
		state->tx_ctcss_ready = 0U;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}
/** @brief Return one independently malformed dcs_turnoff result. */
static enum rptadv_radio_result
processing_corrupt_dcs_turnoff_advance(const struct rptadv_radio_dcs_turnoff_config *config,
				       const struct rptadv_radio_dcs_turnoff_input *input,
				       struct rptadv_radio_dcs_turnoff_state *state)
{
	assert(rptadv_radio_descriptor()->radio_dcs_turnoff_advance(config, input, state) ==
	       RPTADV_RADIO_OK);
	switch (processing_reply_fault) {
	case 0:
		state->tx_state = CHAN_TXSTATE_IDLE;
		break;
	case 1:
		state->dcs_turnoff_remaining_ms = -1;
		break;
	case 2:
		state->finish_requested = 2U;
		break;
	case 3:
		state->finish_elapsed_ms = -1;
		break;
	case 4:
		state->finish_elapsed_ms = input->elapsed_ms + 1;
		break;
	case 5:
		state->finish_requested = 0U;
		state->finish_elapsed_ms = 1;
		break;
	case 6:
		state->tx_state = CHAN_TXSTATE_ACTIVE;
		break;
	case 7:
		state->dcs_turnoff_remaining_ms = config->turnoff_duration_ms + 1;
		break;
	case 8:
		state->tx_hang_remaining_ms = 17;
		break;
	case 9:
		state->finish_requested = !state->finish_requested;
		break;
	case 10:
		state->tx_state = CHAN_TXSTATE_TOC;
		break;
	case 11:
		state->dcs_turnoff_remaining_ms = 1;
		break;
	case 12:
		state->finish_requested = 1U;
		state->finish_elapsed_ms = 1;
		break;
	default:
		break;
	}
	return RPTADV_RADIO_OK;
}

/** @brief Exercise scalar argument guards and validate every returned field. */
static void test_processing_scalar_boundaries(void)
{
	urp_radio_state channel = {0};
	urp_radio_stage vox = {0};
	u32 action = 0, halted = 0, remainder = 0;
	size_t blanked = 0;
	int requested = 0;
	i32 residual = 0, timer = 0;
	struct rptadv_radio_descriptor faulty = *rptadv_radio_descriptor();
	assert(urp_radio_elapsed_ms(&remainder, 48) == 1 && !remainder);
	assert(!urp_radio_timer_consume(&timer, 0));
	assert(urp_radio_rx_blanking_portable(NULL, 0, 0, 1, &blanked) == -1);
	assert(urp_radio_rx_blanking_portable(&channel, 0, 0, 1, NULL) == -1);
	assert(urp_radio_rx_blanking_portable(&channel, 0, 0, 1, &blanked) == -1);
	channel.txrxblankingtimer = 100;
	assert(urp_radio_rx_blanking_portable(&channel, -1, 0, 1, &blanked) == -1);
	assert(urp_radio_rx_blanking_portable(&channel, 0, 0, (size_t)UINT32_MAX + 1U, &blanked) ==
	       -1);
	assert(urp_radio_vox_carrier_portable(NULL, 0) == -1);
	assert(urp_radio_vox_carrier_portable(&channel, 0) == -1);
	channel.spsRxVox = &vox;
	assert(urp_radio_tx_cpu_saver_portable(NULL) == -1);
	assert(urp_radio_rx_cpu_saver_portable(NULL, &action, &halted) == -1);
	assert(urp_radio_rx_cpu_saver_portable(&channel, NULL, &halted) == -1);
	assert(urp_radio_rx_cpu_saver_portable(&channel, &action, NULL) == -1);
	assert(urp_radio_signal_mode_portable(NULL, 0, CTCSS_NULL) == -1);
	assert(urp_radio_signal_mode_portable(&channel, 0, CTCSS_NULL - 1) == -1);
	assert(urp_radio_signal_mode_portable(&channel, 0, CTCSS_NUM_CODES) == -1);
	channel.rxCtcssMap[0] = -3;
	assert(urp_radio_signal_mode_portable(&channel, 0, 0) == -1);
	channel.rxCtcssMap[0] = CTCSS_NUM_CODES;
	assert(urp_radio_signal_mode_portable(&channel, 0, 0) == -1);
	channel.rxCtcssMap[0] = CTCSS_RXONLY;
	assert(!urp_radio_signal_mode_portable(&channel, 0, 0));
	channel.rxCtcssMap[0] = 0;
	assert(urp_radio_ctcss_render_state_portable(NULL, 0) == -1);
	assert(urp_radio_ctcss_render_state_portable(&channel, -1) == -1);
#define INVALID_RENDER(field, value)                                                               \
	do {                                                                                       \
		__typeof__(channel.field) saved = channel.field;                                   \
		channel.field = (value);                                                           \
		assert(urp_radio_ctcss_render_state_portable(&channel, 0) == -1);                  \
		channel.field = saved;                                                             \
	} while (0)
	INVALID_RENDER(txCtcssTocTime, -1);
	INVALID_RENDER(txCtcssOption, -1);
	INVALID_RENDER(txCtcssOption, 4);
	INVALID_RENDER(txCtcssState, -1);
	INVALID_RENDER(txCtcssState, 3);
	INVALID_RENDER(txCtcssEnabled, -1);
	INVALID_RENDER(txCtcssEnabled, 2);
	INVALID_RENDER(txCtcssTurnoffTimer, -1);
	INVALID_RENDER(txCtcssTocShift, NAN);
	INVALID_RENDER(txCtcssTocToneHz, NAN);
	INVALID_RENDER(txCtcssPhaseShift, NAN);
	INVALID_RENDER(txCtcssTailToneHz, NAN);
#undef INVALID_RENDER
	assert(urp_radio_dcs_turnoff_portable(NULL, 0, 1, &requested, &residual) == -1);
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, NULL, &residual) == -1);
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, NULL) == -1);
	assert(urp_radio_dcs_turnoff_portable(&channel, -1, 1, &requested, &residual) == -1);
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, &residual) == -1);
	channel.dcsTurnoffDuration = 100;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, &residual) == -1);
	channel.txState = CHAN_TXSTATE_ACTIVE;
	channel.txPttIn = 1;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, &residual) == -1);
	channel.txPttIn = 0;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, &residual) == -1);
	channel.dcs.enabled_transmit = 1;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 1, &requested, &residual) == -1);
	channel.dcsTurnoffEnabled = 1;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 0, &requested, &residual) == -1);
	channel.txState = CHAN_TXSTATE_TOC;
	assert(urp_radio_dcs_turnoff_portable(&channel, 0, 0, &requested, &residual) == -1);
	assert(urp_radio_complete_tx_portable(NULL) == -1);
	urp_radio_arm_txrx_blanking(NULL);
	assert(urp_radio_enter_finishing_portable(NULL, 0) == -1);
	assert(urp_radio_enter_finishing_portable(&channel, -1) == -1);
	assert(urp_radio_continue_finishing_portable(NULL, 0) == -1);
	assert(urp_radio_continue_finishing_portable(&channel, -1) == -1);
	assert(urp_radio_continue_finishing_portable(&channel, 0) == -1);
	channel.txState = CHAN_TXSTATE_FINISHING;
	channel.txBufferClear = -1;
	assert(urp_radio_continue_finishing_portable(&channel, 0) == -1);
	channel.txBufferClear = 9;
	assert(urp_radio_continue_finishing_portable(&channel, 0) == -1);
	channel.txBufferClear = 3;
	channel.txFinishTimer = -1;
	assert(urp_radio_continue_finishing_portable(&channel, 0) == -1);
	channel.txFinishTimer = 0;
	processing_test_descriptor = &faulty;
	faulty = *rptadv_radio_descriptor();
	faulty.radio_rx_blanking_advance = processing_corrupt_rx_blanking_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 3; ++processing_reply_fault) {
		assert(urp_radio_rx_blanking_portable(&channel, 1, 0, 48, &blanked) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_vox_carrier_advance = processing_corrupt_vox_carrier_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 2; ++processing_reply_fault) {
		assert(urp_radio_vox_carrier_portable(&channel, 1) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_tx_cpu_saver_advance = processing_corrupt_tx_cpu_saver_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 1; ++processing_reply_fault) {
		assert(urp_radio_tx_cpu_saver_portable(&channel) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_rx_cpu_saver_advance = processing_corrupt_rx_cpu_saver_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 5; ++processing_reply_fault) {
		assert(urp_radio_rx_cpu_saver_portable(&channel, &action, &halted) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_signal_mode_advance = processing_corrupt_signal_mode_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 8; ++processing_reply_fault) {
		assert(urp_radio_signal_mode_portable(&channel, 1, CTCSS_NULL) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_ctcss_render_state_advance = processing_corrupt_ctcss_render_state_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 6; ++processing_reply_fault) {
		assert(urp_radio_ctcss_render_state_portable(&channel, 1) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_tx_finish_advance = processing_corrupt_tx_finish_advance;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 5; ++processing_reply_fault) {
		assert(urp_radio_enter_finishing_portable(&channel, 1) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_tx_finish_continue = processing_corrupt_tx_finish_continue;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 5; ++processing_reply_fault) {
		assert(urp_radio_continue_finishing_portable(&channel, 1) == -1);
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_tx_complete = processing_corrupt_tx_complete;
	assert(!urp_radio_core_initialize());
	for (processing_reply_fault = 0; processing_reply_fault < 6; ++processing_reply_fault) {
		assert(urp_radio_complete_tx_portable(&channel) == -1);
	}

	faulty = *rptadv_radio_descriptor();
	faulty.radio_dcs_turnoff_advance = processing_corrupt_dcs_turnoff_advance;
	assert(!urp_radio_core_initialize());
	for (int mode = 0; mode < 3; ++mode) {
		for (processing_reply_fault = 0; processing_reply_fault < 13;
		     ++processing_reply_fault) {
			channel.txState = mode ? CHAN_TXSTATE_TOC : CHAN_TXSTATE_ACTIVE;
			channel.txPttIn = mode == 1;
			channel.dcsTurnoffTimer = 50;
			channel.txHangTime = 0;
			const urp_radio_state before = channel;
			const int result = urp_radio_dcs_turnoff_portable(&channel, 1, mode == 0,
									  &requested, &residual);
			if (result) {
				assert(!memcmp(&channel, &before, sizeof(channel)));
			} else {
				assert(channel.dcsTurnoffTimer >= 0 && residual >= 0 &&
				       residual <= 1);
				assert(!channel.txPttIn || !requested);
			}
		}
	}
	faulty = *rptadv_radio_descriptor();
	faulty.radio_tx_finish_advance = processing_reject_tx_finish_advance;
	faulty.radio_timer_consume = processing_reject_timer_consume;
	assert(!urp_radio_core_initialize());
	assert(urp_radio_enter_finishing(&channel, 80) == 1);
	timer = 5;
	assert(urp_radio_timer_consume(&timer, 0) == 0 && timer == 5);
	processing_test_descriptor = NULL;
	assert(!urp_radio_core_initialize());
}

/** Scripted frontend outcome for public runtime guard checks. */
static i16 processing_frontend_result;
/** Scripted frontend emitted count; no PCM is touched by this boundary fake. */
static i16 processing_frontend_samples;
/** @brief Model an invalid frontend response independently of its DSP implementation. */
static i16 processing_frontend_reply(urp_radio_stage *stage)
{
	stage->nSamples = processing_frontend_samples;
	return processing_frontend_result;
}
/** @brief Cover runtime capacity guards, decoder retirement, and immediate tails. */
static void test_processing_runtime_edges(void)
{
	urp_radio_state template = {
		.pRxCodeSrc = "100.0", .pTxCodeSrc = "100.0", .pTxCodeDefault = "100.0"};
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	i16 input[SAMPLES_PER_BLOCK * 12] = {0};
	i16 output[SAMPLES_PER_BLOCK + 1] = {0};
	struct rptadv_radio_descriptor rejected = *rptadv_radio_descriptor();
	assert(state);
	assert(urp_radio_process_native_timed(state, NULL, NULL, NULL, 1, 1) == 1);
	assert(urp_radio_process_native_timed(state, input, NULL, NULL, 0, 1) == 1);
#define REJECT_RUNTIME(field, value)                                                               \
	do {                                                                                       \
		__typeof__(state->field) saved = state->field;                                     \
		state->field = (value);                                                            \
		assert(urp_radio_process_native_timed(state, input, NULL, NULL, 1, 1) == 1);       \
		state->field = saved;                                                              \
	} while (0)
	REJECT_RUNTIME(nSamplesRx, 0);
	REJECT_RUNTIME(rxBaseCapacity, 0);
	REJECT_RUNTIME(spsRx, NULL);
	REJECT_RUNTIME(spsRxOut, NULL);
#undef REJECT_RUNTIME
	{
		__typeof__(state->spsRx->sigProc) saved = state->spsRx->sigProc;
		void *next = state->spsRx->nextSps;
		state->spsRx->sigProc = processing_frontend_reply;
		processing_frontend_result = 1;
		assert(urp_radio_process_native_timed(state, input, output, NULL, 960, 1) == 1);
		processing_frontend_result = 0;
		processing_frontend_samples = -1;
		assert(urp_radio_process_native_timed(state, input, output, NULL, 960, 1) == 1);
		processing_frontend_samples = (i16)(state->rxBaseCapacity + 1);
		assert(urp_radio_process_native_timed(state, input, output, NULL, 960, 1) == 1);
		state->spsRx->nextSps = NULL;
		state->tracetype = 1;
		processing_frontend_samples = SAMPLES_PER_BLOCK + 1;
		assert(!urp_radio_process_native_timed(state, input, output, NULL, 960, 1));
		state->tracetype = 0;
		state->spsRx->nextSps = next;
		state->spsRx->sigProc = saved;
	}
	{
		__typeof__(state->rxCtcss) decoder = state->rxCtcss;
		state->rxCtcss = NULL;
		state->b.ctcssRxEnable = 1;
		assert(!urp_radio_process_native_timed(state, input, output, NULL, 960, 1));
		state->rxCtcss = decoder;
		state->b.ctcssRxEnable = 0;
	}
	state->rxCtcssMap[0] = CTCSS_NUM_CODES;
	assert(!urp_radio_signal_mode_portable(state, 0, CTCSS_NULL));
	state->rxCtcssMap[0] = 0;
	assert(urp_radio_enter_finishing_portable(state, 80) == 1);
	state->txState = CHAN_TXSTATE_COMPLETE;
	assert(!process_once(state) && state->txState == CHAN_TXSTATE_IDLE);
	for (int fallback = 0; fallback < 2; ++fallback) {
		rejected = *rptadv_radio_descriptor();
		if (fallback) {
			rejected.radio_elapsed_ms = processing_reject_elapsed_ms;
			rejected.radio_timer_consume = processing_reject_timer_consume;
			rejected.radio_rx_blanking_advance = processing_reject_rx_blanking_advance;
			rejected.radio_dcs_turnoff_advance = processing_reject_dcs_turnoff_advance;
			rejected.radio_tx_finish_advance = processing_reject_tx_finish_advance;
			rejected.radio_tx_finish_continue = processing_reject_tx_finish_continue;
			rejected.radio_ctcss_render_state_advance =
				processing_reject_ctcss_render_state_advance;
		}
		processing_test_descriptor = &rejected;
		assert(!urp_radio_core_initialize());
		state->txState = CHAN_TXSTATE_ACTIVE;
		state->txPttIn = 0;
		state->dcs.enabled_transmit = 1;
		state->dcsTurnoffEnabled = 1;
		state->dcsTurnoffDuration = 1;
		assert(!process_once(state) && state->txState == CHAN_TXSTATE_FINISHING);
		state->dcs.enabled_transmit = 0;
		state->dcsTurnoffTimer = 0;
		state->txCtcssEnabled = 1;
		state->txTocType = TOC_NOTONE;
		state->txCtcssTocTime = 1;
		state->txState = CHAN_TXSTATE_ACTIVE;
		assert(!process_once(state) && state->txState == CHAN_TXSTATE_IDLE);
		state->txState = CHAN_TXSTATE_TOC;
		state->txHangTime = 0;
		state->txCtcssState = 0;
		state->txTimerSampleRemainder = 80 * 48;
		assert(!process_once(state) && state->txState == CHAN_TXSTATE_IDLE);
		state->txCtcssOption = 2;
		state->txCtcssTocTime = 1;
		assert(!process_once(state) && !state->txCtcssTurnoffTimer);
		state->txState = CHAN_TXSTATE_FINISHING;
		state->txBufferClear = 0;
		state->txFinishTimer = 0;
		assert(!process_once(state) && state->txState == CHAN_TXSTATE_IDLE);
	}
	state->txrxblankingtimer = 1;
	state->txrxBlankingSampleRemainder = 48;
	assert(!process_once(state) && !state->txrxblankingtimer);
	i32 timer = 0;
	assert(urp_radio_timer_consume(&timer, 5) == 5);
	processing_test_descriptor = NULL;
	assert(!urp_radio_core_initialize());
	assert(!urp_radio_destroy(state));
}

#endif
