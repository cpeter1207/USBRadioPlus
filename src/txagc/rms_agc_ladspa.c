/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 USBRadioPlus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/** @file
 * @brief Causal, activity-gated RMS gain rider hosted only within the FFmpeg graph.
 */

#include "rms_agc_ladspa.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/** @brief Number of scalar controls after the three audio ports. */
#define CONTROL_COUNT (USBRADIOPLUS_AGC_PORT_COUNT - USBRADIOPLUS_AGC_TARGET_DBFS)

/** @brief One control's finite range and fallback for disconnected or invalid input. */
struct control_spec {
	double minimum;	 /**< Smallest accepted value. */
	double maximum;	 /**< Largest accepted value. */
	double fallback; /**< Safe value when a connected control is nonfinite. */
};

/** @brief Control metadata follows the public LADSPA port order. */
static const struct control_spec specs[CONTROL_COUNT] = {
	{-40.0, -3.0, -24.0},  {10.0, 5000.0, 200.0}, {0.1, 100.0, 2.0},     {0.1, 100.0, 6.0},
	{0.0, 30.0, 6.0},      {0.0, 60.0, 6.0},      {-100.0, -3.0, -50.0}, {0.0, 12.0, 3.0},
	{0.0, 10000.0, 500.0}, {0.0, 6.0, 1.0},
};

/** @brief Persistent state contains no sample queue or detector filters. */
struct rms_agc {
	LADSPA_Data *ports[USBRADIOPLUS_AGC_PORT_COUNT]; /**< Host-owned connected buffers. */
	double controls[CONTROL_COUNT]; /**< Sanitized controls cached between host calls. */
	double rate;			/**< Samples per second. */
	double fast_coefficient;  /**< Ten-millisecond activity-power smoothing coefficient. */
	double level_coefficient; /**< Configured level-power smoothing coefficient. */
	double fast_power;    /**< Activity detector's continuously updated mean-square level. */
	double level_power;   /**< Target detector's continuously updated mean-square level. */
	double level_weight;  /**< EMA normalization removes silent history on activity acquisition.
			       */
	double opening_power; /**< Activity opening threshold in squared normalized samples. */
	double closing_power; /**< Activity closing threshold in squared normalized samples. */
	double gain;	      /**< Gain applied to the current sample. */
	double destination_gain; /**< Gain at the end of the current one-millisecond ramp. */
	double gain_step;	 /**< Linear-amplitude change per sample during that ramp. */
	double gain_db;		 /**< Gain requested at the last control update. */
	double hold_samples; /**< Qualified active samples continuously requesting increased gain.
			      */
	double acquisition_samples; /**< Remaining samples protected against stale low-level
				       history. */
	double active_samples;	    /**< Active samples since the last control calculation. */
	unsigned long phase;	    /**< Fractional one-kilohertz control clock, in phase units. */
	unsigned long elapsed;	    /**< Samples since the last control calculation. */
	unsigned long ramp_remaining; /**< Samples until the ramp destination is reached. */
	int active;		      /**< Hysteretic activity state. */
	int configured;		      /**< Cached coefficients have been initialized. */
};

/** @brief Restrict a finite scalar to a closed interval.
 * @param value Scalar to restrict.
 * @param minimum Lower boundary.
 * @param maximum Upper boundary.
 * @return Restricted scalar.
 */
static double bounded(double value, double minimum, double maximum)
{
	return fmin(maximum, fmax(minimum, value));
}

/** @brief Read one control using the same defaults with or without host connection.
 * @param state Plugin instance.
 * @param index Control index after the audio ports.
 * @return Finite, range-checked value.
 */
static double read_control(const struct rms_agc *state, unsigned int index)
{
	const LADSPA_Data *port = state->ports[index + USBRADIOPLUS_AGC_TARGET_DBFS];
	double value = port ? (double)*port : specs[index].fallback;
	if (!isfinite(value))
		value = specs[index].fallback;
	return bounded(value, specs[index].minimum, specs[index].maximum);
}

/** @brief Cache rate-dependent coefficients only when controls change.
 * @param state Plugin instance.
 */
static void configure(struct rms_agc *state)
{
	double values[CONTROL_COUNT];
	int changed = !state->configured;
	for (unsigned int index = 0; index < CONTROL_COUNT; ++index)
		values[index] = read_control(state, index);
	/* An invalid threshold must not leave the activity detector permanently shut. */
	if (values[6] >= values[0])
		values[6] = specs[6].fallback;
	for (unsigned int index = 0; index < CONTROL_COUNT; ++index)
		changed |= values[index] != state->controls[index];
	if (!changed)
		return;
	memcpy(state->controls, values, sizeof(values));
	state->level_coefficient = -expm1(-1000.0 / (state->rate * values[1]));
	state->opening_power = pow(10.0, values[6] / 10.0);
	state->closing_power = pow(10.0, (values[6] - values[7]) / 10.0);
	state->gain_db = bounded(20.0 * log10(state->gain), -values[5], values[4]);
	state->gain = pow(10.0, state->gain_db / 20.0);
	state->destination_gain = state->gain;
	state->ramp_remaining = 0;
	state->hold_samples = 0.0;
	state->configured = 1;
}

/** @brief Update the gain destination at a sample-derived one-kilohertz cadence.
 * @param state Plugin instance.
 */
static void update_gain(struct rms_agc *state)
{
	const double *control = state->controls;
	double level = state->level_power / state->level_weight;
	double wanted;
	double difference;
	double change;
	if (!state->active) {
		state->hold_samples = 0.0;
		return;
	}
	/* On acquisition, the fast estimate prevents stale silence from commanding boost. */
	if (state->acquisition_samples > 0.0)
		level = fmax(level, state->fast_power);
	wanted = control[0] - 10.0 * log10(fmax(level, DBL_MIN));
	difference = wanted - state->gain_db;
	/* Deadband describes distance from the RMS target, not distance from a gain cap. */
	wanted = bounded(wanted, -control[5], control[4]);
	change = 0.0;
	if (difference < -control[9]) {
		state->hold_samples = 0.0;
		change = fmax(wanted - state->gain_db,
			      -control[3] * (double)state->elapsed / state->rate);
	} else if (difference > control[9]) {
		state->hold_samples = fmin(state->hold_samples + state->active_samples,
					   control[8] * state->rate / 1000.0);
		if (state->hold_samples >= control[8] * state->rate / 1000.0)
			change = fmin(wanted - state->gain_db,
				      control[2] * (double)state->elapsed / state->rate);
	} else {
		state->hold_samples = 0.0;
	}
	state->gain_db += change;
	state->destination_gain = pow(10.0, state->gain_db / 20.0);
	/* The following control interval may differ by one sample at unusual rates. */
	state->ramp_remaining = (unsigned long)ceil((state->rate - (double)state->phase) / 1000.0);
	state->gain_step = (state->destination_gain - state->gain) / (double)state->ramp_remaining;
}

/** @brief Reset streaming state without disconnecting any host ports.
 * @param handle Plugin instance.
 */
static void activate(LADSPA_Handle handle)
{
	struct rms_agc *state = handle;
	state->fast_power = 0.0;
	state->level_power = 0.0;
	state->level_weight = 0.0;
	state->gain = 1.0;
	state->destination_gain = 1.0;
	state->gain_db = 0.0;
	state->hold_samples = 0.0;
	state->acquisition_samples = 0.0;
	state->active_samples = 0.0;
	state->phase = 0;
	state->elapsed = 0;
	state->ramp_remaining = 0;
	state->active = 0;
	state->configured = 0;
}

/** @brief Allocate all state before real-time processing starts.
 * @param descriptor Host-provided descriptor; no descriptor-specific allocation is needed.
 * @param sample_rate Samples per second, from 1000 through 384000.
 * @return Allocated instance, or NULL for an invalid rate or allocation failure.
 */
static LADSPA_Handle instantiate(const LADSPA_Descriptor *descriptor, unsigned long sample_rate)
{
	struct rms_agc *state;
	(void)descriptor;
	if (sample_rate < 1000 || sample_rate > 384000)
		return NULL;
	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;
	state->rate = (double)sample_rate;
	state->fast_coefficient = -expm1(-100.0 / state->rate);
	activate(state);
	return state;
}

/** @brief Attach host-owned buffers; reconnecting ports does not reset the leveler.
 * @param handle Plugin instance.
 * @param port Public audio or control port index.
 * @param data Buffer or control value; NULL restores a control's default.
 */
static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data)
{
	if (port < USBRADIOPLUS_AGC_PORT_COUNT) {
		struct rms_agc *state = handle;
		state->ports[port] = data;
	}
}

/** @brief Process samples in place or out of place without buffering the program audio.
 * @param handle Plugin instance.
 * @param count Number of connected audio samples.
 */
static void run(LADSPA_Handle handle, unsigned long count)
{
	struct rms_agc *state = handle;
	const float *input = state->ports[USBRADIOPLUS_AGC_INPUT];
	const float *detector = state->ports[USBRADIOPLUS_AGC_DETECTOR];
	float *output = state->ports[USBRADIOPLUS_AGC_OUTPUT];
	if (!input || !detector || !output)
		return;
	configure(state);
	for (unsigned long index = 0; index < count; ++index) {
		double sample = isfinite(detector[index]) ? (double)detector[index] : 0.0;
		double power = sample * sample;
		double result;
		/* A floor far below every activity threshold prevents denormal CPU stalls in
		 * silence. */
		state->fast_power =
			fmax(1e-30, state->fast_power +
					    state->fast_coefficient * (power - state->fast_power));
		state->level_power =
			fmax(1e-30, state->level_power + state->level_coefficient *
								 (power - state->level_power));
		state->level_weight += state->level_coefficient * (1.0 - state->level_weight);
		if (state->active) {
			if (state->fast_power < state->closing_power) {
				state->active = 0;
				state->hold_samples = 0.0;
				state->active_samples = 0.0;
				state->ramp_remaining = 0;
				state->gain_db = 20.0 * log10(state->gain);
			}
		} else if (state->fast_power >= state->opening_power) {
			state->active = 1;
			/* Normalize the new activity's EMA instead of treating earlier silence as
			 * speech. */
			state->level_power = state->level_coefficient * power;
			state->level_weight = state->level_coefficient;
			state->acquisition_samples = state->controls[1] * state->rate / 1000.0;
		}
		if (state->active)
			state->active_samples += 1.0;
		state->acquisition_samples = fmax(0.0, state->acquisition_samples - 1.0);
		state->phase += 1000;
		state->elapsed++;
		if ((double)state->phase >= state->rate) {
			state->phase -= (unsigned long)state->rate;
			update_gain(state);
			state->elapsed = 0;
			state->active_samples = 0.0;
		}
		if (state->ramp_remaining) {
			state->gain += state->gain_step;
			if (!--state->ramp_remaining)
				state->gain = state->destination_gain;
		}
		/* Corrupt input is silenced, not allowed to poison detector or gain state. */
		result = (double)input[index] * state->gain;
		output[index] = isfinite(result) && fabs(result) <= FLT_MAX ? (float)result : 0.0F;
	}
}

/** @brief Release the instance after processing has stopped.
 * @param handle Instance returned by instantiate.
 */
static void cleanup(LADSPA_Handle handle)
{
	free(handle);
}

/** @brief Audio/control direction and type for every public port. */
static const LADSPA_PortDescriptor port_descriptors[USBRADIOPLUS_AGC_PORT_COUNT] = {
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,	 LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,	 LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
};

/** @brief Human-readable port names for plugin hosts and troubleshooting. */
static const char *const port_names[USBRADIOPLUS_AGC_PORT_COUNT] = {
	"Program input",
	"Detector input",
	"Output",
	"Target RMS (dBFS)",
	"Averaging (ms)",
	"Gain increase (dB/s)",
	"Gain decrease (dB/s)",
	"Maximum boost (dB)",
	"Maximum attenuation (dB)",
	"Activity threshold (dBFS)",
	"Activity hysteresis (dB)",
	"Gain-increase hold (ms)",
	"Deadband (dB)",
};

/** @brief Advertise finite ranges; the surrounding graph always supplies explicit defaults. */
static const LADSPA_PortRangeHint port_hints[USBRADIOPLUS_AGC_PORT_COUNT] = {
	{0, 0, 0},
	{0, 0, 0},
	{0, 0, 0},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, -40, -3},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 10, 5000},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0.1F, 100},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0.1F, 100},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0, 30},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0, 60},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, -100, -3},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0, 12},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0, 10000},
	{LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0, 6},
};

/** @brief Sole plugin exported by this shared object; no alternate processing path exists. */
static const LADSPA_Descriptor descriptor = {
	.UniqueID = 524950,
	.Label = "usbradioplus_agc",
	.Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE,
	.Name = "USBRadioPlus gated RMS gain rider",
	.Maker = "USBRadioPlus contributors",
	.Copyright = "MIT",
	.PortCount = USBRADIOPLUS_AGC_PORT_COUNT,
	.PortDescriptors = port_descriptors,
	.PortNames = port_names,
	.PortRangeHints = port_hints,
	.instantiate = instantiate,
	.connect_port = connect_port,
	.activate = activate,
	.run = run,
	.cleanup = cleanup,
};

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
	return index == 0 ? &descriptor : NULL;
}
