#!/usr/bin/env python3
"""Contact sheet of a frame dump dir: tools/ab/sheet.py <frames dir> <out.png> [step] [first] [last]
Labels each tile with its flip number, so A/B runs can be matched by flip."""
import sys, os, re
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 8
lo = int(sys.argv[4]) if len(sys.argv) > 4 else 0
hi = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30
fs = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
fs = [f for f in fs if lo <= int(re.search(r'flip(\d+)', f).group(1)) <= hi][::step]
tw, th, cols = 320, 180, 5
rows = (len(fs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * th), 'black')
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(os.path.join(d, f)).convert('RGB').resize((tw, th))
    x, y = (i % cols) * tw, (i // cols) * th
    sheet.paste(im, (x, y))
    dr.text((x + 4, y + 4), re.search(r'flip(\d+)', f).group(1), fill='yellow')
sheet.save(out)
print(out, len(fs), 'tiles')
