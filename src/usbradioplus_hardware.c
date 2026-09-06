/** @file
 * @brief Parallel-port channel selection and RTX synthesizer programming.
 */

/* Parallel-port protocol for legacy binary and RTX-programmable radios. */
#include "usbradioplus_hardware.h"

#define URP_PP_REGISTER_BITS 20

#define URP_PP_BIT_TIME 100000

#define URP_DTX_CLK 0x01U /* connector pin 2 */

#define URP_DTX_DATA 0x02U /* connector pin 3 */

#define URP_DTX_ENABLE 0x04U /* connector pin 4 */

#define URP_DTX_TX 0x08U /* connector pin 5 */

#define URP_DTX_TXPWR 0x10U /* connector pin 6; retained low */

#define URP_BIN_PROG_MASK 0xf0U /* connector pins 6 through 9 */

/** @brief Wait the parallel-port protocol interval required for hardware settling.
 * @param multiplier Number of base hardware-settling intervals.
 */
static void urp_hardware_delay(unsigned multiplier)
{
	volatile unsigned i;
	for (i = 0; i < URP_PP_BIT_TIME * multiplier; ++i)
		;
}

/** @brief Write the cached parallel-port byte through the caller's bus callback.
 * @param bus Parallel-port value and caller-supplied write callback.
 */
static void urp_hardware_write(struct urp_parallel_bus *bus)
{
	bus->write(bus->opaque, bus->value);
}

/** @brief Clock a synthesizer programming word over the parallel-port serial interface.
 * @param bus Parallel-port value and caller-supplied write callback.
 * @param data 20-bit synthesizer word, shifted most-significant bit first.
 */
static void urp_hardware_spi(struct urp_parallel_bus *bus, uint32_t data)
{
	static int initialized;
	uint32_t bit = 0x00080000U;
	int i;

	bus->value &= ~(URP_DTX_CLK | URP_DTX_DATA | URP_DTX_ENABLE | URP_DTX_TXPWR | URP_DTX_TX);
	urp_hardware_write(bus);
	urp_hardware_delay(initialized ? 4 : 200);
	initialized = 1;

	for (i = 0; i < URP_PP_REGISTER_BITS; ++i, bit >>= 1) {
		if (data & bit)
			bus->value |= URP_DTX_DATA;
		else
			bus->value &= ~URP_DTX_DATA;
		urp_hardware_write(bus);
		urp_hardware_delay(1);
		bus->value |= URP_DTX_CLK;
		urp_hardware_write(bus);
		urp_hardware_delay(1);
		bus->value &= ~URP_DTX_CLK;
		urp_hardware_write(bus);
		urp_hardware_delay(1);
	}
	bus->value &= ~(URP_DTX_CLK | URP_DTX_DATA);
	urp_hardware_write(bus);
	bus->value |= URP_DTX_ENABLE;
	urp_hardware_write(bus);
	urp_hardware_delay(1);
	bus->value &= ~URP_DTX_ENABLE;
	urp_hardware_write(bus);
}

struct urp_rtx_words urp_hardware_rtx_words(uint32_t rx_freq, uint32_t tx_freq, int transmitting)
{
	const uint32_t reference_freq = rx_freq > 200000000U ? 16012500U : 16000000U;
	const uint32_t step_freq = rx_freq > 200000000U ? 12500U : 5000U;
	const uint32_t rx_if_freq = rx_freq > 200000000U ? 21400000U : 10700000U;
	uint32_t synth_freq = transmitting ? tx_freq : rx_freq - rx_if_freq;
	uint32_t word = (synth_freq / step_freq) << 1;
	struct urp_rtx_words result;

	result.reference = ((reference_freq / step_freq) << 1) | 1U;
	result.synthesizer = ((word & 0xffffff80U) << 1) + (word & 0x7fU);
	return result;
}

void urp_hardware_set_channel(struct urp_parallel_bus *bus, uint8_t channel)
{
	uint8_t channel_mask;

	if (!bus || !bus->write)
		return;
	bus->value |= URP_BIN_PROG_MASK;
	urp_hardware_write(bus);
	/* The four legacy channel-select outputs are active low. */
	channel_mask = (uint8_t)(channel << 4);
	bus->value &= (uint8_t)~channel_mask;
	urp_hardware_write(bus);
}

void urp_hardware_program_radio(struct urp_parallel_bus *bus, uint32_t rx_freq, uint32_t tx_freq,
				int transmitting, int high_power)
{
	struct urp_rtx_words words;
	(void)high_power; /* The original interface never enabled this line. */
	if (!bus || !bus->write || !rx_freq)
		return;
	words = urp_hardware_rtx_words(rx_freq, tx_freq, transmitting);
	urp_hardware_spi(bus, words.reference);
	urp_hardware_spi(bus, words.synthesizer);
	bus->value &= ~(URP_DTX_CLK | URP_DTX_DATA | URP_DTX_ENABLE);
	if (transmitting) {
		bus->value &= ~URP_DTX_TXPWR;
		bus->value |= URP_DTX_TX;
	} else {
		bus->value &= ~(URP_DTX_TX | URP_DTX_TXPWR);
	}
	urp_hardware_write(bus);
}

/** @name File-local and build-time constants
 * @{ */
/** @def URP_PP_REGISTER_BITS
 * @brief Bits shifted into an RTX programming register.
 */
/** @def URP_PP_BIT_TIME
 * @brief Base parallel serial-bit settling interval.
 */
/** @def URP_DTX_CLK
 * @brief connector pin 2
 */
/** @def URP_DTX_DATA
 * @brief connector pin 3
 */
/** @def URP_DTX_ENABLE
 * @brief connector pin 4
 */
/** @def URP_DTX_TX
 * @brief connector pin 5
 */
/** @def URP_DTX_TXPWR
 * @brief connector pin 6; retained low
 */
/** @def URP_BIN_PROG_MASK
 * @brief connector pins 6 through 9
 */
/** @} */
