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
REMOTE_LAUNCHER=${XDS_REMOTE_LAUNCHER:?set XDS_REMOTE_LAUNCHER in "$CONFIG_FILE"}
REMOTE_PID_FILE=${XDS_REMOTE_PID_FILE:?set XDS_REMOTE_PID_FILE in "$CONFIG_FILE"}
REMOTE_MONITOR=${XDS_REMOTE_MONITOR:?set XDS_REMOTE_MONITOR in "$CONFIG_FILE"}
GUEST_USER=${XDS_GUEST_USER:?set XDS_GUEST_USER in "$CONFIG_FILE"}
GUEST_PORT=${XDS_GUEST_PORT:?set XDS_GUEST_PORT in "$CONFIG_FILE"}
GUEST_KEY=${XDS_GUEST_KEY:?set XDS_GUEST_KEY in "$CONFIG_FILE"}
WAIT_TIMEOUT=${XDS_VM_WAIT_TIMEOUT:?set XDS_VM_WAIT_TIMEOUT in "$CONFIG_FILE"}
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
		-o BatchMode=yes -o ConnectTimeout=5 \
		-o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null \
		"$GUEST_USER@127.0.0.1" "$guest_command"
	"${REMOTE_SSH[@]}" "$host_command"
}

log()
{
	printf '==> %s\n' "$*"
}

log "checking VM state on $REMOTE_TARGET"
"${REMOTE_SSH[@]}" bash -s -- \
	"$REMOTE_RUN_DIR" "$REMOTE_LAUNCHER" "$REMOTE_PID_FILE" \
	"$REMOTE_MONITOR" <<'REMOTE_SCRIPT'
set -Eeuo pipefail

run_dir=$1
launcher=$2
pid_file=$3
monitor=$4

[[ -d $run_dir ]] || {
	printf 'remote VM directory does not exist: %s\n' "$run_dir" >&2
	exit 1
}
[[ -x $launcher ]] || {
	printf 'remote VM launcher is not executable: %s\n' "$launcher" >&2
	exit 1
}

if [[ -r $pid_file ]]; then
	read -r pid <"$pid_file"
	if [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
		printf 'VM is already running with pid %s\n' "$pid"
		exit 0
	fi
fi

if pgrep -u "$(id -u)" -f \
	'qemu-system-x86_64.*-name[[:space:]]+xds-kvm-test' >/dev/null; then
	printf 'an xds-kvm-test QEMU process exists without a valid pid file\n' >&2
	exit 1
fi

rm -f -- "$pid_file" "$monitor"
"$launcher"

read -r pid <"$pid_file"
[[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null || {
	printf 'QEMU did not leave a live pid in %s\n' "$pid_file" >&2
	exit 1
}
printf 'started VM with pid %s\n' "$pid"
REMOTE_SCRIPT

log "waiting up to ${WAIT_TIMEOUT}s for guest SSH"
deadline=$((SECONDS + WAIT_TIMEOUT))
while (( SECONDS < deadline )); do
	if guest_exec true >/dev/null 2>&1; then
		log "guest is ready"
		guest_exec bash -c \
			'uname -r; printf "use_cmb_sqes="; cat /sys/module/nvme/parameters/use_cmb_sqes'
		exit 0
	fi
	sleep 2
done

printf 'guest SSH did not become ready within %s seconds\n' \
	"$WAIT_TIMEOUT" >&2
exit 1
