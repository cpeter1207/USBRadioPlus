# USBRadioPlus

USBRadioPlus is a replacement for the ASL3 `chan_usbradio` channel driver. It
is intended for operators who want to improve their node's audio quality with
more configurable filtering and additional dynamics processing while retaining
support for common CM108-, CM109-, and CM119-family USB radio interfaces.

This is alpha software. Test it on a non-critical node, retain a working
`chan_usbradio` installation for rollback, and verify radio levels and deviation
with suitable test equipment.

## Build and install

Signed Debian packages for supported Intel PCs (`amd64`) and 64-bit Raspberry
Pi systems (`arm64`) are available for Debian 12 and 13. The bootstrap installer
detects the ASL3 version and selects the matching package. See
[INSTALL.md](INSTALL.md) for the two installation commands and validation
details.

Download the release tarball to the node, then run:

```text
tar -xf usbradioplus-VERSION.tar.xz
cd usbradioplus-VERSION
sudo ./install.sh
```

The installer obtains the required build tools and dependencies, builds the
module and utilities, runs the hardware-free test suite, and copies the files
into their system locations. It does not load the module, restart Asterisk, or
change `modules.conf` or `rpt.conf`.

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
- [The configuration example](examples/usbradioplus.conf.sample) is an annotated
  multi-channel-capable starting point.
- [Packaging notes](doc/packaging.md) describe the upstream archive and future
  Debian packaging interface.
- [Native radio notes](doc/native-radio.md) describe carrier detection,
  signaling, and their test boundary.
- [Generated source documentation](https://cpeter1207.github.io/USBRadioPlus/docs/)
  provides the Doxygen API and call graphs.
- [Contributing](CONTRIBUTING.md) describes the mandatory quality gate and
  reproducible test containers.

After installation, start with `man 7 usbradioplus`.
