#!/bin/bash
# Prints a PAD_SCRIPT that reaches the Survival tutorial street in ~31 s
# (flip ~1900), from the user's recipe (2026-10-19): START through the boot
# screens (warning, logos, "press any button"), CROSS through the menus
# (Survival, the Pomeranian), START once to skip the opening movie (it opens
# at ~24 s; CROSS does not skip it), then CROSS for the tutorial's NEXT
# prompts. The old 11-press script waited the screens out and got there at
# ~70 s (flip ~4100).
#   usage: PAD_SCRIPT=$(tools/play/fast_pad.sh)
python3 -c "
ev  = [f'{t}:0x0008' for t in range(2, 16)]          # START: boot screens
ev += [f'{t}:0x4000' for t in range(16, 25)]         # CROSS: menus
ev += ['27:0x0008']                                  # START: skip the movie
ev += [f'{t}:0x4000' for t in range(30, 60)]         # CROSS: tutorial prompts
ev += [f'{t}:0x4000' for t in range(60, 95, 5)]
print(','.join(ev))"
