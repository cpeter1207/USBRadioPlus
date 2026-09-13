/** @file
 * @brief Deterministic channel hardware lifecycle tests using shared fake descriptors.
 */

/** @brief Initialize one private channel and deterministic released adapter surface. */
static void channel_hardware_fixture(struct chan_usbradio_pvt *channel)
{
	memset(channel, 0, sizeof(*channel));
	channel->name = "hardware-boundary";
	channel->pttkick[0] = channel->pttkick[1] = -1;
	channel->plus_portaudio_poc = channel->plus_cm119_gpio_poc = 1;
	channel->plus_native_max_frames = URP_NATIVE_SAMPLES;
	channel->plus_deemphasis_corner_hz = 300.0;
	channel->plus_preemphasis_corner_hz = 300.0;
	channel->txmixa = TX_OUT_VOICE;
	channel->rxctcssadj = 1.0F;
	reset_fake_adapters();
	mixer_paths_tx_count = 2U;
	direct_adapter_fixtures_enabled = 1;
	direct_audio_descriptor_override = NULL;
	direct_gpio_descriptor_override = NULL;
	haspp = 0;
	usbradioplus_test_set_parallel_owner(NULL);
}

/** @brief Release all private resources without any physical hardware calls. */
static void channel_hardware_cleanup(struct chan_usbradio_pvt *channel)
{
	cm119_gpio_poc_stop(channel);
	hidthread_close_pttkick(channel);
	if (channel->radio) {
		assert(!urp_radio_destroy(channel->radio));
		channel->radio = NULL;
	}
	usbradioplus_dsp_destroy(channel);
	usbradio_default.next = NULL;
	direct_adapter_fixtures_enabled = 0;
	direct_audio_descriptor_override = NULL;
	direct_gpio_descriptor_override = NULL;
	haspp = 0;
	usbradioplus_test_set_parallel_owner(NULL);
}

/** @brief Preserve identity reservations, selector rules, and fail-closed mixer setup. */
static void test_hardware_identity_and_mixer_helpers(void)
{
	struct chan_usbradio_pvt channel, other = {.name = "other"};

	channel_hardware_fixture(&channel);
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(channel.plus_hardware_adapter_prepared);
	assert(!cm119_gpio_poc_reserve_device_identity(&channel));
	assert(channel.usbass && channel.micmax == 999 && channel.spkrmax == 999);
	other.usbass = 1;
	strcpy(other.plus_hardware_adapter.usb_port_path, selector_topology);
	usbradio_default.next = &other;
	assert(cm119_gpio_poc_reserve_device_identity(&channel) == -1);
	other.usbass = 0;
	assert(!cm119_gpio_poc_reserve_device_identity(&channel));
	other.usbass = 1;
	strcpy(other.plus_hardware_adapter.usb_port_path, "3-2");
	assert(!cm119_gpio_poc_reserve_device_identity(&channel));
	usbradio_default.next = &channel;
	assert(!cm119_gpio_poc_reserve_device_identity(&channel));
	assert(!cm119_gpio_poc_open_mixer(&channel));
	assert(channel.micplaymax == 999);
	assert(!cm119_gpio_poc_set_rx_mixer(&channel, 250));
	assert(mixer_step_value == 25);
	assert(cm119_gpio_poc_set_rx_mixer(NULL, 0) == -1);
	assert(cm119_gpio_poc_set_rx_mixer(&channel, -1) == -1);
	assert(cm119_gpio_poc_set_rx_mixer(&channel, 1000) == -1);
	mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
	atomic_store(&channel.plus_hardware_online, 1);
	assert(cm119_gpio_poc_set_rx_mixer(&channel, 300) == -1);
	assert(!atomic_load(&channel.plus_hardware_online));
	assert(!channel.plus_hardware_mixer_poc.opened);
	assert(cm119_gpio_poc_set_rx_mixer(&channel, 300) == -1);
	mixer_step_result = RPTADV_AUDIO_OK;
	assert(!cm119_gpio_poc_open_mixer(&channel));
	mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
	assert(cm119_gpio_poc_apply_mixer(&channel) == -1);
	mixer_step_result = RPTADV_AUDIO_OK;
	mixer_paths_sidetone_count = 0U;
	channel.duplex3 = 500;
	channel.duplex3mode = DUPLEX3_MODE_HARDWARE;
	assert(cm119_gpio_poc_open_mixer(&channel) == -1);
	channel.duplex3 = 0;
	assert(!cm119_gpio_poc_open_mixer(&channel));
	assert(!channel.micplaymax);
	usbradioplus_hardware_mixer_poc_close(&channel.plus_hardware_mixer_poc);
	mixer_paths_result = RPTADV_AUDIO_UNSUPPORTED;
	assert(cm119_gpio_poc_open_mixer(&channel) == -1);
	channel.plus_hardware_adapter_prepared = 0;
	assert(cm119_gpio_poc_open_mixer(&channel) == -1);
	assert(cm119_gpio_poc_open_mixer(NULL) == -1);
	assert(cm119_gpio_poc_set_rx_mixer(&channel, 0) == -1);
	assert(cm119_gpio_poc_apply_mixer(&channel) == -1);
	cm119_gpio_poc_release_device_identity(&channel);
	assert(!channel.usbass && !channel.hasusb && !channel.devicenum);
	channel_hardware_cleanup(&channel);

	channel_hardware_fixture(&channel);
	strcpy(channel.devstr, "hw:4,0");
	strcpy(channel.serial, "CM119-A");
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(selector_request.selection_policy == RPTADV_AUDIO_USB_SELECTION_EXACT);
	strcpy(channel.plus_cm119_gpio_usb_port_path, "other-device");
	assert(cm119_gpio_poc_prepare_hardware_adapter(&channel) == -1);
	channel.devstr[0] = '\0';
	channel.plus_cm119_gpio_usb_port_path[0] = '\0';
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	strcpy(channel.plus_cm119_gpio_usb_port_path, selector_topology);
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	channel.serial[0] = '\0';
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	selector_result = RPTADV_AUDIO_UNSUPPORTED;
	assert(cm119_gpio_poc_prepare_hardware_adapter(&channel) == -1);
	channel_hardware_cleanup(&channel);
}

/** @brief Verify advisory wake-pipe failures and callback publication invariants. */
static void test_hardware_wake_and_publication_helpers(void)
{
	struct chan_usbradio_pvt channel;
	int event;

	channel_hardware_fixture(&channel);
	assert(!hidthread_parallel_ptt_mask(&channel));
	haspp = 1;
	channel.pps[2] = "ptt";
	channel.pps[3] = "PTT-inverted";
	channel.pps[4] = "out";
	assert(hidthread_parallel_ptt_mask(&channel) == 3U);
	assert(cm119_gpio_poc_parallel_requested(&channel));
	assert(!cm119_gpio_poc_parallel_requested(NULL));
	channel.plus_cm119_gpio_poc = 0;
	assert(!cm119_gpio_poc_parallel_requested(&channel));
	assert(cm119_gpio_poc_worker_validate(&channel));
	channel.plus_cm119_gpio_poc = 1;
	channel.plus_portaudio_poc = 0;
	assert(cm119_gpio_poc_worker_validate(&channel));
	channel.plus_portaudio_poc = 1;
	assert(!cm119_gpio_poc_worker_validate(&channel));
	haspp = 0;
	assert(!hidthread_open_pttkick(&channel));
	assert(!hidthread_open_pttkick(&channel));
	assert(cm119_gpio_poc_worker_wake_read_fd(&channel) == channel.pttkick[0]);
	assert(write(channel.pttkick[1], "xx", 2U) == 2);
	assert(!cm119_gpio_poc_worker_drain_wake(&channel));
	close(channel.pttkick[1]);
	channel.pttkick[1] = -1;
	assert(!cm119_gpio_poc_drain_pttkick(&channel));
	hidthread_close_pttkick(&channel);
	assert(cm119_gpio_poc_drain_pttkick(&channel) == -1);
	mock_pipe_failure = 1;
	assert(hidthread_open_pttkick(&channel) == -1);
	mock_pipe_failure = 0;
	for (int failure = 2; failure <= 4; failure += 2) {
		mock_fcntl_calls = 0;
		mock_fcntl_fail_call = failure;
		assert(hidthread_open_pttkick(&channel) == -1);
		assert(channel.pttkick[0] == -1 && channel.pttkick[1] == -1);
	}
	mock_fcntl_fail_call = 0;
	assert(cm119_gpio_poc_monotonic_milliseconds() > 0U);
	mock_monotonic_clock_failure = 1;
	assert(cm119_gpio_poc_monotonic_milliseconds() > 0U);
	mock_monotonic_clock_failure = 0;
	assert(!cm119_gpio_poc_worker_stop_requested(&channel));
	atomic_store(&channel.plus_hardware_stop_request, 1);
	assert(cm119_gpio_poc_worker_stop_requested(&channel));
	assert(!cm119_gpio_poc_worker_online(&channel));
	cm119_gpio_poc_worker_mark_online(&channel);
	assert(cm119_gpio_poc_worker_online(&channel));
	cm119_gpio_poc_worker_clear_published_state(&channel);
	assert(!cm119_gpio_poc_worker_online(&channel));
	for (event = USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED;
	     event <= USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED; ++event)
		cm119_gpio_poc_worker_report(&channel, event, EIO);
	cm119_gpio_poc_worker_report(&channel, (enum usbradioplus_cm119_gpio_poc_worker_event)99,
				     EIO);
	cm119_gpio_poc_parallel_input_event(NULL, 10U, 1);
	cm119_gpio_poc_parallel_input_event(&channel, 10U, 1);
	channel.owner = (struct ast_channel *)(uintptr_t)1;
	cm119_gpio_poc_parallel_input_event(&channel, 10U, 1);
	channel.owner = NULL;
	cm119_gpio_poc_worker_release_identity(&channel);
	cm119_gpio_poc_worker_stop_attempt(&channel);
	channel_hardware_cleanup(&channel);
}

/** @brief Accept a prepared fake stream without creating a PortAudio callback thread. */
static enum rptadv_audio_result channel_hardware_stream_start(struct rptadv_audio_stream *stream)
{
	assert(stream == (struct rptadv_audio_stream *)&stream_token);
	return RPTADV_AUDIO_OK;
}

/** @brief Fail the ppdev attempt while accepting its configured raw-I/O fallback. */
static enum rptadv_gpio_result
channel_hardware_parallel_fallback(const struct rptadv_gpio_parallel_config *config,
				   struct rptadv_gpio_parallel_device **device)
{
	if (config->transport == RPTADV_GPIO_PARALLEL_TRANSPORT_PPDEV)
		return RPTADV_GPIO_IO_ERROR;
	return fake_parallel_open(config, device);
}

/** @brief Exercise each ordered startup stage and successful adapter-owned shutdown. */
static void test_hardware_start_attempt_boundaries(void)
{
	struct chan_usbradio_pvt channel, other = {.name = "other"};
	struct rptadv_audio_adapter_descriptor audio = fake_audio;
	struct rptadv_gpio_adapter_descriptor gpio = fake_gpio;
	int scenario;

	for (scenario = 0; scenario < 16; ++scenario) {
		channel_hardware_fixture(&channel);
		other.usbass = 0;
		channel.plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT;
		channel.plus_app_rpt_samples = URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES;
		audio.stream_start = channel_hardware_stream_start;
		direct_audio_descriptor_override = &audio;
		if (scenario == 0)
			selector_result = RPTADV_AUDIO_UNSUPPORTED;
		else if (scenario == 1) {
			other.usbass = 1;
			strcpy(other.plus_hardware_adapter.usb_port_path, selector_topology);
			usbradio_default.next = &other;
		} else if (scenario == 2)
			mixer_paths_result = RPTADV_AUDIO_UNSUPPORTED;
		else if (scenario == 3)
			gpio_open_result = RPTADV_GPIO_IO_ERROR;
		else if (scenario == 4) {
			haspp = 1;
			strcpy(pport, "/dev/parport0");
			pbase = 0x378;
			parallel_operation_result = RPTADV_GPIO_IO_ERROR;
		} else if (scenario == 5)
			mock_pipe_failure = 1;
		else if (scenario == 6)
			fail_radio_state_allocation = 1;
		else if (scenario == 7)
			audio.stream_start = fake_audio_stream_start;
		else if (scenario == 9 || scenario == 10) {
			haspp = 1;
			strcpy(pport, "/dev/parport0");
			pbase = 0x378;
			channel.pps[2] = "ptt";
			channel.invertptt = 1;
			atomic_store(&channel.plus_radio_program_rx_frequency, 146000000U);
			usbradio_default.next = &channel;
			if (scenario == 10) {
				gpio.parallel_open = channel_hardware_parallel_fallback;
				direct_gpio_descriptor_override = &gpio;
			}
		}
		if (scenario >= 11) {
			haspp = scenario == 11 ? 2 : 1;
			strcpy(pport, "/dev/parport0");
			pbase = scenario == 12 ? 0 : 0x378;
			usbradio_default.next = &channel;
			if (scenario < 13)
				parallel_operation_result = RPTADV_GPIO_IO_ERROR;
			else if (scenario == 13)
				mixer_step_fail_call = 4U;
			else if (scenario == 14)
				usbradioplus_test_set_parallel_owner(&other);
			else
				atomic_store(&channel.plus_radio_program_generation, 1U);
		}
		assert(cm119_gpio_poc_worker_start_attempt(&channel) ==
		       (scenario < 8 || (scenario >= 11 && scenario <= 13) ? -1 : 0));
		mock_pipe_failure = 0;
		fail_radio_state_allocation = 0;
		if (scenario >= 8 && scenario <= 10) {
			assert(channel.radio && channel.plus_portaudio_stream);
			assert(!cm119_gpio_poc_worker_service(&channel));
		}
		channel_hardware_cleanup(&channel);
	}
}

/** @brief Preserve one physical parallel owner and transfer it only to an eligible peer. */
static void test_hardware_parallel_owner_service(void)
{
	struct chan_usbradio_pvt channel, next = {.name = "next"};
	unsigned int inputs = 0U;

	channel_hardware_fixture(&channel);
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(!cm119_gpio_poc_service_parallel(&channel, &inputs, 0));
	haspp = 1;
	assert(cm119_gpio_poc_service_parallel(&channel, &inputs, 0) == -1);
	assert(!cm119_gpio_poc_service_parallel(&channel, NULL, 1));
	assert(!usbradioplus_parallel_adapter_poc_open(&channel.plus_parallel_adapter_poc,
						       &channel.plus_hardware_adapter, haspp,
						       "/dev/parport0", 0U, 0U));
	usbradioplus_test_set_parallel_owner(&channel);
	channel.pps[10] = "cor";
	channel.pps[11] = "ctcss";
	channel.pps[12] = "in";
	parallel_status_mask = 0x7fU;
	assert(!cm119_gpio_poc_service_parallel(&channel, &inputs, 0));
	assert(inputs & URP_HARDWARE_INPUT_PARALLEL_CARRIER);
	assert(inputs & URP_HARDWARE_INPUT_PARALLEL_CTCSS);
	assert(channel.had_pp_in);
	assert(!cm119_gpio_poc_service_parallel(&channel, NULL, 0));
	atomic_store(&channel.plus_radio_program_generation, 1U);
	assert(!cm119_gpio_poc_service_parallel(&channel, NULL, 0));
	atomic_store(&channel.plus_radio_program_generation, 0U);
	assert(!cm119_gpio_poc_open_mixer(&channel));
	assert(!usbradioplus_hardware_adapter_open_gpio(&channel.plus_hardware_adapter));
	parallel_operation_result = RPTADV_GPIO_IO_ERROR;
	assert(cm119_gpio_poc_service_parallel(&channel, &inputs, 0) == -1);
	assert(cm119_gpio_poc_service(&channel) == -1);
	cm119_gpio_poc_stop(&channel);
	assert(!channel.plus_hardware_adapter_prepared);
	parallel_operation_result = RPTADV_GPIO_OK;
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(!usbradioplus_parallel_adapter_poc_open(&channel.plus_parallel_adapter_poc,
						       &channel.plus_hardware_adapter, haspp,
						       "/dev/parport0", 0U, 0U));
	usbradioplus_test_set_parallel_owner(&channel);
	atomic_store(&channel.plus_parallel_adapter_poc.faulted, 0);
	next.plus_hardware_adapter_prepared = 1;
	next.plus_cm119_gpio_poc = 1;
	next.plus_hardware_adapter.audio = &fake_audio;
	next.plus_hardware_adapter.gpio = &fake_gpio;
	atomic_store(&next.plus_hardware_online, 1);
	channel.next = &next;
	usbradio_default.next = &channel;
	cm119_gpio_poc_discard_hardware_adapter(&channel);
	assert(usbradioplus_parallel_owner() == &next);
	assert(next.plus_hardware_adapter.parallel_device);
	cm119_gpio_poc_discard_hardware_adapter(&next);
	assert(!usbradioplus_parallel_owner());
	channel_hardware_cleanup(&channel);
}

/** @brief Preserve automatic and partial explicit identities when swapping live workers. */
static void test_hardware_swap_identity_boundaries(void)
{
	struct chan_usbradio_pvt first, second;
	int mode;

	for (mode = 0; mode < 3; ++mode) {
		channel_hardware_fixture(&first);
		channel_hardware_fixture(&second);
		first.name = "swap-first";
		second.name = "swap-second";
		strcpy(first.plus_hardware_adapter.usb_port_path, "3-1");
		strcpy(second.plus_hardware_adapter.usb_port_path, "3-2");
		if (mode == 1) {
			strcpy(first.serial, "FIRST");
			strcpy(second.serial, "SECOND");
		} else if (mode == 2) {
			strcpy(first.plus_cm119_gpio_usb_port_path, "3-1");
			strcpy(second.plus_cm119_gpio_usb_port_path, "3-2");
		}
		first.next = &second;
		usbradio_default.next = &first;
		usbradio_active = first.name;
		first.plus_hardware_worker_started = second.plus_hardware_worker_started = 1;
		pthread_create_calls = 0;
		fail_pthread_create_call = mode == 0 ? 2 : 0;
		assert(usb_device_swap(1, second.name) == (mode == 0 ? -1 : 0));
		if (mode == 0) {
			assert(!strcmp(first.devstr, "3-2"));
			assert(!strcmp(second.devstr, "3-1"));
		}
		fail_pthread_create_call = 0;
		channel_hardware_cleanup(&first);
		channel_hardware_cleanup(&second);
	}
	usbradio_active = NULL;
}

/** @brief Exercise live control commands and calibration through semantic handles. */
static void test_hardware_control_and_calibration_helpers(void)
{
	struct chan_usbradio_pvt channel;
	struct ast_channel *owner = (struct ast_channel *)(uintptr_t)1;

	channel_hardware_fixture(&channel);
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(!cm119_gpio_poc_open_mixer(&channel));
	assert(!cm119_gpio_poc_reserve_device_identity(&channel));
	assert(!hidthread_prepare_radio(&channel));
	test_channel_private = &channel;
	channel.valid_gpios = 1;
	assert(!usbradio_text(owner, "GPIO 1 0"));
	assert(channel.plus_hardware_gpio_poc_cancel_mask == 1U);
	assert(!usbradio_text(owner, "PP 2 1"));
	haspp = 1;
	assert(!usbradioplus_parallel_adapter_poc_open(&channel.plus_parallel_adapter_poc,
						       &channel.plus_hardware_adapter, haspp,
						       "/dev/parport0", 0U, 0U));
	usbradioplus_test_set_parallel_owner(&channel);
	parallel_scheduled_pulse_result = RPTADV_GPIO_IO_ERROR;
	assert(!usbradio_text(owner, "PP 2 1"));
	parallel_scheduled_pulse_result = RPTADV_GPIO_OK;
	atomic_store(&channel.plus_parallel_adapter_poc.faulted, 0);
	parallel_operation_result = RPTADV_GPIO_IO_ERROR;
	assert(!usbradio_text(owner, "PP 2 1"));
	parallel_operation_result = RPTADV_GPIO_OK;
	atomic_store(&channel.plus_parallel_adapter_poc.faulted, 0);
	pthread_create_calls = 0;
	fail_pthread_create_call = 1;
	assert(usbradio_call(owner, "", 0) == -1);
	fail_pthread_create_call = 0;
	channel.rxdemod = RX_AUDIO_SPEAKER;
	_menu_rxvoice(1, &channel, "500");
	mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
	_menu_rxvoice(1, &channel, "500");
	mixer_step_result = RPTADV_AUDIO_OK;
	assert(!cm119_gpio_poc_open_mixer(&channel));
	channel.rxdemod = RX_AUDIO_FLAT;
	channel.rxcdtype = CD_HID;
	set_measurements(channel.radio->spsMeasure, 27000, 20U);
	tune_rxinput(1, &channel, 0, 1);
	assert(!channel.radio->b.tuning);
	mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
	tune_rxinput(1, &channel, 0, 1);
	assert(!channel.radio->b.tuning);
	mixer_step_result = RPTADV_AUDIO_OK;
	scripted_measure_stage = NULL;
	usbradio_active = channel.name;
	_menu_print(1, &channel);
	channel.plus_hardware_adapter_prepared = 0;
	_menu_print(1, &channel);
	channel.plus_cm119_gpio_poc = 0;
	_menu_print(1, &channel);
	channel.plus_cm119_gpio_poc = 1;
	radioplus_native_stats_combined_poc(1, NULL);
	channel.plus_portaudio_poc = 0;
	radioplus_native_stats_combined_poc(1, &channel);
	channel.plus_portaudio_poc = 1;
	channel.plus_cm119_gpio_poc = 0;
	radioplus_native_stats_combined_poc(1, &channel);
	channel.plus_cm119_gpio_poc = 1;
	radioplus_native_stats_combined_poc(1, &channel);
	channel.plus_hardware_adapter_prepared = 1;
	radioplus_native_stats_combined_poc(1, &channel);
	channel.plus_portaudio_stream = (struct rptadv_audio_stream *)&stream_token;
	radioplus_native_stats_combined_poc(1, &channel);
	stream_stats_capture_target = 480U;
	radioplus_native_stats_combined_poc(1, &channel);
	channel.plus_portaudio_stream = NULL;
	test_channel_private = NULL;
	usbradio_active = NULL;
	channel_hardware_cleanup(&channel);
}

/** @brief Cover configured radio routing and every parallel-owner eligibility predicate. */
static void test_hardware_routing_and_owner_eligibility(void)
{
	struct chan_usbradio_pvt channel, next;
	int mode;

	for (mode = 0; mode < 3; ++mode) {
		channel_hardware_fixture(&channel);
		channel.plus_advanced = mode == 0;
		channel.radioduplex = mode == 1;
		channel.txmixa = mode == 2 ? TX_OUT_LSD : TX_OUT_COMPOSITE;
		channel.radioactive = 1;
		usbradio_default.next = &channel;
		assert(!hidthread_prepare_radio(&channel));
		assert(channel.radioactive && usbradio_active == channel.name);
		processing_hardware_get_calls = 0;
		fail_processing_hardware_get_call = 1;
		assert(!hidthread_prepare_radio(&channel));
		fail_processing_hardware_get_call = 0;
		channel_hardware_cleanup(&channel);
	}
	usbradio_active = NULL;
	for (mode = 0; mode < 5; ++mode) {
		channel_hardware_fixture(&channel);
		memset(&next, 0, sizeof(next));
		next.name = "ineligible";
		next.plus_hardware_adapter_prepared = mode != 0;
		next.plus_cm119_gpio_poc = mode != 1;
		atomic_store(&next.plus_hardware_online, mode != 2);
		atomic_store(&next.plus_hardware_stop_request, mode == 3);
		haspp = 1;
		assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
		assert(!usbradioplus_parallel_adapter_poc_open(&channel.plus_parallel_adapter_poc,
							       &channel.plus_hardware_adapter,
							       haspp, "/dev/parport0", 0U, 0U));
		atomic_store(&channel.plus_parallel_adapter_poc.faulted, mode == 4);
		usbradioplus_test_set_parallel_owner(&channel);
		usbradio_default.next = &next;
		cm119_gpio_poc_discard_hardware_adapter(&channel);
		assert(!usbradioplus_parallel_owner());
		channel_hardware_cleanup(&channel);
	}
}

/** @brief Check duplex-mode sidetone gating and post-open mixer refresh failures. */
static void test_hardware_sidetone_and_refresh_boundaries(void)
{
	struct chan_usbradio_pvt channel;
	int mode;

	for (mode = 0; mode < 5; ++mode) {
		channel_hardware_fixture(&channel);
		assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
		channel.plus_advanced = mode == 0;
		channel.duplex3 = mode == 3 ? 0 : 500;
		channel.duplex3mode = mode == 1 ? DUPLEX3_MODE_SOFTWARE : DUPLEX3_MODE_HARDWARE;
		if (mode == 4)
			mixer_step_result = RPTADV_AUDIO_UNSUPPORTED;
		assert(cm119_gpio_poc_open_mixer(&channel) == (mode == 4 ? -1 : 0));
		if (mode < 4) {
			assert(!usbradioplus_hardware_adapter_open_gpio(
				&channel.plus_hardware_adapter));
			assert(!cm119_gpio_poc_service(&channel));
			atomic_store(&channel.plus_portaudio_delivered_keyed, 1);
			assert(!cm119_gpio_poc_service(&channel));
		}
		channel_hardware_cleanup(&channel);
	}
	channel_hardware_fixture(&channel);
	assert(!hidthread_prepare_radio(&channel));
	settings_defaults(&settings);
	ast_copy_string(settings.profiles[0].name, channel.name, sizeof(settings.profiles[0].name));
	ast_copy_string(settings.profiles[0].channel, "RadioPlus/hardware-boundary",
			sizeof(settings.profiles[0].channel));
	assert(!apply_processing_config_overrides(&channel, channel.name));
	assert(!hidthread_prepare_radio(&channel));
	add_processing_override("hardware", "hardware_audio_backend", "retired");
	assert(apply_processing_config_overrides(&channel, channel.name) == -1);
	assert(!hidthread_prepare_radio(&channel));
	settings_defaults(&settings);
	channel_hardware_cleanup(&channel);
}

/** @brief Exercise radio preparation and adapter-owned EEPROM/service operations. */
static void test_hardware_radio_and_service_helpers(void)
{
	struct chan_usbradio_pvt channel;
	unsigned int command;

	channel_hardware_fixture(&channel);
	assert(!cm119_gpio_poc_prepare_hardware_adapter(&channel));
	assert(!cm119_gpio_poc_open_mixer(&channel));
	assert(!usbradioplus_hardware_adapter_open_gpio(&channel.plus_hardware_adapter));
	assert(!hidthread_prepare_radio(&channel));
	assert(channel.radio);
	channel.wanteeprom = 1;
	assert(!hidthread_prepare_radio(&channel));
	assert(channel.eepromctl == 1);
	assert(!cm119_gpio_poc_worker_service(&channel));
	assert(!channel.eepromctl);
	assert(atomic_load(&channel.plus_hardware_online));
	assert(atomic_load(&channel.plus_hardware_ptt_applied));
	assert(atomic_load(&channel.plus_hardware_last_service_time));
	channel.clipledgpio = 1;
	channel.plus_hardware_adapter.gpio_output_enable_mask = 1U;
	atomic_store(&channel.plus_clip_led_request, 1);
	gpio_input_snapshot.cor_active = gpio_input_snapshot.ctcss_active = 0U;
	assert(!cm119_gpio_poc_service(&channel));
	for (command = 0U; command <= 3U; ++command) {
		channel.eepromctl = (char)command;
		cm119_gpio_poc_service_eeprom(&channel);
		assert(!channel.eepromctl);
	}
	channel.eepromctl = 1;
	gpio_eeprom_read_image.magic_valid = 0U;
	cm119_gpio_poc_service_eeprom(&channel);
	channel.eepromctl = 1;
	gpio_eeprom_read_result = RPTADV_GPIO_IO_ERROR;
	cm119_gpio_poc_service_eeprom(&channel);
	channel.eepromctl = 2;
	gpio_eeprom_write_result = RPTADV_GPIO_IO_ERROR;
	cm119_gpio_poc_service_eeprom(&channel);
	channel.wanteeprom = 0;
	cm119_gpio_poc_service_eeprom(&channel);
	gpio_service_result = RPTADV_GPIO_IO_ERROR;
	assert(cm119_gpio_poc_service(&channel) == -1);
	gpio_service_result = RPTADV_GPIO_OK;
	gpio_publish_result = RPTADV_GPIO_IO_ERROR;
	assert(cm119_gpio_poc_service(&channel) == -1);
	gpio_publish_result = RPTADV_GPIO_OK;
	usbradioplus_hardware_mixer_poc_close(&channel.plus_hardware_mixer_poc);
	assert(cm119_gpio_poc_service(&channel) == -1);
	channel_hardware_cleanup(&channel);
}
