/**
 * @file
 * @brief USBRadioPlus hardware EEPROM POC.
 *
 * CM119 tuning-EEPROM compatibility mapping for the GPIO-adapter proof.
 */

#include "usbradioplus_hardware_eeprom_poc.h"

#include <string.h>

/**
 * @brief Return whether an adapter result contains a complete valid user image.
 * \param image Candidate physical-addressed GPIO-adapter result.
 * \return Nonzero when the image can be imported safely.
 */
static int hardware_eeprom_poc_image_valid(const struct rptadv_gpio_eeprom_image *image)
{
	return image && image->struct_size >= sizeof(*image) &&
	       image->abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION && image->checksum_valid &&
	       image->magic_valid &&
	       image->words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] == RPTADV_GPIO_CM119_EEPROM_MAGIC;
}

int usbradioplus_hardware_eeprom_poc_import(const struct rptadv_gpio_eeprom_image *image,
					    uint16_t *user_words, size_t user_word_count)
{
	if (!hardware_eeprom_poc_image_valid(image) || !user_words ||
	    user_word_count != USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT)
		return -1;
	memcpy(user_words, &image->words[RPTADV_GPIO_CM119_EEPROM_START_WORD],
	       USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT * sizeof(*user_words));
	return 0;
}

int usbradioplus_hardware_eeprom_poc_export(const uint16_t *user_words, size_t user_word_count,
					    struct rptadv_gpio_eeprom_image *image)
{
	struct rptadv_gpio_eeprom_image next = {
		.struct_size = sizeof(next),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
	};

	if (!user_words || !image ||
	    user_word_count != USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT)
		return -1;
	memcpy(&next.words[RPTADV_GPIO_CM119_EEPROM_START_WORD], user_words,
	       USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT * sizeof(*user_words));
	*image = next;
	return 0;
}
