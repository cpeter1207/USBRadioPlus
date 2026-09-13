/** @file
 * @brief Transport-independent PCM conversion, routing, elastic queues, and echo state.
 */

#include "usbradioplus_channel_core.h"

#include "usbradioplus_radio_core_adapter.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void urp_sample_queue_init(struct urp_sample_queue *queue, short *samples, unsigned int capacity)
{
	queue->samples = samples;
	queue->capacity = capacity;
	atomic_init(&queue->read, 0U);
	atomic_init(&queue->write, 0U);
	atomic_init(&queue->high_water, 0U);
}

void urp_sample_queue_reset(struct urp_sample_queue *queue)
{
	atomic_store_explicit(&queue->read, 0U, memory_order_relaxed);
	atomic_store_explicit(&queue->write, 0U, memory_order_relaxed);
	atomic_store_explicit(&queue->high_water, 0U, memory_order_relaxed);
}

void urp_sample_queue_discard(struct urp_sample_queue *queue)
{
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_acquire);

	/* Only the consumer advances read.  Keeping write monotonic means a
	 * concurrent producer cannot be reset underneath its current publication. */
	atomic_store_explicit(&queue->read, write, memory_order_release);
}

unsigned int urp_sample_queue_samples(const struct urp_sample_queue *queue)
{
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_acquire);
	unsigned int read = atomic_load_explicit(&queue->read, memory_order_acquire);

	return write - read;
}

/** @brief Record a producer-owned sample-ring occupancy peak.
 * @param queue Queue whose high-water mark is updated.
 * @param occupancy Newly observed queue occupancy.
 */
static void urp_sample_queue_note_high_water(struct urp_sample_queue *queue, unsigned int occupancy)
{
	unsigned int high_water = atomic_load_explicit(&queue->high_water, memory_order_relaxed);

	/* The single producer is the only writer, so no compare/exchange retry is needed. */
	if (occupancy > high_water)
		atomic_store_explicit(&queue->high_water, occupancy, memory_order_relaxed);
}

int urp_sample_queue_push_sample(struct urp_sample_queue *queue, short sample)
{
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_relaxed);
	unsigned int read = atomic_load_explicit(&queue->read, memory_order_acquire);

	if (!queue->samples || !queue->capacity)
		return 0;
	if (write - read >= queue->capacity)
		return 0;
	queue->samples[write % queue->capacity] = sample;
	atomic_store_explicit(&queue->write, write + 1U, memory_order_release);
	urp_sample_queue_note_high_water(queue, write + 1U - read);
	return 1;
}

int urp_sample_queue_pop_sample(struct urp_sample_queue *queue, short *sample)
{
	unsigned int read = atomic_load_explicit(&queue->read, memory_order_relaxed);
	unsigned int write = atomic_load_explicit(&queue->write, memory_order_acquire);

	if (!queue->samples || !queue->capacity)
		return 0;
	if (read == write)
		return 0;
	*sample = queue->samples[read % queue->capacity];
	atomic_store_explicit(&queue->read, read + 1U, memory_order_release);
	return 1;
}

unsigned int urp_sample_queue_high_water(const struct urp_sample_queue *queue)
{
	return atomic_load_explicit(&queue->high_water, memory_order_relaxed);
}

void urp_sample_queue_reset_high_water(struct urp_sample_queue *queue)
{
	atomic_store_explicit(&queue->high_water, urp_sample_queue_samples(queue),
			      memory_order_relaxed);
}

/** @brief Clamp an adapter output-stage capacity to its preallocated storage.
 * @param capacity Requested complete-block capacity.
 * @return Capacity bounded to the preallocated block range.
 */
static unsigned int urp_native_output_stage_capacity(unsigned int capacity)
{
	if (capacity < 2U)
		return 2U;
	if (capacity > URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS)
		return URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS;
	return capacity;
}

/** @brief Return the number of complete blocks retained by an output stage.
 * @param stage Output stage to inspect.
 * @return Number of current and pending blocks.
 */
static unsigned int urp_native_output_stage_count(const struct urp_native_output_stage *stage)
{
	return stage->pending_count + (stage->current_valid ? 1U : 0U);
}

/** @brief Promote the oldest complete pending block to the partial-write slot.
 * @param stage Output stage whose current slot may be replenished.
 */
static void urp_native_output_stage_promote(struct urp_native_output_stage *stage)
{
	if (stage->current_valid || !stage->pending_count)
		return;
	stage->current = stage->pending[stage->pending_head];
	stage->pending_head = (stage->pending_head + 1U) % URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS;
	--stage->pending_count;
	stage->current_valid = 1;
}

void urp_native_output_stage_init(struct urp_native_output_stage *stage, unsigned int capacity,
				  size_t maximum_frame_count)
{
	if (!stage)
		return;
	memset(stage, 0, sizeof(*stage));
	stage->capacity = urp_native_output_stage_capacity(capacity);
	if (!maximum_frame_count || maximum_frame_count > URP_NATIVE_MAX_SAMPLES)
		maximum_frame_count = URP_NATIVE_MAX_SAMPLES;
	stage->maximum_frame_count = maximum_frame_count;
}

void urp_native_output_stage_reset(struct urp_native_output_stage *stage)
{
	if (!stage)
		return;
	stage->pending_head = 0U;
	stage->pending_count = 0U;
	stage->high_water = 0U;
	stage->dropped_complete_blocks = 0U;
	stage->partial_writes = 0U;
	stage->stalled_partial_frames = 0U;
	stage->current_valid = 0;
}

int urp_native_output_stage_set_capacity(struct urp_native_output_stage *stage,
					 unsigned int capacity)
{
	if (!stage || urp_native_output_stage_count(stage))
		return 0;
	stage->capacity = urp_native_output_stage_capacity(capacity);
	return 1;
}

int urp_native_output_stage_enqueue(struct urp_native_output_stage *stage, const short *pcm,
				    size_t frame_count, int logical_ptt, int audio_bearing)
{
	struct urp_native_output_block *block;
	unsigned int occupancy;
	int dropped = 0;

	if (!stage || !pcm || !frame_count || frame_count > stage->maximum_frame_count ||
	    !stage->capacity)
		return -1;
	while (urp_native_output_stage_count(stage) >= stage->capacity) {
		/* A partially submitted prefix is immutable. Discard the oldest complete
		 * block behind it instead, preserving the exact remaining PCM sequence. */
		if (stage->current_valid && !stage->current.submitted_frames) {
			stage->current_valid = 0;
			urp_native_output_stage_promote(stage);
		} else if (stage->pending_count) {
			stage->pending_head =
				(stage->pending_head + 1U) % URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS;
			--stage->pending_count;
		} else {
			return -1;
		}
		++stage->dropped_complete_blocks;
		dropped = 1;
	}
	if (!stage->current_valid) {
		block = &stage->current;
		stage->current_valid = 1;
	} else {
		const unsigned int tail = (stage->pending_head + stage->pending_count) %
					  URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS;
		block = &stage->pending[tail];
		++stage->pending_count;
	}
	memcpy(block->pcm, pcm, frame_count * 2U * sizeof(*pcm));
	block->frame_count = frame_count;
	block->submitted_frames = 0U;
	block->logical_ptt = !!logical_ptt;
	block->audio_bearing = !!audio_bearing;
	occupancy = urp_native_output_stage_count(stage);
	if (occupancy > stage->high_water)
		stage->high_water = occupancy;
	return dropped ? 0 : 1;
}

struct urp_native_output_block *urp_native_output_stage_peek(struct urp_native_output_stage *stage)
{
	if (!stage || !stage->current_valid)
		return NULL;
	return &stage->current;
}

int urp_native_output_stage_commit(struct urp_native_output_stage *stage, size_t frame_count,
				   struct urp_native_output_block *finished)
{
	struct urp_native_output_block *block;
	size_t remaining;

	block = urp_native_output_stage_peek(stage);
	if (!block || !frame_count || block->submitted_frames > block->frame_count)
		return -1;
	remaining = block->frame_count - block->submitted_frames;
	if (frame_count > remaining)
		return -1;
	block->submitted_frames += frame_count;
	if (block->submitted_frames != block->frame_count) {
		++stage->partial_writes;
		return 0;
	}
	if (finished)
		*finished = *block;
	/* Age is measured from the first partial submission until the entire block
	 * completes.  Resetting it on a one-sample trickle would retain stale PCM
	 * and keyed output indefinitely on a persistently congested device. */
	stage->stalled_partial_frames = 0U;
	stage->current_valid = 0;
	urp_native_output_stage_promote(stage);
	return 1;
}

int urp_native_output_stage_has_ptt(const struct urp_native_output_stage *stage)
{
	unsigned int pending;

	if (!stage)
		return 0;
	if (stage->current_valid && stage->current.logical_ptt)
		return 1;
	for (pending = 0U; pending < stage->pending_count; ++pending) {
		const unsigned int index =
			(stage->pending_head + pending) % URP_ADAPTER_OUTPUT_STAGE_MAX_BLOCKS;
		if (stage->pending[index].logical_ptt)
			return 1;
	}
	return 0;
}

int urp_native_output_stage_note_unavailable(struct urp_native_output_stage *stage,
					     size_t elapsed_frames)
{
	uint64_t limit;
	uint64_t elapsed;

	if (!stage || !stage->current_valid || !stage->current.submitted_frames) {
		if (stage)
			stage->stalled_partial_frames = 0U;
		return 0;
	}
	limit = (uint64_t)stage->capacity * stage->maximum_frame_count;
	if (!limit)
		return 0;
	if (!elapsed_frames)
		return stage->stalled_partial_frames >= limit;
	elapsed = elapsed_frames;
	if (UINT64_MAX - stage->stalled_partial_frames < elapsed)
		stage->stalled_partial_frames = UINT64_MAX;
	else
		stage->stalled_partial_frames += elapsed;
	return stage->stalled_partial_frames >= limit;
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

int urp_tx_signaling_route_missing(int signaling_enabled, enum urp_tx_output_mode output_a,
				   enum urp_tx_output_mode output_b)
{
	return signaling_enabled && !urp_tx_pair_has_tone(output_a, output_b);
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

/** @brief Convert one compatibility output route to its portable ABI value.
 * @param source Compatibility output route.
 * @param destination Receives the matching portable ABI value.
 * @return Zero on success, or minus one for an unsupported route or missing output.
 */
static int radio_core_output_route(enum urp_tx_output_mode source, uint32_t *destination)
{
	if (!destination)
		return -1;
	switch (source) {
	case URP_TX_OUTPUT_DISABLED:
		*destination = RPTADV_RADIO_TX_OUTPUT_DISABLED;
		return 0;
	case URP_TX_OUTPUT_VOICE:
		*destination = RPTADV_RADIO_TX_OUTPUT_VOICE;
		return 0;
	case URP_TX_OUTPUT_TONE:
		*destination = RPTADV_RADIO_TX_OUTPUT_TONE;
		return 0;
	case URP_TX_OUTPUT_COMPOSITE:
		*destination = RPTADV_RADIO_TX_OUTPUT_COMPOSITE;
		return 0;
	case URP_TX_OUTPUT_AUX_VOICE:
		*destination = RPTADV_RADIO_TX_OUTPUT_AUX_VOICE;
		return 0;
	}
	return -1;
}

int urp_render_transmit_block(const struct rptadv_radio *radio, const double *program,
			      const float *ctcss, const float *dcs, size_t count,
			      enum urp_tx_output_mode output_a, enum urp_tx_output_mode output_b,
			      double ctcss_peak_a, double ctcss_bias_a, double ctcss_peak_b,
			      double ctcss_bias_b, struct urp_transmit_render_workspace *workspace,
			      short *stereo, short *meter_stereo, unsigned long *rail_samples)
{
	const struct rptadv_radio_descriptor *descriptor = urp_radio_core_descriptor_get();
	struct rptadv_radio_transmit_render_config config = {
		.struct_size = sizeof(config),
	};
	uint64_t rendered_rails = 0;
	size_t i;

	if (!radio || !workspace || !rail_samples || count > URP_NATIVE_MAX_SAMPLES ||
	    (count && (!program || !ctcss || !dcs || !stereo)) ||
	    radio_core_output_route(output_a, &config.output_a_route) ||
	    radio_core_output_route(output_b, &config.output_b_route) || !descriptor)
		return -1;
	/* Only the unmigrated program graph retains PCM-code doubles. */
	for (i = 0; i < count; ++i)
		workspace->program[i] = (float)(program[i] / 32767.0);
	config.ctcss_peak_a = (float)(ctcss_peak_a / 32767.0);
	config.ctcss_bias_a = (float)(ctcss_bias_a / 32767.0);
	config.ctcss_peak_b = (float)(ctcss_peak_b / 32767.0);
	config.ctcss_bias_b = (float)(ctcss_bias_b / 32767.0);
	if (descriptor->radio_render_transmit_f32(
		    radio, workspace->program, ctcss, dcs, (uint32_t)count, &config,
		    (int16_t *)stereo, (int16_t *)meter_stereo, &rendered_rails) != RPTADV_RADIO_OK)
		return -1;
	*rail_samples = rendered_rails > ULONG_MAX ? ULONG_MAX : (unsigned long)rendered_rails;
	return 0;
}

int urp_parse_rx_audio_mode(const char *text, enum urp_rx_audio_mode *mode)
{
	uint32_t parsed = 0U;

	if (!text || !mode || urp_radio_core_parse_rx_audio_mode(text, &parsed))
		return -1;
	switch (parsed) {
	case RPTADV_RADIO_RX_AUDIO_DISABLED:
		*mode = URP_RX_AUDIO_DISABLED;
		return 0;
	case RPTADV_RADIO_RX_AUDIO_SPEAKER:
		*mode = URP_RX_AUDIO_SPEAKER;
		return 0;
	case RPTADV_RADIO_RX_AUDIO_FLAT:
		*mode = URP_RX_AUDIO_FLAT;
		return 0;
	default:
		return -1;
	}
}

int urp_parse_carrier_source(const char *text, enum urp_carrier_source *source)
{
	uint32_t parsed = 0U;

	if (!text || !source || urp_radio_core_parse_carrier_source(text, &parsed))
		return -1;
	switch (parsed) {
	case RPTADV_RADIO_CARRIER_DISABLED:
		*source = URP_CARRIER_DISABLED;
		return 0;
	case RPTADV_RADIO_CARRIER_DSP:
		*source = URP_CARRIER_DSP;
		return 0;
	case RPTADV_RADIO_CARRIER_VOX:
		*source = URP_CARRIER_VOX;
		return 0;
	case RPTADV_RADIO_CARRIER_USB:
		*source = URP_CARRIER_USB;
		return 0;
	case RPTADV_RADIO_CARRIER_USB_INVERTED:
		*source = URP_CARRIER_USB_INVERTED;
		return 0;
	case RPTADV_RADIO_CARRIER_PARALLEL:
		*source = URP_CARRIER_PARALLEL;
		return 0;
	case RPTADV_RADIO_CARRIER_PARALLEL_INVERTED:
		*source = URP_CARRIER_PARALLEL_INVERTED;
		return 0;
	default:
		return -1;
	}
}

int urp_parse_ctcss_source(const char *text, enum urp_ctcss_source *source)
{
	uint32_t parsed = 0U;

	if (!text || !source || urp_radio_core_parse_ctcss_source(text, &parsed))
		return -1;
	switch (parsed) {
	case RPTADV_RADIO_CTCSS_DISABLED:
		*source = URP_CTCSS_DISABLED;
		return 0;
	case RPTADV_RADIO_CTCSS_USB:
		*source = URP_CTCSS_USB;
		return 0;
	case RPTADV_RADIO_CTCSS_USB_INVERTED:
		*source = URP_CTCSS_USB_INVERTED;
		return 0;
	case RPTADV_RADIO_CTCSS_DSP:
		*source = URP_CTCSS_DSP;
		return 0;
	case RPTADV_RADIO_CTCSS_PARALLEL:
		*source = URP_CTCSS_PARALLEL;
		return 0;
	case RPTADV_RADIO_CTCSS_PARALLEL_INVERTED:
		*source = URP_CTCSS_PARALLEL_INVERTED;
		return 0;
	default:
		return -1;
	}
}

int urp_parse_tone_off_mode(const char *text, enum urp_tone_off_mode *mode)
{
	uint32_t parsed = 0U;

	if (!text || !mode || urp_radio_core_parse_tone_off_mode(text, &parsed))
		return -1;
	switch (parsed) {
	case RPTADV_RADIO_TONE_OFF_NONE:
		*mode = URP_TONE_OFF_NONE;
		return 0;
	case RPTADV_RADIO_TONE_OFF_PHASE_SHIFT:
		*mode = URP_TONE_OFF_PHASE_SHIFT;
		return 0;
	case RPTADV_RADIO_TONE_OFF_TONE_REMOVE:
		*mode = URP_TONE_OFF_TONE_REMOVE;
		return 0;
	case RPTADV_RADIO_TONE_OFF_TAIL_TONE:
		*mode = URP_TONE_OFF_TAIL_TONE;
		return 0;
	default:
		return -1;
	}
}
