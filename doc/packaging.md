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
