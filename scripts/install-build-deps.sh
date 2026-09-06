#!/bin/sh
## @file
## @brief Install Debian/ASL development dependencies and the official RNNoise release.
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this helper as root." >&2
	exit 1
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
	asl3-asterisk-dev build-essential pkg-config python3 python3-pytest \
	ca-certificates wget xz-utils patch \
	libasound2-dev libusb-dev libusb-1.0-0-dev libsamplerate0-dev \
	libavfilter-dev libavutil-dev portaudio19-dev

if ! pkg-config --exists rnnoise; then
	sh "$(dirname -- "$0")/install-rnnoise.sh"
fi
