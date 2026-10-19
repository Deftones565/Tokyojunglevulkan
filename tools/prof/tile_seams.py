#!/usr/bin/env python3
"""Count frames whose picture is broken into 128x128 tiles (ps3recomp v0.15
upgrade regression): the mean horizontal jump across x = 128k seams against
the mean jump elsewhere. usage: tile_seams.py <frames dir> [first index]"""
import glob, sys
import numpy as np
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + '/*.ppm'))[int(sys.argv[2]) if len(sys.argv) > 2 else 0:]
bad = 0
for f in fs:
    a = np.asarray(Image.open(f).convert('L'), dtype=np.float32)
    dx = np.abs(np.diff(a, axis=1)).mean(axis=0)
    seams = np.arange(127, a.shape[1] - 1, 128)
    bad += dx[seams].mean() / (np.delete(dx, seams).mean() + 1e-3) > 3
print(f'{len(fs)} frames, {bad} tiled')
