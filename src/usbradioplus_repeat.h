/** @file
 * @brief Prepare native repeat audio with repeat level and DTMF muting.
 */

#ifndef USBRADIOPLUS_REPEAT_H
#define USBRADIOPLUS_REPEAT_H

#include <stddef.h>

/** @brief Apply repeat level and native DTMF muting to the transmit-branch copy.
 * @param output Destination sample buffer owned by the caller.
 * @param input Input samples; the caller retains ownership.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param gain Linear amplitude multiplier.
 * @param muted Nonzero replaces native repeat audio with silence.
 */
void urp_native_repeat_prepare(double *output, const double *input, size_t samples, double gain,
			       int muted);

#endif
