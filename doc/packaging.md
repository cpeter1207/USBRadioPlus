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
`asl3-asterisk-dev`, `debhelper-compat`, `pkg-config`,
`libsamplerate0-dev`, `libavfilter-dev`, `libavutil-dev`,
`ladspa-sdk`, `librnnoise-dev`, `librate-adjusting-pcm-ring-dev (>= 1.0.1)`,
`librptadv-samplerate-adapter-dev (>= 0.1.0~alpha1)`,
`librptadvradio-dev (>= 0.1.0~alpha1)`,
`librptadv-ffmpeg-adapter-dev (>= 0.1.0~alpha1)`,
`librptadv-portaudio-alsa-adapter-dev (>= 0.1.0~alpha2)`,
`librptadv-gpio-adapter-dev (>= 0.1.0~alpha1)`, `python3`, and
`python3-pytest`. The USBRadioPlus repository
publishes RNNoise 0.2 separately as `librnnoise0` and `librnnoise-dev`; the
USBRadioPlus package links to that shared library. The interactive source-install
wrapper may download RNNoise; Make and Debian package builds never do.

USBRadioPlus links dynamically to the separately released GPL-2.0-only
`rate_adjusting_pcm_ring` library. Build packages require its development
package; installed modules require its matching runtime package. CI installs
digest-verified release packages to test that public ABI deterministically.

The current mono sinc compatibility path also links dynamically to the
separately released `rptadv_samplerate_adapter` library. It owns the
libsamplerate ABI and exchanges normalized F32 PCM; USBRadioPlus retains S16
conversion only at its legacy Asterisk boundary. Build packages require its
development package and installed modules require its matching runtime package.

The DCS and DCS turn-off shapers use the dynamically linked
`rptadv_ffmpeg_adapter` exact-block API. Its development package supplies only
the public F32 descriptor; FFmpeg objects remain inside the shared library.
Other processing graphs remain in the existing channel engine during migration.

The package includes the original gated RMS AGC as a private LADSPA effect:
`/usr/lib/<multiarch>/usbradioplus/usbradioplus_agc.so`. It runs only inside
the shared FFmpeg graph. The build records that installed path in the channel
module and refreshes it when the installation prefix changes. The runtime
requires FFmpeg's `ladspa` filter, included in the supported Debian packages;
`ladspa-sdk` supplies build headers only. Do not place the effect in Asterisk's
module directory or add it to `modules.conf`.

The Makefile always builds `src/chan_usbradioplus.c` against matching ASL3
headers and dynamically links the released audio and GPIO adapter SONAMEs.
There is no resource-module API autodetection or optional hardware build.
The module link check rejects undefined `ast_radio_*` imports. `res_usbradio.so`
is not required for installation or module loading.

Container callers provide a `shared_packages` context containing the released
Debian packages. The workflow pins release tags and verifies every asset's
SHA-256 digest before passing this context. The staged image installs the
development packages; the final image installs only runtime packages and
checks their versioned dependencies. The channel retains ring ABI 1 from
v1.0.1; the audio adapter's private ring ABI 2 is installed alongside it.

Binary modules are tied to the ASL host interface against which they were
built. Debian 13 builds use ASL3 3.9.3 headers and declare exact alternatives
for ASL3 3.9.3 and 3.10.5. Their module build-options checksum and used public
interfaces match; the required gate must load the same artifact under both
runtimes before release. Other host builds retain their exact
`asl3-asterisk` dependency. The single binary package is named `usbradioplus` on
both supported architectures; no resource-module generation selects its name,
sources, dependencies, or installer path. A new package build
is required when ASL3 Asterisk is updated; do not weaken this dependency unless
ASL publishes a stable module ABI or a suitable virtual ABI package.
The unified package conflicts with and replaces the retired
`usbradioplus-asl3105` variant. The installer permits removal of that specific
package and its `usbradioplus-asl3105-dbgsym` companion during replacement,
while continuing to reject ASL changes and unrelated removals.

Set `SOURCE_DATE_EPOCH` when producing the upstream archive. The `dist` target
normalizes archive ownership, ordering, and timestamps. `distcheck` extracts
the archive into a temporary directory, builds and tests it, and performs a
staged installation. The build and tests do not write to the home directory or
contact the network.

The `debian/` directory builds the `usbradioplus` binary package. Companion
RNNoise packaging is under `packaging/rnnoise/`. GitHub Actions builds both
packages natively for Debian 13 on amd64 and arm64, publishes signed APT
metadata through GitHub Pages, and verifies installation from the public URL.
Debian 12 packaging is aspirational and may be built manually only when
explicitly requested; it has no automatic test or staged-install matrix.

`src/usbradioplus_radio.c`, `src/usbradioplus_radio.h`, and `src/txagc/` are
integrated implementation components, not convenience copies selected in
preference to packaged shared libraries. The radio code contains the native
detectors and signaling state machine; txagc contains the audio-processing
implementation. Their provenance and license status are recorded in
`debian/copyright`.
