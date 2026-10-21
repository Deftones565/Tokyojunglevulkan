#!/bin/bash
# usage: tools/spurs/run_jobguard_churn.sh
# Host test for 0122: 200 "area loads" (job chain at a new EA + two job guards,
# then Shutdown + Join, as Tokyo Jungle's func_001502B0 / func_0014FBCC do)
# against the tree's libs/spurs/cellSpurs.c. Before 0122 it fails at load 16
# with 0x80410721 (CELL_SPURS_TASK_ERROR_AGAIN): the 2026-10-21 30-minute hang.
# Symbols the test never reaches are stubbed.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
S=$R/third_party/ps3recomp
inc="-I$S/include -I$S/runtime/spu"; for d in system filesystem input audio video network spurs sync codec font misc; do inc="$inc -I$S/libs/$d"; done
gcc -c -O2 -g -std=gnu17 -D_GNU_SOURCE -DPS3RECOMP_NO_INTERP=1 -msse4.1 $inc "$R/tools/spurs/jobguard_churn_test.c" -o "$D/t.o"
gcc "$D/t.o" -o "$D/t" -lpthread -lm 2>&1 | grep -o "undefined reference to \`[^']*'" | sed "s/.*\`//; s/'//" | sort -u > "$D/und.txt" || true
python3 - "$D" <<'P'
import sys
d=sys.argv[1]; und=open(d+'/und.txt').read().split()
tls={'g_spurs_job_cmd','g_spurs_job_mbox','g_spurs_job_mbox_intr','g_spurs_job_mbox_valid','g_spurs_job_sent_event','g_spurs_job_in_list'}
data={'g_resv_store_active':'int','g_spu_line_commit_hook':'void*','g_spurs_pm_polls':'unsigned','g_ww_hi':'uint32_t','g_ww_lo':'uint32_t','ppu_vm_size':'uint32_t','g_spurs_job_ls_handle':'uint32_t','vm_base':'uint8_t*'}
o=['#include <stdint.h>']
for s in und:
    if s in tls: o.append(f'__thread uint32_t {s};')
    elif s in data: o.append(f'{data[s]} {s};')
    elif s=='cellGcm_rsx_wait_consumed': o.append('int cellGcm_rsx_wait_consumed(unsigned t){return 1;}')
    else: o.append(f'long {s}(void){{return 0;}}')
open(d+'/stubs.c','w').write('\n'.join(o)+'\n')
P
gcc -c "$D/stubs.c" -o "$D/stubs.o"
gcc "$D/t.o" "$D/stubs.o" -o "$D/t" -lpthread -lm
"$D/t" 2>/dev/null | grep -E "^OK|FAIL"
