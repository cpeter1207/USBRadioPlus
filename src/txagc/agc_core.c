#include "agc_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TXAGC_FLOOR_DBFS (-120.0)

static char *trim_token(char *text)
{
	char *end;
	while (*text == ' ' || *text == '\t') text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
	return text;
}

int txagc_parse_stage_order(const char *text, struct txagc_config *config,
	char *error, size_t error_size)
{
	char copy[128], *cursor, *token;
	unsigned int seen = 0;

	if (!text || !config) return -1;
	if (strlen(text) >= sizeof(copy)) {
		if (error && error_size) snprintf(error, error_size,
			"stage order is too long");
		return -1;
	}
	strcpy(copy, text);
	cursor = copy;
	config->stage_count = 0;
	while ((token = strsep(&cursor, ","))) {
		enum txagc_stage stage;
		unsigned int bit;
		token = trim_token(token);
		if (!*token) goto unknown;
		if (!strcasecmp(token, "expander")) stage = TXAGC_STAGE_EXPANDER;
		else if (!strcasecmp(token, "agc")) stage = TXAGC_STAGE_AGC;
		else if (!strcasecmp(token, "compressor")) stage = TXAGC_STAGE_COMPRESSOR;
		else if (!strcasecmp(token, "limiter")) stage = TXAGC_STAGE_LIMITER;
		else goto unknown;
		bit = 1U << stage;
		if (seen & bit) {
			if (error && error_size) snprintf(error, error_size,
				"duplicate stage '%s'", token);
			return -1;
		}
		seen |= bit;
		config->stage_order[config->stage_count++] = stage;
	}
#define REQUIRE_STAGE(enabled, stage, name) do { \
	if ((enabled) && !(seen & (1U << (stage)))) { \
		if (error && error_size) snprintf(error, error_size, \
			"enabled stage '%s' is missing", (name)); \
		return -1; \
	} \
} while (0)
	REQUIRE_STAGE(config->expander_enabled, TXAGC_STAGE_EXPANDER, "expander");
	REQUIRE_STAGE(config->agc_enabled, TXAGC_STAGE_AGC, "agc");
	REQUIRE_STAGE(config->compressor_enabled, TXAGC_STAGE_COMPRESSOR, "compressor");
	REQUIRE_STAGE(config->limiter_enabled, TXAGC_STAGE_LIMITER, "limiter");
#undef REQUIRE_STAGE
	return 0;
unknown:
	if (error && error_size) snprintf(error, error_size,
		"unknown, empty, or fixed stage '%s'", token);
	return -1;
}

static double clamp_double(double value, double low, double high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static double block_dbfs(const double *samples, size_t count)
{
	double sum = 0.0;
	size_t i;

	if (!count) {
		return TXAGC_FLOOR_DBFS;
	}
	for (i = 0; i < count; ++i) {
		double sample = samples[i];
		sum += sample * sample;
	}
	if (sum <= 0.0) {
		return TXAGC_FLOOR_DBFS;
	}
	return 20.0 * log10(sqrt(sum / count) / 32768.0);
}

static double block_peak_dbfs(const double *samples, size_t count)
{
	double peak = 0.0;
	size_t i;

	for (i = 0; i < count; ++i) {
		double magnitude = fabs((double) samples[i]);
		if (magnitude > peak) {
			peak = magnitude;
		}
	}
	return peak > 0.0 ? 20.0 * log10(peak / 32768.0) : TXAGC_FLOOR_DBFS;
}

static double filtered_dbfs(double highpass_hz, double lowpass_hz,
	double *highpass_z1, double *highpass_z2, double *lowpass_z1, double *lowpass_z2,
	unsigned int *state_sample_rate, const double *samples, size_t count,
	unsigned int sample_rate, double input_gain_start, double input_gain_end)
{
	const double q = 0.7071067811865476;
	double hp_omega, hp_alpha, hp_cosine, hp_scale;
	double hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
	double lp_omega, lp_alpha, lp_cosine, lp_scale;
	double lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;
	double sum = 0.0;
	size_t i;

	if (*state_sample_rate != sample_rate) {
		*highpass_z1 = *highpass_z2 = 0.0;
		*lowpass_z1 = *lowpass_z2 = 0.0;
		*state_sample_rate = sample_rate;
	}
	hp_omega = 2.0 * M_PI * highpass_hz / sample_rate;
	hp_alpha = sin(hp_omega) / (2.0 * q);
	hp_cosine = cos(hp_omega);
	hp_scale = 1.0 / (1.0 + hp_alpha);
	hp_b0 = ((1.0 + hp_cosine) / 2.0) * hp_scale;
	hp_b1 = -(1.0 + hp_cosine) * hp_scale;
	hp_b2 = hp_b0;
	hp_a1 = (-2.0 * hp_cosine) * hp_scale;
	hp_a2 = (1.0 - hp_alpha) * hp_scale;
	lp_omega = 2.0 * M_PI * lowpass_hz / sample_rate;
	lp_alpha = sin(lp_omega) / (2.0 * q);
	lp_cosine = cos(lp_omega);
	lp_scale = 1.0 / (1.0 + lp_alpha);
	lp_b0 = ((1.0 - lp_cosine) / 2.0) * lp_scale;
	lp_b1 = (1.0 - lp_cosine) * lp_scale;
	lp_b2 = lp_b0;
	lp_a1 = (-2.0 * lp_cosine) * lp_scale;
	lp_a2 = (1.0 - lp_alpha) * lp_scale;

	for (i = 0; i < count; ++i) {
		double progress = (double) (i + 1) / count;
		double input_gain = input_gain_start
			+ (input_gain_end - input_gain_start) * progress;
		double input = samples[i] * input_gain;
		double highpassed = hp_b0 * input + *highpass_z1;
		double output;

		*highpass_z1 = hp_b1 * input - hp_a1 * highpassed + *highpass_z2;
		*highpass_z2 = hp_b2 * input - hp_a2 * highpassed;
		output = lp_b0 * highpassed + *lowpass_z1;
		*lowpass_z1 = lp_b1 * highpassed - lp_a1 * output + *lowpass_z2;
		*lowpass_z2 = lp_b2 * highpassed - lp_a2 * output;
		sum += output * output;
	}
	if (sum <= 0.0) {
		return TXAGC_FLOOR_DBFS;
	}
	return 20.0 * log10(sqrt(sum / count) / 32768.0);
}

static double low_limiter_reduction_db(const struct txagc_config *cfg,
	double envelope_dbfs)
{
	double over = envelope_dbfs - cfg->low_limiter_threshold_dbfs;
	double slope = (1.0 / cfg->low_limiter_ratio) - 1.0;
	double half_knee = cfg->low_limiter_knee_db / 2.0;

	if (cfg->low_limiter_knee_db <= 0.0) {
		return over > 0.0 ? slope * over : 0.0;
	}
	if (over <= -half_knee) {
		return 0.0;
	}
	if (over >= half_knee) {
		return slope * over;
	}
	return slope * (over + half_knee) * (over + half_knee)
		/ (2.0 * cfg->low_limiter_knee_db);
}

static double high_limiter_reduction_db(const struct txagc_config *cfg,
	double envelope_dbfs)
{
	double over = envelope_dbfs - cfg->high_clip_dbfs;
	double slope = (1.0 / cfg->high_limiter_ratio) - 1.0;
	double half_knee = cfg->high_limiter_knee_db / 2.0;

	if (cfg->high_limiter_knee_db <= 0.0) {
		return over > 0.0 ? slope * over : 0.0;
	}
	if (over <= -half_knee) return 0.0;
	if (over >= half_knee) return slope * over;
	return slope * (over + half_knee) * (over + half_knee)
		/ (2.0 * cfg->high_limiter_knee_db);
}

void txagc_core_init(struct txagc_core *state)
{
	memset(state, 0, sizeof(*state));
	state->gain = 1.0;
	state->input_dbfs = TXAGC_FLOOR_DBFS;
	state->sidechain_dbfs = TXAGC_FLOOR_DBFS;
	state->expander_sidechain_dbfs = TXAGC_FLOOR_DBFS;
	state->expander_gain = 1.0;
	state->compressor_sidechain_dbfs = TXAGC_FLOOR_DBFS;
	state->compressor_gain = 1.0;
	state->output_dbfs = TXAGC_FLOOR_DBFS;
	state->output_peak_dbfs = TXAGC_FLOOR_DBFS;
	state->max_output_peak_dbfs = TXAGC_FLOOR_DBFS;
	state->low_limiter_reduction_db = 0.0;
	state->high_limiter_gain = 1.0;
	state->high_limiter_envelope = 0.0;
	state->high_limiter_reduction_db = 0.0;
	state->lookahead_gain = 1.0;
}

void txagc_core_stream_reset(struct txagc_core *state)
{
	if (!state) {
		return;
	}
	/* Preserve cumulative measurements, but discard every piece of DSP
	 * history that could leak across a closed-squelch interval. */
	state->gain = 1.0;
	state->below_floor_ms = 0.0;
	state->expander_gain = 1.0;
	state->expander_reduction_db = 0.0;
	state->compressor_gain = 1.0;
	state->compressor_reduction_db = 0.0;
	state->low_limiter_envelope = 0.0;
	state->low_limiter_reduction_db = 0.0;
	state->high_limiter_gain = 1.0;
	state->high_limiter_envelope = 0.0;
	state->high_limiter_reduction_db = 0.0;
	state->lookahead_gain = 1.0;
	state->lookahead_reduction_db = 0.0;
	memset(state->lookahead_buffer, 0, sizeof(state->lookahead_buffer));
	state->lookahead_write = 0;
	state->lookahead_count = 0;
	state->crossover_low = 0.0;
	state->detector_crossover_low = 0.0;
	state->output_lowpass_z1 = state->output_lowpass_z2 = 0.0;
	state->sidechain_z1 = state->sidechain_z2 = 0.0;
	state->sidechain_highpass_z1 = state->sidechain_highpass_z2 = 0.0;
	state->expander_sidechain_z1 = state->expander_sidechain_z2 = 0.0;
	state->expander_sidechain_highpass_z1 =
		state->expander_sidechain_highpass_z2 = 0.0;
	state->compressor_sidechain_z1 = state->compressor_sidechain_z2 = 0.0;
	state->compressor_sidechain_highpass_z1 =
		state->compressor_sidechain_highpass_z2 = 0.0;
}

void txagc_core_process_double(struct txagc_core *state,
	const struct txagc_config *cfg, double *samples, size_t count,
	unsigned int sample_rate)
{
	double frame_ms;
	double desired_gain;
	double min_gain;
	double max_gain;
	double time_constant;
	double coefficient;
	double input_dbfs;
	double control_dbfs;
	double expander_control_dbfs;
	double compressor_control_dbfs;
	double desired_expander_gain;
	double expander_coefficient;
	double desired_compressor_gain;
	double compressor_coefficient;
	double compressor_makeup_gain;
	double limiter_attack;
	double limiter_release;
	double high_limiter_attack;
	double high_limiter_release;
	double crossover_coefficient;
	double output_b0;
	double output_b1;
	double output_b2;
	double output_a1;
	double output_a2;
	double applied_gain;
	double applied_gain_start;
	double expander_gain_start;
	double compressor_gain_start;
	double output_gain;
	double lookahead_attack;
	double lookahead_release;
	size_t lookahead_delay;
	size_t i;

	if (!state || !cfg || !samples || !count || !sample_rate) {
		return;
	}
	{
		double input_gain = pow(10.0, cfg->input_gain_db / 20.0);
		for (i = 0; i < count; ++i) {
			samples[i] *= input_gain;
		}
	}

	frame_ms = (1000.0 * count) / sample_rate;
	input_dbfs = block_dbfs(samples, count);
	control_dbfs = filtered_dbfs(cfg->sidechain_highpass_hz, cfg->sidechain_lowpass_hz,
		&state->sidechain_highpass_z1, &state->sidechain_highpass_z2,
		&state->sidechain_z1, &state->sidechain_z2,
		&state->sidechain_sample_rate, samples, count, sample_rate, 1.0, 1.0);
	expander_control_dbfs = filtered_dbfs(cfg->expander_sidechain_highpass_hz,
		cfg->expander_sidechain_lowpass_hz,
		&state->expander_sidechain_highpass_z1, &state->expander_sidechain_highpass_z2,
		&state->expander_sidechain_z1, &state->expander_sidechain_z2,
		&state->expander_sidechain_sample_rate, samples, count, sample_rate, 1.0, 1.0);
	state->input_dbfs = input_dbfs;
	state->sidechain_dbfs = control_dbfs;
	state->expander_sidechain_dbfs = expander_control_dbfs;
	state->frames++;
	state->samples += count;

	applied_gain_start = cfg->agc_enabled && control_dbfs >= cfg->agc_floor_dbfs
		? state->gain : 1.0;
	if (control_dbfs < cfg->agc_floor_dbfs) {
		state->below_floor_frames++;
		state->below_floor_ms += frame_ms;
		if (cfg->agc_enabled && state->below_floor_ms >= cfg->reset_after_ms) {
			state->gain = 1.0;
		}
	} else {
		state->below_floor_ms = 0.0;
		if (cfg->agc_enabled) {
			min_gain = pow(10.0, -cfg->max_attenuation_db / 20.0);
			max_gain = pow(10.0, cfg->max_gain_db / 20.0);
			desired_gain = pow(10.0, (cfg->target_dbfs - control_dbfs) / 20.0);
			desired_gain = clamp_double(desired_gain, min_gain, max_gain);
			time_constant = desired_gain < state->gain ? cfg->attack_ms : cfg->release_ms;
			coefficient = 1.0 - exp(-frame_ms / time_constant);
			state->gain += coefficient * (desired_gain - state->gain);
			state->gain = clamp_double(state->gain, min_gain, max_gain);
		}
	}
	if (!cfg->agc_enabled) {
		state->gain = 1.0;
	}
	applied_gain = cfg->agc_enabled && control_dbfs >= cfg->agc_floor_dbfs
		? state->gain : 1.0;
	output_gain = pow(10.0, cfg->output_gain_db / 20.0);
	lookahead_delay = (cfg->agc_enabled || cfg->expander_enabled
		|| cfg->compressor_enabled || cfg->limiter_enabled
		|| cfg->lookahead_limiter_enabled)
		? (size_t) lround(sample_rate * cfg->lookahead_ms / 1000.0) : 0;
	if (lookahead_delay + 1 > TXAGC_MAX_LOOKAHEAD_SAMPLES) {
		lookahead_delay = TXAGC_MAX_LOOKAHEAD_SAMPLES - 1;
	}
	lookahead_attack = exp(-1.0 / (sample_rate * cfg->lookahead_attack_ms / 1000.0));
	lookahead_release = exp(-1.0 / (sample_rate * cfg->lookahead_release_ms / 1000.0));
	expander_gain_start = state->expander_gain;
	if (cfg->expander_enabled) {
		double reduction_db = (expander_control_dbfs - cfg->expander_threshold_dbfs)
			* (cfg->expander_ratio - 1.0);

		reduction_db = clamp_double(reduction_db,
			-cfg->expander_max_attenuation_db, 0.0);
		desired_expander_gain = pow(10.0, reduction_db / 20.0);
		/* Open quickly for speech; close more slowly when the signal falls. */
		time_constant = desired_expander_gain > state->expander_gain
			? cfg->expander_attack_ms : cfg->expander_release_ms;
		expander_coefficient = 1.0 - exp(-frame_ms / time_constant);
		state->expander_gain += expander_coefficient
			* (desired_expander_gain - state->expander_gain);
		state->expander_reduction_db = 20.0 * log10(state->expander_gain);
		if (state->expander_gain < 0.999) {
			state->expanded_frames++;
		}
	} else {
		state->expander_gain = 1.0;
		state->expander_reduction_db = 0.0;
	}
	/* Filter the actual signal level presented to the compressor. This keeps
	 * sidechain history correct while AGC or expander gain is changing. */
	compressor_control_dbfs = filtered_dbfs(cfg->compressor_sidechain_highpass_hz,
		cfg->compressor_sidechain_lowpass_hz,
		&state->compressor_sidechain_highpass_z1, &state->compressor_sidechain_highpass_z2,
		&state->compressor_sidechain_z1, &state->compressor_sidechain_z2,
		&state->compressor_sidechain_sample_rate, samples, count, sample_rate,
		applied_gain_start * expander_gain_start,
		applied_gain * state->expander_gain);
	state->compressor_sidechain_dbfs = compressor_control_dbfs;
	compressor_gain_start = state->compressor_gain;
	if (cfg->compressor_enabled) {
		double over_db = compressor_control_dbfs - cfg->compressor_threshold_dbfs;
		double reduction_db = over_db > 0.0
			? -over_db * (1.0 - 1.0 / cfg->compressor_ratio) : 0.0;

		desired_compressor_gain = pow(10.0, reduction_db / 20.0);
		time_constant = desired_compressor_gain < state->compressor_gain
			? cfg->compressor_attack_ms : cfg->compressor_release_ms;
		compressor_coefficient = 1.0 - exp(-frame_ms / time_constant);
		state->compressor_gain += compressor_coefficient
			* (desired_compressor_gain - state->compressor_gain);
		state->compressor_reduction_db = 20.0 * log10(state->compressor_gain);
		if (state->compressor_gain < 0.999) {
			state->compressed_frames++;
		}
	} else {
		state->compressor_gain = 1.0;
		state->compressor_reduction_db = 0.0;
	}
	compressor_makeup_gain = cfg->compressor_enabled
		? pow(10.0, cfg->compressor_makeup_gain_db / 20.0) : 1.0;
	limiter_attack = exp(-1.0 / (sample_rate * cfg->low_limiter_attack_ms / 1000.0));
	limiter_release = exp(-1.0 / (sample_rate * cfg->low_limiter_release_ms / 1000.0));
	high_limiter_attack = exp(-1.0 / (sample_rate * cfg->high_limiter_attack_ms / 1000.0));
	high_limiter_release = exp(-1.0 / (sample_rate * cfg->high_limiter_release_ms / 1000.0));
	crossover_coefficient = exp(-2.0 * M_PI * cfg->limiter_crossover_hz / sample_rate);
	{
		const double q = 0.7071067811865476;
		double omega = 2.0 * M_PI * cfg->output_lowpass_hz / sample_rate;
		double alpha = sin(omega) / (2.0 * q);
		double cosine = cos(omega);
		double scale = 1.0 / (1.0 + alpha);

		output_b0 = ((1.0 - cosine) / 2.0) * scale;
		output_b1 = (1.0 - cosine) * scale;
		output_b2 = output_b0;
		output_a1 = (-2.0 * cosine) * scale;
		output_a2 = (1.0 - alpha) * scale;
	}

	if (state->lookahead_sample_rate != sample_rate
		|| state->lookahead_delay_samples != lookahead_delay) {
		memset(state->lookahead_buffer, 0, sizeof(state->lookahead_buffer));
		state->lookahead_write = 0;
		state->lookahead_count = 0;
		state->lookahead_gain = 1.0;
		state->high_limiter_gain = 1.0;
		state->lookahead_sample_rate = sample_rate;
		state->lookahead_delay_samples = lookahead_delay;
	}

	for (i = 0; i < count; ++i) {
		size_t capacity = lookahead_delay + 1;
		size_t slot = state->lookahead_write;
		double future = samples[i];
		double delayed;
		double progress = (double) (i + 1) / count;
		double sample_agc_gain = applied_gain_start
			+ (applied_gain - applied_gain_start) * progress;
		double sample_expander_gain = expander_gain_start
			+ (state->expander_gain - expander_gain_start) * progress;
		double sample_compressor_gain = compressor_gain_start
			+ (state->compressor_gain - compressor_gain_start) * progress;
		double dynamics_gain = sample_agc_gain * sample_expander_gain
			* sample_compressor_gain * compressor_makeup_gain;
		double future_adjusted = future * dynamics_gain;
		double adjusted;

		state->lookahead_buffer[slot].program = future;
		state->lookahead_buffer[slot].predicted = 0.0;
		state->lookahead_write = (state->lookahead_write + 1) % capacity;
		if (state->lookahead_count < capacity) {
			state->lookahead_count++;
		}
		delayed = state->lookahead_count < capacity
			? 0.0 : state->lookahead_buffer[state->lookahead_write].program;
		adjusted = delayed * dynamics_gain;

		if (cfg->limiter_enabled) {
			double low;
			double high;
			double detector_low;
			double detector_high;
			double level;
			double coefficient;
			double envelope_dbfs;
			double final_limit = 32768.0 * pow(10.0, cfg->final_clip_dbfs / 20.0);
			double desired_high_gain;

			/* Split the undelayed detector and delayed program independently.
			 * The future bands control the corresponding delayed bands. */
			state->detector_crossover_low = crossover_coefficient
				* state->detector_crossover_low
				+ (1.0 - crossover_coefficient) * future_adjusted;
			state->crossover_low = crossover_coefficient * state->crossover_low
				+ (1.0 - crossover_coefficient) * adjusted;
			detector_low = state->detector_crossover_low;
			detector_high = future_adjusted - detector_low;
			low = state->crossover_low;
			high = adjusted - low;
			level = fabs(detector_low) / 32768.0;
			coefficient = level > state->low_limiter_envelope
				? limiter_attack : limiter_release;
			state->low_limiter_envelope = coefficient * state->low_limiter_envelope
				+ (1.0 - coefficient) * level;
			envelope_dbfs = state->low_limiter_envelope > 0.0
				? 20.0 * log10(state->low_limiter_envelope) : TXAGC_FLOOR_DBFS;
			state->low_limiter_reduction_db = low_limiter_reduction_db(cfg, envelope_dbfs);
			if (state->low_limiter_reduction_db < 0.0) {
				low *= pow(10.0, state->low_limiter_reduction_db / 20.0);
				state->low_limited_samples++;
			}
			level = fabs(detector_high) / 32768.0;
			coefficient = level > state->high_limiter_envelope
				? high_limiter_attack : high_limiter_release;
			state->high_limiter_envelope = coefficient
				* state->high_limiter_envelope + (1.0 - coefficient) * level;
			envelope_dbfs = state->high_limiter_envelope > 0.0
				? 20.0 * log10(state->high_limiter_envelope) : TXAGC_FLOOR_DBFS;
			state->high_limiter_reduction_db = high_limiter_reduction_db(cfg,
				envelope_dbfs);
			desired_high_gain = pow(10.0,
				state->high_limiter_reduction_db / 20.0);
			coefficient = desired_high_gain < state->high_limiter_gain
				? high_limiter_attack : high_limiter_release;
			state->high_limiter_gain = coefficient * state->high_limiter_gain
				+ (1.0 - coefficient) * desired_high_gain;
			high *= state->high_limiter_gain;
			if (state->high_limiter_gain < 0.999) {
				state->high_clipped_samples++;
			}
			adjusted = low + high;
			future_adjusted = detector_low
				* pow(10.0, state->low_limiter_reduction_db / 20.0)
				+ detector_high * state->high_limiter_gain;
			if (cfg->final_clipper_enabled) {
				if (adjusted > final_limit) {
					adjusted = final_limit;
					state->final_clipped_samples++;
				} else if (adjusted < -final_limit) {
					adjusted = -final_limit;
					state->final_clipped_samples++;
				}
			}
		} else {
			state->high_limiter_gain = 1.0;
			state->high_limiter_reduction_db = 0.0;
		}
		state->lookahead_buffer[slot].predicted = future_adjusted;
		if (cfg->lookahead_limiter_enabled) {
			double desired_gain;
			double coefficient;
			double limit = 32768.0 * pow(10.0, cfg->lookahead_limit_dbfs / 20.0);
			double peak = 0.0;
			size_t j;

			/* Scan the future predictions in the same ring that delays the
			 * program. A peak therefore drives the gain envelope for the full
			 * lookahead interval before that sample reaches the output. */
			for (j = 0; j < state->lookahead_count; ++j) {
				double magnitude = fabs(state->lookahead_buffer[j].predicted);
				if (magnitude > peak) {
					peak = magnitude;
				}
			}
			desired_gain = peak > limit ? limit / peak : 1.0;
			coefficient = desired_gain < state->lookahead_gain
				? lookahead_attack : lookahead_release;
			state->lookahead_gain = coefficient * state->lookahead_gain
				+ (1.0 - coefficient) * desired_gain;
			state->lookahead_reduction_db = 20.0 * log10(state->lookahead_gain);
			adjusted *= state->lookahead_gain;
			if (state->lookahead_gain < 0.999) {
				state->lookahead_limited_samples++;
			}
		} else {
			state->lookahead_gain = 1.0;
			state->lookahead_reduction_db = 0.0;
		}
		if (cfg->splatter_filter_enabled) {
			double filtered = output_b0 * adjusted + state->output_lowpass_z1;
			state->output_lowpass_z1 = output_b1 * adjusted
				- output_a1 * filtered + state->output_lowpass_z2;
			state->output_lowpass_z2 = output_b2 * adjusted - output_a2 * filtered;
			adjusted = filtered;
		}
		adjusted *= output_gain;
		/* Keep unlimited floating-point headroom between processing stages.
		 * Boundary ceilings belong to the channel driver, not this core. */
		samples[i] = adjusted;
	}
	state->output_dbfs = block_dbfs(samples, count);
	state->output_peak_dbfs = block_peak_dbfs(samples, count);
	if (state->output_peak_dbfs > state->max_output_peak_dbfs) {
		state->max_output_peak_dbfs = state->output_peak_dbfs;
	}
}

void txagc_core_process(struct txagc_core *state, const struct txagc_config *cfg,
	int16_t *samples, size_t count, unsigned int sample_rate)
{
	double work[TXAGC_MAX_LOOKAHEAD_SAMPLES];
	size_t i;

	if (!samples || count > TXAGC_MAX_LOOKAHEAD_SAMPLES) {
		return;
	}
	for (i = 0; i < count; ++i) {
		work[i] = samples[i];
	}
	txagc_core_process_double(state, cfg, work, count, sample_rate);
	for (i = 0; i < count; ++i) {
		long value = lround(work[i]);
		if (value > INT16_MAX) {
			value = INT16_MAX;
			state->clipped_samples++;
		} else if (value < INT16_MIN) {
			value = INT16_MIN;
			state->clipped_samples++;
		}
		samples[i] = (int16_t) value;
	}
}
