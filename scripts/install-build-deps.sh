#!/bin/sh
## @file
## @brief Install Debian/ASL development dependencies and the official RNNoise release.
set -eu

project_repository_url=https://cpeter1207.github.io/USBRadioPlus
project_key_fingerprint=A0D5A79E0F5C45E9E63679950951502BAC795E55
project_keyring=/etc/apt/keyrings/usbradioplus.gpg
project_source_list=/etc/apt/sources.list.d/usbradioplus.list

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

if [ "$(id -u)" -ne 0 ]; then
	die "run this helper as root"
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates gnupg
configure_project_repository
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
	asl3-asterisk-dev build-essential cargo rustc pkg-config python3 python3-pytest \
	ca-certificates wget xz-utils patch \
	ladspa-sdk librate-adjusting-pcm-ring2-dev \
	librptadvradio-dev librptadv-samplerate-adapter-dev \
	librptadv-ffmpeg-adapter-dev librptadv-portaudio-alsa-adapter-dev \
	librptadv-gpio-adapter-dev librptadv-rnnoise-adapter-dev

pkg-config --exists rate_adjusting_pcm_ring2 || \
	die "rate_adjusting_pcm_ring2 is unavailable"
[ "$(pkg-config --variable=abi_version rptadvradio)" = 4 ] || \
	die "rptadvradio descriptor ABI 4 (alpha.4 or newer) is unavailable"
pkg-config --atleast-version=0.1.0~alpha1 rptadv_samplerate_adapter || \
	die "rptadv_samplerate_adapter 0.1.0~alpha1 or newer is unavailable"
pkg-config --atleast-version=0.1.0~alpha1 rptadv_ffmpeg_adapter || \
	die "rptadv_ffmpeg_adapter 0.1.0~alpha1 or newer is unavailable"
pkg-config --atleast-version=0.1.0~alpha2 rptadv_portaudio_alsa_adapter || \
	die "rptadv_portaudio_alsa_adapter 0.1.0~alpha2 or newer is unavailable"
pkg-config --atleast-version=0.1.0~alpha1 rptadv_gpio_adapter || \
	die "rptadv_gpio_adapter 0.1.0~alpha1 or newer is unavailable"

if ! pkg-config --exists rnnoise; then
	sh "$(dirname -- "$0")/install-rnnoise.sh"
fi
