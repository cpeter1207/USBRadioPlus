/**
 * @file
 * @brief USBRadioPlus PortAudio POC status.
 *
 * Bounded status-event retention for the optional PortAudio proof.
 */

#include "usbradioplus_portaudio_poc_status.h"

#include <stddef.h>
#include <string.h>

/**
 * @brief Copy bounded CTCSS text while preserving an intentionally empty value.
 * @param destination Destination event slot.
 * @param frequency Source frequency text, possibly null.
 */
static void
portaudio_poc_status_copy_frequency(struct usbradioplus_portaudio_poc_status_event *destination,
				    const char *frequency)
{
	size_t length = 0U;

	if (frequency) {
		while (length + 1U < sizeof(destination->ctcss_frequency) && frequency[length])
			++length;
		memcpy(destination->ctcss_frequency, frequency, length);
	}
	destination->ctcss_frequency[length] = '\0';
}

/**
 * @brief Reserve and publish one already initialized status event.
 * @param handoff Shared callback-to-worker status handoff.
 * @param event Non-NULL local event to copy into a bounded slot.
 * @return One \c usbradioplus_portaudio_poc_status_result value.
 */
static enum usbradioplus_portaudio_poc_status_result
portaudio_poc_status_publish(struct usbradioplus_portaudio_poc_status_handoff *handoff,
			     const struct usbradioplus_portaudio_poc_status_event *event)
{
	uint64_t produced;
	uint64_t consumed;

	if (!handoff)
		return USBRADIOPLUS_PORTAUDIO_POC_STATUS_INVALID;
	produced = atomic_load_explicit(&handoff->produced, memory_order_relaxed);
	consumed = atomic_load_explicit(&handoff->consumed, memory_order_acquire);
	if (produced - consumed >= URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY)
		return USBRADIOPLUS_PORTAUDIO_POC_STATUS_FULL;
	handoff->events[produced % URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY] = *event;
	atomic_store_explicit(&handoff->produced, produced + 1U, memory_order_release);
	return USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY;
}

void usbradioplus_portaudio_poc_status_init(
	struct usbradioplus_portaudio_poc_status_handoff *handoff)
{
	if (!handoff)
		return;
	atomic_init(&handoff->produced, 0U);
	atomic_init(&handoff->consumed, 0U);
}

void usbradioplus_portaudio_poc_status_reset(
	struct usbradioplus_portaudio_poc_status_handoff *handoff)
{
	if (!handoff)
		return;
	atomic_store_explicit(&handoff->produced, 0U, memory_order_relaxed);
	atomic_store_explicit(&handoff->consumed, 0U, memory_order_relaxed);
}

enum usbradioplus_portaudio_poc_status_result usbradioplus_portaudio_poc_status_publish_ctcss(
	struct usbradioplus_portaudio_poc_status_handoff *handoff, const char *frequency)
{
	struct usbradioplus_portaudio_poc_status_event event = {
		.type = USBRADIOPLUS_PORTAUDIO_POC_STATUS_CTCSS,
	};

	portaudio_poc_status_copy_frequency(&event, frequency);
	return portaudio_poc_status_publish(handoff, &event);
}

enum usbradioplus_portaudio_poc_status_result usbradioplus_portaudio_poc_status_publish_voter(
	struct usbradioplus_portaudio_poc_status_handoff *handoff, int rssi)
{
	const struct usbradioplus_portaudio_poc_status_event event = {
		.type = USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER,
		.voter_rssi = rssi,
	};

	return portaudio_poc_status_publish(handoff, &event);
}

uint64_t usbradioplus_portaudio_poc_status_published(
	const struct usbradioplus_portaudio_poc_status_handoff *handoff)
{
	if (!handoff)
		return 0U;
	return atomic_load_explicit(&handoff->produced, memory_order_acquire);
}

enum usbradioplus_portaudio_poc_status_result
usbradioplus_portaudio_poc_status_consume(struct usbradioplus_portaudio_poc_status_handoff *handoff,
					  uint64_t *next_sequence,
					  struct usbradioplus_portaudio_poc_status_event *event)
{
	uint64_t produced;

	if (!handoff || !next_sequence || !event)
		return USBRADIOPLUS_PORTAUDIO_POC_STATUS_INVALID;
	produced = atomic_load_explicit(&handoff->produced, memory_order_acquire);
	if (*next_sequence >= produced)
		return USBRADIOPLUS_PORTAUDIO_POC_STATUS_EMPTY;
	*event = handoff->events[*next_sequence % URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY];
	++*next_sequence;
	atomic_store_explicit(&handoff->consumed, *next_sequence, memory_order_release);
	return USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY;
}
