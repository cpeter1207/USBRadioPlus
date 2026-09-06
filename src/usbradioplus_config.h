/** @file
 * @brief Update Asterisk configuration variables while preserving their category context.
 */

#ifndef USBRADIOPLUS_CONFIG_H
#define USBRADIOPLUS_CONFIG_H

struct ast_category;
struct ast_config;

/** @brief Update or append a category variable while preserving its Asterisk configuration context.
 * @param config Asterisk configuration tree owned by the caller.
 * @param filename Configuration filename used by Asterisk's update operation.
 * @param category Existing Asterisk category receiving the update.
 * @param variable Configuration option name.
 * @param value New textual option value.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradioplus_config_variable_update(struct ast_config *config, const char *filename,
					struct ast_category *category, const char *variable,
					const char *value);

#endif
