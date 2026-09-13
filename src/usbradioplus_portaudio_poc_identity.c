/**
 * @file
 * @brief USBRadioPlus PortAudio POC identity.
 *
 * No-I/O CM119 identity checks for the opt-in combined hardware proof.
 */

#include "usbradioplus_portaudio_poc_identity.h"

#include <stddef.h>
#include <string.h>

/**
 * @brief Return the length of a USB physical-device component.
 * @param path Nonempty USB topology or interface path, checked by the public boundary.
 * @return Bytes through the physical-device component, excluding any `:` suffix.
 */
static size_t portaudio_poc_physical_usb_path_length(const char *path)
{
	const char *separator;

	separator = strchr(path, ':');
	return separator ? (size_t)(separator - path) : strlen(path);
}

/**
 * @brief Return whether two topology strings identify the same USB device.
 * @param first First USB topology or interface path.
 * @param second Second USB topology or interface path.
 * @return Nonzero when both paths have the same physical-device component.
 */
static int portaudio_poc_same_physical_usb_device(const char *first, const char *second)
{
	size_t first_length = portaudio_poc_physical_usb_path_length(first);
	size_t second_length = portaudio_poc_physical_usb_path_length(second);

	return first_length != 0U && first_length == second_length &&
	       !strncmp(first, second, first_length);
}

enum usbradioplus_portaudio_poc_identity_result usbradioplus_portaudio_poc_combined_facade_validate(
	const struct usbradioplus_hardware_adapter *adapter, int prepared,
	const char *configured_gpio_usb_path)
{
	if (!prepared)
		return USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_MISSING_FACADE;
	if (!adapter || !adapter->audio || !adapter->gpio || !adapter->usb_port_path[0])
		return USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID;
	/* An explicit secondary selector is an extra consistency check. With none,
	 * both transports use the topology already resolved by the composition. */
	if (!configured_gpio_usb_path || !configured_gpio_usb_path[0])
		return USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK;
	if (!portaudio_poc_same_physical_usb_device(adapter->usb_port_path,
						    configured_gpio_usb_path))
		return USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_TOPOLOGY_MISMATCH;
	return USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK;
}
