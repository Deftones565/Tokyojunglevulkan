#!/bin/bash
# usage: tools/release/make_bundle.sh [out.tar.zst]   (default release/TokyoJungle-SteamDeck.tar.zst)
#   env: SAVE=<gamedata dir> (default: this checkout's gamedata/, the save build/ uses)
# One tarball for the Deck: TokyoJungle/ with the AppImage, TokyoJungle-data/ and gamedata/
# (save + trophies, unpacked: the AppImage plays from the gamedata/ beside it). Unpack it where
# the game should live: tar -xf TokyoJungle-SteamDeck.tar.zst  (or open it in Ark).
# Needs release/TokyoJungle-x86_64.AppImage (make_appimage.sh) and release/TokyoJungle-data
# (make_data_dir.sh).
set -euo pipefail
cd /opt/git/tokyoJungleRecomp
out=$(realpath -m "${1:-release/TokyoJungle-SteamDeck.tar.zst}")
save=$(realpath "${SAVE:-gamedata}")
for f in release/TokyoJungle-x86_64.AppImage release/TokyoJungle-data/EBOOT.ELF "$save/dev_hdd0"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
readme=$(mktemp); trap 'rm -f "$readme"' EXIT
cat > "$readme" <<'EOF'
Tokyo Jungle (PS3, statically recompiled) for Linux / Steam Deck

Keep these together in one folder:
  TokyoJungle-x86_64.AppImage   the game (run it; in Game Mode add it via "Add a Non-Steam Game")
  TokyoJungle-data/             game files (EBOOT.ELF, game/, dev_flash/ fonts)
  gamedata/                     your save and trophies; the game reads and writes them here

Settings (Esc / Guide button opens the menu) and the log are in ~/.local/share/tokyojungle/.
If it does not start, look at ~/.local/share/tokyojungle/tokyojungle.log.
Keep the folder's path short (e.g. ~/Games/TokyoJungle): very long paths break the game's
data check.
EOF
parts=(dev_hdd0); [ -d "$save/trophies" ] && parts+=(trophies)
nice -n 10 tar -c \
    --transform 's|^release/|TokyoJungle/|' --transform "s|^$(basename "$save")/|TokyoJungle/gamedata/|" \
    --transform "s|^${readme#/}\$|TokyoJungle/README.txt|" \
    release/TokyoJungle-x86_64.AppImage release/TokyoJungle-data \
    -C "$(dirname "$save")" "${parts[@]/#/$(basename "$save")/}" \
    -C / "${readme#/}" \
  | nice -n 10 zstd -T10 -6 -q -o "$out" -f
tar -tf "$out" | awk -F/ '{print $1"/"$2}' | sort -u
ls -la "$out"
