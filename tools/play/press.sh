#!/bin/bash
# usage: tools/play/press.sh <run-name> <mask> [polls]   -- one PAD_FILE press
# masks: START 0x0008 UP 0x0010 RIGHT 0x0020 DOWN 0x0040 LEFT 0x0080
#        L2 0x0100 R2 0x0200 L1 0x0400 R1 0x0800 TRIANGLE 0x1000 CIRCLE 0x2000 CROSS 0x4000 SQUARE 0x8000
f="$(cd "$(dirname "$0")/../.." && pwd)/build-runs/$1/pad.txt"
for i in $(seq 1 50); do [ -s "$f" ] || break; sleep 0.1; done   # previous press consumed
echo "$2 ${3:-40}" > "$f"
