/**
 * @file
 * @brief USBRadioPlus hardware adapter composition API.
 *
 * Adapter-neutral composition of the released audio and GPIO hardware ABIs.
 *
 * This is a control-plane facade.  It resolves one explicit CM119 identity
 * through both released adapter shared objects and retains no Asterisk or
 * USBRadioPlus channel state.  A future channel shim supplies its own native
 * tick and translates its existing configuration into these small requests.
 */

#ifndef USBRADIOPLUS_HARDWARE_ADAPTER_H
#define USBRADIOPLUS_HARDWARE_ADAPTER_H

#include <stdint.h>

#include <rptadv_gpio_adapter/rptadv_gpio_adapter.h>
#include <rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ABI implemented by the internal hardware-composition facade. */
#define USBRADIOPLUS_HARDWARE_ADAPTER_ABI_VERSION 1U

/** @brief Maximum bytes retained for one stable Linux USB topology string. */
#define USBRADIOPLUS_HARDWARE_ADAPTER_USB_PATH_CAPACITY 64U

/** @brief Result returned by a hardware-composition operation. */
enum usbradioplus_hardware_adapter_result {
	/** Operation completed. */
	USBRADIOPLUS_HARDWARE_ADAPTER_OK = 0,
	/** A pointer, structure size, or value was invalid. */
	USBRADIOPLUS_HARDWARE_ADAPTER_INVALID_ARGUMENT = -1,
	/** A selected released adapter has an incompatible descriptor. */
	USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER = -2,
	/** Audio and GPIO discovery did not identify the same selected interface. */
	USBRADIOPLUS_HARDWARE_ADAPTER_IDENTITY_MISMATCH = -3,
	/** The PortAudio/ALSA adapter rejected an operation. */
	USBRADIOPLUS_HARDWARE_ADAPTER_AUDIO_ERROR = -4,
	/** The GPIO adapter rejected an operation. */
	USBRADIOPLUS_HARDWARE_ADAPTER_GPIO_ERROR = -5,
};

/**
 * @brief Immutable selection of one CM119 interface at composition setup.
 *
 * A caller may provide an already resolved USB topology or the same exact
 * legacy device identifier accepted by the audio adapter.  The facade always
 * asks the audio adapter for one canonical topology before it probes GPIO, so
 * ALSA card-number changes cannot make the two adapters select different
 * hardware.  A nonempty serial adds an exact identity check.  The channel
 * counts are the physical PortAudio channel counts requested from the audio
 * adapter; canonical callback PCM remains stereo regardless of either value.
 */
struct usbradioplus_hardware_adapter_config {
	/** Size of this structure supplied by the caller. */
	uint32_t struct_size;
	/** Required facade ABI version. */
	uint32_t abi_version;
	/**
	 * Optional stable Linux USB topology, for example `3-1` or `3-1:1.0`.
	 * This and \c device_identifier are mutually exclusive.
	 */
	const char *usb_port_path;
	/** Optional exact USB serial number. */
	const char *usb_serial;
	/**
	 * Selection policy from `rptadv_audio_usb_selection_policy`.
	 * Exact selection uses one configured identity; automatic selection leaves
	 * every identity string empty and chooses the lowest usable USB ALSA card.
	 */
	uint32_t device_selection_policy;
	/**
	 * Optional legacy-compatible exact audio identifier, such as `hw:2,0`.
	 * The audio adapter resolves it to \c usb_port_path before GPIO opens.
	 */
	const char *device_identifier;
	/** One `rptadv_gpio_cm119_profile` value. */
	uint32_t cm119_profile;
	/** Nonzero inverts logical PTT at the CM119 boundary. */
	uint32_t ptt_inverted;
	/** Requested physical input channel count: one or two. */
	uint32_t input_device_channels;
	/** Requested physical output channel count: one or two. */
	uint32_t output_device_channels;
	/** Ordinary CM119 GPIO bits that are configured as outputs. */
	uint32_t gpio_output_enable_mask;
	/** Initial logical values for configured ordinary GPIO outputs. */
	uint32_t gpio_output_initial_mask;
};

/**
 * @brief One resolved hardware composition owned by a channel control plane.
 *
 * The caller zero-initializes this object, prepares it once, and closes it
 * after stopping its audio stream.  The facade does not own a PortAudio stream
 * because the caller owns its real-time tick callback and stream lifecycle.
 */
struct usbradioplus_hardware_adapter {
	/** Resolved audio adapter descriptor. */
	const struct rptadv_audio_adapter_descriptor *audio;
	/** Resolved GPIO adapter descriptor. */
	const struct rptadv_gpio_adapter_descriptor *gpio;
	/** Canonical topology resolved by the audio adapter. */
	char usb_port_path[USBRADIOPLUS_HARDWARE_ADAPTER_USB_PATH_CAPACITY];
	/** Optional serial copied from the setup configuration. */
	char usb_serial[RPTADV_GPIO_DEVICE_SERIAL_CAPACITY];
	/** Physical input channel count established at setup. */
	uint32_t input_device_channels;
	/** Physical output channel count established at setup. */
	uint32_t output_device_channels;
	/** Selected CM119 GPIO profile. */
	uint32_t cm119_profile;
	/** Selected PTT inversion. */
	uint32_t ptt_inverted;
	/** Selected ordinary-GPIO output enable mask. */
	uint32_t gpio_output_enable_mask;
	/** Selected initial ordinary-GPIO output mask. */
	uint32_t gpio_output_initial_mask;
	/** Exact ALSA and PortAudio endpoints resolved by the audio adapter. */
	struct rptadv_audio_usb_device_selection audio_selection;
	/** CM119 identity independently probed by the GPIO adapter. */
	struct rptadv_gpio_device_info gpio_info;
	/** Open CM119 HID device, or NULL before GPIO open. */
	struct rptadv_gpio_device *gpio_device;
	/** Open parallel transport, or NULL when it is not configured. */
	struct rptadv_gpio_parallel_device *parallel_device;
};

/** @brief One adapter-owned ALSA mixer control retained by a channel. */
struct usbradioplus_hardware_adapter_mixer {
	/** Descriptor that owns \c mixer. */
	const struct rptadv_audio_adapter_descriptor *audio;
	/** Open ALSA simple-mixer control. */
	struct rptadv_audio_mixer *mixer;
};

/**
 * @brief Configuration for one named ALSA simple-mixer element.
 *
 * Existing channel code discovers several semantic paths.  During migration it
 * creates one facade control for each selected path, preserving the current
 * element name, index, channel, direction, and switch/volume semantics.
 */
struct usbradioplus_hardware_adapter_mixer_config {
	/** Size of this structure supplied by the caller. */
	uint32_t struct_size;
	/** ALSA simple-mixer element name. */
	const char *element;
	/** ALSA element index. */
	uint32_t element_index;
	/** One `rptadv_audio_mixer_channel` value. */
	uint32_t channel;
	/** One `rptadv_audio_mixer_direction` value. */
	uint32_t direction;
};

/**
 * @brief Check descriptor ABI compatibility without accessing hardware.
 * @param audio Audio adapter descriptor.
 * @param gpio GPIO adapter descriptor.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_validate(const struct rptadv_audio_adapter_descriptor *audio,
				       const struct rptadv_gpio_adapter_descriptor *gpio);

/**
 * @brief Prepare one explicitly selected audio and GPIO composition.
 * @param adapter Zero-initialized composition to populate.
 * @param config Immutable selected hardware identity and wiring.
 * @param audio Released audio adapter descriptor.
 * @param gpio Released GPIO adapter descriptor.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This control-plane operation probes both adapters and rejects a configured
 * serial mismatch before either adapter claims a device.  It does not open
 * HID, a parallel port, a mixer, or a PortAudio stream.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_prepare(struct usbradioplus_hardware_adapter *adapter,
				      const struct usbradioplus_hardware_adapter_config *config,
				      const struct rptadv_audio_adapter_descriptor *audio,
				      const struct rptadv_gpio_adapter_descriptor *gpio);

/**
 * @brief Prepare a composition from the released linked adapter shared objects.
 * @param adapter Zero-initialized composition to populate.
 * @param config Immutable selected hardware identity and wiring.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_prepare_released(
	struct usbradioplus_hardware_adapter *adapter,
	const struct usbradioplus_hardware_adapter_config *config);

/**
 * @brief Open and exclusively claim the prepared CM119 HID interface.
 * @param adapter Prepared composition.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This is control-plane work and must not execute in a PCM callback.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_open_gpio(struct usbradioplus_hardware_adapter *adapter);

/**
 * @brief Publish a prepared CM119 PTT/GPIO action without performing HID I/O.
 * @param adapter Prepared composition with an open GPIO interface.
 * @param action Prepared logical output action.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This forwards the GPIO adapter's lock-free publication operation.  Its
 * paired \c usbradioplus_hardware_adapter_service_gpio call belongs to the
 * configured non-real-time HID service owner.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_publish_gpio(struct usbradioplus_hardware_adapter *adapter,
					   const struct rptadv_gpio_output_action *action);

/**
 * @brief Schedule independent timed CM119 PTT/GPIO baseline-XOR pulses.
 * @param adapter Prepared composition with an open GPIO interface.
 * @param action Per-output inversion, duration, and cancellation request.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This is a lock-free publication operation.  The configured non-real-time
 * GPIO service owner starts and expires the requested monotonic deadlines in
 * its next service cycle; this facade performs no HID I/O.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_schedule_gpio_inverting_pulse(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_cm119_scheduled_inverting_pulse_action *action);

/**
 * @brief Flush a published CM119 action and sample one HID input report.
 * @param adapter Prepared composition with an open GPIO interface.
 * @param inputs Optional latest input snapshot.
 * @param stats Optional latest GPIO activity snapshot.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This performs HID I/O and is not callable from a PCM callback.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_service_gpio(struct usbradioplus_hardware_adapter *adapter,
					   struct rptadv_gpio_input_snapshot *inputs,
					   struct rptadv_gpio_device_stats *stats);

/**
 * @brief Read the established CM119 tuning EEPROM through the HID owner.
 * @param adapter Prepared composition with an open GPIO interface.
 * @param image EEPROM image to fill.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_read_eeprom(struct usbradioplus_hardware_adapter *adapter,
					  struct rptadv_gpio_eeprom_image *image);

/**
 * @brief Write the established CM119 tuning EEPROM through the HID owner.
 * @param adapter Prepared composition with an open GPIO interface.
 * @param image EEPROM image to validate and program.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_write_eeprom(struct usbradioplus_hardware_adapter *adapter,
					   struct rptadv_gpio_eeprom_image *image);

/**
 * @brief Create a PortAudio stream bound to the prepared physical endpoints.
 * @param adapter Prepared composition.
 * @param config Native-tick stream configuration; device indexes are replaced.
 * @param stream Receives the exclusively owned stream.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * The caller controls start, stop, and destruction.  It must use the same
 * physical channel counts selected during composition preparation.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_create(const struct usbradioplus_hardware_adapter *adapter,
					    const struct rptadv_audio_stream_config *config,
					    struct rptadv_audio_stream **stream);

/**
 * @brief Read the audio adapter's lock-free raw-device statistics snapshot.
 * @param adapter Prepared composition that owns the selected audio descriptor.
 * @param stream Open stream returned by \c usbradioplus_hardware_adapter_stream_create.
 * @param statistics Caller-sized snapshot to fill.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * The facade does not reinterpret peak, RMS, clipping, queue, or device-error
 * fields.  The Asterisk compatibility shim translates this explicit hardware
 * boundary snapshot to its existing UI counters outside the PCM callback.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_get_stats(const struct usbradioplus_hardware_adapter *adapter,
					       const struct rptadv_audio_stream *stream,
					       struct rptadv_audio_stream_stats *statistics);

/**
 * @brief Read actual PortAudio timing for one stream created by this composition.
 * @param adapter Prepared composition that owns the selected audio descriptor.
 * @param stream Open stream returned by \c usbradioplus_hardware_adapter_stream_create.
 * @param timing Caller-sized timing snapshot to fill.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * Timing is a control-plane observation for playout-delay reporting.  It is
 * deliberately not callable from a native PCM callback and does not alter the
 * stream, its device ownership, or callback scheduling.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_stream_get_timing(const struct usbradioplus_hardware_adapter *adapter,
						const struct rptadv_audio_stream *stream,
						struct rptadv_audio_stream_timing *timing);

/**
 * @brief Discover the prepared CM119 interface's semantic ALSA mixer paths.
 * @param adapter Prepared composition that owns the selected audio descriptor.
 * @param paths Caller-sized semantic path result to fill.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * The result retains the adapter's legacy-compatible RX, TX A/B, sidetone,
 * and optional receive-compatibility path classification.  It is a
 * control-plane query: it opens no retained mixer controls and changes no
 * hardware state.  An audio adapter predating this append-only descriptor
 * member returns \c USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_cm119_mixer_paths_resolve(
	const struct usbradioplus_hardware_adapter *adapter,
	struct rptadv_audio_cm119_mixer_paths *paths);

/**
 * @brief Open one named ALSA mixer path on the prepared USB interface.
 * @param adapter Prepared composition.
 * @param config Semantic mixer path preserved from the existing channel configuration.
 * @param mixer Zero-initialized mixer handle to populate.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_open(
	const struct usbradioplus_hardware_adapter *adapter,
	const struct usbradioplus_hardware_adapter_mixer_config *config,
	struct usbradioplus_hardware_adapter_mixer *mixer);

/**
 * @brief Set one prepared mixer gain on the established normalized 0–999 scale.
 * @param mixer Open mixer handle.
 * @param value Inclusive normalized gain value from zero through 999.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_set_normalized(
	struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t value);

/**
 * @brief Apply the established CM119 RX/DAC gain-step mapping through the released adapter.
 * @param mixer Open mixer handle.
 * @param value Existing USBRadioPlus gain setting from zero through 999.
 * @return A facade result code; unavailable ranges or writes report an audio error.
 *
 * The physical value is floor(value * maximum / 1000), without changing the
 * generic adapter's endpoint-preserving normalized API or the configured gain.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_set_legacy_level(
	struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t value);

/**
 * @brief Read one prepared mixer gain on the established 0–999 scale.
 * @param mixer Open mixer handle.
 * @param value Receives the inclusive normalized gain value.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_get_normalized(
	const struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t *value);

/**
 * @brief Set one prepared ALSA capture or playback switch.
 * @param mixer Open mixer handle.
 * @param enabled Zero disables and one enables the selected path.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_mixer_set_switch(struct usbradioplus_hardware_adapter_mixer *mixer,
					       uint32_t enabled);

/**
 * @brief Read one prepared ALSA capture or playback switch.
 * @param mixer Open mixer handle.
 * @param enabled Receives zero when disabled and one when enabled.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_mixer_get_switch(
	const struct usbradioplus_hardware_adapter_mixer *mixer, uint32_t *enabled);

/**
 * @brief Close one open mixer handle.
 * @param mixer Mixer handle to close; NULL is accepted.
 */
void usbradioplus_hardware_adapter_mixer_close(struct usbradioplus_hardware_adapter_mixer *mixer);

/**
 * @brief Open one explicit parallel-port transport.
 * @param adapter Prepared composition.
 * @param config Explicit ppdev/raw-I/O transport configuration.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_open_parallel(struct usbradioplus_hardware_adapter *adapter,
					    const struct rptadv_gpio_parallel_config *config);

/**
 * @brief Publish a prepared parallel-port output action without bus I/O.
 * @param adapter Prepared composition with an open parallel transport.
 * @param action Prepared output and optional pulse action.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_publish_parallel(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_parallel_output_action *action);

/**
 * @brief Schedule independent timed baseline-XOR pulses on parallel data pins.
 * @param adapter Prepared composition with an open parallel transport.
 * @param action Per-pin inversion, duration, and cancellation request.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This is a lock-free publication operation.  The configured non-real-time
 * parallel service owner starts and expires the requested monotonic deadlines
 * in its next service cycle; this facade performs no port I/O.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_schedule_parallel_inverting_pulse(
	struct usbradioplus_hardware_adapter *adapter,
	const struct rptadv_gpio_parallel_scheduled_inverting_pulse_action *action);

/**
 * @brief Latch one legacy active-low parallel binary channel selection.
 * @param adapter Prepared composition with an open parallel transport.
 * @param channel Legacy four-bit active-low channel value.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This is an optional append-only GPIO-adapter operation.  The caller must
 * serialize it with \c usbradioplus_hardware_adapter_service_parallel and
 * must not call it from a native PCM tick.  An older released GPIO adapter
 * reports \c USBRADIOPLUS_HARDWARE_ADAPTER_INCOMPATIBLE_ADAPTER rather than
 * silently changing the legacy parallel-port behavior.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_set_parallel_binary_channel(
	struct usbradioplus_hardware_adapter *adapter, uint8_t channel);

/**
 * @brief Program one legacy RTX synthesizer through the parallel transport.
 * @param adapter Prepared composition with an open parallel transport.
 * @param rx_frequency_hz Receive frequency in hertz.
 * @param tx_frequency_hz Transmit frequency in hertz.
 * @param transmitting Nonzero selects the established transmit word.
 * @param high_power Legacy compatibility value retained unchanged.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * The GPIO adapter owns the exact established 20-bit, most-significant-bit
 * first serial sequence and its settling timing.  This is control-plane work,
 * serialized with the parallel service cycle, never a native PCM operation.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_program_parallel_rtx(
	struct usbradioplus_hardware_adapter *adapter, uint32_t rx_frequency_hz,
	uint32_t tx_frequency_hz, uint32_t transmitting, uint32_t high_power);

/**
 * @brief Immediately clear legacy RTX transmit and power outputs.
 * @param adapter Prepared composition with an open parallel transport.
 * @return A \c usbradioplus_hardware_adapter_result value.
 *
 * This optional append-only GPIO-adapter operation preserves a programmed
 * synthesizer word while releasing transmit without a new serial sequence.
 * The caller serializes it with parallel service and never calls it from a
 * native PCM tick.
 */
enum usbradioplus_hardware_adapter_result usbradioplus_hardware_adapter_clear_parallel_rtx_transmit(
	struct usbradioplus_hardware_adapter *adapter);

/**
 * @brief Apply and sample one configured parallel-port service cycle.
 * @param adapter Prepared composition with an open parallel transport.
 * @param inputs Optional latest parallel input snapshot.
 * @param stats Optional latest parallel activity snapshot.
 * @return A \c usbradioplus_hardware_adapter_result value.
 */
enum usbradioplus_hardware_adapter_result
usbradioplus_hardware_adapter_service_parallel(struct usbradioplus_hardware_adapter *adapter,
					       struct rptadv_gpio_parallel_input_snapshot *inputs,
					       struct rptadv_gpio_parallel_stats *stats);

/**
 * @brief Close GPIO and parallel hardware after any caller-owned stream stops.
 * @param adapter Composition to close; NULL is accepted.
 */
void usbradioplus_hardware_adapter_close(struct usbradioplus_hardware_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
