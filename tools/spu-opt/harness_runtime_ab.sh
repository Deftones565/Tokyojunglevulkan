#!/bin/bash
# harness_runtime_ab.sh <image id> <outdir> <new ps3recomp dir> [extra new CFLAGS...]:
# the same lifted image built against two runtime trees -- ref.so with
# third_party/ps3recomp, new.so with <new ps3recomp dir> (+ extra flags, e.g.
# -DSPU_LS_NO_PROBES) -- for tools/spu-opt/harness_all.sh. Checks helper
# changes (spu_helpers.h, spu_context.h) on the real lifted code.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${1:?image}; OUT=${2:?outdir}; NEWP=${3:?new ps3recomp}; shift 3
SPU=${SPU_DIR:-$ROOT/generated/spu}
mkdir -p "$OUT"; cp "$SPU/$IMG/spu_recomp.c" "$OUT/rc.c"
inc() { echo "-I$1/runtime/spu -I$1/include -I$1/runtime/ppu -I$1/runtime/memory -I$1/runtime/syscalls -I$1/runtime/platform -I$1/libs/video -I$ROOT/generated/ppu -I$ROOT/src -I$SPU/$IMG"; }
CF="-O2 -std=gnu17 -fno-strict-aliasing -msse4.1 -mfma -ffp-contract=off -fPIC -shared"
clang $CF $(inc "$ROOT/third_party/ps3recomp") "$SPU/$IMG/spu_recomp.c" -o "$OUT/ref.so"
clang $CF "$@" $(inc "$NEWP") "$SPU/$IMG/spu_recomp.c" -o "$OUT/new.so"
clang -O1 $(inc "$ROOT/third_party/ps3recomp") -rdynamic "$ROOT/tools/spu-bisect/harness/harness.c" "$ROOT/tools/spu-bisect/harness/stubs.c" -o "$OUT/harness" -ldl -Wl,--no-as-needed -lm
echo "built $OUT"
