/** @file
 * @brief Executable radio core regression and failure-path checks.
 */

#include "../src/usbradioplus_radio.h"
#include "../src/usbradioplus_radio_core_adapter.h"
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
	strace2(NULL, 0);
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
	strace2(&debug, -1);
	strace2(&debug, SAMPLES_PER_BLOCK + 1);
	strace2(&debug, SAMPLES_PER_BLOCK);
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
}

/** @brief Configure one mono, unit-rate legacy FIR stage for a parity check. */
static void configure_fir_stage(urp_radio_stage *stage, int16_t *input, int16_t *output,
				int16_t sample_count, const int16_t *coefficients,
				int16_t coefficient_count, int32_t input_gain, int32_t output_gain,
				int32_t calc_adjust)
{
	stage->source = input;
	stage->sink = output;
	stage->nSamples = sample_count;
	stage->enabled = 1;
	stage->option = 0;
	stage->numChanOut = 1;
	stage->selChanOut = 0;
	stage->decimate = 1;
	stage->decimator = 1;
	stage->interpolate = 1;
	stage->mixOut = 0;
	stage->monoOut = 0;
	stage->setpt = 0;
	stage->hyst = 0;
	stage->compOut = 0;
	stage->ncoef = coefficient_count;
	stage->nx = coefficient_count;
	stage->size_x = sizeof(int16_t);
	stage->size_coef = sizeof(int16_t);
	stage->coef = (void *)coefficients;
	stage->inputGain = input_gain;
	stage->outputGain = output_gain;
	stage->calcAdjust = calc_adjust;
}

/** @brief Create one radio state that owns the portable FIR workspace. */
static urp_radio_state *create_fir_test_radio(void)
{
	urp_radio_state template = {
		.pRxCodeSrc = "0",
		.pTxCodeSrc = "0",
		.pTxCodeDefault = "0",
	};
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);

	assert(state && state->spsRxLsd && state->firF32Input && state->firF32Output &&
	       state->firHistoryScratch);
	return state;
}

/** @brief Compare the optional mono FIR against the retained generic C stage.
 *
 * The C reference has no parent state and therefore cannot select the optional
 * core. The live stage has the normal CTCSS/voice-filter shape and exercises
 * the F32 bridge. The remainder explicitly proves that unsupported legacy
 * routing, detector, reset, and empty-span shapes retain their C behavior.
 */
static void test_portable_fir(void)
{
	int16_t input[] = {INT16_MIN, -1234, 0, 2345, INT16_MAX, -17, 99};
	const int16_t coefficients[] = {10000, -5000, 3000};
	const int16_t initial_history[] = {1200, -700, 300};
	int16_t fallback_output[7] = {0};
	int16_t fallback_history[3];
	urp_radio_stage fallback = {0};
	urp_radio_state *portable;
	urp_radio_stage *stage;

	memcpy(fallback_history, initial_history, sizeof(fallback_history));
	fallback.x = fallback_history;
	configure_fir_stage(&fallback, input, fallback_output, 7, coefficients, 3, 300, 280, 10000);
	assert(urp_radio_fir(&fallback) == 0);
	assert(!memcmp(fallback_output,
		       (int16_t[]){28794, -16027, 9693, 2530, -31185, 15721, -8766},
		       sizeof(fallback_output)));
	assert(!memcmp(fallback_history, (int16_t[]){116, -19, -27138}, sizeof(fallback_history)));
	assert(fallback.apeak == 0);

	assert(urp_radio_core_initialize() == 0);
	portable = create_fir_test_radio();
	stage = portable->spsRxLsd;
	configure_fir_stage(stage, input, (int16_t[7]){0}, 7, coefficients, 3, 300, 280, 10000);
	memcpy(stage->x, initial_history, sizeof(initial_history));
	assert(urp_radio_fir(stage) == 0);
	assert(!memcmp(stage->sink, fallback_output, sizeof(fallback_output)));
	assert(!memcmp(stage->x, fallback_history, sizeof(fallback_history)));
	assert(stage->apeak == fallback.apeak);
	assert(!urp_radio_destroy(portable));

	for (size_t split = 1U; split < sizeof(input) / sizeof(input[0]); ++split) {
		int16_t output[7] = {0};
		urp_radio_state *split_state = create_fir_test_radio();
		urp_radio_stage *split_stage = split_state->spsRxLsd;

		configure_fir_stage(split_stage, input, output, (int16_t)split, coefficients, 3,
				    300, 280, 10000);
		memcpy(split_stage->x, initial_history, sizeof(initial_history));
		assert(urp_radio_fir(split_stage) == 0);
		split_stage->source = input + split;
		split_stage->sink = output + split;
		split_stage->nSamples = (int16_t)(sizeof(input) / sizeof(input[0]) - split);
		assert(urp_radio_fir(split_stage) == 0);
		assert(!memcmp(output, fallback_output, sizeof(output)));
		assert(!memcmp(split_stage->x, fallback_history, sizeof(fallback_history)));
		assert(!urp_radio_destroy(split_state));
	}

	/* `nx`, not `ncoef`, controls C processing; the unsupported shape must fall back. */
	portable = create_fir_test_radio();
	stage = portable->spsRxLsd;
	configure_fir_stage(stage, input, (int16_t[7]){0}, 7, coefficients, 3, 300, 280, 10000);
	stage->ncoef = 1;
	memcpy(stage->x, initial_history, sizeof(initial_history));
	memcpy(fallback_history, initial_history, sizeof(fallback_history));
	fallback.x = fallback_history;
	configure_fir_stage(&fallback, input, fallback_output, 7, coefficients, 3, 300, 280, 10000);
	fallback.ncoef = 1;
	memset(fallback_output, 0, sizeof(fallback_output));
	assert(urp_radio_fir(&fallback) == 0 && urp_radio_fir(stage) == 0);
	assert(!memcmp(stage->sink, fallback_output, sizeof(fallback_output)));
	assert(!memcmp(stage->x, fallback_history, sizeof(fallback_history)));
	assert(!urp_radio_destroy(portable));

	/* Interpolation/routing/detection stay in C and retain the unusual mono mix. */
	{
		int16_t generic_input[] = {2000, -3000, 4000};
		int16_t generic_output[12] = {32000, -32000, 31000, -31000, 30000, -30000,
					      29000, -29000, 28000, -28000, 27000, -27000};
		int16_t portable_output[12];
		int16_t generic_history[3];
		int16_t portable_history[3];
		urp_radio_stage generic_fallback = {0};
		urp_radio_state *generic_state = create_fir_test_radio();
		urp_radio_stage *generic_stage = generic_state->spsRxLsd;

		memcpy(portable_output, generic_output, sizeof(portable_output));
		memcpy(generic_history, initial_history, sizeof(generic_history));
		memcpy(portable_history, initial_history, sizeof(portable_history));
		generic_fallback.x = generic_history;
		configure_fir_stage(&generic_fallback, generic_input, generic_output, 3,
				    coefficients, 3, 300, 280, 10000);
		configure_fir_stage(generic_stage, generic_input, portable_output, 3, coefficients,
				    3, 300, 280, 10000);
		memcpy(generic_stage->x, portable_history, sizeof(portable_history));
		generic_fallback.interpolate = generic_stage->interpolate = 2;
		generic_fallback.decimate = generic_stage->decimate = -1;
		generic_fallback.decimator = generic_stage->decimator = -1;
		generic_fallback.numChanOut = generic_stage->numChanOut = 2;
		generic_fallback.mixOut = generic_stage->mixOut = 1;
		generic_fallback.monoOut = generic_stage->monoOut = 1;
		generic_fallback.setpt = generic_stage->setpt = 1000;
		generic_fallback.hyst = generic_stage->hyst = 20;
		generic_fallback.discfactor = generic_stage->discfactor = 2;
		generic_fallback.discounteru = generic_stage->discounteru = 1;
		generic_fallback.discounterl = generic_stage->discounterl = 1;
		assert(urp_radio_fir(&generic_fallback) == 0 && urp_radio_fir(generic_stage) == 0);
		assert(!memcmp(portable_output, generic_output, sizeof(portable_output)));
		assert(!memcmp(generic_stage->x, generic_history, sizeof(generic_history)));
		assert(generic_stage->decimator == generic_fallback.decimator &&
		       generic_stage->amax == generic_fallback.amax &&
		       generic_stage->amin == generic_fallback.amin &&
		       generic_stage->apeak == generic_fallback.apeak &&
		       generic_stage->discounteru == generic_fallback.discounteru &&
		       generic_stage->discounterl == generic_fallback.discounterl &&
		       generic_stage->compOut == generic_fallback.compOut);
		assert(!urp_radio_destroy(generic_state));
	}

	/* Option three clears only its selected route and preserves filter state. */
	portable = create_fir_test_radio();
	stage = portable->spsRxLsd;
	{
		int16_t output[6] = {1, 2, 3, 4, 5, 6};
		int16_t before_history[3];

		configure_fir_stage(stage, input, output, 3, coefficients, 3, 300, 280, 10000);
		memcpy(stage->x, initial_history, sizeof(initial_history));
		memcpy(before_history, stage->x, sizeof(before_history));
		stage->option = 3;
		stage->numChanOut = 2;
		stage->selChanOut = 1;
		stage->amax = 101;
		stage->amin = -202;
		stage->apeak = 303;
		assert(urp_radio_fir(stage) == 0 && !stage->enabled && !stage->option);
		assert(output[0] == 1 && output[1] == 0 && output[2] == 3 && output[3] == 0 &&
		       output[4] == 5 && output[5] == 0);
		assert(!memcmp(stage->x, before_history, sizeof(before_history)) &&
		       stage->amax == 101 && stage->amin == -202 && stage->apeak == 303);
	}
	assert(!urp_radio_destroy(portable));

	/* Empty and negative spans are C-only calls that still publish legacy state. */
	fallback = (urp_radio_stage){0};
	fallback.source = input;
	fallback.sink = fallback_output;
	fallback.x = fallback_history;
	fallback.coef = (void *)coefficients;
	fallback.enabled = 1;
	fallback.nx = 3;
	fallback.ncoef = 3;
	fallback.discounteru = 70000;
	fallback.discounterl = -70000;
	fallback.apeak = 99;
	fallback.nSamples = 0;
	assert(urp_radio_fir(&fallback) == 0 && fallback.apeak == 0 &&
	       fallback.discounteru == (int16_t)70000 && fallback.discounterl == (int16_t)-70000);
	fallback.apeak = 88;
	fallback.nSamples = -1;
	assert(urp_radio_fir(&fallback) == 0 && fallback.apeak == 0);
}

/** @brief Configure one live measurement stage for an envelope parity check.
 * @param stage Stage supplied by a fully allocated radio state.
 * @param input Readable signed-16 samples.
 * @param output Writable measured half-peak samples.
 * @param sample_count Number of samples in this test span.
 * @param decay_factor Legacy detector decay interval.
 * @param threshold Final peak comparator threshold.
 */
static void configure_measure_stage(urp_radio_stage *stage, int16_t *input, int16_t *output,
				    int16_t sample_count, int32_t decay_factor, int16_t threshold)
{
	stage->source = input;
	stage->sink = output;
	stage->nSamples = sample_count;
	stage->enabled = 1;
	stage->option = 0;
	stage->amax = 0;
	stage->amin = 0;
	stage->apeak = 0;
	stage->discounteru = 0;
	stage->discounterl = 0;
	stage->discfactor = decay_factor;
	stage->setpt = threshold;
	stage->compOut = 0;
}

/** @brief Verify the required portable envelope meter preserves exact state and partitioning. */
static void test_portable_measure_block(void)
{
	static int16_t exact_input[] = {100, -100, 0, 0};
	static int16_t partition_input[] = {INT16_MIN, INT16_MAX, 0, -500, 500, 0, -1, 1};
	int16_t exact_output[4] = {0};
	int16_t whole_output[8] = {0};
	int16_t split_output[8] = {0};
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	urp_radio_state *exact;
	urp_radio_state *whole;
	urp_radio_state *split;

	assert(urp_radio_core_initialize() == 0);
	exact = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(exact && exact->measureF32Input && exact->measureF32Output);
	configure_measure_stage(exact->spsMeasure, exact_input, exact_output, 4, 1, 97);
	assert(MeasureBlock(exact->spsMeasure) == 0);
	assert(!memcmp(exact_output, (int16_t[]){50, 99, 98, 97}, sizeof(exact_output)));
	assert(exact->spsMeasure->amax == 97 && exact->spsMeasure->amin == -98 &&
	       exact->spsMeasure->apeak == 97 && exact->spsMeasure->discounteru == 1 &&
	       exact->spsMeasure->discounterl == 1 && exact->spsMeasure->compOut);
	assert(!urp_radio_destroy(exact));

	/* A missing preallocated channel workspace is rejected rather than selecting
	 * a second C implementation. */
	{
		urp_radio_stage invalid = {0};

		invalid.source = exact_input;
		invalid.nSamples = 1;
		invalid.enabled = 1;
		invalid.discfactor = 1;
		invalid.setpt = 1;

		assert(MeasureBlock(&invalid) == 1 && !invalid.compOut);
	}

	whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	split = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(whole && split);
	configure_measure_stage(whole->spsMeasure, partition_input, whole_output, 8, 3, 1);
	configure_measure_stage(split->spsMeasure, partition_input, split_output, 3, 3, 1);
	assert(MeasureBlock(whole->spsMeasure) == 0);
	assert(MeasureBlock(split->spsMeasure) == 0);
	split->spsMeasure->source = partition_input + 3;
	split->spsMeasure->sink = split_output + 3;
	split->spsMeasure->nSamples = 5;
	assert(MeasureBlock(split->spsMeasure) == 0);
	assert(!memcmp(whole_output, split_output, sizeof(whole_output)));
	assert(whole->spsMeasure->amax == split->spsMeasure->amax &&
	       whole->spsMeasure->amin == split->spsMeasure->amin &&
	       whole->spsMeasure->apeak == split->spsMeasure->apeak &&
	       whole->spsMeasure->discounteru == split->spsMeasure->discounteru &&
	       whole->spsMeasure->discounterl == split->spsMeasure->discounterl &&
	       whole->spsMeasure->compOut == split->spsMeasure->compOut);
	assert(!urp_radio_destroy(split));
	assert(!urp_radio_destroy(whole));
}

/** @brief Configure the authoritative portable storage and one live delay stage. */
static void configure_delay_stage(urp_radio_state *state, int16_t *input, int16_t *output,
				  const int16_t *storage, uint32_t storage_capacity, uint32_t lead,
				  uint32_t input_index, unsigned int dirty, unsigned int enabled,
				  unsigned int outzero, int16_t sample_count)
{
	urp_radio_stage *stage = state->spsDelayLine;

	assert(stage && storage_capacity <= state->delayF32StorageCapacity);
	memset(state->delayF32Storage, 0,
	       (size_t)state->delayF32StorageCapacity * sizeof(*state->delayF32Storage));
	for (uint32_t index = 0U; storage && index < storage_capacity; ++index)
		state->delayF32Storage[index] = (float)storage[index] / 32768.0F;
	stage->source = input;
	stage->sink = output;
	stage->buffSize = storage_capacity;
	stage->buffLead = lead;
	stage->buffInIndex = input_index;
	stage->b.dirty = dirty;
	stage->enabled = (i16)enabled;
	stage->b.outzero = outzero;
	stage->nSamples = sample_count;
}

/** @brief Create one radio state with the optional squelch-delay stage present. */
static urp_radio_state *create_delay_test_radio(void)
{
	urp_radio_state template = {0};
	urp_radio_state *state;

	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "0";
	template.rxSquelchDelay = 1;
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state && state->spsDelayLine && state->delayF32Input && state->delayF32Output &&
	       state->delayF32Storage);
	return state;
}

/** @brief Assert that portable F32 circular storage preserves signed-16 codes. */
static void assert_portable_delay_storage(const urp_radio_state *state, const int16_t *expected,
					  size_t count)
{
	for (size_t index = 0U; index < count; ++index)
		assert(state->delayF32Storage[index] == (float)expected[index] / 32768.0F);
}

/** @brief Verify required portable delay behavior and its exact legacy vectors.
 *
 * This covers circular wrap order, every two-call partition, a cursor above
 * capacity, in-place aliases, dirty reset, and both clean silent no-op paths.
 */
static void test_portable_delay_line(void)
{
	static int16_t input[] = {10, -20, 30, -40, 50, -60, 70, -80};
	static const int16_t expected_output[] = {0, 0, 0, 10, -20, 30, -40, 50};
	static const int16_t expected_storage[] = {-60, 70, -80, -40, 50};
	int16_t portable_output[8] = {0};
	urp_radio_state *whole;

	assert(urp_radio_core_initialize() == 0);
	whole = create_delay_test_radio();
	configure_delay_stage(whole, input, portable_output, NULL, 5U, 3U, 0U, 0U, 1U, 0U, 8);
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(!memcmp(portable_output, expected_output, sizeof(portable_output)));
	assert(whole->spsDelayLine->buffInIndex == 3U && whole->spsDelayLine->b.dirty);
	assert_portable_delay_storage(whole, expected_storage, 5U);

	for (size_t split = 1U; split < sizeof(input) / sizeof(input[0]); ++split) {
		int16_t output[8] = {0};
		urp_radio_state *split_state = create_delay_test_radio();
		urp_radio_stage *stage = split_state->spsDelayLine;

		configure_delay_stage(split_state, input, output, NULL, 5U, 3U, 0U, 0U, 1U, 0U,
				      (int16_t)split);
		assert(DelayLine(stage) == 0);
		stage->source = input + split;
		stage->sink = output + split;
		stage->nSamples = (int16_t)(sizeof(input) / sizeof(input[0]) - split);
		assert(DelayLine(stage) == 0);
		assert(!memcmp(output, expected_output, sizeof(output)));
		assert(stage->buffInIndex == 3U && stage->b.dirty);
		assert_portable_delay_storage(split_state, expected_storage, 5U);
		assert(!urp_radio_destroy(split_state));
	}

	/* The legacy cursor is reduced modulo the storage size only when consumed. */
	memset(portable_output, 0, sizeof(portable_output));
	configure_delay_stage(whole, input, portable_output, (int16_t[]){1, 2, 3, 4, 5}, 5U, 3U, 7U,
			      1U, 1U, 0U, 3);
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(!memcmp(portable_output, (int16_t[]){5, 1, 2}, 3U * sizeof(int16_t)));
	assert(whole->spsDelayLine->buffInIndex == 5U);
	assert_portable_delay_storage(whole, (int16_t[]){1, 2, 10, -20, 30}, 5U);

	/* The conversion workspace captures all input before publishing output, so
	 * a live stage may legitimately use the same signed-16 span for both. */
	{
		int16_t aliased[8];
		urp_radio_state *alias_state = create_delay_test_radio();

		memcpy(aliased, input, sizeof(aliased));
		configure_delay_stage(alias_state, aliased, aliased, NULL, 5U, 3U, 0U, 0U, 1U, 0U,
				      8);
		assert(DelayLine(alias_state->spsDelayLine) == 0);
		assert(!memcmp(aliased, expected_output, sizeof(aliased)));
		assert_portable_delay_storage(alias_state, expected_storage, 5U);
		assert(!urp_radio_destroy(alias_state));
	}

	/* Clean disabled and forced-silent stages must leave their sink untouched. */
	memset(portable_output, 0x55, sizeof(portable_output));
	configure_delay_stage(whole, NULL, portable_output, NULL, 5U, 3U, 2U, 0U, 0U, 0U, 3);
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(portable_output[0] == (int16_t)0x5555 && portable_output[2] == (int16_t)0x5555);
	whole->spsDelayLine->enabled = 1;
	whole->spsDelayLine->b.outzero = 1;
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(portable_output[0] == (int16_t)0x5555 && portable_output[2] == (int16_t)0x5555);

	/* A dirty silent stage clears history, cursor, and exactly this output span. */
	memset(portable_output, 1, sizeof(portable_output));
	configure_delay_stage(whole, NULL, portable_output, (int16_t[]){1, 1, 1, 1, 1}, 5U, 3U, 4U,
			      1U, 0U, 0U, 3);
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(!whole->spsDelayLine->b.dirty && whole->spsDelayLine->buffInIndex == 0U);
	assert(!portable_output[0] && !portable_output[2]);
	assert_portable_delay_storage(whole, (int16_t[]){0, 0, 0, 0, 0}, 5U);

	memset(portable_output, 1, sizeof(portable_output));
	configure_delay_stage(whole, NULL, portable_output, (int16_t[]){1, 1, 1, 1, 1}, 5U, 3U, 4U,
			      1U, 1U, 1U, 3);
	assert(DelayLine(whole->spsDelayLine) == 0);
	assert(!whole->spsDelayLine->b.dirty && whole->spsDelayLine->buffInIndex == 0U);
	assert(!portable_output[0] && !portable_output[2]);
	assert_portable_delay_storage(whole, (int16_t[]){0, 0, 0, 0, 0}, 5U);
	assert(!urp_radio_destroy(whole));
}

/** @brief Configure one center-slicer stage for retained-C or portable parity checks. */
static void configure_center_slicer_stage(urp_radio_stage *stage, int16_t *input,
					  int16_t *centered_output, int16_t *limited_output,
					  int16_t sample_count, int32_t limit, int16_t setpoint,
					  int32_t decay_factor, int16_t maximum, int16_t minimum,
					  int16_t peak, int32_t upper_decay_counter,
					  int32_t lower_decay_counter, int16_t enabled)
{
	stage->source = input;
	stage->sink = centered_output;
	stage->buff = limited_output;
	stage->nSamples = sample_count;
	stage->inputGainB = limit;
	stage->setpt = setpoint;
	stage->discfactor = decay_factor;
	stage->amax = maximum;
	stage->amin = minimum;
	stage->apeak = peak;
	stage->discounteru = upper_decay_counter;
	stage->discounterl = lower_decay_counter;
	stage->enabled = enabled;
}

/** @brief Assert all persistent center-slicer fields agree exactly. */
static void assert_center_slicer_state_equal(const urp_radio_stage *left,
					     const urp_radio_stage *right)
{
	assert(left->amax == right->amax && left->amin == right->amin &&
	       left->apeak == right->apeak && left->discounteru == right->discounteru &&
	       left->discounterl == right->discounterl);
}

/** @brief Verify the portable center slicer retains fixed-point C behavior.
 *
 * The vectors cover rails, both outputs, preserved counters, every two-call
 * split boundary, diagnostic-trace fallback, disabled handling, and empty
 * signed sample spans. A stack-owned radio state forces the retained C path.
 */
static void test_portable_center_slicer(void)
{
	static int16_t input[] = {INT16_MIN, INT16_MAX, -2000,	   2000, -1,  1,
				  0,	     INT16_MIN, INT16_MAX, 123,	 -123};
	static const int16_t expected_centered[] = {-500, 500,	-500, 500,  -500, -498,
						    -498, -500, 500,  -500, -500};
	static const int16_t expected_limited[] = {-100, 100,  -100, 100,  -100, -100,
						   -100, -100, 100,  -100, -100};
	int16_t fallback_centered[sizeof(input) / sizeof(input[0])] = {0};
	int16_t fallback_limited[sizeof(input) / sizeof(input[0])] = {0};
#if URP_RADIO_DEBUG == 1
	int16_t fallback_trace[sizeof(input) / sizeof(input[0])] = {0};
#endif
	int16_t portable_centered[sizeof(input) / sizeof(input[0])] = {0};
	urp_radio_state fallback_parent = {0};
	urp_radio_stage fallback = {.parentChan = &fallback_parent};
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	urp_radio_state *whole;

#if URP_RADIO_DEBUG == 1
	fallback_parent.pRxLsdCen = fallback_trace;
#endif
	configure_center_slicer_stage(&fallback, input, fallback_centered, fallback_limited,
				      (int16_t)(sizeof(input) / sizeof(input[0])), 100, 1000, 1, 0,
				      0, 777, 123, -456, 1);
	assert(CenterSlicer(&fallback) == 0);
	assert(!memcmp(fallback_centered, expected_centered, sizeof(fallback_centered)));
	assert(!memcmp(fallback_limited, expected_limited, sizeof(fallback_limited)));
	assert(fallback.amax == 876 && fallback.amin == -122 && fallback.apeak == 499 &&
	       fallback.discounteru == 123 && fallback.discounterl == -456);

	assert(urp_radio_core_initialize() == 0);
	whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(whole && whole->spsRxLsdNrz && whole->centerSlicerF32Input &&
	       whole->centerSlicerF32CenteredOutput && whole->centerSlicerF32LimitedOutput);
	configure_center_slicer_stage(
		whole->spsRxLsdNrz, input, portable_centered, (int16_t *)whole->spsRxLsdNrz->buff,
		(int16_t)(sizeof(input) / sizeof(input[0])), 100, 1000, 1, 0, 0, 777, 123, -456, 1);
	assert(CenterSlicer(whole->spsRxLsdNrz) == 0);
	assert(!memcmp(portable_centered, fallback_centered, sizeof(portable_centered)));
	assert(!memcmp(whole->spsRxLsdNrz->buff, fallback_limited, sizeof(fallback_limited)));
	assert_center_slicer_state_equal(whole->spsRxLsdNrz, &fallback);

	/* The legacy limiter accepts a negative magnitude with its historical
	 * ordered comparisons; exercise that odd but observable fixed-point path. */
	{
		int16_t edge_input[] = {10, -10};
		int16_t fallback_edge_centered[2] = {0};
		int16_t fallback_edge_limited[2] = {0};
		int16_t portable_edge_centered[2] = {0};
		urp_radio_stage fallback_edge = {.parentChan = &fallback_parent};
		urp_radio_state *portable_edge;

		configure_center_slicer_stage(&fallback_edge, edge_input, fallback_edge_centered,
					      fallback_edge_limited, 2, -2, 1000, 0, 0, 0, 0, 41,
					      -42, 1);
		assert(CenterSlicer(&fallback_edge) == 0);
		assert(!memcmp(fallback_edge_centered, (int16_t[]){5, -10},
			       sizeof(fallback_edge_centered)));
		assert(!memcmp(fallback_edge_limited, (int16_t[]){-2, 2},
			       sizeof(fallback_edge_limited)));
		portable_edge = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(portable_edge && portable_edge->spsRxLsdNrz);
		configure_center_slicer_stage(portable_edge->spsRxLsdNrz, edge_input,
					      portable_edge_centered,
					      (int16_t *)portable_edge->spsRxLsdNrz->buff, 2, -2,
					      1000, 0, 0, 0, 0, 41, -42, 1);
		assert(CenterSlicer(portable_edge->spsRxLsdNrz) == 0);
		assert(!memcmp(portable_edge_centered, fallback_edge_centered,
			       sizeof(portable_edge_centered)));
		assert(!memcmp(portable_edge->spsRxLsdNrz->buff, fallback_edge_limited,
			       sizeof(fallback_edge_limited)));
		assert_center_slicer_state_equal(portable_edge->spsRxLsdNrz, &fallback_edge);
		assert(!urp_radio_destroy(portable_edge));
	}

	for (size_t split = 1U; split < sizeof(input) / sizeof(input[0]); ++split) {
		int16_t centered[sizeof(input) / sizeof(input[0])] = {0};
		urp_radio_state *split_state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		urp_radio_stage *stage;
		int16_t *limited;

		assert(split_state && split_state->spsRxLsdNrz);
		stage = split_state->spsRxLsdNrz;
		configure_center_slicer_stage(stage, input, centered, (int16_t *)stage->buff,
					      (int16_t)split, 100, 1000, 1, 0, 0, 777, 123, -456,
					      1);
		limited = stage->buff;
		assert(CenterSlicer(stage) == 0);
		stage->source = input + split;
		stage->sink = centered + split;
		stage->buff = limited + split;
		stage->nSamples = (int16_t)(sizeof(input) / sizeof(input[0]) - split);
		assert(CenterSlicer(stage) == 0);
		assert(!memcmp(centered, fallback_centered, sizeof(centered)));
		assert(!memcmp(limited, fallback_limited, sizeof(fallback_limited)));
		assert_center_slicer_state_equal(stage, &fallback);
		assert(!urp_radio_destroy(split_state));
	}

	/* Active diagnostic trace deliberately stays on the retained C path. */
	{
		int16_t traced_centered[sizeof(input) / sizeof(input[0])] = {0};
		urp_radio_state traced_template = template;
		urp_radio_state *traced;

		traced_template.tracetype = 1;
		traced = urp_radio_create(&traced_template, SAMPLES_PER_BLOCK);
		assert(traced && traced->spsRxLsdNrz);
		configure_center_slicer_stage(traced->spsRxLsdNrz, input, traced_centered,
					      (int16_t *)traced->spsRxLsdNrz->buff,
					      (int16_t)(sizeof(input) / sizeof(input[0])), 100,
					      1000, 1, 0, 0, 777, 123, -456, 1);
		assert(CenterSlicer(traced->spsRxLsdNrz) == 0);
		assert(!memcmp(traced_centered, fallback_centered, sizeof(traced_centered)));
		assert(!memcmp(traced->spsRxLsdNrz->buff, fallback_limited,
			       sizeof(fallback_limited)));
		assert_center_slicer_state_equal(traced->spsRxLsdNrz, &fallback);
		assert(!urp_radio_destroy(traced));
	}

	/* Disabled stages do not dereference or change any state. */
	{
		urp_radio_stage disabled = {
			.amax = 1, .amin = -2, .apeak = 3, .discounteru = 4, .discounterl = -5};
		urp_radio_stage expected = disabled;

		assert(CenterSlicer(&disabled) == 1);
		assert_center_slicer_state_equal(&disabled, &expected);
	}

	/* Empty and negative signed spans preserve historical no-op state/output. */
	{
		int16_t sentinel_centered[2] = {(int16_t)0x5555, (int16_t)0x5555};
		int16_t sentinel_limited[2] = {(int16_t)0x5555, (int16_t)0x5555};
		urp_radio_stage empty = {.parentChan = &fallback_parent};
		urp_radio_stage expected;

		configure_center_slicer_stage(&empty, input, sentinel_centered, sentinel_limited, 0,
					      100, 1000, 1, 12, -34, 56, 78, -90, 1);
		expected = empty;
		assert(CenterSlicer(&empty) == 0);
		assert_center_slicer_state_equal(&empty, &expected);
		assert(sentinel_centered[0] == (int16_t)0x5555 &&
		       sentinel_limited[1] == (int16_t)0x5555);
		empty.nSamples = -1;
		assert(CenterSlicer(&empty) == 0);
		assert_center_slicer_state_equal(&empty, &expected);
		assert(sentinel_centered[0] == (int16_t)0x5555 &&
		       sentinel_limited[1] == (int16_t)0x5555);
	}

	assert(!urp_radio_destroy(whole));
}

/** @brief Configure one receiver-deemphasis stage for retained-C or portable checks. */
static void configure_deemphasis_stage(urp_radio_stage *stage, int16_t *input, int16_t *output,
				       int16_t sample_count, int16_t coefficients[2],
				       int32_t history[2], int32_t output_gain, int16_t enabled)
{
	stage->source = input;
	stage->sink = output;
	stage->nSamples = sample_count;
	stage->coef = coefficients;
	stage->x = history;
	stage->outputGain = output_gain;
	stage->enabled = enabled;
}

/** @brief Verify required receiver deemphasis retains exact fixed-point behavior.
 *
 * The vectors cover production coefficients, signed division, each wrapping
 * multiplication, signed-16 narrowing, every two-call partition, CPU-saver
 * freezing, empty spans, and the untouched second state word.
 */
static void test_portable_deemphasis_integrator(void)
{
	static int16_t input[] = {1000, 1000, -1000, -1000, 0, 2000};
	static const int16_t expected[] = {839, 1502, 347, -565, -445, 1327};
	int16_t coefficients[2] = {6878, 25889};
	int16_t portable_output[sizeof(input) / sizeof(input[0])] = {0};
	urp_radio_state template = {.pRxCodeSrc = "0",
				    .pTxCodeSrc = "0",
				    .pTxCodeDefault = "0",
				    .rxDemod = RX_AUDIO_FLAT};
	urp_radio_state *whole;

	assert(urp_radio_core_initialize() == 0);
	whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(whole && whole->spsRxDeEmp && whole->deemphasisIntegratorF32Input &&
	       whole->deemphasisIntegratorF32Output);
	((int32_t *)whole->spsRxDeEmp->x)[0] = 0;
	((int32_t *)whole->spsRxDeEmp->x)[1] = 0x13579bdf;
	configure_deemphasis_stage(whole->spsRxDeEmp, input, portable_output,
				   (int16_t)(sizeof(input) / sizeof(input[0])), coefficients,
				   (int32_t *)whole->spsRxDeEmp->x, M_Q8, 1);
	assert(gp_inte_00(whole->spsRxDeEmp) == 0);
	assert(!memcmp(portable_output, expected, sizeof(portable_output)));
	assert(((int32_t *)whole->spsRxDeEmp->x)[0] == 1581);
	assert(((int32_t *)whole->spsRxDeEmp->x)[1] == 0x13579bdf);

	for (size_t split = 1U; split < sizeof(input) / sizeof(input[0]); ++split) {
		int16_t split_output[sizeof(input) / sizeof(input[0])] = {0};
		urp_radio_state *split_state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		urp_radio_stage *stage;
		int32_t *history;

		assert(split_state && split_state->spsRxDeEmp);
		stage = split_state->spsRxDeEmp;
		history = stage->x;
		history[0] = 0;
		history[1] = 0x13579bdf;
		configure_deemphasis_stage(stage, input, split_output, (int16_t)split, coefficients,
					   history, M_Q8, 1);
		assert(gp_inte_00(stage) == 0);
		stage->source = input + split;
		stage->sink = split_output + split;
		stage->nSamples = (int16_t)(sizeof(input) / sizeof(input[0]) - split);
		assert(gp_inte_00(stage) == 0);
		assert(!memcmp(split_output, expected, sizeof(split_output)));
		assert(history[0] == 1581 && history[1] == 0x13579bdf);
		assert(!urp_radio_destroy(split_state));
	}

	/* Signed division, every wrapping multiplication, and i16 narrowing are all observable. */
	{
		struct deemphasis_case {
			int16_t input[4];
			int16_t coefficient[2];
			int16_t expected_output[4];
			int16_t samples;
			int32_t initial;
			int32_t output_gain;
			int32_t expected_state;
		};
		static const struct deemphasis_case cases[] = {
			{{-1, -2, 3, -3},
			 {8192, 16384},
			 {-250, -126, -61, -32},
			 4,
			 -1000,
			 128,
			 -64},
			{{0, 0, 0, 0}, {1, 32767}, {-4, 0, 0, 0}, 1, 1073741824, 256, -32768},
			{{32767, 0, 0, 0}, {32767, 1}, {-19, 0, 0, 0}, 1, INT32_MAX, 256, 98302},
			{{32767, 0, 0, 0}, {8192, 1}, {-639, 0, 0, 0}, 1, INT32_MAX, 32767, 98302},
			{{20000, -20000, 0, 0},
			 {8192, 0},
			 {-25536, 25536, 0, 0},
			 2,
			 0,
			 512,
			 -20000},
		};

		for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
			const struct deemphasis_case *vector = &cases[index];
			int16_t portable_vector_output[4] = {0};
			int16_t vector_coefficients[2];
			urp_radio_state *portable_vector;
			int32_t *portable_history;

			memcpy(vector_coefficients, vector->coefficient,
			       sizeof(vector_coefficients));
			portable_vector = urp_radio_create(&template, SAMPLES_PER_BLOCK);
			assert(portable_vector && portable_vector->spsRxDeEmp);
			portable_history = portable_vector->spsRxDeEmp->x;
			portable_history[0] = vector->initial;
			portable_history[1] = 0x13579bdf;
			configure_deemphasis_stage(portable_vector->spsRxDeEmp,
						   (int16_t *)vector->input, portable_vector_output,
						   vector->samples, vector_coefficients,
						   portable_history, vector->output_gain, 1);
			assert(gp_inte_00(portable_vector->spsRxDeEmp) == 0);
			assert(!memcmp(portable_vector_output, vector->expected_output,
				       (size_t)vector->samples * sizeof(int16_t)));
			assert(portable_history[0] == vector->expected_state &&
			       portable_history[1] == 0x13579bdf);
			assert(!urp_radio_destroy(portable_vector));
		}
	}

	/* CPU saver freezes this stage before it reads input, output, or history. */
	{
		int16_t sentinel[2] = {(int16_t)0x5555, (int16_t)0x5555};
		int32_t *history = whole->spsRxDeEmp->x;

		history[0] = 4321;
		history[1] = 0x13579bdf;
		configure_deemphasis_stage(whole->spsRxDeEmp, NULL, sentinel, 2, coefficients,
					   history, M_Q8, 0);
		assert(gp_inte_00(whole->spsRxDeEmp) == 1);
		assert(history[0] == 4321 && history[1] == 0x13579bdf);
		assert(sentinel[0] == (int16_t)0x5555 && sentinel[1] == (int16_t)0x5555);
	}

	/* Zero stays portable; a negative signed span uses the unchanged C no-op. */
	{
		int16_t sentinel[2] = {(int16_t)0x5555, (int16_t)0x5555};
		int32_t *history = whole->spsRxDeEmp->x;

		history[0] = -2468;
		history[1] = 0x13579bdf;
		configure_deemphasis_stage(whole->spsRxDeEmp, NULL, sentinel, 0, coefficients,
					   history, M_Q8, 1);
		assert(gp_inte_00(whole->spsRxDeEmp) == 0);
		assert(history[0] == -2468 && history[1] == 0x13579bdf);
		assert(sentinel[0] == (int16_t)0x5555 && sentinel[1] == (int16_t)0x5555);
		whole->spsRxDeEmp->nSamples = -1;
		assert(gp_inte_00(whole->spsRxDeEmp) == 0);
		assert(history[0] == -2468 && history[1] == 0x13579bdf);
		assert(sentinel[0] == (int16_t)0x5555 && sentinel[1] == (int16_t)0x5555);
	}

	assert(!urp_radio_destroy(whole));
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
	assert(state->b.ctcssTxEnable);
	assert(state->rxNoiseSquelchEnable);
	assert(state->rxDeEmpEnable);
	assert(state->b.ctcssRxEnable && state->b.ctcssTxEnable);
	assert(!urp_radio_process_native_timed(
		state, input, output, NULL,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1));
	assert(!urp_radio_destroy(state));
}

/** @brief Verify create variants. */
static void test_create_variants(void)
{
	static const char *const receive_codes[] = {"0",     "67.0", "250.3",	   "67.0,100.0",
						    "123.4", "67.0", "100.0,67.0", "67.0"};
	static const char *const transmit_codes[] = {"0",     "0",	    "250.3",	  "67.0",
						     "123.4", "67.0,100.0", "100.0,67.0", "123.4"};
	static const char *const default_codes[] = {"0",     "67.0", "250.3", "123.4",
						    "100.0", "0",    "100.0", ""};
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
		state->txrxblankingtimer = MS_PER_FRAME;
		assert(!urp_radio_process_native_timed(
			state, input, output, transmit,
			SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1));
		assert(!urp_radio_destroy(state));
	}

	/* A missing default is equivalent to no transmit CTCSS default rather than
	 * an invalid CTCSS token. */
	{
		urp_radio_state template = {0};
		urp_radio_state *state;

		template.pRxCodeSrc = "0";
		template.pTxCodeSrc = "0";
		template.pTxCodeDefault = NULL;
		template.tracelevel = 100;
		state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(state && !state->b.ctcssTxEnable);
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
	return urp_radio_process_native_timed(
		state, input, output, transmit,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1);
}

/** @brief Minimal portable-DCS callback state for signaling-order regression tests. */
struct dcs_radio_callback_state {
	/** Qualification value supplied to the radio state machine. */
	int valid;
	/** Number of callback spans observed. */
	unsigned int calls;
};

/** @brief Supply one portable DCS result at the legacy post-blanking hook. */
static int radio_dcs_receive_callback(void *context, const int16_t *samples, size_t count,
				      size_t stride, unsigned int sample_rate, int *valid)
{
	struct dcs_radio_callback_state *state = context;

	assert(state && samples && valid);
	assert(count == SAMPLES_PER_BLOCK * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK));
	assert(stride == 2U && sample_rate == SAMPLE_RATE_INPUT);
	++state->calls;
	*valid = state->valid;
	return 0;
}

/** @brief Verify transmitter CTCSS startup leaves receive-only mappings silent. */
static void test_ctcss_transmit_startup_edges(void)
{
	urp_radio_state template = {0};
	urp_radio_state *state;
	const int index = urp_ctcss_frequency_index(100.0F);

	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state && index > CTCSS_NULL);

	/* Bypass the live decoder so this test holds the modeled receive decision
	 * steady while asserting the transmitter's mapping behavior. */
	state->b.ctcssRxEnable = 0;
	/* A decoded receive-only tone must not substitute the configured transmitter
	 * default.  It leaves the startup tone unset rather than emitting a stale
	 * value from an earlier transmission. */
	state->smode = SMODE_CTCSS;
	state->rxCtcss->decode = index;
	state->rxCtcssMap[index] = CTCSS_RXONLY;
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txCtcssFreq10 == 0 &&
	       !state->txCtcssEnabled);

	/* An explicit transmit CTCSS inhibit likewise starts PTT without generating
	 * a tone.  This is distinct from a radio configured for carrier signaling. */
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->txPttIn = 1;
	state->b.txCtcssInhibit = 1;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_ACTIVE && !state->txCtcssEnabled);
	assert(!urp_radio_destroy(state));
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
	assert(urp_radio_process_native_timed(
		       NULL, NULL, NULL, NULL,
		       SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1) == 1);

	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	state->b.ctcssRxEnable = 0;
	state->txrxblankingtimer = 2 * MS_PER_FRAME;
	assert(process_once(state) == 0 && state->txrxblankingtimer == MS_PER_FRAME);
	int16_t direct_input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	assert(urp_radio_process_native_timed(
		       state, direct_input, NULL, NULL,
		       SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1) == 0);

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
	/* Completion must re-arm the configured blanking interval and discard the
	 * prior fractional callback remainder, regardless of its pre-drain state. */
	state->txrxblankingtime = 37;
	state->txrxblankingtimer = 9;
	state->txrxBlankingSampleRemainder = 5U;
	state->b.txCtcssReady = 0;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_FINISHING);
	assert(process_once(state) == 0);
	assert(process_once(state) == 0);
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_IDLE && !state->txPttOut &&
	       state->txrxblankingtimer == 37 && state->txrxBlankingSampleRemainder == 0U &&
	       state->b.txCtcssReady);

	state->txPttIn = 1;
	assert(process_once(state) == 0);
	state->txTocType = TOC_NOTONE;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC);
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	/* A no-tone tail clears emitted CTCSS, but a rekey must restore the
	 * configured signaling mode rather than remain in the tail state. */
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txCtcssEnabled &&
	       state->txCtcssState == 1);
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	{
		unsigned int tail_blocks = 0U;

		/* Tone removal holds PTT for the configured CTCSS tail duration even
		 * though the generated CTCSS waveform is already disabled. */
		while (state->txState == CHAN_TXSTATE_TOC) {
			assert(state->txPttOut && !state->txCtcssEnabled);
			assert(process_once(state) == 0);
			assert(++tail_blocks <= 12U);
		}
	}
	assert(state->txState == CHAN_TXSTATE_IDLE && !state->txPttOut);

	state->txState = CHAN_TXSTATE_ACTIVE;
	state->smode = SMODE_CTCSS;
	state->txTocType = TOC_PHASE;
	/* The receiver's selected mode does not imply a transmit tone. Model the
	 * keyed transmit CTCSS state that precedes every real CTCSS tail. */
	state->txCtcssEnabled = 1;
	state->txCtcssState = 1;
	state->txPttOut = 1;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->txCtcssState == 2 && state->txPttOut &&
	       state->txCtcssPhaseShift == CTCSS_TURN_OFF_SHIFT && state->txCtcssTailToneHz == 0.0);
	{
		unsigned int tail_blocks = 0U;

		while (state->txState == CHAN_TXSTATE_TOC) {
			/* The final TOC frame observes the generator's completed state
			 * before advancing to FINISHING, but PTT remains asserted. */
			assert(state->txPttOut);
			if (state->txCtcssState == 2)
				assert(state->txCtcssTailToneHz == 0.0);
			assert(process_once(state) == 0);
			assert(++tail_blocks <= 12U);
		}
	}
	assert(state->txState == CHAN_TXSTATE_FINISHING);

	state->txState = CHAN_TXSTATE_ACTIVE;
	state->smode = SMODE_CTCSS;
	state->txTocType = TOC_PHASE;
	state->txCtcssTocShift = 135.0;
	state->txCtcssEnabled = 1;
	state->txCtcssState = 1;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->txCtcssState == 2 && state->txPttOut &&
	       state->txCtcssPhaseShift == 135.0);
	assert(state->txCtcssTocShift == 135.0);
	assert(state->txCtcssTocTime == CTCSS_TURN_OFF_TIME);
	assert(state->txCtcssTocToneHz == 0.0);

	state->txState = CHAN_TXSTATE_ACTIVE;
	state->smode = SMODE_CTCSS;
	state->txTocType = 3;
	state->txCtcssTocTime = 250;
	state->txCtcssTocToneHz = 55.0;
	state->txCtcssEnabled = 1;
	state->txCtcssState = 1;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->txCtcssState == 2 && state->txPttOut &&
	       state->txCtcssPhaseShift == 0.0 && state->txCtcssTailToneHz == 55.0);
	assert(state->txCtcssTocShift == 0.0);
	assert(state->txCtcssTocTime == 250);
	assert(state->txCtcssTocToneHz == 55.0);
	{
		unsigned int tail_blocks = 0U;

		while (state->txState == CHAN_TXSTATE_TOC) {
			assert(state->txPttOut);
			if (state->txCtcssState == 2)
				assert(state->txCtcssTailToneHz == 55.0);
			assert(process_once(state) == 0);
			assert(++tail_blocks <= 16U);
		}
	}
	assert(state->txState == CHAN_TXSTATE_FINISHING && state->txBufferClear == 8);
	for (unsigned int post_tail_frame = 0U; post_tail_frame < 8U; ++post_tail_frame) {
		assert(state->txPttOut);
		assert(process_once(state) == 0);
	}
	assert(state->txState == CHAN_TXSTATE_IDLE && !state->txPttOut);

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
	assert(state->txCtcssTailToneHz == 55.0);
	state->txCtcssTurnoffTimer = 2 * MS_PER_FRAME;
	assert(process_once(state) == 0 && state->txCtcssOption == 0);
	assert(process_once(state) == 0 && state->txCtcssOption == 3);
	assert(process_once(state) == 0 && state->txCtcssState == 0);
	assert(state->txCtcssTailToneHz == 0.0);

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

/** Verify transmitter signaling cannot run ahead of an unavailable DAC frame. */
static void test_transmit_timeline_admission(void)
{
	urp_radio_state template = {
		.pRxCodeSrc = "100.0", .pTxCodeSrc = "100.0", .pTxCodeDefault = "100.0"};
	int16_t input[SAMPLES_PER_BLOCK * 6 * 2] = {0};
	int16_t output[SAMPLES_PER_BLOCK] = {0};
	int16_t transmit[SAMPLES_PER_BLOCK * 6 * 2];
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);

	assert(state);
	state->b.ctcssRxEnable = 0;
	memset(transmit, 0x5a, sizeof(transmit));
	state->txPttIn = 1;
	assert(!urp_radio_process_native_timed(
		state, input, output, transmit,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 0));
	assert(state->activeSamplesRx == SAMPLES_PER_BLOCK && state->txState == CHAN_TXSTATE_IDLE &&
	       !state->txPttOut);
	for (size_t sample = 0; sample < sizeof(transmit) / sizeof(transmit[0]); ++sample)
		assert(!transmit[sample]);

	/* A caller without a DAC frame can omit its output buffer. The receive
	 * timeline still advances, while transmitter signaling remains frozen. */
	assert(!urp_radio_process_native_timed(
		state, input, output, NULL,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 0));
	assert(state->activeSamplesRx == SAMPLES_PER_BLOCK && state->txState == CHAN_TXSTATE_IDLE &&
	       !state->txPttOut);

	assert(!urp_radio_process_native_timed(
		state, input, output, transmit,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1));
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txPttOut);
	state->txPttIn = 0;
	assert(!urp_radio_process_native_timed(
		state, input, output, transmit,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 0));
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txPttOut);
	assert(!urp_radio_process_native_timed(
		state, input, output, transmit,
		SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK, 1));
	assert(state->txState == CHAN_TXSTATE_FINISHING || state->txState == CHAN_TXSTATE_TOC);
	assert(!urp_radio_destroy(state));
}

/** @brief Verify native-frame partitioning advances equivalent signaling time. */
static void test_native_frame_partitioning(void)
{
	enum { native_frames = SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK };
	urp_radio_state template = {.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
	int16_t input[native_frames * 2U] = {0};
	int16_t output_whole[SAMPLES_PER_BLOCK] = {0};
	int16_t output_split[SAMPLES_PER_BLOCK] = {0};
	int16_t transmit_whole[native_frames * 2U] = {0};
	int16_t transmit_split[native_frames * 2U] = {0};

	template.dcsTurnoffDuration = 150;
	memcpy(template.dcsTxCode, "023N", sizeof(template.dcsTxCode));
	urp_radio_state *whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	urp_radio_state *split = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(whole && split);
	whole->b.ctcssRxEnable = split->b.ctcssRxEnable = 0;
	whole->dcsTurnoffEnabled = split->dcsTurnoffEnabled = 1;
	whole->txPttIn = split->txPttIn = 1;
	assert(!urp_radio_process_native_timed(whole, input, output_whole, transmit_whole,
					       native_frames, 1));
	assert(!urp_radio_process_native_timed(split, input, output_split, transmit_split,
					       native_frames / 2U, 1));
	assert(!urp_radio_process_native_timed(
		split, input + native_frames, output_split + SAMPLES_PER_BLOCK / 2U,
		transmit_split + native_frames, native_frames / 2U, 1));
	assert(whole->txState == split->txState && whole->txPttOut == split->txPttOut);
	assert(whole->txsettletimer == split->txsettletimer);
	assert(whole->txFinishTimer == split->txFinishTimer);
	assert(whole->txHangTime == split->txHangTime);
	assert(whole->txCtcssTurnoffTimer == split->txCtcssTurnoffTimer);
	assert(!memcmp(output_whole, output_split, sizeof(output_whole)));
	assert(!memcmp(transmit_whole, transmit_split, sizeof(transmit_whole)));
	whole->txPttIn = split->txPttIn = 0;
	assert(!urp_radio_process_native_timed(whole, input, output_whole, transmit_whole,
					       native_frames, 1));
	assert(!urp_radio_process_native_timed(split, input, output_split, transmit_split,
					       native_frames / 2U, 1));
	assert(!urp_radio_process_native_timed(
		split, input + native_frames, output_split + SAMPLES_PER_BLOCK / 2U,
		transmit_split + native_frames, native_frames / 2U, 1));
	assert(whole->txState == CHAN_TXSTATE_TOC && whole->txState == split->txState);
	assert(whole->dcsTurnoffTimer == 130 && whole->dcsTurnoffTimer == split->dcsTurnoffTimer);
	assert(urp_radio_process_native_timed(whole, input, output_whole, transmit_whole, 0U, 1));
	assert(whole->dcsTurnoffTimer == 130);
	assert(urp_radio_process_native_timed(whole, input, output_whole, transmit_whole,
					      native_frames + 6U, 1));
	assert(whole->dcsTurnoffTimer == 130);
	assert(!urp_radio_destroy(whole));
	assert(!urp_radio_destroy(split));
}

/** State collected from native and base-rate receive decoder hooks. */
struct variable_frame_receive_observer {
	/** Number of native DCS samples consumed. */
	size_t dcs_samples;
	/** Number of DCS callback spans. */
	unsigned int dcs_calls;
	/** Number of emitted 8 kHz CTCSS samples consumed. */
	size_t ctcss_samples;
	/** Number of nonempty CTCSS callback spans. */
	unsigned int ctcss_calls;
};

/** @brief Record one native DCS callback span without qualifying a code. */
static int variable_frame_dcs_callback(void *context, const int16_t *samples, size_t count,
				       size_t stride, unsigned int sample_rate, int *valid)
{
	struct variable_frame_receive_observer *observer = context;

	assert(observer && samples && valid && stride == 2U && sample_rate == SAMPLE_RATE_INPUT);
	observer->dcs_samples += count;
	++observer->dcs_calls;
	*valid = 0;
	return 0;
}

/** @brief Record one emitted CTCSS span without selecting a tone. */
static int variable_frame_ctcss_callback(void *context, const int16_t *samples, size_t sample_count,
					 uint64_t tone_mask, int relax, int carrier_detect,
					 int *decoded)
{
	struct variable_frame_receive_observer *observer = context;

	(void)tone_mask;
	(void)relax;
	(void)carrier_detect;
	assert(observer && samples && decoded && sample_count > 0U);
	observer->ctcss_samples += sample_count;
	++observer->ctcss_calls;
	*decoded = CTCSS_NULL;
	return 0;
}

/** @brief Create a receiver state with native DCS and base-rate CTCSS hooks. */
static urp_radio_state *
create_variable_frame_radio(struct variable_frame_receive_observer *observer)
{
	urp_radio_state template = {
		.pRxCodeSrc = "100.0",
		.pTxCodeSrc = "0",
		.pTxCodeDefault = "0",
		.rxCdType = CD_XPMR_NOISE,
		.rxDemod = RX_AUDIO_FLAT,
		.txsettletime = 40,
	};
	urp_radio_state *state;

	memcpy(template.dcsRxCode, "023N", sizeof(template.dcsRxCode));
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state && state->rxBaseCapacity == SAMPLES_PER_BLOCK + 1U && state->rxCtcss);
	state->rxCpuSaver = 0;
	state->b.ctcssRxEnable = 1;
	state->rxCtcss->enabled = 1;
	urp_dcs_set_receive_callback(&state->dcs, variable_frame_dcs_callback, observer);
	urp_ctcss_set_receive_callback(state->rxCtcss, variable_frame_ctcss_callback, observer);
	return state;
}

/** @brief Process a contiguous native sequence using caller-selected partitions.
 * @return Total emitted 8 kHz samples.
 */
static size_t process_variable_frame_partitions(urp_radio_state *state, int16_t *input,
						int16_t *output, int16_t *transmit,
						const size_t *partitions, size_t partition_count)
{
	size_t native_offset = 0U;
	size_t output_offset = 0U;
	size_t part;

	for (part = 0U; part < partition_count; ++part) {
		assert(partitions[part] > 0U);
		assert(!urp_radio_process_native_timed(
			state, input + native_offset * 2U, output ? output + output_offset : NULL,
			transmit ? transmit + native_offset * 2U : NULL, partitions[part], 1));
		native_offset += partitions[part];
		output_offset += (size_t)state->activeSamplesRx;
	}
	return output_offset;
}

/** @brief Verify arbitrary native callbacks preserve decimation, DSP, and timing. */
static void test_native_variable_frame_receive_partitioning(void)
{
	enum { native_frames = SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK };
	const size_t complete[] = {native_frames};
	const size_t irregular[] = {1U, 5U, 6U, 7U, 47U, 113U, 781U};
	const size_t split_5_1[] = {5U, 1U};
	const size_t split_1_5[] = {1U, 5U};
	const size_t split_7_5[] = {7U, 5U};
	int16_t input_whole[native_frames * 2U];
	int16_t input_split[native_frames * 2U];
	int16_t output_whole[SAMPLES_PER_BLOCK + 1U] = {0};
	int16_t output_split[SAMPLES_PER_BLOCK + 1U] = {0};
	int16_t transmit_whole[native_frames * 2U] = {0};
	int16_t transmit_split[native_frames * 2U] = {0};
	struct variable_frame_receive_observer whole_observer = {0};
	struct variable_frame_receive_observer split_observer = {0};
	urp_radio_state *whole;
	urp_radio_state *split;
	size_t emitted_whole;
	size_t emitted_split;
	size_t sample;

	for (sample = 0U; sample < native_frames; ++sample) {
		input_whole[sample * 2U] = (int16_t)(((sample * 73U) % 30001U) - 15000);
		input_whole[sample * 2U + 1U] = (int16_t)(15000 - ((sample * 31U) % 30001U));
	}
	memcpy(input_split, input_whole, sizeof(input_split));
	whole = create_variable_frame_radio(&whole_observer);
	split = create_variable_frame_radio(&split_observer);
	whole->txPttIn = split->txPttIn = 1;
	emitted_whole = process_variable_frame_partitions(whole, input_whole, output_whole,
							  transmit_whole, complete, 1U);
	emitted_split = process_variable_frame_partitions(split, input_split, output_split,
							  transmit_split, irregular,
							  sizeof(irregular) / sizeof(irregular[0]));
	assert(emitted_whole == SAMPLES_PER_BLOCK && emitted_split == emitted_whole);
	assert(!memcmp(output_whole, output_split, emitted_whole * sizeof(*output_whole)));
	assert(!memcmp(transmit_whole, transmit_split, sizeof(transmit_whole)));
	assert(whole->spsRx->decimator == split->spsRx->decimator &&
	       whole->spsRx->rssiSamples == 0U && split->spsRx->rssiSamples == 0U &&
	       whole->rxRssi == split->rxRssi && whole->rxCarrierDetect == split->rxCarrierDetect &&
	       whole->txState == split->txState && whole->txPttOut == split->txPttOut &&
	       whole->txsettletimer == split->txsettletimer);
	assert(whole_observer.dcs_samples == native_frames &&
	       split_observer.dcs_samples == native_frames &&
	       whole_observer.ctcss_samples == emitted_whole &&
	       split_observer.ctcss_samples == emitted_split);
	assert(split_observer.dcs_calls == sizeof(irregular) / sizeof(irregular[0]) &&
	       split_observer.ctcss_calls < split_observer.dcs_calls);
	assert(!urp_radio_destroy(split));
	assert(!urp_radio_destroy(whole));

	/* Each requested short partition consumes all native samples but presents
	 * baseband DSP only the samples actually emitted at the sixth-sample edge. */
	for (size_t vector = 0U; vector < 3U; ++vector) {
		const size_t *partitions = vector == 0U	  ? split_5_1
					   : vector == 1U ? split_1_5
							  : split_7_5;
		const size_t partition_count = 2U;
		const size_t frames = vector < 2U ? 6U : 12U;
		int16_t short_input[12U * 2U] = {0};
		int16_t short_output[3] = {111, 222, 333};
		int16_t short_whole_output[3] = {0};
		struct variable_frame_receive_observer short_observer = {0};
		struct variable_frame_receive_observer short_whole_observer = {0};
		urp_radio_state *short_split = create_variable_frame_radio(&short_observer);
		urp_radio_state *short_whole = create_variable_frame_radio(&short_whole_observer);
		size_t short_emitted;
		size_t short_whole_emitted;

		for (sample = 0U; sample < frames; ++sample)
			short_input[sample * 2U] = (int16_t)(sample * 1000U - 4000U);
		short_whole_emitted = process_variable_frame_partitions(
			short_whole, short_input, short_whole_output, NULL, &frames, 1U);
		short_emitted = process_variable_frame_partitions(
			short_split, short_input, short_output, NULL, partitions, partition_count);
		assert(short_whole_emitted == short_emitted &&
		       !memcmp(short_whole_output, short_output,
			       short_emitted * sizeof(*short_output)));
		assert(short_observer.dcs_samples == frames &&
		       short_observer.ctcss_samples == short_emitted);
		assert(short_split->spsRx->decimator == 6 &&
		       short_split->spsRx->rssiSamples == frames);
		assert(!urp_radio_destroy(short_whole));
		assert(!urp_radio_destroy(short_split));
	}

	/* A callback may contain fewer than six native samples. It must still
	 * advance native DCS/MICOR state, preserve the phase, and leave the caller's
	 * baseband output untouched. */
	{
		int16_t short_input[5U * 2U] = {0};
		int16_t untouched = 4321;
		struct variable_frame_receive_observer observer = {0};
		urp_radio_state *state = create_variable_frame_radio(&observer);

		assert(!urp_radio_process_native_timed(state, short_input, &untouched, NULL, 5U,
						       1));
		assert(state->activeSamplesRx == 0 && state->spsRx->decimator == 1 &&
		       state->spsRx->rssiSamples == 5U && untouched == 4321 &&
		       observer.dcs_samples == 5U && observer.ctcss_samples == 0U);
		assert(!urp_radio_destroy(state));
	}

	/* The conservative 161-sample allocation accepts a maximum native span
	 * after a carried phase without changing the fixed 960-frame wrapper. */
	{
		int16_t phase_input[(native_frames + 1U) * 2U] = {0};
		int16_t phase_output[SAMPLES_PER_BLOCK + 1U] = {0};
		struct variable_frame_receive_observer observer = {0};
		urp_radio_state *state = create_variable_frame_radio(&observer);

		assert(!urp_radio_process_native_timed(state, phase_input, phase_output, NULL, 1U,
						       1));
		assert(!urp_radio_process_native_timed(state, phase_input + 2U, phase_output, NULL,
						       native_frames, 1));
		assert(state->activeSamplesRx <= (i16)state->rxBaseCapacity &&
		       observer.dcs_samples == native_frames + 1U);
		assert(!urp_radio_destroy(state));
	}

	/* The adapter-declared native maximum is a hard bound. Rejecting one more
	 * frame must not consume input, alter the decimator phase, or publish output. */
	{
		int16_t oversized_input[(native_frames + 1U) * 2U] = {0};
		int16_t untouched = 2468;
		struct variable_frame_receive_observer observer = {0};
		urp_radio_state *state = create_variable_frame_radio(&observer);

		assert(urp_radio_process_native_timed(state, oversized_input, &untouched, NULL,
						      native_frames + 1U, 1));
		assert(state->spsRx->decimator == 6 && state->spsRx->rssiSamples == 0U &&
		       untouched == 2468 && observer.dcs_samples == 0U &&
		       observer.ctcss_samples == 0U);
		assert(!urp_radio_destroy(state));
	}
}

/** @brief Verify RX blanking protects the same PCM duration across callback splits. */
static void test_rx_blanking_partitioning(void)
{
	enum { native_frames = SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK };
	urp_radio_state template = {.pRxCodeSrc = "0",
				    .pTxCodeSrc = "0",
				    .pTxCodeDefault = "0",
				    .rxCdType = CD_XPMR_NOISE};
	int16_t input_whole[native_frames * 2U];
	int16_t input_split[native_frames * 2U];
	int16_t output_whole[SAMPLES_PER_BLOCK] = {0};
	int16_t output_split[SAMPLES_PER_BLOCK] = {0};
	urp_radio_state *whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	urp_radio_state *split = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	size_t sample;

	assert(whole && split);
	whole->b.ctcssRxEnable = split->b.ctcssRxEnable = 0;
	for (sample = 0U; sample < native_frames; ++sample) {
		input_whole[sample * 2U] = 1000;
		input_whole[sample * 2U + 1U] = -1000;
	}
	memcpy(input_split, input_whole, sizeof(input_split));
	whole->txrxblankingtimer = split->txrxblankingtimer = MS_PER_FRAME / 2;
	assert(!urp_radio_process_native_timed(whole, input_whole, output_whole, NULL,
					       native_frames, 0));
	assert(!urp_radio_process_native_timed(split, input_split, output_split, NULL,
					       native_frames / 2U, 0));
	assert(!urp_radio_process_native_timed(split, input_split + native_frames,
					       output_split + SAMPLES_PER_BLOCK / 2U, NULL,
					       native_frames / 2U, 0));
	assert(!memcmp(input_whole, input_split, sizeof(input_whole)));
	for (sample = 0U; sample < native_frames; ++sample) {
		assert(input_whole[sample * 2U] == (sample < native_frames / 2U ? 0 : 1000));
		assert(input_whole[sample * 2U + 1U] == -1000);
	}
	assert(!whole->txrxblankingtimer && !split->txrxblankingtimer);
	assert(!urp_radio_destroy(whole));
	assert(!urp_radio_destroy(split));

	/* A timer edge can fall between millisecond boundaries.  The two 36-frame
	 * calls must blank the same 48 physical samples as one 72-frame call. */
	{
		enum { sub_frames = 72U, split_frames = sub_frames / 2U };
		int16_t whole_input[sub_frames * 2U];
		int16_t split_input[sub_frames * 2U];
		int16_t whole_output[sub_frames / 6U] = {0};
		int16_t split_output[sub_frames / 6U] = {0};

		whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		split = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(whole && split);
		whole->b.ctcssRxEnable = split->b.ctcssRxEnable = 0;
		for (sample = 0U; sample < sub_frames; ++sample) {
			whole_input[sample * 2U] = 1000;
			whole_input[sample * 2U + 1U] = -1000;
		}
		memcpy(split_input, whole_input, sizeof(split_input));
		whole->txrxblankingtimer = split->txrxblankingtimer = 1;
		assert(!urp_radio_process_native_timed(whole, whole_input, whole_output, NULL,
						       sub_frames, 0));
		assert(!urp_radio_process_native_timed(split, split_input, split_output, NULL,
						       split_frames, 0));
		assert(!urp_radio_process_native_timed(split, split_input + split_frames * 2U,
						       split_output + split_frames / 6U, NULL,
						       split_frames, 0));
		assert(!memcmp(whole_input, split_input, sizeof(whole_input)));
		for (sample = 0U; sample < sub_frames; ++sample) {
			assert(whole_input[sample * 2U] == (sample < 48U ? 0 : 1000));
			assert(whole_input[sample * 2U + 1U] == -1000);
		}
		assert(!whole->txrxblankingtimer && !split->txrxblankingtimer);
		assert(!urp_radio_destroy(whole));
		assert(!urp_radio_destroy(split));
	}

	/* A new physical release begins a complete blanking interval even if an
	 * earlier receiver timer ended with a fractional native-sample remainder. */
	{
		enum { arm_frames = 36U, blank_frames = 48U };
		int16_t arm_input[arm_frames * 2U] = {0};
		int16_t blank_input[blank_frames * 2U];
		int16_t blank_output[blank_frames / 6U] = {0};

		whole = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(whole);
		whole->b.ctcssRxEnable = 0;
		assert(!urp_radio_process_native_timed(whole, arm_input, NULL, NULL, arm_frames,
						       0));
		assert(whole->rxTimerSampleRemainder == arm_frames);
		for (sample = 0U; sample < blank_frames; ++sample) {
			blank_input[sample * 2U] = 1000;
			blank_input[sample * 2U + 1U] = -1000;
		}
		whole->txrxblankingtime = 1;
		urp_radio_arm_txrx_blanking(whole);
		assert(!urp_radio_process_native_timed(whole, blank_input, blank_output, NULL,
						       blank_frames, 0));
		for (sample = 0U; sample < blank_frames; ++sample) {
			assert(blank_input[sample * 2U] == 0);
			assert(blank_input[sample * 2U + 1U] == -1000);
		}
		assert(!whole->txrxblankingtimer);
		assert(!urp_radio_destroy(whole));
	}
}

/** @brief Reject malformed DCS receive and transmit settings independently of CTCSS setup. */
static void test_invalid_dcs_radio_configuration(void)
{
	urp_radio_state template = {0};
	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "0";
	memcpy(template.dcsRxCode, "023X", sizeof(template.dcsRxCode));
	memcpy(template.dcsTxCode, "888N", sizeof(template.dcsTxCode));
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	assert(!state->dcs.enabled_receive);
	assert(!state->dcs.enabled_transmit);
	assert(state->dcs.receive_code == -1);
	assert(state->dcs.transmit_code == -1);
	assert(!urp_radio_destroy(state));
}

/** @brief Verify every user-selectable DCS turn-off duration retains PTT through its tail. */
static void test_dcs_turnoff_duration_bounds(void)
{
	static const int durations[] = {150, 180, 200};
	size_t duration_index;

	for (duration_index = 0; duration_index < sizeof(durations) / sizeof(durations[0]);
	     ++duration_index) {
		urp_radio_state template = {0};
		urp_radio_state *state;
		unsigned int blocks = 0;

		template.pRxCodeSrc = "0";
		template.pTxCodeSrc = "0";
		template.pTxCodeDefault = "0";
		template.dcsTurnoffDuration = durations[duration_index];
		memcpy(template.dcsTxCode, "023N", sizeof(template.dcsTxCode));
		state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		assert(state);
		state->dcsTurnoffEnabled = 1;
		state->txPttIn = 1;
		assert(process_once(state) == 0);
		assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txPttOut);
		state->txPttIn = 0;
		assert(process_once(state) == 0);
		assert(state->txState == CHAN_TXSTATE_TOC &&
		       state->dcsTurnoffTimer == durations[duration_index] - MS_PER_FRAME &&
		       state->txPttOut);
		while (state->txState == CHAN_TXSTATE_TOC) {
			assert(state->txPttOut);
			assert(process_once(state) == 0);
			assert(++blocks <= 11U);
		}
		assert(state->txState == CHAN_TXSTATE_FINISHING && state->dcsTurnoffTimer == 0);
		while (state->txState != CHAN_TXSTATE_IDLE) {
			assert(process_once(state) == 0);
			assert(++blocks <= 16U);
		}
		assert(!state->txPttOut);
		assert(!urp_radio_destroy(state));
	}
}

/** @brief Verify DCS receive qualification and DCS turn-off signaling transitions. */
static void test_dcs_radio_state_machine(void)
{
	urp_radio_state template = {0};
	struct dcs_radio_callback_state callback = {.valid = 1, .calls = 0U};
	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "0";
	template.txCtcssTocTime = 250;
	template.txCtcssTocShift = 135.0;
	template.txCtcssTocToneHz = 67.0;
	template.dcsTurnoffDuration = 180;
	template.dcsPeak = 500.0;
	memcpy(template.dcsRxCode, "023N", sizeof(template.dcsRxCode));
	memcpy(template.dcsTxCode, "023N", sizeof(template.dcsTxCode));
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	assert(state->dcs.enabled_receive && state->dcs.enabled_transmit);
	assert(state->txCtcssTocTime == 250 && state->txCtcssTocShift == 135.0);
	assert(state->txCtcssTocToneHz == 67.0);
	assert(state->dcsTurnoffDuration == 180 && state->dcsPeak == 500.0);

	/* A closed renderer has no portable DCS object and must fail closed. */
	assert(process_once(state) == 0);
	assert(!state->dcs.valid);
	/* The portable receiver is called at the same post-blanking point. Its
	 * result drives SMODE_DCS during this process call and avoids consuming the
	 * retained C phase bank a second time. */
	urp_dcs_configure(&state->dcs, 023, 0, 023, 0);
	urp_dcs_set_receive_callback(&state->dcs, radio_dcs_receive_callback, &callback);
	state->smode = SMODE_NULL;
	state->smodewas = SMODE_NULL;
	state->smodetimer = 0;
	assert(process_once(state) == 0);
	assert(callback.calls == 1U && state->dcs.valid);
	assert(state->smode == SMODE_DCS && state->smodewas == SMODE_DCS);
	urp_dcs_set_receive_callback(&state->dcs, NULL, NULL);

	/* Keep a qualified detector result stable while exercising signaling selection. */
	state->dcs.enabled_receive = 0;
	state->dcs.valid = 1;
	state->smode = SMODE_NULL;
	state->smodetime = 3 * MS_PER_FRAME;
	state->smodetimer = 0;
	assert(process_once(state) == 0);
	assert(state->smode == SMODE_DCS && state->smodewas == SMODE_DCS);
	assert(state->smodetimer == 3 * MS_PER_FRAME);

	/* A qualified DCS detector must not override an active CTCSS selection. */
	state->smode = SMODE_CTCSS;
	assert(process_once(state) == 0 && state->smode == SMODE_CTCSS);
	state->smode = SMODE_NULL;
	state->smodetimer = 0;
	assert(process_once(state) == 0 && state->smode == SMODE_DCS);

	state->dcsTurnoffTimer = 99;
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txPttOut);
	assert(state->dcsTurnoffTimer == 0);

	state->dcsTurnoffEnabled = 1;
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC);
	assert(state->dcsTurnoffTimer == 160 && state->txHangTime == 0 && state->txPttOut);
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->dcsTurnoffTimer == 140);

	/* A rapid rekey must cancel DCS tail audio rather than finish an obsolete TOC. */
	state->txPttIn = 1;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->dcsTurnoffTimer == 0 &&
	       state->txPttOut && !state->txCtcssEnabled);
	state->txPttIn = 0;
	assert(process_once(state) == 0);
	assert(state->txState == CHAN_TXSTATE_TOC && state->dcsTurnoffTimer == 160);
	while (state->txState == CHAN_TXSTATE_TOC) {
		assert(state->txPttOut);
		assert(process_once(state) == 0);
	}
	assert(state->txState == CHAN_TXSTATE_FINISHING && state->dcsTurnoffTimer == 0 &&
	       state->txBufferClear == 3);

	/* A configured DCS transmitter without a turn-off code releases normally. */
	state->txState = CHAN_TXSTATE_IDLE;
	state->txPttOut = 0;
	state->dcsTurnoffEnabled = 0;
	state->txPttIn = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE);
	state->txPttIn = 0;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_FINISHING);
	assert(!urp_radio_destroy(state));
}

/** @brief Verify a receive DCS selection cannot block a transmit CTCSS tail rekey. */
static void test_ctcss_rekey_remains_transmit_directional(void)
{
	urp_radio_state template = {0};
	urp_radio_state *state;

	template.pRxCodeSrc = "0";
	template.pTxCodeSrc = "0";
	template.pTxCodeDefault = "100.0";
	memcpy(template.dcsRxCode, "023N", sizeof(template.dcsRxCode));
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	assert(state->dcs.enabled_receive && !state->dcs.enabled_transmit &&
	       state->b.ctcssTxEnable);
	state->txPttIn = 1;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_ACTIVE &&
	       state->txCtcssEnabled);
	state->txTocType = TOC_NOTONE;
	state->txPttIn = 0;
	assert(process_once(state) == 0 && state->txState == CHAN_TXSTATE_TOC &&
	       !state->txCtcssEnabled);
	/* Model a qualified receive DCS decoder while the transmit CTCSS tail is
	 * active. The transmit selection—not receive smode—must restore CTCSS. */
	state->smode = SMODE_DCS;
	state->txPttIn = 1;
	assert(process_once(state) == 0 && state->smode == SMODE_DCS &&
	       state->txState == CHAN_TXSTATE_ACTIVE && state->txCtcssEnabled &&
	       state->txCtcssState == 1);
	assert(!urp_radio_destroy(state));
}

/** @brief Verify an unselected receive CTCSS decoder uses the transmit default. */
static void test_ctcss_transmit_default_without_decoded_receive_tone(void)
{
	urp_radio_state template = {0};
	urp_radio_state *state;

	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state);
	assert(state->b.ctcssTxEnable);
	state->smode = SMODE_CTCSS;
	state->rxCtcss->decode = CTCSS_NULL;
	state->txPttIn = 1;
	assert(!process_once(state));
	assert(state->txState == CHAN_TXSTATE_ACTIVE && state->txCtcssEnabled);
	assert(state->txCtcssFreq10 == 1000);
	assert(!urp_radio_destroy(state));
}

/** @brief Select one receive tone for a deterministic CTCSS bridge test.
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
	state->rxCarrierDetect = 1;
	state->nSamplesRx = 1;
	state->activeSamplesRx = 1;
	state->rxCtcss->input[0] = 0;
}

/** @brief Forward the C detector tap to the actual dynamically linked Rust decoder.
 * @param context Stream-owned Rust radio object.
 * @param samples Center-sliced S16 detector input.
 * @param sample_count Number of 8 kHz detector samples.
 * @param tone_mask Selected tone indexes.
 * @param relax Talk-off tolerance selection.
 * @param carrier_detect Current carrier qualification.
 * @param decoded Receives the qualified tone index.
 * @return Shared decoder operation status.
 */
static int ctcss_rust_receive_callback(void *context, const i16 *samples, size_t sample_count,
				       uint64_t tone_mask, int relax, int carrier_detect,
				       int *decoded)
{
	float input[SAMPLES_PER_BLOCK];
	struct rptadv_radio *radio = context;
	assert(sample_count <= SAMPLES_PER_BLOCK);
	assert(tone_mask == (UINT64_C(1) << urp_ctcss_frequency_index(100.0F)) && !relax);
	for (size_t i = 0; i < sample_count; ++i)
		input[i] = (float)samples[i] / 32768.0F;
	return urp_radio_core_process_ctcss_receive(radio, input, sample_count, carrier_detect,
						    decoded);
}

/** @brief Prove tone qualification and carrier loss across the C/Rust receive boundary. */
static void test_ctcss_shared_receiver(void)
{
	urp_radio_state template = {0};
	struct rptadv_radio *radio = NULL;
	const int index = urp_ctcss_frequency_index(100.0F);
	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	urp_radio_state *state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state && !urp_radio_core_create(48000U, 960U, &radio));
	assert(!urp_radio_core_configure_ctcss_receive(radio, UINT64_C(1) << index, 0));
	prepare_single_ctcss_detector(state, index);
	state->nSamplesRx = state->activeSamplesRx = SAMPLES_PER_BLOCK;
	urp_ctcss_set_receive_callback(state->rxCtcss, ctcss_rust_receive_callback, radio);
	for (size_t block = 0; block < 50U; ++block) {
		for (size_t i = 0; i < SAMPLES_PER_BLOCK; ++i)
			state->rxCtcss->input[i] =
				(i16)(3000.0 *
				      sin(2.0 * M_PI * 100.0 *
					  (double)(block * SAMPLES_PER_BLOCK + i) / 8000.0));
		assert(urp_ctcss_decode(state) == 0);
	}
	assert(state->rxCtcss->decode == index && !strcmp(state->rxctcssfreq, "100.0"));
	state->rxCarrierDetect = 0;
	assert(urp_ctcss_decode(state) == 0 && state->rxCtcss->decode == CTCSS_NULL);
	assert(!strcmp(state->rxctcssfreq, "0"));
	urp_ctcss_set_receive_callback(state->rxCtcss, NULL, NULL);
	urp_radio_core_destroy(radio);
	assert(!urp_radio_destroy(state));
}

/** Callback state proving CTCSS ordering and failure qualification. */
struct ctcss_callback_state {
	/** Number of callback calls observed by the harness. */
	unsigned int calls;
	/** Callback status returned to the signaling engine. */
	int result;
	/** Portable result returned on callback success. */
	int decoded;
	/** Last sample pointer passed at the established decoder point. */
	const i16 *samples;
	/** Last post-filter sample count. */
	size_t sample_count;
	/** Last configured CTCSS table mask. */
	uint64_t tone_mask;
	/** Last compatibility relaxed-talk-off selection. */
	int relax;
	/** Last carrier decision passed to portable decoding. */
	int carrier_detect;
};

/** @brief Script a portable CTCSS receive result at the legacy decode point.
 * @param context Harness state describing the desired callback result.
 * @param samples Post-filter center-slicer input received from the radio core.
 * @param sample_count Number of post-filter samples.
 * @param tone_mask Selected legacy CTCSS detector indexes.
 * @param relax Current relaxed talk-off setting.
 * @param carrier_detect Current carrier decision.
 * @param decoded Receives the scripted CTCSS table index on success.
 * @return Scripted callback status.
 */
static int ctcss_receive_callback(void *context, const i16 *samples, size_t sample_count,
				  uint64_t tone_mask, int relax, int carrier_detect, int *decoded)
{
	struct ctcss_callback_state *state = context;

	++state->calls;
	state->samples = samples;
	state->sample_count = sample_count;
	state->tone_mask = tone_mask;
	state->relax = relax;
	state->carrier_detect = carrier_detect;
	if (!state->result && decoded)
		*decoded = state->decoded;
	return state->result;
}

/** @brief Verify receiver delegation, status publication, and fail-closed errors. */
static void test_portable_ctcss_receive_callback(void)
{
	urp_radio_state template = {0};
	struct ctcss_callback_state callback = {0};
	const int index = urp_ctcss_frequency_index(100.0F);
	urp_radio_state *state;

	template.pRxCodeSrc = "100.0";
	template.pTxCodeSrc = "100.0";
	template.pTxCodeDefault = "100.0";
	state = urp_radio_create(&template, SAMPLES_PER_BLOCK);
	assert(state && index > CTCSS_NULL);
	prepare_single_ctcss_detector(state, index);
	state->rxCtcss->input[0] = 100;
	state->rxCtcss->relax = 1;
	callback.decoded = index;
	urp_ctcss_set_receive_callback(state->rxCtcss, ctcss_receive_callback, &callback);
	assert(urp_ctcss_decode(state) == 0);
	assert(callback.calls == 1U && callback.samples == state->rxCtcss->input);
	assert(callback.sample_count == 1U && callback.tone_mask == (UINT64_C(1) << index));
	assert(callback.relax && callback.carrier_detect);
	assert(state->rxCtcss->decode == index && !strcmp(state->rxctcssfreq, "100.0"));

	callback.decoded = CTCSS_NULL;
	assert(urp_ctcss_decode(state) == 0);
	assert(state->rxCtcss->decode == CTCSS_NULL && !strcmp(state->rxctcssfreq, "0"));

	/* No stale decision may survive a failed or malformed Rust response. */
	callback.decoded = index;
	assert(urp_ctcss_decode(state) == 0);
	callback.result = -1;
	assert(urp_ctcss_decode(state) == -1);
	assert(state->rxCtcss->decode == CTCSS_NULL && !strcmp(state->rxctcssfreq, "0"));
	callback.result = 0;
	callback.decoded = CTCSS_NUM_CODES;
	assert(urp_ctcss_decode(state) == -1 && state->rxCtcss->decode == CTCSS_NULL);
	callback.decoded = CTCSS_NULL - 1;
	assert(urp_ctcss_decode(state) == -1 && state->rxCtcss->decode == CTCSS_NULL);

	/* Disabled reception does not call the decoder. A detached stream cannot qualify. */
	const unsigned int calls = callback.calls;
	state->rxCtcss->enabled = 0;
	assert(urp_ctcss_decode(state) == 1 && callback.calls == calls);
	state->rxCtcss->enabled = 1;
	urp_ctcss_set_receive_callback(state->rxCtcss, NULL, NULL);
	assert(urp_ctcss_decode(state) == -1 && state->rxCtcss->decode == CTCSS_NULL);
	urp_ctcss_set_receive_callback(NULL, NULL, NULL);
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
			assert(split->spsRx->rssiSamples ==
			       (half == 0U ? SAMPLES_PER_BLOCK * 3U : 0U));
		}
		assert(whole->spsRx->compOut == split->spsRx->compOut);
		/* The tune/calibration scale is a fixed 960-native-sample window,
		 * independent of callback partitioning. */
		assert(whole->rxRssi == split->rxRssi);
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

/** @brief Verify the optional Rust frontend matches the retained C trace path.
 *
 * Setting @c tracetype retains the C frontend because that path publishes
 * diagnostic noise storage.  Its normal DSP result is otherwise the same
 * ordinary discriminator-noise shape, making it a deterministic in-process
 * parity oracle for the transactional portable bridge.
 */
static void test_portable_receive_frontend_parity(void)
{
	enum { native_frames = SAMPLES_PER_BLOCK * SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK };
	urp_radio_state template = {.pRxCodeSrc = "0",
				    .pTxCodeSrc = "0",
				    .pTxCodeDefault = "0",
				    .rxCdType = CD_XPMR_NOISE,
				    .rxDemod = RX_AUDIO_FLAT};

	for (int noise_filter = 0; noise_filter < 2; ++noise_filter) {
		int16_t portable_input[native_frames * 2U];
		int16_t legacy_input[native_frames * 2U];
		urp_radio_state *portable = urp_radio_create(&template, SAMPLES_PER_BLOCK);
		urp_radio_state *legacy = urp_radio_create(&template, SAMPLES_PER_BLOCK);

		assert(portable && legacy);
		portable->rxNoiseFilType = legacy->rxNoiseFilType = noise_filter;
		/* This retains the C frontend while leaving its normal audio/DSP output
		 * unchanged; the trace-only pRxNoise write is deliberately not compared. */
		legacy->tracetype = 1;
		for (unsigned int block = 0U; block < 4U; ++block) {
			for (size_t sample = 0U; sample < native_frames; ++sample) {
				const int16_t left =
					(int16_t)(((sample * 337U + block * 791U) % 28001U) -
						  14000);
				const int16_t right =
					(int16_t)(14000 -
						  ((sample * 149U + block * 431U) % 28001U));

				portable_input[sample * 2U] = legacy_input[sample * 2U] = left;
				portable_input[sample * 2U + 1U] = legacy_input[sample * 2U + 1U] =
					right;
			}
			portable->spsRx->source = portable_input;
			legacy->spsRx->source = legacy_input;
			portable->spsRx->nativeSamples = legacy->spsRx->nativeSamples =
				native_frames;
			portable->spsRx->nSamples = legacy->spsRx->nSamples = SAMPLES_PER_BLOCK;
			assert(!urp_radio_receive_frontend(portable->spsRx));
			assert(!urp_radio_receive_frontend(legacy->spsRx));
			assert(portable->spsRx->nSamples == legacy->spsRx->nSamples);
			assert(!memcmp(portable->pRxBase, legacy->pRxBase,
				       (size_t)portable->spsRx->nSamples *
					       sizeof(*portable->pRxBase)));
			assert(!memcmp(portable->rxCarrierGate, legacy->rxCarrierGate,
				       native_frames * sizeof(*portable->rxCarrierGate)));
			assert(!memcmp(portable->spsRx->x, legacy->spsRx->x,
				       (size_t)portable->spsRx->nx * sizeof(*portable->spsRx->x)));
			assert(portable->spsRx->decimator == legacy->spsRx->decimator &&
			       portable->spsRx->compOut == legacy->spsRx->compOut &&
			       portable->spsRx->apeak == legacy->spsRx->apeak &&
			       portable->spsRx->rssiPower == legacy->spsRx->rssiPower &&
			       portable->spsRx->rssiSamples == legacy->spsRx->rssiSamples &&
			       portable->rxRssi == legacy->rxRssi &&
			       portable->spsRx->micor_squelch.noise_power ==
				       legacy->spsRx->micor_squelch.noise_power &&
			       portable->spsRx->micor_squelch.idle_power ==
				       legacy->spsRx->micor_squelch.idle_power &&
			       portable->spsRx->micor_squelch.hold_charge ==
				       legacy->spsRx->micor_squelch.hold_charge &&
			       portable->spsRx->micor_squelch.settling_samples ==
				       legacy->spsRx->micor_squelch.settling_samples);
		}
		assert(!urp_radio_destroy(legacy));
		assert(!urp_radio_destroy(portable));
	}
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
	state->txFinishTimer = 0;
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
	state->txFinishTimer = 0;
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
	assert(failure_index > 0 && failure_index < 512);
	allocations_until_failure = -1;
}

#include "radio_processing_boundary_cases.h"
#include "radio_primitive_boundary_cases.h"

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
	RUN_TEST(test_portable_fir);
	RUN_TEST(test_portable_measure_block);
	RUN_TEST(test_portable_delay_line);
	RUN_TEST(test_portable_center_slicer);
	RUN_TEST(test_portable_deemphasis_integrator);
	RUN_TEST(test_create_process_destroy);
	RUN_TEST(test_create_variants);
	RUN_TEST(test_runtime_state_machine);
	RUN_TEST(test_transmit_timeline_admission);
	RUN_TEST(test_native_frame_partitioning);
	RUN_TEST(test_native_variable_frame_receive_partitioning);
	RUN_TEST(test_rx_blanking_partitioning);
	RUN_TEST(test_invalid_dcs_radio_configuration);
	RUN_TEST(test_dcs_turnoff_duration_bounds);
	RUN_TEST(test_dcs_radio_state_machine);
	RUN_TEST(test_ctcss_rekey_remains_transmit_directional);
	RUN_TEST(test_ctcss_transmit_default_without_decoded_receive_tone);
	RUN_TEST(test_ctcss_transmit_startup_edges);
	RUN_TEST(test_portable_ctcss_receive_callback);
	RUN_TEST(test_ctcss_shared_receiver);
	RUN_TEST(test_frontend_edges);
	RUN_TEST(test_frontend_sample_gate);
	RUN_TEST(test_portable_receive_frontend_parity);
	RUN_TEST(test_frontend_gaussian_noise);
	RUN_TEST(test_cpu_saver_predicates);
	RUN_TEST(test_lifecycle_edges);
	RUN_TEST(test_allocation_failures);
	RUN_TEST(test_processing_rejected_core_fallbacks);
	RUN_TEST(test_processing_scalar_boundaries);
	RUN_TEST(test_processing_runtime_edges);
	RUN_TEST(test_radio_primitive_boundaries);
#undef RUN_TEST
	puts("native radio core tests passed");
	return 0;
}

/** @def RUN_TEST
 * @brief RUN TEST selection for this isolated test harness.
 */
