#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

CONFIG_FILE=$WORK_DIR/remote.env
: >"$CONFIG_FILE"
export XDS_REMOTE_TEST_ENV=$CONFIG_FILE
export XDS_REMOTE_HOST=example.invalid
export XDS_REMOTE_USER=tester
export XDS_REMOTE_PORT=22
export XDS_GUEST_USER=xds
export XDS_GUEST_PORT=22222
export XDS_GUEST_KEY=/tmp/key
export XDS_GUEST_REPO=/home/xds/xds
export XDS_GUEST_KSRC=/home/xds/oe_knl
export XDS_GUEST_DEV_1=/dev/nvme0n1
export XDS_GUEST_DEV_2=/dev/nvme0n2
export XDS_RESULT_DIR=$WORK_DIR/results
export XDS_SSH_CONFIG=/tmp/xds-ssh-config

# shellcheck source=run_dual_kernel_matrix.sh
source "$SCRIPT_DIR/run_dual_kernel_matrix.sh"

test_copy_artifacts()
{
	local source_dir=$WORK_DIR/source
	local destination_dir=$WORK_DIR/copied

	mkdir -p "$source_dir/one" "$source_dir/two" "$destination_dir"
	printf 'one\n' >"$source_dir/one/first.out"
	printf 'two\n' >"$source_dir/two/second.dmesg"
	copy_artifact_files "$source_dir" "$destination_dir"
	cmp "$source_dir/one/first.out" "$destination_dir/first.out"
	cmp "$source_dir/two/second.dmesg" \
		"$destination_dir/second.dmesg"
}

test_copy_collision()
{
	local source_dir=$WORK_DIR/collision-source
	local destination_dir=$WORK_DIR/collision-destination

	mkdir -p "$source_dir/one" "$source_dir/two" "$destination_dir"
	printf 'one\n' >"$source_dir/one/duplicate.out"
	printf 'two\n' >"$source_dir/two/duplicate.out"
	if copy_artifact_files "$source_dir" "$destination_dir"; then
		printf 'copy_artifact_files accepted duplicate basenames\n' >&2
		return 1
	fi
}

test_suite_failure_propagation()
{
	mkdir -p "$RESULT_BASE/nokasan"
	guest_exec()
	{
		return 23
	}

	if run_suite nokasan failure ignored.sh; then
		printf 'run_suite masked the guest failure\n' >&2
		return 1
	fi
	grep -q '^test_exit_status=23$' \
		"$RESULT_BASE/nokasan/failure.summary"
}

test_running_config_is_restored()
{
	grep -Fq 'zcat /proc/config.gz >.config' \
		"$SCRIPT_DIR/run_dual_kernel_matrix.sh"
	grep -Fq 'modinfo -F vermagic' \
		"$SCRIPT_DIR/run_dual_kernel_matrix.sh"
}

test_ssh_config_is_honored()
{
	[[ ${REMOTE_SSH[1]} == -F ]]
	[[ ${REMOTE_SSH[2]} == "$XDS_SSH_CONFIG" ]]
	grep -Fq "'ssh -F %q -p %q" \
		"$SCRIPT_DIR/run_dual_kernel_matrix.sh"
}

test_copy_artifacts
test_copy_collision
test_suite_failure_propagation
test_running_config_is_restored
test_ssh_config_is_honored
printf 'dual-kernel runner tests passed\n'
