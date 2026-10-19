#!/usr/bin/env python3
"""Fuse the geometry job's two-block loop into one C loop.

usage: fuse_geometry_loop.py <spu dir>        (e.g. a copy of generated/spu)

Tokyo Jungle's geometry job (SPU image jE7517A8D6D4308F3, the biggest single
cost in busy areas) spends its inner loop in two lifted blocks: 0x412C falls
into 0x415C, which loops back to 0x412C until r12 reaches 0 and then leaves for
0x45EC. Each hop goes back through SPU_DRAIN, which runs the lockstep tick, the
task-launch check, the interrupt check and the PC history for every one of
~350 iterations per job. The LasVola/tokyojungle port fuses the same loop
(scripts/fuse_spu_loops.py, its image 17).

0x412C is rewritten into a loop over both bodies, unchanged statement for
statement. It leaves exactly where the blocks did (0x45EC), and also back to
0x412C through the dispatcher when an interrupt is pending, so interrupt
delivery is as before. 0x415C itself is kept for any other entry. The script
checks the lifted text of both blocks first and refuses on any difference;
run it again and it reports the loop already fused.
"""
import sys
from pathlib import Path

IMG = "jE7517A8D6D4308F3"
F412C = "void %s_spu_func_0000412C(spu_context* ctx) {" % IMG
F415C = "void %s_spu_func_0000415C(spu_context* ctx) {" % IMG
MARK = "/* tj-fused-loop 0x412C<->0x415C */"
TAIL_412C = "        { ctx->pc = 0x415C; g_spu_trampoline_fn = %s_spu_func_0000415C; return; }\n" % IMG
EXIT_415C = ("        if (ctx->gpr[12]._u32[0] == 0) { ctx->pc = 0x45EC; "
             "g_spu_trampoline_fn = %s_spu_func_000045EC; return; }\n" % IMG)
TAIL_415C = "        { ctx->pc = 0x412C; g_spu_trampoline_fn = %s_spu_func_0000412C; return; }\n" % IMG


def body(src, sig):
    a = src.index(sig)
    b = src.index("\n}\n", a)
    return a, b + 3, src[a + len(sig) + 1:b + 1]   # statements, each ending "\n"


def main():
    path = Path(sys.argv[1]) / IMG / "spu_recomp.c"
    src = path.read_text()
    if MARK in src:
        print("fuse_geometry_loop: already fused"); return 0
    if src.count(F412C) != 1 or src.count(F415C) != 1:
        sys.exit("fuse_geometry_loop: 0x412C/0x415C not found exactly once")
    a0, a1, b412 = body(src, F412C)
    _, _, b415 = body(src, F415C)
    if not b412.endswith(TAIL_412C) or b412.count("return;") != 1:
        sys.exit("fuse_geometry_loop: 0x412C does not end in the jump to 0x415C")
    if b415.count(EXIT_415C) != 1 or not b415.endswith(TAIL_415C) or b415.count("return;") != 2:
        sys.exit("fuse_geometry_loop: 0x415C is not the expected loop block")
    head, rest = b415.split(EXIT_415C)
    keep = rest[:-len(TAIL_415C)]                    # r40/r39 moves before looping
    ind = lambda s: "".join("    " + l + "\n" for l in s.rstrip("\n").split("\n"))
    fused = (F412C + "\n        " + MARK + "\n"
             "        for (;;) {\n"
             + ind(b412[:-len(TAIL_412C)]) + ind(head)
             + "    " + EXIT_415C + ind(keep) +
             "            /* An interrupt the dispatcher would deliver between hops: leave\n"
             "             * through it at 0x412C, as the unfused loop would. */\n"
             "            if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {\n"
             "                ctx->pc = 0x412C; g_spu_trampoline_fn = %s_spu_func_0000412C; return;\n"
             "            }\n"
             "        }\n"
             "}\n" % IMG)
    path.write_text(src[:a0] + fused + src[a1:])
    print("fuse_geometry_loop: fused 0x412C<->0x415C in", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
