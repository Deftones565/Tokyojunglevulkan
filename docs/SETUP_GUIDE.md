# Setting up Tokyo Jungle

The port doesn't include the game. You install and decrypt your own copy once with
**RPCS3**; the launcher then finds it there, builds the port from it, and starts it.
RPCS3 isn't used after that.

## What you need

| | |
|---|---|
| **The game** | Tokyo Jungle's `.pkg` from the PlayStation Store (US release, NPUA80523), about 2.6 GB. |
| **The 1.01 update** | Its `.pkg`, about 35 MB. Required. |
| **Your licence** | `UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap`, from the PS3 or PSN account you bought the game on. |
| DLC (optional) | The DLC `.pkg` files you own, with their `.rap` licences. |
| **RPCS3** | From [rpcs3.net](https://rpcs3.net) (AppImage) or Flathub (`net.rpcs3.RPCS3`). |
| **PS3 system software** | `PS3UPDAT.PUP` from [Sony](https://www.playstation.com/en-us/support/hardware/ps3/system-software/). The game uses its fonts. |

The European and Japanese releases are different builds and won't work.

## 1. In RPCS3 (once)

1. **Install the system software:** *File > Install Firmware*, choose `PS3UPDAT.PUP`.
2. **Install the game:** *File > Install Packages/Raps/Edats*, and choose the game's
   `.pkg`, the update's `.pkg` and your `.rap` together (plus any DLC and its `.rap`).
3. **Decrypt the executable:** *Utilities > Decrypt PS3 Binaries*, and choose
   `USRDIR/EBOOT.BIN` in the game's folder (right-click the game in RPCS3's list,
   *Open Install Folder*). RPCS3 writes `EBOOT.elf` next to it.

You don't need to play the game in RPCS3 or change its settings.

## 2. Run the launcher

```sh
./tokyojungle.sh
```

The first time, it installs any missing build tools with your package manager (Arch,
Debian/Ubuntu or Fedora; it asks for your password), then opens the launcher, which looks
for the game in RPCS3's folders (Flatpak, AppImage or system install).

The launcher lists each step:

| Step | |
|---|---|
| Your game | Found in RPCS3 and checked: version 1.01, `EBOOT.elf` decrypted, PS3 fonts installed. |
| Recompiler | ps3recomp with this port's fixes. |
| Game code | The game's code, recompiled to C++ from your EBOOT. |
| SPU jobs | The game's 18 SPU programs (sound, animation, geometry), taken from your EBOOT and recompiled. |
| Build | Tokyo Jungle, compiled for this computer. |

Green is done, orange is what **Set up** will do, and red is something to fix in RPCS3
first. Press **Set up** (about 15 minutes the first time), then **Play**. After that the
launcher opens straight on Play.

**Game somewhere else?** Drag its folder (`NPUA80523`) onto the window, or use *Choose
folder*. RPCS3's folder or its `dev_hdd0` works too.

If the game meets an SPU job the launcher doesn't know, it is saved and recompiled the
next time you open the launcher.

**Steam Deck:** SteamOS is read-only, so build in a distrobox (Arch:
`distrobox create -i archlinux`) or on another Linux PC.

## If something goes wrong

| The launcher says | What to do |
|---|---|
| *Not found* | Install the game in RPCS3 (section 1, step 2), or drop its folder on the window. |
| *Version 01.00* | Install the 1.01 update in RPCS3, then decrypt `EBOOT.BIN` again. |
| *EBOOT.elf decrypted from version 1.00* | You decrypted before installing the update: decrypt again. |
| *Not decrypted yet* | Section 1, step 3. If RPCS3 can't decrypt it, the `.rap` isn't for this copy. |
| *No PS3 fonts* | Section 1, step 1. |
| *... failed (see launcher.log)* | Details are at the end of `~/TokyoJungle-data/launcher.log`. |
| *...: install it in RPCS3 first* | You dropped a `.pkg`, `.rap` or `.PUP`. Install those in RPCS3, then drop the installed game's folder. |

## Without the window

```sh
./tokyojungle.sh --status            # what is done and what isn't
./tokyojungle.sh --setup [<folder>]  # do the setup in the terminal
./tokyojungle.sh --play              # start the game
```

## Where things are

| | |
|---|---|
| `~/TokyoJungle-data/` | What the game runs from, your saves (`gamedata/`) and `launcher.log`. Set `TJ_DATA` to use another folder. |
| `build/tokyojungle` | The built game. |
| RPCS3's `dev_hdd0/game/NPUA80523/` | The installed game. The data folder links to these files rather than copying them. |

The launcher only reads RPCS3's folders and never changes them. This repository holds no
keys: RPCS3 decrypts the game, and DLC `.edat` files are decrypted at run time by
ps3recomp's runtime.
