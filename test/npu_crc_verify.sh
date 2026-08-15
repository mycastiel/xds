#!/usr/bin/env bash

# Physical Ascend: NDS PREAD CRC correctness via nds_crc_verify (HBM → host D2H).
#
# No VM helpers / common.sh / stub CMB. Requires:
#   - p2p_dev already loaded
#   - Ascend ACL env sourced (set_env.sh)
#   - topology block device + writable --data-dir on the target FS
#
# Each NPU gets its own patterned file (npu${id}.bin). Cases sweep length ×
# offset × mem-mode, solo then concurrent. After each case group the files are
# regenerated with a new seed to avoid NVMe read-cache reuse.
#
# Kernel P2P_MIN_PAGE_SIZE is 64KiB, but Ascend needs a ≥2MiB P2P-huge HBM
# window (acl policy HUGE_FIRST_P2P=3). register_mem can still test short I/Os
# (e.g. 4K) inside that window; noregister skips lengths < 2M.
#
# Usage:
#   source /usr/local/Ascend/ascend-toolkit/set_env.sh
#   sudo -E ./test/npu_crc_verify.sh \
#     --topology /dev/nvme0n1 --data-dir /mnt/data/xds-crc \
#     --npu-devices 0,1,2,3
#
# Outputs:
#   <outdir>/results.tsv
#   <outdir>/summary.txt

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VERIFY=${NPU_CRC_VERIFY:-$SCRIPT_DIR/nds_crc_verify}
PATTERN_GEN=$SCRIPT_DIR/generate_pattern.py

TOPOLOGY=
DATA_DIR=
NPU_DEVICES=
FILE_SIZE=64M
LENGTH_LIST=64K,128K,1M,2M
MEM_MODE=register,noregister
# Default matches nds_crc_verify HUGE_FIRST_P2P
ACL_POLICY=3
OUTDIR=
SKIP_BUILD=0
SEED_BASE=1000
FAILS=0
# One-shot pin queries iov length; need ≥2MiB for Ascend huge pages
MIN_NOREG_BYTES=$((2 << 20))

die() { printf 'npu_crc_verify: %s\n' "$*" >&2; exit 1; }
log() { printf '==> %s\n' "$*" >&2; }

usage()
{
	cat <<'EOF'
Usage: npu_crc_verify.sh --topology DEV --data-dir DIR --npu-devices IDS [options]

Required:
  --topology DEV          NDS topology block device
  --data-dir DIR          directory for per-NPU patterned files
  --npu-devices LIST      comma list of NPU ids (e.g. 0,1,2,3)

Options:
  --file-size SIZE        per-NPU file size (default: 64M)
  --length-list LIST      transfer lengths (default: 64K,128K,1M,2M)
  --mem-mode LIST         register,noregister (default: both)
                          note: noregister skips lengths < 2M (Ascend huge pages)
  --acl-policy N          aclrt malloc policy (default: 3 = HUGE_FIRST_P2P)
  --outdir DIR            results directory (default: ./npu-crc-results-<timestamp>)
  --skip-build            do not rebuild nds_crc_verify
  -h, --help              show this help

Env overrides: ASCEND_ACL_LIB, NPU_CRC_VERIFY (path to nds_crc_verify binary).
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

build_verify()
{
	[[ $SKIP_BUILD == 1 ]] && return 0
	# Always rebuild so machines do not keep an old binary that still
	# registers 4K windows (invalid page size 4096).
	log "Building nds_crc_verify"
	make -C "$SCRIPT_DIR" nds_crc_verify
	VERIFY=$SCRIPT_DIR/nds_crc_verify
}

prepare_pattern_files()
{
	local seed_round=$1
	local bytes npu path seed

	bytes=$(parse_size_bytes "$FILE_SIZE")
	(( bytes > 0 && (bytes % 512) == 0 )) || \
		die "--file-size must be positive and 512B-aligned"

	mkdir -p "$DATA_DIR"
	[[ -x $PATTERN_GEN || -f $PATTERN_GEN ]] || die "missing $PATTERN_GEN"
	for npu in $(split_csv "$NPU_DEVICES"); do
		path=$(npu_file "$npu")
		seed=$((SEED_BASE + seed_round * 100 + npu))
		log "Pattern file $path size=$FILE_SIZE seed=$seed"
		python3 "$PATTERN_GEN" --output "$path" --size "$bytes" --seed "$seed"
	done
	sync
}

mid_offset_bytes()
{
	local file_bytes length_bytes mid

	file_bytes=$(parse_size_bytes "$FILE_SIZE")
	length_bytes=$(parse_size_bytes "$1")
	mid=$(( (file_bytes / 2) & ~511 ))
	if (( mid + length_bytes > file_bytes )); then
		mid=$(( (file_bytes - length_bytes) & ~511 ))
	fi
	(( mid >= 0 )) || mid=0
	printf '%s\n' "$mid"
}

parse_result_ok()
{
	# stdin → print ok value from last RESULT line
	awk '
		/^RESULT / {
			ok="";
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^ok=/) { ok=$i; sub(/^ok=/,"",ok) }
			}
		}
		END {
			if (ok == "") exit 1
			print ok
		}'
}

append_row()
{
	# mode npu mem length offset ok note
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$OUTDIR/results.tsv"
}

run_one_verify()
{
	local npu=$1 mem=$2 length=$3 offset=$4 logfile=$5
	local args=() target

	target=$(npu_file "$npu")
	args=(
		--topology "$TOPOLOGY"
		--target "$target"
		--npu-device "$npu"
		--offset "$offset"
		--length "$length"
		--acl-policy "$ACL_POLICY"
	)
	case $mem in
		register) ;;
		noregister) args+=(--no-register-mem) ;;
		*) die "bad mem $mem" ;;
	esac

	set +e
	"$VERIFY" "${args[@]}" >"$logfile" 2>&1
	local rc=$?
	set -e
	return "$rc"
}

run_solo_cases()
{
	local length=$1 mem=$2
	local npu offset mid logf ok rc

	mid=$(mid_offset_bytes "$length")
	for offset in 0 "$mid"; do
		for npu in $(split_csv "$NPU_DEVICES"); do
			logf=$OUTDIR/logs/solo.npu${npu}.${mem}.len${length}.off${offset}.log
			log "solo npu=$npu mem=$mem length=$length offset=$offset"
			rc=0
			run_one_verify "$npu" "$mem" "$length" "$offset" "$logf" || rc=$?
			ok=$(parse_result_ok <"$logf" 2>/dev/null || echo 0)
			if [[ $rc -ne 0 || $ok != 1 ]]; then
				append_row "solo" "$npu" "$mem" "$length" "$offset" "0" "FAIL"
				printf '  FAIL npu%s mem=%s len=%s off=%s (see %s)\n' \
					"$npu" "$mem" "$length" "$offset" "$logf"
				FAILS=$((FAILS + 1))
			else
				append_row "solo" "$npu" "$mem" "$length" "$offset" "1" "pass"
				printf '  PASS npu%s mem=%s len=%s off=%s\n' \
					"$npu" "$mem" "$length" "$offset"
			fi
		done
	done
}

run_concurrent_case()
{
	local length=$1 mem=$2
	local npu offset mid logf ok pids=() fails_wait=0 rc

	mid=$(mid_offset_bytes "$length")
	# Concurrent uses offset 0 only (one overlapping wall-clock window).
	offset=0
	log "CONCURRENT mem=$mem length=$length offset=$offset npus=$NPU_DEVICES"

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/conc.npu${npu}.${mem}.len${length}.off${offset}.log
		(
			run_one_verify "$npu" "$mem" "$length" "$offset" "$logf"
		) &
		pids+=($!)
	done

	local pid
	for pid in "${pids[@]}"; do
		wait "$pid" || fails_wait=$((fails_wait + 1))
	done

	for npu in $(split_csv "$NPU_DEVICES"); do
		logf=$OUTDIR/logs/conc.npu${npu}.${mem}.len${length}.off${offset}.log
		ok=$(parse_result_ok <"$logf" 2>/dev/null || echo 0)
		if [[ $ok != 1 ]]; then
			append_row "concurrent" "$npu" "$mem" "$length" "$offset" "0" "FAIL"
			printf '  FAIL npu%s (see %s)\n' "$npu" "$logf"
			FAILS=$((FAILS + 1))
		else
			append_row "concurrent" "$npu" "$mem" "$length" "$offset" "1" "pass"
			printf '  PASS npu%s\n' "$npu"
		fi
	done
	(( fails_wait == 0 )) || true
}

write_summary()
{
	{
		printf 'Physical NPU NDS CRC verify (nds_crc_verify D2H)\n'
		printf 'topology=%s data_dir=%s file_size=%s\n' \
			"$TOPOLOGY" "$DATA_DIR" "$FILE_SIZE"
		printf 'npus=%s lengths=%s mem=%s fails=%s\n\n' \
			"$NPU_DEVICES" "$LENGTH_LIST" "$MEM_MODE" "$FAILS"
		if command -v column >/dev/null 2>&1; then
			column -t -s $'\t' "$OUTDIR/results.tsv"
		else
			cat "$OUTDIR/results.tsv"
		fi
	} | tee "$OUTDIR/summary.txt"
}

main()
{
	local length mem seed_round=0

	while [[ $# -gt 0 ]]; do
		case $1 in
			--topology) TOPOLOGY=$2; shift 2 ;;
			--data-dir) DATA_DIR=$2; shift 2 ;;
			--npu-devices) NPU_DEVICES=$2; shift 2 ;;
			--file-size) FILE_SIZE=$2; shift 2 ;;
			--length-list) LENGTH_LIST=$2; shift 2 ;;
			--mem-mode) MEM_MODE=$2; shift 2 ;;
			--acl-policy) ACL_POLICY=$2; shift 2 ;;
			--outdir) OUTDIR=$2; shift 2 ;;
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
		OUTDIR=$(pwd)/npu-crc-results-$(date +%Y%m%d-%H%M%S)
	fi
	mkdir -p "$OUTDIR/logs"
	printf 'mode\tnpu\tmem\tlength\toffset\tok\tnote\n' >"$OUTDIR/results.tsv"

	build_verify
	[[ -x $VERIFY ]] || die "nds_crc_verify not found at $VERIFY"

	prepare_pattern_files "$seed_round"
	seed_round=$((seed_round + 1))

	for length in $(split_csv "$LENGTH_LIST"); do
		local len_bytes
		len_bytes=$(parse_size_bytes "$length")
		(( len_bytes % 512 == 0 )) || die "length $length not 512B-aligned"
		for mem in $(split_csv "$MEM_MODE"); do
			case $mem in register|noregister) ;; *) die "bad --mem-mode entry: $mem" ;; esac
			if [[ $mem == noregister && $len_bytes -lt $MIN_NOREG_BYTES ]]; then
				log "skip noregister length=$length (<2M Ascend huge-page min)"
				append_row "solo" "-" "$mem" "$length" "-" "-" "skipped_lt_2m"
				append_row "concurrent" "-" "$mem" "$length" "-" "-" "skipped_lt_2m"
				continue
			fi
			run_solo_cases "$length" "$mem"
			prepare_pattern_files "$seed_round"
			seed_round=$((seed_round + 1))
			run_concurrent_case "$length" "$mem"
			prepare_pattern_files "$seed_round"
			seed_round=$((seed_round + 1))
		done
	done

	write_summary
	log "Results: $OUTDIR/results.tsv"
	log "Summary: $OUTDIR/summary.txt"
	(( FAILS == 0 )) || die "$FAILS case(s) failed"
	log "All CRC cases passed"
}

main "$@"
