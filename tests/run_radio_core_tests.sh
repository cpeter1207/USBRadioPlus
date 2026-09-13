#!/bin/sh
## @file
## @brief Compile and run native carrier, CTCSS, hardware, and repeat tests.
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
if [ -z "${RPTADV_RADIO_CFLAGS:-}" ]; then
	RPTADV_RADIO_CFLAGS=$(pkg-config --cflags rptadvradio)
fi
if [ -z "${RPTADV_RADIO_LIBS:-}" ]; then
	RPTADV_RADIO_LIBS=$(pkg-config --libs rptadvradio)
fi

# shellcheck disable=SC2086
cc $common $RPTADV_RADIO_CFLAGS "$root/tests/test_radio_core.c" "$root/src/usbradioplus_radio.c" \
	"$root/src/usbradioplus_squelch.c" "$root/src/usbradioplus_dcs.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" \
	-DAST_MODULE_SELF_SYM=test_module_self -DAST_MODULE='"chan_usbradioplus"' \
	-DURP_RADIO_TRACE=0 -I"$root/src" -o "$out/radio-core" -lm $RPTADV_RADIO_LIBS
"$out/radio-core"

# Exercise diagnostic paths compiled into the shipped module as a separate
# object so coverage merges the trace-enabled and trace-disabled variants.
# shellcheck disable=SC2086
cc $common $RPTADV_RADIO_CFLAGS "$root/tests/test_radio_core.c" "$root/src/usbradioplus_radio.c" \
	"$root/src/usbradioplus_squelch.c" "$root/src/usbradioplus_dcs.c" \
	"$root/src/usbradioplus_radio_core_adapter.c" \
	-DAST_MODULE_SELF_SYM=test_module_self -DAST_MODULE='"chan_usbradioplus"' \
	-DURP_RADIO_TRACE=1 -DURP_TEST_TRACE_PROGRESS -I"$root/src" \
	-o "$out/radio-core-trace" -lm $RPTADV_RADIO_LIBS
"$out/radio-core-trace"

if [ "$cleanup" -eq 0 ]; then
	printf '%s\n' "$out"
fi
