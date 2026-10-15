#!/usr/bin/env python3
"""Call tree of lifted PPU functions: tools/diag/calltree.py <func_XXXXXXXX> [depth] [lo-hi to mark]
Follows direct calls and trampoline tail calls in generated/ppu; HLE imports shown as HLE_<nid>."""
import re, glob, sys
src = {}
for f in sorted(glob.glob('generated/ppu/ppu_recomp_0*.cpp')):
    fn = None; buf = []
    for l in open(f):
        m = re.match(r'void (func_[0-9A-F]+)\(', l)
        if m:
            if fn: src[fn] = buf
            fn = m.group(1); buf = []
        buf.append(l)
    if fn: src[fn] = buf
def callees(fn):
    out = []
    for l in src.get(fn, []):
        for m in re.finditer(r'(func_[0-9A-F]{8})(?:\(ctx\)|;)', l):
            if m.group(1) != fn and m.group(1) not in out: out.append(m.group(1))
        m = re.search(r'ps3_hle_call\(0x([0-9A-F]+)u', l)
        if m and 'HLE_' + m.group(1) not in out: out.append('HLE_' + m.group(1))
    return out
root = sys.argv[1]; maxd = int(sys.argv[2]) if len(sys.argv) > 2 else 2
lo, hi = (int(x, 16) for x in (sys.argv[3].split('-') if len(sys.argv) > 3 else ('1E0000', '260000')))
def mark(f):
    if f.startswith('HLE'): return f
    return f + ('*' if lo <= int(f[5:], 16) < hi else '')
seen = set()
def tree(fn, d):
    cs = callees(fn)
    print('  ' * d + mark(fn), '->', ' '.join(mark(c) for c in cs)[:240])
    if d < maxd:
        for c in cs:
            if c.startswith('func') and c not in seen:
                seen.add(c); tree(c, d + 1)
tree(root, 0)
