#ifndef USBRADIOPLUS_CONFIG_H
#define USBRADIOPLUS_CONFIG_H

struct ast_category;
struct ast_config;

/** Update an existing category variable or append it when it is absent. */
int usbradioplus_config_variable_update(struct ast_config *config, const char *filename,
					struct ast_category *category, const char *variable,
					const char *value);

#endif
