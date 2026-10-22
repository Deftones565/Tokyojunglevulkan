#!/bin/bash
# usage: POSTER=<MAGI name> tools/play/map_check.sh <binary> <run-name> [VAR=value ...]
# Opens the Survival pause-menu map headless and keeps a picture of it, to
# check the area tags (2026-10-22: the nine surface tags were blank). A fresh
# save has no Survival unlocked past the tutorial, whose prompts a fixed
# script cannot get through, so this runs from a COPY of the user's save
# folder (<data root>/saves/tokyojungle, used as the game's cwd, as the
# dashboard does: saves are ./gamedata/...); the real save is never written.
# Driven by PAD_FILE on a fixed schedule: START through the boot screens, CROSS
# on SURVIVAL / NEW GAME / SINGLE PLAYER / Pomeranian, wait for the street,
# START (pause menu with the map), TRIANGLE (zoom out: all nine surface tags),
# SQUARE (underground layer). Frames: build-runs/<run>/map-surface.png and
# map-underground.png; the run dir (with the save copy) is deleted after.
TJ_DATA="${TJ_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/TokyoJungleRecomp}"
set -e
R=/opt/git/tokyoJungleRecomp
bin=$(realpath "$1"); name=$2; shift 2
out=$R/build-runs/$name
rm -rf "$out"; mkdir -p "$out/frames" "$out/vfs"
cp -a "$TJ_DATA/saves/tokyojungle" "$out/cwd"
: > "$out/pad.txt"
$R/.agents/talk say ${POSTER:?set POSTER} "RUN start: $name ($bin, map check, copy of the user's save) $*" >/dev/null
(cd "$out/cwd"; env PS3_VERBOSE=0 TJ_HEADLESS=1 SDL_AUDIODRIVER=dummy TJ_RUN_DIR=$out/vfs PAD_FILE=$out/pad.txt \
  TJ_FRAME_DUMP=$out/frames TJ_FRAME_DUMP_EVERY=30 TJ_FRAME_DUMP_MAX=100000 "$@" \
  timeout -s KILL 300 "$bin" $TJ_DATA/games/NPUA80523/USRDIR/EBOOT.ELF > "$out.txt" 2>&1) &
press() { echo "$1" > "$out/pad.txt"; sleep "$2"; ls "$out/frames" | head -n -20 | sed "s|^|$out/frames/|" | xargs -r rm -f; }
snap() { magick "$out/frames/$(ls -t "$out/frames" | head -1)" "$out/$1.png"; }
sleep 12
for i in $(seq 11); do press 0x0008 2; done      # boot screens to the main menu
for i in 1 2 3; do press 0x4000 5; done          # SURVIVAL, NEW GAME, SINGLE PLAYER
press 0x4000 26                                  # Pomeranian; the street loads
press 0x0008 8                                   # START: pause menu (zoomed map)
press 0x1000 5; snap map-surface                 # TRIANGLE: all nine surface tags
press 0x8000 5; snap map-underground             # SQUARE: underground layer
pkill -KILL -f "PAD_FILE=$out/pad.txt" 2>/dev/null || true
for p in $(pgrep -x tokyojungle); do [ "$(readlink /proc/$p/cwd)" = "$out/cwd" ] && kill -9 $p; done
mv "$out/map-surface.png" "$out/map-underground.png" "$R/build-runs/" 2>/dev/null || true
rm -rf "$out"; mkdir -p "$out"; mv "$R/build-runs/map-surface.png" "$R/build-runs/map-underground.png" "$out/"
echo "$name: $out/map-surface.png $out/map-underground.png; FATAL $(grep -c FATAL $out.txt)"
$R/.agents/talk say $POSTER "RUN done: $name" >/dev/null
