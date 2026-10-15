#!/usr/bin/env python3
"""reach_hle.py <func_XXXXXXXX | 0xADDR> [...] [--depth N] [--nids nid_table]

Which firmware imports (ps3_hle_call NIDs) and lv2 syscalls (r11 constant
before lv2_syscall) can the given lifted PPU functions reach? Walks the
static call graph of generated/ppu/*.cpp (direct calls and tail-call
trampolines; indirect calls through ps3_indirect_call are not followed and
are reported as such). Prints each HLE call / syscall with the shortest call
path to it.

Names for NIDs come from the runtime's NID table if one is found
(--nids <file> with lines '0xNID name', or grep of third_party/ps3recomp for
'0xNIDu' next to a quoted name), else the raw NID is printed."""
import os, re, sys, glob, collections

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
DEPTH = int(opts.get('depth', 12))

FUNC = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{', re.M)
CALL = re.compile(r'\b(func_[0-9A-F]{8})\b')
HLE = re.compile(r'ps3_hle_call\(0x([0-9A-F]{8})u')
SC = re.compile(r'ctx->gpr\[11\] = \(int64_t\)\(int32_t\)\((0x[0-9A-F]+|\d+)\);(?:(?!lv2_syscall).){0,600}?lv2_syscall\(ctx\)', re.S)

body = {}
for path in sorted(glob.glob(os.path.join(ROOT, 'generated/ppu/*.cpp'))):
    text = open(path, errors='replace').read()
    ms = list(FUNC.finditer(text))
    for i, m in enumerate(ms):
        end = ms[i + 1].start() if i + 1 < len(ms) else len(text)
        body[m.group(1)] = text[m.end():end]

info = {}
for f, b in body.items():
    info[f] = (set(c for c in CALL.findall(b) if c != f and c in body),
               set(HLE.findall(b)),
               set(int(x, 0) for x in SC.findall(b)),
               'ps3_indirect_call' in b or 'bctrl' in b)

def nid_names():
    names = {}
    if 'nids' in opts:
        for line in open(opts['nids']):
            p = line.split()
            if len(p) >= 2: names[p[0].lower().replace('0x', '').upper().zfill(8)] = p[1]
        return names
    pat = re.compile(r'0x([0-9A-Fa-f]{8})u?\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"|"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*0x([0-9A-Fa-f]{8})')
    for path in glob.glob(os.path.join(ROOT, 'third_party/ps3recomp/**/*.[ch]*'), recursive=True):
        try: t = open(path, errors='replace').read()
        except OSError: continue
        for m in pat.finditer(t):
            nid, name = (m.group(1), m.group(2)) if m.group(1) else (m.group(4), m.group(3))
            names.setdefault(nid.upper(), name)
    return names

starts = []
for a in args:
    if a.startswith('func_'): starts.append(a)
    else: starts.append('func_%08X' % int(a, 0))
names = nid_names()
seen = {}
q = collections.deque()
for s in starts:
    if s in body: seen[s] = [s]; q.append(s)
    else: print('not a lifted function:', s)
hle, sc, indirect = {}, {}, []
while q:
    f = q.popleft()
    path = seen[f]
    calls, nids, scs, ind = info[f]
    for n in nids: hle.setdefault(n, path)
    for n in scs: sc.setdefault(n, path)
    if ind: indirect.append(path)
    if len(path) >= DEPTH: continue
    for c in sorted(calls):
        if c not in seen:
            seen[c] = path + [c]; q.append(c)

print('%d functions reachable from %s' % (len(seen), ' '.join(starts)))
print('firmware imports:')
for n, p in sorted(hle.items(), key=lambda kv: len(kv[1])):
    print('  0x%s %-36s via %s' % (n, names.get(n, '?'), ' > '.join(p)))
print('lv2 syscalls:')
for n, p in sorted(sc.items()):
    print('  %4d (0x%X)  via %s' % (n, n, ' > '.join(p)))
print('%d reachable functions make indirect calls (not followed)' % len(indirect))
