/**
 * @file
 * @brief USBRadioPlus hardware EEPROM POC API.
 *
 * CM119 tuning-EEPROM compatibility mapping for the GPIO-adapter proof.
 */

#ifndef USBRADIOPLUS_HARDWARE_EEPROM_POC_H
#define USBRADIOPLUS_HARDWARE_EEPROM_POC_H

#include <stddef.h>
#include <stdint.h>

#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>

/** Number of ASL-compatible user words stored in a CM119 EEPROM. */
#define USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT                                           \
	(RPTADV_GPIO_CM119_EEPROM_CHECKSUM_WORD - RPTADV_GPIO_CM119_EEPROM_START_WORD + 1U)

/**
 * @brief Copy a validated physical EEPROM image into ASL-compatible user words.
 * \param image Physical-addressed image returned by the GPIO adapter.
 * \param user_words Destination for the established zero-based user region.
 * \param user_word_count Capacity of \p user_words in 16-bit words.
 * \return Zero on success, or minus one without changing \p user_words.
 *
 * The GPIO adapter owns checksum validation and physical EEPROM transfers.
 * This narrow compatibility helper only translates physical words 51 through
 * 63 to the historical zero-based 13-word tuning image.
 */
int usbradioplus_hardware_eeprom_poc_import(const struct rptadv_gpio_eeprom_image *image,
					    uint16_t *user_words, size_t user_word_count);

/**
 * @brief Prepare a physical EEPROM image from ASL-compatible user words.
 * \param user_words Established zero-based 13-word tuning image.
 * \param user_word_count Number of words supplied in \p user_words.
 * \param image Receives a complete physical-addressed image for the GPIO adapter.
 * \return Zero on success, or minus one without changing \p image.
 *
 * The GPIO adapter stamps the established magic and checksum immediately
 * before programming, preserving manufacturer-reserved EEPROM words.
 */
int usbradioplus_hardware_eeprom_poc_export(const uint16_t *user_words, size_t user_word_count,
					    struct rptadv_gpio_eeprom_image *image);

#endif /* USBRADIOPLUS_HARDWARE_EEPROM_POC_H */
