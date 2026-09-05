#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/usbradioplus_channel_core.h"

int main(void)
{
	struct urp_program_queue queue = {0};
	short input[URP_NATIVE_SAMPLES + 1];
	short output[URP_NATIVE_SAMPLES];
	short native_input[URP_NATIVE_FIFO_SAMPLES + 1];
	size_t i;
	enum urp_rx_audio_mode rx_audio;
	enum urp_tx_output_mode tx_output;
	enum urp_carrier_source carrier;
	enum urp_ctcss_source ctcss;
	enum urp_tone_off_mode tone_off;
	static const char *const rx_names[] = {"no", "SPEAKER", "flat"};
	static const char *const tx_names[] = {"no", "VOICE", "tone", "composite", "auxvoice"};
	static const char *const carrier_names[] = {"no",	 "dsp", "vox",	   "usb",
						    "usbinvert", "pp",	"ppinvert"};
	static const char *const ctcss_names[] = {"no",	 "usb", "usbinvert",
						  "dsp", "pp",	"ppinvert"};
	static const char *const tone_names[] = {"no", "phase", "notone"};

	for (i = 0; i < sizeof(input) / sizeof(input[0]); ++i)
		input[i] = (short)(i + 1);
	assert(!urp_program_queue_pending(&queue));
	assert(!urp_program_queue_pop(&queue, output));
	assert(!urp_program_queue_push(&queue, input, URP_NATIVE_SAMPLES + 1,
				       URP_NATIVE_SAMPLES + 1, 2));
	assert(queue.count == 3 && queue.high_water == 3);
	assert(urp_program_queue_pending(&queue));
	assert(urp_program_queue_pop(&queue, output));
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
		assert(output[i] == 0);
	assert(urp_program_queue_pop(&queue, output));
	assert(urp_program_queue_pop(&queue, output));
	assert(output[0] == 1 && output[URP_NATIVE_SAMPLES - 1] == URP_NATIVE_SAMPLES);

	memset(&queue, 0, sizeof(queue));
	for (i = 0; i < URP_PROGRAM_QUEUE_FRAMES; ++i)
		assert(!urp_program_queue_push(&queue, input, 1, 1, 0));
	assert(urp_program_queue_push(&queue, input, 1, 1, 0));
	assert(queue.count == URP_PROGRAM_QUEUE_FRAMES);
	assert(queue.head == 1);

	memset(&queue, 0, sizeof(queue));
	assert(urp_program_queue_push(&queue, input, URP_NATIVE_SAMPLES, 0, 99));
	assert(queue.count == URP_PROGRAM_QUEUE_FRAMES);
	assert(queue.high_water == URP_PROGRAM_QUEUE_FRAMES);

	{
		struct urp_native_fifo fifo = {0};
		for (i = 0; i < sizeof(native_input) / sizeof(native_input[0]); ++i)
			native_input[i] = (short)i;
		assert(!urp_native_fifo_pop(&fifo, output));
		assert(urp_native_fifo_push(&fifo, native_input,
					    sizeof(native_input) / sizeof(native_input[0])) == 1);
		assert(fifo.count == URP_NATIVE_FIFO_SAMPLES && fifo.head == 1);
		assert(urp_native_fifo_pop(&fifo, output));
		assert(output[0] == native_input[1]);
		fifo.primed = 1;
		urp_native_fifo_reset(&fifo);
		assert(!fifo.head && !fifo.count && !fifo.primed);
	}

	for (i = 0; i < sizeof(rx_names) / sizeof(rx_names[0]); ++i) {
		assert(!urp_parse_rx_audio_mode(rx_names[i], &rx_audio));
		assert((size_t)rx_audio == i);
	}
	for (i = 0; i < sizeof(tx_names) / sizeof(tx_names[0]); ++i) {
		assert(!urp_parse_tx_output_mode(tx_names[i], &tx_output));
		assert((size_t)tx_output == i);
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
	assert(urp_parse_rx_audio_mode(NULL, &rx_audio));
	assert(urp_parse_rx_audio_mode("invalid", &rx_audio));
	assert(urp_parse_rx_audio_mode("no", NULL));
	assert(urp_parse_tx_output_mode("invalid", &tx_output));
	assert(urp_parse_tx_output_mode("no", NULL));
	assert(urp_parse_carrier_source("invalid", &carrier));
	assert(urp_parse_carrier_source("no", NULL));
	assert(urp_parse_ctcss_source("invalid", &ctcss));
	assert(urp_parse_ctcss_source("SD_XPMR", &ctcss));
	assert(urp_parse_ctcss_source("no", NULL));
	assert(urp_parse_tone_off_mode("invalid", &tone_off));
	assert(urp_parse_tone_off_mode("TOC_PHASE", &tone_off));
	assert(urp_parse_tone_off_mode("no", NULL));

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
	assert(!urp_tx_tone_route_missing("", URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_DISABLED));
	assert(urp_tx_tone_route_missing("100.0", URP_TX_OUTPUT_DISABLED, URP_TX_OUTPUT_VOICE));
	assert(!urp_tx_tone_route_missing("100.0", URP_TX_OUTPUT_TONE, URP_TX_OUTPUT_DISABLED));
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
		double storage[8] = {0};
		double source[6] = {1, 2, 3, 4, 5, 6};
		double playback[6] = {0};
		struct urp_parrot_state parrot = {.audio = storage, .capacity = 8};

		assert(!urp_parrot_play(&parrot, playback, 2));
		assert(urp_parrot_record(&parrot, source, 4, 8) == 4);
		assert(parrot.count == 4 && !parrot.truncated);
		assert(urp_parrot_record(&parrot, source + 4, 2, 4) == 0);
		assert(parrot.truncated);
		assert(!urp_parrot_rx_transition(&parrot, 0, 1));
		assert(!parrot.count && !parrot.play && !parrot.playing && !parrot.truncated);
		assert(!urp_parrot_rx_transition(&parrot, 0, 0));
		assert(!urp_parrot_rx_transition(&parrot, 1, 1));
		assert(!urp_parrot_rx_transition(&parrot, 1, 0));
		assert(urp_parrot_record(&parrot, source, 6, 8) == 6);
		assert(urp_parrot_rx_transition(&parrot, 1, 0));
		assert(parrot.playing && !parrot.play);
		assert(urp_parrot_play(&parrot, playback, 2) == 2);
		assert(parrot.playing && parrot.play == 2);
		assert(urp_parrot_play(&parrot, playback + 2, 8) == 4);
		assert(!parrot.playing && parrot.play == 6);
		assert(!memcmp(playback, source, sizeof(source)));
	}
	{
		short stereo[] = {INT16_MAX, 1, INT16_MIN, 2, 100, 3};
		short pcm[3] = {0};
		short delay[2] = {10, 20};
		double working[3] = {0};
		unsigned int delay_index = 9;
		struct urp_receive_block_stats stats;

		urp_prepare_receive_block(stereo, pcm, working, 3, delay, 2, &delay_index, &stats);
		assert(stats.peak == 32768U);
		assert(stats.rail_samples == 2);
		assert(delay_index == 1);
		assert(pcm[0] == 10 && pcm[1] == 20 && pcm[2] == INT16_MAX);
		assert(working[2] == INT16_MAX);
		delay_index = 0;
		urp_prepare_receive_block(stereo + 4, pcm, working, 1, delay, 2, &delay_index,
					  &stats);
		assert(delay_index == 1);
		delay_index = 0;
		urp_prepare_receive_block(stereo + 4, pcm, working, 1, delay, 0, &delay_index,
					  &stats);
		assert(stats.peak == 100U && stats.rail_samples == 0);
		assert(pcm[0] == 100 && working[0] == 100.0);
	}
	{
		const double program[] = {40000.0, -40000.0, 1000.0};
		const double ctcss[] = {1.0, -1.0, 0.5};
		short stereo[6] = {30000, -30000, 0, 0, 10, 20};
		short meter[6] = {0};
		unsigned long rails = urp_render_transmit_block(
			program, ctcss, 3, URP_TX_OUTPUT_COMPOSITE, URP_TX_OUTPUT_TONE, 40000.0,
			0.0, 40000.0, 0.0, stereo, meter);

		assert(rails == 2);
		assert(meter[0] == INT16_MAX && meter[1] == INT16_MAX);
		assert(meter[2] == INT16_MIN && meter[3] == INT16_MIN);
		assert(stereo[0] == INT16_MAX && stereo[1] == 2767);
		assert(stereo[2] == INT16_MIN && stereo[3] == INT16_MIN);
		assert(stereo[4] == 21010 && stereo[5] == 20020);
		assert(urp_render_transmit_block(program + 2, ctcss + 2, 1, URP_TX_OUTPUT_DISABLED,
						 URP_TX_OUTPUT_VOICE, 1.0, 0.0, 1.0, 0.0, stereo,
						 NULL) == 0);
		assert(urp_render_transmit_block(program + 2, ctcss + 2, 1, URP_TX_OUTPUT_TONE,
						 URP_TX_OUTPUT_COMPOSITE, 1.0, 0.0, 1.0, 0.0,
						 stereo, NULL) == 0);
	}

	puts("shared channel queue tests passed");
	return 0;
}
