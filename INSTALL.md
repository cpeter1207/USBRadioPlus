# Building and installing

## Signed Debian packages

For a supported ASL3 node on Debian 13, download and run the bootstrap
installer:

```text
curl -fsSLO https://cpeter1207.github.io/USBRadioPlus/install-usbradioplus.sh
sudo sh install-usbradioplus.sh
```

The installer reports the detected Debian release, architecture, and exact
ASL3 Asterisk version before asking for confirmation. It accepts only published
host combinations, verifies the repository signing-key fingerprint, checks the
selected package's architecture and exact ASL dependency, and simulates the APT
transaction before installation. Unsupported or unknown combinations stop with
an error; the installer never upgrades or downgrades ASL to satisfy the module.
Use `sudo sh install-usbradioplus.sh --dry-run` to perform detection without
changing the node. `--yes` permits an explicitly unattended installation.
Debian 12 packaging is aspirational and is produced manually only when
explicitly requested.

The archive signing-key fingerprint is
`A0D5 A79E 0F5C 45E9 E636 7995 0951 502B AC79 5E55`. Installation does not
activate the module, restart Asterisk, or edit `modules.conf` or `rpt.conf`.
The current signaling configuration is a clean break: the installer does not
translate an existing `chan_usbradio` or earlier USBRadioPlus configuration.
Keep a backup and create `usbradioplus.conf` from the shipped sample before
manual activation.

After uploading and extracting the tarball on an ASL3 node, run:

```text
sudo ./install.sh
```

This verifies the shipped repository key, configures the signed project
repository, and installs the required toolchain, development packages, and
released `rate_adjusting_pcm_ring2`, `rptadvradio`, `rptadv_samplerate_adapter`,
`rptadv_ffmpeg_adapter`, `rptadv_portaudio_alsa_adapter`, and
`rptadv_gpio_adapter`, and `rptadv_rnnoise_adapter` ABIs. It
builds RNNoise when Debian does not provide it, runs the complete hardware-free test suite, and installs
USBRadioPlus. It does not activate the module, restart Asterisk, or edit
`modules.conf` or `rpt.conf`.

The RNNoise bootstrap verifies and builds the official v0.2 release archive in
the source tree's `build` directory. This permits installation on hardened
nodes where `/tmp` is mounted `noexec`. It also restores the support header
omitted from the archive's ARM NEON sources. The temporary source is removed
automatically.

USBRadioPlus requires a matching `asl3-asterisk-dev` package, `ladspa-sdk`, and
the libraries listed in `doc/packaging.md`. Developers with those dependencies
already installed may use `sudo ./install.sh --skip-deps`.

USBRadioPlus uses the released `rate_adjusting_pcm_ring2` shared library for
the lock-free native program FIFO and the released `rptadv_samplerate_adapter`
shared library for its mono sinc compatibility conversion. Native DCS shaping
uses the `rptadv_ffmpeg_adapter` shared library. Every channel uses the
`rptadv_portaudio_alsa_adapter` for audio and mixer control and
`rptadv_gpio_adapter` for CM119 HID, EEPROM, and configured parallel I/O.
The source installer
installs matching development packages and runtime dependencies from the signed
project repository. Direct Make users must install those packages first. A full
Asterisk source tree is not required.

The build compiles one ASL3 channel implementation against the installed
Asterisk headers. Both hardware adapter development packages are mandatory:
`librptadv-portaudio-alsa-adapter-dev` version `0.1.0~alpha2` or newer and
`librptadv-gpio-adapter-dev` version `0.1.0~alpha1` or newer. No backend build
switches are required. The module
does not require `res_usbradio.so`. The single `usbradioplus` Debian package
declares the exact supported ASL3 runtime alternatives. Its release gate loads
the same module under ASL3 3.9.3 and 3.10.5; the bootstrap installer verifies
the installed host is one of those alternatives without changing ASL3.

Build and test without changing the running node:

```text
make
make check
make distcheck
```

Create the versioned upstream source archive:

```text
make dist
```

Install by rebuilding the tested archive. This copies files only; it does not
load the module, restart Asterisk, or edit Asterisk configuration:

```text
sudo make prefix=/usr install-from-dist
```

For packaging or inspection, stage the installation instead:

```text
make DESTDIR=/tmp/usbradioplus-stage prefix=/usr install
```

Override `ASTERISK_INCLUDEDIR`, `asteriskmoduledir`, `CC`, `CFLAGS`, `LDFLAGS`,
or the standard GNU installation-directory variables when required. Run
`make clean` to remove all generated build and distribution artifacts.
On Debian, the default module path includes the host multiarch tuple so it
matches the ASL3 Asterisk module directory.

The build also installs `usbradioplus_agc.so` in
`lib/<multiarch>/usbradioplus` below the chosen prefix. This is a private
LADSPA effect loaded by the shared FFmpeg graph, not an Asterisk module.
Install it with the channel module; no separate plugin host or FFmpeg rebuild
is required on supported Debian systems. See `doc/agc.md` for AGC operation
and the basis for its defaults.
