#!/usr/bin/env bash
# apply.sh: apply our local patches on top of the pinned submodules. Safe to rerun.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for p in "$root"/patches/ps3recomp/*.patch; do
	if git -C "$root/third_party/ps3recomp" apply --reverse --check "$p" 2>/dev/null; then
		echo "already applied: ${p##*/}"
	else
		git -C "$root/third_party/ps3recomp" apply "$p" && echo "applied: ${p##*/}"
	fi
done
