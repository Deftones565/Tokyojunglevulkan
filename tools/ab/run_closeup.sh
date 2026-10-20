#!/bin/bash
# usage: tools/ab/run_closeup.sh <build-dir> <run-name>   (env: POSTER=<board name>, extra env passes through)
# One A/B side for the character close-up: STATUS.md's 11-press script, the Pomeranian's
# model on the Survival animal select (flips ~3260-3540), every 6th present dumped, 140 s. Compare two sides with
# tools/ab/cmp.py or look at them with tools/ab/sheet.py.
cd /opt/git/tokyoJungleRecomp
B=$1; name=$2; log=build-runs/$name.txt; fd=build-runs/$name/frames; mkdir -p $fd
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
vfs=$B/vfs-$name; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($B, close-up A/B side)" >/dev/null
PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_RUN_DIR=$PWD/$vfs \
  TJ_FRAME_DUMP=$PWD/$fd TJ_FRAME_DUMP_EVERY=6 TJ_FRAME_DUMP_SKIP=1087 TJ_FRAME_DUMP_MAX=93 \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000" \
  timeout -s KILL 140 ./$B/tokyojungle gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > $log 2>&1
rm -rf "$vfs"
echo "$name: FATAL $(grep -c FATAL $log); frames $(ls $fd | wc -l) ($(ls $fd | head -1) .. $(ls $fd | tail -1))"
.agents/talk say $POSTER "RUN done: $name" >/dev/null
