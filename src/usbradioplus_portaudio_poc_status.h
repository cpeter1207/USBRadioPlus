/**
 * @file
 * @brief USBRadioPlus PortAudio POC status API.
 *
 * Lock-free status-event handoff for the optional PortAudio proof.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_STATUS_H
#define USBRADIOPLUS_PORTAUDIO_POC_STATUS_H

#include <stdatomic.h>
#include <stdint.h>

/** @brief Preallocated status events retained while the receive audio handoff is full. */
enum { URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY = 32U };

/** @brief Type of an Asterisk text event deferred from the hardware callback. */
enum usbradioplus_portaudio_poc_status_event_type {
	/** Transmit CTCSS readiness text, including an intentionally empty frequency. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_CTCSS = 0,
	/** Voter RSSI text in the legacy normalized 0--1000 scale. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER,
};

/** @brief One complete callback-generated text event. */
struct usbradioplus_portaudio_poc_status_event {
	/** Text event category. */
	enum usbradioplus_portaudio_poc_status_event_type type;
	/** CTCSS frequency text; empty is a valid final `cstx=` event. */
	char ctcss_frequency[32];
	/** Voter RSSI value when \c type is \c USBRADIOPLUS_PORTAUDIO_POC_STATUS_VOTER. */
	int voter_rssi;
};

/**
 * @brief Fixed SPSC event queue retained independently from receive PCM.
 *
 * The callback produces status events before it publishes their associated
 * audio block.  The Asterisk worker later drains events through that block's
 * recorded sequence limit after its key/unkey edge and before its voice frame.
 * Consequently a full PCM handoff cannot discard CTCSS-ready or voter text.
 */
struct usbradioplus_portaudio_poc_status_handoff {
	/** Number of fully initialized events published by the callback. */
	atomic_ullong produced;
	/** Number of events copied by the Asterisk worker. */
	atomic_ullong consumed;
	/** Preallocated event slots indexed by the monotonic sequence. */
	struct usbradioplus_portaudio_poc_status_event
		events[URP_PORTAUDIO_POC_STATUS_EVENT_CAPACITY];
};

/** @brief Result of producing or consuming one status event. */
enum usbradioplus_portaudio_poc_status_result {
	/** An event was accepted or copied. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_READY = 0,
	/** The bounded callback queue has no free event slot. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_FULL,
	/** The worker has no published event to copy. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_EMPTY,
	/** Caller arguments are invalid. */
	USBRADIOPLUS_PORTAUDIO_POC_STATUS_INVALID,
};

/**
 * @brief Initialize a status handoff before its callback and worker start.
 * @param handoff Preallocated handoff storage.
 */
void usbradioplus_portaudio_poc_status_init(
	struct usbradioplus_portaudio_poc_status_handoff *handoff);

/**
 * @brief Reset a status handoff after callback and worker are quiesced.
 * @param handoff Previously initialized handoff storage.
 */
void usbradioplus_portaudio_poc_status_reset(
	struct usbradioplus_portaudio_poc_status_handoff *handoff);

/**
 * @brief Append a CTCSS-ready event without waiting or allocating.
 * @param handoff Shared callback-to-worker status handoff.
 * @param frequency Decoded transmit frequency, or an empty string for final `cstx=`.
 * @return One \c usbradioplus_portaudio_poc_status_result value.
 */
enum usbradioplus_portaudio_poc_status_result usbradioplus_portaudio_poc_status_publish_ctcss(
	struct usbradioplus_portaudio_poc_status_handoff *handoff, const char *frequency);

/**
 * @brief Append one voter RSSI report without waiting or allocating.
 * @param handoff Shared callback-to-worker status handoff.
 * @param rssi Legacy normalized voter RSSI value.
 * @return One \c usbradioplus_portaudio_poc_status_result value.
 */
enum usbradioplus_portaudio_poc_status_result usbradioplus_portaudio_poc_status_publish_voter(
	struct usbradioplus_portaudio_poc_status_handoff *handoff, int rssi);

/**
 * @brief Obtain the completed-event sequence associated with the next PCM block.
 * @param handoff Shared callback-to-worker status handoff.
 * @return Number of events completely published before this call.
 */
uint64_t usbradioplus_portaudio_poc_status_published(
	const struct usbradioplus_portaudio_poc_status_handoff *handoff);

/**
 * @brief Copy the next callback-published event in FIFO order.
 * @param handoff Shared callback-to-worker status handoff.
 * @param next_sequence Worker-local sequence, initialized to zero at stream start.
 * @param event Destination for a copied event.
 * @return One \c usbradioplus_portaudio_poc_status_result value.
 */
enum usbradioplus_portaudio_poc_status_result
usbradioplus_portaudio_poc_status_consume(struct usbradioplus_portaudio_poc_status_handoff *handoff,
					  uint64_t *next_sequence,
					  struct usbradioplus_portaudio_poc_status_event *event);

#endif
