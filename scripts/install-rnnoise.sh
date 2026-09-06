#!/bin/sh
## @file
## @brief Download, patch, and install the official RNNoise release with checksum verification.
set -eu

version=0.2
sha256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
source_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$source_root/build"
work=$(mktemp -d "$source_root/build/rnnoise.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
archive="$work/rnnoise-$version.tar.gz"
wget -qO "$archive" \
	"https://github.com/xiph/rnnoise/releases/download/v$version/rnnoise-$version.tar.gz"
printf '%s  %s\n' "$sha256" "$archive" | sha256sum -c -
tar -C "$work" --strip-components=1 -xzf "$archive"
patch -d "$work" -p1 < \
	"$source_root/packaging/rnnoise/debian/patches/arm-os-support.patch"
patch -d "$work" -p1 < \
	"$source_root/packaging/rnnoise/debian/patches/warning-clean-build.patch"
(
	cd "$work"
	./configure --prefix=/usr/local --disable-examples --disable-doc
	make -j"$(getconf _NPROCESSORS_ONLN)"
	make install
)
ldconfig
