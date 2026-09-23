# COP 4 Output Buffer Fix Implementation Plan

> **For agentic workers:** Execute the checked steps in order; each task ends with focused verification.

**Goal:** Add independent opt-in per-radio PortAudio input and output buffer cushions. UCI80 will receive the approved 20 ms output cushion; input buffering remains at its default unless explicitly configured.

**Architecture:** Keep PortAudio's device low-latency default when either setting is zero. Append optional output and input extra-buffer fields to the existing adapter ABI-2 stream configuration; old callers omit them and get the old behavior. USBRadioPlus parses and forwards each per-radio value independently into the released audio adapter. A positive value requests that many milliseconds beyond the adapter's calculated default buffering for only that direction.

**Tech Stack:** Rust, C ABI, PortAudio/ALSA, Cargo tests, Debian packages.

**Spec:** Approved UCI80-only increase of 20 ms, approximately 60 ms total output buffering; no other radio or audio-processing changes.

## Global Constraints

- Preserve current callback behavior, audio format, worker scheduling, and adapter SONAME/ABI major.
- Zero/unset remains the current `defaultLowOutputLatency` or `defaultLowInputLatency` path for that direction.
- No filter, gain, tone-generation, or other node configuration changes.
- Run focused tests for each change; before push run only formatting, lint, and static analysis locally.
- Package only after the merged source passes the required GitHub PR gate; node installation requires explicit approval, granted for UCI80 in this task.

## Review Focus

- Zero input/output settings preserve device-low latency and the existing callback configuration.
- Positive extra buffering is added only to the matching PortAudio input or output request.
- Invalid or excessive values are rejected during configuration resolution and adapter setup.
- ABI-2 callers using the prior shorter structure remain valid.
- Only UCI80 receives the 20 ms setting; other nodes remain unchanged.

---

### Task 1: Adapter ABI and input/output latency requests

**Files:** `rptadv-portaudio-alsa-adapter/include/rptadv_portaudio_alsa_adapter/rptadv_portaudio_alsa_adapter.h`, `src/lib.rs`, `src/tests.rs`, `tests/descriptor_smoke.c`, `doc/developer.dox`.

- [x] Add tests for zero/default behavior, independent positive input/output buffer addition, invalid range, partial new fields, and ABI-2 callers with older struct sizes.
- [x] Append `extra_output_buffer_milliseconds` and `extra_input_buffer_milliseconds` to the stream config. Read previous ABI-2 prefixes safely; absent tail fields mean zero. Validate each field when present.
- [x] Derive PortAudio input and output suggested latencies independently from their default-low latency, configured callback period, and requested extra buffering.
- [x] Run focused adapter tests and the C descriptor smoke test.

### Task 2: USBRadioPlus per-radio configuration and forwarding

**Files:** `rust/core/src/station_config.rs`, `rust/core/src/tests/station_config_tests.rs`, `rust/audio/src/lib.rs`, `rust/audio/src/tests.rs`, `rust/station/src/hardware.rs`, `rust/station/src/tests.rs`, `examples/usbradioplus.conf.sample`, `man/usbradioplus.conf.5`, `doc/developer.dox`, adapter package dependency metadata.

- [x] Add tests for both zero defaults, per-radio input/output parsing, channel override precedence, invalid ranges, and propagation into the audio adapter's FFI configuration.
- [x] Add `hardware_output_extra_buffer_ms` and `hardware_input_extra_buffer_ms`, each with zero default and bounded integer validation; pass them through the Rust audio API to the appended adapter fields.
- [x] Update the sample and user manual with the setting, default, bound, and PortAudio/ALSA caveat.
- [ ] Require the adapter package release containing the appended field; preserve SONAME 2 and ABI 2.
- [x] Run focused core, station, and audio tests.

### Task 3: Release and UCI80 verification

**Files:** Adapter Debian version/changelog and USBRadioPlus Debian version/changelog plus release metadata.

- [ ] Run fast local formatting, lint, and static analysis only before push.
- [ ] Push and use the GitHub PR gate for full validation; merge and release the adapter before releasing USBRadioPlus against it.
- [ ] Install the Debian 13 arm64 packages on UCI80 when reachable; configure only that radio's `hardware_output_extra_buffer_ms = 20` and leave input buffering at zero.
- [ ] Restart Asterisk, repeat COP 4, and verify continuous 1 kHz output with no new output-underflow or late-start increments during the test.
- [ ] If UCI80 remains unreachable, leave its installation/configuration pending and report exact package readiness and connectivity blocker.
