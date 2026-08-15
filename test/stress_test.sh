#!/usr/bin/env bash

set -Eeuo pipefail

TEST_NAME=stress_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

readonly STRESS_WORKERS=16
readonly STRESS_ITERATIONS=${XDS_STRESS_ITERATIONS:-16}
readonly STRESS_SEED=${XDS_STRESS_SEED:-0x584453}
readonly CQ_RACE_WORKERS=${XDS_CQ_RACE_WORKERS:-24}
readonly CQ_RACE_ITERATIONS=${XDS_CQ_RACE_ITERATIONS:-64}
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
	local api=$1 workload=$2 memory_mode=$3
	local label="stress-$STRESS_MODE.$api.$memory_mode"
	local output="$WORK_DIR/$label.out"
	local result="$WORK_DIR/$label.result.tsv"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local summary="$WORK_DIR/$label.summary"
	local get_param=/sys/module/stub/parameters/get_pa_calls
	local put_param=/sys/module/stub/parameters/put_pa_calls
	local get_before=0 put_before=0 get_after put_after
	local status
	local -a command common_options

	common_options=(--topology "$TARGET" --manifest "$workload" \
		--mode stress --workers "$STRESS_WORKERS" \
		--iterations "$STRESS_ITERATIONS" --cmb-size "$CMB_SIZE" \
		--va-granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" \
		--result-manifest "$result")
	if [[ $memory_mode == registered ]]; then
		common_options+=(--registered-mem)
		[[ -r $get_param && -r $put_param ]] ||
			die "stub PA-list counters are unavailable"
		get_before=$(<"$get_param")
		put_before=$(<"$put_param")
	fi
	if [[ $api == c ]]; then
		command=("$SCRIPT_DIR/c_api_test" "${common_options[@]}")
	elif [[ $api == python ]]; then
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_api_test.py" "${common_options[@]}")
	elif [[ $api == nds-c ]]; then
		command=("$SCRIPT_DIR/nds_api_test" "${common_options[@]}")
	elif [[ $api == nds-python ]]; then
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_nds_api_test.py" "${common_options[@]}")
	else
		die "unsupported stress api '$api'"
	fi

	log "$STRESS_MODE: running $api API $memory_mode-memory dynamic-VA stress"
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
		die "$STRESS_MODE $api $memory_mode stress runner failed"
	fi
	if [[ $memory_mode == registered ]]; then
		get_after=$(<"$get_param")
		put_after=$(<"$put_param")
		(( get_after - get_before == 1 )) ||
			die "$api registered stress used \
$((get_after - get_before)) PA-list gets"
		(( put_after - put_before == 1 )) ||
			die "$api registered stress used \
$((put_after - put_before)) PA-list puts"
	fi

	python3 "$SCRIPT_DIR/check_stress.py" --workload "$workload" \
		--result "$result" --workers "$STRESS_WORKERS" \
		--iterations "$STRESS_ITERATIONS" \
		--cmb-size "$CMB_SIZE" \
		--granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" |
		tee -a "$output"
	python3 "$SCRIPT_DIR/check_crc.py" --manifest "$result" \
		--log "$kernel_log" \
		--crc-tool "$SCRIPT_DIR/crc32_verify" |
		tee -a "$output"
	check_stats
}

run_cq_race_api()
{
	local mode=$1 api=$2 workload=$3 memory_mode=$4
	local label="$mode-$STRESS_MODE.$api.$memory_mode"
	local output="$WORK_DIR/$label.out"
	local result="$WORK_DIR/$label.result.tsv"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local summary="$WORK_DIR/$label.summary"
	local get_param=/sys/module/stub/parameters/get_pa_calls
	local put_param=/sys/module/stub/parameters/put_pa_calls
	local get_before=0 put_before=0 get_after put_after
	local status
	local -a command common_options

	common_options=(--topology "$TARGET" --manifest "$workload" \
		--mode "$mode" --workers "$CQ_RACE_WORKERS" \
		--iterations "$CQ_RACE_ITERATIONS" --cmb-size "$CMB_SIZE" \
		--va-granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" \
		--result-manifest "$result")
	if [[ $memory_mode == registered ]]; then
		common_options+=(--registered-mem)
		[[ -r $get_param && -r $put_param ]] ||
			die "stub PA-list counters are unavailable"
		get_before=$(<"$get_param")
		put_before=$(<"$put_param")
	fi
	if [[ $api == nds-c ]]; then
		command=("$SCRIPT_DIR/nds_api_test" "${common_options[@]}")
	elif [[ $api == nds-python ]]; then
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_nds_api_test.py" "${common_options[@]}")
	else
		die "unsupported cq-race api '$api'"
	fi

	log "$STRESS_MODE: running $api API $memory_mode-memory $mode stress"
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
		die "$STRESS_MODE $api $memory_mode $mode runner failed"
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

	python3 "$SCRIPT_DIR/check_stress.py" --workload "$workload" \
		--result "$result" --workers "$CQ_RACE_WORKERS" \
		--iterations "$CQ_RACE_ITERATIONS" \
		--cmb-size "$CMB_SIZE" \
		--granularity "$VA_GRANULARITY" \
		--backing-page-size "$BACKING_PAGE_SIZE" |
		tee -a "$output"
	python3 "$SCRIPT_DIR/check_crc.py" --manifest "$result" \
		--log "$kernel_log" \
		--crc-tool "$SCRIPT_DIR/crc32_verify" |
		tee -a "$output"
	check_stats
}

main()
{
	local actual_block_size
	local data_dir
	local workload
	local cq_workload
	local api
	local memory_mode

	(( $# == 0 )) || die "stress_test.sh does not accept positional arguments"
	validate_stress_options
	init_work_dir "xds-stress-$STRESS_MODE"
	trap cleanup EXIT

	preflight
	build_all
	crc_self_test
	save_kernel_identity
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
	cq_workload="$WORK_DIR/cq-race-$STRESS_MODE.workload.tsv"
	python3 "$SCRIPT_DIR/generate_stress_workload.py" \
		--directory "$data_dir" --manifest "$workload" \
		--workers "$STRESS_WORKERS" --iterations "$STRESS_ITERATIONS" \
		--seed "$STRESS_SEED" --mode "$STRESS_MODE" --topology "$TARGET"
	sync

	for api in c python nds-c nds-python; do
		for memory_mode in normal registered; do
			run_stress_api "$api" "$workload" "$memory_mode"
		done
	done
	rm -rf "$data_dir"
	python3 "$SCRIPT_DIR/generate_stress_workload.py" \
		--directory "$data_dir" --manifest "$cq_workload" \
		--workers "$CQ_RACE_WORKERS" --iterations "$CQ_RACE_ITERATIONS" \
		--seed "$STRESS_SEED" --mode "$STRESS_MODE" --topology "$TARGET"
	sync
	for mode in cq-race cq-race-drain; do
		for api in nds-c nds-python; do
			for memory_mode in normal registered; do
				run_cq_race_api "$mode" "$api" "$cq_workload" "$memory_mode"
			done
		done
	done
	log "$STRESS_MODE normal and registered dynamic-VA stress tests passed"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
