/**
 * @file test_portaudio_poc_timing.c
 * @brief Hardware-free tests for optional PortAudio POC timing observability.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#include "usbradioplus_portaudio_poc_timing.h"

/** @brief Number of calls received by the fake appended timing entry. */
static unsigned int timing_calls;
/** @brief Result selected for the fake appended timing entry. */
static enum rptadv_audio_result timing_result;
/** @brief Stream argument retained by the fake appended timing entry. */
static const struct rptadv_audio_stream *timing_stream;
/** @brief ABI version returned by an otherwise successful fake timing query. */
static uint32_t timing_abi_version;

/** @brief Return synthetic immutable stream timing without opening PortAudio. */
static enum rptadv_audio_result fake_stream_get_timing(const struct rptadv_audio_stream *stream,
						       struct rptadv_audio_stream_timing *timing)
{
	timing_calls++;
	timing_stream = stream;
	assert(timing);
	if (timing_result != RPTADV_AUDIO_OK) {
		timing->abi_version = 99U;
		timing->input_latency_seconds = 1.0;
		timing->output_latency_seconds = 2.0;
		timing->sample_rate_hz = 3.0;
		return timing_result;
	}
	assert(timing->struct_size == sizeof(*timing));
	timing->abi_version = timing_abi_version;
	timing->input_latency_seconds = 0.008;
	timing->output_latency_seconds = 0.012;
	timing->sample_rate_hz = 47999.5;
	return RPTADV_AUDIO_OK;
}

/** @brief Reset the fake trailing ABI behavior before one independent case. */
static void reset_fake_timing(void)
{
	timing_calls = 0U;
	timing_result = RPTADV_AUDIO_OK;
	timing_stream = NULL;
	timing_abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
}

/** @brief Return a descriptor complete through the appended timing-query entry. */
static struct rptadv_audio_adapter_descriptor complete_descriptor(void)
{
	struct rptadv_audio_adapter_descriptor descriptor = {
		.struct_size = sizeof(descriptor),
		.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
		.stream_get_timing = fake_stream_get_timing,
	};

	return descriptor;
}

/** @brief Verify that an available appended entry returns the actual timing snapshot. */
static void test_available_timing(void)
{
	const struct rptadv_audio_stream *const stream =
		(const struct rptadv_audio_stream *)(uintptr_t)1U;
	const struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct rptadv_audio_stream_timing timing = {
		.struct_size = sizeof(timing),
	};

	reset_fake_timing();
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_OK);
	assert(timing_calls == 1U);
	assert(timing_stream == stream);
	assert(timing.abi_version == RPTADV_AUDIO_ADAPTER_ABI_VERSION);
	assert(timing.input_latency_seconds == 0.008);
	assert(timing.output_latency_seconds == 0.012);
	assert(timing.sample_rate_hz == 47999.5);
}

/** @brief Verify unavailable and failed timing queries fail closed without stale data. */
static void test_unavailable_and_failed_timing(void)
{
	const struct rptadv_audio_stream *const stream =
		(const struct rptadv_audio_stream *)(uintptr_t)1U;
	struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct rptadv_audio_stream_timing timing = {
		.struct_size = sizeof(timing),
		.abi_version = 99U,
		.input_latency_seconds = 1.0,
		.output_latency_seconds = 2.0,
		.sample_rate_hz = 3.0,
	};

	reset_fake_timing();
	descriptor.struct_size =
		offsetof(struct rptadv_audio_adapter_descriptor, stream_get_timing);
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	assert(!timing_calls);
	assert(timing.struct_size == sizeof(timing));
	assert(!timing.abi_version && !timing.input_latency_seconds &&
	       !timing.output_latency_seconds && !timing.sample_rate_hz);

	timing.abi_version = 99U;
	descriptor = complete_descriptor();
	descriptor.abi_version++;
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	assert(!timing_calls);
	assert(!timing.abi_version);

	timing.abi_version = 99U;
	descriptor = complete_descriptor();
	descriptor.stream_get_timing = NULL;
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	assert(!timing_calls);
	assert(!timing.abi_version);

	timing.abi_version = 99U;
	assert(usbradioplus_portaudio_poc_get_stream_timing(NULL, stream, &timing) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	assert(!timing_calls);
	assert(!timing.abi_version);

	timing.abi_version = 99U;
	descriptor = complete_descriptor();
	timing_abi_version = 99U;
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_UNSUPPORTED);
	assert(timing_calls == 1U);
	assert(!timing.abi_version && !timing.input_latency_seconds &&
	       !timing.output_latency_seconds && !timing.sample_rate_hz);

	timing.abi_version = 99U;
	descriptor = complete_descriptor();
	timing_result = RPTADV_AUDIO_PORTAUDIO_ERROR;
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_PORTAUDIO_ERROR);
	assert(timing_calls == 2U);
	assert(!timing.abi_version && !timing.input_latency_seconds &&
	       !timing.output_latency_seconds && !timing.sample_rate_hz);
}

/** @brief Verify invalid caller arguments do not invoke the adapter. */
static void test_invalid_arguments(void)
{
	const struct rptadv_audio_stream *const stream =
		(const struct rptadv_audio_stream *)(uintptr_t)1U;
	const struct rptadv_audio_adapter_descriptor descriptor = complete_descriptor();
	struct rptadv_audio_stream_timing timing = {
		.struct_size = 0U,
	};

	reset_fake_timing();
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, NULL) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, stream, &timing) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(!timing_calls);
	timing.struct_size = sizeof(timing);
	assert(usbradioplus_portaudio_poc_get_stream_timing(&descriptor, NULL, &timing) ==
	       RPTADV_AUDIO_INVALID_ARGUMENT);
	assert(!timing_calls);
}

int main(void)
{
	test_available_timing();
	test_unavailable_and_failed_timing();
	test_invalid_arguments();
	return 0;
}
