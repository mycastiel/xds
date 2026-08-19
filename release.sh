#!/bin/bash
set -euo pipefail

ARCHIVE=nds.tar.gz
REF=origin/release_1

# Fetch the latest ref from remote (no-op if already up to date)
git fetch origin release_1 >/dev/null 2>&1 || true

# Build the list of files to include: everything in $REF except
# plan/, design/, and the release.sh script itself.
mapfile -t FILES < <(git ls-tree -r --name-only --full-tree "$REF" \
	| grep -vE '^(plan|design)/' \
	| grep -vE '^release\.sh$')

if [ ${#FILES[@]} -eq 0 ]; then
	echo "error: no files found in $REF" >&2
	exit 1
fi

# Produce nds.tar.gz with all selected files under an nds/ prefix.
git archive --format=tar --prefix=nds/ "$REF" "${FILES[@]}" \
	| gzip -n > "$ARCHIVE"

echo "Created $ARCHIVE from $REF (${#FILES[@]} files)"
