/** @file
 * @brief Scriptable released-adapter fixtures shared by facade and channel tests.
 */

#ifndef USBRADIOPLUS_HARDWARE_ADAPTER_FIXTURE_H
#define USBRADIOPLUS_HARDWARE_ADAPTER_FIXTURE_H

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
/** @brief Script the optional sidetone and mono/stereo transmit path layouts. */
static uint32_t mixer_paths_sidetone_count, mixer_paths_tx_count;
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
/** @brief Optional raw stream capture target used by the channel status display. */
static uint64_t stream_stats_capture_target;
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
/** @brief Scripted mixer step range and last applied integer step. */
static int64_t mixer_step_minimum, mixer_step_maximum, mixer_step_value;
/** @brief Independent failure when applying the integer mixer step. */
static enum rptadv_audio_result mixer_step_result;
/** @brief One-based mixer write failure used to distinguish open and refresh stages. */
static unsigned int mixer_step_calls, mixer_step_fail_call;
/** @brief Independent failures while reading GPIO snapshots. */
static enum rptadv_gpio_result gpio_inputs_result, gpio_stats_result;
/** @brief Independent failures while reading parallel snapshots. */
static enum rptadv_gpio_result parallel_inputs_result, parallel_stats_result;
/** @brief Scripted raw parallel status bits for channel input translation. */
static uint32_t parallel_status_mask;

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
	statistics->callback_late_start_tolerance_ns = 1U;
	statistics->capture_ring_target_frames = stream_stats_capture_target;
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
	assert(config->element && *config->element);
	assert(config->element_index == 0U);
	assert(config->channel <= RPTADV_AUDIO_MIXER_CHANNEL_RIGHT);
	assert(config->direction <= RPTADV_AUDIO_MIXER_PLAYBACK);
	mixer_open_calls++;
	*mixer = (struct rptadv_audio_mixer *)&mixer_token;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_range_steps(const struct rptadv_audio_mixer *,
							     int64_t *minimum, int64_t *maximum)
{
	*minimum = mixer_step_minimum;
	*maximum = mixer_step_maximum;
	return audio_operation_result;
}

static enum rptadv_audio_result fake_audio_mixer_get_steps(const struct rptadv_audio_mixer *,
							   int64_t *)
{
	return RPTADV_AUDIO_UNSUPPORTED;
}

static enum rptadv_audio_result fake_audio_mixer_set_steps(struct rptadv_audio_mixer *,
							   int64_t value)
{
	mixer_step_value = value;
	if (++mixer_step_calls == mixer_step_fail_call)
		return RPTADV_AUDIO_UNSUPPORTED;
	return mixer_step_result;
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
	paths->tx_playback_path_count = mixer_paths_tx_count;
	paths->sidetone_path_count = mixer_paths_sidetone_count;
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
	paths->tx_playback_paths[1] = paths->tx_playback_paths[0];
	paths->tx_playback_paths[1].channel = RPTADV_AUDIO_MIXER_CHANNEL_RIGHT;
	strcpy(paths->sidetone_paths[0].element, "Mic");
	paths->sidetone_paths[0].channel = RPTADV_AUDIO_MIXER_CHANNEL_RIGHT;
	paths->sidetone_paths[0].direction = RPTADV_AUDIO_MIXER_PLAYBACK;
	paths->sidetone_paths[0].capabilities =
		RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME | RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH;
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
	return gpio_inputs_result;
}

static enum rptadv_gpio_result fake_gpio_stats(const struct rptadv_gpio_device *device,
					       struct rptadv_gpio_device_stats *snapshot)
{
	if (device != (const struct rptadv_gpio_device *)&gpio_device_token || !snapshot ||
	    snapshot->struct_size < sizeof(*snapshot))
		return RPTADV_GPIO_INVALID_ARGUMENT;
	*snapshot = gpio_stats_snapshot;
	return gpio_stats_result;
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

static enum rptadv_gpio_result
fake_parallel_inputs(const struct rptadv_gpio_parallel_device *,
		     struct rptadv_gpio_parallel_input_snapshot *snapshot)
{
	snapshot->abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION;
	snapshot->status_mask = parallel_status_mask;
	return parallel_inputs_result;
}

static enum rptadv_gpio_result fake_parallel_stats(const struct rptadv_gpio_parallel_device *,
						   struct rptadv_gpio_parallel_stats *)
{
	return parallel_stats_result;
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

/** @brief Reset fake adapter state before each independent configuration case. */
static void reset_fake_adapters(void)
{
	audio_operation_result = RPTADV_AUDIO_OK;
	mixer_step_minimum = 0;
	mixer_step_maximum = 100;
	mixer_step_value = -1;
	mixer_step_result = RPTADV_AUDIO_OK;
	mixer_step_calls = mixer_step_fail_call = 0U;
	gpio_inputs_result = RPTADV_GPIO_OK;
	gpio_stats_result = RPTADV_GPIO_OK;
	parallel_inputs_result = RPTADV_GPIO_OK;
	parallel_stats_result = RPTADV_GPIO_OK;
	parallel_status_mask = 0U;
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
	mixer_paths_sidetone_count = 1U;
	mixer_paths_tx_count = 1U;
	stream_timing_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	stream_stats_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	stream_stats_capture_target = 0U;
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

#endif
