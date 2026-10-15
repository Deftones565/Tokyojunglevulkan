#!/bin/bash
# usage: tools/play/session.sh <build-dir> <run-name> [seconds]   (env: POSTER=<MAGI name>)
# Interactive (closed-loop) play: starts the game headless with PAD_FILE so a
# driver can press buttons by writing a mask to build-runs/<run>/pad.txt
# (tools/play/press.sh), and dumps every 30th present to build-runs/<run>/frames
# so the driver can see where the game is (tools/play/latest.sh).
# PS3_STRICT_LIFT=1: the first lift gap ends the run with its address.
cd /opt/git/tokyoJungleRecomp
B=$1; name=$2; secs=${3:-900}; d=build-runs/$name; mkdir -p $d/frames; : > $d/pad.txt
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
vfs=$B/vfs-$name; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($B, interactive PAD_FILE play, STRICT_LIFT, up to ${secs}s)" >/dev/null
PS3_STRICT_LIFT=1 PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_TICK_STATS=1 TJ_RUN_DIR=$PWD/$vfs \
  PAD_FILE=$PWD/$d/pad.txt TJ_FRAME_DUMP=$PWD/$d/frames TJ_FRAME_DUMP_EVERY=30 TJ_FRAME_DUMP_MAX=100000 \
  timeout -s KILL $secs ./$B/tokyojungle input/EBOOT.ELF > $d/log.txt 2>&1
echo "$name: exit $?; FATAL $(grep -c FATAL $d/log.txt); LIFT-GAP $(grep -c LIFT-GAP $d/log.txt); frames $(ls $d/frames | wc -l)"
.agents/talk say $POSTER "RUN done: $name" >/dev/null
