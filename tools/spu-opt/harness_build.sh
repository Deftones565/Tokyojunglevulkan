#!/bin/bash
# harness_build.sh <image id> <outdir>: the differential harness for regcache.py.
# <outdir>/ref.so = the image as lifted, <outdir>/new.so = regcache.py's rewrite,
# both with the game's SPU flags (-O2 -msse4.1 -mfma -ffp-contract=off), and
# <outdir>/harness (tools/spu-bisect/harness). Then tools/spu-opt/harness_all.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${1:?image id}; OUT=${2:?outdir}
PS3=${PS3RECOMP_DIR:-$ROOT/third_party/ps3recomp}; SPU=${SPU_DIR:-$ROOT/generated/spu}
mkdir -p "$OUT"
python3 -I "$ROOT/tools/spu-opt/regcache.py" "$SPU/$IMG/spu_recomp.c" "$OUT/rc.c" --stats
INC="-I$PS3/runtime/spu -I$PS3/include -I$PS3/runtime/ppu -I$PS3/runtime/memory -I$PS3/runtime/syscalls -I$PS3/runtime/platform -I$PS3/libs/video -I$ROOT/generated/ppu -I$ROOT/src -I$SPU/$IMG"
CF="-O2 -std=gnu17 -fno-strict-aliasing -msse4.1 -mfma -ffp-contract=off -fPIC -shared $INC"
clang $CF "$SPU/$IMG/spu_recomp.c" -o "$OUT/ref.so"
clang $CF "$OUT/rc.c" -o "$OUT/new.so"
clang -O1 $INC -rdynamic "$ROOT/tools/spu-bisect/harness/harness.c" "$ROOT/tools/spu-bisect/harness/stubs.c" -o "$OUT/harness" -ldl -Wl,--no-as-needed -lm
echo "built $OUT/{ref.so,new.so,harness}"
