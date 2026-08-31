#ifndef USBRADIOPLUS_PROCESSING_H
#define USBRADIOPLUS_PROCESSING_H

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
	int lookahead_limiter_configured;
	struct txagc_config agc;
};

int usbradioplus_processing_get_local(struct txagc_chain *chain);
int usbradioplus_processing_get_composite(struct txagc_chain *chain);
static int usbradioplus_processing_load(void);
static int usbradioplus_processing_unload(void);
static int usbradioplus_processing_reload(void);

#endif
