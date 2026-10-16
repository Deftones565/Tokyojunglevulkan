#!/bin/bash
# harness_all.sh <outdir> [iters] [jobs]: run every function of harness_build.sh's
# image through the differential harness (new.so vs ref.so, same random state),
# 3 s cap each (a function that loops on a random counter just times out on
# both). Prints counts and every MISMATCH / crash.
OUT=${1:?outdir}; IT=${2:-200}; J=${3:-8}
grep -o '^void \w*(spu_context\* ctx) {' "$OUT/rc.c" | sed 's/^void \(\w*\)(.*/\1/' > "$OUT/funcs.txt"
run() {
    r=$(cd "$OUT" && timeout 3 ./harness "$1" "$IT" -1 ./new.so ./ref.so 2>&1 | tail -n 1); s=$?
    case "$r" in
        *identical) echo "OK $1";;
        *MISMATCH) echo "MISMATCH $1";;
        *) echo "OTHER($s) $1 :: $r";;
    esac
}
export -f run; export OUT IT
xargs -a "$OUT/funcs.txt" -P "$J" -I{} bash -c 'run {}' > "$OUT/results.txt"
cut -d' ' -f1 "$OUT/results.txt" | sed 's/(.*//' | sort | uniq -c
grep -v '^OK ' "$OUT/results.txt" | grep -v '^OTHER(' | head -40
