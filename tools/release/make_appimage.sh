#!/bin/bash
# usage: tools/release/make_appimage.sh [out-dir]   (default release)
# Builds TokyoJungle-x86_64.AppImage for the Steam Deck / other Linux PCs in the tj-release
# container (Ubuntu 24.04, glibc 2.39: runs on SteamOS 3.8's glibc 2.41). Needs Docker running.
# The game data goes beside it separately: tools/release/make_data_dir.sh.
set -euo pipefail
cd /opt/git/tokyoJungleRecomp
out=$(realpath -m "${1:-release}"); mkdir -p "$out/.build"
# JOBS (default 10 of 20 threads) at low priority, the container capped at MEM (14g): the two
# ~19 MB lifted PPU sources need several GB each, and running out must not freeze the desktop.
nice -n 10 docker build -t tj-release tools/release
docker run --rm -u "$(id -u):$(id -g)" -e JOBS="${JOBS:-10}" \
    --memory="${MEM:-14g}" --memory-swap="${MEM:-14g}" --cpu-shares=256 \
    -v "$PWD:/src:ro" -v "$out:/out" tj-release bash /src/tools/release/build_in_container.sh
ls -la "$out/TokyoJungle-x86_64.AppImage"
