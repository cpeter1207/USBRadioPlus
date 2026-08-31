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
`libusb-dev`, `libsamplerate0-dev`, `libavfilter-dev`, `libavutil-dev`,
`librnnoise-dev`, `python3`, and `python3-pytest`. Debian 12 does not currently
provide `librnnoise-dev` in its standard archive, so an ASL package repository
must provide it or RNNoise must be packaged separately before USBRadioPlus can
be accepted as a policy-compliant Debian package. The interactive source-install
wrapper may download RNNoise; Make and Debian package builds never do.

Set `SOURCE_DATE_EPOCH` when producing the upstream archive. The `dist` target
normalizes archive ownership, ordering, and timestamps. `distcheck` extracts
the archive into a temporary directory, builds and tests it, and performs a
staged installation. The build and tests do not write to the home directory or
contact the network.

The upstream project intentionally does not ship a placeholder `debian/`
directory. Add it when the package maintainer, distribution, changelog version,
and exact dependency versions are known; avoid speculative package metadata.

`src/usbradioplus_radio.c`, `src/usbradioplus_radio.h`, and `src/txagc/` are
integrated implementation components, not convenience copies selected in
preference to packaged shared libraries. The radio code contains the native
detectors and signaling state machine; txagc contains the audio-processing
implementation. Record their provenance and license status in the eventual
Debian `debian/copyright` file.
