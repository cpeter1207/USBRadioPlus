/**
 * @file usbradioplus_samplerate_adapter.h
 * @brief Narrow lifecycle bridge to the released libsamplerate adapter ABI.
 *
 * The bridge retains USBRadioPlus ownership of callback workspaces while the
 * versioned shared object owns persistent libsamplerate state.  Its PCM
 * contract is canonical normalized F32; S16 conversion remains in the legacy
 * Asterisk compatibility boundary until that boundary migrates.
 */

#ifndef USBRADIOPLUS_SAMPLERATE_ADAPTER_H
#define USBRADIOPLUS_SAMPLERATE_ADAPTER_H

#include <stdint.h>

#include <rptadv_samplerate_adapter/rptadv_samplerate_adapter.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result returned by one sample-rate-adapter bridge operation. */
enum usbradioplus_samplerate_adapter_result {
	/** Operation completed. */
	USBRADIOPLUS_SAMPLERATE_ADAPTER_OK = 0,
	/** A caller supplied invalid state, PCM, or frame information. */
	USBRADIOPLUS_SAMPLERATE_ADAPTER_INVALID_ARGUMENT = -1,
	/** The selected shared object does not expose the required ABI-v1 prefix. */
	USBRADIOPLUS_SAMPLERATE_ADAPTER_INCOMPATIBLE_ADAPTER = -2,
	/** The selected converter could not be created, reset, or process PCM. */
	USBRADIOPLUS_SAMPLERATE_ADAPTER_CONVERTER_ERROR = -3,
};

/**
 * @brief One prepared persistent mono F32 converter.
 *
 * Setup and close run only on the control plane.  A single native callback
 * owner calls @ref usbradioplus_samplerate_adapter_process synchronously after
 * setup, so no bridge-side synchronization is required.
 */
struct usbradioplus_samplerate_adapter {
	/** Immutable validated descriptor from the released shared object. */
	const struct rptadv_samplerate_adapter_descriptor *descriptor;
	/** Opaque converter state owned by @ref descriptor. */
	struct rptadv_samplerate_converter *converter;
};

/**
 * @brief Verify the complete ABI-v1 descriptor required by this bridge.
 * @param descriptor Candidate descriptor from a linked shared object.
 * @return A @ref usbradioplus_samplerate_adapter_result value.
 */
enum usbradioplus_samplerate_adapter_result usbradioplus_samplerate_adapter_validate(
	const struct rptadv_samplerate_adapter_descriptor *descriptor);

/**
 * @brief Create one persistent mono sinc converter through a validated descriptor.
 * @param adapter Zero-initialized bridge state to populate.
 * @param descriptor Candidate released descriptor to validate.
 * @param quality One supported shared-library conversion-quality value.
 * @return A @ref usbradioplus_samplerate_adapter_result value.
 *
 * This control-plane operation allocates only through the selected shared
 * object.  It leaves @p adapter clear on every failure.
 */
enum usbradioplus_samplerate_adapter_result usbradioplus_samplerate_adapter_prepare(
	struct usbradioplus_samplerate_adapter *adapter,
	const struct rptadv_samplerate_adapter_descriptor *descriptor,
	enum rptadv_samplerate_quality quality);

/**
 * @brief Prepare a converter from the descriptor exported by the linked shared object.
 * @param adapter Zero-initialized bridge state to populate.
 * @param quality One supported shared-library conversion-quality value.
 * @return A @ref usbradioplus_samplerate_adapter_result value.
 */
enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_prepare_released(struct usbradioplus_samplerate_adapter *adapter,
						 enum rptadv_samplerate_quality quality);

/**
 * @brief Clear persistent sinc history at a source discontinuity.
 * @param adapter Prepared bridge state.
 * @return A @ref usbradioplus_samplerate_adapter_result value.
 */
enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_reset(struct usbradioplus_samplerate_adapter *adapter);

/**
 * @brief Convert one bounded canonical-F32 mono PCM block.
 * @param adapter Prepared bridge state.
 * @param input Readable normalized F32 input, required when @p input_frames is nonzero.
 * @param input_frames Input mono frames.
 * @param output Writable normalized F32 output, required when @p output_capacity is nonzero.
 * @param output_capacity Available output mono frames.
 * @param ratio Requested output/input rate ratio.
 * @param input_used Receives consumed input frames.
 * @param output_generated Receives generated output frames.
 * @return A @ref usbradioplus_samplerate_adapter_result value.
 *
 * This is the bounded real-time bridge operation.  It only forwards prebuilt
 * caller buffers to the selected descriptor and never allocates or locks.
 */
enum usbradioplus_samplerate_adapter_result
usbradioplus_samplerate_adapter_process(struct usbradioplus_samplerate_adapter *adapter,
					const float *input, uint32_t input_frames, float *output,
					uint32_t output_capacity, double ratio,
					uint32_t *input_used, uint32_t *output_generated);

/**
 * @brief Destroy a prepared converter and clear bridge state.
 * @param adapter Bridge state to close; NULL is accepted.
 */
void usbradioplus_samplerate_adapter_close(struct usbradioplus_samplerate_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
