#ifndef USBRADIOPLUS_HARDWARE_H
#define USBRADIOPLUS_HARDWARE_H

#include <stdint.h>

struct urp_rtx_words {
	uint32_t reference;
	uint32_t synthesizer;
};

struct urp_parallel_bus {
	uint8_t value;
	void (*write)(void *opaque, uint8_t value);
	void *opaque;
};

struct urp_rtx_words urp_hardware_rtx_words(uint32_t rx_freq, uint32_t tx_freq, int transmitting);
void urp_hardware_set_channel(struct urp_parallel_bus *bus, uint8_t channel);
void urp_hardware_program_radio(struct urp_parallel_bus *bus, uint32_t rx_freq, uint32_t tx_freq,
				int transmitting, int high_power);

#endif
