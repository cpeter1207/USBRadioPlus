/**
 * @file
 * @brief USBRadioPlus hardware GPIO POC API.
 *
 * Legacy CM119 GPIO semantics for the opt-in hardware-adapter proof.
 *
 * The normal ASL HID owner retains its existing GPIO implementation.  This
 * small bridge exists only while the direct PortAudio/CM119 proof is selected:
 * it converts the legacy persistent-output and pulse state to the released
 * hardware facade without putting HID I/O in the audio callback.
 */

#ifndef USBRADIOPLUS_HARDWARE_GPIO_POC_H
#define USBRADIOPLUS_HARDWARE_GPIO_POC_H

#include <stddef.h>
#include <stdint.h>

#include "usbradioplus_hardware_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Number of legacy CM119 GPIO pulse slots. */
#define USBRADIOPLUS_HARDWARE_GPIO_POC_PIN_COUNT 8U

/** \brief Per-channel state retained by the opt-in GPIO proof. */
struct usbradioplus_hardware_gpio_poc_state {
	/** Monotonic millisecond deadline that suppresses repeated clip requests. */
	uint64_t clip_led_deadline_milliseconds;
};

/**
 * \brief Publish one legacy-compatible CM119 GPIO output update through the facade.
 * \param adapter Prepared facade with an open CM119 GPIO device.
 * \param state Per-channel POC state retained across service cycles.
 * \param ptt_asserted Current logical PTT request.
 * \param output_enable_mask Configured ordinary output-bit mask.
 * \param output_mask Current persistent ordinary output values.
 * \param pulse_durations_milliseconds Mutable per-bit legacy pulse durations.
 * \param pulse_count Number of entries in \p pulse_durations_milliseconds.
 * \param cancel_mask Mutable mask of explicitly cancelled legacy pulse pins.
 * \param clip_led_mask Configured clip-indicator bit, or zero when disabled.
 * \param clip_led_requested Nonzero when the audio path observed clipping.
 * \param clip_hold_milliseconds Legacy clip-indicator hold duration.
 * \param monotonic_milliseconds Current monotonic service time in milliseconds.
 * \return A facade result code.
 *
 * Persistent GPIO values are published on every service cycle, exactly as the
 * legacy HID owner does.  A positive per-bit pulse duration is transferred to
 * the adapter's independent baseline-XOR scheduler and cleared only after it
 * is accepted.  Clip requests use the same scheduler but retain the legacy
 * rule that an active clip indication is not extended by another request.
 * This function performs publication only; its caller invokes the facade's
 * HID service operation separately on the non-real-time owner thread.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_gpio_poc_publish(
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_hardware_gpio_poc_state *state, uint32_t ptt_asserted,
	uint32_t output_enable_mask, uint32_t output_mask, int *pulse_durations_milliseconds,
	size_t pulse_count, uint32_t *cancel_mask, uint32_t clip_led_mask,
	uint32_t clip_led_requested, uint32_t clip_hold_milliseconds,
	uint64_t monotonic_milliseconds);

#ifdef __cplusplus
}
#endif

#endif
