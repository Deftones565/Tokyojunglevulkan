# Setting up Tokyo Jungle

Tokyo Jungle for Linux runs your own copy of the PlayStation 3 game. It doesn't
include the game, and it can't install or decrypt it. You do that once with
**RPCS3**, the PlayStation 3 emulator. Tokyo Jungle's launcher then finds the
game in RPCS3, builds the port on your computer from it, and starts it.

RPCS3 is only needed for this one-time step. The game itself runs natively, not
in the emulator.

## What you need

| | What it is |
|---|---|
| **The game's package** | The `.pkg` file of Tokyo Jungle from the PlayStation Store (US release, NPUA80523), about 2.6 GB. |
| **The 1.01 update's package** | The game's update, also a `.pkg` (about 35 MB). The game needs it. |
| **Your licence** | `UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap`, the licence for your copy. It comes from the PlayStation 3 or PSN account you bought the game on. |
| DLC (optional) | The `.pkg` files of any DLC you own, with their `.rap` licences. |
| **RPCS3** | Free, from [rpcs3.net](https://rpcs3.net) (AppImage), or from Flathub (`net.rpcs3.RPCS3`; on the Steam Deck, from Discover). |
| **The PS3 system software** | `PS3UPDAT.PUP`, from [Sony's PS3 system software page](https://www.playstation.com/en-us/support/hardware/ps3/system-software/). The game uses the PS3's fonts from it. |

Other regions (the European or Japanese releases) are a different build of the
game and won't work.

## 1. In RPCS3

Do these once, in this order. If you already play Tokyo Jungle 1.01 in RPCS3,
you've done all of them except maybe the last.

1. **Install the system software:** *File > Install Firmware*, and choose
   `PS3UPDAT.PUP`.
2. **Install the game:** *File > Install Packages/Raps/Edats*, and choose the
   game's `.pkg`, the 1.01 update's `.pkg` and your `.rap` together (and any
   DLC `.pkg` files and their `.rap` files).
3. **Decrypt the game's executable:** *Utilities > Decrypt PS3 Binaries*, and
   choose `EBOOT.BIN` in the game's `USRDIR` folder. To find it, right-click
   Tokyo Jungle in RPCS3's game list and choose *Open Install Folder*, then
   open `USRDIR`. RPCS3 writes `EBOOT.elf` next to it.

You don't need to play the game in RPCS3, or to change any of its settings.

## 2. Start the launcher

From the Tokyo Jungle folder:

```sh
./tokyojungle.sh
```

The first time, it checks for the tools that build the game (a C++ compiler,
CMake, Ninja, Python and a few development libraries) and installs the missing
ones with your system's package manager, which asks for your password. Then
the launcher window opens. It looks for the game in RPCS3's folders (the
Flatpak's, and the AppImage's or a system install's).

The list in the window shows each step:

| Step | What it is |
|---|---|
| Your game | Your copy, found in RPCS3 and checked: version 1.01, `EBOOT.elf` decrypted, PS3 fonts installed. |
| Recompiler | ps3recomp, with this port's fixes applied. |
| Game code | The game's code, recompiled to C++ from your EBOOT. |
| Build | Tokyo Jungle, compiled for this computer. |
| SPU jobs | The game's code for the PS3's SPU coprocessors (sound, animation, geometry): 18 small programs, taken from your EBOOT and recompiled. |

Green is done, orange is what **Set up** will do, red is something you still
need to do in RPCS3 (the line says what). Press **Set up**, and leave it
running: the first time takes about 15 minutes, most of it compiling. Then press
**Play**. From then on the launcher opens on Play straight
away.

**A copy somewhere else** (RPCS3 with a custom game folder, or a copy on another
drive): drag the game's folder (`NPUA80523`) onto the window, or use *Choose
folder*. The RPCS3 folder above it, or its `dev_hdd0` folder, works too.

If the game ever meets an SPU job the launcher doesn't know, it saves it; the
next time you start the launcher, it recompiles it (one orange line, a few
minutes).

### On the Steam Deck

SteamOS can't install the build tools on its read-only system: build in a
distrobox (an Arch one), or on another Linux PC.

## If something goes wrong

| The launcher says | What to do |
|---|---|
| *Not found. Install it in RPCS3* | Do step 2 in RPCS3. If RPCS3 keeps its games somewhere unusual, drop the game's folder on the window. |
| *Version 01.00: install the 1.01 update* | Install the update's `.pkg` in RPCS3, then decrypt `EBOOT.BIN` again. |
| *EBOOT.elf decrypted from version 1.00* | You decrypted before installing the update: decrypt `EBOOT.BIN` again. |
| *Not decrypted yet* | Do step 3. If RPCS3 can't decrypt it, the `.rap` isn't the licence for this copy. |
| *No PS3 fonts* | Do step 1. |
| *... failed (see launcher.log)* | The details are at the end of `~/TokyoJungle-data/launcher.log`. |
| *...: install it in RPCS3 first* | You dropped a `.pkg`, `.rap` or `.PUP`. Those go to RPCS3; the launcher only takes the installed game. |

## Without the window

```sh
./tokyojungle.sh --status            # the same list
./tokyojungle.sh --setup [<folder>]  # everything Set up does, in the terminal
./tokyojungle.sh --play              # start the game
```

## Where things are

| | |
|---|---|
| `~/TokyoJungle-data/` | What the game runs from: `EBOOT.ELF`, `game/`, `dev_flash/` (the PS3's fonts), your saves (`gamedata/`) and `launcher.log`. Set `TJ_DATA` to use another folder. |
| `build/tokyojungle` | The game the launcher built. |
| RPCS3's `dev_hdd0/game/NPUA80523/` | The game as RPCS3 installed it. The data folder links to these files instead of copying them, so it takes no extra space on the same drive, and removing the game from RPCS3 later doesn't break it. |

The launcher only reads RPCS3's folders; it never changes anything in them.
This repository holds no keys: your game and its executable are decrypted by
RPCS3. DLC is the one exception: its `.edat` files are decrypted while the game
runs, by the recompiler's runtime (ps3recomp, with keys in its own source).
Nothing of the game is shipped with the port: everything game-derived is made on
your computer, from your copy.
