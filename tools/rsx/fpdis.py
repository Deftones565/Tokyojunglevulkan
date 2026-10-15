#!/usr/bin/env python3
"""RSX fragment-program disassembler (field layout as in RPCS3's FragmentProgramDecompiler).
usage: fpdis.py <ucode.bin>   (raw guest bytes: big-endian, halfword-swapped words)"""
import struct, sys

OPS = {0:'NOP',1:'MOV',2:'MUL',3:'ADD',4:'MAD',5:'DP3',6:'DP4',7:'DST',8:'MIN',9:'MAX',0xA:'SLT',0xB:'SGE',
       0xC:'SLE',0xD:'SGT',0xE:'SNE',0xF:'SEQ',0x10:'FRC',0x11:'FLR',0x12:'KIL',0x13:'PK4',0x14:'UP4',0x15:'DDX',
       0x16:'DDY',0x17:'TEX',0x18:'TXP',0x19:'TXD',0x1A:'RCP',0x1B:'RSQ',0x1C:'EX2',0x1D:'LG2',0x1E:'LIT',0x1F:'LRP',
       0x20:'STR',0x21:'SFL',0x22:'COS',0x23:'SIN',0x24:'PK2',0x25:'UP2',0x26:'POW',0x27:'PKB',0x28:'UPB',0x29:'PK16',
       0x2A:'UP16',0x2B:'BEM',0x2C:'PKG',0x2D:'UPG',0x2E:'DP2A',0x2F:'TXL',0x31:'TXB',0x38:'DP2',0x39:'NRM',0x3A:'DIV',
       0x3B:'DIVSQ',0x3C:'LIF',0x3D:'FENCT',0x3E:'FENCB',0x40:'BRK',0x41:'CAL',0x42:'IFE',0x43:'LOOP',0x44:'REP',0x45:'RET'}
NSRC = {'MOV':1,'FRC':1,'FLR':1,'RCP':1,'RSQ':1,'EX2':1,'LG2':1,'COS':1,'SIN':1,'DDX':1,'DDY':1,'NRM':1,'TEX':1,'TXP':1,
        'TXL':1,'TXB':1,'PK2':1,'UP2':1,'PK4':1,'UP4':1,'PKB':1,'UPB':1,'PK16':1,'UP16':1,'PKG':1,'UPG':1,'KIL':0,'NOP':0,
        'MAD':3,'LRP':3,'DP2A':3,'TXD':3}
INPUTS = ['WPOS','COL0','COL1','FOGC','TEX0','TEX1','TEX2','TEX3','TEX4','TEX5','TEX6','TEX7','TEX8','TEX9','SSA','FACING']
SC = {0:'',1:'_x2',2:'_x4',3:'_x8',5:'_d2',6:'_d4',7:'_d8'}
X = 'xyzw'

def words(b): return [((w << 16) | (w >> 16)) & 0xffffffff for w in struct.unpack('>4I', b)]
def fl(w): return struct.unpack('<f', struct.pack('<I', w))[0]

def src(w, op0, const, k, w1):
    t = w & 3; idx = (w >> 2) & 0x3f; h = (w >> 8) & 1
    sw = ''.join(X[(w >> (9 + 2 * i)) & 3] for i in range(4))
    neg = (w >> 17) & 1
    ab = ((w1 >> 29) & 1) if k == 0 else (w >> 18) & 1
    if t == 0: s = ('H%d' if h else 'R%d') % idx
    elif t == 1: s = INPUTS[(op0 >> 13) & 0xf]
    elif t == 2: s = 'c[%s]' % ', '.join('%g' % v for v in const)
    else: s = '?type3'
    s += '.' + sw if sw != 'xyzw' else ''
    if ab: s = '|%s|' % s
    if neg: s = '-' + s
    return s, (t, h, idx, sw)

def dis(data):
    out = []; i = 0
    while i + 16 <= len(data):
        w = words(data[i:i+16]); i += 16
        op = ((w[0] >> 24) & 0x3f) | ((w[2] >> 31) << 6)
        name = OPS.get(op, hex(op))
        const = None
        if any((w[k] & 3) == 2 for k in (1, 2, 3)) and i + 16 <= len(data):
            c = words(data[i:i+16]); const = [fl(x) for x in c]; i += 16
        end = w[0] & 1; dreg = (w[0] >> 1) & 0x3f; dh = (w[0] >> 7) & 1
        mask = ''.join(X[k] for k in range(4) if (w[0] >> (9 + k)) & 1)
        tex = (w[0] >> 17) & 0xf; sat = (w[0] >> 31) & 1; nodest = (w[0] >> 30) & 1
        setc = (w[0] >> 8) & 1; prec = (w[0] >> 22) & 3
        scale = SC.get((w[2] >> 28) & 7, '?')
        n = NSRC.get(name, 2)
        srcs = [src(w[1 + k], w[0], const, k, w[1]) for k in range(n)]
        d = ('H%d' if dh else 'R%d') % dreg + ('.' + mask if mask != 'xyzw' else '')
        if nodest: d = 'RC' if setc else '--'
        cond = ''
        lt, eq, gr = (w[1] >> 18) & 1, (w[1] >> 19) & 1, (w[1] >> 20) & 1
        if not (lt and eq and gr):
            cs = ''.join(X[(w[1] >> (21 + 2 * k)) & 3] for k in range(4))
            cond = ' (%s%s%s %s.%s)' % ('LT' if lt else '', 'EQ' if eq else '', 'GT' if gr else '',
                                        'RC1' if (w[1] >> 31) & 1 else 'RC0', cs)
        t = ' tex%d' % tex if name in ('TEX', 'TXP', 'TXL', 'TXB', 'TXD') else ''
        out.append(dict(name=name, dh=dh, dreg=dreg, mask=mask, srcs=[s[1] for s in srcs], nodest=nodest))
        print('%3d: %s%s%s%s %s, %s%s%s%s%s' % (len(out) - 1, name, scale, '_sat' if sat else '', '' if prec == 0 else '_p%d' % prec,
              d, ', '.join(s[0] for s in srcs), t, cond, ' SETC' if setc and not nodest else '', ' END' if end else ''))
        if end: break
    return out

if __name__ == '__main__':
    dis(open(sys.argv[1], 'rb').read())
