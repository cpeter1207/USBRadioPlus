/**
 * @file
 * @brief USBRadioPlus PortAudio POC handoff API.
 *
 * Bounded lock-free receive handoff for the optional PortAudio proof.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_H
#define USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_H

#include <stdatomic.h>

/**
 * @brief Callback-to-worker handoff indexes and loss-resynchronization state.
 *
 * One real-time producer writes complete blocks and one non-real-time consumer
 * copies them.  The consumer claim bit prevents the producer from reusing a
 * slot while it is being copied.  On overload, the producer evicts idle stale
 * blocks and marks a resynchronization; the consumer then skips to the newest
 * coherent block instead of replaying queued latency.
 */
struct usbradioplus_portaudio_poc_handoff {
	/** Next ring slot owned by the callback producer. */
	atomic_uint producer;
	/** Consumer slot plus an internal copy-in-progress claim bit. */
	atomic_uint consumer_state;
	/** Even stable overflow generation; odd while the callback publishes its replacement block.
	 */
	atomic_uint resync_generation;
	/** Callback-private completion flag for an in-progress overflow replacement. */
	unsigned int producer_resync_pending;
	/** Blocks discarded by either side to keep receive delivery current. */
	atomic_ullong discarded;
};

/** @brief Result of reserving or claiming a bounded handoff slot. */
enum usbradioplus_portaudio_poc_handoff_result {
	/** A coherent slot is ready for producer fill or consumer copy. */
	USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY = 0,
	/** No published receive block is currently available to the consumer. */
	USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_EMPTY,
	/** The producer discarded its current block while a consumer copy was active. */
	USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_DROPPED,
	/** Caller arguments cannot describe a bounded ring. */
	USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_INVALID,
};

/**
 * @brief Initialize a handoff before either worker can access it.
 * @param handoff Preallocated handoff state to initialize.
 */
void usbradioplus_portaudio_poc_handoff_init(struct usbradioplus_portaudio_poc_handoff *handoff);

/**
 * @brief Reset a quiesced handoff before restarting its producer and consumer.
 * @param handoff Previously initialized handoff with no active producer or consumer.
 */
void usbradioplus_portaudio_poc_handoff_reset(struct usbradioplus_portaudio_poc_handoff *handoff);

/**
 * @brief Reserve a callback-owned slot, evicting stale idle audio when full.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots, at least two.
 * @param slot Receives the callback-owned slot on \c USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY.
 * @return One \c usbradioplus_portaudio_poc_handoff_result value.
 *
 * The producer never waits.  It drops only the new block if the consumer is
 * actively copying the oldest slot; otherwise it advances the idle consumer
 * cursor and retains the newest callback result.
 */
enum usbradioplus_portaudio_poc_handoff_result usbradioplus_portaudio_poc_handoff_producer_reserve(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int *slot);

/**
 * @brief Publish a completely initialized callback slot to the consumer.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots, at least two.
 */
void usbradioplus_portaudio_poc_handoff_producer_publish(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count);

/**
 * @brief Claim the next coherent receive block, skipping stale overflow backlog.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots, at least two.
 * @param seen_generation Consumer-local generation initialized to zero.
 * @param slot Receives a claimed slot on \c USBRADIOPLUS_PORTAUDIO_POC_HANDOFF_READY.
 * @param resynchronized Receives nonzero when stale pending slots were skipped.
 * @return One \c usbradioplus_portaudio_poc_handoff_result value.
 *
 * The caller must copy the claimed slot before releasing or discarding it.  A
 * claim cannot be overwritten by the callback producer.
 */
enum usbradioplus_portaudio_poc_handoff_result usbradioplus_portaudio_poc_handoff_consumer_claim(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int *seen_generation, unsigned int *slot, int *resynchronized);

/**
 * @brief Return whether no overflow occurred after a claimed block was copied.
 * @param handoff Shared handoff state.
 * @param claim_generation Generation returned by the successful claim.
 * @return Nonzero when the copied block remains current for delivery.
 */
int usbradioplus_portaudio_poc_handoff_claim_current(
	const struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int claim_generation);

/**
 * @brief Release a claimed slot after its copied PCM has been consumed.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots, at least two.
 * @param slot Claimed slot returned by \c usbradioplus_portaudio_poc_handoff_consumer_claim.
 */
void usbradioplus_portaudio_poc_handoff_consumer_release(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int slot);

/**
 * @brief Release a claimed block without delivery because a newer overflow marker arrived.
 * @param handoff Shared handoff state.
 * @param slot_count Number of preallocated slots, at least two.
 * @param slot Claimed slot returned by \c usbradioplus_portaudio_poc_handoff_consumer_claim.
 */
void usbradioplus_portaudio_poc_handoff_consumer_discard(
	struct usbradioplus_portaudio_poc_handoff *handoff, unsigned int slot_count,
	unsigned int slot);

#endif
