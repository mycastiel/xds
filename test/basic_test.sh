#!/usr/bin/env bash

set -Eeuo pipefail

TEST_NAME=basic_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

make_single_manifest()
{
	local api=$1 directory=$2 label=$3 block_size=$4
	local manifest="$WORK_DIR/$label.$api.single.tsv"
	local tiny_va offset_va cross_va large_va tail_len tail_va boundary_off boundary_len

	: >"$manifest"
	if [[ $api == c ]]; then
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

	: >"$manifest"
	if [[ $api == c ]]; then
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

make_concurrent_manifest()
{
	local api=$1 mode=$2 directory=$3 block_size=$4 label=$5
	local manifest="$WORK_DIR/$label.$api.$mode.tsv"
	local base
	local -a lengths offsets
	local i

	if [[ $api == c && $mode == queued ]]; then
		base=$((32 * 1024 * 1024))
	elif [[ $api == python && $mode == queued ]]; then
		base=$((48 * 1024 * 1024))
	elif [[ $api == c ]]; then
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

run_block_matrix()
{
	local target=$1 label=$2 phase_seed=$3 topology_boundary=${4:-0}
	local size api manifest

	size=$(blockdev --getsize64 "$target")
	log "$label: writing deterministic block-device pattern"
	generate_pattern "$target" "$size" "$phase_seed"
	blockdev --flushbufs "$target"
	for api in c python; do
		manifest=$(make_block_manifest "$api" "$target" "$size" "$label" \
			"$topology_boundary")
		run_manifest "$api" "$target" single "$manifest" "$label"
	done
}

run_filesystem_matrix()
{
	local topology=$1 label=$2 block_size=$3 phase_seed=$4
	local directory="$MOUNT_DIR/data"
	local api mode manifest

	mkdir -p "$directory"
	prepare_fs_files "$directory" "$block_size" "$phase_seed"
	for api in c python; do
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
	log "Direct NVMe block devices and ext4"
	wipe_test_devices
	next_seed
	run_block_matrix "$DEV1" direct-nsid-1 "$SEED"
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
	load_modules
	test_direct_nvme
	test_nvme_partition
	test_linear_dm
	test_linear_dm_partitions
	test_raid0
	test_raid0_slot_order
	test_raid0_partitions
	log "All XDS C API and Python API tests passed"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
