#!/bin/bash
# usage: tools/spu-bisect/shard_run.sh <build-dir> <run-name> [VAR=value ...]
# One headless 32-press run with every new guest flip dumped from flip 5500 on (needs a build with
# the PS3RECOMP_VK_DUMP_FROM_FLIP diag), then the one-frame glitch count from shards.py.
# Deletes the dumps afterwards except the flagged frames and their neighbours.
cd "$(dirname "$0")/../.."
B=$1; name=$2; shift 2
if [ "$(pgrep -x tokyojungle | wc -l)" -ge ${SLOTS:-2} ]; then echo "slots busy"; exit 2; fi
fd=build-runs/$name/frames; vfs=$B/vfs-$name; rm -rf $vfs $fd; cp -a build/vfs $vfs; mkdir -p $fd
env "$@" PS3RECOMP_VK_DUMP_FROM_FLIP=5500 TJ_FRAME_DUMP=$PWD/$fd TJ_FRAME_DUMP_EVERY=1 TJ_FRAME_DUMP_MAX=4000 \
  PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_RUN_DIR=$PWD/$vfs \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000,115:0x4000,120:0x4000,125:0x4000,130:0x4000,135:0x4000,140:0x4000,145:0x4000,150:0x4000,155:0x4000,160:0x4000,165:0x4000,170:0x4000,175:0x4000,180:0x4000,185:0x4000,190:0x4000,195:0x4000,200:0x4000,205:0x4000,210:0x4000,215:0x4000" \
  timeout -s KILL 250 ./$B/tokyojungle gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > build-runs/$name.txt 2>&1
echo "$name: FATAL $(grep -c FATAL build-runs/$name.txt), sync cap hits $(grep -c 'still busy' build-runs/$name.txt), frames $(ls $fd | wc -l)"
python3 tools/spu-bisect/shards.py $fd --thresh 0.45 --top 20 | tee build-runs/$name/shards.txt
python3 - "$fd" <<'PY'
import os, re, sys
d = sys.argv[1]
fs = sorted((f for f in os.listdir(d) if re.search(r'flip(\d+)', f)), key=lambda f: int(re.search(r'flip(\d+)', f).group(1)))
keep = set()
for line in open(os.path.join(os.path.dirname(d), 'shards.txt')):
    m = re.search(r'flip\s+(\d+)', line)
    if m: keep.add(int(m.group(1)))
flips = [int(re.search(r'flip(\d+)', f).group(1)) for f in fs]
for i, f in enumerate(fs):
    if not any(abs(flips[i] - k) <= 1 for k in keep): os.unlink(os.path.join(d, f))
PY
