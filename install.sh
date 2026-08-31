#!/bin/sh
set -eu

usage() {
	cat <<'EOF'
Usage: sudo ./install.sh [--skip-deps] [--destdir DIR]

Installs build dependencies, builds and tests USBRadioPlus, then copies its
files. It does not activate the module, restart Asterisk, or edit Asterisk
configuration other than creating usbradioplus-processing.conf when absent.
EOF
}

skip_deps=no
destdir=""
while [ "$#" -gt 0 ]; do
	case "$1" in
		--skip-deps) skip_deps=yes; shift ;;
		--destdir) destdir=${2:?missing destination root}; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

if [ "$(id -u)" -ne 0 ] && [ -z "$destdir" ]; then
	echo "Run as root for a system installation, or use --destdir for staging." >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$skip_deps" = no ]; then
	if [ "$(id -u)" -ne 0 ]; then
		echo "Installing dependencies requires root." >&2
		exit 1
	fi
	"$root/scripts/install-build-deps.sh"
fi

make -C "$root" clean check
make -C "$root" DESTDIR="$destdir" prefix=/usr install

cat <<'EOF'
USBRadioPlus files were installed. A default usbradioplus-processing.conf was
created if none existed. Existing configuration was preserved. Nothing was
activated and Asterisk was not restarted. Review the test results and
documentation before changing modules.conf or rpt.conf.
EOF
