/*
 * USBRadioPlus -- Asterisk module metadata and Rust lifecycle loader
 *
 * Copyright (C) 2026 USBRadioPlus contributors
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2. See COPYING for details.
 */

/**
 * @file
 * @brief Compose released providers and forward Asterisk module lifecycle.
 */

#include "asterisk.h"

#include <stddef.h>
#include <string.h>

#include <rate_adjusting_pcm_ring3/rate_adjusting_pcm_ring3.h>
#include <rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h>
#include <rptadv_gpio_adapter.h>
#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>
#include <rptadv_rnnoise_adapter/rptadv_rnnoise_adapter.h>
#include <rptadv_samplerate_adapter/rptadv_samplerate_adapter.h>
#include <rptadvradio/rptadvradio.h>

#include "asterisk/logger.h"
#include "asterisk/module.h"

#include "usbradioplus_asterisk.h"

/** Capability name required from the co-packaged Rust host. */
#define URP_AST_LOADER_CAPABILITY "usbradioplus.asterisk-loader"

/** Validated process-lifetime Rust loader. */
static const struct urp_asterisk_loader_descriptor *rust_loader;

/**
 * @brief Validate the complete lifecycle descriptor before starting Rust.
 * @param loader Candidate descriptor from the co-packaged shared object.
 * @return Nonzero only for the expected ABI and complete function table.
 */
static int urp_loader_valid(const struct urp_asterisk_loader_descriptor *loader)
{
	return loader && loader->struct_size >= sizeof(*loader) &&
	       loader->abi_version == URP_AST_LOADER_ABI_VERSION && loader->capability &&
	       !strcmp(loader->capability, URP_AST_LOADER_CAPABILITY) && loader->load &&
	       loader->reload && loader->unload;
}

/**
 * @brief Load the complete Rust-owned USBRadioPlus Asterisk host.
 * @return An Asterisk module-load result.
 */
static int load_module(void)
{
	const struct urp_ast_provider_manifest providers = {
		.struct_size = sizeof(providers),
		.abi_version = URP_AST_LOADER_ABI_VERSION,
		.ffmpeg = rptadv_ffmpeg_adapter_descriptor(),
		.rnnoise = rptadv_rnnoise_adapter_descriptor(),
		.ring = rpcr3_descriptor(),
		.radio = rptadv_radio_descriptor(),
		.samplerate = rptadv_samplerate_adapter_descriptor(),
		.audio = rptadv_portaudio_alsa_adapter_descriptor(),
		.gpio = rptadv_gpio_adapter_descriptor(),
	};
	int result;

	rust_loader = usbradioplus_asterisk_loader_descriptor();
	if (!urp_loader_valid(rust_loader)) {
		ast_log(LOG_ERROR, "USBRadioPlus Rust adapter ABI is unavailable\n");
		rust_loader = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}
	result = rust_loader->load(&providers, ast_module_info->self);
	if (result == URP_AST_LOADER_OK)
		return AST_MODULE_LOAD_SUCCESS;
	rust_loader = NULL;
	return result == URP_AST_LOADER_DECLINE ? AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_FAILURE;
}

/**
 * @brief Stop the Rust host after it confirms that no channel remains live.
 * @return Zero on success, or nonzero when Asterisk must retain the module.
 */
static int unload_module(void)
{
	int result;

	if (!rust_loader)
		return 0;
	result = rust_loader->unload();
	if (!result)
		rust_loader = NULL;
	return result;
}

/**
 * @brief Ask the Rust host to validate and publish a new generation.
 * @return Zero on success, or nonzero while the active generation is retained.
 */
static int reload_module(void)
{
	return rust_loader ? rust_loader->reload() : -1;
}

// cppcheck-suppress unknownMacro -- expanded only by Asterisk's build headers.
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "USBRadioPlus channel driver",
		.support_level = AST_MODULE_SUPPORT_EXTENDED, .load = load_module,
		.unload = unload_module, .reload = reload_module, .requires = "");
