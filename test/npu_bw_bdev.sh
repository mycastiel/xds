#!/usr/bin/env bash

# Physical Ascend: NDS bandwidth matrix against a shared block device
# (e.g. /dev/md0), fio-style. Uses nds_bw_bench --target on the bdev.
#
# Unlike npu_bw_matrix.sh (per-NPU files under --data-dir + urandom poison),
# this script:
#   - reads a block/char device directly (--target /dev/md0)
#   - does NOT create or overwrite any files (read-only traffic)
#   - divides the device into one disjoint contiguous region per NPU
#   - each NPU reads sequentially and wraps only inside its own region
#
# nds_bw_bench already accepts block devices (BLKGETSIZE64 + O_DIRECT pread).
#
# Requires:
#   - p2p_dev already loaded
#   - Ascend ACL env sourced (set_env.sh)
#   - topology registered for the NDS path (defaults to --target if omitted)
#
# Usage:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E ./test/npu_bw_bdev.sh \
#     --target /dev/md0 --npu-devices 0,1,2,3 \
#     --bs-list 128K --qd-list 64,128 \
#     --total-size 1G --concurrent
#
#   # topology different from target (optional):
#   sudo -E ./test/npu_bw_bdev.sh \
#     --topology /dev/md0 --target /dev/md0 --npu-devices 0 ...
#
# Outputs:
#   <outdir>/results.tsv
#   <outdir>/summary.txt
#
# Keep npu_bw_matrix.sh for the per-file + poison workflow.

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH=${NPU_BW_BENCH:-$SCRIPT_DIR/nds_bw_bench}

TOPOLOGY=
TARGET=
NPU_DEVICES=
BS_LIST=32K,128K,1M
QD_LIST=16,32,64,128
MEM_MODE=register
PATTERN=sequential
TOTAL_SIZE=1G
PASSES=1
ACL_POLICY=0
CONCURRENT=0
VERIFY_EACH=0
RUNTIME=
OUTDIR=
SKIP_BUILD=0
TARGET_BYTES=0
REGION_BYTES=0

die() { printf 'npu_bw_bdev: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

usage()
{
	cat <<'EOF'
Usage: npu_bw_bdev.sh --target DEV --npu-devices IDS [options]

Read bandwidth matrix against a shared block device (fio-like). No file
create/poison. The device is split into equal per-NPU regions so sequential
readers do not all start at LBA 0. Old per-file workflow remains in
npu_bw_matrix.sh.

Required:
  --target DEV            block device to read (e.g. /dev/md0, /dev/nvme0n1)
  --npu-devices LIST      comma list of NPU ids (e.g. 0,1,2,3)

Optional:
  --topology DEV          NDS topology device (default: same as --target)

Sweep options:
  --bs-list LIST          block sizes (default: 32K,128K,1M)
  --qd-list LIST          queue depths (default: 16,32,64,128)
  --mem-mode LIST         register,noregister (default: register)
  --pattern LIST          sequential,random (default: sequential)

Bench options (forwarded to nds_bw_bench):
  --total-size SIZE       aggregate bytes per run (default: 1G; wraps in device)
  --runtime SEC           timed mode (overrides total-size budget for bench)
  --passes N              (default: 1)
  --acl-policy N          aclrt malloc policy (default: 0)
  --verify-each           after each getevents completion, D2H+CRC vs device

Run mode:
  --concurrent            for each (bs,qd,mem), start one bench per NPU in parallel
  --outdir DIR            results directory (default: ./npu-bw-bdev-<timestamp>)
  --skip-build            do not rebuild nds_bw_bench
  -h, --help              show this help

Env overrides: ASCEND_ACL_LIB, NPU_BW_BENCH.
EOF
}

split_csv()
{
	local raw=${1// /} part
	IFS=',' read -ra parts <<<"$raw"
	for part in "${parts[@]}"; do
		[[ -n $part ]] || continue
		printf '%s\n' "$part"
	done
}

count_csv()
{
	local n=0
	local x
	for x in $(split_csv "$1"); do
		n=$((n + 1))
	done
	printf '%s\n' "$n"
}

npu_rank()
{
	local wanted=$1
	local n=0 x

	for x in $(split_csv "$NPU_DEVICES"); do
		if [[ $x == "$wanted" ]]; then
			printf '%s\n' "$n"
			return 0
		fi
		n=$((n + 1))
	done
	die "NPU $wanted is not in --npu-devices"
}

setup_regions()
{
	local n_npus
	local align=$((1 << 20))

	n_npus=$(count_csv "$NPU_DEVICES")
	(( n_npus > 0 )) || die "empty --npu-devices"
	TARGET_BYTES=$(blockdev --getsize64 "$TARGET") ||
		die "cannot get target size via blockdev: $TARGET"
	# Align each equal region down to 1 MiB. Any small tail is unused.
	REGION_BYTES=$(( (TARGET_BYTES / n_npus / align) * align ))
	(( REGION_BYTES > 0 )) ||
		die "target too small for $n_npus per-NPU regions"
	log "target_bytes=$TARGET_BYTES npus=$n_npus region_bytes=$REGION_BYTES"
}

build_bench()
{
	[[ $SKIP_BUILD == 1 ]] && return 0
	[[ -x $BENCH ]] && [[ $BENCH -nt $SCRIPT_DIR/nds_bw_bench.c ]] && \
		[[ $BENCH -nt $SCRIPT_DIR/hbm_acl.c ]] && \
		[[ $BENCH -nt $SCRIPT_DIR/../file_p2p/nds_api.c ]] && return 0
	log "Building nds_bw_bench"
	make -C "$SCRIPT_DIR" nds_bw_bench
	BENCH=$SCRIPT_DIR/nds_bw_bench
}

# Print: mib_s TAB iops TAB bytes TAB sec TAB verify_each TAB crc_checked TAB
#         crc_ok TAB proc_cpu_pct TAB sys_cpu_pct
parse_result_line()
{
	awk '
		/^RESULT / {
			mib=""; iops=""; bytes=""; sec="";
			ve="0"; checked="0"; ok="1";
			proc="0"; sys="0";
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^mib_s=/) { mib=$i; sub(/^mib_s=/,"",mib) }
				if ($i ~ /^iops=/) { iops=$i; sub(/^iops=/,"",iops) }
				if ($i ~ /^bytes=/) { bytes=$i; sub(/^bytes=/,"",bytes) }
				if ($i ~ /^sec=/) { sec=$i; sub(/^sec=/,"",sec) }
				if ($i ~ /^verify_each=/) { ve=$i; sub(/^verify_each=/,"",ve) }
				if ($i ~ /^crc_checked=/) { checked=$i; sub(/^crc_checked=/,"",checked) }
				if ($i ~ /^crc_ok=/) { ok=$i; sub(/^crc_ok=/,"",ok) }
				if ($i ~ /^proc_cpu_pct=/) { proc=$i; sub(/^proc_cpu_pct=/,"",proc) }
				if ($i ~ /^sys_cpu_pct=/) { sys=$i; sub(/^sys_cpu_pct=/,"",sys) }
			}
		}
		END {
			if (mib == "" || iops == "" || bytes == "" || sec == "") exit 1
			print mib "\t" iops "\t" bytes "\t" sec "\t" ve "\t" checked "\t" ok \
				"\t" proc "\t" sys
		}'
}

run_one_bench()
{
	local npu=$1 bs=$2 qd=$3 mem=$4 pattern=$5 logfile=$6
	local barrier_dir=${7:-}
	local barrier_world=${8:-1}
	local rank start
	local args=()

	rank=$(npu_rank "$npu")
	start=$((rank * REGION_BYTES))
	args=(
		--topology "$TOPOLOGY"
		--target "$TARGET"
		--npu-device "$npu"
		--acl-policy "$ACL_POLICY"
		--io-min "$bs"
		--io-max "$bs"
		--queue-depth "$qd"
		--passes "$PASSES"
		--start-offset "$start"
		--region-size "$REGION_BYTES"
	)
	if [[ -n $RUNTIME ]]; then
		args+=(--runtime "$RUNTIME")
	else
		args+=(--total-size "$TOTAL_SIZE")
	fi
	case $pattern in
		sequential) args+=(--sequential) ;;
		random) args+=(--random) ;;
		*) die "bad pattern $pattern" ;;
	esac
	case $mem in
		register) ;;
		noregister) args+=(--no-register-mem) ;;
		*) die "bad mem $mem" ;;
	esac
	[[ $VERIFY_EACH == 1 ]] && args+=(--verify-each)
	if [[ -n $barrier_dir ]]; then
		args+=(--barrier-dir "$barrier_dir" --barrier-world "$barrier_world")
	fi

	"$BENCH" "${args[@]}" >"$logfile" 2>&1
}

append_row()
{
	# npu pattern bs qd mem mib_s iops sec bytes proc_cpu sys_cpu note
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$OUTDIR/results.tsv"
}

run_cell_sequential()
{
	local bs=$1 qd=$2 mem=$3 pattern=$4
	local npu logf parsed mib iops bytes sec ve checked ok proc_cpu sys_cpu

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		log "npu=$npu region_rank=$(npu_rank "$npu") bs=$bs qd=$qd mem=$mem pattern=$pattern target=$TARGET verify_each=$VERIFY_EACH"
		run_one_bench "$npu" "$bs" "$qd" "$mem" "$pattern" "$logf" || \
			die "bench failed; see $logf"
		parsed=$(parse_result_line <"$logf") || die "no RESULT in $logf"
		mib=$(printf '%s\n' "$parsed" | cut -f1)
		iops=$(printf '%s\n' "$parsed" | cut -f2)
		bytes=$(printf '%s\n' "$parsed" | cut -f3)
		sec=$(printf '%s\n' "$parsed" | cut -f4)
		ve=$(printf '%s\n' "$parsed" | cut -f5)
		checked=$(printf '%s\n' "$parsed" | cut -f6)
		ok=$(printf '%s\n' "$parsed" | cut -f7)
		proc_cpu=$(printf '%s\n' "$parsed" | cut -f8)
		sys_cpu=$(printf '%s\n' "$parsed" | cut -f9)
		if [[ $VERIFY_EACH == 1 && $ve != 1 ]]; then
			die "expected verify_each=1 in $logf (old binary?); RESULT missing flag"
		fi
		if [[ $ve == 1 && $ok != 1 ]]; then
			die "CRC failed npu=$npu; see $logf"
		fi
		append_row "$npu" "$pattern" "$bs" "$qd" "$mem" "$mib" "$iops" "$sec" "$bytes" \
			"$proc_cpu" "$sys_cpu" "solo"
		if [[ $ve == 1 ]]; then
			printf '  npu%s: %s MiB/s  %s IOPS  sec=%s  crc_checked=%s crc_ok=%s  proc_cpu=%s%% sys_cpu=%s%%\n' \
				"$npu" "$mib" "$iops" "$sec" "$checked" "$ok" "$proc_cpu" "$sys_cpu"
		else
			printf '  npu%s: %s MiB/s  %s IOPS  sec=%s  proc_cpu=%s%% sys_cpu=%s%%\n' \
				"$npu" "$mib" "$iops" "$sec" "$proc_cpu" "$sys_cpu"
		fi
	done
}

wait_barrier_ready()
{
	local dir=$1
	local expect=$2
	local ready=0
	local deadline=$((SECONDS + 300))

	while (( SECONDS < deadline )); do
		ready=$(find "$dir" -maxdepth 1 -name 'ready.*' 2>/dev/null | wc -l)
		(( ready >= expect )) && return 0
		sleep 0.05
	done
	die "barrier timeout: only $ready/$expect ready under $dir"
}

run_cell_concurrent()
{
	local bs=$1 qd=$2 mem=$3 pattern=$4
	local npu logf parsed mib iops bytes sec pids=() fails=0
	local sum_mib=0 sum_iops=0 sum_bytes=0 max_sec=0 n_ok=0
	local sum_proc_cpu=0 max_sys_cpu=0
	local overlap_mib go_wall_mib ve checked ok proc_cpu sys_cpu
	local n_npus barrier_dir go_start go_end go_sec

	n_npus=$(count_csv "$NPU_DEVICES")
	barrier_dir=$OUTDIR/barrier.${pattern}.${bs}.qd${qd}.${mem}.$$
	rm -rf -- "$barrier_dir"
	mkdir -p "$barrier_dir"

	log "CONCURRENT bs=$bs qd=$qd mem=$mem pattern=$pattern npus=$NPU_DEVICES target=$TARGET region_bytes=$REGION_BYTES verify_each=$VERIFY_EACH"

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		(
			run_one_bench "$npu" "$bs" "$qd" "$mem" "$pattern" "$logf" \
				"$barrier_dir" "$n_npus"
		) &
		pids+=($!)
	done

	wait_barrier_ready "$barrier_dir" "$n_npus"
	go_start=$(date +%s.%N)
	: >"$barrier_dir/go"
	log "barrier released ($n_npus ranks) go_start=$go_start"

	local pid
	for pid in "${pids[@]}"; do
		wait "$pid" || fails=$((fails + 1))
	done
	go_end=$(date +%s.%N)
	(( fails == 0 )) || die "concurrent cell had $fails failed bench(es); see $OUTDIR/logs"

	go_sec=$(awk -v a="$go_start" -v b="$go_end" \
		'BEGIN { s=b-a; if (s<=0) s=1e-9; printf "%.6f", s }')

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		parsed=$(parse_result_line <"$logf") || die "no RESULT in $logf"
		mib=$(printf '%s\n' "$parsed" | cut -f1)
		iops=$(printf '%s\n' "$parsed" | cut -f2)
		bytes=$(printf '%s\n' "$parsed" | cut -f3)
		sec=$(printf '%s\n' "$parsed" | cut -f4)
		ve=$(printf '%s\n' "$parsed" | cut -f5)
		checked=$(printf '%s\n' "$parsed" | cut -f6)
		ok=$(printf '%s\n' "$parsed" | cut -f7)
		proc_cpu=$(printf '%s\n' "$parsed" | cut -f8)
		sys_cpu=$(printf '%s\n' "$parsed" | cut -f9)
		if [[ $VERIFY_EACH == 1 && $ve != 1 ]]; then
			die "expected verify_each=1 in $logf (old binary?); RESULT missing flag"
		fi
		if [[ $ve == 1 && $ok != 1 ]]; then
			die "CRC failed npu=$npu; see $logf"
		fi
		append_row "$npu" "$pattern" "$bs" "$qd" "$mem" "$mib" "$iops" "$sec" "$bytes" \
			"$proc_cpu" "$sys_cpu" "concurrent"
		if [[ $ve == 1 ]]; then
			printf '  npu%s: %s MiB/s  %s IOPS  bench_sec=%s bytes=%s  crc_checked=%s crc_ok=%s  proc_cpu=%s%% sys_cpu=%s%%\n' \
				"$npu" "$mib" "$iops" "$sec" "$bytes" "$checked" "$ok" "$proc_cpu" "$sys_cpu"
		else
			printf '  npu%s: %s MiB/s  %s IOPS  bench_sec=%s bytes=%s  proc_cpu=%s%% sys_cpu=%s%%\n' \
				"$npu" "$mib" "$iops" "$sec" "$bytes" "$proc_cpu" "$sys_cpu"
		fi
		sum_mib=$(awk -v a="$sum_mib" -v b="$mib" 'BEGIN{printf "%.2f", a+b}')
		sum_iops=$(awk -v a="$sum_iops" -v b="$iops" 'BEGIN{printf "%.0f", a+b}')
		sum_bytes=$(awk -v a="$sum_bytes" -v b="$bytes" 'BEGIN{printf "%.0f", a+b}')
		max_sec=$(awk -v a="$max_sec" -v b="$sec" 'BEGIN{printf "%.6f", (b>a)?b:a}')
		sum_proc_cpu=$(awk -v a="$sum_proc_cpu" -v b="$proc_cpu" 'BEGIN{printf "%.2f", a+b}')
		max_sys_cpu=$(awk -v a="$max_sys_cpu" -v b="$sys_cpu" 'BEGIN{printf "%.2f", (b>a)?b:a}')
		n_ok=$((n_ok + 1))
	done

	overlap_mib=$(awk -v b="$sum_bytes" -v s="$max_sec" \
		'BEGIN{ if (s<=0) s=1e-9; printf "%.2f", (b/1048576.0)/s }')
	go_wall_mib=$(awk -v b="$sum_bytes" -v s="$go_sec" \
		'BEGIN{ if (s<=0) s=1e-9; printf "%.2f", (b/1048576.0)/s }')

	append_row "ALL" "$pattern" "$bs" "$qd" "$mem" "$overlap_mib" "$sum_iops" \
		"$max_sec" "$sum_bytes" "$sum_proc_cpu" "$max_sys_cpu" "io_overlap_${n_ok}_npus"
	append_row "ALL" "$pattern" "$bs" "$qd" "$mem" "$go_wall_mib" "$sum_iops" \
		"$go_sec" "$sum_bytes" "$sum_proc_cpu" "$max_sys_cpu" "parent_go_wall_incl_teardown"
	append_row "ALL" "$pattern" "$bs" "$qd" "$mem" "$sum_mib" "$sum_iops" \
		"-" "$sum_bytes" "$sum_proc_cpu" "$max_sys_cpu" "sum_mib_unreliable"
	printf '  IO_OVERLAP: %s MiB/s  (sum_bytes=%s / max(bench_sec)=%s)  %s NPUs\n' \
		"$overlap_mib" "$sum_bytes" "$max_sec" "$n_ok"
	printf '  (contrast) parent_go_wall=%s MiB/s go_sec=%s  sum_mib_unreliable=%s\n' \
		"$go_wall_mib" "$go_sec" "$sum_mib"
	printf '  CPU: sum(proc_cpu)=%s%%  max(sys_cpu)=%s%%\n' \
		"$sum_proc_cpu" "$max_sys_cpu"
	rm -rf -- "$barrier_dir"
}

write_summary()
{
	{
		printf 'Physical NPU NDS bandwidth matrix (block device / nds_bw_bench)\n'
		printf 'topology=%s target=%s total=%s passes=%s runtime=%s verify_each=%s\n' \
			"$TOPOLOGY" "$TARGET" "$TOTAL_SIZE" "$PASSES" \
			"${RUNTIME:-none}" "$VERIFY_EACH"
		printf 'target_bytes=%s per_npu_region_bytes=%s (disjoint sequential regions)\n' \
			"$TARGET_BYTES" "$REGION_BYTES"
		printf 'npus=%s bs=%s qd=%s mem=%s pattern=%s concurrent=%s\n' \
			"$NPU_DEVICES" "$BS_LIST" "$QD_LIST" "$MEM_MODE" "$PATTERN" "$CONCURRENT"
		printf 'Note: shared --target (no poison); concurrent ALL io_overlap_* = sum(bytes)/max(bench_sec).\n\n'
		if command -v column >/dev/null 2>&1; then
			column -t -s $'\t' "$OUTDIR/results.tsv"
		else
			cat "$OUTDIR/results.tsv"
		fi
	} | tee "$OUTDIR/summary.txt"
}

main()
{
	local bs qd mem pattern

	while [[ $# -gt 0 ]]; do
		case $1 in
			--topology) TOPOLOGY=$2; shift 2 ;;
			--target) TARGET=$2; shift 2 ;;
			--data-dir) die "npu_bw_bdev uses --target DEV; for per-file poison see npu_bw_matrix.sh" ;;
			--npu-devices) NPU_DEVICES=$2; shift 2 ;;
			--bs-list) BS_LIST=$2; shift 2 ;;
			--qd-list) QD_LIST=$2; shift 2 ;;
			--mem-mode) MEM_MODE=$2; shift 2 ;;
			--pattern) PATTERN=$2; shift 2 ;;
			--total-size) TOTAL_SIZE=$2; shift 2 ;;
			--runtime) RUNTIME=$2; shift 2 ;;
			--passes) PASSES=$2; shift 2 ;;
			--acl-policy) ACL_POLICY=$2; shift 2 ;;
			--outdir) OUTDIR=$2; shift 2 ;;
			--concurrent) CONCURRENT=1; shift ;;
			--verify-each) VERIFY_EACH=1; shift ;;
			--skip-build) SKIP_BUILD=1; shift ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown arg: $1 (try --help)" ;;
		esac
	done

	[[ -n $TARGET ]] || die "missing --target (e.g. /dev/md0)"
	[[ -n $NPU_DEVICES ]] || die "missing --npu-devices"
	if [[ -z $TOPOLOGY ]]; then
		TOPOLOGY=$TARGET
		log "topology defaults to target=$TARGET"
	fi
	[[ -b $TOPOLOGY || -c $TOPOLOGY ]] || \
		die "topology $TOPOLOGY is not a block/char device"
	[[ -b $TARGET ]] || die "target $TARGET is not a block device"
	[[ -r $TARGET ]] || die "target $TARGET is not readable"
	[[ -c /dev/p2p_device ]] || die "/dev/p2p_device missing; load p2p_dev first"
	[[ -n ${ASCEND_ACL_LIB:-} || -n ${ASCEND_HOME:-} || -n ${ASCEND_AICPU_PATH:-} || -n ${LD_LIBRARY_PATH:-} ]] || \
		log "warning: Ascend env vars not obvious; source set_env.sh and use sudo -E"

	if [[ -z $OUTDIR ]]; then
		OUTDIR=$(pwd)/npu-bw-bdev-$(date +%Y%m%d-%H%M%S)
	fi
	mkdir -p "$OUTDIR/logs"
	printf 'npu\tpattern\tbs\tqd\tmem\tmib_s\tiops\tsec\tbytes\tproc_cpu_pct\tsys_cpu_pct\tnote\n' \
		>"$OUTDIR/results.tsv"

	build_bench
	[[ -x $BENCH ]] || die "nds_bw_bench not found at $BENCH (make -C test nds_bw_bench)"
	setup_regions

	for bs in $(split_csv "$BS_LIST"); do
		for qd in $(split_csv "$QD_LIST"); do
			for mem in $(split_csv "$MEM_MODE"); do
				case $mem in register|noregister) ;; *) die "bad --mem-mode entry: $mem" ;; esac
				for pattern in $(split_csv "$PATTERN"); do
					case $pattern in sequential|random) ;; *) die "bad --pattern entry: $pattern" ;; esac
					if [[ $CONCURRENT == 1 ]]; then
						run_cell_concurrent "$bs" "$qd" "$mem" "$pattern"
					else
						run_cell_sequential "$bs" "$qd" "$mem" "$pattern"
					fi
				done
			done
		done
	done

	write_summary
	log "Results: $OUTDIR/results.tsv"
	log "Summary: $OUTDIR/summary.txt"
}

main "$@"
