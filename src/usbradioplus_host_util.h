/** @file
 * @brief Small POSIX and Asterisk CLI services used by the compatibility adapter.
 */

#ifndef USBRADIOPLUS_HOST_UTIL_H
#define USBRADIOPLUS_HOST_UTIL_H

#include <sys/time.h>
#include <time.h>

struct rptadv_radio_audio_statistics;

/** @brief Read monotonic elapsed seconds for host-side watchdogs.
 * @param seconds Receives the current monotonic clock in whole seconds.
 */
void usbradioplus_host_time(time_t *seconds);

/** @brief Read monotonic time for host-side interval measurements.
 * @return Monotonic seconds and microseconds, or zero if the clock is unavailable.
 */
struct timeval usbradioplus_host_tvnow(void);

/** @brief Wait for CLI input without consuming the user's keystroke.
 * @param fd Asterisk CLI descriptor.
 * @param milliseconds POSIX poll timeout in milliseconds.
 * @return POSIX poll result: zero on timeout, positive on readiness, negative on error.
 */
int usbradioplus_host_poll_input(int fd, int milliseconds);

/** @brief Wait during calibration, optionally allowing keyboard cancellation.
 * @param fd Asterisk CLI descriptor.
 * @param milliseconds Nonnegative requested delay in milliseconds.
 * @param interactive Nonzero enables input polling and established carriage returns.
 * @return One when interactive input or a poll error interrupts the wait, otherwise zero.
 */
int usbradioplus_host_wait_or_poll(int fd, int milliseconds, int interactive);

/** @brief Print a portable meter snapshot using the established tuning CLI text.
 * @param fd Nonnegative Asterisk CLI output descriptor.
 * @param statistics Portable one-second history in signed-16 measurement units.
 * @param prefix Established receive or transmit label, respectively "Rx" or "Tx".
 */
void usbradioplus_host_print_audio_stats(int fd,
					 const struct rptadv_radio_audio_statistics *statistics,
					 const char *prefix);

#endif
