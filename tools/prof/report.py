#!/usr/bin/env python3
"""Report for tools/prof/sigprof.c samples.

usage: report.py <prof.bin> <binary> [--thread-has FUNC] [--top N] [--under FUNC]

  --thread-has FUNC  keep only samples whose stack contains FUNC (e.g. the
                     FIFO drain: cellGcm_rsx_process_fifo)
  --under FUNC       report callees as seen under FUNC (inclusive time per
                     function counted only within FUNC's subtree)
Prints self and inclusive sample counts per function (symbolised with
addr2line, inlined frames folded into their caller)."""
import argparse, collections, os, struct, subprocess, sys


def load(path):
    d = open(path, 'rb').read()
    a = d.index(b'MAPS\n') + 5; b = d.index(b'\nEND\n', a)
    maps = []
    for line in d[a:b].decode(errors='replace').splitlines():
        parts = line.split()
        if len(parts) < 6: continue
        lo, hi = (int(x, 16) for x in parts[0].split('-'))
        maps.append((lo, hi, int(parts[2], 16), parts[5]))
    p = b + 5; recs = []
    while p + 8 <= len(d):
        tid, n = struct.unpack_from('<II', d, p); p += 8
        if p + 8 * n > len(d): break
        recs.append((tid, struct.unpack_from('<%dQ' % n, d, p))); p += 8 * n
    return maps, recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof'); ap.add_argument('binary')
    ap.add_argument('--thread-has'); ap.add_argument('--under'); ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--as', dest='as_path', help='path the profiled binary ran from (when BINARY is a copy)')
    a = ap.parse_args()
    maps, recs = load(a.prof)
    # --as: the path the profiled process ran the binary from, when `binary`
    # is a saved copy of it (drive_gameplay.py keeps one per run).
    binreal = os.path.realpath(a.as_path or a.binary)
    def to_off(pc):
        for lo, hi, off, path in maps:
            if lo <= pc < hi:
                return (path, pc - lo + off)
        return (None, pc)
    pcs = set(pc for _, st in recs for pc in st)
    offs = {pc: to_off(pc) for pc in pcs}
    mine = sorted(set(o for p, o in offs.values() if p and os.path.realpath(p) == binreal))
    names = {}
    if mine:
        # return addresses point after the call; -1 lands inside it
        out = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary] + ['%x' % (o - 1) for o in mine],
                             capture_output=True, text=True).stdout.splitlines()
        for i, o in enumerate(mine):
            names[o] = out[2 * i] if 2 * i < len(out) else '??'
    def name(pc):
        p, o = offs[pc]
        if p and os.path.realpath(p) == binreal: return names.get(o, '??')
        return '[%s]' % (os.path.basename(p) if p else '?')
    stacks = []
    for tid, st in recs:
        fr = [name(pc) for pc in st[2:]]       # drop the handler and the signal trampoline
        stacks.append(fr)
    if a.thread_has:
        stacks = [s for s in stacks if a.thread_has in s]
    if a.under:
        stacks = [s[:s.index(a.under) + 1] for s in stacks if a.under in s]
    total = len(stacks)
    print('samples: %d of %d%s' % (total, len(recs), ' (filtered)' if total != len(recs) else ''))
    selfc = collections.Counter(s[0] for s in stacks if s)
    incl = collections.Counter()
    for s in stacks:
        for f in set(s): incl[f] += 1
    print('\n-- self --')
    for f, c in selfc.most_common(a.top): print('%6d %5.1f%%  %s' % (c, 100.0 * c / max(total, 1), f))
    print('\n-- inclusive --')
    for f, c in incl.most_common(a.top): print('%6d %5.1f%%  %s' % (c, 100.0 * c / max(total, 1), f))


if __name__ == '__main__':
    main()
