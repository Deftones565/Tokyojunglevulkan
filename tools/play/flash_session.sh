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
# GDB=0 runs without gdb. PROF=1 (implies GDB=0: gdb would relay every
# profiler signal) adds the wall-clock profiler (tools/prof/sigprof.c, 200 Hz,
# all threads) gated to the SLOW seconds only: the watcher opens the gate
# while the last second ran under ${SLOW_FPS:-45} FPS. Report:
#   python3 tools/prof/report.py build-runs/<run>/prof-slow.bin <binary>
#   grep -a '\[flash\]' build-runs/<run>.txt
cd /opt/git/tokyoJungleRecomp
name=$1; bin=${2:-build-magi7/b/tokyojungle}; d=build-runs/$name
mkdir -p $d
bdir=$(dirname "$bin")
[ -f "$bdir/settings.ini" ] || cp build/settings.ini "$bdir/settings.ini"
vfs=$bdir/vfs-$name; rm -rf "$vfs"
.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($bin, gdb=${GDB:-1} prof=${PROF:-0}, USER PLAYING windowed, flash detector)" >/dev/null

[ -n "$PROF" ] && GDB=0
gate=$PWD/$d/prof.gate; rm -f $gate
tools/play/monitor.sh $name &
if [ -n "$PROF" ]; then
  [ -f build-runs/sigprof.so ] || cc -O2 -shared -fPIC -o build-runs/sigprof.so tools/prof/sigprof.c -lpthread
  ( # profiler gate: open while the game runs slow
    while [ -z "$(pgrep -x tokyojungle)" ]; do sleep 1; done
    P=$(pgrep -x tokyojungle | head -1); prev=0
    while kill -0 $P 2>/dev/null; do
        sleep 1
        f=$(tail -1 $d/flips.txt 2>/dev/null | awk '{print $2}'); f=${f:-0}
        fps=$((f - prev)); prev=$f
        if [ $f -gt 3000 ] && [ $fps -gt 0 ] && [ $fps -lt ${SLOW_FPS:-45} ]; then touch $gate; else rm -f $gate; fi
    done; rm -f $gate ) &
  profenv=(LD_PRELOAD=$PWD/build-runs/sigprof.so PROF_OUT=$PWD/$d/prof-slow.bin PROF_WALL_HZ=200
           PROF_GATE=$gate PROF_START_S=0 PROF_END_S=100000)
fi
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
          [ "$GDB" != 0 ] && kill -USR2 $P   # without gdb SIGUSR2 would end the game
      fi
  done ) &

# PASSDUMP=1: on the first detected flash, dump the next ${PASSDUMP_MAX:-300}
# passes (each pass's targets as images, passes.txt, draws.txt) to
# build-runs/<run>/passes (RSX_ENGINE_PASS_DUMP). ~2.7 MB per pass image.
pdenv=()
if [ -n "$PASSDUMP" ]; then
  mkdir -p $d/passes; rm -f $d/flash.gate
  pdenv=(RSX_FLASH_GATE=$PWD/$d/flash.gate RSX_ENGINE_PASS_DUMP=$PWD/$d/passes
         RSX_ENGINE_PASS_DUMP_GATE=$PWD/$d/flash.gate RSX_ENGINE_PASS_DUMP_MAX=${PASSDUMP_MAX:-300}
         RSX_ENGINE_PASS_DUMP_DRAWS=1)
fi
gdbargs=()
if [ "$GDB" != 0 ]; then
  gdbargs=(gdb -q -batch -nx
    -ex "set pagination off" -ex "set confirm off"
    -ex "set print thread-events off" -ex "set print inferior-events off"
    -ex "handle all nostop noprint pass"
    -ex "handle SIGUSR2 stop print nopass"
    -ex "set logging file $PWD/$d/hang-bt.txt" -ex "set logging overwrite off"
    -ex "run"
    -ex "set logging enabled on" -ex "info threads" -ex "thread apply all bt 30"
    -ex "set logging enabled off"
    -ex "continue"
    --args)
fi
env -u TJ_HEADLESS -u PAD_SCRIPT -u PAD_FILE -u PS3_STRICT_LIFT -u SDL_AUDIODRIVER \
  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} DISPLAY=${DISPLAY:-:1} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} \
  PS3_VERBOSE=0 TJ_TICK_STATS=1 GCM_WAIT_STATS=1 TJ_RUN_DIR=$PWD/$vfs TJ_PERF_FILE=$PWD/$d/flips.txt \
  RSX_FLASH_DETECT=1 "${profenv[@]}" "${pdenv[@]}" \
  "${gdbargs[@]}" ./$bin gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > $d.txt 2>&1
echo "$name: FATAL $(grep -ac FATAL $d.txt); LIFT-GAP $(grep -ac LIFT-GAP $d.txt); flashes $(grep -ac '\[flash\] frame' $d.txt)"
.agents/talk say $POSTER "RUN done: $name" >/dev/null
wait
