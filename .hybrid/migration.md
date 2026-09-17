# Rust host migration checkpoint

Resume b5185791 without changing required radio behavior. Reconcile the tested
f0e07559 and 5bb01a39 repairs before validating the migrated host. Do not restore
substantive C implementation when adapting those repairs.

## Design

Rust owns lifecycle, tuning/CLI, reload, channel/direct RX/TX and dispatch.
C remains only for Asterisk-required module metadata and forwarding exports.
Reuse the station, radio, ring, FFmpeg, hardware and audio adapters. Shared public
components remain versioned dynamic dependencies. The architecture source of
truth is rpt_advanced/doc/architecture, particularly ADRs 0013, 0020, 0022 and 0040.
No wishlist features, push, release or node deployment belong to this resume.

## Acceptance

- [ ] Reproduce each regression before its fix; pass affected tests afterward.
- [ ] Reject unsupported direct attachment rather than falsely accepting it.
- [ ] Retain the 10 Hz notch and detected 55 Hz tail-notch behavior.
- [ ] Keep current descriptor and package contracts synchronized and fail safely.
- [ ] Verify migrated lifecycle, tuning, reload and direct audio behavior.
- [ ] Pass the full GitHub PR gate before merge; it has not been run for this work.

The Hybrid recipe supports targeted development checks, not a replacement release
system. The maintained companion-repository workflows remain authoritative.
Update developer docs with code; defer operator documentation until a PR.
