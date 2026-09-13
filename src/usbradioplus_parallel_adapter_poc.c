/**
 * @file
 * @brief Legacy parallel-port semantics over the hardware adapter facade.
 */

#include "usbradioplus_parallel_adapter_poc.h"

#include <limits.h>
#include <stddef.h>
#include <strings.h>

/** @brief Adapter data-register mask covering all legacy parallel outputs. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_OUTPUT_MASK UINT32_C(0xff)

/** @brief Legacy active-low parallel channel-selector data bits. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_BINARY_CHANNEL_MASK UINT32_C(0xf0)

/** @brief Legacy RTX clock, data, enable, transmit, and power data bits. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_RTX_CONTROL_MASK UINT32_C(0x1f)

/** @brief Legacy RTX transmit data bit. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_RTX_TRANSMIT_MASK UINT32_C(0x08)

/** @brief Legacy RTX transmit-power data bit, which remains deasserted. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_RTX_POWER_MASK UINT32_C(0x10)

/** @brief Legacy status-register bit selected by every parallel input pin. */
static const uint8_t parallel_input_shift[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 5, 4, 0, 3};

/** @brief Latch a failed facade publication so the worker fails closed.
 * @param state Validated parallel adapter whose fault latch is updated.
 * @param result Publication result to inspect and retain.
 * @return The supplied publication result.
 */
static enum usbradioplus_hardware_adapter_result
parallel_adapter_result(struct usbradioplus_parallel_adapter_poc_state *state,
			enum usbradioplus_hardware_adapter_result result)
{
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		atomic_store_explicit(&state->faulted, 1, memory_order_release);
	return result;
}

/** @brief Apply configured PTT polarity to one persistent parallel data byte.
 * @param value Current output bits.
 * @param request Requested PTT state, mask, and polarity.
 * @return Output bits with the requested PTT polarity applied.
 */
static uint8_t
parallel_adapter_apply_ptt(uint8_t value,
			   const struct usbradioplus_parallel_adapter_poc_service_request *request)
{
	if (!request->ptt_mask)
		return value;
	if (!!request->ptt_asserted != !!request->ptt_inverted)
		return (uint8_t)(value | request->ptt_mask);
	return (uint8_t)(value & (uint8_t)~request->ptt_mask);
}

void usbradioplus_parallel_adapter_poc_init(struct usbradioplus_parallel_adapter_poc_state *state)
{
	if (!state)
		return;
	state->opened = 0;
	state->last_ptt_asserted = 0;
	state->program_generation = UINT_MAX;
	state->persistent_output = 0U;
	atomic_init(&state->binary_channel, 0U);
	atomic_init(&state->binary_generation, 0U);
	state->serviced_binary_generation = 0U;
	atomic_init(&state->faulted, 0);
}

enum usbradioplus_hardware_adapter_result
usbradioplus_parallel_adapter_poc_open(struct usbradioplus_parallel_adapter_poc_state *state,
				       struct usbradioplus_hardware_adapter *adapter,
				       int legacy_haspp, const char *ppdev_path,
				       uint32_t raw_io_base, uint8_t initial_output)
{
	struct rptadv_gpio_parallel_config config = {
		.struct_size = sizeof(config),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.output_enable_mask = USBRADIOPLUS_PARALLEL_ADAPTER_OUTPUT_MASK,
		.output_initial_mask = initial_output,
	};
	enum usbradioplus_hardware_adapter_result result;

	if (!state || !adapter)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!legacy_haspp)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	if (state->opened)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	if (legacy_haspp == 1) {
		if (!ppdev_path || !*ppdev_path)
			return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
		config.transport = RPTADV_GPIO_PARALLEL_TRANSPORT_PPDEV;
		config.ppdev_path = ppdev_path;
	} else if (legacy_haspp == 2) {
		if (!raw_io_base)
			return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
		config.transport = RPTADV_GPIO_PARALLEL_TRANSPORT_RAW_IO;
		config.raw_io_base = raw_io_base;
	} else {
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	}
	result = usbradioplus_hardware_adapter_open_parallel(adapter, &config);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return result;
	state->opened = 1;
	state->last_ptt_asserted = 0;
	state->program_generation = UINT_MAX;
	state->persistent_output = initial_output;
	state->serviced_binary_generation =
		atomic_load_explicit(&state->binary_generation, memory_order_acquire);
	atomic_store_explicit(&state->faulted, 0, memory_order_release);
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

void usbradioplus_parallel_adapter_poc_reset(struct usbradioplus_parallel_adapter_poc_state *state)
{
	usbradioplus_parallel_adapter_poc_init(state);
}

int usbradioplus_parallel_adapter_poc_transfer(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_parallel_adapter_poc_state *next_state,
	struct usbradioplus_hardware_adapter *next_adapter)
{
	const int last_ptt_asserted = next_state->last_ptt_asserted;
	const uint32_t program_generation = next_state->program_generation;

	if (!state->opened || atomic_load_explicit(&state->faulted, memory_order_acquire))
		return 0;
	*next_state = *state;
	next_state->last_ptt_asserted = last_ptt_asserted;
	next_state->program_generation = program_generation;
	next_adapter->parallel_device = adapter->parallel_device;
	adapter->parallel_device = NULL;
	usbradioplus_parallel_adapter_poc_reset(state);
	return 1;
}

int usbradioplus_parallel_adapter_poc_is_open(
	const struct usbradioplus_parallel_adapter_poc_state *state)
{
	return state && state->opened;
}

uint8_t usbradioplus_parallel_adapter_poc_persistent_output(
	const struct usbradioplus_parallel_adapter_poc_state *state)
{
	return state ? state->persistent_output : 0U;
}

enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_publish_output(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter, uint8_t output)
{
	const struct rptadv_gpio_parallel_output_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.output_mask = output,
	};

	if (!state || !adapter)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!state->opened)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	{
		const enum usbradioplus_hardware_adapter_result result = parallel_adapter_result(
			state, usbradioplus_hardware_adapter_publish_parallel(adapter, &action));

		if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			state->persistent_output = output;
		return result;
	}
}

enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_schedule_pulse(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter, uint8_t invert_mask,
	uint32_t duration_milliseconds, uint8_t cancel_mask)
{
	const struct rptadv_gpio_parallel_scheduled_inverting_pulse_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.invert_mask = invert_mask,
		.pulse_duration_milliseconds = duration_milliseconds,
		.cancel_mask = cancel_mask,
	};

	if (!state || !adapter)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!state->opened)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	return parallel_adapter_result(
		state,
		usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(adapter, &action));
}

void usbradioplus_parallel_adapter_poc_request_binary_channel(
	struct usbradioplus_parallel_adapter_poc_state *state, uint8_t channel)
{
	if (!state)
		return;
	atomic_store_explicit(&state->binary_channel, channel, memory_order_relaxed);
	atomic_fetch_add_explicit(&state->binary_generation, 1U, memory_order_release);
}

enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_service(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_parallel_adapter_poc_state *client_state,
	const struct usbradioplus_parallel_adapter_poc_service_request *request,
	uint8_t *applied_output, struct rptadv_gpio_parallel_input_snapshot *inputs,
	struct rptadv_gpio_parallel_stats *stats)
{
	enum usbradioplus_hardware_adapter_result result;
	uint32_t binary_generation;
	uint8_t output;
	int ptt_changed;

	if (!state || !adapter || !client_state || !request || !applied_output)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!state->opened)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	if (atomic_load_explicit(&state->faulted, memory_order_acquire))
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	binary_generation = atomic_load_explicit(&state->binary_generation, memory_order_acquire);
	if (binary_generation != state->serviced_binary_generation) {
		const uint8_t binary_channel =
			atomic_load_explicit(&state->binary_channel, memory_order_relaxed);
		result = usbradioplus_hardware_adapter_set_parallel_binary_channel(adapter,
										   binary_channel);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return parallel_adapter_result(state, result);
		state->persistent_output |= USBRADIOPLUS_PARALLEL_ADAPTER_BINARY_CHANNEL_MASK;
		state->persistent_output &= (uint8_t)~(uint8_t)(binary_channel << 4U);
		state->serviced_binary_generation = binary_generation;
	}
	if (request->force_unkey && request->ptt_asserted)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	ptt_changed = client_state->last_ptt_asserted != !!request->ptt_asserted;
	if (request->force_unkey && client_state->program_generation != UINT_MAX) {
		result = usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(adapter);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return parallel_adapter_result(state, result);
		state->persistent_output &=
			(uint8_t)~(USBRADIOPLUS_PARALLEL_ADAPTER_RTX_TRANSMIT_MASK |
				   USBRADIOPLUS_PARALLEL_ADAPTER_RTX_POWER_MASK);
	} else if (!request->force_unkey && request->have_program &&
		   (request->program_generation != client_state->program_generation ||
		    ptt_changed)) {
		/* The compatibility implementation leaves an unset receive frequency alone. */
		if (request->rx_frequency_hz) {
			result = usbradioplus_hardware_adapter_program_parallel_rtx(
				adapter, request->rx_frequency_hz, request->tx_frequency_hz,
				(uint32_t)!!request->ptt_asserted, (uint32_t)!!request->high_power);
			if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
				return parallel_adapter_result(state, result);
			state->persistent_output &=
				(uint8_t)~USBRADIOPLUS_PARALLEL_ADAPTER_RTX_CONTROL_MASK;
			if (request->ptt_asserted)
				state->persistent_output |=
					USBRADIOPLUS_PARALLEL_ADAPTER_RTX_TRANSMIT_MASK;
			client_state->program_generation = request->program_generation;
		}
	} else if (client_state->program_generation != UINT_MAX &&
		   client_state->last_ptt_asserted > 0 && !request->ptt_asserted) {
		result = usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(adapter);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return parallel_adapter_result(state, result);
		state->persistent_output &=
			(uint8_t)~(USBRADIOPLUS_PARALLEL_ADAPTER_RTX_TRANSMIT_MASK |
				   USBRADIOPLUS_PARALLEL_ADAPTER_RTX_POWER_MASK);
	}
	output = parallel_adapter_apply_ptt(state->persistent_output, request);
	if (output != state->persistent_output) {
		result = usbradioplus_parallel_adapter_poc_publish_output(state, adapter, output);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	result = usbradioplus_hardware_adapter_service_parallel(adapter, inputs, stats);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return parallel_adapter_result(state, result);
	if (stats)
		*applied_output = (uint8_t)stats->applied_output_mask;
	else
		*applied_output = state->persistent_output;
	client_state->last_ptt_asserted = !!request->ptt_asserted;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

uint32_t usbradioplus_parallel_adapter_poc_translate_inputs(
	uint8_t status_mask, char *const assignments[16], int *had_input, int *last_input,
	usbradioplus_parallel_adapter_poc_input_callback callback, void *opaque)
{
	const uint8_t raw = (uint8_t)(status_mask ^ UINT8_C(0x80));
	uint8_t reported = raw;
	uint32_t result = 0U;
	int pin;

	if (!assignments || !had_input || !last_input)
		return 0U;
	for (pin = 10; pin <= 15; ++pin) {
		if (assignments[pin] && !strcasecmp(assignments[pin], "in"))
			continue;
		reported &= (uint8_t)~(UINT8_C(1) << parallel_input_shift[pin]);
	}
	if (!*had_input || *last_input != reported) {
		for (pin = 10; pin <= 15; ++pin) {
			uint8_t bit;

			if (!assignments[pin] || strcasecmp(assignments[pin], "in"))
				continue;
			bit = (uint8_t)(UINT8_C(1) << parallel_input_shift[pin]);
			if (!*had_input || ((*last_input & bit) != (reported & bit))) {
				if (callback)
					callback(opaque, (unsigned int)pin, !!(reported & bit));
			}
		}
		*last_input = reported;
		*had_input = 1;
	}
	for (pin = 10; pin <= 15; ++pin) {
		const uint8_t bit = (uint8_t)(UINT8_C(1) << parallel_input_shift[pin]);

		if (assignments[pin] && !strcasecmp(assignments[pin], "cor") && (raw & bit))
			result |= USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CARRIER;
		else if (assignments[pin] && !strcasecmp(assignments[pin], "ctcss") && (raw & bit))
			result |= USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CTCSS;
	}
	return result;
}
