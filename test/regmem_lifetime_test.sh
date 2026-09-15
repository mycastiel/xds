#!/usr/bin/env bash

# Owner-fd vs I/O-fd lifetime edges: close/unregister/_exit/SIGKILL while
# registered-memory io_refs are live, then stale-handle checks. Success is
# reported from common.sh cleanup after rmmod.

set -Eeuo pipefail

TEST_NAME=regmem_lifetime_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

main()
{
	(( $# == 0 )) || die "regmem_lifetime_test.sh does not accept positional arguments"
	TEST_PASS_MSG="registered-memory lifetime edge cases passed"
	init_work_dir xds-regmem-lifetime
	trap cleanup EXIT
	preflight

	if [[ ${XDS_SKIP_MODULE_BUILD:-0} == 1 ]]; then
		make -C "$REPO_ROOT/test" -j"$BUILD_JOBS" regmem_lifetime_test
	else
		log "Building p2p modules (KCFLAGS=-DCALC_CRC32)"
		make -C "$REPO_ROOT" clean
		make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1 KCFLAGS="-DCALC_CRC32"
		make -C "$REPO_ROOT/test" -j"$BUILD_JOBS" regmem_lifetime_test
	fi
	load_modules

	log "registered-memory lifetime edges"
	run_logged_case lifetime \
		"$SCRIPT_DIR/regmem_lifetime_test" --topology "$DEV1" --case all
}

run_logged_case()
{
	local label=$1
	shift
	local output="$WORK_DIR/$label.out"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local status

	dmesg -C
	set +e
	timeout --signal=KILL 120 "$@" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		ps -eo pid,state,wchan:32,cmd | awk 'NR==1 || $2 ~ /D/' >&2
		die "$label failed (status=$status)"
	fi
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
