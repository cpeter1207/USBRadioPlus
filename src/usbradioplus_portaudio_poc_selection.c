/**
 * @file
 * @brief USBRadioPlus PortAudio POC selection.
 *
 * Identity-aware endpoint selection for the optional PortAudio proof of concept.
 */

#include "usbradioplus_portaudio_poc_selection.h"

#include <stddef.h>

/** @brief End byte needed to safely read one possibly appended descriptor member. */
#define URP_POC_DESCRIPTOR_MEMBER_END(member)                                                      \
	(offsetof(struct rptadv_audio_adapter_descriptor, member) +                                \
	 sizeof(((struct rptadv_audio_adapter_descriptor *)0)->member))

/** @brief Descriptor size through the appended identity-selector entry. */
#define URP_POC_USB_DEVICE_SELECTOR_DESCRIPTOR_SIZE URP_POC_DESCRIPTOR_MEMBER_END(usb_device_select)

/** @brief Return a nonempty configuration string or NULL for an omitted value.
 * @param value Optional caller-owned configuration text.
 * @return The unchanged nonempty pointer, otherwise NULL.
 */
static const char *portaudio_poc_optional_text(const char *value)
{
	return value && *value ? value : NULL;
}

int usbradioplus_portaudio_poc_selection_config_valid(const char *device_identifier,
						      const char *usb_serial,
						      int explicit_input_device_index,
						      int explicit_output_device_index)
{
	if (explicit_input_device_index >= 0 || explicit_output_device_index >= 0)
		return explicit_input_device_index >= 0 && explicit_output_device_index >= 0;
	return portaudio_poc_optional_text(device_identifier) ||
	       portaudio_poc_optional_text(usb_serial);
}

/** @brief Return whether this adapter exports the append-only identity selector.
 * @param adapter Candidate immutable adapter descriptor.
 * @return Nonzero when the descriptor contains a compatible selector entry.
 */
static int
portaudio_poc_has_usb_device_selector(const struct rptadv_audio_adapter_descriptor *adapter)
{
	return adapter && adapter->abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION &&
	       adapter->struct_size >= URP_POC_USB_DEVICE_SELECTOR_DESCRIPTOR_SIZE &&
	       adapter->usb_device_select;
}

enum rptadv_audio_result usbradioplus_portaudio_poc_select_devices(
	const struct rptadv_audio_adapter_descriptor *adapter, const char *device_identifier,
	const char *usb_serial, int explicit_input_device_index, int explicit_output_device_index,
	struct usbradioplus_portaudio_poc_device_selection *selection)
{
	const char *identifier;
	const char *serial;
	struct rptadv_audio_usb_device_selector selector = {
		.struct_size = sizeof(selector),
		.selection_policy = RPTADV_AUDIO_USB_SELECTION_EXACT,
		.input_device_channels = 1U,
		.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS,
	};
	struct rptadv_audio_usb_device_match match = {
		.struct_size = sizeof(match),
	};
	enum rptadv_audio_result result;

	if (!selection)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	selection->input_device_index = -1;
	selection->output_device_index = -1;
	selection->selected_by_identity = 0;
	if (explicit_input_device_index >= 0 || explicit_output_device_index >= 0) {
		if (!usbradioplus_portaudio_poc_selection_config_valid(
			    device_identifier, usb_serial, explicit_input_device_index,
			    explicit_output_device_index))
			return RPTADV_AUDIO_INVALID_ARGUMENT;
		selection->input_device_index = explicit_input_device_index;
		selection->output_device_index = explicit_output_device_index;
		return RPTADV_AUDIO_OK;
	}
	identifier = portaudio_poc_optional_text(device_identifier);
	serial = portaudio_poc_optional_text(usb_serial);
	if (!usbradioplus_portaudio_poc_selection_config_valid(device_identifier, usb_serial,
							       explicit_input_device_index,
							       explicit_output_device_index))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (!portaudio_poc_has_usb_device_selector(adapter))
		return RPTADV_AUDIO_UNSUPPORTED;
	selector.device_identifier = identifier;
	selector.usb_serial = serial;
	result = adapter->usb_device_select(&selector, &match);
	if (result != RPTADV_AUDIO_OK)
		return result;
	if (match.selection.input_device_index < 0 || match.selection.output_device_index < 0)
		return RPTADV_AUDIO_UNSUPPORTED;
	selection->input_device_index = match.selection.input_device_index;
	selection->output_device_index = match.selection.output_device_index;
	selection->selected_by_identity = 1;
	return RPTADV_AUDIO_OK;
}
