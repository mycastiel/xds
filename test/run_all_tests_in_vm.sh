#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
KERNEL_VARIANT=${XDS_KERNEL_VARIANT:-}
RESULT_ROOT=${XDS_RESULT_ROOT:-$HOME/xds-matrix-results}
DEV1=${XDS_DEV_1:-/dev/nvme0n1}
DEV2=${XDS_DEV_2:-/dev/nvme0n2}
KSRC=${KSRC:-/home/xds/oe_knl}
STRESS_ITERATIONS=16
VARIANT_DIR=
RAW_DIR=

usage()
{
	cat <<EOF
Usage:
  XDS_KERNEL_VARIANT=kasan|nokasan [XDS_RESULT_ROOT=PATH] \\
    $0

Run the complete XDS test suite directly in the current VM. The script is
destructive to XDS_DEV_1 and XDS_DEV_2.

Required:
  XDS_KERNEL_VARIANT  Explicit report variant: kasan or nokasan

Optional:
  XDS_RESULT_ROOT     Matrix result root (default: \$HOME/xds-matrix-results)
  XDS_DEV_1           First disposable NVMe namespace (default: /dev/nvme0n1)
  XDS_DEV_2           Second disposable NVMe namespace (default: /dev/nvme0n2)
  KSRC                 Matching kernel build tree (default: /home/xds/oe_knl)
  XDS_TEST_BUILD_JOBS Parallel build jobs passed to the test suites
EOF
}

die()
{
	printf 'run_all_tests_in_vm.sh: %s\n' "$*" >&2
	return 1
}

validate_options()
{
	case $KERNEL_VARIANT in
		kasan | nokasan) ;;
		"") die "set XDS_KERNEL_VARIANT to kasan or nokasan" ;;
		*) die "XDS_KERNEL_VARIANT must be kasan or nokasan" ;;
	esac
	[[ -n $RESULT_ROOT ]] || die "XDS_RESULT_ROOT must not be empty"
	[[ -d $KSRC ]] || die "kernel build tree does not exist: $KSRC"
}

prepare_result_directories()
{
	mkdir -p "$RESULT_ROOT"
	RESULT_ROOT=$(cd -- "$RESULT_ROOT" && pwd)
	VARIANT_DIR=$RESULT_ROOT/$KERNEL_VARIANT
	RAW_DIR=$VARIANT_DIR/raw

	if [[ -d $VARIANT_DIR ]] &&
	   [[ -n $(find "$VARIANT_DIR" -mindepth 1 -print -quit) ]]; then
		die "$VARIANT_DIR is not empty; select a new XDS_RESULT_ROOT"
	fi
	mkdir -p "$RAW_DIR"
}

copy_report_artifacts()
{
	local source_dir=$1
	local copy_identity=$2
	local source destination
	local copied=0

	while IFS= read -r -d '' source; do
		destination=$VARIANT_DIR/${source##*/}
		[[ ! -e $destination ]] ||
			die "duplicate report artifact: ${source##*/}"
		sudo cp -- "$source" "$destination"
		((copied += 1))
	done < <(
		sudo find "$source_dir" -maxdepth 1 -type f \
			\( -name '*.out' -o -name '*.dmesg' \) -print0
	)
	(( copied > 0 )) || die "no report artifacts found in $source_dir"

	if [[ $copy_identity == 1 ]]; then
		sudo test -f "$source_dir/kernel.info" ||
			die "kernel identity is missing from $source_dir"
		sudo cp -- "$source_dir/kernel.info" "$VARIANT_DIR/kernel.info"
	fi
}

run_suite()
{
	local suite=$1
	local copy_identity=$2
	shift 2
	local log_path=$VARIANT_DIR/$suite.test.log
	local artifact_dir
	local status
	local -a environment=(
		"XDS_DEV_1=$DEV1"
		"XDS_DEV_2=$DEV2"
		"KSRC=$KSRC"
		"XDS_KERNEL_VARIANT=$KERNEL_VARIANT"
		"XDS_TEST_KEEP_WORKDIR=1"
		"XDS_TEST_WORKDIR=$RAW_DIR"
	)

	if [[ -n ${XDS_TEST_BUILD_JOBS:-} ]]; then
		environment+=("XDS_TEST_BUILD_JOBS=$XDS_TEST_BUILD_JOBS")
	fi

	printf '\n==> Running %s/%s\n' "$KERNEL_VARIANT" "$suite"
	set +e
	sudo env "${environment[@]}" "$@" 2>&1 | tee "$log_path"
	status=${PIPESTATUS[0]}
	set -e
	if (( status != 0 )); then
		sudo chown -R "$(id -u):$(id -g)" "$VARIANT_DIR"
		die "$suite failed with status $status; see $log_path"
	fi

	artifact_dir=$(
		sed -n 's/^XDS test artifacts: //p' "$log_path" |
			tail -n 1
	)
	[[ -n $artifact_dir ]] || die "$suite did not report its artifact directory"
	sudo test -d "$artifact_dir" ||
		die "$suite artifact directory is unavailable: $artifact_dir"
	copy_report_artifacts "$artifact_dir" "$copy_identity"
	sudo chown -R "$(id -u):$(id -g)" "$VARIANT_DIR"
}

main()
{
	local mode

	if (( $# == 1 )) && [[ $1 == --help ]]; then
		usage
		return 0
	fi
	(( $# == 0 )) || die "only --help is accepted"

	validate_options
	prepare_result_directories
	sudo -v

	printf 'Kernel variant: %s\n' "$KERNEL_VARIANT"
	printf 'Result root: %s\n' "$RESULT_ROOT"
	printf 'Kernel build tree: %s\n' "$KSRC"
	printf 'WARNING: %s and %s will be destroyed.\n' "$DEV1" "$DEV2"

	run_suite basic 1 "$SCRIPT_DIR/basic_test.sh"
	for mode in raid0 dm nvme; do
		run_suite "stress-$mode" 0 \
			"XDS_STRESS_MODE=$mode" \
			"XDS_STRESS_ITERATIONS=$STRESS_ITERATIONS" \
			"$SCRIPT_DIR/stress_test.sh"
	done
	for mode in raid0 dm nvme; do
		run_suite "cq-race-$mode" 0 \
			"XDS_STRESS_MODE=$mode" \
			"$SCRIPT_DIR/cq_race_test.sh"
	done

	sudo chown -R "$(id -u):$(id -g)" "$VARIANT_DIR"
	printf '\nAll %s tests passed.\n' "$KERNEL_VARIANT"
	printf 'Artifacts: %s\n' "$VARIANT_DIR"
	printf 'After both variants finish, run gen_report_in_vm.sh.\n'
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
