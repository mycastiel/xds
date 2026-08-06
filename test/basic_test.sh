#!/usr/bin/env bash

set -Eeuo pipefail

TEST_NAME=basic_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# Map NDS APIs onto the same CMB VA windows as file_p2p c/python (phases are sequential).
api_va_family()
{
	case $1 in
		c | nds-c) printf 'c\n' ;;
		python | nds-python) printf 'python\n' ;;
		*) die "unsupported api '$1'" ;;
	esac
}

make_single_manifest()
{
	local api=$1 directory=$2 label=$3 block_size=$4
	local manifest="$WORK_DIR/$label.$api.single.tsv"
	local tiny_va offset_va cross_va large_va tail_len tail_va boundary_off boundary_len
	local family

	family=$(api_va_family "$api")
	: >"$manifest"
	if [[ $family == c ]]; then
		tiny_va=0
		offset_va=512
		cross_va=$((2 * 1024 * 1024 - 512))
		large_va=$((8 * 1024 * 1024))
		tail_len=1536
	else
		tiny_va=$((16 * 1024 * 1024))
		offset_va=$((16 * 1024 * 1024 + 512))
		cross_va=$((18 * 1024 * 1024 - 512))
		large_va=$((24 * 1024 * 1024))
		tail_len=2560
	fi
	tail_va=$((CMB_SIZE - tail_len))

	append_case "$manifest" "$label-tiny" "$directory/tiny.dat" 0 "$tiny_va" 512 0
	append_case "$manifest" "$label-offset" "$directory/offset.dat" 512 \
		"$offset_va" 1024 0
	boundary_off=$((block_size - 512))
	boundary_len=$((block_size + 512))
	append_case "$manifest" "$label-fs-boundary" \
		"$directory/fs_boundary.dat" "$boundary_off" \
		$((offset_va + 2 * 1024 * 1024)) "$boundary_len" 0
	append_case "$manifest" "$label-page-cross" \
		"$directory/page_cross.dat" $((2 * 1024 * 1024 - 512)) \
		"$cross_va" $((1024 * 1024 + 512)) 0
	append_case "$manifest" "$label-large" "$directory/large.dat" 4096 \
		"$large_va" $((8 * 1024 * 1024)) 0
	append_case "$manifest" "$label-va-tail" "$directory/va_tail.dat" \
		$((8 * block_size - tail_len)) "$tail_va" "$tail_len" 0
	append_case "$manifest" "$label-invalid-va" "$directory/tiny.dat" 0 \
		"$CMB_SIZE" 512 -22
	printf '%s\n' "$manifest"
}

make_block_manifest()
{
	local api=$1 source=$2 source_size=$3 label=$4 topology_boundary=${5:-0}
	local manifest="$WORK_DIR/$label.$api.single.tsv"
	local tiny_va offset_va cross_va large_va boundary_va tail_len tail_va
	local family

	family=$(api_va_family "$api")
	: >"$manifest"
	if [[ $family == c ]]; then
		tiny_va=0
		offset_va=512
		cross_va=$((2 * 1024 * 1024 - 512))
		large_va=$((8 * 1024 * 1024))
		boundary_va=$((20 * 1024 * 1024))
		tail_len=1536
	else
		tiny_va=$((16 * 1024 * 1024))
		offset_va=$((16 * 1024 * 1024 + 512))
		cross_va=$((18 * 1024 * 1024 - 512))
		large_va=$((24 * 1024 * 1024))
		boundary_va=$((36 * 1024 * 1024))
		tail_len=2560
	fi
	tail_va=$((CMB_SIZE - tail_len))
	append_case "$manifest" "$label-tiny" "$source" 0 "$tiny_va" 512 0
	append_case "$manifest" "$label-offset" "$source" 512 "$offset_va" 1024 0
	append_case "$manifest" "$label-page-cross" "$source" \
		$((2 * 1024 * 1024 - 512)) "$cross_va" $((1024 * 1024 + 512)) 0
	append_case "$manifest" "$label-large" "$source" $((4 * 1024 * 1024 + 512)) \
		"$large_va" $((8 * 1024 * 1024)) 0
	if (( topology_boundary )); then
		append_case "$manifest" "$label-topology-boundary" "$source" \
			$((topology_boundary - 512)) "$boundary_va" 4096 0
	fi
	append_case "$manifest" "$label-va-tail" "$source" \
		$((source_size - tail_len)) "$tail_va" "$tail_len" 0
	append_case "$manifest" "$label-invalid-va" "$source" 0 "$CMB_SIZE" 512 -22
	printf '%s\n' "$manifest"
}

make_registered_manifest()
{
	local api=$1 source=$2 label=$3
	local manifest="$WORK_DIR/$label.$api.queued.tsv"
	local base
	local family

	family=$(api_va_family "$api")
	if [[ $family == c ]]; then
		base=0
	else
		base=$((16 * 1024 * 1024))
	fi
	: >"$manifest"
	append_case "$manifest" "$label-$api-tiny" "$source" 0 \
		$((base + 512)) 512 0
	append_case "$manifest" "$label-$api-offset" "$source" 512 \
		$((base + 1024 * 1024 + 512)) 1024 0
	append_case "$manifest" "$label-$api-page-cross" "$source" \
		$((2 * 1024 * 1024 - 512)) \
		$((base + 2 * 1024 * 1024 - 512)) \
		$((1024 * 1024 + 512)) 0
	append_case "$manifest" "$label-$api-reuse" "$source" \
		$((4 * 1024 * 1024 + 512)) \
		$((base + 4 * 1024 * 1024 + 512)) \
		$((2 * 1024 * 1024 + 1536)) 0
	printf '%s\n' "$manifest"
}

make_concurrent_manifest()
{
	local api=$1 mode=$2 directory=$3 block_size=$4 label=$5
	local manifest="$WORK_DIR/$label.$api.$mode.tsv"
	local base
	local -a lengths offsets
	local i
	local family

	family=$(api_va_family "$api")
	if [[ $family == c && $mode == queued ]]; then
		base=$((32 * 1024 * 1024))
	elif [[ $family == python && $mode == queued ]]; then
		base=$((48 * 1024 * 1024))
	elif [[ $family == c ]]; then
		base=$((64 * 1024 * 1024))
	else
		base=$((80 * 1024 * 1024))
	fi
	lengths=($((512 * 1024)) $((512 * 1024 + 512)) \
		$((1024 * 1024 + 1024)) $((2 * 1024 * 1024 + 1536)))
	offsets=(0 512 $((block_size - 512)) $((1024 * 1024 - 512)))
	: >"$manifest"
	for i in 0 1 2 3; do
		append_case "$manifest" "$label-$mode-$i" \
			"$directory/concurrent_$i.dat" "${offsets[$i]}" \
			$((base + i * 4 * 1024 * 1024)) "${lengths[$i]}" 0
	done
	printf '%s\n' "$manifest"
}

prepare_fs_files()
{
	local directory=$1 block_size=$2 phase_seed=$3
	local i

	generate_pattern "$directory/tiny.dat" 512 $((phase_seed + 1))
	generate_pattern "$directory/offset.dat" $((4 * block_size)) $((phase_seed + 2))
	generate_pattern "$directory/fs_boundary.dat" $((8 * block_size)) $((phase_seed + 3))
	generate_pattern "$directory/page_cross.dat" $((4 * 1024 * 1024)) $((phase_seed + 4))
	generate_pattern "$directory/large.dat" $((16 * 1024 * 1024)) $((phase_seed + 5))
	generate_pattern "$directory/va_tail.dat" $((8 * block_size)) $((phase_seed + 6))
	for i in 0 1 2 3; do
		generate_pattern "$directory/concurrent_$i.dat" \
			$(((i + 2) * 1024 * 1024)) $((phase_seed + 10 + i))
	done
	sync
}

run_mem_registration_case()
{
	local label=$1 expected_calls=$2
	local get_param=/sys/module/stub/parameters/get_pa_calls
	local put_param=/sys/module/stub/parameters/put_pa_calls
	local get_before put_before get_after put_after
	shift 2

	[[ -r $get_param && -r $put_param ]] ||
		die "stub PA-list counters are unavailable"
	get_before=$(<"$get_param")
	put_before=$(<"$put_param")
	"$@"
	get_after=$(<"$get_param")
	put_after=$(<"$put_param")
	if (( get_after - get_before != expected_calls )); then
		printf '%s used %d PA-list gets, expected %d\n' \
			"$label" "$((get_after - get_before))" "$expected_calls" >&2
		return 1
	fi
	if (( put_after - put_before != expected_calls )); then
		printf '%s used %d PA-list puts, expected %d\n' \
			"$label" "$((put_after - put_before))" "$expected_calls" >&2
		return 1
	fi
}

run_logged_mem_registration_case()
{
	local api=$1 expected_calls=$2
	local label="mem-registration.$api.registered"
	local output="$WORK_DIR/$label.out"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local status

	shift 2
	dmesg -C
	set +e
	run_mem_registration_case "$api registration" "$expected_calls" \
		"$@" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		die "$api memory registration tests failed"
	fi
}

run_logged_write_rejection_case()
{
	local api=$1
	local label="rw-rejection.$api.rejection"
	local output="$WORK_DIR/$label.out"
	local kernel_log="$WORK_DIR/$label.dmesg"
	local status

	shift
	dmesg -C
	set +e
	"$@" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		die "$api unified read/write rejection tests failed"
	fi
}

run_write_pipeline()
{
	local target=$1 label=$2 phase_seed=$3 topology_boundary=$4 api=$5
	local registered_mem=${6:-}
	local source_offset=$((8 * 1024 * 1024))
	local length=$((2 * 1024 * 1024 + 4096))
	local target_offset=$((16 * 1024 * 1024))
	local cmb_va expected_crc direct_output suffix
	local mixed_output mixed_log read_output read_log write_output write_log
	local -a actual_crc base_command command

	if (( topology_boundary )); then
		target_offset=$((topology_boundary - 4096))
	fi
	if [[ $api == c ]]; then
		cmb_va=$((96 * 1024 * 1024 - 512))
	else
		cmb_va=$((112 * 1024 * 1024 - 512))
	fi
	suffix=$api
	if [[ -n $registered_mem ]]; then
		suffix=$suffix-registered
	fi

	log "$label: $api XDS read to HBM, then XDS write and O_DIRECT read-back"
	direct_output=$(
		"$SCRIPT_DIR/direct_io_test" --mode prepare --device "$target" \
			--source-offset "$source_offset" \
			--target-offset "$target_offset" --length "$length" \
			--seed "$phase_seed"
	)
	expected_crc=${direct_output#expected_crc32=}
	[[ $expected_crc =~ ^0x[0-9a-f]{8}$ ]] ||
		die "$label $api could not obtain the expected CRC32"

	if [[ $api == c ]]; then
		base_command=("$SCRIPT_DIR/rw_api_test")
	else
		base_command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_rw_api_test.py")
	fi
	base_command+=(--topology "$target" --target "$target" \
		--cmb-va "$cmb_va" --length "$length")
	if [[ -n $registered_mem ]]; then
		base_command+=(--registered-mem)
	fi

	read_output="$WORK_DIR/$label.$suffix.rw-read.out"
	read_log="$WORK_DIR/$label.$suffix.rw-read.dmesg"
	command=("${base_command[@]}" --offset "$source_offset" --op read)
	reset_p2p_stats
	dmesg -C
	"${command[@]}" >"$read_output" 2>&1 || {
		dmesg -c >"$read_log"
		cat "$read_output" >&2
		cat "$read_log" >&2
		die "$label $api HBM population failed"
	}
	dmesg -c >"$read_log"
	cat "$read_output"
	mapfile -t actual_crc < <(
		sed -n 's/.*p2p: crc32 .*crc32=\(0x[0-9a-fA-F]\{8\}\).*/\1/p' \
			"$read_log"
	)
	[[ ${#actual_crc[@]} -eq 1 && ${actual_crc[0],,} == "$expected_crc" ]] ||
		die "$label $api HBM CRC32 mismatch: expected $expected_crc, got ${actual_crc[*]:-none}"
	printf 'crc-pass case=read crc32=%s\n' "${actual_crc[0],,}" \
		>>"$read_output"
	check_stats

	write_output="$WORK_DIR/$label.$suffix.rw-write.out"
	write_log="$WORK_DIR/$label.$suffix.rw-write.dmesg"
	command=("${base_command[@]}" --offset "$target_offset" --op write)
	reset_p2p_stats
	dmesg -C
	"${command[@]}" >"$write_output" 2>&1 || {
		dmesg -c >"$write_log"
		cat "$write_output" >&2
		cat "$write_log" >&2
		die "$label $api XDS write failed"
	}
	dmesg -c >"$write_log"
	cat "$write_output"
	if grep -Eq 'p2p: crc32 |PA 0x[0-9a-fA-F]+ content:' "$write_log"; then
		cat "$write_log" >&2
		die "$label $api write emitted read-only diagnostics"
	fi
	check_stats

	"$SCRIPT_DIR/direct_io_test" --mode verify --device "$target" \
		--source-offset "$source_offset" --target-offset "$target_offset" \
		--length "$length" --seed "$phase_seed"

	"$SCRIPT_DIR/direct_io_test" --mode prepare --device "$target" \
		--source-offset "$source_offset" --target-offset "$target_offset" \
		--length "$length" --seed "$phase_seed" >/dev/null
	mixed_output="$WORK_DIR/$label.$suffix.rw-mixed.out"
	mixed_log="$WORK_DIR/$label.$suffix.rw-mixed.dmesg"
	command=("${base_command[@]}" --offset "$target_offset" \
		--source-offset "$source_offset" --op mixed)
	reset_p2p_stats
	dmesg -C
	"${command[@]}" >"$mixed_output" 2>&1 || {
		dmesg -c >"$mixed_log"
		cat "$mixed_output" >&2
		cat "$mixed_log" >&2
		die "$label $api mixed drain failed"
	}
	dmesg -c >"$mixed_log"
	cat "$mixed_output"
	mapfile -t actual_crc < <(
		sed -n 's/.*p2p: crc32 .*crc32=\(0x[0-9a-fA-F]\{8\}\).*/\1/p' \
			"$mixed_log"
	)
	[[ ${#actual_crc[@]} -eq 1 && ${actual_crc[0],,} == "$expected_crc" ]] ||
		die "$label $api mixed read CRC32 mismatch: expected $expected_crc, got ${actual_crc[*]:-none}"
	printf 'crc-pass case=mixed crc32=%s\n' "${actual_crc[0],,}" \
		>>"$mixed_output"
	check_stats
	"$SCRIPT_DIR/direct_io_test" --mode verify --device "$target" \
		--source-offset "$source_offset" --target-offset "$target_offset" \
		--length "$length" --seed "$phase_seed"
}

run_block_matrix()
{
	local target=$1 label=$2 phase_seed=$3 topology_boundary=${4:-0}
	local size api manifest

	size=$(blockdev --getsize64 "$target")
	log "$label: writing deterministic block-device pattern"
	generate_pattern "$target" "$size" "$phase_seed"
	blockdev --flushbufs "$target"
	for api in c python nds-c nds-python; do
		manifest=$(make_block_manifest "$api" "$target" "$size" "$label" \
			"$topology_boundary")
		run_manifest "$api" "$target" single "$manifest" "$label"
		case $api in
			c | python)
				run_write_pipeline "$target" "$label" \
					"$((phase_seed + 100))" \
					"$topology_boundary" "$api"
				;;
		esac
	done
}

run_filesystem_matrix()
{
	local topology=$1 label=$2 block_size=$3 phase_seed=$4
	local directory="$MOUNT_DIR/data"
	local api mode manifest

	mkdir -p "$directory"
	prepare_fs_files "$directory" "$block_size" "$phase_seed"
	for api in c python nds-c nds-python; do
		manifest=$(make_single_manifest "$api" "$directory" "$label" \
			"$block_size")
		run_manifest "$api" "$topology" single "$manifest" "$label"
		for mode in queued threaded; do
			manifest=$(make_concurrent_manifest "$api" "$mode" "$directory" \
				"$block_size" "$label")
			run_manifest "$api" "$topology" "$mode" "$manifest" "$label"
		done
	done
}

run_ext4_sizes()
{
	local target=$1 label=$2
	local actual_block_size block_size phase_seed

	for block_size in 1024 2048 4096; do
		log "$label: ext4 block size $block_size"
		mkfs.ext4 -F -q -b "$block_size" "$target"
		mount -o noatime "$target" "$MOUNT_DIR"
		actual_block_size=$(stat -f -c %S "$MOUNT_DIR")
		[[ $actual_block_size -eq $block_size ]] ||
			die "ext4 block size $actual_block_size does not match $block_size"
		next_seed
		phase_seed=$SEED
		run_filesystem_matrix "$target" "$label-ext4-$block_size" \
			"$block_size" "$phase_seed"
		umount "$MOUNT_DIR"
	done
}

test_direct_nvme()
{
	local api manifest

	log "Direct NVMe block devices and ext4"
	wipe_test_devices
	next_seed
	run_block_matrix "$DEV1" direct-nsid-1 "$SEED"
	log "Unified read/write API rejection coverage"
	run_logged_write_rejection_case c \
		"$SCRIPT_DIR/uapi_rw_test" --target "$DEV1"
	run_logged_write_rejection_case python env \
		"PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
		"$SCRIPT_DIR/python_rw_rejection_test.py" \
		--regular-file "$WORK_DIR/python-regular-write.dat"
	for api in c python; do
		run_write_pipeline "$DEV1" direct-nsid-1-registered \
			"$((SEED + 200))" 0 "$api" registered
	done
	log "Registered-memory API and lifetime"
	run_logged_mem_registration_case c 5 \
		"$SCRIPT_DIR/mem_registration_test" --topology "$DEV1"
	run_logged_mem_registration_case python 3 env \
		"PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
		"$SCRIPT_DIR/python_mem_registration_test.py" --topology "$DEV1"
	log "Registered-memory CRC32"
	for api in c python nds-c nds-python; do
		manifest=$(make_registered_manifest "$api" "$DEV1" registered-mem)
		run_mem_registration_case "$api registered CRC32" 1 \
			run_manifest "$api" "$DEV1" queued "$manifest" \
			registered-mem registered
	done
	next_seed
	run_block_matrix "$DEV2" direct-nsid-2 "$SEED"
	run_ext4_sizes "$DEV1" direct
}

test_nvme_partition()
{
	local part_dev
	local -a partitions

	log "NVMe partition block device and ext4"
	wipe_test_devices
	printf ',,L\n' | sfdisk --quiet "$DEV1" >/dev/null
	PARTITION_CREATED=1
	udevadm settle
	mapfile -t partitions < <(lsblk -nrpo NAME,TYPE "$DEV1" |
		awk '$2 == "part" { print $1 }')
	[[ ${#partitions[@]} -eq 1 ]] ||
		die "expected exactly one partition on $DEV1"
	part_dev=${partitions[0]}
	[[ -b $part_dev ]] || die "$part_dev was not created"

	next_seed
	run_block_matrix "$part_dev" nvme-part "$SEED"
	run_ext4_sizes "$part_dev" nvme-part
	cleanup_storage
}

test_linear_dm()
{
	local usable1

	log "dm-linear block device and ext4"
	create_linear_dm
	usable1=$(($(blockdev --getsz "$DEV1") - DM_DATA_OFFSET_SECTORS))
	next_seed
	run_block_matrix "$DM_PATH" linear "$SEED" $((usable1 * 512))
	run_ext4_sizes "$DM_PATH" linear
	cleanup_storage
}

test_linear_dm_partitions()
{
	local usable1

	log "dm-linear with NVMe partition components"
	create_component_partitions
	create_linear_dm "$PART_DEV1" "$PART_DEV2"
	usable1=$(($(blockdev --getsz "$PART_DEV1") - DM_DATA_OFFSET_SECTORS))
	next_seed
	run_block_matrix "$DM_PATH" linear-part "$SEED" $((usable1 * 512))
	run_ext4_sizes "$DM_PATH" linear-part
	cleanup_storage
}

test_raid0()
{
	log "MD RAID0 block device and ext4"
	create_raid0
	next_seed
	run_block_matrix "$MD_DEV" raid0 "$SEED" $((64 * 1024))
	run_ext4_sizes "$MD_DEV" raid0
	cleanup_storage
}

test_raid0_slot_order()
{
	log "MD RAID0 discovery ordered by member slot"
	wipe_test_devices
	create_raid0 "$DEV2" "$DEV1"
	verify_raid0_slots "$DEV2" "$DEV1"
	next_seed
	run_block_matrix "$MD_DEV" raid0-slot-order "$SEED" $((64 * 1024))
	cleanup_storage
}

test_raid0_partitions()
{
	log "MD RAID0 with NVMe partition members"
	create_component_partitions
	create_raid0 "$PART_DEV1" "$PART_DEV2"
	next_seed
	run_block_matrix "$MD_DEV" raid0-part "$SEED" $((64 * 1024))
	run_ext4_sizes "$MD_DEV" raid0-part
	cleanup_storage
}

main()
{
	(( $# == 0 )) || die "basic_test.sh does not accept positional arguments"
	init_work_dir xds-basic-test
	trap cleanup EXIT

	preflight
	build_all
	crc_self_test
	save_kernel_identity
	load_modules
	log "NDS rejection coverage"
	"$SCRIPT_DIR/nds_api_test" --topology "$DEV1" --mode reject
	env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
		"$SCRIPT_DIR/python_nds_api_test.py" --topology "$DEV1" --mode reject
	test_direct_nvme
	test_nvme_partition
	test_linear_dm
	test_linear_dm_partitions
	test_raid0
	test_raid0_slot_order
	test_raid0_partitions
	log "All XDS C, Python, and NDS API tests passed"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
