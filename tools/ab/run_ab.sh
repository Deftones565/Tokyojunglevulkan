#!/bin/bash
# usage: tools/ab/run_ab.sh <build-dir> <run-name>   (env: POSTER=<MAGI name>, VFS=<fresh run dir>)
# One A/B side: STATUS.md's 11-press script, dumps every 10th present after 1050 dumps (~flip 5250, max 200), 240 s.
# Run the A and B sides with the same script and fresh run dirs, then compare frames by flip.
cd /opt/git/tokyoJungleRecomp
B=$1; name=$2; log=build-runs/$name.txt; fd=build-runs/$name/frames; mkdir -p $fd
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
vfs=${VFS:-$B/vfs-$name}; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($B, A/B side, 11-press, fresh vfs, dumps every 10 after 1050)" >/dev/null
PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_TICK_STATS=1 TJ_RUN_DIR=$PWD/$vfs \
  TJ_FRAME_DUMP=$PWD/$fd TJ_FRAME_DUMP_EVERY=10 TJ_FRAME_DUMP_SKIP=1050 TJ_FRAME_DUMP_MAX=200 \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000" \
  timeout -s KILL 240 ./$B/tokyojungle input/EBOOT.ELF > $log 2>&1
echo "$name: FATAL $(grep -c FATAL $log); effect.efd $(grep -c effect.efd $log); frames $(ls $fd | wc -l)"
grep tick-stats $log | tail -1
.agents/talk say $POSTER "RUN done: $name" >/dev/null
