# Vendored source snapshot

This directory is a source snapshot of
[rate_adjusting_pcm_ring](https://github.com/cpeter1207/rate_adjusting_pcm_ring),
the lock-free program-audio ring used by USBRadioPlus.  It is included so an
upstream USBRadioPlus tarball and Debian source package build without a second
checkout, a network fetch, or an unpublished runtime package.

The snapshot is based on upstream commit
`3ece3e729d47a0501ae5880874064b0b2dcc9f0b`; it includes the validated
release-candidate working-tree changes to the Makefile, README, public header,
implementation, and ring test made after that commit.  The vendored source is
GPL-2.0-only, as marked by its SPDX identifiers.  USBRadioPlus links its
static archive privately into `chan_usbradioplus.so`; it neither installs nor
requires `librate_adjusting_pcm_ring.so` at run time.

The sibling repository remains the development upstream.  To update this
snapshot, first complete that project's full Debian 12/13 amd64/arm64 quality
matrix, replace this directory from one reviewed upstream revision, update this
record, then run USBRadioPlus `make ci` and `make distcheck`.  Do not change
the snapshot only in a generated source archive.

The vendored Makefile explicitly selects LLVM formatting style. This is the
upstream standalone default and prevents USBRadioPlus's parent formatting file
from changing the nested project's quality result.
