/** @file
 * @brief Transport-independent PCM conversion, routing, elastic queues, and echo state.
 */

#include "usbradioplus_channel_core.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/** A symbolic configuration value and its internal enumeration value. */
struct urp_named_value {
	/** Symbolic name used to identify this entry. */
	const char *name;
	/** Numeric assignment corresponding to the symbolic name. */
	int value;
};

/** @brief Resolve a symbolic option through a bounded name/value table.
 * @param text Symbolic assignment to resolve.
 * @param values Allowed symbolic names and their numeric values.
 * @param count Number of elements available in the supplied block.
 * @param result Receives the matching numeric assignment.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
static int parse_named_value(const char *text, const struct urp_named_value *values, size_t count,
			     int *result)
{
	size_t i;

	if (!text)
		return -1;
	for (i = 0; i < count; ++i) {
		if (!strcasecmp(text, values[i].name)) {
			*result = values[i].value;
			return 0;
		}
	}
	return -1;
}

int urp_program_queue_push(struct urp_program_queue *queue, const short *samples, size_t count,
			   size_t frame_samples, unsigned int seed_frames)
{
	unsigned int frame;
	int overflowed = 0;

	if (count > frame_samples)
		count = frame_samples;
	if (frame_samples > URP_NATIVE_SAMPLES)
		frame_samples = URP_NATIVE_SAMPLES;
	if (count > frame_samples)
		count = frame_samples;
	for (frame = 0; frame < seed_frames && queue->count < URP_PROGRAM_QUEUE_FRAMES; ++frame) {
		memset(queue->frames[queue->tail], 0, sizeof(queue->frames[0]));
		queue->tail = (queue->tail + 1U) % URP_PROGRAM_QUEUE_FRAMES;
		queue->count++;
	}
	if (queue->count == URP_PROGRAM_QUEUE_FRAMES) {
		queue->head = (queue->head + 1U) % URP_PROGRAM_QUEUE_FRAMES;
		queue->count--;
		overflowed = 1;
	}
	memset(queue->frames[queue->tail], 0, sizeof(queue->frames[0]));
	memcpy(queue->frames[queue->tail], samples, count * sizeof(*samples));
	queue->tail = (queue->tail + 1U) % URP_PROGRAM_QUEUE_FRAMES;
	queue->count++;
	if (queue->count > queue->high_water)
		queue->high_water = queue->count;
	return overflowed;
}

int urp_program_queue_pop(struct urp_program_queue *queue, short *samples)
{
	if (!queue->count)
		return 0;
	memcpy(samples, queue->frames[queue->head], sizeof(queue->frames[0]));
	queue->head = (queue->head + 1U) % URP_PROGRAM_QUEUE_FRAMES;
	queue->count--;
	return 1;
}

int urp_program_queue_pending(const struct urp_program_queue *queue)
{
	return queue->count != 0;
}

size_t urp_native_fifo_push(struct urp_native_fifo *fifo, const short *samples, size_t count)
{
	size_t overwritten = 0;

	while (count--) {
		unsigned int tail;
		if (fifo->count == URP_NATIVE_FIFO_SAMPLES) {
			fifo->head = (fifo->head + 1U) % URP_NATIVE_FIFO_SAMPLES;
			fifo->count--;
			overwritten++;
		}
		tail = (fifo->head + fifo->count) % URP_NATIVE_FIFO_SAMPLES;
		fifo->samples[tail] = *samples++;
		fifo->count++;
	}
	return overwritten;
}

int urp_native_fifo_pop(struct urp_native_fifo *fifo, short *samples)
{
	size_t i;

	if (fifo->count < URP_NATIVE_SAMPLES)
		return 0;
	for (i = 0; i < URP_NATIVE_SAMPLES; ++i) {
		samples[i] = fifo->samples[fifo->head];
		fifo->head = (fifo->head + 1U) % URP_NATIVE_FIFO_SAMPLES;
	}
	fifo->count -= URP_NATIVE_SAMPLES;
	return 1;
}

void urp_native_fifo_reset(struct urp_native_fifo *fifo)
{
	fifo->head = 0;
	fifo->count = 0;
	fifo->primed = 0;
}

void urp_native_fifo_note_underrun(struct urp_native_fifo *fifo)
{
	unsigned int target = fifo->target_samples ? fifo->target_samples : URP_FIFO_TARGET_NORMAL;
	fifo->target_samples = target < URP_FIFO_TARGET_MAX - URP_FIFO_TARGET_STEP
				       ? target + URP_FIFO_TARGET_STEP
				       : URP_FIFO_TARGET_MAX;
	fifo->stable_blocks = 0;
}

void urp_native_fifo_note_stable(struct urp_native_fifo *fifo)
{
	if (!fifo->target_samples)
		fifo->target_samples = URP_FIFO_TARGET_NORMAL;
	if (++fifo->stable_blocks < URP_FIFO_TARGET_DECAY_BLOCKS)
		return;
	fifo->stable_blocks = 0;
	if (fifo->target_samples > URP_FIFO_TARGET_MIN + URP_FIFO_TARGET_STEP)
		fifo->target_samples -= URP_FIFO_TARGET_STEP;
	else
		fifo->target_samples = URP_FIFO_TARGET_MIN;
}

int urp_native_fifo_render(struct urp_native_fifo *fifo, short *samples)
{
	size_t i, available = fifo->count;

	if (available >= URP_NATIVE_SAMPLES) {
		short previous[URP_NATIVE_SAMPLES];
		int recovering = fifo->concealing && fifo->have_history;
		memcpy(previous, fifo->history, sizeof(previous));
		(void)urp_native_fifo_pop(fifo, samples);
		if (recovering) {
			const size_t crossfade = URP_NATIVE_SAMPLES / 20U;
			for (i = 0; i < crossfade; ++i) {
				double mix = (double)(i + 1U) / (double)crossfade;
				samples[i] = (short)lrint(
					previous[URP_NATIVE_SAMPLES - crossfade + i] * (1.0 - mix) +
					samples[i] * mix);
			}
		}
		memcpy(fifo->history, samples, sizeof(fifo->history));
		fifo->have_history = 1;
		fifo->concealing = 0;
		return 1;
	}

	for (i = 0; i < available; ++i) {
		samples[i] = fifo->samples[fifo->head];
		fifo->head = (fifo->head + 1U) % URP_NATIVE_FIFO_SAMPLES;
	}
	fifo->count = 0;
	for (; i < URP_NATIVE_SAMPLES; ++i) {
		double fade = 1.0 - (double)(i - available + 1U) /
					    (double)(URP_NATIVE_SAMPLES - available + 1U);
		short source =
			fifo->have_history
				? fifo->history[(URP_NATIVE_SAMPLES / 2U + i) % URP_NATIVE_SAMPLES]
				: 0;
		samples[i] = (short)lrint(source * fade);
	}
	memcpy(fifo->history, samples, sizeof(fifo->history));
	fifo->have_history = 1;
	fifo->concealing = 1;
	return 0;
}

int urp_gain_db_to_mixer(double gain_db)
{
	double setting = 500.0 * pow(10.0, gain_db / 20.0);
	return setting > 999.0 ? 999 : (int)floor(setting + 0.5);
}

double urp_mixer_to_gain_db(int setting)
{
	return 20.0 * log10(fmax(0.000001, (double)setting / 500.0));
}

int urp_hardware_level_multiplier(int value)
{
	const int unity = 256;
	int pot = (value / 4) * 4 + 2;
	return unity - (unity * (3 - value % 4)) / (pot + 2);
}

short urp_saturating_add(short left, short right)
{
	int value = (int)left + (int)right;
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (short)value;
}

short urp_apply_gain(short sample, double linear)
{
	double value = sample * linear;
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (short)lrint(value);
}

unsigned int urp_pcm_peak(const short *samples, size_t count)
{
	unsigned int peak = 0;
	size_t i;
	for (i = 0; i < count; ++i) {
		unsigned int value =
			samples[i] == INT16_MIN ? 32768U : (unsigned int)abs(samples[i]);
		if (value > peak)
			peak = value;
	}
	return peak;
}

double urp_pcm_peak_dbfs(unsigned int peak)
{
	return peak ? 20.0 * log10((double)peak / 32768.0) : -INFINITY;
}

double urp_double_peak(const double *samples, size_t count)
{
	double peak = 0.0;
	size_t i;
	for (i = 0; i < count; ++i) {
		double value = fabs(samples[i]);
		if (value > peak)
			peak = value;
	}
	return peak;
}

int urp_tx_output_has_program(enum urp_tx_output_mode mode)
{
	return mode == URP_TX_OUTPUT_VOICE || mode == URP_TX_OUTPUT_COMPOSITE ||
	       mode == URP_TX_OUTPUT_AUX_VOICE;
}

int urp_tx_output_has_voice(enum urp_tx_output_mode mode)
{
	return mode == URP_TX_OUTPUT_VOICE || mode == URP_TX_OUTPUT_COMPOSITE;
}

int urp_tx_output_has_tone(enum urp_tx_output_mode mode)
{
	return mode == URP_TX_OUTPUT_TONE || mode == URP_TX_OUTPUT_COMPOSITE;
}

int urp_tx_pair_has_voice(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b)
{
	return urp_tx_output_has_voice(output_a) || urp_tx_output_has_voice(output_b);
}

int urp_tx_pair_has_tone(enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b)
{
	return urp_tx_output_has_tone(output_a) || urp_tx_output_has_tone(output_b);
}

int urp_tx_tone_route_missing(const char *frequency, enum urp_tx_output_mode output_a,
			      enum urp_tx_output_mode output_b)
{
	return frequency[0] && !urp_tx_pair_has_tone(output_a, output_b);
}

int urp_parallel_pulser_needed(int parallel_port_enabled, int output_configured)
{
	return parallel_port_enabled && output_configured;
}

int urp_native_echo_enabled(int duplex3_level, int software_mode)
{
	return duplex3_level > 0 && software_mode;
}

void urp_apply_ptt_outputs(int asserted, int inverted, int parallel_mask, int usb_mask,
			   int32_t *usb_value, int8_t *parallel_value)
{
	*usb_value &= ~usb_mask;
	*parallel_value &= (int8_t)~parallel_mask;
	if (!!asserted != !!inverted) {
		*usb_value |= usb_mask;
		*parallel_value |= (int8_t)parallel_mask;
	}
}

int urp_parrot_rx_transition(struct urp_parrot_state *state, int was_keyed, int is_keyed)
{
	if (!was_keyed && is_keyed) {
		state->count = 0;
		state->play = 0;
		state->playing = 0;
		state->truncated = 0;
	} else if (was_keyed && !is_keyed && state->count) {
		state->play = 0;
		state->playing = 1;
		return 1;
	}
	return 0;
}

size_t urp_parrot_play(struct urp_parrot_state *state, double *output, size_t count)
{
	size_t remaining;

	if (!state->playing)
		return 0;
	remaining = state->count - state->play;
	if (count > remaining)
		count = remaining;
	memcpy(output, state->audio + state->play, count * sizeof(*output));
	state->play += count;
	if (state->play >= state->count)
		state->playing = 0;
	return count;
}

size_t urp_parrot_record(struct urp_parrot_state *state, const double *input, size_t count,
			 size_t limit)
{
	size_t space = limit > state->count ? limit - state->count : 0;
	size_t requested = count;
	if (count > space)
		count = space;
	if (count)
		memcpy(state->audio + state->count, input, count * sizeof(*input));
	state->count += count;
	if (count < requested)
		state->truncated = 1;
	return count;
}

void urp_prepare_receive_block(const short *stereo, short *pcm, double *working, size_t count,
			       short *delay, size_t delay_samples, unsigned int *delay_index,
			       struct urp_receive_block_stats *stats)
{
	size_t i;

	stats->peak = 0;
	stats->rail_samples = 0;
	if (delay_samples && *delay_index >= delay_samples)
		*delay_index = 0;
	for (i = 0; i < count; ++i) {
		unsigned int magnitude;
		short sample = stereo[i * 2];

		magnitude = sample == INT16_MIN ? 32768U : (unsigned int)abs(sample);
		if (magnitude > stats->peak)
			stats->peak = magnitude;
		if (sample == INT16_MAX || sample == INT16_MIN)
			stats->rail_samples++;
		if (delay_samples) {
			short delayed = delay[*delay_index];
			delay[*delay_index] = sample;
			sample = delayed;
			if (++*delay_index == delay_samples)
				*delay_index = 0;
		}
		pcm[i] = sample;
		working[i] = sample;
	}
}

unsigned long urp_render_transmit_block(const double *program, const double *ctcss, size_t count,
					enum urp_tx_output_mode output_a,
					enum urp_tx_output_mode output_b, double ctcss_peak_a,
					double ctcss_bias_a, double ctcss_peak_b,
					double ctcss_bias_b, short *stereo, short *meter_stereo)
{
	unsigned long rail_samples = 0;
	size_t i;

	for (i = 0; i < count; ++i) {
		short output;

		if (program[i] > INT16_MAX || program[i] < INT16_MIN)
			rail_samples++;
		output = (short)lrint(fmax(INT16_MIN, fmin(INT16_MAX, program[i])));
		if (meter_stereo) {
			meter_stereo[i * 2] = output;
			meter_stereo[i * 2 + 1] = output;
		}
		if (urp_tx_output_has_program(output_a))
			stereo[i * 2] = urp_saturating_add(stereo[i * 2], output);
		if (urp_tx_output_has_program(output_b))
			stereo[i * 2 + 1] = urp_saturating_add(stereo[i * 2 + 1], output);
		if (output_a == URP_TX_OUTPUT_TONE || output_a == URP_TX_OUTPUT_COMPOSITE) {
			short tone = (short)lrint(
				fmax(INT16_MIN,
				     fmin(INT16_MAX, ctcss[i] * ctcss_peak_a + ctcss_bias_a)));
			stereo[i * 2] = urp_saturating_add(stereo[i * 2], tone);
		}
		if (output_b == URP_TX_OUTPUT_TONE || output_b == URP_TX_OUTPUT_COMPOSITE) {
			short tone = (short)lrint(
				fmax(INT16_MIN,
				     fmin(INT16_MAX, ctcss[i] * ctcss_peak_b + ctcss_bias_b)));
			stereo[i * 2 + 1] = urp_saturating_add(stereo[i * 2 + 1], tone);
		}
	}
	return rail_samples;
}

int urp_parse_rx_audio_mode(const char *text, enum urp_rx_audio_mode *mode)
{
	static const struct urp_named_value values[] = {
		{"no", URP_RX_AUDIO_DISABLED},
		{"speaker", URP_RX_AUDIO_SPEAKER},
		{"flat", URP_RX_AUDIO_FLAT},
	};
	int result;
	if (!mode)
		return -1;
	if (parse_named_value(text, values, sizeof(values) / sizeof(values[0]), &result))
		return -1;
	*mode = (enum urp_rx_audio_mode)result;
	return 0;
}

int urp_parse_tx_output_mode(const char *text, enum urp_tx_output_mode *mode)
{
	static const struct urp_named_value values[] = {
		{"no", URP_TX_OUTPUT_DISABLED},	       {"voice", URP_TX_OUTPUT_VOICE},
		{"tone", URP_TX_OUTPUT_TONE},	       {"composite", URP_TX_OUTPUT_COMPOSITE},
		{"auxvoice", URP_TX_OUTPUT_AUX_VOICE},
	};
	int result;
	if (!mode)
		return -1;
	if (parse_named_value(text, values, sizeof(values) / sizeof(values[0]), &result))
		return -1;
	*mode = (enum urp_tx_output_mode)result;
	return 0;
}

int urp_parse_carrier_source(const char *text, enum urp_carrier_source *source)
{
	static const struct urp_named_value values[] = {
		{"no", URP_CARRIER_DISABLED},
		{"dsp", URP_CARRIER_DSP},
		{"vox", URP_CARRIER_VOX},
		{"usb", URP_CARRIER_USB},
		{"usbinvert", URP_CARRIER_USB_INVERTED},
		{"pp", URP_CARRIER_PARALLEL},
		{"ppinvert", URP_CARRIER_PARALLEL_INVERTED},
	};
	int result;
	if (!source)
		return -1;
	if (parse_named_value(text, values, sizeof(values) / sizeof(values[0]), &result))
		return -1;
	*source = (enum urp_carrier_source)result;
	return 0;
}

int urp_parse_ctcss_source(const char *text, enum urp_ctcss_source *source)
{
	static const struct urp_named_value values[] = {
		{"no", URP_CTCSS_DISABLED},
		{"usb", URP_CTCSS_USB},
		{"usbinvert", URP_CTCSS_USB_INVERTED},
		{"dsp", URP_CTCSS_DSP},
		{"pp", URP_CTCSS_PARALLEL},
		{"ppinvert", URP_CTCSS_PARALLEL_INVERTED},
	};
	int result;
	if (!source)
		return -1;
	if (parse_named_value(text, values, sizeof(values) / sizeof(values[0]), &result))
		return -1;
	*source = (enum urp_ctcss_source)result;
	return 0;
}

int urp_parse_tone_off_mode(const char *text, enum urp_tone_off_mode *mode)
{
	static const struct urp_named_value values[] = {
		{"no", URP_TONE_OFF_NONE},
		{"phase", URP_TONE_OFF_PHASE_REVERSE},
		{"notone", URP_TONE_OFF_REMOVE},
	};
	int result;
	if (!mode)
		return -1;
	if (parse_named_value(text, values, sizeof(values) / sizeof(values[0]), &result))
		return -1;
	*mode = (enum urp_tone_off_mode)result;
	return 0;
}
