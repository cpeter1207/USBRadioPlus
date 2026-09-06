# Alpha release checklist

## Automated evidence

When arm64 tests run through instruction-set emulation, set
`USBRADIOPLUS_TEST_BLOCK_LIMIT_MS` to the measured emulation allowance. This
changes only the wall-clock assertion; every permutation and audio-result test
still runs.

- [x] Python release tests pass (35 tests, 2026-08-30).
- [x] Static artifact validation passes (2026-08-30).
- [x] C DSP tests compile and pass with `-Wall -Wextra -Werror` (7 binaries,
  including all 24 dynamics permutations, 2026-08-30).
- [x] Elastic link clock recovery passes modeled twenty-minute and actual SRC
  integration tests at both -1000 and +1000 ppm without an underrun.
- [x] Channel module builds with `-Wall -Wextra -Werror` against the running
  node's installed ASL3/Asterisk headers and libraries (2026-08-30).
- [x] Channel translation unit compiles strictly and links as an ELF64 module
  against the official matching `asl3-asterisk-dev` package and dependencies.
- [x] Installer dry run confirms the exact install manifest (2026-08-30).
- [x] Installer source audit confirms no activation, configuration edit, or restart.
- [x] Versioned source archive passes extracted-tree build, test, and staged-install checks.
- [x] One-command node wrapper passes an extracted-tarball staged installation.

## Configuration evidence

- [x] One `usbradioplus.conf` defines named radio channels and their hardware,
  Asterisk, duplex, diagnostics, and processing settings.
- [x] Flat sections provide shared defaults and scoped or explicitly referenced
  profiles override them for each named channel.
- [x] Unknown sections, options, profiles, values, and graph orders reject reload
  without replacing the active configuration.
- [x] RX, TX voice, auxiliary, CTCSS, squelch, configuration-save, and EEPROM
  command paths are covered; the mixer-A auxiliary setter defect is fixed.
- [x] Missing optional processing settings leave their stages disabled.
- [x] Native receive preserves configured squelch-delay timing while the native
  detector receives the undelayed pre-squelch copy.
- [x] The bundled XPMR directory and dependency are absent. USBRadioPlus owns
  COR, VOX, CTCSS decoding, measurements, and transmitter signaling state.
- [x] Every allowed graph permutation executes in the declared order.
- [x] Duplicate, unknown, empty, omitted-enabled, and fixed-stage graph names
  are rejected by executable parser tests.
- [x] Every reload validation path precedes the locked settings commit; failed
  reloads retain the preceding live configuration.

## Hardware evidence

- [x] Native DSP COR and CTCSS decoding verified on air with a received
  CTCSS-plus-voice transmission (confirmed by Chris, 2026-08-30).
- [ ] COS and CTCSS sources and polarities verified.
- [ ] RX peak/RMS, ADC rails, SINAD, and squelch margin recorded.
- [ ] Sample-rate DSP squelch: strong-carrier abrupt loss closes under 10 ms;
  weak-carrier loss holds about 150 ms; gradual fades restore flutter hold.
  Follow the receiver check in `doc/native-radio.md`, including CTCSS and
  software-repeat checks. Analog MICOR equivalence still needs bench comparison.
- [ ] TX voice and CTCSS deviation, clipping, and occupied bandwidth recorded.
- [ ] Mixer A/B modes and PTT polarity verified.
- [ ] Normal app_rpt audio verified with `duplex3=0`.
- [ ] `duplex3mode=hardware` verified at 0, intermediate, and 999 levels.
- [ ] `duplex3mode=software` verified through the native 48 kHz local route at 0, intermediate, and 999 levels.
- [ ] USB disconnect/reconnect and Asterisk restart verified.
- [ ] Continuous device output verified on a dummy load: silence on both DAC
  channels while idle; no write-cadence change across PTT, COS, or CTCSS-decode
  transitions; normal link, telemetry, voice, and CTCSS output while keyed.
  Check device underruns and write/drop counters during idle, sustained audio,
  and normal host load. Repeat after reopening the USB device.
- [ ] Thirty-minute receive and keyed soak completed without growing errors.

The software-only checklist is complete. Hardware measurements remain release
risks and must be recorded during alpha testing. Deployment of this refactor
still requires Chris's explicit approval.
