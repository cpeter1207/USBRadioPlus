/**
 * @file
 * @brief USBRadioPlus PortAudio POC timing API.
 *
 * Control-plane stream-timing query for the optional PortAudio proof of concept.
 */

#ifndef USBRADIOPLUS_PORTAUDIO_POC_TIMING_H
#define USBRADIOPLUS_PORTAUDIO_POC_TIMING_H

#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

/**
 * @brief Read immutable actual timing from one optional POC stream.
 * @param adapter Released PortAudio/ALSA adapter descriptor.
 * @param stream Successfully opened direct PortAudio stream.
 * @param timing Caller-sized destination for the adapter timing snapshot.
 * @return A \c rptadv_audio_result value.
 *
 * This helper checks that the appended timing ABI entry is present before it
 * calls it.  It is strictly control-plane code: callers must not invoke it
 * from the PortAudio callback.  On every failure the destination retains its
 * supplied @c struct_size and all other fields are cleared, so an unavailable
 * newer adapter capability cannot leave stale diagnostics behind.
 */
enum rptadv_audio_result
usbradioplus_portaudio_poc_get_stream_timing(const struct rptadv_audio_adapter_descriptor *adapter,
					     const struct rptadv_audio_stream *stream,
					     struct rptadv_audio_stream_timing *timing);

#endif
