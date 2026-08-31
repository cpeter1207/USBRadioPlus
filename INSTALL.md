# Building and installing

After uploading and extracting the tarball on an ASL3 node, run:

```text
sudo ./install.sh
```

This installs the required toolchain and development packages, builds RNNoise
when Debian does not provide it, runs the complete hardware-free test suite,
and installs USBRadioPlus. It does not activate the module, restart Asterisk,
or edit `modules.conf` or `rpt.conf`.

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
