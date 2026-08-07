#!/usr/bin/env bash

# Physical Ascend: NDS bandwidth matrix via nds_bw_bench (same path as the C bench).
#
# No VM helpers / common.sh / stub CMB. Requires:
#   - p2p_dev already loaded on the machine
#   - Ascend ACL env sourced (set_env.sh)
#   - topology block device + writable data directory on the target FS
#
# Sweeps BS × QD × register_mem, optionally across multiple NPU cards.
# Each NPU reads its own file under --data-dir (npu${id}.bin) to avoid sharing
# LBAs. After every (bs,qd,mem,pattern) cell, those files are fully rewritten
# with O_DIRECT urandom to displace NVMe controller read cache.
#
# With --concurrent, for each cell all NPUs run nds_bw_bench at once.
# Note: AGGREGATE is still a sum of per-NPU rates (not wall-clock aggregate).
#
# Usage:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E ./test/npu_bw_matrix.sh \
#     --topology /dev/nvme0n1 --data-dir /mnt/data/xds-bw \
#     --npu-devices 0,1,2,3 \
#     --bs-list 32K,128K,1M --qd-list 16,32,64 \
#     --mem-mode register,noregister \
#     --total-size 1G --concurrent
#
# Outputs:
#   <outdir>/results.tsv   raw rows
#   <outdir>/summary.txt   column-aligned table (+ aggregate when concurrent)

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH=${NPU_BW_BENCH:-$SCRIPT_DIR/nds_bw_bench}

TOPOLOGY=
DATA_DIR=
NPU_DEVICES=
BS_LIST=32K,128K,1M
QD_LIST=16,32,64,128
MEM_MODE=register
PATTERN=sequential
TOTAL_SIZE=1G
PASSES=1
ACL_POLICY=0
CONCURRENT=0
OUTDIR=
SKIP_BUILD=0

die() { printf 'npu_bw_matrix: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

usage()
{
	cat <<'EOF'
Usage: npu_bw_matrix.sh --topology DEV --data-dir DIR --npu-devices IDS [options]

Required:
  --topology DEV          NDS topology block device (passed to nds_bw_bench)
  --data-dir DIR          directory for per-NPU files (npu${id}.bin)
  --npu-devices LIST      comma list of NPU ids (e.g. 0,1,2,3)

Sweep options:
  --bs-list LIST          block sizes (default: 32K,128K,1M)
  --qd-list LIST          queue depths (default: 16,32,64,128)
  --mem-mode LIST         register,noregister (default: register)
  --pattern LIST          sequential,random (default: sequential)

Bench options (forwarded to nds_bw_bench):
  --total-size SIZE       bytes per run / per-NPU file size (default: 1G)
  --passes N              (default: 1)
  --acl-policy N          aclrt malloc policy (default: 0)

Run mode:
  --concurrent            for each (bs,qd,mem), start one bench per NPU in parallel
  --outdir DIR            results directory (default: ./npu-bw-results-<timestamp>)
  --skip-build            do not rebuild nds_bw_bench
  -h, --help              show this help

Env overrides: ASCEND_ACL_LIB, NPU_BW_BENCH (path to nds_bw_bench binary).
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

npu_file()
{
	printf '%s/npu%s.bin' "$DATA_DIR" "$1"
}

build_bench()
{
	[[ $SKIP_BUILD == 1 ]] && return 0
	[[ -x $BENCH ]] && [[ $BENCH -nt $SCRIPT_DIR/nds_bw_bench.c ]] && \
		[[ $BENCH -nt $SCRIPT_DIR/hbm_acl.c ]] && return 0
	log "Building nds_bw_bench"
	make -C "$SCRIPT_DIR" nds_bw_bench
	BENCH=$SCRIPT_DIR/nds_bw_bench
}

# Full-file O_DIRECT urandom rewrite to displace NVMe read cache.
poison_one_file()
{
	local path=$1 bytes=$2
	local bs=$((1 << 20))
	local count=$((bytes / bs))
	local rem=$((bytes % bs))

	if (( count > 0 )); then
		dd if=/dev/urandom of="$path" bs="$bs" count="$count" \
			oflag=direct conv=notrunc status=none 2>/dev/null || \
		dd if=/dev/urandom of="$path" bs="$bs" count="$count" \
			conv=notrunc,fsync status=none
	fi
	if (( rem > 0 )); then
		# rem must be sector-aligned for oflag=direct on many FS.
		dd if=/dev/urandom of="$path" bs="$rem" count=1 \
			seek="$count" oflag=direct conv=notrunc status=none 2>/dev/null || \
		dd if=/dev/urandom of="$path" bs="$rem" count=1 \
			seek="$count" conv=notrunc,fsync status=none
	fi
}

ensure_data_files()
{
	local bytes npu path actual

	bytes=$(parse_size_bytes "$TOTAL_SIZE")
	(( bytes > 0 && (bytes % 512) == 0 )) || \
		die "--total-size must be positive and 512B-aligned (got $TOTAL_SIZE → $bytes)"

	mkdir -p "$DATA_DIR"
	for npu in $(split_csv "$NPU_DEVICES"); do
		path=$(npu_file "$npu")
		if [[ -f $path ]]; then
			actual=$(stat -c %s -- "$path")
			if (( actual != bytes )); then
				log "Resizing $path to $TOTAL_SIZE ($bytes bytes)"
				rm -f -- "$path"
			fi
		fi
		if [[ ! -f $path ]]; then
			log "Creating $path ($TOTAL_SIZE)"
			if ! fallocate -l "$bytes" "$path" 2>/dev/null; then
				dd if=/dev/zero of="$path" bs=1M \
					count=$((bytes >> 20)) status=none
				local rem=$((bytes & ((1 << 20) - 1)))
				if (( rem )); then
					dd if=/dev/zero of="$path" bs="$rem" count=1 \
						seek=$((bytes >> 20)) conv=notrunc status=none
				fi
			fi
		fi
		log "Initial fill $path (O_DIRECT urandom)"
		poison_one_file "$path" "$bytes"
	done
	sync
}

poison_data_files()
{
	local bytes npu path

	bytes=$(parse_size_bytes "$TOTAL_SIZE")
	log "Poisoning data files under $DATA_DIR (displace NVMe read cache)"
	for npu in $(split_csv "$NPU_DEVICES"); do
		path=$(npu_file "$npu")
		[[ -f $path ]] || die "missing data file $path"
		poison_one_file "$path" "$bytes"
	done
	sync
}

parse_result_line()
{
	# stdin: full bench stdout/stderr → print "mib_s<TAB>iops" from last RESULT line
	awk '
		/^RESULT / {
			mib=""; iops="";
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^mib_s=/) { mib=$i; sub(/^mib_s=/,"",mib) }
				if ($i ~ /^iops=/) { iops=$i; sub(/^iops=/,"",iops) }
			}
		}
		END {
			if (mib == "" || iops == "") exit 1
			print mib "\t" iops
		}'
}

run_one_bench()
{
	local npu=$1 bs=$2 qd=$3 mem=$4 pattern=$5 logfile=$6
	local target args=()

	target=$(npu_file "$npu")
	args=(
		--topology "$TOPOLOGY"
		--target "$target"
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
		register) ;;
		noregister) args+=(--no-register-mem) ;;
		*) die "bad mem $mem" ;;
	esac

	# Keep full log for debugging; RESULT is parsed from it.
	"$BENCH" "${args[@]}" >"$logfile" 2>&1
}

append_row()
{
	# npu pattern bs qd mem mib_s iops note
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$OUTDIR/results.tsv"
}

run_cell_sequential()
{
	local bs=$1 qd=$2 mem=$3 pattern=$4
	local npu logf parsed mib iops

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		log "npu=$npu bs=$bs qd=$qd mem=$mem pattern=$pattern target=$(npu_file "$npu")"
		run_one_bench "$npu" "$bs" "$qd" "$mem" "$pattern" "$logf" || \
			die "bench failed; see $logf"
		parsed=$(parse_result_line <"$logf") || die "no RESULT in $logf"
		mib=${parsed%%$'\t'*}
		iops=${parsed#*$'\t'}
		append_row "$npu" "$pattern" "$bs" "$qd" "$mem" "$mib" "$iops" "solo"
		printf '  npu%s: %s MiB/s  %s IOPS\n' "$npu" "$mib" "$iops"
	done
	poison_data_files
}

run_cell_concurrent()
{
	local bs=$1 qd=$2 mem=$3 pattern=$4
	local npu logf parsed mib iops pids=() fails=0
	local sum_mib=0 sum_iops=0 n_ok=0

	log "CONCURRENT bs=$bs qd=$qd mem=$mem pattern=$pattern npus=$NPU_DEVICES"

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		(
			run_one_bench "$npu" "$bs" "$qd" "$mem" "$pattern" "$logf"
		) &
		pids+=($!)
	done

	local pid
	for pid in "${pids[@]}"; do
		wait "$pid" || fails=$((fails + 1))
	done
	(( fails == 0 )) || die "concurrent cell had $fails failed bench(es); see $OUTDIR/logs"

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/npu${npu}.${pattern}.${bs}.qd${qd}.${mem}.log
		parsed=$(parse_result_line <"$logf") || die "no RESULT in $logf"
		mib=${parsed%%$'\t'*}
		iops=${parsed#*$'\t'}
		append_row "$npu" "$pattern" "$bs" "$qd" "$mem" "$mib" "$iops" "concurrent"
		printf '  npu%s: %s MiB/s  %s IOPS\n' "$npu" "$mib" "$iops"
		# bash float via awk for aggregate
		sum_mib=$(awk -v a="$sum_mib" -v b="$mib" 'BEGIN{printf "%.2f", a+b}')
		sum_iops=$(awk -v a="$sum_iops" -v b="$iops" 'BEGIN{printf "%.0f", a+b}')
		n_ok=$((n_ok + 1))
	done
	append_row "ALL" "$pattern" "$bs" "$qd" "$mem" "$sum_mib" "$sum_iops" "sum_of_${n_ok}_npus"
	printf '  AGGREGATE: %s MiB/s  %s IOPS (%s NPUs)\n' "$sum_mib" "$sum_iops" "$n_ok"
	poison_data_files
}

write_summary()
{
	{
		printf 'Physical NPU NDS bandwidth matrix (nds_bw_bench)\n'
		printf 'topology=%s data_dir=%s total=%s passes=%s\n' \
			"$TOPOLOGY" "$DATA_DIR" "$TOTAL_SIZE" "$PASSES"
		printf 'npus=%s bs=%s qd=%s mem=%s pattern=%s concurrent=%s\n\n' \
			"$NPU_DEVICES" "$BS_LIST" "$QD_LIST" "$MEM_MODE" "$PATTERN" "$CONCURRENT"
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
			--data-dir) DATA_DIR=$2; shift 2 ;;
			--target) die "--target removed; use --data-dir DIR (per-NPU npu\${id}.bin)" ;;
			--npu-devices) NPU_DEVICES=$2; shift 2 ;;
			--bs-list) BS_LIST=$2; shift 2 ;;
			--qd-list) QD_LIST=$2; shift 2 ;;
			--mem-mode) MEM_MODE=$2; shift 2 ;;
			--pattern) PATTERN=$2; shift 2 ;;
			--total-size) TOTAL_SIZE=$2; shift 2 ;;
			--passes) PASSES=$2; shift 2 ;;
			--acl-policy) ACL_POLICY=$2; shift 2 ;;
			--outdir) OUTDIR=$2; shift 2 ;;
			--concurrent) CONCURRENT=1; shift ;;
			--skip-build) SKIP_BUILD=1; shift ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown arg: $1 (try --help)" ;;
		esac
	done

	[[ -n $TOPOLOGY ]] || die "missing --topology"
	[[ -n $DATA_DIR ]] || die "missing --data-dir"
	[[ -n $NPU_DEVICES ]] || die "missing --npu-devices"
	[[ -b $TOPOLOGY || -c $TOPOLOGY ]] || \
		die "topology $TOPOLOGY is not a block/char device"
	[[ -c /dev/p2p_device ]] || die "/dev/p2p_device missing; load p2p_dev first"
	[[ -n ${ASCEND_ACL_LIB:-} || -n ${ASCEND_HOME:-} || -n ${ASCEND_AICPU_PATH:-} || -n ${LD_LIBRARY_PATH:-} ]] || \
		log "warning: Ascend env vars not obvious; source set_env.sh and use sudo -E"

	if [[ -z $OUTDIR ]]; then
		OUTDIR=$(pwd)/npu-bw-results-$(date +%Y%m%d-%H%M%S)
	fi
	mkdir -p "$OUTDIR/logs"
	printf 'npu\tpattern\tbs\tqd\tmem\tmib_s\tiops\tnote\n' >"$OUTDIR/results.tsv"

	build_bench
	[[ -x $BENCH ]] || die "nds_bw_bench not found at $BENCH (make -C test nds_bw_bench)"

	ensure_data_files

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
