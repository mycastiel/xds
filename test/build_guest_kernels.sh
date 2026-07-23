#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
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
GUEST_KSRC=${XDS_GUEST_KSRC:?set XDS_GUEST_KSRC in "$CONFIG_FILE"}
BUILD_JOBS=${XDS_TEST_BUILD_JOBS:-16}

readonly REMOTE_TARGET=$REMOTE_USER@$REMOTE_HOST
readonly -a REMOTE_SSH=(
	ssh
	-p "$REMOTE_PORT"
	-o BatchMode=yes
	-o ConnectTimeout=20
	-o StrictHostKeyChecking=accept-new
	"$REMOTE_TARGET"
)

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

build_kernels()
{
	guest_exec bash -s -- "$GUEST_KSRC" "$BUILD_JOBS" <<'BUILD_SCRIPT'
set -Eeuo pipefail

ksrc=$1
build_jobs=$2
base_config=$(mktemp)
trap 'rm -f "$base_config"' EXIT

cd "$ksrc"
cp .config "$base_config"

for variant in nokasan kasan; do
	cp "$base_config" .config
	./scripts/config --set-str LOCALVERSION "-xds-$variant"
	if [[ $variant == kasan ]]; then
		./scripts/config --set-val CONFIG_KASAN y
		./scripts/config --set-val CONFIG_KASAN_GENERIC y
	else
		./scripts/config --set-val CONFIG_KASAN n
	fi
	make olddefconfig
	printf 'building %s with:\n' "$variant"
	grep -E '^(CONFIG_LOCALVERSION|CONFIG_KASAN)=' .config || true
	make clean
	make -j"$build_jobs"
	sudo make modules_install
	sudo make install
	kernel_release=$(make -s kernelrelease)
	sudo cp .config "/boot/config-$kernel_release"
done
BUILD_SCRIPT
}

main()
{
	[[ $BUILD_JOBS =~ ^[1-9][0-9]*$ ]] || {
		printf 'XDS_TEST_BUILD_JOBS must be positive\n' >&2
		return 1
	}
	build_kernels
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	main "$@"
fi
