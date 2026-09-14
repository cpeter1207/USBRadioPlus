/* SPDX-License-Identifier: MIT */
/** @file
 * @brief Frozen LADSPA port contract used by the Rust plugin parity test.
 */
#ifndef USBRADIOPLUS_TEST_RMS_AGC_LADSPA_H
#define USBRADIOPLUS_TEST_RMS_AGC_LADSPA_H

#include <ladspa.h>

/** LADSPA ports shared by the production Rust plugin and frozen C reference. */
enum usbradioplus_agc_port {
	USBRADIOPLUS_AGC_INPUT,
	USBRADIOPLUS_AGC_DETECTOR,
	USBRADIOPLUS_AGC_OUTPUT,
	USBRADIOPLUS_AGC_TARGET_DBFS,
	USBRADIOPLUS_AGC_AVERAGING_MS,
	USBRADIOPLUS_AGC_INCREASE_DB_PER_SECOND,
	USBRADIOPLUS_AGC_DECREASE_DB_PER_SECOND,
	USBRADIOPLUS_AGC_MAX_BOOST_DB,
	USBRADIOPLUS_AGC_MAX_ATTENUATION_DB,
	USBRADIOPLUS_AGC_ACTIVITY_DBFS,
	USBRADIOPLUS_AGC_ACTIVITY_HYSTERESIS_DB,
	USBRADIOPLUS_AGC_HOLD_MS,
	USBRADIOPLUS_AGC_DEADBAND_DB,
	USBRADIOPLUS_AGC_PORT_COUNT,
};

/** Return the production Rust plugin descriptor for index zero. */
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index);

#endif
