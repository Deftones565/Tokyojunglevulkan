#!/usr/bin/env python3
"""Raw RSX register state per draw from an RPCS3 .rrc capture.

usage:
  capture_regs.py <capture.rrc[.gz]> regs <draw> [<draw> ...] [--reg 0x0314 ...]
      print the listed registers (default: blend/mask/shader-control set) as
      they stand at each draw (draw = index of BEGIN_END(prim != 0), from 0)
  capture_regs.py <capture.rrc[.gz]> between <drawA> <drawB>
      list the surface / clear / 2D / program commands issued between two draws

The command-record walk is CASPER-3's (build-runs/atlas-ref/casper3/rrc.py,
draws.py): records are <u32 header, u32 value, varint n, 8*n bytes, 16 bytes>,
and the first record is found by walking back from a known one. The anchor
below (SET_TEXTURE_OFFSET t0 = 0x0F110000, past byte 62,000,000) is the one
that fits capture4 (build-oracle/capture4); pass --anchor-value / --anchor-from
for another capture.
"""
import argparse, gzip, struct, sys

DEFAULT_REGS = [(0x0310, 'BLEND_EN'), (0x0314, 'SFACTOR'), (0x0318, 'DFACTOR'), (0x0320, 'EQUATION'),
                (0x0324, 'COLOR_MASK'), (0x036C, 'BLEND_MRT'), (0x0304, 'ALPHA_TEST'),
                (0x08E4, 'FP_OFFSET'), (0x1D60, 'SHADER_CTRL'), (0x1D88, 'SHADER_WINDOW'),
                (0x0208, 'SURF_FMT'), (0x0210, 'SURF_A'), (0x0214, 'ZETA')]
INTERESTING = {0x0180, 0x0184, 0x0188, 0x018C, 0x0194, 0x0198, 0x0200, 0x0208, 0x020C, 0x0210, 0x0214,
               0x0218, 0x021C, 0x0220, 0x0224, 0x0228, 0x022C, 0x0280, 0x02B8, 0x0310, 0x0314, 0x0318,
               0x0320, 0x0324, 0x08E4, 0x1D60, 0x1D80, 0x1D88, 0x1D90, 0x1D94, 0x1FD8, 0x1808}


def load(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rb') as f:
        return f.read()


def parse(d, p):
    h, v = struct.unpack_from('<II', d, p); p += 8
    n = 0; s = 0
    while True:
        b = d[p]; p += 1; n |= (b & 0x7f) << s; s += 7
        if not b & 0x80: break
    return h, v, p + 8 * n + 16


def find_start(d, anchor):
    best = anchor; moved = True
    while moved:
        moved = False
        for back in range(16, 4096):
            q = best - back
            try: h, v, r = parse(d, q)
            except Exception: continue
            if r == best and ((h & 3) == 0 and (h >> 18) & 0x7ff in (1, 2, 3, 4, 8, 16, 32)
                              or h & 0xFFFC0000 == 0x40000):
                best = q; moved = True; break
    return best


def walk(d, start):
    """Yield (record index, method, value, state-before-or-at, draw index or None)."""
    p = start; idx = 0; draw = 0
    while p < len(d) - 8:
        try: h, v, p2 = parse(d, p)
        except Exception: return
        m = h & 0xFFFC
        is_draw = m == 0x1808 and v != 0
        yield idx, h, m, v, (draw if is_draw else None)
        if is_draw: draw += 1
        p = p2; idx += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture'); ap.add_argument('mode', choices=['regs', 'between'])
    ap.add_argument('draws', nargs='+', type=int)
    ap.add_argument('--reg', action='append', type=lambda x: int(x, 0))
    ap.add_argument('--anchor-value', type=lambda x: int(x, 0), default=0x0F110000)
    ap.add_argument('--anchor-method', type=lambda x: int(x, 0), default=0x00041A00)
    ap.add_argument('--anchor-from', type=int, default=62000000)
    a = ap.parse_args()
    d = load(a.capture)
    anchor = d.find(struct.pack('<II', a.anchor_method, a.anchor_value), a.anchor_from)
    if anchor < 0: sys.exit('anchor record not found; pass --anchor-*')
    start = find_start(d, anchor)
    regs = [(r, '%04X' % r) for r in a.reg] if a.reg else DEFAULT_REGS
    state = {}
    if a.mode == 'regs':
        want = set(a.draws)
        for idx, h, m, v, draw in walk(d, start):
            state[m] = v
            if draw in want:
                print('#%d' % draw, ' '.join('%s=%s' % (n, '0x%08X' % state[r] if r in state else '-')
                                            for r, n in regs))
                want.discard(draw)
                if not want: break
    else:
        lo, hi = a.draws[0], a.draws[1]
        inside = False; other = {}
        for idx, h, m, v, draw in walk(d, start):
            if draw == lo: inside = True
            if inside:
                if m in INTERESTING or m >= 0x2000:
                    print('%6d %s m=%04X v=%08X' % (idx, ('DRAW #%d' % draw) if draw is not None else '       ', m, v))
                else:
                    other[m] = other.get(m, 0) + 1
            if draw == hi: break
        print('other methods (count):', ' '.join('%04X:%d' % kv for kv in sorted(other.items())))


if __name__ == '__main__':
    main()
