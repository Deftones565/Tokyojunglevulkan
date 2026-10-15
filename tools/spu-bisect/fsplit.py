#!/usr/bin/env python3
"""fsplit.py <src> <dst> <lo> <hi>   or   fsplit.py <src> <dst> names <f1,f2,...>: in image 9's lifted TU, only the shufb-using
functions with index in [lo, hi) keep the pshufb spu_shufb; every other
function uses the byte-loop reference (spu_shufb_ref). Prints the chosen names."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
by_name = sys.argv[3] == 'names'
if not by_name: lo, hi = int(sys.argv[3]), int(sys.argv[4])
text = open(src, newline='').read()
nl = '\r\n' if '\r\n' in text else '\n'
lines = text.split(nl)
starts = [i for i, l in enumerate(lines) if re.match(r'^(static )?void \w+\(spu_context\* ctx\) \{$', l)]
funcs = []
for k, s in enumerate(starts):
    e = s
    while lines[e] != '}': e += 1
    body = nl.join(lines[s:e+1])
    funcs.append((s, e, lines[s].split('(')[0].split()[-1], 'spu_shufb(' in body))
users = [f for f in funcs if f[3]]
keep = set(f[2] for f in users[lo:hi]) if not by_name else \
    set(f[2] for f in users if any(f[2].endswith(n) for n in sys.argv[4].split(',')))
out = []
fi = {f[0]: f for f in funcs}; ends = {f[1]: f for f in funcs}
for i, l in enumerate(lines):
    if i in fi and fi[i][3] and fi[i][2] not in keep:
        out.append('#define spu_shufb spu_shufb_ref')
    out.append(l)
    if i in ends and ends[i][3] and ends[i][2] not in keep:
        out.append('#undef spu_shufb')
open(dst, 'w', newline='').write(nl.join(out))
print('shufb users:', len(users), 'keep pshufb:', len(keep))
for n in sorted(keep): print(' ', n)
