/**
 * @file test_portaudio_poc_selection.c
 * @brief Focused identity and explicit-endpoint tests for the PortAudio POC.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#include "usbradioplus_portaudio_poc_selection.h"

/** @brief Count selector calls and retain its most recent input for assertions. */
static unsigned int selector_calls;
/** @brief Result returned by the fake released selector. */
static enum rptadv_audio_result selector_result;
/** @brief Index returned by the fake released selector for capture. */
static int selector_input_index;
/** @brief Index returned by the fake released selector for playback. */
static int selector_output_index;
/** @brief Most recent selector request copied by the fake ABI entry. */
static struct rptadv_audio_usb_device_selector selector_request;

/** @brief Resolve one configured identity without using host hardware. */
static enum rptadv_audio_result
fake_usb_device_select(const struct rptadv_audio_usb_device_selector *selector,
		       struct rptadv_audio_usb_device_match *match)
{
	selector_calls++;
	selector_request = *selector;
	if (selector_result != RPTADV_AUDIO_OK)
		return selector_result;
	assert(match);
	assert(match->struct_size == sizeof(*match));
	match->abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	match->selection.struct_size = sizeof(match->selection);
	match->selection.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	match->selection.input_device_index = selector_input_index;
	match->selection.output_device_index = selector_output_index;
	return RPTADV_AUDIO_OK;
}

/** @brief Reset fake ABI behavior before one independent selection case. */
static void reset_fake_selector(void)
{
	selector_calls = 0U;
	selector_result = RPTADV_AUDIO_OK;
	selector_input_index = 6;
	selector_output_index = 7;
	memset(&selector_request, 0, sizeof(selector_request));
}

/** @brief Return a complete descriptor with the optional selector tail present. */
static struct rptadv_audio_adapter_descriptor complete_descriptor(void)
{
	struct rptadv_audio_adapter_descriptor descriptor = {
		.struct_size = sizeof(descriptor),
		.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
		.usb_device_select = fake_usb_device_select,
	};

	return descriptor;
}

/** @brief Verify the parser accepts one complete selection policy only. */
static void test_configuration_policy(void)
{
	assert(usbradioplus_portaudio_poc_selection_config_valid("hw:4", "CM119", 2, 3));
	assert(usbradioplus_portaudio_poc_selection_config_valid("hw:4", NULL, -1, -1));
	assert(usbradioplus_portaudio_poc_selection_config_valid(NULL, "CM119", -1, -1));
	assert(!usbradioplus_portaudio_poc_selection_config_valid("hw:4", NULL, 2, -1));
	assert(!usbradioplus_portaudio_poc_selection_config_valid(NULL, "CM119", -1, 3));
	assert(!usbradioplus_portaudio_poc_selection_config_valid(NULL, NULL, -1, -1));
}

/** @brief Verify original configured numeric endpoints remain authoritative. */
static void test_explicit_device_indexes(void)
{
	struct usbradioplus_portaudio_poc_device_selection selection;

	reset_fake_selector();
	assert(usbradioplus_portaudio_poc_select_devices(NULL, "hw:4", "CM119", 2, 3, &selection) ==
	       RPTADV_AUDIO_OK);
	assert(selection.input_device_index == 2);
	assert(selection.output_device_index == 3);
	assert(!selection.selected_by_identity);
	assert(!selector_calls);
}

/** @brief Verify an exact legacy identifier and serial reach the adapter unchanged. */
static void test_exact_identifier_and_serial(void)
{
	struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct usbradioplus_portaudio_poc_device_selection selection;

	reset_fake_selector();
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4,0", "CM119-A", -1, -1,
							 &selection) == RPTADV_AUDIO_OK);
	assert(selector_calls == 1U);
	assert(selector_request.struct_size == sizeof(selector_request));
	assert(selector_request.selection_policy == RPTADV_AUDIO_USB_SELECTION_EXACT);
	assert(!strcmp(selector_request.device_identifier, "hw:4,0"));
	assert(!strcmp(selector_request.usb_serial, "CM119-A"));
	assert(selector_request.input_device_channels == 1U);
	assert(selector_request.output_device_channels == RPTADV_AUDIO_CANONICAL_CHANNELS);
	assert(selection.input_device_index == 6);
	assert(selection.output_device_index == 7);
	assert(selection.selected_by_identity);
}

/** @brief Verify an exact serial alone is a valid stable selection request. */
static void test_serial_only_identity(void)
{
	struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct usbradioplus_portaudio_poc_device_selection selection;

	reset_fake_selector();
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "", "CM119-A", -1, -1,
							 &selection) == RPTADV_AUDIO_OK);
	assert(selector_calls == 1U);
	assert(selector_request.device_identifier == NULL);
	assert(!strcmp(selector_request.usb_serial, "CM119-A"));
}

/** @brief Verify invalid, old-ABI, failed, and malformed selector outcomes fail closed. */
static void test_invalid_and_failed_selection(void)
{
	struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct usbradioplus_portaudio_poc_device_selection selection;

	reset_fake_selector();
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1, NULL) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(usbradioplus_portaudio_poc_select_devices(NULL, "hw:4", NULL, -1, -1, &selection) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	descriptor.abi_version++;
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) == RPTADV_AUDIO_UNSUPPORTED);
	descriptor = complete_descriptor();
	descriptor.usb_device_select = NULL;
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) == RPTADV_AUDIO_UNSUPPORTED);
	descriptor = complete_descriptor();
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, 1,
							 &selection) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, 1, -1,
							 &selection) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(!selector_calls);
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, NULL, NULL, -1, -1,
							 &selection) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(!selector_calls);
	descriptor.struct_size =
		offsetof(struct rptadv_audio_adapter_descriptor, usb_device_select);
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) == RPTADV_AUDIO_UNSUPPORTED);
	assert(!selector_calls);
	descriptor = complete_descriptor();
	selector_result = RPTADV_AUDIO_PORTAUDIO_ERROR;
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) ==
	       RPTADV_AUDIO_PORTAUDIO_ERROR);
	assert(selector_calls == 1U);
	selector_result = RPTADV_AUDIO_OK;
	selector_input_index = -1;
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) == RPTADV_AUDIO_UNSUPPORTED);
	selector_input_index = 1;
	selector_output_index = -1;
	assert(usbradioplus_portaudio_poc_select_devices(&descriptor, "hw:4", NULL, -1, -1,
							 &selection) == RPTADV_AUDIO_UNSUPPORTED);
}

int main(void)
{
	test_configuration_policy();
	test_explicit_device_indexes();
	test_exact_identifier_and_serial();
	test_serial_only_identity();
	test_invalid_and_failed_selection();
	return 0;
}
