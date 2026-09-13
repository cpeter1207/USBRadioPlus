/**
 * @file usbradioplus_hardware_mixer_poc.h
 * @brief Semantic CM119 mixer bridge API for the selected hardware adapters.
 *
 * The PortAudio/GPIO composition selects one CM119 through the hardware
 * facade.  This control-plane bridge opens only the facade-discovered mixer
 * paths for that same USB identity.  It deliberately does not accept an ALSA
 * card number or call the retained ASL radio mixer helpers.
 */

#ifndef USBRADIOPLUS_HARDWARE_MIXER_POC_H
#define USBRADIOPLUS_HARDWARE_MIXER_POC_H

#include <stdint.h>

#include "usbradioplus_hardware_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Logical CM119 mixer controls retained by the combined POC. */
enum usbradioplus_hardware_mixer_poc_control {
	/** CM119 ADC capture gain and capture switch. */
	USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE = 0,
	/** First CM119 DAC path, preserving legacy TX A semantics. */
	USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A = 1,
	/** Second CM119 DAC path, preserving legacy TX B semantics. */
	USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B = 2,
	/** Number of logical controls in one bridge. */
	USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT = 3,
};

/**
 * @brief Adapter-owned handles that implement one logical mixer control.
 *
 * A CM119 may expose left and right capture paths.  The RX logical control
 * therefore retains up to two handles and applies a single normalized value
 * or switch state to both.  TX A and TX B each retain exactly one discovered
 * playback path.
 */
struct usbradioplus_hardware_mixer_poc_control_state {
	/** Adapter-owned handles for this semantic control. */
	struct usbradioplus_hardware_adapter_mixer paths[RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY];
	/** Number of opened paths in \c paths. */
	uint32_t path_count;
	/** Nonzero only when every path has a native ALSA switch. */
	uint32_t switch_supported;
};

/**
 * @brief Preallocated control-plane state for one PortAudio/GPIO composition.
 *
 * The caller zero-initializes this state, opens it after the facade has proven
 * its audio/GPIO identity, and closes it before that facade is released.  It
 * is not usable from a PCM callback.
 */
struct usbradioplus_hardware_mixer_poc {
	/** Opened semantic RX, TX A, and TX B control groups. */
	struct usbradioplus_hardware_mixer_poc_control_state
		controls[USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT];
	/** Optional legacy receive-compatibility playback-switch paths. */
	struct usbradioplus_hardware_mixer_poc_control_state rx_compatibility_switch;
	/** Optional CM119 hardware local-repeat playback paths. */
	struct usbradioplus_hardware_mixer_poc_control_state sidetone;
	/** Last successfully applied sidetone gain, after disabled-state normalization. */
	uint32_t sidetone_normalized;
	/** Last successfully applied sidetone switch state. */
	uint32_t sidetone_enabled;
	/** Nonzero when the sidetone cache matches the applied hardware state. */
	uint32_t sidetone_state_valid;
	/** Nonzero after every required semantic path opened successfully. */
	uint32_t opened;
};

/**
 * @brief Resolve and open the required semantic CM119 mixer controls.
 * @param mixer Zero-initialized bridge state to populate.
 * @param adapter Prepared combined PortAudio/GPIO hardware facade.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * The facade resolves controls by its canonical USB topology.  RX capture,
 * TX A, and TX B must all be present and volume-capable.  When the adapter
 * advertises the optional legacy receive-compatibility switch, this bridge
 * opens it as a switch-only path and enables it during
 * \c usbradioplus_hardware_mixer_poc_apply().  Its absence remains valid; a
 * malformed or unopenable advertised path fails closed and releases every
 * handle opened by this call.  No numeric ALSA card or legacy ASL mixer
 * fallback is used.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_open(struct usbradioplus_hardware_mixer_poc *mixer,
				     const struct usbradioplus_hardware_adapter *adapter);

/**
 * @brief Set one logical mixer gain on the portable inclusive 0--999 scale.
 * @param mixer Open semantic mixer bridge.
 * @param control Logical RX, TX A, or TX B control.
 * @param value Normalized mixer value from zero through 999.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * RX and DAC controls retain floor(value * native maximum / 1000), so existing
 * calibration and configuration values produce the same physical gain steps.
 * Sidetone continues to use the generic nearest-step normalized mapping.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_normalized(struct usbradioplus_hardware_mixer_poc *mixer,
					       enum usbradioplus_hardware_mixer_poc_control control,
					       uint32_t value);

/**
 * @brief Read one logical mixer gain on the portable inclusive 0--999 scale.
 * @param mixer Open semantic mixer bridge.
 * @param control Logical RX, TX A, or TX B control.
 * @param value Receives the common adapter-normalized physical observation.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * A multi-path RX capture control is accepted only when its paths report one
 * common normalized value.  An inconsistent hardware state is reported as an
 * error rather than silently choosing one channel. This observation is not an
 * inverse of the legacy gain setting: native hardware steps quantize that setting.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_get_normalized(const struct usbradioplus_hardware_mixer_poc *mixer,
					       enum usbradioplus_hardware_mixer_poc_control control,
					       uint32_t *value);

/**
 * @brief Set one logical mixer capture or playback switch.
 * @param mixer Open semantic mixer bridge.
 * @param control Logical RX, TX A, or TX B control.
 * @param enabled Zero disables and one enables the semantic control.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * A switch is available only when every physical path in the selected logical
 * control advertises one.  The bridge does not emulate a missing switch.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_switch(struct usbradioplus_hardware_mixer_poc *mixer,
					   enum usbradioplus_hardware_mixer_poc_control control,
					   uint32_t enabled);

/**
 * @brief Apply the complete legacy CM119 RX/TX mixer state through semantic controls.
 * @param mixer Open semantic mixer bridge.
 * @param rx_capture Normalized receive capture gain from zero through 999.
 * @param tx_a Normalized TX A playback gain from zero through 999.
 * @param tx_b Normalized TX B playback gain from zero through 999.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This applies the receiver capture and two transmitter DAC paths. It enables a native
 * capture or playback switch when every path in that semantic group exposes
 * one, and enables the optional legacy receive-compatibility switch when the
 * adapter advertises it.  A control with no switch remains valid because
 * CM119 mixer layouts do not all provide one.  Callers treat any gain or
 * available-switch failure as a hardware fault and retire the proof rather
 * than falling back to a legacy numeric ALSA-card mixer.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_apply(struct usbradioplus_hardware_mixer_poc *mixer,
				      uint32_t rx_capture, uint32_t tx_a, uint32_t tx_b);

/**
 * @brief Report whether hardware local repeat has both semantic volume and switch paths.
 * @param mixer Open semantic mixer bridge.
 * @return Nonzero when hardware duplex3 is supported; its normalized maximum is 999.
 */
int usbradioplus_hardware_mixer_poc_sidetone_available(
	const struct usbradioplus_hardware_mixer_poc *mixer);

/**
 * @brief Apply cached hardware local-repeat gain and switch state outside the audio callback.
 * @param mixer Open semantic mixer bridge.
 * @param normalized Enabled sidetone gain on the inclusive 0--999 scale.
 * @param enabled Zero disables and one enables hardware local repeat.
 * @return A facade result code; enabling an unavailable sidetone returns an audio error.
 *
 * Disabled sidetone always has zero gain. Missing sidetone paths are valid only
 * when disabled. Repeated successful states perform no hardware I/O; a failed
 * write invalidates the cache so a retry reapplies every path. Opening a bridge
 * disables every advertised sidetone path before publishing the bridge.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_sidetone(struct usbradioplus_hardware_mixer_poc *mixer,
					     uint32_t normalized, uint32_t enabled);

/**
 * @brief Read one logical mixer capture or playback switch.
 * @param mixer Open semantic mixer bridge.
 * @param control Logical RX, TX A, or TX B control.
 * @param enabled Receives the common zero or one switch state.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * A multi-path RX control reports an error when its paths disagree, avoiding
 * an arbitrary partial representation of the underlying hardware state.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_get_switch(const struct usbradioplus_hardware_mixer_poc *mixer,
					   enum usbradioplus_hardware_mixer_poc_control control,
					   uint32_t *enabled);

/**
 * @brief Release all facade-owned mixer handles and reset the bridge state.
 * @param mixer Bridge to close; NULL is accepted.
 */
void usbradioplus_hardware_mixer_poc_close(struct usbradioplus_hardware_mixer_poc *mixer);

#ifdef __cplusplus
}
#endif

#endif
