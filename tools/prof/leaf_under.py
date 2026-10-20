#!/usr/bin/env python3
"""Where samples under FUNC end up, library frames included.

usage: leaf_under.py <prof.bin> <binary> FUNC [--tid-has FUNC2] [--depth N]

For every wall-clock sample (tools/prof/sigprof.c) whose stack contains FUNC
(a function in the binary), prints the most common chains from the leaf up to
FUNC: library frames as lib+offset (resolved with addr2line against the
library when it has symbols), binary frames by name. Answers "what is FUNC
waiting on" when the time is spent in a driver or libc."""
import argparse, collections, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('prof'); ap.add_argument('binary'); ap.add_argument('func')
ap.add_argument('--tid-has'); ap.add_argument('--depth', type=int, default=6)
ap.add_argument('--top', type=int, default=15)
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
bylib = collections.defaultdict(set)
for p, o in offs.values():
    if p: bylib[p].add(o)
names = {}
for path, os_ in bylib.items():
    os_ = sorted(os_)
    for i in range(0, len(os_), 20000):
        chunk = os_[i:i + 20000]
        out = subprocess.run(['addr2line', '-f', '-C', '-e', path] + ['%x' % (o - 1) for o in chunk],
                             capture_output=True, text=True).stdout.splitlines()
        short = os.path.basename(path)
        for k, o in enumerate(chunk):
            n = out[2 * k] if 2 * k < len(out) else '??'
            names[(path, o)] = (n.split('(')[0] if n != '??' else '%s+%x' % (short, o)) \
                if os.path.realpath(path) == binreal else \
                ('%s:%s' % (short, n.split('(')[0]) if n != '??' else '%s+%x' % (short, o))

def frames(st):
    return [names.get(offs[pc], '?') for pc in st[2:]]
tid_ok = None
if a.tid_has:
    tid_ok = set(t for t, st in recs if a.tid_has in frames(st))
c = collections.Counter(); n = 0; tot = 0
for t, st in recs:
    if tid_ok is not None and t not in tid_ok: continue
    tot += 1
    f = frames(st)
    if a.func not in f: continue
    n += 1
    c[' <- '.join(f[:min(f.index(a.func) + 1, a.depth)])] += 1
print('%d samples under %s (of %d in the selected threads)' % (n, a.func, tot))
for s, k in c.most_common(a.top):
    print('  %5.1f%%  %s' % (100.0 * k / max(1, n), s))
