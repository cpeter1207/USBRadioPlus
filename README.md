# USBRadioPlus

USBRadioPlus is a replacement for the ASL3 `chan_usbradio` channel driver. It
is intended for operators who want to improve their node's audio quality with
more configurable filtering and additional dynamics processing while retaining
support for common CM108-, CM109-, and CM119-family USB radio interfaces.

Every channel requires the released PortAudio/ALSA and GPIO adapters, bound
to one USB identity. The single ASL3 implementation has no `res_usbradio.so`
dependency.

The `usbradioplus` Debian package contains the ASL3 channel and installs the
matching `usbradioplus-tune` package. The tuner can also be installed independently
for a standalone service, without pulling in Asterisk or the channel adapter.
The channel depends on `libusbradioplus-product1`, the versioned radio-processing and
station library also used by the standalone rpt_advanced controller. The
`libusbradioplus-product-dev` package supplies the C header and pkg-config
metadata for consumers; the shared product has no Asterisk dependency.

The alpha35 SDK adds a stopped-bound final-programme permission callback for
controllers that must suppress revoked audio immediately while retiring DSP
history. It runs on the existing TX owner after the final graph and does not
suppress independent radio signaling. Controllers must separately schedule or
retry mandatory identification that was mixed with suppressed programme.
Installing this SDK does not enable phone-to-RF attachment or receive-window
scheduling.

The alpha34 SDK adds conservative programme-buffering reports and explicit
generation-bound final-TX history retirement through the existing owners.
Controllers keep programme gated until matching completed-render feedback;
unknown/raw graphs remain unqualified for a latency guarantee. See the
[buffering contract](doc/programme-buffering.md).

The alpha33 SDK adds radio-output feedback for controller receive-window
integration. It distinguishes requested PTT, completed output service and
independent readback. Failed audio startup keeps transmit and maintenance
commands unavailable until recovery confirms a successful stop and start.
Receive-window scheduling is being implemented in rpt_advanced.

Local receive, incoming link, and voice/telemetry processing each have a
configurable Peak Limiter followed by mandatory integrated loudness and true
peak metering. The existing `lookahead_limiter_*` keys configure Peak Limiter.
The rolling LUFS AGC defaults to a configurable 400 ms window and preserves
its rolling history during PTT gaps; legacy
Asterisk links advance silence through their existing graph using a channel
timer. See [loudness and gain control](doc/agc.md) for settings and diagnostics.

SDK alpha30 extends the sized link observation record with loudness readings
and adds the silence direction to the existing processing calls. Consumers
must use the matching header, verify the returned record size and ABI version,
and retain the provider while an observer is live. rpt_advanced alpha21 is the
matching standalone consumer. SDK alpha31 adds Earshot receive VAD with
Off, Kerchunk filter, and On modes, retaining speech onset in the existing
PCM delay line. It requires radio ABI 5 and direct callback ABI 6;
rpt_advanced alpha22 is its matching standalone consumer.

This is alpha software. Test it on a non-critical node, retain a working
`chan_usbradio` installation for rollback, and verify radio levels and deviation
with suitable test equipment.

The current `usbradioplus.conf` signaling interface is a clean break. It does
not migrate `chan_usbradio` or earlier settings; start from the shipped sample
and back up the prior configuration.

## Build and install

Signed Debian 13 packages for supported `amd64` and `arm64` systems are
available automatically. The bootstrap installer selects the matching ASL3
package. Debian 12 packages are aspirational and built manually only on request. See
[INSTALL.md](INSTALL.md) for the two installation commands and validation
details.

Download the release tarball to the node, then run:

```text
tar -xf usbradioplus-VERSION.tar.xz
cd usbradioplus-VERSION
sudo ./install.sh
```

The installer verifies the signing key, installs dependencies, builds and tests
the module, and copies files into place without activating Asterisk changes.

Developers and package maintainers can use the standard Makefile directly. See
[INSTALL.md](INSTALL.md) for build, test, staged-install, and source-archive
commands.

## Documentation

- [The module manual](man/usbradioplus.7) covers installation, activation, and
  the available configuration and tuning facilities.
- [The configuration manual](man/usbradioplus.conf.5) covers named radio
  channels, hardware profiles, signaling, routing, and audio processing.
- [The tuning manual](man/usbradioplus-tune.8) covers the tuning utilities and
  service-monitor procedure.
- [Standalone tuning](doc/standalone-tuner.md) covers the private local socket,
  configuration scopes, calibration, EEPROM and diagnostics.
- [The configuration example](examples/usbradioplus.conf.sample) is an annotated
  multi-channel-capable starting point.
- [Packaging notes](doc/packaging.md) describe the upstream archive and current
  Debian packaging interface.
- [Native radio notes](doc/native-radio.md) cover signaling. The removed
  software local-repeat and native parrot modes do not affect native DSP.
- [The release checklist](RELEASE-CHECKLIST.md) lists the automated,
  service-monitor, activation, and rollback evidence required for an alpha.
- [Generated source documentation](https://cpeter1207.github.io/USBRadioPlus/docs/)
  provides Rustdoc for the implementation and Doxygen for the Asterisk C boundary.
- [Contributing](CONTRIBUTING.md) describes push checks, the PR gate, and
  reproducible test containers.

Start with `man 7 usbradioplus`.
