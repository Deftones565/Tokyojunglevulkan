#!/usr/bin/env python3
"""Compare actual old/new task-launch hooks, diagnostics and LS writes.
Usage: check_spu_handoff.py BASE_SDK CANDIDATE_SDK OUT
Uses native C fixtures, never guest instruction execution.
"""
import os
from pathlib import Path
import subprocess
import sys

base, candidate, out = map(lambda x: Path(x).resolve(), sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
source = out / 'handoff_fixture.c'
source.write_text(r'''
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <limits.h>
extern void spu_task_launch_check(spu_context*, void*);
extern unsigned spu_recent_pcs(uint32_t*, unsigned);
extern void* volatile g_pm_flow_ctx;
extern volatile unsigned g_pm_flow_n;
extern uint32_t g_pm_flow_buf[];
static unsigned checks, halts, unwind;
static jmp_buf jump;
void spu_halt(spu_context* ctx) { (void)ctx; ++halts; }
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*)) {
    (void)ctx; (void)fn; ++checks;
    if (unwind) longjmp(jump, 1);
}
static void target(spu_context* ctx) { (void)ctx; }
int main(void) {
    spu_context* ctx = calloc(1, sizeof(*ctx));
    ctx->ls = calloc(1, SPU_LS_SIZE);
    unsigned long long hash = 0;
    for (unsigned i=0; i<256; ++i) {
        ctx->pc = (i%4 == 0) ? 0 : (i%4 == 1 ? 0x40000 : 0x7f48);
        ctx->image_id = (int)((i>>4)%4)-1;
        ctx->policy_mode = (i>>2)&1;
        ctx->steps = (i%3 == 0) ? 0 : (i%3 == 1 ? 17 : UINT32_MAX);
        g_pm_flow_ctx = (i%5 == 0) ? ctx : NULL;
        unwind = (i%11 == 0);
        if (!setjmp(jump)) spu_task_launch_check(ctx, (void*)target);
        hash = hash*131 + ctx->steps + ctx->pc + halts + checks + g_pm_flow_n;
    }
    unwind = 0; g_pm_flow_ctx = NULL;
    ctx->pc = 0x2318; ctx->image_id = 2; ctx->policy_mode = 1;
    for(unsigned i=0; i<16; ++i) ctx->ls[0xdf0+i*4+3] = i;
    for(unsigned i=0; i<8192; ++i) spu_task_launch_check(ctx, (void*)target);
    for(unsigned i=0; i<SPU_LS_SIZE; ++i) hash = hash*131 + ctx->ls[i];
    uint32_t pcs[32]; unsigned n = spu_recent_pcs(pcs,32);
    for(unsigned i=0; i<n; ++i) hash = hash*131 + pcs[i];
    for(unsigned i=0; i<g_pm_flow_n; ++i) hash = hash*131 + g_pm_flow_buf[i];
    printf("hash=%llu steps=%llu checks=%u halts=%u flow=%u recent=%u\n",
           hash, (unsigned long long)ctx->steps, checks, halts, g_pm_flow_n, n);
    return 0;
}
''')
env = {k:v for k,v in os.environ.items() if not k.startswith('SPU_')}
binaries=[]
for label, sdk in [('base',base),('candidate',candidate)]:
    binary=out/label
    subprocess.run(['clang','-O2','-std=gnu17','-ffunction-sections','-fdata-sections',
                    '-DPS3RECOMP_NO_INTERP','-I'+str(sdk/'runtime/spu'),'-I'+str(sdk/'include'),
                    str(source),str(sdk/'runtime/spu/spu_drain.c'),'-Wl,--gc-sections',
                    '-o',str(binary)],check=True)
    binaries.append(binary)
cases=[{}, {'SPU_NO_LS0_END':'1'}, {'SPU_EXITTRACE':'1'}, {'SPU_STEPTRACE':'1'},
       {'SPU_STEPTRACE':'1','SPU_STEPTRACE_FROM':'7f48'}, {'SPU_JOBDRAIN':'1'},
       {'SPU_STEPTRACE_FROM':'7f48'}, {'SPU_EXITTRACE':'-1'}]
for settings in cases:
    results=[subprocess.run([str(b)],env=env|settings,capture_output=True,check=True) for b in binaries]
    for stream in ['stdout','stderr']:
        a,b=[getattr(r,stream) for r in results]
        if a!=b: raise AssertionError(f'{settings} {stream}: {a!r} != {b!r}')
    print(f'PASS {settings or "default"}: {results[0].stdout.decode().strip()}')
print('All task-launch differential cases passed (including dynamic flow arm and stack unwind).')
