#include "../src/usbradioplus_radio.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int allocations_until_failure = -1;

void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

void *__ast_calloc(size_t count, size_t size, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	if (allocations_until_failure == 0) {
		return NULL;
	}
	if (allocations_until_failure > 0) {
		--allocations_until_failure;
	}
	return calloc(count, size);
}

void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

static void test_frequency_lookup(void)
{
	assert(urp_ctcss_frequency_index(67.0F) == 0);
	assert(urp_ctcss_frequency_index(250.3F) == CTCSS_NUM_CODES - 1);
	assert(urp_ctcss_frequency_index(123.4F) == CTCSS_NULL);
}

static void test_string_parser(void)
{
	char *storage = NULL;
	char **items = NULL;
	char source[] = "67.0, 100.0,123.0";
	assert(string_parse(source, &storage, &items) == 3);
	assert(!strcmp(items[0], "67.0"));
	assert(!strcmp(items[1], "100.0"));
	assert(!strcmp(items[2], "123.0"));
	assert(string_parse(source, &storage, &items) == 3);
	free(items);
	free(storage);
	storage = NULL;
	items = NULL;
	char spaced[] = " 67.0,, 100.0";
	assert(string_parse(spaced, &storage, &items) == 3);
	free(items);
	free(storage);
}

static void test_debug_buffers(void)
{
	t_sdbg debug = {0};
	int16_t source[SAMPLES_PER_BLOCK];

	for (int i = 0; i < SAMPLES_PER_BLOCK; ++i) {
		source[i] = (int16_t)i;
	}
	for (int i = 0; i < URP_RADIO_DEBUG_CHANNELS; ++i) {
		debug.point[i] = -1;
	}
	strace(0, &debug, 0, 123);
	debug.mode = 1;
	strace(1, &debug, 0, 456);
	debug.point[0] = 2;
	strace(0, &debug, 3, 123);
	assert(debug.buffer[3 * URP_RADIO_DEBUG_CHANNELS + 2] == 123);
	debug.source[4] = source;
	strace2(&debug);
	assert(debug.buffer[7 * URP_RADIO_DEBUG_CHANNELS + 4] == 7);
}

static void test_signal_primitives(void)
{
	int16_t input[8] = {-32767, 32767, -2000, 2000, -10, 10, 0, 0};
	int16_t output[32];
	int16_t history[4] = {0};
	int16_t coefficients[1] = {M_Q15};
	int16_t delay[16];
	urp_radio_state parent = {0};
	urp_radio_stage stage = {0};

	memset(output, 1, sizeof(output));
	stage.parentChan = &parent;
	stage.source = input;
	stage.sink = output;
	stage.x = history;
	stage.coef = coefficients;
	stage.nx = 1;
	stage.nSamples = 8;
	stage.decimate = stage.decimator = stage.interpolate = 1;
	stage.inputGain = stage.outputGain = M_Q8;
	stage.calcAdjust = M_Q15;
	assert(urp_radio_fir(&stage) == 1);
	stage.enabled = 1;
	stage.option = 3;
	stage.numChanOut = 2;
	stage.selChanOut = 1;
	assert(urp_radio_fir(&stage) == 0);
	assert(output[1] == 0 && !stage.enabled);
	stage.enabled = 1;
	stage.option = 3;
	stage.monoOut = 1;
	assert(urp_radio_fir(&stage) == 0);

	stage.enabled = 1;
	stage.option = 0;
	stage.mixOut = 1;
	stage.monoOut = 1;
	stage.interpolate = 2;
	stage.decimate = stage.decimator = -1;
	stage.setpt = 100;
	stage.hyst = 10;
	stage.discfactor = 1;
	stage.compOut = 1;
	memset(output, 0, sizeof(output));
	assert(urp_radio_fir(&stage) == 0);
	assert(stage.compOut);

	stage.monoOut = 0;
	stage.mixOut = 1;
	stage.interpolate = 1;
	stage.decimate = stage.decimator = 1;
	stage.inputGain = 32767;
	stage.outputGain = 32767;
	assert(urp_radio_fir(&stage) == 0);
	stage.mixOut = 0;
	stage.monoOut = 1;
	stage.inputGain = stage.outputGain = M_Q8;
	assert(urp_radio_fir(&stage) == 0);
	stage.monoOut = 0;
	stage.calcAdjust = 1;
	input[0] = 32767;
	input[1] = -32767;
	assert(urp_radio_fir(&stage) == 0);
	assert(output[0] == 32767 && output[1] == -32767);
	stage.calcAdjust = M_Q15;
	memset(input, 0, sizeof(input));
	stage.monoOut = 0;
	stage.setpt = 1000;
	stage.compOut = 1;
	stage.amax = 0;
	stage.amin = 0;
	stage.discounteru = stage.discounterl = 10;
	assert(urp_radio_fir(&stage) == 0 && !stage.compOut);
	stage.nSamples = 1;
	stage.amax = 190;
	stage.amin = 0;
	stage.setpt = 100;
	stage.hyst = 10;
	stage.compOut = 1;
	stage.discounteru = stage.discounterl = 10;
	history[0] = 0;
	input[0] = 0;
	assert(urp_radio_fir(&stage) == 0 && stage.compOut);
	stage.nSamples = 8;

	stage.enabled = 0;
	assert(gp_inte_00(&stage) == 1);
	stage.enabled = 1;
	stage.nSamples = 8;
	stage.outputGain = M_Q8;
	coefficients[0] = 1000;
	int16_t integrator_coefficients[2] = {1000, 1000};
	int32_t integrator_state[1] = {0};
	stage.coef = integrator_coefficients;
	stage.x = integrator_state;
	assert(gp_inte_00(&stage) == 0);

	stage.enabled = 0;
	assert(CenterSlicer(&stage) == 1);
	stage.enabled = 1;
	input[0] = -32767;
	input[1] = 32767;
	input[2] = -2000;
	input[3] = 2000;
	stage.buff = delay;
	stage.inputGainB = 100;
	stage.setpt = 1000;
	stage.discfactor = 1;
	parent.pRxLsdCen = output + 16;
	assert(CenterSlicer(&stage) == 0);
	for (int i = 0; i < 8; ++i) {
		assert(delay[i] >= -100 && delay[i] <= 100);
	}
	stage.nSamples = 1;
	stage.amax = stage.amin = 0;
	input[0] = 100;
	assert(CenterSlicer(&stage) == 0);
	stage.nSamples = 8;

	stage.enabled = 0;
	assert(MeasureBlock(&stage) == 1);
	stage.enabled = 1;
	stage.option = 3;
	assert(MeasureBlock(&stage) == 1 && !stage.enabled);
	stage.enabled = 1;
	stage.option = 0;
	stage.discfactor = 1;
	stage.setpt = 100;
	assert(MeasureBlock(&stage) == 0 && stage.compOut);
	stage.sink = NULL;
	stage.setpt = 32767;
	assert(MeasureBlock(&stage) == 0 && !stage.compOut);

	stage.enabled = 0;
	stage.b.outzero = 0;
	stage.b.dirty = 1;
	stage.sink = output;
	stage.buff = delay;
	stage.buffSize = 16;
	stage.nSamples = 8;
	memset(output, 1, sizeof(output));
	assert(DelayLine(&stage) == 0 && !stage.b.dirty && output[0] == 0);
	stage.enabled = 1;
	stage.b.outzero = 1;
	assert(DelayLine(&stage) == 0);
	stage.b.outzero = 0;
	stage.buffLead = 4;
	assert(DelayLine(&stage) == 0 && stage.b.dirty);
	stage.buffInIndex = 8;
	assert(DelayLine(&stage) == 0);
}

static void test_create_process_destroy(void)
{
	urp_radio_state template = {0};
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	int16_t output[SAMPLES_PER_BLOCK] = {0};

	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	template.rxCdType = CD_XPMR_NOISE;
	template.rxDemod = RX_AUDIO_FLAT;
	template.rxSquelchPoint = 50;
	template.rxCarrierHyst = 2500;
	template.rxlpf = 0;
	template.rxhpf = 0;
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	assert(state->rxNoiseSquelchEnable);
	assert(state->rxDeEmpEnable);
	assert(state->b.ctcssRxEnable && state->b.ctcssTxEnable);
	assert(!urp_radio_process(state, input, output, NULL));
	assert(!urp_radio_destroy(state));
}

static void test_create_variants(void)
{
	static const char *const receive_codes[] = {"0",     "67.0", "250.3",	  "67.0,100.0",
						    "123.4", "67.0", "100.0,67.0"};
	static const char *const transmit_codes[] = {"0",     "0",	    "250.3",	 "67.0",
						     "123.4", "67.0,100.0", "100.0,67.0"};
	static const char *const default_codes[] = {"0",     "67.0", "250.3", "123.4",
						    "100.0", "0",    "100.0"};
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	int16_t output[SAMPLES_PER_BLOCK] = {0};
	int16_t transmit[SAMPLES_PER_BLOCK * 6 * 2] = {0};

	for (size_t variant = 0; variant < sizeof(receive_codes) / sizeof(receive_codes[0]);
	     ++variant) {
		urp_radio_state template = {0};
		template.pRxCodeSrc = (char *)receive_codes[variant];
		template.pTxCodeSrc = (char *)transmit_codes[variant];
		template.pTxCodeDefault = (char *)default_codes[variant];
		template.rxCdType = variant == 1 ? CD_XPMR_VOX : CD_IGNORE;
		template.rxDemod = variant == 2 ? RX_AUDIO_FLAT : RX_AUDIO_SPEAKER;
		template.rxSquelchDelay = variant == 3 ? 999 : (int16_t)variant;
		template.rxSqVoxAdj = variant == 1 ? 100 : 0;
		template.rxCarrierHyst = variant == 4 ? 0 : 100;
		template.rxlpf = variant == 4 ? -1 : (variant == 5 ? 999 : 0);
		template.rxhpf = variant == 4 ? -1 : (variant == 5 ? 999 : 0);
		template.tracetype = (int16_t)(variant + 1);
		urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(state);
		state->b.rxCapture = 1;
		state->txrxblankingtimer = MS_PER_FRAME;
		assert(!urp_radio_process(state, input, output, transmit));
		assert(!urp_radio_destroy(state));
	}

	urp_radio_state *defaults = urp_radio_create(NULL, SAMPLES_PER_BLOCK);
	assert(defaults);
	assert(defaults->txMixA == TX_OUT_VOICE && defaults->txMixB == TX_OUT_LSD);
	assert(!urp_radio_destroy(defaults));
}

static int process_once(urp_radio_state *state)
{
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	int16_t output[SAMPLES_PER_BLOCK] = {0};
	int16_t transmit[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	return urp_radio_process(state, input, output, transmit);
}

static void test_runtime_state_machine(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	template.rxCdType = CD_XPMR_NOISE;
	template.rxDemod = RX_AUDIO_FLAT;
	template.rxCarrierHyst = 100;
	assert(urp_radio_process(NULL, NULL, NULL, NULL) == 1);

	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	state->b.ctcssRxEnable = 0;
	state->txrxblankingtimer = 2 * MS_PER_FRAME;
	assert(process_once(state) == 0 && state->txrxblankingtimer == MS_PER_FRAME);
	int16_t direct_input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	assert(urp_radio_process(state, direct_input, NULL, NULL) == 0);

	state->rxCpuSaver = 1;
	assert(process_once(state) == 0);
	assert(state->b.rxhalted);
	assert(process_once(state) == 0 && state->b.rxhalted);
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	assert(!state->b.rxhalted && state->txState == CHAN_TXSTATE_ACTIVE);
	assert(state->txPttOut && state->txCtcssState == 1);
	assert(process_once(state) == 0);

	state->txTocType = TOC_NONE;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_FINISHING);
	assert(process_once(state) == 0);
	assert(process_once(state) == 0);
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_COMPLETE);
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_IDLE && !state->txPttOut);

	state->txPttIn = 1;
	assert(process_once(state) == 0);
	state->txTocType = TOC_NOTONE;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC);
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_ACTIVE);
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	state->txHangTime = 2;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_TOC);
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_FINISHING);

	state->txState = CHAN_TXSTATE_ACTIVE;
	state->smode = SMODE_CTCSS;
	state->txTocType = TOC_PHASE;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->txCtcssState == 2);
	state->txHangTime = 0;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_TOC);
	state->txCtcssState = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_FINISHING);

	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->txCpuSaver = 1;
	assert(process_once(state) == 1 && state->b.txhalted);
	state->txCpuSaver = 0;
	assert(process_once(state) == 0 && !state->b.txhalted);

	state->txCtcssOption = 1;
	assert(process_once(state) == 0 && state->txCtcssState == 1);
	state->txCtcssOption = 2;
	assert(process_once(state) == 0 && state->txCtcssState == 2);
	state->txCtcssTurnoffTimer = 2 * MS_PER_FRAME;
	assert(process_once(state) == 0 && state->txCtcssOption == 0);
	assert(process_once(state) == 0 && state->txCtcssOption == 3);
	assert(process_once(state) == 0 && state->txCtcssState == 0);

	state->txsettletimer = 2 * MS_PER_FRAME;
	state->txPttHid = 1;
	assert(process_once(state) == 0 && state->txsettletimer == MS_PER_FRAME);
	state->txsettletimer = 1;
	assert(process_once(state) == 0 && state->txsettletimer == 0);

	state->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	state->rxCtcssMap[state->rxCtcss->decode] = urp_ctcss_frequency_index(67.0F);
	state->smode = SMODE_NULL;
	assert(process_once(state) == 0 && state->smode == SMODE_CTCSS);
	state->rxCtcss->decode = CTCSS_NULL;
	assert(process_once(state) == 0);
	state->smodetimer = 1;
	assert(process_once(state) == 0 && state->smode == SMODE_NULL);
	state->b.ctcssRxEnable = 1;
	state->rxCpuSaver = 1;
	state->b.rxhalted = 1;
	state->rxCarrierDetect = 0;
	assert(process_once(state) == 0 && state->b.rxhalted);
	state->rxCtcss->enabled = 0;
	state->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	assert(process_once(state) == 0 && state->b.rxhalted);
	state->rxCpuSaver = 0;
	state->b.rxhalted = 0;
	state->rxCtcss->enabled = 0;
	state->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	state->rxCtcssMap[state->rxCtcss->decode] = CTCSS_RXONLY;
	state->smode = SMODE_CTCSS;
	state->lastrxdecode = CTCSS_NULL;
	assert(process_once(state) == 0);
	state->lastrxdecode = CTCSS_NULL;
	state->txCtcssFreq10 = 670;
	state->rxCtcssMap[state->rxCtcss->decode] = urp_ctcss_frequency_index(67.0F);
	assert(process_once(state) == 0 && state->txCtcssFreq10 == 670);
	state->smode = SMODE_DCS;
	assert(process_once(state) == 0);
	state->b.ctcssRxEnable = 0;
	state->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	state->rxCtcssMap[state->rxCtcss->decode] = urp_ctcss_frequency_index(67.0F);
	state->smode = SMODE_CTCSS;
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttIn = 1;
	assert(process_once(state) == 0 && state->txCtcssFreq10 == 670);
	state->txPttIn = 0;
	state->smode = SMODE_NULL;
	state->rxCtcss->decode = CTCSS_NULL;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_FINISHING);
	state->txState = CHAN_TXSTATE_ACTIVE;
	state->txPttOut = 1;
	state->smode = SMODE_CTCSS;
	state->b.txCtcssInhibit = 0;
	state->b.ctcssTxEnable = 0;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_FINISHING);
	state->txState = CHAN_TXSTATE_TOC;
	state->txPttIn = 1;
	state->smode = SMODE_DCS;
	state->txHangTime = 0;
	state->txCtcssState = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_TOC);
	state->txPttIn = 0;
	state->txsettletimer = MS_PER_FRAME;
	state->txPttHid = 0;
	assert(process_once(state) == 0 && state->txsettletimer == MS_PER_FRAME);
	assert(!urp_radio_destroy(state));

	template.rxCdType = CD_XPMR_VOX;
	template.voxHangTime = 40;
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	state->spsRxVox->setpt = -1;
	assert(process_once(state) == 0 && state->rxCarrierDetect);
	state->spsRxVox->setpt = 32767;
	assert(process_once(state) == 0 && state->rxCarrierDetect);
	assert(process_once(state) == 0 && !state->rxCarrierDetect);
	assert(!urp_radio_destroy(state));

	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "0";
	template.rxCdType = CD_IGNORE;
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	state->smode = SMODE_DCS;
	state->txcodedefaultsmode = SMODE_NULL;
	state->txPttIn = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	assert(!state->txCtcssEnabled);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->txcodedefaultsmode = SMODE_DCS;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->b.txCtcssInhibit = 0;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->smode = SMODE_NULL;
	state->txcodedefaultsmode = SMODE_DCS;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->smode = SMODE_NULL;
	state->txcodedefaultsmode = SMODE_CTCSS;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->smode = SMODE_CTCSS;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->b.txCtcssInhibit = 0;
	state->rxCtcss->decode = urp_ctcss_frequency_index(100.0F);
	state->rxCtcssMap[state->rxCtcss->decode] = CTCSS_RXONLY;
	assert(process_once(state) == 0 && !state->txCtcssEnabled);
	state->txPttIn = 0;
	state->txState = CHAN_TXSTATE_ACTIVE;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_FINISHING);
	assert(!urp_radio_destroy(state));
}

static void prepare_single_ctcss_detector(urp_radio_state *state, int index)
{
	for (int i = 0; i < CTCSS_NUM_CODES; ++i) {
		state->rxCtcssMap[i] = CTCSS_NULL;
	}
	state->rxCtcssMap[index] = index;
	state->rxCtcss->enabled = 1;
	state->rxCtcss->decode = CTCSS_NULL;
	state->rxCtcss->BlankingTimer = 0;
	state->rxCarrierDetect = 1;
	state->nSamplesRx = 1;
	state->rxCtcss->input[0] = 0;
	urp_ctcss_tone_detector *detector = &state->rxCtcss->tdet[index];
	detector->counter = 0;
	detector->counterFactor = CTCSS_SCOUNT_MUL;
	detector->binFactor = M_Q15;
	detector->fudgeFactor = 1;
	detector->peak = 0;
	detector->setpt = -1;
	detector->hyst = 0;
	detector->decode = 0;
	detector->z[0] = detector->z[1] = detector->z[2] = detector->z[3] = 0;
}

static void test_ctcss_decoder_states(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	const int index = urp_ctcss_frequency_index(100.0F);
	prepare_single_ctcss_detector(state, index);
	assert(urp_ctcss_decode(state) == 0);
	assert(state->rxCtcss->decode == index);

	urp_ctcss_tone_detector *detector = &state->rxCtcss->tdet[index];
	detector->counter = 0;
	detector->setpt = 1000;
	detector->hyst = 100;
	detector->decode = 1;
	assert(urp_ctcss_decode(state) == 0);
	assert(state->rxCtcss->decode == CTCSS_NULL);
	assert(state->rxCtcss->BlankingTimer > 0);

	state->rxCtcss->BlankingTimer = 1;
	state->nSamplesRx = 2;
	state->rxCtcss->enabled = 1;
	detector->counter = 0;
	assert(urp_ctcss_decode(state) == 0);
	assert(state->rxCtcss->BlankingTimer == 0);

	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->relax = 1;
	state->rxCtcss->decode = index;
	detector->counter = 0;
	detector->setpt = 1000;
	detector->hyst = 100;
	detector->decode = 2;
	assert(urp_ctcss_decode(state) == 0);

	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->input[0] = 100;
	assert(urp_ctcss_decode(state) == 0);
	prepare_single_ctcss_detector(state, index);
	detector->decode = detector->fudgeFactor * 32;
	assert(urp_ctcss_decode(state) == 0);

	prepare_single_ctcss_detector(state, index);
	state->rxCtcssMap[urp_ctcss_frequency_index(67.0F)] = index;
	state->rxCtcss->decode = index;
	assert(urp_ctcss_decode(state) == 0);

	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->BlankingTimer = 100;
	assert(urp_ctcss_decode(state) == 0);
	assert(state->rxCtcss->decode == CTCSS_NULL);

	prepare_single_ctcss_detector(state, index);
	detector->zd = 1000;
	detector->setpt = 1000;
	assert(urp_ctcss_decode(state) == 0 && detector->dvd < 0);
	prepare_single_ctcss_detector(state, index);
	detector->zd = 1000;
	detector->dvd = -13;
	detector->setpt = 1000;
	assert(urp_ctcss_decode(state) == 0 && detector->dvd < -13);
	detector->counter = 0;
	detector->zd = 0;
	assert(urp_ctcss_decode(state) == 0);

	prepare_single_ctcss_detector(state, index);
	detector->dvd = -14;
	detector->setpt = 1000;
	assert(urp_ctcss_decode(state) == 0 && detector->dvu > 0);
	detector->counter = 0;
	detector->dvd = 0;
	assert(urp_ctcss_decode(state) == 0);

	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->decode = index;
	state->rxCtcss->relax = 0;
	detector->dvu = 26;
	assert(urp_ctcss_decode(state) == 0 && detector->dvu == 0);
	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->decode = index;
	detector->setpt = 1000;
	detector->hyst = -1;
	detector->decode = 2;
	assert(urp_ctcss_decode(state) == 0);

	state->rxCtcss->enabled = 0;
	assert(urp_ctcss_decode(state) == 1);
	state->nSamplesRx = SAMPLES_PER_BLOCK;
	assert(!urp_radio_destroy(state));
}

static void test_frontend_edges(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "250.3";
	template.pTxCodeSrc = "250.3";
	template.pTxCodeDefault = "250.3";
	template.rxCdType = CD_XPMR_NOISE;
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2];
	for (size_t i = 0; i < sizeof(input) / sizeof(input[0]); ++i) {
		input[i] = (i & 2U) ? 32767 : -32767;
	}
	state->spsRx->source = input;
	state->spsRx->enabled = 0;
	assert(urp_radio_receive_frontend(state->spsRx) == 1);
	state->spsRx->enabled = 1;
	state->fever = 1;
	state->rxNoiseFilType = 1;
	state->spsRx->calcAdjust = 1;
	state->spsRx->outputGain = 32767;
	assert(urp_radio_receive_frontend(state->spsRx) == 0);

	allocations_until_failure = 4;
	assert(urp_radio_parse_codes(state) == 1);
	allocations_until_failure = -1;
	assert(!urp_radio_destroy(state));
}

static void test_cpu_saver_predicates(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "0";
	template.rxCdType = CD_IGNORE;
	template.rxDemod = RX_AUDIO_SPEAKER;
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);

	state->rxCpuSaver = 1;
	state->rxCarrierDetect = 1;
	assert(process_once(state) == 0);
	state->rxCarrierDetect = 0;
	state->smode = SMODE_DCS;
	assert(process_once(state) == 0);
	state->smode = SMODE_NULL;
	state->txPttIn = 1;
	state->txState = CHAN_TXSTATE_ACTIVE;
	assert(process_once(state) == 0);
	state->txPttIn = 0;
	state->txPttOut = 1;
	state->txState = CHAN_TXSTATE_FINISHING;
	state->txBufferClear = 2;
	assert(process_once(state) == 0);

	state->rxCpuSaver = 0;
	state->txCpuSaver = 1;
	state->txPttIn = 1;
	state->txPttOut = 0;
	state->txState = CHAN_TXSTATE_ACTIVE;
	assert(process_once(state) == 0);
	state->txPttIn = 0;
	state->txPttOut = 1;
	state->txState = CHAN_TXSTATE_FINISHING;
	state->txBufferClear = 2;
	assert(process_once(state) == 0);
	state->txPttOut = 0;
	state->txState = CHAN_TXSTATE_ACTIVE;
	assert(process_once(state) == 0);
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttIn = 0;
	state->b.txhalted = 1;
	assert(process_once(state) == 1 && state->b.txhalted);
	state->txCpuSaver = 0;
	state->b.txhalted = 0;

	state->rxCpuSaver = 1;
	state->rxCarrierDetect = 0;
	state->smode = SMODE_NULL;
	state->txState = CHAN_TXSTATE_IDLE;
	assert(process_once(state) == 0 && state->b.rxhalted);
	state->rxCpuSaver = 0;
	assert(process_once(state) == 0 && !state->b.rxhalted);
	assert(!urp_radio_destroy(state));
}

static void test_lifecycle_edges(void)
{
	allocations_until_failure = 0;
	assert(urp_radio_create(NULL, SAMPLES_PER_BLOCK) == NULL);
	urp_radio_state parent = {0};
	assert(urp_radio_stage_create(&parent) == NULL);
	allocations_until_failure = -1;
	assert(urp_radio_destroy(NULL) == 1);
	urp_radio_state *empty = calloc(1, sizeof(*empty));
	assert(empty);
	assert(urp_radio_destroy(empty) == 0);
}

static void test_allocation_failures(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	template.rxCdType = CD_XPMR_VOX;
	template.rxDemod = RX_AUDIO_FLAT;
	template.rxSquelchDelay = 10;

	int failure_index;
	for (failure_index = 0; failure_index < 512; ++failure_index) {
		allocations_until_failure = failure_index;
		urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		if (state) {
			assert(urp_radio_destroy(state) == 0);
			break;
		}
	}
	assert(failure_index > 100 && failure_index < 512);
	allocations_until_failure = -1;
}

int main(void)
{
	test_frequency_lookup();
	test_string_parser();
	test_debug_buffers();
	test_signal_primitives();
	test_create_process_destroy();
	test_create_variants();
	test_runtime_state_machine();
	test_ctcss_decoder_states();
	test_frontend_edges();
	test_cpu_saver_predicates();
	test_lifecycle_edges();
	test_allocation_failures();
	puts("native radio core tests passed");
	return 0;
}
