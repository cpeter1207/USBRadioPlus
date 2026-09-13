/**
 * @file test_hardware_mixer_poc.c
 * @brief Hardware-free tests of semantic CM119 mixer bridging.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>
#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#include "usbradioplus_hardware_adapter.h"
#include "usbradioplus_hardware_mixer_poc.h"

/** @brief One fake adapter-owned mixer handle. */
struct fake_mixer {
	uint32_t normalized;
	uint32_t enabled;
	int64_t minimum;
	int64_t maximum;
	int64_t steps;
};

/** @brief One copied semantic path opened through the fake adapter. */
struct fake_opened_path {
	/** Copy of the caller-owned semantic ALSA element name. */
	char element[RPTADV_AUDIO_CM119_MIXER_ELEMENT_NAME_CAPACITY];
	/** ALSA element index. */
	uint32_t element_index;
	/** Selected left or right ALSA mixer channel. */
	uint32_t channel;
	/** Selected capture or playback direction. */
	uint32_t direction;
};

/** @brief Fake mixer handles allocated in semantic discovery order. */
static struct fake_mixer fake_mixers[7];
/** @brief Requested mixer paths captured by the fake adapter. */
static struct fake_opened_path fake_opened_paths[7];
/** @brief Number of successful or attempted mixer-open calls. */
static unsigned int fake_open_calls;
/** @brief Number of adapter mixer-close calls. */
static unsigned int fake_close_calls;
/** @brief Number of normalized gain writes. */
static unsigned int fake_normalized_set_calls;
/** @brief Number of native gain writes through the legacy compatibility mapping. */
static unsigned int fake_steps_set_calls;
/** @brief Scripted native range and gain write results. */
static enum rptadv_audio_result fake_range_result, fake_steps_result;
/** @brief Number of switch writes. */
static unsigned int fake_switch_set_calls;
/** @brief Zero-based open attempt to fail, or a value outside the fake array. */
static unsigned int fake_fail_open_at;
/** @brief Zero-based switch write to fail, or a value outside the fake array. */
static unsigned int fake_fail_switch_at;
/** @brief Zero-based normalized write to fail, or UINT32_MAX. */
static unsigned int fake_fail_normalized_at;
/** @brief Semantic discovery mode used by one test. */
static unsigned int fake_path_mode;
/** @brief Mixer index whose read operations fail, or UINT32_MAX. */
static unsigned int fake_fail_read_at;

enum fake_path_mode {
	FAKE_PATH_STANDARD = 0U,
	FAKE_PATH_MISSING_TX_B = 1U,
	FAKE_PATH_MALFORMED_TX_B = 2U,
	FAKE_PATH_RX_COMPATIBILITY_PRESENT = 3U,
	FAKE_PATH_RX_COMPATIBILITY_MALFORMED = 4U,
	FAKE_PATH_SIDETONE_PRESENT = 5U,
	FAKE_PATH_SIDETONE_NO_SWITCH = 6U,
	FAKE_PATH_SIDETONE_MALFORMED = 7U,
};

/**
 * @brief Keep the facade's released-audio entry point linkable in this fake-only test.
 * @return Null because the test supplies an injected adapter descriptor.
 */
const struct rptadv_audio_adapter_descriptor *rptadv_portaudio_alsa_adapter_descriptor(void)
{
	return NULL;
}

/**
 * @brief Keep the facade's released-GPIO entry point linkable in this fake-only test.
 * @return Null because the test supplies an injected adapter descriptor.
 */
const struct rptadv_gpio_adapter_descriptor *rptadv_gpio_adapter_descriptor(void)
{
	return NULL;
}

/**
 * @brief Populate one valid semantic mixer path.
 * @param path Path to populate.
 * @param element ALSA element name.
 * @param channel Physical left or right channel.
 * @param direction Capture or playback direction.
 * @param capabilities Volume and optional switch capabilities.
 */
static void fake_mixer_path(struct rptadv_audio_cm119_mixer_path *path, const char *element,
			    uint32_t channel, uint32_t direction, uint32_t capabilities)
{
	assert(path != NULL);
	assert(strlen(element) < sizeof(path->element));
	strcpy(path->element, element);
	path->channel = channel;
	path->direction = direction;
	path->capabilities = capabilities;
}

/**
 * @brief Return scripted semantic paths without touching ALSA hardware.
 * @param topology Stable USB topology supplied by the facade.
 * @param paths Caller-sized result to fill.
 * @return The scripted adapter result.
 */
static enum rptadv_audio_result
fake_cm119_mixer_paths_resolve(const char *topology, struct rptadv_audio_cm119_mixer_paths *paths)
{
	uint32_t struct_size;

	assert(topology != NULL);
	assert(!strcmp(topology, "1-2:1.0"));
	assert(paths != NULL);
	struct_size = paths->struct_size;
	memset(paths, 0, sizeof(*paths));
	paths->struct_size = struct_size;
	if (struct_size < sizeof(*paths))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	paths->abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION;
	paths->rx_capture_path_count = 2U;
	paths->tx_playback_path_count = fake_path_mode == FAKE_PATH_MISSING_TX_B ? 1U : 2U;
	fake_mixer_path(&paths->rx_capture_paths[0], "Mic", RPTADV_AUDIO_MIXER_CHANNEL_LEFT,
			RPTADV_AUDIO_MIXER_CAPTURE,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME |
				RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH);
	fake_mixer_path(&paths->rx_capture_paths[1], "Mic", RPTADV_AUDIO_MIXER_CHANNEL_RIGHT,
			RPTADV_AUDIO_MIXER_CAPTURE,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME |
				RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH);
	fake_mixer_path(&paths->tx_playback_paths[0], "Speaker", RPTADV_AUDIO_MIXER_CHANNEL_LEFT,
			RPTADV_AUDIO_MIXER_PLAYBACK,
			RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME |
				RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH);
	if (fake_path_mode != FAKE_PATH_MISSING_TX_B)
		fake_mixer_path(&paths->tx_playback_paths[1], "Speaker",
				RPTADV_AUDIO_MIXER_CHANNEL_RIGHT, RPTADV_AUDIO_MIXER_PLAYBACK,
				RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	if (fake_path_mode == FAKE_PATH_MALFORMED_TX_B)
		paths->tx_playback_paths[1].direction = RPTADV_AUDIO_MIXER_CAPTURE;
	if (fake_path_mode == FAKE_PATH_RX_COMPATIBILITY_PRESENT ||
	    fake_path_mode == FAKE_PATH_RX_COMPATIBILITY_MALFORMED) {
		paths->rx_compatibility_switch_path_count = 1U;
		fake_mixer_path(&paths->rx_compatibility_switch_paths[0], "Auto Gain Control",
				RPTADV_AUDIO_MIXER_CHANNEL_LEFT, RPTADV_AUDIO_MIXER_PLAYBACK,
				fake_path_mode == FAKE_PATH_RX_COMPATIBILITY_PRESENT
					? RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH
					: RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME);
	}
	if (fake_path_mode >= FAKE_PATH_SIDETONE_PRESENT) {
		uint32_t index;

		paths->sidetone_path_count = 2U;
		for (index = 0U; index < 2U; ++index)
			fake_mixer_path(&paths->sidetone_paths[index], "Mic Playback", index,
					RPTADV_AUDIO_MIXER_PLAYBACK,
					RPTADV_AUDIO_CM119_MIXER_PATH_VOLUME |
						(fake_path_mode == FAKE_PATH_SIDETONE_NO_SWITCH
							 ? 0U
							 : RPTADV_AUDIO_CM119_MIXER_PATH_SWITCH));
		if (fake_path_mode == FAKE_PATH_SIDETONE_MALFORMED)
			paths->sidetone_paths[1].direction = RPTADV_AUDIO_MIXER_CAPTURE;
	}
	return RPTADV_AUDIO_OK;
}

/**
 * @brief Open one fake mixer using only the facade's canonical USB topology.
 * @param config Adapter-owned topology and semantic control description.
 * @param mixer Receives the fake opaque mixer handle.
 * @return A scripted result.
 */
static enum rptadv_audio_result
fake_mixer_create_for_usb_interface(const struct rptadv_audio_usb_mixer_config *config,
				    struct rptadv_audio_mixer **mixer)
{
	unsigned int index = fake_open_calls++;

	if (!config || !mixer || index >= sizeof(fake_mixers) / sizeof(fake_mixers[0]))
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	assert(!strcmp(config->usb_interface_path, "1-2:1.0"));
	if (index == fake_fail_open_at)
		return RPTADV_AUDIO_ALSA_ERROR;
	assert(strlen(config->element) < sizeof(fake_opened_paths[index].element));
	strcpy(fake_opened_paths[index].element, config->element);
	fake_opened_paths[index].element_index = config->element_index;
	fake_opened_paths[index].channel = config->channel;
	fake_opened_paths[index].direction = config->direction;
	*mixer = (struct rptadv_audio_mixer *)&fake_mixers[index];
	return RPTADV_AUDIO_OK;
}

/** @brief Return one fake physical mixer range. */
static enum rptadv_audio_result fake_mixer_get_range_steps(const struct rptadv_audio_mixer *mixer,
							   int64_t *minimum, int64_t *maximum)
{
	const struct fake_mixer *state = (const struct fake_mixer *)mixer;

	*minimum = state->minimum;
	*maximum = state->maximum;
	return fake_range_result;
}

/** @brief Capture the exact physical gain step requested by the compatibility bridge. */
static enum rptadv_audio_result fake_mixer_set_steps(struct rptadv_audio_mixer *mixer,
						     int64_t value)
{
	struct fake_mixer *state = (struct fake_mixer *)mixer;

	fake_steps_set_calls++;
	if (fake_steps_result != RPTADV_AUDIO_OK)
		return fake_steps_result;
	assert(value >= state->minimum && value <= state->maximum);
	state->steps = value;
	/* Most fixtures use a 0--1000 native range to preserve their direct value probe. */
	state->normalized = (uint32_t)value;
	return RPTADV_AUDIO_OK;
}

/** @brief Read one fake normalized mixer gain. */
static enum rptadv_audio_result fake_mixer_get_normalized(const struct rptadv_audio_mixer *mixer,
							  uint32_t *value)
{
	if (!mixer || !value)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (fake_fail_read_at < 7U &&
	    mixer == (const struct rptadv_audio_mixer *)&fake_mixers[fake_fail_read_at])
		return RPTADV_AUDIO_ALSA_ERROR;
	*value = ((const struct fake_mixer *)mixer)->normalized;
	return RPTADV_AUDIO_OK;
}

/** @brief Write one fake normalized mixer gain. */
static enum rptadv_audio_result fake_mixer_set_normalized(struct rptadv_audio_mixer *mixer,
							  uint32_t value)
{
	unsigned int index = fake_normalized_set_calls++;

	if (!mixer || value > RPTADV_AUDIO_MIXER_NORMALIZED_MAXIMUM)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (index == fake_fail_normalized_at)
		return RPTADV_AUDIO_ALSA_ERROR;
	((struct fake_mixer *)mixer)->normalized = value;
	return RPTADV_AUDIO_OK;
}

/** @brief Read one fake mixer switch. */
static enum rptadv_audio_result fake_mixer_get_switch(const struct rptadv_audio_mixer *mixer,
						      uint32_t *enabled)
{
	if (!mixer || !enabled)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (fake_fail_read_at < 7U &&
	    mixer == (const struct rptadv_audio_mixer *)&fake_mixers[fake_fail_read_at])
		return RPTADV_AUDIO_ALSA_ERROR;
	*enabled = ((const struct fake_mixer *)mixer)->enabled;
	return RPTADV_AUDIO_OK;
}

/** @brief Write one fake mixer switch. */
static enum rptadv_audio_result fake_mixer_set_switch(struct rptadv_audio_mixer *mixer,
						      uint32_t enabled)
{
	const unsigned int index = fake_switch_set_calls++;

	if (!mixer || enabled > 1U)
		return RPTADV_AUDIO_INVALID_ARGUMENT;
	if (index == fake_fail_switch_at)
		return RPTADV_AUDIO_ALSA_ERROR;
	((struct fake_mixer *)mixer)->enabled = enabled;
	return RPTADV_AUDIO_OK;
}

/** @brief Release one fake adapter-owned mixer handle. */
static void fake_mixer_destroy(struct rptadv_audio_mixer *mixer)
{
	assert(mixer != NULL);
	((struct fake_mixer *)mixer)->normalized = 0U;
	fake_close_calls++;
}

/** @brief Descriptor containing the dynamic control-plane calls used by this test. */
static struct rptadv_audio_adapter_descriptor fake_audio = {
	.struct_size = sizeof(fake_audio),
	.abi_version = RPTADV_AUDIO_ADAPTER_ABI_VERSION,
	.mixer_create_for_usb_interface = fake_mixer_create_for_usb_interface,
	.mixer_get_range_steps = fake_mixer_get_range_steps,
	.mixer_set_steps = fake_mixer_set_steps,
	.mixer_get_normalized = fake_mixer_get_normalized,
	.mixer_set_normalized = fake_mixer_set_normalized,
	.mixer_get_switch = fake_mixer_get_switch,
	.mixer_set_switch = fake_mixer_set_switch,
	.mixer_destroy = fake_mixer_destroy,
	.cm119_mixer_paths_resolve = fake_cm119_mixer_paths_resolve,
};

/**
 * @brief Make one already prepared facade without probing physical hardware.
 * @return A facade with an injected fake audio descriptor.
 */
static struct usbradioplus_hardware_adapter ready_adapter(void)
{
	struct usbradioplus_hardware_adapter adapter = {
		.audio = &fake_audio,
	};

	strcpy(adapter.usb_port_path, "1-2:1.0");
	return adapter;
}

/** @brief Reset deterministic adapter state before one independent test. */
static void reset_fake_adapter(void)
{
	memset(fake_mixers, 0, sizeof(fake_mixers));
	for (size_t index = 0U; index < sizeof(fake_mixers) / sizeof(fake_mixers[0]); ++index)
		fake_mixers[index].maximum = 1000;
	memset(fake_opened_paths, 0, sizeof(fake_opened_paths));
	fake_open_calls = 0U;
	fake_close_calls = 0U;
	fake_normalized_set_calls = 0U;
	fake_steps_set_calls = 0U;
	fake_range_result = fake_steps_result = RPTADV_AUDIO_OK;
	fake_switch_set_calls = 0U;
	fake_fail_open_at = sizeof(fake_mixers) / sizeof(fake_mixers[0]);
	fake_fail_switch_at = UINT32_MAX;
	fake_fail_normalized_at = UINT32_MAX;
	fake_fail_read_at = UINT32_MAX;
	fake_path_mode = FAKE_PATH_STANDARD;
	fake_audio.struct_size = sizeof(fake_audio);
}

/** @brief Verify semantic discovery opens RX, TX A, and TX B through USB identity. */
static void test_semantic_open_and_controls(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};
	uint32_t value;

	reset_fake_adapter();
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer.opened == 1U);
	assert(fake_open_calls == 4U);
	assert(!strcmp(fake_opened_paths[0].element, "Mic"));
	assert(fake_opened_paths[0].channel == RPTADV_AUDIO_MIXER_CHANNEL_LEFT);
	assert(fake_opened_paths[1].channel == RPTADV_AUDIO_MIXER_CHANNEL_RIGHT);
	assert(!strcmp(fake_opened_paths[2].element, "Speaker"));
	assert(fake_opened_paths[2].channel == RPTADV_AUDIO_MIXER_CHANNEL_LEFT);
	assert(fake_opened_paths[3].channel == RPTADV_AUDIO_MIXER_CHANNEL_RIGHT);

	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, 999U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[0].normalized == 999U && fake_mixers[1].normalized == 999U);
	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[2].normalized == 0U && fake_mixers[3].normalized == 500U);
	assert(usbradioplus_hardware_mixer_poc_get_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 999U);

	assert(usbradioplus_hardware_mixer_poc_set_switch(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[0].enabled == 1U && fake_mixers[1].enabled == 1U);
	assert(usbradioplus_hardware_mixer_poc_get_switch(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(value == 1U);
	assert(usbradioplus_hardware_mixer_poc_set_switch(&mixer,
							  USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A,
							  0U) == USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_set_switch(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(fake_switch_set_calls == 3U);
	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B, 1000U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);

	usbradioplus_hardware_mixer_poc_close(&mixer);
	assert(mixer.opened == 0U);
	assert(fake_close_calls == 4U);
}

/** @brief Verify an absent optional compatibility path preserves the prior POC apply behavior. */
static void test_absent_rx_compatibility_switch_preserves_apply(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer.rx_compatibility_switch.path_count == 0U);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 999U, 0U, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[0].normalized == 999U && fake_mixers[1].normalized == 999U);
	assert(fake_mixers[2].normalized == 0U && fake_mixers[3].normalized == 500U);
	/* RX and TX A have switches; TX B deliberately models a valid switchless path. */
	assert(fake_mixers[0].enabled == 1U && fake_mixers[1].enabled == 1U);
	assert(fake_mixers[2].enabled == 1U && fake_mixers[3].enabled == 0U);
	assert(fake_steps_set_calls == 4U && fake_normalized_set_calls == 0U);
	assert(fake_switch_set_calls == 3U);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 1000U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Verify a present compatibility switch is opened and enabled without a gain write. */
static void test_present_rx_compatibility_switch_is_enabled(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_RX_COMPATIBILITY_PRESENT;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer.rx_compatibility_switch.path_count == 1U);
	assert(fake_open_calls == 5U);
	assert(!strcmp(fake_opened_paths[4].element, "Auto Gain Control"));
	assert(fake_opened_paths[4].direction == RPTADV_AUDIO_MIXER_PLAYBACK);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 999U, 0U, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[4].normalized == 0U);
	assert(fake_mixers[4].enabled == 1U);
	assert(fake_steps_set_calls == 4U && fake_normalized_set_calls == 0U);
	assert(fake_switch_set_calls == 4U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
	assert(fake_close_calls == 5U);
}

/** @brief Verify a missing or malformed semantic control never falls back to a card number. */
static void test_missing_or_malformed_controls_fail_closed(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_MISSING_TX_B;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(mixer.opened == 0U && fake_open_calls == 0U && fake_close_calls == 0U);

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_MALFORMED_TX_B;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(mixer.opened == 0U && fake_open_calls == 0U && fake_close_calls == 0U);

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_RX_COMPATIBILITY_MALFORMED;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(mixer.opened == 0U && fake_open_calls == 0U && fake_close_calls == 0U);
}

/** @brief Verify partial semantic opens are released before the bridge reports failure. */
static void test_partial_open_is_released(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_fail_open_at = 2U;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(mixer.opened == 0U);
	assert(fake_open_calls == 3U);
	assert(fake_close_calls == 2U);
}

/** @brief Verify an advertised compatibility switch open failure releases every prior handle. */
static void test_rx_compatibility_open_failure_is_released(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_RX_COMPATIBILITY_PRESENT;
	fake_fail_open_at = 4U;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(mixer.opened == 0U);
	assert(fake_open_calls == 5U);
	assert(fake_close_calls == 4U);
}

/** @brief Verify a compatibility switch write failure is returned to the POC owner. */
static void test_rx_compatibility_switch_failure_is_reported(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_RX_COMPATIBILITY_PRESENT;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	/* RX has two switch paths, TX A has one, then compatibility is fourth. */
	fake_fail_switch_at = 3U;
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 999U, 0U, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(fake_switch_set_calls == 4U);
	assert(fake_mixers[4].enabled == 0U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
	assert(fake_close_calls == 5U);
}

/** @brief Preserve exact legacy capture and DAC steps without touching sidetone conversion. */
static void test_legacy_gain_steps(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_SIDETONE_PRESENT;
	fake_mixers[0].maximum = fake_mixers[1].maximum = 31;
	fake_mixers[2].maximum = fake_mixers[3].maximum = 151;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_normalized_set_calls == 2U && fake_steps_set_calls == 0U);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 500U, 999U, 706U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[0].steps == 15 && fake_mixers[1].steps == 15);
	assert(fake_mixers[2].steps == 150 && fake_mixers[3].steps == 106);
	assert(fake_steps_set_calls == 4U && fake_normalized_set_calls == 2U);
	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, 999U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[0].steps == 30 && fake_mixers[1].steps == 30);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(!fake_mixers[0].steps && !fake_mixers[1].steps && !fake_mixers[2].steps &&
	       !fake_mixers[3].steps);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[4].normalized == 999U && fake_mixers[5].normalized == 999U);
	assert(fake_normalized_set_calls == 4U && fake_steps_set_calls == 10U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Legacy scaling rejects bad ranges/writes and safely handles a wide native maximum. */
static void test_legacy_gain_failures_and_wide_range(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};
	struct usbradioplus_hardware_adapter_mixer invalid = {0};
	struct usbradioplus_hardware_adapter_mixer *path;

	reset_fake_adapter();
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(NULL, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(&invalid, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	invalid.audio = &fake_audio;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(&invalid, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	path = &mixer.controls[USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A].paths[0];
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 1000U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	fake_range_result = RPTADV_AUDIO_ALSA_ERROR;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_range_result = RPTADV_AUDIO_OK;
	fake_mixers[2].minimum = 1001;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_mixers[2].minimum = -2;
	fake_mixers[2].maximum = -1;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 500U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_mixers[2].minimum = 1;
	fake_mixers[2].maximum = 151;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_mixers[2].minimum = 0;
	fake_steps_result = RPTADV_AUDIO_ALSA_ERROR;
	assert(usbradioplus_hardware_mixer_poc_set_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_A, 706U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_steps_result = RPTADV_AUDIO_OK;
	fake_mixers[2].maximum = INT64_MAX;
	assert(usbradioplus_hardware_adapter_mixer_set_legacy_level(path, 999U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[2].steps == INT64_C(9214148664817921031));
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Verify inconsistent multi-channel RX state and malformed values are rejected. */
static void test_inconsistent_readback_fails_closed(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};
	uint32_t value = 123U;

	reset_fake_adapter();
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	fake_mixers[0].normalized = 100U;
	fake_mixers[1].normalized = 101U;
	assert(usbradioplus_hardware_mixer_poc_get_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(value == 0U);
	fake_mixers[0].normalized = 1000U;
	fake_mixers[1].normalized = 1000U;
	assert(usbradioplus_hardware_mixer_poc_get_normalized(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_mixers[0].normalized = 100U;
	fake_mixers[1].normalized = 100U;
	fake_mixers[0].enabled = 0U;
	fake_mixers[1].enabled = 1U;
	assert(usbradioplus_hardware_mixer_poc_get_switch(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(value == 0U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Hardware local repeat starts muted and writes only changed successful states. */
static void test_sidetone_gain_switch_and_cache(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_SIDETONE_PRESENT;
	fake_mixers[4].normalized = fake_mixers[5].normalized = 800U;
	fake_mixers[4].enabled = fake_mixers[5].enabled = 1U;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_sidetone_available(&mixer));
	assert(fake_mixers[4].normalized == 0U && fake_mixers[5].normalized == 0U);
	assert(fake_mixers[4].enabled == 0U && fake_mixers[5].enabled == 0U);
	assert(fake_open_calls == 6U);
	assert(fake_normalized_set_calls == 2U && fake_switch_set_calls == 2U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_normalized_set_calls == 2U && fake_switch_set_calls == 2U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[4].normalized == 999U && fake_mixers[5].normalized == 999U);
	assert(fake_mixers[4].enabled == 1U && fake_mixers[5].enabled == 1U);
	assert(fake_normalized_set_calls == 4U && fake_switch_set_calls == 4U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_normalized_set_calls == 4U && fake_switch_set_calls == 4U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 250U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[4].normalized == 250U && fake_mixers[5].normalized == 250U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 250U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(fake_mixers[4].normalized == 0U && fake_mixers[5].normalized == 0U);
	assert(fake_mixers[4].enabled == 0U && fake_mixers[5].enabled == 0U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
	assert(fake_close_calls == 6U);
	assert(!usbradioplus_hardware_mixer_poc_sidetone_available(&mixer));
}

/** @brief Unsupported sidetone remains disabled and never silently enables hardware repeat. */
static void test_sidetone_unavailable_and_invalid_requests(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};

	reset_fake_adapter();
	assert(!usbradioplus_hardware_mixer_poc_sidetone_available(NULL));
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(NULL, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(!usbradioplus_hardware_mixer_poc_sidetone_available(&mixer));
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 999U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 1000U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 2U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	usbradioplus_hardware_mixer_poc_close(&mixer);
	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_SIDETONE_NO_SWITCH;
	fake_mixers[4].normalized = fake_mixers[5].normalized = 800U;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(!usbradioplus_hardware_mixer_poc_sidetone_available(&mixer));
	assert(fake_mixers[4].normalized == 0U && fake_mixers[5].normalized == 0U);
	assert(fake_switch_set_calls == 0U);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 200U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Partial sidetone writes are reported and retried instead of entering the cache. */
static void test_sidetone_failures_and_retry(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};
	unsigned int mode;

	for (mode = 0U; mode < 4U; ++mode) {
		reset_fake_adapter();
		fake_path_mode =
			mode == 3U ? FAKE_PATH_SIDETONE_MALFORMED : FAKE_PATH_SIDETONE_PRESENT;
		if (mode == 0U)
			fake_fail_open_at = 5U;
		else if (mode == 1U)
			fake_fail_switch_at = 1U;
		else if (mode == 2U)
			fake_fail_normalized_at = 1U;
		assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
		assert(!mixer.opened);
		assert(fake_close_calls == (mode == 0U ? 5U : mode == 3U ? 0U : 6U));
	}
	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_SIDETONE_PRESENT;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	fake_fail_normalized_at = fake_normalized_set_calls + 1U;
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 350U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(!mixer.sidetone_state_valid);
	fake_fail_normalized_at = UINT32_MAX;
	fake_fail_switch_at = fake_switch_set_calls + 1U;
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 350U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(!mixer.sidetone_state_valid);
	fake_fail_switch_at = UINT32_MAX;
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 350U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(mixer.sidetone_state_valid && fake_mixers[4].enabled && fake_mixers[5].enabled);
	assert(fake_mixers[4].normalized == 350U && fake_mixers[5].normalized == 350U);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Run the direct semantic mixer bridge test suite. */
static void test_public_control_boundaries(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0}, saved;
	const enum usbradioplus_hardware_mixer_poc_control rx =
		USBRADIOPLUS_HARDWARE_MIXER_POC_RX_CAPTURE;
	uint32_t value;
	unsigned int invalid;
	reset_fake_adapter();
	usbradioplus_hardware_mixer_poc_close(NULL);
	assert(usbradioplus_hardware_mixer_poc_open(NULL, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_apply(NULL, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 0U, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	saved = mixer;
	for (invalid = 0U; invalid < 5U; ++invalid) {
		struct usbradioplus_hardware_mixer_poc *selected = invalid == 0U ? NULL : &mixer;
		enum usbradioplus_hardware_mixer_poc_control control =
			invalid == 2U ? USBRADIOPLUS_HARDWARE_MIXER_POC_CONTROL_COUNT : rx;
		mixer = saved;
		if (invalid == 1U)
			mixer.opened = 0U;
		if (invalid == 3U)
			mixer.controls[rx].path_count = 0U;
		if (invalid == 4U)
			mixer.controls[rx].path_count = RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY + 1U;
		assert(usbradioplus_hardware_mixer_poc_set_normalized(selected, control, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
		assert(usbradioplus_hardware_mixer_poc_get_normalized(selected, control, &value) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
		assert(usbradioplus_hardware_mixer_poc_set_switch(selected, control, 0U) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
		assert(usbradioplus_hardware_mixer_poc_get_switch(selected, control, &value) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	}
	mixer = saved;
	assert(usbradioplus_hardware_mixer_poc_get_normalized(&mixer, rx, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_get_switch(&mixer, rx, NULL) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_set_switch(&mixer, rx, 2U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	assert(usbradioplus_hardware_mixer_poc_get_switch(
		       &mixer, USBRADIOPLUS_HARDWARE_MIXER_POC_TX_B, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	for (invalid = 0U; invalid < 2U; ++invalid) {
		fake_fail_read_at = invalid;
		assert(usbradioplus_hardware_mixer_poc_get_normalized(&mixer, rx, &value) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
		assert(usbradioplus_hardware_mixer_poc_get_switch(&mixer, rx, &value) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	}
	fake_fail_read_at = UINT32_MAX;
	fake_mixers[0].enabled = 2U;
	assert(usbradioplus_hardware_mixer_poc_get_switch(&mixer, rx, &value) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_mixers[0].enabled = 0U;
	fake_steps_result = RPTADV_AUDIO_ALSA_ERROR;
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 10U, 10U, 10U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	fake_steps_result = RPTADV_AUDIO_OK;
	fake_fail_switch_at = fake_switch_set_calls;
	assert(usbradioplus_hardware_mixer_poc_apply(&mixer, 10U, 10U, 10U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	mixer.sidetone.path_count = RPTADV_AUDIO_CM119_MIXER_PATH_CAPACITY + 1U;
	assert(!usbradioplus_hardware_mixer_poc_sidetone_available(&mixer));
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT);
	mixer = saved;
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Failures at each open stage and muting preserve cleanup and retry state. */
static void test_open_stages_and_sidetone_mute_failure(void)
{
	struct usbradioplus_hardware_adapter adapter = ready_adapter();
	struct usbradioplus_hardware_mixer_poc mixer = {0};
	unsigned int failed;
	for (failed = 0U; failed < 4U; ++failed) {
		reset_fake_adapter();
		fake_fail_open_at = failed;
		assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
		       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
		assert(fake_close_calls == failed && !mixer.opened);
	}
	reset_fake_adapter();
	fake_path_mode = FAKE_PATH_SIDETONE_PRESENT;
	assert(usbradioplus_hardware_mixer_poc_open(&mixer, &adapter) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 1U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	fake_fail_switch_at = fake_switch_set_calls;
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR);
	assert(!mixer.sidetone_state_valid);
	fake_fail_switch_at = UINT32_MAX;
	assert(usbradioplus_hardware_mixer_poc_set_sidetone(&mixer, 0U, 0U) ==
	       USBRADIOPLUS_HARDWARE_ADAPTER_OK);
	usbradioplus_hardware_mixer_poc_close(&mixer);
}

/** @brief Run the direct semantic mixer bridge test suite. */
int main(void)
{
	test_semantic_open_and_controls();
	test_absent_rx_compatibility_switch_preserves_apply();
	test_present_rx_compatibility_switch_is_enabled();
	test_missing_or_malformed_controls_fail_closed();
	test_partial_open_is_released();
	test_rx_compatibility_open_failure_is_released();
	test_rx_compatibility_switch_failure_is_reported();
	test_legacy_gain_steps();
	test_legacy_gain_failures_and_wide_range();
	test_inconsistent_readback_fails_closed();
	test_sidetone_gain_switch_and_cache();
	test_sidetone_unavailable_and_invalid_requests();
	test_sidetone_failures_and_retry();
	test_public_control_boundaries();
	test_open_stages_and_sidetone_mute_failure();
	puts("hardware mixer POC tests passed");
	return 0;
}
