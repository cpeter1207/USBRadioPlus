/** @file
 * @brief Sample-rate conversion, elastic clock recovery, and receive-echo matching.
 */

#ifdef AST_MODULE
#include "asterisk.h"
#endif

#include "usbradioplus_dsp.h"

#include <math.h>
#include <samplerate.h>
#include <stdlib.h>
#include <string.h>

#ifdef AST_MODULE
#include "asterisk/utils.h"
#define URP_CALLOC(n, s) ast_calloc((n), (s))
#define URP_REALLOC(p, s) ast_realloc((p), (s))
#define URP_FREE(p) ast_free((p))
#elif defined(URP_TEST_ALLOCATORS)
void *urp_test_calloc(size_t count, size_t size);
void *urp_test_realloc(void *pointer, size_t size);
#define URP_CALLOC(n, s) urp_test_calloc((n), (s))
#define URP_REALLOC(p, s) urp_test_realloc((p), (s))
#define URP_FREE(p) free((p))
#else

#define URP_CALLOC(n, s) calloc((n), (s))

#define URP_REALLOC(p, s) realloc((p), (s))

#define URP_FREE(p) free((p))
#endif

#ifndef M_PI

#define M_PI 3.14159265358979323846
#endif

/** Owned libsamplerate state and reusable floating-point conversion buffers. */
struct urp_src {
	/** Owned libsamplerate converter handle. */
	SRC_STATE *state;
	/** Number of interleaved channels. */
	unsigned int channels;
	/** Owned floating-point input-conversion workspace. */
	float *input;
	/** Output workspace for the current conversion. */
	float *output;
	/** Allocated input workspace in samples. */
	size_t input_capacity;
	/** Allocated output workspace in samples. */
	size_t output_capacity;
};

void urp_clock_recovery_reset(struct urp_clock_recovery *clock)
{
	if (clock)
		memset(clock, 0, sizeof(*clock));
}

double urp_clock_recovery_update(struct urp_clock_recovery *clock, size_t queued_samples,
				 size_t target_samples)
{
	double error, desired;

	if (!clock || !target_samples)
		return 0.0;
	error = ((double)target_samples - (double)queued_samples) / (double)target_samples;
	/* Filter scheduler jitter, then use a small integral term to follow a
	 * persistent oscillator mismatch without requiring extra FIFO latency. */
	clock->filtered_error += (error - clock->filtered_error) * 0.10;
	clock->integral_error += clock->filtered_error * 0.00002;
	if (clock->integral_error > URP_CLOCK_MAX_CORRECTION)
		clock->integral_error = URP_CLOCK_MAX_CORRECTION;
	if (clock->integral_error < -URP_CLOCK_MAX_CORRECTION)
		clock->integral_error = -URP_CLOCK_MAX_CORRECTION;
	desired = clock->filtered_error * 0.006 + clock->integral_error;
	if (desired > URP_CLOCK_MAX_CORRECTION)
		desired = URP_CLOCK_MAX_CORRECTION;
	if (desired < -URP_CLOCK_MAX_CORRECTION)
		desired = -URP_CLOCK_MAX_CORRECTION;
	/* Smooth ratio changes so clock correction cannot modulate speech abruptly. */
	clock->correction += (desired - clock->correction) * 0.10;
	return clock->correction;
}

/** @brief Round and clamp a sample to the signed 16-bit PCM range.
 * @param value Sample amplitude in signed PCM codes.
 * @return Nearest bounded signed 16-bit PCM sample.
 */
static int16_t saturate(double value)
{
	if (value > 32767.0)
		return 32767;
	if (value < -32768.0)
		return -32768;
	return (int16_t)lrint(value);
}

struct urp_src *urp_src_create(int converter, unsigned int channels)
{
	struct urp_src *src;
	int error = 0;
	if (!channels)
		return NULL;
	src = URP_CALLOC(1, sizeof(*src));
	if (!src)
		return NULL;
	src->state = src_new(converter, (int)channels, &error);
	if (!src->state) {
		URP_FREE(src);
		return NULL;
	}
	src->channels = channels;
	return src;
}

void urp_src_destroy(struct urp_src *src)
{
	if (!src)
		return;
	src_delete(src->state);
	URP_FREE(src->input);
	URP_FREE(src->output);
	URP_FREE(src);
}

void urp_src_reset(struct urp_src *src)
{
	if (src)
		src_reset(src->state);
}

int urp_src_process(struct urp_src *src, const int16_t *input, size_t input_count, int16_t *output,
		    size_t output_capacity, double ratio, size_t *input_used,
		    size_t *output_generated)
{
	SRC_DATA data;
	size_t i, in_values, out_values;
	int result;
	if (!src || !input || !output || ratio <= 0.0)
		return -1;
	in_values = input_count * src->channels;
	out_values = output_capacity * src->channels;
	if (in_values > src->input_capacity) {
		float *p = URP_REALLOC(src->input, in_values * sizeof(*p));
		if (!p)
			return -1;
		src->input = p;
		src->input_capacity = in_values;
	}
	if (out_values > src->output_capacity) {
		float *p = URP_REALLOC(src->output, out_values * sizeof(*p));
		if (!p)
			return -1;
		src->output = p;
		src->output_capacity = out_values;
	}
	src_short_to_float_array(input, src->input, (int)in_values);
	memset(&data, 0, sizeof(data));
	data.data_in = src->input;
	data.data_out = src->output;
	data.input_frames = (long)input_count;
	data.output_frames = (long)output_capacity;
	data.src_ratio = ratio;
	result = src_process(src->state, &data);
	if (result)
		return result;
	src_float_to_short_array(src->output, output,
				 (int)(data.output_frames_gen * src->channels));
	if (input_used)
		*input_used = (size_t)data.input_frames_used;
	if (output_generated)
		*output_generated = (size_t)data.output_frames_gen;
	for (i = data.output_frames_gen * src->channels; i < out_values; ++i)
		output[i] = 0;
	return 0;
}

int urp_rate_convert(struct urp_src *src, const int16_t *input, size_t input_count,
		     unsigned int input_rate, int16_t *output, size_t output_capacity,
		     unsigned int output_rate, size_t *input_used, size_t *output_generated)
{
	size_t copied;
	if (!input || !output || !input_rate || !output_rate)
		return -1;
	if (input_rate != output_rate) {
		return urp_src_process(src, input, input_count, output, output_capacity,
				       (double)output_rate / input_rate, input_used,
				       output_generated);
	}
	/* Matching rates need no converter state, allocation, or filter delay. */
	copied = input_count < output_capacity ? input_count : output_capacity;
	memmove(output, input, copied * sizeof(*output));
	if (copied < output_capacity)
		memset(output + copied, 0, (output_capacity - copied) * sizeof(*output));
	if (input_used)
		*input_used = copied;
	if (output_generated)
		*output_generated = copied;
	return input_count <= output_capacity ? 0 : -1;
}

void urp_extract_mono(const int16_t *stereo, int16_t *mono, size_t frames, unsigned int channel)
{
	size_t i;
	channel = channel ? 1 : 0;
	for (i = 0; i < frames; ++i)
		mono[i] = stereo[i * 2 + channel];
}

void urp_duplicate_mono(const int16_t *mono, int16_t *stereo, size_t frames, double gain_a,
			double gain_b)
{
	size_t i;
	for (i = 0; i < frames; ++i) {
		stereo[i * 2] = saturate(mono[i] * gain_a);
		stereo[i * 2 + 1] = saturate(mono[i] * gain_b);
	}
}

void urp_echo_init(struct urp_echo_replacer *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->last_delay_frames = -1;
}

void urp_echo_push(struct urp_echo_replacer *s, const int16_t *link, const int16_t *native)
{
	struct urp_echo_frame *f;
	if (!s || !link || !native)
		return;
	f = &s->history[s->write_index];
	memcpy(f->link, link, sizeof(f->link));
	memcpy(f->native, native, sizeof(f->native));
	f->sequence = ++s->sequence;
	s->write_index = (s->write_index + 1) % URP_ECHO_HISTORY_FRAMES;
}

int urp_echo_remove(struct urp_echo_replacer *s, int16_t *mixed, int16_t *matched_native,
		    double minimum_correlation)
{
	double best_corr = -1.0, best_scale = 0.0;
	unsigned int best = 0, n;
	int found = 0;
	if (!s || !mixed || !matched_native)
		return 0;
	for (n = 0; n < URP_ECHO_HISTORY_FRAMES; ++n) {
		const struct urp_echo_frame *f = &s->history[n];
		double xy = 0.0, xx = 0.0, yy = 0.0, corr, scale;
		size_t i;
		if (!f->sequence)
			continue;
		for (i = 0; i < URP_LINK_SAMPLES; ++i) {
			double x = f->link[i], y = mixed[i];
			xx += x * x;
			yy += y * y;
			xy += x * y;
		}
		if (xx < 1.0 || yy < 1.0)
			continue;
		corr = xy / sqrt(xx * yy);
		scale = xy / xx;
		if (corr > best_corr && scale > 0.25 && scale < 2.5) {
			best_corr = corr;
			best_scale = scale;
			best = n;
			found = 1;
		}
	}
	if (!found || best_corr < minimum_correlation) {
		s->misses++;
		s->last_correlation = found ? best_corr : 0.0;
		memset(matched_native, 0, URP_NATIVE_SAMPLES * sizeof(*matched_native));
		return 0;
	}
	{
		const struct urp_echo_frame *f = &s->history[best];
		size_t i;
		for (i = 0; i < URP_LINK_SAMPLES; ++i)
			mixed[i] = saturate(mixed[i] - f->link[i] * best_scale);
		for (i = 0; i < URP_NATIVE_SAMPLES; ++i)
			matched_native[i] = saturate(f->native[i] * best_scale);
		s->last_delay_frames = (int)(s->sequence - f->sequence);
	}
	s->last_scale = best_scale;
	s->last_correlation = best_corr;
	s->matches++;
	return 1;
}

/** @name File-local and build-time constants
 * @{ */
/** @def URP_CALLOC
 * @brief Allocation entry point replaceable by the failure-injection harness.
 */
/** @def URP_REALLOC
 * @brief Reallocation entry point replaceable by the failure-injection harness.
 */
/** @def URP_FREE
 * @brief Deallocation entry point replaceable by the failure-injection harness.
 */
/** @def M_PI
 * @brief Pi for platforms whose math headers omit it.
 */
/** @} */
