#ifndef USBRADIOPLUS_PROCESSING_H
#define USBRADIOPLUS_PROCESSING_H

#include <stddef.h>

#include "./txagc/agc_core.h"

struct txagc_avfilter;
enum txagc_source {
	TXAGC_LOCAL,
	TXAGC_LINK,
	TXAGC_VOICE_TELEMETRY,
	TXAGC_SOURCE_COUNT,
};

struct txagc_chain {
	int enabled;
	int rnnoise_enabled;
	int input_gain_configured;
	int ctcss_filter_configured;
	int splatter_filter_configured;
	int lookahead_limiter_configured;
	struct txagc_config agc;
};

enum usbradioplus_hardware_assignment {
	USBRADIOPLUS_HW_OFF = 0,
	USBRADIOPLUS_HW_VOICE = 1,
	USBRADIOPLUS_HW_CTCSS = 2,
	USBRADIOPLUS_HW_VOICE_CTCSS = 3,
	USBRADIOPLUS_HW_AUX_VOICE = 4,
};

struct usbradioplus_hardware_settings {
	double input_gain_db;
	double output_a_gain_db;
	double output_b_gain_db;
	int input_gain_configured;
	int output_a_gain_configured;
	int output_b_gain_configured;
	int output_a_assignment;
	int output_b_assignment;
	int output_a_assignment_configured;
	int output_b_assignment_configured;
	char cos_assignment[16];
	char rx_ctcss_frequencies[512];
	char tx_ctcss_frequencies[512];
	int cos_assignment_configured;
	int rx_ctcss_frequencies_configured;
	int tx_ctcss_frequencies_configured;
};

int usbradioplus_processing_get_local(struct txagc_chain *chain);
int usbradioplus_processing_get_composite(struct txagc_chain *chain);
int usbradioplus_processing_get_hardware(struct usbradioplus_hardware_settings *hardware);
int usbradioplus_processing_get_option(const char *section, const char *name, char *value,
				       size_t value_size);
int usbradioplus_processing_set_local_input_gain(double gain_db);
int usbradioplus_processing_set_hardware_input_gain(double gain_db);
int usbradioplus_processing_save_input_gains(double hardware_gain_db, double local_gain_db);
int usbradioplus_processing_load(void);
int usbradioplus_processing_prime(void);
int usbradioplus_processing_unload(void);
int usbradioplus_processing_reload(void);

#endif
