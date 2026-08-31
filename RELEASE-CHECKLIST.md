# Alpha release checklist

## Automated evidence

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

## Compatibility evidence

- [x] Every option in `tests/data/legacy-options.txt` is recognized (67 options).
- [x] Every explicit legacy default initializer matches the baseline
  chan_usbradio source; legacy fields without initializers remain zero-filled.
- [x] Legacy integer filter selectors reproduce every XPMR table cutoff and
  selector-zero fallback behavior.
- [x] Exact-frequency and yes/no filter forms have positive and invalid-input tests.
- [x] RX, TX voice, auxiliary, CTCSS, squelch, configuration-save, and EEPROM
  command paths are covered; the mixer-A auxiliary setter defect is fixed.
- [x] Missing RadioPlus-only settings leave all optional stages disabled.
- [x] Native receive preserves `rxvoiceadj` scaling and `rxsquelchdelay`
  timing while the native detector receives the undelayed pre-squelch copy.
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
- [ ] TX voice and CTCSS deviation, clipping, and occupied bandwidth recorded.
- [ ] Mixer A/B modes and PTT polarity verified.
- [ ] Normal app_rpt audio verified with `duplex3=0`.
- [ ] `duplex3mode=hardware` verified at 0, intermediate, and 999 levels.
- [ ] `duplex3mode=software` verified through the native 48 kHz local route at 0, intermediate, and 999 levels.
- [ ] USB disconnect/reconnect and Asterisk restart verified.
- [ ] Thirty-minute receive and keyed soak completed without growing errors.

The software-only checklist is complete. Hardware measurements remain release
risks and must be recorded during alpha testing. Deployment of this refactor
still requires Chris's explicit approval.
