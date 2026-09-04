#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=${C_TEST_OUTPUT:-$(mktemp -d "$root/.usbradioplus-radio-tests.XXXXXX")}
cleanup=0
if [ -z "${C_TEST_OUTPUT:-}" ]; then
	cleanup=1
	trap 'rm -rf -- "$out"' EXIT HUP INT TERM
fi
mkdir -p "$out"
common="-std=gnu11 -Wall -Wextra -Werror ${C_TEST_CFLAGS:-}"

# shellcheck disable=SC2086
cc $common "$root/tests/test_radio_core.c" "$root/src/usbradioplus_radio.c" \
	-DAST_MODULE_SELF_SYM=test_module_self -DAST_MODULE='"chan_usbradioplus"' \
	-DURP_RADIO_TRACE=0 -I"$root/src" -o "$out/radio-core" -lm
"$out/radio-core"

# Exercise diagnostic paths compiled into the shipped module as a separate
# object so coverage merges the trace-enabled and trace-disabled variants.
# shellcheck disable=SC2086
cc $common "$root/tests/test_radio_core.c" "$root/src/usbradioplus_radio.c" \
	-DAST_MODULE_SELF_SYM=test_module_self -DAST_MODULE='"chan_usbradioplus"' \
	-DURP_RADIO_TRACE=1 -DURP_TEST_TRACE_PROGRESS -I"$root/src" \
	-o "$out/radio-core-trace" -lm
"$out/radio-core-trace"

if [ "$cleanup" -eq 0 ]; then
	printf '%s\n' "$out"
fi
