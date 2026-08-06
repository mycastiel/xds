#!/usr/bin/env bash

# NPU bandwidth matrix for NDS direct read on physical Ascend machines.
#
# Sweeps: NPU devices × block sizes × queue depths × register_mem modes,
# runs nds_bw_bench against each combination and records a TSV result table.
#
# Required env:
#   XDS_DEV_1                NDS topology block device (e.g. /dev/nvme0n1)
#   XDS_BW_NPU_DEVICES       comma list of NPU ids (e.g. 0,1,2,3)
#   XDS_BW_TARGET_FILE       file to read (must be on a filesystem backed by XDS_DEV_1)
#
# Optional env:
#   XDS_BW_BS_LIST           comma list (default: 32K,128K,1M)
#   XDS_BW_QD_LIST           comma list (default: 16,32,64,128)
#   XDS_BW_TOTAL_SIZE        transfer size per run (default: 1G)
#   XDS_BW_PASSES            passes per run (default: 1)
#   XDS_BW_MEM_MODE          comma list: register,noregister (default: register)
#   XDS_BW_PATTERN           comma list: sequential,random (default: sequential)
#   XDS_BW_ACL_POLICY        aclrt malloc policy (default: 0 = HUGE_FIRST)
#   XDS_BW_SKIP_BUILD        1 = skip rebuild of nds_bw_bench
#   XDS_BW_CONCURRENT        1 = run all NPU devices concurrently (default: sequential)
#   XDS_KERNEL_VARIANT       kasan|nokasan when loading modules via common.sh
#   KSRC                     kernel build tree for modules
#   ASCEND_ACL_LIB           path to libascendcl.so if not on default search path
#
# Example:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E env XDS_DEV_1=/dev/nvme0n1 \
#     XDS_BW_NPU_DEVICES=0,1,2,3 \
#     XDS_BW_TARGET_FILE=/mnt/data/bw.bin \
#     XDS_BW_BS_LIST=32K,128K,1M \
#     XDS_BW_QD_LIST=16,32,64 \
#     XDS_BW_MEM_MODE=register,noregister \
#     XDS_BW_CONCURRENT=1 \
#     ./test/npu_bw_matrix.sh

set -Eeuo pipefail

TEST_NAME=npu_bw_matrix
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

NPU_DEVICES=${XDS_BW_NPU_DEVICES:-}
BS_LIST=${XDS_BW_BS_LIST:-32K,128K,1M}
QD_LIST=${XDS_BW_QD_LIST:-16,32,64,128}
TOTAL_SIZE=${XDS_BW_TOTAL_SIZE:-1G}
PASSES=${XDS_BW_PASSES:-1}
ACL_POLICY=${XDS_BW_ACL_POLICY:-0}
MEM_MODE=${XDS_BW_MEM_MODE:-register}
PATTERN=${XDS_BW_PATTERN:-sequential}
TARGET_FILE=${XDS_BW_TARGET_FILE:-}
SKIP_BUILD=${XDS_BW_SKIP_BUILD:-0}
CONCURRENT=${XDS_BW_CONCURRENT:-0}
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
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$RESULTS_TSV"
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
}

run_one()
{
	local npu=$1 bs=$2 qd=$3 mem=$4 pattern=$5
	local label out mib iops reg_flag args=()

	label="npu${npu}.${pattern}.${bs}.qd${qd}.${mem}"
	log "bench $label"

	args=(
		--topology "$DEV1"
		--target "$TARGET_FILE"
		--npu-device "$npu"
		--acl-policy "$ACL_POLICY"
		--total-size "$TOTAL_SIZE"
		--io-min "$bs"
		--io-max "$bs"
		--queue-depth "$qd"
		--passes "$PASSES"
	)
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

	out=$("$SCRIPT_DIR/nds_bw_bench" "${args[@]}" 2>&1)
	printf '%s\n' "$out" | sed -n 's/^RESULT //p' | tail -n1 >"$WORK_DIR/result.${label}.tmp"
	mib=$(awk '/mib_s=/ { for (i=1;i<=NF;i++) if ($i ~ /^mib_s=/) { sub(/^mib_s=/,"",$i); print $i } }' "$WORK_DIR/result.${label}.tmp" | tail -n1)
	iops=$(awk '/iops=/ { for (i=1;i<=NF;i++) if ($i ~ /^iops=/) { sub(/^iops=/,"",$i); print $i } }' "$WORK_DIR/result.${label}.tmp" | tail -n1)
	[[ -n $mib && -n $iops ]] || die "failed to parse RESULT for $label"
	record "$npu" "$pattern" "$bs" "$qd" "$mem" "$mib" "$iops" "register_mem=$reg_flag"
	printf '%s: %s MiB/s  %s IOPS\n' "$label" "$mib" "$iops"
}

run_device_matrix()
{
	local npu=$1
	local bs qd mem pattern

	for bs in $(split_csv "$BS_LIST"); do
		for qd in $(split_csv "$QD_LIST"); do
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
					run_one "$npu" "$bs" "$qd" "$mem" "$pattern"
				done
			done
		done
	done
}

write_summary()
{
	{
		printf 'NPU NDS bandwidth matrix\n'
		printf 'topology=%s target=%s total=%s passes=%s npu=%s\n' \
			"$DEV1" "$TARGET_FILE" "$TOTAL_SIZE" "$PASSES" "$NPU_DEVICES"
		printf 'bs=%s qd=%s mem=%s pattern=%s\n\n' \
			"$BS_LIST" "$QD_LIST" "$MEM_MODE" "$PATTERN"
		column -t -s $'\t' "$RESULTS_TSV"
	} | tee "$SUMMARY"
}

main()
{
	local npu

	(( $# == 0 )) || die "npu_bw_matrix.sh does not accept positional args"
	[[ -n ${XDS_DEV_1:-} ]] || die "set XDS_DEV_1 to the topology/block device"
	[[ -n ${XDS_BW_NPU_DEVICES:-} ]] || die "set XDS_BW_NPU_DEVICES (comma list of NPU ids)"
	[[ -n ${XDS_BW_TARGET_FILE:-} ]] || die "set XDS_BW_TARGET_FILE"
	[[ -n ${XDS_DEV_2:-} ]] || export XDS_DEV_2=${XDS_DEV_2_FALLBACK:-$XDS_DEV_1}

	init_work_dir xds-npu-bw
	trap cleanup EXIT
	RESULTS_TSV=$WORK_DIR/results.tsv
	SUMMARY=$WORK_DIR/summary.txt
	printf 'npu\tpattern\tbs\tqd\tmem\tmib_s\tiops\tnote\n' >"$RESULTS_TSV"

	preflight
	build_benches
	save_kernel_identity
	ensure_modules

	if [[ ! -f $TARGET_FILE ]]; then
		log "Creating $TARGET_FILE ($TOTAL_SIZE)"
		local bytes
		bytes=$(parse_size_bytes "$TOTAL_SIZE")
		fallocate -l "$bytes" "$TARGET_FILE" || \
			dd if=/dev/zero of="$TARGET_FILE" bs=1M count=$((bytes >> 20)) status=none
	fi

	if [[ $CONCURRENT == 1 ]]; then
		log "Running NPU devices concurrently: $NPU_DEVICES"
		for npu in $(split_csv "$NPU_DEVICES"); do
			(
				# Each device uses a separate work subdir to avoid colliding temp files.
				local dev_work=$WORK_DIR/npu-$npu
				mkdir -p "$dev_work"
				# run_device_matrix still writes to $WORK_DIR; serialize records via flock.
				flock "$WORK_DIR" -c "run_device_matrix $npu"
			) &
		done
		wait
	else
		for npu in $(split_csv "$NPU_DEVICES"); do
			run_device_matrix "$npu"
		done
	fi

	write_summary
	log "Results: $RESULTS_TSV"
	log "Summary: $SUMMARY"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
