/**
 * @file
 * @brief USBRadioPlus hardware adapter composition.
 *
 * Control-plane composition of released PortAudio/ALSA and GPIO adapters.
 */

#include "usbradioplus_hardware_adapter.h"

#include <stddef.h>
#include <string.h>

/** @brief End byte required to read one descriptor member safely. */
#define URP_DESCRIPTOR_MEMBER_END(type, member)                                                    \
	(offsetof(type, member) + sizeof(((type *)0)->member))

/** @brief Required stable audio descriptor surface used by this facade. */
#define URP_AUDIO_DESCRIPTOR_MINIMUM                                                               \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_audio_adapter_descriptor, stream_get_timing)

/** @brief Descriptor size that makes CM119 semantic mixer discovery available. */
#define URP_AUDIO_CM119_MIXER_PATHS_MINIMUM                                                        \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_audio_adapter_descriptor, cm119_mixer_paths_resolve)

/** @brief Required stable GPIO descriptor surface used by this facade. */
#define URP_GPIO_DESCRIPTOR_MINIMUM                                                                \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor, parallel_close)

/** @brief Descriptor size that makes CM119 scheduled pulses available. */
#define URP_GPIO_DEVICE_SCHEDULE_PULSE_MINIMUM                                                     \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor,                           \
				  device_schedule_inverting_pulse)

/** @brief Descriptor size that makes parallel scheduled pulses available. */
#define URP_GPIO_PARALLEL_SCHEDULE_PULSE_MINIMUM                                                   \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor,                           \
				  parallel_schedule_inverting_pulse)

/** @brief Descriptor size that makes binary parallel channel selection available. */
#define URP_GPIO_PARALLEL_BINARY_CHANNEL_MINIMUM                                                   \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor,                           \
				  parallel_set_binary_channel)

/** @brief Descriptor size that makes legacy RTX parallel programming available. */
#define URP_GPIO_PARALLEL_RTX_PROGRAM_MINIMUM                                                      \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor, parallel_program_rtx)

/** @brief Descriptor size that makes immediate legacy RTX transmit clear available. */
#define URP_GPIO_PARALLEL_RTX_CLEAR_MINIMUM                                                        \
	URP_DESCRIPTOR_MEMBER_END(struct rptadv_gpio_adapter_descriptor,                           \
				  parallel_clear_rtx_transmit)

/**
 * @brief Return whether a descriptor has the base ABI expected by this facade.
 * @param audio Candidate audio adapter descriptor.
 * @return Nonzero when the descriptor provides every base facade operation.
 */
static int hardware_audio_descriptor_valid(const struct rptadv_audio_adapter_descriptor *audio)
{
	return audio && audio->struct_size >= URP_AUDIO_DESCRIPTOR_MINIMUM &&
	       audio->abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION &&
	       audio->usb_device_resolve && audio->stream_create && audio->stream_start &&
	       audio->stream_stop && audio->stream_destroy && audio->stream_get_stats &&
	       audio->stream_get_timing && audio->mixer_create_for_usb_interface &&
	       audio->mixer_get_range_steps && audio->mixer_get_steps && audio->mixer_set_steps &&
	       audio->mixer_get_normalized && audio->mixer_set_normalized &&
	       audio->mixer_get_switch && audio->mixer_set_switch && audio->mixer_destroy &&
	       audio->usb_device_select;
}

/**
 * @brief Return whether an append-only audio descriptor member is available.
 * @param audio Non-NULL audio descriptor checked by the public entry point.
 * @param member_end End offset of the required descriptor member.
 * @return Nonzero when the descriptor contains the member.
 */
static int hardware_audio_descriptor_has_member(const struct rptadv_audio_adapter_descriptor *audio,
						size_t member_end)
{
	return audio->struct_size >= member_end;
}

/**
 * @brief Return whether a descriptor has the ABI expected by this facade.
 * @param gpio Candidate GPIO adapter descriptor.
 * @return Nonzero when the descriptor provides every base facade operation.
 */
static int hardware_gpio_descriptor_valid(const struct rptadv_gpio_adapter_descriptor *gpio)
{
	return gpio && gpio->struct_size >= URP_GPIO_DESCRIPTOR_MINIMUM &&
	       gpio->abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION && gpio->device_probe &&
	       gpio->device_open && gpio->device_publish_outputs && gpio->device_service &&
	       gpio->device_get_inputs && gpio->device_get_stats && gpio->device_close &&
	       gpio->device_discover && gpio->device_read_eeprom && gpio->device_write_eeprom &&
	       gpio->parallel_open && gpio->parallel_publish_outputs && gpio->parallel_service &&
	       gpio->parallel_control_write_data && gpio->parallel_get_inputs &&
	       gpio->parallel_get_stats && gpio->parallel_close;
}

/**
 * @brief Return whether an append-only GPIO descriptor member is available.
 * @param gpio Non-NULL GPIO descriptor checked by the public entry point.
 * @param member_end End offset of the required descriptor member.
 * @return Nonzero when the descriptor contains the member.
 */
static int hardware_gpio_descriptor_has_member(const struct rptadv_gpio_adapter_descriptor *gpio,
					       size_t member_end)
{
	return gpio->struct_size >= member_end;
}

/**
 * @brief Validate one semantic CM119 mixer path returned by the audio adapter.
 * @param path Non-NULL element of the bounded returned ALSA path array.
 * @param direction Required capture or playback direction.
 * @param required_capabilities Capabilities required by the path class.
 * @return Nonzero when the path is complete and usable.
 */
static int hardware_cm119_mixer_path_valid(const struct rptadv_audio_cm119_mixer_path *path,
					   uint32_t direction, uint32_t required_capabilities)
{
	return path->element[0] && memchr(path->element, '\0', sizeof(path->element)) != NULL &&
	       path->channel <= RPTADV_AUDIO_MIXER_CHANNEL_RIGHT && path->direction == direction &&
	       (path->capabilities & required_capabilities) == required_capabilities;
}

/**
 * @brief Validate one homogeneous semantic path array returned by the adapter.
 * @param paths Path array to inspect.
 * @param count Number of populated entries.
 * @param direction Required capture or playback direction.
 * @param required_capabilities Capabilities required by the path class.
 * @return Nonzero when the bounded array contains only usable paths.
 */
static int hardware_cm119_mixer_path_group_valid(const struct rptadv_audio_cm119_mixer_path *paths,
						 uint32_t count, uint32_t direction,
						 uint32_t required_capabilities)
{
	uint32_t index;

	if (count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY)
		return 0;
	for (index = 0U; index < count; ++index) {
		if (!hardware_cm119_mixer_path_valid(&paths[index], direction,
						     required_capabilities))
			return 0;
	}
	return 1;
}

/**
 * @brief Validate all semantic CM119 mixer-path classes returned by an adapter.
 * @param paths Non-NULL returned classification checked by the public entry point.
 * @return Nonzero when every populated class preserves its documented semantics.
 */
static int hardware_cm119_mixer_paths_valid(const struct rptadv_audio_cm119_mixer_paths *paths)
{
	return paths->struct_size >= sizeof(*paths) &&
	       paths->abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION &&
	       paths->rx_capture_path_count != 0U && paths->tx_playback_path_count != 0U &&
	       hardware_cm119_mixer_path_group_valid(
		       paths->rx_capture_paths, paths->rx_capture_path_count,
		       RPTADV_AUDIO_MIXER_CAPTURE, RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME) &&
	       hardware_cm119_mixer_path_group_valid(
		       paths->tx_playback_paths, paths->tx_playback_path_count,
		       RPTADV_AUDIO_MIXER_PLAYBACK, RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME) &&
	       hardware_cm119_mixer_path_group_valid(
		       paths->sidetone_paths, paths->sidetone_path_count,
		       RPTADV_AUDIO_MIXER_PLAYBACK, RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME) &&
	       hardware_cm119_mixer_path_group_valid(paths->rx_compatibility_switch_paths,
						     paths->rx_compatibility_switch_path_count,
						     RPTADV_AUDIO_MIXER_PLAYBACK,
						     RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH);
}

/**
 * @brief Copy a required NUL-terminated configuration string into fixed storage.
 * @param destination Non-NULL fixed destination array.
 * @param capacity Nonzero sizeof the destination array in bytes.
 * @param source Non-NULL returned identity array containing the required text.
 * @return Nonzero on a complete bounded copy.
 */
static int hardware_copy_required_text(char *destination, size_t capacity, const char *source)
{
	size_t length;

	if (!*source)
		return 0;
	length = strlen(source);
	if (length >= capacity)
		return 0;
	memcpy(destination, source, length + 1U);
	return 1;
}

/**
 * @brief Copy an optional NUL-terminated configuration string into fixed storage.
 * @param destination Non-NULL fixed destination array.
 * @param capacity Nonzero sizeof the destination array in bytes.
 * @param source Non-NULL returned identity array, optionally empty.
 * @return Nonzero on a complete bounded copy or an accepted omission.
 */
static int hardware_copy_optional_text(char *destination, size_t capacity, const char *source)
{
	size_t length;

	if (!*source) {
		destination[0] = '\0';
		return 1;
	}
	length = strlen(source);
	if (length >= capacity)
		return 0;
	memcpy(destination, source, length + 1U);
	return 1;
}

/**
 * @brief Return a nonempty configuration string, or NULL when it is omitted.
 * @param value Candidate optional configuration text.
 * @return @p value when nonempty, otherwise NULL.
 */
static const char *hardware_optional_text(const char *value)
{
	return value && *value ? value : NULL;
}

/**
 * @brief Resolve any legacy-compatible selector to the shared CM119 identity.
 * @param adapter Composition receiving the canonical identity and endpoints.
 * @param config Immutable channel selection.
 * @param audio Valid audio adapter descriptor.
 * @return One facade result code.
 *
 * The audio adapter owns the legacy `devstr` interpretation.  GPIO sees only
 * the canonical topology returned here, which preserves one-device ownership
 * across ALSA, PortAudio, and HID without retaining an ASL helper dependency.
 */
static enum usbradioplus_hardware_adapter_result
hardware_resolve_identity(struct usbradioplus_hardware_adapter *adapter,
			  const struct usbradioplus_hardware_adapter_config *config,
			  const struct rptadv_audio_adapter_descriptor *audio)
{
	const char *topology = hardware_optional_text(config->usb_port_path);
	const char *identifier = hardware_optional_text(config->device_identifier);
	const char *serial = hardware_optional_text(config->usb_serial);
	struct rptadv_audio_usb_device_selector selector = {
		.struct_size = sizeof(selector),
		.selection_policy = config->device_selection_policy,
		.input_device_channels = config->input_device_channels,
		.output_device_channels = config->output_device_channels,
	};
	struct rptadv_audio_usb_device_match match = {
		.struct_size = sizeof(match),
	};
	enum rptadv_audio_result result;

	if (config->device_selection_policy != RPTADV_AUDIO_USB_SELECTION_EXACT &&
	    config->device_selection_policy !=
		    RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (topology && identifier)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (config->device_selection_policy == RPTADV_AUDIO_USB_SELECTION_EXACT && !topology &&
	    !identifier && !serial)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (config->device_selection_policy ==
		    RPTADV_AUDIO_USB_SELECTION_AUTOMATIC_LOWEST_ALSA_CARD &&
	    (topology || identifier || serial))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	selector.device_identifier = topology ? topology : identifier;
	selector.usb_serial = serial;
	result = audio->usb_device_select(&selector, &match);
	if (result != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (match.abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION ||
	    match.struct_size < sizeof(match) ||
	    match.selection.struct_size < sizeof(match.selection) ||
	    !hardware_copy_required_text(adapter->usb_port_path, sizeof(adapter->usb_port_path),
					 match.usb_interface_path) ||
	    match.selection.abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION ||
	    match.selection.input_device_index < 0 || match.selection.output_device_index < 0)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (serial && (!match.usb_serial[0] || strcmp(serial, match.usb_serial) != 0))
		return USBRADIOPLUS_HARDWARE_ADAPTER_IDENTITY_MISMATCH;
	if (!hardware_copy_optional_text(adapter->usb_serial, sizeof(adapter->usb_serial),
					 match.usb_serial))
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	adapter->audio_selection = match.selection;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_validate(const struct rptadv_audio_adapter_descriptor *audio,
				       const struct rptadv_gpio_adapter_descriptor *gpio)
{
	if (!hardware_audio_descriptor_valid(audio) || !hardware_gpio_descriptor_valid(gpio))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_prepare(struct usbradioplus_hardware_adapter *adapter,
				      const struct usbradioplus_hardware_adapter_config *config,
				      const struct rptadv_audio_adapter_descriptor *audio,
				      const struct rptadv_gpio_adapter_descriptor *gpio)
{
	struct rptadv_gpio_device_config gpio_config = {0};
	enum usbradioplus_hardware_adapter_result facade_result;
	enum rptadv_gpio_result gpio_result;

	if (!adapter || !config ||
	    config->struct_size < sizeof(struct usbradioplus_hardware_adapter_config) ||
	    config->abi_version != USBRADIOPLUS_HARDWARE_ADAPTER_ABI_VERSION ||
	    (config->input_device_channels != 1U && config->input_device_channels != 2U) ||
	    (config->output_device_channels != 1U && config->output_device_channels != 2U))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (usbradioplus_hardware_adapter_validate(audio, gpio) != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;

	memset(&adapter->audio_selection, 0, sizeof(adapter->audio_selection));
	memset(&adapter->gpio_info, 0, sizeof(adapter->gpio_info));
	adapter->gpio_info.struct_size = sizeof(adapter->gpio_info);
	facade_result = hardware_resolve_identity(adapter, config, audio);
	if (facade_result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return facade_result;
	gpio_config.struct_size = sizeof(gpio_config);
	gpio_config.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	gpio_config.usb_port_path = adapter->usb_port_path;
	gpio_config.profile = config->cm119_profile;
	gpio_config.ptt_inverted = !!config->ptt_inverted;
	gpio_config.gpio_output_enable_mask = config->gpio_output_enable_mask;
	gpio_config.gpio_output_initial_mask = config->gpio_output_initial_mask;
	gpio_result = gpio->device_probe(&gpio_config, &adapter->gpio_info);
	if (gpio_result != RPTADV_GPIO_OK || !adapter->gpio_info.present)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	if (adapter->usb_serial[0] && strcmp(adapter->usb_serial, adapter->gpio_info.serial) != 0)
		return USBRADIOPLUS_HARDWARE_ADAPTER_IDENTITY_MISMATCH;

	adapter->audio = audio;
	adapter->gpio = gpio;
	adapter->input_device_channels = config->input_device_channels;
	adapter->output_device_channels = config->output_device_channels;
	adapter->cm119_profile = config->cm119_profile;
	adapter->ptt_inverted = !!config->ptt_inverted;
	adapter->gpio_output_enable_mask = config->gpio_output_enable_mask;
	adapter->gpio_output_initial_mask = config->gpio_output_initial_mask;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_prepare_released(
	struct usbradioplus_hardware_adapter *adapter,
	const struct usbradioplus_hardware_adapter_config *config)
{
	return usbradioplus_hardware_adapter_prepare(adapter, config,
						     rptadv_portaudio_alsa_adapter_descriptor(),
						     rptadv_gpio_adapter_descriptor());
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_open_gpio(struct usbradioplus_hardware_adapter *adapter)
{
	struct rptadv_gpio_device_config config = {0};

	if (!adapter || !adapter->gpio || !adapter->audio)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	config.struct_size = sizeof(config);
	config.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	config.usb_port_path = adapter->usb_port_path;
	config.profile = adapter->cm119_profile;
	config.ptt_inverted = adapter->ptt_inverted;
	config.gpio_output_enable_mask = adapter->gpio_output_enable_mask;
	config.gpio_output_initial_mask = adapter->gpio_output_initial_mask;
	if (adapter->gpio->device_open(&config, &adapter->gpio_device) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_publish_gpio(struct usbradioplus_hardware_adapter *adapter,
					   const struct rptadv_gpio_output_action *action)
{
	if (!adapter || !adapter->gpio || !adapter->gpio_device || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->device_publish_outputs(adapter->gpio_device, action) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action *action)
{
	if (!adapter || !adapter->gpio || !adapter->gpio_device || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_gpio_descriptor_has_member(adapter->gpio,
						 URP_GPIO_DEVICE_SCHEDULE_PULSE_MINIMUM) ||
	    !adapter->gpio->device_schedule_inverting_pulse)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->gpio->device_schedule_inverting_pulse(adapter->gpio_device, action) !=
	    RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_service_gpio(struct usbradioplus_hardware_adapter *adapter,
					   struct rptadv_gpio_input_snapshot *inputs,
					   struct rptadv_gpio_device_stats *stats)
{
	if (!adapter || !adapter->gpio || !adapter->gpio_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->device_service(adapter->gpio_device) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	if (inputs) {
		if (inputs->struct_size < sizeof(*inputs) ||
		    adapter->gpio->device_get_inputs(adapter->gpio_device, inputs) !=
			    RPTADV_GPIO_OK)
			return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	}
	if (stats) {
		if (stats->struct_size < sizeof(*stats) ||
		    adapter->gpio->device_get_stats(adapter->gpio_device, stats) != RPTADV_GPIO_OK)
			return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_read_eeprom(struct usbradioplus_hardware_adapter *adapter,
					  struct rptadv_gpio_eeprom_image *image)
{
	if (!adapter || !adapter->gpio || !adapter->gpio_device || !image ||
	    image->struct_size < sizeof(*image))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->device_read_eeprom(adapter->gpio_device, image) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_write_eeprom(struct usbradioplus_hardware_adapter *adapter,
					   struct rptadv_gpio_eeprom_image *image)
{
	if (!adapter || !adapter->gpio || !adapter->gpio_device || !image ||
	    image->struct_size < sizeof(*image) ||
	    image->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->device_write_eeprom(adapter->gpio_device, image) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_create(const struct usbradioplus_hardware_adapter *adapter,
					    const struct rptadv_audio_stream_config *config,
					    struct rptadv_audio_stream **stream)
{
	struct rptadv_audio_stream_config selected;

	if (stream)
		*stream = NULL;
	if (!adapter || !adapter->audio || !config || !stream ||
	    config->struct_size < sizeof(*config) ||
	    config->abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION ||
	    config->input_device_channels != adapter->input_device_channels ||
	    config->output_device_channels != adapter->output_device_channels)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	selected = *config;
	selected.input_device_index = adapter->audio_selection.input_device_index;
	selected.output_device_index = adapter->audio_selection.output_device_index;
	if (adapter->audio->stream_create(&selected, stream) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_get_stats(const struct usbradioplus_hardware_adapter *adapter,
					       const struct rptadv_audio_stream *stream,
					       struct rptadv_audio_stream_stats *statistics)
{
	if (!adapter || !adapter->audio || !stream || !statistics ||
	    statistics->struct_size < sizeof(*statistics))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->audio->stream_get_stats(stream, statistics) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (statistics->abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_get_timing(const struct usbradioplus_hardware_adapter *adapter,
						const struct rptadv_audio_stream *stream,
						struct rptadv_audio_stream_timing *timing)
{
	if (!adapter || !adapter->audio || !stream || !timing ||
	    timing->struct_size < sizeof(*timing))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->audio->stream_get_timing(stream, timing) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (timing->abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(
	const struct usbradioplus_hardware_adapter *adapter,
	struct rptadv_audio_cm119_mixer_paths *paths)
{
	if (!adapter || !adapter->audio || !paths || paths->struct_size < sizeof(*paths))
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_audio_descriptor_has_member(adapter->audio,
						  URP_AUDIO_CM119_MIXER_PATHS_MINIMUM) ||
	    !adapter->audio->cm119_mixer_paths_resolve)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->audio->cm119_mixer_paths_resolve(adapter->usb_port_path, paths) !=
	    RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (!hardware_cm119_mixer_paths_valid(paths))
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_open(
	const struct usbradioplus_hardware_adapter *adapter,
	const struct usbradioplus_hardware_adapter_mixer_config *config,
	struct usbradioplus_hardware_adapter_mixer *mixer)
{
	struct rptadv_audio_usb_mixer_config selection = {0};

	if (!adapter || !adapter->audio || !config || !mixer ||
	    config->struct_size < sizeof(*config) || !config->element || !*config->element)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->mixer)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	selection.struct_size = sizeof(selection);
	selection.usb_interface_path = adapter->usb_port_path;
	selection.element = config->element;
	selection.element_index = config->element_index;
	selection.channel = config->channel;
	selection.direction = config->direction;
	if (adapter->audio->mixer_create_for_usb_interface(&selection, &mixer->mixer) !=
	    RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	mixer->audio = adapter->audio;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_set_legacy_level(
	struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t value)
{
	int64_t minimum;
	int64_t maximum;
	int64_t steps;

	if (!mixer || !mixer->audio || !mixer->mixer ||
	    value > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->audio->mixer_get_range_steps(mixer->mixer, &minimum, &maximum) !=
		    RPTADV_AUDIO_OK ||
	    minimum > maximum || maximum < 0)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	/* Preserve the former RX/DAC floor(value * maximum / 1000), including
	 * its below-maximum setting 999. Split the product to avoid overflow. */
	steps = (maximum / 1000) * value + ((maximum % 1000) * value) / 1000;
	if (steps < minimum ||
	    mixer->audio->mixer_set_steps(mixer->mixer, steps) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_set_normalized(
	struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t value)
{
	if (!mixer || !mixer->audio || !mixer->mixer ||
	    value > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->audio->mixer_set_normalized(mixer->mixer, value) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_get_normalized(
	const struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t *value)
{
	if (!mixer || !mixer->audio || !mixer->mixer || !value)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->audio->mixer_get_normalized(mixer->mixer, value) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_mixer_set_switch(struct usbradioplus_hardware_adapter_mixer *mixer,
					       uint32_t enabled)
{
	if (!mixer || !mixer->audio || !mixer->mixer || enabled > 1U)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->audio->mixer_set_switch(mixer->mixer, enabled) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_get_switch(
	const struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t *enabled)
{
	if (!mixer || !mixer->audio || !mixer->mixer || !enabled)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (mixer->audio->mixer_get_switch(mixer->mixer, enabled) != RPTADV_AUDIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

void usbradioplus_hardware_adapter_mixer_close(struct usbradioplus_hardware_adapter_mixer *mixer)
{
	if (!mixer)
		return;
	if (mixer->audio && mixer->mixer)
		mixer->audio->mixer_destroy(mixer->mixer);
	mixer->audio = NULL;
	mixer->mixer = NULL;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_open_parallel(struct usbradioplus_hardware_adapter *adapter,
					    const struct rptadv_gpio_parallel_config *config)
{
	if (!adapter || !adapter->gpio || !config || config->struct_size < sizeof(*config) ||
	    config->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->parallel_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	if (adapter->gpio->parallel_open(config, &adapter->parallel_device) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_publish_parallel(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_parallel_output_action *action)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->parallel_publish_outputs(adapter->parallel_device, action) !=
	    RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_parallel_scheduled_inverting_pulse_action *action)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device || !action ||
	    action->struct_size < sizeof(*action) ||
	    action->abi_version != RPTADV_GPIO_ADAPTER_ABI_VERSION)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_gpio_descriptor_has_member(adapter->gpio,
						 URP_GPIO_PARALLEL_SCHEDULE_PULSE_MINIMUM) ||
	    !adapter->gpio->parallel_schedule_inverting_pulse)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->gpio->parallel_schedule_inverting_pulse(adapter->parallel_device, action) !=
	    RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_set_parallel_binary_channel(
	struct usbradioplus_hardware_adapter *adapter, uint8_t channel)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_gpio_descriptor_has_member(adapter->gpio,
						 URP_GPIO_PARALLEL_BINARY_CHANNEL_MINIMUM) ||
	    !adapter->gpio->parallel_set_binary_channel)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->gpio->parallel_set_binary_channel(adapter->parallel_device, channel) !=
	    RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_program_parallel_rtx(
	struct usbradioplus_hardware_adapter *adapter, uint32_t rx_frequency_hz,
	uint32_t tx_frequency_hz, uint32_t transmitting, uint32_t high_power)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_gpio_descriptor_has_member(adapter->gpio,
						 URP_GPIO_PARALLEL_RTX_PROGRAM_MINIMUM) ||
	    !adapter->gpio->parallel_program_rtx)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->gpio->parallel_program_rtx(adapter->parallel_device, rx_frequency_hz,
						tx_frequency_hz, transmitting,
						high_power) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(
	struct usbradioplus_hardware_adapter *adapter)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!hardware_gpio_descriptor_has_member(adapter->gpio,
						 URP_GPIO_PARALLEL_RTX_CLEAR_MINIMUM) ||
	    !adapter->gpio->parallel_clear_rtx_transmit)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER;
	if (adapter->gpio->parallel_clear_rtx_transmit(adapter->parallel_device) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_service_parallel(struct usbradioplus_hardware_adapter *adapter,
					       struct rptadv_gpio_parallel_input_snapshot *inputs,
					       struct rptadv_gpio_parallel_stats *stats)
{
	if (!adapter || !adapter->gpio || !adapter->parallel_device)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (adapter->gpio->parallel_service(adapter->parallel_device) != RPTADV_GPIO_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	if (inputs) {
		if (inputs->struct_size < sizeof(*inputs) ||
		    adapter->gpio->parallel_get_inputs(adapter->parallel_device, inputs) !=
			    RPTADV_GPIO_OK)
			return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	}
	if (stats) {
		if (stats->struct_size < sizeof(*stats) ||
		    adapter->gpio->parallel_get_stats(adapter->parallel_device, stats) !=
			    RPTADV_GPIO_OK)
			return USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

void usbradioplus_hardware_adapter_close(struct usbradioplus_hardware_adapter *adapter)
{
	if (!adapter)
		return;
	if (adapter->gpio && adapter->parallel_device)
		adapter->gpio->parallel_close(adapter->parallel_device);
	if (adapter->gpio && adapter->gpio_device)
		adapter->gpio->device_close(adapter->gpio_device);
	memset(adapter, 0, sizeof(*adapter));
}
