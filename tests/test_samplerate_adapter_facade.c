/**
 * @file
 * @brief Executable descriptor and forwarding checks for the sample-rate facade.
 */

#include "../src/usbradioplus_samplerate_adapter.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** Opaque fake converter storage used only by this descriptor harness. */
static int fake_converter_storage;
/** Configured fake create result. */
static enum rptadv_samplerate_adapter_result fake_create_result;
/** Configured fake reset result. */
static enum rptadv_samplerate_adapter_result fake_reset_result;
/** Configured fake process result. */
static enum rptadv_samplerate_adapter_result fake_process_result;
/** Whether a failed fake creation still returns owned storage for cleanup. */
static int fake_create_returns_converter_on_error;
/** Number of fake lifecycle calls observed by the harness. */
static unsigned int fake_create_calls, fake_reset_calls, fake_process_calls, fake_destroy_calls;

/** @brief Reset deterministic fake-descriptor observations. */
static void reset_fake(void)
{
	fake_create_result = RPTADV_SAMPLERATE_ADAPTER_OK;
	fake_reset_result = RPTADV_SAMPLERATE_ADAPTER_OK;
	fake_process_result = RPTADV_SAMPLERATE_ADAPTER_OK;
	fake_create_returns_converter_on_error = 0;
	fake_create_calls = 0U;
	fake_reset_calls = 0U;
	fake_process_calls = 0U;
	fake_destroy_calls = 0U;
}

/** @brief Create a fake mono converter with a deterministic configured outcome. */
static enum rptadv_samplerate_adapter_result
fake_create(enum rptadv_samplerate_quality quality, uint32_t channels,
	    struct rptadv_samplerate_converter **converter)
{
	fake_create_calls++;
	assert(quality == RPTADV_SAMPLERATE_QUALITY_SINC_BEST);
	assert(channels == 1U);
	assert(converter);
	*converter = (fake_create_result == RPTADV_SAMPLERATE_ADAPTER_OK ||
		      fake_create_returns_converter_on_error)
			     ? (struct rptadv_samplerate_converter *)&fake_converter_storage
			     : NULL;
	return fake_create_result;
}

/** @brief Return the deterministic reset outcome. */
static enum rptadv_samplerate_adapter_result
fake_reset(struct rptadv_samplerate_converter *converter)
{
	fake_reset_calls++;
	assert(converter == (struct rptadv_samplerate_converter *)&fake_converter_storage);
	return fake_reset_result;
}

/** @brief Copy one deterministic sample and report the configured process outcome. */
static enum rptadv_samplerate_adapter_result
fake_process(struct rptadv_samplerate_converter *converter, const float *input,
	     uint32_t input_frames, float *output, uint32_t output_capacity, double ratio,
	     uint32_t *input_used, uint32_t *output_generated)
{
	fake_process_calls++;
	assert(converter == (struct rptadv_samplerate_converter *)&fake_converter_storage);
	assert(input_used && output_generated && ratio == 1.0);
	if (input_frames == 0U || output_capacity == 0U) {
		assert(input_frames == 0U && output_capacity == 0U && !input && !output);
		return fake_process_result;
	}
	assert(input && output && input_frames == 1U && output_capacity == 1U);
	if (fake_process_result == RPTADV_SAMPLERATE_ADAPTER_OK) {
		output[0] = input[0];
		*input_used = 1U;
		*output_generated = 1U;
	}
	return fake_process_result;
}

/** @brief Record destruction of one fake converter. */
static void fake_destroy(struct rptadv_samplerate_converter *converter)
{
	fake_destroy_calls++;
	assert(converter == (struct rptadv_samplerate_converter *)&fake_converter_storage);
}

/** @brief Build one complete fake ABI-v1 descriptor. */
static struct rptadv_samplerate_adapter_descriptor fake_descriptor(void)
{
	return (struct rptadv_samplerate_adapter_descriptor){
		.struct_size = sizeof(struct rptadv_samplerate_adapter_descriptor),
		.abi_version = RPTADV_SAMPLERATE_ADAPTER_ABI_VERSION,
		.capability_name = RPTADV_SAMPLERATE_ADAPTER_CAPABILITY,
		.create = fake_create,
		.reset = fake_reset,
		.process = fake_process,
		.destroy = fake_destroy,
	};
}

/** @brief Verify complete descriptor validation and control-plane lifecycle behavior. */
static void test_descriptor_and_lifecycle(void)
{
	struct rptadv_samplerate_adapter_descriptor descriptor = fake_descriptor();
	struct usbradioplus_samplerate_adapter adapter = {0};
	float input = 0.25F, output = 0.0F;
	uint32_t used = 0U, made = 0U;

	reset_fake();
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	assert(fake_create_calls == 1U);
	assert(usbradioplus_samplerate_adapter_reset(&adapter) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	assert(fake_reset_calls == 1U);
	assert(usbradioplus_samplerate_adapter_process(&adapter, &input, 1U, &output, 1U, 1.0,
						       &used, &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	assert(fake_process_calls == 1U && used == 1U && made == 1U && output == input);
	usbradioplus_samplerate_adapter_close(&adapter);
	assert(fake_destroy_calls == 1U && !adapter.descriptor && !adapter.converter);
	usbradioplus_samplerate_adapter_close(NULL);
}

/** @brief Verify malformed descriptors and invalid bridge calls fail before forwarding. */
static void test_invalid_inputs(void)
{
	struct rptadv_samplerate_adapter_descriptor descriptor = fake_descriptor();
	struct usbradioplus_samplerate_adapter adapter = {0};
	float input = 0.0F, output = 0.0F;
	uint32_t used = 0U, made = 0U;

	reset_fake();
	assert(usbradioplus_samplerate_adapter_validate(NULL) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor.struct_size = RPTADV_SAMPLERATE_ADAPTER_DESCRIPTOR_V1_MIN_SIZE - 1U;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.abi_version++;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.capability_name = "other";
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.capability_name = NULL;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.create = NULL;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.reset = NULL;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.process = NULL;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor();
	descriptor.destroy = NULL;
	assert(usbradioplus_samplerate_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_samplerate_adapter_prepare(NULL, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_reset(NULL) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(NULL, &input, 1U, &output, 1U, 1.0, &used,
						       &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);

	descriptor = fake_descriptor();
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(&adapter, NULL, 1U, &output, 1U, 1.0, &used,
						       &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(&adapter, &input, 1U, NULL, 1U, 1.0, &used,
						       &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(&adapter, &input, 1U, &output, 1U, 1.0, NULL,
						       &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(&adapter, &input, 1U, &output, 1U, 1.0,
						       &used, NULL) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_samplerate_adapter_process(&adapter, NULL, 0U, NULL, 0U, 1.0, &used,
						       &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	usbradioplus_samplerate_adapter_close(&adapter);
}

/** @brief Verify adapter-reported failures are retained as bridge failures. */
static void test_adapter_failures(void)
{
	struct rptadv_samplerate_adapter_descriptor descriptor = fake_descriptor();
	struct usbradioplus_samplerate_adapter adapter = {0};
	float input = 0.0F, output = 0.0F;
	uint32_t used = 0U, made = 0U;

	reset_fake();
	fake_create_result = RPTADV_SAMPLERATE_ADAPTER_LIBSAMPLERATE_ERROR;
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR);
	assert(!adapter.descriptor && !adapter.converter);
	reset_fake();
	fake_create_result = RPTADV_SAMPLERATE_ADAPTER_LIBSAMPLERATE_ERROR;
	fake_create_returns_converter_on_error = 1;
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR);
	assert(fake_destroy_calls == 1U);
	reset_fake();
	assert(usbradioplus_samplerate_adapter_prepare(&adapter, &descriptor,
						       RPTADV_SAMPLERATE_QUALITY_SINC_BEST) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_OK);
	fake_reset_result = RPTADV_SAMPLERATE_ADAPTER_LIBSAMPLERATE_ERROR;
	assert(usbradioplus_samplerate_adapter_reset(&adapter) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR);
	fake_process_result = RPTADV_SAMPLERATE_ADAPTER_LIBSAMPLERATE_ERROR;
	assert(usbradioplus_samplerate_adapter_process(&adapter, &input, 1U, &output, 1U, 1.0,
						       &used, &made) ==
	       USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR);
	usbradioplus_samplerate_adapter_close(&adapter);
}

/** @brief Execute every sample-rate facade regression assertion. */
int main(void)
{
	test_descriptor_and_lifecycle();
	test_invalid_inputs();
	test_adapter_failures();
	puts("sample-rate adapter facade tests passed");
	return 0;
}
