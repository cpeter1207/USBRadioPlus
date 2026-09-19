# Retirement of native mode and native parrot

Status: Implemented in the alpha19 candidate; release verification pending — 2026-09-19.

USBRadioPlus removes its driver-native software local-repeat mode and native
parrot. They are not needed by rpt_advanced and are no longer supported ASL3
operating modes. Released alpha18 selected software repeat with
`duplex_local_repeat_mode=software`; native parrot used that route with echo
enabled. There was no separate `native_mode` setting.

This decision does **not** remove the shared 48 kHz DSP, squelch, CTCSS/DCS,
audio processing, hardware adapters, useful native audio statistics, or the
distinct `RadioPlusAdvanced` controller transport. Ordinary app_rpt audio and
legacy echo, and hardware local repeating, are separate retained paths.
The current Rust implementation has independently paced PortAudio receive and
transmit callbacks, direct rpt_advanced attachment, and prepared generations for
transactional reload and rollback. The broader local/link/telemetry inbound-ring
topology and verified shared-clock optimization remain separate architecture
work. Native-mode retirement does not establish completion of those ADRs.

Mode-only code/state, native-parrot storage and producers, mode selections,
and tuning controls are removed. `duplexmode` and `duplex_local_repeat_mode`
are silently ignored, regardless of value, as required by ADR 0039. They do not
select software repeat, native parrot, or alter configured hardware repeating.
Hardware local repeat, configured with `duplex_local_repeat_level`, retains its
configured level, supported-device checks, and controller-transport restrictions.
The same rule applies on initial load and reload; tuning saves the hardware level.

USBRadioPlus no longer calls or requires shared native-repeat/parrot operations.
Initial-alpha backward compatibility is not required (rpt_advanced ADR 0040).
The shared library removes those operations without compatibility-only
descriptor slots. It uses descriptor ABI 4 and `librptadvradio.so.4`; current
consumers and dependency metadata reject mismatched artifacts.

The authoritative architecture decision is rpt_advanced ADR 0039,
`doc/architecture/decisions/0039-retire-usbradioplus-native-mode.md`, with its
implementation tracked in that project's WISHLIST until verification completes.
Deployment requires separate authorization after release verification.
