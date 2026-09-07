/** @file
 * @brief Thin native-rate channel adapter retaining the existing hardware implementation.
 */
#include "asterisk.h"

#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/format.h"
#include "asterisk/format_cache.h"
#include "asterisk/format_cap.h"
#include "usbradioplus_rpt_advanced.h"

/** Hardware lifecycle and callbacks, owned by this module. */
static const struct ast_channel_tech *hardware;
static struct ast_channel *request(const char *type, struct ast_format_cap *cap,
				   const struct ast_assigned_ids *assignedids,
				   const struct ast_channel *requestor, const char *data,
				   int *cause);

/** @brief Start the reserved hardware.
 * @param channel Owned native channel.
 * @param destination Requested destination.
 * @param timeout Call timeout.
 * @return Backend call status.
 */
static int advanced_call(struct ast_channel *channel, const char *destination, int timeout)
{
	return hardware->call(channel, destination, timeout);
}

/** @brief Release the reserved hardware.
 * @param channel Owned native channel.
 * @return Backend hangup status.
 */
static int advanced_hangup(struct ast_channel *channel)
{
	return hardware->hangup(channel);
}

/** @brief Read a hardware-paced native frame.
 * @param channel Owned native channel.
 * @return Backend frame, including squelched silence.
 */
static struct ast_frame *advanced_read(struct ast_channel *channel)
{
	return hardware->read(channel);
}

/** @brief Queue controller audio for the next hardware tick.
 * @param channel Owned native channel.
 * @param frame Native PCM frame.
 * @return Backend write status.
 */
static int advanced_write(struct ast_channel *channel, struct ast_frame *frame)
{
	return hardware->write(channel, frame);
}

/** @brief Apply controller PTT and radio indications.
 * @param channel Owned native channel.
 * @param condition Asterisk control indication.
 * @param data Optional indication payload.
 * @param length Payload bytes.
 * @return Backend indication status.
 */
static int advanced_indicate(struct ast_channel *channel, int condition, const void *data,
			     size_t length)
{
	return hardware->indicate(channel, condition, data, length);
}

/** Separate controller-facing technology; its capabilities own their format references. */
static struct ast_channel_tech advanced = {
	.type = "RadioPlusAdvanced",
	.description = "USBRadioPlus hardware-clocked native PCM",
	.requester = request,
	.call = advanced_call,
	.hangup = advanced_hangup,
	.read = advanced_read,
	.write = advanced_write,
	.indicate = advanced_indicate,
};
/** Borrowed native PCM format from Asterisk's cache. */
static struct ast_format *native_format;
/** Select shared-engine native controller routing before the channel starts. */
static void (*configure_native)(struct ast_channel *channel);

/** @brief Request exclusive hardware ownership and expose native-rate PCM.
 * @param type Requested technology name.
 * @param cap Caller-supported formats.
 * @param assignedids Assigned channel identifiers.
 * @param requestor Requesting Asterisk channel, if any.
 * @param data Configured USBRadioPlus channel name.
 * @param cause Receives the backend's failure cause.
 * @return New native interface channel, or null on failure.
 */
static struct ast_channel *request(const char *type, struct ast_format_cap *cap,
				   const struct ast_assigned_ids *assignedids,
				   const struct ast_channel *requestor, const char *data,
				   int *cause)
{
	(void)type;
	if (!ast_format_cap_iscompatible(cap, advanced.capabilities))
		return NULL;
	/* The hardware backend reserves its configured device using its app_rpt
	 * interface. No call or audio worker starts until native mode is selected. */
	struct ast_channel *channel = hardware->requester(hardware->type, hardware->capabilities,
							  assignedids, requestor, data, cause);
	if (!channel)
		return NULL;
	configure_native(channel);
	ast_channel_tech_set(channel, &advanced);
	ast_channel_nativeformats_set(channel, advanced.capabilities);
	if (ast_set_read_format(channel, native_format) ||
	    ast_set_write_format(channel, native_format)) {
		ast_hangup(channel);
		return NULL;
	}
	return channel;
}

int usbradioplus_advanced_register(const struct ast_channel_tech *backend, unsigned int native_rate,
				   void (*configure)(struct ast_channel *channel))
{
	native_format = ast_format_cache_get_slin_by_rate(native_rate);
	if (ast_format_get_sample_rate(native_format) != native_rate)
		return -1;
	hardware = backend;
	configure_native = configure;
	advanced.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!advanced.capabilities)
		return -1;
	if (ast_format_cap_append(advanced.capabilities, native_format, 0) ||
	    ast_channel_register(&advanced)) {
		ao2_cleanup(advanced.capabilities);
		advanced.capabilities = NULL;
		return -1;
	}
	return 0;
}

void usbradioplus_advanced_unregister(void)
{
	if (advanced.capabilities) {
		ast_channel_unregister(&advanced);
		ao2_cleanup(advanced.capabilities);
		advanced.capabilities = NULL;
	}
}
