#!/bin/bash
# usage: POSTER=<MAGI name> tools/prof/run_windowed.sh <binary> <run-name> [VAR=value ...]
# A scripted gameplay run in a real window, like the user's sessions (the
# headless runs never showed the 0103-0106 FPS drop): tools/play/fast_pad.sh
# to the Survival tutorial street, the user's build/settings.ini but windowed
# (not borderless), dummy audio, fresh run dir, RUN_S seconds (default 240).
# Per-second flip counts go to build-runs/<run>/flips.txt (TJ_PERF_FILE),
# TJ_TICK_STATS / GCM_WAIT_STATS lines to the log. Extra VAR=value arguments
# go into the game's environment (A/B toggles). A window opens on the desktop.
# SAVE=1 runs with a COPY of the user's save (gamedata/dev_hdd0 in the repo
# root), so the game has their progress; the original is never written.
# DUMP_EVERY=<n> dumps every nth frame to build-runs/<run>/frames (2.7 MB each).
# Output: build-runs/<run-name>.txt, build-runs/<run-name>/{flips.txt,settings.ini}.
set -e
R=/opt/git/tokyoJungleRecomp
bin=$(realpath "$1"); name=$2; shift 2
out=$R/build-runs/$name; log=$R/build-runs/$name.txt
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
rm -rf "$out"; mkdir -p "$out/cwd"
sed 's/^display_mode *=.*/display_mode = windowed/' $R/build/settings.ini > "$out/settings.ini"
$R/.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($bin, WINDOWED scripted, fast pad, fresh vfs, ${RUN_S:-240}s) $*" >/dev/null
if [ -n "$SAVE" ]; then mkdir -p "$out/cwd/gamedata"; cp -a $R/gamedata/dev_hdd0 "$out/cwd/gamedata/"; fi
dump=(); if [ -n "$DUMP_EVERY" ]; then mkdir -p "$out/frames"; dump=(TJ_FRAME_DUMP=$out/frames TJ_FRAME_DUMP_EVERY=$DUMP_EVERY TJ_FRAME_DUMP_MAX=400); fi
cd "$out/cwd"
env -u TJ_HEADLESS "${dump[@]}" WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} DISPLAY=${DISPLAY:-:1} \
  XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} \
  PS3_VERBOSE=0 SDL_AUDIODRIVER=dummy TJ_TICK_STATS=1 GCM_WAIT_STATS=1 TJ_RUN_DIR=$out/vfs \
  TJ_SETTINGS=$out/settings.ini TJ_PERF_FILE=$out/flips.txt \
  PAD_SCRIPT="$($R/tools/play/fast_pad.sh)" \
  "$@" timeout -s KILL ${RUN_S:-240} "$bin" $R/input/EBOOT.ELF > "$log" 2>&1 || true
cd $R; rm -rf "$out/cwd" "$out/vfs"
echo "$name: FATAL $(grep -ac FATAL $log); LIFT-GAP $(grep -ac LIFT-GAP $log); effect.efd $(grep -ac effect.efd $log)"
grep -a '\[tick-stats\]' $log | tail -1 || true
grep -a '\[gcm-wait\]' $log | tail -1 || true
# flips/s per 10 s from the telemetry, then the mean from 40 s (gameplay) on
awk 'NR==1{t0=$1} {t=$1-t0; f[NR]=$2; tt[NR]=t}
     END{for(i=1;i+10<=NR;i+=10) printf "%d:%.0f ", tt[i], (f[i+10]-f[i])/(tt[i+10]-tt[i]);
         print ""; for(i=1;i<=NR;i++) if(tt[i]>=40){a=i;break}
         if(a) printf "gameplay (t>=40s) %.2f flips/s\n", (f[NR]-f[a])/(tt[NR]-tt[a])}' "$out/flips.txt"
$R/.agents/talk say $POSTER "RUN done: $name" >/dev/null
