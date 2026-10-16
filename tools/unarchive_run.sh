#!/bin/bash
# usage: tools/unarchive_run.sh <run> [...]
# Restores build-runs/<run>/ and build-runs/<run>.* from build-runs/archive/<run>.tar.zst
# (made by tools/archive_runs.sh), mtimes included. The archive is kept; run
# tools/archive_runs.sh <run> after looking (it skips while the archive exists,
# so delete the restored files or the archive first).
set -euo pipefail
B=$(cd "$(dirname "$0")/.." && pwd)/build-runs
for n in "$@"; do
    zstd -dc --long=27 "$B/archive/$n.tar.zst" | tar -xf - -C "$B"
    echo "restored $n -> build-runs/"
done
