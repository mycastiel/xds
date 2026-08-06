#!/usr/bin/env bash

set -Eeuo pipefail

TEST_NAME=cq_race_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# Long submit∥getevents∥drain pressure (no CRC).
# Drain overlaps live submit for XDS_CQ_RACE_OVERLAP_MS after the first accept,
# then stop_submit + destroy. Repeat XDS_CQ_RACE_ROUNDS times per API/mem mode.
readonly CQ_RACE_WORKERS=${XDS_CQ_RACE_WORKERS:-32}
# Workload rows per worker (VA patterns). Duration comes from OVERLAP_MS × ROUNDS.
readonly CQ_RACE_ITERATIONS=${XDS_CQ_RACE_ITERATIONS:-256}
readonly CQ_RACE_OVERLAP_MS=${XDS_CQ_RACE_OVERLAP_MS:-30000}
readonly CQ_RACE_ROUNDS=${XDS_CQ_RACE_ROUNDS:-3}
readonly CQ_RACE_SEED=${XDS_CQ_RACE_SEED:-0x584453}
readonly VA_GRANULARITY=$((4 << 10))
readonly BACKING_PAGE_SIZE=$((2 << 20))

if [[ ${XDS_STRESS_MODE+x} ]]; then
	readonly STRESS_MODE=$XDS_STRESS_MODE
else
	readonly STRESS_MODE=nvme
fi

TARGET=

validate_options()
{
	case $STRESS_MODE in
		raid0 | dm | nvme) ;;
		*) die "XDS_STRESS_MODE must be raid0, dm, or nvme" ;;
	esac
	[[ $CQ_RACE_WORKERS =~ ^[1-9][0-9]*$ ]] ||
		die "XDS_CQ_RACE_WORKERS must be a positive integer"
	(( CQ_RACE_WORKERS >= 2 )) ||
		die "XDS_CQ_RACE_WORKERS must be >= 2"
	[[ $CQ_RACE_ITERATIONS =~ ^[1-9][0-9]*$ ]] ||
		die "XDS_CQ_RACE_ITERATIONS must be a positive integer"
	[[ $CQ_RACE_OVERLAP_MS =~ ^[0-9]+$ ]] ||
		die "XDS_CQ_RACE_OVERLAP_MS must be a non-negative integer"
	[[ $CQ_RACE_ROUNDS =~ ^[1-9][0-9]*$ ]] ||
		die "XDS_CQ_RACE_ROUNDS must be a positive integer"
	[[ $CQ_RACE_SEED =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]] ||
		die "XDS_CQ_RACE_SEED must be an unsigned integer"
}

prepare_topology()
{
	case $STRESS_MODE in
		raid0)
			log "Creating MD RAID0 cq-race topology"
			create_raid0
			TARGET=$MD_DEV
			;;
		dm)
			log "Creating dm-linear cq-race topology"
			create_linear_dm
			TARGET=$DM_PATH
			;;
		nvme)
			log "Using direct NVMe cq-race topology $DEV1"
			wipe_test_devices
			TARGET=$DEV1
			;;
	esac
}

run_cq_race_live_api()
{
	local api=$1 workload=$2 memory_mode=$3
	local mode=cq-race-drain-live
	local round label output kernel_log summary status
	local get_param=/sys/module/stub/parameters/get_pa_calls
	local put_param=/sys/module/stub/parameters/put_pa_calls
	local get_before=0 put_before=0 get_after put_after
	local -a command common_options

	common_options=(--topology "$TARGET" --manifest "$workload" \
		--mode "$mode" --workers "$CQ_RACE_WORKERS" \
		--iterations "$CQ_RACE_ITERATIONS" --cmb-size "$CMB_SIZE" \
		--va-granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" \
		--drain-overlap-ms "$CQ_RACE_OVERLAP_MS")
	if [[ $memory_mode == registered ]]; then
		common_options+=(--registered-mem)
		[[ -r $get_param && -r $put_param ]] ||
			die "stub PA-list counters are unavailable"
	fi
	if [[ $api == nds-c ]]; then
		command=("$SCRIPT_DIR/nds_api_test" "${common_options[@]}")
	elif [[ $api == nds-python ]]; then
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_nds_api_test.py" "${common_options[@]}")
	else
		die "unsupported cq-race api '$api'"
	fi

	for ((round = 1; round <= CQ_RACE_ROUNDS; round++)); do
		label="$mode-$STRESS_MODE.$api.$memory_mode.r$round"
		output="$WORK_DIR/$label.out"
		kernel_log="$WORK_DIR/$label.dmesg"
		summary="$WORK_DIR/$label.summary"
		log "$STRESS_MODE: $api $memory_mode $mode round $round/$CQ_RACE_ROUNDS (overlap=${CQ_RACE_OVERLAP_MS}ms, no CRC)"
		if [[ $memory_mode == registered ]]; then
			get_before=$(<"$get_param")
			put_before=$(<"$put_param")
		fi
		reset_p2p_stats
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
			die "$STRESS_MODE $api $memory_mode $mode round $round failed"
		fi
		if [[ $memory_mode == registered ]]; then
			get_after=$(<"$get_param")
			put_after=$(<"$put_param")
			(( get_after - get_before == 1 )) ||
				die "$api registered $mode used \
$((get_after - get_before)) PA-list gets"
			(( put_after - put_before == 1 )) ||
				die "$api registered $mode used \
$((put_after - put_before)) PA-list puts"
		fi
		check_stats
	done
}

main()
{
	local actual_block_size
	local data_dir
	local workload
	local api
	local memory_mode

	(( $# == 0 )) || die "cq_race_test.sh does not accept positional arguments"
	validate_options
	init_work_dir "xds-cq-race-$STRESS_MODE"
	trap cleanup EXIT

	preflight
	build_all
	save_kernel_identity
	load_modules
	prepare_topology

	log "$STRESS_MODE: creating ext4 with 4 KiB blocks"
	mkfs.ext4 -F -q -b 4096 "$TARGET"
	mount -o noatime "$TARGET" "$MOUNT_DIR"
	actual_block_size=$(stat -f -c %S "$MOUNT_DIR")
	[[ $actual_block_size -eq 4096 ]] ||
		die "ext4 block size $actual_block_size does not match 4096"

	data_dir=$MOUNT_DIR/cq-race-data
	workload="$WORK_DIR/cq-race-$STRESS_MODE.workload.tsv"
	python3 "$SCRIPT_DIR/generate_stress_workload.py" \
		--directory "$data_dir" --manifest "$workload" \
		--workers "$CQ_RACE_WORKERS" --iterations "$CQ_RACE_ITERATIONS" \
		--seed "$CQ_RACE_SEED" --mode "$STRESS_MODE" --topology "$TARGET"
	sync

	log "live stress: workers=$CQ_RACE_WORKERS iterations=$CQ_RACE_ITERATIONS overlap_ms=$CQ_RACE_OVERLAP_MS rounds=$CQ_RACE_ROUNDS"

	for api in nds-c nds-python; do
		for memory_mode in normal registered; do
			run_cq_race_live_api "$api" "$workload" "$memory_mode"
		done
	done
	log "$STRESS_MODE cq-race-drain-live concurrency stress passed (rounds=$CQ_RACE_ROUNDS overlap_ms=$CQ_RACE_OVERLAP_MS)"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
