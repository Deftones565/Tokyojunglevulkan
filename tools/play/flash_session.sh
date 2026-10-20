#!/bin/bash
# usage: tools/play/flash_session.sh <run-name> [binary]   (env: POSTER=<MAGI name>)
# The user's own play session for the flashing / hang hunt: windowed with real
# audio and controller and the user's save (like tools/play/user_session.sh),
# with RSX_FLASH_DETECT=1 (each flash logs what the flash frame and its
# neighbours drew where) and the game started UNDER gdb, so that a hang can be
# inspected without attaching (kernel.yama.ptrace_scope=1 allows only a
# parent). gdb passes every signal through untouched except SIGUSR2: when the
# flip count stops for 5 s, the watcher below sends SIGUSR2, gdb writes every
# thread's backtrace to build-runs/<run>/hang-bt.txt and lets the game go on.
# Per-second FPS: build-runs/<run>/monitor.txt (tools/play/monitor.sh).
#   grep -a '\[flash\]' build-runs/<run>.txt
cd /opt/git/tokyoJungleRecomp
name=$1; bin=${2:-build-magi7/b/tokyojungle}; d=build-runs/$name
mkdir -p $d
bdir=$(dirname "$bin")
[ -f "$bdir/settings.ini" ] || cp build/settings.ini "$bdir/settings.ini"
vfs=$bdir/vfs-$name; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($bin under gdb, USER PLAYING windowed, flash detector)" >/dev/null

tools/play/monitor.sh $name &
( # hang watcher: one capture per stall
  while [ -z "$(pgrep -x tokyojungle)" ]; do sleep 1; done
  P=$(pgrep -x tokyojungle | head -1); last=-1; same=0; fired=0
  while kill -0 $P 2>/dev/null; do
      sleep 1
      f=$(tail -1 $d/flips.txt 2>/dev/null | awk '{print $2}')
      if [ -n "$f" ] && [ "$f" = "$last" ]; then same=$((same + 1)); else same=0; fired=0; fi
      last=$f
      if [ $same -ge 5 ] && [ $fired = 0 ]; then
          fired=1; echo "# stall at flip $f, $(date +%T)" >> $d/hang-bt.txt
          tail -400 $d.txt > $d/hang-log-tail.txt
          kill -USR2 $P
      fi
  done ) &

env -u TJ_HEADLESS -u PAD_SCRIPT -u PAD_FILE -u PS3_STRICT_LIFT -u SDL_AUDIODRIVER \
  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} DISPLAY=${DISPLAY:-:1} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} \
  PS3_VERBOSE=0 TJ_TICK_STATS=1 GCM_WAIT_STATS=1 TJ_RUN_DIR=$PWD/$vfs TJ_PERF_FILE=$PWD/$d/flips.txt \
  RSX_FLASH_DETECT=1 \
  gdb -q -batch -nx \
    -ex "set pagination off" -ex "set confirm off" \
    -ex "set print thread-events off" -ex "set print inferior-events off" \
    -ex "handle all nostop noprint pass" \
    -ex "handle SIGUSR2 stop print nopass" \
    -ex "set logging file $PWD/$d/hang-bt.txt" -ex "set logging overwrite off" \
    -ex "run" \
    -ex "set logging enabled on" -ex "info threads" -ex "thread apply all bt 30" \
    -ex "set logging enabled off" \
    -ex "continue" \
    --args ./$bin input/EBOOT.ELF > $d.txt 2>&1
echo "$name: FATAL $(grep -ac FATAL $d.txt); LIFT-GAP $(grep -ac LIFT-GAP $d.txt); flashes $(grep -ac '\[flash\] frame' $d.txt)"
.agents/talk say $POSTER "RUN done: $name" >/dev/null
wait
