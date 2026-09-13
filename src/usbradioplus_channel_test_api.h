/** @file
 * @brief Test entry points for channel adapters without attached radio hardware.
 */

#ifndef USBRADIOPLUS_CHANNEL_TEST_API_H
#define USBRADIOPLUS_CHANNEL_TEST_API_H

#include "usbradioplus_channel_private.h"
#include "usbradioplus_channel_common.h"
#include "usbradioplus_cm119_gpio_poc_worker.h"

/** @brief Display combined native statistics through test linkage.
 * @param fd Asterisk CLI output descriptor.
 * @param channel Radio state whose native statistics are displayed.
 */
void radioplus_native_stats_combined_poc(int fd, struct chan_usbradio_pvt *channel);

/** @brief Test linkage for the unchanged production-local hidthread_parallel_ptt_mask helper. */
uint8_t hidthread_parallel_ptt_mask(const struct chan_usbradio_pvt *o);
/** @brief Test linkage for the unchanged production-local hidthread_close_pttkick helper. */
void hidthread_close_pttkick(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the unchanged production-local hidthread_open_pttkick helper. */
int hidthread_open_pttkick(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local hidthread_prepare_radio helper.
 * @param o Radio state to prepare.
 * @return Zero on success or a negative status on failure.
 */
int hidthread_prepare_radio(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local hidthread_start_audio helper.
 * @param o Prepared radio state.
 * @return Zero on success or a negative status on failure.
 */
int hidthread_start_audio(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_parallel_requested helper.
 * @param o Radio configuration to inspect.
 * @return Nonzero when parallel GPIO is configured.
 */
int cm119_gpio_poc_parallel_requested(const struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_validate helper.
 * @param o Radio configuration to validate.
 * @return Zero when valid or a negative status on failure.
 */
int cm119_gpio_poc_validate(const struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_prepare_hardware_adapter helper.
 * @param o Radio state receiving the prepared hardware adapter.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_prepare_hardware_adapter(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_discard_hardware_adapter helper.
 * @param o Radio state whose prepared adapter is discarded.
 */
void cm119_gpio_poc_discard_hardware_adapter(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_apply_mixer helper.
 * @param o Radio state with validated mixer settings.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_apply_mixer(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_set_rx_mixer helper.
 * @param o Radio state with an opened mixer.
 * @param value Validated capture mixer level.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_set_rx_mixer(struct chan_usbradio_pvt *o, int value);
/** @brief Test linkage for the production-local cm119_gpio_poc_open_mixer helper.
 * @param o Radio state receiving mixer handles.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_open_mixer(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_reserve_device_identity helper.
 * @param o Radio state requesting a unique hardware identity.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_reserve_device_identity(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_release_device_identity helper.
 * @param o Radio state releasing its hardware identity.
 */
void cm119_gpio_poc_release_device_identity(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_drain_pttkick helper.
 * @param o Radio state containing the wake pipe.
 * @return Zero when drained or a negative status on failure.
 */
int cm119_gpio_poc_drain_pttkick(const struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_monotonic_milliseconds helper.
 * @return Monotonic milliseconds, or zero when the clock cannot be read.
 */
uint64_t cm119_gpio_poc_monotonic_milliseconds(void);
/** @brief Test linkage for the production-local cm119_gpio_poc_service_eeprom helper.
 * @param o Radio state containing the EEPROM request.
 */
void cm119_gpio_poc_service_eeprom(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_parallel_input_event helper.
 * @param opaque Radio state supplied to the callback.
 * @param pin Parallel input pin number.
 * @param value Observed logical input level.
 */
void cm119_gpio_poc_parallel_input_event(void *opaque, unsigned int pin, int value);
/** @brief Test linkage for the production-local cm119_gpio_poc_service_parallel helper.
 * @param o Radio state owning the parallel adapter.
 * @param hardware_inputs Input mask updated with observed parallel inputs.
 * @param force_unkey Nonzero to force transmitter outputs inactive.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_service_parallel(struct chan_usbradio_pvt *o, unsigned int *hardware_inputs,
				    int force_unkey);
/** @brief Test linkage for the production-local cm119_gpio_poc_service helper.
 * @param o Radio state whose hardware is serviced.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_service(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_stop helper.
 * @param o Radio state whose hardware is stopped.
 */
void cm119_gpio_poc_stop(struct chan_usbradio_pvt *o);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_stop_requested helper.
 * @param opaque Radio state supplied to the worker.
 * @return Nonzero when shutdown is requested.
 */
int cm119_gpio_poc_worker_stop_requested(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_online helper.
 * @param opaque Radio state supplied to the worker.
 * @return Nonzero when the hardware is online.
 */
int cm119_gpio_poc_worker_online(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_clear_published_state helper.
 * @param opaque Radio state whose public hardware status is cleared.
 */
void cm119_gpio_poc_worker_clear_published_state(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_validate helper.
 * @param opaque Radio state supplied to the worker.
 * @return Zero when valid or a negative status on failure.
 */
int cm119_gpio_poc_worker_validate(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_start_attempt helper.
 * @param opaque Radio state supplied to the worker.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_worker_start_attempt(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_service helper.
 * @param opaque Radio state supplied to the worker.
 * @return Zero on success or a negative status on failure.
 */
int cm119_gpio_poc_worker_service(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_mark_online helper.
 * @param opaque Radio state whose hardware is marked online.
 */
void cm119_gpio_poc_worker_mark_online(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_wake_read_fd helper.
 * @param opaque Radio state containing the wake pipe.
 * @return Wake descriptor, or a negative value when unavailable.
 */
int cm119_gpio_poc_worker_wake_read_fd(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_drain_wake helper.
 * @param opaque Radio state containing the wake pipe.
 * @return Zero when drained or a negative status on failure.
 */
int cm119_gpio_poc_worker_drain_wake(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_stop_attempt helper.
 * @param opaque Radio state whose hardware attempt is stopped.
 */
void cm119_gpio_poc_worker_stop_attempt(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_release_identity helper.
 * @param opaque Radio state whose hardware identity is released.
 */
void cm119_gpio_poc_worker_release_identity(void *opaque);
/** @brief Test linkage for the production-local cm119_gpio_poc_worker_report helper.
 * @param opaque Radio state supplied to the worker.
 * @param event Worker lifecycle event to report.
 * @param detail Event-specific diagnostic status.
 */
void cm119_gpio_poc_worker_report(void *opaque, enum usbradioplus_cm119_gpio_poc_worker_event event,
				  int detail);

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
