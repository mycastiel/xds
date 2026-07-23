#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CONFIG_FILE=${XDS_REMOTE_TEST_ENV:-$SCRIPT_DIR/remote_test.env}
if [[ ! -r $CONFIG_FILE ]]; then
	printf 'remote test configuration is not readable: %s\n' \
		"$CONFIG_FILE" >&2
	printf 'copy %s/remote_test.env.example and update its values\n' \
		"$SCRIPT_DIR" >&2
	exit 1
fi
# This file is local, ignored by git, and contains shell variable assignments.
# shellcheck source=/dev/null
source "$CONFIG_FILE"

REMOTE_HOST=${XDS_REMOTE_HOST:?set XDS_REMOTE_HOST in "$CONFIG_FILE"}
REMOTE_USER=${XDS_REMOTE_USER:?set XDS_REMOTE_USER in "$CONFIG_FILE"}
REMOTE_PORT=${XDS_REMOTE_PORT:?set XDS_REMOTE_PORT in "$CONFIG_FILE"}
REMOTE_RUN_DIR=${XDS_REMOTE_RUN_DIR:?set XDS_REMOTE_RUN_DIR in "$CONFIG_FILE"}
GUEST_USER=${XDS_GUEST_USER:?set XDS_GUEST_USER in "$CONFIG_FILE"}
GUEST_PORT=${XDS_GUEST_PORT:?set XDS_GUEST_PORT in "$CONFIG_FILE"}
GUEST_KEY=${XDS_GUEST_KEY:?set XDS_GUEST_KEY in "$CONFIG_FILE"}
GUEST_REPO=${XDS_GUEST_REPO:?set XDS_GUEST_REPO in "$CONFIG_FILE"}
GUEST_KSRC=${XDS_GUEST_KSRC:?set XDS_GUEST_KSRC in "$CONFIG_FILE"}
GUEST_DEV_1=${XDS_GUEST_DEV_1:?set XDS_GUEST_DEV_1 in "$CONFIG_FILE"}
GUEST_DEV_2=${XDS_GUEST_DEV_2:?set XDS_GUEST_DEV_2 in "$CONFIG_FILE"}
KEEP_WORKDIR=${XDS_TEST_KEEP_WORKDIR:?set XDS_TEST_KEEP_WORKDIR in "$CONFIG_FILE"}
RESULT_DIR=${XDS_RESULT_DIR:-/tmp/xds-remote-test.$(date -u +%Y%m%dT%H%M%SZ)}
SSH_CONFIG=${XDS_SSH_CONFIG:-/dev/null}

readonly REMOTE_TARGET=$REMOTE_USER@$REMOTE_HOST
readonly -a REMOTE_SSH=(
	ssh
	-F "$SSH_CONFIG"
	-p "$REMOTE_PORT"
	-o BatchMode=yes
	-o ConnectTimeout=10
	-o StrictHostKeyChecking=accept-new
	"$REMOTE_TARGET"
)

guest_exec()
{
	local guest_command
	local host_command

	printf -v guest_command '%q ' "$@"
	printf -v host_command '%q ' \
		ssh -F /dev/null -i "$GUEST_KEY" -p "$GUEST_PORT" \
		-o BatchMode=yes -o ConnectTimeout=10 \
		-o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null \
		"$GUEST_USER@127.0.0.1" "$guest_command"
	"${REMOTE_SSH[@]}" "$host_command"
}

capture_kernel_log()
{
	local output=$1
	local errors=$2

	guest_exec sudo -n dmesg --raw >"$output" 2>"$errors"
}

mkdir -p -- "$RESULT_DIR"
RESULT_DIR=$(cd -- "$RESULT_DIR" && pwd)
readonly RESULT_DIR
readonly TEST_LOG=$RESULT_DIR/test.log
readonly KERNEL_BEFORE=$RESULT_DIR/kernel-before.log
readonly KERNEL_LOG=$RESULT_DIR/kernel.log
readonly KERNEL_TEST_LOG=$RESULT_DIR/kernel-test.log
readonly SUMMARY=$RESULT_DIR/summary.txt

printf '==> results: %s\n' "$RESULT_DIR"
printf '==> capturing kernel log before the test\n'
kernel_status=0
capture_kernel_log "$KERNEL_BEFORE" \
	"$RESULT_DIR/kernel-before.stderr" || kernel_status=$?

started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
printf '==> running basic_test.sh in the guest\n'
set +e
guest_exec sudo -n env \
	"XDS_DEV_1=$GUEST_DEV_1" \
	"XDS_DEV_2=$GUEST_DEV_2" \
	"KSRC=$GUEST_KSRC" \
	"XDS_TEST_KEEP_WORKDIR=$KEEP_WORKDIR" \
	"$GUEST_REPO/test/basic_test.sh" 2>&1 | tee "$TEST_LOG"
test_status=${PIPESTATUS[0]}
set -e
finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

printf '==> capturing kernel log after the test\n'
capture_kernel_log "$KERNEL_LOG" \
	"$RESULT_DIR/kernel.stderr" || kernel_status=$?

if (( kernel_status == 0 )); then
	before_lines=$(wc -l <"$KERNEL_BEFORE")
	after_lines=$(wc -l <"$KERNEL_LOG")
	if (( after_lines >= before_lines )); then
		tail -n "+$((before_lines + 1))" "$KERNEL_LOG" >"$KERNEL_TEST_LOG"
	else
		# The ring buffer wrapped during the run; retain everything available.
		cp -- "$KERNEL_LOG" "$KERNEL_TEST_LOG"
	fi
fi

guest_artifacts=$(
	sed -n 's/^XDS test artifacts: //p' "$TEST_LOG" | tail -n 1
)
artifact_status=0
if [[ $guest_artifacts =~ ^/tmp/xds-basic-test\.[[:alnum:]]+$ ]]; then
	printf '==> downloading guest artifacts from %s\n' "$guest_artifacts"
	guest_exec sudo -n tar -C "${guest_artifacts%/*}" -czf - \
		"${guest_artifacts##*/}" \
		>"$RESULT_DIR/guest-artifacts.tar.gz" || artifact_status=$?
elif [[ $KEEP_WORKDIR == 1 ]]; then
	printf 'could not find the retained guest artifact path in test output\n' >&2
	artifact_status=1
fi

if (( artifact_status == 0 )) &&
	[[ -s $RESULT_DIR/guest-artifacts.tar.gz ]]; then
	member_list=$RESULT_DIR/guest-artifacts.list
	if tar -tzf "$RESULT_DIR/guest-artifacts.tar.gz" >"$member_list"; then
		: >"$KERNEL_TEST_LOG"
		dmesg_count=0
		while IFS= read -r member; do
			[[ $member == *.dmesg ]] || continue
			printf '\n===== %s =====\n' "$member" >>"$KERNEL_TEST_LOG"
			if ! tar -xOzf "$RESULT_DIR/guest-artifacts.tar.gz" \
				-- "$member" >>"$KERNEL_TEST_LOG"; then
				artifact_status=1
				break
			fi
			((dmesg_count += 1))
		done <"$member_list"
		if (( dmesg_count == 0 )); then
			printf 'the guest artifact archive has no kernel logs\n' >&2
			artifact_status=1
		fi
	else
		artifact_status=$?
	fi
fi

result=FAIL
if (( test_status == 0 )); then
	result=PASS
fi

{
	printf 'result=%s\n' "$result"
	printf 'test_exit_status=%s\n' "$test_status"
	printf 'kernel_capture_status=%s\n' "$kernel_status"
	printf 'artifact_download_status=%s\n' "$artifact_status"
	printf 'started_at=%s\n' "$started_at"
	printf 'finished_at=%s\n' "$finished_at"
	printf 'remote_host=%s\n' "$REMOTE_TARGET"
	printf 'guest=%s@127.0.0.1:%s\n' "$GUEST_USER" "$GUEST_PORT"
	printf 'guest_artifacts=%s\n' "$guest_artifacts"
} >"$SUMMARY"

printf '==> test result: %s (exit %s)\n' "$result" "$test_status"
printf '==> summary: %s\n' "$SUMMARY"
printf '==> test output: %s\n' "$TEST_LOG"
printf '==> kernel output: %s\n' "$KERNEL_TEST_LOG"

(( test_status == 0 && kernel_status == 0 && artifact_status == 0 ))
