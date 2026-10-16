#!/usr/bin/env python3
"""Keep a lifted SPU function's registers in C locals (register caching).

usage: regcache.py <in spu_recomp.c> <out spu_recomp.c> [--skip FUNC,...] [--only FUNC ...] [--stats]

spu_lifter.py emits every SPU instruction as `ctx->gpr[d] = op(ctx->gpr[a], ...)`.
The local store lives inside the same spu_context, so the compiler must assume
any LS access (and any call) may change ctx->gpr: every result is stored to
memory and every operand reloaded, and each dependent instruction pays a
store-to-load round trip. This pass gives each function one local per SPU
register it uses (`u128 rN`), loaded at entry, and keeps ctx->gpr in step only
where something outside the function can see it:

  * a line that leaves the function (branch out, SPU_RET, halt, an indirect
    branch with ctx->pc) writes the locals back first -- for the
    `if (cond) { ...; return; }` lines inside loops, only the taken path does;
  * a call into another lifted function (the `spu_link(..); { ... f(ctx);
    spu_drain_call(..); ... }` lines) writes them back before and reloads them
    after, since the callee works on ctx->gpr;
  * any other line that names ctx for something other than LS (spu_ls_read*/
    spu_ls_write*) or a channel (spu_rdch/spu_wrch, which take and return
    register VALUES) gets the same write-back before and reload after;
  * falling off the end writes back.

LS accesses and channel accesses do not read or write ctx->gpr, so lines made
only of those and register arithmetic just use the locals. The result computes
exactly what the original does (same helpers, same order); only where the
register values live in between changes. Check with
tools/spu-bisect/harness (every function, original vs rewritten).
"""
import re
import sys

FUNC_RE = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{$')
GPR_RE = re.compile(r'ctx->gpr\[(\d+)\]')
WRITE_RE = re.compile(r'ctx->gpr\[(\d+)\]\s*=(?!=)')
# ctx used as the first argument of these is fine (no gpr access inside).
SAFE_CTX_CALL = re.compile(r'\b(spu_ls_read\w*|spu_ls_write\w*|spu_rdch|spu_wrch)\(ctx,')
IF_EXIT_RE = re.compile(r'^(\s*)if \((.*)\) \{ (.*) \}$')


def sub_locals(s):
    return GPR_RE.sub(lambda m: 'r%s' % m.group(1), s)


def ctx_other_than_gpr(s):
    """Does the line name ctx for anything but gpr and LS/channel calls?"""
    t = GPR_RE.sub('G', s)
    t = SAFE_CTX_CALL.sub(r'\1(CTX,', t)
    return re.search(r'\bctx\b', t) is not None


def transform_function(name, body):
    used = sorted({int(n) for line in body for n in GPR_RE.findall(line)})
    written = sorted({int(n) for line in body for n in WRITE_RE.findall(line)})
    if not used:
        return body, False
    ind = '        '
    flush = ind + ' '.join('ctx->gpr[%d] = r%d;' % (n, n) for n in written) if written else ''
    reload = ind + ' '.join('r%d = ctx->gpr[%d];' % (n, n) for n in used)
    out = [ind + 'u128 ' + ', '.join('r%d = ctx->gpr[%d]' % (n, n) for n in used) + ';']
    for line in body:
        if 'ctx' not in line or not ctx_other_than_gpr(line):
            out.append(sub_locals(line))
            continue
        m = IF_EXIT_RE.match(line)
        if m and 'return;' in m.group(3) and not ctx_other_than_gpr(m.group(2)):
            # Conditional exit: only the taken path writes back.
            fl = (' '.join('ctx->gpr[%d] = r%d;' % (n, n) for n in written) + ' ') if written else ''
            out.append('%sif (%s) { %s%s }' % (m.group(1), sub_locals(m.group(2)), fl, m.group(3)))
            continue
        # Anything else that can see ctx->gpr: write back, run as lifted,
        # and reload unless the line always leaves the function.
        if flush:
            out.append(flush)
        out.append(line)
        always_leaves = (re.match(r'^\s*\{ ctx->pc = .*return; \}$', line) or
                         re.match(r'^\s*SPU_RET', line) or
                         (line.rstrip().endswith('return;') and not line.lstrip().startswith('if')))
        if not always_leaves:
            out.append(reload)
    if flush:
        out.append(flush)            # falling off the end
    return out, True


def main():
    args = sys.argv[1:]
    stats = '--stats' in args
    args = [a for a in args if a != '--stats']
    skip = set()
    if '--skip' in args:
        i = args.index('--skip')
        skip = set(args[i + 1].split(','))
        del args[i:i + 2]
    only = None
    if '--only' in args:
        i = args.index('--only')
        only = set(args[i + 1:])
        args = args[:i]
    src, dst = args
    lines = open(src).read().split('\n')
    out, i, nf, nt = [], 0, 0, 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            out.append(lines[i]); i += 1; continue
        name = m.group(1)
        j = i + 1
        while lines[j] != '}':
            j += 1
        body = lines[i + 1:j]
        nf += 1
        if (only is None or name in only) and name not in skip:
            body, did = transform_function(name, body)
            nt += did
        out.append(lines[i]); out.extend(body); out.append('}')
        i = j + 1
    open(dst, 'w').write('\n'.join(out))
    if stats:
        print('%d functions, %d rewritten' % (nf, nt), file=sys.stderr)


if __name__ == '__main__':
    main()
