/**
 * @file
 * @brief USBRadioPlus PortAudio POC identity API.
 *
 * Identity checks for the opt-in combined PortAudio and CM119 GPIO proof.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_H
#define USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_H

#include "usbradioplus_hardware_adapter.h"

/** @brief Result of validating one opt-in combined POC hardware binding. */
enum usbradioplus_portaudio_poc_identity_result {
	/** The prepared facade proves the selected CM119 identity. */
	USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK = 0,
	/** The selected combined POC has no prepared composition facade. */
	USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_MISSING_FACADE,
	/** The configured GPIO topology and resolved audio topology differ. */
	USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_TOPOLOGY_MISMATCH,
	/** The caller supplied incomplete or malformed identity state. */
	USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID,
};

/**
 * @brief Verify the facade still proves the configured audio-to-HID binding.
 * @param adapter Prepared hardware composition, or NULL.
 * @param prepared Nonzero only after successful composition preparation.
 * @param configured_gpio_usb_path Explicit CM119 GPIO topology from configuration.
 * @return One \c usbradioplus_portaudio_poc_identity_result value.
 *
 * Audio and HID may use different USB interfaces of one CM119.  The check
 * therefore compares their physical USB-device components, not their interface
 * suffixes.  It intentionally does not select hardware or perform I/O.
 */
enum usbradioplus_portaudio_poc_identity_result usbradioplus_portaudio_poc_combined_facade_validate(
	const struct usbradioplus_hardware_adapter *adapter, int prepared,
	const char *configured_gpio_usb_path);

#endif
