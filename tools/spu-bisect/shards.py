#!/usr/bin/env python3
"""Find one-frame geometry glitches ("exploding" models, shards) in a dense frame dump.

usage: shards.py <frames dir> [--from FLIP] [--to FLIP] [--thresh 0.15] [--diff 48] [--agree 16] [--top 15]

Needs consecutive dumps (TJ_FRAME_DUMP_EVERY=1). For each frame f with neighbours p and n:
a pixel is a glitch candidate when |f-p| > DIFF and |f-n| > DIFF while |p-n| < AGREE, i.e. the
frame differs from both neighbours, which agree with each other. Camera motion and animation
move all three frames; a one-frame shard or exploded mesh does not. Prints the frames whose
candidate share (in %) exceeds THRESH, worst first, and writes nothing.
(Idea: CASPER-3, 2026-10-16.)"""
import argparse, os, re
import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('L'), dtype=np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=1 << 30)
    ap.add_argument('--thresh', type=float, default=0.15)
    ap.add_argument('--diff', type=int, default=48)
    ap.add_argument('--agree', type=int, default=16)
    ap.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    fs = []
    for f in os.listdir(a.dir):
        m = re.search(r'flip(\d+)', f)
        if m and a.lo <= int(m.group(1)) <= a.hi: fs.append((int(m.group(1)), f))
    fs.sort()
    hits, n = [], 0
    prev = cur = None
    for i, (flip, f) in enumerate(fs):
        nxt = load(os.path.join(a.dir, f))
        if prev is not None and cur is not None and fs[i - 1][0] - fs[i - 2][0] <= 2 and flip - fs[i - 1][0] <= 2:
            m = (np.abs(cur - prev) > a.diff) & (np.abs(cur - nxt) > a.diff) & (np.abs(prev - nxt) < a.agree)
            pct = 100.0 * m.mean()
            n += 1
            if pct > a.thresh:
                ys, xs = np.nonzero(m)
                hits.append((pct, fs[i - 1][0], fs[i - 1][1], xs.min(), ys.min(), xs.max(), ys.max()))
        prev, cur = cur, nxt
    hits.sort(reverse=True)
    print('%d frames checked, %d flagged (> %.2f%% glitch pixels)' % (n, len(hits), a.thresh))
    for pct, flip, f, x0, y0, x1, y1 in hits[:a.top]:
        print('  flip %6d  %5.2f%%  bbox %d,%d-%d,%d  %s' % (flip, pct, x0, y0, x1, y1, f))


if __name__ == '__main__':
    main()
