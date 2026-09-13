/** @file
 * @brief Prepare native repeat audio with repeat level and DTMF muting.
 */

#ifndef USBRADIOPLUS_REPEAT_H
#define USBRADIOPLUS_REPEAT_H

#include <stddef.h>

#include "usbradioplus_dsp.h"

/** @brief Preallocated canonical-f32 bridge buffers for one native repeat span.
 *
 * The compatibility renderer still holds its PCM-code workspaces as `double`.
 * This bounded workspace converts only the repeat stage at the Rust-core
 * boundary without allocating or blocking from the native tick. It disappears
 * when the remaining renderer stages move to canonical f32 PCM.
 */
struct urp_native_repeat_workspace {
	/** Normalized f32 input passed to the portable radio core. */
	float input[URP_NATIVE_MAX_SAMPLES];
	/** Normalized f32 output returned by the portable radio core. */
	float output[URP_NATIVE_MAX_SAMPLES];
};

/** @brief Validate the released Rust radio-core repeat descriptor at setup.
 * @return Zero when the dynamic core supports the required ABI, otherwise nonzero.
 *
 * This is a control-plane operation. The native tick only reads the immutable
 * descriptor pointer published by this function.
 */
int urp_native_repeat_initialize(void);

/** @brief Apply repeat level and native DTMF muting to the transmit-branch copy.
 * @param output Destination sample buffer owned by the caller.
 * @param input Input samples; the caller retains ownership.
 * @param samples Audio samples; mutable buffers are updated in place.
 * @param gain Linear amplitude multiplier.
 * @param muted Nonzero replaces native repeat audio with silence.
 * @param workspace Preallocated f32 boundary workspace owned by the native renderer.
 * @return Zero after processing, otherwise nonzero after safely silencing @p output.
 */
int urp_native_repeat_prepare(double *output, const double *input, size_t samples, double gain,
			      int muted, struct urp_native_repeat_workspace *workspace);

#endif
