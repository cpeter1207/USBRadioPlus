/** @file
 * @brief Public radio-feedback descriptor capability contract.
 */
#include "usbradioplus_product.h"
#include <assert.h>
#include <stddef.h>

_Static_assert(sizeof(UrpRadioFeedback) == 112, "radio feedback prefix layout");
_Static_assert(offsetof(UrpRadioFeedback, request_id) == 32, "request identity offset");
_Static_assert(offsetof(UrpRadioFeedback, physical_duplex) == 88, "physical duplex offset");
_Static_assert(URP_AST_DIRECT_CALLBACKS_ABI_VERSION == 6, "PCM ABI remains unchanged");
_Static_assert(sizeof(UrpAstFeedbackBinding) == 40, "acknowledged option envelope layout");
_Static_assert(offsetof(UrpAstFeedbackBinding, accepted_abi_version) == 32,
	       "feedback option ack offset");
_Static_assert(URP_AST_OPTION_RADIO_FEEDBACK == 0x52504642, "RPFB option identity");

int main(void)
{
	const size_t existing =
		offsetof(UrpAstDescriptor, channel_processing_status_text) + sizeof(void *);
	assert(sizeof(UrpAstDescriptor) >= existing + 2 * sizeof(void *));
	assert(URP_AST_DESCRIPTOR_FEEDBACK_MIN_SIZE == sizeof(UrpAstDescriptor));
	assert(offsetof(UrpAstDescriptor, native_feedback_bind) == existing);
	assert(offsetof(UrpAstDescriptor, channel_feedback_bind) == existing + sizeof(void *));
	const UrpAstDescriptor *api = usbradioplus_product_descriptor_v1();
	assert(api != NULL && api->struct_size >= URP_AST_DESCRIPTOR_FEEDBACK_MIN_SIZE);
	assert(api->native_feedback_bind != NULL && api->channel_feedback_bind != NULL);
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
