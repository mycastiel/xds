#!/usr/bin/env bash

# Bandwidth matrix: fio vs NDS (registered-mem and one-shot VA).
#
# Env knobs:
#   XDS_BW_TOTAL_SIZE   aggregate NDS transfer (default 1G)
#   XDS_BW_FIO_RUNTIME  fio measurement seconds (default 3)
#   XDS_BW_CMB_VA       CMB VA base (default 0)
#   XDS_BW_SKIP_BUILD   1 = skip module/bench rebuild
#   XDS_BW_SKIP_FIO     1 = skip fio baselines
#   XDS_BW_MEM_MODES    comma list: register,noregister (default both)
#   XDS_BW_CASES        override case list (newline or comma separated
#                       entries of: name|pattern|bs|qd
#                       bs is fixed size like 32K, or range 32K-128K)
#
# Writes:
#   $WORK_DIR/results.tsv   tabular results
#   $WORK_DIR/summary.txt   human summary

set -Eeuo pipefail

TEST_NAME=perf_bw_test
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

TOTAL_SIZE=${XDS_BW_TOTAL_SIZE:-1G}
FIO_RUNTIME=${XDS_BW_FIO_RUNTIME:-3}
CMB_VA=${XDS_BW_CMB_VA:-0}
SKIP_BUILD=${XDS_BW_SKIP_BUILD:-0}
SKIP_FIO=${XDS_BW_SKIP_FIO:-0}
MEM_MODES=${XDS_BW_MEM_MODES:-register,noregister}
RESULTS_TSV=
SUMMARY=

# Convert K/M/G size strings into bytes.
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

default_cases()
{
	cat <<'EOF'
seq-32k-qd128|sequential|32K|128
rand-32k-qd128|random|32K|128
seq-128k-qd128|sequential|128K|128
rand-128k-qd128|random|128K|128
seq-32k-128k-qd64|sequential|32K-128K|64
rand-32k-128k-qd64|random|32K-128K|64
seq-32k-qd64|sequential|32K|64
seq-128k-qd64|sequential|128K|64
EOF
}

list_cases()
{
	local raw=${XDS_BW_CASES:-}
	local line

	if [[ -z $raw ]]; then
		default_cases
		return
	fi
	raw=${raw//,/$'\n'}
	while IFS= read -r line; do
		[[ -n $line ]] || continue
		printf '%s\n' "$line"
	done <<<"$raw"
}

list_mem_modes()
{
	local raw=${MEM_MODES// /}
	local part

	IFS=',' read -ra parts <<<"$raw"
	for part in "${parts[@]}"; do
		case $part in
			register | registered | reg) printf 'register\n' ;;
			noregister | no-register | noreg | oneshot)
				printf 'noregister\n' ;;
			*) die "unknown mem mode '$part' (use register,noregister)" ;;
		esac
	done
}

parse_bs()
{
	# Sets globals IO_MIN_S IO_MAX_S from "32K" or "32K-128K".
	local bs=$1

	if [[ $bs == *-* ]]; then
		IO_MIN_S=${bs%%-*}
		IO_MAX_S=${bs#*-}
	else
		IO_MIN_S=$bs
		IO_MAX_S=$bs
	fi
}

record()
{
	# kind case pattern bs qd mem_mode mib_s iops note
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$RESULTS_TSV"
}

run_fio_case()
{
	local name=$1 pattern=$2 bs=$3 qd=$4
	local fio_rw fio_bs out bw iops

	[[ $SKIP_FIO == 1 ]] && return 0

	case $pattern in
		sequential) fio_rw=read ;;
		random) fio_rw=randread ;;
		*) die "bad pattern $pattern" ;;
	esac
	if [[ $bs == *-* ]]; then
		fio_bs="--bsrange=${bs}"
	else
		fio_bs="--bs=${bs}"
	fi

	log "fio $name"
	# Drop p2p modules so fio hits the raw NVMe path.
	rmmod p2p_dev >/dev/null 2>&1 || true
	rmmod stub >/dev/null 2>&1 || true

	out=$(fio --name="$name" --filename="$DEV1" --direct=1 \
		--ioengine=libaio --rw="$fio_rw" $fio_bs --iodepth="$qd" \
		--numjobs=1 --group_reporting --time_based=1 \
		--runtime="$FIO_RUNTIME" --ramp_time=1 \
		--minimal 2>/dev/null | tail -n1)
	# fio --minimal / terse v3: field7=read BW KiB/s, field8=read IOPS
	bw_kib=$(printf '%s\n' "$out" | cut -d ';' -f7)
	iops=$(printf '%s\n' "$out" | cut -d ';' -f8)
	[[ -n $bw_kib && -n $iops ]] || die "fio terse parse failed: [$out]"
	bw=$(awk -v k="$bw_kib" 'BEGIN { printf "%.2f", k / 1024.0 }')
	printf 'fio %s: %s MiB/s  %s IOPS\n' "$name" "$bw" "$iops"
	record fio "$name" "$pattern" "$bs" "$qd" - "$bw" "$iops" "runtime=${FIO_RUNTIME}s"
}

ensure_modules_loaded()
{
	if [[ ! -c /dev/p2p_device ]]; then
		load_modules
	elif ! lsmod | grep -q '^p2p_dev'; then
		load_modules
	fi
}

run_nds_case()
{
	local name=$1 pattern=$2 bs=$3 qd=$4 mem_mode=$5
	local args=() out mib iops reg_flag label

	parse_bs "$bs"
	local span=$((qd * $(parse_size_bytes "$IO_MAX_S")))
	(( span <= CMB_SIZE )) ||
		die "$name qd=$qd io_max=$IO_MAX_S needs ${span}B CMB (>${CMB_SIZE})"

	ensure_modules_loaded

	args=(--topology "$DEV1" --target "$DEV1" --cmb-va "$CMB_VA"
		--total-size "$TOTAL_SIZE" --io-min "$IO_MIN_S" --io-max "$IO_MAX_S"
		--queue-depth "$qd")
	case $pattern in
		sequential) args+=(--sequential) ;;
		random) args+=(--random) ;;
	esac
	case $mem_mode in
		register)
			reg_flag=1
			label="nds-reg $name"
			;;
		noregister)
			args+=(--no-register-mem)
			reg_flag=0
			label="nds-noreg $name"
			;;
		*) die "mem_mode $mem_mode" ;;
	esac

	log "$label"
	out=$("$SCRIPT_DIR/nds_bw_bench" "${args[@]}" | tee /dev/stderr)
	mib=$(printf '%s\n' "$out" | awk '/^RESULT / {
		for (i = 1; i <= NF; i++)
			if ($i ~ /^mib_s=/) {
				sub(/^mib_s=/, "", $i)
				print $i
			}
	}' | tail -n1)
	iops=$(printf '%s\n' "$out" | awk '/^RESULT / {
		for (i = 1; i <= NF; i++)
			if ($i ~ /^iops=/) {
				sub(/^iops=/, "", $i)
				print $i
			}
	}' | tail -n1)
	[[ -n $mib && -n $iops ]] || die "failed to parse RESULT from nds_bw_bench"
	record nds "$name" "$pattern" "$bs" "$qd" "$mem_mode" "$mib" "$iops" \
		"total=$TOTAL_SIZE register_mem=$reg_flag"
}

build_fast()
{
	log "Building p2p modules + nds_bw_bench (-j$BUILD_JOBS)"
	make -C "$REPO_ROOT" -j"$BUILD_JOBS" W=1
	make -C "$SCRIPT_DIR" -j"$BUILD_JOBS" nds_bw_bench
}

write_summary()
{
	local kernel
	kernel=$(uname -r)

	{
		printf 'kernel=%s\n' "$kernel"
		printf 'device=%s total_size=%s fio_runtime=%ss mem_modes=%s\n' \
			"$DEV1" "$TOTAL_SIZE" "$FIO_RUNTIME" "$MEM_MODES"
		printf '\n'
		printf '%-8s %-22s %-6s %-10s %10s %10s  %s\n' \
			KIND CASE QD BS MIB_S IOPS MEM
		printf '%s\n' "------------------------------------------------------------------------"
		awk -F'\t' -v OFS='\t' 'NR > 1 {
			printf "%-8s %-22s %-6s %-10s %10s %10s  %s\n",
				$1, $2, $5, $4, $7, $8, $6
		}' "$RESULTS_TSV"
	} | tee "$SUMMARY"
}

main()
{
	local case_line name pattern bs qd mode
	local -a modes=()

	init_work_dir xds-bw-matrix
	trap cleanup EXIT
	RESULTS_TSV=$WORK_DIR/results.tsv
	SUMMARY=$WORK_DIR/summary.txt
	printf 'kind\tcase\tpattern\tbs\tqd\tmem_mode\tmib_s\tiops\tnote\n' \
		>"$RESULTS_TSV"

	preflight
	if [[ $SKIP_BUILD != 1 ]]; then
		build_fast
	else
		[[ -x $SCRIPT_DIR/nds_bw_bench ]] ||
			die "nds_bw_bench missing; unset XDS_BW_SKIP_BUILD"
	fi
	save_kernel_identity

	mapfile -t modes < <(list_mem_modes)
	(( ${#modes[@]} > 0 )) || die "no mem modes selected"

	# Warm NDS path once so the first timed case is less cold.
	ensure_modules_loaded
	log "warmup NDS sequential 64M register"
	"$SCRIPT_DIR/nds_bw_bench" --topology "$DEV1" --target "$DEV1" \
		--cmb-va "$CMB_VA" --total-size 64M --io-min 32K --io-max 32K \
		--queue-depth 64 --sequential >/dev/null

	while IFS='|' read -r name pattern bs qd; do
		[[ -n $name ]] || continue
		run_fio_case "$name" "$pattern" "$bs" "$qd"
		for mode in "${modes[@]}"; do
			run_nds_case "$name" "$pattern" "$bs" "$qd" "$mode"
		done
	done < <(list_cases)

	log "Matrix summary"
	write_summary
	printf '\nresults: %s\nsummary: %s\n' "$RESULTS_TSV" "$SUMMARY"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
