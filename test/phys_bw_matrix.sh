#!/usr/bin/env bash

# Physical-machine NDS direct-read bandwidth matrix (C + Python).
#
# Sweeps block sizes × file sizes, runs both nds_bw_bench and
# python_nds_bw_bench.py against real files on a mounted filesystem.
#
# Required env:
#   XDS_DEV_1            block device used as NDS topology (and usually the FS device)
#   XDS_BW_NPU_DEVICE    NPU id for aclrtSetDevice + aclrtMalloc HBM (physical)
#
# Optional env:
#   XDS_BW_MOUNT_DIR     mountpoint (default: create under workdir)
#   XDS_BW_FILE_SIZES    comma list (default: 64M,256M,1G)
#   XDS_BW_BS_LIST       comma list fixed BS (default: 4K,32K,128K,1M)
#   XDS_BW_QD            queue depth (default: 64)
#   XDS_BW_PASSES        passes over each file (default: 1)
#   XDS_BW_TOTAL_SIZE    if set, transfer this much instead of whole file
#   XDS_BW_ACL_POLICY    aclrtMemMallocPolicy (default: 0 = HUGE_FIRST)
#   XDS_BW_CMB_VA        only for VM/stub CMB (ignored when XDS_BW_NPU_DEVICE set)
#   XDS_BW_MEM_MODE      comma list: register,noregister (default: register)
#   XDS_BW_PATTERN       comma list: sequential,random (default: sequential)
#   XDS_BW_APIS          c,python or subset (default: c,python)
#   XDS_BW_SKIP_MKFS     1 = do not wipe/mkfs; use existing FS on XDS_DEV_1
#   XDS_BW_SKIP_BUILD    1 = skip rebuild
#   XDS_KERNEL_VARIANT   kasan|nokasan when loading modules via common.sh
#   KSRC                 kernel build tree for modules
#   ASCEND_ACL_LIB       path to libascendcl.so if not on the default search path
#
# Physical machine (source Ascend env first; preserve it under sudo -E):
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E env XDS_DEV_1=/dev/nvme0n1 XDS_BW_NPU_DEVICE=0 XDS_BW_SKIP_MKFS=1 \
#     XDS_BW_MOUNT_DIR=/mnt/data XDS_BW_FILE_SIZES=256M \
#     XDS_BW_BS_LIST=128K ./test/phys_bw_matrix.sh
#
# VM / stub CMB (no NPU): set XDS_BW_NPU_DEVICE=-1 and XDS_BW_CMB_VA=0.

set -Eeuo pipefail

TEST_NAME=phys_bw_matrix
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FILE_SIZES=${XDS_BW_FILE_SIZES:-64M,256M,1G}
BS_LIST=${XDS_BW_BS_LIST:-4K,32K,128K,1M}
QD=${XDS_BW_QD:-64}
PASSES=${XDS_BW_PASSES:-1}
NPU_DEVICE=${XDS_BW_NPU_DEVICE:--1}
ACL_POLICY=${XDS_BW_ACL_POLICY:-0}
CMB_VA=${XDS_BW_CMB_VA:-0}
MEM_MODE=${XDS_BW_MEM_MODE:-register}
PATTERN=${XDS_BW_PATTERN:-sequential}
APIS=${XDS_BW_APIS:-c,python}
SKIP_MKFS=${XDS_BW_SKIP_MKFS:-0}
SKIP_BUILD=${XDS_BW_SKIP_BUILD:-0}
RESULTS_TSV=
SUMMARY=

parse_size_bytes()
{
	local s=$1
	local n=${s%[KkMmGg]}
	case $s in
		*[Kk]) printf '%s\n' $((n << 10)) ;;
		*[Mm]) printf '%s\n' $((n << 20)) ;;
		*[Gg]) printf '%s\n' $((n << 30)) ;;
		*) printf '%s\n' "$s" ;;
	esac
}

split_csv()
{
	local raw=${1// /}
	local part
	IFS=',' read -ra parts <<<"$raw"
	for part in "${parts[@]}"; do
		[[ -n $part ]] || continue
		printf '%s\n' "$part"
	done
}

record()
{
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$RESULTS_TSV"
}

ensure_modules()
{
	if [[ ! -c /dev/p2p_device ]] || ! lsmod | grep -q '^p2p_dev'; then
		load_modules
	fi
}

build_benches()
{
	[[ $SKIP_BUILD == 1 ]] && return 0
	log "Building modules + nds_bw_bench (-j$BUILD_JOBS)"
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1
	make -C "$SCRIPT_DIR" -j"$BUILD_JOBS" nds_bw_bench
	# Rebuild Python nds extension via existing file_p2p setup path.
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" python >/dev/null 2>&1 ||
		(cd "$REPO_ROOT/file_p2p" && python3 setup.py build_ext --inplace)
}

prepare_mount()
{
	if [[ -n ${XDS_BW_MOUNT_DIR:-} ]]; then
		MOUNT_DIR=$XDS_BW_MOUNT_DIR
		mkdir -p "$MOUNT_DIR"
		if ! mountpoint -q "$MOUNT_DIR"; then
			[[ $SKIP_MKFS == 1 ]] || die "$MOUNT_DIR is not mounted and XDS_BW_SKIP_MKFS=1"
			mount -o noatime "$DEV1" "$MOUNT_DIR"
		fi
		return
	fi
	if [[ $SKIP_MKFS == 1 ]]; then
		die "XDS_BW_SKIP_MKFS=1 requires XDS_BW_MOUNT_DIR"
	fi
	wipe_test_devices
	mkfs.ext4 -F -q -b 4096 "$DEV1"
	mount -o noatime "$DEV1" "$MOUNT_DIR"
}

make_file()
{
	local path=$1 size=$2
	local bytes

	bytes=$(parse_size_bytes "$size")
	if [[ -f $path ]]; then
		local actual
		actual=$(stat -c %s -- "$path")
		(( actual == bytes )) && return 0
	fi
	log "Creating $path ($size)"
	# Prefer fallocate; fall back to dd.
	if ! fallocate -l "$bytes" "$path" 2>/dev/null; then
		dd if=/dev/zero of="$path" bs=1M count=$((bytes >> 20)) status=none
		local rem=$((bytes & ((1 << 20) - 1)))
		if (( rem )); then
			dd if=/dev/zero of="$path" bs="$rem" count=1 seek=$((bytes >> 20)) \
				conv=notrunc status=none
		fi
	fi
	# Touch data so reads are not all sparse zeroes on some FS setups.
	dd if=/dev/urandom of="$path" bs=4K count=16 conv=notrunc status=none 2>/dev/null || true
	sync
}

run_one()
{
	local api=$1 file=$2 file_size=$3 bs=$4 mem=$5 pattern=$6
	local label out mib iops total_arg args=() reg_flag=1

	total_arg=${XDS_BW_TOTAL_SIZE:-$file_size}
	label="$api.$pattern.$bs.$file_size.$mem"
	log "bench $label"

	args=(--topology "$DEV1" --target "$file"
		--total-size "$total_arg" --io-min "$bs" --io-max "$bs"
		--queue-depth "$QD" --passes "$PASSES")
	if (( NPU_DEVICE >= 0 )); then
		args+=(--npu-device "$NPU_DEVICE" --acl-policy "$ACL_POLICY")
	else
		args+=(--cmb-va "$CMB_VA")
	fi
	case $pattern in
		sequential) args+=(--sequential) ;;
		random) args+=(--random) ;;
		*) die "bad pattern $pattern" ;;
	esac
	case $mem in
		register) reg_flag=1 ;;
		noregister)
			args+=(--no-register-mem)
			reg_flag=0
			;;
		*) die "bad mem $mem" ;;
	esac

	local span=$((QD * $(parse_size_bytes "$bs")))
	if (( NPU_DEVICE < 0 )); then
		(( span <= CMB_SIZE )) ||
			die "$label needs ${span}B CMB window (>${CMB_SIZE})"
	fi

	ensure_modules
	if [[ $api == c ]]; then
		out=$("$SCRIPT_DIR/nds_bw_bench" "${args[@]}" | tee /dev/stderr)
	elif [[ $api == python ]]; then
		out=$(env "PYTHONPATH=$REPO_ROOT/file_p2p" python3 \
			"$SCRIPT_DIR/python_nds_bw_bench.py" "${args[@]}" |
			tee /dev/stderr)
	else
		die "bad api $api"
	fi

	mib=$(printf '%s\n' "$out" | awk '/^RESULT / {
		for (i = 1; i <= NF; i++)
			if ($i ~ /^mib_s=/) { sub(/^mib_s=/, "", $i); print $i }
	}' | tail -n1)
	iops=$(printf '%s\n' "$out" | awk '/^RESULT / {
		for (i = 1; i <= NF; i++)
			if ($i ~ /^iops=/) { sub(/^iops=/, "", $i); print $i }
	}' | tail -n1)
	[[ -n $mib && -n $iops ]] || die "failed to parse RESULT for $label"
	record "$api" "$pattern" "$bs" "$file_size" "$QD" "$mem" "$mib" "$iops" \
		"$total_arg" "register_mem=$reg_flag"
	printf '%s: %s MiB/s  %s IOPS\n' "$label" "$mib" "$iops"
}

write_summary()
{
	{
		printf 'Physical NDS bandwidth matrix\n'
		printf 'topology=%s mount=%s qd=%s passes=%s npu=%s\n' \
			"$DEV1" "$MOUNT_DIR" "$QD" "$PASSES" "$NPU_DEVICE"
		printf 'file_sizes=%s bs=%s apis=%s mem=%s pattern=%s\n\n' \
			"$FILE_SIZES" "$BS_LIST" "$APIS" "$MEM_MODE" "$PATTERN"
		column -t -s $'\t' "$RESULTS_TSV"
	} | tee "$SUMMARY"
}

main()
{
	local api mem pattern bs fsize file

	(( $# == 0 )) || die "phys_bw_matrix.sh does not accept positional args"
	[[ -n ${XDS_DEV_1:-} ]] || die "set XDS_DEV_1 to the topology/block device"
	[[ -n ${XDS_BW_NPU_DEVICE+x} ]] ||
		die "set XDS_BW_NPU_DEVICE (NPU id, or -1 for VM/stub --cmb-va)"
	# common.sh preflight needs DEV2 too for the standard flow.
	[[ -n ${XDS_DEV_2:-} ]] || export XDS_DEV_2=${XDS_DEV_2_FALLBACK:-$XDS_DEV_1}

	init_work_dir xds-phys-bw
	trap cleanup EXIT
	RESULTS_TSV=$WORK_DIR/results.tsv
	SUMMARY=$WORK_DIR/summary.txt
	printf 'api\tpattern\tbs\tfile_size\tqd\tmem\tmib_s\tiops\ttotal\tnote\n' \
		>"$RESULTS_TSV"

	preflight
	build_benches
	save_kernel_identity
	ensure_modules
	prepare_mount

	local data_dir=$MOUNT_DIR/xds-bw-data
	mkdir -p "$data_dir"

	for fsize in $(split_csv "$FILE_SIZES"); do
		file=$data_dir/bw-$fsize.bin
		make_file "$file" "$fsize"
		for bs in $(split_csv "$BS_LIST"); do
			for api in $(split_csv "$APIS"); do
				for mem in $(split_csv "$MEM_MODE"); do
					case $mem in
						register | noregister) ;;
						*) die "XDS_BW_MEM_MODE entries must be register or noregister" ;;
					esac
					for pattern in $(split_csv "$PATTERN"); do
						case $pattern in
							sequential | random) ;;
							*) die "XDS_BW_PATTERN entries must be sequential or random" ;;
						esac
						run_one "$api" "$file" "$fsize" "$bs" "$mem" "$pattern"
					done
				done
			done
		done
	done

	write_summary
	log "Results: $RESULTS_TSV"
	log "Summary: $SUMMARY"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
