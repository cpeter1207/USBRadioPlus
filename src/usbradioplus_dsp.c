#include "usbradioplus_dsp.h"

#include <limits.h>
#include <math.h>
#include <samplerate.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef AST_MODULE
#include "asterisk/utils.h"
#define URP_CALLOC(n, s) ast_calloc((n), (s))
#define URP_REALLOC(p, s) ast_realloc((p), (s))
#define URP_FREE(p) ast_free((p))
#else
#define URP_CALLOC(n, s) calloc((n), (s))
#define URP_REALLOC(p, s) realloc((p), (s))
#define URP_FREE(p) free((p))
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct urp_src {
	SRC_STATE *state;
	unsigned int channels;
	float *input;
	float *output;
	size_t input_capacity;
	size_t output_capacity;
};

void urp_clock_recovery_reset(struct urp_clock_recovery *clock)
{
	if (clock) clock->correction = 0.0;
}

double urp_clock_recovery_update(struct urp_clock_recovery *clock,
	size_t queued_samples, size_t target_samples)
{
	double error, desired;

	if (!clock || !target_samples) return 0.0;
	error = ((double) target_samples - queued_samples) / target_samples;
	/* One frame of occupancy error requests about 0.13% correction. This
	 * covers normal independent-oscillator drift while remaining inaudible. */
	desired = error * 0.004;
	if (desired > URP_CLOCK_MAX_CORRECTION) desired = URP_CLOCK_MAX_CORRECTION;
	if (desired < -URP_CLOCK_MAX_CORRECTION) desired = -URP_CLOCK_MAX_CORRECTION;
	/* Smooth ratio changes so clock correction cannot modulate speech abruptly. */
	clock->correction += (desired - clock->correction) * 0.02;
	return clock->correction;
}

static int urp_text_is_true(const char *text)
{
	return !strcasecmp(text, "yes") || !strcasecmp(text, "true")
		|| !strcasecmp(text, "on") || !strcasecmp(text, "y")
		|| !strcasecmp(text, "t");
}

static int urp_text_is_false(const char *text)
{
	return !strcasecmp(text, "no") || !strcasecmp(text, "false")
		|| !strcasecmp(text, "off") || !strcasecmp(text, "n")
		|| !strcasecmp(text, "f");
}

int urp_parse_cutoff(const char *text, double default_hz, double nyquist_hz,
	struct urp_cutoff_setting *setting)
{
	char *end;
	long selector;
	double frequency;

	if (!text || !*text || !setting || !isfinite(default_hz)
		|| default_hz <= 0.0 || default_hz >= nyquist_hz) return -1;

	/* Preserve legacy integer selectors, especially 0, before parsing booleans. */
	if (!strchr(text, '.') && !strchr(text, 'e') && !strchr(text, 'E')) {
		selector = strtol(text, &end, 10);
		if (end != text && !*end) {
			if (selector < 0 || selector > INT_MAX) return -1;
			setting->enabled = 1;
			setting->exact = 0;
			setting->selector = (int) selector;
			setting->frequency_hz = default_hz;
			return 0;
		}
	}
	if (urp_text_is_false(text)) {
		setting->enabled = 0;
		setting->exact = 1;
		setting->selector = 0;
		setting->frequency_hz = default_hz;
		return 0;
	}
	if (urp_text_is_true(text)) {
		setting->enabled = 1;
		setting->exact = 1;
		setting->selector = 0;
		setting->frequency_hz = default_hz;
		return 0;
	}
	frequency = strtod(text, &end);
	if (end == text || *end || !isfinite(frequency) || frequency <= 0.0
		|| frequency >= nyquist_hz) return -1;
	setting->enabled = 1;
	setting->exact = 1;
	setting->selector = 0;
	setting->frequency_hz = frequency;
	return 0;
}

double urp_legacy_cutoff(enum urp_legacy_filter filter, int selector)
{
	static const double values[][3] = {
		[URP_FILTER_RX_LOWPASS] = { 3000.0, 3300.0, 3700.0 },
		[URP_FILTER_RX_HIGHPASS] = { 300.0, 250.0, 0.0 },
		[URP_FILTER_TX_LOWPASS] = { 3000.0, 3300.0, 0.0 },
		[URP_FILTER_TX_HIGHPASS] = { 300.0, 250.0, 120.0 },
	};
	static const unsigned int counts[] = { 3, 2, 2, 3 };

	if (filter < URP_FILTER_RX_LOWPASS || filter > URP_FILTER_TX_HIGHPASS)
		return 0.0;
	if (selector < 0 || (unsigned int) selector >= counts[filter]) selector = 0;
	return values[filter][selector];
}

double urp_legacy_limiter_ceiling_dbfs(int setpoint)
{
	double peak;

	/* XPMR limits voice before its composite mixer, which then applies 2x
	 * voice gain.  Express the equivalent final-output ceiling in dBFS. */
	if (setpoint < 5000) setpoint = 5000;
	if (setpoint > 13000) setpoint = 13000;
	peak = fmin(32766.0, 2.0 * setpoint);
	return 20.0 * log10(peak / 32768.0);
}

static int16_t saturate(double value)
{
	if (value > 32767.0) return 32767;
	if (value < -32768.0) return -32768;
	return (int16_t) lrint(value);
}

void urp_biquad_reset(struct urp_biquad *f)
{
	if (!f) return;
	f->z1 = f->z2 = 0.0;
}

int urp_biquad_highpass(struct urp_biquad *f, unsigned int rate,
	double cutoff_hz, int enabled)
{
	double omega, alpha, cosine, scale;
	const double q = 0.7071067811865476;
	if (!f || !rate || cutoff_hz <= 0.0 || cutoff_hz >= rate * 0.49) return -1;
	omega = 2.0 * M_PI * cutoff_hz / rate;
	alpha = sin(omega) / (2.0 * q);
	cosine = cos(omega);
	scale = 1.0 / (1.0 + alpha);
	f->b0 = ((1.0 + cosine) / 2.0) * scale;
	f->b1 = -(1.0 + cosine) * scale;
	f->b2 = f->b0;
	f->a1 = (-2.0 * cosine) * scale;
	f->a2 = (1.0 - alpha) * scale;
	f->rate = rate;
	f->frequency = cutoff_hz;
	f->enabled = !!enabled;
	urp_biquad_reset(f);
	return 0;
}

void urp_biquad_process(struct urp_biquad *f, int16_t *samples, size_t count)
{
	size_t i;
	if (!f || !samples || !f->enabled) return;
	for (i = 0; i < count; ++i) {
		double out = f->b0 * samples[i] + f->z1;
		f->z1 = f->b1 * samples[i] - f->a1 * out + f->z2;
		f->z2 = f->b2 * samples[i] - f->a2 * out;
		samples[i] = saturate(out);
	}
}

void urp_biquad_process_double(struct urp_biquad *f, double *samples,
	size_t count)
{
	size_t i;
	if (!f || !samples || !f->enabled) return;
	for (i = 0; i < count; ++i) {
		double out = f->b0 * samples[i] + f->z1;
		f->z1 = f->b1 * samples[i] - f->a1 * out + f->z2;
		f->z2 = f->b2 * samples[i] - f->a2 * out;
		samples[i] = out;
	}
}

void urp_deemphasis_reset(struct urp_deemphasis *f)
{
	if (!f) return;
	f->x1 = f->y1 = 0.0;
}

int urp_deemphasis_configure(struct urp_deemphasis *f, unsigned int rate,
	double tau_us, int enabled)
{
	if (!f || !rate || tau_us <= 0.0) return -1;
	f->rate = rate;
	f->tau_us = tau_us;
	f->preemphasis = 0;
	f->enabled = !!enabled;
	urp_deemphasis_reset(f);
	return 0;
}

int urp_preemphasis_configure(struct urp_deemphasis *f, unsigned int rate,
	double tau_us, int enabled)
{
	int result = urp_deemphasis_configure(f, rate, tau_us, enabled);
	if (!result) f->preemphasis = 1;
	return result;
}

int urp_land_mobile_emphasis_configure(struct urp_deemphasis *f,
	unsigned int rate, double corner_hz, int preemphasis, int enabled)
{
	double tau_us;
	if (corner_hz <= 0.0 || corner_hz >= 300.0) return -1;
	tau_us = 1000000.0 / (2.0 * M_PI * corner_hz);
	return preemphasis
		? urp_preemphasis_configure(f, rate, tau_us, enabled)
		: urp_deemphasis_configure(f, rate, tau_us, enabled);
}

void urp_deemphasis_process(struct urp_deemphasis *f, int16_t *samples, size_t count)
{
	double dt, tau, alpha, pole, omega, reference_gain;
	size_t i;
	if (!f || !samples || !f->enabled) return;
	dt = 1.0 / f->rate;
	tau = f->tau_us / 1000000.0;
	alpha = dt / (tau + dt);
	pole = 1.0 - alpha;
	omega = 2.0 * M_PI * 1000.0 / f->rate;
	reference_gain = sqrt(1.0 + pole * pole - 2.0 * pole * cos(omega))
		/ (1.0 - pole);
	for (i = 0; i < count; ++i) {
		double x = samples[i];
		double y;
		if (f->preemphasis) {
			y = (x - pole * f->x1) / (1.0 - pole) / reference_gain;
		} else {
			y = (f->y1 + alpha * (x - f->y1)) * reference_gain;
			f->y1 = y / reference_gain;
		}
		f->x1 = x;
		samples[i] = saturate(y);
	}
}

void urp_deemphasis_process_double(struct urp_deemphasis *f,
	double *samples, size_t count)
{
	double dt, tau, alpha, pole, omega, reference_gain;
	size_t i;
	if (!f || !samples || !f->enabled) return;
	dt = 1.0 / f->rate;
	tau = f->tau_us / 1000000.0;
	alpha = dt / (tau + dt);
	pole = 1.0 - alpha;
	/* Normalize both members of the reciprocal pair to 0 dB at 1 kHz,
	 * the reference frequency required by TIA-603. */
	omega = 2.0 * M_PI * 1000.0 / f->rate;
	reference_gain = sqrt(1.0 + pole * pole - 2.0 * pole * cos(omega))
		/ (1.0 - pole);
	for (i = 0; i < count; ++i) {
		double x = samples[i];
		double y;
		if (f->preemphasis) {
			y = (x - pole * f->x1) / (1.0 - pole) / reference_gain;
		} else {
			y = (f->y1 + alpha * (x - f->y1)) * reference_gain;
			/* Store the unnormalized low-pass state. */
			f->y1 = y / reference_gain;
		}
		f->x1 = x;
		samples[i] = y;
	}
}

struct urp_src *urp_src_create(int converter, unsigned int channels)
{
	struct urp_src *src;
	int error = 0;
	if (!channels) return NULL;
	src = URP_CALLOC(1, sizeof(*src));
	if (!src) return NULL;
	src->state = src_new(converter, channels, &error);
	if (!src->state) { URP_FREE(src); return NULL; }
	src->channels = channels;
	return src;
}

void urp_src_destroy(struct urp_src *src)
{
	if (!src) return;
	if (src->state) src_delete(src->state);
	URP_FREE(src->input);
	URP_FREE(src->output);
	URP_FREE(src);
}

void urp_src_reset(struct urp_src *src)
{
	if (src && src->state) src_reset(src->state);
}

int urp_src_process(struct urp_src *src, const int16_t *input, size_t input_count,
	int16_t *output, size_t output_capacity, double ratio, size_t *input_used,
	size_t *output_generated)
{
	SRC_DATA data;
	size_t i, in_values, out_values;
	int result;
	if (!src || !input || !output || ratio <= 0.0) return -1;
	in_values = input_count * src->channels;
	out_values = output_capacity * src->channels;
	if (in_values > src->input_capacity) {
		float *p = URP_REALLOC(src->input, in_values * sizeof(*p));
		if (!p) return -1;
		src->input = p; src->input_capacity = in_values;
	}
	if (out_values > src->output_capacity) {
		float *p = URP_REALLOC(src->output, out_values * sizeof(*p));
		if (!p) return -1;
		src->output = p; src->output_capacity = out_values;
	}
	src_short_to_float_array(input, src->input, (int) in_values);
	memset(&data, 0, sizeof(data));
	data.data_in = src->input;
	data.data_out = src->output;
	data.input_frames = (long) input_count;
	data.output_frames = (long) output_capacity;
	data.src_ratio = ratio;
	result = src_process(src->state, &data);
	if (result) return result;
	src_float_to_short_array(src->output, output,
		(int) (data.output_frames_gen * src->channels));
	if (input_used) *input_used = (size_t) data.input_frames_used;
	if (output_generated) *output_generated = (size_t) data.output_frames_gen;
	for (i = data.output_frames_gen * src->channels; i < out_values; ++i) output[i] = 0;
	return 0;
}

int urp_rate_convert(struct urp_src *src, const int16_t *input,
	size_t input_count, unsigned int input_rate, int16_t *output,
	size_t output_capacity, unsigned int output_rate, size_t *input_used,
	size_t *output_generated)
{
	size_t copied;
	if (!input || !output || !input_rate || !output_rate) return -1;
	if (input_rate != output_rate) {
		return urp_src_process(src, input, input_count, output, output_capacity,
			(double) output_rate / input_rate, input_used, output_generated);
	}
	/* Matching rates need no converter state, allocation, or filter delay. */
	copied = input_count < output_capacity ? input_count : output_capacity;
	memmove(output, input, copied * sizeof(*output));
	if (copied < output_capacity)
		memset(output + copied, 0, (output_capacity - copied) * sizeof(*output));
	if (input_used) *input_used = copied;
	if (output_generated) *output_generated = copied;
	return copied == input_count ? 0 : -1;
}

void urp_extract_mono(const int16_t *stereo, int16_t *mono, size_t frames,
	unsigned int channel)
{
	size_t i;
	channel = channel ? 1 : 0;
	for (i = 0; i < frames; ++i) mono[i] = stereo[i * 2 + channel];
}

void urp_duplicate_mono(const int16_t *mono, int16_t *stereo, size_t frames,
	double gain_a, double gain_b)
{
	size_t i;
	for (i = 0; i < frames; ++i) {
		stereo[i * 2] = saturate(mono[i] * gain_a);
		stereo[i * 2 + 1] = saturate(mono[i] * gain_b);
	}
}

void urp_echo_init(struct urp_echo_replacer *s)
{
	if (!s) return;
	memset(s, 0, sizeof(*s));
	s->last_delay_frames = -1;
}

void urp_echo_push(struct urp_echo_replacer *s, const int16_t *link,
	const int16_t *native)
{
	struct urp_echo_frame *f;
	if (!s || !link || !native) return;
	f = &s->history[s->write_index];
	memcpy(f->link, link, sizeof(f->link));
	memcpy(f->native, native, sizeof(f->native));
	f->sequence = ++s->sequence;
	s->write_index = (s->write_index + 1) % URP_ECHO_HISTORY_FRAMES;
}

int urp_echo_remove(struct urp_echo_replacer *s, int16_t *mixed,
	int16_t *matched_native, double minimum_correlation)
{
	double best_corr = -1.0, best_scale = 0.0;
	unsigned int best = 0, n;
	int found = 0;
	if (!s || !mixed || !matched_native) return 0;
	for (n = 0; n < URP_ECHO_HISTORY_FRAMES; ++n) {
		const struct urp_echo_frame *f = &s->history[n];
		double xy = 0.0, xx = 0.0, yy = 0.0, corr, scale;
		size_t i;
		if (!f->sequence) continue;
		for (i = 0; i < URP_LINK_SAMPLES; ++i) {
			double x = f->link[i], y = mixed[i];
			xx += x * x; yy += y * y; xy += x * y;
		}
		if (xx < 1.0 || yy < 1.0) continue;
		corr = xy / sqrt(xx * yy);
		scale = xy / xx;
		if (corr > best_corr && scale > 0.25 && scale < 2.5) {
			best_corr = corr; best_scale = scale; best = n; found = 1;
		}
	}
	if (!found || best_corr < minimum_correlation) {
		s->misses++; s->last_correlation = found ? best_corr : 0.0;
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
		s->last_delay_frames = (int) (s->sequence - f->sequence);
	}
	s->last_scale = best_scale; s->last_correlation = best_corr; s->matches++;
	return 1;
}
