#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RESULT_ROOT=${XDS_RESULT_ROOT:-$HOME/xds-matrix-results}
REPORT_PATH=${XDS_REPORT_PATH:-$RESULT_ROOT/report.md}

usage()
{
	cat <<EOF
Usage:
  [XDS_RESULT_ROOT=PATH] [XDS_REPORT_PATH=PATH] $0

Generate one Markdown report after run_all_tests_in_vm.sh has populated the
kasan/ and nokasan/ directories under XDS_RESULT_ROOT.

Optional:
  XDS_RESULT_ROOT  Matrix result root (default: \$HOME/xds-matrix-results)
  XDS_REPORT_PATH  Markdown output path (default: RESULT_ROOT/report.md)
EOF
}

die()
{
	printf 'gen_report_in_vm.sh: %s\n' "$*" >&2
	return 1
}

main()
{
	local variant

	if (( $# == 1 )) && [[ $1 == --help ]]; then
		usage
		return 0
	fi
	(( $# == 0 )) || die "only --help is accepted"
	[[ -d $RESULT_ROOT ]] || die "result root does not exist: $RESULT_ROOT"
	RESULT_ROOT=$(cd -- "$RESULT_ROOT" && pwd)

	for variant in kasan nokasan; do
		[[ -f $RESULT_ROOT/$variant/kernel.info ]] ||
			die "$RESULT_ROOT/$variant/kernel.info is missing"
	done
	mkdir -p "$(dirname -- "$REPORT_PATH")"

	python3 "$SCRIPT_DIR/report.py" \
		--artifact-dir "$RESULT_ROOT" \
		--md-report "$REPORT_PATH"
	printf 'Detailed report: %s\n' "$REPORT_PATH"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
