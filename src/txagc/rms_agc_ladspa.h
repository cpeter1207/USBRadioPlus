/* SPDX-License-Identifier: MIT */
/** @file
 * @brief Ports for the FFmpeg-hosted USBRadioPlus RMS gain rider.
 */
#ifndef USBRADIOPLUS_RMS_AGC_LADSPA_H
#define USBRADIOPLUS_RMS_AGC_LADSPA_H

#include <ladspa.h>

/** @brief LADSPA ports; the two inputs carry program audio and detector audio separately. */
enum usbradioplus_agc_port {
	USBRADIOPLUS_AGC_INPUT,	   /**< Unfiltered normalized floating-point program audio. */
	USBRADIOPLUS_AGC_DETECTOR, /**< Detector audio filtered by the surrounding FFmpeg graph. */
	USBRADIOPLUS_AGC_OUTPUT,   /**< Program audio multiplied by the continuously ramped gain. */
	USBRADIOPLUS_AGC_TARGET_DBFS,  /**< Desired detector RMS level, dBFS. */
	USBRADIOPLUS_AGC_AVERAGING_MS, /**< Target-level RMS averaging time, milliseconds. */
	USBRADIOPLUS_AGC_INCREASE_DB_PER_SECOND, /**< Maximum upward gain rate, dB/second. */
	USBRADIOPLUS_AGC_DECREASE_DB_PER_SECOND, /**< Maximum downward gain rate, dB/second. */
	USBRADIOPLUS_AGC_MAX_BOOST_DB,		 /**< Maximum positive gain, dB. */
	USBRADIOPLUS_AGC_MAX_ATTENUATION_DB,	 /**< Maximum attenuation, positive dB. */
	USBRADIOPLUS_AGC_ACTIVITY_DBFS,		 /**< Activity opening threshold, dBFS RMS. */
	USBRADIOPLUS_AGC_ACTIVITY_HYSTERESIS_DB, /**< Difference between opening and closing
						    thresholds. */
	USBRADIOPLUS_AGC_HOLD_MS,     /**< Continuous low-level activity required before increasing
					 gain. */
	USBRADIOPLUS_AGC_DEADBAND_DB, /**< Level error in either direction that leaves gain
					 unchanged. */
	USBRADIOPLUS_AGC_PORT_COUNT   /**< Number of connected audio and control ports. */
};

/** @brief Return the plugin descriptor for index zero.
 * @param index Plugin index; only zero is defined.
 * @return Static descriptor, or NULL for any other index.
 */
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index);

#endif
