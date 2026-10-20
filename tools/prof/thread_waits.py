#!/usr/bin/env python3
"""Where each thread spends its wall-clock time, for tools/prof/sigprof.c
samples taken with PROF_WALL_HZ (every thread sampled, busy or blocked).

usage: thread_waits.py <prof.bin> <binary> [--has FUNC] [--depth N] [--top N]

For each thread (optionally only threads whose stacks contain FUNC, e.g.
ppu_host_thread_proc for the game's PPU threads) prints its sample count and
its most common "signatures": the first N frames of the stack that are in the
binary, counted from the leaf (so a thread blocked in libc shows the call
chain that blocked it: sys_event_flag_wait <- func_0017B354 <- ...)."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof'); ap.add_argument('binary')
    ap.add_argument('--has'); ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--top', type=int, default=6)
    a = ap.parse_args()
    maps, recs = report.load(a.prof)
    binreal = os.path.realpath(a.binary)

    def to_off(pc):
        for lo, hi, off, path in maps:
            if lo <= pc < hi:
                return (path, pc - lo + off)
        return (None, pc)
    pcs = set(pc for _, st in recs for pc in st)
    offs = {pc: to_off(pc) for pc in pcs}
    mine = sorted(set(o for p, o in offs.values() if p and os.path.realpath(p) == binreal))
    names = {}
    for i in range(0, len(mine), 20000):
        chunk = mine[i:i + 20000]
        out = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary] + ['%x' % (o - 1) for o in chunk],
                             capture_output=True, text=True).stdout.splitlines()
        for k, o in enumerate(chunk):
            names[o] = out[2 * k] if 2 * k < len(out) else '??'

    def frames(st):
        out = []
        for pc in st[2:]:
            p, o = offs[pc]
            if p and os.path.realpath(p) == binreal:
                out.append(names.get(o, '??').split('(')[0])
        return out
    per = collections.defaultdict(list)
    for tid, st in recs:
        per[tid].append(frames(st))
    rows = []
    for tid, stacks in per.items():
        if a.has and not any(a.has in s for st in stacks for s in st):
            continue
        rows.append((len(stacks), tid, stacks))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print('threads %d, samples %d' % (len(rows), total))
    for n, tid, stacks in rows:
        sig = collections.Counter(' <- '.join(s[:a.depth]) or '[no binary frames]' for s in stacks)
        print('\ntid %d: %d samples' % (tid, n))
        for s, c in sig.most_common(a.top):
            print('  %5.1f%%  %s' % (100.0 * c / n, s))


if __name__ == '__main__':
    main()
