# Changelog

## Unreleased

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
