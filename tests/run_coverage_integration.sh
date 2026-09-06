#!/bin/sh
## @file
## @brief Load the instrumented real module and collect integration coverage.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=$(find /usr/lib -type d -path '*/asterisk/modules' -print -quit)

test -n "$module_dir"
install -m 0644 "$root/build/chan_usbradioplus.so" "$module_dir/chan_usbradioplus.so"
plugin_dir="/usr/local/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/usbradioplus"
install -d "$plugin_dir"
install -m 0644 "$root/build/usbradioplus_agc.so" "$plugin_dir/usbradioplus_agc.so"
install -m 0644 "$root/examples/usbradioplus.conf.sample" \
	/etc/asterisk/usbradioplus.conf

URP_COVERAGE_INTEGRATION=1 sh "$root/tests/container-smoke-test.sh"

# Exercise argument validation without requiring an interactive terminal.
if "$root/scripts/usbradioplus-tune" --meter invalid 2>/dev/null; then
	echo "invalid meter unexpectedly succeeded" >&2
	exit 1
fi
