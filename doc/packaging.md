# Packaging notes

USBRadioPlus is an upstream-style, non-native project. `make dist` creates
`dist/usbradioplus-VERSION.tar.xz`. Debian packaging should use that archive as
`usbradioplus_VERSION.orig.tar.xz` and keep Debian revisions in a separate
`debian.tar.xz` using source format `3.0 (quilt)`.

Debhelper uses the standard interfaces:

```text
make all
make check
make DESTDIR="$package_stage" prefix=/usr install
make clean
```

Package builds declare their dependencies and never run `install.sh` or
`scripts/install-build-deps.sh`. The source wrapper configures the signed
USBRadioPlus repository and installs the same development packages used by the
Debian build.

The only production C source is `src/chan_usbradioplus_shim.c`. It builds
against `asl3-asterisk-dev` and links the Rust Asterisk adapter and the released
provider libraries. The module must retain these versioned dynamic dependencies:

- `libusbradioplus_asterisk.so.1`
- `librate_adjusting_pcm_ring2.so.2`
- `librptadvradio.so.4`
- the released samplerate, FFmpeg, PortAudio/ALSA, GPIO, and RNNoise adapters

The corresponding build packages are `librate-adjusting-pcm-ring2-dev`,
`librptadvradio-dev`, `librptadv-samplerate-adapter-dev`,
`librptadv-ffmpeg-adapter-dev`, `librptadv-portaudio-alsa-adapter-dev`,
`librptadv-gpio-adapter-dev`, and `librptadv-rnnoise-adapter-dev`.

The build rejects undefined `ast_radio_*` imports. `res_usbradio.so` is not a
runtime dependency.

The private `usbradioplus_agc.so.1` LADSPA plugin is implemented in Rust and
installed below `/usr/lib/<multiarch>/usbradioplus/`. FFmpeg loads the stable
`usbradioplus_agc.so` symlink by path. It is not an Asterisk module and must not
be registered with `ldconfig` or added to `modules.conf`.

The installed tuner is the Rust `usbradioplus-tune` binary. Python is used only
by build and release validation and is not a binary-package dependency.

Binary Asterisk modules are tied to the ASL3 interface against which they were
built. Debian 13 packages declare the accepted ASL3 runtime versions in
`debian/rules`. A new package build is required when that interface changes.
The package never edits `modules.conf` or `rpt.conf` and never restarts Asterisk.

Set `SOURCE_DATE_EPOCH` when producing archives. `make dist` normalizes archive
ordering, timestamps, ownership, and generated-file exclusions. `make
distcheck` extracts that archive, builds it, runs the configured test target,
and performs a staged installation in a temporary directory.

GitHub Actions builds and tests Debian 13 natively on amd64 and arm64. Complete
production line and branch coverage is required on amd64. Debian 12 support is
aspirational and is built only when explicitly requested.
