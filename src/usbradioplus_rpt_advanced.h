/** @file
 * @brief Separate Asterisk interface for the hardware-clocked rpt_advanced controller.
 */
#ifndef USBRADIOPLUS_RPT_ADVANCED_H
#define USBRADIOPLUS_RPT_ADVANCED_H

struct ast_channel;
struct ast_channel_tech;

/** @brief Register the native-rate interface using the selected hardware backend.
 * @param backend Existing app_rpt technology providing hardware lifecycle callbacks.
 * @param native_rate Hardware engine's native PCM rate.
 * @param configure Select native controller mode on an exclusively owned, unstarted channel.
 * @return Zero on success, minus one on allocation or registration failure.
 */
int usbradioplus_advanced_register(const struct ast_channel_tech *backend, unsigned int native_rate,
				   void (*configure)(struct ast_channel *channel));

/** @brief Unregister and release the advanced interface after its channels stop. */
void usbradioplus_advanced_unregister(void);
#endif
