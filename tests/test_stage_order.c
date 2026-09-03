#include "txagc/agc_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct txagc_config empty_config(void)
{
	struct txagc_config config;

	memset(&config, 0, sizeof(config));
	return config;
}

static void expect_failure(const char *order, struct txagc_config *config, const char *message)
{
	char error[128] = "unchanged";

	assert(txagc_parse_stage_order(order, config, error, sizeof(error)) == -1);
	assert(strstr(error, message) != NULL);
}

int main(void)
{
	struct txagc_config config = empty_config();
	char long_order[129];
	char untouched[] = "unchanged";

	assert(txagc_parse_stage_order(NULL, &config, NULL, 0) == -1);
	assert(txagc_parse_stage_order("agc", NULL, NULL, 0) == -1);

	memset(long_order, 'a', sizeof(long_order) - 1);
	long_order[sizeof(long_order) - 1] = '\0';
	expect_failure(long_order, &config, "too long");
	assert(txagc_parse_stage_order(long_order, &config, untouched, 0) == -1);
	assert(strcmp(untouched, "unchanged") == 0);
	assert(txagc_parse_stage_order(long_order, &config, NULL, sizeof(untouched)) == -1);

	config = empty_config();
	assert(txagc_parse_stage_order(" Equalizer, EXPANDER, agc, deesser, compressor, limiter ",
				       &config, NULL, 0) == 0);
	assert(config.stage_count == TXAGC_MAX_DYNAMICS_STAGES);
	assert(config.stage_order[0] == TXAGC_STAGE_EQUALIZER);
	assert(config.stage_order[5] == TXAGC_STAGE_LIMITER);
	config = empty_config();
	assert(txagc_parse_stage_order("\tAGC\t", &config, NULL, 0) == 0);

	config = empty_config();
	expect_failure("", &config, "unknown, empty, or fixed stage");
	expect_failure("agc,,limiter", &config, "unknown, empty, or fixed stage");
	expect_failure("fixed", &config, "unknown, empty, or fixed stage");
	assert(txagc_parse_stage_order("fixed", &config, untouched, 0) == -1);
	assert(txagc_parse_stage_order("fixed", &config, NULL, sizeof(untouched)) == -1);

	config = empty_config();
	expect_failure("agc,agc", &config, "duplicate stage 'agc'");
	assert(txagc_parse_stage_order("agc,agc", &config, untouched, 0) == -1);
	assert(txagc_parse_stage_order("agc,agc", &config, NULL, sizeof(untouched)) == -1);

#define EXPECT_MISSING(field, order, name)                                                         \
	do {                                                                                       \
		config = empty_config();                                                           \
		config.field = 1;                                                                  \
		expect_failure((order), &config, "enabled stage '" name "' is missing");           \
		assert(txagc_parse_stage_order((order), &config, untouched, 0) == -1);             \
		assert(txagc_parse_stage_order((order), &config, NULL, sizeof(untouched)) == -1);  \
	} while (0)
	EXPECT_MISSING(expander_enabled, "agc", "expander");
	EXPECT_MISSING(agc_enabled, "expander", "agc");
	EXPECT_MISSING(compressor_enabled, "agc", "compressor");
	EXPECT_MISSING(limiter_enabled, "agc", "limiter");
	EXPECT_MISSING(equalizer_enabled, "agc", "equalizer");
	EXPECT_MISSING(deesser_enabled, "agc", "deesser");
#undef EXPECT_MISSING

	config = empty_config();
	config.expander_enabled = config.agc_enabled = config.compressor_enabled = 1;
	config.limiter_enabled = config.equalizer_enabled = config.deesser_enabled = 1;
	assert(txagc_parse_stage_order("expander,agc,compressor,limiter,equalizer,deesser", &config,
				       NULL, 0) == 0);

	puts("stage-order parser tests passed");
	return 0;
}
