#!/usr/bin/env bash

# Rebuild p2p_dev with KCFLAGS=-DCALC_CRC32 so completion-path CRC keeps
# registered-memory io_refs alive long enough to sample a hang if it
# returns. close(owner_fd) and do_exit must finish without D-state wait.
#
# XDS_DSTATE_EXPECT=ok (default) after the async-unpin fix.
# Guest GNU make 4.4.1 cannot compile out-of-tree 5.15 modules. Build
# stub.ko/p2p_dev.ko on the host (see xds-kvm-run/run-zijie-tests.sh) or
# set XDS_SKIP_MODULE_BUILD=1 when prebuilt .ko files are already loaded.

set -Eeuo pipefail

TEST_NAME=release_dstate_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

build_module()
{
	local kflags=$1

	log "Building p2p modules (KCFLAGS=$kflags)"
	make -C "$REPO_ROOT" clean
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1 KCFLAGS="$kflags"
	make -C "$REPO_ROOT/test" -j"$BUILD_JOBS" release_dstate_test
}

run_case()
{
	local label=$1
	shift
	local output="$WORK_DIR/$label.out"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local status

	dmesg -C
	set +e
	"$@" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		die "$label failed"
	fi
}

hung_release_tasks()
{
	ps -eo pid,state,wchan:32,cmd | awk '
		$2 ~ /D/ && $0 ~ /release_dstate_test/ { print }
	' || true
}

main()
{
	local expect=${XDS_DSTATE_EXPECT:-ok}
	local expect_flag

	(( $# == 0 )) || die "release_dstate_test.sh does not accept positional arguments"
	init_work_dir xds-release-dstate
	trap cleanup EXIT
	preflight

	case $expect in
		hang) expect_flag=--expect-hang ;;
		ok) expect_flag=--expect-ok ;;
		*) die "XDS_DSTATE_EXPECT must be hang or ok" ;;
	esac

	if [[ ${XDS_SKIP_MODULE_BUILD:-0} == 1 ]]; then
		make -C "$REPO_ROOT/test" -j"$BUILD_JOBS" release_dstate_test
		[[ -f $REPO_ROOT/stub.ko && -f $REPO_ROOT/p2p_dev.ko ]] ||
			die "XDS_SKIP_MODULE_BUILD requires prebuilt stub.ko and p2p_dev.ko"
		log "Skipping kbuild; using prebuilt modules"
		load_modules
	else
		build_module "-DCALC_CRC32"
		load_modules
	fi

	TEST_PASS_MSG="D-state cases finished (expect=$expect)"

	log "close(owner_fd) path: $expect_flag"
	run_case close-owner \
		"$SCRIPT_DIR/release_dstate_test" --topology "$DEV1" \
		--mode close "$expect_flag"

	if [[ -n $(hung_release_tasks) ]]; then
		SKIP_RMMOD=1
		if [[ $expect == ok ]]; then
			TEST_PASS_MSG=
			printf '%s\n' "$(hung_release_tasks)"
			die "close path left a D-state task"
		fi
		log "close path left a D-state task; skip do_exit case and rmmod"
		printf '%s\n' "$(hung_release_tasks)"
		return 0
	fi

	log "do_exit/_exit() path: $expect_flag"
	run_case exit-do-exit \
		"$SCRIPT_DIR/release_dstate_test" --topology "$DEV1" \
		--mode exit "$expect_flag"

	if [[ -n $(hung_release_tasks) ]]; then
		SKIP_RMMOD=1
		printf '%s\n' "$(hung_release_tasks)"
		if [[ $expect == ok ]]; then
			TEST_PASS_MSG=
			die "exit path left a D-state task"
		fi
		log "hung D-state task still present; skip rmmod in cleanup"
	fi
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
