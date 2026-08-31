# Packaging notes

USBRadioPlus is an upstream-style, non-native project. `make dist` creates the
versioned upstream archive `dist/usbradioplus-VERSION.tar.xz`. A Debian package
should use that archive as `usbradioplus_VERSION.orig.tar.xz` and keep Debian
revisions in a separate `debian.tar.xz` using source format `3.0 (quilt)`.

The Makefile supports the interfaces used by debhelper:

```text
make all
make check
make DESTDIR="$package_stage" prefix=/usr install
make clean
```

Package builds must declare every build dependency and must not run `install.sh`
or `scripts/install-build-deps.sh`. Expected Debian build dependencies include
`asl3-asterisk-dev`, `debhelper-compat`, `pkgconf`, `libasound2-dev`,
`libusb-dev`, `portaudio19-dev`, `libsamplerate0-dev`, `libavfilter-dev`, `libavutil-dev`,
`librnnoise-dev`, `python3`, and `python3-pytest`. The USBRadioPlus repository
publishes RNNoise 0.2 separately as `librnnoise0` and `librnnoise-dev`; the
USBRadioPlus package links to that shared library. The interactive source-install
wrapper may download RNNoise; Make and Debian package builds never do.

The Makefile detects the ASL radio-device API from
`asterisk/res_usbradio.h`. The legacy build uses OSS and libusb-0.1. The modern
build uses the ASL shared-device service, PortAudio, and libusb-1.0. Packagers
may set `ASL_RADIO_API=legacy` or `ASL_RADIO_API=modern` for a controlled build,
but the selected source must be compiled against headers from the matching ASL
package. Both builds use the same configuration, DSP, utilities, and installed
file layout.

Binary modules are tied to the ASL host interface against which they were
built. The Debian package records an exact dependency on that
`asl3-asterisk` version. Published package versions also carry a generation
tag. The original host interface is packaged as `usbradioplus`; the ASL
22.10/app_rpt 3.10 host port is packaged as `usbradioplus-asl3105`. Distinct
binary package names allow both to remain in one Debian suite because reprepro
retains only one version of a package name for each architecture. The modern
package replaces and conflicts with the original package. A new package build
is required when ASL3 Asterisk is updated; do not weaken this dependency unless
ASL publishes a stable module ABI or a suitable virtual ABI package.

Set `SOURCE_DATE_EPOCH` when producing the upstream archive. The `dist` target
normalizes archive ownership, ordering, and timestamps. `distcheck` extracts
the archive into a temporary directory, builds and tests it, and performs a
staged installation. The build and tests do not write to the home directory or
contact the network.

The `debian/` directory builds the `usbradioplus` binary package. Companion
RNNoise packaging is under `packaging/rnnoise/`. GitHub Actions builds both
packages natively for Debian 12 and 13 on amd64 and arm64, publishes signed APT
metadata through GitHub Pages, and verifies installation from the public URL.

`src/usbradioplus_radio.c`, `src/usbradioplus_radio.h`, and `src/txagc/` are
integrated implementation components, not convenience copies selected in
preference to packaged shared libraries. The radio code contains the native
detectors and signaling state machine; txagc contains the audio-processing
implementation. Record their provenance and license status in the eventual
Debian `debian/copyright` file.
