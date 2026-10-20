# How PS3 recomps work, and what this one needs

Researched 2026-10-14 by BALTHASAR-2.

## Do we need a decomp first?

**No.** Static recompilation and decompilation are different things.

- A **decomp** rebuilds human-readable source that compiles back to a matching binary. It takes years,
  and nobody has started one for Tokyo Jungle.
- A **static recomp** mechanically translates every PowerPC instruction of the shipped executable into
  C, then links that against a runtime that stands in for the PS3 OS and hardware. The output is
  unreadable but it runs natively. This is how N64Recomp (Majora's Mask), XenonRecomp (Unleashed
  Recompiled) and ps3recomp work.

A decomp is optional later. You'd use it to hand-replace hot or buggy functions with readable code.
The recomp can be built first and improved function by function.

## How a PS3 recomp is structured (ps3recomp)

The toolkit is [ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT, v0.14.0, Oct 2026). Its PPU
lifter is adapted from XenonRecomp. It is vendored at `third_party/ps3recomp`.

```
EBOOT.BIN (encrypted SELF)
   │  decrypt (needs the title's license: .rap)
   ▼
EBOOT.ELF ──► elf_parser / ppu_loader ──► functions.json + imports.json (NIDs)
   │
   ├─► ppu_lifter.py ──► generated/ppu_recomp_*.cpp   (every PPU function as C)
   ├─► spu_lifter.py ──► SPU job images as C           (SPURS jobs; some are only in data files
   │                                                     and must be dumped at runtime)
   ▼
link with ps3recomp runtime:
   - HLE system libraries (cellGcm, cellSpurs, cellFs, cellAudio, cellPad, sysutil, lv2 syscalls)
   - guest memory / thread model
   - RSX → host GPU backend (D3D12, Vulkan, Metal, null)
   ▼
native executable that reads the original game data from disk
```

Each game is its own repo. It holds `main.cpp` (VM init, ELF load), `stubs.cpp` (game-specific HLE
overrides), a `config.toml` and scripts that run the pipeline. The game code is regenerated
from the user's own ELF and never committed.

## Prior work on this exact game

[sp00nznet/tokyojungle](https://github.com/sp00nznet/tokyojungle) is an earlier ps3recomp port of
NPUA80523, now "not under active development". It's vendored as `third_party/tokyojungle-ref`, and
`docs/PRIOR_PORT.md` covers it in detail. It got to:

- 7,924 PPU functions lifted, and 4,751 SPU functions across 12 images
- data install, SPURS jobs, sound bank load, then the main menu and 16 screens at about 30 fps
- its README's "stall at `0x00C04A68`" is stale: later notes show the audio thread ticks that clock,
  and the fixes are upstream in our ps3recomp pin
- the 12 SPU job images live in the data files, not the EBOOT, and must be re-dumped at runtime
  (`SPU_DUMP_MISS`) and lifted for v1.01
- Windows/D3D12 only, and its glue is hardcoded to v1.00 addresses, so we regenerate rather than copy

So we are not starting from zero. We start from that port's glue, on Linux/Vulkan, with a newer
ps3recomp.

## What this repo has now

| Item | Where | State |
|---|---|---|
| Base 1.00 + 1.01 update PKGs | `gamefiles/packages/*.pkg` | yours, gitignored |
| Game data (v01.01, 2.4 GB) | `gamefiles/games/NPUA80523/USRDIR/` | extracted, gitignored. Only `data/tj.edat` is NPDRM-encrypted. |
| Decrypted executable | `gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF` | **blocked: needs your .rap** |
| Extraction pipeline | `tools/extract/setup_game.sh` | ready |
| ps3recomp + prior port | `third_party/` | submodules |
| Toolchain check | `docs/TOOLCHAIN.md` | Vulkan backend builds and runs on this machine |

## The one blocker: the license

Tokyo Jungle is a paid PSN title, so `EBOOT.BIN` is NPDRM-encrypted with a per-title key that only the
license (`UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap`) unlocks. The 1.01 update EBOOT is locked the
same way (tested: RPCS3 asks for the RAP). Ways to get your own:

1. **From your PS3** (if it runs CFW/HEN): dump the `.rif` and `act.dat` and convert to `.rap`, or use
   a homebrew RAP exporter. RPCS3's quickstart guide covers it: https://rpcs3.net/quickstart
2. **If you already set this game up in RPCS3 somewhere else**, copy the `.rap` from
   `dev_hdd0/home/00000001/exdata/`.

Put it at `gamefiles/raps/UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap` and run `tools/extract/setup_game.sh`.
It's gitignored and never leaves this machine.
