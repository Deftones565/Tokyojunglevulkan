#!/usr/bin/env python3
"""Build one lifted SPU image against private headers and relink a test binary.

Reuse unchanged production objects, never modify build/. Run from repo root:
  python3 tools/spu-opt/build_image_variant.py build-codex j8D485835BE6E8738
Add --all to recompile all lifted SPU images. The output directory must
contain a ps3recomp copy. This is an AOT build only; runtime objects are reused.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out'); p.add_argument('image')
p.add_argument('--all', action='store_true', help='recompile every lifted SPU image, two at a time')
p.add_argument('--runtime-archive', help='private runtime archive with matching region bridge')
p.add_argument('--source', help='private replacement C source for one image')
a = p.parse_args()
if a.source and a.all: p.error('--source requires a single image')
root = Path.cwd(); out = Path(a.out).resolve(); base = root / 'build'
if out == base.resolve() or not (out/'ps3recomp/runtime/spu/spu_helpers.h').is_file():
    p.error('use a private output directory containing a ps3recomp copy')
commands = subprocess.check_output(['ninja', '-C', str(base), '-t', 'commands', 'tokyojungle'], text=True).splitlines()
source = '/spu_recomp.c' if a.all else f'/generated/spu/{a.image}/spu_recomp.c'
selected = [shlex.split(c) for c in commands if c.endswith(source) and '/generated/spu/' in c]
if not selected:
    p.error('no matching lifted SPU compile commands found')
def compile_image(compile_cmd):
    original_obj = compile_cmd[compile_cmd.index('-o')+1]
    stem = Path(compile_cmd[-1]).parent.name
    obj = out / (stem + '.o')
    # Redirect every compiler output and prefer the private runtime headers.
    for flag, value in [('-o', str(obj)), ('-MF', str(out/(stem+'.d'))), ('-MT', str(obj))]:
        compile_cmd[compile_cmd.index(flag)+1] = value
    compile_cmd = [c.replace(str(root/'third_party/ps3recomp'),str(out/'ps3recomp')) for c in compile_cmd]
    if a.source:
        original_source = Path(compile_cmd[-1])
        compile_cmd[-1] = str(Path(a.source).resolve())
        compile_cmd.insert(1, '-I'+str(original_source.parent))
    subprocess.run(compile_cmd, cwd=base, check=True)
    return original_obj, str(obj)
with ThreadPoolExecutor(max_workers=2) as pool:
    replacements = dict(pool.map(compile_image, selected))
# CMake's final shell command includes guard checks after linking. Invoke the
# link argv directly, then run the no-emulation guard explicitly on the result.
link = shlex.split(commands[-1].split(' && ')[1])
link = [str(Path(a.runtime_archive).resolve()) if a.runtime_archive and c.endswith('/libps3recomp_runtime.a') else replacements.get(c,c) for c in link]
link[link.index('-o')+1] = str(out/'tokyojungle.next')
link = [c.replace('--dependency-file=CMakeFiles/tokyojungle.dir/link.d', '--dependency-file='+str(out/'link.d')) for c in link]
subprocess.run(link, cwd=base, check=True)
subprocess.run(['python3', 'scripts/check_no_emulation.py', '--ps3recomp', str(out/'ps3recomp'), '--binary', str(out/'tokyojungle.next')],check=True)
(out/'tokyojungle.next').replace(out/'tokyojungle')
