#!/usr/bin/env python3
"""tile_seams.py <frames dir> [tile=128]: per frame, the mean colour jump across the
tile grid's lines (x, y = k*tile) over the mean jump between neighbouring pixels
elsewhere. ~1.0 in a clean frame; tiles from different frames (the SPU MLAA copied
back before it finished) push it well above. Prints the worst frames and a summary."""
import sys, os, re
import numpy as np
from PIL import Image
d = sys.argv[1]; T = int(sys.argv[2]) if len(sys.argv) > 2 else 128
res = []
for f in sorted(os.listdir(d)):
    if not f.endswith('.ppm') or 'staging' in f: continue
    a = np.asarray(Image.open(os.path.join(d, f)).convert('L'), dtype=np.float32)
    dx = np.abs(np.diff(a, axis=1)); dy = np.abs(np.diff(a, axis=0))   # dx[:, x] = |a[x+1]-a[x]|
    gx = np.zeros(dx.shape[1], bool); gx[T - 1::T] = True
    gy = np.zeros(dy.shape[0], bool); gy[T - 1::T] = True
    seam = (dx[:, gx].mean() + dy[gy, :].mean()) / 2
    rest = (dx[:, ~gx].mean() + dy[~gy, :].mean()) / 2
    res.append((seam / max(rest, 1e-6), f))
r = sorted(res, reverse=True)
for v, f in r[:5]: print('%6.2f  %s' % (v, f))
v = np.array([x for x, _ in res])
print('frames %d  median %.2f  p90 %.2f  max %.2f  >1.5: %d' % (len(v), np.median(v), np.percentile(v, 90), v.max(), (v > 1.5).sum()))
