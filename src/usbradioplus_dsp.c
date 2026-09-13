/** @file
 * @brief Transitional S16 sample-rate conversion.
 *
 * Mono conversion delegates normalized F32 PCM to the released sample-rate
 * adapter. Only the Asterisk-facing S16 boundary remains here.
 */

#ifdef AST_MODULE
#include "asterisk.h"
#endif

#include "usbradioplus_dsp.h"
#include "usbradioplus_samplerate_adapter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef AST_MODULE
#include "asterisk/utils.h"
#define URP_CALLOC(n, s) ast_calloc((n), (s))
#define URP_REALLOC(p, s) ast_realloc((p), (s))
#define URP_FREE(p) ast_free((p))
#elif defined(URP_TEST_ALLOCATORS)
void *urp_test_calloc(size_t count, size_t size);
void *urp_test_realloc(void *pointer, size_t size);
#define URP_CALLOC(n, s) urp_test_calloc((n), (s))
#define URP_REALLOC(p, s) urp_test_realloc((p), (s))
#define URP_FREE(p) free((p))
#else

#define URP_CALLOC(n, s) calloc((n), (s))

#define URP_REALLOC(p, s) realloc((p), (s))

#define URP_FREE(p) free((p))
#endif

/** Owned converter state and reusable floating-point boundary workspaces. */
struct urp_src {
	/** Released Rust adapter owning the persistent mono converter. */
	struct usbradioplus_samplerate_adapter adapter;
	/** Owned floating-point input-conversion workspace. */
	float *input;
	/** Output workspace for the current conversion. */
	float *output;
	/** Allocated input workspace in samples. */
	size_t input_capacity;
	/** Allocated output workspace in samples. */
	size_t output_capacity;
};

/** @brief Round and clamp a sample to the signed 16-bit PCM range.
 * @param value Sample amplitude in signed PCM codes.
 * @return Nearest bounded signed 16-bit PCM sample.
 */
static int16_t saturate(double value)
{
	if (value > 32767.0)
		return 32767;
	if (value < -32768.0)
		return -32768;
	return (int16_t)lrint(value);
}

/**
 * @brief Convert one signed-16 boundary sample to normalized canonical F32.
 * @param sample Signed-16 PCM sample at the legacy boundary.
 * @return Normalized F32 sample in the canonical internal range.
 */
static float pcm_s16_to_f32(int16_t sample)
{
	return (float)sample / 32768.0F;
}

/**
 * @brief Quantize one normalized canonical F32 sample at the legacy boundary.
 * @param sample Normalized F32 sample from the canonical internal range.
 * @return Rounded, saturated signed-16 PCM sample.
 */
static int16_t pcm_f32_to_s16(float sample)
{
	return saturate((double)sample * 32768.0);
}

struct urp_src *urp_src_create(int converter, unsigned int channels)
{
	struct urp_src *src;
	if (channels != 1U)
		return NULL;
	src = URP_CALLOC(1, sizeof(*src));
	if (!src)
		return NULL;
	if (usbradioplus_samplerate_adapter_prepare_released(
		    &src->adapter, (enum rptadv_samplerate_quality)converter) !=
	    USBRADIOPLUS_SAMPLERATE_ADAPTER_OK) {
		URP_FREE(src);
		return NULL;
	}
	return src;
}

void urp_src_destroy(struct urp_src *src)
{
	if (!src)
		return;
	usbradioplus_samplerate_adapter_close(&src->adapter);
	URP_FREE(src->input);
	URP_FREE(src->output);
	URP_FREE(src);
}

void urp_src_reset(struct urp_src *src)
{
	if (!src)
		return;
	(void)usbradioplus_samplerate_adapter_reset(&src->adapter);
}

int urp_src_reserve(struct urp_src *src, size_t input_capacity, size_t output_capacity)
{
	float *input;
	float *output;

	if (!src || !input_capacity || !output_capacity || input_capacity > UINT32_MAX ||
	    output_capacity > UINT32_MAX || input_capacity > SIZE_MAX / sizeof(*input) ||
	    output_capacity > SIZE_MAX / sizeof(*output))
		return -1;
	if (input_capacity > src->input_capacity) {
		input = URP_REALLOC(src->input, input_capacity * sizeof(*input));
		if (!input)
			return -1;
		src->input = input;
		src->input_capacity = input_capacity;
	}
	if (output_capacity > src->output_capacity) {
		output = URP_REALLOC(src->output, output_capacity * sizeof(*output));
		if (!output)
			return -1;
		src->output = output;
		src->output_capacity = output_capacity;
	}
	return 0;
}

int urp_src_process_prepared(struct urp_src *src, const int16_t *input, size_t input_count,
			     int16_t *output, size_t output_capacity, double ratio,
			     size_t *input_used, size_t *output_generated)
{
	uint32_t used = 0U;
	uint32_t made = 0U;
	size_t i;

	if (!src || !input || !output || ratio <= 0.0 || input_count > src->input_capacity ||
	    output_capacity > src->output_capacity)
		return -1;
	/* Setup bounds both workspaces to the adapter's uint32_t frame-count ABI. */
	for (i = 0U; i < input_count; ++i)
		src->input[i] = pcm_s16_to_f32(input[i]);
	if (usbradioplus_samplerate_adapter_process(&src->adapter, src->input,
						    (uint32_t)input_count, src->output,
						    (uint32_t)output_capacity, ratio, &used,
						    &made) != USBRADIOPLUS_SAMPLERATE_ADAPTER_OK)
		return -1;
	for (i = 0U; i < made; ++i)
		output[i] = pcm_f32_to_s16(src->output[i]);
	if (input_used)
		*input_used = used;
	if (output_generated)
		*output_generated = made;
	for (; i < output_capacity; ++i)
		output[i] = 0;
	return 0;
}

int urp_rate_convert_prepared(struct urp_src *src, const int16_t *input, size_t input_count,
			      unsigned int input_rate, int16_t *output, size_t output_capacity,
			      unsigned int output_rate, size_t *input_used,
			      size_t *output_generated)
{
	size_t copied;
	if (!input || !output || !input_rate || !output_rate)
		return -1;
	if (input_rate != output_rate) {
		return urp_src_process_prepared(src, input, input_count, output, output_capacity,
						(double)output_rate / input_rate, input_used,
						output_generated);
	}
	/* Matching rates need no converter state, allocation, or filter delay. */
	copied = input_count < output_capacity ? input_count : output_capacity;
	memmove(output, input, copied * sizeof(*output));
	if (copied < output_capacity)
		memset(output + copied, 0, (output_capacity - copied) * sizeof(*output));
	if (input_used)
		*input_used = copied;
	if (output_generated)
		*output_generated = copied;
	return input_count <= output_capacity ? 0 : -1;
}

/** @name File-local and build-time constants
 * @{ */
/** @def URP_CALLOC
 * @brief Allocation entry point replaceable by the failure-injection harness.
 */
/** @def URP_REALLOC
 * @brief Reallocation entry point replaceable by the failure-injection harness.
 */
/** @def URP_FREE
 * @brief Deallocation entry point replaceable by the failure-injection harness.
 */
/** @} */
