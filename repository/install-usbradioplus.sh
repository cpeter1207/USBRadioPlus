#!/bin/sh
# Install the USBRadioPlus binary built for this node's exact ASL3 host ABI.
# Unknown combinations are rejected so APT cannot replace ASL to satisfy a module.
set -eu

site_url=${USBRADIOPLUS_REPOSITORY_URL:-https://cpeter1207.github.io/USBRadioPlus}
key_fingerprint=A0D5A79E0F5C45E9E63679950951502BAC795E55
keyring=/etc/apt/keyrings/usbradioplus.gpg
source_list=/etc/apt/sources.list.d/usbradioplus.list
assume_yes=no
dry_run=no

usage() {
	cat <<'EOF'
Usage: sudo sh install-usbradioplus.sh [--yes] [--dry-run]

Detect the node's Debian, architecture, and ASL3 Asterisk versions, then install
the exactly matching USBRadioPlus package from the signed project repository.

  -y, --yes      install without the interactive confirmation
      --dry-run  report the selected package without changing the node
  -h, --help     show this help

The package creates a default usbradioplus.conf only when absent.
It does not replace existing configuration, activate the module, or restart
Asterisk.
EOF
}

die() {
	printf 'USBRadioPlus installer: %s\n' "$*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		-y|--yes) assume_yes=yes ;;
		--dry-run) dry_run=yes ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; die "unknown option: $1" ;;
	esac
	shift
done

[ "$(id -u)" -eq 0 ] || die "run this installer with sudo"
os_release=${USBRADIOPLUS_OS_RELEASE:-/etc/os-release}
[ -r "$os_release" ] || die "cannot read $os_release"
# Debian supplies these values; accept only literal supported identifiers below.
# The test override intentionally changes this path.
# shellcheck disable=SC1090
. "$os_release"
[ "${ID:-}" = debian ] || die "this installer supports Debian ASL3 nodes only"
suite=${VERSION_CODENAME:-}
case "$suite" in bookworm|trixie) ;; *) die "unsupported Debian release: ${suite:-unknown}" ;; esac

command -v dpkg >/dev/null 2>&1 || die "dpkg is not available"
command -v dpkg-query >/dev/null 2>&1 || die "dpkg-query is not available"
command -v apt-get >/dev/null 2>&1 || die "apt-get is not available"
architecture=$(dpkg --print-architecture)
case "$architecture" in amd64|arm64) ;; *) die "unsupported architecture: $architecture" ;; esac

asl_status=$(dpkg-query -W -f='${db:Status-Status}' asl3-asterisk 2>/dev/null || true)
[ "$asl_status" = installed ] || die "asl3-asterisk is not installed normally (status: ${asl_status:-missing})"
asl_version=$(dpkg-query -W -f='${Version}' asl3-asterisk)

package=
case "$suite:$architecture:$asl_version" in
	bookworm:amd64:2:22.9.0+asl3-3.9.3-1.deb12|\
	bookworm:arm64:2:22.9.0+asl3-3.9.3-1.deb12|\
	trixie:amd64:2:22.9.0+asl3-3.9.3-1.deb13|\
	trixie:arm64:2:22.9.0+asl3-3.9.3-1.deb13)
		package=usbradioplus ;;
	trixie:arm64:2:22.10.1+asl3-3.10.5-1.deb13)
		package=usbradioplus-asl3105 ;;
	*)
		die "no package is published for Debian $suite $architecture with asl3-asterisk $asl_version"
		;;
esac

cat <<EOF
USBRadioPlus installation plan
  Debian release: $suite
  Architecture:   $architecture
  ASL3 Asterisk:  $asl_version
  Package:        $package

The signed USBRadioPlus repository will be configured and the matching package
will be installed. A missing usbradioplus.conf will be created;
existing configuration will not be changed. Asterisk will not be restarted.
EOF

[ "$dry_run" = no ] || exit 0
if [ "$assume_yes" = no ]; then
	[ -t 0 ] || die "confirmation requires a terminal; rerun with --yes for unattended installation"
	printf 'Continue? [y/N] '
	IFS= read -r answer
	case "$answer" in y|Y|yes|YES|Yes) ;; *) echo "Installation cancelled."; exit 0 ;; esac
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg

temporary_directory=$(mktemp -d /tmp/usbradioplus-installer.XXXXXX)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM
downloaded_key=$temporary_directory/archive-keyring.gpg
curl -fL --proto '=https' --tlsv1.2 -o "$downloaded_key" \
	"$site_url/usbradioplus-archive-keyring.gpg"
downloaded_fingerprint=$(gpg --batch --quiet --show-keys --with-colons "$downloaded_key" | \
	awk -F: '$1 == "fpr" { print $10; exit }')
[ "$downloaded_fingerprint" = "$key_fingerprint" ] || \
	die "repository signing-key fingerprint does not match"

install -d -m 0755 /etc/apt/keyrings
install -m 0644 "$downloaded_key" "$keyring"
printf 'deb [signed-by=%s] %s %s main\n' "$keyring" "$site_url" "$suite" > "$source_list"
chmod 0644 "$source_list"
apt-get update

candidate=$(apt-cache policy "$package" | awk '/Candidate:/ { print $2; exit }')
if [ -z "$candidate" ] || [ "$candidate" = '(none)' ]; then
	die "the repository has no $package candidate"
fi
apt-cache madison "$package" | grep -F "| $candidate | $site_url " >/dev/null || \
	die "candidate $candidate is not supplied by the USBRadioPlus repository"
candidate_architecture=$(apt-cache show "$package=$candidate" | awk '/^Architecture:/ { print $2; exit }')
[ "$candidate_architecture" = "$architecture" ] || \
	die "candidate architecture $candidate_architecture does not match $architecture"
candidate_depends=$(apt-cache show "$package=$candidate" | sed -n 's/^Depends: //p' | head -n 1)
printf '%s\n' "$candidate_depends" | grep -F "asl3-asterisk (= $asl_version)" >/dev/null || \
	die "candidate $candidate does not require the installed ASL3 Asterisk version"

simulation=$temporary_directory/apt-simulation.txt
apt-get -s install "$package=$candidate" > "$simulation"
if grep -E '^(Remv|Inst asl3-asterisk |Conf asl3-asterisk )' "$simulation" >/dev/null; then
	die "APT would change asl3-asterisk; installation refused"
fi
apt-get install -y --no-install-recommends "$package=$candidate"

[ "$(dpkg-query -W -f='${db:Status-Status}' "$package")" = installed ] || \
	die "$package did not finish installing"
[ "$(dpkg-query -W -f='${Version}' asl3-asterisk)" = "$asl_version" ] || \
	die "asl3-asterisk changed unexpectedly"
module_path=$(dpkg-query -L "$package" | sed -n '/\/chan_usbradioplus\.so$/ { p; q; }')
if [ -z "$module_path" ] || [ ! -f "$module_path" ]; then
	die "the installed module file is missing"
fi

cat <<EOF
Installed $package $candidate successfully.

Nothing was activated and Asterisk was not restarted. Read
"man 7 usbradioplus" before changing modules.conf or rpt.conf.
EOF
