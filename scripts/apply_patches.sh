#!/bin/bash
# Apply this project's fixes (patches/ps3recomp/*.patch, in order) to the
# ps3recomp checkout. Run once after cloning / `git submodule update`, and
# again after pulling new patches:
#
#   scripts/apply_patches.sh                 # third_party/ps3recomp
#   scripts/apply_patches.sh <ps3recomp dir> # any other checkout
#
# What has been applied is recorded in <dir>/.tj-applied-patches, so each
# patch is applied once. A checkout with local changes but no record is
# refused (it may be patched already). patches/pending/ is NOT applied: those
# aren't verified yet.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dst=${1:-$root/third_party/ps3recomp}
rec="$dst/.tj-applied-patches"
cd "$dst"
if [ ! -f "$rec" ] && ! git diff --quiet HEAD -- . ':!.tj-applied-patches'; then
    echo "$dst has local changes but no $rec record: refusing (already patched?)." >&2
    echo "Reset it to the pinned commit first (git -C $dst checkout -- . && git -C $dst clean -fd)." >&2
    exit 1
fi
touch "$rec"
applied=0
for p in "$root"/patches/ps3recomp/[0-9]*.patch; do
    name=$(basename "$p")
    grep -qxF "$name" "$rec" && continue
    if ! git apply --check "$p" 2>/dev/null; then
        echo "FAILED   $name does not apply; stopping (earlier ones are recorded in $rec)" >&2
        exit 1
    fi
    git apply "$p"; echo "$name" >> "$rec"; echo "applied  $name"; applied=$((applied + 1))
done
echo "ps3recomp patches: $applied newly applied, $(wc -l < "$rec") total ($dst)"
