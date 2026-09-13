/**
 * @file
 * @brief USBRadioPlus PortAudio POC selection API.
 *
 * Control-plane endpoint selection for the optional PortAudio proof of concept.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_SELECTION_H
#define USBRADIOPLUS_PORTAUDIO_POC_SELECTION_H

#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

/** @brief Selected physical PortAudio endpoints for one optional POC stream. */
struct usbradioplus_portaudio_poc_device_selection {
	/** Exact PortAudio input-device index. */
	int input_device_index;
	/** Exact PortAudio output-device index. */
	int output_device_index;
	/** Nonzero when the released adapter resolved a configured USB identity. */
	int selected_by_identity;
};

/**
 * @brief Validate optional POC endpoint configuration before stream startup.
 * @param device_identifier Optional legacy @c devstr topology or native @c hw: identifier.
 * @param usb_serial Optional legacy exact USB serial number.
 * @param explicit_input_device_index Configured input index, or a negative value when absent.
 * @param explicit_output_device_index Configured output index, or a negative value when absent.
 * @return Nonzero when the configuration selects exactly one endpoint policy.
 *
 * A complete explicit numeric pair is valid.  Otherwise the POC requires a
 * nonempty device identifier or serial so the released selector can perform
 * an exact match.  Mixing a partial numeric pair with identity is rejected.
 */
int usbradioplus_portaudio_poc_selection_config_valid(const char *device_identifier,
						      const char *usb_serial,
						      int explicit_input_device_index,
						      int explicit_output_device_index);

/**
 * @brief Select optional POC endpoints from explicit indexes or legacy device identity.
 * @param adapter Released PortAudio/ALSA adapter descriptor.
 * @param device_identifier Optional legacy @c devstr topology or native @c hw: identifier.
 * @param usb_serial Optional legacy exact USB serial number.
 * @param explicit_input_device_index Configured input index, or a negative value when absent.
 * @param explicit_output_device_index Configured output index, or a negative value when absent.
 * @param selection Receives the exact input and output indexes.
 * @return A \c rptadv_audio_result value.
 *
 * A complete pair of configured nonnegative indexes takes precedence to retain
 * the original opt-in POC configuration behavior.  When both indexes are
 * absent, this function requests an exact identity match through the trailing
 * @c usb_device_select ABI-v1 entry.  A partial numeric pair, no identity, or
 * an adapter without that optional trailing entry is rejected rather than
 * selecting a default device.
 */
enum rptadv_audio_result usbradioplus_portaudio_poc_select_devices(
	const struct rptadv_audio_adapter_descriptor *adapter, const char *device_identifier,
	const char *usb_serial, int explicit_input_device_index, int explicit_output_device_index,
	struct usbradioplus_portaudio_poc_device_selection *selection);

#endif
