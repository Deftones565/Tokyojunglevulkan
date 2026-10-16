#!/usr/bin/env python3
"""Build one lifted SPU image against private headers and relink a test binary.

Reuse unchanged production objects, never modify build/. Run from repo root:
  python3 tools/spu-opt/build_image_variant.py build-codex j8D485835BE6E8738
The output directory must contain a ps3recomp copy. This is an AOT build only.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out'); p.add_argument('image')
a = p.parse_args()
root = Path.cwd(); out = Path(a.out).resolve(); base = root / 'build'
commands = subprocess.check_output(['ninja', '-C', str(base), '-t', 'commands', 'tokyojungle'], text=True).splitlines()
source = f'/generated/spu/{a.image}/spu_recomp.c'
compile_cmd = next(shlex.split(c) for c in commands if c.endswith(source))
original_obj = compile_cmd[compile_cmd.index('-o')+1]
# Redirect all generated compiler outputs, and prefer the private runtime headers.
for flag, value in [('-o', str(out/'image.o')), ('-MF', str(out/'image.d')), ('-MT', str(out/'image.o'))]:
    compile_cmd[compile_cmd.index(flag)+1] = value
compile_cmd = [c.replace(str(root/'third_party/ps3recomp'),str(out/'ps3recomp')) for c in compile_cmd]
subprocess.run(compile_cmd, cwd=base, check=True)
# CMake's final shell command includes guard checks after linking. Invoke the
# link argv directly, then run the no-emulation guard explicitly on the result.
link = shlex.split(commands[-1].split(' && ')[1])
link = [str(out/'image.o') if c == original_obj else c for c in link]
link[link.index('-o')+1] = str(out/'tokyojungle')
link = [c.replace('--dependency-file=CMakeFiles/tokyojungle.dir/link.d', '--dependency-file='+str(out/'link.d')) for c in link]
subprocess.run(link, cwd=base, check=True)
subprocess.run(['python3', 'scripts/check_no_emulation.py', '--ps3recomp', str(out/'ps3recomp'), '--binary', str(out/'tokyojungle')],check=True)
