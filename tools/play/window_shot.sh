#!/bin/bash
# usage: tools/play/window_shot.sh <build-dir> <run-name> <seconds> <shot-times...>
#   env: SETTINGS=<settings.ini to use>,
#        any other TJ_* / PS3RECOMP_* variables pass through (TJ_MENU_OPEN_AFTER_S,
#        TJ_MENU_SELECT, PAD_SCRIPT, ...).
# Runs the game WINDOWED (SDL on XWayland, so the window can be captured with
# ImageMagick's import) from a scratch CWD and a fresh VFS, and saves a PNG of
# the window at each shot time (seconds after start) to
# build-runs/<run>/shot_<t>.png. The window appears on the user's desktop for
# the length of the run.
R="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$R"
B=$1; name=$2; secs=$3; shift 3
d=$PWD/build-runs/$name; mkdir -p $d/cwd
if [ "$(pgrep -x tokyojungle | wc -l)" -ge ${SLOTS:-2} ]; then echo "2 slots busy"; exit 2; fi
vfs=$PWD/$B/vfs-$name; rm -rf "$vfs"
[ -n "$SETTINGS" ] && cp "$SETTINGS" $d/settings.ini
title="tj-shot-$name"
( cd $d/cwd && SDL_VIDEODRIVER=x11 PS3_TITLE=$title PS3_VERBOSE=0 SDL_AUDIODRIVER=dummy \
    TJ_RUN_DIR=$vfs TJ_SETTINGS=$d/settings.ini \
    timeout -s KILL $secs $R/$B/tokyojungle \
        $R/gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF > $d/log.txt 2>&1 ) &
pid=$!
t0=$(date +%s)
for t in "$@"; do
    while [ $(( $(date +%s) - t0 )) -lt $t ]; do sleep 0.5; done
    wid=$(xwininfo -name "$title" 2>/dev/null | awk '/Window id/{print $4}')
    if [ -n "$wid" ]; then import -window "$wid" $d/shot_$t.png && echo "shot_$t.png"
    else echo "no window at $t s"; fi
done
wait $pid
echo "$name: FATAL $(grep -c FATAL $d/log.txt); LIFT-GAP $(grep -c LIFT-GAP $d/log.txt)"
