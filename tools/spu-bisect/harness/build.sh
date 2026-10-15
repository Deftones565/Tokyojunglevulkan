#!/bin/bash
# build.sh <image id> [outdir]: builds the differential harness for one lifted SPU
# image: <outdir>/new.so (pshufb spu_shufb), <outdir>/ref.so (byte-loop
# spu_shufb_ref everywhere), <outdir>/harness. Same flags as the game build of
# that image under PORT_SPU_EXTRA_FLAGS="-msse4.1;-ffp-contract=off".
# Run: <outdir>/harness <function symbol> [iters] [loop-counter gpr|-1] <outdir>/new.so <outdir>/ref.so
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
IMG=${1:?image id, e.g. j8D485835BE6E8738}; OUT=${2:-$ROOT/build-runs/spu-harness-$IMG}
PS3=${PS3RECOMP_DIR:-$ROOT/third_party/ps3recomp}; SPU=${SPU_DIR:-$ROOT/generated/spu}
mkdir -p "$OUT"
INC="-I$PS3/runtime/spu -I$PS3/include -I$PS3/runtime/ppu -I$PS3/runtime/memory -I$PS3/runtime/syscalls -I$PS3/runtime/platform -I$PS3/libs/video -I$ROOT/generated/ppu -I$ROOT/src"
CF="-O2 -std=gnu17 -fno-strict-aliasing -msse4.1 -ffp-contract=off -fPIC -shared $INC"
printf '#include "spu_helpers.h"\n#define spu_shufb spu_shufb_ref\n#include "%s/%s/spu_recomp.c"\n' "$SPU" "$IMG" > "$OUT/ref_wrap.c"
clang $CF "$SPU/$IMG/spu_recomp.c" -o "$OUT/new.so"
clang $CF -I"$SPU/$IMG" "$OUT/ref_wrap.c" -o "$OUT/ref.so"
clang -O1 $INC -rdynamic "$(dirname "$0")/harness.c" "$(dirname "$0")/stubs.c" -o "$OUT/harness" -ldl -Wl,--no-as-needed -lm
echo "built $OUT/{new.so,ref.so,harness}"
