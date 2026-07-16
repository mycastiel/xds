#!/usr/bin/env bash

set -Eeuo pipefail

TEST_NAME=stress_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

readonly STRESS_WORKERS=16
readonly STRESS_ITERATIONS=${XDS_STRESS_ITERATIONS:-16}
readonly STRESS_SEED=${XDS_STRESS_SEED:-0x584453}
readonly VA_GRANULARITY=$((4 << 10))
readonly BACKING_PAGE_SIZE=$((2 << 20))

if [[ ${XDS_STRESS_MODE+x} ]]; then
	readonly STRESS_MODE=$XDS_STRESS_MODE
else
	readonly STRESS_MODE=raid0
fi

TARGET=

validate_stress_options()
{
	case $STRESS_MODE in
		raid0 | dm | nvme) ;;
		*) die "XDS_STRESS_MODE must be raid0, dm, or nvme" ;;
	esac
	[[ $STRESS_ITERATIONS =~ ^[1-9][0-9]*$ ]] ||
		die "XDS_STRESS_ITERATIONS must be a positive integer"
	[[ $STRESS_SEED =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]] ||
		die "XDS_STRESS_SEED must be an unsigned integer"
}

prepare_topology()
{
	case $STRESS_MODE in
		raid0)
			log "Creating MD RAID0 stress topology"
			create_raid0
			TARGET=$MD_DEV
			;;
		dm)
			log "Creating dm-linear stress topology"
			create_linear_dm
			TARGET=$DM_PATH
			;;
		nvme)
			log "Using direct NVMe stress topology $DEV1"
			wipe_test_devices
			TARGET=$DEV1
			;;
	esac
}

run_stress_api()
{
	local api=$1 workload=$2
	local label="stress-$STRESS_MODE.$api"
	local output="$WORK_DIR/$label.out"
	local result="$WORK_DIR/$label.result.tsv"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local summary="$WORK_DIR/$label.summary"
	local status
	local -a command common_options

	common_options=(--topology "$TARGET" --manifest "$workload" \
		--mode stress --workers "$STRESS_WORKERS" \
		--iterations "$STRESS_ITERATIONS" --cmb-size "$CMB_SIZE" \
		--va-granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" \
		--result-manifest "$result")
	if [[ $api == c ]]; then
		command=("$SCRIPT_DIR/c_api_test" "${common_options[@]}")
	else
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_api_test.py" "${common_options[@]}")
	fi

	log "$STRESS_MODE: running $api API dynamic-VA stress"
	: >/sys/kernel/debug/p2p_device/summary
	dmesg -C
	set +e
	"${command[@]}" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat /sys/kernel/debug/p2p_device/summary >"$summary"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		die "$STRESS_MODE $api stress runner failed"
	fi

	python3 "$SCRIPT_DIR/check_stress.py" --workload "$workload" \
		--result "$result" --workers "$STRESS_WORKERS" \
		--iterations "$STRESS_ITERATIONS" --cmb-size "$CMB_SIZE" \
		--granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE"
	python3 "$SCRIPT_DIR/check_crc.py" --manifest "$result" \
		--log "$kernel_log" --crc-tool "$SCRIPT_DIR/crc32_verify"
	check_stats
}

main()
{
	local actual_block_size
	local data_dir
	local workload
	local api

	(( $# == 0 )) || die "stress_test.sh does not accept positional arguments"
	validate_stress_options
	init_work_dir "xds-stress-$STRESS_MODE"
	trap cleanup EXIT

	preflight
	build_all
	crc_self_test
	load_modules
	prepare_topology

	log "$STRESS_MODE: creating ext4 with 4 KiB blocks"
	mkfs.ext4 -F -q -b 4096 "$TARGET"
	mount -o noatime "$TARGET" "$MOUNT_DIR"
	actual_block_size=$(stat -f -c %S "$MOUNT_DIR")
	[[ $actual_block_size -eq 4096 ]] ||
		die "ext4 block size $actual_block_size does not match 4096"

	data_dir=$MOUNT_DIR/stress-data
	workload="$WORK_DIR/stress-$STRESS_MODE.workload.tsv"
	python3 "$SCRIPT_DIR/generate_stress_workload.py" \
		--directory "$data_dir" --manifest "$workload" \
		--workers "$STRESS_WORKERS" --iterations "$STRESS_ITERATIONS" \
		--seed "$STRESS_SEED" --mode "$STRESS_MODE" --topology "$TARGET"
	sync

	for api in c python; do
		run_stress_api "$api" "$workload"
	done
	log "$STRESS_MODE dynamic-VA C and Python stress tests passed"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
