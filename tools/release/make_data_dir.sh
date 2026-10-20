#!/bin/bash
# usage: tools/release/make_data_dir.sh <out-dir>   (e.g. build-magi7/release/TokyoJungle-data)
# The game data the AppImage needs, from this checkout: the decrypted EBOOT.ELF, the extracted
# title (extracted/: PARAM.SFO + USRDIR, ~2.4 GB) and the firmware fonts from the RPCS3 install
# (dev_flash/data/font). Copy the folder next to the AppImage (or to ~/TokyoJungle-data).
# Your own copy of the game: for your own devices, not for sharing.
set -euo pipefail
cd /opt/git/tokyoJungleRecomp
out=${1:?usage: make_data_dir.sh <out-dir>}
mkdir -p "$out/game" "$out/dev_flash/data"
cp input/EBOOT.ELF "$out/EBOOT.ELF"
cp -r extracted/. "$out/game/"
flash=${PS3_DEV_FLASH:-}
for c in "$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash" "$HOME/.config/rpcs3/dev_flash"; do
    [ -z "$flash" ] && [ -d "$c/data/font" ] && flash=$c
done
if [ -n "$flash" ]; then cp -r "$flash/data/font" "$out/dev_flash/data/"
else echo "warning: no dev_flash fonts found; in-game text that uses system fonts will be missing" >&2; fi
du -sh "$out"
