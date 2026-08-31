# USBRadioPlus

USBRadioPlus is a replacement for the ASL3 `chan_usbradio` channel driver. It
is intended for operators who want to improve their node's audio quality with
more configurable filtering and additional dynamics processing while retaining
compatibility with existing USB radio interfaces and `usbradio.conf` settings.

This is alpha software. Test it on a non-critical node, retain a working
`chan_usbradio` installation for rollback, and verify radio levels and deviation
with suitable test equipment.

## Build and install

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
- [The channel configuration manual](man/usbradioplus.conf.5) covers radio
  interface, signaling, routing, filtering, and level options.
- [The processing configuration manual](man/usbradioplus-processing.conf.5)
  covers optional audio graphs and their controls.
- [The tuning manual](man/usbradioplus-tune.8) covers the tuning utilities and
  service-monitor procedure.
- [Configuration examples](examples/) contain annotated channel and processing
  configuration files.
- [Packaging notes](doc/packaging.md) describe the upstream archive and future
  Debian packaging interface.
- [Native radio notes](doc/native-radio.md) describe carrier detection,
  signaling, and their test boundary.

After installation, start with `man 7 usbradioplus`.
