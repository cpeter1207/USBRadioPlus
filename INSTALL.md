# Building and installing

## Signed Debian packages

For a supported ASL3 node on Debian 12 or 13, download and run the bootstrap
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

The archive signing-key fingerprint is
`A0D5 A79E 0F5C 45E9 E636 7995 0951 502B AC79 5E55`. Installation does not
activate the module, restart Asterisk, or edit `modules.conf` or `rpt.conf`.

After uploading and extracting the tarball on an ASL3 node, run:

```text
sudo ./install.sh
```

This installs the required toolchain and development packages, builds RNNoise
when Debian does not provide it, runs the complete hardware-free test suite,
and installs USBRadioPlus. It does not activate the module, restart Asterisk,
or edit `modules.conf` or `rpt.conf`.

The RNNoise bootstrap verifies and builds the official v0.2 release archive in
the source tree's `build` directory. This permits installation on hardened
nodes where `/tmp` is mounted `noexec`. It also restores the support header
omitted from the archive's ARM NEON sources. The temporary source is removed
automatically.

USBRadioPlus requires a matching `asl3-asterisk-dev` package, `ladspa-sdk`, plus the libraries
listed in `doc/packaging.md`. Developers with those dependencies already
installed may use `sudo ./install.sh --skip-deps`.

The build selects the radio-device interface exposed by the installed ASL3
headers. ASL 22.9/app_rpt 3.9 uses the original OSS and libusb-0.1 interface;
ASL 22.10/app_rpt 3.10 uses the shared-device, PortAudio, and libusb-1.0
interface. Run `make -s print-asl-radio-api` to report the selected interface.
Repository packages include the interface and app_rpt generation in their
version and require the exact ASL3 Asterisk build used to compile them. APT
therefore cannot install a module built for the other interface generation.
The bootstrap installer hides the ABI-specific package names from normal users.
The repository names the modern package `usbradioplus-asl3105`; the
`usbradioplus` package targets the earlier host interface. Installing the modern
package replaces the earlier package but does not activate the module or alter
Asterisk configuration.

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
