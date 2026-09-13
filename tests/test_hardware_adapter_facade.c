/**
 * @file test_hardware_adapter_facade.c
 * @brief No-hardware tests for the released-hardware composition facade.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>
#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#include "usbradioplus_hardware_adapter.h"

#include "hardware_adapter_fixture.h"

/** @brief Satisfy the facade's released-descriptor references without loading hardware. */
const struct rptadv_audio_adapter_descriptor *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return &fake_audio;
}

/** @brief Satisfy the facade's released-descriptor references without loading hardware. */
const struct rptadv_gpio_adapter_descriptor *rptadv_gpio_adapter_descriptor(void)
{
	return &fake_gpio;
}

/** @brief Verify a legacy identifier resolves once and binds both adapters to it. */
static void test_exact_identity_resolution(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(selector_calls == 1U);
	assert(gpio_probe_calls == 1U);
	assert(selector_request.selection_policy == RPTADV_AUDIO_USB_SELECTION_EXACT);
	assert(!strcmp(selector_request.device_identifier, "hw:4,0"));
	assert(!strcmp(selector_request.usb_serial, "CM119-A"));
	assert(!strcmp(adapter.usb_port_path, selector_topology));
	assert(!strcmp(adapter.usb_serial, selector_serial));
	assert(adapter.audio_selection.input_device_index == 6);
	assert(adapter.audio_selection.output_device_index == 7);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify the normal facade HID service lifecycle without a CM119 device. */
static void test_gpio_service_lifecycle(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_gpio_output_action action = {
		.struct_size = sizeof(action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.ptt_asserted = 1U,
	};
	struct rptadv_gpio_input_snapshot inputs = {
		.struct_size = sizeof(inputs),
	};
	struct rptadv_gpio_device_stats stats = {
		.struct_size = sizeof(stats),
	};

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_open_gpio(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_open_calls == 1U);
	assert(usbradioplus_hardware_adapter_publish_gpio(&adapter, &action) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_publish_calls == 1U);
	assert(gpio_published_action.ptt_asserted == 1U);
	assert(usbradioplus_hardware_adapter_service_gpio(&adapter, &inputs, &stats) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_service_calls == 1U);
	assert(inputs.cor_active == 1U && inputs.ctcss_active == 1U);
	assert(stats.ptt_applied == 1U && stats.online == 1U);
	usbradioplus_hardware_adapter_close(&adapter);
	assert(gpio_close_calls == 1U);
}

/** @brief Verify EEPROM I/O retains the GPIO adapter's physical image contract. */
static void test_eeprom_lifecycle(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_gpio_eeprom_image image = {
		.struct_size = sizeof(image),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	};

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_read_eeprom(NULL, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(!gpio_eeprom_read_calls);
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_read_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_adapter_open_gpio(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_read_eeprom(&adapter, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	image.struct_size = sizeof(image) - 1U;
	assert(usbradioplus_hardware_adapter_read_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(!gpio_eeprom_read_calls);
	image.struct_size = sizeof(image);
	gpio_eeprom_read_image.words[RPTADV_GPIO_CM119_EEPROM_START_WORD + 2U] = 912U;
	assert(usbradioplus_hardware_adapter_read_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_eeprom_read_calls == 1U);
	assert(gpio_eeprom_read_request_struct_size == sizeof(image));
	assert(image.abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION);
	assert(image.magic_valid == 1U && image.checksum_valid == 1U);
	assert(image.words[RPTADV_GPIO_CM119_EEPROM_START_WORD + 2U] == 912U);
	gpio_eeprom_read_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_hardware_adapter_read_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(gpio_eeprom_read_calls == 2U);

	image.struct_size = sizeof(image);
	image.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	image.magic_valid = 0U;
	image.checksum_valid = 0U;
	image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] = RPTADV_GPIO_CM119_EEPROM_MAGIC;
	assert(usbradioplus_hardware_adapter_write_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_eeprom_write_calls == 1U);
	assert(gpio_eeprom_written_image.magic_valid == 0U &&
	       gpio_eeprom_written_image.checksum_valid == 0U);
	assert(gpio_eeprom_written_image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] ==
	       RPTADV_GPIO_CM119_EEPROM_MAGIC);
	assert(image.magic_valid == 1U && image.checksum_valid == 1U);
	image.abi_version++;
	assert(usbradioplus_hardware_adapter_write_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(gpio_eeprom_write_calls == 1U);
	image.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	image.struct_size = sizeof(image) - 1U;
	assert(usbradioplus_hardware_adapter_write_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(gpio_eeprom_write_calls == 1U);
	image.struct_size = sizeof(image);
	gpio_eeprom_write_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_hardware_adapter_write_eeprom(&adapter, &image) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(gpio_eeprom_write_calls == 2U);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify a pre-resolved topology uses the same audio-to-GPIO handoff. */
static void test_topology_identity_resolution(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();

	reset_fake_adapters();
	config.device_identifier = NULL;
	config.usb_port_path = "2-4:1.0";
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(selector_calls == 1U);
	assert(!strcmp(selector_request.device_identifier, "2-4:1.0"));
	assert(gpio_probe_calls == 1U);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify explicit automatic selection cannot accidentally inherit an identity. */
static void test_automatic_identity_resolution(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();

	reset_fake_adapters();
	config.device_selection_policy = RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD;
	config.device_identifier = NULL;
	config.usb_serial = NULL;
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(selector_calls == 1U);
	assert(selector_request.selection_policy ==
	       RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD);
	assert(selector_request.device_identifier == NULL);
	assert(selector_request.usb_serial == NULL);
	assert(gpio_probe_calls == 1U);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify semantic CM119 mixer discovery follows the resolved topology. */
static void test_cm119_mixer_path_discovery(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_audio_cm119_mixer_paths paths = {.struct_size = sizeof(paths)};
	struct rptadv_audio_adapter_descriptor old_audio = fake_audio;
	struct rptadv_audio_adapter_descriptor incomplete_audio = fake_audio;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer_paths_calls == 1U);
	assert(!strcmp(mixer_paths_topology, selector_topology));
	assert(paths.abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION);
	assert(paths.rx_capture_path_count == 1U);
	assert(paths.tx_playback_path_count == 1U);
	assert(paths.sidetone_path_count == 1U);
	assert(paths.rx_compatibility_switch_path_count == 1U);
	assert(!strcmp(paths.rx_capture_paths[0].element, "Mic"));
	assert(paths.rx_capture_paths[0].direction == RPTADV_AUDIO_MIXER_CAPTURE);
	assert(!strcmp(paths.tx_playback_paths[0].element, "Speaker"));
	assert(paths.tx_playback_paths[0].direction == RPTADV_AUDIO_MIXER_PLAYBACK);
	assert((paths.rx_compatibility_switch_paths[0].capabilities &
		RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH) != 0U);

	paths.struct_size = 0U;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(mixer_paths_calls == 1U);
	paths.struct_size = sizeof(paths);
	mixer_paths_result = RPTADV_AUDIO_ALSA_ERROR;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	mixer_paths_result = RPTADV_AUDIO_OK;
	mixer_paths_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION + 1U;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	mixer_paths_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	mixer_paths_malformed_mode = 1U;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	mixer_paths_malformed_mode = 2U;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);

	old_audio.struct_size =
		offsetof(struct rptadv_audio_adapter_descriptor, cm119_mixer_paths_resolve);
	assert(usbradioplus_hardware_adapter_validate(&old_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	adapter.audio = &old_audio;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	incomplete_audio.cm119_mixer_paths_resolve = NULL;
	adapter.audio = &incomplete_audio;
	assert(usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	adapter.audio = &fake_audio;
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify tuning values keep the existing normalized mixer representation. */
static void test_mixer_controls(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct usbradioplus_hardware_adapter_mixer mixer = {0};
	struct usbradioplus_hardware_adapter_mixer_config mixer_config = {
		.struct_size = sizeof(mixer_config),
		.element = "Mic",
		.element_index = 0U,
		.channel = RPTADV_AUDIO_MIXER_CHANNEL_LEFT,
		.direction = RPTADV_AUDIO_MIXER_CAPTURE,
	};
	uint32_t value = 0U;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_mixer_open(&adapter, &mixer_config, &mixer) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer_open_calls == 1U);
	assert(usbradioplus_hardware_adapter_mixer_get_normalized(&mixer, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 500U);
	assert(usbradioplus_hardware_adapter_mixer_set_normalized(&mixer, 999U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_mixer_get_normalized(&mixer, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 999U);
	assert(usbradioplus_hardware_adapter_mixer_get_switch(&mixer, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 1U);
	assert(usbradioplus_hardware_adapter_mixer_set_switch(&mixer, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_mixer_get_switch(&mixer, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 0U);
	assert(usbradioplus_hardware_adapter_mixer_set_normalized(&mixer, 1000U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_hardware_adapter_mixer_close(&mixer);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify composition timing stays a post-open control-plane query. */
static void test_stream_timing(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_audio_stream_timing timing = {.struct_size = sizeof(timing)};
	const struct rptadv_audio_stream *stream =
		(const struct rptadv_audio_stream *)&stream_token;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_stream_get_timing(&adapter, stream, &timing) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(timing.abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION);
	assert(timing.input_latency_seconds == 0.010);
	assert(timing.output_latency_seconds == 0.020);
	assert(timing.sample_rate_hz == 48000.0);
	stream_timing_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION + 1U;
	assert(usbradioplus_hardware_adapter_stream_get_timing(&adapter, stream, &timing) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(usbradioplus_hardware_adapter_stream_get_timing(&adapter, NULL, &timing) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_hardware_adapter_close(&adapter);
}

/**
 * @brief Verify the POC-facing facade replaces endpoint indexes with one identity.
 *
 * The combined proof intentionally ignores any stale numeric PortAudio indexes
 * once audio and GPIO have been matched through the stable USB topology.
 */
static void test_stream_creation_uses_resolved_endpoints(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config identity = exact_config();
	struct rptadv_audio_stream_config stream_config = {
		.struct_size = sizeof(stream_config),
		.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
		.input_device_index = 101,
		.output_device_index = 202,
		.native_sample_rate_hz = 48000U,
		.maximum_frame_count = 960U,
		.input_device_channels = 1U,
		.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS,
	};
	struct rptadv_audio_stream *stream = NULL;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &identity, &fake_audio,
						     &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_stream_create(&adapter, &stream_config, &stream) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(stream == (struct rptadv_audio_stream *)&stream_token);
	assert(stream_create_calls == 1U);
	assert(stream_create_config.input_device_index == 6);
	assert(stream_create_config.output_device_index == 7);
	assert(stream_create_config.native_sample_rate_hz == 48000U);
	assert(stream_create_config.maximum_frame_count == 960U);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify raw-device snapshots cross the facade without reinterpretation. */
static void test_stream_statistics(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_audio_stream_stats statistics = {.struct_size = sizeof(statistics)};
	const struct rptadv_audio_stream *stream =
		(const struct rptadv_audio_stream *)&stream_token;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_stream_get_stats(&adapter, stream, &statistics) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(statistics.abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION);
	assert(statistics.callback_count == 12U);
	assert(statistics.input_peak == 0.5F);
	assert(statistics.output_rms == 0.25F);
	stream_stats_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION + 1U;
	assert(usbradioplus_hardware_adapter_stream_get_stats(&adapter, stream, &statistics) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(usbradioplus_hardware_adapter_stream_get_stats(&adapter, NULL, &statistics) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify scheduled GPIO pulses use only the appended adapter surface. */
static void test_scheduled_pulses(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_gpio_parallel_config parallel_config = {
		.struct_size = sizeof(parallel_config),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.transport = RPTADV_GPIO_PARALLEL_TRANSPORT_PPDEV,
		.ppdev_path = "/dev/parport0",
	};
	struct rptadv_gpio_cm119_scheduled_inverting_pulse_action gpio_action = {
		.struct_size = sizeof(gpio_action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.ptt_invert = 1U,
		.gpio_invert_mask = 0x06U,
		.pulse_duration_milliseconds = 40U,
	};
	struct rptadv_gpio_parallel_scheduled_inverting_pulse_action parallel_action = {
		.struct_size = sizeof(parallel_action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.invert_mask = 0x55U,
		.pulse_duration_milliseconds = 50U,
		.cancel_mask = 0x80U,
	};
	struct rptadv_gpio_adapter_descriptor old_gpio = fake_gpio;
	struct rptadv_gpio_adapter_descriptor incomplete_gpio = fake_gpio;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_open_gpio(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_open_parallel(&adapter, &parallel_config) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
		       &adapter, &gpio_action) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(gpio_scheduled_pulse_calls == 1U);
	assert(gpio_scheduled_pulse.ptt_invert == 1U);
	assert(gpio_scheduled_pulse.gpio_invert_mask == 0x06U);
	assert(gpio_scheduled_pulse.pulse_duration_milliseconds == 40U);
	assert(usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
		       &adapter, &parallel_action) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(parallel_scheduled_pulse_calls == 1U);
	assert(parallel_scheduled_pulse.invert_mask == 0x55U);
	assert(parallel_scheduled_pulse.pulse_duration_milliseconds == 50U);
	assert(parallel_scheduled_pulse.cancel_mask == 0x80U);

	gpio_scheduled_pulse_result = RPTADV_GPIO_IO_ERROR;
	parallel_scheduled_pulse_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
		       &adapter, &gpio_action) == USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
		       &adapter, &parallel_action) == USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);

	old_gpio.struct_size =
		offsetof(struct rptadv_gpio_adapter_descriptor, device_schedule_inverting_pulse);
	adapter.gpio = &old_gpio;
	assert(usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(&adapter,
									   &gpio_action) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(&adapter,
									       &parallel_action) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	incomplete_gpio.device_schedule_inverting_pulse = NULL;
	incomplete_gpio.parallel_schedule_inverting_pulse = NULL;
	adapter.gpio = &incomplete_gpio;
	assert(usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(&adapter,
									   &gpio_action) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(&adapter,
									       &parallel_action) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(&adapter, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	adapter.gpio = &fake_gpio;
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify legacy parallel radio operations use only appended GPIO ABI fields. */
static void test_parallel_radio_programming(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	struct rptadv_gpio_parallel_config parallel_config = {
		.struct_size = sizeof(parallel_config),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.transport = RPTADV_GPIO_PARALLEL_TRANSPORT_PPDEV,
		.ppdev_path = "/dev/parport0",
	};
	struct rptadv_gpio_adapter_descriptor old_gpio = fake_gpio;
	struct rptadv_gpio_adapter_descriptor incomplete_gpio = fake_gpio;

	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_open_parallel(&adapter, &parallel_config) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_adapter_set_parallel_binary_channel(&adapter, 0x0aU) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(parallel_binary_channel_calls == 1U);
	assert(parallel_binary_channel == 0x0aU);
	assert(usbradioplus_hardware_adapter_program_parallel_rtx(&adapter, 146340000U, 146940000U,
								  1U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(parallel_rtx_program_calls == 1U);
	assert(parallel_rtx_rx_frequency_hz == 146340000U);
	assert(parallel_rtx_tx_frequency_hz == 146940000U);
	assert(parallel_rtx_transmitting == 1U);
	assert(parallel_rtx_high_power == 1U);
	assert(usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(parallel_rtx_clear_calls == 1U);

	parallel_binary_channel_result = RPTADV_GPIO_IO_ERROR;
	parallel_rtx_program_result = RPTADV_GPIO_IO_ERROR;
	parallel_rtx_clear_result = RPTADV_GPIO_IO_ERROR;
	assert(usbradioplus_hardware_adapter_set_parallel_binary_channel(&adapter, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(usbradioplus_hardware_adapter_program_parallel_rtx(&adapter, 0U, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);
	assert(usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR);

	old_gpio.struct_size =
		offsetof(struct rptadv_gpio_adapter_descriptor, parallel_set_binary_channel);
	adapter.gpio = &old_gpio;
	assert(usbradioplus_hardware_adapter_set_parallel_binary_channel(&adapter, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_program_parallel_rtx(&adapter, 0U, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);

	incomplete_gpio.parallel_set_binary_channel = NULL;
	incomplete_gpio.parallel_program_rtx = NULL;
	incomplete_gpio.parallel_clear_rtx_transmit = NULL;
	adapter.gpio = &incomplete_gpio;
	assert(usbradioplus_hardware_adapter_set_parallel_binary_channel(&adapter, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_program_parallel_rtx(&adapter, 0U, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(&adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_hardware_adapter_set_parallel_binary_channel(NULL, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	adapter.gpio = &fake_gpio;
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Verify bad policy combinations and returned identities fail before HID ownership. */
static void test_invalid_identity_requests(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();

	reset_fake_adapters();
	config.usb_port_path = "3-1";
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(!selector_calls && !gpio_probe_calls);
	config = exact_config();
	config.device_identifier = NULL;
	config.usb_serial = NULL;
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(!selector_calls && !gpio_probe_calls);
	config = exact_config();
	config.device_selection_policy = RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD;
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(!selector_calls && !gpio_probe_calls);
	config = exact_config();
	selector_serial = "OTHER";
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_IDENTITY_MISMATCH);
	assert(selector_calls == 1U && !gpio_probe_calls);
	reset_fake_adapters();
	selector_match_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION + 1U;
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(selector_calls == 1U && !gpio_probe_calls);
	reset_fake_adapters();
	selector_selection_struct_size = 0U;
	assert(usbradioplus_hardware_adapter_prepare(&adapter, &config, &fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(selector_calls == 1U && !gpio_probe_calls);
}

/** @brief Verify both descriptor tail availability and the released entry point. */
static void test_descriptor_validation(void)
{
	struct rptadv_audio_adapter_descriptor short_audio = fake_audio;
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();

	assert(usbradioplus_hardware_adapter_validate(&fake_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	short_audio.struct_size =
		offsetof(struct rptadv_audio_adapter_descriptor, usb_device_select);
	assert(usbradioplus_hardware_adapter_validate(&short_audio, &fake_gpio) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER);
	reset_fake_adapters();
	assert(usbradioplus_hardware_adapter_prepare_released(&adapter, &config) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(selector_calls == 1U && gpio_probe_calls == 1U);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Assert one public facade result while preserving readable boundary tests. */
#define EXPECT_FACADE(result, operation)                                                           \
	assert((operation) == USBRADIOPLUS_HARDWARE_ADAPTER_##result)

/** @brief Reject every missing mandatory function before the facade acquires resources. */
static void test_descriptor_completeness(void)
{
	struct rptadv_audio_adapter_descriptor audio = fake_audio;
	struct rptadv_gpio_adapter_descriptor gpio = fake_gpio;

	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(NULL, &gpio));
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, NULL));
	audio.abi_version++;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	gpio.abi_version++;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.struct_size = 0U;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	audio.usb_device_resolve = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_create = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_start = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_stop = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_destroy = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_get_stats = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.stream_get_timing = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_create_for_usb_interface = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_get_range_steps = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_get_steps = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_set_steps = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_get_normalized = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_set_normalized = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_get_switch = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_set_switch = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.mixer_destroy = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	audio.usb_device_select = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	audio = fake_audio;
	gpio.device_probe = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_open = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_publish_outputs = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_service = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_get_inputs = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_get_stats = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_close = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_discover = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_read_eeprom = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.device_write_eeprom = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_open = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_publish_outputs = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_service = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_control_write_data = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_get_inputs = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_get_stats = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
	gpio.parallel_close = NULL;
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER, usbradioplus_hardware_adapter_validate(&audio, &gpio));
	gpio = fake_gpio;
}

/** @brief Check caller ABI errors and malformed identity replies before any device opens. */
static void test_prepare_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter_config config = exact_config();
	char overlong_path[USBRADIOPLUS_HARDWARE_ADAPTER_USB_PATH_CAPACITY + 1U];
	char overlong_serial[RPTADV_GPIO_DEVICE_SERIAL_CAPACITY + 1U];

	reset_fake_adapters();
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_prepare(
						NULL, &config, &fake_audio, &fake_gpio));
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_prepare(
						&adapter, NULL, &fake_audio, &fake_gpio));
	config.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	config.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	config.input_device_channels = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	config.output_device_channels = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	config.device_selection_policy = UINT32_MAX;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	EXPECT_FACADE(INCOMPATIBLE_ADAPTER,
		      usbradioplus_hardware_adapter_prepare(&adapter, &config, NULL, &fake_gpio));
	selector_result = RPTADV_AUDIO_UNSUPPORTED;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_match_struct_size = 0U;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_topology = "";
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_selection_abi_version = 0U;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_input_index = -1;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_output_index = -1;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	selector_serial = "";
	EXPECT_FACADE(IDENTITY_MISMATCH,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	gpio_probe_serial = "OTHER";
	EXPECT_FACADE(IDENTITY_MISMATCH,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	gpio_probe_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	gpio_probe_present = 0U;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	config.input_device_channels = 2U;
	config.output_device_channels = 1U;
	config.usb_serial = "";
	selector_serial = "";
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	usbradioplus_hardware_adapter_close(&adapter);
	config = exact_config();
	config.device_identifier = "";
	EXPECT_FACADE(IDENTITY_MISMATCH,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	config.device_selection_policy = RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD;
	config.device_identifier = NULL;
	config.usb_serial = "CM119-A";
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config.usb_serial = NULL;
	config.usb_port_path = "3-1";
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	config = exact_config();
	memset(overlong_path, 'x', sizeof(overlong_path) - 1U);
	overlong_path[sizeof(overlong_path) - 1U] = '\0';
	selector_topology = overlong_path;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
	reset_fake_adapters();
	memset(overlong_serial, 'x', sizeof(overlong_serial) - 1U);
	overlong_serial[sizeof(overlong_serial) - 1U] = '\0';
	selector_serial = overlong_serial;
	config.usb_serial = NULL;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_prepare_released(&adapter, &config));
}

/** @brief Exercise all malformed semantic path classes, not just incompatible descriptors. */
static void test_mixer_path_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {.audio = &fake_audio};
	struct rptadv_audio_cm119_mixer_paths paths = {.struct_size = sizeof(paths)};
	unsigned int mode;

	reset_fake_adapters();
	strcpy(adapter.usb_port_path, selector_topology);
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(NULL, &paths));
	adapter.audio = NULL;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths));
	adapter.audio = &fake_audio;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, NULL));
	paths.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(&adapter, &paths));
	for (mode = 1U; mode <= 11U; mode++) {
		paths.struct_size = sizeof(paths);
		mixer_paths_malformed_mode = mode;
		EXPECT_FACADE(AUDIO_ERROR, usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(
						   &adapter, &paths));
	}
}

/** @brief Verify independent device, descriptor, and action guards on the GPIO facade. */
static void test_gpio_argument_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {0};
	struct usbradioplus_hardware_adapter *candidate;
	struct rptadv_gpio_output_action action = {.struct_size = sizeof(action),
						   .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_cm119_scheduled_inverting_pulse_action pulse = {
		.struct_size = sizeof(pulse), .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_eeprom_image image = {.struct_size = sizeof(image),
						 .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_parallel_output_action parallel_action = {
		.struct_size = sizeof(parallel_action),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_parallel_scheduled_inverting_pulse_action parallel_pulse = {
		.struct_size = sizeof(parallel_pulse),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_parallel_config config = {
		.struct_size = sizeof(config), .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	unsigned int mode;

	reset_fake_adapters();
	for (mode = 0U; mode < 3U; mode++) {
		adapter.gpio = mode == 1U ? NULL : &fake_gpio;
		adapter.gpio_device =
			mode == 2U ? NULL : (struct rptadv_gpio_device *)&gpio_device_token;
		adapter.parallel_device =
			mode == 2U ? NULL
				   : (struct rptadv_gpio_parallel_device *)&parallel_device_token;
		candidate = mode == 0U ? NULL : &adapter;
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_publish_gpio(candidate, &action));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(candidate,
											  &pulse));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_service_gpio(candidate, NULL, NULL));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_read_eeprom(candidate, &image));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_write_eeprom(candidate, &image));
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_publish_parallel(
							candidate, &parallel_action));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
				      candidate, &parallel_pulse));
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_service_parallel(
							candidate, NULL, NULL));
		EXPECT_FACADE(
			INVALID_ARGUMENT,
			usbradioplus_hardware_adapter_set_parallel_binary_channel(candidate, 0U));
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_program_parallel_rtx(
							candidate, 0U, 0U, 0U, 0U));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(candidate));
	}
	adapter.gpio = &fake_gpio;
	adapter.gpio_device = (struct rptadv_gpio_device *)&gpio_device_token;
	adapter.parallel_device = (struct rptadv_gpio_parallel_device *)&parallel_device_token;

	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_publish_gpio(&adapter, NULL));
	action.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_publish_gpio(&adapter, &action));
	action.struct_size = sizeof(action);
	action.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_publish_gpio(&adapter, &action));
	action.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;

	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(&adapter, NULL));
	pulse.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
						&adapter, &pulse));
	pulse.struct_size = sizeof(pulse);
	pulse.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
						&adapter, &pulse));
	pulse.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;

	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_write_eeprom(&adapter, NULL));
	image.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_write_eeprom(&adapter, &image));
	image.struct_size = sizeof(image);
	image.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_write_eeprom(&adapter, &image));
	image.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;

	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_publish_parallel(&adapter, NULL));
	parallel_action.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_publish_parallel(&adapter, &parallel_action));
	parallel_action.struct_size = sizeof(parallel_action);
	parallel_action.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_publish_parallel(&adapter, &parallel_action));
	parallel_action.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;

	EXPECT_FACADE(
		INVALID_ARGUMENT,
		usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(&adapter, NULL));
	parallel_pulse.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
			      &adapter, &parallel_pulse));
	parallel_pulse.struct_size = sizeof(parallel_pulse);
	parallel_pulse.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
			      &adapter, &parallel_pulse));
	parallel_pulse.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;

	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_open_parallel(&adapter, NULL));
	config.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_open_parallel(&adapter, &config));
	config.struct_size = sizeof(config);
	config.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_open_parallel(&adapter, &config));
	config.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_read_eeprom(&adapter, NULL));
	image.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_read_eeprom(&adapter, &image));
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_open_gpio(NULL));
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_open_gpio(&adapter));
	adapter.audio = &fake_audio;
	adapter.gpio = NULL;
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_open_gpio(&adapter));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_open_parallel(&adapter, &config));
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_open_parallel(NULL, &config));
	adapter.gpio = &fake_gpio;
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_open_gpio(&adapter));
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_open_parallel(&adapter, &config));
	adapter.gpio_device = NULL;
	gpio_open_result = RPTADV_GPIO_IO_ERROR;
	strcpy(adapter.usb_port_path, selector_topology);
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_open_gpio(&adapter));
	adapter.parallel_device = NULL;
	parallel_operation_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_open_parallel(&adapter, &config));
	usbradioplus_hardware_adapter_close(NULL);
	usbradioplus_hardware_adapter_close(&adapter);
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Propagate service and snapshot errors independently for both GPIO transports. */
static void test_gpio_service_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {
		.gpio = &fake_gpio,
		.gpio_device = (struct rptadv_gpio_device *)&gpio_device_token,
		.parallel_device = (struct rptadv_gpio_parallel_device *)&parallel_device_token,
	};
	struct rptadv_gpio_input_snapshot inputs = {.struct_size = sizeof(inputs)};
	struct rptadv_gpio_device_stats stats = {.struct_size = sizeof(stats)};
	struct rptadv_gpio_parallel_input_snapshot parallel_inputs = {
		.struct_size = sizeof(parallel_inputs)};
	struct rptadv_gpio_parallel_stats parallel_stats = {.struct_size = sizeof(parallel_stats)};
	struct rptadv_gpio_parallel_output_action action = {
		.struct_size = sizeof(action), .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};
	struct rptadv_gpio_output_action gpio_action = {
		.struct_size = sizeof(gpio_action), .abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION};

	reset_fake_adapters();
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_service_gpio(&adapter, NULL, NULL));
	gpio_publish_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_publish_gpio(&adapter, &gpio_action));
	gpio_service_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_service_gpio(&adapter, NULL, NULL));
	gpio_service_result = RPTADV_GPIO_OK;
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_publish_parallel(&adapter, &action));
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_service_parallel(&adapter, NULL, NULL));
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_service_parallel(&adapter, &parallel_inputs,
									 &parallel_stats));
	parallel_operation_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_publish_parallel(&adapter, &action));
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_service_parallel(&adapter, NULL, NULL));
	parallel_operation_result = RPTADV_GPIO_OK;

	inputs.struct_size = 0U;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_service_gpio(&adapter, &inputs, &stats));
	inputs.struct_size = sizeof(inputs);
	gpio_inputs_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_service_gpio(&adapter, &inputs, &stats));
	gpio_inputs_result = RPTADV_GPIO_OK;
	stats.struct_size = 0U;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_service_gpio(&adapter, &inputs, &stats));
	stats.struct_size = sizeof(stats);
	gpio_stats_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR,
		      usbradioplus_hardware_adapter_service_gpio(&adapter, &inputs, &stats));
	gpio_stats_result = RPTADV_GPIO_OK;

	parallel_inputs.struct_size = 0U;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_service_parallel(
					  &adapter, &parallel_inputs, &parallel_stats));
	parallel_inputs.struct_size = sizeof(parallel_inputs);
	parallel_inputs_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_service_parallel(
					  &adapter, &parallel_inputs, &parallel_stats));
	parallel_inputs_result = RPTADV_GPIO_OK;
	parallel_stats.struct_size = 0U;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_service_parallel(
					  &adapter, &parallel_inputs, &parallel_stats));
	parallel_stats.struct_size = sizeof(parallel_stats);
	parallel_stats_result = RPTADV_GPIO_IO_ERROR;
	EXPECT_FACADE(GPIO_ERROR, usbradioplus_hardware_adapter_service_parallel(
					  &adapter, &parallel_inputs, &parallel_stats));
	parallel_stats_result = RPTADV_GPIO_OK;
	usbradioplus_hardware_adapter_close(&adapter);
}

/** @brief Assert mixer guards, backend failures, and overflow-safe legacy floor mapping. */
static void test_mixer_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {.audio = &fake_audio};
	struct usbradioplus_hardware_adapter_mixer mixer = {0};
	struct usbradioplus_hardware_adapter_mixer *candidate;
	struct usbradioplus_hardware_adapter_mixer_config config = {
		.struct_size = sizeof(config),
		.element = "Mic",
		.channel = RPTADV_AUDIO_MIXER_CHANNEL_LEFT,
		.direction = RPTADV_AUDIO_MIXER_CAPTURE,
	};
	uint32_t value = 0U;
	unsigned int mode;

	reset_fake_adapters();
	strcpy(adapter.usb_port_path, selector_topology);
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(NULL, &config, &mixer));
	adapter.audio = NULL;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	adapter.audio = &fake_audio;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, NULL, &mixer));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, NULL));
	config.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	config.struct_size = sizeof(config);
	config.element = NULL;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	config.element = "";
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	config.element = "Mic";
	audio_operation_result = RPTADV_AUDIO_UNSUPPORTED;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	mixer.mixer = NULL;
	audio_operation_result = RPTADV_AUDIO_OK;
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_open(&adapter, &config, &mixer));
	for (mode = 0U; mode < 3U; mode++) {
		mixer.audio = mode == 1U ? NULL : &fake_audio;
		mixer.mixer = mode == 2U ? NULL : (struct rptadv_audio_mixer *)&mixer_token;
		candidate = mode == 0U ? NULL : &mixer;
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_mixer_set_legacy_level(candidate, 1U));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_mixer_set_normalized(candidate, 1U));
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_mixer_get_normalized(
							candidate, &value));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_mixer_set_switch(candidate, 1U));
		EXPECT_FACADE(INVALID_ARGUMENT,
			      usbradioplus_hardware_adapter_mixer_get_switch(candidate, &value));
	}
	mixer.audio = &fake_audio;
	mixer.mixer = (struct rptadv_audio_mixer *)&mixer_token;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 1000U));
	EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_mixer_set_switch(&mixer, 2U));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_get_normalized(&mixer, NULL));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_mixer_get_switch(&mixer, NULL));
	audio_operation_result = RPTADV_AUDIO_UNSUPPORTED;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 1U));
	EXPECT_FACADE(AUDIO_ERROR, usbradioplus_hardware_adapter_mixer_set_normalized(&mixer, 1U));
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_get_normalized(&mixer, &value));
	EXPECT_FACADE(AUDIO_ERROR, usbradioplus_hardware_adapter_mixer_set_switch(&mixer, 1U));
	EXPECT_FACADE(AUDIO_ERROR, usbradioplus_hardware_adapter_mixer_get_switch(&mixer, &value));
	audio_operation_result = RPTADV_AUDIO_OK;
	mixer_step_minimum = 2;
	mixer_step_maximum = 1;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 1U));
	mixer_step_minimum = -2;
	mixer_step_maximum = -1;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 1U));
	mixer_step_minimum = 1;
	mixer_step_maximum = 100;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 0U));
	mixer_step_minimum = 0;
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 999U));
	assert(mixer_step_value == 99);
	mixer_step_maximum = INT64_MAX;
	EXPECT_FACADE(OK, usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 500U));
	assert(mixer_step_value == INT64_MAX / 2);
	mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_mixer_set_legacy_level(&mixer, 500U));
	usbradioplus_hardware_adapter_mixer_close(&mixer);
	usbradioplus_hardware_adapter_mixer_close(&mixer);
	mixer.audio = &fake_audio;
	usbradioplus_hardware_adapter_mixer_close(&mixer);
	usbradioplus_hardware_adapter_mixer_close(NULL);
}

/** @brief Check stream API argument boundaries and failure propagation without real audio. */
static void test_stream_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = {
		.audio = &fake_audio, .input_device_channels = 1U, .output_device_channels = 2U};
	struct usbradioplus_hardware_adapter *candidate;
	struct rptadv_audio_stream_config config = {.struct_size = sizeof(config),
						    .abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
						    .input_device_channels = 1U,
						    .output_device_channels = 2U};
	struct rptadv_audio_stream *stream = (struct rptadv_audio_stream *)&stream_token;
	struct rptadv_audio_stream_stats stats = {.struct_size = sizeof(stats)};
	struct rptadv_audio_stream_timing timing = {.struct_size = sizeof(timing)};
	unsigned int mode;

	reset_fake_adapters();
	for (mode = 0U; mode < 2U; mode++) {
		adapter.audio = mode == 1U ? NULL : &fake_audio;
		candidate = mode == 0U ? NULL : &adapter;
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_stream_create(
							candidate, &config, &stream));
		stream = (struct rptadv_audio_stream *)&stream_token;
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_stream_get_stats(
							candidate, stream, &stats));
		EXPECT_FACADE(INVALID_ARGUMENT, usbradioplus_hardware_adapter_stream_get_timing(
							candidate, stream, &timing));
	}
	adapter.audio = &fake_audio;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, NULL, &stream));
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, NULL));
	config.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, &stream));
	config.struct_size = sizeof(config);
	config.abi_version = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, &stream));
	config.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	config.input_device_channels = 2U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, &stream));
	config.input_device_channels = 1U;
	config.output_device_channels = 1U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, &stream));
	config.output_device_channels = 2U;
	audio_operation_result = RPTADV_AUDIO_UNSUPPORTED;
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_stream_create(&adapter, &config, &stream));
	assert(stream == NULL);
	stream = (struct rptadv_audio_stream *)&stream_token;

	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_get_stats(&adapter, stream, NULL));
	stats.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_get_stats(&adapter, stream, &stats));
	stats.struct_size = sizeof(stats);
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_stream_get_stats(
			      &adapter, (struct rptadv_audio_stream *)&mixer_token, &stats));

	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_get_timing(&adapter, stream, NULL));
	timing.struct_size = 0U;
	EXPECT_FACADE(INVALID_ARGUMENT,
		      usbradioplus_hardware_adapter_stream_get_timing(&adapter, stream, &timing));
	timing.struct_size = sizeof(timing);
	EXPECT_FACADE(AUDIO_ERROR,
		      usbradioplus_hardware_adapter_stream_get_timing(
			      &adapter, (struct rptadv_audio_stream *)&mixer_token, &timing));
}

int main(void)
{
	test_exact_identity_resolution();
	test_gpio_service_lifecycle();
	test_eeprom_lifecycle();
	test_topology_identity_resolution();
	test_automatic_identity_resolution();
	test_cm119_mixer_path_discovery();
	test_mixer_controls();
	test_stream_timing();
	test_stream_creation_uses_resolved_endpoints();
	test_stream_statistics();
	test_scheduled_pulses();
	test_parallel_radio_programming();
	test_invalid_identity_requests();
	test_descriptor_validation();
	test_descriptor_completeness();
	test_prepare_boundaries();
	test_mixer_path_boundaries();
	test_gpio_argument_boundaries();
	test_gpio_service_boundaries();
	test_mixer_boundaries();
	test_stream_boundaries();
	return 0;
}
