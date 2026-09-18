#!/usr/bin/env bash

# Physical Ascend: inter-NPU HBM D2D memcpy bandwidth via npu_d2d_bw.
# For each src→dst pair (different cards), enables peer access and times
# aclrtMemcpy(DEVICE_TO_DEVICE). No NVMe / p2p_dev / data-dir.
#
# Usage:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E ./test/npu_d2d_bw.sh \
#     --npu-devices 0,1,2,3 \
#     --bs-list 1M,8M --qd-list 1,4 \
#     --total-size 8G --verify
#
#   # explicit pairs only:
#   sudo -E ./test/npu_d2d_bw.sh --pairs 0:1,1:0 --bs-list 1M
#
# Outputs:
#   <outdir>/results.tsv
#   <outdir>/summary.txt

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH=${NPU_D2D_BENCH:-$SCRIPT_DIR/npu_d2d_bw}

NPU_DEVICES=
PAIRS=
BS_LIST=1M,8M,64M
QD_LIST=1,4,8
TOTAL_SIZE=8G
RUNTIME=
ACL_POLICY=3
VERIFY=0
OUTDIR=
SKIP_BUILD=0

die() { printf 'npu_d2d_bw: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

usage()
{
	cat <<'EOF'
Usage: npu_d2d_bw.sh --npu-devices IDS | --pairs SRC:DST,... [options]

Required (one of):
  --npu-devices LIST      all ordered pairs src→dst among these cards
  --pairs LIST            explicit pairs, e.g. 0:1,1:0,0:2

Sweep options:
  --bs-list LIST          copy sizes (default: 1M,8M,64M)
  --qd-list LIST          queue depths / streams on dst (default: 1,4,8)

Bench options (forwarded to npu_d2d_bw):
  --total-size SIZE       aggregate bytes per run (default: 8G)
  --runtime SEC           timed mode (overrides total-size)
  --acl-policy N          aclrt malloc policy (default: 3 = HUGE_FIRST_P2P)
  --verify                D2H+memcmp first dst slot after warmup

Run mode:
  --outdir DIR            results directory (default: ./npu-d2d-results-<timestamp>)
  --skip-build            do not rebuild npu_d2d_bw
  -h, --help              show this help

Env overrides: ASCEND_ACL_LIB, NPU_D2D_BENCH.
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

list_pairs()
{
	local src dst pair

	if [[ -n $PAIRS ]]; then
		for pair in $(split_csv "$PAIRS"); do
			[[ $pair == *:* ]] || die "bad pair $pair (want SRC:DST)"
			src=${pair%%:*}
			dst=${pair##*:}
			[[ $src != "$dst" ]] || die "pair $pair has src==dst"
			printf '%s %s\n' "$src" "$dst"
		done
		return
	fi
	for src in $(split_csv "$NPU_DEVICES"); do
		for dst in $(split_csv "$NPU_DEVICES"); do
			[[ $src != "$dst" ]] || continue
			printf '%s %s\n' "$src" "$dst"
		done
	done
}

build_bench()
{
	[[ $SKIP_BUILD == 1 ]] && return 0
	[[ -x $BENCH ]] && [[ $BENCH -nt $SCRIPT_DIR/npu_d2d_bw.c ]] && \
		[[ $BENCH -nt $SCRIPT_DIR/hbm_acl.c ]] && return 0
	log "Building npu_d2d_bw"
	make -C "$SCRIPT_DIR" npu_d2d_bw
	BENCH=$SCRIPT_DIR/npu_d2d_bw
}

parse_result_line()
{
	awk '
		/^RESULT / {
			mib=""; iops=""; bytes=""; sec="";
			proc="0"; sys="0";
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^mib_s=/) { mib=$i; sub(/^mib_s=/,"",mib) }
				if ($i ~ /^iops=/) { iops=$i; sub(/^iops=/,"",iops) }
				if ($i ~ /^bytes=/) { bytes=$i; sub(/^bytes=/,"",bytes) }
				if ($i ~ /^sec=/) { sec=$i; sub(/^sec=/,"",sec) }
				if ($i ~ /^proc_cpu_pct=/) { proc=$i; sub(/^proc_cpu_pct=/,"",proc) }
				if ($i ~ /^sys_cpu_pct=/) { sys=$i; sub(/^sys_cpu_pct=/,"",sys) }
			}
		}
		END {
			if (mib == "" || iops == "" || bytes == "" || sec == "") exit 1
			print mib "\t" iops "\t" bytes "\t" sec "\t" proc "\t" sys
		}'
}

run_one_bench()
{
	local src=$1 dst=$2 bs=$3 qd=$4 logfile=$5
	local args=()

	args=(
		--src-device "$src"
		--dst-device "$dst"
		--copy-size "$bs"
		--queue-depth "$qd"
		--acl-policy "$ACL_POLICY"
	)
	if [[ -n $RUNTIME ]]; then
		args+=(--runtime "$RUNTIME")
	else
		args+=(--total-size "$TOTAL_SIZE")
	fi
	[[ $VERIFY == 1 ]] && args+=(--verify)

	"$BENCH" "${args[@]}" >"$logfile" 2>&1
}

append_row()
{
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$OUTDIR/results.tsv"
}

run_cell()
{
	local bs=$1 qd=$2
	local src dst logf parsed mib iops bytes sec proc_cpu sys_cpu

	while read -r src dst; do
		[[ -n $src && -n $dst ]] || continue
		logf=$OUTDIR/logs/src${src}-dst${dst}.${bs}.qd${qd}.log
		log "src=$src dst=$dst bs=$bs qd=$qd"
		run_one_bench "$src" "$dst" "$bs" "$qd" "$logf" || \
			die "bench failed src=$src dst=$dst; see $logf"
		parsed=$(parse_result_line <"$logf") || die "no RESULT in $logf"
		mib=$(printf '%s\n' "$parsed" | cut -f1)
		iops=$(printf '%s\n' "$parsed" | cut -f2)
		bytes=$(printf '%s\n' "$parsed" | cut -f3)
		sec=$(printf '%s\n' "$parsed" | cut -f4)
		proc_cpu=$(printf '%s\n' "$parsed" | cut -f5)
		sys_cpu=$(printf '%s\n' "$parsed" | cut -f6)
		append_row "$src" "$dst" "$bs" "$qd" "$mib" "$iops" "$sec" \
			"$bytes" "$proc_cpu" "$sys_cpu"
		printf '  %s→%s: %s MiB/s  %s copies/s  sec=%s  proc_cpu=%s%% sys_cpu=%s%%\n' \
			"$src" "$dst" "$mib" "$iops" "$sec" "$proc_cpu" "$sys_cpu"
	done < <(list_pairs)
}

write_summary()
{
	{
		printf 'Physical NPU inter-card HBM D2D memcpy bandwidth\n'
		printf 'devices=%s pairs=%s bs=%s qd=%s total=%s runtime=%s acl_policy=%s verify=%s\n\n' \
			"${NPU_DEVICES:-n/a}" "${PAIRS:-all-ordered}" "$BS_LIST" "$QD_LIST" \
			"$TOTAL_SIZE" "${RUNTIME:-none}" "$ACL_POLICY" "$VERIFY"
		if command -v column >/dev/null 2>&1; then
			column -t -s $'\t' "$OUTDIR/results.tsv"
		else
			cat "$OUTDIR/results.tsv"
		fi
	} | tee "$OUTDIR/summary.txt"
}

main()
{
	local bs qd n_pairs

	while [[ $# -gt 0 ]]; do
		case $1 in
			--npu-devices) NPU_DEVICES=$2; shift 2 ;;
			--pairs) PAIRS=$2; shift 2 ;;
			--bs-list) BS_LIST=$2; shift 2 ;;
			--qd-list) QD_LIST=$2; shift 2 ;;
			--total-size) TOTAL_SIZE=$2; shift 2 ;;
			--runtime) RUNTIME=$2; shift 2 ;;
			--acl-policy) ACL_POLICY=$2; shift 2 ;;
			--outdir) OUTDIR=$2; shift 2 ;;
			--verify) VERIFY=1; shift ;;
			--skip-build) SKIP_BUILD=1; shift ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown arg: $1 (try --help)" ;;
		esac
	done

	[[ -n $NPU_DEVICES || -n $PAIRS ]] || die "missing --npu-devices or --pairs"
	n_pairs=$(list_pairs | wc -l)
	(( n_pairs > 0 )) || die "no src!=dst pairs to run"
	[[ -n ${ASCEND_ACL_LIB:-} || -n ${ASCEND_HOME:-} || -n ${ASCEND_AICPU_PATH:-} || -n ${LD_LIBRARY_PATH:-} ]] || \
		log "warning: Ascend env vars not obvious; source set_env.sh and use sudo -E"

	if [[ -z $OUTDIR ]]; then
		OUTDIR=$(pwd)/npu-d2d-results-$(date +%Y%m%d-%H%M%S)
	fi
	mkdir -p "$OUTDIR/logs"
	printf 'src\tdst\tbs\tqd\tmib_s\tiops\tsec\tbytes\tproc_cpu_pct\tsys_cpu_pct\n' \
		>"$OUTDIR/results.tsv"

	build_bench
	[[ -x $BENCH ]] || die "npu_d2d_bw not found at $BENCH (make -C test npu_d2d_bw)"

	log "running $n_pairs pair(s)"
	for bs in $(split_csv "$BS_LIST"); do
		for qd in $(split_csv "$QD_LIST"); do
			run_cell "$bs" "$qd"
		done
	done

	write_summary
	log "Results: $OUTDIR/results.tsv"
	log "Summary: $OUTDIR/summary.txt"
}

main "$@"
