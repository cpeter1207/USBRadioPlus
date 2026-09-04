#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=$(find /usr/lib -type d -path '*/asterisk/modules' -print -quit)

test -n "$module_dir"
install -m 0644 "$root/build/chan_usbradioplus.so" "$module_dir/chan_usbradioplus.so"
install -m 0644 "$root/examples/usbradioplus.conf.default" /etc/asterisk/usbradioplus.conf
install -m 0644 "$root/examples/usbradioplus-processing.conf.sample" \
	/etc/asterisk/usbradioplus-processing.conf

URP_COVERAGE_INTEGRATION=1 sh "$root/tests/container-smoke-test.sh"

# Exercise argument validation without requiring an interactive terminal.
if "$root/build/usbradioplus-tune" --meter invalid 2>/dev/null; then
	echo "invalid meter unexpectedly succeeded" >&2
	exit 1
fi
