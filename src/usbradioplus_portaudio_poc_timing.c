/**
 * @file
 * @brief USBRadioPlus PortAudio POC timing.
 *
 * Safe trailing-ABI timing query for the optional PortAudio proof of concept.
 */

#include "usbradioplus_portaudio_poc_timing.h"

#include <stddef.h>
#include <string.h>

/** @brief End byte required to read one append-only adapter descriptor member. */
#define URP_POC_DESCRIPTOR_MEMBER_END(member)                                                      \
	(offsetof(struct rptadv_audio_adapter_descriptor, member) +                                \
	 sizeof(((struct rptadv_audio_adapter_descriptor *)0)->member))

/** @brief Descriptor size through the append-only timing-query entry. */
#define URP_POC_STREAM_TIMING_DESCRIPTOR_SIZE URP_POC_DESCRIPTOR_MEMBER_END(stream_get_timing)

/** @brief Clear a caller-sized timing result without discarding its ABI size.
 * @param timing Writable timing result whose struct_size must be retained.
 */
static void portaudio_poc_clear_stream_timing(struct rptadv_audio_stream_timing *timing)
{
	const uint32_t struct_size = timing->struct_size;

	memset(timing, 0, sizeof(*timing));
	timing->struct_size = struct_size;
}

/** @brief Return whether an adapter exports the append-only timing-query entry.
 * @param adapter Candidate immutable adapter descriptor.
 * @return Nonzero when the descriptor contains a compatible timing entry.
 */
static int portaudio_poc_has_stream_timing(const struct rptadv_audio_adapter_descriptor *adapter)
{
	return adapter && adapter->abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION &&
	       adapter->struct_size >= URP_POC_STREAM_TIMING_DESCRIPTOR_SIZE &&
	       adapter->stream_get_timing;
}

enum rptadv_audio_result
usbradioplus_portaudio_poc_get_stream_timing(const struct rptadv_audio_adapter_descriptor *adapter,
					     const struct rptadv_audio_stream *stream,
					     struct rptadv_audio_stream_timing *timing)
{
	enum rptadv_audio_result result;

	if (!timing)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	portaudio_poc_clear_stream_timing(timing);
	if (timing->struct_size < sizeof(*timing) || !stream)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (!portaudio_poc_has_stream_timing(adapter))
		return RPTADV_AUDIO_UNSUPPORTED;
	result = adapter->stream_get_timing(stream, timing);
	if (result != RPTADV_AUDIO_OK || timing->abi_version != RPTADV_AUDIO_ADAPTER_ABI_VERSION) {
		portaudio_poc_clear_stream_timing(timing);
		return result == RPTADV_AUDIO_OK ? RPTADV_AUDIO_UNSUPPORTED : result;
	}
	return RPTADV_AUDIO_OK;
}
