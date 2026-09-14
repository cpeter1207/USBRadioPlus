#!/bin/sh
## @file
## @brief Verify the installed module and CLI inside an ASL test container.
set -eu

log=${TMPDIR:-/tmp}/usbradioplus-asterisk.log
asterisk -f -n -q >"$log" 2>&1 &
asterisk_pid=$!

## @brief Stop the isolated test Asterisk instance and remove temporary test files.
cleanup()
{
	asterisk -rx 'core stop now' >/dev/null 2>&1 || true
	wait "$asterisk_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

## @brief Return success when the background Asterisk child has already exited.
asterisk_child_exited()
{
	if ! kill -0 "$asterisk_pid" 2>/dev/null; then
		return 0
	fi
	# A shell can retain a terminated child as a zombie until wait reaps it.
	ps -o stat= -p "$asterisk_pid" 2>/dev/null | grep -q '[Z]'
}

## @brief Preserve a crashed Asterisk child's status and diagnostic log.
fail_if_asterisk_exited()
{
	phase=$1
	status=0

	if ! asterisk_child_exited; then
		return 0
	fi
	if wait "$asterisk_pid" 2>/dev/null; then
		status=0
	else
		status=$?
	fi
	echo "Asterisk exited unexpectedly after $phase (status $status)" >&2
	tail -n 100 "$log" >&2
	# A clean but premature exit is still a failed smoke test. Preserve a
	# nonzero child status, such as a signal-derived 139, for diagnostics.
	if [ "$status" -eq 0 ]; then
		exit 1
	fi
	exit "$status"
}

## @brief Require both the manager socket and its owning child after a phase.
require_asterisk_cli()
{
	phase=$1

	if asterisk -rx 'core show uptime' >/dev/null 2>&1; then
		return 0
	fi
	fail_if_asterisk_exited "$phase"
	echo "Asterisk CLI became unavailable after $phase" >&2
	tail -n 100 "$log" >&2
	exit 1
}

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

# The CLI socket can accept a command before asynchronous module autoload has
# completed. Wait for Asterisk's own boot barrier before inspecting modules.
if ! asterisk -rx 'core waitfullybooted' >/dev/null 2>&1; then
	echo "Asterisk did not finish booting" >&2
	tail -n 100 "$log" >&2
	exit 1
fi

## @brief Wait for the named Asterisk module to reach Running state.
wait_for_module()
{
	module=$1
	limit=$2
	attempt=0
	while [ "$attempt" -lt "$limit" ]; do
		if asterisk -rx "module show like $module" 2>/dev/null | grep -E \
			"${module}[.]so.*Running"; then
			return 0
		fi
		fail_if_asterisk_exited "waiting for $module"
		attempt=$((attempt + 1))
		sleep 0.1
	done
	return 1
}

if ! wait_for_module chan_usbradioplus 10; then
	module_load_output=$(asterisk -rx 'module load chan_usbradioplus.so' 2>&1 || true)
fi
if ! wait_for_module chan_usbradioplus 20; then
	echo "chan_usbradioplus did not become ready" >&2
	printf '%s\n' "${module_load_output:-no module-load response}" >&2
	asterisk -rx 'module show like chan_usbradioplus' >&2 || true
	tail -n 100 "$log" >&2
	exit 1
fi
require_asterisk_cli 'chan_usbradioplus module readiness'
if ! asterisk -rx 'radioplus channel list' | grep -Fx 'usb'; then
	require_asterisk_cli 'radioplus channel-list CLI check'
	echo 'radioplus channel list did not report the shipped radio' >&2
	tail -n 100 "$log" >&2
	exit 1
fi

if [ "${URP_COVERAGE_INTEGRATION:-0}" = 1 ]; then
	# Exercise the non-hardware typed control surface in the real Asterisk host.
	# Hardware operations remain in the deterministic host-stub shim harness.
	asterisk -rx 'radioplus processing reload' | grep -F 'Configuration reloaded'
fi

cleanup
trap - EXIT HUP INT TERM
