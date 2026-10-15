#!/usr/bin/env python3
"""
Lift Tokyo Jungle's SPURS job images and register them with the runtime.

Tokyo Jungle's SPU jobs are raw code+data blobs the game loads from its data
files, not SPU ELFs inside the EBOOT, so they can only be captured while it runs:

  SPU_DUMP_MISS=$PWD/generated/spu/dumps ./build/tokyojungle input/EBOOT.ELF

writes generated/spu/dumps/spujob_<FINGERPRINT>_<SIZE>.bin for every job the
runtime had no lifted code for. This script then, for each dump:

  1. checks the bytes still hash to the fingerprint in the name (FNV-1a-64,
     as runtime/spu/spu_workload.c computes it), so a stale or truncated dump
     can never register code under the wrong key;
  2. find_spu_functions.py --raw --base 0, then spu_lifter.py --base 0 into
     generated/spu/j<FP>/ with symbol prefix j<FP>_, so every image links
     alongside the others;
  3. rewrites the lifter's relative runtime includes to bare ones (CMakeLists
     adds runtime/spu to these sources' include path);

and writes generated/spu/spu_workloads.c, which registers each image by
fingerprint from a constructor. CMakeLists.txt picks up the lift and that file
on the next configure. Lifting only: nothing here enables an interpreter.

  .venv/bin/python scripts/lift_spu.py            # lift new dumps, rewrite registry
  .venv/bin/python scripts/lift_spu.py --relift   # lift every dump again
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "third_party" / "ps3recomp" / "tools"
SPU = ROOT / "generated" / "spu"
DUMP_RE = re.compile(r"spujob_([0-9A-F]{16})_(\d+)\.bin$")

# What the prior port identified (docs/PRIOR_PORT.md). Labels only.
KNOWN = {
    0xF523F26F69764538: "SGX sound down-mix job chain (socjob)",
    0xFDFFAAC1BCD5352E: "SGX sound DSP job: compressor / amp / peak (dspjob)",
    0x78FEA84E1C23BB5C: "sets the SPURS event flag the boot thread waits on",
}

# Branch targets find_spu_functions can't see statically (passed to spu_lifter
# as --extra-funcs), on top of the jump-table targets found below. Each one
# was a lift gap: the runtime, built with PS3RECOMP_NO_INTERP, stops with
# "no lifted code at LS 0x....". Add the address here and re-lift; never
# interpret it (AGENTS.md).
EXTRA_ENTRIES: dict[int, list[int]] = {}

NOPS = {0x40200000, 0x00200000}            # nop, lnop


def rel_jump_table_targets(data: bytes) -> list[int]:
    """Targets of the images' self-relative jump tables.

    The SGX jobs dispatch through tables of signed 32-bit offsets measured from
    the table's own start (target = table + entry), loaded into a register and
    taken with `bi`/`bisl`. Nothing branches to those targets directly, so
    find_spu_functions classifies them as data and they were never lifted
    (socjob's table at 0x5674 holds every DSP module's command handler).
    A run of >= 4 such words counts as a table when at least 80% of its
    targets sit right after an unconditional branch (skipping nop padding) --
    i.e. code nothing falls through into."""
    n = len(data) & ~3
    u32 = lambda a: int.from_bytes(data[a:a + 4], "big")
    s32 = lambda a: int.from_bytes(data[a:a + 4], "big", signed=True)
    is_br = lambda w: (w >> 23) == 0x64 or (w >> 21) == 0x1A8      # br / bi

    out: set[int] = set()
    a = 0
    while a < n:
        b = a
        while b < n and s32(b) and s32(b) % 4 == 0 and 0 <= a + s32(b) < n:
            b += 4
        if (b - a) // 4 < 4:
            a += 4
            continue

        def after_branch(t: int) -> bool:
            p = t - 4
            while p >= 0 and u32(p) in NOPS:
                p -= 4
            return p >= 0 and (is_br(u32(p)) or p == b - 4)

        targets = {a + s32(x) for x in range(a, b, 4)}
        if sum(after_branch(t) for t in targets) >= 0.8 * len(targets):
            out |= {t for t in targets if u32(t) and u32(t) not in NOPS}
        a = b
    return sorted(out)


def fnv1a64(data: bytes) -> int:
    # The runtime's offset basis, which is NOT standard FNV-1a: it is
    # 1469598103934665603, the standard 14695981039346656037 with the last digit
    # dropped. The fingerprints the runtime logs and dumps are keyed on this
    # value, so match it rather than "fixing" it.
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def run(args: list[str]) -> str:
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"failed ({r.returncode}): {' '.join(args)}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr


def spu_elf(data: bytes):
    """(entry, code segment LS address, code bytes) for an SPU ELF, else None.

    SPURS jobs are raw blobs that run from LS 0, but taskset TASKS are ELFs
    (cellSpursCreateTask takes an eaElf): the runtime loads their segments into
    local store and calls the lifted function at e_entry (spu_workload.c)."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        return None
    u16 = lambda o: int.from_bytes(data[o:o + 2], "big")
    u32 = lambda o: int.from_bytes(data[o:o + 4], "big")
    entry, phoff, phent, phnum = u32(0x18), u32(0x1C), u16(0x2A), u16(0x2C)
    for i in range(phnum):
        o = phoff + i * phent
        p_type, p_off, p_va, p_filesz, p_flags = u32(o), u32(o + 4), u32(o + 8), u32(o + 16), u32(o + 24)
        if p_type == 1 and p_flags & 1:            # PT_LOAD, executable
            return entry, p_va, data[p_off:p_off + p_filesz]
    return None


def entry_of(dump: Path) -> int:
    e = spu_elf(dump.read_bytes())
    return e[0] if e else 0


def lift(dump: Path, fp: int, relift: bool) -> dict:
    name = f"j{fp:016X}"
    out = SPU / name
    src = out / "spu_recomp.c"
    if src.exists() and not relift:
        return {"name": name, "lifted": False}
    out.mkdir(parents=True, exist_ok=True)
    py = sys.executable
    funcs = out / "funcs.json"
    elf = spu_elf(dump.read_bytes())
    if elf:
        # A task ELF: functions and jump tables in LS addresses of its code segment.
        _, code_va, code = elf
        found = run([py, str(TOOLS / "find_spu_functions.py"), "--out", str(funcs), str(dump)])
        tables = [code_va + t for t in rel_jump_table_targets(code)]
        where = ["--auto-functions", str(dump)]
    else:
        found = run([py, str(TOOLS / "find_spu_functions.py"), "--raw", "--base", "0",
                     "--out", str(funcs), str(dump)])
        tables = rel_jump_table_targets(dump.read_bytes())
        where = ["--base", "0", "--functions", str(funcs)]
    extra = sorted(set(EXTRA_ENTRIES.get(fp, [])) | set(tables))
    log = run([py, str(TOOLS / "spu_lifter.py")] + where +
              ["--symbol-prefix", f"{name}_", "-o", str(out), str(dump)]
              + (["--extra-funcs", ",".join(f"0x{a:X}" for a in extra)] if extra else []))
    for f in (out / "spu_recomp.c", out / "spu_recomp.h"):
        text = re.sub(r'#include "(?:\.\./)+runtime/spu/([^"]+)"', r'#include "\1"', f.read_text())
        f.write_text(text)
    cov = re.search(r"Function coverage: ([^\n]+)", found)
    nfun = re.search(r"(\d+) function\(s\) lifted", log)
    unsup = re.findall(r"^\s+(\S+)\s+(\d+)$", log.split("unsupported", 1)[-1], re.M) if "unsupported" in log else []
    return {"name": name, "lifted": True, "table_entries": len(extra),
            "functions": int(nfun.group(1)) if nfun else None,
            "coverage": cov.group(1) if cov else None,
            "unsupported": {m: int(n) for m, n in unsup}}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dumps", type=Path, default=SPU / "dumps")
    ap.add_argument("--relift", action="store_true", help="lift every dump again")
    ap.add_argument("--only", type=lambda x: int(x, 16), action="append", default=[],
                    metavar="FP", help="with --relift: re-lift only this fingerprint (hex)")
    a = ap.parse_args()

    dumps = sorted(p for p in a.dumps.glob("spujob_*.bin") if DUMP_RE.search(p.name))
    if not dumps:
        sys.exit(f"no spujob_*.bin in {a.dumps}. Capture them with SPU_DUMP_MISS first.")

    images = []
    for d in dumps:
        m = DUMP_RE.search(d.name)
        fp, size = int(m.group(1), 16), int(m.group(2))
        data = d.read_bytes()
        if len(data) != size or fnv1a64(data) != fp:
            sys.exit(f"{d.name}: bytes do not match the name "
                     f"(size {len(data)}, fnv1a64 {fnv1a64(data):016X}); re-dump it.")
        r = lift(d, fp, a.relift and (not a.only or fp in a.only))
        r.update(fp=fp, size=size, dump=d.name, entry=entry_of(d))
        images.append(r)
        if r["lifted"]:
            extra = "" if not r["unsupported"] else f", unsupported {r['unsupported']}"
            print(f"lifted {r['name']}: {r['functions']} functions ({r['table_entries']} jump-table/extra "
                  f"entries), coverage {r['coverage']}{extra}")
        else:
            print(f"kept   {r['name']} (already lifted; --relift to redo)")

    # Image ids start at 1; 0 is "no image" (spu_begin_image(0) ends the run).
    lines = [
        "/* Tokyo Jungle SPU workload registry -- GENERATED by scripts/lift_spu.py.",
        " * Do not edit; re-run the script after capturing new dumps.",
        " *",
        " * The runtime fingerprints each job image the game hands to SPURS and runs",
        " * the lifted entry registered here. An unregistered image is a dispatch",
        " * MISS: logged, and the job does nothing. */",
        '#include "spu_workload.h"',
        "",
        "extern void spu_begin_image(int image_id);",
        "",
    ]
    for im in images:
        lines += [f"extern void {im['name']}_spu_func_{im['entry']:08X}(spu_context*);",
                  f"extern void {im['name']}_spu_recomp_register(void);"]
    lines += ["", "void tj_spu_register_all(void)", "{"]
    for i, im in enumerate(images, 1):
        label = KNOWN.get(im["fp"])
        lines.append(f"    /* {im['dump']}{' -- ' + label if label else ''} */")
        lines += [f"    spu_begin_image({i});",
                  f"    {im['name']}_spu_recomp_register();",
                  f"    spu_workload_register_img(0x{im['fp']:016X}ULL, "
                  f"{im['name']}_spu_func_{im['entry']:08X}, {i}, \"{im['name']}\");", ""]
    lines += ["    spu_begin_image(0);", "}", "",
              "/* Register before the game creates any SPURS workload, task or job chain. */",
              "__attribute__((constructor)) static void tj_spu_register_all_ctor(void)",
              "{", "    tj_spu_register_all();", "}", ""]
    (SPU / "spu_workloads.c").write_text("\n".join(lines))
    (SPU / "images.json").write_text(json.dumps(images, indent=1))
    print(f"\nregistered {len(images)} image(s) -> {SPU / 'spu_workloads.c'}\n"
          f"re-run cmake (a configure picks up new image dirs), then build.")


if __name__ == "__main__":
    main()
