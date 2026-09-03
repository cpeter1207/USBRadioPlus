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

asterisk -rx 'module load chan_usbradioplus.so' | grep -F 'Loaded chan_usbradioplus.so'
asterisk -rx 'module show like chan_usbradioplus' | grep -E \
	'chan_usbradioplus[.]so.*Running'
asterisk -rx 'radioplus processing show' | grep -F 'Chain local:'

cleanup
trap - EXIT HUP INT TERM
