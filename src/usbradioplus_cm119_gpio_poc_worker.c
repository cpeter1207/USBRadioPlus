/**
 * @file
 * @brief USBRadioPlus CM119 GPIO POC worker.
 *
 * Shared retry and wake-poll lifecycle for the CM119 GPIO proof.
 */

#include "usbradioplus_cm119_gpio_poc_worker.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

/** \brief Maximum time between non-wake HID service passes. */
#define URP_CM119_GPIO_POC_WORKER_POLL_MILLISECONDS 5
/** \brief Backoff after a failed hardware ownership attempt. */
#define URP_CM119_GPIO_POC_WORKER_RETRY_MICROSECONDS 500000U

/**
 * \brief Return whether a callback table is complete enough to run safely.
 * \param ops Candidate adapter callback table.
 * \return Nonzero only when every lifecycle callback is available.
 */
static int cm119_gpio_poc_worker_ops_valid(const struct usbradioplus_cm119_gpio_poc_worker_ops *ops)
{
	return ops && ops->struct_size >= sizeof(*ops) && ops->stop_requested && ops->online &&
	       ops->clear_published_state && ops->validate && ops->start_attempt && ops->service &&
	       ops->mark_online && ops->wake_read_fd && ops->drain_wake && ops->stop_attempt &&
	       ops->release_identity && ops->report;
}

/**
 * \brief Retire one started or partially started attempt in ownership order.
 * \param ops Complete adapter callback table for the retiring attempt.
 */
static void cm119_gpio_poc_worker_cleanup(const struct usbradioplus_cm119_gpio_poc_worker_ops *ops)
{
	ops->stop_attempt(ops->opaque);
	ops->release_identity(ops->opaque);
}

/**
 * \brief Wait before retrying unless a shutdown request arrived first.
 * \param ops Complete adapter callback table for the retrying worker.
 */
static void
cm119_gpio_poc_worker_retry_delay(const struct usbradioplus_cm119_gpio_poc_worker_ops *ops)
{
	if (!ops->stop_requested(ops->opaque))
		(void)usleep(URP_CM119_GPIO_POC_WORKER_RETRY_MICROSECONDS);
}

void *
usbradioplus_cm119_gpio_poc_worker_run(const struct usbradioplus_cm119_gpio_poc_worker_ops *ops)
{
	struct pollfd wake;

	if (!cm119_gpio_poc_worker_ops_valid(ops))
		return NULL;
	ops->clear_published_state(ops->opaque);
	if (ops->validate(ops->opaque)) {
		ops->report(ops->opaque, USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED, 0);
		return NULL;
	}

	while (!ops->stop_requested(ops->opaque)) {
		int result = ops->start_attempt(ops->opaque);
		if (result) {
			ops->report(ops->opaque, USBRADIOPLUS_CM119_GPIO_POC_WORKER_START_FAILED,
				    result);
			cm119_gpio_poc_worker_cleanup(ops);
			cm119_gpio_poc_worker_retry_delay(ops);
			continue;
		}
		result = ops->service(ops->opaque);
		if (result) {
			ops->report(ops->opaque,
				    USBRADIOPLUS_CM119_GPIO_POC_WORKER_INITIAL_SERVICE_FAILED,
				    result);
			cm119_gpio_poc_worker_cleanup(ops);
			cm119_gpio_poc_worker_retry_delay(ops);
			continue;
		}
		ops->mark_online(ops->opaque);

		while (!ops->stop_requested(ops->opaque) && ops->online(ops->opaque)) {
			wake.fd = ops->wake_read_fd(ops->opaque);
			wake.events = POLLIN;
			wake.revents = 0;
			if (wake.fd < 0) {
				ops->report(ops->opaque,
					    USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED, EINVAL);
				break;
			}
			result = poll(&wake, 1U, URP_CM119_GPIO_POC_WORKER_POLL_MILLISECONDS);
			if (result < 0) {
				ops->report(ops->opaque,
					    USBRADIOPLUS_CM119_GPIO_POC_WORKER_POLL_FAILED, errno);
				continue;
			}
			if (wake.revents && ops->drain_wake(ops->opaque)) {
				ops->report(ops->opaque,
					    USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED, errno);
				break;
			}
			result = ops->service(ops->opaque);
			if (result) {
				ops->report(ops->opaque,
					    USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED,
					    result);
				break;
			}
		}

		cm119_gpio_poc_worker_cleanup(ops);
		cm119_gpio_poc_worker_retry_delay(ops);
	}
	return NULL;
}
