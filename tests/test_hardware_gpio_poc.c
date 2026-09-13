/**
 * @file test_hardware_gpio_poc.c
 * @brief Fake-facade checks for the opt-in legacy CM119 GPIO proof bridge.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>

#include "usbradioplus_hardware_adapter.h"
#include "usbradioplus_hardware_gpio_poc.h"

/**
 * @brief Keep the facade's released-descriptor entry point linkable in this fake-only test.
 * @return Null because this test supplies its GPIO descriptor directly.
 */
const struct rptadv_audio_adapter_descriptor *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return NULL;
}

/**
 * @brief Keep the facade's released-descriptor entry point linkable in this fake-only test.
 * @return Null because this test supplies its GPIO descriptor directly.
 */
const struct rptadv_gpio_adapter_descriptor *rptadv_gpio_adapter_descriptor(void)
{
	return NULL;
}

/** @brief Address used as the one fake open CM119 GPIO device. */
static int fake_device_token;
/** @brief Number of persistent-output publications observed by the fake. */
static unsigned int publish_calls;
/** @brief Number of independently scheduled pulse publications observed by the fake. */
static unsigned int schedule_calls;
/** @brief Latest persistent output action supplied to the fake. */
static struct rptadv_gpio_output_action published_action;
/** @brief Bounded sequence of pulse actions supplied to the fake. */
static struct rptadv_gpio_cm119_scheduled_inverting_pulse_action scheduled_actions[8];
/** @brief Result returned by the fake persistent-output publication. */
static enum rptadv_gpio_result publish_result;
/** @brief Result returned by the fake scheduled-pulse publication. */
static enum rptadv_gpio_result schedule_result;

/**
 * @brief Record one persistent logical CM119 output action without HID I/O.
 * @param device Fake open device passed by the facade.
 * @param action Prepared action to record.
 * @return The scripted adapter result.
 */
static enum rptadv_gpio_result fake_publish_outputs(struct rptadv_gpio_device *device,
						    const struct rptadv_gpio_output_action *action)
{
	assert(device == (struct rptadv_gpio_device *)&fake_device_token);
	assert(action != NULL);
	assert(action->struct_size == sizeof(*action));
	assert(action->abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION);
	publish_calls++;
	published_action = *action;
	return publish_result;
}

/**
 * @brief Record one independently scheduled GPIO pulse without HID I/O.
 * @param device Fake open device passed by the facade.
 * @param action Prepared scheduled pulse action to record.
 * @return The scripted adapter result.
 */
static enum rptadv_gpio_result fake_schedule_inverting_pulse(
	struct rptadv_gpio_device *device,
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action *action)
{
	assert(device == (struct rptadv_gpio_device *)&fake_device_token);
	assert(action != NULL);
	assert(action->struct_size == sizeof(*action));
	assert(action->abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION);
	assert(schedule_calls < sizeof(scheduled_actions) / sizeof(scheduled_actions[0]));
	scheduled_actions[schedule_calls++] = *action;
	return schedule_result;
}

/** @brief Complete fake descriptor with only the surfaces used by this test. */
static struct rptadv_gpio_adapter_descriptor fake_gpio = {
	.struct_size = sizeof(fake_gpio),
	.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	.device_publish_outputs = fake_publish_outputs,
	.device_schedule_inverting_pulse = fake_schedule_inverting_pulse,
};

/**
 * @brief Create one ready facade without probing or opening physical hardware.
 * @return A facade bound to the fake open GPIO device.
 */
static struct usbradioplus_hardware_adapter ready_adapter(void)
{
	return (struct usbradioplus_hardware_adapter){
		.gpio = &fake_gpio,
		.gpio_device = (struct rptadv_gpio_device *)&fake_device_token,
	};
}

/** @brief Reset fake publication observations and scripted results. */
static void reset_fake_gpio(void)
{
	publish_calls = 0U;
	schedule_calls = 0U;
	memset(&published_action, 0, sizeof(published_action));
	memset(scheduled_actions, 0, sizeof(scheduled_actions));
	publish_result = RPTADV_GPIO_OK;
	schedule_result = RPTADV_GPIO_OK;
	fake_gpio.struct_size = sizeof(fake_gpio);
}

/** @brief Verify persistent values and each accepted legacy pulse use the facade. */
static void test_persistent_outputs_and_pulses(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {25, 50, 17};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 1U, 0x05U, 0xffU, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0U, 0U, 1000U,
						      10U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(publish_calls == 1U);
	assert(published_action.ptt_asserted == 1U);
	assert(published_action.gpio_output_mask == 0x05U);
	assert(schedule_calls == 2U);
	assert(scheduled_actions[0].gpio_invert_mask == 0x01U);
	assert(scheduled_actions[0].pulse_duration_milliseconds == 25U);
	assert(scheduled_actions[1].gpio_invert_mask == 0x04U);
	assert(scheduled_actions[1].pulse_duration_milliseconds == 17U);
	assert(pulses[0] == 0 && pulses[1] == 0 && pulses[2] == 0);
}

/** \brief Verify an explicit legacy GPIO cancellation reaches the scheduled-pulse facade. */
static void test_explicit_gpio_pulse_cancellation(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {25};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0U, 0U, 1000U,
						      100U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 1U);
	assert(scheduled_actions[0].gpio_invert_mask == 0x01U);

	cancel_mask = 0x01U;
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0U, 0U, 1000U,
						      101U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 2U);
	assert(scheduled_actions[1].gpio_invert_mask == 0U);
	assert(scheduled_actions[1].gpio_cancel_mask == 0x01U);
	assert(scheduled_actions[1].pulse_duration_milliseconds == 0U);
	assert(cancel_mask == 0U);
}

/** @brief Verify a clip indication is held once and is not extended while active. */
static void test_clip_led_does_not_extend_active_hold(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {0};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x04U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x04U, 1U, 1000U,
						      100U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 1U);
	assert(scheduled_actions[0].gpio_invert_mask == 0x04U);
	assert(scheduled_actions[0].pulse_duration_milliseconds == 1000U);
	assert(state.clip_led_deadline_milliseconds == 1100U);

	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x04U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x04U, 1U, 1000U,
						      101U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 1U);
	assert(state.clip_led_deadline_milliseconds == 1100U);

	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x04U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x04U, 1U, 1000U,
						      1100U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 2U);
	assert(state.clip_led_deadline_milliseconds == 2100U);
}

/** @brief Verify an ordinary pulse on the LED pin suppresses a competing clip pulse. */
static void test_clip_led_defers_to_an_existing_gpio_pulse(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {0, 0, 25};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x04U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x04U, 1U, 1000U,
						      100U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 1U);
	assert(scheduled_actions[0].gpio_invert_mask == 0x04U);
	assert(scheduled_actions[0].pulse_duration_milliseconds == 25U);
	assert(state.clip_led_deadline_milliseconds == 0U);

	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x04U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x04U, 1U, 1000U,
						      101U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(schedule_calls == 2U);
	assert(scheduled_actions[1].gpio_invert_mask == 0x04U);
	assert(scheduled_actions[1].pulse_duration_milliseconds == 1000U);
	assert(state.clip_led_deadline_milliseconds == 1101U);
}

/** @brief Verify failed and unavailable scheduled-pulse operations preserve work to retry. */
static void test_pending_pulses_survive_schedule_failure(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {10};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	schedule_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0U, 0U, 1000U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(publish_calls == 1U && schedule_calls == 1U);
	assert(pulses[0] == 10);

	reset_fake_gpio();
	fake_gpio.struct_size =
		offsetof(struct rptadv_gpio_adapter_descriptor, device_schedule_inverting_pulse);
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0U, 0U, 1000U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(pulses[0] == 10);
}

/** @brief Verify invalid clip configuration cannot publish a partially valid action. */
static void test_invalid_clip_configuration_rejected(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_gpio_poc_state state = {0};
	int pulses[USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT] = {0};
	uint32_t cancel_mask = 0U;

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x02U, 1U, 1000U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(publish_calls == 0U && schedule_calls == 0U);

	reset_fake_gpio();
	assert(usbradioplus_hardware_gpio_poc_publish(&adapter, &state, 0U, 0x01U, 0U, pulses,
						      USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT,
						      &cancel_mask, 0x01U, 1U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(publish_calls == 0U && schedule_calls == 0U);
}

/** @brief Run deterministic hardware-adapter POC GPIO regression checks. */
int main(void)
{
	test_persistent_outputs_and_pulses();
	test_explicit_gpio_pulse_cancellation();
	test_clip_led_does_not_extend_active_hold();
	test_clip_led_defers_to_an_existing_gpio_pulse();
	test_pending_pulses_survive_schedule_failure();
	test_invalid_clip_configuration_rejected();
	return 0;
}
