#!/bin/sh
set -eu

if [ "$#" -lt 1 ]; then
	echo "usage: $0 IMAGE [COMMAND [ARG...]]" >&2
	exit 2
fi

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
image=$1
shift
scope=$(printf '%s' "$root" | cksum | awk '{print $1}')
label="org.usbradioplus.test.scope=$scope"
name="usbradioplus-test-$scope-$$"

cleanup_stale()
{
	stale=$(docker container ls --all --quiet --filter "label=$label")
	if [ -n "$stale" ]; then
		# IDs come directly from Docker's exact-label query.
		# shellcheck disable=SC2086
		docker container rm --force $stale >/dev/null
	fi
}

cleanup_current()
{
	docker container rm --force "$name" >/dev/null 2>&1 || true
}

cleanup_stale
trap cleanup_current EXIT HUP INT TERM

if [ "$#" -eq 0 ]; then
	set -- make platform-verify
fi

docker run --rm --name "$name" --label "$label" \
	--volume "$root:/workspace" --workdir /workspace "$image" "$@"
