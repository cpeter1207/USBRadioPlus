#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this helper as root." >&2
	exit 1
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
	asl3-asterisk-dev build-essential pkg-config python3 python3-pytest \
	git autoconf automake libtool ca-certificates xz-utils \
	libasound2-dev libusb-dev libusb-1.0-0-dev libsamplerate0-dev \
	libavfilter-dev libavutil-dev

if ! pkg-config --exists rnnoise; then
	# Hardened nodes commonly mount /tmp noexec.  Build beside the source so
	# Autoconf can run its generated checks on the filesystem that ran install.sh.
	source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
	mkdir -p "$source_root/build"
	rnnoise_dir=$(mktemp -d "$source_root/build/rnnoise.XXXXXX")
	trap 'rm -rf -- "$rnnoise_dir"' EXIT HUP INT TERM
	git clone --quiet --branch v0.2 --depth 1 \
		https://github.com/xiph/rnnoise.git "$rnnoise_dir"
	(
		cd "$rnnoise_dir"
		./autogen.sh
		./configure --prefix=/usr/local --disable-examples --disable-doc
		make -j"$(getconf _NPROCESSORS_ONLN)"
		make install
	)
	ldconfig
fi
