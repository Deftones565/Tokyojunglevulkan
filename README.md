# Tokyo Jungle Recompiled

A native Linux port of **Tokyo Jungle** (PS3, NPUA80523, v1.01), made by static
recompilation with [ps3recomp](https://github.com/sp00nznet/ps3recomp). The game's PPU
and SPU code is translated to C++ ahead of time and compiled into a normal program that
renders through Vulkan. No emulator runs while you play.

**Nothing of the game is in this repository.** You need your own copy. The port is built
on your computer from it.

## Status

| | |
|---|---|
| Boot, menus, movies | ✅ Works |
| Survival mode | ✅ Fully playable: hunting, bosses, mating, generations, game over |
| Story mode | ⚠️ Not re-tested since a fix for a hang after a Survival game over |
| Graphics | ✅ Vulkan, with the game's own shaders; colours match RPCS3; up to 400% internal resolution |
| Text | ✅ Drawn natively, with the PS3's own fonts |
| Audio | ✅ Works |
| Saves and trophies | ✅ Work |
| DLC | ✅ Works |
| Settings | ✅ In-game menu: display mode, resolution, internal resolution, aspect ratio, V-sync, FPS counter |
| Frame rate | ✅ 60 FPS in gameplay on an RTX 5060 laptop, like the PS3 |
| Steam Deck | ⚠️ 30 FPS at the right speed; 60 is being worked on. Build in a distrobox (SteamOS is read-only) |
| Platforms | Linux, x86-64, Vulkan |

Known issue: a one-frame flicker of the Pomeranian's fur on close-ups (about 1 frame in
1300). Details and everything else in progress: [docs/STATUS.md](docs/STATUS.md).

## Playing

You need Linux with a Vulkan GPU, about 3 GB of free space, RPCS3, and your own copy of
the game: its PSN package, the 1.01 update, and your licence (`.rap`).

1. **In RPCS3**, once: install the PS3 system software, install the game with its update
   and licence, and decrypt the game's `EBOOT.BIN` (*Utilities > Decrypt PS3 Binaries*).
2. **Run `./tokyojungle.sh`.** It installs the build tools it needs (asking for your
   password), then opens the launcher, which finds the game in RPCS3.
3. **Press Set up**, then **Play**. The first setup builds the port for your computer
   and takes about 15 minutes; after that the launcher opens straight on Play.

Step by step, with troubleshooting: [docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md).

This repository holds no keys and ships nothing game-derived: RPCS3 decrypts the game,
and the launcher makes everything else on your computer from your copy. (DLC `.edat`
files are the one exception: ps3recomp's runtime decrypts those while the game runs.)

## Building by hand

The launcher runs these steps; to run them yourself (clang, CMake, Ninja, Python 3.11+,
and the Vulkan, SDL2, FFmpeg and glslang development files):

```sh
git submodule update --init
scripts/apply_patches.sh                       # this port's fixes to ps3recomp
python3 -m venv .venv && .venv/bin/pip install -r third_party/ps3recomp/requirements.txt
.venv/bin/python scripts/analyze.py --elf <decrypted EBOOT.ELF>       # PPU code -> generated/ppu
.venv/bin/python scripts/extract_spu_jobs.py <decrypted EBOOT.ELF>    # the 18 SPU jobs
.venv/bin/python scripts/lift_spu.py                                  # SPU code -> generated/spu
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
./build/tokyojungle <EBOOT.ELF> <game folder holding USRDIR/>
```

Use clang: the lifted SPU code needs its `musttail`. Run-time options are listed in
[config/tokyojungle.toml](config/tokyojungle.toml).

## Docs

- [docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md): setting up and playing
- [docs/STATUS.md](docs/STATUS.md): what works, what's open, every patch
- [docs/RESEARCH.md](docs/RESEARCH.md): how PS3 static recompilation works
- [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md): the build toolchain and render backends
- [docs/PRIOR_PORT.md](docs/PRIOR_PORT.md): what the earlier port solved
- [tools/oracle/README.md](tools/oracle/README.md): comparing against RPCS3 when something goes wrong

## License

MIT, for this project's own code ([LICENSE](LICENSE)). Tokyo Jungle belongs to Sony
Interactive Entertainment; this project is not affiliated with or endorsed by Sony.
Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and its
contributors.
