#!/bin/bash
# usage: tools/release/make_save_tar.sh <out.tar.gz>   (env: SAVE=<gamedata dir>, default ./gamedata)
# Packs the save build/ uses (gamedata/dev_hdd0 + gamedata/trophies, not the backups) as
# gamedata/... Unpack it in the folder holding the AppImage: tar -xzf TokyoJungle-save.tar.gz
set -euo pipefail
cd "$(dirname "$0")/../.."
src=$(realpath "${SAVE:-gamedata}"); out=$(realpath -m "${1:?usage: make_save_tar.sh <out.tar.gz>}")
parts=(dev_hdd0); [ -d "$src/trophies" ] && parts+=(trophies)
tar -czf "$out" -C "$(dirname "$src")" --transform "s|^$(basename "$src")|gamedata|" "${parts[@]/#/$(basename "$src")/}"
tar -tzvf "$out"
