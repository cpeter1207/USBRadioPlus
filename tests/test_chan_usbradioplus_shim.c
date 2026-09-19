/** @file
 * @brief Host-stub checks for the metadata-only Asterisk loader.
 */

#include "asterisk.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usbradioplus_asterisk.h"

static struct ast_module fake_module;
static struct ast_module_info_fixture fake_module_info = {.self = &fake_module};
struct ast_module_info_fixture *ast_module_info = &fake_module_info;

static int load_result;
static int reload_result;
static int unload_result;
static unsigned int load_calls;
static unsigned int reload_calls;
static unsigned int unload_calls;
static struct urp_ast_provider_manifest captured_providers;
static void *captured_module;
static unsigned int log_calls;
static int log_level;
static char log_message[128];

void ast_log(int level, const char *format, ...)
{
	va_list arguments;

	++log_calls;
	log_level = level;
	va_start(arguments, format);
	(void)vsnprintf(log_message, sizeof(log_message), format, arguments);
	va_end(arguments);
}

static int fake_load(const struct urp_ast_provider_manifest *providers, void *module_self)
{
	++load_calls;
	captured_providers = *providers;
	captured_module = module_self;
	return load_result;
}

static int fake_reload(void)
{
	++reload_calls;
	return reload_result;
}

static int fake_unload(void)
{
	++unload_calls;
	return unload_result;
}

static struct urp_asterisk_loader_descriptor fake_loader;
static const struct urp_asterisk_loader_descriptor *descriptor_override = &fake_loader;

const struct urp_asterisk_loader_descriptor *usbradioplus_asterisk_loader_descriptor(void)
{
	return descriptor_override;
}

static const uint8_t ffmpeg_provider;
static const uint8_t rnnoise_provider;
static const uint8_t ring_provider;
static const uint8_t radio_provider;
static const uint8_t samplerate_provider;
static const uint8_t audio_provider;
static const uint8_t gpio_provider;

const void *rptadv_ffmpeg_adapter_descriptor(void)
{
	return &ffmpeg_provider;
}

const void *rptadv_rnnoise_adapter_descriptor(void)
{
	return &rnnoise_provider;
}

const void *rpcr2_descriptor(void)
{
	return &ring_provider;
}

const void *rptadv_radio_descriptor(void)
{
	return &radio_provider;
}

const void *rptadv_samplerate_adapter_descriptor(void)
{
	return &samplerate_provider;
}

const void *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return &audio_provider;
}

const void *rptadv_gpio_adapter_descriptor(void)
{
	return &gpio_provider;
}

static void reset_fixture(void)
{
	static const char capability[] = "usbradioplus.asterisk-loader";

	memset(&captured_providers, 0, sizeof(captured_providers));
	captured_module = NULL;
	load_result = URP_AST_LOADER_OK;
	reload_result = 0;
	unload_result = 0;
	load_calls = 0;
	reload_calls = 0;
	unload_calls = 0;
	log_calls = 0;
	log_level = 0;
	log_message[0] = '\0';
	fake_loader = (struct urp_asterisk_loader_descriptor){
		.struct_size = sizeof(fake_loader),
		.abi_version = URP_AST_LOADER_ABI_VERSION,
		.capability = capability,
		.load = fake_load,
		.reload = fake_reload,
		.unload = fake_unload,
	};
	descriptor_override = &fake_loader;
	(void)ast_module_entry_points.unload();
}

static void test_valid_loader_receives_every_provider(void)
{
	reset_fixture();
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_SUCCESS);
	assert(load_calls == 1);
	assert(captured_module == &fake_module);
	assert(captured_providers.struct_size == sizeof(captured_providers));
	assert(captured_providers.abi_version == URP_AST_LOADER_ABI_VERSION);
	assert(captured_providers.ffmpeg == &ffmpeg_provider);
	assert(captured_providers.rnnoise == &rnnoise_provider);
	assert(captured_providers.ring == &ring_provider);
	assert(captured_providers.radio == &radio_provider);
	assert(captured_providers.samplerate == &samplerate_provider);
	assert(captured_providers.audio == &audio_provider);
	assert(captured_providers.gpio == &gpio_provider);
	assert(ast_module_entry_points.reload() == 0);
	assert(reload_calls == 1);
	assert(ast_module_entry_points.unload() == 0);
	assert(unload_calls == 1);
	assert(ast_module_entry_points.reload() == -1);
}

static void test_loader_failures_are_translated(void)
{
	reset_fixture();
	load_result = URP_AST_LOADER_DECLINE;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	assert(ast_module_entry_points.reload() == -1);

	load_result = URP_AST_LOADER_FAILURE;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_FAILURE);
	assert(ast_module_entry_points.reload() == -1);
}

static void test_invalid_descriptors_are_rejected_before_load(void)
{
	reset_fixture();
	descriptor_override = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	assert(log_calls == 1 && log_level == LOG_ERROR);
	assert(!strcmp(log_message, "USBRadioPlus Rust adapter ABI is unavailable\n"));

	reset_fixture();
	fake_loader.struct_size -= 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);

	reset_fixture();
	fake_loader.abi_version += 1;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);

	reset_fixture();
	fake_loader.capability = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	assert(load_calls == 0);

	reset_fixture();
	fake_loader.capability = "wrong";
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);

	reset_fixture();
	fake_loader.load = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);

	reset_fixture();
	fake_loader.reload = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);

	reset_fixture();
	fake_loader.unload = NULL;
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_DECLINE);
	assert(load_calls == 0);
}

static void test_failed_unload_keeps_forwarding_live(void)
{
	reset_fixture();
	assert(ast_module_entry_points.load() == AST_MODULE_LOAD_SUCCESS);
	unload_result = -1;
	assert(ast_module_entry_points.unload() == -1);
	assert(ast_module_entry_points.reload() == 0);
	assert(reload_calls == 1);
	unload_result = 0;
	assert(ast_module_entry_points.unload() == 0);
}

int main(void)
{
	test_valid_loader_receives_every_provider();
	test_loader_failures_are_translated();
	test_invalid_descriptors_are_rejected_before_load();
	test_failed_unload_keeps_forwarding_live();
	return 0;
}
