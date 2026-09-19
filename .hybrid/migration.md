# Rust host migration validation

Resume b5185791 without changing required radio behavior. Reconcile the tested
f0e07559 and 5bb01a39 repairs before validating the migrated host. Do not restore
substantive C implementation when adapting those repairs.

## Design

Rust owns lifecycle, tuning/CLI, reload, channel/direct RX/TX and dispatch.
C remains only for Asterisk-required module metadata and forwarding exports.
Reuse the station, radio, ring, FFmpeg, hardware and audio adapters. Shared public
components remain versioned dynamic dependencies. The architecture source of
truth is rpt_advanced/doc/architecture, particularly ADRs 0013, 0020, 0022 and 0040.
No wishlist features, push, or public release belong to this resume. The owner
approved completing the migration, then native build, installation, and testing
on node 524950. Preserve a matched recovery set and existing radio settings;
do not initiate external link connections for the tests.

## Acceptance

- [x] Reproduce each regression before its fix; pass affected tests afterward.
- [x] Reject unsupported direct attachment rather than falsely accepting it.
- [x] Retain the 10 Hz notch and detected 55 Hz tail-notch behavior.
- [x] Keep current descriptor and package contracts synchronized and fail safely.
- [x] Verify migrated lifecycle, tuning, reload and direct audio behavior.

## Native validation — 2026-09-17

USBRadioPlus implementation `560ee0a6` and the matching rpt_advanced direct
callback ABI 2 implementation `0cf8c582` were built natively and installed on
Debian 13 arm64 node 524950. The rpt_advanced architecture integration at
`2726d390` preserves that implementation and the saved architecture changes.
Only the Asterisk metadata/provider-loading shim remains C; product behavior is
Rust, using the existing released dynamic providers.

The resumed workspace passed 407 Rust tests before the reload repair. All 157
tests in the three affected crates passed after that repair on both amd64 and
arm64, with the final station-lifecycle assertion rerun after its last edit.
Formatting, Clippy, Rustdoc and the applicable C/Python/shell checks passed.
The source archive passed 72 packaging/infrastructure tests, and the C/LADSPA
checks preserved exact AGC parity for 3,228,820 PCM values.

The operator confirmed spoken time, repeat audio and courtesy tone. The final
build then passed three in-place reloads and both modules' unload/load cycle
under one unchanged Asterisk PID. The tuner prerequisite check passed, installed
module/private-library bytes matched the native build, and final status reported
48 kHz operation, zero input/output xruns, zero late callbacks and idle PTT.
Configuration hashes and hardware mixer levels were unchanged. No external links
were connected for validation.

## PR/release gate still pending

This is implementation and native-node validation, not full release approval.
The required full GitHub PR gate has not passed for this migration. Production
coverage gaps in the Rust Asterisk host remain; close them and update operator
documentation before a PR, and require the full gate before merging to main.

The Hybrid recipe supports targeted development checks, not a replacement release
system. The maintained companion-repository workflows remain authoritative.
Update developer docs with code; defer operator documentation until a PR.

## ADR alignment — 2026-09-17

The owner authorized closing the audited ADR 0001/0002/0019/0023/0025/0026/0027
gaps. ADR 0023 is authoritative for telemetry: local commands reply to local
RF, peer commands reply only to their source, and CLI replies stay on the CLI.
ADR 0025's all-local telemetry rule was an intermediate implementation.
This work does not authorize node deployment, publication, or new wishlist features.

Reuse the existing released ring, prepared radio contexts, bounded SPSC queues,
control executor, and generation/hazard ownership. Do not add parallel DSP or
replace released dynamic components. Work through independently tested parts:

- [ ] Resolve missing profile selectors through defaults with useful warnings;
  preserve structural-error rejection and existing lower-level editing contracts.
- [ ] Latch local and peer DTMF muting until command termination and gate delayed
  inbound PCM at the output, using the existing shared ring and command policy.
- [ ] Own local rings, qualification metadata and delay settings in each RPT
  operating generation so same-device reload adopts all three together.
- [ ] Retain USB device leases across normal reload; prepare/warm replacement DSP
  off callback, adopt RX/TX independently, reclaim only after quiescence.
- [ ] Move telemetry policy to station control, original-rate file/speech PCM to
  the single telemetry producer/ring, and route replies according to ADR 0023.
- [ ] Reconcile architecture status, add focused regression tests and Rustdoc,
  run affected checks, and leave the full hosted PR gate explicitly pending.

The sample-associated qualification wording conflicts with immediate queued-tail
suppression. The owner has been asked to resolve this before implementing that
boundary; independent configuration work can proceed meanwhile.
