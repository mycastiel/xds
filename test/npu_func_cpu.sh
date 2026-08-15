#!/usr/bin/env bash

# Physical Ascend: run nds_bw_bench while collecting:
#   - /sys/kernel/debug/p2p_device/summary  (debugfs.c I/O counters)
#   - proc_cpu_pct / sys_cpu_pct from nds_bw_bench RESULT (NDS userspace path)
#
# Same idea as test/common.sh: mount debugfs if needed, reset summary, run I/O,
# snapshot summary, umount on exit if we mounted it. No ftrace / set_ftrace_filter.
#
# Requires:
#   - p2p_dev already loaded
#   - Ascend ACL env sourced (set_env.sh)
#   - topology block device + writable --data-dir
#
# Usage:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E ./test/npu_func_cpu.sh \
#     --topology /dev/nvme0n1 --data-dir /mnt/data/xds-bw \
#     --npu-devices 0 --bs 128K --qd 64 --total-size 256M
#
# Outputs under <outdir>/:
#   p2p_summary_before.txt / p2p_summary_after.txt
#   logs/npu*.log          nds_bw_bench stdout (incl. RESULT cpu fields)
#   summary.txt            combined view

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH=${NPU_BW_BENCH:-$SCRIPT_DIR/nds_bw_bench}

TOPOLOGY=
DATA_DIR=
NPU_DEVICES=
BS=128K
QD=64
MEM_MODE=register
PATTERN=sequential
TOTAL_SIZE=256M
PASSES=1
ACL_POLICY=0
CONCURRENT=0
OUTDIR=
SKIP_BUILD=0

DEBUGFS_MOUNTED=0
P2P_SUMMARY=/sys/kernel/debug/p2p_device/summary

die() { printf 'npu_func_cpu: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

usage()
{
	cat <<'EOF'
Usage: npu_func_cpu.sh --topology DEV --data-dir DIR --npu-devices IDS [options]

Collect p2p_device/summary (debugfs) + nds_bw_bench CPU%% around one I/O cell.

Required:
  --topology DEV          NDS topology block device
  --data-dir DIR          directory for per-NPU files (npu${id}.bin)
  --npu-devices LIST      comma list of NPU ids (e.g. 0 or 0,1,2,3)

Bench options:
  --bs SIZE               I/O size (default: 128K)
  --qd N                  queue depth (default: 64)
  --mem-mode MODE         register or noregister (default: register)
  --pattern MODE          sequential or random (default: sequential)
  --total-size SIZE       bytes per run / per-NPU file size (default: 256M)
  --passes N              (default: 1)
  --acl-policy N          aclrt malloc policy (default: 0)
  --concurrent            run one bench per NPU in parallel

Run options:
  --outdir DIR            results directory (default: ./npu-func-cpu-<timestamp>)
  --skip-build            do not rebuild nds_bw_bench
  -h, --help              show this help

Env overrides: ASCEND_ACL_LIB, NPU_BW_BENCH.
EOF
}

cleanup()
{
	local ec=$?

	if (( DEBUGFS_MOUNTED )); then
		umount /sys/kernel/debug >/dev/null 2>&1 || true
		DEBUGFS_MOUNTED=0
	fi
	exit "$ec"
}

trap cleanup EXIT INT TERM

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
	local n=0 x
	for x in $(split_csv "$1"); do
		n=$((n + 1))
	done
	printf '%s\n' "$n"
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
		poison_one_file "$path" "$bytes"
	done
	sync
}

mount_debugfs()
{
	if ! mountpoint -q /sys/kernel/debug; then
		log "Mounting debugfs on /sys/kernel/debug"
		mount -t debugfs debugfs /sys/kernel/debug
		DEBUGFS_MOUNTED=1
	fi
	[[ -w $P2P_SUMMARY ]] || die "p2p debugfs summary unavailable at $P2P_SUMMARY"
}

reset_p2p_summary()
{
	printf '0\n' >"$P2P_SUMMARY"
}

save_p2p_summary()
{
	local dest=$1
	cp -a -- "$P2P_SUMMARY" "$dest"
}

# Extract RESULT cpu / bw fields from a bench log.
parse_result_line()
{
	awk '
		/^RESULT / {
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^mib_s=/) { mib=$i; sub(/^mib_s=/,"",mib) }
				if ($i ~ /^sec=/) { sec=$i; sub(/^sec=/,"",sec) }
				if ($i ~ /^bytes=/) { bytes=$i; sub(/^bytes=/,"",bytes) }
				if ($i ~ /^proc_cpu_pct=/) { proc=$i; sub(/^proc_cpu_pct=/,"",proc) }
				if ($i ~ /^sys_cpu_pct=/) { sys=$i; sub(/^sys_cpu_pct=/,"",sys) }
				if ($i ~ /^ios=/) { ios=$i; sub(/^ios=/,"",ios) }
			}
		}
		END {
			if (mib == "" || sec == "") exit 1
			print mib "\t" sec "\t" bytes "\t" ios "\t" proc "\t" sys
		}'
}

run_one_bench()
{
	local npu=$1 logfile=$2
	local barrier_dir=${3:-}
	local barrier_world=${4:-1}
	local target args=()

	target=$(npu_file "$npu")
	args=(
		--topology "$TOPOLOGY"
		--target "$target"
		--npu-device "$npu"
		--acl-policy "$ACL_POLICY"
		--io-min "$BS"
		--io-max "$BS"
		--queue-depth "$QD"
		--passes "$PASSES"
		--total-size "$TOTAL_SIZE"
	)
	case $PATTERN in
		sequential) args+=(--sequential) ;;
		random) args+=(--random) ;;
		*) die "bad pattern $PATTERN" ;;
	esac
	case $MEM_MODE in
		register) ;;
		noregister) args+=(--no-register-mem) ;;
		*) die "bad mem $MEM_MODE" ;;
	esac
	if [[ -n $barrier_dir ]]; then
		args+=(--barrier-dir "$barrier_dir" --barrier-world "$barrier_world")
	fi

	"$BENCH" "${args[@]}" >"$logfile" 2>&1
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

run_bench_cell()
{
	local npu logf pids=() fails=0
	local n_npus barrier_dir

	n_npus=$(count_csv "$NPU_DEVICES")
	mkdir -p "$OUTDIR/logs"

	if [[ $CONCURRENT == 1 && $n_npus -gt 1 ]]; then
		barrier_dir=$OUTDIR/barrier.$$
		rm -rf -- "$barrier_dir"
		mkdir -p "$barrier_dir"
		log "CONCURRENT npus=$NPU_DEVICES bs=$BS qd=$QD"
		for npu in $(split_csv "$NPU_DEVICES"); do
			logf=$OUTDIR/logs/npu${npu}.log
			(
				run_one_bench "$npu" "$logf" "$barrier_dir" "$n_npus"
			) &
			pids+=($!)
		done
		wait_barrier_ready "$barrier_dir" "$n_npus"
		: >"$barrier_dir/go"
		local pid
		for pid in "${pids[@]}"; do
			wait "$pid" || fails=$((fails + 1))
		done
		rm -rf -- "$barrier_dir"
		(( fails == 0 )) || die "$fails bench(es) failed; see $OUTDIR/logs"
	else
		for npu in $(split_csv "$NPU_DEVICES"); do
			logf=$OUTDIR/logs/npu${npu}.log
			log "npu=$npu bs=$BS qd=$QD target=$(npu_file "$npu")"
			run_one_bench "$npu" "$logf" || die "bench failed; see $logf"
		done
	fi
}

write_summary()
{
	local npu logf parsed mib sec bytes ios proc_cpu sys_cpu

	{
		printf 'npu_func_cpu (debugfs summary + nds_bw_bench CPU)\n'
		printf 'topology=%s data_dir=%s npus=%s bs=%s qd=%s total=%s concurrent=%s\n\n' \
			"$TOPOLOGY" "$DATA_DIR" "$NPU_DEVICES" "$BS" "$QD" "$TOTAL_SIZE" "$CONCURRENT"

		printf '--- p2p_device/summary after (debugfs.c) ---\n'
		cat "$OUTDIR/p2p_summary_after.txt"
		printf '\n--- per-NPU nds_bw_bench RESULT ---\n'
		for npu in $(split_csv "$NPU_DEVICES"); do
			logf=$OUTDIR/logs/npu${npu}.log
			parsed=$(parse_result_line <"$logf") || {
				printf 'npu%s: (no RESULT in %s)\n' "$npu" "$logf"
				continue
			}
			mib=$(printf '%s\n' "$parsed" | cut -f1)
			sec=$(printf '%s\n' "$parsed" | cut -f2)
			bytes=$(printf '%s\n' "$parsed" | cut -f3)
			ios=$(printf '%s\n' "$parsed" | cut -f4)
			proc_cpu=$(printf '%s\n' "$parsed" | cut -f5)
			sys_cpu=$(printf '%s\n' "$parsed" | cut -f6)
			printf 'npu%s: mib_s=%s sec=%s bytes=%s ios=%s proc_cpu=%s%% sys_cpu=%s%%\n' \
				"$npu" "$mib" "$sec" "$bytes" "$ios" "$proc_cpu" "$sys_cpu"
		done
	} | tee "$OUTDIR/summary.txt"
}

main()
{
	while [[ $# -gt 0 ]]; do
		case $1 in
			--topology) TOPOLOGY=$2; shift 2 ;;
			--data-dir) DATA_DIR=$2; shift 2 ;;
			--npu-devices) NPU_DEVICES=$2; shift 2 ;;
			--bs) BS=$2; shift 2 ;;
			--qd) QD=$2; shift 2 ;;
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

	if [[ -z $OUTDIR ]]; then
		OUTDIR=$(pwd)/npu-func-cpu-$(date +%Y%m%d-%H%M%S)
	fi
	mkdir -p "$OUTDIR"

	build_bench
	[[ -x $BENCH ]] || die "nds_bw_bench not found at $BENCH"
	ensure_data_files

	mount_debugfs
	reset_p2p_summary
	save_p2p_summary "$OUTDIR/p2p_summary_before.txt"

	log "Running nds_bw_bench (NDS API path); collecting p2p_device/summary"
	run_bench_cell

	save_p2p_summary "$OUTDIR/p2p_summary_after.txt"
	write_summary

	log "Results: $OUTDIR"
	log "Summary: $OUTDIR/summary.txt"
}

main "$@"
