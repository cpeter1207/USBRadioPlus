/**
 * @file test_portaudio_poc_identity.c
 * @brief No-hardware identity checks for the combined PortAudio/CM119 GPIO proof.
 */

#include <assert.h>
#include <string.h>

#include "usbradioplus_portaudio_poc_identity.h"

/** @brief Return one structurally prepared facade snapshot without opening hardware. */
static struct usbradioplus_hardware_adapter prepared_adapter(void)
{
	static const struct rptadv_audio_adapter_descriptor audio = {
		.struct_size = sizeof(audio),
		.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
	};
	static const struct rptadv_gpio_adapter_descriptor gpio = {
		.struct_size = sizeof(gpio),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	};
	struct usbradioplus_hardware_adapter adapter = {
		.audio = &audio,
		.gpio = &gpio,
	};

	strcpy(adapter.usb_port_path, "3-1:1.0");
	return adapter;
}

/** @brief Confirm one complete combined selection binds audio and HID. */
static void test_valid_combined_binding(void)
{
	struct usbradioplus_hardware_adapter adapter = prepared_adapter();

	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK);
}

/** @brief A selected combined POC must not fall back when its facade is absent. */
static void test_missing_prepared_facade_is_rejected(void)
{
	struct usbradioplus_hardware_adapter adapter = prepared_adapter();

	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 0, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_MISSING_FACADE);
}

/** @brief Reject a GPIO topology that does not identify the audio-selected CM119. */
static void test_mismatched_gpio_topology_is_rejected(void)
{
	struct usbradioplus_hardware_adapter adapter = prepared_adapter();

	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "2-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_TOPOLOGY_MISMATCH);
}

/** @brief Validate optional topology and every incomplete composition boundary. */
static void test_identity_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = prepared_adapter();
	assert(usbradioplus_portaudio_poc_combined_facade_validate(NULL, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID);
	adapter.audio = NULL;
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID);
	adapter = prepared_adapter();
	adapter.gpio = NULL;
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID);
	adapter = prepared_adapter();
	adapter.usb_port_path[0] = '\0';
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_INVALID);
	adapter = prepared_adapter();
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, NULL) ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK);
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_OK);
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1.2") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_TOPOLOGY_MISMATCH);
	strcpy(adapter.usb_port_path, ":1.0");
	assert(usbradioplus_portaudio_poc_combined_facade_validate(&adapter, 1, "3-1") ==
	       USBRADIOPLUS_PORTAUDIO_POC_IDENTITY_TOPOLOGY_MISMATCH);
}

int main(void)
{
	test_valid_combined_binding();
	test_missing_prepared_facade_is_rejected();
	test_mismatched_gpio_topology_is_rejected();
	test_identity_boundaries();
	return 0;
}
