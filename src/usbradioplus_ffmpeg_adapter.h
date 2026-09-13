/**
 * @file usbradioplus_ffmpeg_adapter.h
 * @brief Control-plane facade for the released FFmpeg graph adapter ABI.
 *
 * This facade intentionally has no channel, configuration, or direct FFmpeg
 * dependency.  It verifies the append-only exact-block ABI tail before it
 * creates an opaque normalized-F32 graph for native DCS shaping.
 */

#ifndef USBRADIOPLUS_FFMPEG_ADAPTER_H
#define USBRADIOPLUS_FFMPEG_ADAPTER_H

#include <stdint.h>

#include <rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result returned by one FFmpeg-adapter facade operation. */
enum usbradioplus_ffmpeg_adapter_result {
	/** Operation completed. */
	USBRADIOPLUS_FFMPEG_ADAPTER_OK = 0,
	/** A caller supplied an invalid pointer, rate, frame count, or description. */
	USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT = -1,
	/** The released adapter lacks the required exact-block ABI surface. */
	USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER = -2,
	/** The released adapter rejected graph construction or block processing. */
	USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR = -3,
};

/**
 * @brief One prepared opaque exact-block graph retained by a control plane.
 *
 * The object is zero-initialized, prepared outside the native callback, used
 * synchronously by one callback owner, then destroyed after that owner stops.
 */
struct usbradioplus_ffmpeg_adapter {
	/** Verified released shared-library descriptor. */
	const struct rptadv_ffmpeg_adapter_descriptor *descriptor;
	/** Opaque graph created through the verified descriptor. */
	struct rptadv_ffmpeg_graph *graph;
	/** Immutable graph rate in samples per second. */
	uint32_t sample_rate_hz;
	/** Maximum native callback frame count reserved by the graph. */
	uint32_t maximum_frame_count;
};

/**
 * @brief Verify that a descriptor supports the exact-block ABI-v1 extension.
 *
 * @param descriptor Candidate descriptor from a dynamically linked adapter.
 * @return A @ref usbradioplus_ffmpeg_adapter_result value.
 */
enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_validate(const struct rptadv_ffmpeg_adapter_descriptor *descriptor);

/**
 * @brief Create one prepared normalized-F32 exact-block graph.
 *
 * @param adapter Zero-initialized facade state to populate.
 * @param descriptor Previously linked adapter descriptor to validate.
 * @param filter_description FFmpeg graph between the adapter's implicit endpoints.
 * @param sample_rate_hz Immutable graph rate in samples per second.
 * @param maximum_frame_count Largest callback block reserved during setup.
 * @return A @ref usbradioplus_ffmpeg_adapter_result value.
 *
 * This is control-plane work.  It allocates only through the dynamically
 * linked adapter's create operation and never modifies a channel graph.
 */
enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare(struct usbradioplus_ffmpeg_adapter *adapter,
				    const struct rptadv_ffmpeg_adapter_descriptor *descriptor,
				    const char *filter_description, uint32_t sample_rate_hz,
				    uint32_t maximum_frame_count);

/**
 * @brief Prepare a graph using the descriptor exported by the linked shared object.
 *
 * @param adapter Zero-initialized facade state to populate.
 * @param filter_description FFmpeg graph between the adapter's implicit endpoints.
 * @param sample_rate_hz Immutable graph rate in samples per second.
 * @param maximum_frame_count Largest callback block reserved during setup.
 * @return A @ref usbradioplus_ffmpeg_adapter_result value.
 */
enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare_released(struct usbradioplus_ffmpeg_adapter *adapter,
					     const char *filter_description,
					     uint32_t sample_rate_hz, uint32_t maximum_frame_count);

/**
 * @brief Prepare the fixed DCS or turn-off-tone spectral shaper.
 * @param adapter Zero-initialized graph facade.
 * @param turnoff Nonzero selects unity-gain turn-off-tone shaping; zero selects NRZ.
 * @param sample_rate_hz Immutable native rate.
 * @param maximum_frame_count Largest callback frame count.
 * @return A @ref usbradioplus_ffmpeg_adapter_result value.
 *
 * The 250 Hz, twentieth-order crossover and calibrated NRZ gain match the
 * transmitter's existing response. Both graphs exchange normalized F32 PCM.
 */
enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare_dcs(struct usbradioplus_ffmpeg_adapter *adapter, int turnoff,
					uint32_t sample_rate_hz, uint32_t maximum_frame_count);

/**
 * @brief Process exactly one normalized-F32 native block through a prepared graph.
 *
 * @param adapter Prepared facade state.
 * @param input Normalized mono F32 input PCM.
 * @param frame_count Input and output frame count.
 * @param output Destination for exactly @p frame_count normalized F32 samples.
 * @return A @ref usbradioplus_ffmpeg_adapter_result value.
 *
 * This is the native callback boundary. It only forwards the already
 * prepared shared adapter's exact-block call; it has no direct FFmpeg API use.
 */
enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_process_block(struct usbradioplus_ffmpeg_adapter *adapter,
					  const float *input, uint32_t frame_count, float *output);

/**
 * @brief Destroy a prepared graph and clear its retained control-plane state.
 *
 * @param adapter Facade state to close; NULL is accepted.
 */
void usbradioplus_ffmpeg_adapter_close(struct usbradioplus_ffmpeg_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
