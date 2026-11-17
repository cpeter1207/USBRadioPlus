/** @file
 * @brief Public radio-feedback descriptor capability contract.
 */
#include "usbradioplus_product.h"
#include <assert.h>
#include <stddef.h>

_Static_assert(sizeof(UrpProgrammeBuffering) == 64, "programme buffering layout");
_Static_assert(offsetof(UrpRadioFeedback, programme_buffering) == 112,
	       "SDK33 physical feedback prefix is preserved");
_Static_assert(sizeof(UrpRadioFeedback) == 184, "optional programme tail layout");
_Static_assert(offsetof(UrpRadioFeedback, request_id) == 32, "request identity offset");
_Static_assert(offsetof(UrpRadioFeedback, physical_duplex) == 88, "physical duplex offset");
_Static_assert(URP_AST_DIRECT_CALLBACKS_ABI_VERSION == 6, "PCM ABI remains unchanged");
_Static_assert(sizeof(UrpAstFeedbackBinding) == 40, "acknowledged option envelope layout");
_Static_assert(offsetof(UrpAstFeedbackBinding, accepted_abi_version) == 32,
	       "feedback option ack offset");
_Static_assert(URP_AST_OPTION_RADIO_FEEDBACK == 0x52504642, "RPFB option identity");
_Static_assert(sizeof(UrpAstProgrammeRetirement) == 32, "RPPR scalar envelope layout");
_Static_assert(offsetof(UrpAstProgrammeRetirement, accepted_abi_version) == 24,
	       "RPPR dispatch acknowledgment offset");
_Static_assert(URP_AST_OPTION_PROGRAMME_RETIRE == 0x52505052, "RPPR option identity");

_Static_assert(sizeof(UrpProgrammeFenceBinding) == 32, "RPPF sized binding layout");
_Static_assert(offsetof(UrpProgrammeFenceBinding, accepted_abi_version) == 24,
	       "RPPF retention acknowledgment offset");
_Static_assert(URP_AST_OPTION_PROGRAMME_FENCE == 0x52505046, "RPPF option identity");
_Static_assert(URP_PROGRAMME_FENCE_ABI_VERSION == 1, "V1 source contract is unchanged");
_Static_assert(URP_PROGRAMME_FENCE_V2_ABI_VERSION == 2, "V2 is explicitly negotiated");
_Static_assert(sizeof(UrpProgrammeFenceBindingV2) == 40, "V2 binding extent");
_Static_assert(offsetof(UrpProgrammeFenceBindingV2, observed_permit) == 32,
	       "V2 callback follows unchanged V1 prefix");
_Static_assert(sizeof(UrpProgrammeObservationV1) == 112, "software observation extent");
_Static_assert(offsetof(UrpProgrammeObservationV1, radio_generation) == 8,
	       "actual physical generation offset");
_Static_assert(offsetof(UrpProgrammeObservationV1, frame_count) == 16,
	       "actual graph frame count offset");
_Static_assert(offsetof(UrpProgrammeObservationV1, current_retirement_request_id) == 24,
	       "coherent retirement authority offset");
_Static_assert(offsetof(UrpProgrammeObservationV1, programme_buffering) == 32,
	       "actual all-audible graph record offset");

_Static_assert(offsetof(UrpProgrammeObservationV1, clock_flags) == 96, "clock tail offset");
_Static_assert(offsetof(UrpProgrammeObservationV1, clock_domain) == 100, "clock kind offset");
_Static_assert(offsetof(UrpProgrammeObservationV1, observed_monotonic_ns) == 104,
	       "clock sample offset");
_Static_assert(URP_PROGRAMME_OBSERVATION_ABI_VERSION == 1, "observation ABI unchanged");

int main(void)
{
	const size_t existing =
		offsetof(UrpAstDescriptor, channel_processing_status_text) + sizeof(void *);
	assert(sizeof(UrpAstDescriptor) >= existing + 2 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_FEEDBACK_MIN_SIZE == existing + 2 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_PROGRAMME_BUFFERING_MIN_SIZE == existing + 3 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_PROGRAMME_RETIRE_MIN_SIZE == existing + 5 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_PROGRAMME_FENCE_MIN_SIZE == existing + 7 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_PROGRAMME_FENCE_MIN_SIZE == sizeof(UrpAstDescriptor));
	assert(offsetof(UrpAstDescriptor, native_feedback_bind) == existing);
	assert(offsetof(UrpAstDescriptor, channel_feedback_bind) == existing + sizeof(void *));
	const UrpAstDescriptor *api = usbradioplus_product_descriptor_v1();
	assert(api != NULL && api->struct_size >= URP_AST_DESCRIPTOR_FEEDBACK_MIN_SIZE);
	assert(api->native_feedback_bind != NULL && api->channel_feedback_bind != NULL);
	assert(api->link_programme_buffering != NULL);
	assert(api->native_programme_retire != NULL && api->channel_programme_retire != NULL);
	assert(api->struct_size >= URP_AST_DESCRIPTOR_PROGRAMME_FENCE_MIN_SIZE);
	assert(api->native_programme_fence_bind != NULL &&
	       api->channel_programme_fence_bind != NULL);
	UrpProgrammeFenceBinding fence = {
		.struct_size = sizeof(fence),
		.abi_version = URP_PROGRAMME_FENCE_ABI_VERSION,
	};
	assert(api->native_programme_fence_bind(NULL, &fence) == URP_AST_INVALID_ARGUMENT);
	assert(api->channel_programme_fence_bind(NULL, &fence) == URP_AST_INVALID_ARGUMENT);
	assert(fence.accepted_abi_version == 0);
	UrpProgrammeFenceBindingV2 observed = {
		.base = {.struct_size = sizeof(observed),
			 .abi_version = URP_PROGRAMME_FENCE_V2_ABI_VERSION},
	};
	assert(api->native_programme_fence_bind(NULL, &observed.base) == URP_AST_INVALID_ARGUMENT);
	assert(api->channel_programme_fence_bind(NULL, &observed.base) == URP_AST_INVALID_ARGUMENT);
	assert(observed.base.accepted_abi_version == 0);

	UrpProgrammeBuffering buffering = {.struct_size = sizeof(buffering), .abi_version = 1};
	assert(api->link_programme_buffering(NULL, &buffering) == URP_AST_INVALID_ARGUMENT);
	assert(buffering.flags == 0 && buffering.struct_size == sizeof(buffering));
	assert(api->native_programme_retire(NULL, 1, 1) == URP_AST_INVALID_ARGUMENT);
	assert(api->channel_programme_retire(NULL, 1, 1) == URP_AST_INVALID_ARGUMENT);
	UrpAstFeedbackBinding option = {
		.struct_size = sizeof(UrpAstFeedbackBinding),
		.abi_version = URP_AST_FEEDBACK_OPTION_ABI_VERSION,
		.port = {.struct_size = sizeof(UrpRadioFeedbackPort), .abi_version = 1},
	};
	assert(option.accepted_abi_version == 0 && option.reserved == 0);
	assert(api->native_feedback_bind(NULL, &option.port) == URP_AST_INVALID_ARGUMENT);
	assert(api->channel_feedback_bind(NULL, &option.port) == URP_AST_INVALID_ARGUMENT);
	assert(option.accepted_abi_version == 0);
	return 0;
}
