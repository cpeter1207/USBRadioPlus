#!/bin/sh
## @file
## @brief Install Debian/ASL development dependencies and the official RNNoise release.
set -eu

project_repository_url=https://cpeter1207.github.io/USBRadioPlus
project_key_fingerprint=A0D5A79E0F5C45E9E63679950951502BAC795E55
project_keyring=/etc/apt/keyrings/usbradioplus.gpg
project_source_list=/etc/apt/sources.list.d/usbradioplus.list
shared_ring_version=1.0.1
shared_ring_release_url=https://github.com/cpeter1207/rate_adjusting_pcm_ring/releases/download/v1.0.1

## @brief Print a dependency-installation diagnostic and terminate with failure.
die() {
	printf 'USBRadioPlus build dependencies: %s\n' "$*" >&2
	exit 1
}

## @brief Configure the signed project repository for the released shared ring ABI.
configure_project_repository() {
	script_directory=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
	project_key=$script_directory/../packaging/repository/usbradioplus-archive-keyring.gpg
	[ -r "$project_key" ] || die "cannot read shipped repository key"

	os_release=/etc/os-release
	[ -r "$os_release" ] || die "cannot read $os_release"
	# shellcheck disable=SC1090
	. "$os_release"
	[ "${ID:-}" = debian ] || die "this installer supports Debian ASL3 nodes only"
	suite=${VERSION_CODENAME:-}
	case "$suite" in
		bookworm|trixie) ;;
		*) die "unsupported Debian release: ${suite:-unknown}" ;;
	esac

	key_fingerprint=$(gpg --batch --quiet --show-keys --with-colons "$project_key" | \
		awk -F: '$1 == "fpr" { print $10; exit }')
	[ "$key_fingerprint" = "$project_key_fingerprint" ] || \
		die "shipped repository signing-key fingerprint does not match"

	install -d -m 0755 /etc/apt/keyrings
	install -m 0644 "$project_key" "$project_keyring"
	printf 'deb [signed-by=%s] %s %s main\n' \
		"$project_keyring" "$project_repository_url" "$suite" > "$project_source_list"
	chmod 0644 "$project_source_list"
}

## @brief Install the released shared ring when the project repository cannot yet provide it.
##
## The first USBRadioPlus package release needs the ring development package to
## build its source archive, while that same release is what publishes the
## package repository.  Bootstrap only Debian 13 from the checksum-pinned ring
## release to avoid that circular dependency.  Debian 12 remains a manual-only
## package target and continues to use the configured project repository.
install_released_shared_ring() {
	if pkg-config --atleast-version="$shared_ring_version" rate_adjusting_pcm_ring; then
		return
	fi

	if [ "$suite" != trixie ]; then
		DEBIAN_FRONTEND=noninteractive apt-get install -y \
			librate-adjusting-pcm-ring-dev
		return
	fi

	architecture=$(dpkg --print-architecture)
	case "$architecture" in
		amd64)
			runtime_sha256=6cacfc2de93523d28fc563d58b5c2b902c5c10ebb3fbe532b01cbb2b2324a2b1
			development_sha256=49ede1baa582dab20bae4763562494628efe4be145071fd49e0b8ab74296b464
			;;
		arm64)
			runtime_sha256=06c161d6307a35eb60e7e1d1fa145b9688378cc7f8e5c98a902b7b7da6d14422
			development_sha256=68f12a826e18540aaded7ff3cfffd2528d2b6f071961744ba514087fa10576c6
			;;
		*) die "unsupported Debian 13 architecture for the shared ring: $architecture" ;;
	esac

	runtime_package="librate-adjusting-pcm-ring1_${shared_ring_version}-1_debian13_${architecture}.deb"
	development_package="librate-adjusting-pcm-ring-dev_${shared_ring_version}-1_debian13_${architecture}.deb"
	download_directory=$(mktemp -d "${TMPDIR:-/tmp}/usbradioplus-ring.XXXXXX") || \
		die "cannot create a temporary shared-ring download directory"
	cleanup_shared_ring_download() {
		rm -f "$download_directory/$runtime_package" \
			"$download_directory/$development_package"
		rmdir "$download_directory" 2>/dev/null || true
	}
	trap cleanup_shared_ring_download EXIT HUP INT TERM

	wget -q -O "$download_directory/$runtime_package" \
		"$shared_ring_release_url/$runtime_package"
	wget -q -O "$download_directory/$development_package" \
		"$shared_ring_release_url/$development_package"
	printf '%s  %s\n%s  %s\n' \
		"$runtime_sha256" "$download_directory/$runtime_package" \
		"$development_sha256" "$download_directory/$development_package" | sha256sum -c -
	dpkg -i "$download_directory/$runtime_package" \
		"$download_directory/$development_package"
}

if [ "$(id -u)" -ne 0 ]; then
	die "run this helper as root"
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates gnupg
configure_project_repository
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
	asl3-asterisk-dev build-essential pkg-config python3 python3-pytest \
	ca-certificates wget xz-utils patch \
	libasound2-dev libusb-dev libusb-1.0-0-dev libsamplerate0-dev \
	libavfilter-dev libavutil-dev ladspa-sdk portaudio19-dev

install_released_shared_ring

pkg-config --atleast-version=1.0.1 rate_adjusting_pcm_ring || \
	die "rate_adjusting_pcm_ring 1.0.1 or newer is unavailable"

if ! pkg-config --exists rnnoise; then
	sh "$(dirname -- "$0")/install-rnnoise.sh"
fi
