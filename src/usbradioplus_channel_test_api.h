/** @file
 * @brief Test entry points for channel adapters without attached radio hardware.
 */

#ifndef USBRADIOPLUS_CHANNEL_TEST_API_H
#define USBRADIOPLUS_CHANNEL_TEST_API_H

#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"

/** @brief Select a deterministic shared parallel-port owner for channel tests.
 * @param owner Prepared fixture owner, or null to clear it.
 */
void usbradioplus_test_set_parallel_owner(struct chan_usbradio_pvt *owner);

int usbradio_text(struct ast_channel *c, const char *text);
/** @brief Release Asterisk ownership and reset radio call state.
 * @param c Asterisk channel associated with the radio or link.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_hangup(struct ast_channel *c);
/** @brief Connect an outbound Asterisk call to the configured radio.
 * @param c Asterisk channel associated with the radio or link.
 * @param dest Asterisk dial destination; unused by this radio callback.
 * @param timeout Asterisk call timeout; unused by this radio callback.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_call(struct ast_channel *c, const char *dest, int timeout);
/** @brief Queue an outbound app_rpt voice frame for native transmitter rendering.
 * @param c Asterisk channel associated with the radio or link.
 * @param f Outbound Asterisk audio frame.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int usbradio_write(struct ast_channel *c, struct ast_frame *f);
/** @brief Show or select the radio used by interactive calibration commands.
 * @param fd Asterisk CLI output descriptor.
 * @param argc Number of CLI arguments.
 * @param argv CLI argument vector.
 * @return Asterisk tuning-command result code.
 */
int radio_active(int fd, int argc, const char *const *argv);
/** @brief Register the channel technology and start configured radio workers.
 * @return Asterisk module-load status.
 */
int load_module(void);
/** @brief Stop workers and release channel, hardware, and processing resources.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
int unload_module(void);
/** @brief Inject module metadata into the isolated channel test harness.
 * @param info Test module metadata.
 */
void usbradioplus_test_set_module_info(struct ast_module_info *info);
/** @brief Service USB GPIO, PTT, EEPROM, device recovery, and radio status.
 * @param arg Private radio state passed to the worker.
 * @return NULL when the HID worker exits.
 */
void *hidthread(void *arg);
/** @brief Return received PCM or signaling events to Asterisk.
 * @param c Asterisk channel associated with the radio or link.
 * @return Received Asterisk frame or the adapter's null/error result.
 */
struct ast_frame *usbradio_read(struct ast_channel *c);
/** @brief Create an Asterisk channel and associate it with private radio state.
 * @param o Private state of the selected radio channel.
 * @param ext Asterisk extension for the new channel.
 * @param ctx Asterisk context for the new channel.
 * @param state Initial Asterisk channel state.
 * @param assignedids Asterisk-assigned channel identifiers.
 * @param requestor Asterisk channel associated with the radio or link.
 * @return New Asterisk channel reference, or NULL on failure.
 */
struct ast_channel *usbradio_new(struct chan_usbradio_pvt *o, char *ext, char *ctx, int state,
				 const struct ast_assigned_ids *assignedids,
				 const struct ast_channel *requestor);
/** @brief Allocate an Asterisk radio channel for a configured device.
 * @param type Requested Asterisk channel technology.
 * @param cap Requested Asterisk format capabilities.
 * @param assignedids Asterisk-assigned channel identifiers.
 * @param requestor Asterisk channel associated with the radio or link.
 * @param data Configured radio name in the dial request.
 * @param cause Receives the Asterisk failure cause when channel creation fails.
 * @return New Asterisk channel reference, or NULL with a failure cause.
 */
struct ast_channel *usbradio_request(const char *type, struct ast_format_cap *cap,
				     const struct ast_assigned_ids *assignedids,
				     const struct ast_channel *requestor, const char *data,
				     int *cause);
/** @brief Translate a tuning command result into an Asterisk CLI result.
 * @param result Tuning command's RESULT_SUCCESS, RESULT_SHOWUSAGE, or failure status.
 * @return Asterisk CLI_SUCCESS, CLI_SHOWUSAGE, or CLI_FAILURE sentinel.
 */
char *res2cli(int result);
/** @brief Register, complete, or execute the radio key CLI command.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_console_key(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or execute the radio unkey CLI command.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_console_unkey(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or dispatch radio tuning CLI commands.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_radio_tune(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or select the active radio for tuning.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_radio_active(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or print active radio settings.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_show_settings(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or change the radio debug level.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_set_dsp_debug(struct ast_cli_entry *entry, int command, struct ast_cli_args *args);
/** @brief Register, complete, or report native audio and FIFO statistics.
 * @param entry CLI command registration.
 * @param command CLI initialization, completion, or execution selector.
 * @param args CLI argument and output descriptor.
 * @return Asterisk CLI result sentinel, or NULL during registration/completion.
 */
char *handle_radioplus_native_stats(struct ast_cli_entry *entry, int command,
				    struct ast_cli_args *args);
#endif
