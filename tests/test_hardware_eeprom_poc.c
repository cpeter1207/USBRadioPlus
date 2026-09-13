/**
 * @file test_hardware_eeprom_poc.c
 * @brief Regression tests for CM119 GPIO-adapter EEPROM compatibility mapping.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "usbradioplus_hardware_eeprom_poc.h"

/** @brief Construct one valid physical image with recognizable user words. */
static struct rptadv_gpio_eeprom_image valid_image(void)
{
	struct rptadv_gpio_eeprom_image image = {
		.struct_size = sizeof(image),
		.abi_version = RPTADV_GPIO_ADAPTER_ABI_VERSION,
		.checksum_valid = 1U,
		.magic_valid = 1U,
	};
	size_t index;

	image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] = RPTADV_GPIO_CM119_EEPROM_MAGIC;
	for (index = 1U; index < USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT; ++index)
		image.words[RPTADV_GPIO_CM119_EEPROM_START_WORD + index] = (uint16_t)(400U + index);
	return image;
}

/** @brief Verify physical-to-user import is exact and transactional on rejection. */
static void test_import(void)
{
	struct rptadv_gpio_eeprom_image image = valid_image();
	uint16_t words[USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT] = {0};
	uint16_t retained[USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT];

	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == 0);
	assert(words[0] == RPTADV_GPIO_CM119_EEPROM_MAGIC);
	assert(words[1] == 401U);
	assert(words[USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT - 1U] ==
	       400U + USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT - 1U);

	memcpy(retained, words, sizeof(retained));
	assert(usbradioplus_hardware_eeprom_poc_import(
		       NULL, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, NULL, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	image.struct_size = sizeof(image) - 1U;
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	image = valid_image();
	image.abi_version++;
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	image = valid_image();
	image.checksum_valid = 0U;
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	image = valid_image();
	image.magic_valid = 0U;
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	image = valid_image();
	image.checksum_valid = 1U;
	image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] = 0U;
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT) == -1);
	assert(!memcmp(words, retained, sizeof(words)));
	image = valid_image();
	assert(usbradioplus_hardware_eeprom_poc_import(
		       &image, words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT - 1U) == -1);
}

/** @brief Verify user-to-physical export preserves the manufacturer region. */
static void test_export(void)
{
	uint16_t words[USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT] = {0};
	struct rptadv_gpio_eeprom_image image = valid_image();
	struct rptadv_gpio_eeprom_image retained = image;
	size_t index;

	for (index = 0U; index < USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT; ++index)
		words[index] = (uint16_t)(700U + index);
	assert(usbradioplus_hardware_eeprom_poc_export(
		       words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT, &image) == 0);
	assert(image.struct_size == sizeof(image));
	assert(image.abi_version == RPTADV_GPIO_ADAPTER_ABI_VERSION);
	assert(!image.magic_valid && !image.checksum_valid);
	for (index = 0U; index < RPTADV_GPIO_CM119_EEPROM_START_WORD; ++index)
		assert(image.words[index] == 0U);
	for (index = 0U; index < USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT; ++index)
		assert(image.words[RPTADV_GPIO_CM119_EEPROM_START_WORD + index] == words[index]);
	image = retained;
	assert(usbradioplus_hardware_eeprom_poc_export(
		       NULL, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT, &image) == -1);
	assert(!memcmp(&image, &retained, sizeof(image)));
	assert(usbradioplus_hardware_eeprom_poc_export(
		       words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT, NULL) == -1);
	image = retained;
	assert(usbradioplus_hardware_eeprom_poc_export(
		       words, USBRADIOPLUS_HARDWARE_EEPROM_POC_USER_WORD_COUNT - 1U, &image) == -1);
	assert(image.words[RPTADV_GPIO_CM119_EEPROM_MAGIC_WORD] == RPTADV_GPIO_CM119_EEPROM_MAGIC);
}

int main(void)
{
	test_import();
	test_export();
	return 0;
}
