# Hardware adapter composition

USBRadioPlus uses one ASL3 channel implementation and two mandatory released
hardware contracts: `rptadv_portaudio_alsa_adapter` and
`rptadv_gpio_adapter`. This implements the hardware boundaries in
rpt_advanced ADR 0028 and the descriptor/SONAME requirements in ADR 0022.
The radio core remains independent of Asterisk and hardware-library types.

Every channel resolves one supported USB identity from its device identifier,
serial, optional topology constraint, or an unambiguous automatic match.
Capture, playback, mixer, and CM119 HID must refer to that same identity.
Only the selected composition owns the device.

## Responsibilities

| Capability | Owner |
| --- | --- |
| Canonical F32 PCM transport and stream lifetime | Released PortAudio/ALSA adapter |
| Device timing and raw capture statistics | Released PortAudio/ALSA adapter |
| RX capture and TX A/B normalized mixer controls | Released PortAudio/ALSA adapter |
| Hardware local-repeat sidetone | Released PortAudio/ALSA semantic mixer paths |
| CM119 PTT, COR/CTCSS, GPIO and EEPROM transport | Released GPIO adapter |
| Configured Linux parallel I/O | Released GPIO adapter |
| Receive qualification, rendering, and desired PTT | Portable radio core |
| Complete app_rpt/RadioPlusAdvanced frame assembly | Single ASL3 adapter |

The PortAudio callback processes its actual bounded frame count using
preallocated state. The app_rpt frame path hands PCM to a non-real-time Asterisk
delivery worker and stays at 8 kHz. The current rpt_advanced controller instead
attaches direct 48 kHz normalized F32 callbacks: receive delivers processed PCM
and keyed state, while independently paced transmit fills the current block and
returns PTT. No PCM callback opens a device, allocates, logs, or calls Asterisk
frame APIs. See [the controller interface](rpt-advanced-interface.md).

The control-plane GPIO worker owns PTT output and publishes input snapshots.
The channel coordinates setup, failure, and ordered cleanup with the shared
worker. Restart stops callbacks and delivery before releasing adapter handles.

Configured nodes share one adapter-owned parallel transport. PP and SETCHAN
requests reach its owner from any node, while per-node input notifications
remain independent. Healthy owner shutdown transfers the open transport and
pending pulses; a transport fault reopens it with configured transmit outputs
released.

## Mixer and EEPROM behavior

The mixer bridge opens required RX capture and TX A/B paths resolved by the
audio adapter. Values use the inclusive 0--999 scale. Available capture and
playback switches are enabled; an advertised receive-compatibility switch is
also enabled.

Optional sidetone paths are muted during setup. Hardware duplex3 requires
both volume and switch support and advertises a normalized maximum of 999.
The GPIO service loop applies its requested gain and enable state; repeated
successful states perform no mixer I/O. Disabling mutes the switch before
setting zero gain. A failed write invalidates the cached state and is reported
to the channel. Unsupported hardware duplex3 fails setup.

EEPROM preserves the established 13-word tuning view at the channel boundary.
The GPIO adapter owns the physical 64-word transfer and checksum. Device
selection, calibration, GPIO, and EEPROM retain the same resolved identity.

## Build and configuration

Install `librptadv-portaudio-alsa-adapter-dev` and
`librptadv-gpio-adapter-dev` alongside the other development dependencies, then
run `make`. The module links their versioned shared objects. Build or startup
fails when a selected contract is absent or incompatible.

Every channel uses the PortAudio/ALSA and CM119 GPIO adapters. Device-selection
changes require a channel restart; audio and GPIO resolve from the same stable
USB identity.

The installed module does not require `res_usbradio.so`. The build rejects
undefined `ast_radio_*` imports and the smoke check loads USBRadioPlus directly.
The hardware-free tests exercise descriptor validation, semantic mixer paths,
sidetone, EEPROM, GPIO, parallel I/O, endpoint selection, callback handoff,
and channel behavior. The complete pull-request gate still requires native
Debian 13 amd64 and arm64 validation.
