# Changelog

## Unreleased

## 0.1.0~alpha9 - 2026-09-04

- Replaces the remaining XPMR dependencies with native radio signaling, CTCSS, squelch, hardware control, and 48 kHz audio handling.
- Expands the processing configuration and accessible tuning utility with unified hardware, duplex, Asterisk, diagnostics, parrot, calibration, equalizer, de-esser, and multiband-limiter controls.
- Improves duplex-3 PTT behavior, native echo/parrot routing, DTMF muting, transmit FIFO recovery, filter behavior, fresh-install defaults, and CM119 calibration.
- Builds shared sources as linked objects and accelerates the required Debian 12/13 amd64/arm64 quality, coverage, package, and container verification.
- Publishes updated Debian packages, release archive, documentation, Doxygen output, and clean/install-test container images.

## 0.1.0~alpha8 - 2026-09-01

Mute DTMF in the native parrot processed recording while preserving the unmuted app_rpt detector feed.

## 0.1.0~alpha7 - 2026-09-01

Fixes elastic transmit FIFO startup and recovery so short telemetry is preserved, buffered audio drains before PTT release, and independent app_rpt and CM119 clocks retain the full safety margin without audible dropouts.

## 0.1.0~alpha6 - 2026-09-01

Normalizes de-esser maximum-reduction behavior across the FFmpeg versions shipped by Debian 12 and Debian 13. Includes the equalizer and de-esser processing, tuner, documentation, validation, and performance coverage introduced in alpha5.

## 0.1.0~alpha5 - 2026-09-01

Adds configurable three-band equalization and split-band de-essing to local receive, linked audio, and voice/telemetry processing. Both stages are reorderable, strictly validated, documented, and exposed through the processing tuner, with signal-response, graph-permutation, performance, and compatibility coverage.

## 0.1.0~alpha4 - 2026-09-01

Improves local receive filtering. The PL notch now follows only the currently decoded CTCSS tone, uses a 5 Hz higher-order response, and provides at least 50 dB rejection at the TIA-603 +/-0.5% tolerance edges; obsolete comb mode is removed. Adds a configurable 20-5000 Hz brick-wall receive band-pass immediately after squelch gating and before PL filtering, with matching tuner controls, documentation, strict validation, regression tests, and real-time performance coverage.

## 0.1.0~alpha3 - 2026-08-31

Corrects local PL-filter ordering and mode handling; consolidates transmitter filtering into one final brick-wall band-pass with explicit processing settings and legacy txhpf/txlpf fallback; removes redundant link and transmitter high-pass stages; expands tune controls, documentation, installer validation, and regression coverage.

- Corrects local PL filtering so high-pass, notch, and comb modes run after
  receiver qualification and before RNNoise and dynamics.
- Uses one final transmitter brick-wall band-pass; `txhpf` sets its low edge
  and `txlpf` sets its high edge when processing-file cutoffs are absent.
- Removes the separate link and transmitter voice high-pass stages and their
  configuration options.
- Makes explicit transmitter-tail filter settings authoritative even when
  voice/telemetry dynamics are disabled.
- Expands processing-tuner filter controls, examples, manual pages, strict
  validation, and regression coverage.
- Adds a strictly validated bootstrap installer that detects supported Debian,
  architecture, and ASL3 combinations and installs the matching signed package.
- Verifies the repository key, package origin, architecture, exact ASL runtime
  dependency, and simulated APT transaction before installation.

## 0.1.0~alpha2 - 2026-08-31

- Supports both ASL 22.9/app_rpt 3.9 OSS/libusb-0.1 and ASL 22.10/app_rpt 3.10 shared-device/PortAudio/libusb-1.0 host interfaces.\n- Automatically selects the matching source port from the installed ASL development headers.\n- Publishes generation-tagged Debian packages with an exact ASL runtime dependency so incompatible modules cannot be installed.\n- Retains the same native USBRadioPlus DSP, radio signaling, configuration, and tuning utilities on both host interfaces.\n- Adds strict dual-generation builds, package validation, archive checks, and regression tests.

## 0.1.0~alpha1 - 2026-08-31

- Initial USBRadioPlus alpha release.
- Provides a drop-in chan_usbradio replacement with configurable native audio filtering and dynamics processing.
- Includes configuration examples, manual pages, accessible tuning utilities, build/install tooling, and hardware-independent automated tests.
- Intended for controlled alpha testing on ASL3; review the documentation and back up node configuration before activation.

- Initial public development release.
