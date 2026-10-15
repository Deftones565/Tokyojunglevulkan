#!/usr/bin/env python3
"""DIAGNOSTIC ONLY, never for landing: a traced copy of the lifted PPU code.

usage: tools/diag/sgs_instrument.py <generated/ppu> <out dir> [lo hi]

Copies the lifted sources to <out dir>, renames every lifted function whose
address is in [lo, hi) (default: the game's sgsfile library, 0x230000-0x239600)
to func_X__orig and appends a wrapper func_X that calls it between
sgsdiag_enter/sgsdiag_leave (tools/diag/sgs_diag.cpp, copied alongside).
Callers are untouched: they call func_X by name, which is now the wrapper.
Configure a private build with -DRECOMP_DIR=<out dir>.
"""
import os, re, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
lo = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x230000
hi = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x239600
os.makedirs(dst, exist_ok=True)
DEF = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{$', re.M)
total = 0
for f in sorted(os.listdir(src)):
    p = os.path.join(src, f)
    if not f.endswith('.cpp'):
        shutil.copy2(p, os.path.join(dst, f)); continue
    text = open(p, newline='').read()
    hits = [m for m in DEF.finditer(text) if lo <= int(m.group(1), 16) < hi]
    for m in hits:
        a = m.group(1)
        text = text.replace(m.group(0), 'void func_%s__orig(ppu_context* ctx) {' % a, 1)
    if hits:
        text += '\n/* ---- sgsdiag wrappers (tools/diag/sgs_instrument.py) ---- */\n'
        text += 'extern "C" void sgsdiag_enter(uint32_t fn, ppu_context* ctx, void** tok);\n'
        text += 'extern "C" void sgsdiag_leave(uint32_t fn, ppu_context* ctx, void* tok);\n'
        for m in hits:
            a = m.group(1)
            text += ('void func_%s(ppu_context* ctx) { void* t; sgsdiag_enter(0x%s, ctx, &t); '
                     'func_%s__orig(ctx); sgsdiag_leave(0x%s, ctx, t); }\n' % (a, a, a, a))
    total += len(hits)
    open(os.path.join(dst, f), 'w', newline='').write(text)
shutil.copy2(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sgs_diag.cpp'),
             os.path.join(dst, 'sgs_diag.cpp'))
print('wrapped %d functions in [0x%X, 0x%X) -> %s' % (total, lo, hi, dst))
