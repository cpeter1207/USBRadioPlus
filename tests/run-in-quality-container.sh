#!/bin/sh
## @file
## @brief Run checks in a labeled disposable container with exit and stale-container cleanup.
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

## @brief Remove only stopped containers matching this workspace's test-scope label.
cleanup_stale()
{
	stale=$(docker container ls --all --quiet --filter 'label=rpt_advanced.test=true' \
		--filter "label=$label" \
		--filter 'status=exited' --filter 'status=dead')
	if [ -n "$stale" ]; then
		# Do not force removal: a stopped container could have been restarted
		# after discovery, and another parallel check owns that invocation.
		while IFS= read -r container; do
			[ -n "$container" ] || continue
			docker container rm "$container" >/dev/null 2>&1 || true
		done <<EOF
$stale
EOF
	fi
}

## @brief Remove this invocation's test container when the runner exits.
cleanup_current()
{
	docker container rm --force "$name" >/dev/null 2>&1 || true
}

cleanup_stale
trap cleanup_current EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "$#" -eq 0 ]; then
	set -- make platform-verify
fi

host_root=$root
case $(uname -s) in
	MINGW*|MSYS*)
		host_root=$(cd "$root" && pwd -W)
		export MSYS_NO_PATHCONV=1
		;;
esac

docker run --rm --name "$name" --label "$label" \
	--label rpt_advanced.test=true \
	--volume "$host_root:/workspace" --workdir /workspace "$image" "$@"
