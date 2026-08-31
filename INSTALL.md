# Building and installing

## Signed Debian packages

For Debian 12 or 13 on amd64 or arm64:

```text
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://cpeter1207.github.io/USBRadioPlus/usbradioplus-archive-keyring.gpg |
    sudo tee /etc/apt/keyrings/usbradioplus.gpg >/dev/null
echo "deb [signed-by=/etc/apt/keyrings/usbradioplus.gpg] https://cpeter1207.github.io/USBRadioPlus $(. /etc/os-release; echo $VERSION_CODENAME) main" |
    sudo tee /etc/apt/sources.list.d/usbradioplus.list
sudo apt update
sudo apt install usbradioplus
```

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

USBRadioPlus requires a matching `asl3-asterisk-dev` package plus the libraries
listed in `doc/packaging.md`. Developers with those dependencies already
installed may use `sudo ./install.sh --skip-deps`.

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
