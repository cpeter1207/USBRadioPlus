/**
 * @file
 * @brief USBRadioPlus CM119 GPIO POC worker API.
 *
 * Adapter-neutral lifecycle worker for the opt-in CM119 GPIO proof.
 *
 * The legacy and future modern channel adapters own incompatible private
 * channel structures.  This worker therefore owns only the common retry,
 * wake-poll, and teardown sequencing while a small callback table supplies
 * adapter-specific state access and hardware operations.
 */

#ifndef USBRADIOPLUS_CM119_GPIO_POC_WORKER_H
#define USBRADIOPLUS_CM119_GPIO_POC_WORKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Lifecycle failure reported by the common CM119 GPIO POC worker. */
enum usbradioplus_cm119_gpio_poc_worker_event {
	/** Adapter configuration was not valid for the opt-in proof. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_VALIDATE_FAILED,
	/** A hardware ownership attempt did not start. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_START_FAILED,
	/** The first HID service pass after startup failed. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_INITIAL_SERVICE_FAILED,
	/** Polling the advisory PTT wake descriptor failed. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_POLL_FAILED,
	/** Draining the advisory PTT wake descriptor failed. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_WAKE_FAILED,
	/** A later HID service pass failed. */
	USBRADIOPLUS_CM119_GPIO_POC_WORKER_SERVICE_FAILED,
};

/**
 * \brief Adapter-specific operations used by one CM119 GPIO POC worker.
 *
 * Every callback runs on the worker thread.  The common worker never
 * interprets \c opaque, so adapters with different private channel layouts
 * can share its lifecycle without casting one private structure to another.
 */
struct usbradioplus_cm119_gpio_poc_worker_ops {
	/** Size of this structure for append-only compatibility checks. */
	size_t struct_size;
	/** Adapter-private channel or worker state. */
	void *opaque;
	/** Return nonzero when the adapter has requested worker shutdown. */
	int (*stop_requested)(void *opaque);
	/** Return nonzero while the adapter considers hardware online. */
	int (*online)(void *opaque);
	/** Clear published hardware state before validation and startup. */
	void (*clear_published_state)(void *opaque);
	/** Validate immutable opt-in configuration before any claim attempt. */
	int (*validate)(void *opaque);
	/** Prepare, reserve, and start one complete hardware ownership attempt. */
	int (*start_attempt)(void *opaque);
	/** Service one HID/GPIO cycle and publish its resulting state. */
	int (*service)(void *opaque);
	/** Publish online state after the first successful service cycle. */
	void (*mark_online)(void *opaque);
	/** Return the advisory PTT wake descriptor, or a negative value on failure. */
	int (*wake_read_fd)(void *opaque);
	/** Drain pending advisory wake bytes. */
	int (*drain_wake)(void *opaque);
	/** Stop an attempt and clear adapter-owned stream/HID state. */
	void (*stop_attempt)(void *opaque);
	/** Release the adapter-specific device-identity reservation. */
	void (*release_identity)(void *opaque);
	/** Report one failure; \p detail is a callback result or errno where useful. */
	void (*report)(void *opaque, enum usbradioplus_cm119_gpio_poc_worker_event event,
		       int detail);
};

/**
 * \brief Run the common CM119 GPIO POC worker lifecycle.
 * \param ops Complete callback table for one adapter-private channel state.
 * \return Always NULL so the function can be used directly as a thread entry point.
 *
 * The worker clears published state, validates once, retries failed ownership
 * attempts after 500 ms, services HID/GPIO at most every 5 ms, and always
 * stops the attempt before releasing its identity reservation.  It owns no
 * file descriptor or adapter resource directly; callbacks retain that
 * ownership so existing adapter shutdown paths remain authoritative.
 */
void *
usbradioplus_cm119_gpio_poc_worker_run(const struct usbradioplus_cm119_gpio_poc_worker_ops *ops);

#ifdef __cplusplus
}
#endif

#endif
