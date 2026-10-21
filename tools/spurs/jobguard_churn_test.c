/* 200 area loads: each makes a chain at a new EA with two guards, then shuts
 * it down and joins it, as Tokyo Jungle's func_001502B0 / func_0014FBCC do. */
#include "../../third_party/ps3recomp/libs/spurs/cellSpurs.c"
#include <sys/mman.h>
int main(void)
{
    vm_base = mmap(0, 0x10000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    /* a long-lived chain whose guard must survive all the churn */
    cellSpursCreateJobChain(0x100, 0x00800000, 0, 128, 1, 0, 1, 0);
    if (cellSpursJobGuardInitialize(0x00800000, 0x00800200, 1, 1, 1)) return 2;
    for (int k = 0; k < 200; k++) {
        u32 jc = 0x01000000 + k * 0x1000;
        cellSpursCreateJobChain(0x100, jc, 0, 128, 1, 0, 1, 0);
        s32 a = cellSpursJobGuardInitialize(jc, jc + 0x200, 1, 1, 1);
        s32 b = cellSpursJobGuardInitialize(jc, jc + 0x280, 1, 1, 1);
        if (a || b) { printf("FAIL at load %d: %08X %08X\n", k, a, b); return 1; }
        cellSpursRunJobChain(jc);
        if (jg_find(jc + 0x200) < 0 || jg_find(0x00800200) < 0) { printf("FAIL lost guard at %d\n", k); return 1; }
        cellSpursShutdownJobChain(jc);
        cellSpursJoinJobChain(jc);
    }
    int chains = 0; for (int i = 0; i < MAX_JOBCHAINS; i++) chains += s_jobchains[i].jc_ea != 0;
    printf("OK: 200 loads, %d chain slots used, long-lived guard kept: %d\n", chains, jg_find(0x00800200) >= 0);
    return 0;
}
