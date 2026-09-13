/** @file
 * @brief Focused rejected-shape and transactional radio primitive checks.
 */
#ifndef USBRADIOPLUS_RADIO_PRIMITIVE_BOUNDARY_CASES_H
#define USBRADIOPLUS_RADIO_PRIMITIVE_BOUNDARY_CASES_H

int urp_radio_receive_frontend_portable(urp_radio_stage *stage);
int urp_radio_fir_portable(urp_radio_stage *stage);
int gp_inte_00_portable(urp_radio_stage *stage);
int center_slicer_portable(urp_radio_stage *stage);
int measure_block_portable(urp_radio_stage *stage);
int delay_line_portable(urp_radio_stage *stage);
int urp_radio_fir_s16_spans_overlap(const i16 *left, size_t left_count, const i16 *right,
				    size_t right_count);

/** @brief Verify overlap arithmetic without dereferencing artificial boundary addresses. */
static void test_radio_primitive_overlap_boundaries(void)
{
	i16 storage[16] = {0};
	assert(!urp_radio_fir_s16_spans_overlap(NULL, 1U, storage, 1U));
	assert(!urp_radio_fir_s16_spans_overlap(storage, 1U, NULL, 1U));
	assert(!urp_radio_fir_s16_spans_overlap(storage, 0U, storage, 1U));
	assert(!urp_radio_fir_s16_spans_overlap(storage, 1U, storage, 0U));
	assert(urp_radio_fir_s16_spans_overlap(storage, SIZE_MAX, storage, 1U));
	assert(urp_radio_fir_s16_spans_overlap(storage, 1U, storage, SIZE_MAX));
	assert(urp_radio_fir_s16_spans_overlap((const i16 *)(UINTPTR_MAX - 1U), 2U, storage, 1U));
	assert(urp_radio_fir_s16_spans_overlap(storage, 1U, (const i16 *)(UINTPTR_MAX - 1U), 2U));
	assert(urp_radio_fir_s16_spans_overlap(storage, 4U, storage + 2, 4U));
	assert(!urp_radio_fir_s16_spans_overlap(storage, 4U, storage + 4, 4U));
	assert(!urp_radio_fir_s16_spans_overlap(storage + 4, 4U, storage, 4U));
}

/** @brief Every unsupported FIR shape is rejected before modifying caller storage. */
static void test_radio_primitive_fir_boundaries(void)
{
	urp_radio_state *channel = create_fir_test_radio();
	i16 input[8] = {100, -100, 0};
	i16 output[8] = {0};
	i16 history[3] = {0};
	i16 coefficients[3] = {10000, -5000, 3000};
	urp_radio_stage valid = *channel->spsRxLsd;
	configure_fir_stage(&valid, input, output, 3, coefficients, 3, 256, 256, 10000);
	valid.x = history;
	assert(!urp_radio_fir_portable(&valid));
	for (unsigned int invalid = 0U; invalid < 27U; ++invalid) {
		urp_radio_stage stage = valid;
		urp_radio_stage *selected = &stage;
		switch (invalid) {
		case 0:
			selected = NULL;
			break;
		case 1:
			stage.parentChan = NULL;
			break;
		case 2:
			stage.source = NULL;
			break;
		case 3:
			stage.sink = NULL;
			break;
		case 4:
			stage.x = NULL;
			break;
		case 5:
			stage.coef = NULL;
			break;
		case 6:
			stage.enabled = 0;
			break;
		case 7:
			stage.option = 1;
			break;
		case 8:
			stage.nSamples = 0;
			break;
		case 9:
			stage.nx = 0;
			break;
		case 10:
			stage.ncoef = 1;
			break;
		case 11:
			stage.size_x = sizeof(i32);
			break;
		case 12:
			stage.size_coef = sizeof(i32);
			break;
		case 13:
			stage.decimate = 2;
			break;
		case 14:
			stage.interpolate = 2;
			break;
		case 15:
			stage.numChanOut = 2;
			break;
		case 16:
			stage.selChanOut = 1;
			break;
		case 17:
			stage.mixOut = 1;
			break;
		case 18:
			stage.monoOut = 1;
			break;
		case 19:
			stage.setpt = 1;
			break;
		case 20:
			stage.source = stage.sink;
			break;
		case 21:
			stage.source = stage.x;
			break;
		case 22:
			stage.sink = stage.x;
			break;
		case 23:
			stage.source = stage.coef;
			break;
		case 24:
			stage.sink = stage.coef;
			break;
		case 25:
			stage.x = stage.coef;
			break;
		default:
			stage.calcAdjust = 0;
			break;
		}
		assert(urp_radio_fir_portable(selected) == -1);
	}
	assert(!urp_radio_destroy(channel));
}

/** @brief Validate native frontend shape, capacity and aliasing before any portable update. */
static void test_radio_primitive_frontend_boundaries(void)
{
	urp_radio_state *channel = create_fir_test_radio();
	i16 input[SAMPLES_PER_BLOCK * 12] = {0};
	i16 output[SAMPLES_PER_BLOCK + 1] = {0};
	urp_radio_stage valid = *channel->spsRx;
	valid.source = input;
	valid.sink = output;
	valid.nativeSamples = 6U;
	valid.nSamples = 1;
	const i16 prior_samples = channel->nSamplesRx;
	const i16 prior_lpf = channel->rxlpf;
	const i16 prior_cd = channel->rxCdType;
	assert(!urp_radio_receive_frontend_portable(&valid));
	valid.nativeSamples = 6U;
	for (unsigned int invalid = 0U; invalid < 25U; ++invalid) {
		urp_radio_stage stage = valid;
		urp_radio_stage *selected = &stage;
		channel->nSamplesRx = prior_samples;
		channel->rxlpf = prior_lpf;
		channel->rxCdType = prior_cd;
		channel->fever = channel->tracetype = 0;
		switch (invalid) {
		case 0:
			selected = NULL;
			break;
		case 1:
			stage.parentChan = NULL;
			break;
		case 2:
			stage.source = NULL;
			break;
		case 3:
			stage.sink = NULL;
			break;
		case 4:
			stage.x = NULL;
			break;
		case 5:
			stage.enabled = 0;
			break;
		case 6:
			stage.nSamples = -1;
			break;
		case 7:
			stage.nativeSamples = 0U;
			break;
		case 8:
			stage.nativeSamples = SAMPLES_PER_BLOCK * 6U + 1U;
			break;
		case 9:
			stage.decimate = 0;
			break;
		case 10:
			stage.nx = 0;
			break;
		case 11:
			stage.calcAdjust = 0;
			break;
		case 12:
			channel->rxCdType = CD_XPMR_VOX;
			break;
		case 13:
			channel->fever = 1;
			break;
		case 14:
			channel->tracetype = 1;
			break;
		case 15:
			channel->rxlpf = -1;
			break;
		case 16:
			channel->rxlpf = INT16_MAX;
			break;
		case 17:
			stage.nx--;
			break;
		case 18:
			stage.source = stage.sink;
			break;
		case 19:
			stage.source = stage.x;
			break;
		case 20:
			stage.sink = stage.x;
			break;
		case 21:
			channel->nSamplesRx = 0;
			break;
		case 22:
			channel->receiveFrontendNativeCapacity = 0U;
			break;
		case 23:
			stage.decimate = -1;
			break;
		default:
			channel->nSamplesRx = -1;
			break;
		}
		assert(urp_radio_receive_frontend_portable(selected) == -1);
		channel->receiveFrontendNativeCapacity = SAMPLES_PER_BLOCK * 6U;
	}
	channel->nSamplesRx = prior_samples;
	channel->rxlpf = prior_lpf;
	channel->rxCdType = prior_cd;
	for (unsigned int invalid = 0U; invalid < 11U; ++invalid) {
		urp_radio_stage stage = valid;
		urp_radio_stage *selected = &stage;
		channel->rxBaseCapacity = SAMPLES_PER_BLOCK;
		channel->nSamplesRx = prior_samples;
		switch (invalid) {
		case 0:
			selected = NULL;
			break;
		case 1:
			stage.enabled = 0;
			break;
		case 2:
			stage.parentChan = NULL;
			break;
		case 3:
			stage.source = NULL;
			break;
		case 4:
			stage.sink = NULL;
			break;
		case 5:
			stage.x = NULL;
			break;
		case 6:
			stage.decimate = 0;
			break;
		case 7:
			stage.nSamples = -1;
			break;
		case 8:
			stage.nativeSamples = SAMPLES_PER_BLOCK * 6U + 1U;
			break;
		case 9:
			channel->rxBaseCapacity = 0U;
			break;
		default:
			channel->nSamplesRx = 0;
			break;
		}
		assert(urp_radio_receive_frontend(selected) == 1);
	}
	assert(!urp_radio_destroy(channel));
}

/** @brief Recursive, center, envelope and delay errors leave their state uncommitted. */
static void test_radio_primitive_stateful_boundaries(void)
{
	urp_radio_state template = {.pRxCodeSrc = "0",
				    .pTxCodeSrc = "0",
				    .pTxCodeDefault = "0",
				    .rxDemod = RX_AUDIO_FLAT,
				    .rxSquelchDelay = 10};
	urp_radio_state *channel = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	i16 input[8] = {100, -100};
	i16 output[8] = {0};
	assert(channel && channel->spsRxDeEmp && channel->spsDelayLine);
	assert(gp_inte_00_portable(NULL) == -1);
	assert(center_slicer_portable(NULL) == -1);
	assert(measure_block_portable(NULL) == -1);
	assert(delay_line_portable(NULL) == -1);
	for (unsigned int invalid = 0U; invalid < 5U; ++invalid) {
		urp_radio_stage stage = *channel->spsRxDeEmp;
		stage.source = input;
		stage.sink = output;
		stage.nSamples = 2;
		if (invalid == 0U)
			stage.parentChan = NULL;
		if (invalid == 1U)
			stage.nSamples = -1;
		if (invalid == 2U)
			stage.coef = NULL;
		if (invalid == 3U)
			stage.x = NULL;
		if (invalid == 4U)
			channel->deemphasisIntegratorF32Capacity = 0U;
		assert(gp_inte_00_portable(&stage) == -1);
	}
	{
		urp_radio_stage stage = {.parentChan = NULL, .nSamples = 1, .enabled = 1};
		assert(center_slicer_portable(&stage) == -1);
		stage.parentChan = channel;
		stage.nSamples = -1;
		assert(center_slicer_portable(&stage) == -1);
	}
	{
		urp_radio_stage stage = *channel->spsMeasure;
		stage.nSamples = 0;
		stage.source = NULL;
		assert(measure_block_portable(&stage) == 0);
	}
	for (unsigned int invalid = 0U; invalid < 4U; ++invalid) {
		urp_radio_stage stage = *channel->spsMeasure;
		stage.source = input;
		stage.sink = output;
		stage.nSamples = 2;
		if (invalid == 0U)
			stage.parentChan = NULL;
		if (invalid == 1U)
			stage.nSamples = -1;
		if (invalid == 2U)
			stage.source = NULL;
		if (invalid == 3U)
			channel->measureF32Capacity = 0U;
		assert(measure_block_portable(&stage) == -1);
	}
	{
		urp_radio_stage stage = *channel->spsDelayLine;
		stage.source = input;
		stage.sink = output;
		stage.nSamples = 2;
		stage.parentChan = NULL;
		assert(delay_line_portable(&stage) == -1);
		stage.parentChan = channel;
		channel->delayF32FrameCapacity = 0U;
		assert(delay_line_portable(&stage) == -1);
		stage.nSamples = -1;
		assert(DelayLine(&stage) == 0);
	}
	assert(!urp_radio_destroy(channel));
}

/** @brief Run independent primitive shape, overlap and transactional-error checks. */
static void test_radio_primitive_boundaries(void)
{
	assert(urp_radio_core_initialize() == 0);
	test_radio_primitive_overlap_boundaries();
	test_radio_primitive_fir_boundaries();
	test_radio_primitive_frontend_boundaries();
	test_radio_primitive_stateful_boundaries();
}
#endif
