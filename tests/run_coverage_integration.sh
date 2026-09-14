#!/bin/sh
## @file
## @brief Load the instrumented real module and collect integration coverage.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=$(find /usr/lib -type d -path '*/asterisk/modules' -print -quit)

test -n "$module_dir"
install -m 0644 "$root/build/chan_usbradioplus.so" "$module_dir/chan_usbradioplus.so"
plugin_dir="/usr/local/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/usbradioplus"
runtime_libdir="/usr/local/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
install -d "$plugin_dir"
install -d "$runtime_libdir"
install -m 0755 "$root/build/libusbradioplus_asterisk.so.1" \
	"$runtime_libdir/libusbradioplus_asterisk.so.1"
install -m 0644 "$root/build/usbradioplus_agc.so.1" \
	"$plugin_dir/usbradioplus_agc.so.1"
ln -sf usbradioplus_agc.so.1 "$plugin_dir/usbradioplus_agc.so"
ldconfig
install -m 0644 "$root/examples/usbradioplus.conf.sample" \
	/etc/asterisk/usbradioplus.conf

URP_COVERAGE_INTEGRATION=1 sh "$root/tests/container-smoke-test.sh"
