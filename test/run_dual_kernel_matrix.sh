#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
CONFIG_FILE=${XDS_REMOTE_TEST_ENV:-$SCRIPT_DIR/remote_test.env}
if [[ ! -r $CONFIG_FILE ]]; then
	printf 'remote test configuration is not readable: %s\n' \
		"$CONFIG_FILE" >&2
	exit 1
fi
# shellcheck source=/dev/null
source "$CONFIG_FILE"

REMOTE_HOST=${XDS_REMOTE_HOST:?set XDS_REMOTE_HOST in "$CONFIG_FILE"}
REMOTE_USER=${XDS_REMOTE_USER:?set XDS_REMOTE_USER in "$CONFIG_FILE"}
REMOTE_PORT=${XDS_REMOTE_PORT:?set XDS_REMOTE_PORT in "$CONFIG_FILE"}
GUEST_USER=${XDS_GUEST_USER:?set XDS_GUEST_USER in "$CONFIG_FILE"}
GUEST_PORT=${XDS_GUEST_PORT:?set XDS_GUEST_PORT in "$CONFIG_FILE"}
GUEST_KEY=${XDS_GUEST_KEY:?set XDS_GUEST_KEY in "$CONFIG_FILE"}
GUEST_REPO=${XDS_GUEST_REPO:?set XDS_GUEST_REPO in "$CONFIG_FILE"}
GUEST_KSRC=${XDS_GUEST_KSRC:?set XDS_GUEST_KSRC in "$CONFIG_FILE"}
GUEST_DEV_1=${XDS_GUEST_DEV_1:?set XDS_GUEST_DEV_1 in "$CONFIG_FILE"}
GUEST_DEV_2=${XDS_GUEST_DEV_2:?set XDS_GUEST_DEV_2 in "$CONFIG_FILE"}
KERNEL_BASE_VERSION=${XDS_KERNEL_BASE_VERSION:-6.6.0}
BUILD_KERNELS=${XDS_MATRIX_BUILD_KERNELS:-0}
MATRIX_STRESS_ITERATIONS=16
SSH_CONFIG=${XDS_SSH_CONFIG:-/dev/null}

readonly REMOTE_TARGET=$REMOTE_USER@$REMOTE_HOST
readonly -a REMOTE_SSH=(
	ssh
	-F "$SSH_CONFIG"
	-p "$REMOTE_PORT"
	-o BatchMode=yes
	-o ConnectTimeout=20
	-o StrictHostKeyChecking=accept-new
	"$REMOTE_TARGET"
)

NOW=$(date -u +%Y%m%dT%H%M%SZ)
RESULT_BASE=${XDS_RESULT_DIR:-/tmp/xds-matrix-$NOW}
mkdir -p "$RESULT_BASE"
RESULT_BASE=$(cd -- "$RESULT_BASE" && pwd)

log()
{
	printf '\n==> %s\n' "$*"
}

guest_exec()
{
	local guest_command
	local host_command

	printf -v guest_command '%q ' "$@"
	printf -v host_command '%q ' \
		ssh -i "$GUEST_KEY" -p "$GUEST_PORT" \
		-o BatchMode=yes -o ConnectTimeout=10 \
		-o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null \
		"$GUEST_USER@127.0.0.1" "$guest_command"
	"${REMOTE_SSH[@]}" "$host_command"
}

sync_repo()
{
	local ssh_transport

	log "syncing $REPO_ROOT to the guest"
	"${REMOTE_SSH[@]}" mkdir -p /tmp/xds-sync
	printf -v ssh_transport \
		'ssh -F %q -p %q -o BatchMode=yes -o ConnectTimeout=20 ' \
		"$SSH_CONFIG" "$REMOTE_PORT"
	ssh_transport+='-o StrictHostKeyChecking=accept-new'
	rsync -az --delete \
		--exclude=.git \
		--exclude=.vimrc \
		--exclude=__pycache__ \
		--exclude=file_p2p/build \
		--exclude='*.o' \
		--exclude='*.ko' \
		--exclude='*.mod' \
		--exclude='*.mod.c' \
		--exclude='*.so' \
		--exclude=Module.symvers \
		--exclude=modules.order \
		-e "$ssh_transport" \
		"$REPO_ROOT/" "$REMOTE_TARGET:/tmp/xds-sync/"
	"${REMOTE_SSH[@]}" bash -s -- \
		"$GUEST_KEY" "$GUEST_PORT" "$GUEST_USER" \
		"$GUEST_REPO" <<'SYNC_SCRIPT'
set -Eeuo pipefail

key=$1
port=$2
user=$3
repo=$4
transport="ssh -i $key -p $port"
transport+=" -o BatchMode=yes -o StrictHostKeyChecking=no"
transport+=" -o UserKnownHostsFile=/dev/null"
rsync -a --delete -e "$transport" \
	--rsync-path='sudo rsync' \
	/tmp/xds-sync/ "$user@127.0.0.1:$repo/"
SYNC_SCRIPT
}

boot_kernel()
{
	local variant=$1
	local version="$KERNEL_BASE_VERSION-xds-$variant"

	log "booting $version"
	guest_exec sudo grubby --set-default "/boot/vmlinuz-$version"
	guest_exec sudo shutdown -r now >/dev/null 2>&1 || true

	local deadline=$((SECONDS + 300))
	while (( SECONDS < deadline )); do
		if guest_exec true >/dev/null 2>&1; then
			local actual

			actual=$(guest_exec uname -r 2>/dev/null)
			actual=${actual//$'\r'/}
			actual=${actual//$'\n'/}
			if [[ $actual == "$version" ]]; then
				log "running exact kernel $actual"
				return 0
			fi
		fi
		sleep 5
	done
	printf 'guest did not boot into %s\n' "$version" >&2
	return 1
}

prepare_guest()
{
	local variant=$1
	local version="$KERNEL_BASE_VERSION-xds-$variant"

	log "preparing module build tree for $version"
	guest_exec bash -s -- \
		"$GUEST_KSRC" "$GUEST_REPO" "$version" <<'PREPARE_SCRIPT'
set -Eeuo pipefail

ksrc=$1
repo=$2
version=$3

sudo rmmod nvme 2>/dev/null || true
sudo modprobe nvme use_cmb_sqes=0
value=$(cat /sys/module/nvme/parameters/use_cmb_sqes)
case ${value,,} in
	n | 0 | no | false) ;;
	*)
		printf 'nvme.use_cmb_sqes remains enabled: %s\n' "$value" >&2
		exit 1
		;;
esac

cd "$ksrc"
if [[ -r /proc/config.gz ]]; then
	zcat /proc/config.gz >.config
elif [[ -r /boot/config-$version ]]; then
	cp "/boot/config-$version" .config
else
	printf 'no config found for running kernel %s\n' "$version" >&2
	exit 1
fi
make olddefconfig
make modules_prepare

cd "$repo"
sudo make clean
sudo make
vermagic=$(modinfo -F vermagic "$repo/stub.ko")
if [[ $vermagic != "$version "* ]]; then
	printf 'stub.ko vermagic mismatch: %s, expected %s\n' \
		"$vermagic" "$version" >&2
	exit 1
fi
PREPARE_SCRIPT
}

save_kernel_identity()
{
	local variant=$1
	local variant_dir="$RESULT_BASE/$variant"

	mkdir -p "$variant_dir"
	guest_exec bash -s -- "$GUEST_KSRC" "$variant" <<'IDENTITY_SCRIPT' \
		>"$variant_dir/kernel.info"
set -Eeuo pipefail

ksrc=$1
variant=$2
kasan_config=$(
	grep -E '^#?[[:space:]]*CONFIG_KASAN=' "$ksrc/.config" || true
)

printf 'UNAME=%s\n' "$(uname -a)"
printf 'VERSION=%s\n' "$(uname -r)"
printf 'VARIANT=%s\n' "$variant"
printf 'CMDLINE=%s\n' "$(tr '\n' ' ' </proc/cmdline)"
if [[ $kasan_config == CONFIG_KASAN=y ]]; then
	printf 'KASAN_ENABLED=true\n'
	printf 'KASAN_MODE=generic\n'
else
	printf 'KASAN_ENABLED=false\n'
	printf 'KASAN_MODE=none\n'
fi
if [[ -n $kasan_config ]]; then
	printf 'KASAN_CONFIG=%s\n' "$kasan_config"
else
	printf 'KASAN_CONFIG=# CONFIG_KASAN is not set\n'
fi
IDENTITY_SCRIPT
	cat "$variant_dir/kernel.info"
}

run_suite()
{
	local variant=$1
	local suite=$2
	local script=$3
	shift 3
	local variant_dir="$RESULT_BASE/$variant"
	local test_log="$variant_dir/$suite.test.log"
	local summary="$variant_dir/$suite.summary"
	local started_at
	local finished_at
	local status

	log "running $variant/$suite"
	started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	set +e
	guest_exec sudo -n env \
		"XDS_DEV_1=$GUEST_DEV_1" \
		"XDS_DEV_2=$GUEST_DEV_2" \
		"KSRC=$GUEST_KSRC" \
		"XDS_TEST_KEEP_WORKDIR=1" \
		"$@" \
		"$GUEST_REPO/test/$script" 2>&1 |
		tee "$test_log"
	status=${PIPESTATUS[0]}
	set -e
	finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

	{
		printf 'suite=%s/%s\n' "$variant" "$suite"
		printf 'result=%s\n' "$([[ $status -eq 0 ]] && echo PASS || echo FAIL)"
		printf 'test_exit_status=%s\n' "$status"
		printf 'started_at=%s\n' "$started_at"
		printf 'finished_at=%s\n' "$finished_at"
	} >"$summary"
	cat "$summary"
	return "$status"
}

copy_artifact_files()
{
	local source_dir=$1
	local destination_dir=$2
	local source
	local destination
	local copied=0

	while IFS= read -r -d '' source; do
		destination="$destination_dir/${source##*/}"
		if [[ -e $destination ]]; then
			printf 'duplicate artifact basename: %s\n' \
				"${source##*/}" >&2
			return 1
		fi
		cp "$source" "$destination"
		((copied += 1))
	done < <(
		find "$source_dir" -type f \
			\( -name '*.out' -o -name '*.dmesg' \) -print0
	)
	(( copied > 0 )) || {
		printf 'no reportable artifacts in %s\n' "$source_dir" >&2
		return 1
	}
}

download_guest_artifacts()
{
	local variant=$1
	local suite=$2
	local variant_dir="$RESULT_BASE/$variant"
	local test_log="$variant_dir/$suite.test.log"
	local archive="$variant_dir/$suite.artifacts.tar.gz"
	local extract_dir="$variant_dir/$suite-artifacts"
	local guest_artifacts

	guest_artifacts=$(
		sed -n 's/^XDS test artifacts: //p' "$test_log" |
			tail -n 1
	)
	if [[ ! $guest_artifacts =~ ^/tmp/xds-[[:alnum:]-]+\.[[:alnum:]]+$ ]]; then
		printf '%s/%s did not report a retained artifact path\n' \
			"$variant" "$suite" >&2
		return 1
	fi

	guest_exec sudo -n tar -C "${guest_artifacts%/*}" -czf - \
		"${guest_artifacts##*/}" >"$archive"
	mkdir -p "$extract_dir"
	tar -xzf "$archive" -C "$extract_dir"
	copy_artifact_files "$extract_dir" "$variant_dir"
}

run_variant()
{
	local variant=$1
	local mode

	boot_kernel "$variant"
	prepare_guest "$variant"
	save_kernel_identity "$variant"
	run_suite "$variant" basic basic_test.sh
	download_guest_artifacts "$variant" basic

	for mode in raid0 dm nvme; do
		run_suite "$variant" "stress-$mode" stress_test.sh \
			"XDS_STRESS_MODE=$mode" \
			"XDS_STRESS_ITERATIONS=$MATRIX_STRESS_ITERATIONS"
		download_guest_artifacts "$variant" "stress-$mode"
	done
}

generate_reports()
{
	python3 "$SCRIPT_DIR/report.py" \
		--artifact-dir "$RESULT_BASE" \
		--json-report "$RESULT_BASE/report.json" \
		--md-report "$RESULT_BASE/report.md" \
		--cases-tsv "$RESULT_BASE/cases.tsv"
}

main()
{
	printf 'matrix results: %s\n' "$RESULT_BASE"
	if [[ $BUILD_KERNELS == 1 ]]; then
		"$SCRIPT_DIR/build_guest_kernels.sh"
	elif [[ $BUILD_KERNELS != 0 ]]; then
		printf 'XDS_MATRIX_BUILD_KERNELS must be 0 or 1\n' >&2
		return 1
	fi

	sync_repo
	run_variant nokasan
	run_variant kasan
	generate_reports
	log "dual-kernel matrix passed: $RESULT_BASE"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
