#!/bin/bash
# usage: tools/archive_runs.sh [run ...]      (default: every finished run in build-runs/)
#
# Packs each run's output -- build-runs/<run>/ (frame dumps etc.) and its loose
# files build-runs/<run>.* (logs) -- into ONE file, build-runs/archive/<run>.tar.zst,
# checks the archive (zstd -t, member count), then removes the originals.
# tools/unarchive_run.sh <run> puts a run back exactly as it was (mtimes kept:
# fps.py / fps_timeline.py read frame mtimes).
#
# Never touched: build-runs/archive/, directories holding code (.py .c .cpp .h
# .sh .patch .md: atlas-ref, handoffs, harnesses -- the docs link to them),
# loose code files (.diff .patch .so .py), a run with anything modified in the
# last MIN_AGE_MIN minutes (default 30), and a run a live process has open.
# (Project rule, user 2026-10-16: compress huge logs; delete stale ones.)
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd); B=$R/build-runs; A=$B/archive
MIN_AGE_MIN=${MIN_AGE_MIN:-30}
mkdir -p "$A"; cd "$B" || exit 1

is_code_dir() {
    [ -n "$(find "$1" -type f \( -name '*.py' -o -name '*.c' -o -name '*.cpp' -o -name '*.h' \
          -o -name '*.sh' -o -name '*.patch' -o -name '*.md' \) -print -quit)" ]
}
open_by_process() {   # any process with a cwd or fd inside build-runs/<run>
    local n=$1 p
    for p in /proc/[0-9]*; do
        { readlink "$p/cwd"; ls -l "$p/fd" 2>/dev/null; } 2>/dev/null | grep -q "$B/$n\([./]\|$\)" && return 0
    done
    return 1
}

if [ $# -gt 0 ]; then runs=("$@"); else
    mapfile -t runs < <(for e in *; do [ "$e" = archive ] && continue; echo "${e%%.*}"; done | sort -u)
fi

done_n=0; skip_n=0
for n in "${runs[@]}"; do
    members=()
    [ -d "$n" ] && members+=("$n")
    for f in "$n".*; do [ -f "$f" ] && members+=("$f"); done
    [ ${#members[@]} -eq 0 ] && continue
    why=""
    [ -d "$n" ] && is_code_dir "$n" && why="holds code"
    for m in "${members[@]}"; do
        case "$m" in *.diff|*.patch|*.so|*.py) why="code file $m";; esac
    done
    [ -z "$why" ] && [ -n "$(find "${members[@]}" -mmin -"$MIN_AGE_MIN" -print -quit)" ] && why="modified in the last ${MIN_AGE_MIN} min"
    [ -z "$why" ] && open_by_process "$n" && why="open by a running process"
    [ -z "$why" ] && [ -e "$A/$n.tar.zst" ] && why="archive/$n.tar.zst already exists"
    if [ -n "$why" ]; then echo "skip     $n ($why)"; skip_n=$((skip_n + 1)); continue; fi

    tmp="$A/.$n.tar.zst.part"
    if ! tar -cf - "${members[@]}" | nice -n 19 zstd -q -T0 -6 --long=27 -o "$tmp"; then
        echo "FAILED   $n (tar/zstd); originals kept" >&2; rm -f "$tmp"; continue
    fi
    want=$(find "${members[@]}" | wc -l)
    got=$(zstd -dc --long=27 "$tmp" | tar -tf - | wc -l)
    if ! zstd -q -t --long=27 "$tmp" || [ "$want" != "$got" ]; then
        echo "FAILED   $n (verify: $got of $want members); originals kept" >&2; rm -f "$tmp"; continue
    fi
    mv "$tmp" "$A/$n.tar.zst"
    before=$(du -sc "${members[@]}" | tail -1 | cut -f1)
    rm -rf -- "${members[@]}"
    printf 'archived %s  %s -> %s\n' "$n" "$(numfmt --to=iec $((before * 1024)))" \
        "$(du -h "$A/$n.tar.zst" | cut -f1)"
    done_n=$((done_n + 1))
done
echo "archive_runs: $done_n archived, $skip_n skipped; $(du -sh "$A" | cut -f1) in build-runs/archive"
