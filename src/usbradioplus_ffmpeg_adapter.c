/**
 * @file usbradioplus_ffmpeg_adapter.c
 * @brief Control-plane validation and lifecycle forwarding for FFmpeg graphs.
 */

#include "usbradioplus_ffmpeg_adapter.h"

#include <string.h>

/** @brief Return whether a descriptor safely exposes the exact-block ABI tail.
 * @param descriptor Candidate shared-library function table.
 * @return Nonzero when every required operation is available.
 */
static int
ffmpeg_adapter_descriptor_valid(const struct rptadv_ffmpeg_adapter_descriptor *descriptor)
{
	return descriptor &&
	       descriptor->struct_size >=
		       RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE &&
	       descriptor->abi_version == RPTADV_FFMPEG_ADAPTER_ABI_VERSION &&
	       descriptor->capability_name &&
	       !strcmp(descriptor->capability_name, RPTADV_FFMPEG_ADAPTER_CAPABILITY) &&
	       descriptor->create && descriptor->destroy && descriptor->process_block;
}

enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_validate(const struct rptadv_ffmpeg_adapter_descriptor *descriptor)
{
	return ffmpeg_adapter_descriptor_valid(descriptor)
		       ? USBRADIOPLUS_FFMPEG_ADAPTER_OK
		       : USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER;
}

enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare(struct usbradioplus_ffmpeg_adapter *adapter,
				    const struct rptadv_ffmpeg_adapter_descriptor *descriptor,
				    const char *filter_description, uint32_t sample_rate_hz,
				    uint32_t maximum_frame_count)
{
	struct rptadv_ffmpeg_graph_config config;
	struct rptadv_ffmpeg_graph *graph = NULL;

	if (!adapter || adapter->graph || !filter_description || !*filter_description ||
	    sample_rate_hz == 0U || maximum_frame_count == 0U)
		return USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT;
	if (usbradioplus_ffmpeg_adapter_validate(descriptor) != USBRADIOPLUS_FFMPEG_ADAPTER_OK)
		return USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER;
	memset(&config, 0, sizeof(config));
	config.struct_size = sizeof(config);
	config.abi_version = RPTADV_FFMPEG_ADAPTER_ABI_VERSION;
	config.sample_rate_hz = sample_rate_hz;
	config.maximum_frame_count = maximum_frame_count;
	config.filter_description = filter_description;
	if (descriptor->create(&config, &graph) != RPTADV_FFMPEG_ADAPTER_OK || !graph) {
		if (graph)
			descriptor->destroy(graph);
		return USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR;
	}
	adapter->descriptor = descriptor;
	adapter->graph = graph;
	adapter->sample_rate_hz = sample_rate_hz;
	adapter->maximum_frame_count = maximum_frame_count;
	return USBRADIOPLUS_FFMPEG_ADAPTER_OK;
}

enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare_released(struct usbradioplus_ffmpeg_adapter *adapter,
					     const char *filter_description,
					     uint32_t sample_rate_hz, uint32_t maximum_frame_count)
{
	return usbradioplus_ffmpeg_adapter_prepare(adapter, rptadv_ffmpeg_adapter_descriptor(),
						   filter_description, sample_rate_hz,
						   maximum_frame_count);
}

enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_prepare_dcs(struct usbradioplus_ffmpeg_adapter *adapter, int turnoff,
					uint32_t sample_rate_hz, uint32_t maximum_frame_count)
{
	/* Match the existing crossover precision and calibrated peak response.
	 * NRZ needs -3.42 dB compensation; the EOT sine does not. */
	const char *description =
		turnoff ? "acrossover=split=250:order=20th:precision=float[low][high];"
			  "[high]anullsink;[low]aformat=sample_fmts=flt"
			: "acrossover=split=250:order=20th:precision=float[low][high];"
			  "[high]anullsink;[low]volume=-3.42dB,aformat=sample_fmts=flt";

	return usbradioplus_ffmpeg_adapter_prepare_released(adapter, description, sample_rate_hz,
							    maximum_frame_count);
}

enum usbradioplus_ffmpeg_adapter_result
usbradioplus_ffmpeg_adapter_process_block(struct usbradioplus_ffmpeg_adapter *adapter,
					  const float *input, uint32_t frame_count, float *output)
{
	if (!adapter || !adapter->descriptor || !adapter->graph || !input || !output ||
	    frame_count == 0U || frame_count > adapter->maximum_frame_count)
		return USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT;
	if (adapter->descriptor->process_block(adapter->graph, input, frame_count, output) !=
	    RPTADV_FFMPEG_ADAPTER_OK)
		return USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR;
	return USBRADIOPLUS_FFMPEG_ADAPTER_OK;
}

void usbradioplus_ffmpeg_adapter_close(struct usbradioplus_ffmpeg_adapter *adapter)
{
	if (!adapter)
		return;
	if (adapter->descriptor && adapter->graph)
		adapter->descriptor->destroy(adapter->graph);
	memset(adapter, 0, sizeof(*adapter));
}
