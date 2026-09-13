/**
 * @file usbradioplus_samplerate_adapter.c
 * @brief Descriptor validation and lifecycle forwarding for sample-rate conversion.
 */

#include "usbradioplus_samplerate_adapter.h"

#include <string.h>

/**
 * @brief Return whether a descriptor safely exposes the complete ABI-v1 contract.
 * @param descriptor Candidate shared-library descriptor.
 * @return Nonzero when every required ABI-v1 member is available.
 */
static int
samplerate_adapter_descriptor_valid(const struct rptadv_samplerate_adapter_descriptor *descriptor)
{
	return descriptor &&
	       descriptor->struct_size >= RPTADV_SAMPLERATE_ADAPTER_DESCRIPTOR_V1_MIN_SIZE &&
	       descriptor->abi_version == RPTADV_SAMPLERATE_ADAPTER_ABI_VERSION &&
	       descriptor->capability_name &&
	       !strcmp(descriptor->capability_name, RPTADV_SAMPLERATE_ADAPTER_CAPABILITY) &&
	       descriptor->create && descriptor->reset && descriptor->process &&
	       descriptor->destroy;
}

enum usbradioplus_samplerate_adapter_result usbradioplus_samplerate_adapter_validate(
	const struct rptadv_samplerate_adapter_descriptor *descriptor)
{
	return samplerate_adapter_descriptor_valid(descriptor)
		       ? USBRADIOPLUS_SAMPLERATE_ADAPTER_OK
		       : USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER;
}

enum usbradioplus_samplerate_adapter_result usbradioplus_samplerate_adapter_prepare(
	struct usbradioplus_samplerate_adapter *adapter,
	const struct rptadv_samplerate_adapter_descriptor *descriptor,
	enum rptadv_samplerate_quality quality)
{
	struct rptadv_samplerate_converter *converter = NULL;

	if (!adapter || adapter->descriptor || adapter->converter)
		return USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT;
	if (usbradioplus_samplerate_adapter_validate(descriptor) !=
	    USBRADIOPLUS_SAMPLERATE_ADAPTER_OK)
		return USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (descriptor->create(quality, 1U, &converter) != RPTADV_SAMPLERATE_ADAPTER_OK ||
	    !converter) {
		if (converter)
			descriptor->destroy(converter);
		return USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR;
	}
	adapter->descriptor = descriptor;
	adapter->converter = converter;
	return USBRADIOPLUS_SAMPLERATE_ADAPTER_OK;
}

enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_prepare_released(struct usbradioplus_samplerate_adapter *adapter,
						 enum rptadv_samplerate_quality quality)
{
	return usbradioplus_samplerate_adapter_prepare(
		adapter, rptadv_samplerate_adapter_descriptor(), quality);
}

enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_reset(struct usbradioplus_samplerate_adapter *adapter)
{
	if (!adapter || !adapter->descriptor || !adapter->converter)
		return USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT;
	return adapter->descriptor->reset(adapter->converter) == RPTADV_SAMPLERATE_ADAPTER_OK
		       ? USBRADIOPLUS_SAMPLERATE_ADAPTER_OK
		       : USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR;
}

enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_process(struct usbradioplus_samplerate_adapter *adapter,
					const float *input, uint32_t input_frames, float *output,
					uint32_t output_capacity, double ratio,
					uint32_t *input_used, uint32_t *output_generated)
{
	if (!adapter || !adapter->descriptor || !adapter->converter || !input_used ||
	    !output_generated || (input_frames != 0U && !input) ||
	    (output_capacity != 0U && !output))
		return USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT;
	return adapter->descriptor->process(adapter->converter, input, input_frames, output,
					    output_capacity, ratio, input_used,
					    output_generated) == RPTADV_SAMPLERATE_ADAPTER_OK
		       ? USBRADIOPLUS_SAMPLERATE_ADAPTER_OK
		       : USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR;
}

void usbradioplus_samplerate_adapter_close(struct usbradioplus_samplerate_adapter *adapter)
{
	if (!adapter)
		return;
	if (adapter->descriptor && adapter->converter)
		adapter->descriptor->destroy(adapter->converter);
	memset(adapter, 0, sizeof(*adapter));
}
