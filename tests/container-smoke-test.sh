#!/bin/sh
set -eu

log=${TMPDIR:-/tmp}/usbradioplus-asterisk.log
asterisk -f -n -q >"$log" 2>&1 &
asterisk_pid=$!

cleanup()
{
	asterisk -rx 'core stop now' >/dev/null 2>&1 || true
	wait "$asterisk_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

ready=false
attempt=0
while [ "$attempt" -lt 50 ]; do
	if asterisk -rx 'core show uptime' >/dev/null 2>&1; then
		ready=true
		break
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$ready" != true ]; then
	echo "Asterisk did not become ready" >&2
	tail -n 100 "$log" >&2
	exit 1
fi

module_ready=false
attempt=0
while [ "$attempt" -lt 50 ]; do
	# Autoload may still be running after the Asterisk core becomes responsive.
	# Loading is idempotent; the status query is the authoritative result.
	asterisk -rx 'module load chan_usbradioplus.so' >/dev/null 2>&1 || true
	if asterisk -rx 'module show like chan_usbradioplus' 2>/dev/null | grep -E \
		'chan_usbradioplus[.]so.*Running'; then
		module_ready=true
		break
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done
if [ "$module_ready" != true ]; then
	echo "chan_usbradioplus did not become ready" >&2
	tail -n 100 "$log" >&2
	exit 1
fi
asterisk -rx 'radioplus processing show' | grep -F 'Chain local:'

if [ "${URP_COVERAGE_INTEGRATION:-0}" = 1 ]; then
	# Drive the module side of the tune protocol through a configured channel.
	# Interactive meter and transmit-test commands are covered by the focused C
	# harness. Keep this noninteractive sweep bounded so a regression cannot hang
	# a coverage or release job indefinitely.
	asterisk -rx 'radioplus active 1999' | grep -F '1999'
	for command in \
		0 0+9 0+10 1 2 3 \
		a b c c500 d e e500 f f500 g g500 h h500 i \
		k k0 k1 L L4999 L5000 L13000 L13001 \
		D D0 D500 D999 D1000 M M0 M1 M2 \
		o o0 p p0 q q10 q100000 r r10 r100000 \
		s s0 s1 t t0 t1 u u2 w w1 x x2 Y Z unknown; do
		timeout 10s asterisk -rx "radioplus tune menu-support $command" >/dev/null
	done
fi

cleanup
trap - EXIT HUP INT TERM
