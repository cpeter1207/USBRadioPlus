/**
 * @file
 * @brief USBRadioPlus hardware GPIO POC.
 *
 * Legacy CM119 GPIO semantics for the opt-in hardware-adapter proof.
 */

#include "usbradioplus_hardware_gpio_poc.h"

/**
 * \brief Return whether a retained monotonic deadline has elapsed.
 * \param now_milliseconds Current monotonic time.
 * \param deadline_milliseconds Previously retained deadline, or zero when idle.
 * \return Nonzero when a new clip pulse may be scheduled.
 */
static int hardware_gpio_poc_deadline_reached(uint64_t now_milliseconds,
					      uint64_t deadline_milliseconds)
{
	return deadline_milliseconds == 0U ||
	       (int64_t)(now_milliseconds - deadline_milliseconds) >= 0;
}

/**
 * \brief Schedule one accepted legacy GPIO baseline-XOR pulse.
 * \param adapter Prepared facade with an open GPIO device.
 * \param mask One or more configured ordinary GPIO bits to invert.
 * \param duration_milliseconds Pulse duration in milliseconds.
 * \return A facade result code.
 */
static enum usbradioplus_hardware_adapter_result
hardware_gpio_poc_schedule(struct usbradioplus_hardware_adapter *adapter, uint32_t mask,
			   uint32_t duration_milliseconds)
{
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.gpio_invert_mask = mask,
		.pulse_duration_milliseconds = duration_milliseconds,
	};

	return usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(adapter, &action);
}

/**
 * \brief Cancel selected legacy GPIO baseline-XOR pulses.
 * \param adapter Prepared facade with an open GPIO device.
 * \param mask Configured ordinary GPIO bits whose pulses are cancelled.
 * \return A facade result code.
 */
static enum usbradioplus_hardware_adapter_result
hardware_gpio_poc_cancel(struct usbradioplus_hardware_adapter *adapter, uint32_t mask)
{
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.gpio_cancel_mask = mask,
	};

	return usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(adapter, &action);
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_gpio_poc_publish(
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_hardware_gpio_poc_state *state, uint32_t ptt_asserted,
	uint32_t output_enable_mask, uint32_t output_mask, int *pulse_durations_milliseconds,
	size_t pulse_count, uint32_t *cancel_mask, uint32_t clip_led_mask,
	uint32_t clip_led_requested, uint32_t clip_hold_milliseconds,
	uint64_t monotonic_milliseconds)
{
	const struct rptadv_gpio_output_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.ptt_asserted = !!ptt_asserted,
		.gpio_output_mask = output_mask & output_enable_mask,
	};
	enum usbradioplus_hardware_adapter_result result;
	size_t pin;
	int clip_pulse_pending = 0;

	if (!adapter || !state || !pulse_durations_milliseconds || !cancel_mask ||
	    pulse_count != USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT ||
	    (*cancel_mask & ~output_enable_mask) != 0U ||
	    (clip_led_mask & ~output_enable_mask) != 0U ||
	    (clip_led_mask != 0U && (clip_led_mask & (clip_led_mask - 1U)) != 0U) ||
	    (clip_led_requested && clip_led_mask != 0U && clip_hold_milliseconds == 0U))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;

	for (pin = 0U; pin < pulse_count; ++pin) {
		const uint32_t pin_mask = UINT32_C(1) << pin;

		if (pulse_durations_milliseconds[pin] > 0 && (clip_led_mask & pin_mask) != 0U)
			clip_pulse_pending = 1;
	}
	result = usbradioplus_hardware_adapter_publish_gpio(adapter, &action);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return result;
	if (*cancel_mask != 0U) {
		result = hardware_gpio_poc_cancel(adapter, *cancel_mask);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
		if (clip_led_mask != 0U && (*cancel_mask & clip_led_mask) != 0U)
			state->clip_led_deadline_milliseconds = 0U;
		*cancel_mask = 0U;
	}

	for (pin = 0U; pin < pulse_count; ++pin) {
		const uint32_t pin_mask = UINT32_C(1) << pin;
		const int duration = pulse_durations_milliseconds[pin];

		if (duration <= 0)
			continue;
		/* Legacy accepts a pulse command for a valid input pin but cannot drive
		 * it. Retire that inert request here rather than changing another pin. */
		if ((pin_mask & output_enable_mask) == 0U) {
			pulse_durations_milliseconds[pin] = 0;
			continue;
		}
		result = hardware_gpio_poc_schedule(adapter, pin_mask, (uint32_t)duration);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
		pulse_durations_milliseconds[pin] = 0;
	}

	if (clip_led_requested && clip_led_mask != 0U && !clip_pulse_pending &&
	    hardware_gpio_poc_deadline_reached(monotonic_milliseconds,
					       state->clip_led_deadline_milliseconds)) {
		if (clip_hold_milliseconds == 0U)
			return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
		result = hardware_gpio_poc_schedule(adapter, clip_led_mask, clip_hold_milliseconds);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
		state->clip_led_deadline_milliseconds =
			monotonic_milliseconds + (uint64_t)clip_hold_milliseconds;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}
