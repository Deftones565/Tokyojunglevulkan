# Fonts: Sony's libfont, lifted

Decided 2026-10-14 (BALTHASAR-2, from MELCHIOR-1's findings). **Done 2026-10-14**: the game draws
its text through the firmware's own font stack, statically lifted. Nothing is interpreted.

## Why
- RPCS3 runs Tokyo Jungle against the firmware's real `libfont.sprx`, not an HLE.
- The game imports 31 font functions (cellFont 30, cellFontFT 1). ps3recomp's HLE covers 15, and its
  OpenFontset never loads a real font, so glyphs rendered empty.
- Statically lifting the firmware libraries is pure recomp and gives exact semantics.

## How to build it
```sh
tools/firmware/decrypt_prx.sh                 # dev_flash -> gamefiles/firmware/modules/<module>.prx
.venv/bin/python scripts/lift_firmware.py     # -> generated/prx/
cmake -S . -B build && cmake --build build
```
Both outputs are firmware-derived and gitignored. Never commit them.

## What runs
| Module | Base | Lifted functions | Exports |
|---|---|---|---|
| libfont (`cellFont`) | 0x31000000 | 174 | 88 |
| libfontFT (`cellFontFT`) | 0x31100000 | 182 | 102 |
| libfreetype (`cell_FreeType2`) | 0x31200000 | 849 | 249 |

These are the three modules RPCS3 loads for this game. libfreetypeTT is not used. 0x30000000 to
0x3FFFFFFF is the runtime's PRX code window.

- `scripts/lift_firmware.py` runs `lift_prx.py` then `ppu_lifter.py --raw` per module, with a
  symbol prefix, the module's TOC, and `--hle-stubs` so each import becomes `ps3_hle_call(nid)`.
- `generated/prx/prx_modules.cpp` copies the images into guest memory, registers their functions,
  and registers one HLE handler per exported NID that enters the lifted export with the module's
  TOC. That one table binds the game's font imports and the modules' imports of each other.
- `src/stubs.cpp` hooks `cellSysmoduleLoadModule` to run a module's `module_start` when the game
  loads it, as the firmware does (only libfont has one).
- With the modules present, the old `cellFontInitializeWithRevision` forwarder is not registered.
- libfont opens the same dev_flash fonts, in the same order, as under RPCS3
  (`SCE-PS3-RD-R-LATIN2`, `NR-R-JPN`, `YG-R-KOR`; `.ccd` first, then `.TTF`).

## Toolkit fixes it needed
- `0007-lifter-raw-toc-restore.patch`: `--toc` now also lowers `ld r2,N(r1)` TOC restores to the
  module's TOC. With `--hle-stubs` the stub that saved r2 is gone, so the stack read was garbage.
- `0008-hle-ctx-replace-and-capacity.patch`: the context-handler table held 256 entries and dropped
  the rest silently; it now holds 2048, complains when full, and re-registering a NID replaces
  the old handler instead of being shadowed by it.

## Open
- A white rectangle covers the middle of the screen while the disclaimer text fades in. Looks like
  an RSX/texture problem, not a font one.
