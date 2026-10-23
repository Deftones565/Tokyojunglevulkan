# Tokyo Jungle Recompiled

A native Linux port of **Tokyo Jungle** (PS3, NPUA80523, v1.01), statically recompiled
with [ps3recomp](https://github.com/sp00nznet/ps3recomp) and rendered through Vulkan.
No emulator runs while you play.

**No game files are included.** You need your own copy; the port is built from it on
your computer.

## Status

| | |
|---|---|
| Boot, menus, movies | ✅ |
| Survival mode | ✅ |
| Story mode | ✅ |
| Graphics | ✅ Up to 400% internal resolution |
| Audio | ✅ |
| Saves and trophies | ✅ |
| DLC | ✅ |
| Frame rate | ✅ 60 FPS |
| Steam Deck | ⚠️ 30 FPS |

## Playing

You need Linux with a Vulkan GPU, RPCS3, and your own copy of the game (PSN package,
1.01 update and `.rap` licence).

1. In RPCS3: install the PS3 system software and the game, then decrypt `EBOOT.BIN`.
2. Run `./tokyojungle.sh`.
3. Press **Set up** (about 15 minutes the first time), then **Play**.

Full instructions: [docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md).

## Building by hand

Needs clang, CMake, Ninja, Python 3.11+, and the Vulkan, SDL2, FFmpeg and glslang
development files.

```sh
git submodule update --init
scripts/apply_patches.sh
python3 -m venv .venv && .venv/bin/pip install -r third_party/ps3recomp/requirements.txt
.venv/bin/python scripts/analyze.py --elf <EBOOT.ELF>
.venv/bin/python scripts/extract_spu_jobs.py <EBOOT.ELF>
.venv/bin/python scripts/lift_spu.py
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
./build/tokyojungle <EBOOT.ELF> <game folder>
```

Run-time options: [config/tokyojungle.toml](config/tokyojungle.toml).

## No emulation

All PPU and SPU code is recompiled ahead of time; there is no interpreter or JIT, not
even as a fallback. Only the GPU command stream is translated at run time, to Vulkan.
`scripts/check_no_emulation.py` runs after every build and fails it if that changes.

## License

MIT ([LICENSE](LICENSE)). Tokyo Jungle belongs to Sony Interactive Entertainment; this
project is not affiliated with Sony. Built on
[ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet.
