#!/usr/bin/env python3
"""Where each frame's time goes, from a TJ_FRAME_TRACE file (private diag in
cellGcmSys.c: "<ns> <tid> <ev> <arg>" lines).

usage: frame_trace.py <trace> [--from-s S] [--slow-ms 17.5]

Tokyo Jungle's main thread ends a frame with
    do usleep(100) while (cellGcmGetFlipStatus() != DONE);   (G .. D)
    cellGcmResetFlipStatus(); _cellGcmSetFlipCommandWithWaitLabel(...)  (R, F)
so a frame is R..R on the main thread (the tid that calls R). Per frame:
  main_wait_rsx  w..W on the main thread (0055 guard before a job chain)
  main_wait_ef   e..E (SPURS event flag waits: SPU jobs)
  main_wait_join j..J (job chain joins)
  flip_wait      G..R (frame done, waiting for the previous flip)
  main_run       the rest (guest code + HLE work)
and for the flip F issued at the frame's start: F->M (drain reaches its
marker), M->T (next vblank tick that completes it), T->D (delivery).
Drain thread busy (p..P minus kick waits k..K) and kick waits per frame too."""
import argparse, bisect, collections, statistics, sys

ap = argparse.ArgumentParser()
ap.add_argument('trace'); ap.add_argument('--from-s', type=float, default=40.0)
ap.add_argument('--slow-ms', type=float, default=17.5)
a = ap.parse_args()

ev = []
for line in open(a.trace, errors='replace'):
    p = line.split()
    if len(p) != 4: continue
    try: ev.append((int(p[0]), int(p[1]), p[2], int(p[3])))
    except ValueError: pass
if not ev: sys.exit('empty trace')
t0 = ev[0][0]
main = next(t for _, t, e, _ in ev if e == 'R')
R = [t for t, tid, e, _ in ev if e == 'R']
start_ns = t0 + int(a.from_s * 1e9)

def intervals(open_ev, close_ev, tid=None):
    out, cur = [], {}
    for t, th, e, _ in ev:
        if tid is not None and th != tid: continue
        if e == open_ev: cur[th] = t
        elif e == close_ev and th in cur: out.append((cur.pop(th), t))
    return out

def overlap(iv, lo, hi):
    s = 0
    for x, y in iv:
        if y <= lo or x >= hi: continue
        s += min(y, hi) - max(x, lo)
    return s

def sorted_iv(iv):
    iv.sort(); return iv

def ov(iv, starts, lo, hi):
    i = bisect.bisect_left(starts, lo) - 1
    s = 0
    for x, y in iv[max(i, 0):]:
        if x >= hi: break
        if y <= lo: continue
        s += min(y, hi) - max(x, lo)
    return s

W = {k: sorted_iv(intervals(o, c, main)) for k, (o, c) in
     {'rsx': ('w', 'W'), 'ef': ('e', 'E'), 'join': ('j', 'J')}.items()}
drain_tid = next((th for _, th, e, _ in ev if e == 'p'), None)
P = sorted_iv(intervals('p', 'P', drain_tid)); K = sorted_iv(intervals('k', 'K'))
S = sorted_iv(intervals('s', 'S'))
starts = {k: [x for x, _ in v] for k, v in W.items()}
Ps, Ks, Ss = [x for x, _ in P], [x for x, _ in K], [x for x, _ in S]
G = [t for t, th, e, _ in ev if e == 'G' and th == main]
F = [t for t, th, e, _ in ev if e == 'F' and th == main]
M = [t for t, _, e, _ in ev if e == 'M']
T1 = [t for t, _, e, x in ev if e == 'T' and x == 1]
D = [t for t, _, e, _ in ev if e == 'D']

def nxt(lst, t):
    i = bisect.bisect_left(lst, t)
    return lst[i] if i < len(lst) else None

rows = []
for i in range(len(R) - 1):
    lo, hi = R[i], R[i + 1]
    if lo < start_ns: continue
    tot = hi - lo
    g = nxt(G, lo); g = g if g is not None and g < hi else hi
    r = {'t': (lo - t0) / 1e9, 'total': tot / 1e6}
    for k in W: r['wait_' + k] = ov(W[k], starts[k], lo, g) / 1e6
    r['flip_wait'] = (hi - g) / 1e6
    r['main_run'] = r['total'] - r['flip_wait'] - sum(r['wait_' + k] for k in W)
    f = nxt(F, lo); m = nxt(M, f) if f else None
    tk = nxt(T1, m) if m else None; d = nxt(D, tk) if tk else None
    r['F_M'] = (m - f) / 1e6 if m else float('nan')
    r['M_T'] = (tk - m) / 1e6 if tk else float('nan')
    r['T_D'] = (d - tk) / 1e6 if d else float('nan')
    busy = ov(P, Ps, lo, hi); kw = ov(K, Ks, lo, hi); pw = ov(S, Ss, lo, hi)
    r['drain_busy'] = (busy - kw) / 1e6; r['kick_wait'] = kw / 1e6; r['present'] = pw / 1e6
    rows.append(r)

cols = ['total', 'main_run', 'wait_rsx', 'wait_ef', 'wait_join', 'flip_wait',
        'F_M', 'M_T', 'T_D', 'drain_busy', 'kick_wait', 'present']
def summary(name, rs):
    if not rs: print('%s: none' % name); return
    print('%s: %d frames' % (name, len(rs)))
    for c in cols:
        v = [r[c] for r in rs if r[c] == r[c]]
        if v: print('  %-11s mean %6.2f  median %6.2f  p90 %6.2f ms' %
                    (c, statistics.mean(v), statistics.median(v), sorted(v)[int(len(v) * .9)]))
slow = [r for r in rows if r['total'] > a.slow_ms]
fast = [r for r in rows if r['total'] <= a.slow_ms]
print('frames from t=%.0fs: %d, slow (> %.1f ms): %d (%.0f%%)' %
      (a.from_s, len(rows), a.slow_ms, len(slow), 100.0 * len(slow) / max(1, len(rows))))
summary('FAST', fast); summary('SLOW', slow)
hist = collections.Counter(min(int(r['total'] / (1001 / 60.0) + 0.5), 6) for r in rows)
print('vblanks per frame:', ' '.join('%d:%d' % (k, hist[k]) for k in sorted(hist)))
