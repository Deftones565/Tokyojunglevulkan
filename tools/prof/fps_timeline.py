#!/usr/bin/env python3
"""fps_timeline.py <frames dir> [--step N] [--from-flip F]: guest flips per wall
second over time from a frame dump (frame_NNNNNN_flipFFFFFF.ppm, file mtimes =
wall time), one line per N dumped frames, then the mean over flips >= F
(default 4200: the Survival tutorial street with STATUS.md's 11-press script)."""
import argparse, os, re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--step', type=int, default=40)
    ap.add_argument('--from-flip', type=int, default=4200)
    a = ap.parse_args()
    pts = []
    for f in os.listdir(a.dir):
        m = re.match(r'frame_\d+_flip(\d+)\.ppm$', f)
        if m:
            pts.append((os.path.getmtime(os.path.join(a.dir, f)), int(m.group(1))))
    pts.sort()
    if len(pts) < 2:
        print('not enough frames'); return
    t00 = pts[0][0]
    for i in range(0, len(pts) - 1, a.step):
        (t, f), (t2, f2) = pts[i], pts[min(i + a.step, len(pts) - 1)]
        print(f't={t - t00:6.1f}s flips {f:6d}..{f2:6d}: {(f2 - f) / max(t2 - t, 1e-9):5.1f} flips/s')
    g = [p for p in pts if p[1] >= a.from_flip]
    if len(g) >= 2:
        (t, f), (t2, f2) = g[0], g[-1]
        print(f'gameplay flips {f}..{f2} over {t2 - t:.1f} s = {(f2 - f) / (t2 - t):.2f} flips/s')


if __name__ == '__main__':
    main()
