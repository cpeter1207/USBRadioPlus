#include "asterisk.h"

#include <string.h>

#include "asterisk/config.h"
#include "asterisk/utils.h"

#include "usbradioplus_config.h"

int usbradioplus_config_variable_update(struct ast_config *config, const char *filename,
					struct ast_category *category, const char *variable,
					const char *value)
{
	struct ast_variable *current;
	struct ast_variable *match = NULL;

	/* Find the concrete variable rather than only retrieving its string value;
	 * inherited template values must be overridden in the target category. */
	for (current = ast_variable_browse(config, ast_category_get_name(category)); current;
	     current = current->next) {
		if (!strcasecmp(variable, current->name)) {
			match = current;
		}
	}

	if (match && !strcmp(match->value, value)) {
		return 0;
	}

	if (match && !match->inherited &&
	    !ast_variable_update(category, variable, value, match->value, match->object)) {
		return 0;
	}

	match = ast_variable_new(variable, value, filename);
	if (!match) {
		return -1;
	}
	ast_variable_append(category, match);
	return 0;
}
