#!/usr/bin/env python3
"""
Cut Tokyo Jungle's SPURS job images out of the decrypted 1.01 EBOOT.

  python3 scripts/extract_spu_jobs.py <EBOOT.ELF> [<dumps dir>]   (default generated/spu/dumps)

The game's 18 SPU job images are raw code+data blobs. They were first captured
while the game ran (SPU_DUMP_MISS), but every one of them sits, byte for byte,
inside the decrypted EBOOT at the offset below (the runtime fetches them from
guest memory, where the EBOOT's segments put them). So they are cut out of the
player's own EBOOT here: this table is offsets and sizes, not game data.

Each image is written as spujob_<FINGERPRINT>_<SIZE>.bin, the name
SPU_DUMP_MISS uses, after checking its bytes hash to the fingerprint
(lift_spu.fnv1a64, as runtime/spu/spu_workload.c computes it). Then
scripts/lift_spu.py lifts them. An EBOOT that isn't 1.01's fails the check.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lift_spu import SPU, fnv1a64   # noqa: E402

# fingerprint, offset in the 1.01 EBOOT.ELF, size
JOBS = [
    (0x1B6BDAC375C5438D, 2959616, 13840),
    (0x48AF2BC79A72CB50, 3142912, 25232),
    (0x49C1D6C47E80AF43, 3118080, 24720),
    (0x4EC5C59B2D4E508B, 2832768, 6480),
    (0x5D51D353BBB628AC, 2843904, 10708),
    (0x5DA195169669A5FF, 2991104, 72464),
    (0x80435D36E623FAA0, 3098496, 19488),
    (0x82D0BE145EC28AEE, 2973568, 17424),
    (0x8D485835BE6E8738, 2854656, 33876),
    (0x9804ABA84566F431, 3224832, 20624),
    (0xABD49FC58F53F368, 3168256, 9488),
    (0xB3A92792A279C7F3, 2839296, 4544),
    (0xD58063CD2A0236B5, 3245568, 53408),
    (0xE0CE22AC6DC5A6D4, 2743680, 40896),
    (0xE7517A8D6D4308F3, 2784640, 48032),
    (0xF523F26F69764538, 2894592, 64912),
    (0xFDFFAAC1BCD5352E, 3063680, 34704),
    (0xFE031AF67ED048DE, 3177856, 46928),
]


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().split("\n\n")[1])
    elf = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else SPU / "dumps"
    out.mkdir(parents=True, exist_ok=True)
    for fp, off, size in JOBS:
        data = elf[off:off + size]
        if len(data) != size or fnv1a64(data) != fp:
            sys.exit(f"job {fp:016X} is not at 0x{off:X} in {sys.argv[1]}: "
                     "is it the decrypted 1.01 EBOOT?")
        (out / f"spujob_{fp:016X}_{size}.bin").write_bytes(data)
    print(f"{len(JOBS)} SPU job images -> {out}")


if __name__ == "__main__":
    main()
