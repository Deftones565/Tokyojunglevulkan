#!/usr/bin/env python3
"""Re-lift image 9 with the optional pure-region pass into a private directory.
Usage: lift_image9_region.py SDK OUTPUT_DIR
Requires SDK with pending 0086 applied; leaves production generated/ unchanged.
"""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
root=Path(__file__).resolve().parents[2]
sdk=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
if out==root/'generated/spu/j8D485835BE6E8738':
    raise SystemExit('Use a private output directory until this candidate is accepted')
spec=importlib.util.spec_from_file_location('tj_lift',root/'scripts/lift_spu.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
dump=root/'generated/spu/dumps/spujob_8D485835BE6E8738_33876.bin'
_,base,code=mod.spu_elf(dump.read_bytes())
extra=sorted(set(base+t for t in mod.rel_jump_table_targets(code))|set(mod.EXTRA_ENTRIES.get(0x8D485835BE6E8738,[])))
cmd=[sys.executable,str(sdk/'tools/spu_lifter.py'),'--auto-functions',str(dump),
     '--symbol-prefix','j8D485835BE6E8738_','--region','0x7F48,0x8000','-o',str(out)]
if extra:cmd+=['--extra-funcs',','.join(hex(a) for a in extra)]
subprocess.run(cmd,check=True)
for p in [out/'spu_recomp.c',out/'spu_recomp.h']:
    p.write_text(re.sub(r'#include "(?:\.\./)+runtime/spu/([^"]+)"',r'#include "\1"',p.read_text()))
original=(root/'generated/spu/j8D485835BE6E8738/spu_recomp.c').read_text()
if not (out/'spu_recomp.c').read_text().startswith(original):
    raise SystemExit('Unexpected changes outside appended region; inspect before building')
print('Original 770 entry functions unchanged; AOT region appended.')
