/**
 * @file test_ffmpeg_adapter_facade.c
 * @brief Focused ABI and lifecycle tests for the isolated FFmpeg facade.
 */

#include <assert.h>
#include <math.h>

#include "usbradioplus_ffmpeg_adapter.h"

/** @brief Return whether two normalized F32 PCM samples are sufficiently equal. */
static int approximately_equal(float left, float right)
{
	return fabsf(left - right) < 0.0001F;
}

/** @brief Private backing state for the opaque graph token used by test fakes. */
struct fake_graph_state {
	/** Number of fake callback operations performed through this token. */
	unsigned int callback_count;
};

/** @brief Opaque non-null graph state used by descriptor validation fakes. */
static struct fake_graph_state fake_graph_state;
/** @brief Number of fake descriptor destroy calls observed by the test. */
static unsigned int fake_destroy_calls;
/** Simulate an incompatible provider returning success with no graph. */
static int fake_empty_graph;

/** @brief Return the stable opaque graph token used by this test. */
static struct rptadv_ffmpeg_graph *fake_graph(void)
{
	return (struct rptadv_ffmpeg_graph *)(void *)&fake_graph_state;
}

/** @brief Simulate successful control-plane graph creation. */
static enum rptadv_ffmpeg_adapter_result
fake_create_ok(const struct rptadv_ffmpeg_graph_config *config, struct rptadv_ffmpeg_graph **graph)
{
	assert(config != NULL);
	assert(graph != NULL);
	*graph = fake_empty_graph ? NULL : fake_graph();
	return RPTADV_FFMPEG_ADAPTER_OK;
}

/** @brief Simulate an adapter failure that returns no graph. */
static enum rptadv_ffmpeg_adapter_result
fake_create_fail(const struct rptadv_ffmpeg_graph_config *config,
		 struct rptadv_ffmpeg_graph **graph)
{
	assert(config != NULL);
	assert(graph != NULL);
	*graph = NULL;
	return RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR;
}

/** @brief Simulate a malformed adapter failure retaining a graph that needs cleanup. */
static enum rptadv_ffmpeg_adapter_result
fake_create_fail_with_graph(const struct rptadv_ffmpeg_graph_config *config,
			    struct rptadv_ffmpeg_graph **graph)
{
	assert(config != NULL);
	assert(graph != NULL);
	*graph = fake_graph();
	return RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR;
}

/** @brief Count graph destruction without dereferencing the opaque graph token. */
static void fake_destroy(struct rptadv_ffmpeg_graph *graph)
{
	assert(graph == fake_graph());
	++((struct fake_graph_state *)(void *)graph)->callback_count;
	++fake_destroy_calls;
}

/** @brief Simulate an exact-block adapter error. */
static enum rptadv_ffmpeg_adapter_result fake_process_fail(struct rptadv_ffmpeg_graph *graph,
							   const float *input, uint32_t frame_count,
							   float *output)
{
	assert(graph == fake_graph());
	assert(input != NULL);
	assert(frame_count != 0U);
	assert(output != NULL);
	++((struct fake_graph_state *)(void *)graph)->callback_count;
	output[0] = input[0];
	return RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR;
}

/** @brief Return one complete fake descriptor with a selectable create operation. */
static struct rptadv_ffmpeg_adapter_descriptor fake_descriptor(enum rptadv_ffmpeg_adapter_result (
	*create)(const struct rptadv_ffmpeg_graph_config *, struct rptadv_ffmpeg_graph **))
{
	return (struct rptadv_ffmpeg_adapter_descriptor){
		.struct_size = sizeof(struct rptadv_ffmpeg_adapter_descriptor),
		.abi_version = RPTADV_FFMPEG_ADAPTER_ABI_VERSION,
		.capability_name = RPTADV_FFMPEG_ADAPTER_CAPABILITY,
		.create = create,
		.destroy = fake_destroy,
		.process_block = fake_process_fail,
	};
}

/** @brief Verify descriptor-tail validation rejects a provider without process_block. */
static void test_descriptor_validation(void)
{
	struct rptadv_ffmpeg_adapter_descriptor incomplete = {
		.struct_size = RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_MIN_SIZE,
		.abi_version = RPTADV_FFMPEG_ADAPTER_ABI_VERSION,
		.capability_name = RPTADV_FFMPEG_ADAPTER_CAPABILITY,
	};
	const struct rptadv_ffmpeg_adapter_descriptor *released =
		rptadv_ffmpeg_adapter_descriptor();

	assert(released != NULL);
	assert(usbradioplus_ffmpeg_adapter_validate(released) == USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	assert(usbradioplus_ffmpeg_adapter_validate(NULL) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_ffmpeg_adapter_validate(&incomplete) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
}

/** @brief Verify every required exact-block descriptor member is checked. */
static void test_descriptor_member_validation(void)
{
	struct rptadv_ffmpeg_adapter_descriptor descriptor = fake_descriptor(fake_create_ok);

	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) == USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	descriptor.abi_version += 1U;
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor(fake_create_ok);
	descriptor.capability_name = NULL;
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor(fake_create_ok);
	descriptor.capability_name = "wrong";
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor(fake_create_ok);
	descriptor.create = NULL;
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor(fake_create_ok);
	descriptor.destroy = NULL;
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	descriptor = fake_descriptor(fake_create_ok);
	descriptor.process_block = NULL;
	assert(usbradioplus_ffmpeg_adapter_validate(&descriptor) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
}

/** @brief Verify local validation and cleanup of prepared facade state. */
static void test_facade_error_lifecycle(void)
{
	struct usbradioplus_ffmpeg_adapter adapter = {0};
	struct rptadv_ffmpeg_adapter_descriptor descriptor = fake_descriptor(fake_create_ok);
	float sample = 0.0F;

	assert(usbradioplus_ffmpeg_adapter_prepare(NULL, &descriptor, "anull", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, NULL, 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 0U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000U, 0U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	descriptor = fake_descriptor(fake_create_fail);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR);
	descriptor = fake_descriptor(fake_create_fail_with_graph);
	fake_destroy_calls = 0U;
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR);
	assert(fake_destroy_calls == 1U);
	descriptor = fake_descriptor(fake_create_ok);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_process_block(NULL, &sample, 1U, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, &sample, 0U, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, &sample, 1U, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR);
	usbradioplus_ffmpeg_adapter_close(NULL);
	fake_destroy_calls = 0U;
	usbradioplus_ffmpeg_adapter_close(&adapter);
	assert(fake_destroy_calls == 1U);
	assert(adapter.graph == NULL);
	assert(adapter.descriptor == NULL);
}

/** @brief Verify the facade forwards one prepared exact F32 block without FFmpeg headers. */
static void test_prepared_exact_block_lifecycle(void)
{
	struct usbradioplus_ffmpeg_adapter adapter = {0};
	const float input[8] = {-1.0F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
	float output[8] = {0};

	assert(usbradioplus_ffmpeg_adapter_prepare_released(&adapter, "volume=0.5", 48000U, 8U) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	assert(adapter.graph != NULL);
	assert(adapter.sample_rate_hz == 48000U);
	assert(adapter.maximum_frame_count == 8U);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, input, 8U, output) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	assert(approximately_equal(output[0], -0.5F));
	assert(approximately_equal(output[7], 0.5F));
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, input, 9U, output) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_ffmpeg_adapter_close(&adapter);
	assert(adapter.graph == NULL);
	assert(adapter.descriptor == NULL);
}

/** @brief Execute focused FFmpeg-adapter facade tests. */
int main(void)
{
	struct rptadv_ffmpeg_adapter_descriptor descriptor = fake_descriptor(fake_create_ok);
	struct usbradioplus_ffmpeg_adapter adapter = {0};
	float sample = 0.0F;
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, NULL, "anull", 48000, 1) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INCOMPATIBLE_ADAPTER);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, &sample, 1, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_ffmpeg_adapter_close(&adapter);
	adapter.descriptor = &descriptor;
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, &sample, 1, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_ffmpeg_adapter_close(&adapter);
	fake_empty_graph = 1;
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000, 1) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_GRAPH_ERROR);
	fake_empty_graph = 0;
	assert(usbradioplus_ffmpeg_adapter_prepare(&adapter, &descriptor, "anull", 48000, 1) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_OK);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, NULL, 1, &sample) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_ffmpeg_adapter_process_block(&adapter, &sample, 1, NULL) ==
	       USBRADIOPLUS_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_ffmpeg_adapter_close(&adapter);
	test_descriptor_validation();
	test_descriptor_member_validation();
	test_facade_error_lifecycle();
	test_prepared_exact_block_lifecycle();
	return 0;
}
