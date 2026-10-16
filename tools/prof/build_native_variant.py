#!/usr/bin/env python3
"""Relink production objects with selected privately compiled sources.

Example: build_native_variant.py build-codex --main build-codex/main.cpp
         --runtime libs/video/rsx_vulkan_backend.c
Only files named on the command line are recompiled; do not use this for
ABI/layout changes. A private ps3recomp copy must already exist in OUT.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('out');p.add_argument('--main');p.add_argument('--runtime',action='append',default=[])
a=p.parse_args(); root=Path.cwd(); base=root/'build'; out=Path(a.out).resolve()
if out==base.resolve() or not (out/'ps3recomp').is_dir(): p.error('private OUT/ps3recomp required')
commands=subprocess.check_output(['ninja','-C',str(base),'-t','commands','tokyojungle'],text=True).splitlines()
def compile_one(spec):
    rel, replacement=spec
    matches=[shlex.split(c) for c in commands if c.endswith('/'+rel) and ' -c ' in c]
    if len(matches)!=1: raise RuntimeError(f'{rel}: expected one compile command, got {len(matches)}')
    cmd=matches[0]; original=cmd[cmd.index('-o')+1]
    obj=out/Path(original).name
    for flag,value in [('-o',str(obj)),('-MF',str(obj)+'.d'),('-MT',str(obj))]: cmd[cmd.index(flag)+1]=value
    cmd=[v.replace(str(root/'third_party/ps3recomp'),str(out/'ps3recomp')) for v in cmd]
    if replacement: cmd[cmd.index('-c')+1]=str(Path(replacement).resolve())
    subprocess.run(cmd,cwd=base,check=True)
    return original,str(obj)
specs=[('third_party/ps3recomp/'+f,None) for f in a.runtime]
if a.main: specs.append(('src/main.cpp',a.main))
with ThreadPoolExecutor(max_workers=2) as pool: replacements=dict(pool.map(compile_one,specs))
link=shlex.split(commands[-1].split(' && ')[1])
if a.runtime:
    old=next(v for v in link if v.endswith('/libps3recomp_runtime.a'))
    archive=out/'libps3recomp_runtime.a';shutil.copy2(base/old,archive)
    objects=[v for k,v in replacements.items() if 'ps3recomp_sdk/' in k]
    if len(objects)!=len(a.runtime): raise RuntimeError('runtime archive object selection failed')
    subprocess.run(['ar','r',str(archive),*objects],check=True)
    replacements[old]=str(archive)
link=[replacements.get(v,v) for v in link]
link[link.index('-o')+1]=str(out/'tokyojungle.next')
link=[v.replace('--dependency-file=CMakeFiles/tokyojungle.dir/link.d','--dependency-file='+str(out/'native-link.d')) for v in link]
subprocess.run(link,cwd=base,check=True)
subprocess.run(['python3','scripts/check_no_emulation.py','--ps3recomp',str(out/'ps3recomp'),'--binary',str(out/'tokyojungle.next')],check=True)
(out/'tokyojungle.next').replace(out/'tokyojungle')
