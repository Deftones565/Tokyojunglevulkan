#!/usr/bin/env python3
"""Find a v1.00 Tokyo Jungle PPU function in another EBOOT by instruction shape.

usage: match_func.py <src EBOOT.ELF> <src addr hex> <dst EBOOT.ELF> [--len N]

Compares the first N words (default: up to the first blr, max 96) with the
16-bit immediates and branch displacements masked out, since those move
between versions (TOC offsets, relocated callees). Prints the best matches."""
import struct, sys
def load(path):
    d = open(path, 'rb').read()
    phoff = struct.unpack('>Q', d[0x20:0x28])[0]; phnum = struct.unpack('>H', d[0x38:0x3A])[0]
    segs = []
    for i in range(phnum):
        o = phoff + i * 56
        t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', d[o:o + 56])
        if t == 1: segs.append((va, off, fs, fl))
    return d, segs
def words(d, segs, va, n):
    for sva, off, fs, fl in segs:
        if sva <= va < sva + fs:
            o = off + va - sva
            return list(struct.unpack('>%dI' % n, d[o:o + 4 * n]))
def mask(w):
    op = w >> 26
    if op in (16, 18): return w & 0xFC000003          # bc / b: drop displacement
    if op in (14, 15, 24, 25, 28, 29, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 44, 45, 48, 50, 52, 54, 58, 62):
        return w & 0xFFFF0000                          # D/DS-form: drop immediate
    return w
src_path, src_va, dst_path = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
n = int(sys.argv[sys.argv.index('--len') + 1]) if '--len' in sys.argv else None
sd, ss = load(src_path); dd, ds = load(dst_path)
if n is None:
    ws = words(sd, ss, src_va, 96)
    n = next((i + 1 for i, w in enumerate(ws) if w == 0x4E800020), 96)
pat = [mask(w) for w in words(sd, ss, src_va, n)]
best = []
for sva, off, fs, fl in ds:
    if not fl & 1: continue                            # executable segments
    data = struct.unpack('>%dI' % (fs // 4), dd[off:off + fs // 4 * 4])
    for i in range(len(data) - n):
        if mask(data[i]) != pat[0]: continue
        score = sum(1 for k in range(n) if mask(data[i + k]) == pat[k])
        if score >= n * 0.8: best.append((score, sva + 4 * i))
best.sort(reverse=True)
print(f'{n} words; best matches:')
for s, va in best[:5]: print(f'  0x{va:08X}  {s}/{n}')
