#!/usr/bin/env python3
"""Fail if anything in the port could emulate the PS3 at run time.

This is a static recompilation (AGENTS.md, "Hard rule"): game and firmware
code is lifted ahead of time, and nothing may interpret, JIT or single-step
guest instructions, not even as a fallback. ps3recomp ships an SPU
interpreter; this project builds it with PS3RECOMP_NO_INTERP so every entry
into it exits with the unlifted address. This script checks that this is
still true, so the rule doesn't depend on reviewers remembering it:

  1. CMakeLists.txt still defines PS3RECOMP_NO_INTERP=1 on the runtime.
  2. Every function in the runtime that can execute guest bytes still opens
     with the PS3RECOMP_NO_INTERP fatal guard.
  3. No other function that interprets, JITs or steps guest code has appeared
     in the runtime, the HLE libraries or src/ (names are matched against an
     allowlist of the guarded ones above; extend it only for a function that
     really doesn't run guest code, and say why).
  4. Nothing maps host memory executable at run time beyond the known
     guest-permission helpers (a JIT needs that).
  5. With --binary: the built executable carries the guard's message, i.e.
     the guard was really compiled in.

Run it before approving or landing any patch, and the build runs it after
every link of the game (CMakeLists.txt, POST_BUILD). Exit status 1 = violation.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Functions that execute guest bytes. Each must start with the NO_INTERP guard.
GUARDED = {
    "runtime/spu/spu_interp.c": ["spu_interp_run_until"],
    "runtime/spu/spu_channels.c": ["spu_smc_microstep"],
}
GUARD_CALL = "spu_no_interp_fatal"
GUARD_MESSAGE = b"Interpretation is disabled (PS3RECOMP_NO_INTERP)"

# Functions whose names look like an interpreter/JIT/stepper but are known to
# be fine: they only reach guest code through the guarded functions above, or
# aren't about guest code at all.
ALLOWED_NAMES = {
    "spu_interp_run",            # -> spu_interp_run_until (guarded)
    "spu_interp_run_until",      # guarded
    "spu_smc_microstep",         # guarded
    "spu_no_interp_fatal",       # the guard itself
    "spu_interp_fallback",       # -> spu_interp_run (guarded)
    "spu_run_interp_job",        # -> spu_interp_run (guarded)
    "cellRescSetPalInterpolateDropFlexRatio",  # video scaler API, not code
    "eng_format_reinterprets_rgba8",           # texture format helper
}
SUSPECT_NAME = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b(\w*(?:interp|jit|emulat|microstep|single_?step|"
    r"step_insn|decode_and_exec|exec_insn)\w*)\s*\([^;]*$",
    re.IGNORECASE | re.MULTILINE)

# Files allowed to talk about executable host memory (guest page permission
# mirroring, not code generation).
EXEC_MEM_ALLOWED = {
    "runtime/memory/vm.h",
    "runtime/platform/win32_compat.c",
    "runtime/platform/win32_compat.h",   # the PAGE_* constant definitions
}
EXEC_MEM = re.compile(r"\b(PROT_EXEC|MAP_JIT|PAGE_EXECUTE\w*|VM_PROT_EXECUTE)\b")

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc")


def sources(top):
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = [d for d in dirnames if d not in ("tests", ".git")]
        for f in filenames:
            if f.endswith(SOURCE_EXT):
                yield os.path.join(dirpath, f)


def function_head(text, name, lines=8):
    """The first `lines` lines of the body of function `name`, or None."""
    m = re.search(r"^[^\n;]*\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), text, re.MULTILINE)
    if not m:
        return None
    return "\n".join(text[m.end():].splitlines()[:lines])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ps3recomp", default=os.path.join(ROOT, "third_party", "ps3recomp"),
                    help="ps3recomp tree the build uses (default: the submodule)")
    ap.add_argument("--binary", help="built game executable to check for the guard")
    args = ap.parse_args()

    errors = []
    rt = args.ps3recomp

    # 1. The build still turns the interpreter into a fatal error.
    cml = open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read()
    if not re.search(r"^\s*target_compile_definitions\(\s*ps3recomp_runtime\b[^)]*"
                     r"\bPS3RECOMP_NO_INTERP=1\b", cml, re.MULTILINE):
        errors.append("CMakeLists.txt no longer defines PS3RECOMP_NO_INTERP=1 on "
                      "ps3recomp_runtime")

    # 2. Every guest-executing function still opens with the guard.
    for rel, names in GUARDED.items():
        path = os.path.join(rt, rel)
        if not os.path.exists(path):
            errors.append("%s is missing; update GUARDED in this script if it moved" % rel)
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        for name in names:
            head = function_head(text, name)
            if head is None:
                errors.append("%s: function %s not found; update GUARDED if it was "
                              "renamed" % (rel, name))
            elif "PS3RECOMP_NO_INTERP" not in head or GUARD_CALL not in head:
                errors.append("%s: %s no longer starts with the PS3RECOMP_NO_INTERP "
                              "guard (%s)" % (rel, name, GUARD_CALL))

    # 3 + 4. No new interpreter/JIT/stepper, no executable host memory.
    for top, prefix in ((rt, ""), (os.path.join(ROOT, "src"), "src/")):
        for path in sources(top):
            rel = prefix + os.path.relpath(path, top).replace(os.sep, "/")
            if rel.startswith(("tools/", "third_party/")):
                continue
            text = open(path, encoding="utf-8", errors="replace").read()
            for m in SUSPECT_NAME.finditer(text):
                name = m.group(1)
                if name not in ALLOWED_NAMES:
                    line = text.count("\n", 0, m.start()) + 1
                    errors.append("%s:%d: function '%s' looks like an interpreter/JIT/"
                                  "stepper; if it doesn't run guest code, add it to "
                                  "ALLOWED_NAMES with a reason" % (rel, line, name))
            if rel not in EXEC_MEM_ALLOWED:
                for m in EXEC_MEM.finditer(text):
                    line = text.count("\n", 0, m.start()) + 1
                    errors.append("%s:%d: executable host memory (%s): JIT is not "
                                  "allowed" % (rel, line, m.group(1)))

    # 5. The guard is really in the binary.
    if args.binary:
        with open(args.binary, "rb") as f:
            if GUARD_MESSAGE not in f.read():
                errors.append("%s does not contain the PS3RECOMP_NO_INTERP guard: the "
                              "interpreter may be live" % args.binary)

    if errors:
        print("check_no_emulation: FAILED (recompilation only, no emulation; see AGENTS.md)",
              file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    print("check_no_emulation: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
