#!/bin/bash
# usage: tools/extract/install_dlc.sh <DLC .pkg>
#
# Puts Tokyo Jungle's DLC EDATs (USRDIR/data/dlc/dlc*.edat) into the game data
# this port reads (gamefiles/games/NPUA80523/USRDIR). Same approach as setup_game.sh: the RPCS3
# flatpak is used only as the package installer, nothing here holds keys.
#
# The EDATs are license type 3, encrypted under the game's own klicensee, which
# the game passes to sceNpDrmIsAvailable at run time (patch 0094); the runtime
# decrypts them then. The DLC .rap licenses are copied to RPCS3's exdata as
# well, as on a console, though type-3 content does not need them.
#
# A package may also carry a replacement EBOOT.BIN (a firmware "fix"). This
# script never touches gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF, but RPCS3's own copy of the game gets
# that EBOOT: rerun setup_game.sh's update install before using it again.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
pkg=$(realpath "${1:?usage: install_dlc.sh <DLC .pkg>}")
hdd="$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_hdd0"
game="$hdd/game/NPUA80523"

mkdir -p "$hdd/home/00000001/exdata"
cp "$root"/gamefiles/raps/UP9000-NPUA80523_00-DLC*.rap "$hdd/home/00000001/exdata/" 2>/dev/null || true

QT_QPA_PLATFORM=offscreen flatpak run --filesystem="$(dirname "$pkg")" net.rpcs3.RPCS3 \
    --headless --installpkg "$pkg" || true    # RPCS3 exits non-zero at teardown

n=$(ls "$game"/USRDIR/data/dlc/dlc*.edat 2>/dev/null | wc -l)
[[ $n -gt 0 ]] || { echo "No DLC EDATs under $game/USRDIR/data/dlc" >&2; exit 1; }
mkdir -p "$root/gamefiles/games/NPUA80523/USRDIR/data/dlc"
cp "$game"/USRDIR/data/dlc/dlc*.edat "$root/gamefiles/games/NPUA80523/USRDIR/data/dlc/"
echo "OK: $n DLC EDATs in gamefiles/games/NPUA80523/USRDIR/data/dlc"
