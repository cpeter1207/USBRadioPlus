/** @file
 * @brief Executable radio core regression and failure-path checks.
 */

#include "../src/usbradioplus_radio.h"
#include <sys/types.h>
#include "asterisk/options.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Controls injected allocations until failure failure for this test. */
static int allocations_until_failure = -1;
/** Minimal Asterisk flag state required by the radio harness. */
struct ast_flags64 {
	/** Harness flags used to script and verify host behavior. */
	uint64_t flags;
};
/** Harness ast options used to script and verify host behavior. */
struct ast_flags64 ast_options;
/** Harness option debug used to script and verify host behavior. */
int option_debug = 100;
/** Harness module debug level used to script and verify host behavior. */
static unsigned int module_debug_level = 100;
/** Harness file debug level used to script and verify host behavior. */
static unsigned int file_debug_level = 100;

/** @brief Host-API test double for ast_test_flag64; observable effects are recorded in harness
 * state.
 * @param flags Host API option bit mask.
 * @param flag Bit mask tested in the fake flags structure.
 * @return Scripted host result for the current test scenario.
 */
int ast_test_flag64(const struct ast_flags64 *flags, uint64_t flag)
{
	return (flags->flags & flag) != 0;
}

/** @brief Host-API test double for ast_debug_get_by_module; effects are recorded in this harness.
 * @param module Asterisk module reference.
 * @return Scripted host result for the current test scenario.
 */
unsigned int ast_debug_get_by_module(const char *module)
{
	return !strcmp(module, AST_MODULE) ? module_debug_level : file_debug_level;
}

/** @brief Host-API test double for ast_log; effects are recorded in this harness.
 * @param level Requested level or normalized tuning level, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param format printf-style message format.
 * @param ... Values required by the wrapped variadic API.
 */
void ast_log(int level, const char *file, int line, const char *function, const char *format, ...)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

/** @brief Host-API test double for ast_log_ap; effects are recorded in this harness.
 * @param level Requested level or normalized tuning level, as declared.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @param format printf-style message format.
 * @param arguments Formatted-message values or filter options.
 */
void ast_log_ap(int level, const char *file, int line, const char *function, const char *format,
		va_list arguments)
{
	(void)level;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
	(void)arguments;
}

/** @brief Host-API test double for __ast_calloc; effects are recorded in this harness.
 * @param count Number of elements available in the supplied block.
 * @param size Destination capacity in bytes, including the terminator for text.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 * @return Scripted host result for the current test scenario.
 */
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

/** @brief Host-API test double for __ast_free; effects are recorded in this harness.
 * @param pointer Allocated buffer passed through the failure-injection shim.
 * @param file Source filename supplied by the host API's diagnostic wrapper.
 * @param line Source line supplied by the host API's diagnostic wrapper.
 * @param function Calling function name supplied by the host API.
 */
void __ast_free(void *pointer, const char *file, int line, const char *function)
{
	(void)file;
	(void)line;
	(void)function;
	free(pointer);
}

/** @brief Verify frequency lookup. */
static void test_frequency_lookup(void)
{
	assert(urp_ctcss_frequency_index(67.0F) == 0);
	assert(urp_ctcss_frequency_index(250.3F) == CTCSS_NUM_CODES - 1);
	assert(urp_ctcss_frequency_index(123.4F) == CTCSS_NULL);
}

/** @brief Verify string parser. */
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

/** @brief Verify debug buffers. */
static void test_debug_buffers(void)
{
	t_sdbg debug = {0};
	int16_t source[SAMPLES_PER_BLOCK];

	option_debug = 0;
	module_debug_level = 0;
	file_debug_level = 0;
	ast_options.flags = 0;
	urp_radio_trace_log(0, 1, "disabled trace\n");
	urp_radio_trace_log(1, 1, "disabled debug\n");
	ast_options.flags = AST_OPT_FLAG_DEBUG_MODULE;
	urp_radio_trace_log(1, 1, "disabled module and file debug\n");
	module_debug_level = 1;
	urp_radio_trace_log(1, 1, "enabled module debug\n");
	module_debug_level = 0;
	file_debug_level = 1;
	urp_radio_trace_log(1, 1, "enabled file debug\n");
	option_debug = 100;
	module_debug_level = 100;
	file_debug_level = 100;
	urp_radio_trace_log(1, 1, "enabled trace\n");

	strace(0, NULL, 0, 123);
	strace2(NULL);
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

/** @brief Verify signal primitives. */
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

/** @brief Verify create process destroy. */
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
	template.tracelevel = 100;
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

/** @brief Verify create variants. */
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
		template.tracelevel = variant & 1U ? 100 : 0;
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

/** @brief Advance one radio-signaling block and verify its return status.
 * @param state Processor or stream state owned by the caller.
 * @return Result used by the test's assertions.
 */
static int process_once(urp_radio_state *state)
{
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	int16_t output[SAMPLES_PER_BLOCK] = {0};
	int16_t transmit[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	return urp_radio_process(state, input, output, transmit);
}

/** @brief Verify runtime state machine. */
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

/** @brief Configure exactly one correlator for a deterministic CTCSS test.
 * @param state Processor or stream state owned by the caller.
 * @param index Sample position within the trace block.
 */
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

/** @brief Verify ctcss decoder states. */
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

	/* Let an earlier enabled detector win, then visit a later enabled detector
	 * while thit names the first one. */
	const int first_index = urp_ctcss_frequency_index(67.0F);
	prepare_single_ctcss_detector(state, first_index);
	state->rxCtcssMap[index] = index;
	detector = &state->rxCtcss->tdet[index];
	detector->counter = 0;
	detector->counterFactor = CTCSS_SCOUNT_MUL;
	detector->binFactor = M_Q15;
	detector->fudgeFactor = 1;
	detector->setpt = 1000;
	detector->decode = 0;
	assert(urp_ctcss_decode(state) == 0);

	state->rxCtcss->enabled = 0;
	assert(urp_ctcss_decode(state) == 1);
	state->nSamplesRx = SAMPLES_PER_BLOCK;
	assert(!urp_radio_destroy(state));
}

/** @brief Verify frontend edges. */
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

/** @brief Preserve sample decisions across block boundaries and the existing RSSI scale. */
static void test_frontend_sample_gate(void)
{
	urp_radio_state template = {.pRxCodeSrc = "0",
				    .pTxCodeSrc = "0",
				    .pTxCodeDefault = "0",
				    .rxCdType = CD_XPMR_NOISE};
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	urp_radio_state *whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	urp_radio_state *split = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(whole && split && whole->rxCarrierGate && split->rxCarrierGate);
	whole->spsRx->setpt = split->spsRx->setpt = 7000;
	whole->spsRx->hyst = split->spsRx->hyst = 500;
	split->spsRx->nSamples = SAMPLES_PER_BLOCK / 2;

	for (unsigned int block = 0; block < 97; ++block) {
		double split_power = 0.0;
		/* Train the idle reference, receive a fully quieted carrier, then
		 * remove it halfway through a block. No detector state is reset. */
		for (size_t i = 0; i < SAMPLES_PER_BLOCK * 6; ++i) {
			int noise = block < 64 || (block == 96 && i >= SAMPLES_PER_BLOCK * 3);
			input[2 * i] = noise ? ((i & 2U) ? 12000 : -12000) : 0;
		}
		whole->spsRx->source = input;
		assert(!urp_radio_receive_frontend(whole->spsRx));
		for (size_t half = 0; half < 2; ++half) {
			split->spsRx->source = input + half * SAMPLES_PER_BLOCK * 6;
			assert(!urp_radio_receive_frontend(split->spsRx));
			assert(!memcmp(whole->rxCarrierGate + half * SAMPLES_PER_BLOCK * 3,
				       split->rxCarrierGate, SAMPLES_PER_BLOCK * 3));
			split_power += (double)split->rxRssi * split->rxRssi;
		}
		assert(whole->spsRx->compOut == split->spsRx->compOut);
		/* Integer RSSI truncation loses less than two codes when the same
		 * block is metered in two halves: sqrt(sum(sample^2))/16. */
		assert(fabs(whole->rxRssi - sqrt(split_power)) < 2.0);
	}
	assert(whole->rxCarrierGate[0]);
	assert(!whole->rxCarrierGate[SAMPLES_PER_BLOCK * 6 - 1]);
	assert(whole->spsRx->compOut);

	/* VOX keeps its separate detector and never consumes the DSP mask. */
	whole->rxCdType = CD_XPMR_VOX;
	memset(whole->rxCarrierGate, 0x5a, SAMPLES_PER_BLOCK * 6);
	int previous_rssi = whole->rxRssi;
	assert(!urp_radio_receive_frontend(whole->spsRx));
	assert(whole->rxRssi == previous_rssi);
	for (size_t i = 0; i < SAMPLES_PER_BLOCK * 6; ++i)
		assert(whole->rxCarrierGate[i] == 0x5a);
	assert(!urp_radio_destroy(split));
	assert(!urp_radio_destroy(whole));
}

/** @brief Fill one stereo block with bounded, reproducible Gaussian-like discriminator noise.
 * @param input Stereo PCM destination; only the left channel carries discriminator audio.
 * @param random_state Persistent nonzero xorshift seed, independent of block boundaries.
 * @param divisor Noise attenuation used to simulate carrier quieting.
 */
static void fill_discriminator_noise(int16_t *input, uint32_t *random_state, int divisor)
{
	for (size_t sample = 0; sample < SAMPLES_PER_BLOCK * 6; ++sample) {
		int sum = -1530;
		/* Twelve uniforms approximate Gaussian noise without unbounded ADC
		 * peaks. The real frontend supplies the selected detector bandpass. */
		for (unsigned int term = 0; term < 12; ++term) {
			*random_state ^= *random_state << 13;
			*random_state ^= *random_state >> 17;
			*random_state ^= *random_state << 5;
			sum += (int)(*random_state >> 24);
		}
		input[2 * sample] = (int16_t)(sum * 20 / divisor);
		input[2 * sample + 1] = 0;
	}
}

/** @brief Reject idle noise troughs and close promptly after a genuinely quieted carrier. */
static void test_frontend_gaussian_noise(void)
{
	for (int noise_filter = 0; noise_filter < 2; ++noise_filter) {
		urp_radio_state template = {.pRxCodeSrc = "0",
					    .pTxCodeSrc = "0",
					    .pTxCodeDefault = "0",
					    .rxCdType = CD_XPMR_NOISE};
		urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		uint32_t random_state = 0x9e3779b9U;
		int16_t input[SAMPLES_PER_BLOCK * 6 * 2];
		double idle_level = 0.0;
		assert(state);
		state->spsRx->source = input;
		state->rxNoiseFilType = noise_filter;
		state->spsRx->setpt = 0;
		state->spsRx->hyst = 0;
		for (unsigned int block = 0; block < 100; ++block) {
			fill_discriminator_noise(input, &random_state, 1);
			assert(!urp_radio_receive_frontend(state->spsRx));
			idle_level += state->rxRssi / 100.0;
		}
		assert(idle_level > 1000.0 && idle_level < 30000.0);
		state->spsRx->setpt = (int16_t)(idle_level * 0.8);
		state->spsRx->hyst = (int16_t)(idle_level * 0.12);

		/* Thirty seconds before and after a transmission catch slow hold
		 * accumulation; all comparator and capacitor states remain live. */
		for (unsigned int phase = 0; phase < 2; ++phase) {
			for (unsigned int block = 0; block < 1500; ++block) {
				fill_discriminator_noise(input, &random_state, 1);
				assert(!urp_radio_receive_frontend(state->spsRx));
				for (size_t sample = 0; sample < SAMPLES_PER_BLOCK * 6; ++sample)
					assert(!state->rxCarrierGate[sample]);
			}
			if (phase != 0)
				break;
			/* A 64:1 voltage reduction is 36 dB of discriminator quieting. */
			for (unsigned int block = 0; block < 100; ++block) {
				fill_discriminator_noise(input, &random_state, 64);
				assert(!urp_radio_receive_frontend(state->spsRx));
				if (block >= 10) {
					for (size_t sample = 0; sample < SAMPLES_PER_BLOCK * 6;
					     ++sample)
						assert(state->rxCarrierGate[sample]);
				}
			}
			fill_discriminator_noise(input, &random_state, 1);
			const size_t carrier_loss = SAMPLES_PER_BLOCK * 6 / 4;
			for (size_t sample = 0; sample < carrier_loss; ++sample)
				input[2 * sample] /= 64;
			assert(!urp_radio_receive_frontend(state->spsRx));
			size_t first_closed = carrier_loss;
			while (first_closed < SAMPLES_PER_BLOCK * 6 &&
			       state->rxCarrierGate[first_closed])
				++first_closed;
			assert(first_closed - carrier_loss < 480); /* Less than 10 ms at 48 kHz. */
			for (size_t sample = first_closed; sample < SAMPLES_PER_BLOCK * 6; ++sample)
				assert(!state->rxCarrierGate[sample]);
			printf("noise filter %d: idle %.1f, abrupt carrier loss closes in %.3f "
			       "ms\n",
			       noise_filter, idle_level, (first_closed - carrier_loss) / 48.0);
		}
		assert(!urp_radio_destroy(state));
	}
}

/** @brief Verify cpu saver predicates. */
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

/** @brief Verify lifecycle edges. */
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

/** @brief Verify allocation failures. */
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

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
#ifdef URP_TEST_TRACE_PROGRESS
#define RUN_TEST(test)                                                                             \
	do {                                                                                       \
		fprintf(stderr, "running %s\n", #test);                                            \
		test();                                                                            \
	} while (0)
#else

#define RUN_TEST(test) test()
#endif
	RUN_TEST(test_frequency_lookup);
	RUN_TEST(test_string_parser);
	RUN_TEST(test_debug_buffers);
	RUN_TEST(test_signal_primitives);
	RUN_TEST(test_create_process_destroy);
	RUN_TEST(test_create_variants);
	RUN_TEST(test_runtime_state_machine);
	RUN_TEST(test_ctcss_decoder_states);
	RUN_TEST(test_frontend_edges);
	RUN_TEST(test_frontend_sample_gate);
	RUN_TEST(test_frontend_gaussian_noise);
	RUN_TEST(test_cpu_saver_predicates);
	RUN_TEST(test_lifecycle_edges);
	RUN_TEST(test_allocation_failures);
#undef RUN_TEST
	puts("native radio core tests passed");
	return 0;
}

/** @def RUN_TEST
 * @brief RUN TEST selection for this isolated test harness.
 */
