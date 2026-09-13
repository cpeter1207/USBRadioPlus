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

/** @brief Count fake audio identity requests. */
static unsigned int selector_calls;
/** @brief Count fake GPIO probes. */
static unsigned int gpio_probe_calls;
/** @brief Latest selection request issued by the facade. */
static struct rptadv_audio_usb_device_selector selector_request;
/** @brief Canonical topology returned by the fake audio adapter. */
static const char *selector_topology = "3-1:1.0";
/** @brief Serial returned by the fake audio adapter. */
static const char *selector_serial = "CM119-A";
/** @brief ABI version returned in one fake complete selection. */
static uint32_t selector_match_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
/** @brief Returned nested selection size for malformed-result tests. */
static uint32_t selector_selection_struct_size;
/** @brief Count opens of the canonical test ALSA mixer path. */
static unsigned int mixer_open_calls;
/** @brief Fake adapter-owned normalized mixer value. */
static uint32_t mixer_normalized;
/** @brief Fake adapter-owned mixer switch state. */
static uint32_t mixer_switch;
/** @brief Number of CM119 semantic mixer-path discovery requests. */
static unsigned int mixer_paths_calls;
/** @brief Canonical topology supplied to semantic mixer-path discovery. */
static const char *mixer_paths_topology;
/** @brief Scripted semantic mixer-path resolver result. */
static enum rptadv_audio_result mixer_paths_result;
/** @brief Scripted ABI version returned with semantic mixer paths. */
static uint32_t mixer_paths_abi_version;
/** @brief Scripted malformed semantic mixer-path response mode. */
static unsigned int mixer_paths_malformed_mode;
/** @brief Storage whose address represents one opaque open mixer. */
static int mixer_token;
/** @brief Storage whose address represents one opaque open stream. */
static int stream_token;
/** @brief Storage whose address represents one opaque CM119 GPIO device. */
static int gpio_device_token;
/** @brief Number of CM119 GPIO device-open operations. */
static unsigned int gpio_open_calls;
/** @brief Number of CM119 GPIO output publications. */
static unsigned int gpio_publish_calls;
/** @brief Number of CM119 GPIO service cycles. */
static unsigned int gpio_service_calls;
/** @brief Number of CM119 EEPROM reads issued through the facade. */
static unsigned int gpio_eeprom_read_calls;
/** @brief Number of CM119 EEPROM writes issued through the facade. */
static unsigned int gpio_eeprom_write_calls;
/** @brief Number of CM119 GPIO device-close operations. */
static unsigned int gpio_close_calls;
/** @brief Latest output action published through the CM119 facade. */
static struct rptadv_gpio_output_action gpio_published_action;
/** @brief Input snapshot returned by the deterministic CM119 service fake. */
static struct rptadv_gpio_input_snapshot gpio_input_snapshot;
/** @brief Statistics returned by the deterministic CM119 service fake. */
static struct rptadv_gpio_device_stats gpio_stats_snapshot;
/** @brief Scripted result from CM119 output publication. */
static enum rptadv_gpio_result gpio_publish_result;
/** @brief Scripted result from CM119 HID service. */
static enum rptadv_gpio_result gpio_service_result;
/** @brief Scripted result from CM119 EEPROM reads. */
static enum rptadv_gpio_result gpio_eeprom_read_result;
/** @brief Scripted result from CM119 EEPROM writes. */
static enum rptadv_gpio_result gpio_eeprom_write_result;
/** @brief Size supplied by the facade for the last CM119 EEPROM read. */
static uint32_t gpio_eeprom_read_request_struct_size;
/** @brief Complete EEPROM image returned by the deterministic CM119 fake. */
static struct rptadv_gpio_eeprom_image gpio_eeprom_read_image;
/** @brief EEPROM image received by the deterministic CM119 write fake. */
static struct rptadv_gpio_eeprom_image gpio_eeprom_written_image;
/** @brief Storage whose address represents one opaque parallel GPIO device. */
static int parallel_device_token;
/** @brief Timing ABI version returned by the fake stream. */
static uint32_t stream_timing_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
/** @brief Statistics ABI version returned by the fake stream. */
static uint32_t stream_stats_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
/** @brief Number of scheduled CM119 pulse publications. */
static unsigned int gpio_scheduled_pulse_calls;
/** @brief Number of scheduled parallel pulse publications. */
static unsigned int parallel_scheduled_pulse_calls;
/** @brief Last scheduled CM119 pulse action. */
static struct rptadv_gpio_cm119_scheduled_inverting_pulse_action gpio_scheduled_pulse;
/** @brief Last scheduled parallel pulse action. */
static struct rptadv_gpio_parallel_scheduled_inverting_pulse_action parallel_scheduled_pulse;
/** @brief Result returned by the fake scheduled CM119 pulse API. */
static enum rptadv_gpio_result gpio_scheduled_pulse_result;
/** @brief Result returned by the fake scheduled parallel pulse API. */
static enum rptadv_gpio_result parallel_scheduled_pulse_result;
/** @brief Number of binary channel selections forwarded through the facade. */
static unsigned int parallel_binary_channel_calls;
/** @brief Last legacy binary channel supplied to the GPIO adapter. */
static uint8_t parallel_binary_channel;
/** @brief Result returned by the fake binary channel API. */
static enum rptadv_gpio_result parallel_binary_channel_result;
/** @brief Number of RTX programming operations forwarded through the facade. */
static unsigned int parallel_rtx_program_calls;
/** @brief Last receive frequency supplied to the fake RTX operation. */
static uint32_t parallel_rtx_rx_frequency_hz;
/** @brief Last transmit frequency supplied to the fake RTX operation. */
static uint32_t parallel_rtx_tx_frequency_hz;
/** @brief Last transmit-state value supplied to the fake RTX operation. */
static uint32_t parallel_rtx_transmitting;
/** @brief Last legacy high-power value supplied to the fake RTX operation. */
static uint32_t parallel_rtx_high_power;
/** @brief Result returned by the fake RTX programming API. */
static enum rptadv_gpio_result parallel_rtx_program_result;
/** @brief Number of immediate RTX transmit-clear operations forwarded. */
static unsigned int parallel_rtx_clear_calls;
/** @brief Result returned by the fake immediate RTX transmit-clear API. */
static enum rptadv_gpio_result parallel_rtx_clear_result;
/** @brief Number of stream creations forwarded through the composition. */
static unsigned int stream_create_calls;
/** @brief Stream request last received by the fake audio adapter. */
static struct rptadv_audio_stream_config stream_create_config;
/** @brief Override ordinary fake audio operations for failure propagation tests. */
static enum rptadv_audio_result audio_operation_result;
/** @brief Override parallel fake operations for success and failure checks. */
static enum rptadv_gpio_result parallel_operation_result;
/** @brief Override identity selection before a result is returned. */
static enum rptadv_audio_result selector_result;
/** @brief Override the nested endpoint ABI in returned identity data. */
static uint32_t selector_selection_abi_version;
/** @brief Override the returned PortAudio input and output endpoint indices. */
static int selector_input_index, selector_output_index;
/** @brief Override the top-level identity size returned by the audio adapter. */
static uint32_t selector_match_struct_size;
/** @brief Script a GPIO probe failure or missing device without real USB I/O. */
static enum rptadv_gpio_result gpio_probe_result;
/** @brief Presence returned by the scripted GPIO probe. */
static uint32_t gpio_probe_present;
/** @brief Optional independent GPIO serial, used to reject cross-device matches. */
static const char *gpio_probe_serial;
/** @brief Script a CM119 open failure. */
static enum rptadv_gpio_result gpio_open_result;

/** @brief Capture one facade-bound stream request without opening PortAudio. */
static enum rptadv_audio_result
fake_audio_stream_create(const struct rptadv_audio_stream_config *config,
			 struct rptadv_audio_stream **stream)
{
	if (!config || !stream)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	stream_create_calls++;
	if (audio_operation_result != RPTADV_AUDIO_OK)
		return audio_operation_result;
	stream_create_config = *config;
	*stream = (struct rptadv_audio_stream *)&stream_token;
	return RPTADV_AUDIO_OK;
}

static enum rptadv_audio_result fake_audio_stream_start(struct rptadv_audio_stream *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_stream_stop(struct rptadv_audio_stream *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result
fake_audio_stream_stats(const struct rptadv_audio_stream *stream,
			struct rptadv_audio_stream_stats *statistics)
{
	if (stream != (const struct rptadv_audio_stream *)&stream_token || !statistics ||
	    statistics->struct_size < sizeof(*statistics))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	statistics->abi_version = stream_stats_abi_version;
	statistics->callback_count = 12U;
	statistics->input_peak = 0.5F;
	statistics->output_rms = 0.25F;
	return RPTADV_AUDIO_OK;
}

static void fake_audio_stream_destroy(struct rptadv_audio_stream *)
{
}

/** @brief Return deterministic post-open stream timing without PortAudio. */
static enum rptadv_audio_result
fake_audio_stream_get_timing(const struct rptadv_audio_stream *stream,
			     struct rptadv_audio_stream_timing *timing)
{
	if (stream != (const struct rptadv_audio_stream *)&stream_token || !timing ||
	    timing->struct_size < sizeof(*timing))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	timing->abi_version = stream_timing_abi_version;
	timing->input_latency_seconds = 0.010;
	timing->output_latency_seconds = 0.020;
	timing->sample_rate_hz = 48000.0;
	return RPTADV_AUDIO_OK;
}

static enum rptadv_audio_result fake_audio_mixer_create(const struct rptadv_audio_mixer_config *,
							struct rptadv_audio_mixer **)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_range_centibels(const struct rptadv_audio_mixer *,
								 int64_t *, int64_t *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_get_centibels(const struct rptadv_audio_mixer *,
							       int64_t *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_set_centibels(struct rptadv_audio_mixer *, int64_t)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static void fake_audio_mixer_destroy(struct rptadv_audio_mixer *)
{
}

static enum rptadv_audio_result
fake_audio_mixer_create_usb(const struct rptadv_audio_usb_mixer_config *config,
			    struct rptadv_audio_mixer **mixer)
{
	assert(config);
	assert(mixer);
	assert(!strcmp(config->usb_interface_path, selector_topology));
	assert(!strcmp(config->element, "Mic"));
	assert(config->element_index == 0U);
	assert(config->channel == RPTADV_AUDIO_MIXER_CHANNEL_LEFT);
	assert(config->direction == RPTADV_AUDIO_MIXER_CAPTURE);
	mixer_open_calls++;
	*mixer = (struct rptadv_audio_mixer *)&mixer_token;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_range_steps(const struct rptadv_audio_mixer *,
							     int64_t *, int64_t *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_get_steps(const struct rptadv_audio_mixer *,
							   int64_t *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_set_steps(struct rptadv_audio_mixer *, int64_t)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_get_normalized(const struct rptadv_audio_mixer *,
								uint32_t *value)
{
	if (!value)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	*value = mixer_normalized;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_set_normalized(struct rptadv_audio_mixer *,
								uint32_t value)
{
	mixer_normalized = value;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_get_switch(const struct rptadv_audio_mixer *,
							    uint32_t *value)
{
	if (!value)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	*value = mixer_switch;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_set_switch(struct rptadv_audio_mixer *,
							    uint32_t value)
{
	mixer_switch = value;
	return audio_operation_result;
}

/**
 * @brief Return deterministic CM119 mixer paths without opening ALSA.
 * @param usb_interface_path Canonical topology selected by the facade.
 * @param paths Caller-sized semantic result to fill.
 * @return The scripted audio-adapter result.
 */
static enum rptadv_audio_result
fake_audio_cm119_mixer_paths_resolve(const char *usb_interface_path,
				     struct rptadv_audio_cm119_mixer_paths *paths)
{
	uint32_t paths_size;

	if (!usb_interface_path || !paths)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	mixer_paths_calls++;
	mixer_paths_topology = usb_interface_path;
	paths_size = paths->struct_size;
	memset(paths, 0, sizeof(*paths));
	paths->struct_size = paths_size;
	if (paths_size < sizeof(*paths))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (mixer_paths_result != RPTADV_AUDIO_OK)
		return mixer_paths_result;
	paths->abi_version = mixer_paths_abi_version;
	paths->rx_capture_path_count = 1U;
	paths->tx_playback_path_count = 1U;
	paths->sidetone_path_count = 1U;
	paths->rx_compatibility_switch_path_count = 1U;
	strcpy(paths->rx_capture_paths[0].element, "Mic");
	paths->rx_capture_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_LEFT;
	paths->rx_capture_paths[0].direction = RPTADV_AUDIO_MIXER_CAPTURE;
	paths->rx_capture_paths[0].capabilities =
		RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME | RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH;
	strcpy(paths->tx_playback_paths[0].element, "Speaker");
	paths->tx_playback_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_LEFT;
	paths->tx_playback_paths[0].direction = RPTADV_AUDIO_MIXER_PLAYBACK;
	paths->tx_playback_paths[0].capabilities = RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME;
	strcpy(paths->sidetone_paths[0].element, "Mic");
	paths->sidetone_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_RIGHT;
	paths->sidetone_paths[0].direction = RPTADV_AUDIO_MIXER_PLAYBACK;
	paths->sidetone_paths[0].capabilities = RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME;
	strcpy(paths->rx_compatibility_switch_paths[0].element, "Auto Gain Control");
	paths->rx_compatibility_switch_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_LEFT;
	paths->rx_compatibility_switch_paths[0].direction = RPTADV_AUDIO_MIXER_PLAYBACK;
	paths->rx_compatibility_switch_paths[0].capabilities = RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH;
	if (mixer_paths_malformed_mode == 1U)
		paths->rx_capture_path_count = RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY + 1U;
	else if (mixer_paths_malformed_mode == 2U)
		paths->tx_playback_paths[0].direction = RPTADV_AUDIO_MIXER_CAPTURE;
	else if (mixer_paths_malformed_mode == 3U)
		paths->struct_size = 0U;
	else if (mixer_paths_malformed_mode == 4U)
		paths->rx_capture_path_count = 0U;
	else if (mixer_paths_malformed_mode == 5U)
		paths->tx_playback_path_count = 0U;
	else if (mixer_paths_malformed_mode == 6U)
		paths->rx_capture_paths[0].element[0] = '\0';
	else if (mixer_paths_malformed_mode == 7U)
		memset(paths->rx_capture_paths[0].element, 'X',
		       sizeof(paths->rx_capture_paths[0].element));
	else if (mixer_paths_malformed_mode == 8U)
		paths->rx_capture_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_RIGHT + 1U;
	else if (mixer_paths_malformed_mode == 9U)
		paths->rx_capture_paths[0].capabilities = 0U;
	else if (mixer_paths_malformed_mode == 10U)
		paths->sidetone_paths[0].capabilities = 0U;
	else if (mixer_paths_malformed_mode == 11U)
		paths->rx_compatibility_switch_paths[0].capabilities = 0U;
	return RPTADV_AUDIO_OK;
}

static enum rptadv_audio_result
fake_audio_usb_resolve(const struct rptadv_audio_usb_device_identity *,
		       struct rptadv_audio_usb_device_selection *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

/** @brief Resolve one legacy selector to a deterministic CM119 identity. */
static enum rptadv_audio_result
fake_audio_usb_select(const struct rptadv_audio_usb_device_selector *selector,
		      struct rptadv_audio_usb_device_match *match)
{
	selector_calls++;
	assert(selector);
	assert(match);
	selector_request = *selector;
	assert(match->struct_size == sizeof(*match));
	if (selector_result != RPTADV_AUDIO_OK)
		return selector_result;
	memset(match, 0, sizeof(*match));
	match->struct_size = selector_match_struct_size;
	match->abi_version = selector_match_abi_version;
	strcpy(match->usb_interface_path, selector_topology);
	strcpy(match->usb_serial, selector_serial);
	match->selection.struct_size = selector_selection_struct_size;
	match->selection.abi_version = selector_selection_abi_version;
	match->selection.alsa_card_index = 4U;
	match->selection.input_device_index = selector_input_index;
	match->selection.output_device_index = selector_output_index;
	return RPTADV_AUDIO_OK;
}

static enum rptadv_gpio_result fake_gpio_probe(const struct rptadv_gpio_device_config *config,
					       struct rptadv_gpio_device_info *info)
{
	gpio_probe_calls++;
	assert(config);
	assert(info);
	assert(!strcmp(config->usb_port_path, selector_topology));
	assert(info->struct_size == sizeof(*info));
	memset(info, 0, sizeof(*info));
	info->struct_size = sizeof(*info);
	info->abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	info->present = gpio_probe_present;
	strcpy(info->serial, gpio_probe_serial ? gpio_probe_serial : selector_serial);
	return gpio_probe_result;
}

static enum rptadv_gpio_result fake_gpio_open(const struct rptadv_gpio_device_config *,
					      struct rptadv_gpio_device **device)
{
	if (!device)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_open_calls++;
	if (gpio_open_result != RPTADV_GPIO_OK)
		return gpio_open_result;
	*device = (struct rptadv_gpio_device *)&gpio_device_token;
	return RPTADV_GPIO_OK;
}

static enum rptadv_gpio_result fake_gpio_publish(struct rptadv_gpio_device *device,
						 const struct rptadv_gpio_output_action *action)
{
	if (device != (struct rptadv_gpio_device *)&gpio_device_token || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_publish_calls++;
	gpio_published_action = *action;
	return gpio_publish_result;
}

static enum rptadv_gpio_result fake_gpio_service(struct rptadv_gpio_device *device)
{
	if (device != (struct rptadv_gpio_device *)&gpio_device_token)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_service_calls++;
	return gpio_service_result;
}

static enum rptadv_gpio_result fake_gpio_inputs(const struct rptadv_gpio_device *device,
						struct rptadv_gpio_input_snapshot *snapshot)
{
	if (device != (const struct rptadv_gpio_device *)&gpio_device_token || !snapshot ||
	    snapshot->struct_size < sizeof(*snapshot))
		return RPTADV_GPIO_INVALID_ARGUMENT;
	*snapshot = gpio_input_snapshot;
	return RPTADV_GPIO_OK;
}

static enum rptadv_gpio_result fake_gpio_stats(const struct rptadv_gpio_device *device,
					       struct rptadv_gpio_device_stats *snapshot)
{
	if (device != (const struct rptadv_gpio_device *)&gpio_device_token || !snapshot ||
	    snapshot->struct_size < sizeof(*snapshot))
		return RPTADV_GPIO_INVALID_ARGUMENT;
	*snapshot = gpio_stats_snapshot;
	return RPTADV_GPIO_OK;
}

static void fake_gpio_close(struct rptadv_gpio_device *device)
{
	assert(device == (struct rptadv_gpio_device *)&gpio_device_token);
	gpio_close_calls++;
}

static enum rptadv_gpio_result fake_gpio_discover(struct rptadv_gpio_device_list *)
{
	return RPTADV_GPIO_UNSUPPORTED;
}

static enum rptadv_gpio_result fake_gpio_read_eeprom(struct rptadv_gpio_device *device,
						     struct rptadv_gpio_eeprom_image *image)
{
	if (device != (struct rptadv_gpio_device *)&gpio_device_token || !image ||
	    image->struct_size < sizeof(*image))
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_eeprom_read_calls++;
	gpio_eeprom_read_request_struct_size = image->struct_size;
	if (gpio_eeprom_read_result != RPTADV_GPIO_OK)
		return gpio_eeprom_read_result;
	*image = gpio_eeprom_read_image;
	return RPTADV_GPIO_OK;
}

static enum rptadv_gpio_result fake_gpio_write_eeprom(struct rptadv_gpio_device *device,
						      struct rptadv_gpio_eeprom_image *image)
{
	if (device != (struct rptadv_gpio_device *)&gpio_device_token || !image ||
	    image->struct_size < sizeof(*image) ||
	    image->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_eeprom_write_calls++;
	gpio_eeprom_written_image = *image;
	if (gpio_eeprom_write_result != RPTADV_GPIO_OK)
		return gpio_eeprom_write_result;
	/* The released GPIO adapter stamps these physical compatibility fields. */
	image->magic_valid = 1U;
	image->checksum_valid = 1U;
	image->words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] = RPTADV_GPIO_CM119_EEPROM_MAGIC;
	return RPTADV_GPIO_OK;
}

/** @brief Record one scheduled CM119 pulse request without USB I/O. */
static enum rptadv_gpio_result fake_gpio_schedule_inverting_pulse(
	struct rptadv_gpio_device *device,
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action *action)
{
	if (device != (struct rptadv_gpio_device *)&gpio_device_token || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	gpio_scheduled_pulse_calls++;
	gpio_scheduled_pulse = *action;
	return gpio_scheduled_pulse_result;
}

static enum rptadv_gpio_result fake_parallel_open(const struct rptadv_gpio_parallel_config *,
						  struct rptadv_gpio_parallel_device **device)
{
	if (!device)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	if (parallel_operation_result != RPTADV_GPIO_OK)
		return parallel_operation_result;
	*device = (struct rptadv_gpio_parallel_device *)&parallel_device_token;
	return RPTADV_GPIO_OK;
}

static enum rptadv_gpio_result
fake_parallel_publish(struct rptadv_gpio_parallel_device *,
		      const struct rptadv_gpio_parallel_output_action *)
{
	return parallel_operation_result;
}

static enum rptadv_gpio_result fake_parallel_service(struct rptadv_gpio_parallel_device *)
{
	return parallel_operation_result;
}

static enum rptadv_gpio_result fake_parallel_control(struct rptadv_gpio_parallel_device *, uint32_t)
{
	return RPTADV_GPIO_UNSUPPORTED;
}

static enum rptadv_gpio_result fake_parallel_inputs(const struct rptadv_gpio_parallel_device *,
						    struct rptadv_gpio_parallel_input_snapshot *)
{
	return parallel_operation_result;
}

static enum rptadv_gpio_result fake_parallel_stats(const struct rptadv_gpio_parallel_device *,
						   struct rptadv_gpio_parallel_stats *)
{
	return parallel_operation_result;
}

static void fake_parallel_close(struct rptadv_gpio_parallel_device *)
{
}

/** @brief Record one scheduled parallel pulse request without port I/O. */
static enum rptadv_gpio_result fake_parallel_schedule_inverting_pulse(
	struct rptadv_gpio_parallel_device *device,
	const struct rptadv_gpio_parallel_scheduled_inverting_pulse_action *action)
{
	if (device != (struct rptadv_gpio_parallel_device *)&parallel_device_token || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	parallel_scheduled_pulse_calls++;
	parallel_scheduled_pulse = *action;
	return parallel_scheduled_pulse_result;
}

/** @brief Record one legacy binary parallel channel selection without I/O. */
static enum rptadv_gpio_result
fake_parallel_set_binary_channel(struct rptadv_gpio_parallel_device *device, uint8_t channel)
{
	if (device != (struct rptadv_gpio_parallel_device *)&parallel_device_token)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	parallel_binary_channel_calls++;
	parallel_binary_channel = channel;
	return parallel_binary_channel_result;
}

/** @brief Record one legacy RTX programming operation without I/O. */
static enum rptadv_gpio_result fake_parallel_program_rtx(struct rptadv_gpio_parallel_device *device,
							 uint32_t rx_frequency_hz,
							 uint32_t tx_frequency_hz,
							 uint32_t transmitting, uint32_t high_power)
{
	if (device != (struct rptadv_gpio_parallel_device *)&parallel_device_token)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	parallel_rtx_program_calls++;
	parallel_rtx_rx_frequency_hz = rx_frequency_hz;
	parallel_rtx_tx_frequency_hz = tx_frequency_hz;
	parallel_rtx_transmitting = transmitting;
	parallel_rtx_high_power = high_power;
	return parallel_rtx_program_result;
}

/** @brief Record one immediate legacy RTX transmit clear without I/O. */
static enum rptadv_gpio_result
fake_parallel_clear_rtx_transmit(struct rptadv_gpio_parallel_device *device)
{
	if (device != (struct rptadv_gpio_parallel_device *)&parallel_device_token)
		return RPTADV_GPIO_INVALID_ARGUMENT;
	parallel_rtx_clear_calls++;
	return parallel_rtx_clear_result;
}

/** @brief Complete fake audio ABI surface; only identity selection is exercised. */
static const struct rptadv_audio_adapter_descriptor fake_audio = {
	.struct_size = sizeof(fake_audio),
	.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
	.capability_name = "test-audio",
	.stream_create = fake_audio_stream_create,
	.stream_start = fake_audio_stream_start,
	.stream_stop = fake_audio_stream_stop,
	.stream_get_stats = fake_audio_stream_stats,
	.stream_destroy = fake_audio_stream_destroy,
	.mixer_create = fake_audio_mixer_create,
	.mixer_get_range_centibels = fake_audio_mixer_range_centibels,
	.mixer_get_centibels = fake_audio_mixer_get_centibels,
	.mixer_set_centibels = fake_audio_mixer_set_centibels,
	.mixer_destroy = fake_audio_mixer_destroy,
	.mixer_create_for_usb_interface = fake_audio_mixer_create_usb,
	.mixer_get_range_steps = fake_audio_mixer_range_steps,
	.mixer_get_steps = fake_audio_mixer_get_steps,
	.mixer_set_steps = fake_audio_mixer_set_steps,
	.mixer_get_normalized = fake_audio_mixer_get_normalized,
	.mixer_set_normalized = fake_audio_mixer_set_normalized,
	.mixer_get_switch = fake_audio_mixer_get_switch,
	.mixer_set_switch = fake_audio_mixer_set_switch,
	.usb_device_resolve = fake_audio_usb_resolve,
	.usb_device_select = fake_audio_usb_select,
	.stream_get_timing = fake_audio_stream_get_timing,
	.cm119_mixer_paths_resolve = fake_audio_cm119_mixer_paths_resolve,
};

/** @brief Complete fake GPIO ABI surface; only the stable identity probe is exercised. */
static const struct rptadv_gpio_adapter_descriptor fake_gpio = {
	.struct_size = sizeof(fake_gpio),
	.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	.capability_name = "test-gpio",
	.device_probe = fake_gpio_probe,
	.device_open = fake_gpio_open,
	.device_publish_outputs = fake_gpio_publish,
	.device_service = fake_gpio_service,
	.device_get_inputs = fake_gpio_inputs,
	.device_get_stats = fake_gpio_stats,
	.device_close = fake_gpio_close,
	.device_discover = fake_gpio_discover,
	.device_read_eeprom = fake_gpio_read_eeprom,
	.device_write_eeprom = fake_gpio_write_eeprom,
	.parallel_open = fake_parallel_open,
	.parallel_publish_outputs = fake_parallel_publish,
	.parallel_service = fake_parallel_service,
	.parallel_control_write_data = fake_parallel_control,
	.parallel_get_inputs = fake_parallel_inputs,
	.parallel_get_stats = fake_parallel_stats,
	.parallel_close = fake_parallel_close,
	.device_schedule_inverting_pulse = fake_gpio_schedule_inverting_pulse,
	.parallel_schedule_inverting_pulse = fake_parallel_schedule_inverting_pulse,
	.parallel_set_binary_channel = fake_parallel_set_binary_channel,
	.parallel_program_rtx = fake_parallel_program_rtx,
	.parallel_clear_rtx_transmit = fake_parallel_clear_rtx_transmit,
};

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

/** @brief Reset fake adapter state before each independent configuration case. */
static void reset_fake_adapters(void)
{
	audio_operation_result = RPTADV_AUDIO_OK;
	parallel_operation_result = RPTADV_GPIO_OK;
	selector_result = RPTADV_AUDIO_OK;
	selector_selection_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	selector_input_index = 6;
	selector_output_index = 7;
	selector_match_struct_size = sizeof(struct rptadv_audio_usb_device_match);
	gpio_probe_result = RPTADV_GPIO_OK;
	gpio_probe_present = 1U;
	gpio_probe_serial = NULL;
	gpio_open_result = RPTADV_GPIO_OK;
	selector_calls = 0U;
	gpio_probe_calls = 0U;
	memset(&selector_request, 0, sizeof(selector_request));
	selector_topology = "3-1:1.0";
	selector_serial = "CM119-A";
	selector_match_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	selector_selection_struct_size = sizeof(struct rptadv_audio_usb_device_selection);
	mixer_open_calls = 0U;
	mixer_normalized = 500U;
	mixer_switch = 1U;
	mixer_paths_calls = 0U;
	mixer_paths_topology = NULL;
	mixer_paths_result = RPTADV_AUDIO_OK;
	mixer_paths_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	mixer_paths_malformed_mode = 0U;
	stream_timing_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	stream_stats_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	gpio_scheduled_pulse_calls = 0U;
	parallel_scheduled_pulse_calls = 0U;
	parallel_binary_channel_calls = 0U;
	parallel_binary_channel = 0U;
	parallel_binary_channel_result = RPTADV_GPIO_OK;
	parallel_rtx_program_calls = 0U;
	parallel_rtx_rx_frequency_hz = 0U;
	parallel_rtx_tx_frequency_hz = 0U;
	parallel_rtx_transmitting = 0U;
	parallel_rtx_high_power = 0U;
	parallel_rtx_program_result = RPTADV_GPIO_OK;
	parallel_rtx_clear_calls = 0U;
	parallel_rtx_clear_result = RPTADV_GPIO_OK;
	stream_create_calls = 0U;
	gpio_open_calls = 0U;
	gpio_publish_calls = 0U;
	gpio_service_calls = 0U;
	gpio_eeprom_read_calls = 0U;
	gpio_eeprom_write_calls = 0U;
	gpio_close_calls = 0U;
	memset(&stream_create_config, 0, sizeof(stream_create_config));
	memset(&gpio_published_action, 0, sizeof(gpio_published_action));
	memset(&gpio_input_snapshot, 0, sizeof(gpio_input_snapshot));
	memset(&gpio_stats_snapshot, 0, sizeof(gpio_stats_snapshot));
	gpio_input_snapshot.struct_size = sizeof(gpio_input_snapshot);
	gpio_input_snapshot.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	gpio_input_snapshot.cor_active = 1U;
	gpio_input_snapshot.ctcss_active = 1U;
	gpio_stats_snapshot.struct_size = sizeof(gpio_stats_snapshot);
	gpio_stats_snapshot.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	gpio_stats_snapshot.ptt_applied = 1U;
	gpio_stats_snapshot.online = 1U;
	gpio_publish_result = RPTADV_GPIO_OK;
	gpio_service_result = RPTADV_GPIO_OK;
	gpio_eeprom_read_result = RPTADV_GPIO_OK;
	gpio_eeprom_write_result = RPTADV_GPIO_OK;
	gpio_eeprom_read_request_struct_size = 0U;
	memset(&gpio_eeprom_read_image, 0, sizeof(gpio_eeprom_read_image));
	gpio_eeprom_read_image.struct_size = sizeof(gpio_eeprom_read_image);
	gpio_eeprom_read_image.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	gpio_eeprom_read_image.magic_valid = 1U;
	gpio_eeprom_read_image.checksum_valid = 1U;
	gpio_eeprom_read_image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] =
		RPTADV_GPIO_CM119_EEPROM_MAGIC;
	memset(&gpio_eeprom_written_image, 0, sizeof(gpio_eeprom_written_image));
	memset(&gpio_scheduled_pulse, 0, sizeof(gpio_scheduled_pulse));
	memset(&parallel_scheduled_pulse, 0, sizeof(parallel_scheduled_pulse));
	gpio_scheduled_pulse_result = RPTADV_GPIO_OK;
	parallel_scheduled_pulse_result = RPTADV_GPIO_OK;
}

/** @brief Make a valid exact-selection configuration. */
static struct usbradioplus_hardware_adapter_config exact_config(void)
{
	struct usbradioplus_hardware_adapter_config config = {
		.struct_size = sizeof(config),
		.abi_version = USBRADIOPLUS_HARDWARE_ADAPTER_ABI_VERSION,
		.device_selection_policy = RPTADV_AUDIO_USB_SELECTION_EXACT,
		.device_identifier = "hw:4,0",
		.usb_serial = "CM119-A",
		.cm119_profile = RPTADV_GPIO_CM119_DUDEUSB,
		.input_device_channels = 1U,
		.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS,
	};

	return config;
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
	return 0;
}
