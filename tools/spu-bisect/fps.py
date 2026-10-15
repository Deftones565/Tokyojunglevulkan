#!/usr/bin/env python3
"""fps.py <frames dir> [from_flip] [to_flip]: guest flips per wall second from a
frame dump (frame_NNNNNN_flipFFFFFF.ppm; file mtimes give the wall time). The
default window is the whole dump; pass a flip range to measure only gameplay."""
import os, re, sys
d = sys.argv[1]; lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0; hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
pts = []
for f in os.listdir(d):
    m = re.match(r'frame_\d+_flip(\d+)\.ppm$', f)
    if m and lo <= int(m.group(1)) <= hi:
        pts.append((os.path.getmtime(os.path.join(d, f)), int(m.group(1))))
pts.sort()
if len(pts) < 2: sys.exit('not enough frames in range')
(t0, f0), (t1, f1) = pts[0], pts[-1]
print(f'{d}: flips {f0}..{f1} over {t1 - t0:.1f} s = {(f1 - f0) / (t1 - t0):.2f} flips/s ({len(pts)} frames)')
