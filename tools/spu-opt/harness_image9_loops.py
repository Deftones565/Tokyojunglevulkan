#!/usr/bin/env python3
"""Exercise image 9's counted loops with small valid counters in both builds.
Run after harness_runtime_ab.sh: harness_image9_loops.py <harness-directory>.
The generic random-state harness times out on these near-unbounded counters.
"""
import subprocess
import sys
from pathlib import Path
out=Path(sys.argv[1]).resolve()
loops={
 '03AA0':(12,96),'03B00':(12,16),'03B30':(12,1),'047E8':(12,2),
 '05C60':(13,2),'064D8':(32,1),'06788':(23,1),'06928':(12,16),
 '06AD8':(17,16),'06C80':(59,16),'06E28':(43,16),'06FC8':(9,16),
 '07130':(75,16),'071E0':(77,16),'07290':(78,16),'07340':(79,16),
 '07850':(56,1),'07C88':(56,1),'082D0':(20,4),'08778':(15,64),
 '08C50':(18,2),'08E40':(17,2),'098E0':(21,1),
}
for addr,(reg,step) in loops.items():
    subprocess.run(['./harness','j8D485835BE6E8738_spu_func_000'+addr,'200',str(reg),'./new.so','./ref.so',str(step)],cwd=out,check=True,timeout=20)
print(f'PASS: {len(loops)} counted loops')
