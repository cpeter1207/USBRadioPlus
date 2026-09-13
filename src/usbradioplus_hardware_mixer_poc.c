/**
 * @file usbradioplus_hardware_mixer_poc.c
 * @brief Semantic CM119 mixer bridge for the selected hardware adapters.
 */

#include "usbradioplus_hardware_mixer_poc.h"

#include <string.h>

/**
 * @brief Return whether a caller selected one defined logical mixer control.
 * @param control Candidate logical control.
 * @return Nonzero when @p control indexes the bounded bridge control array.
 */
static int hardware_mixer_poc_control_valid(enum usbradioplus_hardware_mixer_poc_control control)
{
	return (unsigned int)control < USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT;
}

/**
 * @brief Return one opened logical control state.
 * @param mixer Open bridge state.
 * @param control Logical control to select.
 * @return The selected state, or NULL for an unavailable control.
 */
static struct usbradioplus_hardware_mixer_poc_control_state *
hardware_mixer_poc_control(struct usbradioplus_hardware_mixer_poc *mixer,
			   enum usbradioplus_hardware_mixer_poc_control control)
{
	if (!mixer || !mixer->opened || !hardware_mixer_poc_control_valid(control))
		return NULL;
	return &mixer->controls[(unsigned int)control];
}

/**
 * @brief Return one read-only opened logical control state.
 * @param mixer Open bridge state.
 * @param control Logical control to select.
 * @return The selected state, or NULL for an unavailable control.
 */
static const struct usbradioplus_hardware_mixer_poc_control_state *
hardware_mixer_poc_control_const(const struct usbradioplus_hardware_mixer_poc *mixer,
				 enum usbradioplus_hardware_mixer_poc_control control)
{
	if (!mixer || !mixer->opened || !hardware_mixer_poc_control_valid(control))
		return NULL;
	return &mixer->controls[(unsigned int)control];
}

/**
 * @brief Return whether a semantic path is usable for the selected control.
 * @param path Candidate path returned by facade semantic discovery.
 * @param direction Required capture or playback direction.
 * @param required_capabilities Capabilities that the selected path must provide.
 * @return Nonzero when the path is complete and capability-compatible.
 */
static int hardware_mixer_poc_path_valid(const struct rptadv_audio_cm119_mixer_path *path,
					 uint32_t direction, uint32_t required_capabilities)
{
	return path && path->element[0] &&
	       memchr(path->element, '\0', sizeof(path->element)) != NULL &&
	       path->channel <= RPTADV_AUDIO_MIXER_CHANNEL_RIGHT && path->direction == direction &&
	       required_capabilities != 0U &&
	       (path->capabilities & required_capabilities) == required_capabilities;
}

/**
 * @brief Open a complete group of semantic adapter-discovered paths.
 * @param state Destination control state.
 * @param adapter Prepared hardware facade.
 * @param paths Bounded semantic path group to open.
 * @param path_count Number of paths to open.
 * @param direction Required capture or playback direction.
 * @param required_capabilities Capabilities every path must advertise.
 * @return A facade result code.
 */
static enum usbradioplus_hardware_adapter_result
hardware_mixer_poc_open_paths(struct usbradioplus_hardware_mixer_poc_control_state *state,
			      const struct usbradioplus_hardware_adapter *adapter,
			      const struct rptadv_audio_cm119_mixer_path *paths,
			      uint32_t path_count, uint32_t direction,
			      uint32_t required_capabilities)
{
	uint32_t index;

	if (!state || !adapter || !paths || path_count == 0U ||
	    path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY || required_capabilities == 0U)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	state->switch_supported = 1U;
	for (index = 0U; index < path_count; ++index) {
		const struct rptadv_audio_cm119_mixer_path *path = &paths[index];
		const struct usbradioplus_hardware_adapter_mixer_config config = {
			.struct_size = sizeof(config),
			.element = path->element,
			.element_index = path->element_index,
			.channel = path->channel,
			.direction = path->direction,
		};
		enum usbradioplus_hardware_adapter_result result;

		if (!hardware_mixer_poc_path_valid(path, direction, required_capabilities))
			return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
		result = usbradioplus_hardware_adapter_mixer_open(adapter, &config,
								  &state->paths[index]);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
		state->path_count++;
		if ((path->capabilities & RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH) == 0U)
			state->switch_supported = 0U;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

/**
 * @brief Release every adapter-owned handle in one semantic control group.
 * @param state Control group whose handles are safe to close.
 */
static void hardware_mixer_poc_close_control_handles(
	struct usbradioplus_hardware_mixer_poc_control_state *state)
{
	uint32_t path;

	if (!state)
		return;
	for (path = 0U; path < RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY; ++path)
		usbradioplus_hardware_adapter_mixer_close(&state->paths[path]);
}

/**
 * @brief Release every handle retained by an incomplete or complete bridge.
 * @param mixer Bridge whose handles are safe to close.
 */
static void hardware_mixer_poc_close_handles(struct usbradioplus_hardware_mixer_poc *mixer)
{
	unsigned int control;

	if (!mixer)
		return;
	for (control = 0U; control < USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT; ++control)
		hardware_mixer_poc_close_control_handles(&mixer->controls[control]);
	hardware_mixer_poc_close_control_handles(&mixer->rx_compatibility_switch);
	hardware_mixer_poc_close_control_handles(&mixer->sidetone);
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_open(struct usbradioplus_hardware_mixer_poc *mixer,
				     const struct usbradioplus_hardware_adapter *adapter)
{
	struct rptadv_audio_cm119_mixer_paths paths = {
		.struct_size = sizeof(paths),
	};
	struct usbradioplus_hardware_mixer_poc candidate = {0};
	enum usbradioplus_hardware_adapter_result result;

	if (!mixer || mixer->opened || !adapter)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	result = usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(adapter, &paths);
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return result;
	/* The semantic adapter names only the first two playback paths.  The
	 * combined proof has two DAC routes, so it cannot substitute or guess when
	 * TX B is absent. */
	if (paths.rx_capture_path_count == 0U ||
	    paths.rx_capture_path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY ||
	    paths.tx_playback_path_count != 2U)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;

	result = hardware_mixer_poc_open_paths(
		&candidate.controls[USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE], adapter,
		paths.rx_capture_paths, paths.rx_capture_path_count, RPTADV_AUDIO_MIXER_CAPTURE,
		RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		result = hardware_mixer_poc_open_paths(
			&candidate.controls[USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A], adapter,
			&paths.tx_playback_paths[0], 1U, RPTADV_AUDIO_MIXER_PLAYBACK,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		result = hardware_mixer_poc_open_paths(
			&candidate.controls[USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B], adapter,
			&paths.tx_playback_paths[1], 1U, RPTADV_AUDIO_MIXER_PLAYBACK,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK &&
	    paths.rx_compatibility_switch_path_count != 0U)
		result = hardware_mixer_poc_open_paths(&candidate.rx_compatibility_switch, adapter,
						       paths.rx_compatibility_switch_paths,
						       paths.rx_compatibility_switch_path_count,
						       RPTADV_AUDIO_MIXER_PLAYBACK,
						       RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK && paths.sidetone_path_count != 0U)
		result = hardware_mixer_poc_open_paths(
			&candidate.sidetone, adapter, paths.sidetone_paths,
			paths.sidetone_path_count, RPTADV_AUDIO_MIXER_PLAYBACK,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	if (result == USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		candidate.opened = 1U;
		result = usbradioplus_hardware_mixer_poc_set_sidetone(&candidate, 0U, 0U);
	}
	if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK) {
		hardware_mixer_poc_close_handles(&candidate);
		return result;
	}
	*mixer = candidate;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_normalized(struct usbradioplus_hardware_mixer_poc *mixer,
					       enum usbradioplus_hardware_mixer_poc_control control,
					       uint32_t value)
{
	struct usbradioplus_hardware_mixer_poc_control_state *state =
		hardware_mixer_poc_control(mixer, control);
	uint32_t index;

	if (!state || state->path_count == 0U ||
	    state->path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY ||
	    value > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	for (index = 0U; index < state->path_count; ++index) {
		enum usbradioplus_hardware_adapter_result result =
			usbradioplus_hardware_adapter_mixer_set_legacy_level(&state->paths[index],
									     value);

		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_get_normalized(const struct usbradioplus_hardware_mixer_poc *mixer,
					       enum usbradioplus_hardware_mixer_poc_control control,
					       uint32_t *value)
{
	const struct usbradioplus_hardware_mixer_poc_control_state *state =
		hardware_mixer_poc_control_const(mixer, control);
	uint32_t expected;
	uint32_t index;

	if (value)
		*value = 0U;
	if (!state || !value || state->path_count == 0U ||
	    state->path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (usbradioplus_hardware_adapter_mixer_get_normalized(&state->paths[0], &expected) !=
	    USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (expected > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	for (index = 1U; index < state->path_count; ++index) {
		uint32_t observed;

		if (usbradioplus_hardware_adapter_mixer_get_normalized(
			    &state->paths[index], &observed) != USBRADIOPLUS_HARDWARE_ADAPTER_OK ||
		    observed != expected)
			return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	}
	*value = expected;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

/**
 * @brief Apply one enabled state to every path in one semantic switch group.
 * @param state Open semantic control group.
 * @param enabled Zero disables and one enables the group.
 * @return A facade result code.
 */
static enum usbradioplus_hardware_adapter_result
hardware_mixer_poc_set_control_switch(struct usbradioplus_hardware_mixer_poc_control_state *state,
				      uint32_t enabled)
{
	uint32_t index;

	if (!state || state->path_count == 0U ||
	    state->path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY || enabled > 1U)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!state->switch_supported)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	for (index = 0U; index < state->path_count; ++index) {
		enum usbradioplus_hardware_adapter_result result =
			usbradioplus_hardware_adapter_mixer_set_switch(&state->paths[index],
								       enabled);

		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_switch(struct usbradioplus_hardware_mixer_poc *mixer,
					   enum usbradioplus_hardware_mixer_poc_control control,
					   uint32_t enabled)
{
	struct usbradioplus_hardware_mixer_poc_control_state *state =
		hardware_mixer_poc_control(mixer, control);

	return hardware_mixer_poc_set_control_switch(state, enabled);
}

int usbradioplus_hardware_mixer_poc_sidetone_available(
	const struct usbradioplus_hardware_mixer_poc *mixer)
{
	return mixer && mixer->opened && mixer->sidetone.path_count != 0U &&
	       mixer->sidetone.path_count <= RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY &&
	       mixer->sidetone.switch_supported;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_set_sidetone(struct usbradioplus_hardware_mixer_poc *mixer,
					     uint32_t normalized, uint32_t enabled)
{
	uint32_t index;
	uint32_t applied = enabled ? normalized : 0U;
	enum usbradioplus_hardware_adapter_result result;

	if (!mixer || !mixer->opened || normalized > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM ||
	    enabled > 1U || mixer->sidetone.path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (enabled && !usbradioplus_hardware_mixer_poc_sidetone_available(mixer))
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (mixer->sidetone_state_valid && mixer->sidetone_normalized == applied &&
	    mixer->sidetone_enabled == enabled)
		return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
	mixer->sidetone_state_valid = 0U;
	/* Mute before reducing volume, and set volume before enabling playback. */
	if (!enabled && mixer->sidetone.path_count && mixer->sidetone.switch_supported) {
		result = hardware_mixer_poc_set_control_switch(&mixer->sidetone, 0U);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	for (index = 0U; index < mixer->sidetone.path_count; ++index) {
		result = usbradioplus_hardware_adapter_mixer_set_normalized(
			&mixer->sidetone.paths[index], applied);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	if (enabled) {
		result = hardware_mixer_poc_set_control_switch(&mixer->sidetone, 1U);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	mixer->sidetone_normalized = applied;
	mixer->sidetone_enabled = enabled;
	mixer->sidetone_state_valid = 1U;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_apply(struct usbradioplus_hardware_mixer_poc *mixer,
				      uint32_t rx_capture, uint32_t tx_a, uint32_t tx_b)
{
	static const enum usbradioplus_hardware_mixer_poc_control controls[] = {
		USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE,
		USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A,
		USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B,
	};
	const uint32_t values[] = {rx_capture, tx_a, tx_b};
	uint32_t index;

	if (!mixer || !mixer->opened)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	for (index = 0U; index < USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT; ++index) {
		enum usbradioplus_hardware_adapter_result result =
			usbradioplus_hardware_mixer_poc_set_normalized(mixer, controls[index],
								       values[index]);

		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	for (index = 0U; index < USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT; ++index) {
		const struct usbradioplus_hardware_mixer_poc_control_state *state =
			hardware_mixer_poc_control(mixer, controls[index]);
		enum usbradioplus_hardware_adapter_result result;

		if (!state || !state->switch_supported)
			continue;
		result = usbradioplus_hardware_mixer_poc_set_switch(mixer, controls[index], 1U);
		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	if (mixer->rx_compatibility_switch.path_count != 0U) {
		/* Legacy setup always writes one to this switch, which removes the
		 * CM119 receive attenuator without exposing a separate gain control. */
		enum usbradioplus_hardware_adapter_result result =
			hardware_mixer_poc_set_control_switch(&mixer->rx_compatibility_switch, 1U);

		if (result != USBRADIOPLUS_HARDWARE_ADAPTER_OK)
			return result;
	}
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_mixer_poc_get_switch(const struct usbradioplus_hardware_mixer_poc *mixer,
					   enum usbradioplus_hardware_mixer_poc_control control,
					   uint32_t *enabled)
{
	const struct usbradioplus_hardware_mixer_poc_control_state *state =
		hardware_mixer_poc_control_const(mixer, control);
	uint32_t expected;
	uint32_t index;

	if (enabled)
		*enabled = 0U;
	if (!state || !enabled || state->path_count == 0U ||
	    state->path_count > RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY)
		return USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT;
	if (!state->switch_supported)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (usbradioplus_hardware_adapter_mixer_get_switch(&state->paths[0], &expected) !=
	    USBRADIOPLUS_HARDWARE_ADAPTER_OK)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	if (expected > 1U)
		return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	for (index = 1U; index < state->path_count; ++index) {
		uint32_t observed;

		if (usbradioplus_hardware_adapter_mixer_get_switch(
			    &state->paths[index], &observed) != USBRADIOPLUS_HARDWARE_ADAPTER_OK ||
		    observed != expected)
			return USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR;
	}
	*enabled = expected;
	return USBRADIOPLUS_HARDWARE_ADAPTER_OK;
}

void usbradioplus_hardware_mixer_poc_close(struct usbradioplus_hardware_mixer_poc *mixer)
{
	if (!mixer)
		return;
	hardware_mixer_poc_close_handles(mixer);
	memset(mixer, 0, sizeof(*mixer));
}
