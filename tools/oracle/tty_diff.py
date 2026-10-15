#!/usr/bin/env python3
"""
Find where the recomp's guest output diverges from RPCS3's.

  tools/oracle/tty_diff.py build-oracle/<run>/TTY.log recomp.log

RPCS3 keeps the guest's sys_tty_write output in its own TTY.log. The recomp
writes the same bytes raw to stderr, interleaved with the runtime's own
logging, so a plain diff is useless. Instead, walk RPCS3's TTY lines in order
and match each one against the recomp log in order (a subsequence match). The
first line that never appears is where the two runs part ways; everything
before it, the recomp reproduced.

Numbers are ignored by default. Guest addresses, ids and handles legitimately
differ between RPCS3 and the recomp (RPCS3: "created SOC job (30258180,20480,
8D008600,17)", recomp: "created SOC job (40246480,20480,3,16)"), so every hex
or decimal number is normalised to '#' before matching. --exact turns that off.

Several guest threads print, so two lines can come out in either order (the
audio thread's "start audio thread" vs "start audio write loop"). A line may
therefore match up to --window recomp lines BEFORE the previous match, as
long as that recomp line wasn't used already. --window 0 is strictly in order.

Exit status: 0 if every TTY line was matched, 1 at the first missing one.
"""

import argparse
import re
import sys
from pathlib import Path


NUM = re.compile(r"\b(?:0[xX])?[0-9A-Fa-f]*[0-9][0-9A-Fa-f]*\b")


def norm(line: str) -> str:
    return NUM.sub("#", line.strip())


def lines(path: Path) -> list[str]:
    text = path.read_bytes().decode("utf-8", errors="replace").lstrip("﻿")
    return [ln.rstrip() for ln in text.splitlines()]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("oracle_tty", type=Path, help="RPCS3's TTY.log")
    ap.add_argument("recomp_log", type=Path, help="the recomp's stderr (or combined) log")
    ap.add_argument("-c", "--context", type=int, default=3,
                    help="matched lines to show before the divergence (default 3)")
    ap.add_argument("-w", "--window", type=int, default=20,
                    help="recomp lines a match may sit before the previous one, "
                         "for output from different threads (default 20; 0 = strict)")
    ap.add_argument("--exact", action="store_true",
                    help="compare numbers too (by default they are normalised away)")
    a = ap.parse_args()
    key = (lambda s: s.strip()) if a.exact else norm

    want = [ln for ln in lines(a.oracle_tty) if ln.strip()]
    have = lines(a.recomp_log)
    have_k = [key(h) for h in have]
    if not want:
        print(f"{a.oracle_tty} has no TTY output; nothing to compare.")
        return 0

    pos = 0
    used: set[int] = set()
    matched: list[tuple[int, int, str]] = []   # (oracle line no, recomp line no, text)
    for i, w in enumerate(want):
        # The recomp may print a TTY line inside a longer log line, so match by
        # containment rather than equality.
        wk = key(w)
        j = next((k for k in range(max(0, pos - 1 - a.window), len(have))
                  if k not in used and wk in have_k[k]), None)
        if j is None:
            print(f"matched {i} of {len(want)} RPCS3 TTY lines, in order.")
            for oi, rj, text in matched[-a.context:]:
                print(f"  ok  tty:{oi + 1:<6} recomp:{rj + 1:<7} {text}")
            print(f"  --  tty:{i + 1:<6} MISSING        {w}")
            later = next((k for k in range(len(have)) if wk in have_k[k]), None)
            if later is not None:
                print(f"      (it does appear at recomp line {later + 1}, but out of order)")
            return 1
        matched.append((i, j, w))
        used.add(j)
        pos = max(pos, j + 1)

    print(f"all {len(want)} RPCS3 TTY lines appear in the recomp log, in order.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
