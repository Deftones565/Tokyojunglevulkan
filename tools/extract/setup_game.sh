#!/usr/bin/env bash
# setup_game.sh: turn your own Tokyo Jungle PSN files into the inputs the recomp needs.
#
#   input/tokyo_jungle_1.0.0.pkg          base game (NPUA80523)
#   input/tokyo_jungle_update_1.0.1.pkg   1.01 update (its EBOOT replaces the base one)
#   input/UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap   your license, dumped from your own PS3/account
#
# Produces:
#   input/EBOOT.ELF      decrypted 1.01 executable (what the lifter reads)
#   extracted/USRDIR/    game data (assets the runtime loads)
#
# Uses the RPCS3 flatpak (flatpak install --user flathub net.rpcs3.RPCS3) only as an
# installer/decrypter. Nothing here contains keys; RPCS3 brings its own.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
title=NPUA80523
content_id=UP9000-NPUA80523_00-TOKYOJUNGLE00001
hdd="$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_hdd0"
game="$hdd/game/$title"
rpcs3() { QT_QPA_PLATFORM=offscreen flatpak run --filesystem="$root" net.rpcs3.RPCS3 --headless "$@"; }

rap="$root/input/$content_id.rap"
[[ -f $rap ]] || { echo "Missing $rap (the EBOOT is NPDRM-locked; see docs/RESEARCH.md)." >&2; exit 1; }
mkdir -p "$hdd/home/00000001/exdata"
cp "$rap" "$hdd/home/00000001/exdata/$content_id.rap"

# Install base then update, unless the update's EBOOT is already in place.
if [[ ! -f $game/USRDIR/EBOOT.BIN ]]; then
	rpcs3 --installpkg "$root/input/tokyo_jungle_1.0.0.pkg"
	rpcs3 --installpkg "$root/input/tokyo_jungle_update_1.0.1.pkg"
fi

rpcs3 --decrypt "$game/USRDIR/EBOOT.BIN"
elf=
for f in "$game"/USRDIR/EBOOT.elf "$game"/USRDIR/EBOOT.ELF; do [[ -f $f ]] && elf=$f; done
[[ -n $elf ]] || { echo "RPCS3 did not produce a decrypted ELF." >&2; exit 1; }
cp "$elf" "$root/input/EBOOT.ELF"

mkdir -p "$root/extracted"
rsync -a --delete --exclude 'EBOOT.*' "$game/USRDIR/" "$root/extracted/USRDIR/"
cp "$game/PARAM.SFO" "$root/extracted/"

head -c 4 "$root/input/EBOOT.ELF" | grep -q $'\x7fELF' || { echo "input/EBOOT.ELF is not an ELF." >&2; exit 1; }
echo "OK: input/EBOOT.ELF and extracted/USRDIR are ready."
