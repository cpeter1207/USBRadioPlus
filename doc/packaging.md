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

The Rust Asterisk host generates its narrow bindings from the installed public
Asterisk headers. `libclang-dev` is therefore a build-only dependency; it is
not installed with the binary package.

The only production C source is `src/chan_usbradioplus_shim.c`. It declares
Asterisk metadata, composes the provider manifest, and forwards load, reload,
and unload through the versioned Rust lifecycle descriptor. It builds against
`asl3-asterisk-dev` and links the Rust Asterisk host and the released provider
libraries. The module must retain these versioned dynamic dependencies:

- `libusbradioplus_asterisk.so.1`
- `libusbradioplus_product.so.1`
- `librate_adjusting_pcm_ring3.so.3`
- `librptadvradio.so.4`
- the released samplerate ABI-2, FFmpeg, PortAudio/ALSA, GPIO, and RNNoise adapters

The corresponding build packages are `librate-adjusting-pcm-ring3-dev`,
`librptadvradio-dev`, `librptadv-samplerate-adapter-dev`,
`librptadv-ffmpeg-adapter-dev`, `librptadv-portaudio-alsa-adapter-dev`,
`librptadv-gpio-adapter-dev`, and `librptadv-rnnoise-adapter-dev`.

`chan_usbradioplus.so` and `libusbradioplus_asterisk.so.1` are private,
co-packaged in `usbradioplus`. The separately packaged
`libusbradioplus-product1` contains `libusbradioplus_product.so.1` and its
private AGC plugin; `libusbradioplus-product-dev` contains
`usbradioplus_product.h`, the linker name, and `usbradioplus_product.pc`.
The channel package depends on the exact same-version product runtime.
The product packages have no Asterisk or controller dependency. The metadata
shim validates the lifecycle descriptor size and ABI
before Rust registers either channel technology, so manually mixed artifacts
fail safely instead of partially loading.

The build rejects undefined `ast_radio_*` imports. `res_usbradio.so` is not a
runtime dependency.

The private `usbradioplus_agc.so.1` LADSPA plugin is implemented in Rust and
installed below `/usr/lib/<multiarch>/usbradioplus/` by the product runtime
package. FFmpeg loads the stable
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
