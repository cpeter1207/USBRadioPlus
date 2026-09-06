/** @file
 * @brief Parallel-port channel selection and RTX synthesizer programming.
 */

#ifndef USBRADIOPLUS_HARDWARE_H
#define USBRADIOPLUS_HARDWARE_H

#include <stdint.h>

/** RTX reference-divider and synthesizer programming words. */
struct urp_rtx_words {
	/** RTX reference-divider programming word. */
	uint32_t reference;
	/** RTX synthesizer programming word. */
	uint32_t synthesizer;
};

/** Parallel-port output byte with a caller-owned write callback and context. */
struct urp_parallel_bus {
	/** Cached parallel-port output byte. */
	uint8_t value;
	/** Callback that writes the cached hardware output byte. */
	void (*write)(void *opaque, uint8_t value);
	/** Caller-owned callback context. */
	void *opaque;
};

/** @brief Calculate RTX reference and synthesizer words for receive or transmit.
 * @param rx_freq Receive frequency in Hz.
 * @param tx_freq Transmit frequency in Hz.
 * @param transmitting Nonzero selects the transmit synthesizer settings.
 * @return Reference and synthesizer words for the requested operating frequency.
 */
struct urp_rtx_words urp_hardware_rtx_words(uint32_t rx_freq, uint32_t tx_freq, int transmitting);
/** @brief Latch a binary channel number onto the parallel-port radio interface.
 * @param bus Parallel-port value and caller-supplied write callback.
 * @param channel Binary radio channel-select code.
 */
void urp_hardware_set_channel(struct urp_parallel_bus *bus, uint8_t channel);
/** @brief Program RTX frequency and power using the parallel-port bus.
 * @param bus Parallel-port value and caller-supplied write callback.
 * @param rx_freq Receive frequency in Hz.
 * @param tx_freq Transmit frequency in Hz.
 * @param transmitting Nonzero selects the transmit synthesizer settings.
 * @param high_power Nonzero requests the hardware high-power setting.
 */
void urp_hardware_program_radio(struct urp_parallel_bus *bus, uint32_t rx_freq, uint32_t tx_freq,
				int transmitting, int high_power);

#endif
