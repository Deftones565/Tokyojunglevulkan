# Tokyo Jungle Recomp

A static recompilation of **Tokyo Jungle** (NPUA80523, v1.01) for Linux, built
with [ps3recomp](https://github.com/sp00nznet/ps3recomp). The game's PowerPC code
is lifted to C++ and compiled as a native x86-64 program that renders through
Vulkan. No emulator runs at play time.

**Status:** the project, toolchain and debugging tools are set up and tested
against ps3recomp's built-in boot test. **The game itself hasn't been lifted
yet.** That's waiting on a decryption licence (see step 1).

**This repo contains no game data.** You need your own copies of the game's PSN
packages and your own licence. Everything derived from them (`input/`,
`extracted/`, `generated/`) is gitignored and must never be committed.

## Requirements

- Linux with a Vulkan driver. Developed on Arch with an RTX 5060.
- clang, cmake, ninja, python 3.11 or later, Vulkan headers, glslang, SDL2.
- The RPCS3 flatpak: `flatpak install --user flathub net.rpcs3.RPCS3`.
  It's used to install and decrypt the game and as the reference emulator.
- Submodules: `git submodule update --init`.

Versions known to work are listed in [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md).

## Build and run

### 1. Inputs

Put these in `input/`:

| File | What it is |
|---|---|
| `tokyo_jungle_1.0.0.pkg` | the base game package |
| `tokyo_jungle_update_1.0.1.pkg` | the 1.01 update |
| `UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap` | **your** licence, dumped from your own PS3 or PSN account |

Then run:

```sh
tools/extract/setup_game.sh
```

It installs both packages into RPCS3, decrypts the 1.01 EBOOT to
`input/EBOOT.ELF`, and copies the game data to `extracted/USRDIR/`.

### 2. Firmware (once)

The PS3's system fonts and the RPCS3 reference both need the firmware. Download
the current `PS3UPDAT.PUP` from Sony's PS3 system-software update page into
`input/`, then:

```sh
flatpak run --filesystem="$PWD/input:ro" net.rpcs3.RPCS3 --installfw "$PWD/input/PS3UPDAT.PUP"
```

Close the window when it finishes. Don't add `--headless`: that aborts without
installing anything. The runner then finds RPCS3's `dev_flash` by itself.

### 3. Lift

```sh
python3 -m venv .venv && .venv/bin/pip install -r third_party/ps3recomp/requirements.txt
.venv/bin/python scripts/analyze.py
```

This runs ps3recomp's `ppu_loader`, then `find_functions`, then `ppu_lifter`
on `input/EBOOT.ELF`, writing to `generated/ppu/`. Add `--clean` when re-lifting
so no stale source files survive.

### 4. Build

First apply this project's fixes to ps3recomp (once after cloning, and again
whenever `patches/ps3recomp/` gains a patch; already-applied ones are skipped):

```sh
git submodule update --init
scripts/apply_patches.sh
```


```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

- **Use clang.** The lifted SPU code relies on clang's `musttail`.
- **Vulkan is on by default on Linux** (`-DTJ_VULKAN=OFF` gives the headless
  null renderer).
- Without a lift in `generated/ppu/`, configure stops and tells you to run step 3.

### 5. Run

```sh
PS3_VERBOSE=0 ./build/tokyojungle input/EBOOT.ELF
```

- The game data is read from `extracted/` by default.
- The runner builds a PS3-shaped directory tree in `build/vfs/` that links to
  that data, so saves land there rather than in `extracted/`.
- **Use `PS3_VERBOSE=0`.** Redirecting stderr otherwise turns on the full
  runtime log, which is heavy enough to change how the game behaves.

#### Environment variables

| Variable | Effect |
|---|---|
| `TJ_GAME_DIR` | game data directory (holds `USRDIR/`); also accepted as the second argument |
| `TJ_HEADLESS=1` | no window (one opens by default when a display is available) |
| `TJ_FRAME_DUMP=<dir>` | save presented frames as numbered PPMs. Related: `_EVERY` (30), `_SKIP` (0), `_MAX` (100) |
| `PS3RECOMP_VK_GUEST_PROGRAMS=1` | run the game's own shaders on Vulkan |
| `PS3_DEV_FLASH` | firmware tree; default is RPCS3's |
| `PS3_BOOT_PATH` | guest `argv[0]`; default `/dev_hdd0/game/NPUA80523/USRDIR/EBOOT.BIN` |
| `TJ_RUN_DIR` | where the VFS tree is built; default `<exe dir>/vfs` |
| `PS3_VFS_ROOT` | skip the VFS tree and use this directory as-is |

The full list, with notes, is in [config/tokyojungle.toml](config/tokyojungle.toml).
Trust frame dumps over screenshots. The prior port's screenshots showed a
different picture from what the game had actually drawn.

## Comparing against RPCS3

When the recomp goes wrong, run the real game in RPCS3 and find where the two diverge:

```sh
tools/oracle/run_rpcs3.sh -s 90                      # -> build-oracle/<timestamp>/
TTY_NO_DEDUPE=1 PS3_VERBOSE=0 ./build/tokyojungle input/EBOOT.ELF > recomp.log 2>&1
tools/oracle/tty_diff.py build-oracle/<timestamp>/TTY.log recomp.log
```

`TTY_NO_DEDUPE=1` matters: by default the runtime stops printing a guest line
after it repeats four times, and the game's repeated `failed to sync file`
errors would then look missing. The game ignores SIGTERM, so stop timed runs
with `timeout -s KILL`.

`tty_diff.py` reports the first line the game prints under RPCS3 that it never
prints under the recomp. Look in that run's `RPCS3.log` around the same point.
See [tools/oracle/README.md](tools/oracle/README.md).

## Layout

| Path | |
|---|---|
| `src/` | the runner (`main.cpp`) and title-specific overrides (`stubs.cpp`) |
| `scripts/analyze.py` | the lift pipeline |
| `config/tokyojungle.toml` | paths and the environment-variable reference |
| `tools/extract/` | package install and EBOOT decryption |
| `tools/oracle/` | RPCS3 reference runs and TTY diffing |
| `third_party/ps3recomp` | the toolkit (submodule, pinned) |
| `third_party/tokyojungle-ref` | the earlier Windows port, for reference only |
| `input/`, `extracted/`, `generated/`, `build*/` | game-derived or built; gitignored |

## Docs

- [docs/RESEARCH.md](docs/RESEARCH.md): how PS3 recomps work and why no decomp is needed
- [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md): ps3recomp on this machine, render backends, caveats
- [docs/PRIOR_PORT.md](docs/PRIOR_PORT.md): what the earlier port solved, what's still open,
  what carried over. **Read this before chasing a boot problem.**
