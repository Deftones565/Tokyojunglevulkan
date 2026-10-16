#!/usr/bin/env python3
"""What each thread is doing, by wall time (tools/prof/sigprof.c PROF_WALL_HZ samples).

usage: waits.py <prof.bin> <binary> [--hz 200] [--depth 3] [--min-pct 2] [--filter FUNC]

Per thread id: samples (= seconds * hz), and its most common "first frames in the
binary" (the innermost DEPTH frames of our own code under any libc/libm wait),
which names what a blocked thread is waiting in."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof'); ap.add_argument('binary')
    ap.add_argument('--hz', type=float, default=200.0)
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--min-pct', type=float, default=2.0)
    ap.add_argument('--filter')
    ap.add_argument('--as', dest='as_path', help='path the profiled binary ran from (when BINARY is a copy)')
    a = ap.parse_args()
    maps, recs = report.load(a.prof)
    # --as: the path the profiled process ran the binary from, when `binary`
    # is a saved copy of it (drive_gameplay.py keeps one per run).
    binreal = os.path.realpath(a.as_path or a.binary)

    def to_off(pc):
        for lo, hi, off, path in maps:
            if lo <= pc < hi: return (path, pc - lo + off)
        return (None, pc)
    offs = {pc: to_off(pc) for pc in set(pc for _, st in recs for pc in st)}
    mine = sorted(set(o for p, o in offs.values() if p and os.path.realpath(p) == binreal))
    names = {}
    if mine:
        out = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary] + ['%x' % (o - 1) for o in mine],
                             capture_output=True, text=True).stdout.splitlines()
        for i, o in enumerate(mine): names[o] = out[2 * i] if 2 * i < len(out) else '??'

    def name(pc):
        p, o = offs[pc]
        if p and os.path.realpath(p) == binreal: return names.get(o, '??')
        return None
    per = collections.defaultdict(collections.Counter)
    for tid, st in recs:
        ours = [n for n in (name(pc) for pc in st[2:]) if n]
        if a.filter and a.filter not in ours: continue
        key = ' < '.join(ours[:a.depth]) if ours else '[outside binary]'
        per[tid][key] += 1
    for tid, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(c.values())
        print('\ntid %d: %d samples (%.1f s)' % (tid, n, n / a.hz))
        for k, v in c.most_common(8):
            if 100.0 * v / n < a.min_pct: break
            print('   %5.1f%%  %s' % (100.0 * v / n, k))


if __name__ == '__main__':
    main()
