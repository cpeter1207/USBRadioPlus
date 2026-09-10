# Alpha release checklist

This is a checklist for the exact release-candidate commit and artifacts. A
check from an earlier alpha is not evidence for this release. Leave an item
unchecked until its command, test record, or hardware notes identify the commit
or package tested.

USBRadioPlus is alpha software. Do not deploy, change an active node, key a
transmitter, or dispatch the release workflow without Chris's explicit
approval. The installer copies files only: activation and any `rpt.conf` change
remain manual operations described in `usbradioplus(7)`.

## Source and automated gate

- [ ] Review `git status --short`, the complete diff, and generated artifacts.
      Remove recordings, coverage output, package build output, and other
      non-source material before the release commit.
- [ ] Set `VERSION`, `debian/changelog`, and the unreleased `CHANGELOG.md`
      entry for the intended alpha. Confirm the release notes describe only
      changes in this release.
- [ ] Run `make ci` on the release-candidate commit. It must pass strict
      compilation, Ruff, ShellCheck, Clang-Format, Cppcheck, Clang-Tidy,
      Doxygen, all tests, and 100% line and branch coverage, including the
      vendored program-ring gate.
- [ ] Run `make distcheck`, then inspect the extracted archive's staged install.
      Confirm it contains the module, private AGC effect, unified tuner, sample,
      manuals, and the vendored `rate_adjusting_pcm_ring` source, but no build
      output, coverage data, audio captures, or test recordings.
- [ ] Run `python3 tools/validate_release.py`, `make docs`, and the man-page
      render checks. Confirm README, installation instructions, all three man
      pages, the sample, packaging notes, and native-radio notes link to files
      shipped by the archive.
- [ ] Run the Debian 12 and Debian 13 amd64 and arm64 quality/install matrix
      for the same commit. Record each job URL and its exact package or archive
      artifact.

## Clean-slate configuration and signaling

The current configuration interface is a clean break. It does not translate
old `chan_usbradio` or earlier USBRadioPlus signaling names. Start a new
`usbradioplus.conf` from `examples/usbradioplus.conf.sample`, preserve a backup
of any prior configuration, and make manual activation changes only after the
following checks pass.

- [ ] Load the shipped sample through the parser and unified tuner. Confirm
      every displayed setting is a concrete current value, not a legacy
      fallback or an unspecified module default.
- [ ] Verify scoped-profile precedence and strict rejection of an unknown
      section, option, profile, value, or graph stage without replacing the
      prior live configuration.
- [ ] Verify each direction accepts exactly one `signaling_method`: `carrier`,
      `ctcss`, or `dcs`. Confirm invalid combinations are rejected, including
      missing CTCSS source/default tone, missing DCS code, invalid code spelling,
      invalid signal levels, and no hardware output carrying selected signaling.
- [ ] Verify `[ctcss]` settings independently: receive source and decoder gain,
      receive and transmit tone lists, transmit peak dBFS, and all tail modes
      (`no`, `ctcss_phase_shift`, `ctcss_tone_remove`, and `ctcss_tail_tone`).
- [ ] Verify `[dcs]` settings independently: normal and inverted receive and
      transmit codes, peak dBFS, and the 134.4 Hz turn-off tone and duration.
- [ ] Verify the unified tuner can display and edit the new `[receive]`,
      `[transmit]`, `[ctcss]`, and `[dcs]` sections, preserves its
      screen-reader/keyboard behavior, and reports reload errors without
      corrupting the file.

## Real-time audio and distribution behavior

- [ ] Confirm the app_rpt-to-CM119 bridge is the sole sample-at-a-time,
      lock-free SPSC program ring. Its 110 ms occupancy target is clock
      recovery only; it must not delay initial playout, gate PTT, or reserve
      source audio before output.
- [ ] Confirm temporary program-source shortfalls use bounded smooth
      concealment, sustained shortfalls fade to silence, and counters expose
      both shortfalls and producer overflow.
- [ ] Confirm app_rpt input and 48 kHz device output continue while idle with
      silence. Echo playback is the only intended exception to app_rpt program
      admission. PTT, COS, and CTCSS/DCS decode must not alter device-write
      cadence.
- [ ] Confirm native carrier, CTCSS, and DCS callback paths make no Asterisk
      logging calls, take no locks, allocate no memory, and perform no file I/O.
      Confirm they do not execute FFmpeg, RNNoise, or sample-rate conversion.
      Control-plane parsing and construction diagnostics remain allowed.
- [ ] Confirm the hardware callback and native render worker use only bounded,
      preallocated SPSC PCM queues. Exercise empty and full input/output queues,
      worker-start failure, orderly stop, and teardown with an outstanding graph
      reference; record the tests or failure-injection evidence for this commit.
- [ ] Confirm a missing completed worker block produces one silent device block
      without stopping device cadence, wedging queue metadata, changing program
      ring accounting, or changing PTT. Exercise worker lag and a program-ring
      shortfall separately.
- [ ] Confirm PTT is published synchronously from the signaling engine, not from
      rendered audio or either queue. Exercise a worker shortfall and CTCSS/DCS
      turn-off tails; physical PTT must remain asserted until the signaling
      engine releases it.
- [ ] Confirm native statistics are read as coherent worker snapshots and that
      meter reset and echo-clear requests are consumed only between complete
      render frames. Do not accept direct control-plane reads of mutable FFmpeg,
      RNNoise, SRC, or parrot state as evidence.
- [ ] Confirm each incoming-link audiohook callback only exchanges bounded,
      preallocated PCM through its SPSC queues. FFmpeg execution and graph-meter
      updates must occur only in that hook's worker; the callback must not lock,
      allocate, log, or run FFmpeg.
- [ ] Confirm link output is snapshotted before current input is queued, so it
      renders only a complete preceding input frame. Exercise startup, worker
      lag, malformed metadata, and reload generation changes; each must taper
      to silence without replaying stale audio or changing PTT. Record link
      worker input-overflow, output-underflow, malformed-output, and graph
      shortfall counters.
- [ ] Confirm link meter reads use a coherent reader-pinned worker snapshot
      that may be briefly stale or unavailable rather than blocking audio.
      Confirm detach stops and joins every link worker before graph-slot,
      queued-reference, or SPSC-storage destruction.
- [ ] Confirm package and tarball installation neither loads the module nor
      restarts Asterisk, edits `modules.conf`, or edits `rpt.conf`.

## Manual radio and service-monitor tests

Perform RF tests on a dummy load or another authorized test path. Record the
radio, USB interface, ASL version, configuration checksum, test equipment, and
measured results beside each completed item.

- [ ] Calibrate CM119 input gain and DSP squelch with flat discriminator audio.
      Record no-carrier peak/RMS, ADC rails, threshold, hysteresis, and the
      strong-loss and weak-fade closing times.
- [ ] Verify COS sources, CTCSS sources, polarity settings, receive CTCSS
      decoder gain, and receive DCS qualification independently.
- [ ] Verify receiver audio, DTMF decode/muting, local processing, link
      processing, and voice/telemetry processing with stages both disabled and
      enabled as configured.
- [ ] Set TX voice and CTCSS/DCS deviation with a service monitor. Record peak,
      RMS, clipping, occupied bandwidth, pre-emphasis, output assignment, and
      both CM119 DAC levels.
- [ ] Verify carrier, CTCSS, and DCS transmit selections independently. Verify
      CTCSS phase shift, early tone removal, and replacement-tail tone; verify
      DCS's 134.4 Hz turn-off tone and its duration before PTT release.
- [ ] Verify half duplex, normal app_rpt audio, and both local-repeat modes:
      `duplex_local_repeat_mode=hardware` and `software` at zero, intermediate,
      and full repeat levels. Confirm app_rpt, not the program FIFO, owns PTT.
- [ ] Verify continuous DAC output: silence while idle and normal voice, link,
      telemetry, CTCSS, and DCS output while keyed. Check device errors,
      dropped frames, program-ring overflow/shortfall counters, and clock-ratio
      observations during idle, sustained audio, and normal host load.
- [ ] Complete a 30-minute receive and keyed-transmit soak, including USB
      disconnect/reconnect and an Asterisk restart. Record counters before and
      after the soak.

## Activation, rollback, and publication

- [ ] Back up the running Asterisk configuration and retain the working
      `chan_usbradio` module and configuration before activation.
- [ ] Follow `usbradioplus(7)` during a maintenance window: edit
      `modules.conf` to load `chan_usbradioplus.so`, edit only the intended
      `rpt.conf` channel technology, restart Asterisk, and verify the selected
      `RadioPlus/<name>` channel. Do not load both drivers against one USB
      interface.
- [ ] Verify rollback: restore the saved `modules.conf`, `rpt.conf`, and prior
      module/configuration, restart Asterisk, and confirm the original channel
      works before declaring the test reversible.
- [ ] Obtain Chris's explicit approval after reviewing the current automated
      evidence and manual test record.
- [ ] Dispatch the release workflow from the approved commit. Record its
      quality, tag, source archive, package, signature, and GitHub Release URLs.
- [ ] Install the published package on one non-critical supported node and
      repeat startup, signaling, audio, and rollback smoke tests from the
      published artifact rather than a working-tree build.
