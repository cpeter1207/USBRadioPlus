#!/bin/sh
## @file
## @brief Verify the Rust LADSPA plugin and minimal Asterisk C shim.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${C_TEST_OUTPUT:-}" ]; then
	out=$C_TEST_OUTPUT
	mkdir -p "$out"
else
	out=$(mktemp -d "$root/.usbradioplus-tests.XXXXXX")
	trap 'rm -rf -- "$out"' EXIT HUP INT TERM
fi

if [ -z "${URP_AGC_PLUGIN:-}" ]; then
	make -s -C "$root" build/usbradioplus_agc.so
	URP_AGC_PLUGIN="$root/build/usbradioplus_agc.so.1"
fi

plugin_dir="$out/plugin"
mkdir -p "$plugin_dir"
cp "$URP_AGC_PLUGIN" "$plugin_dir/usbradioplus_agc.so.1"
ln -sf usbradioplus_agc.so.1 "$plugin_dir/usbradioplus_agc.so"

# shellcheck disable=SC2086
cc -std=c11 -Wall -Wextra -Werror ${C_TEST_CFLAGS:-} \
	-I"$root/tests/fixtures" "$root/tests/test_rms_agc_ladspa.c" \
	"$root/tests/fixtures/rms_agc_reference.c" -Wl,--wrap=calloc \
	-L"$plugin_dir" -Wl,-rpath,"$plugin_dir" -l:usbradioplus_agc.so \
	-o "$out/rms-agc" -lm
"$out/rms-agc"

# Compile the production metadata loader against deterministic host stubs.
# The Asterisk module macro exposes its three lifecycle entry points to the
# harness without adding test-only exports to the shipped module.
# Compiler flag lists intentionally undergo POSIX word splitting.
# shellcheck disable=SC2086
cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
	${C_TEST_CFLAGS:-} \
	-I"$root/tests/fixtures/shim-host/include" -I"$root/rust/asterisk/include" \
	-c "$root/src/chan_usbradioplus_shim.c" -o "$out/channel-shim.o"
# shellcheck disable=SC2086
cc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror ${C_TEST_CFLAGS:-} \
	-I"$root/tests/fixtures/shim-host/include" -I"$root/tests/fixtures/shim-host" \
	-I"$root/rust/asterisk/include" "$root/tests/test_chan_usbradioplus_shim.c" \
	"$out/channel-shim.o" -o "$out/channel-shim" -lm
"$out/channel-shim"
