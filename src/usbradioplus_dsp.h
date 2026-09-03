#ifndef USBRADIOPLUS_DSP_H
#define USBRADIOPLUS_DSP_H

#include <stddef.h>
#include <stdint.h>

#define URP_APP_RPT_RATE_DEFAULT 8000
#define URP_RATE_LINK URP_APP_RPT_RATE_DEFAULT
#define URP_RATE_NATIVE 48000
#define URP_LINK_SAMPLES 160
#define URP_NATIVE_SAMPLES 960
#define URP_ECHO_HISTORY_FRAMES 32
#define URP_CLOCK_MAX_CORRECTION 0.005

struct urp_clock_recovery {
	double correction;
};

void urp_clock_recovery_reset(struct urp_clock_recovery *clock);
double urp_clock_recovery_update(struct urp_clock_recovery *clock, size_t queued_samples,
				 size_t target_samples);

struct urp_cutoff_setting {
	int enabled;
	int exact;
	int selector;
	double frequency_hz;
};

enum urp_legacy_filter {
	URP_FILTER_RX_LOWPASS,
	URP_FILTER_RX_HIGHPASS,
	URP_FILTER_TX_LOWPASS,
	URP_FILTER_TX_HIGHPASS,
};

int urp_parse_cutoff(const char *text, double default_hz, double nyquist_hz,
		     struct urp_cutoff_setting *setting);
double urp_legacy_cutoff(enum urp_legacy_filter filter, int selector);
double urp_legacy_limiter_ceiling_dbfs(int setpoint);

struct urp_src;

struct urp_echo_frame {
	int16_t link[URP_LINK_SAMPLES];
	int16_t native[URP_NATIVE_SAMPLES];
	uint64_t sequence;
};

struct urp_echo_replacer {
	struct urp_echo_frame history[URP_ECHO_HISTORY_FRAMES];
	unsigned int write_index;
	uint64_t sequence;
	int last_delay_frames;
	double last_scale;
	double last_correlation;
	uint64_t matches;
	uint64_t misses;
};

struct urp_src *urp_src_create(int converter, unsigned int channels);
void urp_src_destroy(struct urp_src *src);
void urp_src_reset(struct urp_src *src);
int urp_src_process(struct urp_src *src, const int16_t *input, size_t input_count, int16_t *output,
		    size_t output_capacity, double ratio, size_t *input_used,
		    size_t *output_generated);
int urp_rate_convert(struct urp_src *src, const int16_t *input, size_t input_count,
		     unsigned int input_rate, int16_t *output, size_t output_capacity,
		     unsigned int output_rate, size_t *input_used, size_t *output_generated);

void urp_extract_mono(const int16_t *stereo, int16_t *mono, size_t frames, unsigned int channel);
void urp_duplicate_mono(const int16_t *mono, int16_t *stereo, size_t frames, double gain_a,
			double gain_b);

void urp_echo_init(struct urp_echo_replacer *state);
void urp_echo_push(struct urp_echo_replacer *state, const int16_t *link, const int16_t *native);
int urp_echo_remove(struct urp_echo_replacer *state, int16_t *mixed_link, int16_t *matched_native,
		    double minimum_correlation);

#endif
