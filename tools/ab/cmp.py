#!/usr/bin/env python3
"""Per-flip compare of two frame dump dirs: tools/ab/cmp.py <A frames> <B frames>
Prints, for each flip both dumped: mean RGB of A and B, mean |A-B|, % of pixels differing by > 16."""
import sys, os, re
from PIL import Image, ImageChops, ImageStat
def flips(d):
    return {int(re.search(r'flip(\d+)', f).group(1)): os.path.join(d, f) for f in os.listdir(d) if f.endswith('.ppm')}
A, B = flips(sys.argv[1]), flips(sys.argv[2])
for fl in sorted(set(A) & set(B)):
    a, b = Image.open(A[fl]).convert('RGB'), Image.open(B[fl]).convert('RGB')
    if a.size != b.size: print(fl, 'size', a.size, b.size); continue
    d = ImageChops.difference(a, b)
    md = sum(ImageStat.Stat(d).mean) / 3
    big = sum(1 for p in d.convert('L').getdata() if p > 16) * 100.0 / (a.size[0] * a.size[1])
    ma = '/'.join('%d' % v for v in ImageStat.Stat(a).mean); mb = '/'.join('%d' % v for v in ImageStat.Stat(b).mean)
    print('%6d  A %-11s B %-11s |d| %5.2f  >16: %5.2f%%' % (fl, ma, mb, md, big))
