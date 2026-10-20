#!/bin/bash
# usage: tools/play/user_session.sh <run-name>   (env: POSTER=<MAGI name>)
# The user's own play session: build/tokyojungle WINDOWED on the desktop with
# real audio and the user's controller (SDL game controller), its own fresh
# run dir, log to build-runs/<run>.txt, a frame dumped every 300 presents to
# build-runs/<run>/frames (pictures of where it broke). PS3_STRICT_LIFT is
# NOT set: a lift gap is logged, the game keeps running. No time limit.
cd /opt/git/tokyoJungleRecomp
name=$1; d=build-runs/$name; mkdir -p $d/frames
vfs=${B:-build}/vfs-$name; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name (${B:-build}, USER PLAYING windowed, holds one slot while they play)" >/dev/null
env -u TJ_HEADLESS -u PAD_SCRIPT -u PAD_FILE -u PS3_STRICT_LIFT -u SDL_AUDIODRIVER \
  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} DISPLAY=${DISPLAY:-:1} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} \
  PS3_VERBOSE=0 TJ_TICK_STATS=1 TJ_RUN_DIR=$PWD/$vfs \
  TJ_FRAME_DUMP=$PWD/$d/frames TJ_FRAME_DUMP_EVERY=${DUMP_EVERY:-300} SGSDIAG_OUT=$PWD/$d/sgsdiag TJ_FRAME_DUMP_MAX=100000 \
  ./${B:-build}/tokyojungle gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > $d.txt 2>&1
rc=$?
echo "$name: exit $rc; FATAL $(grep -c FATAL $d.txt); LIFT-GAP $(grep -c LIFT-GAP $d.txt); frames $(ls $d/frames | wc -l)"
.agents/talk say $POSTER "RUN done: $name (exit $rc)" >/dev/null
