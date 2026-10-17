#!/usr/bin/env python3
"""Private AOT experiment: disambiguate GPR and LS memory in selected pure lifts.
No register caching or control-flow changes. Every selected function is checked
for context callbacks, and the wrapper retains the original code for aliased LS.
Usage: noalias.py INPUT OUTPUT --only FUNCTION [FUNCTION ...]
"""
import argparse
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source'); p.add_argument('output')
p.add_argument('--only', nargs='+', required=True)
a = p.parse_args()
s = Path(a.source).read_text()
selected = set(a.only); found = set()
pattern = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{\n(.*?)^\}', re.M | re.S)

def rewrite(m):
    name, body = m.groups()
    if name not in selected: return m[0]
    # The only context-taking helpers allowed inside the restricted scope are
    # the probe-free SIMD LS128 helpers. Return macros are expanded below.
    calls = set(re.findall(r'\b(\w+)\s*\(ctx\s*[,)]', body))
    allowed = {'spu_ls_read128', 'spu_ls_write128', 'SPU_RET', 'SPU_RET_REG'}
    if calls - allowed:
        raise ValueError(f'{name}: context callbacks {calls - allowed}')
    if re.search(r'ctx->(?:ls|gpr)\s*=', body):
        raise ValueError(f'{name}: pointer rebinding')
    if 'spu_ls_' not in body or 'ctx->gpr[' not in body:
        raise ValueError(f'{name}: no mixed LS/GPR accesses')
    changed = body.replace('ctx->gpr[', 'tj_regs[')
    def ret(reg):
        return ('do { ctx->pc = tj_regs['+reg+']._u32[0]; '
                'if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch; '
                'return; } while (0)')
    changed = re.sub(r'SPU_RET_REG\(ctx,\s*(\d+)\)', lambda m:ret(m[1]), changed)
    changed = changed.replace('SPU_RET(ctx)', ret('0'))
    assert 'ctx->gpr' not in changed
    found.add(name)
    return (f'#if defined(SPU_LS_NO_PROBES) && defined(__SSSE3__) && defined(__GNUC__)\n'
            f'static inline __attribute__((always_inline)) void {name}_noalias(spu_context* ctx, u128* __restrict tj_regs) {{\n'
            f'{changed}}}\n#endif\n'
            f'void {name}(spu_context* ctx) {{\n'
            '#if defined(SPU_LS_NO_PROBES) && defined(__SSSE3__) && defined(__GNUC__)\n'
            '    const uintptr_t ls = (uintptr_t)ctx->ls, regs = (uintptr_t)ctx->gpr;\n'
            '    if (regs >= ls ? regs - ls >= SPU_LS_SIZE : ls - regs >= sizeof(ctx->gpr)) {\n'
            f'        {name}_noalias(ctx, ctx->gpr); return;\n'
            '    }\n#endif\n'+body+'}')

out = pattern.sub(rewrite, s)
if found != selected: p.error(f'functions not found: {selected-found}')
Path(a.output).write_text(out)
print(f'{len(found)} pure lifted functions transformed')
