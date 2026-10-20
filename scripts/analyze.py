#!/usr/bin/env python3
"""
Lift Tokyo Jungle's EBOOT into C++ that CMakeLists.txt builds.

Runs ps3recomp's PPU pipeline, in the order docs/GAME_PORTING_GUIDE.md gives:

  1. ppu_loader.py      .opd table -> functions, firmware imports, entry/TOC
  2. find_functions.py  heuristic scan for what .opd misses, seeded with (1)
  3. ppu_lifter.py      ELF -> ppu_recomp.h + ppu_recomp_NNN.cpp, with every
                        import stub emitted as ps3_hle_call(<nid>)

Output goes under generated/ (gitignored: it is derived from the game):

  generated/analysis/   the JSON from steps 1 and 2
  generated/ppu/        the lift; CMakeLists.txt reads it as RECOMP_DIR

Paths default from config/tokyojungle.toml. Run it with the project venv:

  .venv/bin/python scripts/analyze.py              # gamefiles/games/NPUA80523/USRDIR/EBOOT.ELF
  .venv/bin/python scripts/analyze.py --elf other.elf --out /tmp/x

This replaces the prior port's analyze.py + gen_stubs.py + post_lift.py.
Those patched an older lifter's output by hand and hardcode v1.00 addresses
(TOC 0x00359220, CRT bodies at 0x10230, 0x25FC00, ...), which do not hold for
the v1.01 EBOOT. The current lifter does that work itself.
"""

import argparse
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "third_party" / "ps3recomp" / "tools"
CONFIG = ROOT / "config" / "tokyojungle.toml"


def run(step: str, args: list[str]) -> None:
    print(f"\n=== {step}\n$ {' '.join(args)}", flush=True)
    t0 = time.monotonic()
    rc = subprocess.run(args).returncode
    if rc != 0:
        sys.exit(f"{step} failed (exit {rc})")
    print(f"--- {step}: {time.monotonic() - t0:.1f}s", flush=True)


def main() -> None:
    cfg = tomllib.loads(CONFIG.read_text())
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--elf", type=Path, default=ROOT / cfg["input"]["elf_path"],
                    help="decrypted PPU ELF (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=ROOT / cfg["output"]["dir"],
                    help="output root (default: %(default)s)")
    ap.add_argument("--opd-only", action="store_true",
                    help="lift only the functions .opd declares; skip the heuristic scan")
    ap.add_argument("--jobs", "-j", type=int, help="lifter worker processes")
    ap.add_argument("--chunk-lines", type=int,
                    default=cfg["output"].get("chunk_lines"),
                    help="max lines per lifted source file")
    ap.add_argument("--clean", action="store_true",
                    help="delete the previous lift first, so no stale chunk survives")
    a = ap.parse_args()

    if not a.elf.is_file():
        sys.exit(f"no ELF at {a.elf}. It must be the decrypted EBOOT (SELF -> ELF).")
    with a.elf.open("rb") as f:
        if f.read(4) != b"\x7fELF":
            sys.exit(f"{a.elf} is not an ELF. An encrypted SELF starts with 'SCE'.")

    py = sys.executable
    analysis = a.out / "analysis"
    ppu = a.out / "ppu"
    if a.clean and ppu.exists():
        shutil.rmtree(ppu)
    analysis.mkdir(parents=True, exist_ok=True)
    ppu.mkdir(parents=True, exist_ok=True)
    stem = a.elf.name.split(".")[0]

    run("ppu_loader", [py, str(TOOLS / "ppu_loader.py"), str(a.elf), "-o", str(analysis)])
    funcs = analysis / f"{stem}.functions.json"
    imports = analysis / f"{stem}.imports.json"

    if not a.opd_only:
        merged = analysis / "functions.json"
        run("find_functions", [py, str(TOOLS / "find_functions.py"), str(a.elf),
                               "--seed-json", str(funcs), "--output", str(merged)])
        funcs = merged

    lift = [py, str(TOOLS / "ppu_lifter.py"), str(a.elf),
            "--functions", str(funcs), "--hle-stubs", str(imports),
            "--output", str(ppu)]
    if a.jobs:
        lift += ["--jobs", str(a.jobs)]
    if a.chunk_lines:
        lift += ["--chunk-lines", str(a.chunk_lines)]
    run("ppu_lifter", lift)

    print(f"\nLifted into {ppu}. Build with:\n"
          f"  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang "
          f"-DCMAKE_CXX_COMPILER=clang++\n  cmake --build build")


if __name__ == "__main__":
    main()
