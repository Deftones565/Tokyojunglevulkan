#!/usr/bin/env python3
"""nv3089_census.py <log>: summarise the NV3089 blits a run logged with
NV3089_GATE=<file> (cellGcmSys.c logs every blit's geometry while <file>
exists; tools/rsx/run_nv3089_census.sh opens the gate during gameplay).

One line per distinct blit shape (format, size, scale, source, destination,
swizzled or not), most frequent first, with the pixels it moves."""
import re, sys
from collections import Counter

pat = re.compile(r'\[NV3089G\] fmt=0x(\w+) out=(\d+)x(\d+) at (\d+),(\d+) in=(\d+)x(\d+) '
                 r'uv=0x(\w+) ds=0x(\w+) dt=0x(\w+) src=0x(\w+)/(\d+) dst=0x(\w+)/(\d+) swz=(\d)')
shapes = Counter()
n = 0
for line in open(sys.argv[1], errors='replace'):
    m = pat.search(line)
    if not m: continue
    n += 1
    fmt, w, h, ox, oy, iw, ih, uv, ds, dt, src, sp, dst, dp, swz = m.groups()
    scale = ' 1:1' if ds == '100000' and dt == '100000' else f' ds={ds} dt={dt}'
    shapes[(fmt, int(w), int(h), ox + ',' + oy, uv, scale, src, sp, dst, dp, swz)] += 1
print(f'{n} blits, {len(shapes)} shapes')
print(f'{"count":>6} {"Mpx":>7}  fmt  size        at       uv         scale       src/pitch -> dst/pitch  swz')
for k, c in shapes.most_common():
    fmt, w, h, at, uv, scale, src, sp, dst, dp, swz = k
    print(f'{c:6d} {c * w * h / 1e6:7.2f}  0x{fmt:<2} {w:4d}x{h:<5d} {at:>8} {uv:>10} {scale:>11}  '
          f'0x{src}/{sp} -> 0x{dst}/{dp}  {swz}')
