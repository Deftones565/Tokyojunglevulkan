#!/usr/bin/env python3
"""Per-thread CPU summary for tools/prof/sigprof.c samples.

usage: threads.py <prof.bin> <binary> [--seconds S] [--hz HZ] [--top N]

Groups samples by thread id, labels each thread by the outermost frames of its
stacks that are in the binary (its entry function), and merges threads with the
same label (short-lived SPURS walker threads get a new id per run). With the
sampling window length (--seconds, PROF_END_S - PROF_START_S) it prints how many
cores each kind of thread used, plus its top self functions."""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof'); ap.add_argument('binary')
    ap.add_argument('--seconds', type=float, default=60.0)
    ap.add_argument('--hz', type=float, default=997.0)
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
    if mine:
        out = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary] + ['%x' % (o - 1) for o in mine],
                             capture_output=True, text=True).stdout.splitlines()
        for i, o in enumerate(mine):
            names[o] = out[2 * i] if 2 * i < len(out) else '??'

    def name(pc):
        p, o = offs[pc]
        if p and os.path.realpath(p) == binreal: return names.get(o, '??')
        return '[%s]' % (os.path.basename(p) if p else '?')

    per_tid = collections.defaultdict(list)
    for tid, st in recs:
        per_tid[tid].append([name(pc) for pc in st[2:]])
    # label: most common outermost binary frame of the thread
    groups = collections.defaultdict(lambda: [0, collections.Counter(), set()])
    for tid, stacks in per_tid.items():
        roots = collections.Counter()
        for s in stacks:
            ours = [f for f in s if not f.startswith('[')]
            if ours: roots[ours[-1] if len(ours) < 2 else ours[-2] + ' <- ' + ours[-1]] += 1
        label = roots.most_common(1)[0][0] if roots else '[no binary frames]'
        g = groups[label]
        g[0] += len(stacks); g[2].add(tid)
        for s in stacks:
            if s: g[1][s[0]] += 1
    total = sum(g[0] for g in groups.values())
    print('samples %d, window %.0f s -> %.2f cores busy' % (total, a.seconds, total / a.hz / a.seconds))
    for label, (n, selfc, tids) in sorted(groups.items(), key=lambda kv: -kv[1][0]):
        print('\n%6d %5.1f%% %.2f cores  %d thread(s)  %s' % (n, 100.0 * n / total, n / a.hz / a.seconds, len(tids), label))
        for f, c in selfc.most_common(a.top):
            print('         %5.1f%%  %s' % (100.0 * c / n, f))


if __name__ == '__main__':
    main()
