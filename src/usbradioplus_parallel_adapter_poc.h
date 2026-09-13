/**
 * @file
 * @brief Legacy parallel-port semantics over the hardware adapter facade.
 *
 * The shared direct-hardware path keeps all ppdev/raw-I/O access in the
 * released GPIO adapter.  This bridge translates existing USBRadioPlus
 * control requests and input assignments without adding an adapter ABI.
 */

#ifndef USBRADIOPLUS_PARALLEL_ADAPTER_POC_H
#define USBRADIOPLUS_PARALLEL_ADAPTER_POC_H

#include <stdatomic.h>
#include <stdint.h>

#include "usbradioplus_hardware_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Parallel input result bit for a configured carrier input. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CARRIER UINT32_C(1)
/** @brief Parallel input result bit for a configured CTCSS input. */
#define USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_CTCSS UINT32_C(2)

/** @brief State retained by one selected adapter-owned parallel transport. */
struct usbradioplus_parallel_adapter_poc_state {
	/** Nonzero after the facade successfully opened the selected transport. */
	int opened;
	/** This channel's most recently serviced PTT request; startup begins unkeyed. */
	int last_ptt_asserted;
	/** This channel's latest consumed RTX generation, or UINT_MAX before programming. */
	uint32_t program_generation;
	/** Persistent baseline data byte, separate from any temporarily pulsed output. */
	uint8_t persistent_output;
	/** Latest binary-channel request published by the compatibility control plane. */
	atomic_uchar binary_channel;
	/** Generation making \c binary_channel visible to the service owner. */
	atomic_uint binary_generation;
	/** Binary-channel generation already serialized with the facade. */
	uint32_t serviced_binary_generation;
	/** Latches a failed asynchronous output publication until teardown. */
	atomic_int faulted;
};

/** @brief Radio-control snapshot consumed by one non-real-time service cycle. */
struct usbradioplus_parallel_adapter_poc_service_request {
	/** Nonzero when the control-plane programming snapshot is coherent. */
	int have_program;
	/** Monotonic radio-programming generation when c have_program is nonzero. */
	uint32_t program_generation;
	/** Requested receive frequency in hertz. */
	uint32_t rx_frequency_hz;
	/** Requested transmit frequency in hertz. */
	uint32_t tx_frequency_hz;
	/** Nonzero requests the legacy high-power compatibility value. */
	int high_power;
	/** Desired physical PTT state published by the native signaling tick. */
	int ptt_asserted;
	/** Nonzero inverts the configured parallel PTT pins. */
	int ptt_inverted;
	/** Parallel data-register bits assigned to PTT. */
	uint8_t ptt_mask;
	/** Nonzero requests a physical RTX and PTT release during teardown. */
	int force_unkey;
};

/** @brief Callback receiving one legacy `PP<pin> <value>` input transition. */
typedef void (*usbradioplus_parallel_adapter_poc_input_callback)(void *opaque, unsigned int pin,
								 int value);

/**
 * @brief Initialize one optional parallel bridge state.
 * @param state State to initialize.
 */
void usbradioplus_parallel_adapter_poc_init(struct usbradioplus_parallel_adapter_poc_state *state);

/**
 * @brief Open the configured legacy parallel transport through the facade.
 * @param state Initialized bridge state.
 * @param adapter Prepared hardware facade.
 * @param legacy_haspp Legacy transport selector: zero, ppdev one, or raw-I/O two.
 * @param ppdev_path Configured ppdev path when @p legacy_haspp is one.
 * @param raw_io_base Configured raw I/O base when @p legacy_haspp is two.
 * @param initial_output Legacy persistent parallel data byte.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * A zero legacy selector is a successful absent-path no-op.  The caller owns
 * legacy pre-open cleanup before this function claims an equivalent port.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_parallel_adapter_poc_open(struct usbradioplus_parallel_adapter_poc_state *state,
				       struct usbradioplus_hardware_adapter *adapter,
				       int legacy_haspp, const char *ppdev_path,
				       uint32_t raw_io_base, uint8_t initial_output);

/**
 * @brief Forget one closed parallel bridge without touching the facade.
 * @param state State to reset.
 *
 * The caller closes the encompassing facade after this reset so CM119 and
 * parallel resources retain their existing ordered teardown ownership.
 */
void usbradioplus_parallel_adapter_poc_reset(struct usbradioplus_parallel_adapter_poc_state *state);

/**
 * @brief Transfer a healthy shared transport while retaining recipient channel history.
 * @param state Current shared transport owner state.
 * @param adapter Current owner's prepared facade.
 * @param next_state Initialized recipient channel state without an open transport.
 * @param next_adapter Recipient's prepared facade without a parallel device.
 * @return Nonzero after transfer; zero when the source is absent or faulted.
 *
 * The caller serializes this operation with all parallel service and publication.
 */
int usbradioplus_parallel_adapter_poc_transfer(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_parallel_adapter_poc_state *next_state,
	struct usbradioplus_hardware_adapter *next_adapter);

/**
 * @brief Report whether this state owns an adapter parallel transport.
 * @param state Candidate bridge state.
 * @return Nonzero only after a successful non-absent open.
 */
int usbradioplus_parallel_adapter_poc_is_open(
	const struct usbradioplus_parallel_adapter_poc_state *state);

/**
 * @brief Publish a complete persistent legacy parallel output byte.
 * @param state Selected bridge state.
 * @param adapter Prepared facade with the bridge transport open.
 * @param output Legacy persistent data-register byte.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This publication performs no port I/O.  The existing non-real-time worker
 * applies it during its next service cycle.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_publish_output(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter, uint8_t output);

/**
 * @brief Schedule or cancel independent legacy baseline-XOR output pulses.
 * @param state Selected bridge state.
 * @param adapter Prepared facade with the bridge transport open.
 * @param invert_mask Data bits to invert for the supplied duration.
 * @param duration_milliseconds Duration for newly selected bits.
 * @param cancel_mask Data bits whose pending pulses are cancelled.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_schedule_pulse(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter, uint8_t invert_mask,
	uint32_t duration_milliseconds, uint8_t cancel_mask);

/**
 * @brief Publish a legacy active-low binary channel request for worker service.
 * @param state Selected bridge state.
 * @param channel Four-bit legacy channel value.
 */
void usbradioplus_parallel_adapter_poc_request_binary_channel(
	struct usbradioplus_parallel_adapter_poc_state *state, uint8_t channel);

/**
 * @brief Return the bridge's persistent data byte without temporary pulse inversions.
 * @param state Selected bridge state.
 * @return The retained baseline output byte, or zero when @p state is null.
 */
uint8_t usbradioplus_parallel_adapter_poc_persistent_output(
	const struct usbradioplus_parallel_adapter_poc_state *state);

/**
 * @brief Service adapter-owned parallel I/O and legacy radio signaling.
 * @param state Selected bridge state.
 * @param adapter Prepared facade with the bridge transport open.
 * @param client_state Requesting channel's initialized PTT/program history; may equal state.
 * @param request Current lock-free signaling/programming snapshot.
 * @param applied_output Receives the actual applied data byte after service.
 * @param inputs Optional raw input snapshot to fill.
 * @param stats Optional service statistics snapshot to fill.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * Only the existing non-real-time HID worker calls this function.  It
 * serializes binary-channel and RTX programming with facade service, then
 * applies the configured PTT bits and samples the status register.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_parallel_adapter_poc_service(
	struct usbradioplus_parallel_adapter_poc_state *state,
	struct usbradioplus_hardware_adapter *adapter,
	struct usbradioplus_parallel_adapter_poc_state *client_state,
	const struct usbradioplus_parallel_adapter_poc_service_request *request,
	uint8_t *applied_output, struct rptadv_gpio_parallel_input_snapshot *inputs,
	struct rptadv_gpio_parallel_stats *stats);

/**
 * @brief Translate raw parallel status to existing input assignments.
 * @param status_mask Raw IEEE 1284 status-register byte.
 * @param assignments Legacy pin-assignment array indexed by pin number.
 * @param had_input Receives nonzero after the first reported input state.
 * @param last_input Receives the last filtered status byte.
 * @param callback Optional recipient for changed `in` assignment states.
 * @param opaque Callback context.
 * @return A bitwise OR of USBRADIOPLUS_PARALLEL_ADAPTER_INPUT_* values.
 *
 * This preserves the compatibility adapter's status-bit inversion and its
 * historic input pin mapping for pins 10, 11, 12, 13, and 15.
 */
uint32_t usbradioplus_parallel_adapter_poc_translate_inputs(
	uint8_t status_mask, char *const assignments[16], int *had_input, int *last_input,
	usbradioplus_parallel_adapter_poc_input_callback callback, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
