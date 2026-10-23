#!/bin/bash
# usage: tools/ab/run_display_mode.sh <build-dir> <run-name> [modes]
# WINDOWED run (env SETTINGS=<settings.ini> to use another file) with the 11-press script into gameplay, the FPS overlay's value logged once a
# second ([fps] lines, TJ_FPS_LOG), and the display mode switched on a timer (TJ_DISPLAY_MODE_AT,
# default "130:0,150:1,170:2,190:1": windowed, borderless, exclusive, borderless). 210 s.
cd "$(dirname "$0")/../.."
B=$1; name=$2; modes=${3:-130:0,150:1,170:2,190:1}; log=build-runs/$name.txt
if [ "$(pgrep -x tokyojungle | wc -l)" -ge 2 ]; then echo "2 slots busy"; exit 2; fi
vfs=$B/vfs-$name; rm -rf "$vfs"
set_env=; [ -n "$SETTINGS" ] && set_env="TJ_SETTINGS=$SETTINGS"   # SETTINGS=<ini> instead of the build's own
env -u TJ_HEADLESS WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} DISPLAY=${DISPLAY:-:1} \
  XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} SDL_AUDIODRIVER=dummy \
  PS3_VERBOSE=0 TJ_TICK_STATS=1 TJ_RUN_DIR=$PWD/$vfs TJ_FPS_LOG=1 TJ_DISPLAY_MODE_AT=$modes $set_env \
  PAD_SCRIPT="30:0x4000,36:0x4000,42:0x4000,48:0x4000,54:0x4000,60:0x4000,66:0x4000,72:0x4000,100:0x4000,105:0x4000,110:0x4000" \
  timeout -s KILL 210 ./$B/tokyojungle gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > $log 2>&1
grep -n '\[settings\] display\|window swapchain\|\[fps\]' $log | awk -F: '{printf "%s ", $2} END{print ""}' | fold -w 200
