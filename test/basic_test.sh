#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly CMB_SIZE=$((128 << 20))
readonly CMB_MAX_PAGES=$((CMB_SIZE >> 12))
readonly NS_SIZE=$((64 << 20))
readonly STUB_PAGE_SIZE=$((2 << 20))
readonly DM_DATA_OFFSET=$((1 << 20))
readonly DM_DATA_OFFSET_SECTORS=$((DM_DATA_OFFSET / 512))
readonly DM_NAME=xds_linear
readonly DM_PATH=/dev/mapper/$DM_NAME
readonly MD_DEV=/dev/md/xds_test

WORK_BASE=${XDS_TEST_WORKDIR:-}
WORK_DIR=
KEEP_WORK_DIR=${XDS_TEST_KEEP_WORKDIR:-0}
BUILD_JOBS=${XDS_TEST_BUILD_JOBS:-8}
DEV1=
DEV2=
CTRL_NAME=
BAR2_START=
MOUNT_DIR=
DM_CREATED=0
MD_CREATED=0
DEBUGFS_MOUNTED=0
SEED=100

log()
{
	printf '\n==> %s\n' "$*"
}

die()
{
	printf 'basic_test: %s\n' "$*" >&2
	return 1
}

cleanup_storage()
{
	set +e
	if [[ -n ${MOUNT_DIR:-} ]] && mountpoint -q "$MOUNT_DIR"; then
		umount "$MOUNT_DIR"
	fi
	if (( DM_CREATED )); then
		dmsetup remove --retry "$DM_NAME" >/dev/null 2>&1
		DM_CREATED=0
	fi
	if (( MD_CREATED )); then
		mdadm --stop "$MD_DEV" >/dev/null 2>&1
		wipefs -a -f "$DEV1" "$DEV2" >/dev/null 2>&1
		MD_CREATED=0
	fi
	udevadm settle >/dev/null 2>&1
	set -e
}

cleanup()
{
	local status=$?

	trap - EXIT
	cleanup_storage
	set +e
	rmmod p2p_dev >/dev/null 2>&1
	rmmod stub >/dev/null 2>&1
	if (( DEBUGFS_MOUNTED )); then
		umount /sys/kernel/debug >/dev/null 2>&1
	fi
	set -e

	if (( status == 0 )) && [[ $KEEP_WORK_DIR != 1 ]]; then
		rm -rf -- "$WORK_DIR"
	else
		printf 'XDS test artifacts: %s\n' "$WORK_DIR" >&2
	fi
	exit "$status"
}

require_commands()
{
	local command
	local missing=0

	for command in awk blockdev cat chmod dmesg dmsetup findmnt grep insmod \
		lsblk make mdadm mkdir mkfs.ext4 mktemp modinfo mount mountpoint \
		python3 readlink rm rmmod sed stat sync swapon tr udevadm umount \
		wipefs; do
		if ! command -v "$command" >/dev/null 2>&1; then
			printf 'missing required command: %s\n' "$command" >&2
			missing=1
		fi
	done
	(( missing == 0 )) || die "install the missing VM test dependencies"
}

check_not_in_use()
{
	local dev=$1
	local name=${dev##*/}
	local child
	local mountpoints

	mountpoints=$(lsblk -nrpo MOUNTPOINT "$dev" | awk 'NF { print }')
	[[ -z $mountpoints ]] || die "$dev or one of its children is mounted"
	while read -r child; do
		if swapon --noheadings --show=NAME | grep -Fxq "$child"; then
			die "$child is active swap"
		fi
	done < <(lsblk -nrpo NAME "$dev")
	if compgen -G "/sys/class/block/$name/holders/*" >/dev/null; then
		die "$dev has active block-device holders"
	fi
	if findmnt -rn -S "$dev" >/dev/null 2>&1; then
		die "$dev backs a mounted filesystem"
	fi
}

find_pci_device()
{
	local path

	path=$(readlink -f "/sys/class/nvme/$CTRL_NAME")
	while [[ $path == /sys/devices/* ]]; do
		if [[ -f $path/resource && -f $path/vendor ]]; then
			printf '%s\n' "$path"
			return 0
		fi
		path=${path%/*}
	done
	return 1
}

read_controller_attr()
{
	local attr=$1
	local path

	for path in "/sys/class/nvme/$CTRL_NAME/$attr" \
		"/sys/class/nvme/$CTRL_NAME/device/$attr"; do
		if [[ -r $path ]]; then
			tr -d '[:space:]' <"$path"
			return 0
		fi
	done
	return 1
}

preflight()
{
	local name1 name2 ctrl1 ctrl2 nsid1 nsid2 pci_path
	local bar_start bar_end bar_flags bar_size cmbloc cmbsz cmb_sqes

	[[ $EUID -eq 0 ]] || die "run as root"
	: "${XDS_DEV_1:?XDS_DEV_1 must name the first disposable NVMe namespace}"
	: "${XDS_DEV_2:?XDS_DEV_2 must name the second disposable NVMe namespace}"
	[[ $BUILD_JOBS =~ ^[1-9][0-9]*$ ]] ||
		die "XDS_TEST_BUILD_JOBS must be a positive integer"
	require_commands

	DEV1=$(readlink -f -- "$XDS_DEV_1")
	DEV2=$(readlink -f -- "$XDS_DEV_2")
	[[ -b $DEV1 && -b $DEV2 ]] || die "XDS_DEV_1 and XDS_DEV_2 must be block devices"
	[[ $DEV1 != "$DEV2" ]] || die "XDS_DEV_1 and XDS_DEV_2 must differ"
	name1=${DEV1##*/}
	name2=${DEV2##*/}
	[[ $name1 =~ ^nvme([0-9]+)n([0-9]+)$ ]] || die "$DEV1 is not a whole NVMe namespace"
	ctrl1=nvme${BASH_REMATCH[1]}
	[[ $name2 =~ ^nvme([0-9]+)n([0-9]+)$ ]] || die "$DEV2 is not a whole NVMe namespace"
	ctrl2=nvme${BASH_REMATCH[1]}
	[[ $ctrl1 == "$ctrl2" ]] || die "the namespaces must use one NVMe controller"
	CTRL_NAME=$ctrl1
	[[ ! -e /sys/class/block/$name1/partition &&
	   ! -e /sys/class/block/$name2/partition ]] || die "partitions are unsupported"
	[[ $(blockdev --getsize64 "$DEV1") -eq $NS_SIZE &&
	   $(blockdev --getsize64 "$DEV2") -eq $NS_SIZE ]] ||
		die "both namespaces must be exactly 64 MiB"
	[[ -r /sys/class/block/$name1/nsid && -r /sys/class/block/$name2/nsid ]] ||
		die "namespace ID sysfs attributes are missing"
	nsid1=$(<"/sys/class/block/$name1/nsid")
	nsid2=$(<"/sys/class/block/$name2/nsid")
	[[ $nsid1 != "$nsid2" ]] || die "the namespaces must have different NSIDs"
	check_not_in_use "$DEV1"
	check_not_in_use "$DEV2"

	[[ -r /sys/module/nvme/parameters/use_cmb_sqes ]] ||
		die "cannot verify nvme use_cmb_sqes"
	cmb_sqes=$(tr '[:upper:]' '[:lower:]' </sys/module/nvme/parameters/use_cmb_sqes)
	case $cmb_sqes in
		n | 0 | no | false) ;;
		*) die "boot/load NVMe with nvme.use_cmb_sqes=0" ;;
	esac
	cmbloc=$(read_controller_attr cmbloc) || die "NVMe CMBLOC is unavailable"
	cmbsz=$(read_controller_attr cmbsz) || die "NVMe CMBSZ is unavailable"
	(( cmbsz != 0 )) || die "the NVMe controller has no enabled CMB"
	(( (cmbloc & 0x7) == 2 && ((cmbloc >> 12) & 0xfffff) == 0 )) ||
		die "the CMB must start at offset zero in BAR2"

	pci_path=$(find_pci_device) || die "cannot locate the NVMe PCI device"
	read -r bar_start bar_end bar_flags < <(sed -n '3p' "$pci_path/resource")
	[[ -n $bar_start && -n $bar_end && -n $bar_flags ]] || die "BAR2 is missing"
	BAR2_START=$((bar_start))
	bar_size=$((bar_end - bar_start + 1))
	[[ $BAR2_START -gt 0 && $bar_size -ge $CMB_SIZE ]] ||
		die "BAR2 must provide at least 128 MiB of active CMB space"
	(( bar_flags & 0x200 )) || die "BAR2 is not a memory resource"
	(( BAR2_START % STUB_PAGE_SIZE == 0 )) || die "BAR2 is not 2 MiB aligned"

	KSRC=${KSRC:-/lib/modules/$(uname -r)/build}
	[[ -d $KSRC ]] || die "kernel build tree $KSRC does not exist"
	export KSRC

	printf 'controller=%s nsid={%s,%s} BAR2=[0x%x,0x%x)\n' \
		"$CTRL_NAME" "$nsid1" "$nsid2" "$BAR2_START" \
		"$((BAR2_START + bar_size))"
}

build_all()
{
	log "Building normal and CRC-enabled modules plus both userspace APIs (-j$BUILD_JOBS)"
	make -C "$REPO_ROOT" clean
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1
	make -C "$REPO_ROOT" clean
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1 KCFLAGS=-DCALC_CRC32
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" test
	(
		cd "$REPO_ROOT/file_p2p"
		python3 setup.py build_ext --inplace --force
	)
	PYTHONPATH="$REPO_ROOT/file_p2p" python3 -c 'import file_p2p'
}

crc_self_test()
{
	local empty_crc vector_crc

	: >"$WORK_DIR/empty"
	printf '123456789' >"$WORK_DIR/vector"
	empty_crc=$("$SCRIPT_DIR/crc32_verify" -f "$WORK_DIR/empty" | awk '{print $NF}')
	vector_crc=$("$SCRIPT_DIR/crc32_verify" -f "$WORK_DIR/vector" | awk '{print $NF}')
	[[ $empty_crc == 0x00000000 ]] || die "empty CRC32 vector failed"
	[[ $vector_crc == 0xcbf43926 ]] || die "123456789 CRC32 vector failed"
}

load_modules()
{
	log "Loading CMB stub at PA 0x$(printf '%x' "$BAR2_START")"
	rmmod p2p_dev >/dev/null 2>&1 || true
	rmmod stub >/dev/null 2>&1 || true
	insmod "$REPO_ROOT/stub.ko" base_pa="$BAR2_START" max_pages="$CMB_MAX_PAGES"
	insmod "$REPO_ROOT/p2p_dev.ko"
	[[ -c /dev/p2p_device ]] || die "/dev/p2p_device was not created"
	chmod 600 /dev/p2p_device

	if ! mountpoint -q /sys/kernel/debug; then
		mount -t debugfs debugfs /sys/kernel/debug
		DEBUGFS_MOUNTED=1
	fi
	[[ -w /sys/kernel/debug/p2p_device/summary ]] ||
		die "p2p debugfs summary is unavailable"
}

next_seed()
{
	SEED=$((SEED + 1))
}

generate_pattern()
{
	local path=$1
	local size=$2
	local seed=$3

	python3 "$SCRIPT_DIR/generate_pattern.py" \
		--output "$path" --size "$size" --seed "$seed"
}

append_case()
{
	local manifest=$1 id=$2 path=$3 offset=$4 va=$5 length=$6 expected=$7

	printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$id" "$path" "$offset" "$va" "$length" "$expected" >>"$manifest"
}

check_stats()
{
	local summary=/sys/kernel/debug/p2p_device/summary

	awk '$1 == "io_issued:" { found = 1; if ($2 > 0) ok = 1 } END { exit !(found && ok) }' \
		"$summary" || die "p2p issued no NVMe requests"
	awk '$1 == "io_bytes:" { found = 1; if ($2 > 0) ok = 1 } END { exit !(found && ok) }' \
		"$summary" || die "p2p transferred no bytes"
	grep -Eq '^io_inflight: 0$' "$summary" || die "p2p I/O remains inflight"
	grep -Eq '^io_failed: 0$' "$summary" || die "p2p completion failure recorded"
	grep -Eq '^io_issue_failed: 0$' "$summary" || die "p2p submission failure recorded"
}

run_manifest()
{
	local api=$1 topology=$2 mode=$3 manifest=$4 label=$5
	local output="$WORK_DIR/$label.$api.$mode.out"
	local kernel_log="$WORK_DIR/$label.$api.$mode.dmesg"
	local status
	local -a command

	: >/sys/kernel/debug/p2p_device/summary
	dmesg -C
	if [[ $api == c ]]; then
		command=("$SCRIPT_DIR/c_api_test" --topology "$topology" \
			--manifest "$manifest" --mode "$mode")
	else
		command=(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_api_test.py" --topology "$topology" \
			--manifest "$manifest" --mode "$mode")
	fi

	set +e
	"${command[@]}" >"$output" 2>&1
	status=$?
	set -e
	dmesg -c >"$kernel_log"
	cat "$output"
	if (( status != 0 )); then
		cat "$kernel_log" >&2
		die "$label $api $mode runner failed"
	fi
	python3 "$SCRIPT_DIR/check_crc.py" --manifest "$manifest" \
		--log "$kernel_log" --crc-tool "$SCRIPT_DIR/crc32_verify"
	check_stats
}

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

wipe_test_devices()
{
	wipefs -a -f "$DEV1" "$DEV2"
	udevadm settle
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

test_linear_dm()
{
	local actual_sectors
	local sectors1
	local sectors2
	local table
	local usable1
	local usable2

	log "dm-linear block device and ext4"
	wipe_test_devices
	if dmsetup info "$DM_NAME" >/dev/null 2>&1 || [[ -e $DM_PATH ]]; then
		die "$DM_NAME already exists"
	fi
	sectors1=$(blockdev --getsz "$DEV1")
	sectors2=$(blockdev --getsz "$DEV2")
	(( sectors1 > DM_DATA_OFFSET_SECTORS )) ||
		die "$DEV1 is too small for the 1 MiB data offset"
	(( sectors2 > DM_DATA_OFFSET_SECTORS )) ||
		die "$DEV2 is too small for the 1 MiB data offset"
	usable1=$((sectors1 - DM_DATA_OFFSET_SECTORS))
	usable2=$((sectors2 - DM_DATA_OFFSET_SECTORS))
	printf -v table '0 %s linear %s %s\n%s %s linear %s %s' \
		"$usable1" "$DEV1" "$DM_DATA_OFFSET_SECTORS" \
		"$usable1" "$usable2" "$DEV2" "$DM_DATA_OFFSET_SECTORS"
	if ! dmsetup create "$DM_NAME" --table "$table"; then
		dmsetup remove --retry "$DM_NAME" >/dev/null 2>&1 || true
		die "failed to create $DM_PATH"
	fi
	DM_CREATED=1
	udevadm settle
	[[ -b $DM_PATH ]] || die "$DM_PATH was not created"
	table=$(dmsetup table "$DM_NAME")
	awk -v offset="$DM_DATA_OFFSET_SECTORS" \
		-v usable1="$usable1" -v usable2="$usable2" '
		NR == 1 {
			valid = ($1 == 0 && $2 == usable1 && $3 == "linear" &&
				 $5 == offset)
		}
		NR == 2 {
			valid = valid && ($1 == usable1 && $2 == usable2 &&
				 $3 == "linear" && $5 == offset)
		}
		END { exit !(NR == 2 && valid) }
	' <<<"$table" || die "$DM_PATH does not use the 1 MiB data offsets"
	actual_sectors=$(blockdev --getsz "$DM_PATH")
	[[ $actual_sectors -eq $((usable1 + usable2)) ]] ||
		die "$DM_PATH size does not match the usable namespace space"
	next_seed
	run_block_matrix "$DM_PATH" linear "$SEED" $((usable1 * 512))
	run_ext4_sizes "$DM_PATH" linear
	cleanup_storage
}

test_raid0()
{
	log "MD RAID0 block device and ext4"
	wipe_test_devices
	mkdir -p /dev/md
	[[ ! -e $MD_DEV ]] || die "$MD_DEV already exists"
	if ! mdadm --create "$MD_DEV" --run --metadata=1.2 --level=0 \
		--raid-devices=2 --chunk=64 "$DEV1" "$DEV2"; then
		mdadm --stop "$MD_DEV" >/dev/null 2>&1 || true
		wipe_test_devices
		die "failed to create $MD_DEV"
	fi
	MD_CREATED=1
	udevadm settle
	[[ -b $MD_DEV ]] || die "$MD_DEV was not created"
	next_seed
	run_block_matrix "$MD_DEV" raid0 "$SEED" $((64 * 1024))
	run_ext4_sizes "$MD_DEV" raid0
	cleanup_storage
}

main()
{
	(( $# == 0 )) || die "basic_test.sh does not accept positional arguments"
	if [[ -z $WORK_BASE ]]; then
		WORK_DIR=$(mktemp -d /tmp/xds-basic-test.XXXXXX)
	else
		mkdir -p "$WORK_BASE"
		WORK_BASE=$(readlink -f "$WORK_BASE")
		WORK_DIR=$(mktemp -d "$WORK_BASE/xds-basic-test.XXXXXX")
	fi
	MOUNT_DIR=$WORK_DIR/mnt
	mkdir -p "$MOUNT_DIR"
	trap cleanup EXIT

	preflight
	build_all
	crc_self_test
	load_modules
	test_direct_nvme
	test_linear_dm
	test_raid0
	log "All XDS C API and Python API tests passed"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
