/** @file
 *  @brief Versioned loader ABI for the Rust-owned USBRadioPlus Asterisk host.
 */

#ifndef USBRADIOPLUS_ASTERISK_H
#define USBRADIOPLUS_ASTERISK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current loader-descriptor ABI. */
#define URP_AST_LOADER_ABI_VERSION UINT32_C(4)
/** Rust host loaded successfully. */
#define URP_AST_LOADER_OK 0
/** Configuration or provider validation declined module loading. */
#define URP_AST_LOADER_DECLINE 1
/** Asterisk host registration failed after validation. */
#define URP_AST_LOADER_FAILURE 2

/** Private initial-alpha Asterisk option ID; call ast_channel_setoption with block=0. */
#define URP_AST_OPTION_DIRECT_CALLBACKS 0x52504144
/** Exact direct callback descriptor ABI. */
#define URP_AST_DIRECT_CALLBACKS_ABI_VERSION UINT32_C(2)

/**
 * @brief Borrowed direct RadioPlusAdvanced PCM endpoints, copied before call().
 *
 * Mutable normalized mono F32 at native 48000 Hz, exactly frame_count samples.
 * Both functions and contexts are required. Each endpoint has one serial owner;
 * RX and TX may run concurrently. Callbacks return zero on success and must not
 * allocate, lock, block, log, call Asterisk, access files/network or change refcounts.
 * The owner retains contexts and executable code until synchronous channel stop
 * or hangup returns; USBRadioPlus never owns or frees contexts. Reload copies the
 * same attachment before starting the replacement stream. No DSO lookup is used.
 */
struct urp_ast_direct_callbacks {
	uint32_t struct_size; /**< Exact sizeof(struct urp_ast_direct_callbacks). */
	uint32_t abi_version; /**< Exact URP_AST_DIRECT_CALLBACKS_ABI_VERSION. */
	void *receive_context; /**< Caller-owned RX context. */
	int (*receive)(void *context, uint32_t receiver_keyed, float *samples,
		       uint32_t frame_count); /**< Process qualified USB receive output. */
	void *transmit_context; /**< Caller-owned TX context. */
	int (*transmit)(void *context, float *samples, uint32_t frame_count,
			uint32_t *keyed); /**< Fill program audio and write zero/one PTT. */
	uint32_t accepted_abi_version; /**< Initialize to zero; host acknowledges retained ABI. */
};

/** @brief Process-lifetime providers selected by the Asterisk module. */
struct urp_ast_provider_manifest {
	uint32_t struct_size; /**< Size of this structure in bytes. */
	uint32_t abi_version; /**< @c URP_AST_LOADER_ABI_VERSION. */
	const void *ffmpeg;   /**< Released FFmpeg-graph descriptor. */
	const void *rnnoise;  /**< Released RNNoise descriptor. */
	const void *ring;     /**< Released rate-adjusting-ring descriptor. */
	const void *radio;    /**< Released radio-core descriptor. */
	const void *samplerate; /**< Released sample-rate-adapter descriptor. */
	const void *audio;      /**< Released audio-adapter descriptor. */
	const void *gpio;       /**< Released GPIO-adapter descriptor. */
};

/** @brief Start the complete Rust-owned Asterisk host. */
typedef int (*urp_ast_loader_load_fn)(const struct urp_ast_provider_manifest *providers,
				      void *module_self);
/** @brief Reload the complete Rust-owned Asterisk host in place. */
typedef int (*urp_ast_loader_reload_fn)(void);
/** @brief Stop the complete Rust-owned Asterisk host. */
typedef int (*urp_ast_loader_unload_fn)(void);

/** @brief Versioned lifecycle table consumed by the metadata-only C module. */
struct urp_asterisk_loader_descriptor {
	uint32_t struct_size; /**< Size of this structure in bytes. */
	uint32_t abi_version; /**< @c URP_AST_LOADER_ABI_VERSION. */
	const char *capability; /**< Stable NUL-terminated capability name. */
	urp_ast_loader_load_fn load;     /**< Load and register the Rust host. */
	urp_ast_loader_reload_fn reload; /**< Reload the active Rust host. */
	urp_ast_loader_unload_fn unload; /**< Unregister and destroy the Rust host. */
};

/**
 * @brief Return the immutable process-lifetime loader descriptor.
 * @return Descriptor for @c URP_AST_LOADER_ABI_VERSION.
 */
const struct urp_asterisk_loader_descriptor *usbradioplus_asterisk_loader_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif
