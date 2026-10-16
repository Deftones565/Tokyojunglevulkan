#!/bin/bash
# usage: POSTER=<MAGI name> tools/prof/run_gameplay.sh <binary> <run-name> [VAR=value ...]
# One gameplay perf run: STATUS.md's 11-press script (reaches the Survival
# tutorial street at ~flip 4100, ~70 s), headless, from a scratch CWD with a
# fresh run dir, 240 s. Frame dumps every 15 flips give flips/s over time
# (tools/prof/fps_timeline.py); a wall-clock profile (tools/prof/sigprof.c,
# PROF_WALL_HZ=200) covers t=160-230 s, which is gameplay. Extra VAR=value
# arguments go into the game's environment (A/B toggles).
# Output: build-runs/<run-name>.txt, build-runs/<run-name>/{frames,prof.bin}.
set -e
R=/opt/git/tokyoJungleRecomp
bin=$(realpath "$1"); name=$2; shift 2
out=$R/build-runs/$name; log=$R/build-runs/$name.txt
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
[ -f $R/build-runs/sigprof.so ] || cc -O2 -shared -fPIC -o $R/build-runs/sigprof.so $R/tools/prof/sigprof.c -lpthread
rm -rf "$out"; mkdir -p "$out/frames" "$out/cwd"
$R/.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($bin, gameplay perf, 11-press, fresh vfs, 240s) $*" >/dev/null
cd "$out/cwd"
env PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_TICK_STATS=1 GCM_WAIT_STATS=1 TJ_RUN_DIR=$out/vfs \
  TJ_FRAME_DUMP=$out/frames TJ_FRAME_DUMP_EVERY=15 TJ_FRAME_DUMP_MAX=2000 \
  PROF_OUT=$out/prof.bin PROF_WALL_HZ=200 PROF_START_S=160 PROF_END_S=230 LD_PRELOAD=$R/build-runs/sigprof.so \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000" \
  "$@" timeout -s KILL 240 "$bin" $R/input/EBOOT.ELF > "$log" 2>&1 || true
cd $R; rm -rf "$out/cwd" "$out/vfs"
echo "$name: FATAL $(grep -c FATAL $log); LIFT-GAP $(grep -c LIFT-GAP $log); effect.efd $(grep -c effect.efd $log)"
grep '\[gcm-wait\]' $log | tail -2 || true
python3 $R/tools/prof/fps_timeline.py "$out/frames"
$R/.agents/talk say $POSTER "RUN done: $name" >/dev/null
