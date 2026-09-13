/** @file
 * @brief Executable channel shared core regression and failure-path checks.
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/usbradioplus_channel_core.h"
#include "../src/usbradioplus_radio_core_adapter.h"
#include "channel_shared_boundary_cases.h"

/** @brief Execute this harness's regression assertions and report any failures.
 * @return Zero when all checks pass; assertions or a nonzero result indicate failure.
 */
int main(void)
{
	test_output_stage_boundaries();
	test_shared_render_boundaries();
	test_shared_parser_boundaries();
	short sample;
	size_t i;
	enum urp_rx_audio_mode rx_audio;
	enum urp_carrier_source carrier;
	enum urp_ctcss_source ctcss;
	enum urp_tone_off_mode tone_off;
	uint32_t parser_value;
	static const char *const rx_names[] = {"no", "SPEAKER", "flat"};
	static const char *const carrier_names[] = {"no",	 "dsp", "vox",	   "usb",
						    "usbinvert", "pp",	"ppinvert"};
	static const char *const ctcss_names[] = {"no",	 "usb", "usbinvert",
						  "dsp", "pp",	"ppinvert"};
	static const char *const tone_names[] = {"no", "ctcss_phase_shift", "ctcss_tone_remove",
						 "ctcss_tail_tone"};

	/* The generic ring also backs legacy echo and rejects an uninitialized queue. */
	{
		struct urp_sample_queue generic = {0};
		short generic_samples[2] = {0};
		unsigned int cursor;

		assert(!urp_sample_queue_push_sample(&generic, 1));
		assert(!urp_sample_queue_pop_sample(&generic, &sample));
		generic.samples = generic_samples;
		assert(!urp_sample_queue_push_sample(&generic, 1));
		assert(!urp_sample_queue_pop_sample(&generic, &sample));
		generic.samples = NULL;
		generic.capacity = 1;
		assert(!urp_sample_queue_push_sample(&generic, 1));
		assert(!urp_sample_queue_pop_sample(&generic, &sample));
		urp_sample_queue_init(&generic, generic_samples,
				      sizeof(generic_samples) / sizeof(generic_samples[0]));
		assert(urp_sample_queue_push_sample(&generic, 11));
		assert(urp_sample_queue_push_sample(&generic, 12));
		assert(!urp_sample_queue_push_sample(&generic, 13));
		assert(urp_sample_queue_samples(&generic) ==
		       sizeof(generic_samples) / sizeof(generic_samples[0]));
		assert(urp_sample_queue_high_water(&generic) ==
		       sizeof(generic_samples) / sizeof(generic_samples[0]));
		assert(urp_sample_queue_pop_sample(&generic, &sample));
		assert(sample == 11);
		urp_sample_queue_reset_high_water(&generic);
		assert(urp_sample_queue_high_water(&generic) == 1U);
		/* A consumer-side discard must retain monotonic producer state.  The
		 * next producer publication therefore remains usable without resetting
		 * either endpoint under a live SPSC queue. */
		cursor = atomic_load_explicit(&generic.write, memory_order_acquire);
		urp_sample_queue_discard(&generic);
		assert(!urp_sample_queue_samples(&generic));
		assert(atomic_load_explicit(&generic.read, memory_order_acquire) == cursor);
		assert(atomic_load_explicit(&generic.write, memory_order_acquire) == cursor);
		assert(urp_sample_queue_push_sample(&generic, 14));
		assert(atomic_load_explicit(&generic.write, memory_order_acquire) == cursor + 1U);
		assert(urp_sample_queue_pop_sample(&generic, &sample));
		assert(sample == 14);
		urp_sample_queue_reset(&generic);
		assert(!urp_sample_queue_samples(&generic));
		assert(!urp_sample_queue_high_water(&generic));
		assert(!urp_sample_queue_pop_sample(&generic, &sample));
	}

	for (i = 0; i < sizeof(rx_names) / sizeof(rx_names[0]); ++i) {
		assert(!urp_parse_rx_audio_mode(rx_names[i], &rx_audio));
		assert((size_t)rx_audio == i);
	}
	for (i = 0; i < sizeof(carrier_names) / sizeof(carrier_names[0]); ++i) {
		assert(!urp_parse_carrier_source(carrier_names[i], &carrier));
		assert((size_t)carrier == i);
	}
	for (i = 0; i < sizeof(ctcss_names) / sizeof(ctcss_names[0]); ++i) {
		static const enum urp_ctcss_source expected[] = {
			URP_CTCSS_DISABLED, URP_CTCSS_USB,	URP_CTCSS_USB_INVERTED,
			URP_CTCSS_DSP,	    URP_CTCSS_PARALLEL, URP_CTCSS_PARALLEL_INVERTED};
		assert(!urp_parse_ctcss_source(ctcss_names[i], &ctcss));
		assert(ctcss == expected[i]);
	}
	for (i = 0; i < sizeof(tone_names) / sizeof(tone_names[0]); ++i) {
		assert(!urp_parse_tone_off_mode(tone_names[i], &tone_off));
		assert((size_t)tone_off == i);
	}
	rx_audio = URP_RX_AUDIO_FLAT;
	assert(urp_parse_rx_audio_mode(NULL, &rx_audio));
	assert(rx_audio == URP_RX_AUDIO_FLAT);
	assert(urp_parse_rx_audio_mode("invalid", &rx_audio));
	assert(rx_audio == URP_RX_AUDIO_FLAT);
	assert(urp_parse_rx_audio_mode("no", NULL));
	carrier = URP_CARRIER_PARALLEL_INVERTED;
	assert(urp_parse_carrier_source("invalid", &carrier));
	assert(carrier == URP_CARRIER_PARALLEL_INVERTED);
	assert(urp_parse_carrier_source("no", NULL));
	ctcss = URP_CTCSS_PARALLEL_INVERTED;
	assert(urp_parse_ctcss_source("invalid", &ctcss));
	assert(ctcss == URP_CTCSS_PARALLEL_INVERTED);
	assert(urp_parse_ctcss_source("SD_XPMR", &ctcss));
	assert(ctcss == URP_CTCSS_PARALLEL_INVERTED);
	assert(urp_parse_ctcss_source("no", NULL));
	tone_off = URP_TONE_OFF_TAIL_TONE;
	assert(urp_parse_tone_off_mode("invalid", &tone_off));
	assert(tone_off == URP_TONE_OFF_TAIL_TONE);
	assert(urp_parse_tone_off_mode("TOC_PHASE", &tone_off));
	assert(tone_off == URP_TONE_OFF_TAIL_TONE);
	assert(urp_parse_tone_off_mode("no", NULL));

	/* The compatibility parser invokes the descriptor lazily because config
	 * loading precedes renderer setup. The bridge uses temporary storage so a
	 * rejected token never changes its caller's assignment. */
	parser_value = UINT32_MAX;
	assert(!urp_radio_core_parse_rx_audio_mode("flat", &parser_value));
	assert(parser_value == RPTADV_RADIO_RX_AUDIO_FLAT);
	assert(urp_radio_core_parse_rx_audio_mode("flat ", &parser_value));
	assert(parser_value == RPTADV_RADIO_RX_AUDIO_FLAT);
	parser_value = UINT32_MAX;
	assert(!urp_radio_core_parse_carrier_source("PPINVERT", &parser_value));
	assert(parser_value == RPTADV_RADIO_CARRIER_PARALLEL_INVERTED);
	assert(urp_radio_core_parse_carrier_source("sd_xpmr", &parser_value));
	assert(parser_value == RPTADV_RADIO_CARRIER_PARALLEL_INVERTED);
	parser_value = UINT32_MAX;
	assert(!urp_radio_core_parse_ctcss_source("dsp", &parser_value));
	assert(parser_value == RPTADV_RADIO_CTCSS_DSP);
	assert(urp_radio_core_parse_ctcss_source(NULL, &parser_value));
	assert(parser_value == RPTADV_RADIO_CTCSS_DSP);
	parser_value = UINT32_MAX;
	assert(!urp_radio_core_parse_tone_off_mode("ctcss_tail_tone", &parser_value));
	assert(parser_value == RPTADV_RADIO_TONE_OFF_TAIL_TONE);
	assert(urp_radio_core_parse_tone_off_mode("TOC_PHASE", &parser_value));
	assert(parser_value == RPTADV_RADIO_TONE_OFF_TAIL_TONE);

	assert(urp_gain_db_to_mixer(0.0) == 500);
	assert(urp_gain_db_to_mixer(20.0) == 999);
	assert(urp_gain_db_to_mixer(-200.0) == 0);
	assert(fabs(urp_mixer_to_gain_db(500)) < 0.000001);
	assert(urp_mixer_to_gain_db(0) < -100.0);
	assert(urp_hardware_level_multiplier(0) == 64);
	assert(urp_hardware_level_multiplier(1) == 128);
	assert(urp_hardware_level_multiplier(2) == 192);
	assert(urp_hardware_level_multiplier(3) == 256);
	assert(urp_saturating_add(100, 200) == 300);
	assert(urp_saturating_add(30000, 30000) == 32767);
	assert(urp_saturating_add(-30000, -30000) == -32768);
	assert(urp_apply_gain(1000, 0.5) == 500);
	assert(urp_apply_gain(30000, 2.0) == 32767);
	assert(urp_apply_gain(-30000, 2.0) == -32768);
	{
		const short pcm[] = {-32768, 100, 32767};
		const double floating[] = {-0.75, 0.5};
		assert(urp_pcm_peak(pcm, sizeof(pcm) / sizeof(pcm[0])) == 32768U);
		assert(urp_pcm_peak(pcm, 0) == 0);
		assert(fabs(urp_pcm_peak_dbfs(32768U)) < 0.000001);
		assert(urp_pcm_peak_dbfs(0) == -INFINITY);
		assert(fabs(urp_double_peak(floating, 2) - 0.75) < 0.000001);
		assert(urp_double_peak(floating, 0) == 0.0);
	}
	assert(!urp_tx_output_has_program(URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_output_has_program(URP_TX_OUTPUT_VOICE));
	assert(!urp_tx_output_has_program(URP_TX_OUTPUT_TONE));
	assert(urp_tx_output_has_program(URP_TX_OUTPUT_COMPOSITE));
	assert(urp_tx_output_has_program(URP_TX_OUTPUT_AUX_VOICE));
	assert(!urp_tx_output_has_voice(URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_output_has_voice(URP_TX_OUTPUT_VOICE));
	assert(!urp_tx_output_has_voice(URP_TX_OUTPUT_TONE));
	assert(urp_tx_output_has_voice(URP_TX_OUTPUT_COMPOSITE));
	assert(!urp_tx_output_has_voice(URP_TX_OUTPUT_AUX_VOICE));
	assert(!urp_tx_output_has_tone(URP_TX_OUTPUT_DISABLED));
	assert(!urp_tx_output_has_tone(URP_TX_OUTPUT_VOICE));
	assert(urp_tx_output_has_tone(URP_TX_OUTPUT_TONE));
	assert(urp_tx_output_has_tone(URP_TX_OUTPUT_COMPOSITE));
	assert(!urp_tx_output_has_tone(URP_TX_OUTPUT_AUX_VOICE));
	assert(!urp_tx_pair_has_voice(URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_TONE));
	assert(urp_tx_pair_has_voice(URP_TX_OUTPUT_VOICE, URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_pair_has_voice(URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_COMPOSITE));
	assert(!urp_tx_pair_has_tone(URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_VOICE));
	assert(urp_tx_pair_has_tone(URP_TX_OUTPUT_TONE, URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_pair_has_tone(URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_COMPOSITE));
	assert(!urp_tx_signaling_route_missing(0, URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_signaling_route_missing(1, URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_VOICE));
	assert(!urp_tx_signaling_route_missing(1, URP_TX_OUTPUT_TONE, URP_TX_OUTPUT_DISABLED));
	assert(!urp_parallel_pulser_needed(0, 0));
	assert(!urp_parallel_pulser_needed(0, 1));
	assert(!urp_parallel_pulser_needed(1, 0));
	assert(urp_parallel_pulser_needed(1, 1));
	assert(!urp_native_echo_enabled(0, 1));
	assert(!urp_native_echo_enabled(999, 0));
	assert(urp_native_echo_enabled(999, 1));
	{
		int32_t usb;
		int8_t parallel;

		for (int asserted = 0; asserted <= 1; ++asserted) {
			for (int inverted = 0; inverted <= 1; ++inverted) {
				usb = 0xff;
				parallel = 0xff;
				urp_apply_ptt_outputs(asserted, inverted, 0x03, 0x08, &usb,
						      &parallel);
				assert(!!(usb & 0x08) == (asserted != inverted));
				assert((parallel & 0x03) == (asserted != inverted ? 0x03 : 0x00));
			}
		}
		usb = parallel = 0x55;
		urp_apply_ptt_outputs(1, 0, 0, 0x08, &usb, &parallel);
		assert((usb & 0x08) && parallel == 0x55);
	}
	{
		struct urp_parrot_state parrot = {.count = 4, .play = 2, .truncated = 1};

		assert(!urp_parrot_rx_transition(&parrot, 0, 1));
		assert(!parrot.count && !parrot.play && !parrot.playing && !parrot.truncated);
		assert(!urp_parrot_rx_transition(&parrot, 0, 0));
		assert(!urp_parrot_rx_transition(&parrot, 1, 1));
		assert(!urp_parrot_rx_transition(&parrot, 1, 0));
		parrot.count = 6;
		assert(urp_parrot_rx_transition(&parrot, 1, 0));
		assert(parrot.playing && !parrot.play);
	}
	{
		/* Recording and playback belong exclusively to the Rust core. Exercise
		 * their bounds and cursor transitions through the consumer ABI. */
		float storage[8] = {0};
		const float source[] = {0.125F, -0.25F, 0.375F, -0.5F, 0.625F, -0.75F};
		float playback[6] = {0};
		struct rptadv_radio_native_parrot_status status = {0};
		struct rptadv_radio *radio = NULL;
		size_t samples = 99U;
		int playback_started = 1;

		assert(urp_radio_core_initialize() == 0);
		assert(urp_radio_core_create(URP_RATE_NATIVE, 8, &radio) == 0);
		assert(urp_radio_core_native_parrot_bind(radio, storage, 8) == 0);
		assert(urp_radio_core_native_parrot_play(radio, playback, 2, &samples) == 0);
		assert(samples == 0U);
		assert(urp_radio_core_native_parrot_record(radio, source, 4, 8, &samples) == 0);
		assert(samples == 4U);
		assert(urp_radio_core_native_parrot_status(radio, &status) == 0);
		assert(status.recorded_samples == 4U && !status.playing && !status.truncated);
		assert(urp_radio_core_native_parrot_record(radio, source + 4, 2, 4, &samples) == 0);
		assert(samples == 0U);
		assert(urp_radio_core_native_parrot_status(radio, &status) == 0);
		assert(status.truncated);
		assert(urp_radio_core_native_parrot_rx_transition(radio, 0, 1, &playback_started) ==
		       0);
		assert(!playback_started);
		assert(urp_radio_core_native_parrot_status(radio, &status) == 0);
		assert(!status.recorded_samples && !status.playback_offset && !status.playing &&
		       !status.truncated);
		assert(urp_radio_core_native_parrot_record(radio, source, 6, 8, &samples) == 0);
		assert(samples == 6U);
		assert(urp_radio_core_native_parrot_rx_transition(radio, 1, 0, &playback_started) ==
		       0);
		assert(playback_started);
		assert(urp_radio_core_native_parrot_play(radio, playback, 2, &samples) == 0);
		assert(samples == 2U);
		assert(urp_radio_core_native_parrot_play(radio, playback + 2, 8, &samples) == 0);
		assert(samples == 4U);
		assert(!memcmp(playback, source, sizeof(source)));
		assert(urp_radio_core_native_parrot_status(radio, &status) == 0);
		assert(!status.playing && status.playback_offset == 6U);
		assert(urp_radio_core_native_parrot_reset(radio) == 0);
		assert(urp_radio_core_native_parrot_status(radio, &status) == 0);
		assert(!status.recorded_samples && !status.playback_offset && !status.playing &&
		       !status.truncated);
		urp_radio_core_destroy(radio);
	}
	{
		const float stereo[] = {32767.0F / 32768.0F, 1.0F / 32768.0F,	-1.0F,
					2.0F / 32768.0F,     100.0F / 32768.0F, 3.0F / 32768.0F};
		float pcm[3] = {0};
		float delay[2] = {10.0F / 32768.0F, 20.0F / 32768.0F};
		unsigned int delay_index = 9;
		float peak = 0.0F;
		unsigned long rail_samples = 0U;
		struct rptadv_radio *radio = NULL;

		assert(urp_radio_core_initialize() == 0);
		assert(urp_radio_core_create(URP_RATE_NATIVE, 3, &radio) == 0);
		assert(urp_radio_core_extract_receive(radio, stereo, pcm, 3, delay, 2, &delay_index,
						      &peak, &rail_samples) == 0);
		assert(peak == 1.0F);
		assert(rail_samples == 2U);
		assert(delay_index == 1);
		assert(pcm[0] == 10.0F / 32768.0F && pcm[1] == 20.0F / 32768.0F &&
		       pcm[2] == 32767.0F / 32768.0F);
		delay_index = 0;
		assert(urp_radio_core_extract_receive(radio, stereo + 4, pcm, 1, delay, 2,
						      &delay_index, &peak, &rail_samples) == 0);
		assert(delay_index == 1);
		delay_index = 0;
		assert(urp_radio_core_extract_receive(radio, stereo + 4, pcm, 1, NULL, 0,
						      &delay_index, &peak, &rail_samples) == 0);
		assert(peak == 100.0F / 32768.0F && rail_samples == 0U);
		assert(pcm[0] == 100.0F / 32768.0F);
		urp_radio_core_destroy(radio);
	}
	{
		const double program[] = {40000.0, -40000.0, 1000.0};
		const float ctcss_samples[] = {1.0F, -1.0F, 0.5F};
		const float dcs[] = {0.0F, 0.0F, 0.0F};
		short stereo[6] = {30000, -30000, 0, 0, 10, 20};
		short meter[6] = {0};
		struct rptadv_radio *radio = NULL;
		struct urp_transmit_render_workspace workspace = {0};
		unsigned long rails = 0;

		assert(urp_radio_core_initialize() == 0);
		assert(urp_radio_core_create(URP_RATE_NATIVE, 3, &radio) == 0);
		assert(urp_render_transmit_block(radio, program, ctcss_samples, dcs, 3,
						 URP_TX_OUTPUT_COMPOSITE, URP_TX_OUTPUT_TONE,
						 40000.0, 0.0, 40000.0, 0.0, &workspace, stereo,
						 meter, &rails) == 0);

		assert(rails == 2);
		assert(meter[0] == INT16_MAX && meter[1] == INT16_MAX);
		assert(meter[2] == INT16_MIN && meter[3] == INT16_MIN);
		assert(stereo[0] == INT16_MAX && stereo[1] == 2767);
		assert(stereo[2] == INT16_MIN && stereo[3] == INT16_MIN);
		assert(stereo[4] == 21010 && stereo[5] == 20020);
		assert(urp_render_transmit_block(radio, program + 2, ctcss_samples + 2, dcs + 2, 1,
						 URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_VOICE, 1.0,
						 0.0, 1.0, 0.0, &workspace, stereo, NULL,
						 &rails) == 0);
		assert(urp_render_transmit_block(radio, program + 2, ctcss_samples + 2, dcs + 2, 1,
						 URP_TX_OUTPUT_TONE, URP_TX_OUTPUT_COMPOSITE, 1.0,
						 0.0, 1.0, 0.0, &workspace, stereo, NULL,
						 &rails) == 0);
		urp_radio_core_destroy(radio);
	}
	{
		const double program[] = {0.0};
		const float ctcss_samples[] = {0.0F};
		const float dcs[] = {2000.0F / 32767.0F};
		short stereo[] = {0, 0};
		struct rptadv_radio *radio = NULL;
		struct urp_transmit_render_workspace workspace = {0};
		unsigned long rails = 0;

		/* DCS is already an absolute PCM level. It must not be passed through
		 * the CTCSS calibration multiplier before tone/composite routing. */
		assert(urp_radio_core_create(URP_RATE_NATIVE, 1, &radio) == 0);
		assert(urp_render_transmit_block(radio, program, ctcss_samples, dcs, 1,
						 URP_TX_OUTPUT_TONE, URP_TX_OUTPUT_COMPOSITE,
						 16000.0, 0.0, 16000.0, 0.0, &workspace, stereo,
						 NULL, &rails) == 0);
		assert(stereo[0] == 2000 && stereo[1] == 2000);
		urp_radio_core_destroy(radio);
	}
	{
		struct rptadv_radio *radio = NULL;
		float ctcss_output[4] = {0};
		float dcs_output[4] = {0};
		double ctcss_phase = 0.0;
		double phase_before_mute;

		/* CTCSS and DCS are generated by the same fixed-rate Rust object as the
		 * transmitter renderer.  These checks keep their signaling state out of
		 * the C compatibility renderer while preserving silent no-advance calls. */
		assert(urp_radio_core_create(URP_RATE_NATIVE, 4, &radio) == 0);
		assert(urp_radio_core_generate_ctcss(radio, ctcss_output, 4, 114.8, 1.0F, 1, 0.0) ==
		       0);
		assert(ctcss_output[0] == 0.0F && ctcss_output[1] > ctcss_output[0]);
		assert(urp_radio_core_ctcss_phase(radio, &ctcss_phase) == 0);
		phase_before_mute = ctcss_phase;
		assert(urp_radio_core_generate_ctcss(radio, ctcss_output, 4, 114.8, 1.0F, 0, 0.0) ==
		       0);
		for (i = 0; i < sizeof(ctcss_output) / sizeof(ctcss_output[0]); ++i)
			assert(ctcss_output[i] == 0.0F);
		assert(urp_radio_core_ctcss_phase(radio, &ctcss_phase) == 0);
		assert(ctcss_phase == phase_before_mute);
		assert(urp_radio_core_generate_ctcss_tail(radio, ctcss_output, 4, 55.0, 1.0F, 1) ==
		       0);
		assert(ctcss_output[1] > ctcss_output[0]);

		assert(urp_radio_core_configure_dcs(radio, 023, 0) == 0);
		assert(urp_radio_core_generate_dcs(radio, dcs_output, 4, 1000.0 / 32767.0, 1, 0) ==
		       0);
		for (i = 0; i < sizeof(dcs_output) / sizeof(dcs_output[0]); ++i)
			assert(fabsf(dcs_output[i]) == (float)(1000.0 / 32767.0));
		assert(urp_radio_core_configure_dcs(radio, -1, 0) == 0);
		assert(urp_radio_core_generate_dcs(radio, dcs_output, 4, 1000.0 / 32767.0, 0, 0) ==
		       0);
		for (i = 0; i < sizeof(dcs_output) / sizeof(dcs_output[0]); ++i)
			assert(dcs_output[i] == 0.0F);
		urp_radio_core_destroy(radio);
	}
	{
		struct rptadv_radio *radio = NULL;
		const float ctcss_input[] = {100.0F / 32768.0F};
		int decoded = 99;

		/* The portable decoder consumes only the already-filtered 8 kHz
		 * compatibility span. Its selected-tone mask and carrier state cross
		 * the bridge unchanged; qualification itself is covered in the Rust
		 * core's focused fixed-point regression suite. */
		assert(urp_radio_core_create(URP_RATE_NATIVE, 1, &radio) == 0);
		assert(urp_radio_core_configure_ctcss_receive(radio, 0U, 0) == 0);
		assert(urp_radio_core_process_ctcss_receive(radio, ctcss_input, 1, 1, &decoded) ==
		       0);
		assert(decoded == -1);
		assert(urp_radio_core_configure_ctcss_receive(radio, UINT64_C(1) << 11, 1) == 0);
		assert(urp_radio_core_process_ctcss_receive(radio, ctcss_input, 1, 0, &decoded) ==
		       0);
		assert(decoded == -1);
		assert(urp_radio_core_process_ctcss_receive(radio, NULL, 1, 1, &decoded) != 0);
		assert(decoded == -1);
		urp_radio_core_destroy(radio);
	}
	{
		/* The calibration source is now owned by the F32 radio core.  Keep the
		 * established 1 kHz amplitude, phase reset, and program-preservation
		 * contract visible at the C compatibility boundary. */
		struct rptadv_radio *radio = NULL;
		float tone[48] = {0};
		double phase = -1.0;
		double expected_phase = 0.0;
		const double phase_step = 2.0 * acos(-1.0) *
					  RPTADV_RADIO_CALIBRATED_TEST_TONE_FREQUENCY_HZ /
					  URP_RATE_NATIVE;

		assert(urp_radio_core_create(URP_RATE_NATIVE, 48, &radio) == 0);
		assert(urp_radio_core_render_calibrated_test_tone(
			       radio, tone, sizeof(tone) / sizeof(tone[0]), 1) == 0);
		assert(tone[0] == 0.0F);
		assert(fabsf(tone[12] - RPTADV_RADIO_CALIBRATED_TEST_TONE_PEAK_PCM_CODES /
						32767.0F) < 0.000001F);
		for (i = 0; i < sizeof(tone) / sizeof(tone[0]); ++i) {
			expected_phase += phase_step;
			if (expected_phase >= 2.0 * acos(-1.0))
				expected_phase -= 2.0 * acos(-1.0);
		}
		assert(urp_radio_core_calibrated_test_tone_phase(radio, &phase) == 0);
		assert(fabs(phase - expected_phase) < 0.000001);
		tone[0] = 0.125F;
		assert(urp_radio_core_render_calibrated_test_tone(radio, tone, 1, 0) == 0);
		assert(tone[0] == 0.125F);
		assert(urp_radio_core_calibrated_test_tone_phase(radio, &phase) == 0);
		assert(phase == 0.0);
		assert(urp_radio_core_render_calibrated_test_tone(radio, tone, 1, 1) == 0);
		assert(tone[0] == 0.0F);
		urp_radio_core_destroy(radio);
	}

	puts("shared channel queue tests passed");
	return 0;
}
