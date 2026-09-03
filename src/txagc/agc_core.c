#include "agc_core.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* This file contains graph-configuration parsing only. Audio processing is
 * intentionally implemented by the shared FFmpeg graph. */
static char *trim_token(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t')
		text++;
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return text;
}

int txagc_parse_stage_order(const char *text, struct txagc_config *config, char *error,
			    size_t error_size)
{
	char copy[128], *cursor, *token;
	unsigned int seen = 0;

	if (!text || !config)
		return -1;
	if (strlen(text) >= sizeof(copy)) {
		if (error && error_size)
			snprintf(error, error_size, "stage order is too long");
		return -1;
	}
	memcpy(copy, text, strlen(text) + 1);
	cursor = copy;
	config->stage_count = 0;
	while ((token = strsep(&cursor, ","))) {
		enum txagc_stage stage;
		unsigned int bit;

		token = trim_token(token);
		if (!*token)
			goto unknown;
		if (!strcasecmp(token, "expander"))
			stage = TXAGC_STAGE_EXPANDER;
		else if (!strcasecmp(token, "agc"))
			stage = TXAGC_STAGE_AGC;
		else if (!strcasecmp(token, "compressor"))
			stage = TXAGC_STAGE_COMPRESSOR;
		else if (!strcasecmp(token, "limiter"))
			stage = TXAGC_STAGE_LIMITER;
		else if (!strcasecmp(token, "equalizer"))
			stage = TXAGC_STAGE_EQUALIZER;
		else if (!strcasecmp(token, "deesser"))
			stage = TXAGC_STAGE_DEESSER;
		else
			goto unknown;
		bit = 1U << stage;
		if (seen & bit) {
			if (error && error_size)
				snprintf(error, error_size, "duplicate stage '%s'", token);
			return -1;
		}
		seen |= bit;
		config->stage_order[config->stage_count++] = stage;
	}
#define REQUIRE_STAGE(enabled, stage, name)                                                        \
	do {                                                                                       \
		if ((enabled) && !(seen & (1U << (stage)))) {                                      \
			if (error && error_size)                                                   \
				snprintf(error, error_size, "enabled stage '%s' is missing",       \
					 (name));                                                  \
			return -1;                                                                 \
		}                                                                                  \
	} while (0)
	REQUIRE_STAGE(config->expander_enabled, TXAGC_STAGE_EXPANDER, "expander");
	REQUIRE_STAGE(config->agc_enabled, TXAGC_STAGE_AGC, "agc");
	REQUIRE_STAGE(config->compressor_enabled, TXAGC_STAGE_COMPRESSOR, "compressor");
	REQUIRE_STAGE(config->limiter_enabled, TXAGC_STAGE_LIMITER, "limiter");
	REQUIRE_STAGE(config->equalizer_enabled, TXAGC_STAGE_EQUALIZER, "equalizer");
	REQUIRE_STAGE(config->deesser_enabled, TXAGC_STAGE_DEESSER, "deesser");
#undef REQUIRE_STAGE
	return 0;
unknown:
	if (error && error_size)
		snprintf(error, error_size, "unknown, empty, or fixed stage '%s'", token);
	return -1;
}
