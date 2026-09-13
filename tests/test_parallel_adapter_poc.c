/**
 * @file
 * @brief Fake-facade coverage for optional parallel transport migration.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usbradioplus_parallel_adapter_poc.h"

/** @brief Deterministic fake parallel-port state. */
struct fake_parallel {
	int open_calls;
	int close_calls;
	int service_calls;
	int publish_calls;
	int pulse_calls;
	int binary_calls;
	int rtx_calls;
	int clear_calls;
	enum rptadv_gpio_result open_result;
	enum rptadv_gpio_result publish_result;
	enum rptadv_gpio_result service_result;
	enum rptadv_gpio_result binary_result;
	enum rptadv_gpio_result rtx_result;
	enum rptadv_gpio_result clear_result;
	struct rptadv_gpio_parallel_config config;
	struct rptadv_gpio_parallel_output_action output;
	struct rptadv_gpio_parallel_scheduled_inverting_pulse_action pulse;
	struct rptadv_gpio_parallel_input_snapshot inputs;
	struct rptadv_gpio_parallel_stats stats;
	uint8_t binary_channel;
	uint32_t rtx_rx;
	uint32_t rtx_tx;
	uint32_t rtx_transmitting;
	uint32_t rtx_high_power;
	/** Last mutable opaque device token supplied by the descriptor boundary. */
	struct rptadv_gpio_parallel_device *last_device;
};

/** @brief Single fake opaque adapter device. */
static unsigned char fake_parallel_device;
/** @brief Mutable state used by every fake descriptor entry point. */
static struct fake_parallel fake;

/** @brief Reset fake state to a healthy online parallel transport. */
static void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.open_result = RPTADV_GPIO_OK;
	fake.publish_result = RPTADV_GPIO_OK;
	fake.service_result = RPTADV_GPIO_OK;
	fake.binary_result = RPTADV_GPIO_OK;
	fake.rtx_result = RPTADV_GPIO_OK;
	fake.clear_result = RPTADV_GPIO_OK;
	fake.inputs.struct_size = sizeof(fake.inputs);
	fake.inputs.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	fake.inputs.online = 1U;
	fake.stats.struct_size = sizeof(fake.stats);
	fake.stats.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	fake.stats.online = 1U;
}

/** @brief Fake exclusive transport open. */
static enum rptadv_gpio_result fake_parallel_open(const struct rptadv_gpio_parallel_config *config,
						  struct rptadv_gpio_parallel_device **device)
{
	++fake.open_calls;
	fake.config = *config;
	if (fake.open_result != RPTADV_GPIO_OK)
		return fake.open_result;
	*device = (struct rptadv_gpio_parallel_device *)&fake_parallel_device;
	return RPTADV_GPIO_OK;
}

/** @brief Preserve the mutable opaque token required by descriptor callbacks. */
static void fake_capture_device(struct rptadv_gpio_parallel_device *device)
{
	assert(device == (struct rptadv_gpio_parallel_device *)&fake_parallel_device);
	fake.last_device = device;
}

/** @brief Fake lock-free output publication. */
static enum rptadv_gpio_result
fake_parallel_publish(struct rptadv_gpio_parallel_device *device,
		      const struct rptadv_gpio_parallel_output_action *action)
{
	fake_capture_device(device);
	++fake.publish_calls;
	fake.output = *action;
	return fake.publish_result;
}

/** @brief Fake non-real-time transport service. */
static enum rptadv_gpio_result fake_parallel_service(struct rptadv_gpio_parallel_device *device)
{
	fake_capture_device(device);
	++fake.service_calls;
	if (fake.publish_calls)
		fake.stats.applied_output_mask = fake.output.output_mask;
	return fake.service_result;
}

/** @brief Return one scripted raw status byte. */
static enum rptadv_gpio_result
fake_parallel_get_inputs(const struct rptadv_gpio_parallel_device *device,
			 struct rptadv_gpio_parallel_input_snapshot *inputs)
{
	assert(device == (const struct rptadv_gpio_parallel_device *)&fake_parallel_device);
	*inputs = fake.inputs;
	return RPTADV_GPIO_OK;
}

/** @brief Return scripted adapter-owned output observations. */
static enum rptadv_gpio_result
fake_parallel_get_stats(const struct rptadv_gpio_parallel_device *device,
			struct rptadv_gpio_parallel_stats *stats)
{
	assert(device == (const struct rptadv_gpio_parallel_device *)&fake_parallel_device);
	*stats = fake.stats;
	return RPTADV_GPIO_OK;
}

/** @brief Fake physical transport close. */
static void fake_parallel_close(struct rptadv_gpio_parallel_device *device)
{
	fake_capture_device(device);
	++fake.close_calls;
}

/** @brief Capture independently scheduled compatibility pulses. */
static enum rptadv_gpio_result fake_parallel_schedule_pulse(
	struct rptadv_gpio_parallel_device *device,
	const struct rptadv_gpio_parallel_scheduled_inverting_pulse_action *action)
{
	fake_capture_device(device);
	++fake.pulse_calls;
	fake.pulse = *action;
	return RPTADV_GPIO_OK;
}

/** @brief Capture serialized active-low channel requests. */
static enum rptadv_gpio_result fake_parallel_set_binary(struct rptadv_gpio_parallel_device *device,
							uint8_t channel)
{
	fake_capture_device(device);
	++fake.binary_calls;
	fake.binary_channel = channel;
	return fake.binary_result;
}

/** @brief Capture one serialized RTX programming operation. */
static enum rptadv_gpio_result fake_parallel_program_rtx(struct rptadv_gpio_parallel_device *device,
							 uint32_t rx_frequency_hz,
							 uint32_t tx_frequency_hz,
							 uint32_t transmitting, uint32_t high_power)
{
	fake_capture_device(device);
	++fake.rtx_calls;
	fake.rtx_rx = rx_frequency_hz;
	fake.rtx_tx = tx_frequency_hz;
	fake.rtx_transmitting = transmitting;
	fake.rtx_high_power = high_power;
	return fake.rtx_result;
}

/** @brief Capture one immediate RTX transmitter release. */
static enum rptadv_gpio_result fake_parallel_clear_rtx(struct rptadv_gpio_parallel_device *device)
{
	fake_capture_device(device);
	++fake.clear_calls;
	return fake.clear_result;
}

/** @brief Minimal released descriptor exposing every migrated parallel primitive. */
static const struct rptadv_gpio_adapter_descriptor fake_descriptor = {
	.struct_size = sizeof(fake_descriptor),
	.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	.parallel_open = fake_parallel_open,
	.parallel_publish_outputs = fake_parallel_publish,
	.parallel_service = fake_parallel_service,
	.parallel_get_inputs = fake_parallel_get_inputs,
	.parallel_get_stats = fake_parallel_get_stats,
	.parallel_close = fake_parallel_close,
	.parallel_schedule_inverting_pulse = fake_parallel_schedule_pulse,
	.parallel_set_binary_channel = fake_parallel_set_binary,
	.parallel_program_rtx = fake_parallel_program_rtx,
	.parallel_clear_rtx_transmit = fake_parallel_clear_rtx,
};

/** @brief Satisfy the facade's released-descriptor lookup without loading hardware. */
const struct rptadv_gpio_adapter_descriptor *rptadv_gpio_adapter_descriptor(void)
{
	return &fake_descriptor;
}

/** @brief Satisfy the unrelated audio lookup retained by facade setup helpers. */
const struct rptadv_audio_adapter_descriptor *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return NULL;
}

/** @brief Initialize a facade with the fake GPIO descriptor. */
static struct usbradioplus_hardware_adapter fake_adapter(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};

	adapter.gpio = &fake_descriptor;
	return adapter;
}

/** @brief Verify absent, malformed, and both legacy transport selections. */
static void test_open_contract(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 0, NULL, 0U, 0x5aU) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(!usbradioplus_parallel_adapter_poc_is_open(&state));
	assert(fake.open_calls == 0);
	assert(usbradioplus_parallel_adapter_poc_publish_output(&state, &adapter, 0x21U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 3, "/dev/parport0", 0U,
						      0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, NULL, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "", 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 2, NULL, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(fake.open_calls == 0);

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      0x42U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_parallel_adapter_poc_is_open(&state));
	assert(fake.open_calls == 1);
	assert(fake.config.transport == RPTADV_GPIO_PARALLEL_TRANSPORT_PPDEV);
	assert(!strcmp(fake.config.ppdev_path, "/dev/parport0"));
	assert(fake.config.output_enable_mask == UINT32_C(0xff));
	assert(fake.config.output_initial_mask == UINT32_C(0x42));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0x42));
	usbradioplus_hardware_adapter_close(&adapter);
	assert(fake.close_calls == 1);
	usbradioplus_parallel_adapter_poc_reset(&state);
	assert(!usbradioplus_parallel_adapter_poc_is_open(&state));

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 2, NULL, 0x378U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.config.transport == RPTADV_GPIO_PARALLEL_TRANSPORT_RAW_IO);
	assert(fake.config.raw_io_base == 0x378U);
}

/** @brief Verify failures latch and no partial ownership survives open rejection. */
static void test_open_and_publish_failures(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;

	fake_reset();
	fake.open_result = RPTADV_GPIO_IO_ERROR;
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(!usbradioplus_parallel_adapter_poc_is_open(&state));
	assert(!adapter.parallel_device);

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      0U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	fake.publish_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_parallel_adapter_poc_publish_output(&state, &adapter, 0x80U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(atomic_load_explicit(&state.faulted, memory_order_acquire));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == 0U);
}

/** @brief Verify service owns serialized programming, PTT, and status sampling. */
static void test_service_and_pulses(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;
	struct usbradioplus_parallel_adapter_poc_service_request request = {
		.have_program = 1,
		.program_generation = 4U,
		.rx_frequency_hz = 146520000U,
		.tx_frequency_hz = 146940000U,
		.high_power = 1,
		.ptt_asserted = 1,
		.ptt_inverted = 0,
		.ptt_mask = UINT8_C(0x04),
	};
	struct rptadv_gpio_parallel_input_snapshot inputs = {
		.struct_size = sizeof(inputs),
	};
	struct rptadv_gpio_parallel_stats stats = {
		.struct_size = sizeof(stats),
	};
	uint8_t output = UINT8_C(0x20);

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      output) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	usbradioplus_parallel_adapter_poc_request_binary_channel(&state, UINT8_C(0x0d));
	assert(usbradioplus_parallel_adapter_poc_schedule_pulse(&state, &adapter, UINT8_C(0x02),
								17U, UINT8_C(0x80)) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.pulse_calls == 1);
	assert(fake.pulse.invert_mask == UINT32_C(0x02));
	assert(fake.pulse.pulse_duration_milliseconds == 17U);
	assert(fake.pulse.cancel_mask == UINT32_C(0x80));
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, &inputs, &stats) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.binary_calls == 1 && fake.binary_channel == UINT8_C(0x0d));
	assert(fake.rtx_calls == 1 && fake.rtx_rx == 146520000U && fake.rtx_tx == 146940000U);
	assert(fake.rtx_transmitting == 1U && fake.rtx_high_power == 1U);
	assert(fake.publish_calls == 1 && fake.output.output_mask == UINT32_C(0x2c));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0x2c));
	assert(fake.service_calls == 1 && output == UINT8_C(0x2c));
	request.ptt_asserted = 0;
	request.have_program = 0;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, &inputs, &stats) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.clear_calls == 1);
	assert((output & UINT8_C(0x04)) == 0U);

	fake.service_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, &inputs, &stats) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(atomic_load_explicit(&state.faulted, memory_order_acquire));
}

/** @brief Verify baseline protocol state survives pulse and physical-output observation. */
static void test_baseline_protocol_state(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;
	struct usbradioplus_parallel_adapter_poc_service_request request = {
		.ptt_mask = UINT8_C(0x02),
	};
	uint8_t output = UINT8_C(0x20);

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      output) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	/* Legacy startup begins unkeyed, so an idle first service does not rewrite output. */
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.publish_calls == 0);
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0x20));

	usbradioplus_parallel_adapter_poc_request_binary_channel(&state, UINT8_C(0x05));
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.binary_channel == UINT8_C(0x05));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xa0));

	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.output.output_mask == UINT32_C(0xa2));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xa2));
	request.ptt_asserted = 0;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.clear_calls == 0);
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xa0));

	request.have_program = 1;
	request.program_generation = 1U;
	request.rx_frequency_hz = 146520000U;
	request.tx_frequency_hz = 146940000U;
	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 1 && fake.rtx_transmitting == 1U);
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xaa));
	request.ptt_asserted = 0;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 2 && fake.rtx_transmitting == 0U);
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xa0));

	/* A zero RX frequency remains a legacy programming no-op, but teardown still clears TX. */
	request.program_generation = 2U;
	request.rx_frequency_hz = 0U;
	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 2);
	request.ptt_asserted = 0;
	request.force_unkey = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.clear_calls == 1);
	assert(usbradioplus_parallel_adapter_poc_persistent_output(&state) == UINT8_C(0xa0));
	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
}

/** @brief Verify a serialized binary-channel set failure becomes a visible fault. */
static void test_binary_set_failure(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;
	struct usbradioplus_parallel_adapter_poc_service_request request = {0};
	uint8_t output = 0U;

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      0U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	fake.binary_result = RPTADV_GPIO_IO_ERROR;
	usbradioplus_parallel_adapter_poc_request_binary_channel(&state, UINT8_C(0x01));
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &output, NULL, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(fake.binary_calls == 1);
	assert(atomic_load_explicit(&state.faulted, memory_order_acquire));
}

/** @brief Keep independent node PTT masks and programming generations on one port. */
static void test_shared_channel_service(void)
{
	struct usbradioplus_parallel_adapter_poc_state owner, peer;
	struct usbradioplus_hardware_adapter adapter;
	struct usbradioplus_parallel_adapter_poc_service_request request = {
		.ptt_asserted = 1,
		.ptt_mask = UINT8_C(0x04),
	};
	uint8_t output = 0U;

	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&owner);
	usbradioplus_parallel_adapter_poc_init(&peer);
	assert(usbradioplus_parallel_adapter_poc_open(&owner, &adapter, 1, "/dev/parport0", 0U,
						      UINT8_C(0x10)) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &owner, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(output == UINT8_C(0x14));
	request.ptt_mask = UINT8_C(0x80);
	request.ptt_asserted = 0;
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &peer, &request, &output,
							 NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(output == UINT8_C(0x14) && fake.clear_calls == 0);
	assert(owner.last_ptt_asserted == 1 && peer.last_ptt_asserted == 0);
	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &peer, &request, &output,
							 NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(output == UINT8_C(0x94));
	request.ptt_asserted = 0;
	request.force_unkey = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &peer, &request, &output,
							 NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(output == UINT8_C(0x14) && fake.clear_calls == 0);
	request.force_unkey = 0;
	request.have_program = 1;
	request.program_generation = 4U;
	request.rx_frequency_hz = 146520000U;
	request.tx_frequency_hz = 146940000U;
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &owner, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 1);
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &peer, &request, &output,
							 NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 2);
	assert(usbradioplus_parallel_adapter_poc_service(&owner, &adapter, &owner, &request,
							 &output, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 2);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Transfer pending shared state without resetting the recipient or a live port. */
static void test_shared_transport_transfer(void)
{
	struct usbradioplus_parallel_adapter_poc_state owner, peer;
	struct usbradioplus_hardware_adapter adapter, next_adapter;

	fake_reset();
	adapter = fake_adapter();
	next_adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&owner);
	usbradioplus_parallel_adapter_poc_init(&peer);
	assert(!usbradioplus_parallel_adapter_poc_transfer(&owner, &adapter, &peer, &next_adapter));
	assert(usbradioplus_parallel_adapter_poc_open(&owner, &adapter, 1, "/dev/parport0", 0U,
						      UINT8_C(0x5a)) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	owner.program_generation = 17U;
	peer.program_generation = 9U;
	peer.last_ptt_asserted = 1;
	usbradioplus_parallel_adapter_poc_request_binary_channel(&owner, UINT8_C(0x03));
	assert(usbradioplus_parallel_adapter_poc_schedule_pulse(&owner, &adapter, 1U, 17U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_parallel_adapter_poc_transfer(&owner, &adapter, &peer, &next_adapter));
	assert(!owner.opened && !adapter.parallel_device);
	assert(peer.opened && next_adapter.parallel_device);
	assert(peer.persistent_output == UINT8_C(0x5a));
	assert(peer.program_generation == 9U && peer.last_ptt_asserted == 1);
	assert(atomic_load(&peer.binary_channel) == 3U &&
	       atomic_load(&peer.binary_generation) == 1U);
	assert(peer.serviced_binary_generation == 0U);
	assert(fake.open_calls == 1 && fake.close_calls == 0 && fake.pulse_calls == 1);
	usbradioplus_hardware_adapter_close(&adapter);
	assert(fake.close_calls == 0);
	atomic_store(&peer.faulted, 1);
	assert(!usbradioplus_parallel_adapter_poc_transfer(&peer, &next_adapter, &owner, &adapter));
	assert(peer.opened && next_adapter.parallel_device && !adapter.parallel_device);
	usbradioplus_hardware_adapter_close(&next_adapter);
	assert(fake.close_calls == 1);
	usbradioplus_parallel_adapter_poc_reset(&peer);
	next_adapter = fake_adapter();
	assert(usbradioplus_parallel_adapter_poc_open(&peer, &next_adapter, 1, "/dev/parport0", 0U,
						      UINT8_C(0x5a)) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(!atomic_load(&peer.faulted) && fake.open_calls == 2);
	usbradioplus_hardware_adapter_close(&next_adapter);
}

/** @brief Capture translated text-input events. */
struct input_events {
	unsigned int count;
	unsigned int pin;
	int value;
};

/** @brief Save the most recent translated text-input event. */
static void input_event(void *opaque, unsigned int pin, int value)
{
	struct input_events *events = opaque;

	++events->count;
	events->pin = pin;
	events->value = value;
}

/** @brief Verify legacy status inversion, input events, COR, and CTCSS mapping. */
static void test_input_translation(void)
{
	char *assignments[16] = {0};
	struct input_events events = {0};
	int had_input = 0;
	int last_input = 0;
	uint32_t result;

	assignments[10] = "in";
	assignments[12] = "cor";
	assignments[15] = "ctcss";
	/* XOR 0x80 yields set bits 6, 5, and 3 for pins 10, 12, and 15. */
	result = usbradioplus_parallel_adapter_poc_translate_inputs(
		UINT8_C(0xe8), assignments, &had_input, &last_input, input_event, &events);
	assert(result == (USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CARRIER |
			  USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CTCSS));
	assert(events.count == 1U && events.pin == 10U && events.value == 1);
	/* The same status does not produce a duplicate input event. */
	(void)usbradioplus_parallel_adapter_poc_translate_inputs(
		UINT8_C(0xe8), assignments, &had_input, &last_input, input_event, &events);
	assert(events.count == 1U);
}

/** @brief Exercise every nullable public boundary before facade publication. */
static void test_null_and_closed_boundaries(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter = fake_adapter();
	struct usbradioplus_parallel_adapter_poc_service_request request = {0};
	uint8_t applied = 99U;
	unsigned int missing;
	usbradioplus_parallel_adapter_poc_init(NULL);
	usbradioplus_parallel_adapter_poc_request_binary_channel(NULL, 1U);
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(!usbradioplus_parallel_adapter_poc_is_open(NULL));
	assert(usbradioplus_parallel_adapter_poc_persistent_output(NULL) == 0U);
	for (missing = 0U; missing < 2U; ++missing) {
		assert(usbradioplus_parallel_adapter_poc_open(
			       missing ? &state : NULL, missing ? NULL : &adapter, 0, NULL, 0U,
			       0U) == USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
		assert(usbradioplus_parallel_adapter_poc_publish_output(
			       missing ? &state : NULL, missing ? NULL : &adapter, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
		assert(usbradioplus_parallel_adapter_poc_schedule_pulse(
			       missing ? &state : NULL, missing ? NULL : &adapter, 0U, 0U, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	}
	assert(usbradioplus_parallel_adapter_poc_schedule_pulse(&state, &adapter, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	for (missing = 0U; missing < 5U; ++missing)
		assert(usbradioplus_parallel_adapter_poc_service(
			       missing == 0U ? NULL : &state, missing == 1U ? NULL : &adapter,
			       missing == 2U ? NULL : &state, missing == 3U ? NULL : &request,
			       missing == 4U ? NULL : &applied, NULL,
			       NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &applied, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(applied == 99U);
}

/** @brief Verify service errors latch faults and leave requested transitions retryable. */
static void test_service_failure_boundaries(void)
{
	struct usbradioplus_parallel_adapter_poc_state state;
	struct usbradioplus_hardware_adapter adapter;
	struct usbradioplus_parallel_adapter_poc_service_request request;
	uint8_t applied = 0U;
	unsigned int failure;
	for (failure = 0U; failure < 6U; ++failure) {
		fake_reset();
		adapter = fake_adapter();
		usbradioplus_parallel_adapter_poc_init(&state);
		assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0",
							      0U, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
		assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, NULL, 0U, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
		request = (struct usbradioplus_parallel_adapter_poc_service_request){0};
		if (failure == 0U) {
			request.force_unkey = 1;
			request.ptt_asserted = 1;
		} else if (failure == 1U) {
			request.have_program = 1;
			request.rx_frequency_hz = 146520000U;
			request.tx_frequency_hz = 146520000U;
			fake.rtx_result = RPTADV_GPIO_IO_ERROR;
		} else if (failure == 2U || failure == 3U) {
			request.force_unkey = failure == 2U;
			state.program_generation = 0U;
			state.last_ptt_asserted = 1;
			fake.clear_result = RPTADV_GPIO_IO_ERROR;
		} else if (failure == 4U) {
			request.ptt_asserted = 1;
			request.ptt_mask = 1U;
			fake.publish_result = RPTADV_GPIO_IO_ERROR;
		} else {
			atomic_store(&state.faulted, 1);
		}
		assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
								 &applied, NULL, NULL) ==
		       (failure == 0U ? USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT
				      : USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR));
		if (failure != 0U)
			assert(atomic_load(&state.faulted));
		usbradioplus_hardware_adapter_close(&adapter);
	}
	fake_reset();
	adapter = fake_adapter();
	usbradioplus_parallel_adapter_poc_init(&state);
	assert(usbradioplus_parallel_adapter_poc_open(&state, &adapter, 1, "/dev/parport0", 0U,
						      0U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	request = (struct usbradioplus_parallel_adapter_poc_service_request){.have_program = 1};
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &applied, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.rtx_calls == 0 && applied == 0U);
	state.program_generation = 0U;
	state.last_ptt_asserted = 1;
	request.have_program = 0;
	request.ptt_asserted = 1;
	assert(usbradioplus_parallel_adapter_poc_service(&state, &adapter, &state, &request,
							 &applied, NULL,
							 NULL) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake.clear_calls == 0);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Preserve independent input bits, ignored assignments, and optional notifications. */
static void test_input_notification_boundaries(void)
{
	char *assignments[16] = {0};
	int had_input = 0, last_input = 0;
	assert(usbradioplus_parallel_adapter_poc_translate_inputs(0U, NULL, &had_input, &last_input,
								  NULL, NULL) == 0U);
	assert(usbradioplus_parallel_adapter_poc_translate_inputs(0U, assignments, NULL,
								  &last_input, NULL, NULL) == 0U);
	assert(usbradioplus_parallel_adapter_poc_translate_inputs(0U, assignments, &had_input, NULL,
								  NULL, NULL) == 0U);
	assignments[10] = "in";
	assignments[11] = "in";
	assignments[12] = "cor";
	assignments[15] = "ctcss";
	(void)usbradioplus_parallel_adapter_poc_translate_inputs(0x80U, assignments, &had_input,
								 &last_input, NULL, NULL);
	(void)usbradioplus_parallel_adapter_poc_translate_inputs(0xc0U, assignments, &had_input,
								 &last_input, NULL, NULL);
	assert(had_input && last_input == 0x40);
}

int main(void)
{
	test_open_contract();
	test_open_and_publish_failures();
	test_service_and_pulses();
	test_binary_set_failure();
	test_baseline_protocol_state();
	test_shared_channel_service();
	test_shared_transport_transfer();
	test_input_translation();
	test_null_and_closed_boundaries();
	test_service_failure_boundaries();
	test_input_notification_boundaries();
	puts("parallel adapter POC tests passed");
	return 0;
}
