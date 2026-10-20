#!/bin/bash
# Prints a PAD_ROUTE (left-stick walk, see build-magi7 cellPad PAD_ROUTE) that
# wanders the Survival map from t=32 s: 5 s legs in a fixed pseudo-random
# order of 8 directions, so every run walks the same way. With ATTACK=1 it
# also taps SQUARE (attack) for 0.5 s every second, so the animal fights and
# eats what it runs into.
#   usage: PAD_ROUTE=$(ATTACK=1 tools/play/route_walk.sh [seed] [legs])
python3 -c "
import random, sys, os
r = random.Random(int(sys.argv[1])); legs = int(sys.argv[2]); atk = os.environ.get('ATTACK')
dirs = [(128,0),(255,0),(255,128),(255,255),(128,255),(0,255),(0,128),(0,0)]
t, d, out = 32, 0, []
for i in range(legs):
    d = (d + r.choice([-2,-1,0,1,2])) % 8
    x, y = dirs[d]
    if atk:
        for k in range(5):
            out.append(f'{t+k}:{x},{y},0x8000'); out.append(f'{t+k+0.5}:{x},{y}')
    else:
        out.append(f'{t}:{x},{y}')
    t += 5
print(';'.join(out))" ${1:-7} ${2:-60}
