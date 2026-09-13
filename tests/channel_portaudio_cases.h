/** @file
 * @brief No-hardware lifecycle failure and teardown checks for the direct PortAudio bridge.
 */

/** @brief Scripted stream lifecycle outcomes and ownership observations. */
static enum rptadv_audio_result poc_start_result, poc_create_result;
static unsigned int poc_destroy_calls, poc_stop_calls;
static int poc_failed_create_returns_handle;

/** @brief Return deterministic adapter storage, including a malformed failure response. */
static enum rptadv_audio_result poc_stream_create(const struct rptadv_audio_stream_config *config,
						  struct rptadv_audio_stream **stream)
{
	stream_create_calls++;
	stream_create_config = *config;
	*stream = poc_create_result == RPTADV_AUDIO_OK || poc_failed_create_returns_handle
			  ? (struct rptadv_audio_stream *)&stream_token
			  : NULL;
	return poc_create_result;
}

/** @brief Start the fake stream without invoking hardware or an audio callback. */
static enum rptadv_audio_result poc_stream_start(struct rptadv_audio_stream *stream)
{
	assert(stream == (struct rptadv_audio_stream *)&stream_token);
	return poc_start_result;
}

/** @brief Count stop even when the adapter cannot report a successful stop. */
static enum rptadv_audio_result poc_stream_stop(struct rptadv_audio_stream *stream)
{
	assert(stream == (struct rptadv_audio_stream *)&stream_token);
	poc_stop_calls++;
	return RPTADV_AUDIO_PORTAUDIO_ERROR;
}

/** @brief Observe exactly-once destruction of the fake adapter-owned stream. */
static void poc_stream_destroy(struct rptadv_audio_stream *stream)
{
	assert(stream == (struct rptadv_audio_stream *)&stream_token);
	poc_destroy_calls++;
}

/** @brief Prepare a complete facade with independently scripted stream lifecycle operations. */
static void poc_lifecycle_fixture(struct chan_usbradio_pvt *channel,
				  struct rptadv_audio_adapter_descriptor *audio)
{
	channel_hardware_fixture(channel);
	*audio = fake_audio;
	audio->stream_create = poc_stream_create;
	audio->stream_start = poc_stream_start;
	audio->stream_stop = poc_stream_stop;
	audio->stream_destroy = poc_stream_destroy;
	channel->plus_hardware_adapter.audio = audio;
	channel->plus_hardware_adapter.gpio = &fake_gpio;
	channel->plus_hardware_adapter.audio_selection.struct_size =
		sizeof(channel->plus_hardware_adapter.audio_selection);
	channel->plus_hardware_adapter.audio_selection.abi_version =
		RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	channel->plus_hardware_adapter.audio_selection.input_device_index = 6;
	channel->plus_hardware_adapter.audio_selection.output_device_index = 7;
	channel->plus_hardware_adapter.input_device_channels = 1U;
	channel->plus_hardware_adapter.output_device_channels = RPTADV_AUDIO_CANONICAL_CHANNELS;
	strcpy(channel->plus_hardware_adapter.usb_port_path, "3-1");
	channel->plus_hardware_adapter_prepared = 1;
	channel->plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT;
	channel->plus_app_rpt_samples = URP_PORTAUDIO_POC_LEGACY_RX_BLOCK_SAMPLES;
	usbradioplus_portaudio_poc_handoff_init(&channel->plus_portaudio_rx_handoff);
	usbradioplus_portaudio_poc_status_init(&channel->plus_portaudio_status_handoff);
	poc_create_result = poc_start_result = RPTADV_AUDIO_OK;
	poc_destroy_calls = poc_stop_calls = 0U;
	poc_failed_create_returns_handle = 0;
	pthread_create_calls = fail_pthread_create_call = 0;
}

/** @brief Queue a complete voice block through the production producer handoff. */
static void poc_queue_test_block(struct chan_usbradio_pvt *channel, int keyed, uint64_t limit)
{
	unsigned int slot;
	assert(usbradioplus_portaudio_poc_handoff_producer_reserve(
		       &channel->plus_portaudio_rx_handoff, URP_PORTAUDIO_POC_RX_BLOCK_COUNT,
		       &slot) == USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY);
	memset(&channel->plus_portaudio_rx_blocks[slot], 0,
	       sizeof(channel->plus_portaudio_rx_blocks[slot]));
	channel->plus_portaudio_rx_blocks[slot].frame_count = 1U;
	channel->plus_portaudio_rx_blocks[slot].keyed = keyed;
	channel->plus_portaudio_rx_blocks[slot].status_event_limit = limit;
	usbradioplus_portaudio_poc_handoff_producer_publish(&channel->plus_portaudio_rx_handoff,
							    URP_PORTAUDIO_POC_RX_BLOCK_COUNT);
}

/** @brief Cover coherent drops, empty waits, malformed status, and saturated status retention. */
static void test_portaudio_handoff_delivery_boundaries(void)
{
	struct chan_usbradio_pvt channel = {0};
	unsigned int generation = 0U;
	uint64_t sequence = 0U;
	int keyed = 0;
	channel.name = "portaudio-delivery-boundary";
	channel.plus_advanced = 1;
	channel.plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT;
	usbradioplus_portaudio_poc_handoff_init(&channel.plus_portaudio_rx_handoff);
	usbradioplus_portaudio_poc_status_init(&channel.plus_portaudio_status_handoff);
	assert(!usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, &sequence));
	poc_queue_test_block(&channel, 0, 0U);
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, &sequence) ==
	       1);
	poc_queue_test_block(&channel, 0, 0U);
	usbradioplus_portaudio_poc_test_invalidate_next_claim();
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, &sequence) ==
	       1);
	assert(atomic_load(&channel.plus_portaudio_rx_handoff.discarded) == 1U);
	usbradioplus_portaudio_poc_handoff_reset(&channel.plus_portaudio_rx_handoff);
	generation = 0U;
	channel.owner = (struct ast_channel *)(uintptr_t)1U;
	poc_queue_test_block(&channel, 1, 1U);
	/* A status boundary beyond the published sequence must withhold matching voice. */
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, &sequence) ==
	       1);
	assert(!sequence && keyed == 1);
	channel.plus_portaudio_status_handoff.events[0].type =
		(enum usbradioplus_portaudio_poc_status_event_type)99;
	atomic_store(&channel.plus_portaudio_status_handoff.produced, 1U);
	poc_queue_test_block(&channel, 1, 1U);
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, &sequence) ==
	       1);
	assert(sequence == 1U);
	channel.owner = NULL;
	poc_queue_test_block(&channel, 0, 0U);
	portaudio_stop_on_usleep = &channel;
	assert(!portaudio_test_worker_entry(&channel));
	assert(atomic_load(&channel.plus_portaudio_delivery_stop));
	portaudio_stop_on_usleep = NULL;

	{
		urp_radio_state radio_config = {
			.pRxCodeSrc = "0", .pTxCodeSrc = "0", .pTxCodeDefault = "0"};
		short program[URP_NATIVE_SAMPLES];
		float output[URP_NATIVE_SAMPLES * 2U];
		memset(&channel, 0, sizeof(channel));
		settings_defaults(&settings);
		strcpy(settings.profiles[0].name, "portaudio-poc-callback");
		strcpy(settings.profiles[0].channel, "RadioPlus/portaudio-poc-callback");
		settings.profiles[0].enabled = 0;
		portaudio_poc_callback_channel_init(&channel, &radio_config, program,
						    ARRAY_LEN(program), 0);
		for (unsigned int event = 0U; event < URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY;
		     ++event)
			assert(usbradioplus_portaudio_poc_status_publish_voter(
				       &channel.plus_portaudio_status_handoff, 1) ==
			       USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY);
		channel.radio->b.txCtcssReady = 1;
		channel.sendvoter = channel.rxkeyed = 1;
		usbradioplus_portaudio_poc_test_capture_status(&channel, 1U);
		assert(channel.radio->b.txCtcssReady &&
		       !channel.plus_portaudio_voter_remaining_frames);
		atomic_store(&channel.plus_portaudio_rx_handoff.producer,
			     URP_PORTAUDIO_POC_RX_BLOCK_COUNT - 1U);
		atomic_store(&channel.plus_portaudio_rx_handoff.consumer_state,
			     UINT_MAX - (UINT_MAX >> 1U));
		channel.plus_portaudio_legacy_rx_assembler.sample_count =
			URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES + 1U;
		assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, output,
								 URP_NATIVE_SAMPLES));
		assert(atomic_load(&channel.plus_portaudio_rx_handoff.discarded) == 1U);
		assert(channel.plus_portaudio_legacy_rx_assembler.sample_count <=
		       URP_PORTAUDIO_POC_LEGACY_RX_FIFO_SAMPLES);
		usbradioplus_dsp_destroy(&channel);
		assert(!urp_radio_destroy(channel.radio));
	}
}

/** @brief Validate selection, descriptor, worker, stream, and final-key teardown boundaries. */
static void test_portaudio_lifecycle_boundaries(void)
{
	struct chan_usbradio_pvt channel;
	struct rptadv_audio_adapter_descriptor audio;
	struct ast_channel *owner = (struct ast_channel *)(uintptr_t)1U;
	float output[(URP_NATIVE_MAX_SAMPLES + 1U) * 2U];
	unsigned int generation = 0U;
	uint64_t sequence = 0U;
	int keyed = 0;

	poc_lifecycle_fixture(&channel, &audio);
	assert(usbradioplus_portaudio_poc_start(NULL) == -1);
	channel.plus_portaudio_poc = 0;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	channel.plus_portaudio_poc = 1;
	channel.plus_cm119_gpio_poc = 0;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	channel.plus_cm119_gpio_poc = 1;
	channel.plus_app_rpt_rate = 16000;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	channel.plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT;
	channel.plus_app_rpt_samples++;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	channel.plus_app_rpt_samples--;
	channel.plus_hardware_adapter_prepared = 0;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	channel.plus_hardware_adapter_prepared = 1;
	audio.struct_size = 0U;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	audio.struct_size = sizeof(audio);
	audio.abi_version++;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	audio.abi_version--;
#define POC_REJECT_MISSING(member)                                                                 \
	do {                                                                                       \
		audio.member = NULL;                                                               \
		assert(usbradioplus_portaudio_poc_start(&channel) == -1);                          \
		audio.member = poc_##member;                                                       \
	} while (0)
	POC_REJECT_MISSING(stream_create);
	POC_REJECT_MISSING(stream_start);
	POC_REJECT_MISSING(stream_stop);
	POC_REJECT_MISSING(stream_destroy);
#undef POC_REJECT_MISSING
	atomic_store(&channel.plus_hardware_stop_request, 1);
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	assert(!pthread_create_calls);
	atomic_store(&channel.plus_hardware_stop_request, 0);
	fail_pthread_create_call = 1;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	assert(!atomic_load(&channel.plus_portaudio_delivery_running));
	fail_pthread_create_call = 0;
	poc_create_result = RPTADV_AUDIO_PORTAUDIO_ERROR;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	assert(!channel.plus_portaudio_stream && !poc_destroy_calls);
	poc_failed_create_returns_handle = 1;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	assert(!channel.plus_portaudio_stream && poc_destroy_calls == 1U);
	poc_failed_create_returns_handle = 0;
	poc_create_result = RPTADV_AUDIO_OK;
	poc_start_result = RPTADV_AUDIO_UNSUPPORTED;
	assert(usbradioplus_portaudio_poc_start(&channel) == -1);
	assert(!channel.plus_portaudio_stream && poc_destroy_calls == 2U);
	poc_start_result = RPTADV_AUDIO_OK;
	assert(!usbradioplus_portaudio_poc_start(&channel));
	assert(channel.plus_portaudio_stream &&
	       atomic_load(&channel.plus_portaudio_delivery_running));
	assert(portaudio_test_worker_entry && portaudio_test_worker_data == &channel);
	atomic_store(&channel.plus_portaudio_delivery_stop, 1);
	assert(!portaudio_test_worker_entry(portaudio_test_worker_data));
	atomic_store(&channel.plus_portaudio_delivery_stop, 0);
	atomic_store(&channel.plus_portaudio_rx_handoff.consumer_state,
		     UINT_MAX - (UINT_MAX >> 1U));
	assert(!portaudio_test_worker_entry(portaudio_test_worker_data));
	usbradioplus_portaudio_poc_handoff_reset(&channel.plus_portaudio_rx_handoff);
	assert(stream_create_config.input_device_index == 6 &&
	       stream_create_config.output_device_index == 7);
	assert(!usbradioplus_portaudio_poc_start(&channel));
	usbradioplus_portaudio_poc_stop(&channel);
	assert(!channel.plus_portaudio_stream && poc_stop_calls == 1U && poc_destroy_calls == 3U);
	assert(!atomic_load(&channel.plus_portaudio_delivery_running));
	usbradioplus_portaudio_poc_stop(&channel);
	usbradioplus_portaudio_poc_stop(NULL);

	channel.plus_advanced = 1;
	stream_timing_abi_version++;
	assert(!usbradioplus_portaudio_poc_start(&channel));
	stream_timing_abi_version--;
	channel.owner = owner;
	atomic_store(&channel.plus_portaudio_delivered_keyed, 1);
	usbradioplus_portaudio_poc_stop(&channel);
	assert(!channel.lastrx && !atomic_load(&channel.plus_portaudio_delivered_keyed));
	channel.owner = NULL;
	atomic_store(&channel.plus_portaudio_delivered_keyed, 1);
	usbradioplus_portaudio_poc_stop(&channel);
	channel.plus_portaudio_stream = (struct rptadv_audio_stream *)&stream_token;
	channel.plus_hardware_adapter.audio = NULL;
	usbradioplus_portaudio_poc_stop(&channel);
	channel.plus_portaudio_stream = NULL;
	channel.plus_hardware_adapter.audio = &audio;

	memset(output, 1, sizeof(output));
	assert(!usbradioplus_portaudio_poc_test_callback(NULL, NULL, output, 1U));
	assert(output[0] == 0.0F && output[1] == 0.0F);
	assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, NULL, 1U));
	assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, output, 0U));
	assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, output,
							 URP_NATIVE_SAMPLES + 1U));
	channel.plus_native_max_frames = URP_NATIVE_MAX_SAMPLES + 1U;
	assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, output,
							 URP_NATIVE_MAX_SAMPLES + 1U));
	channel.plus_native_max_frames = URP_NATIVE_SAMPLES;
	assert(!usbradioplus_portaudio_poc_test_callback(&channel, NULL, output, 1U));
	assert(usbradioplus_portaudio_poc_test_deliver(NULL, &keyed, &generation, &sequence) == -1);
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, NULL, &generation, &sequence) ==
	       -1);
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, NULL, &sequence) == -1);
	assert(usbradioplus_portaudio_poc_test_deliver(&channel, &keyed, &generation, NULL) == -1);
	channel_hardware_cleanup(&channel);
	test_portaudio_handoff_delivery_boundaries();
}
