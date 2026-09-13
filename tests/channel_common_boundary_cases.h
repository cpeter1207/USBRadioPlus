/** @file
 * @brief Channel lifecycle guards and immutable hardware configuration tests.
 */

/** @brief Preserve safe startup/teardown behavior when no radio exists. */
static void test_common_lifecycle_boundaries(void)
{
	struct chan_usbradio_pvt channel = {0};
	urp_radio_state radio = {0};
	const struct urp_native_output_block block = {.frame_count = 2};
	short pcm[4] = {1};
	struct chan_usbradio_pvt *saved_first = usbradio_default.next;
	char *saved_active = usbradio_active;
	usbradioplus_import_external_ptt_request(NULL);
	usbradioplus_import_external_ptt_request(&channel);
	usbradioplus_note_hardware_ptt_applied(NULL);
	usbradioplus_note_hardware_ptt_applied(&channel);
	usbradioplus_native_output_stage_enqueue(NULL, 1);
	usbradioplus_native_output_stage_enqueue(&channel, 0);
	usbradioplus_native_output_stage_enqueue(&channel, 1);
	usbradioplus_native_output_stage_complete(NULL, &block, 0);
	usbradioplus_native_output_stage_complete(&channel, NULL, 0);
	assert(!usbradioplus_native_output_stage_recover_stall(NULL, 1));
	assert(!usbradioplus_native_output_stage_recover_stall(&channel, 1));
	usbradioplus_native_output_stage_reset(NULL);
	usbradioplus_native_output_stage_fail_safe_reset(NULL);
	usbradioplus_native_output_stage_request_reset(NULL);
	usbradioplus_native_output_stage_request_reset(&channel);
	usbradioplus_native_output_stage_consume_reset_request(NULL);
	usbradioplus_native_output_stage_consume_reset_request(&channel);
	usbradioplus_measure_rx_audio(NULL, pcm, 2);
	usbradioplus_measure_rx_audio(&channel, NULL, 2);
	usbradioplus_prepare_squelch_audio(NULL, 1);
	usbradioplus_prepare_squelch_audio(&channel, 0);
	usbradioplus_prepare_squelch_audio(&channel, 1);
	assert(!usbradioplus_ctcss_detected(NULL));
	assert(!usbradioplus_ctcss_detected(&channel));
	assert(!usbradioplus_update_receive_state(NULL));
	assert(!usbradioplus_update_receive_state(&channel));
	assert(!usbradioplus_update_receive_state_timed(NULL, 1));
	assert(!usbradioplus_update_receive_state_timed(&channel, 1));
	usbradioplus_refresh_ctcss_decode(NULL);
	usbradioplus_refresh_ctcss_decode(&channel);
	channel.radio = &radio;
	channel.plus_tx_playout_hold.hardware_ptt_applied = 1;
	atomic_store(&channel.plus_hardware_ptt_applied, 1);
	usbradioplus_note_hardware_ptt_applied(&channel);
	channel.plus_cm119_gpio_poc = 1;
	channel.name = "boundary";
	assert(call_radio_tune(&channel, 3, "rxnoise", NULL) == RESULT_SUCCESS);
	usbradio_default.next = saved_first;
	usbradio_active = saved_active;
	usbradioplus_refresh_ctcss_decode(&channel);
	atomic_store(&channel.txtestkey, 1);
	usbradioplus_import_external_ptt_request(&channel);
	assert(radio.txPttIn);
	radio.txPttIn = 0;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(radio.txPttOut);
	atomic_store(&channel.txtestkey, 0);
	atomic_store(&channel.txkeyed, 1);
	radio.txPttOut = 0;
	usbradioplus_tx_playout_hold_apply(&channel);
	assert(radio.txPttOut);
	atomic_store(&channel.txkeyed, 0);
	usbradioplus_tx_playout_hold_reset(&channel);
	usbradioplus_tx_playout_hold_note_output(&channel, 0, 1, 2, 2);
	usbradioplus_tx_playout_hold_advance(&channel, 0);
	channel.plus_native_max_frames = 2;
	urp_native_output_stage_init(&channel.plus_native_output_stage, 2, 2);
	memcpy(channel.usbradio_write_buf, pcm, sizeof(pcm));
	atomic_store(&channel.plus_radio_tx_active, 1);
	usbradioplus_native_output_stage_enqueue(&channel, 2);
	usbradioplus_native_output_stage_enqueue(&channel, 2);
	usbradioplus_native_output_stage_enqueue(&channel, 2);
	assert(channel.plus_sound_dropped_frames == 1);
	assert(!urp_native_output_stage_commit(&channel.plus_native_output_stage, 1, NULL));
	assert(usbradioplus_native_output_stage_recover_stall(&channel, 4));
	assert(channel.plus_sound_dropped_frames == 2);
	assert(!urp_native_output_stage_peek(&channel.plus_native_output_stage));
	atomic_store(&channel.plus_radio_tx_active, 0);
	usbradioplus_native_output_stage_enqueue(&channel, 2);
	atomic_store(&channel.plus_radio_tx_active, 1);
	memset(channel.usbradio_write_buf, 0, sizeof(pcm));
	usbradioplus_native_output_stage_enqueue(&channel, 2);
	channel.rxsdtype = SD_HID;
	channel.rxhidctcss = 1;
	channel.rxcdtype = CD_HID;
	channel.rxhidsq = 1;
	channel.txoffdelay = 0;
	radio.txPttOut = 0;
	assert(usbradioplus_update_receive_state_timed(&channel, 0) == channel.rxkeyed);
}

/** @brief Reject live changes to every hardware identity field independently. */
static void test_common_hardware_reload_boundaries(void)
{
	static const struct {
		const char *key;
		const char *value;
	} changes[] = {
		{"hardware_portaudio_input_device_index", "5"},
		{"hardware_portaudio_output_device_index", "6"},
		{"hardware_device_identifier", "different"},
		{"hardware_serial", "different"},
		{"hardware_interface_type", "1"},
		{"hardware_ptt_inverted", "yes"},
		{"hardware_gpio_usb_port_path", "3-2"},
	};
	for (size_t index = 0; index < ARRAY_LEN(changes) + 2; ++index) {
		struct chan_usbradio_pvt channel = {0};
		urp_radio_state radio = {0};
		settings_defaults(&settings);
		channel.plus_portaudio_poc = index != ARRAY_LEN(changes);
		channel.plus_cm119_gpio_poc = index != ARRAY_LEN(changes) + 1;
		channel.plus_portaudio_input_device_index = 3;
		channel.plus_portaudio_output_device_index = 4;
		channel.radio = &radio;
		if (index < ARRAY_LEN(changes))
			add_processing_override("hardware", changes[index].key,
						changes[index].value);
		assert(apply_processing_config_overrides(&channel, "usb") == -1);
	}
	for (unsigned int index = 0; index < 2; ++index) {
		struct chan_usbradio_pvt channel = {0};
		settings_defaults(&settings);
		add_processing_override("hardware",
					index ? "hardware_portaudio_output_device_index"
					      : "hardware_portaudio_input_device_index",
					"-2");
		assert(apply_processing_config_overrides(&channel, "usb") == -1);
	}
	settings_defaults(&settings);
}

/** @brief A live parallel owner rejects path/base changes without opening hardware. */
static void test_common_parallel_reload_boundaries(void)
{
	struct chan_usbradio_pvt channel = {0};
	struct ast_config *saved_config = test_config_load_result;
	char saved_path[sizeof(pport)];
	char *saved_active = usbradio_active;
	int saved_base = pbase;
	int saved_haspp = haspp;
	struct chan_usbradio_pvt *saved_owner = usbradioplus_parallel_owner();
	ast_copy_string(saved_path, pport, sizeof(saved_path));
	usbradio_active = "usb";
	test_config_load_result = (struct ast_config *)(uintptr_t)1;
	test_config_category = NULL;
	test_config_variables = NULL;
	for (unsigned int index = 0; index < 2; ++index) {
		settings_defaults(&settings);
		ast_copy_string(pport, PP_PORT, sizeof(pport));
		pbase = PP_IOPORT;
		add_processing_override("hardware",
					index ? "hardware_parallel_port_base_address"
					      : "hardware_parallel_port_device",
					index ? "0x278" : "/dev/parport1");
		usbradioplus_test_set_parallel_owner(&channel);
		assert(load_config(1) == -1);
	}
	usbradioplus_set_channel(3);
	settings_defaults(&settings);
	ast_copy_string(pport, PP_PORT, sizeof(pport));
	pbase = PP_IOPORT;
	assert(!load_config(1));
	usbradioplus_test_set_parallel_owner(NULL);
	usbradioplus_set_channel(3);
	settings_defaults(&settings);
	add_processing_override("hardware", "hardware_parallel_port_base_address", "0");
	assert(!load_config(1));
	assert(pbase == PP_IOPORT);
	haspp = 1;
	add_processing_override("hardware", "hardware_parallel_port_device", "");
	assert(!load_config(1));
	assert(haspp == 2);
	usbradioplus_test_set_parallel_owner(saved_owner);
	ast_copy_string(pport, saved_path, sizeof(pport));
	pbase = saved_base;
	haspp = saved_haspp;
	usbradio_active = saved_active;
	test_config_load_result = saved_config;
	settings_defaults(&settings);
}
